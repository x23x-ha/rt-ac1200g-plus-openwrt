// SPDX-License-Identifier: GPL-2.0
/* NAT fast path for the Archer C1200 v2 (BCM53573: bgmac + BCM53125 behind a
 * Broadcom-tagged DSA conduit) - the same idea as Broadcom's closed CTF.
 *
 * bgmac hands every received frame to fp_rx() (the bgmac_rx_hook from patch
 * 323) before it enters the stack. For IPv4 TCP/UDP frames that belong to a
 * flow the kernel has already offloaded into the netfilter flowtable, going
 * out of another switch port on the same conduit, the frame is NATed in place
 * (addresses, ports, checksums, TTL), the MACs and the Broadcom tag are
 * rewritten for the egress port, and it goes straight onto the conduit's TX
 * ring. No bridge, DSA, netfilter, routing or qdisc work. Flows with direct
 * xmit carry their MACs; "neighbour" flows (out of a port that is not in a
 * bridge, e.g. LAN -> wan: every upload) get the next hop's MAC from the
 * cached route's neighbour entry (fp_out4).
 * Everything else - and any packet that fails a check - returns false
 * untouched and takes the normal path.
 *
 * The flowtable must list the switch ports (wan, lan1-4), not just the
 * bridges, so flows get FLOW_OFFLOAD_XMIT_DIRECT with port and MACs filled in
 * (see fastpath.nft). The flowtable is looked up per packet from the ingress
 * port's netfilter hooks, so firewall reloads are safe.
 *
 *   cat /proc/fastpath            counters
 *   echo 0 > /proc/fastpath       stop claiming packets (1 to resume, r resets)
 *   echo v > /proc/fastpath       verify TCP checksums before/after NAT (V: off)
 *   echo b > /proc/fastpath       batch transmits per NAPI poll (default; B: off)
 *   echo w > /proc/fastpath       bridged LAN -> 5 GHz Wi-Fi shortcut (default; W: off)
 *   echo h > /proc/fastpath       send the last tail_copy bytes from a fresh copy (default)
 *   echo k > /proc/fastpath       transmit a full fresh copy of each packet
 *                                 (K: in place, a: full copy at a 2-byte offset)
 *   echo m > /proc/fastpath       with K: re-verify each packet in DRAM at TX done (M: off)
 *   echo x > /proc/fastpath       mirror check: log bytes that differ in frames the
 *                                 switch mirrors back from mirror_port (X: off)
 */
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/etherdevice.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/netfilter.h>
#include <net/ip.h>
#include <net/route.h>
#include <net/arp.h>
#include <net/neighbour.h>
#include <net/dsa.h>
#include <net/sch_generic.h>
#include <linux/bcma/bcma.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <net/netfilter/nf_flow_table.h>
#include <net/netfilter/nf_conntrack_acct.h>
#include <net/netfilter/nf_conntrack.h>

extern bool (__rcu *bgmac_rx_hook)(struct sk_buff *skb);
extern unsigned int bgmac_tx_dirty_hint;
extern bool bgmac_rx_lazy_sync;

#define BRCM_TAG_LEN		4
#define BRCM_OPCODE_SHIFT	5
#define BRCM_EG_RC_RSVD		(3 << 6)
#define BRCM_EG_PID_MASK	0x1f
#define BRCM_EG_RC_MIRROR	(1 << 0)
#define FP_PORTS		9

static char *conduit_name = "eth0";	/* RT-AC1200G+: gmac0 = eth0 (Archer: eth1) */
module_param(conduit_name, charp, 0444);
MODULE_PARM_DESC(conduit_name, "DSA conduit (bgmac) interface");

static struct net_device *conduit;
static struct net_device *port_dev[FP_PORTS];

/* The Broadcom wl radios (5 GHz SoC core, 2.4 GHz PCIe). They load late,
 * from the USB stick, so the netdev notifier picks them up by driver name
 * ("wl", from ethtool drvinfo). Flows whose egress is one of them (it must be
 * in the flowtable, see fastpath-wifi.sh) are NATed here and handed straight
 * to its ndo_start_xmit - the wl shim chains them. */
#define FP_WIFI_MAX		2
static int wifi_ifindex[FP_WIFI_MAX];
#define FP_OUT_WIFI		FP_PORTS

static bool fp_is_wl(struct net_device *dev)
{
	struct ethtool_drvinfo info = {};

	if (!dev->ethtool_ops || !dev->ethtool_ops->get_drvinfo)
		return false;
	dev->ethtool_ops->get_drvinfo(dev, &info);
	return !strcmp(info.driver, "wl");
}

static bool fp_is_wifi_ifindex(int ifindex)
{
	int i;

	for (i = 0; i < FP_WIFI_MAX; i++)
		if (ifindex && READ_ONCE(wifi_ifindex[i]) == ifindex)
			return true;
	return false;
}

/*
 * Bridged LAN -> Wi-Fi shortcut. The wl shim reports the source MAC of every
 * frame a station sends, and the radio it came in on (fastpath_wifi_learn2);
 * a frame from a LAN port whose destination is such a station, with both
 * ports in the same bridge, goes straight to that radio. A station that has
 * been silent for FP_STA_TTL, or any (dis)association, drops out and the
 * bridge decides again. Once per second per sender a frame still takes the
 * bridge, so its fdb keeps the LAN host's port and replies are not flooded.
 */
#define FP_STA_BITS		5
#define FP_STA_TTL		(2 * HZ)
struct fp_sta {
	u8 mac[ETH_ALEN];
	int ifindex;
	unsigned long seen;
};
static struct fp_sta fp_sta[1 << FP_STA_BITS];
static unsigned long fp_src_seen[1 << FP_STA_BITS];
static bool bridge_wifi = true;

static unsigned int fp_mac_hash(const u8 *a)
{
	return (a[3] ^ a[4] ^ a[5]) & ((1 << FP_STA_BITS) - 1);
}

/* Called by the wl shim for each frame received from a station on radio
 * dev; a NULL mac forgets them all. Runs in softirq context, as does every
 * reader. (The "2" is the ABI: v1 had no dev and assumed one radio.) */
void fastpath_wifi_learn2(const u8 *mac, struct net_device *dev);
void fastpath_wifi_learn2(const u8 *mac, struct net_device *dev)
{
	struct fp_sta *s;

	if (!mac) {
		memset(fp_sta, 0, sizeof(fp_sta));
		return;
	}
	s = &fp_sta[fp_mac_hash(mac)];
	if (!ether_addr_equal_unaligned(s->mac, mac))
		memcpy(s->mac, mac, ETH_ALEN);
	s->ifindex = dev->ifindex;
	s->seen = jiffies;
}
EXPORT_SYMBOL_GPL(fastpath_wifi_learn2);

/* The radio a live station was last heard on, 0 if none. */
static int fp_wifi_sta_ifindex(const u8 *mac)
{
	const struct fp_sta *s = &fp_sta[fp_mac_hash(mac)];

	if (s->seen && ether_addr_equal_unaligned(s->mac, mac) &&
	    time_before(jiffies, s->seen + FP_STA_TTL))
		return s->ifindex;
	return 0;
}

/* The other direction: LAN hosts and the switch port they were last heard
 * on (from IPv4/IPv6 frames fp_rx sees), for Wi-Fi -> LAN frames. */
struct fp_host {
	u8 mac[ETH_ALEN];
	u8 port;
	unsigned long seen;
};
static struct fp_host fp_host[1 << FP_STA_BITS];
static unsigned long fp_wsrc_seen[1 << FP_STA_BITS];

static void fp_host_learn(const u8 *mac, int port)
{
	struct fp_host *h = &fp_host[fp_mac_hash(mac)];

	if (!ether_addr_equal_unaligned(h->mac, mac))
		memcpy(h->mac, mac, ETH_ALEN);
	h->port = port;
	h->seen = jiffies;
}
static bool enabled = true;
static bool verify;	/* debug: check every TCP checksum before and after NAT */
/* Frames are forwarded in place, straight out of their receive buffer. That
 * used to corrupt 0.1-4% of them: bgmac programmed 128-byte TX DMA bursts
 * (a Northstar setting), and on BCM53573 those put one burst's data in the
 * previous burst's place under load. Patch 323 makes 64-byte bursts the
 * default (bgmac.tx_bl=2), which is clean. The copy modes remain for
 * debugging.
 */
