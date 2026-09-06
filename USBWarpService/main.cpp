/*
 * main.cpp — UsbWarp Service entry point.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Complete fixes: #1 (IOCTL lifecycle), #3 (orphan/heartbeat), #4 (URB note),
 * #5 (Ring device msgs), #7 (heartbeat 1s), #9 (event wait), #10 (negotiate
 * capabilities), #11 (guest heartbeat), #12 (emergency shutdown), #13 (stats).
 */

#include "warp_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Thread-safe logging
 *
 *   Console mode → stderr.  Service mode → append to
 *   %ProgramData%\UsbWarp\service.log (stderr is an invalid handle in a
 *   service session).  Unbuffered so a crash doesn't lose the tail.
 * ═══════════════════════════════════════════════════════════════════════════ */

CRITICAL_SECTION *g_logLock = NULL;
static FILE *g_logSink = NULL;      /* NULL = stderr */

void LogInit(struct warp_session *s, bool serviceMode)
{
    InitializeCriticalSection(&s->logLock);
    s->logLockInit = true;
    g_logLock = &s->logLock;

    if (!serviceMode)
        return;

    char dir[MAX_PATH];
    if (GetEnvironmentVariableA("ProgramData", dir, sizeof(dir)) > 0) {
        char path[MAX_PATH];
        FILE *f = NULL;
        snprintf(path, sizeof(path), "%s\\UsbWarp", dir);
        CreateDirectoryA(path, NULL);
        snprintf(path, sizeof(path), "%s\\UsbWarp\\service.log", dir);
        if (fopen_s(&f, path, "a") == 0 && f) {
            g_logSink = f;
            setvbuf(g_logSink, NULL, _IONBF, 0);
        }
    }
    /* If the file couldn't be opened we keep stderr — writes there fail
     * silently in a service session, which is the best we can do. */
}

void LogCleanup(struct warp_session *s)
{
    if (s->logLockInit) {
        g_logLock = NULL;
        DeleteCriticalSection(&s->logLock);
        s->logLockInit = false;
    }
    if (g_logSink) {
        fclose(g_logSink);
        g_logSink = NULL;
    }
}

static void LogTimestamp(FILE *f)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static void LogV(const char *level, const char *fmt, va_list ap)
{
    FILE *f = g_logSink ? g_logSink : stderr;

    if (g_logLock) EnterCriticalSection(g_logLock);
    LogTimestamp(f);
    fprintf(f, "%s", level);
    vfprintf(f, fmt, ap);
    fprintf(f, "\n");
    if (g_logLock) LeaveCriticalSection(g_logLock);
}

void LogInfo(const char *fmt, ...)  { va_list ap; va_start(ap,fmt); LogV("[INFO]  ",fmt,ap); va_end(ap); }
void LogWarn(const char *fmt, ...)  { va_list ap; va_start(ap,fmt); LogV("[WARN]  ",fmt,ap); va_end(ap); }
void LogError(const char *fmt, ...) { va_list ap; va_start(ap,fmt); LogV("[ERROR] ",fmt,ap); va_end(ap); }

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Ctrl+C handling
 * ═══════════════════════════════════════════════════════════════════════════ */

static struct warp_session *g_session_for_signal = NULL;

static BOOL WINAPI CtrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        LogInfo("Shutdown signal received");
        if (g_session_for_signal)
            g_session_for_signal->shutdown_requested = true;
        return TRUE;
    }
    return FALSE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Configuration: defaults → registry → command line
 *
 *   The registry (written by --install) is what an auto-start service
 *   reads at boot; command-line args override it in both modes.
 * ═══════════════════════════════════════════════════════════════════════════ */

