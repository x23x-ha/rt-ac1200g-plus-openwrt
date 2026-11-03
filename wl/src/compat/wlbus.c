/*
 * wlbus.c - bus glue: hand the blob's PCI probe a synthesized 2.6.36 pci_dev.
 *
 * wl_apsta.o registers itself as a PCI driver matching any Broadcom network
 * device. On this board only the 2.4 GHz BCM43217 is really on PCIe; the
 * 5 GHz AC radio is a core on the SoC backplane, which the stock 2.6.36 BSP
 * presented as a fake PCI device too. We do the same, from bcma, and call the
 * blob's probe directly rather than letting it near the real PCI core.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/bcma/bcma.h>
#include <linux/dma-mapping.h>
#include "k26.h"

#define BRCM_VENDOR	0x14e4
#define SOC_DEVID_DFLT	0x43c8		/* sb/1/devid on this board */

/* 2.6.36 pci_driver, as the blob lays it out (108 bytes). */
struct k26_pci_driver {
	struct list_head node;		/*  0 */
	char *name;			/*  8 */
	const void *id_table;		/* 12 */
	int (*probe)(void *dev, const void *id);	/* 16 */
	void (*remove)(void *dev);	/* 20 */
	/* rest unused */
};

struct k26_pdev {
	struct list_head list;
	u8 cfg[K26_PCIDEV_SIZE];	/* what the blob sees */
	void *drvdata;
	struct device *dmadev;
	struct pci_dev *realpci;	/* NULL for a SoC core */
	u32 cfgspace[64];		/* synthetic config space for SoC cores */
	void *wl;			/* blob's per-device context */
};

static LIST_HEAD(pdev_list);
static DEFINE_MUTEX(pdev_lock);
static struct k26_pci_driver *blob_drv;

/* Which D11 core unit on the SoC backplane to drive; see wl_bcma_probe(). */
static int coreunit = 1;
module_param(coreunit, int, 0444);
MODULE_PARM_DESC(coreunit, "SoC D11 core unit to attach (default 1)");

/*
 * The blob attaches through core unit 1 (that is where the NVRAM calibration
 * block lives, sb/1/), but it brings the MAC up on D11 core unit 0: with the
 * driver up, unit 1's maccontrol reads 0x400 (MAC and PSM stopped, RX DMA off)
 * while beacons keep going out. Its interrupts therefore arrive on unit 0's
 * line, and listening on unit 1's meant the ISR never saw a single interrupt
 * it could claim - so the RX ring was never drained and no client could
 * associate. -1 means "same as coreunit".
 */
static int irqunit = 0;
module_param(irqunit, int, 0444);
MODULE_PARM_DESC(irqunit, "SoC D11 core unit whose interrupt to request (default 0, -1 = coreunit)");

extern int wl_blob_init(void);
extern void wl_blob_exit(void);
extern int nvram_init(void *sih);
extern void nvram_exit(void);
extern char *nvram_get(const char *name);

/* ------------------------------------------------------------------ */
/* Lookups used by the shim and the osl.				*/
/* ------------------------------------------------------------------ */
static struct k26_pdev *pdev_of_locked(void *p)
{
	struct k26_pdev *k;

	list_for_each_entry(k, &pdev_list, list)
		if (k->cfg == (u8 *)p)
			return k;
	return NULL;
}

/* p may be the pci_dev itself or the &pdev->dev inside it */
static struct k26_pdev *pdev_of_any(void *p)
{
	struct k26_pdev *k;

	list_for_each_entry(k, &pdev_list, list)
		if (k->cfg == (u8 *)p || k->cfg + K26_PD_DEV == (u8 *)p)
			return k;
	return NULL;
}

/* osl_pci_bus(): the real bus number of a PCIe radio (1 for the BCM43217,
 * matching its devpath0=pci/1/1/ in NVRAM); 0 for a SoC core. */
unsigned int k26_pci_bus(void *pdev)
{
	struct k26_pdev *k;
	unsigned int bus = 0;

	mutex_lock(&pdev_lock);
	k = pdev_of_any(pdev);
	if (k && k->realpci)
		bus = k->realpci->bus->number;
	mutex_unlock(&pdev_lock);
	return bus;
}

struct device *k26_pdev_dma_dev(void *pdev)
{
	struct k26_pdev *k;
	struct device *d = NULL;

