// SPDX-License-Identifier: GPL-2.0
/*
 * usbwarp_pci.c — PCI driver registration, probe, remove, module init/exit.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * FIX: Cache data_region_offset from Control Block into struct usbwarp_hcd
 *      so that usbwarp_buf_offset() doesn't need ioread32 on every call.
 */

#include "usbwarp_guest.h"
#include "usbwarp_layout.h"
#include "usbwarp_version.h"
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Module parameters
 * ═══════════════════════════════════════════════════════════════════════════ */

bool     usbwarp_enable_iso = false;
module_param_named(enable_iso, usbwarp_enable_iso, bool, 0644);
MODULE_PARM_DESC(enable_iso, "Enable isochronous transfer support (default: false)");

unsigned usbwarp_max_pending = USBWARP_PENDING_URBS_DEFAULT;
module_param_named(max_pending, usbwarp_max_pending, uint, 0644);
MODULE_PARM_DESC(max_pending, "Max pending URBs per device (default: 256)");

unsigned usbwarp_poll_us = 0;
static int usbwarp_set_poll_us(const char *value, const struct kernel_param *kp)
{
    unsigned int interval;
    int ret = kstrtouint(value, 0, &interval);
    if (ret) return ret;
    if (interval > 1000) return -EINVAL;
    WRITE_ONCE(*(unsigned int *)kp->arg, interval);
    return 0;
}
static const struct kernel_param_ops usbwarp_poll_ops = {
    .set = usbwarp_set_poll_us,
    .get = param_get_uint,
};
module_param_cb(poll_us, &usbwarp_poll_ops, &usbwarp_poll_us, 0644);
MODULE_PARM_DESC(poll_us, "Empty-poll sleep us (1..1000), 0=adaptive; ISO capped at 10 us");

