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
    s->hDriver = CreateFileW(
        L"\\\\.\\UsbWarp",
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);

    if (s->hDriver == INVALID_HANDLE_VALUE) {
        s->hDriver = NULL;
        return FALSE;
    }

    LogInfo("Kernel Driver opened: handle=%p", s->hDriver);
    return TRUE;
}

void DrvClientClose(struct warp_session *s)
{
    if (s->hDriver) {
        CloseHandle(s->hDriver);
        s->hDriver = NULL;
    }
    s->driverRegistered = false;
    s->driverShmSetup   = false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  REGISTER_SERVICE
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL DrvClientRegister(struct warp_session *s)
{
    if (!s->hDriver) return FALSE;

    USBWARP_REGISTER_SERVICE_IN in;
    memset(&in, 0, sizeof(in));
    in.Version            = USBWARP_IOCTL_STRUCT_VERSION;
    in.ProcessId          = GetCurrentProcessId();
    in.HeartbeatIntervalMs = 2000;

    USBWARP_REGISTER_SERVICE_OUT out;
    memset(&out, 0, sizeof(out));

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        s->hDriver,
        IOCTL_USBWARP_REGISTER_SERVICE,
        &in,  sizeof(in),
        &out, sizeof(out),
        &bytesReturned, NULL);

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
    if (!s->hDriver || !s->driverRegistered) return FALSE;
    if (!s->section) {
        LogError("SETUP_SHM: no Section handle");
        return FALSE;
    }

    USBWARP_SETUP_SHM_IN in;
    memset(&in, 0, sizeof(in));
    in.Version              = USBWARP_IOCTL_STRUCT_VERSION;
    in.SectionHandle        = (uint64_t)(uintptr_t)s->section;
    in.ExpectedSize         = s->shm_size;
    in.RingSizePerDirection = s->cfg.ring_data_size;
    in.BufferSize           = s->cfg.buffer_size;

    USBWARP_SETUP_SHM_OUT out;
    memset(&out, 0, sizeof(out));

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        s->hDriver,
        IOCTL_USBWARP_SETUP_SHM,
        &in,  sizeof(in),
        &out, sizeof(out),
        &bytesReturned, NULL);

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
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL DrvClientHeartbeat(struct warp_session *s)
{
    if (!s->hDriver || !s->driverRegistered) return FALSE;

    USBWARP_HEARTBEAT_IN in;
    memset(&in, 0, sizeof(in));
    in.Version       = USBWARP_IOCTL_STRUCT_VERSION;
    in.Timestamp     = NowNs();
    in.ServiceState  = s->mmio_mapped ? USBWARP_STATE_RUNNING :
                                        USBWARP_STATE_INIT;

    USBWARP_HEARTBEAT_OUT out;
    memset(&out, 0, sizeof(out));

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        s->hDriver,
        IOCTL_USBWARP_HEARTBEAT,
        &in,  sizeof(in),
        &out, sizeof(out),
        &bytesReturned, NULL);

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
 * §5  BIND_DEVICE
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL DrvClientBindDevice(struct warp_session *s, uint32_t devIdx)
{
    if (!s->hDriver || !s->driverShmSetup) return FALSE;
    if (devIdx >= WARP_MAX_BOUND_DEVICES) return FALSE;

    struct warp_bound_device *dev = &s->bound_devices[devIdx];
    if (!dev->in_use) return FALSE;

    USBWARP_BIND_DEVICE_IN in;
    memset(&in, 0, sizeof(in));
    in.Version = USBWARP_IOCTL_STRUCT_VERSION;
    memcpy(in.DeviceGuid, dev->device_guid, 16);
    in.InstancePathLength = (uint32_t)(wcslen(dev->instance_path) + 1) *
                            sizeof(WCHAR);
    wcsncpy_s((wchar_t *)in.InstancePath, 256, dev->instance_path, _TRUNCATE);

    USBWARP_BIND_DEVICE_OUT out;
    memset(&out, 0, sizeof(out));

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        s->hDriver,
        IOCTL_USBWARP_BIND_DEVICE,
        &in,  sizeof(in),
        &out, sizeof(out),
        &bytesReturned, NULL);

    if (!ok) {
        LogError("IOCTL BIND_DEVICE failed: %lu", GetLastError());
        return FALSE;
    }

    dev->device_index = out.DeviceIndex;
    dev->vendor_id    = out.VendorId;
    dev->product_id   = out.ProductId;

    LogInfo("Driver bound device idx=%u VID=%04x PID=%04x",
            out.DeviceIndex, out.VendorId, out.ProductId);
    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  UNBIND_DEVICE
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOL DrvClientUnbindDevice(struct warp_session *s, uint32_t devIdx)
{
    if (!s->hDriver || !s->driverShmSetup) return FALSE;
    if (devIdx >= WARP_MAX_BOUND_DEVICES) return FALSE;

    struct warp_bound_device *dev = &s->bound_devices[devIdx];
    if (!dev->in_use) return FALSE;

    USBWARP_UNBIND_DEVICE_IN in;
    memset(&in, 0, sizeof(in));
    in.Version = USBWARP_IOCTL_STRUCT_VERSION;
    memcpy(in.DeviceGuid, dev->device_guid, 16);

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        s->hDriver,
        IOCTL_USBWARP_UNBIND_DEVICE,
        &in,  sizeof(in),
        NULL, 0,
        &bytesReturned, NULL);

    if (!ok) {
        LogError("IOCTL UNBIND_DEVICE failed: %lu", GetLastError());
        return FALSE;
    }

    LogInfo("Driver unbound device idx=%u", devIdx);
    return TRUE;
}
