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
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/io.h>

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
module_param_named(poll_us, usbwarp_poll_us, uint, 0644);
MODULE_PARM_DESC(poll_us, "Polling interval us, 0=adaptive (default: 0)");

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

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Probe
 * ═══════════════════════════════════════════════════════════════════════════ */

static int usbwarp_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct usb_hcd *hcd;
	struct usbwarp_hcd *w;
	int ret, retries;
	uint32_t magic;
	void __iomem *bar0;
	struct usbwarp_control_block __iomem *cb;
	uint32_t g2h_off, g2h_sz, h2g_off, h2g_sz;
	uint32_t dr_off, dr_sz, buf_sz, buf_cnt;

	(void)id;

	dev_info(&pdev->dev, "usbwarp: probing PCI %04x:%04x\n",
		 pdev->vendor, pdev->device);

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "usbwarp: pci_enable_device failed: %d\n",
			ret);
		return ret;
	}

	ret = pci_request_regions(pdev, "usbwarp");
	if (ret) {
		dev_err(&pdev->dev, "usbwarp: pci_request_regions failed: %d\n",
			ret);
		goto err_disable;
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

	if (g2h_sz < USBWARP_RING_HEADER_SIZE ||
	    h2g_sz < USBWARP_RING_HEADER_SIZE ||
	    buf_cnt == 0 || buf_sz == 0) {
		dev_err(&pdev->dev, "usbwarp: invalid SHM layout\n");
		ret = -EINVAL;
		goto err_unmap;
	}

	/* Create USB HCD. */
	hcd = usb_create_hcd(&usbwarp_hc_driver, &pdev->dev, "usbwarp");
	if (!hcd) {
		dev_err(&pdev->dev, "usbwarp: usb_create_hcd failed\n");
		ret = -ENOMEM;
		goto err_unmap;
	}

	w = hcd_to_warp(hcd);
	memset(w, 0, sizeof(*w));

	w->pdev      = pdev;
	w->bar0      = bar0;
	w->bar0_size = (uint32_t)pci_resource_len(pdev, 0);
	w->cb        = cb;

	/* Init Ring wrappers. */
	usbwarp_ring_init(&w->g2h,
			  (struct usbwarp_ring_header __iomem *)(bar0 + g2h_off),
			  bar0 + g2h_off + USBWARP_RING_HEADER_SIZE);
	usbwarp_ring_init(&w->h2g,
			  (struct usbwarp_ring_header __iomem *)(bar0 + h2g_off),
			  bar0 + h2g_off + USBWARP_RING_HEADER_SIZE);

	w->data_region        = bar0 + dr_off;
	w->data_region_size   = dr_sz;
	w->buffer_size        = buf_sz;
	w->buffer_count       = buf_cnt;
	w->data_region_offset = dr_off;  /* cached — avoids ioread32 per URB */
	w->max_pending_urbs   = usbwarp_max_pending;

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
	w->port_change = 0;

	pci_set_drvdata(pdev, hcd);

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

	device_wakeup_enable(hcd->self.controller);

	usbwarp_debugfs_set_hcd(w);

	ret = usbwarp_poll_start(w);
	if (ret) {
		dev_err(&pdev->dev, "usbwarp: poll thread failed: %d\n", ret);
		goto err_remove_hcd;
	}

	ret = usbwarp_send_negotiate(w);
	if (ret)
		dev_warn(&pdev->dev,
			 "usbwarp: negotiate send failed: %d\n", ret);

	dev_info(&pdev->dev, "usbwarp: probe complete — HCD registered\n");
	return 0;

err_remove_hcd:
	usb_remove_hcd(hcd);
err_buf:
	usbwarp_buf_cleanup(w);
err_put_hcd:
	usb_put_hcd(hcd);
err_unmap:
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
	struct usb_hcd *hcd = pci_get_drvdata(pdev);
	struct usbwarp_hcd *w;

	if (!hcd)
		return;

	w = hcd_to_warp(hcd);

	dev_info(&pdev->dev, "usbwarp: removing\n");

	w->shutting_down = true;

	usbwarp_debugfs_set_hcd(NULL);
	usbwarp_poll_stop(w);
	usb_remove_hcd(hcd);
	usbwarp_buf_cleanup(w);

	if (w->bar0)
		pci_iounmap(pdev, w->bar0);

	usb_put_hcd(hcd);

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
MODULE_VERSION("0.1");
