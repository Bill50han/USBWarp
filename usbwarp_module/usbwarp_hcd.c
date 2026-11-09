// SPDX-License-Identifier: GPL-2.0
/* Virtual USB HCD. urb_lock serializes publication, unlink and completion.
 * Transfer deadlines belong to the USB client; the worker retries unlinks. */
#include "usbwarp_guest.h"
#include <linux/usb/ch9.h>
#include <linux/usb/ch11.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <asm/unaligned.h>

static uint32_t next_txn_id(struct usbwarp_hcd *w)
{
	u32 id;
	do { id = (u32)atomic_inc_return(&w->txn_counter); } while (!id);
	return id;
}

static void fill_msg_header(struct usbwarp_msg_header *h, u16 type,
			    u32 dev_id, u32 txn_id, u32 len)
{
	memset(h, 0, sizeof(*h));
	h->magic = USBWARP_MSG_MAGIC;
	h->message_type = type;
	h->protocol_version = USBWARP_PROTOCOL_VERSION;
	h->message_length = len;
	h->device_id = dev_id;
	h->transaction_id = txn_id;
	h->timestamp = ktime_get_ns();
}

static int usbwarp_check_iso(struct usbwarp_hcd *w, struct urb *urb,
			    u32 speed, u32 *packed_length)
{
	u32 i, length = 0;
	if ((w->negotiated_caps & (USBWARP_CAP_ISO_TRANSFER | USBWARP_CAP_ISO_PACKED)) !=
	    (USBWARP_CAP_ISO_TRANSFER | USBWARP_CAP_ISO_PACKED))
		return -EOPNOTSUPP;
	/* Cross-VM frame clocks aren't synchronized. Never silently reinterpret
	 * an explicit frame schedule as ASAP. */
	if (!(urb->transfer_flags & URB_ISO_ASAP))
		return -EOPNOTSUPP;
	if (!usbwarp_iso_packet_count_valid(urb->number_of_packets, speed, urb->interval))
		return -EINVAL;
	for (i = 0; i < urb->number_of_packets; ++i) {
		u32 off = urb->iso_frame_desc[i].offset;
		u32 len = urb->iso_frame_desc[i].length;
		if (off > urb->transfer_buffer_length ||
		    len > urb->transfer_buffer_length - off ||
		    len > USBWARP_MAX_TRANSFER_SIZE - length)
			return -EINVAL;
		length += len;
	}
	*packed_length = length;
	return 0;
}