static int copy_tx;	/* 0: in place, 1: full copy, 2: full copy at +2, 3: tail copy */
static int tail_copy = 640;
module_param(tail_copy, int, 0644);
MODULE_PARM_DESC(tail_copy, "bytes at the end of each forwarded frame sent from a fresh copy");

static struct {
	u64 fwd, fwd_bytes;
	u64 not_ip, no_port, no_ft, no_flow, not_direct, other_out, fin_rst, mtu;
	u64 neigh, neigh_miss;
	u64 busy, busy_drop, xmit_err;
	u64 checked, bad_rx, bad_tx;
	u64 mem_checked, mem_bad;
	u64 mir, mir_bad, mir_nosnap;
	u64 tail_copied;
	u64 cache_hit;
	u64 fwd6, v6_nat;
	u64 wifi, wifi_busy, bridged, wifi_up, wifi_route_up;
} st;
static int noflow_log;	/* debug: log this many packets that miss the flowtable */
module_param(noflow_log, int, 0644);
static bool memcheck;	/* debug: re-verify in-place packets in DRAM at TX completion */
static bool touch;	/* debug: with K, rewrite every cache line of the frame before TX */

/* Full TCP checksum check of an IPv4 packet; true if it is valid. */
static bool fp_tcp_csum_ok(const struct iphdr *iph, unsigned int tot)
{
	unsigned int len = tot - sizeof(*iph);
	const void *th = (const void *)iph + sizeof(*iph);

	return !csum_tcpudp_magic(iph->saddr, iph->daddr, len, IPPROTO_TCP,
				  csum_partial(th, len, 0));
}

static void fp_dump_bad(const char *where, const u8 *mac, unsigned int len)
{
	static int dumped;

	if (dumped++ >= 6)
		return;
	pr_info("fastpath: bad TCP checksum %s, frame of %u bytes:\n", where, len);
	print_hex_dump(KERN_INFO, "fastpath: ", DUMP_PREFIX_OFFSET, 16, 1,
		       mac, min(len, 128u), false);
}

/* The flowtable hooked into this device's ingress, if any. nf_tables marks its
 * chain hooks NF_HOOK_OP_NF_TABLES; flowtable hooks are left undefined and
 * carry the nf_flowtable as priv, whose type->hook is the hook itself.
 */
static struct nf_flowtable *fp_flowtable(const struct net_device *dev)
{
	const struct nf_hook_entries *e = rcu_dereference(dev->nf_hooks_ingress);
	struct nf_hook_ops **ops;
	unsigned int i;

	if (!e)
		return NULL;
	ops = nf_hook_entries_get_hook_ops(e);
	for (i = 0; i < e->num_hook_entries; i++) {
		struct nf_flowtable *ft = e->hooks[i].priv;

		if (ops[i]->hook_ops_type != NF_HOOK_OP_UNDEFINED ||
		    ops[i]->pf != NFPROTO_NETDEV || !ft)
			continue;
		if (ft->type && ft->type->hook == e->hooks[i].hook)
			return ft;
	}
	return NULL;
}

/* Switch port index of an egress device, FP_OUT_WIFI for the wl radio. */
static int fp_port_of_ifindex(int ifindex)
{
	int p;

	for (p = 0; p < FP_PORTS; p++)
		if (port_dev[p] && port_dev[p]->ifindex == ifindex)
			return p;
	if (fp_is_wifi_ifindex(ifindex))
		return FP_OUT_WIFI;
	return -1;
}

/*
 * Egress port and MACs of an IPv4 flow. "Direct" entries (path through a
 * bridge) carry both MACs. "Neighbour" entries (e.g. LAN -> a WAN port that is
 * not in a bridge: every upload) only carry the route; resolve the next hop as
 * nf_flow_offload_ip_hook() does. Called in softirq (RCU held).
 */
static bool fp_out4(const struct flow_offload *flow,
		    const struct flow_offload_tuple_rhash *th, int dir,
		    int *outport, u8 *dmac, u8 *smac)
{
	const struct dst_entry *dst;
	struct net_device *odev;
	struct neighbour *n;
	__be32 nh;

	if (th->tuple.encap_num)
		return false;
	if (th->tuple.xmit_type == FLOW_OFFLOAD_XMIT_DIRECT) {
		*outport = fp_port_of_ifindex(th->tuple.out.ifidx);
		memcpy(dmac, th->tuple.out.h_dest, ETH_ALEN);
		memcpy(smac, th->tuple.out.h_source, ETH_ALEN);
		return true;
	}
	if (th->tuple.xmit_type != FLOW_OFFLOAD_XMIT_NEIGH ||
	    th->tuple.l3proto != AF_INET)
		return false;
	dst = th->tuple.dst_cache;
	if (!dst || !dst_check((struct dst_entry *)dst, th->tuple.dst_cookie))
		goto miss;
	odev = dst->dev;
	*outport = fp_port_of_ifindex(odev->ifindex);
	if (*outport < 0 || *outport == FP_OUT_WIFI)
		goto miss;
	nh = rt_nexthop((struct rtable *)dst, flow->tuplehash[!dir].tuple.src_v4.s_addr);
	n = __ipv4_neigh_lookup_noref(odev, (__force u32)nh);
	if (!n || !(READ_ONCE(n->nud_state) & NUD_VALID))
		goto miss;
	/* the kernel no longer sees these packets: keep the entry confirmed */
	if (!(READ_ONCE(n->nud_state) & NUD_CONNECTED))
		neigh_event_send(n, NULL);
	neigh_ha_snapshot(dmac, n, odev);
	memcpy(smac, odev->dev_addr, ETH_ALEN);
	st.neigh++;
	return true;
miss:
	st.neigh_miss++;
	return false;
}

/* IPv4 address NAT as nf_flow_table_ip.c does it (those helpers are static) */
static void fp_nat_l4(struct sk_buff *skb, struct iphdr *iph, unsigned int thoff,
		      __be32 addr, __be32 new_addr)
{
	if (iph->protocol == IPPROTO_TCP) {
		struct tcphdr *tcph = (void *)iph + thoff;

		inet_proto_csum_replace4(&tcph->check, skb, addr, new_addr, true);
	} else {
		struct udphdr *udph = (void *)iph + thoff;

		if (udph->check) {
			inet_proto_csum_replace4(&udph->check, skb, addr, new_addr, true);
			if (!udph->check)
				udph->check = CSUM_MANGLED_0;
		}
	}
}

static void fp_nat_ip(const struct flow_offload *flow, struct sk_buff *skb,
		      struct iphdr *iph, unsigned int thoff,
		      enum flow_offload_tuple_dir dir)
{
	const struct flow_offload_tuple *orig = &flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].tuple;
	const struct flow_offload_tuple *repl = &flow->tuplehash[FLOW_OFFLOAD_DIR_REPLY].tuple;
	__be32 addr, new_addr;

	if (test_bit(NF_FLOW_SNAT, &flow->flags)) {
		nf_flow_snat_port(flow, skb, thoff, iph->protocol, dir);
		if (dir == FLOW_OFFLOAD_DIR_ORIGINAL) {
			addr = iph->saddr;
			new_addr = repl->dst_v4.s_addr;
			iph->saddr = new_addr;
		} else {
			addr = iph->daddr;
			new_addr = orig->src_v4.s_addr;
			iph->daddr = new_addr;
		}
		csum_replace4(&iph->check, addr, new_addr);
		fp_nat_l4(skb, iph, thoff, addr, new_addr);
	}
	if (test_bit(NF_FLOW_DNAT, &flow->flags)) {
		nf_flow_dnat_port(flow, skb, thoff, iph->protocol, dir);
		if (dir == FLOW_OFFLOAD_DIR_ORIGINAL) {
			addr = iph->daddr;
			new_addr = repl->src_v4.s_addr;
			iph->daddr = new_addr;
		} else {
			addr = iph->saddr;
			new_addr = orig->dst_v4.s_addr;
			iph->saddr = new_addr;
		}
		csum_replace4(&iph->check, addr, new_addr);
		fp_nat_l4(skb, iph, thoff, addr, new_addr);
	}
}

