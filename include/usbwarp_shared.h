/*
 * usbwarp_shared.h — UsbWarp Cross-Platform Shared Memory Protocol Definitions
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * This header defines every data structure that lives in the Section-Backed
 * shared memory region between the Windows Kernel Driver (UsbWarp.sys) and
 * the Linux Virtual HCD module (usbwarp.ko).  Both sides MUST use identical
 * layout.  All fields are little-endian, naturally aligned, and use C99
 * fixed-width integer types.
 *
 * Include prerequisites:
 *   Linux kernel  — #include <linux/types.h>   (before this header)
 *   Windows WDK   — #include <ntddk.h>         (before this header)
 *   User-space    — (none; stdint.h included automatically)
 */

#ifndef USBWARP_SHARED_H
#define USBWARP_SHARED_H

/* ═══════════════════════════════════════════════════════════════════════════
 * §0  Type compatibility
 * ═══════════════════════════════════════════════════════════════════════════ */

#if defined(__KERNEL__)                         /* Linux kernel */
  #include <linux/types.h>
  #include <linux/stddef.h>
#elif defined(_MSC_VER) && defined(_KERNEL_MODE) /* Windows WDK */
  /* ntddk.h already included by caller; provide C99 aliases if absent */
  #ifndef _STDINT
    typedef unsigned __int8   uint8_t;
    typedef unsigned __int16  uint16_t;
    typedef unsigned __int32  uint32_t;
    typedef unsigned __int64  uint64_t;
    typedef __int32           int32_t;
    typedef __int64           int64_t;
    #ifdef  _WIN64
        #define offsetof(s,m)   (size_t)( (ptrdiff_t)&(((s *)0)->m) )
    #else
        #define offsetof(s,m)   (size_t)&(((s *)0)->m)
    #endif
  #endif
#else                                           /* User-space */
  #include <stdint.h>
  #include <stddef.h>
#endif

/* Static assertion — works in C11, C++11, GCC ≥ 4.6, MSVC ≥ 2015 */
#ifdef __cplusplus
  #define USBWARP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(_MSC_VER)
  #define USBWARP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
  #define USBWARP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#elif defined(__GNUC__)
  #define USBWARP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
  #define USBWARP_STATIC_ASSERT(cond, msg)  /* no-op fallback */
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Fundamental constants
 * ═══════════════════════════════════════════════════════════════════════════ */

/* --- Magic values -------------------------------------------------------- */
#define USBWARP_CONTROL_BLOCK_MAGIC   0x56444855u   /* "UHDV" LE */
#define USBWARP_MSG_MAGIC             0x4D444855u   /* "UHDM" LE */

/* --- Protocol version ---------------------------------------------------- */
#define USBWARP_PROTOCOL_VERSION_MAJOR  1
#define USBWARP_PROTOCOL_VERSION_MINOR  0

#define USBWARP_MAKE_VERSION(maj, min)  (((uint16_t)(maj) << 8) | (uint16_t)(min))
#define USBWARP_VERSION_MAJOR(v)        ((uint16_t)(v) >> 8)
#define USBWARP_VERSION_MINOR(v)        ((uint16_t)(v) & 0xFF)

#define USBWARP_PROTOCOL_VERSION \
    USBWARP_MAKE_VERSION(USBWARP_PROTOCOL_VERSION_MAJOR, \
                         USBWARP_PROTOCOL_VERSION_MINOR)

/* --- PCI identity (virtual device) --------------------------------------- */
#define USBWARP_PCI_VENDOR_ID     0x1234u
#define USBWARP_PCI_DEVICE_ID     0x5678u
#define USBWARP_PCI_BASE_CLASS    0xFFu   /* Unclassified — prevents auto-bind */
#define USBWARP_PCI_SUB_CLASS     0x00u
#define USBWARP_PCI_PROG_IF       0x00u
#define USBWARP_PCI_REVISION_ID   0x01u
#define USBWARP_BAR0_PROBE_MASK   0xFFFFF000u  /* 4 KB minimum granularity */

