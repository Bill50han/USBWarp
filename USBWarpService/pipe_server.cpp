// SPDX-License-Identifier: GPL-2.0-only
/* Local control plane. Queries use kernel snapshots; only administrators may
 * change bindings. No service-owned device table or transport-ring access. */
#include "warp_service.h"
#include <stdio.h>
#include <sddl.h>
#include <new>

namespace {
constexpr DWORD MaxFrame = 65536;
constexpr DWORD ClientTimeout = 30000;

BOOL Transfer(warp_session *s, HANDLE pipe, void *buffer, DWORD size, DWORD *bytes, bool write)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return FALSE;
    BOOL ok = write ? WriteFile(pipe, buffer, size, bytes, &ov) : ReadFile(pipe, buffer, size, bytes, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_PENDING) {
        HANDLE events[] = {ov.hEvent, s->stopEvent};
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, ClientTimeout);
        if (wait != WAIT_OBJECT_0) CancelIoEx(pipe, &ov);
        ok = GetOverlappedResult(pipe, &ov, bytes, TRUE);
        error = ok ? ERROR_SUCCESS : GetLastError();
        if (wait != WAIT_OBJECT_0 && ok) { ok = FALSE; error = ERROR_OPERATION_ABORTED; }
    }
    CloseHandle(ov.hEvent);
    SetLastError(error);
    return ok && !StopRequested(s);
}

BOOL Respond(warp_session *s, HANDLE pipe, uint32_t seq, int32_t status, const void *payload, uint32_t size)
{
    if (size > MaxFrame - sizeof(usbwarp_pipe_header) - sizeof(usbwarp_pipe_response)) return FALSE;
    DWORD total = (DWORD)(sizeof(usbwarp_pipe_header) + sizeof(usbwarp_pipe_response)) + size;
    BYTE *data = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (!data) return FALSE;
    usbwarp_pipe_header header{USBWARP_PIPE_MAGIC, USBWARP_PIPE_CMD_RESPONSE,
        (uint32_t)sizeof(usbwarp_pipe_response) + size, seq};
    usbwarp_pipe_response response{status, size};
    memcpy(data, &header, sizeof(header));
    memcpy(data + sizeof(header), &response, sizeof(response));
    if (size) memcpy(data + sizeof(header) + sizeof(response), payload, size);
    DWORD written = 0;
    BOOL ok = Transfer(s, pipe, data, total, &written, true) && written == total;
    HeapFree(GetProcessHeap(), 0, data);
    return ok;
}
BOOL Error(warp_session *s, HANDLE pipe, uint32_t seq, DWORD code, const char *message)
{
    char text[256];
    snprintf(text, sizeof(text), "%s (error %lu)", message, code);
    return Respond(s, pipe, seq, -(int32_t)(code ? code : ERROR_GEN_FAILURE), text, (uint32_t)strlen(text));
}
BOOL Simple(warp_session *s, HANDLE pipe, uint32_t seq, const char *message)
{
    return Respond(s, pipe, seq, 0, message, (uint32_t)strlen(message));
}

bool Administrator(warp_session *s, HANDLE pipe)
{
    if (!ImpersonateNamedPipeClient(pipe)) return false;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr, system = nullptr;
    BOOL admin = FALSE, localSystem = FALSE;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &administrators)) {
        CheckTokenMembership(nullptr, administrators, &admin);
        FreeSid(administrators);
    }
    if (AllocateAndInitializeSid(&authority, 1, SECURITY_LOCAL_SYSTEM_RID,
            0,0,0,0,0,0,0, &system)) {
        CheckTokenMembership(nullptr, system, &localSystem);
        FreeSid(system);
    }
    if (!RevertToSelf()) { RequestStop(s, ERROR_ACCESS_DENIED); return false; }
    return admin || localSystem;
}

BOOL Status(warp_session *s, HANDLE pipe, uint32_t seq)
{
    usbwarp_pipe_status status{};
    status.session_state = s->stage.load();
    status.uptime_seconds = (NowNs() - s->start_time_ns) / 1000000000ULL;
    if (status.session_state == USBWARP_SESSION_RUNNING || status.session_state == USBWARP_SESSION_WAIT_GUEST) {
        USBWARP_QUERY_STATUS_OUT kernel{};
        if (!DrvClientQueryStatus(s, &kernel)) return Error(s, pipe, seq, GetLastError(), "driver status unavailable");
        status.bound_device_count = status.active_device_count = kernel.ActiveDeviceCount;
        status.orphan_mode = kernel.OrphanMode;
        status.total_urbs_processed = kernel.TotalUrbsProcessed;
        status.total_bytes_transferred = kernel.TotalBytesTransferred;
        if (kernel.DriverState == USBWARP_STATE_ERROR) status.session_state = USBWARP_SESSION_ERROR;
    }
    return Respond(s, pipe, seq, 0, &status, sizeof(status));
}