/* Debug (memcheck): when the TX of an in-place packet completes, drop the
 * frame's cache lines and verify the TCP checksum against what is now in
 * DRAM - i.e. was the buffer overwritten while the DMA was reading it?
 */
static void fp_memcheck_destructor(struct sk_buff *skb)
{
	struct device *dd = conduit ?
		container_of(conduit->dev.parent, struct bcma_device, dev)->dma_dev : NULL;
	struct iphdr *iph = (struct iphdr *)skb_network_header(skb);
	dma_addr_t a;

	if (!dd)
		return;
	a = dma_map_single(dd, skb->data, skb->len, DMA_FROM_DEVICE);
	if (dma_mapping_error(dd, a))
		return;
	dma_unmap_single(dd, a, skb->len, DMA_FROM_DEVICE);
	st.mem_checked++;
	if (!fp_tcp_csum_ok(iph, ntohs(iph->tot_len))) {
		st.mem_bad++;
		fp_dump_bad("in DRAM at TX completion", skb->data, skb->len);
	}
}

/* skb->cb marker for frames sent in place (not a copy), and how many leading
 * bytes of such a frame fp_rx() rewrote (MAC, tag, IPv4, TCP/UDP header up to
 * the checksum): bgmac then cleans only those from the cache.
 */
#define FP_CB_INPLACE		0x46505450
#define FP_SKB_INPLACE(skb)	(*(u32 *)(skb)->cb == FP_CB_INPLACE)
#define FP_DIRTY_BYTES		64

static void fp_xmit(struct sk_buff *skb)
{
	struct netdev_queue *txq = netdev_get_tx_queue(conduit, 0);
	netdev_tx_t rc;

	skb->dev = conduit;
	skb_set_queue_mapping(skb, 0);

	/* Never overtake packets the qdisc is still holding (it only holds any
	 * when the ring was full): reordering makes TCP see duplicate ACKs,
	 * retransmit spuriously and halve its window.
	 */
	if (unlikely(qdisc_qlen(rcu_dereference_bh(txq->qdisc)))) {
		st.busy++;
		if (dev_queue_xmit(skb) != NET_XMIT_SUCCESS)
			st.busy_drop++;
		return;
	}
	__netif_tx_lock(txq, smp_processor_id());
	if (unlikely(netif_xmit_frozen_or_stopped(txq))) {
		/* ring full: let the qdisc hold it until the ring drains */
		__netif_tx_unlock(txq);
		st.busy++;
		if (dev_queue_xmit(skb) != NET_XMIT_SUCCESS)
			st.busy_drop++;
		return;
	}
	/* in place: only the header we rewrote can be dirty in the cache */
	if (!skb_shinfo(skb)->nr_frags && FP_SKB_INPLACE(skb))
		bgmac_tx_dirty_hint = FP_DIRTY_BYTES;
	rc = netdev_start_xmit(skb, conduit, txq, false);
	bgmac_tx_dirty_hint = 0;
	__netif_tx_unlock(txq);
	if (unlikely(!dev_xmit_complete(rc))) {
		st.xmit_err++;
		kfree_skb(skb);
	}
}

/* Packets claimed during one NAPI poll are sent together from a tasklet,
 * which runs right after the receive softirq. Transmitting a receive buffer
 * the instant it arrived let ~0.1-1.6% of frames go out with stale bytes
 * (the TX DMA apparently racing the RX DMA's writes); the batch delay also
 * keeps the TX ring work together.
 */
static struct sk_buff_head fp_txq;
static struct tasklet_struct fp_tx_tasklet;
static bool batch;

/* Straight into the wl driver, same rules as fp_xmit(): never overtake what
 * the qdisc holds, and let the qdisc have it while the queue is stopped. */
static void fp_xmit_wifi(struct sk_buff *skb)
{
	struct net_device *dev = skb->dev;
	struct netdev_queue *txq = netdev_get_tx_queue(dev, 0);
	netdev_tx_t rc;

	if (unlikely(qdisc_qlen(rcu_dereference_bh(txq->qdisc)))) {
		st.wifi_busy++;
		dev_queue_xmit(skb);
		return;
	}
	__netif_tx_lock(txq, smp_processor_id());
	if (unlikely(netif_xmit_frozen_or_stopped(txq))) {
		__netif_tx_unlock(txq);
		st.wifi_busy++;
		dev_queue_xmit(skb);
		return;
	}
	rc = netdev_start_xmit(skb, dev, txq, false);
	__netif_tx_unlock(txq);
	if (unlikely(!dev_xmit_complete(rc))) {
		st.xmit_err++;
		kfree_skb(skb);
	}
}

static void fp_tx_flush(struct tasklet_struct *t)
{
	struct sk_buff *skb;

	while ((skb = __skb_dequeue(&fp_txq))) {
		if (skb->dev == conduit)
			fp_xmit(skb);
		else
			fp_xmit_wifi(skb);
	}
}

/*
 * Turn a claimed frame into a plain Ethernet frame for the wl device and
 * queue it. The frame is [dst][src][Broadcom tag][EtherType]..., skb->data
 * 2 bytes into the tag: the new MACs go right in front of the EtherType and
 * the tag disappears.
 */
static void fp_queue_wifi(struct sk_buff *skb, u8 *eth, struct net_device *dev)
{
	skb_push(skb, skb->data - eth);
	skb_reset_mac_header(skb);
	skb->dev = dev;
	*(u32 *)skb->cb = 0;
	if (batch) {
		__skb_queue_tail(&fp_txq, skb);
		if (skb_queue_len(&fp_txq) == 1)
			tasklet_schedule(&fp_tx_tasklet);
	} else {
		fp_xmit_wifi(skb);
	}
}

static struct net_device *fp_wifi_dev(const struct flow_offload_tuple_rhash *th)
{
	struct net_device *dev = dev_get_by_index_rcu(&init_net, th->tuple.out.ifidx);

	return dev && (dev->flags & IFF_UP) ? dev : NULL;
}

static void fp_to_wifi(struct sk_buff *skb, u8 *mac,
		       const struct flow_offload_tuple_rhash *th, struct net_device *dev)
{
	u8 *eth = mac + BRCM_TAG_LEN;

	memcpy(eth, th->tuple.out.h_dest, ETH_ALEN);
	memcpy(eth + ETH_ALEN, th->tuple.out.h_source, ETH_ALEN);
	st.wifi++;
	fp_queue_wifi(skb, eth, dev);
}

/* The bridged shortcut (see fp_sta): true if the frame was taken. */
static bool fp_bridge_wifi(struct sk_buff *skb, u8 *mac, struct net_device *in)
{
	struct net_device *wdev, *br;
	unsigned long *src;
	int ifindex;
	u8 *eth;

	if (!READ_ONCE(bridge_wifi) || !(ifindex = fp_wifi_sta_ifindex(mac)) ||
	    !fp_is_wifi_ifindex(ifindex))
		return false;
	wdev = dev_get_by_index_rcu(&init_net, ifindex);
	if (!wdev || !netif_running(wdev))
		return false;
	br = netdev_master_upper_dev_get_rcu(in);
	if (!br || !netif_is_bridge_master(br) || br != netdev_master_upper_dev_get_rcu(wdev))
		return false;
	/* let the bridge see each sender now and then, to keep its fdb */
	src = &fp_src_seen[fp_mac_hash(mac + ETH_ALEN)];
	if (time_after(jiffies, *src + HZ)) {
		*src = jiffies;
		return false;
	}
	if (unlikely(skb_cloned(skb)))
		return false;

	/* committed: drop the Broadcom tag, MACs stay */
	eth = mac + BRCM_TAG_LEN;
	memmove(eth, mac, 2 * ETH_ALEN);
	st.bridged++;
	fp_queue_wifi(skb, eth, wdev);
	return true;
}