/* --- Sizing -------------------------------------------------------------- */
#define USBWARP_CACHELINE_SIZE      64u
#define USBWARP_PAGE_SIZE           4096u
#define USBWARP_CONTROL_BLOCK_SIZE  4096u       /* 1 page, fixed */
#define USBWARP_RING_HEADER_SIZE    192u        /* 3 cache lines */
#define USBWARP_INLINE_DATA_SIZE    96u         /* max inline payload */
#define USBWARP_MSG_HEADER_SIZE     32u

/* Shared-memory total size bounds (bytes) */
#define USBWARP_SHM_SIZE_MIN        (4u   * 1024 * 1024)   /*   4 MB */
#define USBWARP_SHM_SIZE_MAX        (256u * 1024 * 1024)    /* 256 MB */
#define USBWARP_SHM_SIZE_DEFAULT    (32u  * 1024 * 1024)    /*  32 MB */

/* Ring data-area size bounds (must be power of 2) */
#define USBWARP_RING_DATA_SIZE_MIN  (64u  * 1024)           /*  64 KB */
#define USBWARP_RING_DATA_SIZE_MAX  (4u   * 1024 * 1024)    /*   4 MB */

/* Single buffer size bounds (must be power of 2) */
#define USBWARP_BUFFER_SIZE_MIN     4096u                    /*   4 KB */
#define USBWARP_BUFFER_SIZE_MAX     (1u   * 1024 * 1024)     /*   1 MB */
#define USBWARP_BUFFER_SIZE_DEFAULT 65536u                   /*  64 KB */

/* Device limits */
#define USBWARP_MAX_DEVICES_LIMIT   32u
#define USBWARP_MAX_DEVICES_DEFAULT 8u

/* Per-device defaults */
#define USBWARP_PENDING_URBS_MIN        8u
#define USBWARP_PENDING_URBS_DEFAULT    256u
#define USBWARP_PENDING_URBS_MAX        1024u
#define USBWARP_BUFFER_QUOTA_MIN        4u

/* TransactionId sentinel */
#define USBWARP_TXN_ID_NONE  0u

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Alignment helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Round `x` up to the next multiple of cache-line size. */
#define USBWARP_ALIGN_CACHELINE(x) \
    (((uint32_t)(x) + USBWARP_CACHELINE_SIZE - 1u) & ~(USBWARP_CACHELINE_SIZE - 1u))

/* Round `x` up to the next multiple of page size. */
#define USBWARP_ALIGN_PAGE(x) \
    (((uint32_t)(x) + USBWARP_PAGE_SIZE - 1u) & ~(USBWARP_PAGE_SIZE - 1u))

/* True if `x` is a non-zero power of 2. */
#define USBWARP_IS_POWER_OF_2(x)  ((x) != 0 && (((x) & ((x) - 1)) == 0))

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Capability flags  (used in NEGOTIATE / NEGOTIATE_RESP)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define USBWARP_CAP_ISO_TRANSFER    0x00000001u  /* ISO transfer support      */
#define USBWARP_CAP_INLINE_DATA     0x00000002u  /* Inline data ≤ 96 B        */
#define USBWARP_CAP_BATCH_SUBMIT    0x00000004u  /* Batch produce / consume   */
#define USBWARP_CAP_STATS           0x00000008u  /* Stats query messages      */
/* Bits 4–31 reserved for future versions */

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Control Block global-flags
 * ═══════════════════════════════════════════════════════════════════════════ */

#define USBWARP_FLAG_ISO_ENABLED    0x00000001u
#define USBWARP_FLAG_DEBUG_ENABLED  0x00000002u

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Endpoint / session state
 * ═══════════════════════════════════════════════════════════════════════════ */

enum usbwarp_state {
    USBWARP_STATE_INIT            = 0,
    USBWARP_STATE_READY           = 1,   /* awaiting handshake      */
    USBWARP_STATE_RUNNING         = 2,
    USBWARP_STATE_SHUTTING_DOWN   = 3,
    USBWARP_STATE_STOPPED         = 4,
    USBWARP_STATE_ERROR           = 5,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Control Block   (offset 0x000, size 4096 bytes = 1 page)
 *
 *   Cache Line 0  [0x000–0x03F]  Protocol identity + layout (Host-init, RO)
 *   Cache Line 1  [0x040–0x07F]  Host heartbeat   (Host writes, Guest reads)
 *   Cache Line 2  [0x080–0x0BF]  Guest heartbeat  (Guest writes, Host reads)
 *   Remainder     [0x0C0–0xFFF]  Reserved / zero
 * ═══════════════════════════════════════════════════════════════════════════ */

struct usbwarp_control_block {
    /* ── Cache Line 0: protocol identity + memory layout (read-only) ────── */
    uint32_t magic;                      /* [0x000] USBWARP_CONTROL_BLOCK_MAGIC */
    uint16_t protocol_version_major;     /* [0x004]                             */
    uint16_t protocol_version_minor;     /* [0x006]                             */
    uint32_t shm_total_size;             /* [0x008] total shared-memory bytes   */
    uint32_t flags;                      /* [0x00C] USBWARP_FLAG_*              */

