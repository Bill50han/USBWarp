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
 * ═══════════════════════════════════════════════════════════════════════════ */

static void HandleQueryStatus(struct warp_session *s, HANDLE hPipe,
                              uint32_t seqId)
{
    struct usbwarp_pipe_status st;
    memset(&st, 0, sizeof(st));

    st.session_state       = s->mmio_mapped ? 2 : (s->hdv_started ? 1 : 0);
    st.bound_device_count  = s->bound_device_count;
    st.active_device_count = s->bound_device_count;
    st.orphan_mode         = 0;

    UINT64 now = NowNs();
    st.uptime_seconds          = (now - s->start_time_ns) / 1000000000ULL;
    st.total_urbs_processed    = s->stats.total_urb_submits;
    st.total_bytes_transferred = s->stats.total_bytes_out + s->stats.total_bytes_in;

    SendResponse(hPipe, seqId, 0, &st, sizeof(st));
}

/* FIX #13: Return collected statistics. */
static void HandleQueryStats(struct warp_session *s, HANDLE hPipe,
                             uint32_t seqId)
{
    struct usbwarp_pipe_stats_response sr;
    memset(&sr, 0, sizeof(sr));

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
            (uint32_t)((uint64_t)g2h_used * 100 / s->g2h_ring->data_size) : 0;
    }

    SendResponse(hPipe, seqId, 0, &sr, sizeof(sr));
}

static void HandleListDevices(struct warp_session *s, HANDLE hPipe,
                              uint32_t seqId)
{
    /* Build device list from bound_devices array. */
    EnterCriticalSection(&s->deviceLock);

    struct usbwarp_pipe_device_list list;
    list.device_count = s->bound_device_count;

    /* For simplicity, send just the count header + entries inline.
     * Each entry is fixed-size with empty description. */
    uint32_t payloadLen = sizeof(list);
    for (uint32_t i = 0; i < WARP_MAX_BOUND_DEVICES; i++) {
        if (s->bound_devices[i].in_use)
            payloadLen += sizeof(struct usbwarp_pipe_device_entry);
    }

    BYTE *payload = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       payloadLen);
    if (!payload) {
        LeaveCriticalSection(&s->deviceLock);
        SendSimple(hPipe, seqId, -1, "out of memory");
        return;
    }

    memcpy(payload, &list, sizeof(list));
    BYTE *cursor = payload + sizeof(list);

    for (uint32_t i = 0; i < WARP_MAX_BOUND_DEVICES; i++) {
        if (!s->bound_devices[i].in_use) continue;
        struct usbwarp_pipe_device_entry entry;
        memset(&entry, 0, sizeof(entry));
        memcpy(entry.device_guid, s->bound_devices[i].device_guid, 16);
        entry.vendor_id  = s->bound_devices[i].vendor_id;
        entry.product_id = s->bound_devices[i].product_id;
        entry.bound      = 1;
        memcpy(cursor, &entry, sizeof(entry));
        cursor += sizeof(entry);
    }

    LeaveCriticalSection(&s->deviceLock);
    SendResponse(hPipe, seqId, 0, payload, payloadLen);
    HeapFree(GetProcessHeap(), 0, payload);
}

/* P0#2: Atomic bind — allocate slot tentatively, then attempt driver IOCTL
 * and Ring message.  Only finalize on success; rollback on any failure.    */