/*
 * Wi-Fi -> wired: called by the wl shim for each received frame (skb->data
 * past the Ethernet header, mac_header set). A frame for the bridge's own
 * MAC is routed if its flow is offloaded (fp_wifi_route); a frame for a LAN
 * host we know the port of, both ports in one bridge, is bridged. Either
 * way it gets a Broadcom tag for its port and goes onto the conduit's TX
 * ring. True if taken.
 */
static bool fp_wifi_route(struct sk_buff *skb, u8 *eth, struct net_device *in);

/*
 * Softirq kick. With HZ=100 the bgmac NAPI poll was regularly found queued
 * (NET_RX pending) while the CPU sat idle, and only ran at the next interrupt
 * of any kind - up to a 10 ms tick later. A 13 ms internet RTT TCP stream
 * reached ~550 Mbit/s through the router against ~900 direct; any 1 kHz
 * interrupt source brought it to ~880. So while we forward, a 1 kHz hrtimer
 * runs; its irq exit services whatever softirq was left pending. It stops
 * itself once traffic stops.
 */
static unsigned int kick_us = 1000;
module_param(kick_us, uint, 0644);
MODULE_PARM_DESC(kick_us, "softirq kick period while forwarding, 0 = off");
static struct hrtimer fp_kick;
static unsigned long fp_act, fp_act_seen;
static bool fp_kick_on;

static enum hrtimer_restart fp_kick_fn(struct hrtimer *t)
{
	unsigned int us = READ_ONCE(kick_us);

	if (us && fp_act != fp_act_seen) {
		fp_act_seen = fp_act;
		hrtimer_forward_now(t, ns_to_ktime(us * NSEC_PER_USEC));
		return HRTIMER_RESTART;
	}
	fp_kick_on = false;
	return HRTIMER_NORESTART;
}

static inline void fp_kick_arm(void)
{
	unsigned int us;

	fp_act++;
	if (likely(fp_kick_on))
		return;
	us = READ_ONCE(kick_us);
	if (us) {
		fp_kick_on = true;
		hrtimer_start(&fp_kick, ns_to_ktime(us * NSEC_PER_USEC), HRTIMER_MODE_REL_PINNED);
	}
}

static void fp_queue_conduit(struct sk_buff *skb)
{
	skb_reset_mac_header(skb);
	skb->dev = conduit;
	*(u32 *)skb->cb = 0;
	if (batch) {
		__skb_queue_tail(&fp_txq, skb);
		if (skb_queue_len(&fp_txq) == 1)
			tasklet_schedule(&fp_tx_tasklet);
	} else {
		fp_xmit(skb);
	}
}

bool fastpath_wifi_rx(struct sk_buff *skb);
bool fastpath_wifi_rx(struct sk_buff *skb)
{
	u8 *eth = skb_mac_header(skb), *tag;
	const struct fp_host *h;
	struct net_device *out, *br;
	unsigned long *src;

	if (!READ_ONCE(enabled) || !conduit || is_multicast_ether_addr(eth))
		return false;
	fp_kick_arm();
	br = netdev_master_upper_dev_get_rcu(skb->dev);
	if (!br || !netif_is_bridge_master(br))
		return false;
	if (ether_addr_equal_unaligned(eth, br->dev_addr))
		return fp_wifi_route(skb, eth, skb->dev);
	if (!READ_ONCE(bridge_wifi))
		return false;
	h = &fp_host[fp_mac_hash(eth)];
	if (!h->seen || !ether_addr_equal_unaligned(h->mac, eth) ||
	    time_after(jiffies, h->seen + FP_STA_TTL))
		return false;
	out = port_dev[h->port];
	if (!out || !netif_running(out) || br != netdev_master_upper_dev_get_rcu(out))
		return false;
	src = &fp_wsrc_seen[fp_mac_hash(eth + ETH_ALEN)];
	if (time_after(jiffies, *src + HZ)) {
		*src = jiffies;
		return false;
	}
	if (unlikely(skb_cloned(skb)) || eth - skb->head < BRCM_TAG_LEN)
		return false;

	/* committed: [dst][src][tag][EtherType], tag directing it to the port */
	skb_push(skb, skb->data - eth + BRCM_TAG_LEN);
	memmove(skb->data, eth, 2 * ETH_ALEN);
	tag = skb->data + 2 * ETH_ALEN;
	tag[0] = 1 << BRCM_OPCODE_SHIFT;
	tag[1] = 0;
	tag[2] = h->port == 8 ? 1 : 0;
	tag[3] = (1 << h->port) & 0xff;
	st.wifi_up++;
	fp_queue_conduit(skb);
	return true;
}
EXPORT_SYMBOL_GPL(fastpath_wifi_rx);

/* Debug (mirror check): the switch mirrors everything it sends out of
 * mirror_port back to the CPU. We keep a copy of each TCP frame we send to
 * that port, and when a mirrored frame comes back with a bad checksum we
 * find our copy by sequence number and log which bytes differ.
 */
#define FP_SNAPS	2048
struct fp_snap {
	__be32 seq;
	__be16 dport;
	u16 len;
	u8 data[1536];
};
static struct fp_snap *snaps;
static unsigned int snap_head;
static int mirror_port = 2;
module_param(mirror_port, int, 0444);

static bool fp_is_our_mac(const u8 *a)
{
	int p;

	if (ether_addr_equal(a, conduit->dev_addr))
		return true;
	for (p = 0; p < FP_PORTS; p++)
		if (port_dev[p] && ether_addr_equal(a, port_dev[p]->dev_addr))
			return true;
	return false;
}

static void fp_snap_store(const struct iphdr *iph, unsigned int tot)
{
	const struct tcphdr *th = (const void *)iph + sizeof(*iph);
	struct fp_snap *s = &snaps[snap_head++ % FP_SNAPS];

	s->seq = th->seq;
	s->dport = th->dest;
	s->len = min(tot, 1536u);
	memcpy(s->data, iph, s->len);
}

static void fp_mirror_check(struct sk_buff *skb, const u8 *mac)
{
	const struct iphdr *iph = (const void *)(mac + 18);
	const struct tcphdr *th = (const void *)iph + sizeof(*iph);
	unsigned int tot = ntohs(iph->tot_len), i, n, first = 0, last = 0;
	static int logged;
	struct fp_snap *s = NULL;

	st.mir++;
	if (iph->version != 4 || iph->ihl != 5 || iph->protocol != IPPROTO_TCP ||
	    tot > skb->len + ETH_HLEN - 18 || fp_tcp_csum_ok(iph, tot))
		return;
	st.mir_bad++;
	for (i = 0; i < FP_SNAPS; i++)
		if (snaps[i].seq == th->seq && snaps[i].dport == th->dest &&
		    snaps[i].len == min(tot, 1536u)) {
			s = &snaps[i];
			break;
		}
	if (!s) {
		st.mir_nosnap++;
		return;
	}
	for (i = 0, n = 0; i < s->len; i++)
		if (((const u8 *)iph)[i] != s->data[i]) {
			if (!n++)
				first = i;
			last = i;
		}
	if (logged++ >= 8)
		return;
	pr_info("fastpath: mirror: %u-byte packet, %u bytes differ, IP offsets %u..%u\n",
		tot, n, first, last);
	if (n) {
		unsigned int from = first & ~15u;

		print_hex_dump(KERN_INFO, "fastpath: sent ", DUMP_PREFIX_OFFSET, 16, 1,
			       s->data + from, min(64u, s->len - from), false);
		print_hex_dump(KERN_INFO, "fastpath: wire ", DUMP_PREFIX_OFFSET, 16, 1,
			       (const u8 *)iph + from, min(64u, s->len - from), false);
	}
}

/* A few recently used flows, so bulk transfers skip the rhashtable walk. The
 * cache only lives for one receive softirq run: a tasklet (which runs right
 * after NET_RX, before any RCU quiescent state) clears it, so a cached flow
 * can never outlive the RCU read section it was looked up in.
 */
