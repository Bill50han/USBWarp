/*
 * winsvc.cpp — SCM integration for UsbWarpService.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Dual-mode entry: wmain calls RunServiceDispatcher() first; started by
 * the SCM the process runs as a real Windows service, started from a
 * shell the dispatcher fails with ERROR_FAILED_SERVICE_CONTROLLER_CONNECT
 * and main.cpp falls back to the console build.
 *
 * --install persists the configuration under HKLM\SOFTWARE\UsbWarp\Service
 * (the auto-start service reads it at boot) and creates the service with
 * AUTO_START + crash-restart failure actions.  Config errors terminate
 * with a clean SERVICE_STOPPED + error code (no restart loop).
 */

#include "warp_service.h"
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Status reporting (no-ops in console mode)
 * ═══════════════════════════════════════════════════════════════════════════ */

static SERVICE_STATUS_HANDLE g_statusHandle;
static DWORD g_checkPoint;

static void SetState(DWORD state, DWORD exitCode, DWORD waitHint)
{
    SERVICE_STATUS st;

    if (!g_statusHandle)
        return;

    memset(&st, 0, sizeof(st));
    st.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    st.dwCurrentState     = state;
    st.dwWin32ExitCode    = exitCode;
    st.dwWaitHint         = waitHint;
    st.dwCheckPoint       = (state == SERVICE_START_PENDING ||
                             state == SERVICE_STOP_PENDING) ?
                            ++g_checkPoint : 0;
    st.dwControlsAccepted = (state == SERVICE_RUNNING) ?
                            (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;

    SetServiceStatus(g_statusHandle, &st);
}

void SvcCheckpoint(void) { SetState(SERVICE_START_PENDING, 0, 60000); }
void SvcRunning(void)    { SetState(SERVICE_RUNNING, 0, 0); }

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  ServiceMain + control handler
 * ═══════════════════════════════════════════════════════════════════════════ */

static DWORD WINAPI SvcHandlerEx(DWORD control, DWORD event,
                                 LPVOID data, LPVOID context)
{
    struct warp_session *s = (struct warp_session *)context;

    UNREFERENCED_PARAMETER(event);
    UNREFERENCED_PARAMETER(data);

    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        LogInfo("Service stop requested");
        SetState(SERVICE_STOP_PENDING, 0, 30000);
        s->shutdown_requested = true;   /* same flag the Ctrl handler uses */
        return NO_ERROR;
    default:
        return NO_ERROR;
    }
}

static VOID WINAPI UsbWarpServiceMain(DWORD argc, WCHAR **argv)
{
    struct warp_session *s = &g_sess;
    DWORD exitCode;

    g_statusHandle = RegisterServiceCtrlHandlerExW(
        USBWARP_SERVICE_NAME, SvcHandlerEx, s);
    if (!g_statusHandle)
        return;

    /* Init chain: HcsWait alone can block 15s, VM-wait retries in 10s
     * steps — a generous hint with checkpoints from ServiceRun. */
    SetState(SERVICE_START_PENDING, 0, 60000);

    LogInit(s, true);

    exitCode = ServiceRun(s, true, (int)argc, argv);

    /* Nothing may touch the log sink after this point. */
    SetState(SERVICE_STOPPED, exitCode, 0);
}