static int usbwarp_urb_enqueue(struct usb_hcd *hcd, struct urb *urb, gfp_t mem_flags)
{
	struct usbwarp_hcd *w = hcd_to_warp(hcd);
	struct usbwarp_urb_priv *p;
	struct usbwarp_device *dev;
	struct usbwarp_msg_urb_submit submit = { 0 };
	struct usbwarp_msg_iso_submit *iso = NULL;
	void *message = &submit;
	unsigned long flags;
	u32 id, generation, epoch, speed, length, slots = 0, msg_len, i, off = 0;
	bool is_iso = usb_pipeisoc(urb->pipe), out = usb_pipeout(urb->pipe), inl;
	int ret, buf = -1;

	if (READ_ONCE(w->shutting_down) || READ_ONCE(w->quiescing))
		return -ESHUTDOWN;
	if (urb->transfer_buffer_length < 0 ||
	    urb->transfer_buffer_length > USBWARP_MAX_TRANSFER_SIZE ||
	    (urb->transfer_buffer_length && !urb->transfer_buffer && !urb->num_sgs))
		return -EINVAL;
	if (!urb->dev->parent)
		return -ENODEV;
	/* One root-hub port per exported physical device. */
	spin_lock_irqsave(&w->dev_lock, flags);
	id = usbwarp_find_devid_by_port(hcd, urb->dev->portnum);
	if (id && usb_pipedevice(urb->pipe))
		w->devices[id - 1].usb_addr = usb_pipedevice(urb->pipe);
	spin_unlock_irqrestore(&w->dev_lock, flags);
	if (!id)
		return -ENODEV;
	dev = &w->devices[id - 1];
	spin_lock_irqsave(&w->urb_lock, flags);
	generation = dev->generation;
	epoch = dev->epoch;
	speed = dev->speed;
	if (dev->udev != urb->dev) {
		spin_unlock_irqrestore(&w->urb_lock, flags);
		return -ENODEV;
	}
	spin_unlock_irqrestore(&w->urb_lock, flags);
	if (urb->stream_id && (!(w->negotiated_caps & USBWARP_CAP_STREAMS) ||
	    !usb_pipebulk(urb->pipe) || urb->stream_id > USBWARP_MAX_STREAMS ||
	    urb->stream_id > urb->ep->streams))
		return -EINVAL;
	length = urb->transfer_buffer_length;
	if (is_iso) {
		ret = usbwarp_check_iso(w, urb, speed, &length);
		if (ret)
			return ret;
	}
	inl = !is_iso && length && length <= USBWARP_INLINE_DATA_SIZE &&
	      (w->negotiated_caps & USBWARP_CAP_INLINE_DATA);
	if (!inl && (length || is_iso)) {
		slots = DIV_ROUND_UP(max(length, 1u), w->buffer_size);
		buf = usbwarp_buf_alloc_contig(w, slots);
		if (buf < 0)
			return buf;
	}
	p = kzalloc(sizeof(*p), mem_flags);
	if (!p) { ret = -ENOMEM; goto free_buffer; }
	p->urb = urb;
	p->device_id = id;
	p->epoch = epoch;
	p->transaction_id = next_txn_id(w);
	p->buffer_index = buf;
	p->slot_count = slots;
	p->number_of_packets = is_iso ? urb->number_of_packets : 0;
	p->state = URB_STATE_ACTIVE;
	/* Like a native HCD, leave asynchronous requests pending until completion
	 * or client unlink. HID/serial/network receives may legitimately wait
	 * indefinitely; liveness and DMA-safe teardown are handled separately. */

	if (is_iso) {
		struct usbwarp_iso_packet_desc *packets;
		msg_len = usbwarp_iso_message_size(urb->number_of_packets);
		iso = kzalloc(msg_len, mem_flags);
		if (!iso) { ret = -ENOMEM; goto free_priv; }
		message = iso;
		fill_msg_header(&iso->hdr, USBWARP_MSG_ISO_SUBMIT, id, p->transaction_id, msg_len);
        ((struct usbwarp_msg_header *)message)->flags = epoch;
		iso->device_id = id;
		iso->endpoint = usb_pipeendpoint(urb->pipe);
		iso->direction = !out;
		iso->number_of_packets = urb->number_of_packets;
		iso->total_length = length;
		iso->buffer_offset = usbwarp_buf_offset(w, buf);
		iso->interval = urb->interval;
		packets = usbwarp_iso_packets(iso);
		for (i = 0; i < iso->number_of_packets; ++i) {
			u32 n = urb->iso_frame_desc[i].length;
			packets[i].offset = off;
			packets[i].length = n;
			if (out && n && usbwarp_copy_urb_data(urb, urb->iso_frame_desc[i].offset, NULL,
                    usbwarp_buf_addr(w, buf) + off, n, false) != n) {
                ret = -EINVAL; goto free_priv;
            }
			off += n;
		}
	} else {
		msg_len = USBWARP_MSG_URB_SUBMIT_BASE_SIZE + (inl && out ? length : 0);
		fill_msg_header(&submit.hdr, USBWARP_MSG_URB_SUBMIT, id, p->transaction_id, msg_len);
        ((struct usbwarp_msg_header *)message)->flags = epoch;
		submit.device_id = id;
		submit.endpoint = usb_pipeendpoint(urb->pipe);
		submit.direction = !out;
		submit.transfer_length = length;
		submit.transfer_flags = urb->transfer_flags;
        if (usb_pipebulk(urb->pipe) && out && length &&
            (urb->transfer_flags & URB_ZERO_PACKET) && usb_endpoint_maxp(&urb->ep->desc) &&
            length % usb_endpoint_maxp(&urb->ep->desc) == 0)
            p->need_zlp = true;
		submit.stream_id = urb->stream_id;
		submit.interval = urb->interval;
		if (usb_pipecontrol(urb->pipe)) {
			if (!urb->setup_packet) { ret = -EINVAL; goto free_priv; }
			submit.transfer_type = USBWARP_XFER_CONTROL;
			memcpy(submit.setup_packet, urb->setup_packet, 8);
		} else if (usb_pipebulk(urb->pipe))
			submit.transfer_type = USBWARP_XFER_BULK;
		else if (usb_pipeint(urb->pipe))
			submit.transfer_type = USBWARP_XFER_INTERRUPT;
		else { ret = -EINVAL; goto free_priv; }
		submit.data_mode = !length ? USBWARP_DATA_NONE :
				   inl ? USBWARP_DATA_INLINE : USBWARP_DATA_BUFFER;
		if (inl && out && usbwarp_copy_urb_data(urb, 0, submit.inline_data, NULL, length, false) != length) {
            ret = -EINVAL; goto free_priv;
        }
		if (buf >= 0) {
			submit.buffer_offset = usbwarp_buf_offset(w, buf);
			if (out && length && usbwarp_copy_urb_data(urb, 0, NULL,
                usbwarp_buf_addr(w, buf), length, false) != length) {
                ret = -EINVAL; goto free_priv;
            }
		}
	}

	if (!is_iso) p->wire_submit = submit;

	/* Initialize and link everything before the host can see the message.
	 * Completion, cancellation and device removal take this same lock. */
	spin_lock_irqsave(&w->urb_lock, flags);
	if (w->shutting_down || w->quiescing || !dev->connected || dev->generation != generation || dev->udev != urb->dev) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (usbwarp_debugfs_g2h_paused()) { ret = -EBUSY; goto unlock; }
	if (atomic_read(&dev->pending_urbs) >= w->max_pending_urbs ||
	    !usbwarp_device_quota_allows(w, id, slots)) { ret = -ENOSPC; goto unlock; }
	ret = usb_hcd_link_urb_to_ep(hcd, urb);
	if (ret)
		goto unlock;
	urb->hcpriv = p;
	hash_add(w->urb_table, &p->hash_node, p->transaction_id);
	atomic_inc(&w->total_pending);
	atomic_inc(&dev->pending_urbs);
	atomic_add(slots, &dev->buf_in_use);
	if (is_iso) {
		atomic_inc(&w->iso_pending);
        smp_mb__after_atomic();
    }
    if (!is_iso && usbwarp_submission_blocked(p)) {
        p->queued = true;
        atomic_inc(&w->tx_pending);
        ret = 0;
    } else {
        ret = usbwarp_ring_produce(&w->g2h, message, msg_len);
        if (!ret && !is_iso) {
            p->published_ns = ktime_get_ns();
            p->busy_eligible = usbwarp_rx_submit(w, submit.transfer_type, length);
        }
    }
	if (ret) {
		hash_del(&p->hash_node);
		urb->hcpriv = NULL;
		usb_hcd_unlink_urb_from_ep(hcd, urb);
		atomic_dec(&w->total_pending);
		atomic_dec(&dev->pending_urbs);
		atomic_sub(slots, &dev->buf_in_use);
		if (is_iso)
			atomic_dec(&w->iso_pending);
	}
unlock:
	spin_unlock_irqrestore(&w->urb_lock, flags);
    if (!ret && (atomic_read(&w->tx_pending) ||
        (is_iso && usbwarp_rx_priority_pending(w)))) wake_up(&w->irq_wait);
	kfree(iso);
	iso = NULL;
	if (!ret)
		return 0; /* p and urb may already have completed. */
free_priv:
	kfree(iso);
	kfree(p);
free_buffer:
	if (buf >= 0)
		usbwarp_buf_free_range(w, buf, slots);
	return ret;
}