struct fp_fcache {
	__be32 saddr, daddr;
	u32 ports;
	int iif;
	u8 proto;
	const struct nf_flowtable *ft;
	struct flow_offload_tuple_rhash *th;
};
static struct fp_fcache fcache[4];
/* same for IPv6 (most of the phone's internet traffic) */
struct fp_fcache6 {
	struct in6_addr saddr, daddr;
	u32 ports;
	int iif;
	u8 proto;
	const struct nf_flowtable *ft;
	struct flow_offload_tuple_rhash *th;
};
static struct fp_fcache6 fcache6[4];
static bool fcache_armed;
static struct tasklet_struct fp_fcache_tasklet;

static void fp_fcache_clear(struct tasklet_struct *t)
{
	memset(fcache, 0, sizeof(fcache));
	memset(fcache6, 0, sizeof(fcache6));
	fcache_armed = false;
}

static void fp_fcache_arm(void)
{
	if (!fcache_armed) {
		fcache_armed = true;
		tasklet_schedule(&fp_fcache_tasklet);
	}
}

/* flow_offload_lookup() through fcache */
static struct flow_offload_tuple_rhash *
fp_lookup4(struct nf_flowtable *ft, struct flow_offload_tuple *tuple)
{
	u32 pw = (__force u32)tuple->src_port | (__force u32)tuple->dst_port << 16;
	struct fp_fcache *c = &fcache[(pw ^ (pw >> 16) ^ tuple->iifidx) & 3];
	struct flow_offload_tuple_rhash *th;

	if (c->th && c->saddr == tuple->src_v4.s_addr && c->daddr == tuple->dst_v4.s_addr &&
	    c->ports == pw && c->iif == tuple->iifidx &&
	    c->proto == tuple->l4proto && c->ft == ft) {
		st.cache_hit++;
		return c->th;
	}
	th = flow_offload_lookup(ft, tuple);
	if (th) {
		*c = (struct fp_fcache){ tuple->src_v4.s_addr, tuple->dst_v4.s_addr, pw,
					 tuple->iifidx, tuple->l4proto, ft, th };
		fp_fcache_arm();
	}
	return th;
}

/* flow_offload_lookup() for IPv6 through fcache6 */
static struct flow_offload_tuple_rhash *
fp_lookup6(struct nf_flowtable *ft, struct flow_offload_tuple *tuple)
{
	u32 pw = (__force u32)tuple->src_port | (__force u32)tuple->dst_port << 16;
	struct fp_fcache6 *c = &fcache6[(pw ^ (pw >> 16) ^ tuple->iifidx ^
					 (__force u32)tuple->src_v6.s6_addr32[3]) & 3];
	struct flow_offload_tuple_rhash *th;

	if (c->th && c->ports == pw && c->iif == tuple->iifidx &&
	    c->proto == tuple->l4proto && c->ft == ft &&
	    ipv6_addr_equal(&c->saddr, &tuple->src_v6) &&
	    ipv6_addr_equal(&c->daddr, &tuple->dst_v6)) {
		st.cache_hit++;
		return c->th;
	}
	th = flow_offload_lookup(ft, tuple);
	if (th) {
		*c = (struct fp_fcache6){ tuple->src_v6, tuple->dst_v6, pw,
					  tuple->iifidx, tuple->l4proto, ft, th };
		fp_fcache_arm();
	}
	return th;
}

/* New MACs from the flow, Broadcom tag turned into an egress-directed one. */
static void fp_retag(u8 *mac, u8 *tag, const u8 *dmac, const u8 *smac,
		     int outport)
{
	memcpy(mac, dmac, ETH_ALEN);
	memcpy(mac + ETH_ALEN, smac, ETH_ALEN);
	tag[0] = 1 << BRCM_OPCODE_SHIFT;	/* egress-directed, TC 0 */
	tag[1] = 0;
	tag[2] = outport == 8 ? 1 : 0;
	tag[3] = (1 << outport) & 0xff;
}

/*
 * Wi-Fi -> switch port, routed (uploads): a TCP/UDP flow the kernel has
 * offloaded from eth2's ingress (eth2 is in the flowtable). Same work as
 * fp_rx - NAT, TTL, MACs - but the frame arrives as plain Ethernet, so the
 * Broadcom tag is inserted in front of the EtherType. IPv6 only without NAT.
 */
static bool fp_wifi_route(struct sk_buff *skb, u8 *eth, struct net_device *in)
{
	struct flow_offload_tuple tuple = {};
	struct flow_offload_tuple_rhash *th;
	enum flow_offload_tuple_dir dir;
	struct flow_offload *flow;
	struct nf_flowtable *ft;
	__be16 etype = *(__be16 *)(eth + 2 * ETH_ALEN);
	u8 *l3 = eth + ETH_HLEN;
	unsigned int avail = skb->len - (l3 - skb->data), thoff, tot, hdrsize;
	struct ipv6hdr *ip6h = NULL;
	struct iphdr *iph = NULL;
	struct flow_ports *ports;
	int outport;
	u8 dmac[ETH_ALEN], smac[ETH_ALEN];
	u8 proto;

	if (etype == htons(ETH_P_IP)) {
		iph = (struct iphdr *)l3;
		if (avail < sizeof(*iph) + 8 || iph->version != 4 || iph->ihl != 5 ||
		    ip_is_fragment(iph) || iph->ttl <= 1)
			return false;
		proto = iph->protocol;
		thoff = sizeof(*iph);
		tot = ntohs(iph->tot_len);
	} else if (etype == htons(ETH_P_IPV6)) {
		ip6h = (struct ipv6hdr *)l3;
		if (avail < sizeof(*ip6h) + 8 || ip6h->version != 6 || ip6h->hop_limit <= 1)
			return false;
		proto = ip6h->nexthdr;
		thoff = sizeof(*ip6h);
		tot = sizeof(*ip6h) + ntohs(ip6h->payload_len);
	} else {
		return false;
	}
	if (proto == IPPROTO_TCP)
		hdrsize = sizeof(struct tcphdr);
	else if (proto == IPPROTO_UDP)
		hdrsize = sizeof(struct udphdr);
	else
		return false;
	if (tot < thoff + hdrsize || avail < tot)
		return false;

	ft = fp_flowtable(in);
	if (!ft)
		return false;
	ports = (struct flow_ports *)(l3 + thoff);
	if (iph) {
		tuple.src_v4.s_addr = iph->saddr;
		tuple.dst_v4.s_addr = iph->daddr;
		tuple.l3proto = AF_INET;
	} else {
		tuple.src_v6 = ip6h->saddr;
		tuple.dst_v6 = ip6h->daddr;
		tuple.l3proto = AF_INET6;
	}
	tuple.src_port = ports->source;
	tuple.dst_port = ports->dest;
	tuple.l4proto = proto;
	tuple.iifidx = in->ifindex;
	th = iph ? fp_lookup4(ft, &tuple) : fp_lookup6(ft, &tuple);
	if (!th)
		return false;
	dir = th->tuple.dir;
	flow = container_of(th, struct flow_offload, tuplehash[dir]);
	if (test_bit(NF_FLOW_TEARDOWN, &flow->flags) || nf_ct_is_dying(flow->ct))
		return false;
	if (iph) {
		if (!fp_out4(flow, th, dir, &outport, dmac, smac))
			return false;
	} else {
		if (th->tuple.xmit_type != FLOW_OFFLOAD_XMIT_DIRECT || th->tuple.encap_num)
			return false;
		outport = fp_port_of_ifindex(th->tuple.out.ifidx);
		memcpy(dmac, th->tuple.out.h_dest, ETH_ALEN);
		memcpy(smac, th->tuple.out.h_source, ETH_ALEN);
	}
	if (outport < 0 || outport == FP_OUT_WIFI || tot > th->tuple.mtu)
		return false;
	if (ip6h && (test_bit(NF_FLOW_SNAT, &flow->flags) || test_bit(NF_FLOW_DNAT, &flow->flags)))
		return false;
	if (proto == IPPROTO_TCP) {
		struct tcphdr *tcph = (void *)(l3 + thoff);

		if (unlikely(tcph->fin || tcph->rst)) {
			flow_offload_teardown(flow);
			st.fin_rst++;
			return false;
		}
	}
	if (unlikely(skb_cloned(skb)) || eth - skb->head < BRCM_TAG_LEN)
		return false;

	/* committed */
	flow_offload_refresh(ft, flow, false);
	if (iph) {
		skb_set_network_header(skb, l3 - skb->data);
		fp_nat_ip(flow, skb, iph, thoff, dir);
		ip_decrease_ttl(iph);
	} else {
		ip6h->hop_limit--;
	}
	if (ft->flags & NF_FLOWTABLE_COUNTER)
		nf_ct_acct_update(flow->ct, dir, tot);
	/* [new dst][new src][tag] in front of the EtherType */
	skb_push(skb, skb->data - eth + BRCM_TAG_LEN);
	fp_retag(skb->data, skb->data + 2 * ETH_ALEN, dmac, smac, outport);
	st.fwd++;
	st.fwd_bytes += tot;
	st.wifi_route_up++;
	fp_queue_conduit(skb);
	return true;
}

