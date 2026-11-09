// SPDX-License-Identifier: GPL-2.0
/*
 * usbwarp_ring.c — SPSC ring operations over MMIO-mapped shared memory.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Device memory ordering:
 *   Producer: copy payload -> wmb -> iowrite32(producer_index).
 *   Consumer: ioread32(producer_index) -> rmb -> copy -> mb -> consumer index.
 * RAM-only smp_* atomics must not be used on __iomem pointers.
 */

#include "usbwarp_guest.h"
#include <linux/io.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Initialisation
 * ═══════════════════════════════════════════════════════════════════════════ */

void usbwarp_ring_init(struct usbwarp_ring *r,
		       struct usbwarp_ring_header __iomem *hdr,
		       void __iomem *data, uint32_t data_size)
{
	r->hdr       = hdr;
	r->data      = data;
    r->doorbell = NULL;
	/* Cache the validated layout, never reread peer-controlled geometry. */
	r->data_size = data_size;
	r->data_mask = data_size - 1;
	spin_lock_init(&r->produce_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Produce  (Guest writes to G2H ring)
 *
 *   Serialized by produce_lock: G2H has multiple producers (urb_enqueue
 *   task/softirq contexts, urb_dequeue and poll thread
 *   control messages).  Without the lock, two producers can read the same
 *   producer_index and one message is silently lost.  The lock must span
 *   data write AND index advance — the index store is the publication.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int ring_write(struct usbwarp_ring *r, const void *msg, uint32_t len, bool raw)
{
	uint32_t aligned = USBWARP_ALIGN_CACHELINE(len);
	uint32_t pi;
	uint32_t ci;
	uint32_t avail, pos, first;
	unsigned long flags;

	if (!len || (!raw && len < sizeof(struct usbwarp_msg_header)) ||
	    len > USBWARP_MAX_MESSAGE_SIZE || aligned >= r->data_size)
		return -EMSGSIZE;

	spin_lock_irqsave(&r->produce_lock, flags);

	pi = ioread32(&r->hdr->producer_index);

	ci = ioread32(&r->hdr->consumer_index);
	rmb();
    if (!usbwarp_ring_state_valid(pi, ci, r->data_mask)) {
        spin_unlock_irqrestore(&r->produce_lock, flags);
        return -EPROTO;
    }

	avail = usbwarp_ring_available(pi, ci, r->data_mask);
	if (avail < aligned) {
		spin_unlock_irqrestore(&r->produce_lock, flags);
		return -ENOSPC;
	}

	pos   = pi & r->data_mask;
	first = r->data_size - pos;

	if (first >= aligned) {
		memcpy_toio(r->data + pos, msg, len);
		if (aligned > len)
			memset_io(r->data + pos + len, 0, aligned - len);
	} else {
		uint32_t tail = len;

		if (first < len) {
			memcpy_toio(r->data + pos, msg, first);
			tail -= first;
		} else {
			memcpy_toio(r->data + pos, msg, len);
			if (first > len)
				memset_io(r->data + pos + len, 0, first - len);
			tail = 0;
		}
		if (tail)
			memcpy_toio(r->data, (const u8 *)msg + first, tail);
		if (aligned > first + tail)
			memset_io(r->data + tail, 0,
				  aligned - first - tail);
	}

	wmb();
	iowrite32(pi + aligned, &r->hdr->producer_index);
    if (r->doorbell) {
        wmb();
        writeq(1, r->doorbell);
    }

	spin_unlock_irqrestore(&r->produce_lock, flags);

	return 0;
}

int usbwarp_ring_produce(struct usbwarp_ring *r, const void *msg, uint32_t len)
{
    return ring_write(r, msg, len, false);
}

int usbwarp_ring_inject_raw(struct usbwarp_ring *r, const void *msg, uint32_t len)
{
    return ring_write(r, msg, len, true);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Consume  (Guest reads from H2G ring)
 *
 *   Local copy into caller buffer prevents TOCTOU from hostile host.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void usbwarp_ring_release(struct usbwarp_ring *r, uint32_t next)
{
    mb();
    iowrite32(next, &r->hdr->consumer_index);
    mb();
    if (r->doorbell && (ioread32(&r->hdr->producer_flags) & USBWARP_RING_FLAG_NEED_SPACE))
        writeq(1, r->doorbell);
}

int usbwarp_ring_consume(struct usbwarp_ring *r,
			 void *msg_out, uint32_t buf_size,
			 uint32_t *msg_len_out)
{
	uint32_t ci = ioread32(&r->hdr->consumer_index);
	uint32_t pi;
	uint32_t used, pos, first;
	struct usbwarp_msg_header hdr_copy;
	uint32_t msg_len, aligned;

    *msg_len_out = 0;

	pi = ioread32(&r->hdr->producer_index);
	rmb();
    if (!usbwarp_ring_state_valid(pi, ci, r->data_mask)) return -EPROTO;

	used = usbwarp_ring_used(pi, ci, r->data_mask);
	if (used < sizeof(struct usbwarp_msg_header))
		return -EAGAIN;  /* empty */

	/* Peek at header — local copy for TOCTOU safety. */
	pos   = ci & r->data_mask;
	first = r->data_size - pos;

	if (first >= sizeof(hdr_copy)) {
		memcpy_fromio(&hdr_copy, r->data + pos, sizeof(hdr_copy));
	} else {
		memcpy_fromio(&hdr_copy, r->data + pos, first);
		memcpy_fromio((u8 *)&hdr_copy + first, r->data,
			      sizeof(hdr_copy) - first);
	}

	if (hdr_copy.magic != USBWARP_MSG_MAGIC)
		return -EPROTO;

	msg_len = hdr_copy.message_length;
	if (msg_len < sizeof(struct usbwarp_msg_header) ||
	    msg_len > USBWARP_MAX_MESSAGE_SIZE || msg_len > r->data_mask)
		return -EPROTO;

	aligned = USBWARP_ALIGN_CACHELINE(msg_len);
	if (aligned > used)
		return -EPROTO;

	if (msg_len > buf_size) {
		/* Skip message to avoid wedging ring. */
        usbwarp_ring_release(r, ci + aligned);
		return -EMSGSIZE;
	}

	/* Copy full message into caller's local buffer. */
	pos   = ci & r->data_mask;
	first = r->data_size - pos;

	if (first >= msg_len) {
		memcpy_fromio(msg_out, r->data + pos, msg_len);
	} else {
		memcpy_fromio(msg_out, r->data + pos, first);
		memcpy_fromio((u8 *)msg_out + first, r->data,
			      msg_len - first);
	}

	/* Reads must finish before the peer can reuse this ring storage. */
    if (memcmp(msg_out, &hdr_copy, sizeof(hdr_copy))) return -EPROTO;
    usbwarp_ring_release(r, ci + aligned);

	*msg_len_out = msg_len;
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Usage query
 * ═══════════════════════════════════════════════════════════════════════════ */

unsigned usbwarp_ring_usage_percent(const struct usbwarp_ring *r)
{
	uint32_t pi = ioread32(&r->hdr->producer_index);
	uint32_t ci = ioread32(&r->hdr->consumer_index);
	uint32_t used = usbwarp_ring_used(pi, ci, r->data_mask);

	if (r->data_size == 0)
		return 0;
	return (unsigned)((uint64_t)used * 100 / r->data_size);
}
