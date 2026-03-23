// SPDX-License-Identifier: GPL-2.0
/*
 * usbwarp_ring.c — SPSC ring operations over MMIO-mapped shared memory.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Memory ordering:
 *   Producer:  write data → smp_wmb → store-release producer_index
 *   Consumer:  load-acquire producer_index → read data → store-release consumer_index
 *
 * The smp_load_acquire / smp_store_release pairs provide all needed ordering.
 * No additional smp_rmb is needed between the data read and consumer_index
 * update because store-release already includes a full barrier on the
 * consumer side.
 */

#include "usbwarp_guest.h"
#include <linux/io.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Initialisation
 * ═══════════════════════════════════════════════════════════════════════════ */

void usbwarp_ring_init(struct usbwarp_ring *r,
		       struct usbwarp_ring_header __iomem *hdr,
		       void __iomem *data)
{
	r->hdr       = hdr;
	r->data      = data;
	r->data_size = ioread32(&hdr->data_size);
	r->data_mask = ioread32(&hdr->data_size_mask);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Produce  (Guest writes to G2H ring)
 * ═══════════════════════════════════════════════════════════════════════════ */

int usbwarp_ring_produce(struct usbwarp_ring *r,
			 const void *msg, uint32_t len)
{
	uint32_t aligned = USBWARP_ALIGN_CACHELINE(len);
	uint32_t pi      = ioread32(&r->hdr->producer_index);
	uint32_t ci;
	uint32_t avail, pos, first;

	ci = smp_load_acquire((const uint32_t __force *)&r->hdr->consumer_index);

	avail = usbwarp_ring_available(pi, ci, r->data_mask);
	if (avail < aligned)
		return -ENOSPC;

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

	smp_wmb();

	smp_store_release((uint32_t __force *)&r->hdr->producer_index,
			  pi + aligned);

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Consume  (Guest reads from H2G ring)
 *
 *   Local copy into caller buffer prevents TOCTOU from hostile host.
 * ═══════════════════════════════════════════════════════════════════════════ */

int usbwarp_ring_consume(struct usbwarp_ring *r,
			 void *msg_out, uint32_t buf_size,
			 uint32_t *msg_len_out)
{
	uint32_t ci = ioread32(&r->hdr->consumer_index);
	uint32_t pi;
	uint32_t used, pos, first;
	struct usbwarp_msg_header hdr_copy;
	uint32_t msg_len, aligned;

	pi = smp_load_acquire((const uint32_t __force *)&r->hdr->producer_index);

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
	if (msg_len < sizeof(struct usbwarp_msg_header))
		return -EPROTO;

	aligned = USBWARP_ALIGN_CACHELINE(msg_len);
	if (aligned > used)
		return -EPROTO;

	if (msg_len > buf_size) {
		/* Skip message to avoid wedging ring. */
		smp_store_release(
			(uint32_t __force *)&r->hdr->consumer_index,
			ci + aligned);
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

	/* Advance consumer_index.  smp_store_release provides the needed
	 * ordering: all reads above are visible before the index update.
	 * No separate smp_rmb is needed. */
	smp_store_release((uint32_t __force *)&r->hdr->consumer_index,
			  ci + aligned);

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
