/*
 * compat.c - 2.6.36 kernel ABI for the prebuilt wl_apsta.o, on Linux 6.12.
 *
 * The blob is renamed at build time so every kernel symbol it imports becomes
 * wlc26_<name> (see mkblob.sh); each of those is implemented here. Structures
 * the blob embeds (timer_list, tasklet_struct, work_struct, semaphore, sk_buff,
 * net_device, pci_dev) still have their 2.6.36 layouts, so where the sizes
 * differ we keep a side table mapping the blob's object to a real 6.12 one.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/hash.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/skbuff.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/if_arp.h>
#include <linux/scatterlist.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/unaligned.h>
#include "k26.h"

/* ------------------------------------------------------------------ */
/* The blob was compiled !SMP: its spinlocks are zero-sized and compile	*/
/* away, so spin_lock_bh(&wl->lock) became a bare local_bh_disable().	*/
/* This SoC is single-core, so that is still exactly the right mutual	*/
/* exclusion - and the real thing nests, which an added lock would not.	*/
/* ------------------------------------------------------------------ */
void wlc26_local_bh_disable(void)
{
	local_bh_disable();
}

void wlc26_local_bh_enable(void)
{
	local_bh_enable();
}

/* ------------------------------------------------------------------ */
/* Generic blob-pointer -> shim-object side table.			*/
/* ------------------------------------------------------------------ */
struct side {
	struct hlist_node node;
	void *key;
	void *obj;
};

#define SIDE_BITS 6
static struct hlist_head side_tab[1 << SIDE_BITS];
static DEFINE_SPINLOCK(side_lock);

static void *side_find(void *key)
{
	struct side *s;
	unsigned long f;
	void *r = NULL;

	spin_lock_irqsave(&side_lock, f);
	hlist_for_each_entry(s, &side_tab[hash_ptr(key, SIDE_BITS)], node)
		if (s->key == key) {
			r = s->obj;
			break;
		}
	spin_unlock_irqrestore(&side_lock, f);
	return r;
}

static int side_add(void *key, void *obj)
{
	struct side *s = kzalloc(sizeof(*s), GFP_ATOMIC);
	unsigned long f;

	if (!s)
		return -ENOMEM;
	s->key = key;
	s->obj = obj;
	spin_lock_irqsave(&side_lock, f);
	hlist_add_head(&s->node, &side_tab[hash_ptr(key, SIDE_BITS)]);
	spin_unlock_irqrestore(&side_lock, f);
	return 0;
}

static void *side_del(void *key)
{
	struct side *s;
	unsigned long f;
	void *r = NULL;

	spin_lock_irqsave(&side_lock, f);
	hlist_for_each_entry(s, &side_tab[hash_ptr(key, SIDE_BITS)], node)
		if (s->key == key) {
			r = s->obj;
			hlist_del(&s->node);
			kfree(s);
			break;
		}
	spin_unlock_irqrestore(&side_lock, f);
	return r;
}

/* ------------------------------------------------------------------ */
/* Semaphores: the 2.6.36 struct is 12 bytes (spinlock_t is empty on	*/
/* !SMP), 6.12 wants 16. sema_init() is inlined in the blob and writes	*/
/* the count at offset 0, so we seed the real semaphore from it.	*/
/* ------------------------------------------------------------------ */
static struct semaphore *sem_for(void *blob)
{
	struct semaphore *s = side_find(blob);

	if (s)
		return s;
	s = kzalloc(sizeof(*s), GFP_ATOMIC);
	if (!s)
		return NULL;
	sema_init(s, (int)k26_rd32(blob, 0));
	if (side_add(blob, s)) {
		kfree(s);
		return NULL;
	}
	return s;
}

void wlc26_down(void *blob)
{
	struct semaphore *s = sem_for(blob);

	if (s)
		down(s);
}

void wlc26_up(void *blob)
{
	struct semaphore *s = sem_for(blob);

	if (s)
		up(s);
}

/* ------------------------------------------------------------------ */
/* Timers: 2.6.36 timer_list is 28 bytes with a void(*)(unsigned long)	*/
/* callback; 6.12 is 20 and passes the timer itself. The blob writes	*/
/* .function/.data/.expires inline, so we read them back at fire time.	*/
/* ------------------------------------------------------------------ */
struct k26_timer {
	struct timer_list t;
	void *blob;
};

static atomic_t k26_timer_fires = ATOMIC_INIT(0);
static atomic_t k26_timer_adds = ATOMIC_INIT(0);

static void k26_timer_fire(struct timer_list *t)
{
	struct k26_timer *kt = from_timer(kt, t, t);
	void (*fn)(unsigned long) = k26_rdp(kt->blob, K26_TIMER_FUNCTION);
	unsigned long data = k26_rd32(kt->blob, K26_TIMER_DATA);
	int n = atomic_inc_return(&k26_timer_fires);

	if (n <= 5)
		pr_info("wl-dbg: timer fire #%d blob=%p fn=%p\n", n, kt->blob, fn);

	if (fn)
		fn(data);
}

static struct k26_timer *timer_for(void *blob)
{
	struct k26_timer *kt = side_find(blob);

	if (kt)
		return kt;
	kt = kzalloc(sizeof(*kt), GFP_ATOMIC);
	if (!kt)
		return NULL;
	kt->blob = blob;
	timer_setup(&kt->t, k26_timer_fire, 0);
	if (side_add(blob, kt)) {
		kfree(kt);
		return NULL;
	}
	return kt;
}

void wlc26_init_timer_key(void *blob, const char *name, void *key)
{
	timer_for(blob);
}

void wlc26_add_timer(void *blob)
{
	struct k26_timer *kt = timer_for(blob);
	unsigned long exp = (unsigned long)k26_rd32(blob, K26_TIMER_EXPIRES);
	int n = atomic_inc_return(&k26_timer_adds);

	if (n <= 5)
		pr_info("wl-dbg: timer add #%d blob=%p exp=%lu now=%lu delta=%ld\n",
			n, blob, exp, jiffies, (long)(exp - jiffies));

	if (kt)
		mod_timer(&kt->t, exp);
}

int wlc26_del_timer(void *blob)
{
	struct k26_timer *kt = side_find(blob);

	return kt ? timer_delete(&kt->t) : 0;
}

/* ------------------------------------------------------------------ */
/* Tasklets: 2.6.36 struct is 20 bytes, 6.12 is larger.			*/
/* ------------------------------------------------------------------ */
struct k26_tasklet {
	struct tasklet_struct t;
	void *blob;
};

static atomic_t k26_tasklet_runs = ATOMIC_INIT(0);
static atomic_t k26_tasklet_scheds = ATOMIC_INIT(0);

static void k26_tasklet_run(struct tasklet_struct *t)
{
	struct k26_tasklet *kt = container_of(t, struct k26_tasklet, t);
	void (*fn)(unsigned long) = k26_rdp(kt->blob, K26_TASKLET_FUNC);
	unsigned long data = k26_rd32(kt->blob, K26_TASKLET_DATA);

	/*
	 * tasklet_schedule() is inlined in the blob as
	 *	if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state))
	 *		__tasklet_schedule(t);
	 * against its own struct. Real Linux clears that bit as the tasklet
	 * runs; our trampoline runs a different struct, so we must clear it
	 * here or the blob only ever schedules its DPC once - which leaves
	 * device interrupts masked forever and RX dead.
	 */
	clear_bit(TASKLET_STATE_SCHED,
		  (unsigned long *)((char *)kt->blob + K26_TASKLET_STATE));

	if (fn)
		fn(data);
}

static struct k26_tasklet *tasklet_for(void *blob)
{
	struct k26_tasklet *kt = side_find(blob);

