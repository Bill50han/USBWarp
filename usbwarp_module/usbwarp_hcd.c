// SPDX-License-Identifier: GPL-2.0
/*
 * usbwarp_hcd.c — Virtual USB Host Controller implementation.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Key design points:
 *   - device_id mapping: Host assigns device_id via MSG_DEVICE_ADDED.
 *     Linux USB Core assigns usb_addr via SET_ADDRESS during enumeration.
 *     We maintain a mapping table in usbwarp_device[].usb_addr so that
 *     urb_enqueue can look up the correct device_id.
 *
 *   - Root hub: USB Core expects bit 0 = hub change, bit N = port N
 *     (1-based).  Port index in our arrays is 0-based.
 */

#include "usbwarp_guest.h"
#include <linux/usb/ch9.h>
#include <linux/usb/ch11.h>
#include <linux/slab.h>
#include <linux/io.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t next_txn_id(struct usbwarp_hcd *w)
{
	uint32_t id;

	do {
		id = (uint32_t)atomic_inc_return(&w->txn_counter);
	} while (id == USBWARP_TXN_ID_NONE);

	return id;
}

static void fill_msg_header(struct usbwarp_msg_header *h,
			    uint16_t type, uint32_t dev_id,
			    uint32_t txn_id, uint32_t total_len)
{
	h->magic            = USBWARP_MSG_MAGIC;
	h->message_type     = type;
	h->protocol_version = USBWARP_PROTOCOL_VERSION;
	h->message_length   = total_len;
	h->transaction_id   = txn_id;
	h->device_id        = dev_id;
	h->flags            = 0;
	h->timestamp        = (uint64_t)ktime_get_ns();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  URB pending table
 * ═══════════════════════════════════════════════════════════════════════════ */

static void urb_table_add(struct usbwarp_hcd *w, struct usbwarp_urb_priv *priv)
{
	unsigned long flags;

	spin_lock_irqsave(&w->urb_lock, flags);
	hash_add(w->urb_table, &priv->hash_node, priv->transaction_id);
	atomic_inc(&w->total_pending);
	spin_unlock_irqrestore(&w->urb_lock, flags);
}

static struct usbwarp_urb_priv *urb_table_find(struct usbwarp_hcd *w,
					       uint32_t txn_id)
{
	struct usbwarp_urb_priv *priv;
	unsigned long flags;

	spin_lock_irqsave(&w->urb_lock, flags);
	hash_for_each_possible(w->urb_table, priv, hash_node, txn_id) {
		if (priv->transaction_id == txn_id) {
			spin_unlock_irqrestore(&w->urb_lock, flags);
			return priv;
		}
	}
	spin_unlock_irqrestore(&w->urb_lock, flags);
	return NULL;
}

static void urb_table_remove(struct usbwarp_hcd *w,
			     struct usbwarp_urb_priv *priv)
{
	unsigned long flags;

	spin_lock_irqsave(&w->urb_lock, flags);
	hash_del(&priv->hash_node);
	atomic_dec(&w->total_pending);
	spin_unlock_irqrestore(&w->urb_lock, flags);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  URB timeout callback
 *
 *   CAS ACTIVE→CANCELING.  Send MSG_URB_CANCEL — if ring is full, log
 *   the failure but don't leak: the URB will eventually be giveback'd
 *   by the shutdown path or a subsequent completion.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void urb_timeout_fn(struct timer_list *t)
{
	struct usbwarp_urb_priv *priv =
		from_timer(priv, t, timeout_timer);
	struct urb *urb = priv->urb;
	struct usb_hcd *hcd = bus_to_hcd(urb->dev->bus);
	struct usbwarp_hcd *w = hcd_to_warp(hcd);
	struct usbwarp_msg_urb_cancel cancel_msg;
	int ret;

	if (atomic_cmpxchg(&priv->state,
			   URB_STATE_ACTIVE,
			   URB_STATE_CANCELING) != URB_STATE_ACTIVE)
		return;

	dev_dbg(&w->pdev->dev,
		"usbwarp: URB timeout txn=%u dev=%u\n",
		priv->transaction_id, priv->device_id);

	memset(&cancel_msg, 0, sizeof(cancel_msg));
	fill_msg_header(&cancel_msg.hdr,
			USBWARP_MSG_URB_CANCEL,
			priv->device_id,
			priv->transaction_id,
			sizeof(cancel_msg));
	cancel_msg.device_id = priv->device_id;

	ret = usbwarp_ring_produce(&w->g2h, &cancel_msg, sizeof(cancel_msg));
	if (ret) {
		dev_warn(&w->pdev->dev,
			 "usbwarp: cancel send failed (ring full) txn=%u\n",
			 priv->transaction_id);
		/* URB remains CANCELING.  It will be cleaned up by:
		 * (a) a later Host completion arriving, or
		 * (b) the device removal / shutdown path.          */
	}
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  urb_enqueue
 *
 *   FIX: Use usbwarp_find_devid_by_addr() to map Linux USB address
 *   to Host-assigned device_id.  The old code used usb_pipedevice()
 *   directly, which is wrong because the Host assigns independent IDs.
 *
 *   FIX: Per-device buffer quota check before alloc.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int usbwarp_urb_enqueue(struct usb_hcd *hcd, struct urb *urb,
			       gfp_t mem_flags)
{
	struct usbwarp_hcd *w = hcd_to_warp(hcd);
	struct usbwarp_urb_priv *priv;
	struct usbwarp_msg_urb_submit submit;
	struct usbwarp_device *wdev;
	int buf_idx = -1;
	uint32_t dev_id;
	uint32_t xfer_len;
	bool is_out, use_inline;
	int ret;

	if (w->shutting_down)
		return -ESHUTDOWN;

	/* Map Linux USB device address → Host-assigned device_id.
	 *
	 * Three cases:
	 *
	 * 1) Address 0: default USB address during enumeration before
	 *    SET_ADDRESS.  Look up by port number.
	 *
	 * 2) Non-zero address, found in map: normal fast path.
	 *
	 * 3) Non-zero address, NOT in map: this happens because Linux USB
	 *    core's hub_set_address() calls our address_device() callback
	 *    BEFORE update_devnum() sets udev->devnum.  So address_device
	 *    sees devnum=0 and can't store the real address.  We handle
	 *    this with lazy mapping: look up by port, then update the
	 *    address map for future fast-path hits.
	 */
	if (usb_pipedevice(urb->pipe) == 0) {
		/* Case 1: address 0 → find by port. */
		if (!urb->dev || !urb->dev->parent)
			return -EINVAL;
		dev_id = usbwarp_find_devid_by_port(w, urb->dev->portnum);
	} else {
		/* Case 2: try address lookup first. */
		dev_id = usbwarp_find_devid_by_addr(w, usb_pipedevice(urb->pipe));

		/* Case 3: lazy address mapping fallback. */
		if (dev_id == 0 && urb->dev && urb->dev->parent) {
			dev_id = usbwarp_find_devid_by_port(w, urb->dev->portnum);
			if (dev_id != 0) {
				/* Store the mapping so future lookups are fast.
				 * set_usb_addr takes 0-based port index. */
				usbwarp_set_usb_addr(w, urb->dev->portnum - 1,
						     usb_pipedevice(urb->pipe));
				dev_dbg(&w->pdev->dev,
					"usbwarp: lazy addr map port=%u addr=%u → devid=%u\n",
					urb->dev->portnum,
					usb_pipedevice(urb->pipe), dev_id);
			}
		}
	}

	if (dev_id == 0)
		return -ENODEV;

	wdev = usbwarp_get_device(w, dev_id);
	if (!wdev)
		return -ENODEV;

	/* Per-device pending URB limit. */
	if (atomic_read(&wdev->pending_urbs) >= (int)w->max_pending_urbs)
		return -ENOSPC;

	xfer_len = urb->transfer_buffer_length;
	is_out   = usb_pipeout(urb->pipe);

	use_inline = (xfer_len > 0 && xfer_len <= USBWARP_INLINE_DATA_SIZE &&
		      (w->negotiated_caps & USBWARP_CAP_INLINE_DATA));

	/* Allocate Data Region buffer for non-inline transfers. */
	if (xfer_len > 0 && !use_inline) {
		/* Per-device quota check. */
		if (!usbwarp_device_quota_allows(w, dev_id))
			return -ENOSPC;

		buf_idx = usbwarp_buf_alloc(w);
		if (buf_idx < 0)
			return -ENOMEM;

		atomic_inc(&wdev->buf_in_use);

		if (is_out && urb->transfer_buffer) {
			memcpy_toio(usbwarp_buf_addr(w, buf_idx),
				    urb->transfer_buffer, xfer_len);
			smp_wmb();
		}
	}

	/* Allocate URB private context. */
	priv = kzalloc(sizeof(*priv), mem_flags);
	if (!priv) {
		if (buf_idx >= 0) {
			usbwarp_buf_free(w, buf_idx);
			atomic_dec(&wdev->buf_in_use);
		}
		return -ENOMEM;
	}

	priv->urb            = urb;
	priv->transaction_id = next_txn_id(w);
	priv->device_id      = dev_id;
	priv->buffer_index   = buf_idx;
	priv->submit_time    = ktime_get();
	atomic_set(&priv->state, URB_STATE_ACTIVE);
	urb->hcpriv = priv;

	/* Add to pending table BEFORE ring produce (two-phase commit). */
	urb_table_add(w, priv);
	atomic_inc(&wdev->pending_urbs);

	/* Build MSG_URB_SUBMIT. */
	memset(&submit, 0, sizeof(submit));

	{
		uint32_t msg_len;

		if (use_inline && is_out)
			msg_len = USBWARP_MSG_URB_SUBMIT_BASE_SIZE + xfer_len;
		else
			msg_len = USBWARP_MSG_URB_SUBMIT_BASE_SIZE;

		fill_msg_header(&submit.hdr, USBWARP_MSG_URB_SUBMIT,
				dev_id, priv->transaction_id, msg_len);
	}

	submit.device_id       = dev_id;
	submit.endpoint        = usb_pipeendpoint(urb->pipe);
	submit.direction       = is_out ? 0 : 1;
	submit.transfer_flags  = urb->transfer_flags;
	submit.transfer_length = xfer_len;

	if (usb_pipecontrol(urb->pipe)) {
		submit.transfer_type = USBWARP_XFER_CONTROL;
		if (urb->setup_packet)
			memcpy(submit.setup_packet, urb->setup_packet, 8);
	} else if (usb_pipebulk(urb->pipe)) {
		submit.transfer_type = USBWARP_XFER_BULK;
	} else if (usb_pipeint(urb->pipe)) {
		submit.transfer_type = USBWARP_XFER_INTERRUPT;
		submit.interval      = urb->interval;
	}

	if (xfer_len == 0) {
		submit.data_mode = USBWARP_DATA_NONE;
	} else if (use_inline) {
		submit.data_mode = USBWARP_DATA_INLINE;
		if (is_out && urb->transfer_buffer)
			memcpy(submit.inline_data, urb->transfer_buffer,
			       xfer_len);
	} else {
		submit.data_mode     = USBWARP_DATA_BUFFER;
		submit.buffer_offset = usbwarp_buf_offset(w, buf_idx);
	}

	/* Reject URB submit while G2H is paused for fuzz injection.
	 * Preserves SPSC invariant — fuzz injector is sole producer. */
	if (usbwarp_debugfs_g2h_paused()) {
		atomic_dec(&wdev->pending_urbs);
		urb_table_remove(w, priv);
		urb->hcpriv = NULL;
		kfree(priv);
		if (buf_idx >= 0) {
			usbwarp_buf_free(w, buf_idx);
			atomic_dec(&wdev->buf_in_use);
		}
		return -EBUSY;
	}

	ret = usbwarp_ring_produce(&w->g2h, &submit,
				   submit.hdr.message_length);
	if (ret) {
		atomic_dec(&wdev->pending_urbs);
		urb_table_remove(w, priv);
		urb->hcpriv = NULL;
		kfree(priv);
		if (buf_idx >= 0) {
			usbwarp_buf_free(w, buf_idx);
			atomic_dec(&wdev->buf_in_use);
		}
		return ret;
	}

	/* Start timeout timer. */
	{
		unsigned long timeout_ms =
			usb_pipecontrol(urb->pipe) ? 10000 : 5000;
		timer_setup(&priv->timeout_timer, urb_timeout_fn, 0);
		mod_timer(&priv->timeout_timer,
			  jiffies + msecs_to_jiffies(timeout_ms));
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  urb_dequeue
 * ═══════════════════════════════════════════════════════════════════════════ */

static int usbwarp_urb_dequeue(struct usb_hcd *hcd, struct urb *urb,
			       int status)
{
	struct usbwarp_hcd *w = hcd_to_warp(hcd);
	struct usbwarp_urb_priv *priv = urb->hcpriv;
	struct usbwarp_msg_urb_cancel cancel_msg;

	if (!priv)
		return -EINVAL;

	if (atomic_cmpxchg(&priv->state,
			   URB_STATE_ACTIVE,
			   URB_STATE_CANCELING) != URB_STATE_ACTIVE)
		return 0;

	memset(&cancel_msg, 0, sizeof(cancel_msg));
	fill_msg_header(&cancel_msg.hdr,
			USBWARP_MSG_URB_CANCEL,
			priv->device_id,
			priv->transaction_id,
			sizeof(cancel_msg));
	cancel_msg.device_id = priv->device_id;

	/* Best-effort send. */
	usbwarp_ring_produce(&w->g2h, &cancel_msg, sizeof(cancel_msg));

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  URB completion  (called from poll thread on MSG_URB_COMPLETE)
 *
 *   FIX: Validate host message fields (device_id range, actual_length
 *   bounds, data_mode sanity) before trusting them.
 * ═══════════════════════════════════════════════════════════════════════════ */

void usbwarp_complete_urb(struct usbwarp_hcd *w,
			  const struct usbwarp_msg_urb_complete *msg)
{
	struct usbwarp_urb_priv *priv;
	struct usbwarp_device *wdev;
	struct urb *urb;
	int urb_status;
	int old_state;
	uint32_t actual_len;

	/* Validate transaction_id lookup. */
	priv = urb_table_find(w, msg->hdr.transaction_id);
	if (!priv) {
		dev_warn(&w->pdev->dev,
			 "usbwarp: complete for unknown txn %u\n",
			 msg->hdr.transaction_id);
		return;
	}

	/* Validate device_id matches what we expect. */
	if (msg->hdr.device_id != priv->device_id) {
		dev_warn(&w->pdev->dev,
			 "usbwarp: complete dev_id mismatch: msg=%u priv=%u\n",
			 msg->hdr.device_id, priv->device_id);
		return;
	}

	/* CAS → DEAD. */
	old_state = atomic_read(&priv->state);
	if (old_state == URB_STATE_DEAD)
		return;

	if (atomic_cmpxchg(&priv->state, old_state, URB_STATE_DEAD)
	    != old_state)
		return;

	del_timer_sync(&priv->timeout_timer);

	urb = priv->urb;

	/* Validate actual_length: must not exceed transfer_buffer_length. */
	actual_len = msg->actual_length;
	if (actual_len > urb->transfer_buffer_length)
		actual_len = urb->transfer_buffer_length;

	/* Map protocol status to Linux errno. */
	switch (msg->status) {
	case USBWARP_STATUS_SUCCESS:       urb_status = 0;            break;
	case USBWARP_STATUS_CANCELLED:     urb_status = -ECONNRESET;  break;
	case USBWARP_STATUS_STALL:         urb_status = -EPIPE;       break;
	case USBWARP_STATUS_TIMEOUT:       urb_status = -ETIMEDOUT;   break;
	case USBWARP_STATUS_SHORT_PACKET:  urb_status = -EREMOTEIO;   break;
	case USBWARP_STATUS_DISCONNECTED:  urb_status = -ESHUTDOWN;   break;
	case USBWARP_STATUS_OVERFLOW:      urb_status = -EOVERFLOW;   break;
	case USBWARP_STATUS_NO_RESOURCE:   urb_status = -ENOMEM;      break;
	case USBWARP_STATUS_SHUTDOWN:      urb_status = -ESHUTDOWN;   break;
	default:                           urb_status = -EPROTO;      break;
	}

	urb->actual_length = actual_len;
	urb->status        = urb_status;

	/* Copy IN data from shared memory to URB transfer_buffer. */
	if (urb_status == 0 && !usb_pipeout(urb->pipe) &&
	    actual_len > 0 && urb->transfer_buffer) {
		if (msg->data_mode == USBWARP_DATA_INLINE) {
			uint32_t copy_len = min_t(uint32_t,
						  actual_len,
						  USBWARP_INLINE_DATA_SIZE);
			memcpy(urb->transfer_buffer, msg->inline_data,
			       copy_len);
		} else if (msg->data_mode == USBWARP_DATA_BUFFER &&
			   priv->buffer_index >= 0) {
			uint32_t copy_len = min_t(uint32_t,
						  actual_len,
						  urb->transfer_buffer_length);
			memcpy_fromio(urb->transfer_buffer,
				      usbwarp_buf_addr(w, priv->buffer_index),
				      copy_len);
		}
	}

	/* Free Data Region buffer + update per-device counter. */
	if (priv->buffer_index >= 0) {
		usbwarp_buf_free(w, priv->buffer_index);
		wdev = usbwarp_get_device(w, priv->device_id);
		if (wdev)
			atomic_dec(&wdev->buf_in_use);
	}

	/* Update per-device pending count. */
	wdev = usbwarp_get_device(w, priv->device_id);
	if (wdev)
		atomic_dec(&wdev->pending_urbs);

	urb_table_remove(w, priv);

	usb_hcd_giveback_urb(warp_to_hcd(w), urb, urb_status);

	urb->hcpriv = NULL;
	kfree(priv);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Cancel all pending URBs for a device (issue #2 fix)
 *
 *   Called from usbwarp_handle_device_removed before clearing the device
 *   slot.  Walks the hash table, finds all URBs with matching device_id,
 *   marks them DEAD, and gives them back to USB Core with -ESHUTDOWN.
 * ═══════════════════════════════════════════════════════════════════════════ */

void usbwarp_cancel_device_urbs(struct usbwarp_hcd *w, uint32_t device_id)
{
	struct usbwarp_urb_priv *priv;
	struct hlist_node *tmp;
	unsigned long flags;
	int bkt;
	struct usbwarp_device *wdev;
	LIST_HEAD(giveback_list);

	/* Collect URBs to give back (can't call giveback under urb_lock). */
	spin_lock_irqsave(&w->urb_lock, flags);
	hash_for_each_safe(w->urb_table, bkt, tmp, priv, hash_node) {
		if (priv->device_id != device_id)
			continue;

		/* Force to DEAD regardless of current state. */
		atomic_set(&priv->state, URB_STATE_DEAD);
		hash_del(&priv->hash_node);
		atomic_dec(&w->total_pending);

		/* Move to local list via the dedicated cancel_node member. */
		list_add_tail(&priv->cancel_node, &giveback_list);
	}
	spin_unlock_irqrestore(&w->urb_lock, flags);

	/* Give back collected URBs outside the lock. */
	while (!list_empty(&giveback_list)) {
		priv = list_first_entry(&giveback_list,
					struct usbwarp_urb_priv, cancel_node);
		list_del(&priv->cancel_node);

		del_timer_sync(&priv->timeout_timer);

		if (priv->buffer_index >= 0) {
			usbwarp_buf_free(w, priv->buffer_index);
			wdev = usbwarp_get_device(w, device_id);
			if (wdev)
				atomic_dec(&wdev->buf_in_use);
		}

		wdev = usbwarp_get_device(w, device_id);
		if (wdev)
			atomic_dec(&wdev->pending_urbs);

		priv->urb->status = -ESHUTDOWN;
		priv->urb->actual_length = 0;
		usb_hcd_giveback_urb(warp_to_hcd(w), priv->urb, -ESHUTDOWN);
		priv->urb->hcpriv = NULL;
		kfree(priv);
	}
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Root hub emulation
 *
 *   FIX: hub_status_data bitmap: bit 0 = hub change (always 0 for us),
 *   bit N = port N (1-based).  So port index i maps to bit (i+1).
 *
 *   FIX: Set usb_addr on port reset — USB Core reads the device address
 *   from the port during enumeration after SET_ADDRESS succeeds.
 *   We record the mapping in the address_change callback.
 * ═══════════════════════════════════════════════════════════════════════════ */

static const u8 usbwarp_rh_desc[] = {
	0x09,                   /* bLength            */
	USB_DT_HUB,            /* bDescriptorType    */
	USBWARP_MAX_PORTS,     /* bNbrPorts          */
	0x00, 0x00,             /* wHubCharacteristics */
	0x01,                   /* bPwrOn2PwrGood     */
	0x00,                   /* bHubContrCurrent   */
	0x00,                   /* DeviceRemovable    */
	0xff,                   /* PortPwrCtrlMask    */
};

static int usbwarp_hub_status_data(struct usb_hcd *hcd, char *buf)
{
	struct usbwarp_hcd *w = hcd_to_warp(hcd);
	int i, changed = 0;
	/* Bitmap size: need (USBWARP_MAX_PORTS + 1) bits.
	 * bit 0 = hub status change (always 0).
	 * bit i+1 = port i change.                         */
	int nbytes = (USBWARP_MAX_PORTS + 1 + 7) / 8;

	memset(buf, 0, nbytes);

	for (i = 0; i < USBWARP_MAX_PORTS; i++) {
		if (test_and_clear_bit(i, &w->port_change)) {
			/* Port i change → bit (i+1) in the bitmap. */
			buf[(i + 1) / 8] |= 1 << ((i + 1) % 8);
			changed = 1;
		}
	}

	return changed ? nbytes : 0;
}

/* Called by USB Core to set the USB address on a device after SET_ADDRESS. */
void usbwarp_set_usb_addr(struct usbwarp_hcd *w, unsigned int port,
			   int usb_addr)
{
	unsigned long flags;

	if (port >= USBWARP_MAX_PORTS)
		return;

	spin_lock_irqsave(&w->dev_lock, flags);
	if (w->devices[port].connected)
		w->devices[port].usb_addr = usb_addr;
	spin_unlock_irqrestore(&w->dev_lock, flags);
}

static int usbwarp_hub_control(struct usb_hcd *hcd, u16 typeReq, u16 wValue,
			       u16 wIndex, char *buf, u16 wLength)
{
	struct usbwarp_hcd *w = hcd_to_warp(hcd);
	unsigned port = wIndex - 1;   /* wIndex is 1-based */

	switch (typeReq) {
	case GetHubDescriptor:
		if (wLength < sizeof(usbwarp_rh_desc))
			return -EINVAL;
		memcpy(buf, usbwarp_rh_desc, sizeof(usbwarp_rh_desc));
		return 0;

	case GetHubStatus:
		memset(buf, 0, 4);
		return 0;

	case GetPortStatus:
		if (port >= USBWARP_MAX_PORTS)
			return -EPIPE;
		((u32 *)buf)[0] = cpu_to_le32(w->port_status[port]);
		return 0;

	case SetPortFeature:
		if (port >= USBWARP_MAX_PORTS)
			return -EPIPE;
		switch (wValue) {
		case USB_PORT_FEAT_POWER:
			w->port_status[port] |= USB_PORT_STAT_POWER;
			return 0;
		case USB_PORT_FEAT_RESET:
			/* Simulate reset complete.  After reset, USB Core
			 * will assign a USB address via SET_ADDRESS.  We
			 * record the mapping later via address_change. */
			w->port_status[port] |= USB_PORT_STAT_RESET;
			w->port_status[port] &= ~USB_PORT_STAT_RESET;
			w->port_status[port] |= USB_PORT_STAT_ENABLE |
						 (USB_PORT_STAT_C_RESET << 16);
			set_bit(port, &w->port_change);
			return 0;
		default:
			return 0;
		}

	case ClearPortFeature:
		if (port >= USBWARP_MAX_PORTS)
			return -EPIPE;
		switch (wValue) {
		case USB_PORT_FEAT_C_CONNECTION:
			w->port_status[port] &=
				~(USB_PORT_STAT_C_CONNECTION << 16);
			return 0;
		case USB_PORT_FEAT_C_RESET:
			w->port_status[port] &=
				~(USB_PORT_STAT_C_RESET << 16);
			return 0;
		case USB_PORT_FEAT_C_ENABLE:
			w->port_status[port] &=
				~(USB_PORT_STAT_C_ENABLE << 16);
			return 0;
		default:
			return 0;
		}

	default:
		return -EPIPE;
	}
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Device add / remove handlers
 *
 *   FIX (issue #3): Validate incoming message fields.
 *   FIX (issue #2): Cancel all pending URBs before clearing device slot.
 * ═══════════════════════════════════════════════════════════════════════════ */

void usbwarp_handle_device_added(struct usbwarp_hcd *w,
				 const struct usbwarp_msg_device_added *msg)
{
	uint32_t id = msg->device_id;
	unsigned long flags;
	struct usbwarp_msg_device_ack ack;

	/* Validate device_id range. */
	if (id == 0 || id > USBWARP_MAX_PORTS) {
		dev_warn(&w->pdev->dev,
			 "usbwarp: device_added bad id %u\n", id);
		return;
	}

	/* Validate speed field. */
	if (msg->speed > USBWARP_SPEED_SUPER) {
		dev_warn(&w->pdev->dev,
			 "usbwarp: device_added bad speed %u\n", msg->speed);
		return;
	}

	spin_lock_irqsave(&w->dev_lock, flags);

	w->devices[id - 1].device_id  = id;
	w->devices[id - 1].connected  = true;
	w->devices[id - 1].vendor_id  = msg->vendor_id;
	w->devices[id - 1].product_id = msg->product_id;
	w->devices[id - 1].speed      = msg->speed;
	w->devices[id - 1].usb_addr   = 0;  /* set later by USB Core */
	atomic_set(&w->devices[id - 1].pending_urbs, 0);
	atomic_set(&w->devices[id - 1].buf_in_use, 0);
	w->device_count++;

	w->port_status[id - 1] = USB_PORT_STAT_CONNECTION |
				  USB_PORT_STAT_POWER |
				  (USB_PORT_STAT_C_CONNECTION << 16);

	switch (msg->speed) {
	case USBWARP_SPEED_LOW:
		w->port_status[id - 1] |= USB_PORT_STAT_LOW_SPEED;
		break;
	case USBWARP_SPEED_HIGH:
		w->port_status[id - 1] |= USB_PORT_STAT_HIGH_SPEED;
		break;
	default:
		break;
	}

	set_bit(id - 1, &w->port_change);

	spin_unlock_irqrestore(&w->dev_lock, flags);

	usb_hcd_poll_rh_status(warp_to_hcd(w));

	dev_info(&w->pdev->dev,
		 "usbwarp: device added id=%u VID=%04x PID=%04x speed=%u\n",
		 id, msg->vendor_id, msg->product_id, msg->speed);

	memset(&ack, 0, sizeof(ack));
	fill_msg_header(&ack.hdr, USBWARP_MSG_DEVICE_ADD_ACK, id, 0,
			sizeof(ack));
	ack.device_id = id;
	ack.status    = 0;
	usbwarp_ring_produce(&w->g2h, &ack, sizeof(ack));
}

void usbwarp_handle_device_removed(struct usbwarp_hcd *w,
				   const struct usbwarp_msg_device_removed *msg)
{
	uint32_t id = msg->device_id;
	unsigned long flags;
	struct usbwarp_msg_device_ack ack;

	if (id == 0 || id > USBWARP_MAX_PORTS)
		return;

	/* Cancel all pending URBs for this device FIRST. */
	usbwarp_cancel_device_urbs(w, id);

	spin_lock_irqsave(&w->dev_lock, flags);

	w->devices[id - 1].connected = false;
	w->devices[id - 1].usb_addr  = 0;
	if (w->device_count > 0)
		w->device_count--;

	w->port_status[id - 1] = USB_PORT_STAT_POWER |
				  (USB_PORT_STAT_C_CONNECTION << 16);
	set_bit(id - 1, &w->port_change);

	spin_unlock_irqrestore(&w->dev_lock, flags);

	usb_hcd_poll_rh_status(warp_to_hcd(w));

	dev_info(&w->pdev->dev,
		 "usbwarp: device removed id=%u reason=%u\n",
		 id, msg->reason);

	memset(&ack, 0, sizeof(ack));
	fill_msg_header(&ack.hdr, USBWARP_MSG_DEVICE_REMOVE_ACK, id, 0,
			sizeof(ack));
	ack.device_id = id;
	ack.status    = 0;
	usbwarp_ring_produce(&w->g2h, &ack, sizeof(ack));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  HCD callbacks: address_device — capture USB address mapping
 *
 *   When USB Core completes SET_ADDRESS for a new device, it calls
 *   address_device if provided, or we intercept via the alloc_dev_fn.
 *   Since the virtual HCD doesn't do real USB transactions, we use
 *   the hub_control RESET completion to detect the port, then match
 *   during the first URB enqueue.
 *
 *   Alternative: implement .address_device callback.  For now, we
 *   scan at enqueue time and set usb_addr on first successful match
 *   via the alloc_dev/free_dev hooks.
 *
 *   SIMPLIFIED APPROACH: In alloc_dev, the USB Core tells us the
 *   devnum.  We match it to the most recently reset port.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int usbwarp_alloc_dev(struct usb_hcd *hcd, struct usb_device *udev)
{
	struct usbwarp_hcd *w = hcd_to_warp(hcd);

	/* Root hub itself: always succeed. */
	if (!udev->parent)
		return 1;

	/* udev->portnum is 1-based.  Map to our 0-based port index. */
	if (udev->portnum >= 1 && udev->portnum <= USBWARP_MAX_PORTS) {
		unsigned int port = udev->portnum - 1;

		if (w->devices[port].connected) {
			/* Record the USB address → device_id mapping.
			 * At this point udev->devnum may be 0 (not yet
			 * assigned).  We store portnum for now and update
			 * devnum later via .address_device or lazily in
			 * urb_enqueue. */
			dev_dbg(&w->pdev->dev,
				"usbwarp: alloc_dev port=%u devnum=%u\n",
				udev->portnum, udev->devnum);
		}
	}

	return 1;  /* 1 = success in alloc_dev */
}

static void usbwarp_free_dev(struct usb_hcd *hcd, struct usb_device *udev)
{
	struct usbwarp_hcd *w = hcd_to_warp(hcd);
	unsigned long flags;
	unsigned int i;

	if (!udev->parent)
		return;

	/* Clear USB address mapping. */
	spin_lock_irqsave(&w->dev_lock, flags);
	for (i = 0; i < USBWARP_MAX_PORTS; i++) {
		if (w->devices[i].connected &&
		    w->devices[i].usb_addr == udev->devnum) {
			w->devices[i].usb_addr = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&w->dev_lock, flags);
}

static int usbwarp_address_device(struct usb_hcd *hcd, struct usb_device *udev,
				  unsigned int timeout_ms)
{
	struct usbwarp_hcd *w = hcd_to_warp(hcd);

	(void)timeout_ms;
	if (udev->parent && udev->portnum >= 1 &&
	    udev->portnum <= USBWARP_MAX_PORTS) {
		unsigned int port = udev->portnum - 1;

		/* NOTE: udev->devnum is 0 here!  Linux USB core calls
		 * address_device() BEFORE update_devnum().  We must NOT
		 * store usb_addr=0 as that would overwrite any valid
		 * mapping.  The real address will be captured lazily in
		 * urb_enqueue when the first URB arrives at the new
		 * address and falls through to port-based lookup. */
		if (udev->devnum != 0) {
			usbwarp_set_usb_addr(w, port, udev->devnum);
		}

		dev_info(&w->pdev->dev,
			 "usbwarp: address_device port=%u devnum=%u → devid=%u\n",
			 udev->portnum, udev->devnum,
			 w->devices[port].device_id);
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  HCD start / stop / stubs
 * ═══════════════════════════════════════════════════════════════════════════ */

static int usbwarp_hcd_start(struct usb_hcd *hcd)
{
	hcd->state = HC_STATE_RUNNING;
	hcd->uses_new_polling = 1;
	return 0;
}

static void usbwarp_hcd_stop(struct usb_hcd *hcd)
{
	(void)hcd;
}

static void usbwarp_endpoint_disable(struct usb_hcd *hcd,
				     struct usb_host_endpoint *ep)
{
	(void)hcd; (void)ep;
}

static int usbwarp_get_frame_number(struct usb_hcd *hcd)
{
	(void)hcd;
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §12  hc_driver table
 * ═══════════════════════════════════════════════════════════════════════════ */

const struct hc_driver usbwarp_hc_driver = {
	.description       = "usbwarp-hcd",
	.product_desc      = "UsbWarp Virtual Host Controller",
	.hcd_priv_size     = sizeof(struct usbwarp_hcd),
	.flags             = HCD_USB2,

	.start             = usbwarp_hcd_start,
	.stop              = usbwarp_hcd_stop,

	.urb_enqueue       = usbwarp_urb_enqueue,
	.urb_dequeue       = usbwarp_urb_dequeue,

	.hub_status_data   = usbwarp_hub_status_data,
	.hub_control       = usbwarp_hub_control,

	.alloc_dev         = usbwarp_alloc_dev,
	.free_dev          = usbwarp_free_dev,
	.address_device    = usbwarp_address_device,

	.endpoint_disable  = usbwarp_endpoint_disable,
	.get_frame_number  = usbwarp_get_frame_number,
};
