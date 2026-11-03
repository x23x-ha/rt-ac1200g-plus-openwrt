/*
 * nvram.c - Broadcom NVRAM for the wl blob and for the stock userspace tools.
 *
 * Two consumers, one store:
 *
 *  - the driver (siutils/bcmsrom via getvar) just wants nvram_get(); the
 *    calibration data for both radios lives here (sb/1/... for the SoC 5 GHz
 *    radio, 0:... for the BCM43217 behind devpath0=pci/1/1/).
 *
 *  - nas/eapd/wl link against the stock libnvram.so, which talks to
 *    /dev/nvram: it mmap()s the whole variable area and read(2) returns the
 *    *offset* of a value within that mapping. So every value has to live
 *    inside one contiguous, page-aligned, mappable buffer - which is exactly
 *    how Broadcom's own nvram_linux.c lays it out, and why this keeps the
 *    raw "name=value\0..." block rather than copying into a list.
 *
 * OpenWrt does not expose the NVRAM partition, so we find the block by
 * scanning the MTD devices for the FLSH magic. Writes stay in RAM: commit is
 * deliberately a no-op, since nothing here should be rewriting the factory
 * calibration block.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mtd/mtd.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/string.h>

#define NVRAM_MAGIC	0x48534C46	/* "FLSH" */
#define NVRAM_HDR_SIZE	20
#define NVRAM_BUF_SIZE	0x10000		/* MAX_NVRAM_SPACE */
#define NVRAM_SCAN_WIN	0x200000	/* search the top 2 MiB of each device */

struct nv_var {
	struct list_head list;
	const char *name;	/* into nvram_buf */
	char *value;		/* into nvram_buf */
};

static char *nvram_buf;			/* page-aligned, mmap'd by libnvram */
static unsigned long nvram_buf_order;
static char *nv_end;			/* first free byte in nvram_buf */
static LIST_HEAD(nv_vars);
static DEFINE_MUTEX(nv_lock);
static bool nv_ready;

/* ------------------------------------------------------------------ */
/* Store									*/
/* ------------------------------------------------------------------ */
static struct nv_var *nv_find(const char *name)
{
	struct nv_var *v;

	list_for_each_entry(v, &nv_vars, list)
		if (!strcmp(v->name, name))
			return v;
	return NULL;
}

/* Index the "name=value\0" records already sitting in nvram_buf. */
static void nvram_index(size_t len)
{
	char *p = nvram_buf + NVRAM_HDR_SIZE;
	char *end = nvram_buf + len;

	while (p < end && *p) {
		char *rec = p, *eq;
		size_t n = strnlen(p, end - p);

		p += n + 1;
		eq = strchr(rec, '=');
		if (!eq)
			continue;
		*eq = '\0';
		{
			struct nv_var *v = kzalloc(sizeof(*v), GFP_KERNEL);

			if (!v)
				return;
			v->name = rec;
			v->value = eq + 1;
			list_add_tail(&v->list, &nv_vars);
		}
	}
	nv_end = p;
}

char *nvram_get(const char *name)
{
	struct nv_var *v;
	char *r = NULL;

	if (!nv_ready || !name)
		return NULL;
	mutex_lock(&nv_lock);
	v = nv_find(name);
	if (v)
		r = v->value;
	mutex_unlock(&nv_lock);
	return r;
}
EXPORT_SYMBOL(nvram_get);

int nvram_set(const char *name, const char *value)
{
	struct nv_var *v;
	size_t need;
	char *rec;
	int ret = 0;

	if (!nv_ready || !name || !value)
		return -EINVAL;

	mutex_lock(&nv_lock);
	v = nv_find(name);
	if (v && strlen(v->value) >= strlen(value)) {
		/* fits where it is: keep every existing mmap offset valid */
		strcpy(v->value, value);
		goto out;
	}

	need = strlen(name) + strlen(value) + 2;
	if (nv_end + need + 1 > nvram_buf + NVRAM_BUF_SIZE) {
		ret = -ENOSPC;
		goto out;
	}
	rec = nv_end;
	/* stored as "name\0value\0" so name and value are both C strings */
	strcpy(rec, name);
	strcpy(rec + strlen(name) + 1, value);
	nv_end += need;
	*nv_end = '\0';

	if (v) {
		v->name = rec;
		v->value = rec + strlen(name) + 1;
	} else {
		v = kzalloc(sizeof(*v), GFP_KERNEL);
		if (!v) {
			ret = -ENOMEM;
			goto out;
		}
		v->name = rec;
		v->value = rec + strlen(name) + 1;
		list_add_tail(&v->list, &nv_vars);
	}
out:
	mutex_unlock(&nv_lock);
	return ret;
}
EXPORT_SYMBOL(nvram_set);