	if (kt)
		return kt;
	kt = kzalloc(sizeof(*kt), GFP_ATOMIC);
	if (!kt)
		return NULL;
	kt->blob = blob;
	tasklet_setup(&kt->t, k26_tasklet_run);
	if (side_add(blob, kt)) {
		kfree(kt);
		return NULL;
	}
	return kt;
}

void wlc26_tasklet_init(void *blob, void (*func)(unsigned long), unsigned long data)
{
	k26_wrp(blob, K26_TASKLET_FUNC, func);
	k26_wr32(blob, K26_TASKLET_DATA, (u32)data);
	pr_info("wl-dbg: tasklet_init blob=%p func=%p data=%lx\n", blob, func, data);
	tasklet_for(blob);
}

void wlc26___tasklet_schedule(void *blob)
{
	struct k26_tasklet *kt = tasklet_for(blob);
	int n = atomic_inc_return(&k26_tasklet_scheds);

	if (n <= 3)
		pr_info("wl-dbg: tasklet sched #%d blob=%p kt=%p\n", n, blob, kt);

	if (kt)
		tasklet_schedule(&kt->t);
}

void wlc26_tasklet_kill(void *blob)
{
	struct k26_tasklet *kt = side_del(blob);

	if (kt) {
		tasklet_kill(&kt->t);
		kfree(kt);
	}
}

/* ------------------------------------------------------------------ */
/* Work items. The blob's handler container_of()s out of its own	*/
/* work_struct, so we must hand it that pointer, never ours.		*/
/* ------------------------------------------------------------------ */
struct k26_work {
	struct delayed_work dw;
	void *blob;
};

static void k26_work_run(struct work_struct *w)
{
	struct k26_work *kw = container_of(to_delayed_work(w), struct k26_work, dw);
	void (*fn)(void *) = k26_rdp(kw->blob, K26_WORK_FUNC);

	/* same inlined-bit problem as tasklets: queue_work() tests and sets
	 * WORK_STRUCT_PENDING_BIT in the blob's own work_struct.data */
	clear_bit(WORK_STRUCT_PENDING_BIT, (unsigned long *)kw->blob);

	if (fn)
		fn(kw->blob);
}

static struct k26_work *work_for(void *blob)
{
	struct k26_work *kw = side_find(blob);

	if (kw)
		return kw;
	kw = kzalloc(sizeof(*kw), GFP_ATOMIC);
	if (!kw)
		return NULL;
	kw->blob = blob;
	INIT_DELAYED_WORK(&kw->dw, k26_work_run);
	if (side_add(blob, kw)) {
		kfree(kw);
		return NULL;
	}
	return kw;
}

int wlc26_schedule_work(void *blob)
{
	struct k26_work *kw = work_for(blob);

	return kw ? schedule_delayed_work(&kw->dw, 0) : 0;
}

int wlc26_schedule_delayed_work(void *blob, unsigned long delay)
{
	struct k26_work *kw = work_for(blob);

	return kw ? schedule_delayed_work(&kw->dw, delay) : 0;
}

bool wlc26_flush_delayed_work(void *blob)
{
	struct k26_work *kw = side_find(blob);

	return kw ? flush_delayed_work(&kw->dw) : false;
}

void wlc26_flush_scheduled_work(void)
{
	flush_scheduled_work();
}

/* ------------------------------------------------------------------ */
/* Packets. The blob and the recompiled osl both see 2.6.36-layout	*/
/* sk_buffs; a real 6.12 skb rides along owning the data buffer, so	*/
/* nothing is copied at the netdev boundary.				*/
/* ------------------------------------------------------------------ */
static void k26_skb_mirror(struct k26_skb *p, struct sk_buff *r)
{
	p->real = r;
	p->magic = K26_SKB_MAGIC;
	p->head = r->head;
	p->data = r->data;
	p->tail = skb_tail_pointer(r);
	p->end = skb_end_pointer(r);
	p->len = r->len;
	p->truesize = r->truesize;
	atomic_set(&p->users, 1);
}

/*
 * Every packet needs a k26_skb shell, and the slab round trip for it showed
 * up in the profile. Freed shells go on a LIFO list instead; single core, so
 * irqs off is all the locking needed (osl_pktfree runs in any context).
 */
#define K26_SHELL_POOL	512
static struct k26_skb *k26_shells;
static unsigned int k26_nshells;

/* /proc/wl-shim counters */
static unsigned long k26_st_tx, k26_st_chains, k26_st_chained, k26_st_maxchain;
static unsigned long k26_st_alloc, k26_st_shell_miss, k26_st_short;

static struct k26_skb *k26_shell_get(void)
{
	struct k26_skb *p;
	unsigned long f;

	local_irq_save(f);
	p = k26_shells;
	if (p) {
		k26_shells = p->next;
		k26_nshells--;
	}
	local_irq_restore(f);
	if (!p) {
		k26_st_shell_miss++;
		return kzalloc(sizeof(*p), GFP_ATOMIC);
	}
	memset(p, 0, sizeof(*p));
	return p;
}

static void k26_shell_put(struct k26_skb *p)
{
	unsigned long f;

	local_irq_save(f);
	if (k26_nshells < K26_SHELL_POOL) {
		p->next = k26_shells;
		k26_shells = p;
		k26_nshells++;
		p = NULL;
	}
	local_irq_restore(f);
	kfree(p);
}

static void k26_shells_drain(void)
{
	struct k26_skb *p;

	while ((p = k26_shells)) {
		k26_shells = p->next;
		kfree(p);
	}
	k26_nshells = 0;
}

struct k26_skb *k26_skb_alloc(unsigned int len)
{
	struct sk_buff *r;
	struct k26_skb *p;

	r = __netdev_alloc_skb(NULL, len, GFP_ATOMIC);
	if (!r)
		return NULL;
	p = k26_shell_get();
	if (!p) {
		dev_kfree_skb_any(r);
		return NULL;
	}
	k26_skb_mirror(p, r);
	return p;
}
EXPORT_SYMBOL(k26_skb_alloc);

/*
 * wlc_hdr_proc() builds the d11/802.11 headers in front of the frame when it
 * has more than 191 bytes of headroom (and is not cloned); otherwise it gets
 * a separate 192-byte header buffer from osl_pktget() for every frame and
 * the DMA sends two pieces. Chained frames after the first skip that check
 * (wlc_pktc_sdu_prep() just pushes in place), so they must have the room.
 */
#define K26_TXOFF	192
static unsigned int txheadroom = K26_TXOFF + 8;
module_param(txheadroom, uint, 0644);
MODULE_PARM_DESC(txheadroom, "headroom the shim guarantees on transmit (copies if short)");

/* Wrap a real skb for handing down into the blob (transmit path). */
struct k26_skb *k26_skb_from_real(struct sk_buff *r)
{
	struct k26_skb *p;

	/* the blob prepends headers in place, so guarantee headroom first */
	if (skb_cow_head(r, max(READ_ONCE(txheadroom), 64u)))
		return NULL;
	p = k26_shell_get();
	if (!p)
		return NULL;
	k26_skb_mirror(p, r);
	p->priority = r->priority;
	p->protocol = r->protocol;
	return p;
}

void k26_skb_sync_to_real(struct k26_skb *p)
{
	struct sk_buff *r = p->real;

	if (!r)
		return;
	r->data = p->data;
	r->len = p->len;
	skb_set_tail_pointer(r, p->len);
	r->priority = p->priority;
	r->protocol = p->protocol;
}

void k26_skb_free(struct k26_skb *p)
{
	if (!p)
		return;
	if (p->real)
		dev_kfree_skb_any(p->real);
	p->magic = 0;
	k26_shell_put(p);
}
EXPORT_SYMBOL(k26_skb_free);

/* Duplicate, used by osl_pktdup(). Copies, which the fast path avoids. */
struct k26_skb *k26_skb_dup(struct k26_skb *p)
{
	struct k26_skb *n = k26_skb_alloc(p->len + 128);

