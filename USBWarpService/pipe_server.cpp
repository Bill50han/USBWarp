/*
 * pipe_server.cpp — UsbWarp Named Pipe server.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * FIX #7:  SendResponse uses heap buffer with size check (was fixed-stack).
 * FIX #8:  ReadFile loop for large messages.
 * FIX #5:  Bind/unbind send Ring messages (DEVICE_ADDED/REMOVED).
 * FIX #13: QUERY_STATS returns collected statistics.
 */

#include "warp_service.h"
#include <stdio.h>
#include <string.h>
#include <sddl.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Response helpers (#7 fix: heap buffer, size check)
 * ═══════════════════════════════════════════════════════════════════════════ */

static BOOL SendResponse(HANDLE hPipe, uint32_t seqId,
                         int32_t status, const void *payload,
                         uint32_t payloadLen)
{
    struct usbwarp_pipe_header hdr;
    hdr.magic          = USBWARP_PIPE_MAGIC;
    hdr.command        = USBWARP_PIPE_CMD_RESPONSE;
    hdr.payload_length = (uint32_t)sizeof(struct usbwarp_pipe_response) + payloadLen;
    hdr.sequence_id    = seqId;

    struct usbwarp_pipe_response resp;
    resp.status        = status;
    resp.detail_length = payloadLen;

    uint32_t totalLen = sizeof(hdr) + sizeof(resp) + payloadLen;

    /* FIX #7: Use heap for large payloads instead of fixed-size stack buffer. */
    BYTE stackBuf[1024];
    BYTE *buf = stackBuf;
    bool heapAllocated = false;

    if (totalLen > sizeof(stackBuf)) {
        buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, totalLen);
        if (!buf) return FALSE;
        heapAllocated = true;
    }

    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), &resp, sizeof(resp));
    if (payloadLen > 0 && payload)
        memcpy(buf + sizeof(hdr) + sizeof(resp), payload, payloadLen);

    DWORD written;
    BOOL ok = WriteFile(hPipe, buf, totalLen, &written, NULL);

    if (heapAllocated) HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