BOOL Dispatch(warp_session *s, HANDLE pipe, const usbwarp_pipe_header &header, const BYTE *payload)
{
    uint32_t size = header.payload_length, seq = header.sequence_id;
    bool mutation = header.command == USBWARP_PIPE_CMD_BIND_DEVICE || header.command == USBWARP_PIPE_CMD_UNBIND_DEVICE ||
        header.command == USBWARP_PIPE_CMD_SET_TARGET;
    if (mutation && !Administrator(s, pipe)) return Error(s, pipe, seq, ERROR_ACCESS_DENIED, "administrator privileges required");
    if (header.command == USBWARP_PIPE_CMD_QUERY_STATUS) {
        if (size) return Error(s, pipe, seq, ERROR_INVALID_DATA, "unexpected payload");
        return Status(s, pipe, seq);
    }
    if (header.command == USBWARP_PIPE_CMD_QUERY_TARGET) {
        if (size) return Error(s, pipe, seq, ERROR_INVALID_DATA, "unexpected payload");
        usbwarp_pipe_target_status target{};
        VmTargetStatus(s, &target);
        return Respond(s, pipe, seq, 0, &target, sizeof(target));
    }
    if (header.command == USBWARP_PIPE_CMD_LIST_VMS) {
        if (size != sizeof(usbwarp_pipe_vm_list_request)) return Error(s, pipe, seq, ERROR_INVALID_DATA, "invalid VM list request");
        usbwarp_pipe_vm_list_request request;
        memcpy(&request, payload, sizeof(request));
        std::vector<BYTE> reply;
        DWORD error;
        try { error = VmList(s, request, reply); }
        catch (const std::bad_alloc &) { error = ERROR_NOT_ENOUGH_MEMORY; }
        if (error) return Error(s, pipe, seq, error, "VM list changed or request is invalid; retry");
        return Respond(s, pipe, seq, 0, reply.data(), (uint32_t)reply.size());
    }
    if (header.command == USBWARP_PIPE_CMD_SET_TARGET) {
        if (size != sizeof(usbwarp_pipe_target)) return Error(s, pipe, seq, ERROR_INVALID_DATA, "invalid target request");
        usbwarp_pipe_target wire; memcpy(&wire, payload, sizeof(wire));
        warp_target target{};
        DWORD error = TargetFromWire(wire, &target);
        if (!error) error = VmSelect(s, target);
        if (error) return Error(s, pipe, seq, error, error == ERROR_BUSY ?
            "unbind all USB devices and finish pending restoration before changing target" : "target selection was not saved");
        return Simple(s, pipe, seq, "target saved; service resolves and connects asynchronously; use vm status");
    }
    if (s->stage.load() != USBWARP_SESSION_RUNNING || !DrvClientActive(s))
        return Error(s, pipe, seq, ERROR_NOT_READY, "USB session is not ready");

    switch (header.command) {
    case USBWARP_PIPE_CMD_LIST_DEVICES: {
        if (size) return Error(s, pipe, seq, ERROR_INVALID_DATA, "unexpected payload");
        USBWARP_ENUM_DEVICES_OUT devices{};
        if (!DrvClientEnumDevices(s, &devices)) return Error(s, pipe, seq, GetLastError(), "driver enumeration failed");
        struct { usbwarp_pipe_device_list list; usbwarp_pipe_device_entry entries[USBWARP_MAX_DEVICES_LIMIT]; } result{};
        result.list.device_count = devices.DeviceCount;
        for (uint32_t i = 0; i < devices.DeviceCount; ++i) {
            auto &entry = result.entries[i];
            memcpy(entry.device_guid, devices.Devices[i].DeviceGuid, 16);
            entry.vendor_id = devices.Devices[i].VendorId;
            entry.product_id = devices.Devices[i].ProductId;
            entry.speed = devices.Devices[i].Speed;
            entry.bound = 1;
        }
        return Respond(s, pipe, seq, 0, &result,
            (uint32_t)sizeof(result.list) + devices.DeviceCount * (uint32_t)sizeof(result.entries[0]));
    }
    case USBWARP_PIPE_CMD_QUERY_DEVICE:
    case USBWARP_PIPE_CMD_QUERY_STATS: {
        // Older Controllers send an empty global-stats request.
        if (size != 16 && !(header.command == USBWARP_PIPE_CMD_QUERY_STATS && !size))
            return Error(s, pipe, seq, ERROR_INVALID_DATA, "expected device GUID");
        bool global = true;
        for (unsigned i = 0; i < size; ++i) if (payload[i]) global = false;
        USBWARP_ENUM_DEVICES_OUT devices{};
        const USBWARP_ENUM_DEVICE_ENTRY *device = nullptr;
        if (!global || header.command == USBWARP_PIPE_CMD_QUERY_DEVICE) {
            if (!DrvClientEnumDevices(s, &devices)) return Error(s, pipe, seq, GetLastError(), "driver enumeration failed");
            for (uint32_t i = 0; i < devices.DeviceCount; ++i)
                if (!memcmp(devices.Devices[i].DeviceGuid, payload, 16)) { device = &devices.Devices[i]; break; }
            if (!device) return Error(s, pipe, seq, ERROR_NOT_FOUND, "device not found");
        }
        if (header.command == USBWARP_PIPE_CMD_QUERY_DEVICE) {
            usbwarp_pipe_query_device_response result{};
            memcpy(result.device_guid, device->DeviceGuid, 16);
            result.vendor_id = device->VendorId; result.product_id = device->ProductId;
            result.speed = device->Speed; result.bound = 1;
            result.pending_urb_count = device->PendingCount;
            result.total_bytes = device->TotalBytes; result.circuit_breaker_state = device->BreakerState;
            return Respond(s, pipe, seq, 0, &result, sizeof(result));
        }
        usbwarp_pipe_stats_response result{};
        if (device) {
            result.urb_submit_count = device->TotalUrbs;
            result.bytes_out = device->TotalBytes; // protocol legacy combined-byte field
        } else {
            USBWARP_QUERY_STATUS_OUT kernel{};
            if (!DrvClientQueryStatus(s, &kernel)) return Error(s, pipe, seq, GetLastError(), "driver statistics unavailable");
            result.urb_submit_count = kernel.TotalUrbsProcessed;
            result.bytes_out = kernel.TotalBytesTransferred;
            result.ring_usage_percent = kernel.RingG2HUsagePercent;
        }
        return Respond(s, pipe, seq, 0, &result, sizeof(result));
    }
    case USBWARP_PIPE_CMD_BIND_DEVICE: {
        if (size < sizeof(usbwarp_pipe_bind_request)) return Error(s, pipe, seq, ERROR_INVALID_DATA, "bind header missing");
        usbwarp_pipe_bind_request request;
        memcpy(&request, payload, sizeof(request));
        if (!request.instance_path_length || request.instance_path_length != size - sizeof(request) ||
            memchr(payload + sizeof(request), 0, request.instance_path_length) || request.speed > USBWARP_SPEED_MAX)
            return Error(s, pipe, seq, ERROR_INVALID_DATA, "invalid device path");
        WCHAR path[256];
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            (const char *)(payload + sizeof(request)), request.instance_path_length, path, 255);
        if (n <= 0) return Error(s, pipe, seq, ERROR_INVALID_DATA, "invalid UTF-8 path");
        path[n] = 0;
        // A driverless USB devnode may have no active device interface. The
        // capture layer resolves and validates instance/ContainerId/hub/port;
        // only its NEW capture interface is ever passed to the kernel driver.
        if (!_wcsnicmp(path, L"\\\\?\\USB#", 8)) path[1]=L'?';
        else if (_wcsnicmp(path, L"USB\\VID_", 8))
            return Error(s, pipe, seq, ERROR_INVALID_NAME, "expected a physical USB instance or interface path");
        uint32_t index; uint16_t vid, pid;
        if (!DeviceBind(s, request.device_guid, path, request.speed, &index, &vid, &pid))
            return Error(s, pipe, seq, GetLastError(), "could not capture/bind device; see service log for PnP/install details");
        char detail[64]; snprintf(detail, sizeof(detail), "bound to slot %u", index);
        return Simple(s, pipe, seq, detail);
    }
    case USBWARP_PIPE_CMD_UNBIND_DEVICE: {
        if (size != sizeof(usbwarp_pipe_bind_request)) return Error(s, pipe, seq, ERROR_INVALID_DATA, "invalid unbind request");
        usbwarp_pipe_bind_request request; memcpy(&request, payload, sizeof(request));
        if (DeviceUnbind(s, request.device_guid)) return Simple(s, pipe, seq, "unbound; original Windows device state restored");
        return Error(s, pipe, seq, GetLastError(), "unbind or Windows storage restoration failed");
    }
    default: return Error(s, pipe, seq, ERROR_INVALID_FUNCTION, "unknown command");
    }
}

