// SPDX-License-Identifier: GPL-2.0-only
/* The kernel is the only device manager. Every I/O has its own OVERLAPPED;
 * cancellation is drained before buffers, events or the driver handle die. */
#include "warp_service.h"
#include <wchar.h>

namespace {
bool DriverLost(DWORD error)
{
    return error == ERROR_INVALID_HANDLE || error == ERROR_DEVICE_NOT_CONNECTED ||
           error == ERROR_FILE_NOT_FOUND || error == ERROR_GEN_FAILURE || error == ERROR_REVISION_MISMATCH;
}

BOOL Ioctl(warp_session *s, HANDLE driver, DWORD code, void *input, DWORD inBytes,
           void *output, DWORD outBytes, DWORD *returned, DWORD timeout, bool cancellable)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return FALSE;
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(driver, code, input, inBytes, output, outBytes, &bytes, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_PENDING) {
        HANDLE events[] = {ov.hEvent, s->stopEvent, s->sessionStopEvent};
        DWORD result = WaitForMultipleObjects(cancellable ? 3 : 1, events, FALSE, timeout);
        if (result != WAIT_OBJECT_0) CancelIoEx(driver, &ov);
        // CancelIoEx only requests cancellation. No early return is safe here.
        ok = GetOverlappedResult(driver, &ov, &bytes, TRUE);
        error = ok ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(ov.hEvent);
    if (returned) *returned = bytes;
    SetLastError(error);
    return ok;
}

BOOL Call(warp_session *s, DWORD code, void *input, DWORD inBytes,
          void *output, DWORD outBytes, DWORD *returned, DWORD timeout = 30000)
{
    AcquireSRWLockShared(&s->driverLock);
    BOOL ok = FALSE;
    DWORD error = ERROR_NOT_READY;
    if (s->hDriver && s->driverShmSetup && !StopRequested(s) &&
        WaitForSingleObject(s->sessionStopEvent, 0) != WAIT_OBJECT_0) {
        ok = Ioctl(s, s->hDriver, code, input, inBytes, output, outBytes, returned, timeout, true);
        error = ok ? ERROR_SUCCESS : GetLastError();
    }
    ReleaseSRWLockShared(&s->driverLock);
    if (!ok && (code == IOCTL_USBWARP_HEARTBEAT || code == IOCTL_USBWARP_QUERY_STATUS ||
                code == IOCTL_USBWARP_ENUM_DEVICES) && DriverLost(error)) {
        s->lastSessionError.store(error);
        SetEvent(s->driverFaultEvent);
    }
    SetLastError(error);
    return ok;
}

BOOL ValidReply(DWORD actual, DWORD needed, uint32_t version)
{
    if (actual < needed) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
    if (version != USBWARP_IOCTL_STRUCT_VERSION) { SetLastError(ERROR_REVISION_MISMATCH); return FALSE; }
    return TRUE;
}
}