int nvram_unset(const char *name)
{
	struct nv_var *v;

	if (!nv_ready || !name)
		return -EINVAL;
	mutex_lock(&nv_lock);
	v = nv_find(name);
	if (v) {
		list_del(&v->list);
		kfree(v);
	}
	mutex_unlock(&nv_lock);
	return 0;
}
EXPORT_SYMBOL(nvram_unset);

/* Fill buf with "name=value\0name=value\0...\0". */
int nvram_getall(char *buf, int count)
{
	struct nv_var *v;
	int used = 0;

	if (!buf || count <= 0)
		return -EINVAL;
	mutex_lock(&nv_lock);
	list_for_each_entry(v, &nv_vars, list) {
		int n = strlen(v->name) + strlen(v->value) + 2;

		if (used + n + 1 > count) {
			mutex_unlock(&nv_lock);
			return -EOVERFLOW;
		}
		used += scnprintf(buf + used, count - used, "%s=%s",
				  v->name, v->value) + 1;
	}
	if (used < count)
		buf[used] = '\0';
	mutex_unlock(&nv_lock);
	return 0;
}
EXPORT_SYMBOL(nvram_getall);

/* Deliberately not wired to flash: this block is factory calibration. */
int nvram_commit(void)
{
	return 0;
}
EXPORT_SYMBOL(nvram_commit);

/* ------------------------------------------------------------------ */
/* /dev/nvram - the protocol stock libnvram.so expects			*/
/* ------------------------------------------------------------------ */
static ssize_t dev_nvram_read(struct file *f, char __user *buf, size_t count,
			      loff_t *ppos)
{
	char tmp[128], *name = tmp, *value;
	unsigned long off;
	ssize_t ret;

	if (count + 1 > sizeof(tmp)) {
		name = kmalloc(count + 1, GFP_KERNEL);
		if (!name)
			return -ENOMEM;
	}
	if (copy_from_user(name, buf, count)) {
		ret = -EFAULT;
		goto done;
	}
	name[count] = '\0';

	if (*name == '\0') {
		/* whole table */
		ret = nvram_getall(name, count);
		if (ret == 0)
			ret = copy_to_user(buf, name, count) ? -EFAULT : count;
		goto done;
	}

	value = nvram_get(name);
	if (!value) {
		ret = 0;
		goto done;
	}
	/* hand back the offset into the mmap()ed buffer */
	off = (unsigned long)value - (unsigned long)nvram_buf;
	ret = copy_to_user(buf, &off, sizeof(off)) ? -EFAULT : sizeof(off);
done:
	if (name != tmp)
		kfree(name);
	return ret;
}

static ssize_t dev_nvram_write(struct file *f, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	char tmp[512], *name = tmp, *value;
	ssize_t ret;

	if (count + 1 > sizeof(tmp)) {
		name = kmalloc(count + 1, GFP_KERNEL);
		if (!name)
			return -ENOMEM;
	}
	if (copy_from_user(name, buf, count)) {
		ret = -EFAULT;
		goto done;
	}
	name[count] = '\0';
	value = name;
	name = strsep(&value, "=");
	ret = value ? nvram_set(name, value) : nvram_unset(name);
	if (ret == 0)
		ret = count;
done:
	if (name != tmp && name)
		kfree(name);
	return ret;
}

static long dev_nvram_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	if (cmd != NVRAM_MAGIC)
		return -EINVAL;
	return nvram_commit();
}