DWORD WINAPI ClientWorker(void *context)
{
    auto client = (warp_pipe_client *)context;
    warp_session *s = client->session;
    BYTE *buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, MaxFrame);
    if (buffer) {
        while (!StopRequested(s)) {
            DWORD read = 0;
            if (!Transfer(s, client->pipe, buffer, MaxFrame, &read, false)) break;
            // ERROR_MORE_DATA closes this connection; partial messages are never dispatched.
            if (read < sizeof(usbwarp_pipe_header)) break;
            usbwarp_pipe_header header; memcpy(&header, buffer, sizeof(header));
            if (header.magic != USBWARP_PIPE_MAGIC || header.payload_length != read - sizeof(header)) break;
            if (!Dispatch(s, client->pipe, header, buffer + sizeof(header))) break;
        }
        HeapFree(GetProcessHeap(), 0, buffer);
    }
    // FlushFileBuffers would wait for the client to read and can prevent shutdown.
    DisconnectNamedPipe(client->pipe);
    CloseHandle(client->pipe);
    client->pipe = INVALID_HANDLE_VALUE;
    return 0;
}

HANDLE CreatePipe(warp_session *s, bool first)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    // AU may exchange commands but cannot create another server instance.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        USBWARP_PIPE_SDDL, SDDL_REVISION_1, &descriptor, nullptr))
        return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    HANDLE pipe = CreateNamedPipeW(s->pipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        WARP_PIPE_CLIENTS + 1, MaxFrame, MaxFrame, 1000, &attributes);
    DWORD error = GetLastError(); LocalFree(descriptor); SetLastError(error);
    return pipe;
}