BOOL DrvClientOpen(warp_session *s)
{
    AcquireSRWLockExclusive(&s->driverLock);
    if (s->hDriver) { ReleaseSRWLockExclusive(&s->driverLock); SetLastError(ERROR_ALREADY_EXISTS); return FALSE; }
    HANDLE h = CreateFileW(L"\\\\.\\UsbWarp", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    DWORD error = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (!error) s->hDriver = h;
    ReleaseSRWLockExclusive(&s->driverLock);
    SetLastError(error);
    return error == ERROR_SUCCESS;
}

BOOL DrvClientRegister(warp_session *s)
{
    USBWARP_REGISTER_SERVICE_IN in{};
    USBWARP_REGISTER_SERVICE_OUT out{};
    in.Version = USBWARP_IOCTL_STRUCT_VERSION;
    in.ProcessId = GetCurrentProcessId();
    in.HeartbeatIntervalMs = 2000;
    DWORD bytes = 0;
    AcquireSRWLockExclusive(&s->driverLock);
    SetLastError(ERROR_NOT_READY);
    BOOL ok = s->hDriver && Ioctl(s, s->hDriver, IOCTL_USBWARP_REGISTER_SERVICE,
        &in, sizeof(in), &out, sizeof(out), &bytes, 10000, true);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (ok) {
        s->driverRegistered = true; // even a malformed success reply must be torn down
        ok = ValidReply(bytes, sizeof(out), out.Version);
        if (ok) s->driverCaps = out.Capabilities;
        error = ok ? ERROR_SUCCESS : GetLastError();
    }
    ReleaseSRWLockExclusive(&s->driverLock);
    SetLastError(error);
    return ok;
}

BOOL DrvClientSetupShm(warp_session *s)
{
    USBWARP_SETUP_SHM_IN in{};
    USBWARP_SETUP_SHM_OUT out{};
    in.Version = USBWARP_IOCTL_STRUCT_VERSION;
    in.SectionHandle = (uint64_t)(uintptr_t)s->section;
    in.ExpectedSize = s->shm_size;
    in.RingSizePerDirection = s->cfg.ring_data_size;
    in.BufferSize = s->cfg.buffer_size;
    in.MaxDevices = s->cfg.max_devices;
    in.DoorbellEventHandle = (uint64_t)(uintptr_t)s->doorbellEvent;
    DWORD bytes = 0;
    AcquireSRWLockExclusive(&s->driverLock);
    SetLastError(ERROR_NOT_READY);
    BOOL ok = s->hDriver && s->driverRegistered && s->section &&
        Ioctl(s, s->hDriver, IOCTL_USBWARP_SETUP_SHM, &in, sizeof(in), &out,
              sizeof(out), &bytes, 10000, true);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (ok) {
        s->driverShmSetup = true;
        ok = ValidReply(bytes, sizeof(out), out.Version);
        error = ok ? ERROR_SUCCESS : GetLastError();
    }
    ReleaseSRWLockExclusive(&s->driverLock);
    SetLastError(error);
    return ok;
}

DWORD DrvClientClose(warp_session *s)
{
    if (s->sessionStopEvent) SetEvent(s->sessionStopEvent);
    // Shared callers cancel and drain; the exclusive lease then owns all I/O.
    AcquireSRWLockExclusive(&s->driverLock);
    DWORD error = ERROR_SUCCESS;
    if (s->hDriver) {
        if (s->driverRegistered) {
            USBWARP_TEARDOWN_SHM_IN in{};
            in.Version = USBWARP_IOCTL_STRUCT_VERSION;
            in.Flags = USBWARP_TEARDOWN_FLAG_GRACEFUL;
            if (!Ioctl(s, s->hDriver, IOCTL_USBWARP_TEARDOWN_SHM, &in, sizeof(in),
                       nullptr, 0, nullptr, INFINITE, false)) error = GetLastError();
        }
        CloseHandle(s->hDriver); // kernel file cleanup is the crash/failure fallback
        s->hDriver = nullptr;
    }
    s->driverRegistered = s->driverShmSetup = false;
    s->driverCaps = 0;
    s->driverState.store(USBWARP_STATE_INIT);
    ReleaseSRWLockExclusive(&s->driverLock);
    return error;
}

BOOL DrvClientActive(warp_session *s)
{
    AcquireSRWLockShared(&s->driverLock);
    BOOL active = s->hDriver && s->driverRegistered && s->driverShmSetup &&
        !StopRequested(s) && WaitForSingleObject(s->sessionStopEvent, 0) != WAIT_OBJECT_0;
    ReleaseSRWLockShared(&s->driverLock);
    return active;
}

BOOL DrvClientConfigureIrq(warp_session *s)
{
    if (!s->msixPresent) return TRUE;
    // Close holds driverLock while the kernel waits for the guest shutdown
    // ACK. Guest vector teardown must not wait on that same lock. The PCI
    // callback has already updated its shadow; shutdown quiesces the route.
    if (StopRequested(s) || WaitForSingleObject(s->sessionStopEvent, 0) == WAIT_OBJECT_0)
        return TRUE;
    USBWARP_CONFIGURE_IRQ_IN in{};
    in.Version = USBWARP_IOCTL_STRUCT_VERSION;
    AcquireSRWLockExclusive(&s->pciLock);
    memcpy(in.InstanceId, &s->instance_id, 16);
    in.Nonce = s->irqNonce;
    in.Revision = ++s->irqRevision;
    memcpy(&in.MsiAddress, s->msixTable, 8);
    memcpy(&in.MsiData, s->msixTable + 8, 4);
    uint32_t control;
    memcpy(&control, s->msixTable + 12, 4);
    uint32_t cfg = s->pci_cfg[0x50 / 4];
    if (cfg & 0x80000000u) in.Flags |= USBWARP_IRQ_CONFIG_ENABLED;
    if ((cfg & 0x40000000u) || (control & 1)) in.Flags |= USBWARP_IRQ_CONFIG_MASKED;
    ReleaseSRWLockExclusive(&s->pciLock);
    BOOL ok = Call(s, IOCTL_USBWARP_CONFIGURE_IRQ, &in, sizeof(in), nullptr, 0, nullptr);
    // The PCI shell can mask/free its vector after kernel session teardown.
    // Keep emulating configuration until HdvTeardownDeviceHost returns.
    if (!ok && (StopRequested(s) || WaitForSingleObject(s->sessionStopEvent, 0) == WAIT_OBJECT_0)) return TRUE;
    return ok;
}

BOOL DrvClientHeartbeat(warp_session *s)
{
    USBWARP_HEARTBEAT_IN in{};
    USBWARP_HEARTBEAT_OUT out{};
    in.Version = USBWARP_IOCTL_STRUCT_VERSION;
    in.Timestamp = NowNs();
    in.ServiceState = s->mmio_mapped.load() ? USBWARP_STATE_RUNNING : USBWARP_STATE_INIT;
    DWORD bytes = 0;
    if (!Call(s, IOCTL_USBWARP_HEARTBEAT, &in, sizeof(in), &out, sizeof(out), &bytes, 5000)) return FALSE;
    if (!ValidReply(bytes, sizeof(out), out.Version)) return FALSE;
    s->driverState.store(out.DriverState);
    BOOL healthy = out.DriverState != USBWARP_STATE_ERROR && out.DriverState != USBWARP_STATE_STOPPED &&
                   out.DriverState != USBWARP_STATE_SHUTTING_DOWN;
    if (!healthy) SetLastError(ERROR_DEVICE_NOT_CONNECTED);
    return healthy;
}

BOOL DrvClientQueryStatus(warp_session *s, USBWARP_QUERY_STATUS_OUT *out)
{
    memset(out, 0, sizeof(*out));
    DWORD bytes = 0;
    return Call(s, IOCTL_USBWARP_QUERY_STATUS, nullptr, 0, out, sizeof(*out), &bytes) &&
           ValidReply(bytes, sizeof(*out), out->Version);
}
BOOL DrvClientEnumDevices(warp_session *s, USBWARP_ENUM_DEVICES_OUT *out)
{
    memset(out, 0, sizeof(*out));
    DWORD bytes = 0;
    if (!Call(s, IOCTL_USBWARP_ENUM_DEVICES, nullptr, 0, out, sizeof(*out), &bytes) ||
        !ValidReply(bytes, sizeof(*out), out->Version)) return FALSE;
    if (out->DeviceCount > USBWARP_MAX_DEVICES_LIMIT) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
    return TRUE;
}
BOOL DrvClientBindGuid(warp_session *s, const uint8_t guid[16], const WCHAR *path,
                       uint8_t speed, uint32_t *index, uint16_t *vid, uint16_t *pid)
{
    USBWARP_BIND_DEVICE_IN in{};
    USBWARP_BIND_DEVICE_OUT out{};
    size_t length = wcsnlen_s(path, _countof(in.InstancePath));
    if (length == _countof(in.InstancePath)) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
    in.Version = USBWARP_IOCTL_STRUCT_VERSION;
    memcpy(in.DeviceGuid, guid, 16);
    memcpy(in.InstancePath, path, (length + 1) * sizeof(WCHAR));
    in.InstancePathLength = (uint32_t)((length + 1) * sizeof(WCHAR));
    in.Speed = speed;
    DWORD bytes = 0;
    if (!Call(s, IOCTL_USBWARP_BIND_DEVICE, &in, sizeof(in), &out, sizeof(out), &bytes) ||
        !ValidReply(bytes, sizeof(out), out.Version)) return FALSE;
    if (!out.DeviceIndex || out.DeviceIndex > USBWARP_MAX_DEVICES_LIMIT) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
    if (index) *index = out.DeviceIndex;
    if (vid) *vid = out.VendorId;
    if (pid) *pid = out.ProductId;
    return TRUE;
}
BOOL DrvClientUnbindGuid(warp_session *s, const uint8_t guid[16])
{
    USBWARP_UNBIND_DEVICE_IN in{};
    in.Version = USBWARP_IOCTL_STRUCT_VERSION;
    memcpy(in.DeviceGuid, guid, 16);
    return Call(s, IOCTL_USBWARP_UNBIND_DEVICE, &in, sizeof(in), nullptr, 0, nullptr);
}