static void HandleBindDevice(struct warp_session *s, HANDLE hPipe,
                             const BYTE *payload, uint32_t payloadLen,
                             uint32_t seqId)
{
    if (payloadLen < sizeof(struct usbwarp_pipe_bind_request)) {
        SendSimple(hPipe, seqId, -1, "payload too small");
        return;
    }

    if (!s->mmio_mapped || !s->negotiated) {
        SendSimple(hPipe, seqId, -1, "session not ready");
        return;
    }

    const struct usbwarp_pipe_bind_request *req =
        (const struct usbwarp_pipe_bind_request *)payload;

    /* Extract instance path from variable-length payload. */
    const char *instancePath = NULL;
    uint16_t    instancePathLen = req->instance_path_length;

    if (instancePathLen > 0 &&
        payloadLen >= sizeof(*req) + instancePathLen) {
        instancePath = (const char *)(payload + sizeof(*req));
    }

    if (!instancePath || instancePathLen == 0) {
        SendSimple(hPipe, seqId, -1, "missing instance path");
        return;
    }

    /* Find a free slot (tentative allocation under lock). */
    EnterCriticalSection(&s->deviceLock);

    int freeSlot = -1;
    for (uint32_t i = 0; i < WARP_MAX_BOUND_DEVICES; i++) {
        if (!s->bound_devices[i].in_use) { freeSlot = (int)i; break; }
    }

    if (freeSlot < 0) {
        LeaveCriticalSection(&s->deviceLock);
        SendSimple(hPipe, seqId, -1, "no free device slots");
        return;
    }

    /* Tentatively populate the slot (not yet committed). */
    struct warp_bound_device *dev = &s->bound_devices[freeSlot];
    memset(dev, 0, sizeof(*dev));
    dev->device_index = (uint32_t)(freeSlot + 1);
    memcpy(dev->device_guid, req->device_guid, 16);

    /* Convert device path from UTF-8 to wide string for driver. */
    MultiByteToWideChar(CP_UTF8, 0, instancePath, (int)instancePathLen,
                        dev->instance_path, 255);
    dev->instance_path[255] = L'\0';

    /* Convert Win32 device path prefix \\?\ to NT kernel \??\
     * WdfIoTargetOpen needs NT-style path for kernel access. */
    if (dev->instance_path[0] == L'\\' &&
        dev->instance_path[1] == L'\\' &&
        dev->instance_path[2] == L'?' &&
        dev->instance_path[3] == L'\\') {
        dev->instance_path[1] = L'?';
    }

    /* Mark in_use LAST, after all fields are set. */
    dev->in_use = true;
    s->bound_device_count++;

    LeaveCriticalSection(&s->deviceLock);

    /* Step 1: If Kernel Driver is available, bind via IOCTL. */
    if (s->hDriver && s->driverShmSetup) {
        if (!DrvClientBindDevice(s, (uint32_t)freeSlot)) {
            LogError("Driver BIND_DEVICE failed — rolling back");
            goto rollback;
        }
        /* Driver will send DEVICE_ADDED to Guest via its own Ring produce.
         * Service must NOT also produce (P0#1). */
    } else {
        /* Service-only mode: we own the Ring, send DEVICE_ADDED. */
        ServiceSendDeviceAdded(s, (uint32_t)freeSlot);
    }

    {
        char detail[64];
        snprintf(detail, sizeof(detail), "bound to slot %d", freeSlot + 1);
        SendSimple(hPipe, seqId, 0, detail);
    }

    LogInfo("Device bound: slot=%d guid=%02x%02x%02x%02x...",
            freeSlot + 1,
            req->device_guid[0], req->device_guid[1],
            req->device_guid[2], req->device_guid[3]);
    return;

rollback:
    EnterCriticalSection(&s->deviceLock);
    dev->in_use = false;
    if (s->bound_device_count > 0) s->bound_device_count--;
    LeaveCriticalSection(&s->deviceLock);
    SendSimple(hPipe, seqId, -1, "bind failed (driver rejected)");
}

/* P0#2: Atomic unbind — driver IOCTL first (it sends DEVICE_REMOVED),
 * then Service sends Ring message only in service-only mode.              */
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

    /* Driver mode: IOCTL handles Ring message + USB device close. */
    if (s->hDriver && s->driverShmSetup)
        DrvClientUnbindDevice(s, (uint32_t)foundSlot);
    else
        ServiceSendDeviceRemoved(s, (uint32_t)foundSlot, 0);

    EnterCriticalSection(&s->deviceLock);
    s->bound_devices[foundSlot].in_use = false;
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
        case USBWARP_PIPE_CMD_QUERY_STATS:
            HandleQueryStats(s, hPipe, hdr->sequence_id);
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

    HANDLE hDummy = CreateFileA(USBWARP_PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hDummy != INVALID_HANDLE_VALUE) CloseHandle(hDummy);

    WaitForSingleObject(s->pipeThread, 3000);
    CloseHandle(s->pipeThread);
    s->pipeThread = NULL;
}
