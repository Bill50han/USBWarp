// SPDX-License-Identifier: GPL-2.0-only
#include "warp_service.h"
#include <stdio.h>
#include <string>
#include <vector>

namespace {
SRWLOCK statusLock = SRWLOCK_INIT;
SERVICE_STATUS_HANDLE statusHandle;
SERVICE_STATUS currentStatus{};
bool terminal;
DWORD serviceExit;

void SetState(DWORD state, DWORD error = ERROR_SUCCESS)
{
    AcquireSRWLockExclusive(&statusLock);
    if (!statusHandle || terminal ||
        (currentStatus.dwCurrentState == SERVICE_STOP_PENDING &&
         (state == SERVICE_RUNNING || state == SERVICE_START_PENDING))) {
        ReleaseSRWLockExclusive(&statusLock);
        return;
    }
    DWORD previous = currentStatus.dwCurrentState;
    currentStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    currentStatus.dwCurrentState = state;
    currentStatus.dwWin32ExitCode = error;
    currentStatus.dwControlsAccepted = state == SERVICE_RUNNING ?
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN : 0;
    bool pending = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING;
    currentStatus.dwWaitHint = pending ? 30000 : 0;
    currentStatus.dwCheckPoint = pending ? (previous == state ? currentStatus.dwCheckPoint + 1 : 1) : 0;
    SetServiceStatus(statusHandle, &currentStatus);
    if (state == SERVICE_STOPPED) terminal = true;
    ReleaseSRWLockExclusive(&statusLock);
}

DWORD WINAPI Control(DWORD control, DWORD, void *, void *context)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_PRESHUTDOWN:
        // No logging, I/O, joins or teardown on the SCM control-handler thread.
        RequestStop((warp_session *)context);
        SetState(SERVICE_STOP_PENDING);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        AcquireSRWLockShared(&statusLock);
        if (statusHandle && !terminal) SetServiceStatus(statusHandle, &currentStatus);
        ReleaseSRWLockShared(&statusLock);
        return NO_ERROR;
    default: return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD argc, WCHAR **argv)
{
    statusHandle = RegisterServiceCtrlHandlerExW(USBWARP_SERVICE_NAME, Control, &g_sess);
    if (!statusHandle) { serviceExit = GetLastError(); return; }
    SetState(SERVICE_START_PENDING);
    if (!LogInit(true)) {
        serviceExit = ERROR_WRITE_FAULT;
    } else {
        SvcCheckpoint();
        serviceExit = ServiceRun(&g_sess, true, (int)argc, argv);
        if (serviceExit) LogError("Service exited with error %lu", serviceExit);
        else LogInfo("Service stopped after all workers and sessions drained");
        if (!g_sess.hdv_host) LogCleanup();
    }
    SetState(SERVICE_STOPPED, serviceExit);
}

DWORD WaitStopped(SC_HANDLE service)
{
    ULONGLONG end = GetTickCount64() + 120000;
    bool requested = false;
    for (;;) {
        SERVICE_STATUS_PROCESS state{};
        DWORD bytes;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (BYTE *)&state, sizeof(state), &bytes))
            return GetLastError();
        if (state.dwCurrentState == SERVICE_STOPPED) return ERROR_SUCCESS;
        if (!requested && state.dwCurrentState != SERVICE_START_PENDING && state.dwCurrentState != SERVICE_STOP_PENDING) {
            SERVICE_STATUS ignored;
            if (!ControlService(service, SERVICE_CONTROL_STOP, &ignored)) {
                DWORD error = GetLastError();
                if (error != ERROR_SERVICE_NOT_ACTIVE && error != ERROR_SERVICE_CANNOT_ACCEPT_CTRL) return error;
            } else requested = true;
        }
        if (GetTickCount64() >= end) return ERROR_SERVICE_REQUEST_TIMEOUT;
        Sleep(250);
    }
}
}

void SvcCheckpoint() { SetState(SERVICE_START_PENDING); }
void SvcRunning() { SetState(SERVICE_RUNNING); }
void SvcStopping() { SetState(SERVICE_STOP_PENDING); }

int RunServiceDispatcher()
{
    SERVICE_TABLE_ENTRYW table[] = {{(LPWSTR)USBWARP_SERVICE_NAME, ServiceMain}, {nullptr, nullptr}};
    if (StartServiceCtrlDispatcherW(table)) return (int)serviceExit;
    DWORD error = GetLastError();
    return error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT ? -1 : (int)error;
}