	mutex_lock(&pdev_lock);
	k = pdev_of_any(pdev);
	if (k)
		d = k->dmadev;
	mutex_unlock(&pdev_lock);
	return d;
}
EXPORT_SYMBOL(k26_pdev_dma_dev);

/* osl_cache_flush/inv get no device; every radio here is a non-coherent
 * direct-mapped DMA master, so any of them gives the right cache maintenance. */
struct device *k26_default_dma_dev(void)
{
	struct k26_pdev *k;
	struct device *d = NULL;

	mutex_lock(&pdev_lock);
	k = list_first_entry_or_null(&pdev_list, struct k26_pdev, list);
	if (k)
		d = k->dmadev;
	mutex_unlock(&pdev_lock);
	return d;
}
EXPORT_SYMBOL(k26_default_dma_dev);

/* drvdata: the blob calls dev_set_drvdata(&pdev->dev, wl), and 160 bytes of
 * 2.6.36 struct device is nothing like a 6.12 one, so keep it on the side. */
void wlc26_dev_set_drvdata(void *dev, void *data)
{
	struct k26_pdev *k;

	mutex_lock(&pdev_lock);
	k = pdev_of_any(dev);
	if (k)
		k->drvdata = data;
	mutex_unlock(&pdev_lock);
	if (!k)
		dev_set_drvdata((struct device *)dev, data);
}

void *wlc26_dev_get_drvdata(const void *dev)
{
	struct k26_pdev *k;
	void *r = NULL;

	mutex_lock(&pdev_lock);
	k = pdev_of_any((void *)dev);
	if (k)
		r = k->drvdata;
	mutex_unlock(&pdev_lock);
	return k ? r : dev_get_drvdata((const struct device *)dev);
}

/* ------------------------------------------------------------------ */
/* Config space. Real PCIe devices pass through; SoC cores get the	*/
/* synthetic space we built for them.					*/
/* ------------------------------------------------------------------ */
int wlc26_pci_read_config_dword(void *pdev, int where, u32 *val)
{
	struct k26_pdev *k;

	mutex_lock(&pdev_lock);
	k = pdev_of_any(pdev);
	mutex_unlock(&pdev_lock);
	if (!k)
		return -ENODEV;
	if (k->realpci)
		return pci_read_config_dword(k->realpci, where, val);
	*val = (where >= 0 && where < 256) ? k->cfgspace[where >> 2] : 0xffffffff;
	return 0;
}
EXPORT_SYMBOL(wlc26_pci_read_config_dword);

int wlc26_pci_write_config_dword(void *pdev, int where, u32 val)
{
	struct k26_pdev *k;

	mutex_lock(&pdev_lock);
	k = pdev_of_any(pdev);
	mutex_unlock(&pdev_lock);
	if (!k)
		return -ENODEV;
	if (k->realpci)
		return pci_write_config_dword(k->realpci, where, val);
	if (where >= 0 && where < 256)
		k->cfgspace[where >> 2] = val;
	return 0;
}
EXPORT_SYMBOL(wlc26_pci_write_config_dword);

/* ------------------------------------------------------------------ */
/* Driver registration the blob thinks it is doing.			*/
/* ------------------------------------------------------------------ */
int wlc26___pci_register_driver(void *drv, struct module *owner, const char *mod_name)
{
	blob_drv = drv;
	pr_info("wl: blob registered pci driver '%s'\n",
		blob_drv->name ? blob_drv->name : "wl");
	return 0;
}

void wlc26_pci_unregister_driver(void *drv)
{
	blob_drv = NULL;
}

int wlc26_pci_enable_device(void *pdev) { return 0; }
void wlc26_pci_disable_device(void *pdev) { }
void wlc26_pci_set_master(void *pdev) { }

/* ------------------------------------------------------------------ */
/* Building a fake pci_dev and running the blob's probe on it.		*/
/* ------------------------------------------------------------------ */
static struct k26_pdev *pdev_new(u16 vendor, u16 device, u32 irq,
				 resource_size_t regs, resource_size_t len,
				 struct device *dmadev, struct pci_dev *realpci)
{
	struct k26_pdev *k = kzalloc(sizeof(*k), GFP_KERNEL);

	if (!k)
		return NULL;
	k->dmadev = dmadev;
	k->realpci = realpci;

