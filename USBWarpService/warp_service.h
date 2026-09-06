/*
 * warp_service.h — UsbWarp Service internal state and declarations.
 *
 * Copyright (c) 2026 UsbWarp Project
 */

#ifndef USBWARP_SERVICE_H
#define USBWARP_SERVICE_H

#include "hdv_defs.h"
#include "../include/usbwarp_shared.h"
#include "../include/usbwarp_pipe.h"
#include "../include/usbwarp_ioctl.h"

#include <stdbool.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

struct warp_config {
    WCHAR    vm_id[256];
    uint32_t shm_size;
    uint32_t ring_data_size;
    uint32_t buffer_size;
    uint32_t max_devices;
    bool     enable_iso;
    bool     debug;
};

static inline void warp_config_defaults(struct warp_config *c)
{
    memset(c, 0, sizeof(*c));
    c->shm_size        = USBWARP_SHM_SIZE_DEFAULT;
    c->ring_data_size  = 256u * 1024;
    c->buffer_size     = USBWARP_BUFFER_SIZE_DEFAULT;
    c->max_devices     = USBWARP_MAX_DEVICES_DEFAULT;
    c->enable_iso      = false;
    c->debug           = false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  PCI config space
 * ═══════════════════════════════════════════════════════════════════════════ */

#define USBWARP_PCI_CFG_SIZE    256
#define USBWARP_PCI_CFG_DWORDS  (USBWARP_PCI_CFG_SIZE / 4)

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Per-device tracking (Service-side)
 *
 *   Only used by the no-driver debug path (Service owns the Ring and
 *   answers LIST/BIND/UNBIND itself).  In driver mode the kernel is the
 *   single source of truth — queries go through IOCTL_USBWARP_ENUM_DEVICES
 *   and this array is not consulted.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct warp_bound_device {
    bool     in_use;
    bool     binding;                /* tentative slot, not externally visible */
    uint32_t device_index;           /* driver-assigned, 1-based            */
    uint8_t  device_guid[16];
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  speed;                  /* enum usbwarp_usb_speed, 0 unknown   */
    WCHAR    instance_path[256];

    /* Statistics */
    uint64_t urb_submit_count;
    uint64_t urb_complete_count;
    uint64_t bytes_transferred;
};

#define WARP_MAX_BOUND_DEVICES  USBWARP_MAX_DEVICES_LIMIT

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Global statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

struct warp_stats {
    uint64_t total_urb_submits;
    uint64_t total_urb_completes;
    uint64_t total_urb_cancels;
    uint64_t total_urb_errors;
    uint64_t total_bytes_out;
    uint64_t total_bytes_in;
    uint64_t g2h_messages_consumed;
    uint64_t h2g_messages_produced;
    uint32_t negotiate_count;
    uint32_t guest_heartbeat_timeouts;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Service runtime state
 * ═══════════════════════════════════════════════════════════════════════════ */

struct warp_session {
    /* ── Configuration ──────────────────────────────────────────────────── */
    struct warp_config  cfg;

    /* ── HDV API ────────────────────────────────────────────────────────── */
    struct usbwarp_hdv_api api;

    /* ── HCS / HDV handles ──────────────────────────────────────────────── */
    HCS_SYSTEM          hcs_system;
    HDV_HOST            hdv_host;
    HDV_DEVICE          hdv_device;
    GUID                class_id;
    GUID                instance_id;

    /* ── PCI config space ───────────────────────────────────────────────── */
    UINT32              pci_cfg[USBWARP_PCI_CFG_DWORDS];
    UINT32              bar0_mask;
    bool                bar0_probing;

    /* ── Shared memory ──────────────────────────────────────────────────── */
    HANDLE              section;
    void               *shm_base;
    uint32_t            shm_size;

    volatile struct usbwarp_control_block  *cb;
    volatile struct usbwarp_ring_header    *g2h_ring;
    volatile struct usbwarp_ring_header    *h2g_ring;
    volatile uint8_t                       *g2h_ring_data;
    volatile uint8_t                       *h2g_ring_data;
    volatile uint8_t                       *data_region;
    uint32_t            data_region_size;
    uint32_t            buffer_count;

    /* ── HDV callback flags ─────────────────────────────────────────────── */
    volatile bool       hdv_started;
    volatile bool       hdv_torn_down;
    volatile bool       mem_space_on;
    volatile bool       mmio_mapped;
    HANDLE              memSpaceEvent;

    /* ── Lifecycle ──────────────────────────────────────────────────────── */
    volatile bool       shutdown_requested;

    /* ── Kernel Driver IOCTL (#1 full) ──────────────────────────────────── */
    HANDLE              hDriver;         /* \\.\UsbWarp handle, or NULL     */
    bool                driverRegistered;
    bool                driverShmSetup;

    /* Protects the hDriver lifetime: IOCTL callers take it shared, open/
     * close take it exclusive.  Prevents CloseHandle from racing an
     * in-flight DeviceIoControl on the pipe thread.  SRW (not deviceLock)
     * because a bind can hold the shared lock for seconds and the 2s
     * heartbeat must never queue behind it (orphan grace is 30s). */
    SRWLOCK             driverLock;

    /* ── Heartbeat timing (#7) ──────────────────────────────────────────── */
    UINT64              last_host_heartbeat_ns;

    /* ── Guest heartbeat monitoring (#11) ───────────────────────────────── */
    UINT64              last_guest_heartbeat_seen;
    UINT64              guest_heartbeat_last_check_ns;
    uint32_t            guest_heartbeat_miss_count;

    /* ── Negotiation ────────────────────────────────────────────────────── */
    bool                negotiated;

    /* ── Device tracking (#2, #5) ───────────────────────────────────────── */
    struct warp_bound_device bound_devices[WARP_MAX_BOUND_DEVICES];
    uint32_t            bound_device_count;
    CRITICAL_SECTION    deviceLock;
    bool                deviceLockInit;

    /* ── Named Pipe (#2) ────────────────────────────────────────────────── */
    HANDLE              pipeThread;
    volatile bool       pipeRunning;

    /* ── Statistics (#13) ───────────────────────────────────────────────── */
    struct warp_stats   stats;

    /* ── Timing ─────────────────────────────────────────────────────────── */
    int                 cfg_reads;
    int                 cfg_writes;
    UINT64              start_time_ns;

    /* ── Logging (#13-log) ──────────────────────────────────────────────── */
    CRITICAL_SECTION    logLock;
    bool                logLockInit;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Shared-memory
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL ShmCreate(struct warp_session *s);
void ShmDestroy(struct warp_session *s);

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  HDV session lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL HdvSessionOpen(struct warp_session *s);
BOOL HdvSessionMapMmio(struct warp_session *s);
void HdvSessionClose(struct warp_session *s);

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Kernel Driver IOCTL client (#1 full implementation)
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL DrvClientOpen(struct warp_session *s);
void DrvClientClose(struct warp_session *s);
BOOL DrvClientRegister(struct warp_session *s);
BOOL DrvClientSetupShm(struct warp_session *s);
BOOL DrvClientHeartbeat(struct warp_session *s);

/* GUID-keyed operations — the kernel is the single source of truth for
 * bound-device state; the Service keeps no mirror in driver mode. */
BOOL DrvClientQueryStatus(struct warp_session *s, USBWARP_QUERY_STATUS_OUT *out);
BOOL DrvClientEnumDevices(struct warp_session *s, USBWARP_ENUM_DEVICES_OUT *out);
BOOL DrvClientBindGuid(struct warp_session *s, const uint8_t device_guid[16],
                       const WCHAR *instance_path, uint8_t speed,
                       uint32_t *device_index, uint16_t *vendor_id,
                       uint16_t *product_id);
BOOL DrvClientUnbindGuid(struct warp_session *s, const uint8_t device_guid[16]);
void DrvClientUnbindAll(struct warp_session *s);

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Named Pipe server
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL PipeServerStart(struct warp_session *s);
void PipeServerStop(struct warp_session *s);

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  Ring helpers (Service-side, when no Kernel Driver)
 * ═══════════════════════════════════════════════════════════════════════════ */

int  ServiceRingConsume(struct warp_session *s, void *msg_out,
                        uint32_t buf_size, uint32_t *msg_len_out);
int  ServiceRingProduce(struct warp_session *s, const void *msg,
                        uint32_t msg_len);

/* Send specific Ring messages. */
void ServiceSendDeviceAdded(struct warp_session *s, uint32_t devIdx);
void ServiceSendDeviceRemoved(struct warp_session *s, uint32_t devIdx,
                              uint32_t reason);

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Emergency shutdown (#12)
 * ═══════════════════════════════════════════════════════════════════════════ */

void ServiceEmergencyShutdown(struct warp_session *s);

/* ═══════════════════════════════════════════════════════════════════════════
 * §11b  SCM service integration (winsvc.cpp)
 *
 *   Dual-mode binary: when launched by the SCM the process runs as a
 *   real Windows service; run from a shell it behaves exactly like the
 *   old console build.  Service config persists under
 *   HKLM\SOFTWARE\UsbWarp\Service (written by --install).
 * ═══════════════════════════════════════════════════════════════════════════ */

#define USBWARP_SERVICE_NAME     L"UsbWarpService"
#define USBWARP_SERVICE_DESCR    L"UsbWarp - HDV USB passthrough for WSL2"
#define USBWARP_SERVICE_REG_KEY  L"SOFTWARE\\UsbWarp\\Service"

int   SvcInstall(int argc, WCHAR *argv[]);   /* --install (admin)          */
int   SvcUninstall(void);                    /* --uninstall (admin)        */
int   RunServiceDispatcher(void);            /* 0 = ran as service; -1 = not SCM */
void  SvcCheckpoint(void);                   /* START_PENDING progress; console no-op */
void  SvcRunning(void);                      /* RUNNING; console no-op     */

/* ═══════════════════════════════════════════════════════════════════════════
 * §11c  Session lifecycle (main.cpp, shared by both modes)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Static storage — referenced by the Ctrl/SCM handlers, HDV callbacks and
 * the pipe thread; must outlive them all. */
extern struct warp_session g_sess;

/* Apply command-line overrides to a config (no vm_id requirement —
 * registry values may have supplied it). */
bool  ParseArgOverrides(int argc, WCHAR *argv[], struct warp_config *cfg);

/* Full session run: config → init chain → pipe server → main loop →
 * teardown.  Returns a Win32 exit code for the SCM. */
DWORD ServiceRun(struct warp_session *s, bool serviceMode,
                 int argc, WCHAR *argv[]);

/* ═══════════════════════════════════════════════════════════════════════════
 * §12  Logging (thread-safe)
 *
 *   Console mode logs to stderr; service mode appends to
 *   %ProgramData%\UsbWarp\service.log (stderr is invalid there).
 * ═══════════════════════════════════════════════════════════════════════════ */

void LogInit(struct warp_session *s, bool serviceMode);
void LogCleanup(struct warp_session *s);
void LogInfo(const char *fmt, ...);
void LogWarn(const char *fmt, ...);
void LogError(const char *fmt, ...);
extern CRITICAL_SECTION *g_logLock;

/* ═══════════════════════════════════════════════════════════════════════════
 * §13  Timestamp helper
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline UINT64 NowNs(void)
{
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return (UINT64)((ctr.QuadPart * 1000000000ULL) / freq.QuadPart);
}

#endif /* USBWARP_SERVICE_H */