static int usbwarp_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
	struct usbwarp_hcd *w = hcd_to_warp(hcd);
	struct usbwarp_urb_priv *p;
	unsigned long flags;
	int ret;
	spin_lock_irqsave(&w->urb_lock, flags);
	ret = usb_hcd_check_unlink_urb(hcd, urb, status);
	p = urb->hcpriv;
	if (!ret && p) {
		p->state = URB_STATE_CANCELING;
		p->cancel_status = status;
		if (!p->queued) usbwarp_send_cancel_locked(w, p);
	}
	spin_unlock_irqrestore(&w->urb_lock, flags);
	return ret;
}

/* Remove and claim exactly once, while the pointer is still protected. */
static struct usbwarp_urb_priv *take_urb(struct usbwarp_hcd *w,
		const struct usbwarp_msg_header *hdr, bool iso)
{
	struct usbwarp_urb_priv *p;
	unsigned long flags;
	spin_lock_irqsave(&w->urb_lock, flags);
	hash_for_each_possible(w->urb_table, p, hash_node, hdr->transaction_id) {
		if (p->transaction_id != hdr->transaction_id || p->device_id != hdr->device_id ||
		    p->epoch != hdr->flags || p->queued || !!p->number_of_packets != iso)
			continue;
		hash_del(&p->hash_node);
		p->state = URB_STATE_DEAD;
		p->urb->hcpriv = NULL;
		usb_hcd_unlink_urb_from_ep(bus_to_hcd(p->urb->dev->bus), p->urb);
		spin_unlock_irqrestore(&w->urb_lock, flags);
		return p;
	}
	spin_unlock_irqrestore(&w->urb_lock, flags);
	return NULL;
}

