/*
 * usbwarp_drv_client.cpp — IOCTL client for UsbWarp.sys Kernel Driver.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * FIX #1: Complete IOCTL communication.
 *
 * Implements the Service → Kernel Driver interface defined in usbwarp_ioctl.h:
 *   - REGISTER_SERVICE:  Establish identity, enable heartbeat monitoring.
 *   - SETUP_SHM:         Pass Section handle for kernel-side mapping.
 *   - HEARTBEAT:         Periodic keep-alive (called every 2 seconds).
 *   - BIND_DEVICE:       Open a USB device for passthrough.
 *   - UNBIND_DEVICE:     Release a passthrough device.
 *   - QUERY_STATUS:      Read driver health/state.
 *   - ENUM_DEVICES:      Authoritative per-device snapshot.
 *
 * Concurrency: the kernel driver is the single source of truth for
 * bound-device state, so bind/unbind/list are GUID-keyed pass-throughs
 * with no Service-side mirror.  driverLock (SRWLOCK) guards the hDriver
 * lifetime — IOCTLs take it shared, open/close take it exclusive — so a
 * bind that runs for seconds never blocks the 2s heartbeat.
 *
 * All functions return FALSE if the driver is not loaded (graceful degradation).
 */

#include "warp_service.h"
#include <stdio.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Open / Close
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL DrvClientOpen(struct warp_session *s)
{
    AcquireSRWLockExclusive(&s->driverLock);

    if (s->hDriver) {
        ReleaseSRWLockExclusive(&s->driverLock);
        return TRUE;
    }

    s->hDriver = CreateFileW(
        L"\\\\.\\UsbWarp",
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);

    if (s->hDriver == INVALID_HANDLE_VALUE) {
        s->hDriver = NULL;
        ReleaseSRWLockExclusive(&s->driverLock);
        return FALSE;
    }

    ReleaseSRWLockExclusive(&s->driverLock);

    LogInfo("Kernel Driver opened: handle=%p", s->hDriver);
    return TRUE;
}

