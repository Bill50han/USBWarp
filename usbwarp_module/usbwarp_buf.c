// SPDX-License-Identifier: GPL-2.0
/*
 * usbwarp_buf.c — Data Region buffer bitmap allocator.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Zero-on-free: spec §04 §3.2 (cache hotness), spec §05 §6 (leak prevention).
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

int usbwarp_buf_alloc(struct usbwarp_hcd *w)
{
	unsigned long idx;
	unsigned long flags;

	if (atomic_read(&w->buf_free_count) <= 0)
		return -ENOMEM;

	spin_lock_irqsave(&w->buf_lock, flags);

	idx = find_first_bit(w->buf_bitmap, w->buffer_count);
	if (idx >= w->buffer_count) {
		spin_unlock_irqrestore(&w->buf_lock, flags);
		return -ENOMEM;
	}

	clear_bit(idx, w->buf_bitmap);
	atomic_dec(&w->buf_free_count);

	spin_unlock_irqrestore(&w->buf_lock, flags);

	return (int)idx;
}

void usbwarp_buf_free(struct usbwarp_hcd *w, int idx)
{
	if (idx < 0 || (uint32_t)idx >= w->buffer_count) {
		dev_warn(&w->pdev->dev,
			 "usbwarp: buf_free: invalid index %d\n", idx);
		return;
	}

	memset_io(usbwarp_buf_addr(w, idx), 0, w->buffer_size);
	smp_wmb();

	set_bit((unsigned long)idx, w->buf_bitmap);
	atomic_inc(&w->buf_free_count);
}