static int status_to_errno(int32_t status)
{
	switch (status) {
	case USBWARP_STATUS_SUCCESS: return 0;
	case USBWARP_STATUS_NOT_SUPPORTED: return -EOPNOTSUPP;
	case USBWARP_STATUS_CANCELLED: return -ECONNRESET;
	case USBWARP_STATUS_STALL: return -EPIPE;
	case USBWARP_STATUS_TIMEOUT: return -ETIMEDOUT;
	case USBWARP_STATUS_ISO_MISSED: return -EXDEV;
	case USBWARP_STATUS_CRC: return -EILSEQ;
	case USBWARP_STATUS_BABBLE: return -EOVERFLOW;
	case USBWARP_STATUS_SHORT_PACKET: return -EREMOTEIO;
	case USBWARP_STATUS_DISCONNECTED:
	case USBWARP_STATUS_SHUTDOWN: return -ESHUTDOWN;
	case USBWARP_STATUS_OVERFLOW: return -EOVERFLOW;
	case USBWARP_STATUS_NO_RESOURCE: return -ENOMEM;
	default: return -EPROTO;
	}
}

static void finish_urb(struct usbwarp_hcd *w, struct usbwarp_urb_priv *p, int status)
{
	struct urb *urb = p->urb;
	struct usbwarp_device *dev = &w->devices[p->device_id - 1];
	unsigned long flags;
	if (p->buffer_index >= 0)
		usbwarp_buf_free_range(w, p->buffer_index, p->slot_count);
	atomic_sub(p->slot_count, &dev->buf_in_use);
	atomic_dec(&dev->pending_urbs);
	atomic_dec(&w->total_pending);
	if (p->number_of_packets)
		atomic_dec(&w->iso_pending);
	if (p->cancel_status)
		status = p->cancel_status;
    usbwarp_rx_progress(w, p->busy_eligible, p->published_ns, status);
	kfree(p);
	/* Callback may free or immediately resubmit urb. Touch neither after it. */
	local_irq_save(flags);
	usb_hcd_giveback_urb(bus_to_hcd(urb->dev->bus), urb, status);
	local_irq_restore(flags);
}

void usbwarp_flush_queued(struct usbwarp_hcd *w)
{
    struct usbwarp_urb_priv *p, *next;
    struct hlist_node *tmp;
    unsigned long flags;
    int bkt;
    LIST_HEAD(done);
    if (!atomic_read(&w->tx_pending)) return;
    spin_lock_irqsave(&w->urb_lock, flags);
    hash_for_each_safe(w->urb_table, bkt, tmp, p, hash_node) {
        if (!p->queued) continue;
        if (p->state == URB_STATE_CANCELING || w->quiescing || w->shutting_down) {
            hash_del(&p->hash_node);
            p->urb->hcpriv = NULL;
            usb_hcd_unlink_urb_from_ep(bus_to_hcd(p->urb->dev->bus), p->urb);
            atomic_dec(&w->tx_pending);
            p->queued = false;
            if (!p->cancel_status) p->cancel_status = -ESHUTDOWN;
            list_add_tail(&p->cancel_node, &done);
        } else usbwarp_publish_queued(w, p);
    }
    spin_unlock_irqrestore(&w->urb_lock, flags);
    list_for_each_entry_safe(p, next, &done, cancel_node) {
        list_del(&p->cancel_node);
        p->urb->actual_length = p->saved_actual;
        finish_urb(w, p, p->cancel_status);
    }
}

void usbwarp_complete_urb(struct usbwarp_hcd *w, const struct usbwarp_msg_urb_complete *msg)
{
	struct usbwarp_urb_priv *p;
	struct urb *urb;
	u32 n;
	int status;
    if (usbwarp_stage_zlp(w, msg)) return;
    p = take_urb(w, &msg->hdr, false);
	if (!p)
		return;
	urb = p->urb;
	n = msg->actual_length;
	status = status_to_errno(msg->status);
	if (msg->device_id != p->device_id || msg->endpoint != usb_pipeendpoint(urb->pipe) ||
	    n > urb->transfer_buffer_length || msg->data_mode > USBWARP_DATA_INLINE ||
	    (msg->data_mode == USBWARP_DATA_INLINE &&
	     (n > USBWARP_INLINE_DATA_SIZE || (!usb_pipeout(urb->pipe) &&
	      msg->hdr.message_length < USBWARP_MSG_URB_COMPLETE_BASE_SIZE + n))) ||
	    (msg->data_mode == USBWARP_DATA_BUFFER &&
	     (p->buffer_index < 0 || msg->buffer_offset != usbwarp_buf_offset(w, p->buffer_index))) ||
	    (n && msg->data_mode == USBWARP_DATA_NONE)) {
		status = -EPROTO;
		n = 0;
	}
	/* Short/error transfers can contain valid partial IN data. */
	if (n && !usb_pipeout(urb->pipe)) {
        size_t copied = usbwarp_copy_urb_data(urb, 0, (void *)msg->inline_data,
            msg->data_mode == USBWARP_DATA_BUFFER ? usbwarp_buf_addr(w, p->buffer_index) : NULL,
            n, true);
        if (copied != n) { status = -EFAULT; n = 0; }
	}
	urb->actual_length = p->zlp_stage ? p->saved_actual : n;
	finish_urb(w, p, status);
}

