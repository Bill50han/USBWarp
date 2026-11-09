// SPDX-License-Identifier: GPL-2.0
/*
 * usbwarp_poll.c — Adaptive polling kthread and H2G message dispatch.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Both WSL2 targets are 64-bit. Heartbeats use an aligned writeq rather
 * than two independently visible 32-bit writes.
 */

#include "usbwarp_guest.h"
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/task.h>

#define ACTIVE_THRESHOLD   100
#define LIGHT_THRESHOLD   1000
#define LIGHT_SLEEP_US      10
#define IDLE_SLEEP_US     1000


/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Host message validation (issue #3)
 *
 *   Although the Host is generally trusted, defend against Host driver
 *   bugs that could crash the Guest kernel.  Check basic sanity before
 *   dispatching.
 * ═══════════════════════════════════════════════════════════════════════════ */

bool usbwarp_validate_h2g_message(struct usbwarp_hcd *w,
				 const void *msg_buf, uint32_t msg_len)
{
	const struct usbwarp_msg_header *hdr =
		(const struct usbwarp_msg_header *)msg_buf;

	if (msg_len < sizeof(*hdr))
		return false;

	if (hdr->magic != USBWARP_MSG_MAGIC) {
		dev_warn(&w->pdev->dev,
			 "usbwarp: H2G bad magic 0x%x\n", hdr->magic);
		return false;
	}
	if (hdr->protocol_version != USBWARP_PROTOCOL_VERSION)
		return false;

	if (hdr->message_length < sizeof(*hdr) ||
	    hdr->message_length != msg_len) {
		dev_warn(&w->pdev->dev,
			 "usbwarp: H2G bad length %u (buf=%u)\n",
			 hdr->message_length, msg_len);
		return false;
	}

	/* Validate device_id for device-specific messages. */
	if (hdr->message_type == USBWARP_MSG_DEVICE_ADDED ||
	    hdr->message_type == USBWARP_MSG_DEVICE_REMOVED ||
	    hdr->message_type == USBWARP_MSG_URB_COMPLETE ||
	    hdr->message_type == USBWARP_MSG_ISO_COMPLETE) {
		if (hdr->device_id > USBWARP_MAX_DEVICES_LIMIT) {
			dev_warn(&w->pdev->dev,
				 "usbwarp: H2G device_id OOB %u\n",
				 hdr->device_id);
			return false;
		}
	}

	/* ISO_COMPLETE carries a variable results array — its declared
	 * length must fit in the received message. */
	if (hdr->message_type == USBWARP_MSG_ISO_COMPLETE) {
		const struct usbwarp_msg_iso_complete *ic =
			(const struct usbwarp_msg_iso_complete *)msg_buf;
		if (!usbwarp_iso_complete_valid(ic, msg_len)) {
			return false;
		}
	}

	return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Message dispatch
 * ═══════════════════════════════════════════════════════════════════════════ */

void usbwarp_dispatch_message(struct usbwarp_hcd *w,
			     const void *msg_buf, uint32_t msg_len)
{
	const struct usbwarp_msg_header *hdr =
		(const struct usbwarp_msg_header *)msg_buf;

	switch (hdr->message_type) {

	case USBWARP_MSG_NEGOTIATE_RESP: {
		const struct usbwarp_msg_negotiate_resp *resp =
			(const struct usbwarp_msg_negotiate_resp *)msg_buf;
		if (msg_len < sizeof(*resp))
			break;
		if (resp->status == 0) {
			w->negotiated_version = resp->negotiated_version;
			w->negotiated_caps    = resp->capabilities;
			w->max_pending_urbs   = resp->max_pending_urbs;
			if (w->max_pending_urbs == 0)
				w->max_pending_urbs = USBWARP_PENDING_URBS_DEFAULT;
			w->negotiation_done   = true;
			iowrite32(USBWARP_STATE_RUNNING, &w->cb->guest_state);
			dev_info(&w->pdev->dev,
				 "usbwarp: negotiation OK ver=%u.%u caps=0x%x pending=%u devs=%u\n",
				 USBWARP_VERSION_MAJOR(resp->negotiated_version),
				 USBWARP_VERSION_MINOR(resp->negotiated_version),
				 resp->capabilities,
				 resp->max_pending_urbs,
				 resp->max_devices);
		} else {
			dev_err(&w->pdev->dev,
				"usbwarp: negotiation REJECTED status=%d\n",
				resp->status);
		}
		break;
	}

	case USBWARP_MSG_DEVICE_ADDED: {
		const struct usbwarp_msg_device_added *da =
			(const struct usbwarp_msg_device_added *)msg_buf;
		if (msg_len < sizeof(*da))
			break;
		usbwarp_handle_device_added(w, da);
		break;
	}

	case USBWARP_MSG_DEVICE_REMOVED: {
		const struct usbwarp_msg_device_removed *dr =
			(const struct usbwarp_msg_device_removed *)msg_buf;
		if (msg_len < sizeof(*dr))
			break;
		usbwarp_handle_device_removed(w, dr);
		break;
	}

	case USBWARP_MSG_URB_COMPLETE: {
		const struct usbwarp_msg_urb_complete *uc =
			(const struct usbwarp_msg_urb_complete *)msg_buf;
		if (msg_len < USBWARP_MSG_URB_COMPLETE_BASE_SIZE)
			break;
		usbwarp_complete_urb(w, uc);
		break;
	}

	case USBWARP_MSG_ISO_COMPLETE: {
		const struct usbwarp_msg_iso_complete *ic =
			(const struct usbwarp_msg_iso_complete *)msg_buf;
		if (msg_len < USBWARP_MSG_ISO_COMPLETE_BASE_SIZE)
			break;
		usbwarp_complete_iso_urb(w, ic, msg_len);
		break;
	}

    case USBWARP_MSG_STREAMS_RESPONSE:
        if (msg_len == sizeof(struct usbwarp_msg_streams_response))
            usbwarp_complete_streams(w, msg_buf);
        break;

	case USBWARP_MSG_URB_CANCEL_ACK:
		break;

	case USBWARP_MSG_SHUTDOWN_ACK:
		if (READ_ONCE(w->quiescing) && msg_len >= sizeof(struct usbwarp_msg_shutdown_ack))
			complete(&w->shutdown_ack);
		break;

	case USBWARP_MSG_HEARTBEAT:
		break;

	case USBWARP_MSG_HOST_SHUTDOWN: {
		const struct usbwarp_msg_shutdown *sd =
			(const struct usbwarp_msg_shutdown *)msg_buf;
		dev_warn(&w->pdev->dev,
			 "usbwarp: host shutdown reason=%u\n",
			 (msg_len >= sizeof(*sd)) ? sd->reason : 0xFFu);
		usbwarp_shutdown_all_devices(w, USBWARP_REMOVE_HOST_SHUTDOWN);

        /* Free the MSI-X mapping while the HDV callbacks still exist. Once
         * ACK is published the host may destroy its emulation endpoint. */
        usbwarp_irq_cleanup(w);

		{
			struct usbwarp_msg_shutdown_ack ack;

			memset(&ack, 0, sizeof(ack));
			ack.hdr.magic            = USBWARP_MSG_MAGIC;
			ack.hdr.message_type     = USBWARP_MSG_SHUTDOWN_ACK;
			ack.hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
			ack.hdr.message_length   = sizeof(ack);
			ack.hdr.timestamp        = (uint64_t)ktime_get_ns();
			usbwarp_ring_produce(&w->g2h, &ack, sizeof(ack));
		}
        usbwarp_retire_hcd(w);
		break;
	}

	case USBWARP_MSG_CIRCUIT_BREAKER_TRIP:
		dev_warn(&w->pdev->dev,
			 "usbwarp: circuit breaker trip dev=%u\n",
			 hdr->device_id);
		break;

	default:
		dev_dbg(&w->pdev->dev,
			"usbwarp: unknown H2G type %u\n",
			hdr->message_type);
		break;
	}
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Poll thread main function
 * ═══════════════════════════════════════════════════════════════════════════ */

#define POLL_MSG_BUF_SIZE  USBWARP_MAX_MESSAGE_SIZE

static int usbwarp_poll_fn(void *data)
{
	struct usbwarp_hcd *w = data;
	uint8_t *msg_buf;
	uint64_t heartbeat_interval_ns = 1000000000ULL; /* 1 s */
	uint64_t last_heartbeat_ns = 0;
    u64 host_tick = 0, host_progress_ns = ktime_get_ns();
    u64 last_host_check_ns = host_progress_ns;
	unsigned long next_cancel_scan = jiffies;

    msg_buf = w->poll_buffer;

	w->poll_state = POLL_BUSY;
	w->idle_count = 0;

	dev_info(&w->pdev->dev, "usbwarp: poll thread started\n");

	while (!kthread_should_stop() && !READ_ONCE(w->shutting_down)) {

        bool got_msg;
        usbwarp_irq_step(w);
        got_msg = usbwarp_rx_worker(w);

		usbwarp_flush_queued(w);
		if (time_after_eq(jiffies, next_cancel_scan)) {
			usbwarp_retry_cancels(w);
			next_cancel_scan = jiffies + max(1UL, msecs_to_jiffies(10));
		}

        if (usbwarp_rx_busy_poll(w)) got_msg = true;
		/* Streaming completion must not fall back to millisecond sleeps. */
		if ((atomic_read(&w->iso_pending) || atomic_read(&w->tx_pending)) && !got_msg) {
			w->poll_state = POLL_LIGHT;
			w->idle_count = 0;
		}
		/* Adaptive poll state machine. */
		if (got_msg) {
			cond_resched();
			w->poll_state = POLL_BUSY;
			w->idle_count = 0;
		} else if (usbwarp_irq_wait(w)) {
            w->idle_count = 0;
		} else if (READ_ONCE(usbwarp_poll_us)) {
            unsigned int interval = READ_ONCE(usbwarp_poll_us);
            if (atomic_read(&w->iso_pending)) interval = min(interval, 10u);
            /* A concurrent write of zero selects adaptive on the next pass. */
            if (interval) usleep_range(interval, interval + max(1u, interval / 4));
            w->poll_state = POLL_BUSY;
            w->idle_count = 0;
		} else {
			w->idle_count++;

			switch (w->poll_state) {
			case POLL_BUSY:
				if (w->idle_count >= ACTIVE_THRESHOLD)
					w->poll_state = POLL_ACTIVE;
				else
					cpu_relax();
				break;

			case POLL_ACTIVE:
				if (w->idle_count >= LIGHT_THRESHOLD)
					w->poll_state = POLL_LIGHT;
				else
					cond_resched();
				break;

			case POLL_LIGHT:
				usleep_range(LIGHT_SLEEP_US,
					     LIGHT_SLEEP_US * 2);
				if (w->idle_count >= LIGHT_THRESHOLD * 10)
					w->poll_state = POLL_IDLE;
				break;

			case POLL_IDLE:
				usleep_range(IDLE_SLEEP_US,
					     IDLE_SLEEP_US * 2);
				break;
			}
		}

		/* Periodic heartbeat (issue #10 fix: atomic 64-bit write).
		 * Skip when G2H is paused for fuzz injection — the fuzz
		 * injector becomes the sole G2H producer (SPSC invariant). */
		{
			uint64_t now = (uint64_t)ktime_get_ns();

            if (!READ_ONCE(w->shutting_down) && now - last_host_check_ns >= heartbeat_interval_ns) {
                u64 tick = readq(&w->cb->host_heartbeat_ts);
                /* A debugger or VM pause stops this observer too. Start a
                 * fresh observation window when we resume, not a false loss. */
                if (now - last_host_check_ns > 3 * heartbeat_interval_ns)
                    host_progress_ns = now;
                last_host_check_ns = now;
                if (tick != host_tick) {
                    host_tick = tick;
                    host_progress_ns = now;
                } else if (now - host_progress_ns > 10000000000ULL) {
                    dev_warn(&w->pdev->dev, "usbwarp: host heartbeat lost; retiring session\n");
                    iowrite32(USBWARP_STATE_ERROR, &w->cb->guest_state);
                    usbwarp_shutdown_all_devices(w, USBWARP_REMOVE_HOST_SHUTDOWN);
                    usbwarp_retire_hcd(w);
                }
            }

			if (!READ_ONCE(w->shutting_down) &&
			    !READ_ONCE(w->quiescing) &&
			    now - last_heartbeat_ns >= heartbeat_interval_ns &&
			    !usbwarp_debugfs_g2h_paused()) {
				usbwarp_send_heartbeat(w);

				writeq(now, &w->cb->guest_heartbeat_ts);

				iowrite32(w->negotiation_done ? USBWARP_STATE_RUNNING : USBWARP_STATE_READY,
					  (void __iomem *)&w->cb->guest_state);

				last_heartbeat_ns = now;
			}
		}
	}

    usbwarp_rx_stop(w);
    usbwarp_irq_restore(w);
	kfree(w->rx_buffer);
    w->rx_buffer = NULL;
	kfree(msg_buf);
    w->poll_buffer = NULL;
	dev_info(&w->pdev->dev, "usbwarp: poll thread stopped\n");
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Start / stop
 * ═══════════════════════════════════════════════════════════════════════════ */

int usbwarp_poll_start(struct usbwarp_hcd *w)
{
    w->poll_buffer = kmalloc(POLL_MSG_BUF_SIZE, GFP_KERNEL);
    if (!w->poll_buffer) return -ENOMEM;
    w->rx_buffer = kmalloc(POLL_MSG_BUF_SIZE, GFP_KERNEL);
    if (!w->rx_buffer) { kfree(w->poll_buffer); w->poll_buffer = NULL; return -ENOMEM; }
	w->poll_thread = kthread_create(usbwarp_poll_fn, w, "usbwarp-poll");
	if (IS_ERR(w->poll_thread)) {
		int err = PTR_ERR(w->poll_thread);
		w->poll_thread = NULL;
        kfree(w->poll_buffer);
        w->poll_buffer = NULL;
        kfree(w->rx_buffer);
        w->rx_buffer = NULL;
		return err;
	}
    // The poller may exit on host shutdown before the PCI remove callback.
    get_task_struct(w->poll_thread);
    wake_up_process(w->poll_thread);
	return 0;
}

void usbwarp_poll_stop(struct usbwarp_hcd *w)
{
	if (w->poll_thread) {
		WRITE_ONCE(w->shutting_down, true);
		kthread_stop(w->poll_thread);
        put_task_struct(w->poll_thread);
		w->poll_thread = NULL;
	}
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Protocol negotiation
 * ═══════════════════════════════════════════════════════════════════════════ */

int usbwarp_send_negotiate(struct usbwarp_hcd *w)
{
	struct usbwarp_msg_negotiate neg;

	memset(&neg, 0, sizeof(neg));

	neg.hdr.magic            = USBWARP_MSG_MAGIC;
	neg.hdr.message_type     = USBWARP_MSG_NEGOTIATE;
	neg.hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
	neg.hdr.message_length   = sizeof(neg);
	neg.hdr.timestamp        = (uint64_t)ktime_get_ns();

	neg.min_version      = USBWARP_PROTOCOL_VERSION;
	neg.max_version      = USBWARP_PROTOCOL_VERSION;
	neg.max_pending_urbs = usbwarp_max_pending;

	neg.capabilities = USBWARP_CAP_INLINE_DATA |
			   USBWARP_CAP_BATCH_SUBMIT |
			   USBWARP_CAP_STATS | USBWARP_CAP_USB3 | USBWARP_CAP_STREAMS | USBWARP_CAP_DEVICE_EPOCH;

	if (usbwarp_enable_iso)
		neg.capabilities |= USBWARP_CAP_ISO_TRANSFER | USBWARP_CAP_ISO_PACKED;
    if (w->g2h.doorbell) neg.capabilities |= USBWARP_CAP_G2H_DOORBELL;

	return usbwarp_ring_produce(&w->g2h, &neg, sizeof(neg));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Heartbeat
 * ═══════════════════════════════════════════════════════════════════════════ */

void usbwarp_send_heartbeat(struct usbwarp_hcd *w)
{
	struct usbwarp_msg_heartbeat hb;

	memset(&hb, 0, sizeof(hb));

	hb.hdr.magic            = USBWARP_MSG_MAGIC;
	hb.hdr.message_type     = USBWARP_MSG_HEARTBEAT;
	hb.hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
	hb.hdr.message_length   = sizeof(hb);
	hb.hdr.timestamp        = (uint64_t)ktime_get_ns();

	hb.sender_timestamp  = (uint64_t)ktime_get_ns();
	hb.sender_state      = USBWARP_STATE_RUNNING;
	hb.pending_urb_count = (uint32_t)atomic_read(&w->total_pending);

	usbwarp_ring_produce(&w->g2h, &hb, sizeof(hb));
}

/* Wait for physical I/O to stop before discarding pending URBs or the bitmap.
 * Polling continues during this handshake to deliver completions and the ACK. */
void usbwarp_guest_disconnect(struct usbwarp_hcd *w)
{
    struct usbwarp_msg_shutdown msg = { 0 };
    unsigned long flags, deadline;
    bool sent = false, drained = false;
    if (READ_ONCE(w->shutting_down) || !w->negotiation_done)
        return;
    spin_lock_irqsave(&w->urb_lock, flags);
    w->quiescing = true;
    spin_unlock_irqrestore(&w->urb_lock, flags);
    msg.hdr.magic = USBWARP_MSG_MAGIC;
    msg.hdr.message_type = USBWARP_MSG_GUEST_SHUTDOWN;
    msg.hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
    msg.hdr.message_length = sizeof(msg);
    msg.reason = USBWARP_SHUTDOWN_NORMAL;
    deadline = jiffies + msecs_to_jiffies(1000);
    do {
        if (!usbwarp_ring_produce(&w->g2h, &msg, sizeof(msg))) { sent = true; break; }
        msleep(1);
    } while (time_before(jiffies, deadline));
    if (sent)
        drained = wait_for_completion_timeout(&w->shutdown_ack, msecs_to_jiffies(10000)) != 0;
    iowrite32(drained ? USBWARP_STATE_STOPPED : USBWARP_STATE_ERROR, &w->cb->guest_state);
    if (!drained)
        dev_warn(&w->pdev->dev, "usbwarp: host drain not acknowledged; restart service before reloading\n");
}
