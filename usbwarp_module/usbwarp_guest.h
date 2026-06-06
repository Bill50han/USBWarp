/* SPDX-License-Identifier: GPL-2.0 */
/*
 * usbwarp_guest.h — UsbWarp Linux Virtual HCD internal definitions.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Private to the usbwarp.ko kernel module.  Not exported.
 */

#ifndef USBWARP_GUEST_H
#define USBWARP_GUEST_H

#include <linux/pci.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/kthread.h>
#include <linux/timer.h>
#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>

#include "../include/usbwarp_shared.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Module parameters (extern declarations; defined in usbwarp_pci.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

extern bool     usbwarp_enable_iso;
extern unsigned usbwarp_max_pending;
extern unsigned usbwarp_poll_us;
extern bool     usbwarp_debug;

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  URB private context
 * ═══════════════════════════════════════════════════════════════════════════ */

enum usbwarp_urb_state {
	URB_STATE_ACTIVE    = 0,
	URB_STATE_CANCELING = 1,
	URB_STATE_DEAD      = 2,
};

struct usbwarp_urb_priv {
	struct urb          *urb;
	uint32_t             transaction_id;
	uint32_t             device_id;       /* Host-assigned warp device_id  */
	int                  buffer_index;    /* Data Region slot, -1 = none   */

	atomic_t             state;           /* enum usbwarp_urb_state        */

	struct hlist_node    hash_node;       /* pending URB hash table        */
	struct list_head     cancel_node;     /* temp list for cancel/giveback */
	struct timer_list    timeout_timer;

	ktime_t              submit_time;
};

/* Hash table order: 2^8 = 256 buckets — covers max 1024 pending URBs. */
#define USBWARP_URB_HASH_BITS  8

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Per-device context
 *
 *   Each device has a Host-assigned device_id (1-based, from
 *   MSG_DEVICE_ADDED) and a Linux-assigned USB address (from USB Core
 *   enumeration).  The usb_addr is set during hub_control SetPortFeature
 *   RESET when USB Core assigns an address.
 *
 *   Mapping:  usb_addr → device_id  is needed in urb_enqueue.
 *             device_id → port      is (device_id - 1).
 * ═══════════════════════════════════════════════════════════════════════════ */

struct usbwarp_device {
	uint32_t             device_id;       /* Host-assigned, 1-based        */
	bool                 connected;
	uint16_t             vendor_id;
	uint16_t             product_id;
	uint8_t              speed;           /* enum usbwarp_usb_speed        */

	/* Linux USB address mapping (set during enumeration) */
	int                  usb_addr;        /* Linux-assigned, 0 = unset     */

	/* Per-device resource quota */
	atomic_t             pending_urbs;
	atomic_t             buf_in_use;      /* buffers held by this device   */

	/* Statistics */
	uint64_t             total_urbs;
	uint64_t             total_bytes;
};

/* Per-device buffer quota: max 25% of total buffers. */
#define USBWARP_DEVICE_BUF_QUOTA_PCT  25

/* Linux USB Core limits root hubs to USB_MAXCHILDREN = 31 ports.
 * USBWARP_MAX_DEVICES_LIMIT is 32, but we must report ≤ 31 ports
 * in the hub descriptor or USB Core rejects the hub entirely.    */