void usbwarp_complete_iso_urb(struct usbwarp_hcd *w,
		const struct usbwarp_msg_iso_complete *msg, uint32_t msg_len)
{
	struct usbwarp_urb_priv *p = take_urb(w, &msg->hdr, true);
	const struct usbwarp_iso_packet_result *results = usbwarp_iso_results_const(msg);
	struct urb *urb;
	u32 i, off = 0, actual = 0;
	int status;
	bool valid;
	if (!p)
		return;
	urb = p->urb;
	status = status_to_errno(msg->status);
	valid = usbwarp_iso_complete_valid(msg, msg_len) &&
		msg->endpoint == usb_pipeendpoint(urb->pipe) &&
		msg->number_of_packets == p->number_of_packets &&
		(msg->data_mode == USBWARP_DATA_BUFFER && p->buffer_index >= 0 &&
		 msg->buffer_offset == usbwarp_buf_offset(w, p->buffer_index));
	if (!valid && !status)
		status = -EPROTO;
	urb->error_count = 0;
	for (i = 0; i < p->number_of_packets; ++i) {
		struct usb_iso_packet_descriptor *d = &urb->iso_frame_desc[i];
		u32 n = valid ? results[i].actual_length : 0;
		int ps = valid ? status_to_errno(results[i].status) : status;
		if (n > d->length) { n = 0; ps = -EOVERFLOW; }
        if (n && !usb_pipeout(urb->pipe) && usbwarp_copy_urb_data(urb, d->offset, NULL,
            usbwarp_buf_addr(w, p->buffer_index) + off, n, true) != n) {
            n = 0;
            ps = -EFAULT;
        }
        if (ps) ++urb->error_count;
        d->actual_length = n;
        d->status = ps;
		off += d->length;
		actual += n;
	}
	urb->actual_length = actual;
	/* Windows reports frames, Linux HS ISO URBs report microframes. */
	urb->start_frame = msg->start_frame * (urb->dev->speed >= USB_SPEED_HIGH ? 8 : 1);
	finish_urb(w, p, status);
}

void usbwarp_cancel_device_urbs(struct usbwarp_hcd *w, uint32_t device_id)
{
	struct usbwarp_urb_priv *p, *next;
	struct hlist_node *tmp;
	unsigned long flags;
	int bkt, i;
	LIST_HEAD(done);
	spin_lock_irqsave(&w->urb_lock, flags);
	/* Prevent a completion callback from submitting another URB on removal. */
	w->devices[device_id - 1].connected = false;
    if (w->config_txn && w->config_device == device_id) {
        w->config_status = -ESHUTDOWN;
        complete(&w->config_done);
    }
	hash_for_each_safe(w->urb_table, bkt, tmp, p, hash_node) {
		if (p->device_id != device_id)
			continue;
		hash_del(&p->hash_node);
        if (p->queued) atomic_dec(&w->tx_pending);
		p->urb->hcpriv = NULL;
		usb_hcd_unlink_urb_from_ep(bus_to_hcd(p->urb->dev->bus), p->urb);
		list_add_tail(&p->cancel_node, &done);
	}
	spin_unlock_irqrestore(&w->urb_lock, flags);
	list_for_each_entry_safe(p, next, &done, cancel_node) {
		list_del(&p->cancel_node);
		p->urb->actual_length = p->saved_actual;
		p->urb->error_count = p->number_of_packets;
		for (i = 0; i < p->number_of_packets; ++i) {
			p->urb->iso_frame_desc[i].actual_length = 0;
			p->urb->iso_frame_desc[i].status = -ESHUTDOWN;
		}
		finish_urb(w, p, -ESHUTDOWN);
	}
}

void usbwarp_shutdown_all_devices(struct usbwarp_hcd *w, uint32_t reason)
{
    unsigned long flags;
    unsigned int bus, i;
    WRITE_ONCE(w->shutting_down, true);
    for (i = 0; i < USBWARP_MAX_DEVICES_LIMIT; ++i)
        usbwarp_cancel_device_urbs(w, i + 1);
    spin_lock_irqsave(&w->dev_lock, flags);
    for (bus = 0; bus < 2; ++bus) {
        struct usbwarp_roothub *rh = hcd_to_rh(w->hcd[bus]);
        for (i = 0; i < rh->num_ports; ++i) {
            rh->device_id[i] = 0;
            rh->port_status[i] = (bus ? USB_SS_PORT_STAT_POWER : USB_PORT_STAT_POWER) |
                                (USB_PORT_STAT_C_CONNECTION << 16);
        }
    }
    w->device_count = 0;
    spin_unlock_irqrestore(&w->dev_lock, flags);
    usb_hcd_poll_rh_status(w->hcd[0]);
    usb_hcd_poll_rh_status(w->hcd[1]);
    dev_info(&w->pdev->dev, "usbwarp: shutdown reason=%u\n", reason);
}

