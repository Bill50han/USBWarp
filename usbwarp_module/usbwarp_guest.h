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
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/hrtimer.h>
#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include "../include/usbwarp_usb3.h"

#include "../include/usbwarp_iso.h"

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
	u32                  epoch;
	int                  buffer_index;    /* base slot, -1 = none          */
	uint16_t             slot_count;      /* contiguous slots from base    */
	uint16_t             number_of_packets; /* ISO only, 0 = not ISO       */
	bool                 queued;
	bool                 need_zlp;
	bool                 zlp_stage;
	u32                  saved_actual;
	struct usbwarp_msg_urb_submit wire_submit;

	int                  state;           /* protected by urb_lock        */
	int                  cancel_status;
	unsigned long        cancel_retry;

	struct hlist_node    hash_node;       /* pending URB hash table        */
	struct list_head     cancel_node;     /* temp list for cancel/giveback */

    u64                  published_ns;
    bool                 busy_eligible;
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
 *   Device IDs are independent of USB addresses and root ports. Each slot
 *   records its owning HCD/port and USB-core device identity.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct usbwarp_device {
	uint32_t             device_id;       /* Host-assigned, 1-based        */
	bool                 connected;
	u32                  generation;      /* reject submit across rebind   */
	u32                  epoch;           /* host binding epoch on the wire */
	struct usb_hcd      *hcd;
	u8                   port;            /* 1-based on the owning root hub */
	struct usb_device   *udev;             /* identity only, owned by USB core */
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

/* Root capacities: USB2 31 ports, USB3 15 ports. Global device IDs use
 * their independent 32-entry table and are mapped onto free root ports. */
#define USBWARP_MAX_PORTS USBWARP_USB2_PORTS

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
    void __iomem                        *doorbell;
	uint32_t                             data_size;
	uint32_t                             data_mask;

	/* G2H ring only: serialize producers.  The consumer side stays
	 * lock-free (single consumer = poll thread).  Producers span task/softirq
	 * context and the poll thread itself,
	 * so the read-modify-write of producer_index must be guarded.  irqsave
	 * is mandatory: USB completion softirqs can submit.  The critical section is
	 * the memcpy_toio — UC-MMIO writes dominate it by ~100×, so the lock
	 * is not the cost of a produce. */
	spinlock_t                           produce_lock;
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
 * §6  Shared transport context (one per PCI function, used by two HCDs)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct usbwarp_roothub {
    struct usbwarp_hcd *warp;
    u32 port_status[USBWARP_MAX_PORTS];
    u32 device_id[USBWARP_MAX_PORTS];
    unsigned int num_ports;
};

struct usbwarp_hcd {
	struct usb_hcd *hcd[2]; /* USB2 primary, USB3.2 shared */
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
	struct usbwarp_device                devices[USBWARP_MAX_DEVICES_LIMIT];
	uint32_t                             device_count;
	spinlock_t                           dev_lock;

	/* ── Pending URB tracking ───────────────────────────────────────────── */
	DECLARE_HASHTABLE(urb_table, USBWARP_URB_HASH_BITS);
	spinlock_t                           urb_lock;
	atomic_t                             txn_counter;   /* next transaction_id */
	atomic_t                             total_pending;
	atomic_t                             tx_pending;

	/* ── Polling thread ─────────────────────────────────────────────────── */
	struct task_struct                  *poll_thread;
    void                                *poll_buffer;
    void                                *rx_buffer;
    spinlock_t                          rx_lock; /* sole H2G consumer, BH-safe */
    struct tasklet_struct               rx_tasklet;
    struct hrtimer                      rx_repoll_timer;
    atomic_t                            rx_repoll_pending;
    bool                                rx_repoll_active;
    u32                                 rx_repoll_left;
    u64                                 rx_repoll_until, rx_repoll_due, rx_repoll_started;
    u64                                 rx_repoll_arms, rx_repoll_fires, rx_repoll_hits, rx_repoll_empty;
    u64                                 rx_repoll_late_ns, rx_repoll_late_max, rx_repoll_delay_ns;
    atomic64_t                          rx_busy_until, rx_busy_kicks;
    atomic64_t                          rx_busy_submits, rx_busy_fast, rx_busy_slow;
    bool                                rx_busy_active;
    u64                                 rx_busy_windows, rx_busy_ns, rx_busy_polls;
    u64                                 rx_busy_completions, rx_busy_yields;
    bool                                rx_fast, rx_stopping, rx_control_pending;
    u32                                 rx_control_length;
    int                                 rx_error;
    u64                                 rx_batches, rx_completions, rx_deferrals, rx_reschedules;
    wait_queue_head_t                   irq_wait;
    atomic_t                            irq_pending;
    atomic64_t                          irq_count;
    u64                                 irq_last_ns;
    int                                 irq_vector;
    cpumask_t                           irq_original_cpus;
    bool                                irq_affinity_saved, irq_affinity_applied;
    int                                 irq_affinity_cpu;
    bool                                irq_requested, marker_owned, irq_failed;
    u32                                 irq_request;
    u64                                 irq_token, marker_original, marker_last;
    struct delayed_work                 marker_watchdog;
    void __iomem                        *doorbell;
    struct work_struct                  retire_work;
    bool                                hcds_removed;
    bool                                removing;
	enum usbwarp_poll_state              poll_state;
	unsigned int                         idle_count;

	/* ── ISO pacing ─────────────────────────────────────────────────────── *
	 * While any ISO URB is in flight the poll thread must not degrade to
	 * POLL_IDLE (1-2 ms sleep would starve stream completion). */
	atomic_t                             iso_pending;

	/* ── Lifecycle flags ────────────────────────────────────────────────── */
	bool                                 shutting_down;
	bool                                 quiescing;
	struct completion                    shutdown_ack;
    struct mutex config_lock;
    struct completion config_done;
    u32 config_txn;
    u32 config_device;
    u32 config_epoch;
    int config_status;
    u32 config_streams;
};

static inline struct usbwarp_roothub *hcd_to_rh(struct usb_hcd *hcd)
{
    return (struct usbwarp_roothub *)hcd->hcd_priv;
}
static inline struct usbwarp_hcd *hcd_to_warp(struct usb_hcd *hcd)
{
    return hcd_to_rh(hcd)->warp;
}
static inline bool usbwarp_is_usb3(struct usb_hcd *hcd)
{
    return !usb_hcd_is_primary_hcd(hcd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Device ID mapping helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t usbwarp_find_devid_by_port(struct usb_hcd *hcd, unsigned int port)
{
    struct usbwarp_roothub *rh = hcd_to_rh(hcd);
    if (port < 1 || port > rh->num_ports)
        return 0;
    return rh->device_id[port - 1];
}

/* Look up warp device by device_id.  Returns NULL if invalid or not connected. */
static inline struct usbwarp_device *usbwarp_get_device(struct usbwarp_hcd *w,
							uint32_t device_id)
{
	if (device_id == 0 || device_id > USBWARP_MAX_DEVICES_LIMIT)
		return NULL;
	if (!w->devices[device_id - 1].connected)
		return NULL;
	return &w->devices[device_id - 1];
}

/* Per-device buffer quota check (slot units — a multi-slot transfer
 * counts every slot it spans). */
static inline bool usbwarp_device_quota_allows(struct usbwarp_hcd *w,
					       uint32_t device_id,
					       uint32_t extra)
{
	struct usbwarp_device *dev = usbwarp_get_device(w, device_id);
	uint32_t max_per_dev;

	if (!dev)
		return false;
	max_per_dev = w->buffer_count * USBWARP_DEVICE_BUF_QUOTA_PCT / 100;
	if (max_per_dev == 0)
		max_per_dev = 1;
	return (uint32_t)atomic_read(&dev->buf_in_use) + extra <= max_per_dev;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Ring operations  (usbwarp_ring.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

void usbwarp_ring_init(struct usbwarp_ring *r,
		       struct usbwarp_ring_header __iomem *hdr,
		       void __iomem *data, uint32_t data_size);

int  usbwarp_ring_produce(struct usbwarp_ring *r,
			  const void *msg, uint32_t len);
int usbwarp_ring_inject_raw(struct usbwarp_ring *r, const void *msg, uint32_t len);

int  usbwarp_ring_consume(struct usbwarp_ring *r,
			  void *msg_out, uint32_t buf_size,
			  uint32_t *msg_len_out);

unsigned usbwarp_ring_usage_percent(const struct usbwarp_ring *r);

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Buffer management  (usbwarp_buf.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

int  usbwarp_buf_init(struct usbwarp_hcd *w);
void usbwarp_buf_cleanup(struct usbwarp_hcd *w);

int  usbwarp_buf_alloc_contig(struct usbwarp_hcd *w, uint32_t count);
void usbwarp_buf_free_range(struct usbwarp_hcd *w, int base, uint32_t count);

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
int usbwarp_hub_status_data(struct usb_hcd *hcd, char *buf);
int usbwarp_hub_control(struct usb_hcd *hcd, u16 request, u16 value,
                       u16 index, char *buf, u16 length);

/* Cancel all pending URBs for a specific device. */
void usbwarp_cancel_device_urbs(struct usbwarp_hcd *w, uint32_t device_id);
void usbwarp_shutdown_all_devices(struct usbwarp_hcd *w, uint32_t reason);
void usbwarp_retire_hcd(struct usbwarp_hcd *w);
void usbwarp_retry_cancels(struct usbwarp_hcd *w);
void usbwarp_send_cancel_locked(struct usbwarp_hcd *w, struct usbwarp_urb_priv *p);
void usbwarp_flush_queued(struct usbwarp_hcd *w);
bool usbwarp_submission_blocked(struct usbwarp_urb_priv *p);
void usbwarp_publish_queued(struct usbwarp_hcd *w, struct usbwarp_urb_priv *p);
bool usbwarp_stage_zlp(struct usbwarp_hcd *w, const struct usbwarp_msg_urb_complete *msg);
size_t usbwarp_copy_urb_data(struct urb *urb, u32 offset, void *memory,
                           void __iomem *mmio, u32 length, bool to_urb);

void usbwarp_complete_streams(struct usbwarp_hcd *w,
    const struct usbwarp_msg_streams_response *msg);

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Polling / message dispatch  (usbwarp_poll.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

int  usbwarp_poll_start(struct usbwarp_hcd *w);
void usbwarp_poll_stop(struct usbwarp_hcd *w);

int  usbwarp_send_negotiate(struct usbwarp_hcd *w);
void usbwarp_irq_init(struct usbwarp_hcd *w);
void usbwarp_irq_cleanup(struct usbwarp_hcd *w);
void usbwarp_irq_step(struct usbwarp_hcd *w);
void usbwarp_irq_disarm(struct usbwarp_hcd *w);
bool usbwarp_irq_wait(struct usbwarp_hcd *w);
void usbwarp_irq_restore(struct usbwarp_hcd *w);
bool usbwarp_irq_ready(struct usbwarp_hcd *w);
bool usbwarp_irq_arm(struct usbwarp_hcd *w, bool test, u64 *sequence);
void usbwarp_rx_init(struct usbwarp_hcd *w);
void usbwarp_rx_stop(struct usbwarp_hcd *w);
bool usbwarp_rx_interrupt(struct usbwarp_hcd *w);
void usbwarp_rx_watchdog(struct usbwarp_hcd *w);
bool usbwarp_rx_priority_pending(struct usbwarp_hcd *w);
bool usbwarp_rx_worker(struct usbwarp_hcd *w);
bool usbwarp_rx_submit(struct usbwarp_hcd *w, u32 type, u32 length);
void usbwarp_rx_progress(struct usbwarp_hcd *w, bool eligible, u64 published_ns, int status);
bool usbwarp_rx_busy_pending(struct usbwarp_hcd *w);
bool usbwarp_rx_busy_poll(struct usbwarp_hcd *w);
bool usbwarp_validate_h2g_message(struct usbwarp_hcd *w, const void *message, u32 length);
void usbwarp_dispatch_message(struct usbwarp_hcd *w, const void *message, u32 length);
u64 usbwarp_irq_os_id(void);
void usbwarp_send_heartbeat(struct usbwarp_hcd *w);
void usbwarp_guest_disconnect(struct usbwarp_hcd *w);

void usbwarp_complete_urb(struct usbwarp_hcd *w,
			  const struct usbwarp_msg_urb_complete *msg);
void usbwarp_complete_iso_urb(struct usbwarp_hcd *w,
			      const struct usbwarp_msg_iso_complete *msg,
			      uint32_t msg_len);

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
void usbwarp_debugfs_clear_hcd(struct usbwarp_hcd *w);
bool usbwarp_debugfs_g2h_paused(void);

#endif /* USBWARP_GUEST_H */