#define USBWARP_MAX_PORTS  31

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Ring wrapper (ioremap pointers)
 *
 *   NOTE: Linux kernel convention: use __iomem annotation, NOT volatile.
 *   ioread32/iowrite32 and memcpy_fromio/memcpy_toio provide the correct
 *   access semantics.  Adding volatile causes -Wdiscarded-qualifiers.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct usbwarp_ring {
	struct usbwarp_ring_header __iomem  *hdr;
	void __iomem                        *data;
	uint32_t                             data_size;
	uint32_t                             data_mask;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Adaptive polling state
 * ═══════════════════════════════════════════════════════════════════════════ */

enum usbwarp_poll_state {
	POLL_BUSY   = 0,
	POLL_ACTIVE = 1,
	POLL_LIGHT  = 2,
	POLL_IDLE   = 3,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Main HCD context (embedded in struct usb_hcd via hcd_priv)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct usbwarp_hcd {
	/* ── PCI / MMIO ─────────────────────────────────────────────────────── */
	struct pci_dev                      *pdev;
	void __iomem                        *bar0;
	uint32_t                             bar0_size;

	/* ── Typed pointers into BAR0 (no volatile — __iomem only) ──────────
	 *
	 * volatile is intentionally omitted.  All MMIO accesses go through
	 * ioread32/iowrite32/memcpy_fromio/memcpy_toio which provide the
	 * correct compiler barriers and ordering semantics.
	 */
	struct usbwarp_control_block __iomem *cb;
	struct usbwarp_ring                  g2h;      /* Guest→Host ring */
	struct usbwarp_ring                  h2g;      /* Host→Guest ring */
	void __iomem                        *data_region;
	uint32_t                             data_region_size;
	uint32_t                             buffer_size;
	uint32_t                             buffer_count;
	uint32_t                             data_region_offset; /* cached from CB */

	/* ── Data Region buffer bitmap ──────────────────────────────────────── */
	unsigned long                       *buf_bitmap;    /* 1=free, 0=used */
	atomic_t                             buf_free_count;
	spinlock_t                           buf_lock;      /* protect bitmap */

	/* ── Negotiated parameters ──────────────────────────────────────────── */
	uint16_t                             negotiated_version;
	uint32_t                             negotiated_caps;
	uint32_t                             max_pending_urbs;
	bool                                 negotiation_done;

	/* ── Devices ────────────────────────────────────────────────────────── */
	struct usbwarp_device                devices[USBWARP_MAX_PORTS];
	uint32_t                             device_count;
	spinlock_t                           dev_lock;

	/* Root hub port status (1 port per possible device) */
	uint32_t                             port_status[USBWARP_MAX_PORTS];
	unsigned long                        port_change;  /* bitmask */

	/* ── Pending URB tracking ───────────────────────────────────────────── */
	DECLARE_HASHTABLE(urb_table, USBWARP_URB_HASH_BITS);
	spinlock_t                           urb_lock;
	atomic_t                             txn_counter;   /* next transaction_id */
	atomic_t                             total_pending;

	/* ── Polling thread ─────────────────────────────────────────────────── */
	struct task_struct                  *poll_thread;
	enum usbwarp_poll_state              poll_state;
	unsigned int                         idle_count;

	/* ── Lifecycle flags ────────────────────────────────────────────────── */
	bool                                 shutting_down;
};

/* Retrieve usbwarp_hcd from a struct usb_hcd pointer. */
static inline struct usbwarp_hcd *hcd_to_warp(struct usb_hcd *hcd)
{
	return (struct usbwarp_hcd *)hcd->hcd_priv;
}

static inline struct usb_hcd *warp_to_hcd(struct usbwarp_hcd *w)
{
	return container_of((void *)w, struct usb_hcd, hcd_priv);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Device ID mapping helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Look up warp device_id by Linux USB device address.  Returns 0 if none. */
static inline uint32_t usbwarp_find_devid_by_addr(struct usbwarp_hcd *w,
						   int usb_addr)
{
	unsigned int i;

	for (i = 0; i < USBWARP_MAX_PORTS; i++) {
		if (w->devices[i].connected &&
		    w->devices[i].usb_addr == usb_addr)
			return w->devices[i].device_id;
	}
	return 0;  /* not found */
}

/* Look up warp device_id by port number (1-based).  Used for address 0
 * during USB enumeration before SET_ADDRESS completes.  Returns 0 if none. */
static inline uint32_t usbwarp_find_devid_by_port(struct usbwarp_hcd *w,
						    unsigned int portnum)
{
	if (portnum >= 1 && portnum <= USBWARP_MAX_PORTS) {
		unsigned int idx = portnum - 1;
		if (w->devices[idx].connected)
			return w->devices[idx].device_id;
	}
	return 0;
}

/* Look up warp device by device_id.  Returns NULL if invalid or not connected. */
static inline struct usbwarp_device *usbwarp_get_device(struct usbwarp_hcd *w,
							uint32_t device_id)
{
	if (device_id == 0 || device_id > USBWARP_MAX_PORTS)
		return NULL;
	if (!w->devices[device_id - 1].connected)
		return NULL;
	return &w->devices[device_id - 1];
}

/* Per-device buffer quota check. */
static inline bool usbwarp_device_quota_allows(struct usbwarp_hcd *w,
					       uint32_t device_id)
{
	struct usbwarp_device *dev = usbwarp_get_device(w, device_id);
	uint32_t max_per_dev;

	if (!dev)
		return false;
	max_per_dev = w->buffer_count * USBWARP_DEVICE_BUF_QUOTA_PCT / 100;
	if (max_per_dev == 0)
		max_per_dev = 1;
	return atomic_read(&dev->buf_in_use) < (int)max_per_dev;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Ring operations  (usbwarp_ring.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

void usbwarp_ring_init(struct usbwarp_ring *r,
		       struct usbwarp_ring_header __iomem *hdr,
		       void __iomem *data);

int  usbwarp_ring_produce(struct usbwarp_ring *r,
			  const void *msg, uint32_t len);

int  usbwarp_ring_consume(struct usbwarp_ring *r,
			  void *msg_out, uint32_t buf_size,
			  uint32_t *msg_len_out);

unsigned usbwarp_ring_usage_percent(const struct usbwarp_ring *r);

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Buffer management  (usbwarp_buf.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

int  usbwarp_buf_init(struct usbwarp_hcd *w);
void usbwarp_buf_cleanup(struct usbwarp_hcd *w);

int  usbwarp_buf_alloc(struct usbwarp_hcd *w);
void usbwarp_buf_free(struct usbwarp_hcd *w, int idx);

static inline void __iomem *usbwarp_buf_addr(struct usbwarp_hcd *w, int idx)
{
	return w->data_region + (uint32_t)idx * w->buffer_size;
}

static inline uint32_t usbwarp_buf_offset(struct usbwarp_hcd *w, int idx)
{
	return w->data_region_offset + (uint32_t)idx * w->buffer_size;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  HCD operations  (usbwarp_hcd.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

extern const struct hc_driver usbwarp_hc_driver;

/* Cancel all pending URBs for a specific device. */
void usbwarp_cancel_device_urbs(struct usbwarp_hcd *w, uint32_t device_id);

/* Assign Linux USB address to a warp device (called from hub_control RESET). */
void usbwarp_set_usb_addr(struct usbwarp_hcd *w, unsigned int port,
			   int usb_addr);

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Polling / message dispatch  (usbwarp_poll.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

int  usbwarp_poll_start(struct usbwarp_hcd *w);
void usbwarp_poll_stop(struct usbwarp_hcd *w);

int  usbwarp_send_negotiate(struct usbwarp_hcd *w);
void usbwarp_send_heartbeat(struct usbwarp_hcd *w);

void usbwarp_complete_urb(struct usbwarp_hcd *w,
			  const struct usbwarp_msg_urb_complete *msg);

void usbwarp_handle_device_added(struct usbwarp_hcd *w,
				 const struct usbwarp_msg_device_added *msg);
void usbwarp_handle_device_removed(struct usbwarp_hcd *w,
				   const struct usbwarp_msg_device_removed *msg);

/* ═══════════════════════════════════════════════════════════════════════════
 * §14  Debugfs fuzz support (gated by module param debug=1)
 *
 *   Always compiled in.  debugfs files only created when debug=1.
 *   g2h_paused() returns false when debug=0 (zero overhead).
 * ═══════════════════════════════════════════════════════════════════════════ */

int  usbwarp_debugfs_init(void);
void usbwarp_debugfs_exit(void);
void usbwarp_debugfs_set_hcd(struct usbwarp_hcd *w);
bool usbwarp_debugfs_g2h_paused(void);

#endif /* USBWARP_GUEST_H */