bool     usbwarp_debug = false;
module_param_named(debug, usbwarp_debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debug logging (default: false)");

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  PCI device table
 * ═══════════════════════════════════════════════════════════════════════════ */

static const struct pci_device_id usbwarp_pci_ids[] = {
	{ PCI_DEVICE(USBWARP_PCI_VENDOR_ID, USBWARP_PCI_DEVICE_ID) },
	{ 0 },
};
MODULE_DEVICE_TABLE(pci, usbwarp_pci_ids);

/* HCS cannot hot-remove a FlexibleIov PCI instance. Retire the dead USB buses
 * and mappings locally so reconnecting does not exhaust USB bus numbers. The
 * small PCI shell is reclaimed by the platform at VM teardown. */
static void usbwarp_retire_work(struct work_struct *work)
{
    struct usbwarp_hcd *w = container_of(work, struct usbwarp_hcd, retire_work);
    usbwarp_poll_stop(w);
    usbwarp_irq_cleanup(w);
    usbwarp_debugfs_clear_hcd(w);
    usb_remove_hcd(w->hcd[1]);
    usb_remove_hcd(w->hcd[0]);
    usb_put_hcd(w->hcd[1]);
    usb_put_hcd(w->hcd[0]);
    w->hcd[0] = w->hcd[1] = NULL;
    w->hcds_removed = true;
    usbwarp_buf_cleanup(w);
    pci_iounmap(w->pdev, w->bar0);
    w->bar0 = NULL;
    w->cb = NULL;
    if (w->doorbell) { pci_iounmap(w->pdev, w->doorbell); w->doorbell = NULL; }
    dev_info(&w->pdev->dev, "usbwarp: inactive session retired\n");
}

void usbwarp_retire_hcd(struct usbwarp_hcd *w)
{
    unsigned long flags;
    spin_lock_irqsave(&w->dev_lock, flags);
    if (!w->removing)
        schedule_work(&w->retire_work);
    spin_unlock_irqrestore(&w->dev_lock, flags);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Probe
 * ═══════════════════════════════════════════════════════════════════════════ */

static int usbwarp_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct usb_hcd *hcd, *shared;
	struct usbwarp_hcd *w;
	int ret, retries;
	uint32_t magic;
	void __iomem *bar0;
	struct usbwarp_control_block __iomem *cb;
	uint32_t g2h_off, g2h_sz, h2g_off, h2g_sz;
	uint32_t dr_off, dr_sz, buf_sz, buf_cnt;
    struct usbwarp_shm_layout layout;
    resource_size_t bar_size = pci_resource_len(pdev, 0);

	(void)id;

	dev_info(&pdev->dev, "usbwarp: probing PCI %04x:%04x\n",
		 pdev->vendor, pdev->device);

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "usbwarp: pci_enable_device failed: %d\n",
			ret);
		return ret;
	}

    /* This is a CPU-copy HCD (no HCD_DMA), but usb-storage still consults the
     * sysdev DMA aperture when setting max_hw_sectors. A default 32-bit PCI
     * mask needlessly selects SWIOTLB's 256-KiB limit on a >4-GiB guest.
     * Describe the 64-bit aperture; this does not enable bus mastering or DMA.
     * Device quirks and the protocol's transfer limit continue to apply. */
    {
        size_t before = dma_max_mapping_size(&pdev->dev);
        if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64)))
            dev_warn(&pdev->dev, "usbwarp: keeping platform DMA aperture limit\n");
        dev_info(&pdev->dev, "usbwarp: mapping size limit %zu -> %zu (CPU-copy HCD)\n",
                 before, dma_max_mapping_size(&pdev->dev));
    }

	ret = pci_request_regions(pdev, "usbwarp");
	if (ret) {
		dev_err(&pdev->dev, "usbwarp: pci_request_regions failed: %d\n",
			ret);
		goto err_disable;
	}

    if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
        bar_size < USBWARP_SHM_SIZE_MIN || bar_size > USBWARP_SHM_SIZE_MAX) {
        ret = -EINVAL;
        goto err_release;
    }
	bar0 = pci_iomap(pdev, 0, 0);
	if (!bar0) {
		dev_err(&pdev->dev, "usbwarp: pci_iomap BAR0 failed\n");
		ret = -ENOMEM;
		goto err_release;
	}

	dev_info(&pdev->dev, "usbwarp: BAR0 mapped at %pK, size=%llu\n",
		 bar0, (unsigned long long)pci_resource_len(pdev, 0));

	/* Wait for Control Block magic (host may still be mapping). */
	cb = (struct usbwarp_control_block __iomem *)bar0;

	magic = 0;
	for (retries = 0; retries < 50; retries++) {
		magic = ioread32(&cb->magic);
		if (magic == USBWARP_CONTROL_BLOCK_MAGIC)
			break;
		msleep(10);
	}

	if (magic != USBWARP_CONTROL_BLOCK_MAGIC) {
		dev_err(&pdev->dev,
			"usbwarp: CB magic mismatch: 0x%08x (expected 0x%08x) "
			"after %d retries\n",
			magic, USBWARP_CONTROL_BLOCK_MAGIC, retries);
		ret = -ENODEV;
		goto err_unmap;
	}

	dev_info(&pdev->dev,
		 "usbwarp: Control Block verified (%d retries)\n", retries);

    /* This device's explicit RAM contract permits coherent CPU caching. A
     * normal device BAR must never be mapped this way. Keep UC for old hosts. */
    if ((ioread32(&cb->flags) & USBWARP_FLAG_COHERENT_RAM) &&
        (pci_resource_flags(pdev, 0) & IORESOURCE_PREFETCH)) {
        pci_iounmap(pdev, bar0);
        bar0 = ioremap_cache(pci_resource_start(pdev, 0), pci_resource_len(pdev, 0));
        if (!bar0) { ret = -ENOMEM; goto err_release; }
        cb = (struct usbwarp_control_block __iomem *)bar0;
        dev_info(&pdev->dev, "usbwarp: coherent section RAM mapped write-back\n");
    }

	/* A previous guest may still own DMA buffers. Reusing its bitmap would
	 * expose new requests to late writes. A new service session resets this. */
	if (ioread32(&cb->guest_state) != USBWARP_STATE_INIT &&
	    ioread32(&cb->guest_state) != USBWARP_STATE_READY) {
		dev_err(&pdev->dev, "usbwarp: previous session present; restart the service before reloading\n");
		ret = -EBUSY;
		goto err_unmap;
	}

	/* Parse layout — read once and cache locally. */
	g2h_off = ioread32(&cb->g2h_ring_offset);
	g2h_sz  = ioread32(&cb->g2h_ring_size);
	h2g_off = ioread32(&cb->h2g_ring_offset);
	h2g_sz  = ioread32(&cb->h2g_ring_size);
	dr_off  = ioread32(&cb->data_region_offset);
	dr_sz   = ioread32(&cb->data_region_size);
	buf_sz  = ioread32(&cb->buffer_size);
	buf_cnt = ioread32(&cb->buffer_count);

	dev_info(&pdev->dev,
		 "usbwarp: layout g2h@0x%x(%u) h2g@0x%x(%u) "
		 "data@0x%x(%u) buf=%uK x%u\n",
		 g2h_off, g2h_sz, h2g_off, h2g_sz,
		 dr_off, dr_sz, buf_sz / 1024, buf_cnt);

    layout.total_size = ioread32(&cb->shm_total_size);
    layout.g2h_offset = g2h_off; layout.g2h_size = g2h_sz;
    layout.h2g_offset = h2g_off; layout.h2g_size = h2g_sz;
    layout.data_offset = dr_off; layout.data_size = dr_sz;
    layout.buffer_size = buf_sz; layout.buffer_count = buf_cnt;
	if (ioread16(&cb->protocol_version_major) != USBWARP_PROTOCOL_VERSION_MAJOR ||
        !usbwarp_layout_valid(&layout, bar_size)) {
		dev_err(&pdev->dev, "usbwarp: invalid SHM layout\n");
		ret = -EINVAL;
		goto err_unmap;
	}

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w) { ret = -ENOMEM; goto err_unmap; }
    INIT_WORK(&w->retire_work, usbwarp_retire_work);
	hcd = usb_create_hcd(&usbwarp_hc_driver, &pdev->dev, "usbwarp-usb2");
	if (!hcd) { ret = -ENOMEM; goto err_free_w; }
	w->hcd[0] = hcd;
	hcd_to_rh(hcd)->warp = w;

	w->pdev      = pdev;
	w->bar0      = bar0;
	w->bar0_size = (uint32_t)pci_resource_len(pdev, 0);
	w->cb        = cb;

	/* Init Ring wrappers. */
	usbwarp_ring_init(&w->g2h,
			  (struct usbwarp_ring_header __iomem *)(bar0 + g2h_off),
			  bar0 + g2h_off + USBWARP_RING_HEADER_SIZE,
                  g2h_sz - USBWARP_RING_HEADER_SIZE);
	usbwarp_ring_init(&w->h2g,
			  (struct usbwarp_ring_header __iomem *)(bar0 + h2g_off),
			  bar0 + h2g_off + USBWARP_RING_HEADER_SIZE,
                  h2g_sz - USBWARP_RING_HEADER_SIZE);

	w->data_region        = bar0 + dr_off;
	w->data_region_size   = dr_sz;
	w->buffer_size        = buf_sz;
	w->buffer_count       = buf_cnt;
	w->data_region_offset = dr_off;  /* cached — avoids ioread32 per URB */
	w->max_pending_urbs   = usbwarp_max_pending;
    if (ioread32(&cb->flags) & USBWARP_FLAG_G2H_DOORBELL) {
        if (!(pci_resource_flags(pdev,2) & IORESOURCE_MEM) || pci_resource_len(pdev,2)<PAGE_SIZE) {
            ret=-ENODEV; goto err_put_hcd;
        }
        w->doorbell=pci_iomap(pdev,2,PAGE_SIZE);
        if (!w->doorbell) { ret=-ENOMEM; goto err_put_hcd; }
        w->g2h.doorbell=w->doorbell;
        w->h2g.doorbell=w->doorbell; /* ring-space credit notification */
    }

	/* Init buffer bitmap. */
	ret = usbwarp_buf_init(w);
	if (ret)
		goto err_put_hcd;

	/* Init locks, hash table, counters. */
	spin_lock_init(&w->dev_lock);
	spin_lock_init(&w->urb_lock);
	hash_init(w->urb_table);
	atomic_set(&w->txn_counter, 0);
	atomic_set(&w->total_pending, 0);
	atomic_set(&w->tx_pending, 0);
	atomic_set(&w->iso_pending, 0);
	init_completion(&w->shutdown_ack);
	mutex_init(&w->config_lock);
	init_completion(&w->config_done);

	pci_set_drvdata(pdev, w);

	/* Declare built-in Transaction Translator (TT).
	 * As a USB 2.0 HCD (HCD_USB2), we must have a TT to handle
	 * full-speed and low-speed devices.  Without this, Linux USB core
	 * refuses to enumerate full-speed devices with "parent hub has no TT".
	 * This is what dummy_hcd and vhci-hcd do. */
	hcd->has_tt = 1;

	ret = usb_add_hcd(hcd, 0, 0);
	if (ret) {
		dev_err(&pdev->dev, "usbwarp: usb_add_hcd failed: %d\n", ret);
		goto err_buf;
	}

	shared = usb_create_shared_hcd(&usbwarp_hc_driver, &pdev->dev, "usbwarp-usb3", hcd);
	if (!shared) { ret = -ENOMEM; goto err_remove_hcd; }
	w->hcd[1] = shared;
	hcd_to_rh(shared)->warp = w;
	ret = usb_add_hcd(shared, 0, 0);
	if (ret) goto err_put_shared;

	device_wakeup_enable(hcd->self.controller);

	usbwarp_debugfs_set_hcd(w);
	usbwarp_irq_init(w);

	ret = usbwarp_poll_start(w);
	if (ret) {
		dev_err(&pdev->dev, "usbwarp: poll thread failed: %d\n", ret);
		goto err_remove_shared;
	}

	ret = usbwarp_send_negotiate(w);
	if (ret)
		dev_warn(&pdev->dev,
			 "usbwarp: negotiate send failed: %d\n", ret);

	dev_info(&pdev->dev, "usbwarp: probe complete — HCD registered\n");
	return 0;