int RunServiceDispatcher(void)
{
    SERVICE_TABLE_ENTRYW table[] = {
        { (LPWSTR)USBWARP_SERVICE_NAME, UsbWarpServiceMain },
        { NULL, NULL }
    };

    if (StartServiceCtrlDispatcherW(table))
        return 0;
    if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
        return -1;              /* not launched by the SCM */
    return (int)GetLastError();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  --install / --uninstall
 * ═══════════════════════════════════════════════════════════════════════════ */

static void WriteRegDword(HKEY key, const WCHAR *name, uint32_t value)
{
    RegSetValueExW(key, name, 0, REG_DWORD,
                   (const BYTE *)&value, sizeof(value));
}

static BOOL WriteRegConfig(const struct warp_config *cfg)
{
    HKEY key;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, USBWARP_SERVICE_REG_KEY,
                        0, NULL, 0, KEY_WRITE, NULL, &key, NULL) !=
        ERROR_SUCCESS)
        return FALSE;

    RegSetValueExW(key, L"VmId", 0, REG_SZ, (const BYTE *)cfg->vm_id,
                   (DWORD)((wcslen(cfg->vm_id) + 1) * sizeof(WCHAR)));
    WriteRegDword(key, L"ShmSizeMb", cfg->shm_size / (1024u * 1024u));
    WriteRegDword(key, L"RingSizeKb", cfg->ring_data_size / 1024u);
    WriteRegDword(key, L"BufferSizeKb", cfg->buffer_size / 1024u);
    WriteRegDword(key, L"MaxDevices", cfg->max_devices);
    WriteRegDword(key, L"EnableIso", cfg->enable_iso ? 1 : 0);

    RegCloseKey(key);
    return TRUE;
}

int SvcInstall(int argc, WCHAR *argv[])
{
    struct warp_config cfg;
    WCHAR path[MAX_PATH];
    SC_HANDLE scm, svc;

    warp_config_defaults(&cfg);
    if (!ParseArgOverrides(argc, argv, &cfg) || cfg.vm_id[0] == L'\0') {
        fprintf(stderr, "--install requires --id <VM-GUID> [options]\n");
        return 1;
    }

    if (!WriteRegConfig(&cfg)) {
        fprintf(stderr, "Failed to write HKLM\\%ls (%lu)\n",
                USBWARP_SERVICE_REG_KEY, GetLastError());
        return 1;
    }

    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) {
        fprintf(stderr, "GetModuleFileName failed (%lu)\n", GetLastError());
        return 1;
    }

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed (%lu) — run as administrator\n",
                GetLastError());
        return 1;
    }

    svc = CreateServiceW(
        scm, USBWARP_SERVICE_NAME, USBWARP_SERVICE_NAME,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, path, NULL, NULL, NULL, NULL, NULL);

    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS)
            fprintf(stderr, "Service already installed (config updated).\n");
        else
            fprintf(stderr, "CreateService failed (%lu)\n", err);
        CloseServiceHandle(scm);
        return (err == ERROR_SERVICE_EXISTS) ? 0 : 1;
    }

    {
        SERVICE_DESCRIPTIONW descr = { (LPWSTR)USBWARP_SERVICE_DESCR };
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &descr);
    }
    {
        /* Restart on crash (process death without clean SERVICE_STOPPED).
         * Clean stops and config errors do NOT restart. */
        SC_ACTION actions[1] = { { SC_ACTION_RESTART, 5000 } };
        SERVICE_FAILURE_ACTIONSW fa;
        memset(&fa, 0, sizeof(fa));
        fa.cActions    = 1;
        fa.lpsaActions = actions;
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    printf("UsbWarpService installed (auto-start, VM=%ls).\n", cfg.vm_id);
    printf("Start with: sc start UsbWarpService\n");
    return 0;
}

int SvcUninstall(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;
    int i;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed (%lu) — run as administrator\n",
                GetLastError());
        return 1;
    }

    svc = OpenServiceW(scm, USBWARP_SERVICE_NAME,
                       DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        fprintf(stderr, "Service not installed.\n");
        CloseServiceHandle(scm);
        return 0;
    }

    if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
        for (i = 0; i < 30; i++) {
            if (!QueryServiceStatus(svc, &st)) break;
            if (st.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(1000);
        }
    }

    if (!DeleteService(svc)) {
        fprintf(stderr, "DeleteService failed (%lu)\n", GetLastError());
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    /* Remove only the key we own.  Note: the service entry is removed
     * once the last handle to it closes. */
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, USBWARP_SERVICE_REG_KEY);

    printf("UsbWarpService uninstalled.\n");
    return 0;
}