static BOOL SendSimple(HANDLE hPipe, uint32_t seqId, int32_t status,
                       const char *detail)
{
    uint32_t detailLen = detail ? (uint32_t)strlen(detail) : 0;
    return SendResponse(hPipe, seqId, status, detail, detailLen);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Command handlers
 *
 *   Driver mode: the kernel is the single source of truth — LIST/QUERY_*
 *   take a fresh ENUM/QUERY snapshot per request, BIND/UNBIND are pure
 *   GUID-keyed pass-throughs (the kernel performs duplicate detection and
 *   assigns the DeviceIndex).  The bound_devices[] mirror is consulted
 *   only in the no-driver debug path.
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool DriverActive(struct warp_session *s)
{
    return s->hDriver && s->driverShmSetup;
}

static void HandleQueryStatus(struct warp_session *s, HANDLE hPipe,
                              uint32_t seqId)
{
    struct usbwarp_pipe_status st;
    memset(&st, 0, sizeof(st));

    st.session_state = s->mmio_mapped ? 2 : (s->hdv_started ? 1 : 0);

    if (DriverActive(s)) {
        USBWARP_QUERY_STATUS_OUT q;
        if (DrvClientQueryStatus(s, &q)) {
            st.bound_device_count       = q.ActiveDeviceCount;
            st.active_device_count      = q.ActiveDeviceCount;
            st.orphan_mode              = q.OrphanMode;
            st.total_urbs_processed     = q.TotalUrbsProcessed;
            st.total_bytes_transferred  = q.TotalBytesTransferred;
        } else {
            LogWarn("QUERY_STATUS: driver query failed");
        }
    } else {
        st.bound_device_count  = s->bound_device_count;
        st.active_device_count = s->bound_device_count;
        st.total_urbs_processed    = s->stats.total_urb_submits;
        st.total_bytes_transferred = s->stats.total_bytes_out +
                                     s->stats.total_bytes_in;
    }

    UINT64 now = NowNs();
    st.uptime_seconds = (now - s->start_time_ns) / 1000000000ULL;

    SendResponse(hPipe, seqId, 0, &st, sizeof(st));
}

/* FIX #13: Return collected statistics (driver counters in driver mode). */
static void HandleQueryStats(struct warp_session *s, HANDLE hPipe,
                             const BYTE *payload, uint32_t payloadLen,
                             uint32_t seqId)
{
    struct usbwarp_pipe_stats_response sr;
    memset(&sr, 0, sizeof(sr));

    if (DriverActive(s)) {
        const struct usbwarp_pipe_stats_request *req =
            (payloadLen >= sizeof(*req)) ?
            (const struct usbwarp_pipe_stats_request *)payload : NULL;

        bool anyGuid = false;
        if (req) {
            for (int i = 0; i < 16; i++)
                if (req->device_guid[i]) { anyGuid = true; break; }
        }

        if (anyGuid) {
            /* Per-device stats from the authoritative snapshot.  The kernel
             * tracks a combined byte counter — bytes_in stays 0. */
            USBWARP_ENUM_DEVICES_OUT en;
            if (DrvClientEnumDevices(s, &en)) {
                for (uint32_t i = 0; i < en.DeviceCount; i++) {
                    if (memcmp(en.Devices[i].DeviceGuid,
                               req->device_guid, 16) == 0) {
                        sr.urb_submit_count = en.Devices[i].TotalUrbs;
                        sr.bytes_out        = en.Devices[i].TotalBytes;
                        break;
                    }
                }
            }
        } else {
            USBWARP_QUERY_STATUS_OUT q;
            if (DrvClientQueryStatus(s, &q)) {
                sr.urb_submit_count   = q.TotalUrbsProcessed;
                sr.bytes_out          = q.TotalBytesTransferred;
                sr.ring_usage_percent = q.RingG2HUsagePercent;
            }
        }
    } else {
        sr.urb_submit_count   = s->stats.total_urb_submits;
        sr.urb_complete_count = s->stats.total_urb_completes;
        sr.urb_cancel_count   = s->stats.total_urb_cancels;
        sr.urb_error_count    = s->stats.total_urb_errors;
        sr.bytes_out          = s->stats.total_bytes_out;
        sr.bytes_in           = s->stats.total_bytes_in;

        if (s->mmio_mapped && s->g2h_ring && s->h2g_ring) {
            uint32_t g2h_used = usbwarp_ring_used(
                s->g2h_ring->producer_index,
                s->g2h_ring->consumer_index,
                s->g2h_ring->data_size_mask);
            sr.ring_usage_percent = s->g2h_ring->data_size ?
                (uint32_t)((uint64_t)g2h_used * 100 /
                           s->g2h_ring->data_size) : 0;
        }
    }

    SendResponse(hPipe, seqId, 0, &sr, sizeof(sr));
}

static void HandleQueryDevice(struct warp_session *s, HANDLE hPipe,
                              const BYTE *payload, uint32_t payloadLen,
                              uint32_t seqId)
{
    if (payloadLen < sizeof(struct usbwarp_pipe_query_device_request)) {
        SendSimple(hPipe, seqId, -1, "payload too small");
        return;
    }

    const struct usbwarp_pipe_query_device_request *req =
        (const struct usbwarp_pipe_query_device_request *)payload;

    if (!DriverActive(s)) {
        SendSimple(hPipe, seqId, -1, "driver not connected");
        return;
    }

    USBWARP_ENUM_DEVICES_OUT en;
    if (!DrvClientEnumDevices(s, &en)) {
        SendSimple(hPipe, seqId, -1, "driver query failed");
        return;
    }

    for (uint32_t i = 0; i < en.DeviceCount; i++) {
        const USBWARP_ENUM_DEVICE_ENTRY *e = &en.Devices[i];

        if (memcmp(e->DeviceGuid, req->device_guid, 16) != 0)
            continue;

        struct usbwarp_pipe_query_device_response resp;
        memset(&resp, 0, sizeof(resp));
        resp.status                = 0;
        memcpy(resp.device_guid, e->DeviceGuid, 16);
        resp.vendor_id             = e->VendorId;
        resp.product_id            = e->ProductId;
        resp.speed                 = e->Speed;
        resp.bound                 = 1;
        resp.circuit_breaker_state = e->BreakerState;
        resp.pending_urb_count     = e->PendingCount;
        resp.total_bytes           = e->TotalBytes;
        /* avg_latency_us / error_rate_60s_permille: no kernel source
         * yet — left 0 rather than invented. */
        SendResponse(hPipe, seqId, 0, &resp, sizeof(resp));
        return;
    }

    SendSimple(hPipe, seqId, -1, "device not found");
}

static void HandleListDevices(struct warp_session *s, HANDLE hPipe,
                              uint32_t seqId)
{
    struct usbwarp_pipe_device_entry entries[WARP_MAX_BOUND_DEVICES];
    uint32_t count = 0;

    memset(entries, 0, sizeof(entries));

    if (DriverActive(s)) {
        USBWARP_ENUM_DEVICES_OUT en;
        if (!DrvClientEnumDevices(s, &en)) {
            SendSimple(hPipe, seqId, -1, "driver query failed");
            return;
        }
        for (uint32_t i = 0; i < en.DeviceCount &&
                            count < WARP_MAX_BOUND_DEVICES; i++) {
            memcpy(entries[count].device_guid, en.Devices[i].DeviceGuid, 16);
            entries[count].vendor_id  = en.Devices[i].VendorId;
            entries[count].product_id = en.Devices[i].ProductId;
            entries[count].bound      = 1;
            count++;
        }
    } else {
        /* No-driver debug path: the Service-owned mirror is the truth. */
        EnterCriticalSection(&s->deviceLock);
        for (uint32_t i = 0; i < WARP_MAX_BOUND_DEVICES; i++) {
            if (!s->bound_devices[i].in_use) continue;
            memcpy(entries[count].device_guid,
                   s->bound_devices[i].device_guid, 16);
            entries[count].vendor_id  = s->bound_devices[i].vendor_id;
            entries[count].product_id = s->bound_devices[i].product_id;
            entries[count].bound      = 1;
            count++;
        }
        LeaveCriticalSection(&s->deviceLock);
    }

    struct usbwarp_pipe_device_list list;
    list.device_count = count;

    uint32_t payloadLen = sizeof(list) + count * sizeof(entries[0]);
    BYTE *payload = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       payloadLen);
    if (!payload) {
        SendSimple(hPipe, seqId, -1, "out of memory");
        return;
    }

    memcpy(payload, &list, sizeof(list));
    if (count > 0)
        memcpy(payload + sizeof(list), entries,
               count * sizeof(entries[0]));

    SendResponse(hPipe, seqId, 0, payload, payloadLen);
    HeapFree(GetProcessHeap(), 0, payload);
}