/* IPv6: routed, normally without NAT, so only the hop limit changes (no
 * checksum work). Flows the flowtable NATs are left to the kernel.
 */
static bool fp_rx6(struct sk_buff *skb, u8 *mac, u8 *tag, struct net_device *in)
{
	struct ipv6hdr *ip6h = (struct ipv6hdr *)(tag + BRCM_TAG_LEN + 2);
	struct net_device *wdev = NULL;
	struct flow_offload_tuple tuple = {};
	struct flow_offload_tuple_rhash *th;
	enum flow_offload_tuple_dir dir;
	struct flow_offload *flow;
	struct nf_flowtable *ft;
	struct flow_ports *ports;
	unsigned int hdrsize, tot;
	int outport;
	u8 dmac[ETH_ALEN], smac[ETH_ALEN];

	/* skb->data is 4 bytes before the IPv6 header */
	if (skb->len < 4 + sizeof(*ip6h) + sizeof(struct udphdr) ||
	    ip6h->version != 6 || ip6h->hop_limit <= 1)
		goto slow;
	switch (ip6h->nexthdr) {
	case IPPROTO_TCP:
		hdrsize = sizeof(struct tcphdr);
		break;
	case IPPROTO_UDP:
		hdrsize = sizeof(struct udphdr);
		break;
	default:
		goto slow;
	}
	tot = sizeof(*ip6h) + ntohs(ip6h->payload_len);
	if (tot < sizeof(*ip6h) + hdrsize || skb->len < 4 + tot)
		goto slow;

	ft = fp_flowtable(in);
	if (!ft) {
		st.no_ft++;
		return false;
	}
	ports = (struct flow_ports *)(ip6h + 1);
	tuple.src_v6 = ip6h->saddr;
	tuple.dst_v6 = ip6h->daddr;
	tuple.src_port = ports->source;
	tuple.dst_port = ports->dest;
	tuple.l3proto = AF_INET6;
	tuple.l4proto = ip6h->nexthdr;
	tuple.iifidx = in->ifindex;

	th = fp_lookup6(ft, &tuple);
	if (!th) {
		st.no_flow++;
		return false;
	}
	dir = th->tuple.dir;
	flow = container_of(th, struct flow_offload, tuplehash[dir]);
	/* a cached flow may have been torn down since (FIN/RST, ct death) */
	if (unlikely(test_bit(NF_FLOW_TEARDOWN, &flow->flags) || nf_ct_is_dying(flow->ct))) {
		st.no_flow++;
		return false;
	}
	if (th->tuple.xmit_type != FLOW_OFFLOAD_XMIT_DIRECT || th->tuple.encap_num) {
		st.not_direct++;
		return false;
	}
	if (test_bit(NF_FLOW_SNAT, &flow->flags) || test_bit(NF_FLOW_DNAT, &flow->flags)) {
		st.v6_nat++;
		return false;
	}
	outport = fp_port_of_ifindex(th->tuple.out.ifidx);
	memcpy(dmac, th->tuple.out.h_dest, ETH_ALEN);
	memcpy(smac, th->tuple.out.h_source, ETH_ALEN);
	if (outport < 0) {
		st.other_out++;
		return false;
	}
	if (tot > th->tuple.mtu) {
		st.mtu++;
		return false;
	}
	if (ip6h->nexthdr == IPPROTO_TCP) {
		struct tcphdr *tcph = (void *)(ip6h + 1);

		if (unlikely(tcph->fin || tcph->rst)) {
			flow_offload_teardown(flow);
			st.fin_rst++;
			return false;
		}
	}
	if (unlikely(skb_cloned(skb)))
		goto slow;
	if (outport == FP_OUT_WIFI && !(wdev = fp_wifi_dev(th))) {
		st.other_out++;
		return false;
	}

	/* committed */
	flow_offload_refresh(ft, flow, false);
	ip6h->hop_limit--;
	if (ft->flags & NF_FLOWTABLE_COUNTER)
		nf_ct_acct_update(flow->ct, dir, tot);
	st.fwd++;
	st.fwd6++;
	st.fwd_bytes += tot;
	if (wdev) {
		fp_to_wifi(skb, mac, th, wdev);
		return true;
	}
	fp_retag(mac, tag, dmac, smac, outport);

	skb_push(skb, ETH_HLEN);
	*(u32 *)skb->cb = FP_CB_INPLACE;
	fp_xmit(skb);
	return true;
slow:
	st.not_ip++;
	return false;
}

/* Called from bgmac's NAPI poll with skb->data just past the 14 bytes
 * eth_type_trans() consumed - i.e. 2 bytes into the Broadcom tag, which sits
 * between the source MAC and the real EtherType.
 */
