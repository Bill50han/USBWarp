// SPDX-License-Identifier: GPL-2.0-only
// Run as administrator in the test VM with UsbWarp.sys loaded and its service stopped.
// Exercises real kernel session ownership without binding any USB device or VM.
#include "../USBWarpService/warp_service.h"
#include <stdio.h>
#include <string>
#include <stdexcept>
void LogInfo(const char *, ...) {}
void LogWarn(const char *, ...) {}
void LogError(const char *, ...) {}
bool StopRequested(const warp_session *s) { return s->shutdown_requested.load(); }
static unsigned checks;
static HRESULT WINAPI Bell(HDV_DEVICE,HDV_PCI_BAR_SELECTOR,UINT64,UINT64,UINT64,HANDLE) { return S_OK; }
static HRESULT WINAPI Unbell(HDV_DEVICE,HDV_PCI_BAR_SELECTOR,UINT64,UINT64,UINT64) { return S_OK; }
#define CHECK(x) do { ++checks; if (!(x)) { char error[256]; snprintf(error,sizeof(error),"line %d: %s (error %lu)",__LINE__,#x,GetLastError()); throw std::runtime_error(error); } } while(0)
static void init(warp_session &s)
{
    warp_config_defaults(&s.cfg);
    s.api.HdvRegisterDoorbell=Bell; s.api.HdvUnregisterDoorbell=Unbell;
    s.stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    s.sessionStopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    s.driverFaultEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    CHECK(s.stopEvent && s.sessionStopEvent && s.driverFaultEvent);
}
static void finish(warp_session &s)
{
    DrvClientClose(&s); ShmDestroy(&s);
    CloseHandle(s.stopEvent); CloseHandle(s.sessionStopEvent); CloseHandle(s.driverFaultEvent);
}
static void open(warp_session &s)
{
    ResetEvent(s.sessionStopEvent);
    ULONGLONG deadline=GetTickCount64()+10000;
    while (!DrvClientOpen(&s)) { CHECK(GetTickCount64()<deadline); Sleep(25); }
    CHECK(DrvClientRegister(&s));
}
static void deny_unprivileged()
{
    BYTE admin[SECURITY_MAX_SID_SIZE]; DWORD size=sizeof(admin);
    CHECK(CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admin,&size));
    HANDLE token=nullptr, restricted=nullptr;
    CHECK(OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE|TOKEN_QUERY,&token));
    SID_AND_ATTRIBUTES sid{admin,0};
    CHECK(CreateRestrictedToken(token,DISABLE_MAX_PRIVILEGE,1,&sid,0,nullptr,0,nullptr,&restricted));
    CloseHandle(token); CHECK(ImpersonateLoggedOnUser(restricted));
    HANDLE handle=CreateFileW(L"\\\\.\\UsbWarp",GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    DWORD deniedError=GetLastError();
    BOOL reverted=RevertToSelf(); CloseHandle(restricted);
    if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);
    CHECK(reverted && handle==INVALID_HANDLE_VALUE && deniedError==ERROR_ACCESS_DENIED);
}
int wmain(int argc, WCHAR **argv)
{
    warp_session s; HANDLE child=nullptr, ready=nullptr;
    bool initialized=false;
    try {
        init(s); initialized=true;
        if (argc==3 && !wcscmp(argv[1],L"--hold")) {
            open(s); // deliberately die before SETUP_SHM/poll-thread creation
            ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[2]); CHECK(ready);
            SetEvent(ready); Sleep(INFINITE);
        }
        deny_unprivileged();
        for (unsigned cycle=0; cycle<5; ++cycle) {
            open(s); CHECK(ShmCreate(&s));
            if (!cycle) {
                auto cb=(usbwarp_control_block *)s.shm_base;
                uint32_t magic=cb->magic,offset=cb->g2h_ring_offset;
                cb->magic=0;
                CHECK(!DrvClientSetupShm(&s) && !s.driverShmSetup);
                cb->magic=magic; cb->g2h_ring_offset=UINT32_MAX;
                CHECK(!DrvClientSetupShm(&s) && !s.driverShmSetup);
                cb->g2h_ring_offset=offset;
            }
            if (!cycle && s.doorbellEvent) {
                HANDLE event=s.doorbellEvent;
                s.doorbellEvent=s.section; // reject a handle of the wrong object type
                BOOL accepted=DrvClientSetupShm(&s);
                s.doorbellEvent=event;
                CHECK(!accepted && !s.driverShmSetup);
            }
            CHECK(DrvClientSetupShm(&s));
            if (s.driverCaps & USBWARP_DRIVER_CAP_H2G_IRQ) {
                s.msixPresent=true;
                CHECK(SUCCEEDED(CoCreateGuid(&s.instance_id)));
                s.irqNonce=1; s.irqRevision=0; s.msixTable[12]=1;
                CHECK(DrvClientConfigureIrq(&s));
                s.irqNonce=2;
                CHECK(!DrvClientConfigureIrq(&s)); // immutable session identity
                s.irqNonce=1;
                CHECK(DrvClientConfigureIrq(&s));
                s.msixPresent=false;
            }
            if (s.doorbellEvent) CHECK(SetEvent(s.doorbellEvent));
            CHECK(DrvClientHeartbeat(&s) && s.driverState==USBWARP_STATE_READY);
            USBWARP_QUERY_STATUS_OUT status{};
            CHECK(DrvClientQueryStatus(&s,&status) && status.DriverState==USBWARP_STATE_READY &&
                  status.ServiceRegistered && status.ShmEstablished && !status.ActiveDeviceCount);
            CHECK(DrvClientClose(&s)==ERROR_SUCCESS);
            ShmDestroy(&s);
        }
        // No USB device is bound: exercise malicious peer framing without
        // forwarding any device requests to physical hardware.
        open(s); CHECK(ShmCreate(&s) && DrvClientSetupShm(&s));
        {
            auto cb=(usbwarp_control_block *)s.shm_base;
            auto ring=(usbwarp_ring_header *)((BYTE *)s.shm_base+cb->g2h_ring_offset);
            BYTE *data=(BYTE *)ring+USBWARP_RING_HEADER_SIZE;
            usbwarp_msg_header bad{}; bad.magic=USBWARP_MSG_MAGIC;
            bad.protocol_version=USBWARP_PROTOCOL_VERSION;
            bad.message_type=USBWARP_MSG_HEARTBEAT; bad.message_length=UINT32_MAX-62;
            usbwarp_msg_heartbeat good{};
            good.hdr.magic=USBWARP_MSG_MAGIC; good.hdr.protocol_version=USBWARP_PROTOCOL_VERSION;
            good.hdr.message_type=USBWARP_MSG_HEARTBEAT; good.hdr.message_length=sizeof(good);
            memcpy(data,&bad,sizeof(bad)); memcpy(data+64,&good,sizeof(good));
            ring->data_size=ring->data_size_mask=ring->max_message_size=UINT32_MAX;
            MemoryBarrier(); InterlockedExchange((LONG *)&ring->producer_index,128);
            if(s.doorbellEvent) SetEvent(s.doorbellEvent);
            ULONGLONG until=GetTickCount64()+5000;
            while(InterlockedCompareExchange((LONG *)&ring->consumer_index,0,0)!=128 && GetTickCount64()<until) Sleep(10);
            CHECK(ring->consumer_index==128);
            USBWARP_QUERY_STATUS_OUT status{};
            CHECK(DrvClientQueryStatus(&s,&status) && status.DriverState==USBWARP_STATE_READY);
            InterlockedExchange((LONG *)&ring->consumer_index,0);
            InterlockedExchange((LONG *)&ring->producer_index,s.cfg.ring_data_size+64);
            if(s.doorbellEvent) SetEvent(s.doorbellEvent);
            until=GetTickCount64()+5000;
            do {CHECK(DrvClientQueryStatus(&s,&status));if(status.DriverState==USBWARP_STATE_ERROR)break;Sleep(10);} while(GetTickCount64()<until);
            CHECK(status.DriverState==USBWARP_STATE_ERROR && !status.ActiveDeviceCount);
        }
        CHECK(DrvClientClose(&s)==ERROR_SUCCESS); ShmDestroy(&s);
        open(s); CHECK(DrvClientClose(&s)==ERROR_SUCCESS); // registered without SHM
        WCHAR name[128]; swprintf_s(name,L"Local\\UsbWarpKernelTest.%lu",GetCurrentProcessId());
        ready=CreateEventW(nullptr,TRUE,FALSE,name); CHECK(ready);
        WCHAR executable[32768]; CHECK(GetModuleFileNameW(nullptr,executable,_countof(executable)));
        std::wstring command=L"\""+std::wstring(executable)+L"\" --hold "+name;
        STARTUPINFOW start{}; start.cb=sizeof(start); start.dwFlags=STARTF_USESHOWWINDOW; start.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION process{};
        CHECK(CreateProcessW(executable,&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&process));
        CloseHandle(process.hThread); child=process.hProcess;
        HANDLE events[]={ready,child}; CHECK(WaitForMultipleObjects(2,events,FALSE,10000)==WAIT_OBJECT_0);
        CHECK(TerminateProcess(child,99)); CHECK(WaitForSingleObject(child,10000)==WAIT_OBJECT_0);
        CloseHandle(child); child=nullptr; CloseHandle(ready); ready=nullptr;
        open(s); CHECK(ShmCreate(&s) && DrvClientSetupShm(&s) && DrvClientHeartbeat(&s));
        CHECK(DrvClientClose(&s)==ERROR_SUCCESS); finish(s);
        printf("Real kernel session lifetime: %u checks passed (no USB binding or HDV)\n",checks);
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr,"Kernel session regression failed: %s\n",error.what());
        if (child) { TerminateProcess(child,1); WaitForSingleObject(child,5000); CloseHandle(child); }
        if (ready) CloseHandle(ready);
        if (initialized) finish(s);
        return 1;
    }
}
