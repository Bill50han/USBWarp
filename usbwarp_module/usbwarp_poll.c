// SPDX-License-Identifier: GPL-2.0
/*
 * usbwarp_poll.c — Adaptive polling kthread and H2G message dispatch.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * FIX (issue #10): 64-bit guest_heartbeat_ts written as two 32-bit
 *   stores with smp_wmb in between (low word first) to prevent tear
 *   on 32-bit or non-atomic-64-bit platforms.
 *
 * FIX (issue #3): Basic validation of Host messages before dispatch.
 */

#include "usbwarp_guest.h"
#include <linux/delay.h>
#include <linux/sched.h>

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

static bool validate_h2g_message(struct usbwarp_hcd *w,
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

	if (hdr->message_length < sizeof(*hdr) ||
	    hdr->message_length > msg_len) {
		dev_warn(&w->pdev->dev,
			 "usbwarp: H2G bad length %u (buf=%u)\n",
			 hdr->message_length, msg_len);
		return false;
	}

	/* Validate device_id for device-specific messages. */
	if (hdr->message_type == USBWARP_MSG_DEVICE_ADDED ||
	    hdr->message_type == USBWARP_MSG_DEVICE_REMOVED ||
	    hdr->message_type == USBWARP_MSG_URB_COMPLETE) {
		if (hdr->device_id > USBWARP_MAX_PORTS) {
			dev_warn(&w->pdev->dev,
				 "usbwarp: H2G device_id OOB %u\n",
				 hdr->device_id);
			return false;
		}
	}

	return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Message dispatch
 * ═══════════════════════════════════════════════════════════════════════════ */

static void dispatch_message(struct usbwarp_hcd *w,
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

	case USBWARP_MSG_URB_CANCEL_ACK:
		break;

	case USBWARP_MSG_HEARTBEAT:
		break;

	case USBWARP_MSG_HOST_SHUTDOWN: {
		const struct usbwarp_msg_shutdown *sd =
			(const struct usbwarp_msg_shutdown *)msg_buf;
		dev_warn(&w->pdev->dev,
			 "usbwarp: host shutdown reason=%u\n",
			 (msg_len >= sizeof(*sd)) ? sd->reason : 0xFFu);
		w->shutting_down = true;

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

#define POLL_MSG_BUF_SIZE  4096

static int usbwarp_poll_fn(void *data)
{
	struct usbwarp_hcd *w = data;
	uint8_t *msg_buf;
	uint64_t heartbeat_interval_ns = 1000000000ULL; /* 1 s */
	uint64_t last_heartbeat_ns = 0;

	msg_buf = kmalloc(POLL_MSG_BUF_SIZE, GFP_KERNEL);
	if (!msg_buf) {
		dev_err(&w->pdev->dev,
			"usbwarp: poll thread: cannot alloc msg buffer\n");
		return -ENOMEM;
	}

	w->poll_state = POLL_BUSY;
	w->idle_count = 0;

	dev_info(&w->pdev->dev, "usbwarp: poll thread started\n");

	while (!kthread_should_stop() && !w->shutting_down) {

		uint32_t msg_len = 0;
		int ret;
		bool got_msg = false;

		ret = usbwarp_ring_consume(&w->h2g, msg_buf,
					   POLL_MSG_BUF_SIZE, &msg_len);
		if (ret == 0 && msg_len > 0) {
			/* Validate before dispatch (issue #3). */
			if (validate_h2g_message(w, msg_buf, msg_len))
				dispatch_message(w, msg_buf, msg_len);
			got_msg = true;
		}

		/* Adaptive poll state machine. */
		if (got_msg) {
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

		/* Periodic heartbeat (issue #10 fix: atomic 64-bit write). */
		{
			uint64_t now = (uint64_t)ktime_get_ns();

			if (now - last_heartbeat_ns >= heartbeat_interval_ns) {
				usbwarp_send_heartbeat(w);

				/* Write 64-bit guest_heartbeat_ts as two
				 * 32-bit stores: low word first, barrier,
				 * then high word.  This prevents the Host
				 * from seeing a torn timestamp. */
				iowrite32((u32)(now & 0xFFFFFFFF),
					  (void __iomem *)&w->cb->guest_heartbeat_ts);
				smp_wmb();
				iowrite32((u32)(now >> 32),
					  (void __iomem *)((u8 __iomem *)&w->cb->guest_heartbeat_ts + 4));

				iowrite32(USBWARP_STATE_RUNNING,
					  (void __iomem *)&w->cb->guest_state);

				last_heartbeat_ns = now;
			}
		}
	}

	kfree(msg_buf);
	dev_info(&w->pdev->dev, "usbwarp: poll thread stopped\n");
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Start / stop
 * ═══════════════════════════════════════════════════════════════════════════ */

int usbwarp_poll_start(struct usbwarp_hcd *w)
{
	w->poll_thread = kthread_run(usbwarp_poll_fn, w, "usbwarp-poll");
	if (IS_ERR(w->poll_thread)) {
		int err = PTR_ERR(w->poll_thread);
		w->poll_thread = NULL;
		return err;
	}
	return 0;
}

void usbwarp_poll_stop(struct usbwarp_hcd *w)
{
	if (w->poll_thread) {
		kthread_stop(w->poll_thread);
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
			   USBWARP_CAP_STATS;

	if (usbwarp_enable_iso)
		neg.capabilities |= USBWARP_CAP_ISO_TRANSFER;

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