	k26_wr16(k->cfg, K26_PD_VENDOR, vendor);
	k26_wr16(k->cfg, K26_PD_DEVICE, device);
	k26_wr32(k->cfg, K26_PD_IRQ, irq);
	k26_wr32(k->cfg, K26_PD_RESOURCE, (u32)regs);		/* resource[0].start */
	k26_wr32(k->cfg, K26_PD_RESOURCE + 4, (u32)(regs + len - 1));	/* .end */

	/*
	 * Synthetic config space for a SoC core. siutils detects "a si core
	 * mimicking a pci core" by reading PCI_SPROM_CONTROL and finding it all
	 * ones, and then switches itself to SI_BUS - which is what we want,
	 * since there is no real config space and no BAR0 window to slide.
	 * So: everything reads as 0xffffffff.
	 */
	memset(k->cfgspace, 0xff, sizeof(k->cfgspace));

	mutex_lock(&pdev_lock);
	list_add_tail(&k->list, &pdev_list);
	mutex_unlock(&pdev_lock);
	return k;
}

static int pdev_probe(struct k26_pdev *k)
{
	int err;

	if (!blob_drv || !blob_drv->probe) {
		pr_err("wl: blob did not register a probe\n");
		return -ENODEV;
	}
	/*
	 * Pass the blob's own id table: its single match-all entry carries
	 * driver_data = &cmndata (a struct in the blob's .bss shared by all
	 * radios), which wl_attach dereferences.
	 */
	err = blob_drv->probe(k->cfg, blob_drv->id_table);
	if (err) {
		pr_err("wl: blob probe failed for %04x:%04x: %d\n",
		       k26_rd16(k->cfg, K26_PD_VENDOR),
		       k26_rd16(k->cfg, K26_PD_DEVICE), err);
		return err;
	}
	k->wl = k->drvdata;
	pr_info("wl: attached %04x:%04x regs 0x%08x irq %u\n",
		k26_rd16(k->cfg, K26_PD_VENDOR), k26_rd16(k->cfg, K26_PD_DEVICE),
		k26_rd32(k->cfg, K26_PD_RESOURCE), k26_rd32(k->cfg, K26_PD_IRQ));
	return 0;
}

static void pdev_free(struct k26_pdev *k)
{
	if (blob_drv && blob_drv->remove)
		blob_drv->remove(k->cfg);
	mutex_lock(&pdev_lock);
	list_del(&k->list);
	mutex_unlock(&pdev_lock);
	kfree(k);
}

/* ------------------------------------------------------------------ */
/* SoC backplane radios (bcma).						*/
/* ------------------------------------------------------------------ */
static u16 soc_devid(struct bcma_device *core)
{
	char key[24];
	const char *v;
	unsigned int i;

	/* nvram names the backplane radios sb/<unit>/..., e.g. sb/1/devid */
	for (i = 0; i < 4; i++) {
		scnprintf(key, sizeof(key), "sb/%u/devid", i);
		v = nvram_get(key);
		if (v)
			return simple_strtoul(v, NULL, 0);
	}
	return SOC_DEVID_DFLT;
}

static int wl_bcma_probe(struct bcma_device *core)
{
	struct k26_pdev *k;
	unsigned int irq;
	int err;

	/*
	 * BCM53573 exposes two D11 cores on the SoC bus. Only one is the real
	 * calibrated radio: the NVRAM carries an sb/1/... block and no sb/0/,
	 * so unit 1 is the one with PHY calibration. Attaching the other gives
	 * a radio that beacons at default power but cannot receive.
	 * A D11 core on a PCIe host bus (the BCM43217) goes through wl_pci.
	 */
	if (core->bus->hosttype != BCMA_HOSTTYPE_SOC || core->core_unit != coreunit)
		return -ENODEV;

	irq = bcma_core_irq(core, 0);
	if (irqunit >= 0 && irqunit != coreunit) {
		struct bcma_device *ic = bcma_find_core_unit(core->bus,
							     BCMA_CORE_80211, irqunit);

		if (ic) {
			irq = bcma_core_irq(ic, 0);
			pr_info("wl: taking the interrupt of D11 unit %d at 0x%08x: irq %u\n",
				irqunit, ic->addr, irq);
		}
	}

	k = pdev_new(BRCM_VENDOR, soc_devid(core), irq,
		     core->addr, 0x2000, &core->dev, NULL);
	if (!k)
		return -ENOMEM;
	err = pdev_probe(k);
	if (err) {
		pdev_free(k);
		return err;
	}
	bcma_set_drvdata(core, k);
	return 0;
}

