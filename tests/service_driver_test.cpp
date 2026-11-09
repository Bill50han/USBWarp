// SPDX-License-Identifier: GPL-2.0-only
// Includes the real driver client. No real device handles or IOCTLs are used.
#include "../USBWarpService/warp_service.h"
#include <stdio.h>
#include <vector>
#include <mutex>

static HANDLE fakeDriver = (HANDLE)(ULONG_PTR)0x123456;
static HANDLE entered, canceled;
static OVERLAPPED *pending;
static bool asyncBind;
static std::mutex traceLock;
static std::vector<DWORD> trace;
static void record(DWORD code) { std::lock_guard<std::mutex> lock(traceLock); trace.push_back(code); }
static BOOL WINAPI FakeClose(HANDLE h) { if (h != fakeDriver) return CloseHandle(h); record(0xff00); return TRUE; }
static HANDLE WINAPI FakeOpen(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                               DWORD creation, DWORD flags, HANDLE templateFile)
{
    (void)access; (void)share; (void)sa; (void)creation; (void)templateFile;
    if (wcscmp(path,L"\\\\.\\UsbWarp") || !(flags & FILE_FLAG_OVERLAPPED)) return INVALID_HANDLE_VALUE;
    return fakeDriver;
}
static BOOL WINAPI FakeIoctl(HANDLE h, DWORD code, LPVOID input, DWORD inputSize,
                             LPVOID output, DWORD outputSize, LPDWORD returned, LPOVERLAPPED ov)
{
    (void)input; (void)inputSize;
    if (h != fakeDriver || !ov) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    record(code);
    if (asyncBind && code == IOCTL_USBWARP_BIND_DEVICE) {
        pending = ov; SetEvent(entered); SetLastError(ERROR_IO_PENDING); return FALSE;
    }
    if (outputSize) {
        memset(output,0,outputSize);
        *(uint32_t *)output = USBWARP_IOCTL_STRUCT_VERSION;
    }
    if (returned) *returned = outputSize;
    return TRUE;
}
static BOOL WINAPI FakeCancel(HANDLE, LPOVERLAPPED) { record(0xff01); SetEvent(canceled); return TRUE; }
static BOOL WINAPI FakeResult(HANDLE, LPOVERLAPPED ov, LPDWORD count, BOOL wait)
{
    if (wait) WaitForSingleObject(ov->hEvent, INFINITE);
    record(0xff02); if (count) *count = 0; SetLastError(ERROR_OPERATION_ABORTED); return FALSE;
}
#define CreateFileW FakeOpen
#define CloseHandle FakeClose
#define DeviceIoControl FakeIoctl
#define CancelIoEx FakeCancel
#define GetOverlappedResult FakeResult
#include "../USBWarpService/usbwarp_drv_client.cpp"
#undef CreateFileW
#undef CloseHandle
#undef DeviceIoControl
#undef CancelIoEx
#undef GetOverlappedResult

bool StopRequested(const warp_session *s) { return s->shutdown_requested.load(); }
static DWORD WINAPI Bind(void *context)
{
    auto s = (warp_session *)context;
    uint8_t guid[16]{};
    return DrvClientBindGuid(s, guid, L"\\??\\USB#TEST", 2, nullptr, nullptr, nullptr) ? 1 : 0;
}
static DWORD WINAPI Close(void *context) { return DrvClientClose((warp_session *)context); }
static DWORD WINAPI IrqClosing(void *context) { return DrvClientConfigureIrq((warp_session *)context) ? 1 : 0; }
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    warp_session s;
    s.stopEvent = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    s.sessionStopEvent = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    s.driverFaultEvent = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    entered = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    canceled = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    CHECK(DrvClientOpen(&s));
    CHECK(DrvClientRegister(&s));
    CHECK(DrvClientClose(&s) == ERROR_SUCCESS);
    CHECK(trace.size() == 3 && trace[0] == IOCTL_USBWARP_REGISTER_SERVICE &&
          trace[1] == IOCTL_USBWARP_TEARDOWN_SHM && trace[2] == 0xff00);
    CHECK(!s.hDriver && !s.driverRegistered && !s.driverShmSetup);
    trace.clear(); ResetEvent(s.sessionStopEvent);
    CHECK(DrvClientOpen(&s) && DrvClientRegister(&s));
    s.section = (HANDLE)(ULONG_PTR)1; s.shm_size = USBWARP_SHM_SIZE_DEFAULT;
    warp_config_defaults(&s.cfg);
    CHECK(DrvClientSetupShm(&s));
    trace.clear(); asyncBind = true;
    HANDLE binder = CreateThread(nullptr,0,Bind,&s,0,nullptr);
    CHECK(WaitForSingleObject(entered,5000) == WAIT_OBJECT_0);
    HANDLE closer = CreateThread(nullptr,0,Close,&s,0,nullptr);
    CHECK(WaitForSingleObject(canceled,5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(binder,50) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(closer,50) == WAIT_TIMEOUT);
    SetEvent(pending->hEvent); // the kernel has now actually completed cancellation
    CHECK(WaitForSingleObject(binder,5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(closer,5000) == WAIT_OBJECT_0);
    CHECK(trace.size() == 5 && trace[0] == IOCTL_USBWARP_BIND_DEVICE && trace[1] == 0xff01 &&
          trace[2] == 0xff02 && trace[3] == IOCTL_USBWARP_TEARDOWN_SHM && trace[4] == 0xff00);
    s.msixPresent = true;
    AcquireSRWLockExclusive(&s.driverLock);
    HANDLE irq = CreateThread(nullptr,0,IrqClosing,&s,0,nullptr);
    DWORD irqWait = WaitForSingleObject(irq,1000);
    ReleaseSRWLockExclusive(&s.driverLock);
    CHECK(irqWait == WAIT_OBJECT_0); // teardown cannot wait on the ACK owner's driver lock
    DWORD irqResult = 0;
    CHECK(GetExitCodeThread(irq,&irqResult) && irqResult == 1);
    CloseHandle(irq);
    CloseHandle(binder); CloseHandle(closer); CloseHandle(entered); CloseHandle(canceled);
    CloseHandle(s.stopEvent); CloseHandle(s.sessionStopEvent); CloseHandle(s.driverFaultEvent);
    printf("Driver client lifetime: %u checks passed\n",checks);
    return 0;
}