static bool fp_rx(struct sk_buff *skb)
{
	struct net_device *wdev = NULL;
	struct flow_offload_tuple tuple = {};
	struct flow_offload_tuple_rhash *th;
	enum flow_offload_tuple_dir dir;
	struct flow_offload *flow;
	struct nf_flowtable *ft;
	struct net_device *in;
	unsigned int thoff, hdrsize, tot;
	struct flow_ports *ports;
	struct iphdr *iph;
	u8 *mac, *tag;
	int srcport, outport;
	u8 dmac[ETH_ALEN], smac[ETH_ALEN];
	__be16 etype;

	if (!READ_ONCE(enabled) || skb->dev != conduit)
		return false;
	fp_kick_arm();

	mac = skb_mac_header(skb);
	tag = mac + 2 * ETH_ALEN;
	/* tag(4) + EtherType(2) + IPv4(20) + ports(4), counted from the MAC */
	etype = *(__be16 *)(tag + BRCM_TAG_LEN);
	if (skb->len + ETH_HLEN < 2 * ETH_ALEN + BRCM_TAG_LEN + 2 + sizeof(*iph) + 4 ||
	    (etype != htons(ETH_P_IP) && etype != htons(ETH_P_IPV6)) ||
	    ((tag[0] >> BRCM_OPCODE_SHIFT) & 7) || (tag[2] & BRCM_EG_RC_RSVD)) {
		st.not_ip++;
		return false;
	}

	/* mirrored copies of our own frames: never forward them */
	if (unlikely(snaps) && ((tag[2] & BRCM_EG_RC_MIRROR) || fp_is_our_mac(mac + ETH_ALEN))) {
		fp_mirror_check(skb, mac);
		kfree_skb(skb);
		return true;
	}

	srcport = tag[3] & BRCM_EG_PID_MASK;
	in = srcport < FP_PORTS ? port_dev[srcport] : NULL;
	if (!in) {
		st.no_port++;
		return false;
	}
	fp_host_learn(mac + ETH_ALEN, srcport);
	if (!is_multicast_ether_addr(mac) && fp_bridge_wifi(skb, mac, in))
		return true;
	if (etype == htons(ETH_P_IPV6))
		return fp_rx6(skb, mac, tag, in);

	iph = (struct iphdr *)(tag + BRCM_TAG_LEN + 2);
	if (iph->version != 4 || iph->ihl != 5 || ip_is_fragment(iph) || iph->ttl <= 1) {
		st.not_ip++;
		return false;
	}
	switch (iph->protocol) {
	case IPPROTO_TCP:
		hdrsize = sizeof(struct tcphdr);
		break;
	case IPPROTO_UDP:
		hdrsize = sizeof(struct udphdr);
		break;
	default:
		st.not_ip++;
		return false;
	}
	thoff = sizeof(*iph);
	tot = ntohs(iph->tot_len);
	/* skb->data is 4 bytes before the IP header */
	if (tot < thoff + hdrsize || skb->len < 4 + tot) {
		st.not_ip++;
		return false;
	}

	ft = fp_flowtable(in);
	if (!ft) {
		st.no_ft++;
		return false;
	}

	ports = (struct flow_ports *)((void *)iph + thoff);
	tuple.src_v4.s_addr = iph->saddr;
	tuple.dst_v4.s_addr = iph->daddr;
	tuple.src_port = ports->source;
	tuple.dst_port = ports->dest;
	tuple.l3proto = AF_INET;
	tuple.l4proto = iph->protocol;
	tuple.iifidx = in->ifindex;

	th = fp_lookup4(ft, &tuple);
	if (!th) {
		st.no_flow++;
		if (unlikely(noflow_log > 0)) {
			struct net_device *br = netdev_master_upper_dev_get_rcu(in);
			struct flow_offload_tuple_rhash *alt = NULL;

			noflow_log--;
			if (br) {
				tuple.iifidx = br->ifindex;
				alt = flow_offload_lookup(ft, &tuple);
				tuple.iifidx = in->ifindex;
			}
			pr_info("fastpath: no flow %pI4:%u -> %pI4:%u proto %u iif %s len %u flags %02x via %s: %s xmit %d out %d\n",
				&iph->saddr, ntohs(ports->source), &iph->daddr, ntohs(ports->dest),
				iph->protocol, in->name, tot,
				iph->protocol == IPPROTO_TCP ? ((u8 *)iph)[thoff + 13] : 0,
				br ? br->name : "-", alt ? "FOUND" : "none",
				alt ? alt->tuple.xmit_type : -1, alt ? alt->tuple.out.ifidx : -1);
		}
		return false;
	}
	dir = th->tuple.dir;
	flow = container_of(th, struct flow_offload, tuplehash[dir]);
	/* a cached flow may have been torn down since (FIN/RST, ct death) */
	if (unlikely(test_bit(NF_FLOW_TEARDOWN, &flow->flags) || nf_ct_is_dying(flow->ct))) {
		st.no_flow++;
		return false;
	}

	if (!fp_out4(flow, th, dir, &outport, dmac, smac)) {
		st.not_direct++;
		return false;
	}
	if (outport < 0) {
		st.other_out++;
		return false;
	}
	if (tot > th->tuple.mtu) {
		st.mtu++;
		return false;
	}
	if (iph->protocol == IPPROTO_TCP) {
		struct tcphdr *tcph = (void *)iph + thoff;

		if (unlikely(tcph->fin || tcph->rst)) {
			flow_offload_teardown(flow);
			st.fin_rst++;
			return false;
		}
	}
	if (unlikely(skb_cloned(skb))) {
		st.not_ip++;
		return false;
	}
	if (outport == FP_OUT_WIFI && !(wdev = fp_wifi_dev(th))) {
		st.other_out++;
		return false;
	}

	if (unlikely(verify) && iph->protocol == IPPROTO_TCP) {
		st.checked++;
		if (!fp_tcp_csum_ok(iph, tot)) {
			st.bad_rx++;
			fp_dump_bad("on arrival", mac, 18 + tot);
		}
	}

	/* committed: from here on the packet is ours */
	flow_offload_refresh(ft, flow, false);
	if (unlikely(noflow_log > 0)) {
		noflow_log--;
		pr_info("fastpath: hit %pI4:%u -> %pI4:%u iif %s\n", &iph->saddr,
			ntohs(ports->source), &iph->daddr, ntohs(ports->dest), in->name);
	}

	skb_set_network_header(skb, (u8 *)iph - skb->data);
	fp_nat_ip(flow, skb, iph, thoff, dir);
	ip_decrease_ttl(iph);
	if (unlikely(verify) && iph->protocol == IPPROTO_TCP &&
	    !fp_tcp_csum_ok(iph, tot)) {
		st.bad_tx++;
		fp_dump_bad("after NAT", mac, 18 + tot);
	}
	if (ft->flags & NF_FLOWTABLE_COUNTER)
		nf_ct_acct_update(flow->ct, dir, tot);

	st.fwd++;
	st.fwd_bytes += tot;
	if (wdev) {
		fp_to_wifi(skb, mac, th, wdev);
		return true;
	}
	fp_retag(mac, tag, dmac, smac, outport);

	skb_push(skb, ETH_HLEN);
	*(u32 *)skb->cb = (!copy_tx && !touch) ? FP_CB_INPLACE : 0;
	if (unlikely(touch) && !copy_tx) {
		/* debug: make the CPU own every line of the frame, so the DMA map
		 * writes it all back before the TX DMA reads it */
		u32 *w = (u32 *)PTR_ALIGN(skb->data, 4), *end = (u32 *)(skb->data + skb->len);

		for (; w + 1 <= end; w += 8)
			WRITE_ONCE(*w, READ_ONCE(*w));
	}
	if (unlikely(snaps) && outport == mirror_port && iph->protocol == IPPROTO_TCP)
		fp_snap_store(iph, tot);
	if (unlikely(memcheck) && !copy_tx && iph->protocol == IPPROTO_TCP)
		skb->destructor = fp_memcheck_destructor;
	if (copy_tx == 3) {
		/* frames up to 192 bytes are covered by the header lines the
		 * CPU rewrote; longer ones get their tail from a fresh copy */
		unsigned int t = min_t(unsigned int, tail_copy, skb->len - 64);
		void *frag;

		if (skb->len > 192 && t && !skb_shinfo(skb)->nr_frags &&
		    (frag = netdev_alloc_frag(t))) {
			struct page *page = virt_to_head_page(frag);

			memcpy(frag, skb->data + skb->len - t, t);
			skb_set_tail_pointer(skb, skb->len - t);
			skb_fill_page_desc(skb, 0, page, frag - page_address(page), t);
			skb->data_len = t;
			skb->truesize += t;
			st.tail_copied++;
		}
	} else if (copy_tx) {
		struct sk_buff *n = netdev_alloc_skb(conduit, skb->len + 2);

		if (n) {
			if (copy_tx == 2)
				skb_reserve(n, 2);
			skb_put_data(n, skb->data, skb->len);
			consume_skb(skb);
			skb = n;
		}
	}
	if (batch) {
		__skb_queue_tail(&fp_txq, skb);
		if (skb_queue_len(&fp_txq) == 1)
			tasklet_schedule(&fp_tx_tasklet);
	} else {
		fp_xmit(skb);
	}
	return true;
}