    uint32_t g2h_ring_offset;            /* [0x010] Guest→Host ring byte offset */
    uint32_t g2h_ring_size;              /* [0x014] including 192-B header      */
    uint32_t h2g_ring_offset;            /* [0x018] Host→Guest ring byte offset */
    uint32_t h2g_ring_size;              /* [0x01C] including 192-B header      */

    uint32_t data_region_offset;         /* [0x020] page-aligned                */
    uint32_t data_region_size;           /* [0x024]                             */
    uint32_t buffer_size;                /* [0x028] per-slot, power of 2        */
    uint32_t buffer_count;               /* [0x02C]                             */

    uint8_t  _pad0[16];                  /* [0x030] pad to cache-line boundary  */

    /* ── Cache Line 1: host heartbeat ────────────────────────────────────── */
    uint64_t host_heartbeat_ts;          /* [0x040] nanosecond monotonic        */
    uint32_t host_state;                 /* [0x048] enum usbwarp_state          */
    uint32_t _host_reserved;             /* [0x04C]                             */
    uint8_t  _pad1[48];                  /* [0x050] pad to cache-line boundary  */

    /* ── Cache Line 2: guest heartbeat ───────────────────────────────────── */
    uint64_t guest_heartbeat_ts;         /* [0x080] nanosecond monotonic        */
    uint32_t guest_state;                /* [0x088] enum usbwarp_state          */
    uint32_t _guest_reserved;            /* [0x08C]                             */
    uint8_t  _pad2[48];                  /* [0x090] pad to cache-line boundary  */

    /* ── Remainder: reserved to 4 KB ─────────────────────────────────────── */
    uint8_t  _reserved[USBWARP_CONTROL_BLOCK_SIZE - 3 * USBWARP_CACHELINE_SIZE];
};

USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_control_block) == 4096,
                       "control_block must be exactly 4096 bytes");

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Ring Header   (192 bytes = 3 cache lines)
 *
 *   The ring data area follows immediately after the header.
 *   Ring total size = 192 + data_size.
 *   data_size MUST be a power of 2.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Ring producer / consumer flag bits */
#define USBWARP_RING_FLAG_NO_NOTIFY  0x00000001u  /* suppress peer wakeup */

struct usbwarp_ring_header {
    /* ── Cache Line 0: producer-owned (only producer writes) ─────────────── */
    uint32_t producer_index;             /* [0x00] next write offset (bytes)  */
    uint32_t producer_flags;             /* [0x04] USBWARP_RING_FLAG_*        */
    uint8_t  _pad0[56];                  /* [0x08]                            */

    /* ── Cache Line 1: consumer-owned (only consumer writes) ─────────────── */
    uint32_t consumer_index;             /* [0x40] next read offset (bytes)   */
    uint32_t consumer_flags;             /* [0x44] USBWARP_RING_FLAG_*        */
    uint8_t  _pad1[56];                  /* [0x48]                            */

    /* ── Cache Line 2: metadata (read-only after init) ───────────────────── */
    uint32_t data_size;                  /* [0x80] ring data-area bytes (2^N) */
    uint32_t data_size_mask;             /* [0x84] data_size − 1              */
    uint32_t max_message_size;           /* [0x88] hard limit per message     */
    uint32_t _reserved_meta;             /* [0x8C]                            */
    uint8_t  _pad2[48];                  /* [0x90]                            */
};

USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_ring_header) == 192,
                       "ring_header must be exactly 192 bytes");

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Ring inline helpers
 *
 *   available = (ci − pi − 1) & mask
 *   used      = (pi − ci)     & mask
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t usbwarp_ring_available(uint32_t pi, uint32_t ci,
                                              uint32_t mask)
{
    return (ci - pi - 1u) & mask;
}

static inline uint32_t usbwarp_ring_used(uint32_t pi, uint32_t ci,
                                         uint32_t mask)
{
    return (pi - ci) & mask;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Message header  (32 bytes, begins every ring message)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct usbwarp_msg_header {
    uint32_t magic;                      /* [0x00] USBWARP_MSG_MAGIC           */
    uint16_t message_type;               /* [0x04] enum usbwarp_message_type   */
    uint16_t protocol_version;           /* [0x06] USBWARP_MAKE_VERSION(M,m)   */
    uint32_t message_length;             /* [0x08] total bytes including header */
    uint32_t transaction_id;             /* [0x0C] request/response correlator  */
    uint32_t device_id;                  /* [0x10] 0=global, 1+=per-device     */
    uint32_t flags;                      /* [0x14] reserved, set 0             */
    uint64_t timestamp;                  /* [0x18] sender monotonic ns          */
};

USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_msg_header) == 32,
                       "msg_header must be exactly 32 bytes");

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  Message type enumeration
 * ═══════════════════════════════════════════════════════════════════════════ */

enum usbwarp_message_type {
    /* ── Control messages (1–99) ──────────────────────────────────────────── */
    USBWARP_MSG_NEGOTIATE             = 1,
    USBWARP_MSG_NEGOTIATE_RESP        = 2,

    USBWARP_MSG_DEVICE_ADDED          = 3,   /* H → G */
    USBWARP_MSG_DEVICE_REMOVED        = 4,   /* H → G */
    USBWARP_MSG_DEVICE_ADD_ACK        = 5,   /* G → H */
    USBWARP_MSG_DEVICE_REMOVE_ACK     = 6,   /* G → H */

    USBWARP_MSG_HEARTBEAT             = 20,  /* bidirectional */

    USBWARP_MSG_HOST_SHUTDOWN         = 30,  /* H → G */
    USBWARP_MSG_GUEST_SHUTDOWN        = 31,  /* G → H */
    USBWARP_MSG_SHUTDOWN_ACK          = 32,  /* bidirectional */

    /* ── URB messages (100–199) ───────────────────────────────────────────── */
    USBWARP_MSG_URB_SUBMIT            = 100, /* G → H */
    USBWARP_MSG_URB_COMPLETE          = 101, /* H → G */
    USBWARP_MSG_URB_CANCEL            = 102, /* G → H */
    USBWARP_MSG_URB_CANCEL_ACK        = 103, /* H → G */

    /* ── ISO messages (200–299, optional) ─────────────────────────────────── */
    USBWARP_MSG_ISO_SUBMIT            = 200, /* G → H */
    USBWARP_MSG_ISO_COMPLETE          = 201, /* H → G */

    /* ── Circuit-breaker messages (300–399) ────────────────────────────────── */
    USBWARP_MSG_CIRCUIT_BREAKER_TRIP  = 300, /* bidirectional */
    USBWARP_MSG_CIRCUIT_BREAKER_RESET = 301, /* bidirectional */

    /* ── Diagnostic messages (400–499) ────────────────────────────────────── */
    USBWARP_MSG_STATS_REQUEST         = 400, /* bidirectional */
    USBWARP_MSG_STATS_RESPONSE        = 401, /* bidirectional */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Protocol status codes   (used in completion / error messages)
 * ═══════════════════════════════════════════════════════════════════════════ */

enum usbwarp_status {
    /* Success */
    USBWARP_STATUS_SUCCESS            =    0,

    /* URB-level errors (−1 … −99) */
    USBWARP_STATUS_CANCELLED          =   -1,
    USBWARP_STATUS_STALL              =   -2,
    USBWARP_STATUS_TIMEOUT            =   -3,
    USBWARP_STATUS_SHORT_PACKET       =   -4,
    USBWARP_STATUS_OVERFLOW           =   -5,
    USBWARP_STATUS_BABBLE             =   -6,
    USBWARP_STATUS_CRC                =   -7,
    USBWARP_STATUS_BITSTUFF           =   -8,

