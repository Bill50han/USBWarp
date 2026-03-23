/*
 * usbwarp_pipe.h — UsbWarp Controller ↔ Service Named Pipe Protocol
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Binary message protocol over \\.\pipe\UsbWarpServicePipe.
 * Message mode (PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE).
 * Single concurrent connection; latecomers receive ERROR_PIPE_BUSY.
 *
 * Requires <stdint.h> or equivalent before inclusion.
 */

#ifndef USBWARP_PIPE_H
#define USBWARP_PIPE_H

#include "usbwarp_shared.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Pipe identity
 * ═══════════════════════════════════════════════════════════════════════════ */

#define USBWARP_PIPE_NAME       "\\\\.\\pipe\\UsbWarpServicePipe"
#define USBWARP_PIPE_NAME_W    L"\\\\.\\pipe\\UsbWarpServicePipe"
#define USBWARP_PIPE_MAGIC      0x50524157u   /* "WARP" LE */

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Message envelope
 *
 *   Every message (request and response) begins with this 16-byte header.
 *   payload_length is the byte count of the data that follows the header.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct usbwarp_pipe_header {
    uint32_t magic;                      /* USBWARP_PIPE_MAGIC                */
    uint32_t command;                    /* enum usbwarp_pipe_command          */
    uint32_t payload_length;             /* bytes after this header            */
    uint32_t sequence_id;                /* caller-assigned, echoed in reply   */
};

USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_pipe_header) == 16,
                       "pipe_header must be 16 bytes");

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Command codes
 * ═══════════════════════════════════════════════════════════════════════════ */

enum usbwarp_pipe_command {
    /* ── Queries (Controller → Service, Service replies) ──────────────────── */
    USBWARP_PIPE_CMD_LIST_DEVICES       = 1,
    USBWARP_PIPE_CMD_QUERY_STATUS       = 2,
    USBWARP_PIPE_CMD_QUERY_DEVICE       = 3,
    USBWARP_PIPE_CMD_QUERY_STATS        = 4,

    /* ── Mutations (Controller → Service, Service replies) ────────────────── */
    USBWARP_PIPE_CMD_BIND_DEVICE        = 10,
    USBWARP_PIPE_CMD_UNBIND_DEVICE      = 11,
    USBWARP_PIPE_CMD_START_SESSION      = 12,
    USBWARP_PIPE_CMD_STOP_SESSION       = 13,

    /* ── Response (Service → Controller) ──────────────────────────────────── */
    USBWARP_PIPE_CMD_RESPONSE           = 100,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Payload structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── 4.1  Generic response ────────────────────────────────────────────────── */

struct usbwarp_pipe_response {
    int32_t  status;                     /* 0 = success, <0 = error           */
    uint32_t detail_length;              /* UTF-8 detail string bytes (may 0) */
    /* uint8_t detail[];  — variable length                                   */
};

/* ── 4.2  LIST_DEVICES response ───────────────────────────────────────────── */

struct usbwarp_pipe_device_entry {
    uint8_t  device_guid[16];            /* binary GUID                       */
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  bound;                      /* 1 = attached to guest             */
    uint16_t description_length;         /* UTF-8 bytes that follow           */
    uint8_t  _pad[2];
    /* uint8_t description[];  — variable length UTF-8                        */
};

struct usbwarp_pipe_device_list {
    uint32_t device_count;
    /* struct usbwarp_pipe_device_entry entries[];  — variable length          */
};

/* ── 4.3  BIND / UNBIND request ───────────────────────────────────────────── */

struct usbwarp_pipe_bind_request {
    uint8_t  device_guid[16];
    uint16_t instance_path_length;       /* bytes of UTF-8 instance path      */
    uint16_t _pad;
    /* char instance_path[];  — variable length, NOT NUL-terminated          */
};

/* ── 4.4  QUERY_STATUS response ───────────────────────────────────────────── */

struct usbwarp_pipe_status {
    uint32_t session_state;
    uint32_t bound_device_count;
    uint32_t active_device_count;
    uint32_t orphan_mode;                /* 0 or 1                            */
    uint64_t uptime_seconds;
    uint64_t total_urbs_processed;
    uint64_t total_bytes_transferred;
};

/* ── 4.5  QUERY_STATS request / response ──────────────────────────────────── */

struct usbwarp_pipe_stats_request {
    uint8_t  device_guid[16];            /* all-zero = global stats           */
};

struct usbwarp_pipe_stats_response {
    uint64_t urb_submit_count;
    uint64_t urb_complete_count;
    uint64_t urb_cancel_count;
    uint64_t urb_error_count;
    uint64_t bytes_out;
    uint64_t bytes_in;
    uint32_t latency_p50_us;
    uint32_t latency_p99_us;
    uint32_t ring_usage_percent;
    uint32_t buffer_usage_percent;
    uint32_t rate_limit_hits;
    uint32_t circuit_breaker_trips;
};

/* ── 4.6  QUERY_DEVICE request / response ─────────────────────────────────── */

struct usbwarp_pipe_query_device_request {
    uint8_t  device_guid[16];
};

struct usbwarp_pipe_query_device_response {
    int32_t  status;                     /* 0 = found, <0 = not found         */
    uint8_t  device_guid[16];
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  speed;                      /* enum usbwarp_usb_speed            */
    uint8_t  bound;
    uint8_t  circuit_breaker_state;      /* 0=CLOSED, 1=OPEN, 2=HALF_OPEN    */
    uint8_t  _pad[2];
    uint32_t pending_urb_count;
    uint64_t total_bytes;
    uint32_t error_rate_60s_permille;    /* 0–1000                            */
    uint32_t avg_latency_us;
};

#endif /* USBWARP_PIPE_H */
