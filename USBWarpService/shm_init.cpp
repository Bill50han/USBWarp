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
        LogError("ShmCreate called while a section is still owned by the session");
        SetLastError(ERROR_INVALID_STATE);
        return FALSE;
    }

    const uint32_t ring_data = s->cfg.ring_data_size;
    const uint32_t buf_size  = s->cfg.buffer_size;
    uint32_t       total     = s->cfg.shm_size;

    if (!USBWARP_IS_POWER_OF_2(ring_data) ||
        ring_data < USBWARP_RING_DATA_SIZE_MIN ||
        ring_data > USBWARP_RING_DATA_SIZE_MAX) {
        LogError("ring_data_size 0x%X invalid", ring_data);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!USBWARP_IS_POWER_OF_2(buf_size) ||
        buf_size < USBWARP_BUFFER_SIZE_MIN ||
        buf_size > USBWARP_BUFFER_SIZE_MAX) {
        LogError("buffer_size 0x%X invalid", buf_size);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (total < USBWARP_SHM_SIZE_MIN || total > USBWARP_SHM_SIZE_MAX) {
        LogError("shm_size %u out of range", total);
        SetLastError(ERROR_INVALID_PARAMETER);
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
            SetLastError(ERROR_ARITHMETIC_OVERFLOW);
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
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    const uint32_t dr_size = total - dr_off;
    const uint32_t buf_cnt = dr_size / buf_size;

    if (buf_cnt < USBWARP_BUFFER_QUOTA_MIN) {
        LogError("Only %u buffers, need >= %u", buf_cnt, USBWARP_BUFFER_QUOTA_MIN);
        SetLastError(ERROR_INVALID_PARAMETER);
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
        DWORD error = GetLastError();
        LogError("MapViewOfFile failed: %lu", error);
        CloseHandle(s->section);
        s->section = NULL;
        SetLastError(error);
        return FALSE;
    }

    memset(s->shm_base, 0, total);
    if ((s->driverCaps & USBWARP_DRIVER_CAP_G2H_DOORBELL) &&
        s->api.HdvRegisterDoorbell && s->api.HdvUnregisterDoorbell) {
        s->doorbellEvent = CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (!s->doorbellEvent) {
            DWORD error=GetLastError(); ShmDestroy(s); SetLastError(error); return FALSE;
        }
    }

    /* Write Control Block. */
    volatile struct usbwarp_control_block *cb =
        (volatile struct usbwarp_control_block *)s->shm_base;

    cb->magic                   = USBWARP_CONTROL_BLOCK_MAGIC;
    cb->protocol_version_major  = USBWARP_PROTOCOL_VERSION_MAJOR;
    cb->protocol_version_minor  = USBWARP_PROTOCOL_VERSION_MINOR;
    cb->shm_total_size          = total;
    cb->flags                   = USBWARP_FLAG_COHERENT_RAM |
                                  (s->cfg.enable_iso ? USBWARP_FLAG_ISO_ENABLED : 0);
    if (s->cfg.debug) cb->flags |= USBWARP_FLAG_DEBUG_ENABLED;
    if (s->doorbellEvent) cb->flags |= USBWARP_FLAG_G2H_DOORBELL;
    if (s->driverCaps & USBWARP_DRIVER_CAP_H2G_IRQ) cb->flags |= USBWARP_FLAG_H2G_IRQ;
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
    const uint32_t max_msg = USBWARP_MAX_MESSAGE_SIZE;   /* 1024 ISO results */
    InitRingHeader(g2h, ring_data, max_msg);
    InitRingHeader(h2g, ring_data, max_msg);

    /* After initialization only the kernel and guest access transport state. */
    s->shm_size         = total;

    LogInfo("SHM created: section=%p  base=%p  cb.magic=0x%08X",
            s->section, s->shm_base, cb->magic);
    return TRUE;
}

void ShmDestroy(struct warp_session *s)
{
    if (s->shm_base) { UnmapViewOfFile(s->shm_base); s->shm_base = NULL; }
    if (s->section)  { CloseHandle(s->section);       s->section  = NULL; }
    if (s->doorbellEvent) { CloseHandle(s->doorbellEvent); s->doorbellEvent=nullptr; }
    s->shm_size = 0;
}
