/*
 * k26.h - Linux 2.6.36 (Broadcom-patched) ABI as seen by wl_apsta.o.
 * Layouts are transcribed verbatim from the blob's own DWARF (pahole dumps in
 * ~/wlport/layouts). Every offset below is load-bearing: the blob accesses these
 * fields with inlined code, so nothing here may be reordered or resized.
 */
#ifndef _K26_H_
#define _K26_H_

#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>

#define K26_SKB_SIZE	232
#define K26_NETDEV_SIZE	768
#define K26_PCIDEV_SIZE	840

/* 2.6.36 sk_buff as patched by Broadcom (CTF fields inserted near the front). */
struct k26_skb {
	struct k26_skb		*next;			/*   0 */
	struct k26_skb		*prev;			/*   4 */
	unsigned char		pktc_cb[8];		/*   8 */
	unsigned char		ctf_pppoe_cb[8];	/*  16 */
	void			*ctfmap;		/*  24 */
	u32			_hole28;		/*  28 */
	u64			tstamp;			/*  32 */
	void			*sk;			/*  40 */
	void			*dev;			/*  44 */
	char			cb[48];			/*  48 */
	unsigned long		_skb_refdst;		/*  96 */
	void			*sp;			/* 100 */
	void			*ctfpool;		/* 104 */
	u32			pktc_flags;		/* 108 */
	void			*ctf_ipc_txif;		/* 112 */
	u32			napt_idx;		/* 116 */
	u32			napt_flags;		/* 120 */
	u8			tcpf_bits;		/* 124 */
	u8			_hole125[3];
	unsigned int		len;			/* 128 */
	unsigned int		data_len;		/* 132 */
	u16			mac_len;		/* 136 */
	u16			hdr_len;		/* 138 */
	u32			csum;			/* 140 */
	u32			priority;		/* 144 */
	u8			bits148;		/* 148: local_df,cloned,ip_summed,nohdr,nfctinfo */
	u8			bits149;		/* 149: pkt_type,fclone,ipvs_property,peeked,nf_trace */
	__be16			protocol;		/* 150 */
	void			(*destructor)(struct k26_skb *);	/* 152 */
	int			skb_iif;		/* 156 */
	u32			rxhash;			/* 160 */
	u32			mark;			/* 164 */
	u16			vlan_tci;		/* 168 */
	u16			_hole170;
	unsigned char		*transport_header;	/* 172 */
	unsigned char		*network_header;	/* 176 */
	unsigned char		*mac_header;		/* 180 */
	unsigned char		*tail;			/* 184 */
	unsigned char		*end;			/* 188 */
	unsigned char		*head;			/* 192 */
	unsigned char		*data;			/* 196 */
	unsigned int		truesize;		/* 200 */
	atomic_t		users;			/* 204 */
	u16			queue_mapping;		/* 208 */
	u8			bits210;		/* 210 */
	u8			_hole211;
	void			*nfct;			/* 212 */
	void			*nfct_reasm;		/* 216 */
	u32			nfcache;		/* 220 */
	u16			tc_index;		/* 224 */
	u16			tc_verd;		/* 226 */
	u32			_pad228;		/* 228 -> size 232 */

	/* Private tail. Invisible to the blob, which never looks past 232. */
	struct sk_buff		*real;		/* companion 6.12 skb owning the buffer */
	u32			magic;
};

#define K26_SKB_MAGIC	0x6b32366bU
/* cloned bit is bit 1 of the byte at offset 148 */
#define K26_CLONED	0x02

/* 2.6.36 net_device_ops - the blob fills one of these in and we call through it. */
struct k26_netdev_ops {
	int	(*ndo_init)(void *dev);
	void	(*ndo_uninit)(void *dev);
	int	(*ndo_open)(void *dev);
	int	(*ndo_stop)(void *dev);
	int	(*ndo_start_xmit)(struct k26_skb *skb, void *dev);
	u16	(*ndo_select_queue)(void *dev, struct k26_skb *skb);
	void	(*ndo_change_rx_flags)(void *dev, int flags);
	void	(*ndo_set_rx_mode)(void *dev);
	void	(*ndo_set_multicast_list)(void *dev);
	int	(*ndo_set_mac_address)(void *dev, void *addr);
	int	(*ndo_validate_addr)(void *dev);
	int	(*ndo_do_ioctl)(void *dev, struct ifreq *ifr, int cmd);
	int	(*ndo_set_config)(void *dev, void *map);
	int	(*ndo_change_mtu)(void *dev, int new_mtu);
	int	(*ndo_neigh_setup)(void *dev, void *parms);
	void	(*ndo_tx_timeout)(void *dev);
	void	*ndo_get_stats64;
	void	*(*ndo_get_stats)(void *dev);
	/* remaining members unused by us; struct is 108 bytes */
	void	*_rest[9];
};