	if (!n)
		return NULL;
	skb_reserve(n->real, 64);
	n->data = n->real->data;
	n->tail = n->data;
	memcpy(wlc26_skb_put(n, p->len), p->data, p->len);
	memcpy(n->cb, p->cb, sizeof(n->cb));
	n->priority = p->priority;
	return n;
}
EXPORT_SYMBOL(k26_skb_dup);

void *wlc26_dev_alloc_skb(unsigned int len)
{
	k26_st_alloc++;
	return k26_skb_alloc(len);
}

unsigned char *wlc26_skb_put(struct k26_skb *p, unsigned int len)
{
	unsigned char *tail = p->tail;

	p->tail += len;
	p->len += len;
	return tail;
}
EXPORT_SYMBOL(wlc26_skb_put);

unsigned char *wlc26_skb_push(struct k26_skb *p, unsigned int len)
{
	p->data -= len;
	p->len += len;
	return p->data;
}

unsigned char *wlc26_skb_pull(struct k26_skb *p, unsigned int len)
{
	if (len > p->len)
		return NULL;
	p->len -= len;
	p->data += len;
	return p->data;
}

/* ------------------------------------------------------------------ */
/* Net devices. The blob gets a 768-byte 2.6.36 net_device; we pair it	*/
/* with a real one whose ndo_* trampoline into the blob's ops.		*/
/* ------------------------------------------------------------------ */
struct k26_ndev {
	struct list_head list;
	void *k26;			/* blob-visible net_device + priv */
	struct net_device *real;
	struct netdev_queue txq;	/* storage for k26 dev->_tx */
	void *mc_nodes;			/* 2.6.36 netdev_hw_addr array for mc */
	unsigned int mc_cap;
	bool registered;
	/* transmit chain being built (Broadcom PKTC), see tramp_xmit() */
	struct k26_skb *ch_head, *ch_tail;
	unsigned int ch_n, ch_bytes;
	struct tasklet_struct txflush;
};

/*
 * 2.6.36 struct netdev_hw_addr. Only the list link and the address are read
 * by the driver, but the whole thing has to be the right size because the
 * list is walked with list_for_each_entry.
 */
struct k26_hw_addr {
	struct list_head list;		/*  0 */
	unsigned char addr[32];		/*  8 */
	unsigned char type;		/* 40 */
	bool synced;			/* 41 */
	bool global_use;		/* 42 */
	int refcount;			/* 44 */
	unsigned long rcu_head[2];	/* 48 */
};					/* 56 */

/* An empty struct netdev_hw_addr_list at the given offset. */
static void k26_hwaddr_list_init(void *dev, int off)
{
	struct list_head *h = (struct list_head *)((char *)dev + off);

	INIT_LIST_HEAD(h);
	*(int *)((char *)dev + off + 8) = 0;	/* .count */
}

static LIST_HEAD(k26_ndev_list);
static DEFINE_MUTEX(k26_ndev_lock);
/*
 * Last device a frame was received on. The receive path runs in the blob's
 * tasklet, where the mutex must not be taken; the list only changes in
 * process context under the mutex with bottom halves off, so a softirq
 * never sees it half-updated.
 */
static struct k26_ndev *k26_rx_ndev;

/*
 * fastpath.ko (if loaded - it autoloads long before us) forwards bridged
 * LAN frames for our stations straight to us; it learns them from the
 * source MAC of every frame they send, and forgets them all on any
 * (dis)association. Looked up by name so wl.ko does not depend on it.
 */
static void (*k26_fp_learn)(const u8 *mac, struct net_device *dev);
/* ...and takes received frames for known LAN hosts straight to their port */
static bool (*k26_fp_rx)(struct sk_buff *skb);

static struct k26_ndev *ndev_by_k26(void *k26)
{
	struct k26_ndev *n;

	list_for_each_entry(n, &k26_ndev_list, list)
		if (n->k26 == k26)
			return n;
	return NULL;
}

void *k26_netdev_real(void *k26dev)
{
	struct k26_ndev *n = ndev_by_k26(k26dev);

	return n ? n->real : NULL;
}

static struct k26_netdev_ops *k26_ops(void *k26)
{
	return k26_rdp(k26, K26_ND_NETDEV_OPS);
}

void wlc26_ether_setup(void *dev)
{
	k26_wr16(dev, K26_ND_TYPE, ARPHRD_ETHER);
	k26_wr16(dev, K26_ND_HARD_HEADER_LEN, ETH_HLEN);
	k26_wr32(dev, K26_ND_MTU, ETH_DATA_LEN);
	*((u8 *)dev + K26_ND_ADDR_LEN) = ETH_ALEN;
	k26_wr32(dev, K26_ND_TX_QUEUE_LEN, 1000);
	k26_wr32(dev, K26_ND_FLAGS, IFF_BROADCAST | IFF_MULTICAST);
	/* 2.6.36 points dev_addr into the device's own storage */
	k26_wrp(dev, K26_ND_DEV_ADDR, (char *)dev + K26_ND_PERM_ADDR);
	memset((char *)dev + K26_ND_BROADCAST, 0xff, ETH_ALEN);
}

void *wlc26_alloc_netdev_mq(int sizeof_priv, const char *name,
			    void (*setup)(void *), unsigned int queue_count)
{
	struct k26_ndev *n;
	void *dev;

	dev = kzalloc(K26_NETDEV_SIZE + ALIGN(sizeof_priv, 32) + 32, GFP_KERNEL);
	if (!dev)
		return NULL;
	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n) {
		kfree(dev);
		return NULL;
	}
	n->k26 = dev;

	/* 2.6.36's alloc_netdev did this; the blob walks these lists on the
	 * first ndo_set_rx_mode, and a zeroed head is a NULL deref. */
	k26_hwaddr_list_init(dev, K26_ND_UC);
	k26_hwaddr_list_init(dev, K26_ND_MC);
	k26_hwaddr_list_init(dev, K26_ND_DEV_ADDRS);

	k26_wrp(dev, K26_ND_TX, &n->txq);
	k26_wr32(dev, K26_ND_NUM_TX_QUEUES, queue_count ? queue_count : 1);
	k26_wr32(dev, K26_ND_REAL_NUM_TX, queue_count ? queue_count : 1);
	strscpy((char *)dev + K26_ND_NAME, name ? name : "wl%d", IFNAMSIZ);

	if (setup)
		setup(dev);

	mutex_lock(&k26_ndev_lock);
	local_bh_disable();
	list_add_tail(&n->list, &k26_ndev_list);
	local_bh_enable();
	mutex_unlock(&k26_ndev_lock);
	return dev;
}

void wlc26_free_netdev(void *dev)
{
	struct k26_ndev *n;

	mutex_lock(&k26_ndev_lock);
	n = ndev_by_k26(dev);
	if (n) {
		local_bh_disable();
		list_del(&n->list);
		k26_rx_ndev = NULL;
		local_bh_enable();
		if (n->real)
			free_netdev(n->real);
		kfree(n->mc_nodes);
		kfree(n);
	}
	mutex_unlock(&k26_ndev_lock);
	kfree(dev);
}

/* --- trampolines: real 6.12 netdev -> blob ops ---------------------- */
static atomic_t k26_rx_ok = ATOMIC_INIT(0);

static struct k26_ndev *real_priv(struct net_device *d)
{
	return *(struct k26_ndev **)netdev_priv(d);
}

static int tramp_open(struct net_device *d)
{
	struct k26_ndev *n = real_priv(d);
	struct k26_netdev_ops *o = k26_ops(n->k26);
	int r = o && o->ndo_open ? o->ndo_open(n->k26) : 0;

	if (!r)
		netif_start_queue(d);
	return r;
}