static int fp_show(struct seq_file *m, void *v)
{
	int p;

	seq_printf(m, "enabled %d conduit %s\nports", enabled,
		   conduit ? conduit->name : "-");
	for (p = 0; p < FP_PORTS; p++)
		if (port_dev[p])
			seq_printf(m, " %d=%s", p, port_dev[p]->name);
	seq_printf(m, "\nforwarded %llu packets %llu bytes\n", st.fwd, st.fwd_bytes);
	seq_printf(m, "flow cache hits %llu\n", st.cache_hit);
	seq_printf(m, "ipv6 forwarded %llu, left to kernel (NAT) %llu\n", st.fwd6, st.v6_nat);
	seq_printf(m, "slow path: not_ipv4_tcp_udp %llu no_port %llu no_flowtable %llu no_flow %llu not_direct %llu other_out %llu fin_rst %llu mtu %llu\n",
		   st.not_ip, st.no_port, st.no_ft, st.no_flow, st.not_direct,
		   st.other_out, st.fin_rst, st.mtu);
	seq_printf(m, "neighbour (routed out, e.g. upload): resolved %llu missed %llu\n",
		   st.neigh, st.neigh_miss);
	seq_printf(m, "tx: ring_busy %llu (qdisc dropped %llu) xmit_err %llu\n",
		   st.busy, st.busy_drop, st.xmit_err);
	seq_printf(m, "wifi ifindex %d %d: routed %llu bridged %llu up %llu routed_up %llu (shortcut %d) queued_to_qdisc %llu\n",
		   wifi_ifindex[0], wifi_ifindex[1], st.wifi, st.bridged, st.wifi_up,
		   st.wifi_route_up, bridge_wifi, st.wifi_busy);
	seq_printf(m, "verify %d: checked %llu bad_on_arrival %llu bad_after_nat %llu\n",
		   verify, st.checked, st.bad_rx, st.bad_tx);
	seq_printf(m, "batch %d\n", batch);
	seq_printf(m, "copy_tx %d (tail_copy %d, tail-copied %llu)\n", copy_tx, tail_copy, st.tail_copied);
	seq_printf(m, "touch %d\n", touch);
	seq_printf(m, "mirror %d (port %d): seen %llu bad %llu no_copy %llu\n",
		   !!snaps, mirror_port, st.mir, st.mir_bad, st.mir_nosnap);
	seq_printf(m, "memcheck %d: checked %llu bad_in_dram_at_tx_done %llu\n",
		   memcheck, st.mem_checked, st.mem_bad);
	return 0;
}

static int fp_open(struct inode *inode, struct file *file)
{
	return single_open(file, fp_show, NULL);
}

/* bgmac only invalidates the head of each receive buffer while we are hooked
 * in; debug modes that read or rewrite whole frames need the full invalidate.
 */
static void fp_update_lazy(void)
{
	WRITE_ONCE(bgmac_rx_lazy_sync, !(verify || copy_tx || touch || snaps));
}

static ssize_t fp_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	char c;

	if (len < 1 || get_user(c, ubuf))
		return -EFAULT;
	if (c == '0' || c == '1')
		WRITE_ONCE(enabled, c == '1');
	else if (c == 'r')
		memset(&st, 0, sizeof(st));
	else if (c == 'v' || c == 'V')
		WRITE_ONCE(verify, c == 'v');
	else if (c == 'k' || c == 'K')
		WRITE_ONCE(copy_tx, c == 'k' ? 1 : 0);
	else if (c == 'a')
		WRITE_ONCE(copy_tx, 2);
	else if (c == 'h')
		WRITE_ONCE(copy_tx, 3);
	else if (c == 'b' || c == 'B')
		WRITE_ONCE(batch, c == 'b');
	else if (c == 'w' || c == 'W')
		WRITE_ONCE(bridge_wifi, c == 'w');
	else if (c == 'm' || c == 'M')
		WRITE_ONCE(memcheck, c == 'm');
	else if (c == 't' || c == 'T')
		WRITE_ONCE(touch, c == 't');
	else if (c == 'x' && !snaps) {
		struct fp_snap *p = vzalloc(sizeof(*p) * FP_SNAPS);

		WRITE_ONCE(snaps, p);
	} else if (c == 'X' && snaps) {
		struct fp_snap *p = snaps;

		WRITE_ONCE(snaps, NULL);
		synchronize_net();
		vfree(p);
	}
	fp_update_lazy();
	return len;
}

static const struct proc_ops fp_proc_ops = {
	.proc_open	= fp_open,
	.proc_read	= seq_read,
	.proc_write	= fp_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static void fp_put_devs(void)
{
	int p;

	for (p = 0; p < FP_PORTS; p++) {
		if (port_dev[p])
			dev_put(port_dev[p]);
		port_dev[p] = NULL;
	}
	if (conduit)
		dev_put(conduit);
	conduit = NULL;
}

static void fp_unhook(void)
{
	if (rcu_access_pointer(bgmac_rx_hook) == fp_rx) {
		RCU_INIT_POINTER(bgmac_rx_hook, NULL);
		synchronize_net();
		tasklet_kill(&fp_tx_tasklet);
		tasklet_kill(&fp_fcache_tasklet);
		__skb_queue_purge(&fp_txq);
	}
	hrtimer_cancel(&fp_kick);
}

/* Any of our devices going away (e.g. b53 unbound) ends the fast path. */
static int fp_netdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (dev_net(dev) == &init_net && fp_is_wl(dev)) {
		int i;

		if (event == NETDEV_REGISTER) {
			for (i = 0; i < FP_WIFI_MAX; i++)
				if (!wifi_ifindex[i]) {
					WRITE_ONCE(wifi_ifindex[i], dev->ifindex);
					pr_info("fastpath: direct to Wi-Fi via %s\n", dev->name);
					break;
				}
		} else if (event == NETDEV_UNREGISTER) {
			for (i = 0; i < FP_WIFI_MAX; i++)
				if (wifi_ifindex[i] == dev->ifindex)
					WRITE_ONCE(wifi_ifindex[i], 0);
			/* send what is queued while the device still exists */
			local_bh_disable();
			fp_tx_flush(NULL);
			local_bh_enable();
		}
		return NOTIFY_DONE;
	}
	if (event != NETDEV_UNREGISTER)
		return NOTIFY_DONE;
	if (dev != conduit && fp_port_of_ifindex(dev->ifindex) < 0)
		return NOTIFY_DONE;
	pr_warn("fastpath: %s is going away, fast path off\n", dev->name);
	fp_unhook();
	fp_put_devs();
	return NOTIFY_DONE;
}

static struct notifier_block fp_nb = {
	.notifier_call = fp_netdev_event,
};

static int __init fp_init(void)
{
	struct net_device *dev;
	int n = 0, ret;

	conduit = dev_get_by_name(&init_net, conduit_name);
	if (!conduit) {
		pr_err("fastpath: no %s\n", conduit_name);
		return -ENODEV;
	}

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		struct dsa_port *dp;

		if (!dsa_user_dev_check(dev))
			continue;
		dp = dsa_port_from_netdev(dev);
		if (IS_ERR(dp) || dp->index >= FP_PORTS || !dp->cpu_dp ||
		    dp->cpu_dp->conduit != conduit)
			continue;
		dev_hold(dev);
		port_dev[dp->index] = dev;
		n++;
	}
	rtnl_unlock();
	if (!n) {
		pr_err("fastpath: no switch ports behind %s\n", conduit_name);
		fp_put_devs();
		return -ENODEV;
	}
	if (rcu_access_pointer(bgmac_rx_hook)) {
		pr_err("fastpath: bgmac_rx_hook already taken\n");
		fp_put_devs();
		return -EBUSY;
	}

	skb_queue_head_init(&fp_txq);
	tasklet_setup(&fp_tx_tasklet, fp_tx_flush);
	hrtimer_init(&fp_kick, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	fp_kick.function = fp_kick_fn;
	ret = register_netdevice_notifier(&fp_nb);
	if (ret) {
		fp_put_devs();
		return ret;
	}
	if (!proc_create("fastpath", 0600, NULL, &fp_proc_ops)) {
		unregister_netdevice_notifier(&fp_nb);
		fp_put_devs();
		return -ENOMEM;
	}
	tasklet_setup(&fp_fcache_tasklet, fp_fcache_clear);
	rcu_assign_pointer(bgmac_rx_hook, fp_rx);
	pr_info("fastpath: on for %d switch ports behind %s\n", n, conduit_name);
	return 0;
}

static void __exit fp_exit(void)
{
	fp_unhook();
	vfree(snaps);
	remove_proc_entry("fastpath", NULL);
	unregister_netdevice_notifier(&fp_nb);
	fp_put_devs();
}

module_init(fp_init);
module_exit(fp_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CTF-like NAT fast path for bgmac + Broadcom-tagged DSA");