static void wl_bcma_remove(struct bcma_device *core)
{
	struct k26_pdev *k = bcma_get_drvdata(core);

	if (k)
		pdev_free(k);
}

static const struct bcma_device_id wl_bcma_tbl[] = {
	BCMA_CORE(BCMA_MANUF_BCM, BCMA_CORE_80211, BCMA_ANY_REV, BCMA_ANY_CLASS),
	{},
};
MODULE_DEVICE_TABLE(bcma, wl_bcma_tbl);

static struct bcma_driver wl_bcma_driver = {
	.name		= "wl-bcma",
	.id_table	= wl_bcma_tbl,
	.probe		= wl_bcma_probe,
	.remove		= wl_bcma_remove,
};

/* ------------------------------------------------------------------ */
/* Real PCIe radios.							*/
/* ------------------------------------------------------------------ */
static int wl_pci_probe_real(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct k26_pdev *k;
	int err;

	err = pci_enable_device(pdev);
	if (err)
		return err;
	pci_set_master(pdev);
	if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32))) {
		dev_err(&pdev->dev, "wl: no 32-bit DMA\n");
		err = -EIO;
		goto disable;
	}

	k = pdev_new(pdev->vendor, pdev->device, pdev->irq,
		     pci_resource_start(pdev, 0), pci_resource_len(pdev, 0),
		     &pdev->dev, pdev);
	if (!k) {
		err = -ENOMEM;
		goto disable;
	}
	err = pdev_probe(k);
	if (err) {
		pdev_free(k);
		goto disable;
	}
	pci_set_drvdata(pdev, k);
	return 0;

disable:
	pci_disable_device(pdev);
	return err;
}

static void wl_pci_remove_real(struct pci_dev *pdev)
{
	struct k26_pdev *k = pci_get_drvdata(pdev);

	if (k)
		pdev_free(k);
	pci_disable_device(pdev);
}

static const struct pci_device_id wl_pci_tbl[] = {
	{ PCI_DEVICE(BRCM_VENDOR, PCI_ANY_ID),
	  .class = PCI_CLASS_NETWORK_OTHER << 8, .class_mask = 0xffff00 },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, wl_pci_tbl);

static struct pci_driver wl_pci_driver = {
	.name		= "wl-pci",
	.id_table	= wl_pci_tbl,
	.probe		= wl_pci_probe_real,
	.remove		= wl_pci_remove_real,
};

/* ------------------------------------------------------------------ */
/* Module.								*/
/* ------------------------------------------------------------------ */
static bool bcma_registered, pci_registered;

static int __init wlcompat_init(void)
{
	int err;

	nvram_init(NULL);	/* non-fatal: the blob reports its own failure */

	err = wl_blob_init();
	if (err) {
		pr_err("wl: blob init_module failed: %d\n", err);
		goto out_nvram;
	}

	err = bcma_driver_register(&wl_bcma_driver);
	if (err)
		pr_warn("wl: bcma_driver_register: %d\n", err);
	else
		bcma_registered = true;

	err = pci_register_driver(&wl_pci_driver);
	if (err)
		pr_warn("wl: pci_register_driver: %d\n", err);
	else
		pci_registered = true;

	if (!bcma_registered && !pci_registered) {
		err = -ENODEV;
		goto out_blob;
	}
	return 0;

out_blob:
	wl_blob_exit();
out_nvram:
	nvram_exit();
	return err;
}

static void __exit wlcompat_exit(void)
{
	if (pci_registered)
		pci_unregister_driver(&wl_pci_driver);
	if (bcma_registered)
		bcma_driver_unregister(&wl_bcma_driver);
	wl_blob_exit();
	k26_compat_exit();
	nvram_exit();
}

module_init(wlcompat_init);
module_exit(wlcompat_exit);

MODULE_DESCRIPTION("Broadcom wl (SDK 9.10.178.50) on Linux 6.12 via a 2.6.36 ABI shim");
/* Private, non-distributed build for the owner's own router: the bcma and mtd
 * glue needs GPL-only exports. Do not redistribute the resulting wl.ko. */
MODULE_LICENSE("GPL");