/*
 * Transmit chaining (Broadcom PKTC). Stock firmware hands wl_start() chains
 * of frames for one station, built by the Ethernet driver; wlc_sendpkt() then
 * does the station lookup and header work once and runs the cheap
 * wlc_pktc_sdu_prep() for the rest. We collect frames for the same
 * destination as the stack hands them over and flush the chain from a
 * tasklet, which runs right after the NET_RX softirq that produced them.
 *
 * Layout, from linux_osl.h (the blob is built with PKTC): pktc_cb holds
 * struct chain_node { link; flags:3, pkts:9, bytes:20 }, and pktc_flags bit 3
 * (CHAINED) means "part of a chain". Like the Ethernet driver, every frame of
 * a chain is marked CHAINED and the last link is NULL; a chain of one goes
 * down unmarked. txchain = longest chain, 0 = off (the old path).
 */
static unsigned int txchain = 32;
module_param(txchain, uint, 0644);
MODULE_PARM_DESC(txchain, "max frames per transmit chain, 0 = no chaining");

#define K26_PKTC_CHAINED	(1 << 3)

static void k26_chain_set(struct k26_skb *p, struct k26_skb *link,
			  unsigned int pkts, unsigned int bytes)
{
	*(struct k26_skb **)p->pktc_cb = link;
	*(u32 *)(p->pktc_cb + 4) = (pkts & 0x1ff) << 3 | (bytes & 0xfffff) << 12;
}

static void k26_txflush_dev(struct k26_ndev *n)
{
	struct k26_netdev_ops *o = k26_ops(n->k26);
	struct k26_skb *h = n->ch_head;

	if (!h)
		return;
	if (n->ch_n > 1) {
		k26_chain_set(h, *(struct k26_skb **)h->pktc_cb, n->ch_n, n->ch_bytes);
		k26_st_chains++;
		k26_st_chained += n->ch_n;
		if (n->ch_n > k26_st_maxchain)
			k26_st_maxchain = n->ch_n;
	}
	n->ch_head = n->ch_tail = NULL;
	n->ch_n = n->ch_bytes = 0;
	o->ndo_start_xmit(h, n->k26);
}

static void k26_txflush_tasklet(struct tasklet_struct *t)
{
	struct k26_ndev *n = from_tasklet(n, t, txflush);

	k26_txflush_dev(n);
}

/* Plain unicast IPv4/IPv6 with DSCP 0 and no priority: wl_start_int() only
 * runs pktsetprio() on the head, so every frame of a chain must map to the
 * same access category, and DSCP 0 maps to best effort. */
static bool k26_chainable(const struct sk_buff *skb)
{
	const u8 *d = skb->data;

	if (skb->priority || skb->len < ETH_HLEN + 20 || is_multicast_ether_addr(d))
		return false;
	if (d[12] == 0x08 && d[13] == 0x00)
		return !(d[15] & 0xfc);
	if (d[12] == 0x86 && d[13] == 0xdd)
		return !(d[14] & 0x0f) && !(d[15] & 0xc0);
	return false;
}

static void k26_chain_add(struct k26_ndev *n, struct k26_skb *p)
{
	if (n->ch_head) {
		*(struct k26_skb **)n->ch_tail->pktc_cb = p;
		n->ch_tail->pktc_flags |= K26_PKTC_CHAINED;
		p->pktc_flags |= K26_PKTC_CHAINED;
	} else {
		n->ch_head = p;
		tasklet_schedule(&n->txflush);
	}
	n->ch_tail = p;
	n->ch_n++;
	n->ch_bytes += p->len;
}

static int tramp_stop(struct net_device *d)
{
	struct k26_ndev *n = real_priv(d);
	struct k26_netdev_ops *o = k26_ops(n->k26);

	netif_stop_queue(d);
	local_bh_disable();
	k26_txflush_dev(n);
	local_bh_enable();
	return o && o->ndo_stop ? o->ndo_stop(n->k26) : 0;
}

/* Set when the driver reports a station associating; tramp_xmit() logs
 * everything it hands the blob for a short window after that. */
static unsigned long k26_trace_until;

