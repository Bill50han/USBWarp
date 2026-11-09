// SPDX-License-Identifier: GPL-2.0-only
#include "warp_service.h"

namespace {
DWORD WINAPI HeartbeatWorker(void *context)
{
    auto s = (warp_session *)context;
    HANDLE stop[] = {s->stopEvent, s->sessionStopEvent};
    while (WaitForMultipleObjects(2, stop, FALSE, 0) == WAIT_TIMEOUT) {
        if (!DrvClientHeartbeat(s)) {
            if (!StopRequested(s) && WaitForSingleObject(s->sessionStopEvent, 0) != WAIT_OBJECT_0) {
                DWORD error = GetLastError();
                s->lastSessionError.store(error ? error : ERROR_DEVICE_NOT_CONNECTED);
                SetEvent(s->driverFaultEvent);
            }
            break;
        }
        if (WaitForMultipleObjects(2, stop, FALSE, 2000) != WAIT_TIMEOUT) break;
    }
    return ERROR_SUCCESS;
}

bool CloseSession(warp_session *s)
{
    s->stage.store(StopRequested(s) ? USBWARP_SESSION_STOPPING : USBWARP_SESSION_RETRYING);
    SetEvent(s->sessionStopEvent);
    if (s->heartbeatThread) {
        WaitForSingleObject(s->heartbeatThread, INFINITE);
        CloseHandle(s->heartbeatThread);
        s->heartbeatThread = nullptr;
    }
    DWORD error = DeviceCloseSession(s);
    if (error) LogWarn("Session teardown/Windows driver restoration returned %lu; recovery state retained", error);
    // The kernel has drained USB I/O before HDV or section resources go away.
    if (!HdvSessionClose(s)) {
        LogError("HDV teardown failed; retaining callback resources until process exit");
        RequestStop(s, ERROR_GEN_FAILURE);
        return false;
    }
    ShmDestroy(s);
    return true;
}
}

DWORD WINAPI SessionWorker(void *context)
{
    auto s = (warp_session *)context;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { RequestStop(s, HRESULT_CODE(initialized)); return ERROR_GEN_FAILURE; }
    while (!StopRequested(s)) {
        s->stage.store(USBWARP_SESSION_WAIT_DRIVER);
        ResetEvent(s->sessionStopEvent);
        ResetEvent(s->driverFaultEvent);
        if (!DrvClientOpen(s)) {
            s->lastSessionError.store(GetLastError());
            if (WaitForSingleObject(s->stopEvent, 3000) == WAIT_OBJECT_0) break;
            continue;
        }
        if (!DrvClientRegister(s) || !DeviceRecover(s) || !ShmCreate(s) || !DrvClientSetupShm(s)) {
            DWORD error = GetLastError();
            s->lastSessionError.store(error);
            LogWarn("Driver session initialization failed: %lu", error);
            if (!CloseSession(s)) break;
            if (WaitForSingleObject(s->stopEvent, 3000) == WAIT_OBJECT_0) break;
            continue;
        }
        s->heartbeatThread = CreateThread(nullptr, 0, HeartbeatWorker, s, 0, nullptr);
        if (!s->heartbeatThread) {
            RequestStop(s, GetLastError());
            CloseSession(s);
            break;
        }

        // Waiting for the configured VM is a session state, never SCM START_PENDING.
        s->stage.store(USBWARP_SESSION_WAIT_VM);
        HANDLE waiting[] = {s->stopEvent, s->driverFaultEvent};
        bool opened = false;
        while (WaitForMultipleObjects(2, waiting, FALSE, 0) == WAIT_TIMEOUT) {
            // A captured device may have been unplugged during recovery and
            // reappear while WSL is stopped. Return it independently of HDV.
            DeviceReconcile(s);
            if (HdvSessionOpen(s)) { opened = true; break; }
            s->lastSessionError.store(GetLastError());
            if (s->hdv_host) break; // mapped or failed cleanup: drain through CloseSession
            s->stage.store(USBWARP_SESSION_WAIT_VM);
            if (WaitForMultipleObjects(2, waiting, FALSE, 5000) != WAIT_TIMEOUT) break;
        }
        if (opened) {
            s->stage.store(USBWARP_SESSION_WAIT_GUEST);
            HANDLE events[] = {s->stopEvent, s->driverFaultEvent, s->memSpaceEvent};
            ULONGLONG nextReconcile = GetTickCount64() + 2000;
            while (!StopRequested(s) && !s->hdv_torn_down.load() && s->hdv_started.load() &&
                   WaitForSingleObject(s->driverFaultEvent, 0) != WAIT_OBJECT_0) {
                if (s->mem_space_on.load() && !s->mmio_mapped.load()) {
                    if (!HdvSessionMapMmio(s)) {
                        s->lastSessionError.store(GetLastError());
                        break;
                    }
                }
                if (s->mmio_mapped.load()) {
                    uint32_t state = s->driverState.load();
                    s->stage.store(state == USBWARP_STATE_RUNNING ? USBWARP_SESSION_RUNNING : USBWARP_SESSION_WAIT_GUEST);
                    if (state == USBWARP_STATE_ERROR || state == USBWARP_STATE_STOPPED ||
                        state == USBWARP_STATE_SHUTTING_DOWN) break;
                }
                if (GetTickCount64() >= nextReconcile) {
                    DeviceReconcile(s);
                    nextReconcile = GetTickCount64() + 2000;
                }
                DWORD wait = WaitForMultipleObjects(3, events, FALSE, 250);
                if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) break;
                if (wait == WAIT_OBJECT_0 + 2) ResetEvent(s->memSpaceEvent);
                if (wait == WAIT_FAILED) { RequestStop(s, GetLastError()); break; }
            }
        }
        if (!CloseSession(s)) break;
        if (!StopRequested(s)) {
            LogInfo("Session ended; waiting to attach the configured VM again");
            if (WaitForSingleObject(s->stopEvent, 1000) == WAIT_OBJECT_0) break;
        }
    }
    if (!s->hdv_host) CloseSession(s);
    CoUninitialize();
    return s->fatalError.load();
}