bool ParseArgOverrides(int argc, WCHAR *argv[], struct warp_config *cfg)
{
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--id") == 0 && i+1 < argc)
            wcscpy_s(cfg->vm_id, argv[++i]);
        else if (_wcsicmp(argv[i], L"--shm-size") == 0 && i+1 < argc)
            cfg->shm_size = (uint32_t)(_wtoi(argv[++i])) * 1024u * 1024u;
        else if (_wcsicmp(argv[i], L"--ring-size") == 0 && i+1 < argc)
            cfg->ring_data_size = (uint32_t)(_wtoi(argv[++i])) * 1024u;
        else if (_wcsicmp(argv[i], L"--buffer-size") == 0 && i+1 < argc)
            cfg->buffer_size = (uint32_t)(_wtoi(argv[++i])) * 1024u;
        else if (_wcsicmp(argv[i], L"--max-devices") == 0 && i+1 < argc)
            cfg->max_devices = (uint32_t)_wtoi(argv[++i]);
        else if (_wcsicmp(argv[i], L"--iso") == 0)
            cfg->enable_iso = true;
        else if (_wcsicmp(argv[i], L"--debug") == 0)
            cfg->debug = true;
        else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0)
            return false;
        else { LogError("Unknown argument: %ls", argv[i]); return false; }
    }
    return true;
}

static bool RegReadDword(HKEY key, const WCHAR *name, uint32_t *out)
{
    DWORD type, v, size = sizeof(v);
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&v, &size) ==
            ERROR_SUCCESS &&
        type == REG_DWORD) {
        *out = v;
        return true;
    }
    return false;
}

static void LoadRegistryConfig(struct warp_config *cfg)
{
    HKEY key;
    uint32_t v;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, USBWARP_SERVICE_REG_KEY,
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return;

    {
        WCHAR vmId[256];
        DWORD size = sizeof(vmId);
        if (RegQueryValueExW(key, L"VmId", NULL, NULL,
                             (BYTE *)vmId, &size) == ERROR_SUCCESS &&
            size >= sizeof(WCHAR) && vmId[0])
            wcscpy_s(cfg->vm_id, vmId);
    }

    if (RegReadDword(key, L"ShmSizeMb", &v) && v > 0)
        cfg->shm_size = v * 1024u * 1024u;
    if (RegReadDword(key, L"RingSizeKb", &v) && v > 0)
        cfg->ring_data_size = v * 1024u;
    if (RegReadDword(key, L"BufferSizeKb", &v) && v > 0)
        cfg->buffer_size = v * 1024u;
    if (RegReadDword(key, L"MaxDevices", &v) && v > 0)
        cfg->max_devices = v;
    if (RegReadDword(key, L"EnableIso", &v))
        cfg->enable_iso = (v != 0);

    RegCloseKey(key);
}

static bool LoadConfig(int argc, WCHAR *argv[], struct warp_config *cfg)
{
    warp_config_defaults(cfg);
    LoadRegistryConfig(cfg);
    if (!ParseArgOverrides(argc, argv, cfg))
        return false;
    if (cfg->vm_id[0] == L'\0') {
        LogError("VM GUID required: pass --id <VM-GUID> or set "
                 "HKLM\\SOFTWARE\\UsbWarp\\Service\\VmId");
        return false;
    }
    return true;
}