void usbwarp_handle_device_added(struct usbwarp_hcd *w, const struct usbwarp_msg_device_added *msg)
{
    u32 id = msg->device_id;
    unsigned long flags;
    struct usbwarp_msg_device_ack ack = { 0 };
    struct usbwarp_device *dev;
    struct usb_hcd *hcd;
    struct usbwarp_roothub *rh;
    unsigned int port;
    bool ss = usbwarp_speed_is_super(msg->speed);
    int status = USBWARP_STATUS_SUCCESS;
    if (!id || id > USBWARP_MAX_DEVICES_LIMIT || msg->speed < USBWARP_SPEED_LOW || msg->speed > USBWARP_SPEED_MAX)
        return;
    hcd = w->hcd[ss ? 1 : 0];
    rh = hcd_to_rh(hcd);
    dev = &w->devices[id - 1];
    spin_lock_irqsave(&w->dev_lock, flags);
    spin_lock(&w->urb_lock);
    if (dev->connected) { status = USBWARP_STATUS_INVALID_PARAM; goto unlock; }
    for (port = 0; port < rh->num_ports; ++port)
        if (!rh->device_id[port]) break;
    if (port == rh->num_ports) { status = USBWARP_STATUS_NO_RESOURCE; goto unlock; }
    dev->device_id = id;
    dev->epoch = msg->hdr.flags;
    ++dev->generation;
    dev->udev = NULL;
    dev->hcd = hcd;
    dev->port = port + 1;
    dev->connected = true;
    dev->vendor_id = msg->vendor_id;
    dev->product_id = msg->product_id;
    dev->speed = msg->speed;
    dev->usb_addr = 0;
    atomic_set(&dev->pending_urbs, 0);
    atomic_set(&dev->buf_in_use, 0);
    ++w->device_count;
    rh->device_id[port] = id;
    rh->port_status[port] = USB_PORT_STAT_CONNECTION | (USB_PORT_STAT_C_CONNECTION << 16) |
                           (ss ? USB_SS_PORT_STAT_POWER | USB_PORT_STAT_ENABLE : USB_PORT_STAT_POWER);
    if (msg->speed == USBWARP_SPEED_LOW) rh->port_status[port] |= USB_PORT_STAT_LOW_SPEED;
    if (msg->speed == USBWARP_SPEED_HIGH) rh->port_status[port] |= USB_PORT_STAT_HIGH_SPEED;
unlock:
    spin_unlock(&w->urb_lock);
    spin_unlock_irqrestore(&w->dev_lock, flags);
    if (!status) usb_hcd_poll_rh_status(hcd);
    fill_msg_header(&ack.hdr, USBWARP_MSG_DEVICE_ADD_ACK, id, 0, sizeof(ack));
    ack.device_id = id;
    ack.status = status;
    usbwarp_ring_produce(&w->g2h, &ack, sizeof(ack));
}

void usbwarp_handle_device_removed(struct usbwarp_hcd *w, const struct usbwarp_msg_device_removed *msg)
{
    u32 id = msg->device_id;
    struct usbwarp_device *dev;
    struct usb_hcd *hcd;
    struct usbwarp_roothub *rh;
    struct usbwarp_msg_device_ack ack = { 0 };
    unsigned long flags;
    unsigned int port;
    if (!id || id > USBWARP_MAX_DEVICES_LIMIT) return;
    dev = &w->devices[id - 1];
    if (!dev->connected || dev->epoch != msg->hdr.flags) return;
    hcd = dev->hcd;
    rh = hcd_to_rh(hcd);
    port = dev->port - 1;
    usbwarp_cancel_device_urbs(w, id);
    spin_lock_irqsave(&w->dev_lock, flags);
    rh->device_id[port] = 0;
    rh->port_status[port] = (usbwarp_is_usb3(hcd) ? USB_SS_PORT_STAT_POWER : USB_PORT_STAT_POWER) |
                           (USB_PORT_STAT_C_CONNECTION << 16);
    if (w->device_count) --w->device_count;
    spin_unlock_irqrestore(&w->dev_lock, flags);
    usb_hcd_poll_rh_status(hcd);
    fill_msg_header(&ack.hdr, USBWARP_MSG_DEVICE_REMOVE_ACK, id, 0, sizeof(ack));
    ack.device_id = id;
    usbwarp_ring_produce(&w->g2h, &ack, sizeof(ack));
}