/* Convert Win32 device path prefix \\?\ to NT kernel \??\ in place.
 * WdfIoTargetOpen needs the NT-style path for kernel access. */
static void NormalizeDevicePath(WCHAR *path)
{
    if (path[0] == L'\\' && path[1] == L'\\' &&
        path[2] == L'?'  && path[3] == L'\\')
        path[1] = L'?';
}

/* Bind request shared by both modes: extracts and converts the instance
 * path.  Returns FALSE (and replies) on malformed input. */
static bool ParseBindRequest(const BYTE *payload, uint32_t payloadLen,
                             uint32_t seqId, HANDLE hPipe,
                             const struct usbwarp_pipe_bind_request **reqOut,
                             WCHAR widePath[256])
{
    if (payloadLen < sizeof(struct usbwarp_pipe_bind_request)) {
        SendSimple(hPipe, seqId, -1, "payload too small");
        return false;
    }

    const struct usbwarp_pipe_bind_request *req =
        (const struct usbwarp_pipe_bind_request *)payload;

    uint16_t instancePathLen = req->instance_path_length;
    const char *instancePath =
        (instancePathLen > 0 && payloadLen >= sizeof(*req) + instancePathLen) ?
        (const char *)(payload + sizeof(*req)) : NULL;

    if (!instancePath) {
        SendSimple(hPipe, seqId, -1, "missing instance path");
        return false;
    }

    int wideLen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      instancePath, (int)instancePathLen,
                                      widePath, 255);
    if (wideLen <= 0) {
        SendSimple(hPipe, seqId, -1, "invalid UTF-8 instance path");
        return false;
    }
    widePath[wideLen] = L'\0';
    NormalizeDevicePath(widePath);

    *reqOut = req;
    return true;
}