static netdev_tx_t tramp_xmit(struct sk_buff *skb, struct net_device *d)
{
	struct k26_ndev *n = real_priv(d);
	struct k26_netdev_ops *o = k26_ops(n->k26);
	struct k26_skb *p;
	unsigned int max = READ_ONCE(txchain);
	bool chain;

	k26_st_tx++;
	/* EAPOL out (nas -> eapd -> here): the 4-way handshake */
	if (skb->len >= 14 && skb->data[12] == 0x88 && skb->data[13] == 0x8e)
		pr_info("wl-dbg: tx EAPOL len=%u to %pM\n", skb->len, skb->data);

	if (!o || !o->ndo_start_xmit) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	p = k26_skb_from_real(skb);
	if (!p) {
		dev_kfree_skb_any(skb);
		d->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	p->dev = n->k26;
	if (k26_headroom(p) < K26_TXOFF)
		k26_st_short++;
	/* chained frames get their headers pushed in place, unchecked */
	chain = max > 1 && k26_headroom(p) >= K26_TXOFF + 8 && k26_chainable(skb);
	if (time_before(jiffies, k26_trace_until)) {
		static int tl;
		int rc;

		if (tl++ < 200) {
			u8 *e = skb->data;

			pr_info("wl-dbg: txwin dst=%pM src=%pM proto=%02x%02x len=%u k26len=%u hdr-%s\n",
				e, e + 6, e[12], e[13], skb->len, p->len,
				memcmp(p->data, skb->data, 14) ? "MISMATCH" : "ok");
			k26_txflush_dev(n);
			rc = o->ndo_start_xmit(p, n->k26);
			pr_info("wl-dbg: txwin rc=%d\n", rc);
			return NETDEV_TX_OK;
		}
	}
	/* a chain only ever holds one destination, and frames must not
	 * overtake the chain, so anything else flushes it first */
	if (n->ch_head && (!chain || !ether_addr_equal_unaligned(n->ch_head->data, p->data)))
		k26_txflush_dev(n);
	if (chain) {
		k26_chain_add(n, p);
		if (n->ch_n >= max)
			k26_txflush_dev(n);
		return NETDEV_TX_OK;
	}
	o->ndo_start_xmit(p, n->k26);
	return NETDEV_TX_OK;
}

/* Rebuild the blob's 2.6.36 multicast list from the real netdev's. */
static void k26_mc_sync(struct k26_ndev *n)
{
	struct list_head *head = (struct list_head *)((char *)n->k26 + K26_ND_MC);
	struct netdev_hw_addr *ha;
	struct k26_hw_addr *nodes;
	unsigned int want = netdev_mc_count(n->real), i = 0;

	INIT_LIST_HEAD(head);
	if (want > n->mc_cap) {
		void *p = kcalloc(want + 8, sizeof(struct k26_hw_addr), GFP_ATOMIC);

		if (!p) {
			*(int *)((char *)n->k26 + K26_ND_MC + 8) = 0;
			return;
		}
		kfree(n->mc_nodes);
		n->mc_nodes = p;
		n->mc_cap = want + 8;
	}
	nodes = n->mc_nodes;
	netdev_for_each_mc_addr(ha, n->real) {
		if (i >= want)
			break;
		memcpy(nodes[i].addr, ha->addr, ETH_ALEN);
		nodes[i].type = 0;		/* NETDEV_HW_ADDR_T_MULTICAST */
		nodes[i].refcount = 1;
		list_add_tail(&nodes[i].list, head);
		i++;
	}
	*(int *)((char *)n->k26 + K26_ND_MC + 8) = i;
}

static void tramp_set_rx_mode(struct net_device *d)
{
	struct k26_ndev *n = real_priv(d);
	struct k26_netdev_ops *o = k26_ops(n->k26);

	/* mirror what the blob reads out of its own net_device */
	/*
	 * Never pass IFF_PROMISC down. Being a bridge port puts the real
	 * netdev in promiscuous mode, but wl_set_multicast_list() turns that
	 * into hardware promiscuous mode on the radio, which makes the blob
	 * hand raw 802.11 management frames up the data path and stops normal
	 * AP association handling. mac80211 drivers ignore it for AP vifs too.
	 */
	/*
	 * Keep the radio promiscuous. Without it the hardware delivers no
	 * management frames to the host at all (rxauth/rxassoc stay 0 and no
	 * client can associate), while the ucode still answers probes from its
	 * own template - so the AP looks alive but nothing can connect.
	 * Something in the normal RX address filter is not being programmed;
	 * promiscuous mode is what currently makes host RX work.
	 */
	k26_wr32(n->k26, K26_ND_FLAGS, d->flags | IFF_PROMISC);
	k26_wr32(n->k26, K26_ND_PROMISCUITY, d->promiscuity ? d->promiscuity : 1);
	k26_wr32(n->k26, K26_ND_ALLMULTI, d->allmulti ? d->allmulti : 1);
	k26_mc_sync(n);

	if (o && o->ndo_set_multicast_list)
		o->ndo_set_multicast_list(n->k26);
	else if (o && o->ndo_set_rx_mode)
		o->ndo_set_rx_mode(n->k26);
}

static int tramp_set_mac(struct net_device *d, void *addr)
{
	struct k26_ndev *n = real_priv(d);
	struct k26_netdev_ops *o = k26_ops(n->k26);
	struct sockaddr *sa = addr;
	int r = 0;

	if (o && o->ndo_set_mac_address)
		r = o->ndo_set_mac_address(n->k26, addr);
	if (!r)
		eth_hw_addr_set(d, sa->sa_data);
	return r;
}

/* wl's userspace tools talk to the driver over SIOCDEVPRIVATE. */
/*
 * The blob's wl_ioctl() decides between "copy the buffer in" and "use it
 * directly" with an inlined segment_eq(get_fs(), KERNEL_DS): it reads
 * addr_limit at offset 8 of the 2.6.36 thread_info, which on 6.12 is the
 * far end of the kernel stack - zero, i.e. KERNEL_DS. So it always uses
 * ioc.buf as a kernel pointer. With a user pointer that only works while the
 * buffer's pages happen to be mapped; `wl scanresults` (a fresh 16+ KB
 * buffer) faulted on an unpopulated page and panicked.
 *
 * Give it what it believes it has: the wl_ioctl_t and its buffer copied into
 * kernel memory, a kernel ifreq pointing at them, and copy back afterwards.
 * wlc26___copy_from/to_user() pass kernel pointers through as memcpy.
 */
struct k26_wl_ioctl {			/* 2.6.36 wl_ioctl_t */
	u32 cmd;
	u32 buf;
	u32 len;
	u8 set;
	u32 used;
	u32 needed;
};
#define K26_IOCTL_MINBUF	8192	/* the blob reads up to this much */
#define K26_IOCTL_MAXBUF	(128 * 1024)

static int tramp_siocdevprivate(struct net_device *d, struct ifreq *ifr,
				void __user *data, int cmd)
{
	struct k26_ndev *n = real_priv(d);
	struct k26_netdev_ops *o = k26_ops(n->k26);
	struct k26_wl_ioctl ioc, kioc;
	void __user *ubuf;
	struct ifreq kifr;
	char name[24] = "";
	void *kbuf = NULL;
	bool trace;
	int rc;

	if (!o || !o->ndo_do_ioctl)
		return -EOPNOTSUPP;
	if (!data || copy_from_user(&ioc, data, sizeof(ioc)))
		return -EFAULT;
	if (ioc.len > K26_IOCTL_MAXBUF)
		return -EINVAL;
	ubuf = (void __user *)(uintptr_t)ioc.buf;

	kioc = ioc;
	if (ubuf) {
		kbuf = kvzalloc(max_t(u32, ioc.len, K26_IOCTL_MINBUF), GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;
		/* gets carry their iovar name in the buffer too */
		if (ioc.len && copy_from_user(kbuf, ubuf, ioc.len)) {
			kvfree(kbuf);
			return -EFAULT;
		}
		kioc.buf = (u32)(uintptr_t)kbuf;
	}
	kifr = *ifr;
	kifr.ifr_data = (void __user *)&kioc;

	/* Debug: log the authenticator's ioctls (nas, eapd) and their results. */
	trace = !strncmp(current->comm, "nas", 3) || !strncmp(current->comm, "eapd", 4);
	if (trace && (ioc.cmd == 262 || ioc.cmd == 263) && kbuf)
		strscpy(name, kbuf, sizeof(name));

	rc = o->ndo_do_ioctl(n->k26, &kifr, cmd);

	if (kbuf && ioc.len && copy_to_user(ubuf, kbuf, ioc.len))
		rc = rc ?: -EFAULT;
	kvfree(kbuf);
	ioc.used = kioc.used;
	ioc.needed = kioc.needed;
	if (copy_to_user(data, &ioc, sizeof(ioc)))
		rc = rc ?: -EFAULT;
	if (trace && ioc.cmd != 1 && ioc.cmd != 0)
		pr_info("wl-dbg: ioctl %s cmd=%u %s len=%u set=%u -> %d\n",
			current->comm, ioc.cmd, name, ioc.len, ioc.set, rc);
	return rc;
}

static void tramp_tx_timeout(struct net_device *d, unsigned int q)
{
	struct k26_ndev *n = real_priv(d);
	struct k26_netdev_ops *o = k26_ops(n->k26);

	if (o && o->ndo_tx_timeout)
		o->ndo_tx_timeout(n->k26);
}

static const struct net_device_ops k26_real_ops = {
	.ndo_open		= tramp_open,
	.ndo_stop		= tramp_stop,
	.ndo_start_xmit		= tramp_xmit,
	.ndo_set_rx_mode	= tramp_set_rx_mode,
	.ndo_set_mac_address	= tramp_set_mac,
	.ndo_siocdevprivate	= tramp_siocdevprivate,
	.ndo_tx_timeout		= tramp_tx_timeout,
	.ndo_validate_addr	= eth_validate_addr,
};

/* Broadcom's userspace identifies a wl interface with ETHTOOL_GDRVINFO and
 * then checks that the driver name starts with "wl" - libshared's wl_probe()
 * does exactly that, and nas calls wl_probe() on its -i argument. 2.6.36 still
 * routed SIOCETHTOOL to the driver's own ioctl handler, which is where the
 * blob answers it; since 3.x the core handles SIOCETHTOOL itself and needs
 * ethtool_ops. Without this the ioctl returns -EOPNOTSUPP, nas silently drops
 * its -i and then quits with "Command line parsing error", so nothing ever
 * installs the RSN IE and WPA2 clients bounce off with "wrong password".
 */
static void k26_get_drvinfo(struct net_device *d, struct ethtool_drvinfo *info)
{
	strscpy(info->driver, "wl", sizeof(info->driver));
	strscpy(info->version, "9.10.178.50", sizeof(info->version));
	strscpy(info->bus_info, "bcma", sizeof(info->bus_info));
}

/* defined with the rest of the irq plumbing further down */

static const struct ethtool_ops k26_ethtool_ops = {
	.get_drvinfo	= k26_get_drvinfo,
	.get_link	= ethtool_op_get_link,
};

/*
 * pub->brc_hot is CTF's bridge hot cache, which the stock wl_linux.c takes
 * from ctf.ko. With packet chaining on (the default, and A-MPDU transmit
 * stalls without it), wlc_rxframe_chainable() indexes it by da[4]^da[5] for
 * every HT/VHT data frame, and NULL there panics. A zeroed table never
 * matches, so frames take the normal receive path.
 * Pointer chain from the blob's DWARF: priv (+768) -> wl_if, wl_if+4 -> wl_info,
 * wl_info+4 -> wlc_pub, wlc_pub+108 -> brc_hot.
 */
#define K26_WLIF_WL		4
#define K26_WL_PUB		4
#define K26_PUB_BRC_HOT		108
#define K26_BRC_HOT_SIZE	(256 * 12)	/* ctf_brc_hot_t: ether_addr + tx_dev */

static void k26_brc_hot_init(void *k26dev)
{
	void *wlif, *wl, *pub, **hot;

	wlif = *(void **)((char *)k26dev + K26_ND_PRIV);
	wl = wlif ? *(void **)((char *)wlif + K26_WLIF_WL) : NULL;
	pub = wl ? *(void **)((char *)wl + K26_WL_PUB) : NULL;
	if (!pub)
		return;
	hot = (void **)((char *)pub + K26_PUB_BRC_HOT);
	if (!*hot)
		*hot = kzalloc(K26_BRC_HOT_SIZE, GFP_KERNEL);
	pr_info("wl: brc_hot table %s\n", *hot ? "allocated" : "missing");
}

static struct proc_dir_entry *k26_shim_proc;

static int k26_shim_show(struct seq_file *m, void *v)
{
	seq_printf(m, "txchain %u\ntx %lu\nchains %lu\nchained %lu\nmaxchain %lu\n",
		   txchain, k26_st_tx, k26_st_chains, k26_st_chained, k26_st_maxchain);
	seq_printf(m, "blob_alloc %lu\nshells %u\nshell_miss %lu\n",
		   k26_st_alloc, k26_nshells, k26_st_shell_miss);
	seq_printf(m, "txheadroom %u\nshort_headroom %lu\n", txheadroom, k26_st_short);
	return 0;
}

int wlc26_register_netdev(void *k26dev)
{
	struct k26_ndev *n;
	struct net_device *d;
	const char *want = (const char *)k26dev + K26_ND_NAME;
	int err;

	mutex_lock(&k26_ndev_lock);
	n = ndev_by_k26(k26dev);
	mutex_unlock(&k26_ndev_lock);
	if (!n)
		return -ENODEV;

	d = alloc_etherdev(sizeof(struct k26_ndev *));
	if (!d)
		return -ENOMEM;
	*(struct k26_ndev **)netdev_priv(d) = n;
	n->real = d;
	tasklet_setup(&n->txflush, k26_txflush_tasklet);
	if (!k26_fp_learn)
		k26_fp_learn = __symbol_get("fastpath_wifi_learn2");
	if (!k26_fp_rx)
		k26_fp_rx = __symbol_get("fastpath_wifi_rx");
	if (!k26_shim_proc)
		k26_shim_proc = proc_create_single("wl-shim", 0444, NULL, k26_shim_show);

	d->netdev_ops = &k26_real_ops;
	d->ethtool_ops = &k26_ethtool_ops;
	d->watchdog_timeo = 5 * HZ;
	/* locally generated frames: leave room for the blob's headers */
	d->needed_headroom = K26_TXOFF + 16;
	d->mtu = k26_rd32(k26dev, K26_ND_MTU) ?: ETH_DATA_LEN;
	eth_hw_addr_set(d, (u8 *)k26dev + K26_ND_PERM_ADDR);
	strscpy(d->name, want[0] ? want : "wl%d", IFNAMSIZ);

	err = register_netdev(d);
	if (err) {
		free_netdev(d);
		n->real = NULL;
		return err;
	}
	n->registered = true;
	k26_brc_hot_init(k26dev);
	/* hand the kernel-assigned name and index back to the blob */
	strscpy((char *)k26dev + K26_ND_NAME, d->name, IFNAMSIZ);
	k26_wr32(k26dev, K26_ND_IFINDEX, d->ifindex);
	return 0;
}

void wlc26_unregister_netdev(void *k26dev)
{
	struct k26_ndev *n;

	mutex_lock(&k26_ndev_lock);
	n = ndev_by_k26(k26dev);
	mutex_unlock(&k26_ndev_lock);
	if (n && n->registered) {
		unregister_netdev(n->real);	/* ndo_stop flushed the chain */
		tasklet_kill(&n->txflush);
		local_bh_disable();
		n->registered = false;
		k26_rx_ndev = NULL;
		local_bh_enable();
	}
}

/* Called from module exit, after the blob has let go of everything. */
void k26_compat_exit(void)
{
	if (k26_fp_learn)
		__symbol_put("fastpath_wifi_learn2");
	k26_fp_learn = NULL;
	if (k26_fp_rx)
		__symbol_put("fastpath_wifi_rx");
	k26_fp_rx = NULL;
	if (k26_shim_proc)
		proc_remove(k26_shim_proc);
	k26_shim_proc = NULL;
	k26_shells_drain();
}

int wlc26_netif_rx(struct k26_skb *p)
{
	struct k26_ndev *n;
	struct sk_buff *r = p->real;
	void (*learn)(const u8 *mac, struct net_device *dev) = READ_ONCE(k26_fp_learn);
	const u8 *src = NULL;

	/* Broadcom events (ethertype 0x886c): eth header, 10-byte bcmeth
	 * header, then wl_event_msg_t, all big-endian - version, flags,
	 * event_type, status, reason, auth_type, datalen, addr. nas only
	 * starts the 4-way handshake on ASSOC_IND (8) / REASSOC_IND (10).
	 * 44 is PROBREQ_MSG, which floods, so it is skipped. */
	if (p->mac_header && ntohs(p->protocol) == 0x886c) {
		static int evlog;
		u8 *e = (u8 *)p->mac_header + 14 + 10;
		u32 type = get_unaligned_be32(e + 4);

		/* DEAUTH(_IND) 5/6 .. DISASSOC(_IND) 11/12: stations moved */
		if (learn && type >= 5 && type <= 12)
			learn(NULL, NULL);
		if (type != 44 && evlog < 300) {
			static int stacks;

			evlog++;
			pr_info("wl-dbg: event type=%u status=%u reason=%u flags=%04x datalen=%u addr=%pM\n",
				type, get_unaligned_be32(e + 8),
				get_unaligned_be32(e + 12), get_unaligned_be16(e + 2),
				get_unaligned_be32(e + 20), e + 24);
			/* who in the blob decided to drop the station? the blob
			 * keeps its symbols, so the trace names the function */
			if (type == 8)
				k26_trace_until = jiffies + msecs_to_jiffies(300);
		}
	} else if (p->mac_header) {
		u8 *h = (u8 *)p->mac_header;

		if (learn)
			src = h + ETH_ALEN;	/* reported once the netdev is known */
		if (atomic_read(&k26_rx_ok) < 2000 && h[12] == 0x88 && h[13] == 0x8e)
			pr_info("wl-dbg: rx EAPOL from %pM\n", h + 6);
	}

	if (!r) {
		k26_skb_free(p);
		return NET_RX_DROP;
	}
	n = k26_rx_ndev;
	if (!n || n->k26 != p->dev) {
		n = ndev_by_k26(p->dev);
		if (!n || !n->registered) {
			pr_info("wl-dbg: rx DROP - no netdev for dev=%p\n", p->dev);
			k26_skb_free(p);
			return NET_RX_DROP;
		}
		k26_rx_ndev = n;
	}
	atomic_inc(&k26_rx_ok);
	k26_skb_sync_to_real(p);
	r->dev = n->real;
	if (src)
		learn(src, n->real);
	r->protocol = p->protocol;
	r->pkt_type = p->bits149 & 0x07;
	/*
	 * eth_type_trans() recorded mac_header and then pulled ETH_HLEN, so
	 * p->data already points past the ethernet header. Carry that offset
	 * over rather than resetting, or the bridge reads addresses from the
	 * payload and never learns the client.
	 */
	if (p->mac_header && p->mac_header <= p->data)
		skb_set_mac_header(r, (int)(p->mac_header - p->data));
	else
		skb_reset_mac_header(r);
	p->real = NULL;		/* the real skb now belongs to the stack */
	k26_skb_free(p);
	if (k26_fp_rx && r->protocol != htons(0x886c)) {
		bool taken;

		rcu_read_lock();
		taken = k26_fp_rx(r);
		rcu_read_unlock();
		if (taken)
			return NET_RX_SUCCESS;
	}
	return netif_rx(r);
}

/* 2.6.36 eth_type_trans() over the fake structures. */
__be16 wlc26_eth_type_trans(struct k26_skb *p, void *k26dev)
{
	const struct ethhdr *eth = (const struct ethhdr *)p->data;
	const u8 *myaddr = (const u8 *)k26dev + K26_ND_PERM_ADDR;
	u8 pkt_type;

	p->dev = k26dev;
	p->mac_header = p->data;
	wlc26_skb_pull(p, ETH_HLEN);

	if (is_multicast_ether_addr(eth->h_dest))
		pkt_type = is_broadcast_ether_addr(eth->h_dest) ?
			PACKET_BROADCAST : PACKET_MULTICAST;
	else if (!ether_addr_equal(eth->h_dest, myaddr))
		pkt_type = PACKET_OTHERHOST;
	else
		pkt_type = PACKET_HOST;
	p->bits149 = (p->bits149 & ~0x07) | pkt_type;

	if (be16_to_cpu(eth->h_proto) >= ETH_P_802_3_MIN)
		p->protocol = eth->h_proto;
	else
		p->protocol = htons(ETH_P_802_2);
	return p->protocol;
}

void wlc26___netif_schedule(void *txq)
{
	struct k26_ndev *n;

	mutex_lock(&k26_ndev_lock);
	list_for_each_entry(n, &k26_ndev_list, list)
		if (&n->txq == txq && n->real) {
			netif_wake_queue(n->real);
			break;
		}
	mutex_unlock(&k26_ndev_lock);
}

/* Our packets are always linear, so this is a one-entry scatterlist. */
int k26_skb_to_sgvec(struct k26_skb *p, struct scatterlist *sg, unsigned int len)
{
	sg_init_one(sg, p->data, len);
	return 1;
}
EXPORT_SYMBOL(k26_skb_to_sgvec);

/* ------------------------------------------------------------------ */
/* DMA. The osl passes the synthesized pci_dev around; route through	*/
/* whichever real device (bcma core or PCIe function) owns the radio.	*/
/* ------------------------------------------------------------------ */
dma_addr_t wlc26_pci_map_single(void *pdev, void *va, size_t size, int dir)
{
	struct device *d = k26_pdev_dma_dev(pdev);

	if (!d)
		return 0;
	return dma_map_single(d, va, size,
			      dir == 1 ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
}
EXPORT_SYMBOL(wlc26_pci_map_single);

void wlc26_pci_unmap_single(void *pdev, dma_addr_t pa, size_t size, int dir)
{
	struct device *d = k26_pdev_dma_dev(pdev);

	if (!d) {
		static int _warned;

		if (!_warned++)
			pr_err("wl-dbg: no dma dev for pdev=%p - RX cache never invalidated\n", pdev);
	}
	if (d)
		dma_unmap_single(d, pa, size,
				 dir == 1 ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
}
EXPORT_SYMBOL(wlc26_pci_unmap_single);

int wlc26_pci_map_sg(void *pdev, struct scatterlist *sg, int nents, int dir)
{
	struct device *d = k26_pdev_dma_dev(pdev);

	if (!d)
		return 0;
	return dma_map_sg(d, sg, nents,
			  dir == 1 ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
}
EXPORT_SYMBOL(wlc26_pci_map_sg);

/* ------------------------------------------------------------------ */
/* Odds and ends.							*/
/* ------------------------------------------------------------------ */
unsigned long num_physpages;

asmlinkage __visible int wlc26_printk(const char *fmt, ...)
{
	va_list args;
	int r;

	va_start(args, fmt);
	r = vprintk(fmt, args);
	va_end(args);
	return r;
}

void wlc26_warn_slowpath_null(const char *file, int line)
{
	pr_warn("wl: WARN at %s:%d\n", file, line);
}

/* Inlined preempt_enable() calls this; OpenWrt builds without preemption,
 * so there is never anything to do. */
void wlc26_preempt_schedule(void)
{
}

void *wlc26___memzero(void *p, size_t n)
{
	return memset(p, 0, n);
}

unsigned int wlc26_jiffies_to_msecs(unsigned long j)
{
	return jiffies_to_msecs(j);
}

unsigned long wlc26_msecs_to_jiffies(unsigned int m)
{
	return msecs_to_jiffies(m);
}

/* tramp_siocdevprivate() hands the blob kernel copies of the request; those
 * pointers come through here and are plain memory. */
unsigned long wlc26___copy_from_user(void *to, const void __user *from, unsigned long n)
{
	if ((unsigned long)from >= TASK_SIZE) {
		memcpy(to, (const void __force *)from, n);
		return 0;
	}
	return copy_from_user(to, from, n);
}

unsigned long wlc26___copy_to_user(void __user *to, const void *from, unsigned long n)
{
	if ((unsigned long)to >= TASK_SIZE) {
		memcpy((void __force *)to, from, n);
		return 0;
	}
	return copy_to_user(to, from, n);
}

/*
 * Debug: /proc/wl-regs dumps the D11 core's interrupt and RX DMA registers.
 * Read it only while the device is up - with the MAC clock-gated (during
 * attach, or with the interface down) a register read hangs the SoC bus. It
 * is read on demand from process context for exactly that reason; the ISR
 * must never do it.
 */
#define K26_D11_PHYS	0x1800a000
static void __iomem *k26_d11_regs;
static struct proc_dir_entry *k26_regs_proc;

static int k26_regs_show(struct seq_file *m, void *v)
{
	void __iomem *b = k26_d11_regs;
	int k;

	if (!b)
		return -ENODEV;
	/* b43's GEN_IRQ_REASON / GEN_IRQ_MASK */
	seq_printf(m, "macintstatus 0x128 = %08x\n", readl(b + 0x128));
	seq_printf(m, "macintmask   0x12c = %08x\n", readl(b + 0x12c));
	seq_printf(m, "maccontrol   0x120 = %08x\n", readl(b + 0x120));
	/* intctrlregs[k] = { intstatus, intmask } per DMA FIFO */
	for (k = 0; k < 6; k++)
		seq_printf(m, "fifo%d int   0x%03x = %08x / %08x\n", k,
			   0x20 + 8 * k, readl(b + 0x20 + 8 * k),
			   readl(b + 0x24 + 8 * k));
	/* dma64 RX for FIFO 0: control, ptr, addrlo, addrhi, status0/1 */
	for (k = 0; k < 6; k++)
		seq_printf(m, "rx0 dma      0x%03x = %08x\n",
			   0x220 + 4 * k, readl(b + 0x220 + 4 * k));

	/* the other D11 core, which the blob actually runs - see irqunit */
	b = ioremap(0x18001000, 0x1000);
	if (b) {
		seq_printf(m, "--- core 1 @ 0x18001000 ---\n");
		seq_printf(m, "macintstatus 0x128 = %08x\n", readl(b + 0x128));
		seq_printf(m, "macintmask   0x12c = %08x\n", readl(b + 0x12c));
		seq_printf(m, "maccontrol   0x120 = %08x\n", readl(b + 0x120));
		seq_printf(m, "fifo0 int    0x020 = %08x / %08x\n",
			   readl(b + 0x20), readl(b + 0x24));
		seq_printf(m, "rx0 dma ctl  0x220 = %08x ptr %08x st0 %08x\n",
			   readl(b + 0x220), readl(b + 0x224), readl(b + 0x230));
		iounmap(b);
	}
	return 0;
}

void __iomem *wlc26___arm_ioremap(phys_addr_t phys, size_t size, unsigned int mtype)
{
	void __iomem *p = ioremap(phys, size);

	pr_info("wl-dbg: ioremap %pa +0x%zx -> %p\n", &phys, size, p);
	if (p && phys <= K26_D11_PHYS && phys + size >= K26_D11_PHYS + 0x1000) {
		k26_d11_regs = p + (K26_D11_PHYS - phys);
		if (!k26_regs_proc)
			k26_regs_proc = proc_create_single("wl-regs", 0400, NULL,
							   k26_regs_show);
	}
	return p;
}

void wlc26___iounmap(volatile void __iomem *addr)
{
	if (k26_d11_regs && addr <= k26_d11_regs &&
	    k26_d11_regs < (volatile void __iomem *)addr + 0x8000) {
		proc_remove(k26_regs_proc);
		k26_regs_proc = NULL;
		k26_d11_regs = NULL;
	}
	iounmap(addr);
}

/* dev_get_drvdata / dev_set_drvdata live in wlbus.c: they have to special-case
 * the struct device embedded in a synthesized pci_dev. */

static irq_handler_t k26_real_isr;
static void *k26_isr_dev;
static atomic_t k26_isr_calls = ATOMIC_INIT(0);

static int k26_isr_num = -1;
static atomic_t k26_isr_masked = ATOMIC_INIT(0);
static unsigned int k26_isr_none_run;	/* unclaimed since the last claim */
static unsigned int k26_isr_backoff;	/* jiffies to stay masked */
static struct timer_list k26_isr_rearm;

static void k26_isr_rearm_fn(struct timer_list *t)
{
	if (atomic_xchg(&k26_isr_masked, 0))
		enable_irq(k26_isr_num);
}

static irqreturn_t k26_isr_wrap(int irq, void *dev)
{
	irqreturn_t r = k26_real_isr(irq, dev);
	int n = atomic_inc_return(&k26_isr_calls);

	/* Never touch the core's registers from here: during attach the MAC
	 * is clock-gated and a read hangs the SoC bus (boot-looped the box).
	 * Use `wl macreg` once the device is up instead. Keep this print rare
	 * - at one line per 1000 the storm floods the 115200 console. */
	if (n <= 3)
		pr_info("wl-dbg: isr #%d ret=%d\n", n, (int)r);

	if (r != IRQ_NONE) {
		k26_isr_none_run = 0;
		k26_isr_backoff = 0;
		return r;
	}

	/*
	 * The chip asserts this line continuously whenever the blob is not up
	 * - through the whole attach, and again between attach and `wl up` -
	 * and its ISR claims nothing in that state. Left alone that is ~200k
	 * unclaimed interrupts per second, which both trips the kernel's
	 * spurious detector (it disables the line for good, and then the
	 * driver never drains the RX ring so no client can associate) and
	 * starves userspace badly enough to trip the watchdog.
	 *
	 * So never report it unclaimed, and throttle it: let a short burst
	 * through, then mask the line for a while, backing off to 100ms so a
	 * persistent storm costs almost nothing. Any interrupt the blob does
	 * claim resets this, so normal operation is unaffected.
	 */
	if (++k26_isr_none_run >= 16) {
		k26_isr_none_run = 0;
		if (k26_isr_backoff < 25)
			k26_isr_backoff++;
		if (!atomic_xchg(&k26_isr_masked, 1)) {
			disable_irq_nosync(irq);
			mod_timer(&k26_isr_rearm, jiffies + k26_isr_backoff);
		}
	}
	return IRQ_HANDLED;
}

int wlc26_request_threaded_irq(unsigned int irq, irq_handler_t handler,
			       irq_handler_t thread_fn, unsigned long flags,
			       const char *name, void *dev)
{
	pr_info("wl-dbg: request_irq %u handler=%p thread=%p flags=0x%lx dev=%p\n",
		irq, handler, thread_fn, flags, dev);
	k26_real_isr = handler;
	k26_isr_dev = dev;
	handler = k26_isr_wrap;

	/* 2.6.36's IRQF_DISABLED (0x20) is IRQF_PERCPU today - drop it. */
	flags &= ~0x20UL;

	/*
	 * The chip asserts its interrupt the moment the line is unmasked, but
	 * the blob's ISR claims nothing until wlc_attach() has finished - it
	 * returns IRQ_NONE, and the attach takes ~0.8s. That is ~200k
	 * unhandled interrupts, so the kernel's spurious detector hits its
	 * 100k limit and disables the line for good, 4ms before the driver
	 * becomes ready:
	 *
	 *   isr #1 ret=0 ... isr #100000 ret=0
	 *   irq 33: nobody cared  /  Disabling IRQ #33
	 *   eth2: Broadcom BCM43c8 802.11 Wireless Controller
	 *
	 * The driver then runs forever with no interrupts: beacons still go
	 * out because the d11 ucode sends those by itself, but nothing is ever
	 * taken off the RX ring, so no client can authenticate.
	 *
	 * The interrupts themselves are needed: requesting the line masked
	 * makes the probe fail with -ENODEV. k26_isr_wrap() handles it.
	 */
	k26_isr_none_run = 0;
	k26_isr_backoff = 0;
	k26_isr_num = irq;
	atomic_set(&k26_isr_masked, 0);
	timer_setup(&k26_isr_rearm, k26_isr_rearm_fn, 0);
	return request_threaded_irq(irq, handler, thread_fn, flags, name, dev);
}

void wlc26_free_irq(unsigned int irq, void *dev)
{
	del_timer_sync(&k26_isr_rearm);
	if (atomic_xchg(&k26_isr_masked, 0))
		enable_irq(irq);
	free_irq(irq, dev);
}

/*
 * The blob creates one /proc entry and writes read_proc/write_proc/data into
 * the returned struct. Nothing reads it back, so a zeroed scratch buffer of
 * the 2.6.36 size keeps those stores harmless and the attach path on its
 * success branch.
 */
void *wlc26_create_proc_entry(const char *name, umode_t mode, void *parent)
{
	return kzalloc(256, GFP_KERNEL);
}

void wlc26_remove_proc_entry(const char *name, void *parent)
{
}

/* ------------------------------------------------------------------ */
/* Broadcom accelerators that are separate modules we do not build:	*/
/* CTF, EMF, IGS and DPSTA. The blob tests these for failure.		*/
/* ------------------------------------------------------------------ */
void *wlc26_ctf_attach_fn;
int wlc26_dpsta_register(int unit, void *cb) { return -1; }
void *wlc26_dpsta_recv;
int wlc26_emfc_init(void *a, void *b, void *c) { return -1; }
void wlc26_emfc_exit(void *a) { }
int wlc26_emfc_input(void *a, void *b, void *c, void *d, bool e) { return -1; }
int wlc26_emfc_cfg_request_process(void *a) { return -1; }
int wlc26_igsc_init(void *a, void *b, void *c) { return -1; }
void wlc26_igsc_exit(void *a) { }
int wlc26_igsc_interface_rtport_del(void *a, void *b) { return -1; }
int wlc26_igsc_sdb_interface_del(void *a, void *b) { return -1; }

/* CTF packet pool: disabled, so these are no-ops. */
void wlc26_osl_ctfpool_init(void *osh, uint n, uint s) { }
void wlc26_osl_ctfpool_cleanup(void *osh) { }
void wlc26_osl_ctfpool_replenish(void *osh, uint n) { }
int wlc26_osl_ctfpool_add(void *osh) { return 0; }