err_remove_shared:
    usbwarp_irq_cleanup(w);
    usbwarp_debugfs_clear_hcd(w);
	usb_remove_hcd(shared);
err_put_shared:
	usb_put_hcd(shared);
err_remove_hcd:
	usb_remove_hcd(hcd);
err_buf:
	usbwarp_buf_cleanup(w);
err_put_hcd:
    if (w->doorbell) pci_iounmap(pdev,w->doorbell);
	usb_put_hcd(hcd);
err_free_w:
	kfree(w);
err_unmap:
	pci_set_drvdata(pdev, NULL);
	pci_iounmap(pdev, bar0);
err_release:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
	return ret;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Remove
 * ═══════════════════════════════════════════════════════════════════════════ */

static void usbwarp_pci_remove(struct pci_dev *pdev)
{
	struct usbwarp_hcd *w = pci_get_drvdata(pdev);
    unsigned long flags;

	if (!w)
		return;

	dev_info(&pdev->dev, "usbwarp: removing\n");

    spin_lock_irqsave(&w->dev_lock, flags);
    w->removing = true;
    spin_unlock_irqrestore(&w->dev_lock, flags);
    cancel_work_sync(&w->retire_work);
	usbwarp_debugfs_clear_hcd(w);
	usbwarp_guest_disconnect(w);
	WRITE_ONCE(w->shutting_down, true);
	usbwarp_poll_stop(w);
	usbwarp_irq_cleanup(w);
    if (!w->hcds_removed) {
        usbwarp_shutdown_all_devices(w, USBWARP_REMOVE_HOST_SHUTDOWN);
        usb_remove_hcd(w->hcd[1]);
        usb_remove_hcd(w->hcd[0]);
    }
	usbwarp_buf_cleanup(w);

	if (w->bar0)
		pci_iounmap(pdev, w->bar0);
    if (w->doorbell) pci_iounmap(pdev,w->doorbell);

    if (w->hcd[1]) usb_put_hcd(w->hcd[1]);
    if (w->hcd[0]) usb_put_hcd(w->hcd[0]);
	kfree(w);
	pci_set_drvdata(pdev, NULL);

	pci_release_regions(pdev);
	pci_disable_device(pdev);

	dev_info(&pdev->dev, "usbwarp: removed\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  PCI driver struct + module init/exit
 * ═══════════════════════════════════════════════════════════════════════════ */

static struct pci_driver usbwarp_pci_driver = {
	.name     = "usbwarp",
	.id_table = usbwarp_pci_ids,
	.probe    = usbwarp_pci_probe,
	.remove   = usbwarp_pci_remove,
};

static int __init usbwarp_init(void)
{
	int ret;

	pr_info("usbwarp: loading (protocol %u.%u)\n",
		USBWARP_PROTOCOL_VERSION_MAJOR,
		USBWARP_PROTOCOL_VERSION_MINOR);

	usbwarp_debugfs_init();

	ret = pci_register_driver(&usbwarp_pci_driver);
	if (ret)
		usbwarp_debugfs_exit();
	return ret;
}

static void __exit usbwarp_exit(void)
{
	pci_unregister_driver(&usbwarp_pci_driver);
	usbwarp_debugfs_exit();
	pr_info("usbwarp: unloaded\n");
}

module_init(usbwarp_init);
module_exit(usbwarp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("UsbWarp Project");
MODULE_DESCRIPTION("UsbWarp — Virtual USB HCD over HDV Shared Memory");
MODULE_VERSION(USBWARP_VERSION_STRING);
