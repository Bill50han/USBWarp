/*
 * usbwarp_pipe.h — UsbWarp Controller ↔ Service Named Pipe Protocol
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Binary message protocol over \\.\pipe\UsbWarpServicePipe.
 * Message mode (PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE).
 * Multiple concurrent local connections, bounded by the service.
 *
 * Requires <stdint.h> or equivalent before inclusion.
 */

#ifndef USBWARP_PIPE_H
#define USBWARP_PIPE_H

#include "usbwarp_shared.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Pipe identity
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef USBWARP_PIPE_NAME
#define USBWARP_PIPE_NAME       "\\\\.\\pipe\\UsbWarpServicePipe"
#endif
#ifndef USBWARP_PIPE_NAME_W
#define USBWARP_PIPE_NAME_W    L"\\\\.\\pipe\\UsbWarpServicePipe"
#endif
#define USBWARP_PIPE_MAGIC      0x50524157u   /* "WARP" LE */

/* OW must be present to suppress the owner's implicit WRITE_DAC. An
 * unelevated token for the service account gets client I/O, not permission
 * to add server instances or rewrite the ACL. Elevated BA/SY retain control. */
#define USBWARP_PIPE_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;RC;;;OW)(A;;0x12019b;;;AU)"

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
    USBWARP_PIPE_CMD_LIST_VMS           = 5,
    USBWARP_PIPE_CMD_QUERY_TARGET       = 6,

    /* ── Mutations (Controller → Service, Service replies) ────────────────── */
    USBWARP_PIPE_CMD_BIND_DEVICE        = 10,
    USBWARP_PIPE_CMD_UNBIND_DEVICE      = 11,
    USBWARP_PIPE_CMD_START_SESSION      = 12,
    USBWARP_PIPE_CMD_STOP_SESSION       = 13,
    USBWARP_PIPE_CMD_SET_TARGET         = 14,

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
    uint8_t  speed;                      /* negotiated speed; former padding */
    uint8_t  _pad;
    /* uint8_t description[];  — variable length UTF-8                        */
};

struct usbwarp_pipe_device_list {
    uint32_t device_count;
    /* struct usbwarp_pipe_device_entry entries[];  — variable length          */
};

/* ── 4.3  BIND / UNBIND request ───────────────────────────────────────────── */

struct usbwarp_pipe_bind_request {
    uint8_t  device_guid[16];            /* binary GUID                       */
    uint16_t instance_path_length;       /* bytes of UTF-8 instance path      */
    uint8_t  speed;                      /* enum usbwarp_usb_speed, 0 = unknown */
    uint8_t  _pad;
    /* char instance_path[];  — variable length, NOT NUL-terminated          */
};

/* ── 4.4  QUERY_STATUS response ───────────────────────────────────────────── */

enum usbwarp_session_stage {
    USBWARP_SESSION_INITIALIZING = 0,
    USBWARP_SESSION_CONNECTING = 1,
    USBWARP_SESSION_RUNNING = 2,
    USBWARP_SESSION_STOPPING = 3,
    USBWARP_SESSION_STOPPED = 4,
    USBWARP_SESSION_ERROR = 5,
    USBWARP_SESSION_WAIT_DRIVER = 6,
    USBWARP_SESSION_WAIT_VM = 7,
    USBWARP_SESSION_WAIT_GUEST = 8,
    USBWARP_SESSION_RETRYING = 9,
    USBWARP_SESSION_WAIT_TARGET = 10,
};

/* VM selection is control-plane only. Existing messages retain their layout. */
enum usbwarp_target_kind {
    USBWARP_TARGET_NONE = 0,
    USBWARP_TARGET_ID = 1,
    USBWARP_TARGET_NAME = 2,
    USBWARP_TARGET_WSL = 3,
};
#define USBWARP_VM_PAGE_SIZE 32u
#define USBWARP_VM_VALUE_BYTES 1024u
#define USBWARP_VM_OWNER_BYTES 256u

/* Fixed UTF-8 fields must be terminated, valid UTF-8 and zero-padded. */
struct usbwarp_pipe_target {
    uint32_t kind;
    uint32_t reserved;
    char value[USBWARP_VM_VALUE_BYTES];
    char owner[USBWARP_VM_OWNER_BYTES];
};
struct usbwarp_pipe_vm_entry {
    char id[256];
    char name[USBWARP_VM_VALUE_BYTES];
    char owner[USBWARP_VM_OWNER_BYTES];
    char state[32];
};
struct usbwarp_pipe_vm_list_request {
    uint64_t generation; /* zero for the first page; prevents mixed snapshots */
    uint32_t offset;
    uint32_t reserved;
};
struct usbwarp_pipe_vm_list {
    uint64_t generation;
    uint32_t total_count;
    uint32_t count;
    uint32_t error; /* discovery error: entries are diagnostic, never used to attach */
    uint32_t reserved;
    /* usbwarp_pipe_vm_entry entries[count] */
};
struct usbwarp_pipe_target_status {
    struct usbwarp_pipe_target selected;
    char resolved_id[256];
    char attached_id[256];
    uint32_t matches;
    uint32_t error;
    uint32_t switching;
    uint32_t reserved;
};
USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_pipe_target) == 1288, "target wire size");
USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_pipe_vm_entry) == 1568, "VM entry wire size");
USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_pipe_target_status) == 1816, "target status wire size");

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
    /* Current kernel snapshots provide total URBs, combined bytes in
     * bytes_out, and global G2H ring usage. Other fields are reserved and
     * zero, not measured zeros. Keep this layout for existing clients. */
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
