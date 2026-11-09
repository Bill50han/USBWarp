// SPDX-License-Identifier: GPL-2.0
#include "usbwarp_guest.h"

/* Called with urb_lock. An unlink only asks the host to stop: keep the URB
 * and shared slots until completion or a DMA-safe teardown acknowledgement. */
void usbwarp_send_cancel_locked(struct usbwarp_hcd *w, struct usbwarp_urb_priv *p)
{
    struct usbwarp_msg_urb_cancel msg = { 0 };
    msg.hdr.magic = USBWARP_MSG_MAGIC;
    msg.hdr.message_type = USBWARP_MSG_URB_CANCEL;
    msg.hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
    msg.hdr.message_length = sizeof(msg);
    msg.hdr.device_id = msg.device_id = p->device_id;
    msg.hdr.transaction_id = p->transaction_id;
    msg.hdr.flags = p->epoch;
    msg.hdr.timestamp = ktime_get_ns();
    usbwarp_ring_produce(&w->g2h, &msg, sizeof(msg));
    p->cancel_retry = jiffies + msecs_to_jiffies(100);
}

void usbwarp_retry_cancels(struct usbwarp_hcd *w)
{
    struct usbwarp_urb_priv *p;
    unsigned long flags;
    int bkt;
    spin_lock_irqsave(&w->urb_lock, flags);
    hash_for_each(w->urb_table, bkt, p, hash_node) {
        if (p->state == URB_STATE_CANCELING && !p->queued &&
            time_after_eq(jiffies, p->cancel_retry))
            usbwarp_send_cancel_locked(w, p);
    }
    spin_unlock_irqrestore(&w->urb_lock, flags);
}

static u32 next_txn_id(struct usbwarp_hcd *w)
{
    u32 id;
    do { id = (u32)atomic_inc_return(&w->txn_counter); } while (!id);
    return id;
}

/* A ZLP belongs immediately after its data, before a later URB on the same
 * endpoint/stream. Ordinary bulk URBs remain pipelined; only a ZLP fences later
 * submissions until its zero-length transfer completes. Caller holds urb_lock. */
bool usbwarp_submission_blocked(struct usbwarp_urb_priv *p)
{
    struct urb *earlier;
    list_for_each_entry(earlier, &p->urb->ep->urb_list, urb_list) {
        struct usbwarp_urb_priv *prior;
        if (earlier == p->urb) break;
        if (earlier->stream_id != p->urb->stream_id) continue;
        prior = earlier->hcpriv;
        if (prior && (prior->need_zlp || prior->queued)) return true;
    }
    return false;
}

void usbwarp_publish_queued(struct usbwarp_hcd *w, struct usbwarp_urb_priv *p)
{
    if (!p->queued || w->quiescing || w->shutting_down || usbwarp_debugfs_g2h_paused() || usbwarp_submission_blocked(p)) return;
    if (!usbwarp_ring_produce(&w->g2h, &p->wire_submit, p->wire_submit.hdr.message_length)) {
        p->queued = false;
        atomic_dec(&w->tx_pending);
        p->published_ns = ktime_get_ns();
        p->busy_eligible = usbwarp_rx_submit(w, p->wire_submit.transfer_type,
                                           p->wire_submit.transfer_length);
    }
}

bool usbwarp_stage_zlp(struct usbwarp_hcd *w,
                                     const struct usbwarp_msg_urb_complete *msg)
{
    struct usbwarp_urb_priv *p;
    unsigned long flags;
    bool staged = false;
    spin_lock_irqsave(&w->urb_lock, flags);
    hash_for_each_possible(w->urb_table, p, hash_node, msg->hdr.transaction_id) {
        if (p->transaction_id != msg->hdr.transaction_id || p->device_id != msg->hdr.device_id || p->epoch != msg->hdr.flags)
            continue;
        if (p->need_zlp && !p->zlp_stage && !p->queued && p->state == URB_STATE_ACTIVE &&
            msg->status == USBWARP_STATUS_SUCCESS && msg->device_id == p->device_id &&
            msg->endpoint == usb_pipeendpoint(p->urb->pipe) &&
            msg->data_mode == p->wire_submit.data_mode &&
            msg->actual_length == p->urb->transfer_buffer_length) {
            /* Use a new transaction ID so a late duplicate data completion
             * cannot complete the zero-length stage. Physical data I/O is done. */
            usbwarp_rx_progress(w, p->busy_eligible, p->published_ns, 0);
            hash_del(&p->hash_node);
            p->transaction_id = next_txn_id(w);
            hash_add(w->urb_table, &p->hash_node, p->transaction_id);
            p->wire_submit.hdr.transaction_id = p->transaction_id;
            p->wire_submit.hdr.message_length = USBWARP_MSG_URB_SUBMIT_BASE_SIZE;
            p->wire_submit.transfer_length = 0;
            p->wire_submit.data_mode = USBWARP_DATA_NONE;
            p->wire_submit.buffer_offset = 0;
            p->wire_submit.transfer_flags = 0;
            p->saved_actual = msg->actual_length;
            p->zlp_stage = true;
            p->queued = true;
            p->busy_eligible = false;
            p->published_ns = 0;
            atomic_inc(&w->tx_pending);
            usbwarp_publish_queued(w, p);
            staged = true;
        }
        break;
    }
    spin_unlock_irqrestore(&w->urb_lock, flags);
    return staged;
}
