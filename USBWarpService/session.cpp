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
    if (StopRequested(s)) s->stage.store(USBWARP_SESSION_STOPPING);
    else {
        warp_target target{};
        uint64_t generation = VmSelection(s, &target);
        VmStage(s, generation, target.kind == USBWARP_TARGET_NONE ?
            USBWARP_SESSION_WAIT_TARGET : USBWARP_SESSION_RETRYING);
    }
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
    VmDetached(s);
    return true;
}
}

DWORD WINAPI SessionWorker(void *context)
{
    auto s = (warp_session *)context;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { RequestStop(s, HRESULT_CODE(initialized)); return ERROR_GEN_FAILURE; }
    while (!StopRequested(s)) {
        VmRefresh(s); // discovery/control remains available while the driver is absent
        warp_target initialTarget{};
        uint64_t initialGeneration = VmSelection(s, &initialTarget);
        VmStage(s, initialGeneration, initialTarget.kind == USBWARP_TARGET_NONE ?
            USBWARP_SESSION_WAIT_TARGET : USBWARP_SESSION_WAIT_DRIVER);
        ResetEvent(s->sessionStopEvent);
        ResetEvent(s->driverFaultEvent);
        if (!DrvClientOpen(s)) {
            s->stage.store(USBWARP_SESSION_WAIT_DRIVER);
            s->lastSessionError.store(GetLastError());
            if (WaitForSingleObject(s->stopEvent, 3000) == WAIT_OBJECT_0) break;
            continue;
        }
        bool registered = DrvClientRegister(s) && DeviceRecover(s);
        if (registered) {
            warp_target selected{};
            uint64_t selectedGeneration = VmSelection(s, &selected);
            if (selected.kind == USBWARP_TARGET_NONE) {
                // Recover abandoned captures, but do not allocate/start a
                // shared-memory transport for an unconfigured service.
                if (!CloseSession(s)) break;
                VmStage(s, selectedGeneration, USBWARP_SESSION_WAIT_TARGET);
                HANDLE idle[] = {s->stopEvent, s->targetChangedEvent};
                DWORD wait = WaitForMultipleObjects(2, idle, FALSE, 5000);
                if (wait == WAIT_OBJECT_0) break;
                if (wait == WAIT_FAILED) { RequestStop(s, GetLastError()); break; }
                continue;
            }
        }
        if (!registered || !ShmCreate(s) || !DrvClientSetupShm(s)) {
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
        HANDLE waiting[] = {s->stopEvent, s->driverFaultEvent, s->targetChangedEvent};
        bool opened = false;
        uint64_t generation = 0;
        while (WaitForMultipleObjects(2, waiting, FALSE, 0) == WAIT_TIMEOUT) {
            // A captured device may have been unplugged during recovery and
            // reappear while WSL is stopped. Return it independently of HDV.
            DeviceReconcile(s);
            VmRefresh(s);
            warp_target target{};
            generation = VmSelection(s, &target);
            DWORD error = VmResolve(s, target, generation);
            if (!error) {
                if (!VmStage(s, generation, USBWARP_SESSION_CONNECTING)) continue;
                if (HdvSessionOpen(s)) {
                    opened = VmAttached(s, generation);
                    break; // a superseded open must drain before the new target is tried
                }
                error = GetLastError();
                VmFailure(s, generation, error);
            }
            s->lastSessionError.store(error);
            if (s->hdv_host) break; // mapped or failed cleanup: drain through CloseSession
            VmStage(s, generation, target.kind == USBWARP_TARGET_NONE ?
                USBWARP_SESSION_WAIT_TARGET : USBWARP_SESSION_WAIT_VM);
            DWORD wait = WaitForMultipleObjects(3, waiting, FALSE, 5000);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) break;
            if (wait == WAIT_FAILED) { RequestStop(s, GetLastError()); break; }
        }
        if (opened) {
            VmStage(s, generation, USBWARP_SESSION_WAIT_GUEST);
            HANDLE events[] = {s->stopEvent, s->driverFaultEvent, s->memSpaceEvent, s->targetChangedEvent};
            ULONGLONG nextReconcile = GetTickCount64() + 2000;
            ULONGLONG nextDiscovery = GetTickCount64() + 5000;
            while (!StopRequested(s) && !s->hdv_torn_down.load() && s->hdv_started.load() &&
                   VmCurrent(s, generation) &&
                   WaitForSingleObject(s->driverFaultEvent, 0) != WAIT_OBJECT_0) {
                if (s->mem_space_on.load() && !s->mmio_mapped.load()) {
                    if (!HdvSessionMapMmio(s)) {
                        s->lastSessionError.store(GetLastError());
                        break;
                    }
                }
                if (s->mmio_mapped.load()) {
                    uint32_t state = s->driverState.load();
                    VmStage(s, generation, state == USBWARP_STATE_RUNNING ? USBWARP_SESSION_RUNNING : USBWARP_SESSION_WAIT_GUEST);
                    if (state == USBWARP_STATE_ERROR || state == USBWARP_STATE_STOPPED ||
                        state == USBWARP_STATE_SHUTTING_DOWN) break;
                }
                if (GetTickCount64() >= nextReconcile) {
                    DeviceReconcile(s);
                    nextReconcile = GetTickCount64() + 2000;
                }
                if (GetTickCount64() >= nextDiscovery) {
                    VmRefresh(s);
                    nextDiscovery = GetTickCount64() + 5000;
                }
                DWORD wait = WaitForMultipleObjects(4, events, FALSE, 250);
                if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) break;
                if (wait == WAIT_OBJECT_0 + 3 && !VmCurrent(s, generation)) break;
                if (wait == WAIT_OBJECT_0 + 2) ResetEvent(s->memSpaceEvent);
                if (wait == WAIT_FAILED) { RequestStop(s, GetLastError()); break; }
            }
        }
        if (!CloseSession(s)) break;
        if (!StopRequested(s)) {
            LogInfo("Session ended; resolving the selected target again");
            HANDLE retry[] = {s->stopEvent, s->targetChangedEvent};
            if (WaitForMultipleObjects(2, retry, FALSE, 1000) == WAIT_OBJECT_0) break;
        }
    }
    if (!s->hdv_host) CloseSession(s);
    CoUninitialize();
    return s->fatalError.load();
}