static int usbwarp_enable_device(struct usb_hcd *hcd, struct usb_device *udev)
{
    struct usbwarp_hcd *w = hcd_to_warp(hcd);
    struct usbwarp_device *dev;
    unsigned long flags;
    u32 id;
    int ret = -ENODEV;
    if (!udev->parent) return 0;
    if (udev->parent->parent) return -ENODEV;
    spin_lock_irqsave(&w->dev_lock, flags);
    spin_lock(&w->urb_lock);
    id = usbwarp_find_devid_by_port(hcd, udev->portnum);
    if (id) {
        dev = &w->devices[id - 1];
        if (dev->connected && dev->hcd == hcd && (!dev->udev || dev->udev == udev)) {
            dev->udev = udev;
            ret = 0;
        }
    }
    spin_unlock(&w->urb_lock);
    spin_unlock_irqrestore(&w->dev_lock, flags);
    return ret;
}

static void usbwarp_free_dev(struct usb_hcd *hcd, struct usb_device *udev)
{
    struct usbwarp_hcd *w = hcd_to_warp(hcd);
    unsigned long flags;
    unsigned int i;
    spin_lock_irqsave(&w->urb_lock, flags);
    for (i = 0; i < USBWARP_MAX_DEVICES_LIMIT; ++i)
        if (w->devices[i].udev == udev) w->devices[i].udev = NULL;
    spin_unlock_irqrestore(&w->urb_lock, flags);
}

static int usbwarp_address_device(struct usb_hcd *hcd, struct usb_device *udev, unsigned int timeout_ms)
{
    (void)timeout_ms;
    /* Physical addresses stay owned by Windows. */
    return usbwarp_enable_device(hcd, udev);
}

static int usbwarp_hcd_setup(struct usb_hcd *hcd)
{
    bool ss = usbwarp_is_usb3(hcd);
    struct usbwarp_roothub *rh = hcd_to_rh(hcd);
    rh->num_ports = ss ? USBWARP_USB3_PORTS : USBWARP_USB2_PORTS;
    hcd->speed = ss ? HCD_USB32 : HCD_USB2;
    hcd->self.root_hub->speed = ss ? USB_SPEED_SUPER_PLUS : USB_SPEED_HIGH;
    if (ss) {
        hcd->self.root_hub->ssp_rate = USB_SSP_GEN_2x2;
        hcd->self.root_hub->rx_lanes = 2;
        hcd->self.root_hub->tx_lanes = 2;
    }
    hcd->self.sg_tablesize = 128;
    hcd->self.no_sg_constraint = 1;
    hcd->can_do_streams = ss;
    return 0;
}

static int usbwarp_hcd_start(struct usb_hcd *hcd)
{
    hcd->state = HC_STATE_RUNNING;
    hcd->uses_new_polling = 1;
    return 0;
}
static void usbwarp_hcd_stop(struct usb_hcd *hcd) { (void)hcd; }
static int usbwarp_bus_suspend(struct usb_hcd *hcd) { hcd->state = HC_STATE_SUSPENDED; return 0; }
static int usbwarp_bus_resume(struct usb_hcd *hcd) { hcd->state = HC_STATE_RUNNING; return 0; }
static void usbwarp_endpoint_disable(struct usb_hcd *hcd, struct usb_host_endpoint *ep)
{
    (void)hcd; (void)ep; /* USB core has flushed its URB list before this callback. */
}
static int usbwarp_get_frame_number(struct usb_hcd *hcd)
{
    (void)hcd;
    return (ktime_get_ns() / NSEC_PER_MSEC) & 0x7ff;
}

/* Stream RPC callbacks are defined below; they execute in sleepable context. */
static int usbwarp_alloc_streams(struct usb_hcd *, struct usb_device *,
    struct usb_host_endpoint **, unsigned int, unsigned int, gfp_t);
static int usbwarp_free_streams(struct usb_hcd *, struct usb_device *,
    struct usb_host_endpoint **, unsigned int, gfp_t);

const struct hc_driver usbwarp_hc_driver = {
    .description = "usbwarp-hcd",
    .product_desc = "UsbWarp USB 2 / USB 3.2 Virtual Host Controller",
    .hcd_priv_size = sizeof(struct usbwarp_roothub),
    .flags = HCD_USB3 | HCD_SHARED | HCD_BH,
    .reset = usbwarp_hcd_setup,
    .start = usbwarp_hcd_start,
    .stop = usbwarp_hcd_stop,
    .urb_enqueue = usbwarp_urb_enqueue,
    .urb_dequeue = usbwarp_urb_dequeue,
    .hub_status_data = usbwarp_hub_status_data,
    .hub_control = usbwarp_hub_control,
    .enable_device = usbwarp_enable_device,
    .free_dev = usbwarp_free_dev,
    .address_device = usbwarp_address_device,
    .endpoint_disable = usbwarp_endpoint_disable,
    .get_frame_number = usbwarp_get_frame_number,
    .bus_suspend = usbwarp_bus_suspend,
    .bus_resume = usbwarp_bus_resume,
    .alloc_streams = usbwarp_alloc_streams,
    .free_streams = usbwarp_free_streams,
};