static void PrintUsage(void)
{
    fprintf(stderr,
        "UsbWarp Service — HDV USB passthrough for WSL2\n\n"
        "Usage:\n  UsbWarpService.exe --id <VM-GUID> [options]\n\n"
        "Options:\n"
        "  --id <GUID>            WSL2 VM GUID (required)\n"
        "  --shm-size <MB>        Shared memory size       [default: 32]\n"
        "  --ring-size <KB>       Ring data per direction   [default: 256]\n"
        "  --buffer-size <KB>     Data Region buffer size   [default: 64]\n"
        "  --max-devices <N>      Max passthrough devices   [default: 8]\n"
        "  --iso                  Enable ISO transfer support\n"
        "  --debug                Enable debug logging\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Ring helpers (when operating without Kernel Driver)
 * ═══════════════════════════════════════════════════════════════════════════ */

int ServiceRingConsume(struct warp_session *s, void *msg_out,
                       uint32_t buf_size, uint32_t *msg_len_out)
{
    volatile struct usbwarp_ring_header *r = s->g2h_ring;
    volatile uint8_t *data = s->g2h_ring_data;
    uint32_t ds = r->data_size, dm = r->data_size_mask;
    uint32_t ci = r->consumer_index, pi = r->producer_index;
    struct usbwarp_msg_header hc;
    uint32_t used, pos, first, ml, al;

    *msg_len_out = 0;
    used = usbwarp_ring_used(pi, ci, dm);
    if (used < sizeof(hc)) return -1;

    pos = ci & dm; first = ds - pos;
    if (first >= sizeof(hc)) memcpy(&hc, (const void*)(data+pos), sizeof(hc));
    else { memcpy(&hc, (const void*)(data+pos), first);
           memcpy((uint8_t*)&hc+first, (const void*)data, sizeof(hc)-first); }

    if (hc.magic != USBWARP_MSG_MAGIC) return -2;
    ml = hc.message_length;
    if (ml < sizeof(hc)) return -2;
    al = USBWARP_ALIGN_CACHELINE(ml);
    if (al > used) return -2;
    if (ml > buf_size) { r->consumer_index = ci + al; return -3; }

    pos = ci & dm; first = ds - pos;
    if (first >= ml) memcpy(msg_out, (const void*)(data+pos), ml);
    else { memcpy(msg_out, (const void*)(data+pos), first);
           memcpy((uint8_t*)msg_out+first, (const void*)data, ml-first); }

    MemoryBarrier();
    r->consumer_index = ci + al;
    *msg_len_out = ml;
    s->stats.g2h_messages_consumed++;
    return 0;
}

int ServiceRingProduce(struct warp_session *s, const void *msg, uint32_t ml)
{
    volatile struct usbwarp_ring_header *r = s->h2g_ring;
    volatile uint8_t *data = s->h2g_ring_data;
    uint32_t ds = r->data_size, dm = r->data_size_mask;
    uint32_t pi = r->producer_index, ci = r->consumer_index;
    uint32_t al = USBWARP_ALIGN_CACHELINE(ml);
    uint32_t avail, pos, first;

    avail = usbwarp_ring_available(pi, ci, dm);
    if (avail < al) return -1;

    pos = pi & dm; first = ds - pos;
    if (first >= al) {
        memcpy((void*)(data+pos), msg, ml);
        if (al > ml) memset((void*)(data+pos+ml), 0, al-ml);
    } else if (first >= ml) {
        memcpy((void*)(data+pos), msg, ml);
        if (first > ml) memset((void*)(data+pos+ml), 0, first-ml);
        if (al > first) memset((void*)data, 0, al-first);
    } else {
        memcpy((void*)(data+pos), msg, first);
        memcpy((void*)data, (const uint8_t*)msg+first, ml-first);
        if (al > ml) memset((void*)(data+ml-first), 0, al-ml);
    }

    MemoryBarrier();
    r->producer_index = pi + al;
    s->stats.h2g_messages_produced++;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Device management Ring messages (#5)
 * ═══════════════════════════════════════════════════════════════════════════ */

void ServiceSendDeviceAdded(struct warp_session *s, uint32_t devIdx)
{
    if (!s->mmio_mapped || devIdx >= WARP_MAX_BOUND_DEVICES) return;

    /* P0#1: When driver owns the Ring, it sends DEVICE_ADDED itself
     * via IOCTL_USBWARP_BIND_DEVICE.  Service must not duplicate. */
    if (s->driverShmSetup) return;

    struct warp_bound_device *dev = &s->bound_devices[devIdx];
    struct usbwarp_msg_device_added msg;
    memset(&msg, 0, sizeof(msg));

    msg.hdr.magic            = USBWARP_MSG_MAGIC;
    msg.hdr.message_type     = USBWARP_MSG_DEVICE_ADDED;
    msg.hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
    msg.hdr.message_length   = sizeof(msg);
    msg.hdr.device_id        = dev->device_index;
    msg.hdr.timestamp        = NowNs();

    msg.device_id  = dev->device_index;
    msg.vendor_id  = dev->vendor_id;
    msg.product_id = dev->product_id;
    msg.speed      = USBWARP_SPEED_HIGH;  /* default; actual from USB enum */

    if (ServiceRingProduce(s, &msg, sizeof(msg)) != 0)
        LogError("Failed to send DEVICE_ADDED (H2G ring full)");
    else
        LogInfo("Sent DEVICE_ADDED id=%u to Guest", dev->device_index);
}

void ServiceSendDeviceRemoved(struct warp_session *s, uint32_t devIdx,
                              uint32_t reason)
{
    if (!s->mmio_mapped || devIdx >= WARP_MAX_BOUND_DEVICES) return;

    /* P0#1: When driver owns the Ring, it sends DEVICE_REMOVED itself
     * via IOCTL_USBWARP_UNBIND_DEVICE.  Service must not duplicate. */
    if (s->driverShmSetup) return;

    struct warp_bound_device *dev = &s->bound_devices[devIdx];
    struct usbwarp_msg_device_removed msg;
    memset(&msg, 0, sizeof(msg));

    msg.hdr.magic            = USBWARP_MSG_MAGIC;
    msg.hdr.message_type     = USBWARP_MSG_DEVICE_REMOVED;
    msg.hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
    msg.hdr.message_length   = sizeof(msg);
    msg.hdr.device_id        = dev->device_index;
    msg.hdr.timestamp        = NowNs();

    msg.device_id = dev->device_index;
    msg.reason    = reason;

    if (ServiceRingProduce(s, &msg, sizeof(msg)) != 0)
        LogError("Failed to send DEVICE_REMOVED (H2G ring full)");
    else
        LogInfo("Sent DEVICE_REMOVED id=%u reason=%u to Guest",
                dev->device_index, reason);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Negotiate handler (#10: honest capabilities)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void HandleNegotiate(struct warp_session *s,
                            const struct usbwarp_msg_negotiate *neg)
{
    struct usbwarp_msg_negotiate_resp resp;
    uint16_t version = USBWARP_PROTOCOL_VERSION;

    memset(&resp, 0, sizeof(resp));
    resp.hdr.magic            = USBWARP_MSG_MAGIC;
    resp.hdr.message_type     = USBWARP_MSG_NEGOTIATE_RESP;
    resp.hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
    resp.hdr.message_length   = sizeof(resp);
    resp.hdr.timestamp        = NowNs();

    if (version < neg->min_version || version > neg->max_version) {
        resp.status = -1;
        LogWarn("Negotiate REJECTED: version mismatch");
    } else {
        resp.status             = 0;
        resp.negotiated_version = version;

        /* FIX #10: Only advertise capabilities we actually support.
         * This path runs only when the Service owns the Ring (no Kernel
         * Driver) — the driver answers NEGOTIATE itself otherwise.  ISO
         * comes from the Control Block flag (set from --iso), the same
         * source the driver mode uses, so both hosts advertise alike. */
        uint32_t our_caps = USBWARP_CAP_INLINE_DATA | USBWARP_CAP_STATS;
        if (s->cb && (s->cb->flags & USBWARP_FLAG_ISO_ENABLED))
            our_caps |= USBWARP_CAP_ISO_TRANSFER;
        resp.capabilities     = neg->capabilities & our_caps;
        resp.max_pending_urbs = USBWARP_PENDING_URBS_DEFAULT;
        resp.max_devices      = s->cfg.max_devices;
        s->negotiated = true;
        s->stats.negotiate_count++;

        LogInfo("Negotiate OK: ver=%u.%u caps=0x%x (driver=%s)",
                USBWARP_VERSION_MAJOR(version),
                USBWARP_VERSION_MINOR(version),
                resp.capabilities,
                s->hDriver ? "yes" : "no");
    }

    if (ServiceRingProduce(s, &resp, sizeof(resp)) != 0)
        LogError("Failed to send negotiate response");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  G2H message dispatch
 * ═══════════════════════════════════════════════════════════════════════════ */

static void DispatchG2hMessage(struct warp_session *s,
                               const void *buf, uint32_t len)
{
    const struct usbwarp_msg_header *hdr =
        (const struct usbwarp_msg_header *)buf;

    switch (hdr->message_type) {
    case USBWARP_MSG_NEGOTIATE:
        if (len >= sizeof(struct usbwarp_msg_negotiate))
            HandleNegotiate(s, (const struct usbwarp_msg_negotiate *)buf);
        break;
    case USBWARP_MSG_HEARTBEAT:
        break;
    case USBWARP_MSG_DEVICE_ADD_ACK:
    case USBWARP_MSG_DEVICE_REMOVE_ACK:
        break;
    case USBWARP_MSG_SHUTDOWN_ACK:
        LogInfo("Guest acknowledged shutdown");
        break;
    case USBWARP_MSG_URB_SUBMIT:
        /* #4: Without kernel driver, URBs cannot be proxied. */
        s->stats.total_urb_submits++;
        if (s->cfg.debug)
            LogWarn("URB submit dropped (no driver) txn=%u dev=%u",
                    hdr->transaction_id, hdr->device_id);
        break;
    default:
        if (s->cfg.debug)
            LogWarn("Unknown G2H type %u len=%u", hdr->message_type, len);
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Emergency shutdown (#12)
 * ═══════════════════════════════════════════════════════════════════════════ */

void ServiceEmergencyShutdown(struct warp_session *s)
{
    LogWarn("EMERGENCY SHUTDOWN initiated");

    s->shutdown_requested = true;

    /* P2#6: Stop pipe thread first to prevent new commands. */
    PipeServerStop(s);

    /* Priority 1: Stop accepting new operations. */
    s->negotiated = false;

    /* Priority 2: Unbind all devices (kernel is the source of truth). */
    if (s->hDriver && s->driverShmSetup) {
        DrvClientUnbindAll(s);
    } else {
        for (uint32_t i = 0; i < WARP_MAX_BOUND_DEVICES; i++) {
            if (s->bound_devices[i].in_use) {
                ServiceSendDeviceRemoved(s, i, 1 /* emergency */);
                s->bound_devices[i].in_use = false;
            }
        }
        s->bound_device_count = 0;
    }

    /* Priority 3: Update Control Block. */
    if (s->cb) s->cb->host_state = USBWARP_STATE_ERROR;

    /* Priority 4: Send shutdown to Guest (only if Service owns Ring). */
    if (s->mmio_mapped && !s->driverShmSetup) {
        struct usbwarp_msg_shutdown sd;
        memset(&sd, 0, sizeof(sd));
        sd.hdr.magic            = USBWARP_MSG_MAGIC;
        sd.hdr.message_type     = USBWARP_MSG_HOST_SHUTDOWN;
        sd.hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
        sd.hdr.message_length   = sizeof(sd);
        sd.hdr.timestamp        = NowNs();
        sd.reason               = 1;
        ServiceRingProduce(s, &sd, sizeof(sd));
    }

    /* Priority 5: Close driver connection. */
    DrvClientClose(s);

    LogWarn("Emergency shutdown complete");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Guest heartbeat monitoring (#11)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void CheckGuestHeartbeat(struct warp_session *s)
{
    if (!s->cb || !s->negotiated) return;

    UINT64 now = NowNs();
    if (now - s->guest_heartbeat_last_check_ns < 3000000000ULL)
        return;  /* check every 3 seconds */

    s->guest_heartbeat_last_check_ns = now;

    UINT64 guest_hb = s->cb->guest_heartbeat_ts;
    if (guest_hb == s->last_guest_heartbeat_seen) {
        s->guest_heartbeat_miss_count++;
        if (s->guest_heartbeat_miss_count >= 3) {
            LogWarn("Guest heartbeat timeout (%u consecutive misses)",
                    s->guest_heartbeat_miss_count);
            s->stats.guest_heartbeat_timeouts++;
            /* Don't emergency-shutdown: Guest kernel module might just
             * be slow to load.  Log for diagnostics. */
        }
    } else {
        if (s->guest_heartbeat_miss_count >= 3)
            LogInfo("Guest heartbeat recovered");
        s->last_guest_heartbeat_seen  = guest_hb;
        s->guest_heartbeat_miss_count = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  Main loop
 * ═══════════════════════════════════════════════════════════════════════════ */

static void MainLoop(struct warp_session *s)
{
    LogInfo("Entering main loop (stop signal to shutdown)");

    UINT64 poll_count       = 0;
    UINT64 last_status_ns   = 0;
    UINT64 last_drv_hb_ns   = 0;
    UINT64 last_drv_retry_ns = 0;
    bool   mmio_announced   = false;

    uint8_t msg_buf[4096];

    while (!s->shutdown_requested && !s->hdv_torn_down) {

        /* ── Wait for MemSpace enable or timeout ────────────────────────── */
        if (!s->mmio_mapped && !s->mem_space_on && s->memSpaceEvent)
            WaitForSingleObject(s->memSpaceEvent, 50);

        /* ── Deferred MMIO mapping ──────────────────────────────────────── */
        if (s->mem_space_on && !s->mmio_mapped) {
            if (!HdvSessionMapMmio(s)) {
                LogError("MMIO mapping failed — emergency shutdown");
                ServiceEmergencyShutdown(s);
                break;
            }
            mmio_announced = false;
        }

        if (s->mmio_mapped && s->cb) {
            if (!mmio_announced) {
                LogInfo("SHM online: magic=0x%08X ver=%u.%u total=%uMB buf=%ux%uK",
                        s->cb->magic,
                        s->cb->protocol_version_major,
                        s->cb->protocol_version_minor,
                        s->cb->shm_total_size / (1024*1024),
                        s->cb->buffer_count, s->cb->buffer_size / 1024);
                mmio_announced = true;
            }

            /* ── Host heartbeat (1 second interval, #7) ────────────────── */
            UINT64 now = NowNs();
            if (now - s->last_host_heartbeat_ns >= 1000000000ULL) {
                s->cb->host_heartbeat_ts = now;
                s->cb->host_state        = USBWARP_STATE_RUNNING;
                s->last_host_heartbeat_ns = now;
            }

            /* ── Detect guest RUNNING state (continuous check) ─────────── *
             * Guest negotiate may be handled by the driver poll thread
             * asynchronously.  We can't check just once — there's a race
             * between DrvClientSetupShm returning and the driver's poll
             * thread completing the negotiate handshake.  So we check on
             * every iteration until negotiated becomes true.              */
            if (!s->negotiated && s->cb->guest_state == USBWARP_STATE_RUNNING) {
                LogInfo("Guest entered RUNNING state — session ready");
                s->negotiated = true;
            }

            /* ── Consume G2H Ring messages ──────────────────────────────── *
             * P0#1 FIX: When Kernel Driver has established SHM, the driver
             * owns the Ring (it is the single consumer of G2H and single
             * producer to H2G).  Service must NOT touch the Ring, otherwise
             * the SPSC invariant is broken and data corruption results.
             * Service communicates with driver exclusively via IOCTLs.    */
            if (!s->driverShmSetup) {
                for (int burst = 0; burst < 64; burst++) {
                    uint32_t msg_len = 0;
                    if (ServiceRingConsume(s, msg_buf, sizeof(msg_buf),
                                          &msg_len) != 0 || msg_len == 0)
                        break;
                    DispatchG2hMessage(s, msg_buf, msg_len);
                }
            }

            /* ── Kernel Driver lifecycle (P1#3: periodic retry) ─────────── *
             * If driver is not connected, retry every 60 seconds.
             * This handles: (a) driver loaded after Service starts,
             * (b) driver crash + restart, (c) driver update.            */
            if (!s->hDriver || !s->driverShmSetup) {
                now = NowNs();
                if (now - last_drv_retry_ns >= 60000000000ULL ||
                    last_drv_retry_ns == 0) {
                    last_drv_retry_ns = now;

                    /* Clean up stale state from previous connection. */
                    if (s->hDriver && !s->driverShmSetup)
                        DrvClientClose(s);

                    if (DrvClientOpen(s)) {
                        if (DrvClientRegister(s)) {
                            if (DrvClientSetupShm(s)) {
                                LogInfo("Kernel Driver connected — "
                                        "Ring ownership transferred to driver");
                            } else {
                                LogWarn("Driver SETUP_SHM failed");
                                DrvClientClose(s);
                            }
                        } else {
                            LogWarn("Driver REGISTER failed");
                            DrvClientClose(s);
                        }
                    }
                    /* If DrvClientOpen fails, it's not loaded — will retry. */
                }
            }

            /* ── Driver heartbeat IOCTL (every 2 seconds, #3 orphan) ──── */
            if (s->hDriver && s->driverRegistered) {
                now = NowNs();
                if (now - last_drv_hb_ns >= 2000000000ULL) {
                    DrvClientHeartbeat(s);
                    last_drv_hb_ns = now;
                }
            }

            /* ── Guest heartbeat monitoring (#11) ──────────────────────── */
            CheckGuestHeartbeat(s);

            /* ── Periodic status dump (every 5 seconds) ────────────────── */
            now = NowNs();
            if (now - last_status_ns > 5000000000ULL) {
                LogInfo("poll #%llu  gs=%u  ghb=%llu  "
                        "g2h=%u/%u  h2g=%u/%u  devs=%u  drv=%s",
                        (unsigned long long)poll_count,
                        s->cb->guest_state,
                        (unsigned long long)s->cb->guest_heartbeat_ts,
                        s->g2h_ring->producer_index,
                        s->g2h_ring->consumer_index,
                        s->h2g_ring->producer_index,
                        s->h2g_ring->consumer_index,
                        s->bound_device_count,
                        s->hDriver ? "yes" : "no");
                last_status_ns = now;
            }
        }

        poll_count++;
        Sleep(1);
    }

    LogInfo("Main loop exited after %llu polls", (unsigned long long)poll_count);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Session lifecycle + entry point
 *
 *   Dual-mode binary: launched by the SCM it runs as a real Windows
 *   service (ServiceMain → ServiceRun → SERVICE_STOPPED); run from a
 *   shell it behaves exactly like the old console build.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct warp_session g_sess;

DWORD ServiceRun(struct warp_session *s, bool serviceMode,
                 int argc, WCHAR *argv[])
{
    DWORD exitCode = ERROR_SUCCESS;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    InitializeCriticalSection(&s->deviceLock);
    s->deviceLockInit = true;
    InitializeSRWLock(&s->driverLock);

    if (!LoadConfig(argc, argv, &s->cfg)) {
        if (!serviceMode) PrintUsage();
        exitCode = ERROR_INVALID_PARAMETER;
        goto cleanup;
    }

    if (!serviceMode) {
        g_session_for_signal = s;
        SetConsoleCtrlHandler(CtrlHandler, TRUE);
    }

    s->start_time_ns = NowNs();

    if (!HdvLoadApis(&s->api)) {
        LogError("HCS/HDV APIs unavailable");
        exitCode = ERROR_NOT_SUPPORTED;
        goto cleanup;
    }
    if (!ShmCreate(s)) {
        LogError("SHM creation failed");
        exitCode = ERROR_OUTOFMEMORY;
        goto cleanup;
    }
    SvcCheckpoint();

    /* An auto-start service may boot before the WSL utility VM exists.
     * Wait for it instead of dying — SCM failure-restart would only
     * spin.  Stop requests are honored within 100 ms. */
    while (!HdvSessionOpen(s)) {
        if (s->shutdown_requested) {
            exitCode = ERROR_SERVICE_DEPENDENCY_FAIL;
            goto cleanup;
        }
        LogWarn("HDV session failed — retrying in 10s (VM not running yet?)");
        for (int i = 0; i < 100 && !s->shutdown_requested; i++)
            Sleep(100);
    }
    SvcCheckpoint();

    SvcRunning();

    PipeServerStart(s);
    MainLoop(s);

cleanup:
    LogInfo("Shutting down ...");
    PipeServerStop(s);

    /* Unbind all devices before closing the driver connection — the
     * kernel's device table is the source of truth, so iterate it rather
     * than any Service-side mirror. */
    if (s->hDriver && s->driverShmSetup) {
        DrvClientUnbindAll(s);
    } else {
        for (uint32_t i = 0; i < WARP_MAX_BOUND_DEVICES; i++) {
            if (s->bound_devices[i].in_use) {
                LogInfo("Cleanup: unbinding slot %u", i + 1);
                ServiceSendDeviceRemoved(s, i, 1 /* emergency */);
                s->bound_devices[i].in_use = false;
            }
        }
        s->bound_device_count = 0;
    }

    DrvClientClose(s);
    HdvSessionClose(s);
    ShmDestroy(s);
    HdvUnloadApis(&s->api);
    CoUninitialize();

    if (s->deviceLockInit)
        DeleteCriticalSection(&s->deviceLock);

    LogInfo("Clean shutdown complete.");
    LogCleanup(s);
    return exitCode;
}

int wmain(int argc, WCHAR *argv[])
{
    /* One-shot management commands (require admin). */
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--install") == 0)
            return SvcInstall(argc, argv);
        if (_wcsicmp(argv[i], L"--uninstall") == 0)
            return SvcUninstall();
    }

    /* Service mode when launched by the SCM. */
    {
        int r = RunServiceDispatcher();
        if (r == 0) return 0;         /* ran as a service            */
        if (r != -1) return r;        /* dispatcher hard failure     */
        /* -1: not SCM-launched — fall through to console mode.      */
    }

    fprintf(stderr,
        "\n+----------------------------------------------+\n"
        "|   UsbWarp Service v0.4  (console mode)       |\n"
        "+----------------------------------------------+\n\n");

    LogInit(&g_sess, false);
    return (int)ServiceRun(&g_sess, false, argc, argv);
}
