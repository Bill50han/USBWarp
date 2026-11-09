// SPDX-License-Identifier: GPL-2.0-only
// Runs the production SCM status/handler code with a simulated SCM API.
#include "../USBWarpService/warp_service.h"
#include <stdio.h>
#include <vector>
#include <assert.h>

warp_session g_sess;
static std::vector<SERVICE_STATUS> reported;
static LPHANDLER_FUNCTION_EX handler;
static void *handlerContext;
static DWORD runError, dispatcherError;
static bool logAvailable=true;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)

static SERVICE_STATUS_HANDLE WINAPI Register(LPCWSTR name, LPHANDLER_FUNCTION_EX callback, void *context)
{ assert(!wcscmp(name,USBWARP_SERVICE_NAME)); handler=callback; handlerContext=context; return (SERVICE_STATUS_HANDLE)1; }
static BOOL WINAPI Report(SERVICE_STATUS_HANDLE, LPSERVICE_STATUS status)
{ reported.push_back(*status); return TRUE; }
static BOOL WINAPI Dispatcher(const SERVICE_TABLE_ENTRYW *table)
{
    if (dispatcherError) { SetLastError(dispatcherError); return FALSE; }
    WCHAR *args[]={table[0].lpServiceName}; table[0].lpServiceProc(1,args); return TRUE;
}
#define RegisterServiceCtrlHandlerExW Register
#define SetServiceStatus Report
#define StartServiceCtrlDispatcherW Dispatcher
#include "../USBWarpService/winsvc.cpp"
#undef RegisterServiceCtrlHandlerExW
#undef SetServiceStatus
#undef StartServiceCtrlDispatcherW

BOOL LogInit(bool) { return logAvailable; }
void LogCleanup() {}
void LogInfo(const char *, ...) {}
void LogError(const char *, ...) {}
void RequestStop(warp_session *s, DWORD error) { s->shutdown_requested=true; s->fatalError=error; }
DWORD LoadConfig(int, WCHAR **, warp_config *, bool) { return ERROR_INVALID_PARAMETER; }
DWORD SaveRegistryConfig(const warp_config *) { return ERROR_INVALID_PARAMETER; }
DWORD RemoveRegistryConfig() { return ERROR_INVALID_PARAMETER; }
DWORD DeviceCapturePending(bool *pending) { *pending=false; return ERROR_SUCCESS; }
void PrintUsage() {}
DWORD ServiceRun(warp_session *s, bool serviceMode, int, WCHAR **)
{
    assert(serviceMode && reported.back().dwCurrentState==SERVICE_START_PENDING);
    SvcRunning();
    assert(reported.back().dwControlsAccepted==(SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN|SERVICE_ACCEPT_PRESHUTDOWN));
    handler(SERVICE_CONTROL_INTERROGATE,0,nullptr,handlerContext);
    handler(SERVICE_CONTROL_PRESHUTDOWN,0,nullptr,handlerContext);
    assert(s->shutdown_requested && reported.back().dwCurrentState==SERVICE_STOP_PENDING);
    assert(!reported.back().dwControlsAccepted);
    DWORD checkpoint=reported.back().dwCheckPoint;
    SvcStopping(); assert(reported.back().dwCheckPoint==checkpoint+1);
    size_t count=reported.size(); SvcRunning(); assert(count==reported.size());
    return runError;
}
static void reset()
{
    reported.clear(); currentStatus={}; statusHandle=nullptr; terminal=false; serviceExit=0;
    g_sess.shutdown_requested=false;
}
int main()
{
    CHECK(RunServiceDispatcher()==0);
    CHECK(reported.front().dwCurrentState==SERVICE_START_PENDING && !reported.front().dwControlsAccepted);
    CHECK(reported.back().dwCurrentState==SERVICE_STOPPED && !reported.back().dwWin32ExitCode);
    CHECK(!reported.back().dwCheckPoint && !reported.back().dwWaitHint);
    size_t count=reported.size(); SvcStopping(); SvcRunning(); CHECK(count==reported.size());
    reset(); runError=ERROR_INVALID_DATA;
    CHECK(RunServiceDispatcher()==ERROR_INVALID_DATA && reported.back().dwWin32ExitCode==ERROR_INVALID_DATA);
    reset(); logAvailable=false;
    CHECK(RunServiceDispatcher()==ERROR_WRITE_FAULT && reported.back().dwCurrentState==SERVICE_STOPPED);
    CHECK(reported.size()==2);
    dispatcherError=ERROR_FAILED_SERVICE_CONTROLLER_CONNECT; CHECK(RunServiceDispatcher()==-1);
    dispatcherError=ERROR_ACCESS_DENIED; CHECK(RunServiceDispatcher()==ERROR_ACCESS_DENIED);
    printf("SCM status/controls: %u checks passed\n",checks);
    return 0;
}