    /* Device / system errors (−100 … −199) */
    USBWARP_STATUS_DISCONNECTED       = -100,
    USBWARP_STATUS_DEVICE_ERROR       = -101,
    USBWARP_STATUS_NOT_FOUND          = -102,
    USBWARP_STATUS_INVALID_PARAM      = -103,
    USBWARP_STATUS_NO_RESOURCE        = -104,
    USBWARP_STATUS_SHUTDOWN           = -105,

    /* Security / protocol errors (−200 … −299) */
    USBWARP_STATUS_RATE_LIMITED       = -200,
    USBWARP_STATUS_CIRCUIT_BREAK      = -201,
    USBWARP_STATUS_PROTOCOL_ERROR     = -202,
    USBWARP_STATUS_ACCESS_DENIED      = -203,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §12  USB transfer / data-mode / speed enumerations
 * ═══════════════════════════════════════════════════════════════════════════ */

enum usbwarp_transfer_type {
    USBWARP_XFER_CONTROL              = 0,
    /* 1 reserved (former ISO slot; ISO uses separate message type) */
    USBWARP_XFER_BULK                 = 2,
    USBWARP_XFER_INTERRUPT            = 3,
};

enum usbwarp_data_mode {
    USBWARP_DATA_NONE                 = 0,   /* zero-length transfer        */
    USBWARP_DATA_BUFFER               = 1,   /* payload in Data Region slot */
    USBWARP_DATA_INLINE               = 2,   /* payload appended to message */
};

enum usbwarp_usb_speed {
    USBWARP_SPEED_LOW                 = 1,   /*   1.5 Mbps */
    USBWARP_SPEED_FULL                = 2,   /*    12 Mbps */
    USBWARP_SPEED_HIGH                = 3,   /*   480 Mbps */
    USBWARP_SPEED_SUPER               = 4,   /*     5 Gbps */
};

enum usbwarp_shutdown_reason {
    USBWARP_SHUTDOWN_NORMAL           = 0,
    USBWARP_SHUTDOWN_SERVICE_LOST     = 1,
    USBWARP_SHUTDOWN_VM_STOPPING      = 2,
    USBWARP_SHUTDOWN_ERROR            = 3,
};

enum usbwarp_remove_reason {
    USBWARP_REMOVE_USER_REQUEST       = 0,
    USBWARP_REMOVE_DEVICE_UNPLUG      = 1,
    USBWARP_REMOVE_CIRCUIT_BREAKER    = 2,
    USBWARP_REMOVE_HOST_SHUTDOWN      = 3,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §13  Message structures
 *
 *   Naming convention:
 *     struct usbwarp_msg_<lowercase_short_name>
 *
 *   Ring slot consumption:
 *     USBWARP_ALIGN_CACHELINE(header.message_length)
 *
 *   For URB_SUBMIT / URB_COMPLETE the struct includes a fixed 96-byte
 *   inline_data buffer.  When data_mode != INLINE the producer sets
 *   message_length to the base size (header + body) and the inline area
 *   is not written to the ring.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── 13.1  NEGOTIATE  (Guest → Host) ─────────────────────────────────────── */

struct usbwarp_msg_negotiate {
    struct usbwarp_msg_header hdr;       /* message_type = NEGOTIATE          */
    uint16_t min_version;                /* lowest acceptable version         */
    uint16_t max_version;                /* highest offered version           */
    uint32_t capabilities;               /* USBWARP_CAP_* bitmask            */
    uint32_t max_pending_urbs;           /* desired concurrency               */
    uint32_t _reserved[4];
};  /* 32 + 28 = 60 bytes → ring slot 64 */

/* ── 13.2  NEGOTIATE_RESP  (Host → Guest) ────────────────────────────────── */

struct usbwarp_msg_negotiate_resp {
    struct usbwarp_msg_header hdr;       /* message_type = NEGOTIATE_RESP     */
    int32_t  status;                     /* 0 = accepted, <0 = rejected       */
    uint16_t negotiated_version;
    uint16_t _pad0;
    uint32_t capabilities;               /* agreed USBWARP_CAP_* intersection */
    uint32_t max_pending_urbs;           /* host-side limit                   */
    uint32_t max_devices;
    uint32_t _reserved[2];
};  /* 32 + 28 = 60 bytes → ring slot 64 */

/* ── 13.3  DEVICE_ADDED  (Host → Guest) ──────────────────────────────────── */

struct usbwarp_msg_device_added {
    struct usbwarp_msg_header hdr;       /* message_type = DEVICE_ADDED       */