int SvcInstall(int argc, WCHAR *argv[])
{
    warp_config config;
    DWORD error = LoadConfig(argc, argv, &config, true);
    if (error) { fprintf(stderr, "Invalid install configuration (%lu)\n", error); PrintUsage(); return (int)error; }
    std::vector<WCHAR> path(32768);
    DWORD n = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    if (!n || n >= path.size()) return ERROR_FILENAME_EXCED_RANGE;
    std::wstring command = L"\"" + std::wstring(path.data(), n) + L"\"";
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!manager) return (int)GetLastError();
    for (const WCHAR *dep = USBWARP_SERVICE_DEPENDENCIES; *dep; dep += wcslen(dep) + 1) {
        SC_HANDLE dependency = OpenServiceW(manager, dep, SERVICE_QUERY_STATUS);
        if (!dependency) {
            error = GetLastError();
            fprintf(stderr, "Required driver service %ls is not installed/accessible (%lu)\n", dep, error);
            CloseServiceHandle(manager); return (int)error;
        }
        CloseServiceHandle(dependency);
    }
    bool created = false;
    SC_HANDLE service = OpenServiceW(manager, USBWARP_SERVICE_NAME,
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START | DELETE);
    if (!service) {
        error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            service = CreateServiceW(manager, USBWARP_SERVICE_NAME, USBWARP_SERVICE_NAME,
                SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START | DELETE,
                SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                command.c_str(), nullptr, nullptr, USBWARP_SERVICE_DEPENDENCIES, nullptr, nullptr);
            created = service != nullptr;
            error = service ? ERROR_SUCCESS : GetLastError();
        }
    } else {
        error = ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
            command.c_str(), nullptr, nullptr, USBWARP_SERVICE_DEPENDENCIES, nullptr, nullptr, nullptr) ?
            ERROR_SUCCESS : GetLastError();
    }
    if (service && !error) {
        SERVICE_DESCRIPTIONW description{(LPWSTR)USBWARP_SERVICE_DESCR};
        SC_ACTION actions[] = {{SC_ACTION_RESTART,5000}, {SC_ACTION_RESTART,15000},
                               {SC_ACTION_RESTART,60000}, {SC_ACTION_NONE,0}};
        SERVICE_FAILURE_ACTIONSW recovery{};
        recovery.dwResetPeriod = 86400;
        recovery.cActions = _countof(actions); recovery.lpsaActions = actions;
        SERVICE_FAILURE_ACTIONS_FLAG failures{TRUE};
        SERVICE_PRESHUTDOWN_INFO shutdown{180000};
        if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description) ||
            !ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &recovery) ||
            !ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failures) ||
            !ChangeServiceConfig2W(service, SERVICE_CONFIG_PRESHUTDOWN_INFO, &shutdown))
            error = GetLastError();
        if (!error) error = SaveRegistryConfig(&config);
    }
    if (error && created) DeleteService(service); // never leave a new half-configured service
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (error) fprintf(stderr, "Service installation/update failed (%lu)\n", error);
    else printf("%ls %s. Start/restart it to apply VM=%ls.\n", USBWARP_SERVICE_NAME,
                created ? "installed" : "updated", config.vm_id);
    return (int)error;
}

int SvcUninstall()
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return (int)GetLastError();
    SC_HANDLE service = OpenServiceW(manager, USBWARP_SERVICE_NAME, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
    DWORD error = service ? WaitStopped(service) : GetLastError();
    if (!error || error == ERROR_SERVICE_DOES_NOT_EXIST) {
        bool pending=false;
        DWORD check=DeviceCapturePending(&pending);
        if (check || pending) error=check ? check : ERROR_BUSY;
    }
    if (service && !error && !DeleteService(service)) error = GetLastError();
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (error == ERROR_SERVICE_DOES_NOT_EXIST) error = ERROR_SUCCESS;
    if (!error) error = RemoveRegistryConfig();
    if (error) fprintf(stderr, "Service removal failed (%lu); configuration retained if stop failed.\n", error);
    else printf("%ls uninstalled.\n", USBWARP_SERVICE_NAME);
    return (int)error;
}
