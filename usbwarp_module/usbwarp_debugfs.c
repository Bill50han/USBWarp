// SPDX-License-Identifier: GPL-2.0-only
/*
 * usbwarp_debugfs.c — debugfs interface for fuzz testing.
 *
 * Provides /sys/kernel/debug/usbwarp/ with:
 *   inject_g2h   (write-only)  Write raw bytes directly to G2H Ring
 *   pause_g2h    (read/write)  Pause/resume Guest G2H production
 *   ring_status   (read-only)   Current Ring indices and stats
 *
 * All debugfs files are only created when module param debug=1:
 *   sudo insmod usbwarp.ko debug=1
 *
 * When debug=0 (default), usbwarp_debugfs_g2h_paused() always returns
 * false — zero overhead on the hot path.
 *
 * SAFETY: The inject_g2h interface bypasses all normal USB Core and HCD
 * validation.  The pause_g2h interface must be used BEFORE injecting to
 * prevent SPSC Ring violation (fuzz injector must be the sole G2H producer).
 *
 * Always compiled as part of usbwarp.ko (Makefile includes usbwarp_debugfs.o).
 */

#include "usbwarp_guest.h"
#include <linux/debugfs.h>
#include <linux/uaccess.h>

/* Module parameter defined in usbwarp_pci.c */
extern bool usbwarp_debug;

static struct dentry *dbg_dir;

/* Global pointer to the active HCD — set during probe, cleared on remove.
 * Protected by RCU for safe access from debugfs handlers. */
static struct usbwarp_hcd *g_warp_hcd __read_mostly;
static DEFINE_SPINLOCK(g_warp_lock);

/* Fuzz state: when fuzz_paused is set, the Guest poll thread stops
 * writing to the G2H Ring (heartbeats and URB submits).  This prevents
 * SPSC violation — G2H Ring must have exactly ONE producer at a time. */
static atomic_t fuzz_paused = ATOMIC_INIT(0);

/* Statistics */
static atomic_t fuzz_inject_count = ATOMIC_INIT(0);
static atomic_t fuzz_inject_bytes = ATOMIC_INIT(0);
static atomic_t fuzz_inject_fail  = ATOMIC_INIT(0);

void usbwarp_debugfs_set_hcd(struct usbwarp_hcd *w)
{
	spin_lock(&g_warp_lock);
	g_warp_hcd = w;
	spin_unlock(&g_warp_lock);
}

/* Check if G2H production is paused (called by poll thread).
 * Always returns false when debug=false (zero overhead). */
bool usbwarp_debugfs_g2h_paused(void)
{
	if (!usbwarp_debug)
		return false;
	return atomic_read(&fuzz_paused) != 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * inject_g2h — write raw bytes to G2H Ring
 *
 * Usage from user space:
 *   echo -ne '\x55\x48\x44\x4d...' > /sys/kernel/debug/usbwarp/inject_g2h
 *   # or from the fuzz generator:
 *   ./ring_fuzz > /sys/kernel/debug/usbwarp/inject_g2h
 * ═══════════════════════════════════════════════════════════════════════════ */

static ssize_t inject_g2h_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct usbwarp_hcd *w;
	uint8_t *kbuf;
	int ret;

	if (count == 0 || count > 4096)
		return -EINVAL;

	spin_lock(&g_warp_lock);
	w = g_warp_hcd;
	spin_unlock(&g_warp_lock);

	if (!w || !w->g2h.hdr)
		return -ENODEV;

	kbuf = kmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	if (copy_from_user(kbuf, ubuf, count)) {
		kfree(kbuf);
		return -EFAULT;
	}

	/* Write raw bytes directly into G2H Ring — no validation.
	 * This is intentional: we're testing Host-side validation. */
	ret = usbwarp_ring_produce(&w->g2h, kbuf, count);

	if (ret == 0) {
		atomic_inc(&fuzz_inject_count);
		atomic_add(count, &fuzz_inject_bytes);
	} else {
		atomic_inc(&fuzz_inject_fail);
	}

	kfree(kbuf);
	return ret == 0 ? count : ret;
}

