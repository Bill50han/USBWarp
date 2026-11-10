// SPDX-License-Identifier: GPL-2.0-only
// Backend for the SCM test executable ONLY. It never opens a driver or VM.
#include "../USBWarpService/warp_service.h"
#include <assert.h>
#include <stdio.h>

#ifndef USBWARP_TEST_DIRECTORY
#error A private fixture directory is required
#endif

static SRWLOCK stateLock = SRWLOCK_INIT;
static bool bound;
static uint8_t boundGuid[16];
static HANDLE controlThread;
extern warp_session g_sess;
static bool marker(const WCHAR *name)
{
    WCHAR path[1024]; swprintf_s(path, L"%ls\\%ls", USBWARP_TEST_DIRECTORY, name);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static DWORD WINAPI FixtureControl(void *)
{
    while (WaitForSingleObject(g_sess.stopEvent, 20) == WAIT_TIMEOUT)
        if (marker(L"stop_service")) { RequestStop(&g_sess); break; }
    return 0;
}
BOOL HdvLoadApis(usbwarp_hdv_api *)
{ controlThread = CreateThread(nullptr,0,FixtureControl,nullptr,0,nullptr); return controlThread != nullptr; }
void HdvUnloadApis(usbwarp_hdv_api *)
{ if (controlThread) { WaitForSingleObject(controlThread,INFINITE); CloseHandle(controlThread); controlThread=nullptr; } }
static const WCHAR *FixtureVmId()
{ return marker(L"vm_new_id") ? L"22222222-2222-2222-2222-222222222222" : L"11111111-1111-1111-1111-111111111111"; }
DWORD EnumerateVms(warp_session *, std::vector<warp_vm> &vms)
{
    vms.clear();
    if (marker(L"discovery_error")) return ERROR_ACCESS_DENIED;
    if (marker(L"vm_up")) {
        warp_vm vm{};
        wcscpy_s(vm.id,FixtureVmId()); wcscpy_s(vm.name,L"Fixture Linux");
        wcscpy_s(vm.owner,L"WSL"); wcscpy_s(vm.state,L"Running");
        vms.push_back(vm);
        if (marker(L"vm_ambiguous")) {wcscpy_s(vm.id,L"33333333-3333-3333-3333-333333333333");vms.push_back(vm);}
    }
    return ERROR_SUCCESS;
}
BOOL HdvSessionOpen(warp_session *s)
{
    if (!marker(L"vm_up") || wcscmp(s->openingVmId,FixtureVmId())) { SetLastError(ERROR_FILE_NOT_FOUND); return FALSE; }
    s->memSpaceEvent = CreateEventW(nullptr,TRUE,TRUE,nullptr);
    if (!s->memSpaceEvent) return FALSE;
    s->hdv_host = (HDV_HOST)(ULONG_PTR)1;
    s->hdv_started = true; s->hdv_torn_down = false; s->mem_space_on = true;
    return TRUE;
}
BOOL HdvSessionMapMmio(warp_session *s) { s->mmio_mapped = true; return TRUE; }
BOOL HdvSessionClose(warp_session *s)
{
    assert(!s->hDriver); // production supervisor must tear down the driver first
    s->hdv_host = nullptr; s->hdv_started = false; s->mmio_mapped = false; s->mem_space_on = false;
    if (s->memSpaceEvent) { CloseHandle(s->memSpaceEvent); s->memSpaceEvent = nullptr; }
    return TRUE;
}
BOOL ShmCreate(warp_session *s) {
    if(marker(L"forbid_shm")){SetLastError(ERROR_INVALID_STATE);return FALSE;}
    s->section = (HANDLE)(ULONG_PTR)2;return TRUE;
}
void ShmDestroy(warp_session *s) { assert(!s->hDriver && !s->hdv_host); s->section = nullptr; }

BOOL DrvClientOpen(warp_session *s)
{
    if (marker(L"driver_down")) { SetLastError(ERROR_FILE_NOT_FOUND); return FALSE; }
    AcquireSRWLockExclusive(&s->driverLock); s->hDriver = (HANDLE)(ULONG_PTR)3;
    ReleaseSRWLockExclusive(&s->driverLock); return TRUE;
}
BOOL DrvClientRegister(warp_session *s)
{
    AcquireSRWLockExclusive(&s->driverLock); s->driverRegistered = true;
    ReleaseSRWLockExclusive(&s->driverLock); return TRUE;
}
BOOL DrvClientSetupShm(warp_session *s)
{
    AcquireSRWLockExclusive(&s->driverLock); s->driverShmSetup = true;
    s->driverState.store(USBWARP_STATE_READY);
    ReleaseSRWLockExclusive(&s->driverLock); return TRUE;
}
DWORD DrvClientClose(warp_session *s)
{
    AcquireSRWLockExclusive(&s->driverLock);
    s->hDriver = nullptr; s->driverRegistered = s->driverShmSetup = false;
    s->driverState.store(USBWARP_STATE_INIT);
    ReleaseSRWLockExclusive(&s->driverLock);
    AcquireSRWLockExclusive(&stateLock); bound = false; ReleaseSRWLockExclusive(&stateLock);
    return ERROR_SUCCESS;
}
BOOL DrvClientActive(warp_session *s)
{
    AcquireSRWLockShared(&s->driverLock);
    BOOL active = s->hDriver && s->driverShmSetup && !StopRequested(s);
    ReleaseSRWLockShared(&s->driverLock); return active;
}
BOOL DrvClientHeartbeat(warp_session *s)
{
    if (marker(L"driver_down")) { SetLastError(ERROR_DEVICE_NOT_CONNECTED); return FALSE; }
    if (s->mmio_mapped && (!marker(L"vm_up") || wcscmp(s->openingVmId,FixtureVmId()))) { s->hdv_torn_down = true; SetEvent(s->memSpaceEvent); }
    s->driverState.store(s->mmio_mapped ? USBWARP_STATE_RUNNING : USBWARP_STATE_READY);
    return TRUE;
}
BOOL DrvClientEnumDevices(warp_session *s, USBWARP_ENUM_DEVICES_OUT *out)
{
    memset(out,0,sizeof(*out)); out->Version = USBWARP_IOCTL_STRUCT_VERSION;
    if (!DrvClientActive(s)) { SetLastError(ERROR_NOT_READY); return FALSE; }
    AcquireSRWLockShared(&stateLock);
    if ((bound || marker(L"backend_bound")) && !marker(L"device_gone")) {
        out->DeviceCount = 1;
        memcpy(out->Devices[0].DeviceGuid,boundGuid,16);
        if (!bound) out->Devices[0].DeviceGuid[0] = 1;
        out->Devices[0].DeviceIndex = 1;
        out->Devices[0].VendorId = 0x1234; out->Devices[0].ProductId = 0xabcd;
        out->Devices[0].Speed = USBWARP_SPEED_SUPER_PLUS_2X;
    }
    ReleaseSRWLockShared(&stateLock);
    return TRUE;
}
BOOL DrvClientQueryStatus(warp_session *s, USBWARP_QUERY_STATUS_OUT *out)
{
    USBWARP_ENUM_DEVICES_OUT devices;
    if (!DrvClientEnumDevices(s,&devices)) return FALSE;
    memset(out,0,sizeof(*out)); out->Version = USBWARP_IOCTL_STRUCT_VERSION;
    out->DriverState = s->driverState.load(); out->ServiceRegistered = out->ShmEstablished = 1;
    out->ActiveDeviceCount = devices.DeviceCount; return TRUE;
}
BOOL DrvClientBindGuid(warp_session *s, const uint8_t guid[16], const WCHAR *, uint8_t,
                       uint32_t *index, uint16_t *vid, uint16_t *pid)
{
    if (!DrvClientActive(s)) { SetLastError(ERROR_NOT_READY); return FALSE; }
    AcquireSRWLockExclusive(&stateLock); bound = true; memcpy(boundGuid,guid,16); ReleaseSRWLockExclusive(&stateLock);
    if (index) *index = 1; if (vid) *vid = 0x1234; if (pid) *pid = 0xabcd;
    return TRUE;
}
BOOL DrvClientUnbindGuid(warp_session *s, const uint8_t[16])
{
    if (!DrvClientActive(s)) { SetLastError(ERROR_NOT_READY); return FALSE; }
    AcquireSRWLockExclusive(&stateLock); bound = false; ReleaseSRWLockExclusive(&stateLock); return TRUE;
}
BOOL DeviceBind(warp_session *s,const uint8_t guid[16],const WCHAR *path,uint8_t speed,
                uint32_t *index,uint16_t *vid,uint16_t *pid)
{
    AcquireSRWLockExclusive(&s->captureLock);
    BOOL result;
    if (!VmCanBind(s)) {SetLastError(ERROR_NOT_READY);result=FALSE;}
    else result=DrvClientBindGuid(s,guid,path,speed,index,vid,pid);
    ReleaseSRWLockExclusive(&s->captureLock);
    return result;
}
BOOL DeviceUnbind(warp_session *s,const uint8_t guid[16]) {
    AcquireSRWLockExclusive(&s->captureLock);
    BOOL result=DrvClientUnbindGuid(s,guid);
    ReleaseSRWLockExclusive(&s->captureLock);return result;
}
BOOL DeviceRecover(warp_session *) { return TRUE; }
DWORD DeviceCloseSession(warp_session *s) {
    AcquireSRWLockExclusive(&s->captureLock);
    DWORD result=DrvClientClose(s);
    ReleaseSRWLockExclusive(&s->captureLock);return result;
}
void DeviceReconcile(warp_session *) {}
DWORD DeviceCapturePending(bool *pending) { *pending=false; return ERROR_SUCCESS; }