    uint32_t device_id;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  max_packet_size_ep0;
    uint16_t bcd_device;
    uint8_t  num_configurations;
    uint8_t  speed;                      /* enum usbwarp_usb_speed            */
    uint8_t  device_descriptor[18];      /* raw 18-byte USB device descriptor */
    uint8_t  _pad1[2];
    uint32_t config_descriptor_length;   /* bytes of trailing config desc     */
    /* uint8_t config_descriptor_data[]; — variable-length, follows struct    */
};  /* 32 + 40 = 72 bytes base; ring slot = ALIGN_CL(72 + cfg_desc_len) */

/* ── 13.4  DEVICE_REMOVED  (Host → Guest) ────────────────────────────────── */

struct usbwarp_msg_device_removed {
    struct usbwarp_msg_header hdr;       /* message_type = DEVICE_REMOVED     */
    uint32_t device_id;
    uint32_t reason;                     /* enum usbwarp_remove_reason        */
    uint32_t _reserved[2];
};  /* 32 + 16 = 48 → ring slot 64 */

/* ── 13.5  DEVICE_ADD_ACK / DEVICE_REMOVE_ACK  (Guest → Host) ────────────── */

struct usbwarp_msg_device_ack {
    struct usbwarp_msg_header hdr;       /* ADD_ACK or REMOVE_ACK             */
    uint32_t device_id;
    int32_t  status;                     /* 0 = ok                            */
    uint32_t _reserved[2];
};  /* 32 + 16 = 48 → ring slot 64 */

/* ── 13.6  HEARTBEAT  (bidirectional) ────────────────────────────────────── */

struct usbwarp_msg_heartbeat {
    struct usbwarp_msg_header hdr;       /* message_type = HEARTBEAT          */
    uint64_t sender_timestamp;           /* nanosecond monotonic              */
    uint32_t sender_state;               /* enum usbwarp_state                */
    uint32_t pending_urb_count;
    uint32_t _reserved[2];
};  /* 32 + 24 = 56 → ring slot 64 */

/* ── 13.7  SHUTDOWN / SHUTDOWN_ACK  (bidirectional) ──────────────────────── */

struct usbwarp_msg_shutdown {
    struct usbwarp_msg_header hdr;       /* HOST_SHUTDOWN / GUEST_SHUTDOWN    */
    uint32_t reason;                     /* enum usbwarp_shutdown_reason      */
    uint32_t _reserved[3];
};  /* 32 + 16 = 48 → ring slot 64 */

struct usbwarp_msg_shutdown_ack {
    struct usbwarp_msg_header hdr;       /* SHUTDOWN_ACK                      */
    uint32_t _reserved[4];
};  /* 32 + 16 = 48 → ring slot 64 */

/* ── 13.8  URB_SUBMIT  (Guest → Host) ────────────────────────────────────── *
 *
 *   Layout (192 bytes = 3 cache lines):
 *     [0x00 – 0x1F]  header          32 B
 *     [0x20 – 0x5F]  body            64 B
 *     [0x60 – 0xBF]  inline_data     96 B
 *
 *   When data_mode != INLINE, set message_length = 96 (header+body only)
 *   and do NOT write the inline_data region to the ring.
 *   When data_mode == INLINE, set message_length = 96 + actual_data_len.
 */

#define USBWARP_MSG_URB_SUBMIT_BASE_SIZE  96u   /* header + body, no inline */

struct usbwarp_msg_urb_submit {
    struct usbwarp_msg_header hdr;       /* 32 B — message_type = URB_SUBMIT  */