bool Connect(warp_session *s, HANDLE pipe)
{
    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    BOOL connected = ConnectNamedPipe(pipe, &ov);
    DWORD error = connected ? ERROR_SUCCESS : GetLastError();
    if (!connected && error == ERROR_PIPE_CONNECTED) connected = TRUE;
    else if (!connected && error == ERROR_IO_PENDING) {
        HANDLE events[] = {ov.hEvent, s->stopEvent};
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wait != WAIT_OBJECT_0) CancelIoEx(pipe, &ov);
        DWORD transferred;
        connected = GetOverlappedResult(pipe, &ov, &transferred, TRUE);
    }
    CloseHandle(ov.hEvent);
    return connected && !StopRequested(s);
}

DWORD WINAPI AcceptWorker(void *context)
{
    auto s = (warp_session *)context;
    HANDLE listener = s->pipeListener;
    while (!StopRequested(s)) {
        if (!Connect(s, listener)) {
            if (!StopRequested(s)) RequestStop(s, ERROR_PIPE_NOT_CONNECTED);
            break;
        }
        warp_pipe_client *slot = nullptr;
        for (auto &client : s->clients) {
            if (client.thread && WaitForSingleObject(client.thread, 0) == WAIT_OBJECT_0) {
                CloseHandle(client.thread); client.thread = nullptr;
            }
            if (!client.thread && !slot) slot = &client;
        }
        if (!slot) { DisconnectNamedPipe(listener); continue; }
        HANDLE next = CreatePipe(s, false); // preserve ownership of the pipe name continuously
        if (next == INVALID_HANDLE_VALUE) { RequestStop(s, GetLastError()); break; }
        slot->session = s; slot->pipe = listener;
        slot->thread = CreateThread(nullptr, 0, ClientWorker, slot, 0, nullptr);
        if (!slot->thread) {
            DWORD error = GetLastError(); CloseHandle(next); RequestStop(s, error); break;
        }
        listener = next;
    }
    DisconnectNamedPipe(listener);
    CloseHandle(listener);
    s->pipeListener = INVALID_HANDLE_VALUE;
    return 0;
}
}

BOOL PipeServerStart(warp_session *s)
{
    if (!s->pipeName[0] && !MultiByteToWideChar(CP_UTF8, 0, USBWARP_PIPE_NAME, -1, s->pipeName, _countof(s->pipeName)))
        return FALSE;
    s->pipeListener = CreatePipe(s, true);
    if (s->pipeListener == INVALID_HANDLE_VALUE) return FALSE;
    s->pipeThread = CreateThread(nullptr, 0, AcceptWorker, s, 0, nullptr);
    if (!s->pipeThread) {
        DWORD error = GetLastError(); CloseHandle(s->pipeListener); s->pipeListener = INVALID_HANDLE_VALUE;
        SetLastError(error); return FALSE;
    }
    return TRUE;
}
void PipeServerStop(warp_session *s)
{
    if (!s->pipeThread) return;
    RequestStop(s);
    JoinServiceThread(s, s->pipeThread);
    CloseHandle(s->pipeThread); s->pipeThread = nullptr;
    for (auto &client : s->clients) if (client.thread) {
        JoinServiceThread(s, client.thread);
        CloseHandle(client.thread); client.thread = nullptr;
    }
}