/* Driver mode: pure pass-through — the kernel performs duplicate detection
 * (ContainerId + GUID) and assigns the DeviceIndex.  No service-side slot. */
static void HandleBindDevice(struct warp_session *s, HANDLE hPipe,
                             const BYTE *payload, uint32_t payloadLen,
                             uint32_t seqId)
{
    if (!s->mmio_mapped || !s->negotiated) {
        SendSimple(hPipe, seqId, -1, "session not ready");
        return;
    }

    const struct usbwarp_pipe_bind_request *req;
    WCHAR widePath[256];

    if (!ParseBindRequest(payload, payloadLen, seqId, hPipe, &req, widePath))
        return;

    if (DriverActive(s)) {
        uint32_t deviceIndex;
        uint16_t vid, pid;

        if (!DrvClientBindGuid(s, req->device_guid, widePath,
                               &deviceIndex, &vid, &pid)) {
            SendSimple(hPipe, seqId, -1, "bind failed (driver rejected)");
            return;
        }
        /* Driver sends DEVICE_ADDED to the Guest itself (P0#1). */

        char detail[64];
        snprintf(detail, sizeof(detail), "bound to slot %u", deviceIndex);
        SendSimple(hPipe, seqId, 0, detail);

        LogInfo("Device bound: idx=%u VID=%04x PID=%04x guid=%02x%02x%02x%02x...",
                deviceIndex, vid, pid,
                req->device_guid[0], req->device_guid[1],
                req->device_guid[2], req->device_guid[3]);
        return;
    }

    /* Service-only debug path: tentative mirror slot, then Ring message. */
    EnterCriticalSection(&s->deviceLock);

    int freeSlot = -1;
    for (uint32_t i = 0; i < WARP_MAX_BOUND_DEVICES; i++) {
        if ((s->bound_devices[i].in_use || s->bound_devices[i].binding) &&
            memcmp(s->bound_devices[i].device_guid, req->device_guid, 16) == 0) {
            LeaveCriticalSection(&s->deviceLock);
            SendSimple(hPipe, seqId, -1, "device already bound");
            return;
        }
        if (freeSlot < 0 &&
            !s->bound_devices[i].in_use && !s->bound_devices[i].binding) {
            freeSlot = (int)i;
        }
    }

    if (freeSlot < 0) {
        LeaveCriticalSection(&s->deviceLock);
        SendSimple(hPipe, seqId, -1, "no free device slots");
        return;
    }

    /* Tentatively populate the slot (not yet committed). */
    struct warp_bound_device *dev = &s->bound_devices[freeSlot];
    memset(dev, 0, sizeof(*dev));
    dev->binding = true;
    dev->device_index = (uint32_t)(freeSlot + 1);
    memcpy(dev->device_guid, req->device_guid, 16);
    wcscpy_s(dev->instance_path, widePath);

    LeaveCriticalSection(&s->deviceLock);

    /* We own the Ring in this mode: send DEVICE_ADDED ourselves. */
    ServiceSendDeviceAdded(s, (uint32_t)freeSlot);

    EnterCriticalSection(&s->deviceLock);
    dev->binding = false;
    dev->in_use = true;
    s->bound_device_count++;
    LeaveCriticalSection(&s->deviceLock);

    char detail[64];
    snprintf(detail, sizeof(detail), "bound to slot %d", freeSlot + 1);
    SendSimple(hPipe, seqId, 0, detail);

    LogInfo("Device bound (service-only): slot=%d guid=%02x%02x%02x%02x...",
            freeSlot + 1,
            req->device_guid[0], req->device_guid[1],
            req->device_guid[2], req->device_guid[3]);
}