    /* ── Body (64 bytes) ──────────────────────────────────────────────────── */
    uint32_t device_id;                  /* [0x20] redundant for fast parse   */
    uint32_t endpoint;                   /* [0x24] 0–15                       */
    uint32_t direction;                  /* [0x28] 0=OUT, 1=IN               */
    uint32_t transfer_type;              /* [0x2C] enum usbwarp_transfer_type */
    uint32_t transfer_length;            /* [0x30] requested byte count       */
    uint32_t transfer_flags;             /* [0x34] USB HCD transfer flags     */
    uint32_t data_mode;                  /* [0x38] enum usbwarp_data_mode     */
    uint32_t buffer_offset;              /* [0x3C] Data Region byte offset    */
    uint8_t  setup_packet[8];            /* [0x40] Control xfer only          */
    uint32_t interval;                   /* [0x48] Interrupt polling (ms)     */
    uint32_t _reserved[5];              /* [0x4C – 0x5F]                     */

    /* ── Inline data (96 bytes) ───────────────────────────────────────────── */
    uint8_t  inline_data[USBWARP_INLINE_DATA_SIZE]; /* [0x60 – 0xBF] */
};

USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_msg_urb_submit) == 192,
                       "msg_urb_submit must be exactly 192 bytes (3 cache lines)");

/* ── 13.9  URB_COMPLETE  (Host → Guest) ──────────────────────────────────── *
 *
 *   Same 192-byte envelope as URB_SUBMIT so that inline IN data can be
 *   returned without a Data Region round-trip.
 */

#define USBWARP_MSG_URB_COMPLETE_BASE_SIZE  96u

struct usbwarp_msg_urb_complete {
    struct usbwarp_msg_header hdr;       /* 32 B — message_type = URB_COMPLETE */

    /* ── Body (64 bytes) ──────────────────────────────────────────────────── */
    uint32_t device_id;                  /* [0x20]                            */
    uint32_t endpoint;                   /* [0x24]                            */
    int32_t  status;                     /* [0x28] enum usbwarp_status        */
    uint32_t actual_length;              /* [0x2C] bytes actually transferred */
    uint32_t data_mode;                  /* [0x30] BUFFER or INLINE or NONE   */
    uint32_t buffer_offset;              /* [0x34] IN data location           */
    uint32_t _reserved[10];              /* [0x38 – 0x5F]                    */

    /* ── Inline data (96 bytes) ───────────────────────────────────────────── */
    uint8_t  inline_data[USBWARP_INLINE_DATA_SIZE]; /* [0x60 – 0xBF] */
};

USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_msg_urb_complete) == 192,
                       "msg_urb_complete must be exactly 192 bytes (3 cache lines)");

/* ── 13.10  URB_CANCEL  (Guest → Host) ───────────────────────────────────── */

struct usbwarp_msg_urb_cancel {
    struct usbwarp_msg_header hdr;       /* transaction_id = target URB's TID */
    uint32_t device_id;
    uint32_t _reserved[3];
};  /* 32 + 16 = 48 → ring slot 64 */

/* ── 13.11  URB_CANCEL_ACK  (Host → Guest) ───────────────────────────────── */

struct usbwarp_msg_urb_cancel_ack {
    struct usbwarp_msg_header hdr;
    int32_t  status;                     /* 0 = cancelled, NOT_FOUND = late   */
    uint32_t _reserved[3];
};  /* 32 + 16 = 48 → ring slot 64 */

/* ── 13.12  ISO_SUBMIT  (Guest → Host, optional) ────────────────────────── */

struct usbwarp_iso_packet_desc {
    uint32_t offset;                     /* byte offset within buffer         */
    uint32_t length;                     /* requested packet length           */
};

struct usbwarp_msg_iso_submit {
    struct usbwarp_msg_header hdr;       /* message_type = ISO_SUBMIT         */
    uint32_t device_id;
    uint32_t endpoint;
    uint32_t direction;
    uint32_t number_of_packets;
    uint32_t start_frame;                /* 0 = ASAP                          */
    uint32_t buffer_offset;
    uint32_t total_length;
    uint32_t _reserved;
    /* struct usbwarp_iso_packet_desc packets[]; — variable length            */
};  /* 32 + 32 = 64 base; ring slot = ALIGN_CL(64 + 8*npackets) */

/* ── 13.13  ISO_COMPLETE  (Host → Guest, optional) ──────────────────────── */

struct usbwarp_iso_packet_result {
    uint32_t actual_length;
    int32_t  status;
};