static const struct file_operations inject_g2h_fops = {
	.owner = THIS_MODULE,
	.write = inject_g2h_write,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * pause_g2h — pause/resume Guest G2H production
 *
 * Usage:
 *   echo 1 > /sys/kernel/debug/usbwarp/pause_g2h   # pause
 *   ./ring_fuzz                                      # inject fuzz data
 *   echo 0 > /sys/kernel/debug/usbwarp/pause_g2h   # resume
 *
 * While paused, the Guest poll thread stops writing heartbeats and
 * URB submits to the G2H Ring.  This ensures the fuzz injector is
 * the sole producer, preserving the SPSC invariant.
 * ═══════════════════════════════════════════════════════════════════════════ */

static ssize_t pause_g2h_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	char c;

	if (count < 1)
		return -EINVAL;
	if (get_user(c, ubuf))
		return -EFAULT;

	if (c == '1') {
		atomic_set(&fuzz_paused, 1);
		pr_info("usbwarp: G2H production PAUSED for fuzz\n");
	} else if (c == '0') {
		atomic_set(&fuzz_paused, 0);
		pr_info("usbwarp: G2H production RESUMED\n");
	} else {
		return -EINVAL;
	}

	return count;
}

static ssize_t pause_g2h_read(struct file *file, char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	char buf[4];
	int len;

	len = snprintf(buf, sizeof(buf), "%d\n", atomic_read(&fuzz_paused));
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static const struct file_operations pause_g2h_fops = {
	.owner = THIS_MODULE,
	.write = pause_g2h_write,
	.read  = pause_g2h_read,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * ring_status — show current Ring indices
 * ═══════════════════════════════════════════════════════════════════════════ */

static int ring_status_show(struct seq_file *s, void *v)
{
	struct usbwarp_hcd *w;

	spin_lock(&g_warp_lock);
	w = g_warp_hcd;
	spin_unlock(&g_warp_lock);

	if (!w || !w->g2h.hdr) {
		seq_puts(s, "not connected\n");
		return 0;
	}

	seq_printf(s, "g2h: pi=%u ci=%u data_size=%u\n",
		   ioread32(&w->g2h.hdr->producer_index),
		   ioread32(&w->g2h.hdr->consumer_index),
		   w->g2h.data_size);
	seq_printf(s, "h2g: pi=%u ci=%u data_size=%u\n",
		   ioread32(&w->h2g.hdr->producer_index),
		   ioread32(&w->h2g.hdr->consumer_index),
		   w->h2g.data_size);
	seq_printf(s, "fuzz: injected=%d bytes=%d failed=%d\n",
		   atomic_read(&fuzz_inject_count),
		   atomic_read(&fuzz_inject_bytes),
		   atomic_read(&fuzz_inject_fail));
	return 0;
}

static int ring_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, ring_status_show, NULL);
}

static const struct file_operations ring_status_fops = {
	.owner   = THIS_MODULE,
	.open    = ring_status_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Init / cleanup
 * ═══════════════════════════════════════════════════════════════════════════ */

int usbwarp_debugfs_init(void)
{
	if (!usbwarp_debug) {
		pr_info("usbwarp: debugfs disabled (load with debug=1 to enable)\n");
		return 0;
	}

	dbg_dir = debugfs_create_dir("usbwarp", NULL);
	if (IS_ERR_OR_NULL(dbg_dir))
		return dbg_dir ? PTR_ERR(dbg_dir) : -ENOMEM;

	debugfs_create_file("inject_g2h", 0200, dbg_dir, NULL,
			    &inject_g2h_fops);
	debugfs_create_file("ring_status", 0444, dbg_dir, NULL,
			    &ring_status_fops);
	debugfs_create_file("pause_g2h", 0644, dbg_dir, NULL,
			    &pause_g2h_fops);

	pr_info("usbwarp: debugfs interface created\n");
	return 0;
}

void usbwarp_debugfs_exit(void)
{
	debugfs_remove_recursive(dbg_dir);
	dbg_dir = NULL;
}
