/*
 * shm_init.cpp — Shared memory Section creation and layout initialisation.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * FIX #3: shm_size rounded up to page boundary before Section creation.
 * FIX #8: Assert section is NULL before creating (prevent handle leak).
 */

#include "warp_service.h"
#include <stdio.h>
#include <string.h>

static void InitRingHeader(volatile struct usbwarp_ring_header *rh,
                           uint32_t data_size, uint32_t max_msg)
{
    memset((void *)rh, 0, USBWARP_RING_HEADER_SIZE);
    rh->producer_index   = 0;
    rh->consumer_index   = 0;
    rh->data_size        = data_size;
    rh->data_size_mask   = data_size - 1;
    rh->max_message_size = max_msg;
}

BOOL ShmCreate(struct warp_session *s)
{
    /* FIX #8: Prevent double-create / handle leak. */
    if (s->section != NULL || s->shm_base != NULL) {
        LogError("ShmCreate called but section already exists — destroying first");
        ShmDestroy(s);
    }

    const uint32_t ring_data = s->cfg.ring_data_size;
    const uint32_t buf_size  = s->cfg.buffer_size;
    uint32_t       total     = s->cfg.shm_size;

    if (!USBWARP_IS_POWER_OF_2(ring_data) ||
        ring_data < USBWARP_RING_DATA_SIZE_MIN ||
        ring_data > USBWARP_RING_DATA_SIZE_MAX) {
        LogError("ring_data_size 0x%X invalid", ring_data);
        return FALSE;
    }
    if (!USBWARP_IS_POWER_OF_2(buf_size) ||
        buf_size < USBWARP_BUFFER_SIZE_MIN ||
        buf_size > USBWARP_BUFFER_SIZE_MAX) {
        LogError("buffer_size 0x%X invalid", buf_size);
        return FALSE;
    }
    if (total < USBWARP_SHM_SIZE_MIN || total > USBWARP_SHM_SIZE_MAX) {
        LogError("shm_size %u out of range", total);
        return FALSE;
    }

    /* FIX #3: Round up to page boundary.  Ensures Host and Guest views
     * are identical and prevents partial-page mapping issues.           */
    total = USBWARP_ALIGN_PAGE(total);

    /* P1#4: PCI BAR sizes MUST be powers of 2.  Round up to the next
     * power of 2 if not already.  Without this, bar0_mask computation
     * produces a wrong value and Guest BAR enumeration fails.         */
    {
        uint32_t p2 = 1;
        while (p2 < total && p2 != 0)
            p2 <<= 1;
        if (p2 == 0) {
            LogError("SHM size overflow during power-of-2 rounding");
            return FALSE;
        }
        if (p2 != total) {
            LogInfo("SHM size rounded up from 0x%X to 0x%X (power-of-2 for PCI BAR)",
                    total, p2);
            total = p2;
        }
    }

    if (total != s->cfg.shm_size) {
        LogInfo("SHM size adjusted: requested=0x%X actual=0x%X",
                s->cfg.shm_size, total);
    }

    const uint32_t ring_total = USBWARP_RING_HEADER_SIZE + ring_data;
    const uint32_t g2h_off    = USBWARP_CONTROL_BLOCK_SIZE;
    const uint32_t h2g_off    = g2h_off + ring_total;
    const uint32_t dr_off     = USBWARP_ALIGN_PAGE(h2g_off + ring_total);

    if (dr_off >= total) {
        LogError("SHM too small: need 0x%X, have 0x%X", dr_off, total);
        return FALSE;
    }

    const uint32_t dr_size = total - dr_off;
    const uint32_t buf_cnt = dr_size / buf_size;

    if (buf_cnt < USBWARP_BUFFER_QUOTA_MIN) {
        LogError("Only %u buffers, need >= %u", buf_cnt, USBWARP_BUFFER_QUOTA_MIN);
        return FALSE;
    }

    LogInfo("SHM layout: total=%uMB  g2h@0x%05X  h2g@0x%05X  data@0x%05X  "
            "ring_data=%uKB  buf=%uKB x%u",
            total / (1024*1024), g2h_off, h2g_off, dr_off,
            ring_data / 1024, buf_size / 1024, buf_cnt);

    /* Create NT Section (page-file backed). */
    s->section = CreateFileMappingW(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, total, NULL);

    if (!s->section) {
        LogError("CreateFileMappingW failed: %lu", GetLastError());
        return FALSE;
    }

    s->shm_base = MapViewOfFile(s->section, FILE_MAP_ALL_ACCESS, 0, 0, total);
    if (!s->shm_base) {
        LogError("MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(s->section);
        s->section = NULL;
        return FALSE;
    }

    memset(s->shm_base, 0, total);

    /* Write Control Block. */
    volatile struct usbwarp_control_block *cb =
        (volatile struct usbwarp_control_block *)s->shm_base;

    cb->magic                   = USBWARP_CONTROL_BLOCK_MAGIC;
    cb->protocol_version_major  = USBWARP_PROTOCOL_VERSION_MAJOR;
    cb->protocol_version_minor  = USBWARP_PROTOCOL_VERSION_MINOR;
    cb->shm_total_size          = total;
    cb->flags                   = s->cfg.enable_iso ? USBWARP_FLAG_ISO_ENABLED : 0;
    if (s->cfg.debug) cb->flags |= USBWARP_FLAG_DEBUG_ENABLED;
    cb->g2h_ring_offset         = g2h_off;
    cb->g2h_ring_size           = ring_total;
    cb->h2g_ring_offset         = h2g_off;
    cb->h2g_ring_size           = ring_total;
    cb->data_region_offset      = dr_off;
    cb->data_region_size        = dr_size;
    cb->buffer_size             = buf_size;
    cb->buffer_count            = buf_cnt;
    cb->host_state              = USBWARP_STATE_INIT;

    /* Initialise Ring headers. */
    uint8_t *base = (uint8_t *)s->shm_base;
    volatile struct usbwarp_ring_header *g2h =
        (volatile struct usbwarp_ring_header *)(base + g2h_off);
    volatile struct usbwarp_ring_header *h2g =
        (volatile struct usbwarp_ring_header *)(base + h2g_off);
    const uint32_t max_msg = 4096;
    InitRingHeader(g2h, ring_data, max_msg);
    InitRingHeader(h2g, ring_data, max_msg);

    /* Populate session pointers. */
    s->cb              = cb;
    s->g2h_ring        = g2h;
    s->h2g_ring        = h2g;
    s->g2h_ring_data   = (volatile uint8_t *)g2h + USBWARP_RING_HEADER_SIZE;
    s->h2g_ring_data   = (volatile uint8_t *)h2g + USBWARP_RING_HEADER_SIZE;
    s->data_region     = (volatile uint8_t *)(base + dr_off);
    s->data_region_size = dr_size;
    s->buffer_count     = buf_cnt;
    s->shm_size         = total;

    LogInfo("SHM created: section=%p  base=%p  cb.magic=0x%08X",
            s->section, s->shm_base, cb->magic);
    return TRUE;
}

void ShmDestroy(struct warp_session *s)
{
    if (s->shm_base) { UnmapViewOfFile(s->shm_base); s->shm_base = NULL; }
    if (s->section)  { CloseHandle(s->section);       s->section  = NULL; }
    s->cb = NULL;
    s->g2h_ring = NULL;
    s->h2g_ring = NULL;
    s->data_region = NULL;
}