struct usbwarp_msg_iso_complete {
    struct usbwarp_msg_header hdr;       /* message_type = ISO_COMPLETE       */
    uint32_t device_id;
    uint32_t endpoint;
    int32_t  status;                     /* overall status                    */
    uint32_t error_count;
    uint32_t number_of_packets;
    uint32_t _reserved[3];
    /* struct usbwarp_iso_packet_result results[]; — variable length          */
};  /* 32 + 32 = 64 base */

/* ── 13.14  CIRCUIT_BREAKER  (bidirectional) ─────────────────────────────── */

struct usbwarp_msg_circuit_breaker {
    struct usbwarp_msg_header hdr;       /* TRIP or RESET                     */
    uint32_t device_id;                  /* 0 = global                        */
    uint32_t reason;
    uint32_t error_count;                /* meaningful on TRIP only            */
    uint32_t _reserved;
};  /* 32 + 16 = 48 → ring slot 64 */

/* ── 13.15  STATS_REQUEST / STATS_RESPONSE  (bidirectional) ──────────────── */

struct usbwarp_msg_stats_request {
    struct usbwarp_msg_header hdr;       /* message_type = STATS_REQUEST      */
    uint32_t device_id;                  /* 0 = global stats                  */
    uint32_t _reserved[3];
};  /* 32 + 16 = 48 → ring slot 64 */

struct usbwarp_msg_stats_response {
    struct usbwarp_msg_header hdr;       /* message_type = STATS_RESPONSE     */
    uint32_t device_id;
    uint32_t _pad0;
    uint64_t urb_submit_count;
    uint64_t urb_complete_count;
    uint64_t urb_cancel_count;
    uint64_t urb_error_count;
    uint64_t bytes_out;
    uint64_t bytes_in;
    uint64_t inline_submit_count;
    uint64_t batch_submit_count;
    uint64_t rate_limit_hits;
    uint64_t msg_validation_failures;
    uint64_t ring_full_events;
    uint32_t latency_p50_us;
    uint32_t latency_p99_us;
    uint32_t ring_usage_percent;
    uint32_t buffer_usage_percent;
    uint32_t _reserved[2];
};  /* 32 + 120 = 152 → ring slot 192 */

/* ═══════════════════════════════════════════════════════════════════════════
 * §14  Offset / size verification
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Control Block field offsets */
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, magic)                == 0x000, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, protocol_version_major) == 0x004, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, shm_total_size)       == 0x008, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, flags)                == 0x00C, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, g2h_ring_offset)      == 0x010, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, h2g_ring_offset)      == 0x018, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, data_region_offset)   == 0x020, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, buffer_size)          == 0x028, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, buffer_count)         == 0x02C, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, host_heartbeat_ts)    == 0x040, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, host_state)           == 0x048, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, guest_heartbeat_ts)   == 0x080, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_control_block, guest_state)          == 0x088, "");

/* Ring header field offsets */
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_ring_header, producer_index)  == 0x00, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_ring_header, consumer_index)  == 0x40, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_ring_header, data_size)       == 0x80, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_ring_header, data_size_mask)  == 0x84, "");

/* Message header field offsets */
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_header, magic)            == 0x00, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_header, message_type)     == 0x04, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_header, message_length)   == 0x08, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_header, transaction_id)   == 0x0C, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_header, device_id)        == 0x10, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_header, timestamp)        == 0x18, "");

/* URB_SUBMIT body field offsets (relative to struct start) */
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_submit, device_id)       == 0x20, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_submit, endpoint)        == 0x24, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_submit, direction)       == 0x28, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_submit, transfer_type)   == 0x2C, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_submit, transfer_length) == 0x30, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_submit, transfer_flags)  == 0x34, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_submit, data_mode)       == 0x38, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_submit, buffer_offset)   == 0x3C, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_submit, setup_packet)    == 0x40, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_submit, interval)        == 0x48, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_submit, inline_data)     == 0x60, "");

/* URB_COMPLETE body field offsets */
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_complete, device_id)      == 0x20, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_complete, status)         == 0x28, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_complete, actual_length)  == 0x2C, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_complete, data_mode)      == 0x30, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_complete, buffer_offset)  == 0x34, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_complete, inline_data)    == 0x60, "");

#endif /* USBWARP_SHARED_H */