static int dev_nvram_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long pfn = __pa(nvram_buf) >> PAGE_SHIFT;

	if (vma->vm_end - vma->vm_start > NVRAM_BUF_SIZE)
		return -EINVAL;
	if (remap_pfn_range(vma, vma->vm_start, pfn,
			    vma->vm_end - vma->vm_start, vma->vm_page_prot))
		return -EAGAIN;
	return 0;
}

static const struct file_operations dev_nvram_fops = {
	.owner		= THIS_MODULE,
	.llseek		= noop_llseek,
	.read		= dev_nvram_read,
	.write		= dev_nvram_write,
	.unlocked_ioctl	= dev_nvram_ioctl,
	.compat_ioctl	= dev_nvram_ioctl,
	.mmap		= dev_nvram_mmap,
};

static struct miscdevice nvram_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "nvram",
	.fops	= &dev_nvram_fops,
	.mode	= 0600,
};
static bool nvram_dev_up;

/* ------------------------------------------------------------------ */
/* Load from flash							*/
/* ------------------------------------------------------------------ */
struct nv_hdr {
	u32 magic, len, crc_ver_init, config_refresh, config_ncdl;
};

static size_t nvram_scan_mtd(struct mtd_info *mtd)
{
	loff_t start, off;
	size_t rd, len;
	struct nv_hdr hdr;

	start = mtd->size > NVRAM_SCAN_WIN ? mtd->size - NVRAM_SCAN_WIN : 0;
	for (off = start; off + NVRAM_HDR_SIZE < mtd->size; off += 0x1000) {
		if (mtd_read(mtd, off, sizeof(hdr), &rd, (u_char *)&hdr) ||
		    rd != sizeof(hdr))
			continue;
		if (le32_to_cpu(hdr.magic) != NVRAM_MAGIC)
			continue;
		len = le32_to_cpu(hdr.len);
		if (len < NVRAM_HDR_SIZE || len > NVRAM_BUF_SIZE)
			continue;
		if (off + len > mtd->size)
			continue;
		if (mtd_read(mtd, off, len, &rd, nvram_buf) || rd != len)
			continue;
		pr_info("wl: NVRAM found on %s at 0x%llx, %zu bytes\n",
			mtd->name, off, len);
		return len;
	}
	return 0;
}

int nvram_init(void *sih)
{
	struct mtd_info *mtd;
	size_t len = 0;
	int i;

	if (nv_ready)
		return 0;

	nvram_buf_order = get_order(NVRAM_BUF_SIZE);
	nvram_buf = (char *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
					     nvram_buf_order);
	if (!nvram_buf)
		return -ENOMEM;

	for (i = 0; i < 32 && !len; i++) {
		mtd = get_mtd_device(NULL, i);
		if (IS_ERR(mtd))
			continue;
		len = nvram_scan_mtd(mtd);
		put_mtd_device(mtd);
	}
	if (!len) {
		pr_err("wl: no Broadcom NVRAM block found - radio uncalibrated\n");
		free_pages((unsigned long)nvram_buf, nvram_buf_order);
		nvram_buf = NULL;
		return -ENODEV;
	}

	nvram_index(len);
	nv_ready = true;
	pr_info("wl: NVRAM parsed, %u variables\n", list_count_nodes(&nv_vars));

	if (misc_register(&nvram_miscdev))
		pr_warn("wl: /dev/nvram not available\n");
	else {
		nvram_dev_up = true;
		pr_info("wl: /dev/nvram is char 10:%d\n", nvram_miscdev.minor);
	}
	return 0;
}
EXPORT_SYMBOL(nvram_init);

void nvram_exit(void)
{
	struct nv_var *v, *t;

	if (nvram_dev_up) {
		misc_deregister(&nvram_miscdev);
		nvram_dev_up = false;
	}
	list_for_each_entry_safe(v, t, &nv_vars, list) {
		list_del(&v->list);
		kfree(v);
	}
	if (nvram_buf) {
		free_pages((unsigned long)nvram_buf, nvram_buf_order);
		nvram_buf = NULL;
	}
	nv_ready = false;
}
