// SPDX-License-Identifier: GPL-2.0
/*
 * usbwarp_buf.c — Data Region buffer bitmap allocator.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Only initialized OUT payload and completed IN bytes are transferred.
 * Slots all belong to the same guest; clearing whole 64-KB slots on every
 * small audio packet wastes MMIO bandwidth without isolating any principals.
 *
 * Multi-slot: a transfer larger than one slot gets a contiguous run.
 * The host-side L5 bounds check validates buffer_offset+length against
 * the whole data region, so a span crossing slot boundaries needs no
 * wire-protocol change.
 */

#include "usbwarp_guest.h"
#include <linux/slab.h>
#include <linux/io.h>

int usbwarp_buf_init(struct usbwarp_hcd *w)
{
	uint32_t count = w->buffer_count;

	w->buf_bitmap = bitmap_zalloc(count, GFP_KERNEL);
	if (!w->buf_bitmap)
		return -ENOMEM;

	bitmap_fill(w->buf_bitmap, count);
	atomic_set(&w->buf_free_count, (int)count);
	spin_lock_init(&w->buf_lock);

	return 0;
}

void usbwarp_buf_cleanup(struct usbwarp_hcd *w)
{
	bitmap_free(w->buf_bitmap);
	w->buf_bitmap = NULL;
}

/* Allocate `count` contiguous free slots.  Returns the base index or
 * -ENOMEM when no run is available (fragmentation is possible — class
 * drivers retry with backoff). */
int usbwarp_buf_alloc_contig(struct usbwarp_hcd *w, uint32_t count)
{
	unsigned long flags;
	unsigned long pos, found;

	if (count == 0 || count > w->buffer_count)
		return -EINVAL;

	if ((uint32_t)atomic_read(&w->buf_free_count) < count)
		return -ENOMEM;

	spin_lock_irqsave(&w->buf_lock, flags);

	found = w->buffer_count;
	pos = 0;
	while (pos + count <= w->buffer_count) {
		unsigned long bit =
			find_next_bit(w->buf_bitmap, w->buffer_count, pos);

		if (bit >= w->buffer_count || bit + count > w->buffer_count)
			break;

		/* First zero inside [bit, bit+count): the run breaks there. */
		{
			unsigned long zero =
				find_next_zero_bit(w->buf_bitmap, bit + count, bit);

			if (zero >= bit + count) {
				found = bit;
				break;
			}
			pos = zero + 1;
		}
	}

	if (found >= w->buffer_count) {
		spin_unlock_irqrestore(&w->buf_lock, flags);
		return -ENOMEM;
	}

	bitmap_clear(w->buf_bitmap, found, count);
	atomic_sub((int)count, &w->buf_free_count);

	spin_unlock_irqrestore(&w->buf_lock, flags);

	return (int)found;
}

void usbwarp_buf_free_range(struct usbwarp_hcd *w, int base, uint32_t count)
{
	unsigned long flags;

	if (base < 0 || count == 0 ||
	    (uint32_t)base + count > w->buffer_count) {
		dev_warn(&w->pdev->dev,
			 "usbwarp: buf_free_range: invalid base=%d count=%u\n",
			 base, count);
		return;
	}

	spin_lock_irqsave(&w->buf_lock, flags);
	bitmap_set(w->buf_bitmap, base, count);
	atomic_add((int)count, &w->buf_free_count);
	spin_unlock_irqrestore(&w->buf_lock, flags);
}