void DrvClientClose(struct warp_session *s)
{
    AcquireSRWLockExclusive(&s->driverLock);

    if (s->hDriver) {
        CloseHandle(s->hDriver);
        s->hDriver = NULL;
    }
    s->driverRegistered = false;
    s->driverShmSetup   = false;

    ReleaseSRWLockExclusive(&s->driverLock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  REGISTER_SERVICE
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL DrvClientRegister(struct warp_session *s)
{
    USBWARP_REGISTER_SERVICE_IN in;
    USBWARP_REGISTER_SERVICE_OUT out;
    DWORD bytesReturned = 0;
    BOOL ok;

    memset(&in, 0, sizeof(in));
    in.Version             = USBWARP_IOCTL_STRUCT_VERSION;
    in.ProcessId           = GetCurrentProcessId();
    in.HeartbeatIntervalMs = 2000;
    memset(&out, 0, sizeof(out));

    AcquireSRWLockShared(&s->driverLock);
    if (!s->hDriver) {
        ReleaseSRWLockShared(&s->driverLock);
        return FALSE;
    }
    ok = DeviceIoControl(s->hDriver, IOCTL_USBWARP_REGISTER_SERVICE,
                         &in, sizeof(in), &out, sizeof(out),
                         &bytesReturned, NULL);
    ReleaseSRWLockShared(&s->driverLock);

    if (!ok) {
        LogError("IOCTL REGISTER_SERVICE failed: %lu", GetLastError());
        return FALSE;
    }

    s->driverRegistered = true;
    LogInfo("Driver registered: driverVer=0x%x maxDevices=%u",
            out.DriverVersion, out.MaxDevices);
    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  SETUP_SHM
 *
 *   Passes the Section handle to the kernel driver.  The driver calls
 *   ObReferenceObjectByHandle(UserMode) to independently hold the Section,
 *   then maps it into system space via MmMapViewInSystemSpace.
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL DrvClientSetupShm(struct warp_session *s)
{
    USBWARP_SETUP_SHM_IN in;
    USBWARP_SETUP_SHM_OUT out;
    DWORD bytesReturned = 0;
    BOOL ok;

    if (!s->section) {
        LogError("SETUP_SHM: no Section handle");
        return FALSE;
    }

    memset(&in, 0, sizeof(in));
    in.Version              = USBWARP_IOCTL_STRUCT_VERSION;
    in.SectionHandle        = (uint64_t)(uintptr_t)s->section;
    in.ExpectedSize         = s->shm_size;
    in.RingSizePerDirection = s->cfg.ring_data_size;
    in.BufferSize           = s->cfg.buffer_size;
    memset(&out, 0, sizeof(out));

    AcquireSRWLockShared(&s->driverLock);
    if (!s->hDriver || !s->driverRegistered) {
        ReleaseSRWLockShared(&s->driverLock);
        return FALSE;
    }
    ok = DeviceIoControl(s->hDriver, IOCTL_USBWARP_SETUP_SHM,
                         &in, sizeof(in), &out, sizeof(out),
                         &bytesReturned, NULL);
    ReleaseSRWLockShared(&s->driverLock);

    if (!ok) {
        LogError("IOCTL SETUP_SHM failed: %lu", GetLastError());
        return FALSE;
    }

    s->driverShmSetup = true;
    LogInfo("Driver SHM established: buffers=%u dataOff=0x%llx dataSz=0x%llx",
            out.BufferCount,
            (unsigned long long)out.DataRegionOffset,
            (unsigned long long)out.DataRegionSize);
    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  HEARTBEAT
 *
 *   Called every 2 seconds from the main loop.  Sends Service state to
 *   driver; receives driver state and pending URB count.
 *   This is also used by the driver for orphan detection (#3/#10).
 *
 *   Note: the fatal-error close happens AFTER releasing the shared lock —
 *   DrvClientClose takes it exclusively.
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL DrvClientHeartbeat(struct warp_session *s)
{
    USBWARP_HEARTBEAT_IN in;
    USBWARP_HEARTBEAT_OUT out;
    DWORD bytesReturned = 0;
    BOOL ok = FALSE;

    memset(&in, 0, sizeof(in));
    in.Version      = USBWARP_IOCTL_STRUCT_VERSION;
    in.Timestamp    = NowNs();
    in.ServiceState = s->mmio_mapped ? USBWARP_STATE_RUNNING :
                                      USBWARP_STATE_INIT;
    memset(&out, 0, sizeof(out));

    AcquireSRWLockShared(&s->driverLock);
    if (s->hDriver && s->driverRegistered) {
        ok = DeviceIoControl(s->hDriver, IOCTL_USBWARP_HEARTBEAT,
                             &in, sizeof(in), &out, sizeof(out),
                             &bytesReturned, NULL);
    }
    ReleaseSRWLockShared(&s->driverLock);

    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_INVALID_HANDLE) {
            LogWarn("Driver connection lost (heartbeat failed: %lu)", err);
            DrvClientClose(s);
        }
        return FALSE;
    }

    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  QUERY_STATUS / ENUM_DEVICES
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL DrvClientQueryStatus(struct warp_session *s, USBWARP_QUERY_STATUS_OUT *out)
{
    DWORD bytesReturned = 0;
    BOOL ok;

    memset(out, 0, sizeof(*out));

    AcquireSRWLockShared(&s->driverLock);
    if (!s->hDriver) {
        ReleaseSRWLockShared(&s->driverLock);
        return FALSE;
    }
    ok = DeviceIoControl(s->hDriver, IOCTL_USBWARP_QUERY_STATUS,
                         NULL, 0, out, sizeof(*out),
                         &bytesReturned, NULL);
    ReleaseSRWLockShared(&s->driverLock);

    return ok;
}

BOOL DrvClientEnumDevices(struct warp_session *s, USBWARP_ENUM_DEVICES_OUT *out)
{
    DWORD bytesReturned = 0;
    BOOL ok;

    memset(out, 0, sizeof(*out));

    AcquireSRWLockShared(&s->driverLock);
    if (!s->hDriver) {
        ReleaseSRWLockShared(&s->driverLock);
        return FALSE;
    }
    ok = DeviceIoControl(s->hDriver, IOCTL_USBWARP_ENUM_DEVICES,
                         NULL, 0, out, sizeof(*out),
                         &bytesReturned, NULL);
    ReleaseSRWLockShared(&s->driverLock);

    return ok;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  BIND_DEVICE (GUID-keyed)
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL DrvClientBindGuid(struct warp_session *s, const uint8_t device_guid[16],
                       const WCHAR *instance_path,
                       uint32_t *device_index, uint16_t *vendor_id,
                       uint16_t *product_id)
{
    USBWARP_BIND_DEVICE_IN in;
    USBWARP_BIND_DEVICE_OUT out;
    DWORD bytesReturned = 0;
    BOOL ok;

    memset(&in, 0, sizeof(in));
    in.Version = USBWARP_IOCTL_STRUCT_VERSION;
    memcpy(in.DeviceGuid, device_guid, 16);
    in.InstancePathLength =
        (uint32_t)(wcslen(instance_path) + 1) * sizeof(WCHAR);
    wcsncpy_s((wchar_t *)in.InstancePath, 256, instance_path, _TRUNCATE);
    memset(&out, 0, sizeof(out));

    AcquireSRWLockShared(&s->driverLock);
    if (!s->hDriver || !s->driverShmSetup) {
        ReleaseSRWLockShared(&s->driverLock);
        return FALSE;
    }
    ok = DeviceIoControl(s->hDriver, IOCTL_USBWARP_BIND_DEVICE,
                         &in, sizeof(in), &out, sizeof(out),
                         &bytesReturned, NULL);
    ReleaseSRWLockShared(&s->driverLock);

    if (!ok) {
        LogError("IOCTL BIND_DEVICE failed: %lu", GetLastError());
        return FALSE;
    }

    if (device_index) *device_index = out.DeviceIndex;
    if (vendor_id)    *vendor_id    = out.VendorId;
    if (product_id)   *product_id   = out.ProductId;

    LogInfo("Driver bound device idx=%u VID=%04x PID=%04x",
            out.DeviceIndex, out.VendorId, out.ProductId);
    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  UNBIND_DEVICE (GUID-keyed) + unbind-all
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL DrvClientUnbindGuid(struct warp_session *s, const uint8_t device_guid[16])
{
    USBWARP_UNBIND_DEVICE_IN in;
    DWORD bytesReturned = 0;
    BOOL ok;

    memset(&in, 0, sizeof(in));
    in.Version = USBWARP_IOCTL_STRUCT_VERSION;
    memcpy(in.DeviceGuid, device_guid, 16);

    AcquireSRWLockShared(&s->driverLock);
    if (!s->hDriver || !s->driverShmSetup) {
        ReleaseSRWLockShared(&s->driverLock);
        return FALSE;
    }
    ok = DeviceIoControl(s->hDriver, IOCTL_USBWARP_UNBIND_DEVICE,
                         &in, sizeof(in), NULL, 0,
                         &bytesReturned, NULL);
    ReleaseSRWLockShared(&s->driverLock);

    if (!ok)
        LogError("IOCTL UNBIND_DEVICE failed: %lu", GetLastError());

    return ok;
}

void DrvClientUnbindAll(struct warp_session *s)
{
    USBWARP_ENUM_DEVICES_OUT en;

    for (int pass = 0; pass < 3; pass++) {
        if (!DrvClientEnumDevices(s, &en))
            return;
        if (en.DeviceCount == 0)
            return;

        for (uint32_t i = 0; i < en.DeviceCount; i++) {
            /* Removing=1: hot-remove teardown owns the slot and will
             * free it on its own — don't fight it. */
            if (en.Devices[i].Removing)
                continue;
            DrvClientUnbindGuid(s, en.Devices[i].DeviceGuid);
        }
    }

    if (DrvClientEnumDevices(s, &en) && en.DeviceCount > 0)
        LogWarn("unbind-all: %u device(s) still bound after 3 passes",
                en.DeviceCount);
}