/* Driver mode: GUID-keyed pass-through.  A kernel refusal for a device the
 * authoritative snapshot no longer lists (hot-remove already freed the
 * slot) is success, not an error. */
static void HandleUnbindDevice(struct warp_session *s, HANDLE hPipe,
                               const BYTE *payload, uint32_t payloadLen,
                               uint32_t seqId)
{
    if (payloadLen < sizeof(struct usbwarp_pipe_bind_request)) {
        SendSimple(hPipe, seqId, -1, "payload too small");
        return;
    }

    const struct usbwarp_pipe_bind_request *req =
        (const struct usbwarp_pipe_bind_request *)payload;

    if (DriverActive(s)) {
        if (!DrvClientUnbindGuid(s, req->device_guid)) {
            USBWARP_ENUM_DEVICES_OUT en;
            bool stillBound = false;

            if (DrvClientEnumDevices(s, &en)) {
                for (uint32_t i = 0; i < en.DeviceCount; i++) {
                    if (memcmp(en.Devices[i].DeviceGuid,
                               req->device_guid, 16) == 0) {
                        stillBound = true;
                        break;
                    }
                }
            }

            if (stillBound) {
                SendSimple(hPipe, seqId, -1, "unbind failed (driver rejected)");
                return;
            }
            /* Kernel doesn't hold it anymore (hot-remove freed the slot). */
            SendSimple(hPipe, seqId, 0, "unbound (already gone)");
            return;
        }

        SendSimple(hPipe, seqId, 0, "unbound");
        LogInfo("Device unbound (driver mode)");
        return;
    }

    /* Service-only debug path: mirror lookup + Service-owned Ring message. */
    EnterCriticalSection(&s->deviceLock);

    int foundSlot = -1;
    for (uint32_t i = 0; i < WARP_MAX_BOUND_DEVICES; i++) {
        if (s->bound_devices[i].in_use &&
            memcmp(s->bound_devices[i].device_guid, req->device_guid, 16) == 0) {
            foundSlot = (int)i;
            break;
        }
    }

    if (foundSlot < 0) {
        LeaveCriticalSection(&s->deviceLock);
        SendSimple(hPipe, seqId, -1, "device not found");
        return;
    }

    LeaveCriticalSection(&s->deviceLock);

    ServiceSendDeviceRemoved(s, (uint32_t)foundSlot, 0);

    EnterCriticalSection(&s->deviceLock);
    memset(&s->bound_devices[foundSlot], 0, sizeof(s->bound_devices[foundSlot]));
    if (s->bound_device_count > 0) s->bound_device_count--;
    LeaveCriticalSection(&s->deviceLock);

    SendSimple(hPipe, seqId, 0, "unbound");
    LogInfo("Device unbound: slot=%d", foundSlot + 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Connection handler (#8 fix: handle large messages)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* FIX #8: Use larger buffer and handle MESSAGE pipe semantics properly. */
#define PIPE_READ_BUF_SIZE  65536

static void HandleConnection(struct warp_session *s, HANDLE hPipe)
{
    BYTE *buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, PIPE_READ_BUF_SIZE);
    if (!buf) return;

    while (!s->shutdown_requested) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hPipe, buf, PIPE_READ_BUF_SIZE, &bytesRead, NULL);

        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_MORE_DATA) {
                /* FIX #8: Message was truncated.  In message mode, the
                 * excess data is lost.  Send error response. */
                LogWarn("Pipe: message too large (>%u bytes)", PIPE_READ_BUF_SIZE);
            }
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA)
                break;
            if (err != ERROR_MORE_DATA)
                break;
        }

        if (bytesRead < sizeof(struct usbwarp_pipe_header))
            continue;

        struct usbwarp_pipe_header *hdr = (struct usbwarp_pipe_header *)buf;
        if (hdr->magic != USBWARP_PIPE_MAGIC) {
            LogWarn("Pipe: bad magic 0x%x", hdr->magic);
            continue;
        }

        uint32_t payloadLen = hdr->payload_length;
        const BYTE *payload = buf + sizeof(*hdr);

        if (sizeof(*hdr) + payloadLen > bytesRead) {
            SendSimple(hPipe, hdr->sequence_id, -1, "truncated");
            continue;
        }

        switch (hdr->command) {
        case USBWARP_PIPE_CMD_QUERY_STATUS:
            HandleQueryStatus(s, hPipe, hdr->sequence_id);
            break;
        case USBWARP_PIPE_CMD_QUERY_DEVICE:
            HandleQueryDevice(s, hPipe, payload, payloadLen, hdr->sequence_id);
            break;
        case USBWARP_PIPE_CMD_QUERY_STATS:
            HandleQueryStats(s, hPipe, payload, payloadLen, hdr->sequence_id);
            break;
        case USBWARP_PIPE_CMD_LIST_DEVICES:
            HandleListDevices(s, hPipe, hdr->sequence_id);
            break;
        case USBWARP_PIPE_CMD_BIND_DEVICE:
            HandleBindDevice(s, hPipe, payload, payloadLen, hdr->sequence_id);
            break;
        case USBWARP_PIPE_CMD_UNBIND_DEVICE:
            HandleUnbindDevice(s, hPipe, payload, payloadLen, hdr->sequence_id);
            break;
        default:
            SendSimple(hPipe, hdr->sequence_id, -1, "unknown command");
            break;
        }
    }

    HeapFree(GetProcessHeap(), 0, buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Pipe server thread
 * ═══════════════════════════════════════════════════════════════════════════ */

static DWORD WINAPI PipeServerThread(LPVOID param)
{
    struct warp_session *s = (struct warp_session *)param;
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR pSD = NULL;

    LogInfo("Named Pipe server started: %s", USBWARP_PIPE_NAME);

    /* Create a security descriptor that allows any authenticated user
     * to read/write the pipe.  Without this, if Service runs elevated
     * and Controller runs as normal user (or vice versa), CreateFile
     * returns ERROR_ACCESS_DENIED (5). */
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GRGW;;;AU)",  /* Allow Authenticated Users read+write */
            SDDL_REVISION_1, &pSD, NULL)) {
        LogWarn("Failed to create pipe security descriptor (%lu) — "
                "using default (may cause access denied)", GetLastError());
    }

    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle       = FALSE;

    while (!s->shutdown_requested) {
        HANDLE hPipe = CreateNamedPipeA(
            USBWARP_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, PIPE_READ_BUF_SIZE, PIPE_READ_BUF_SIZE, 1000,
            pSD ? &sa : NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            LogError("CreateNamedPipe failed: %lu", GetLastError());
            Sleep(1000);
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, NULL) ||
                         (GetLastError() == ERROR_PIPE_CONNECTED);

        if (connected && !s->shutdown_requested) {
            LogInfo("Pipe: client connected");
            HandleConnection(s, hPipe);
            LogInfo("Pipe: client disconnected");
            FlushFileBuffers(hPipe);
            DisconnectNamedPipe(hPipe);
        }
        CloseHandle(hPipe);
    }

    LogInfo("Named Pipe server stopped");

    if (pSD) LocalFree(pSD);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL PipeServerStart(struct warp_session *s)
{
    s->pipeRunning = true;
    s->pipeThread = CreateThread(NULL, 0, PipeServerThread, s, 0, NULL);
    if (!s->pipeThread) {
        LogError("Pipe server thread failed: %lu", GetLastError());
        s->pipeRunning = false;
        return FALSE;
    }
    return TRUE;
}

void PipeServerStop(struct warp_session *s)
{
    if (!s->pipeThread) return;
    s->pipeRunning = false;

    /* Cancel the pipe thread's blocking ConnectNamedPipe/ReadFile in one
     * call — no overlapped rework needed.  The thread observes
     * shutdown_requested and exits its loops. */
    CancelSynchronousIo(s->pipeThread);

    if (WaitForSingleObject(s->pipeThread, 5000) == WAIT_OBJECT_0) {
        CloseHandle(s->pipeThread);
        s->pipeThread = NULL;
    } else {
        /* Never close a handle a live thread may still be using —
         * leak it and log.  (The old code closed it unconditionally.) */
        LogWarn("Pipe thread did not stop within 5s — handle leaked");
    }
}
