// SPDX-License-Identifier: GPL-2.0-only
#include "warp_service.h"
#include "../include/usbwarp_version.h"
#include <stdio.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

warp_session g_sess;
static warp_session *consoleSession;

bool StopRequested(const warp_session *s) { return s->shutdown_requested.load(); }
void RequestStop(warp_session *s, DWORD error)
{
    if (error) {
        DWORD expected = ERROR_SUCCESS;
        s->fatalError.compare_exchange_strong(expected, error);
    }
    s->shutdown_requested.store(true);
    AcquireSRWLockShared(&s->lifecycleLock);
    if (s->stopEvent) SetEvent(s->stopEvent);
    if (s->sessionStopEvent) SetEvent(s->sessionStopEvent);
    ReleaseSRWLockShared(&s->lifecycleLock);
}

void JoinServiceThread(warp_session *s, HANDLE thread)
{
    if (!thread) return;
    while (WaitForSingleObject(thread, 1000) != WAIT_OBJECT_0) {
        if (StopRequested(s)) SvcStopping();
        // A timeout never grants permission to free a worker's resources.
    }
}

static BOOL WINAPI ConsoleControl(DWORD control)
{
    if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT && control != CTRL_CLOSE_EVENT &&
        control != CTRL_SHUTDOWN_EVENT) return FALSE;
    if (consoleSession) RequestStop(consoleSession);
    return TRUE;
}

DWORD ServiceRun(warp_session *s, bool serviceMode, int argc, WCHAR *argv[])
{
    s->serviceMode = serviceMode;
    DWORD error = LoadConfig(argc, argv, &s->cfg);
    if (error) { LogError("Invalid service configuration: %lu", error); return error; }
    s->selectedTarget = s->cfg.target;
    s->targetError = s->cfg.target.kind == USBWARP_TARGET_NONE ? ERROR_SUCCESS : ERROR_NOT_READY;
    s->start_time_ns = NowNs();
    AcquireSRWLockExclusive(&s->lifecycleLock);
    s->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s->stopEvent) error = GetLastError();
    if (!error) {
        s->sessionStopEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (!s->sessionStopEvent) error = GetLastError();
    }
    if (!error) {
        s->driverFaultEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!s->driverFaultEvent) error = GetLastError();
    }
    if (!error) {
        s->targetChangedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!s->targetChangedEvent) error = GetLastError();
    }
    ReleaseSRWLockExclusive(&s->lifecycleLock);
    if (error) goto cleanup;
    if (!serviceMode) {
        consoleSession = s;
        if (!SetConsoleCtrlHandler(ConsoleControl, TRUE)) { error = GetLastError(); goto cleanup; }
    }
    if (!HdvLoadApis(&s->api)) { error = ERROR_NOT_SUPPORTED; goto cleanup; }
    if (!PipeServerStart(s)) { error = GetLastError(); goto cleanup; }
    s->sessionThread = CreateThread(nullptr, 0, SessionWorker, s, 0, nullptr);
    if (!s->sessionThread) { error = GetLastError(); goto cleanup; }
    // IPC is listening, events/workers exist, and STOP is now actionable.
    SvcRunning();
    {
        HANDLE events[] = {s->stopEvent, s->sessionThread};
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wait == WAIT_FAILED) error = GetLastError();
        else if (wait == WAIT_OBJECT_0 + 1 && !StopRequested(s)) error = ERROR_SERVICE_NOT_ACTIVE;
    }
cleanup:
    RequestStop(s, error);
    s->stage.store(USBWARP_SESSION_STOPPING);
    SvcStopping();
    PipeServerStop(s);
    JoinServiceThread(s, s->sessionThread);
    if (s->sessionThread) { CloseHandle(s->sessionThread); s->sessionThread = nullptr; }
    if (!serviceMode && consoleSession) {
        SetConsoleCtrlHandler(ConsoleControl, FALSE);
        consoleSession = nullptr;
    }
    // Failed native teardown may still have callbacks: retain its DLL/events
    // until process termination instead of freeing memory underneath them.
    if (!s->hdv_host) {
        HdvUnloadApis(&s->api);
        AcquireSRWLockExclusive(&s->lifecycleLock);
        if (s->driverFaultEvent) CloseHandle(s->driverFaultEvent);
        if (s->targetChangedEvent) CloseHandle(s->targetChangedEvent);
        if (s->sessionStopEvent) CloseHandle(s->sessionStopEvent);
        if (s->stopEvent) CloseHandle(s->stopEvent);
        s->driverFaultEvent = s->sessionStopEvent = s->stopEvent = nullptr;
        s->targetChangedEvent = nullptr;
        ReleaseSRWLockExclusive(&s->lifecycleLock);
    }
    s->stage.store(USBWARP_SESSION_STOPPED);
    return s->fatalError.load();
}

int wmain(int argc, WCHAR *argv[])
{
    bool install = false, uninstall = false;
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--version")) {
            printf("USBWarp Service v%s\n", USBWARP_VERSION_STRING);
            return 0;
        }
        if (!_wcsicmp(argv[i], L"--help") || !_wcsicmp(argv[i], L"-h")) { PrintUsage(); return 0; }
        if (!_wcsicmp(argv[i], L"--install")) install = true;
        if (!_wcsicmp(argv[i], L"--uninstall")) uninstall = true;
    }
    if (install && uninstall) { PrintUsage(); return ERROR_INVALID_PARAMETER; }
    if (install) return SvcInstall(argc, argv);
    if (uninstall) return argc == 2 ? SvcUninstall() : ERROR_INVALID_PARAMETER;
    int dispatched = RunServiceDispatcher();
    if (dispatched != -1) return dispatched;
    if (!LogInit(false)) return ERROR_WRITE_FAULT;
    DWORD error = ServiceRun(&g_sess, false, argc, argv);
    if (!g_sess.hdv_host) LogCleanup();
    return (int)error;
}