/* Offsets into the blob's 768-byte net_device. */
#define K26_ND_NAME		0
#define K26_ND_BASE_ADDR	60
#define K26_ND_IRQ		64
#define K26_ND_STATE		72
#define K26_ND_FEATURES		100
#define K26_ND_IFINDEX		104
#define K26_ND_STATS		112	/* struct net_device_stats, 92 bytes */
#define K26_ND_NETDEV_OPS	204
#define K26_ND_ETHTOOL_OPS	208
#define K26_ND_FLAGS		216
#define K26_ND_MTU		228
#define K26_ND_TYPE		232
#define K26_ND_HARD_HEADER_LEN	234
#define K26_ND_PERM_ADDR	244
#define K26_ND_ADDR_LEN		277
#define K26_ND_UC		280	/* struct netdev_hw_addr_list {list; count} */
#define K26_ND_MC		292
#define K26_ND_PROMISCUITY	308
#define K26_ND_ALLMULTI		312
#define K26_ND_IEEE80211_PTR	340
#define K26_ND_DEV_ADDR		348
#define K26_ND_DEV_ADDRS	352	/* struct netdev_hw_addr_list */
#define K26_ND_BROADCAST	364
#define K26_ND_TX		456	/* struct netdev_queue * */
#define K26_ND_NUM_TX_QUEUES	460
#define K26_ND_REAL_NUM_TX	464
#define K26_ND_TX_QUEUE_LEN	472
#define K26_ND_TRANS_START	476
#define K26_ND_WATCHDOG_TIMEO	480
#define K26_ND_PRIV		768	/* netdev_priv(): NETDEV_ALIGN(768) == 768 */

/* Offsets into the blob's 840-byte pci_dev (only these are ever read). */
#define K26_PD_DEV		96	/* struct device */
#define K26_PD_VENDOR		32
#define K26_PD_DEVICE		34
#define K26_PD_IRQ		260
#define K26_PD_RESOURCE		264	/* resource[0].start */

/* Offsets into the blob's 28-byte timer_list / 20-byte tasklet_struct. */
#define K26_TIMER_EXPIRES	8
#define K26_TIMER_FUNCTION	16
#define K26_TIMER_DATA		20
#define K26_TASKLET_STATE	4
#define K26_TASKLET_FUNC	12
#define K26_TASKLET_DATA	16
#define K26_WORK_FUNC		12

static inline u32 k26_rd32(void *base, int off) { return *(u32 *)((char *)base + off); }
static inline void k26_wr32(void *base, int off, u32 v) { *(u32 *)((char *)base + off) = v; }
static inline u16 k26_rd16(void *base, int off) { return *(u16 *)((char *)base + off); }
static inline void k26_wr16(void *base, int off, u16 v) { *(u16 *)((char *)base + off) = v; }
static inline void *k26_rdp(void *base, int off) { return *(void **)((char *)base + off); }
static inline void k26_wrp(void *base, int off, void *v) { *(void **)((char *)base + off) = v; }

/* Exported to the rest of the shim, and to the recompiled osl. */
struct k26_skb *k26_skb_from_real(struct sk_buff *real);
struct k26_skb *k26_skb_alloc(unsigned int len);
struct k26_skb *k26_skb_dup(struct k26_skb *p);
void k26_skb_free(struct k26_skb *p);
void k26_skb_sync_to_real(struct k26_skb *p);
int k26_skb_to_sgvec(struct k26_skb *p, struct scatterlist *sg, unsigned int len);
void *k26_netdev_real(void *k26dev);
unsigned char *wlc26_skb_put(struct k26_skb *p, unsigned int len);
unsigned char *wlc26_skb_push(struct k26_skb *p, unsigned int len);
unsigned char *wlc26_skb_pull(struct k26_skb *p, unsigned int len);
void k26_compat_exit(void);

/* The osl talks DMA through the synthesized pci_dev; map it to the real one. */
struct device *k26_pdev_dma_dev(void *pdev);
struct device *k26_default_dma_dev(void);
unsigned int k26_pci_bus(void *pdev);
int wlc26_pci_read_config_dword(void *pdev, int where, u32 *val);
int wlc26_pci_write_config_dword(void *pdev, int where, u32 val);

#define k26_headroom(p)	((unsigned int)((p)->data - (p)->head))
#define k26_tailroom(p)	((unsigned int)((p)->end - (p)->tail))

/* 2.6.36 __skb_trim() for linear k26 skbs (PKTSETLEN). */
static inline void k26_skb_trim(struct k26_skb *p, unsigned int len)
{
	p->len = len;
	p->tail = p->data + len;
}

#endif /* _K26_H_ */