void usbwarp_complete_streams(struct usbwarp_hcd *w,
                             const struct usbwarp_msg_streams_response *msg)
{
    unsigned long flags;
    spin_lock_irqsave(&w->urb_lock, flags);
    if (w->config_txn && msg->hdr.transaction_id == w->config_txn &&
        msg->hdr.device_id == w->config_device && msg->hdr.flags == w->config_epoch && w->config_status == -EINPROGRESS) {
        w->config_status = status_to_errno(msg->status);
        w->config_streams = msg->num_streams;
        complete(&w->config_done);
    }
    spin_unlock_irqrestore(&w->urb_lock, flags);
}

static int streams_rpc(struct usb_hcd *hcd, struct usb_device *udev,
                       struct usb_host_endpoint **eps, unsigned int num_eps,
                       unsigned int count)
{
    struct usbwarp_hcd *w = hcd_to_warp(hcd);
    struct usbwarp_msg_streams_request msg = { 0 };
    unsigned long flags;
    u32 id;
    int ret;
    unsigned int i;
    if (!usbwarp_is_usb3(hcd) || !(w->negotiated_caps & USBWARP_CAP_STREAMS))
        return -EOPNOTSUPP;
    if (!num_eps || num_eps > 30 || in_interrupt()) return -EINVAL;
    for (i = 0; i < num_eps; ++i) {
        u32 bit = usbwarp_endpoint_mask(eps[i]->desc.bEndpointAddress);
        if (!bit || (msg.endpoint_mask & bit) || !usb_endpoint_xfer_bulk(&eps[i]->desc))
            return -EINVAL;
        msg.endpoint_mask |= bit;
    }
    mutex_lock(&w->config_lock);
    spin_lock_irqsave(&w->dev_lock, flags);
    spin_lock(&w->urb_lock);
    id = usbwarp_find_devid_by_port(hcd, udev->portnum);
    if (!id || w->devices[id - 1].udev != udev || !w->devices[id - 1].connected ||
        w->shutting_down || w->quiescing) {
        ret = -ENODEV;
        goto unlock;
    }
    fill_msg_header(&msg.hdr, USBWARP_MSG_STREAMS_REQUEST, id, next_txn_id(w), sizeof(msg));
    msg.hdr.flags = w->devices[id - 1].epoch;
    msg.num_streams = min(count, USBWARP_MAX_STREAMS);
    reinit_completion(&w->config_done);
    w->config_txn = msg.hdr.transaction_id;
    w->config_device = id;
    w->config_epoch = msg.hdr.flags;
    w->config_status = -EINPROGRESS;
    ret = usbwarp_ring_produce(&w->g2h, &msg, sizeof(msg));
unlock:
    spin_unlock(&w->urb_lock);
    spin_unlock_irqrestore(&w->dev_lock, flags);
    if (!ret) {
        if (!wait_for_completion_timeout(&w->config_done, msecs_to_jiffies(30000))) {
            /* State may have changed on the host. Stop new requests until a
             * session restart rather than guess whether stream handles exist. */
            WRITE_ONCE(w->quiescing, true);
            ret = -ETIMEDOUT;
        } else {
            ret = w->config_status;
            if (!ret && count) {
                ret = w->config_streams;
                if (ret <= 0 || ret > msg.num_streams) ret = -EPROTO;
            }
        }
    }
    spin_lock_irqsave(&w->urb_lock, flags);
    w->config_txn = 0;
    spin_unlock_irqrestore(&w->urb_lock, flags);
    mutex_unlock(&w->config_lock);
    return ret;
}
static int usbwarp_alloc_streams(struct usb_hcd *hcd, struct usb_device *dev,
    struct usb_host_endpoint **eps, unsigned int num_eps, unsigned int count, gfp_t flags)
{
    (void)flags;
    if (!count) return -EINVAL;
    return streams_rpc(hcd, dev, eps, num_eps, count);
}
static int usbwarp_free_streams(struct usb_hcd *hcd, struct usb_device *dev,
    struct usb_host_endpoint **eps, unsigned int num_eps, gfp_t flags)
{
    (void)flags;
    return streams_rpc(hcd, dev, eps, num_eps, 0);
}
