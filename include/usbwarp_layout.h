/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USBWARP_LAYOUT_H
#define USBWARP_LAYOUT_H
#include "usbwarp_shared.h"

/* Local snapshot, never a wire structure. Validate these exact values before
 * forming pointers; a peer may rewrite every byte of the shared control block. */
struct usbwarp_shm_layout {
    uint32_t total_size;
    uint32_t g2h_offset, g2h_size, h2g_offset, h2g_size;
    uint32_t data_offset, data_size, buffer_size, buffer_count;
};

static inline int usbwarp_layout_valid(const struct usbwarp_shm_layout *l,
                                      uint64_t mapped_size)
{
    uint32_t ring_size;
    uint64_t second, data;
    if (mapped_size < USBWARP_SHM_SIZE_MIN || mapped_size > USBWARP_SHM_SIZE_MAX ||
        l->total_size != mapped_size || !USBWARP_IS_POWER_OF_2(l->total_size) ||
        l->g2h_size <= USBWARP_RING_HEADER_SIZE || l->h2g_size != l->g2h_size ||
        !USBWARP_IS_POWER_OF_2(l->buffer_size) ||
        l->buffer_size < USBWARP_BUFFER_SIZE_MIN || l->buffer_size > USBWARP_BUFFER_SIZE_MAX)
        return 0;
    ring_size = l->g2h_size - USBWARP_RING_HEADER_SIZE;
    if (!USBWARP_IS_POWER_OF_2(ring_size) || ring_size < USBWARP_RING_DATA_SIZE_MIN ||
        ring_size > USBWARP_RING_DATA_SIZE_MAX) return 0;
    second = (uint64_t)USBWARP_CONTROL_BLOCK_SIZE + l->g2h_size;
    data = (second + l->h2g_size + USBWARP_PAGE_SIZE - 1) & ~((uint64_t)USBWARP_PAGE_SIZE - 1);
    return l->g2h_offset == USBWARP_CONTROL_BLOCK_SIZE && l->h2g_offset == second &&
        data < mapped_size && l->data_offset == data && l->data_size == mapped_size - data &&
        l->buffer_count == l->data_size / l->buffer_size &&
        l->buffer_count >= USBWARP_BUFFER_QUOTA_MIN;
}
#endif
