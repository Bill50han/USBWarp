/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USBWARP_SERVICE_H
#define USBWARP_SERVICE_H
#include "hdv_defs.h"
#include "../include/usbwarp_shared.h"
#include "../include/usbwarp_pipe.h"
#include "../include/usbwarp_ioctl.h"
#include <atomic>
#include "vm_target.h"

#ifndef USBWARP_SERVICE_NAME
#define USBWARP_SERVICE_NAME L"UsbWarpService"
#endif
#ifndef USBWARP_SERVICE_REG_KEY
#define USBWARP_SERVICE_REG_KEY L"SOFTWARE\\UsbWarp\\Service"
#endif
#ifndef USBWARP_SERVICE_DEPENDENCIES
#define USBWARP_SERVICE_DEPENDENCIES L"UsbWarp\0"
#endif
#define USBWARP_SERVICE_DESCR L"UsbWarp - HDV USB passthrough for WSL2"
#define USBWARP_PCI_CFG_SIZE 256
#define USBWARP_PCI_CFG_DWORDS (USBWARP_PCI_CFG_SIZE / 4)
#define WARP_PIPE_CLIENTS 8

struct warp_config {
    warp_target target;
    uint32_t shm_size, ring_data_size, buffer_size, max_devices;
    bool enable_iso, debug;
};
static inline void warp_config_defaults(warp_config *c)
{
    memset(c, 0, sizeof(*c));
    c->shm_size = USBWARP_SHM_SIZE_DEFAULT;
    c->ring_data_size = 256u * 1024;
    c->buffer_size = USBWARP_BUFFER_SIZE_DEFAULT;
    c->max_devices = USBWARP_MAX_DEVICES_DEFAULT;
}

struct warp_session;
struct warp_pipe_client {
    warp_session *session = nullptr;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE thread = nullptr;
};

/* Session resources have one owner: SessionWorker. The service/IPC lifetime
 * spans multiple sessions. There is no service-side USB device table or ring
 * consumer; the kernel owns device state, negotiation and both data rings. */
struct warp_session {
    warp_config cfg{};
    usbwarp_hdv_api api{};
    HCS_SYSTEM hcs_system = nullptr;
    HDV_HOST hdv_host = nullptr;
    HDV_DEVICE hdv_device = nullptr;
    GUID class_id{}, instance_id{};
    bool hdv_added = false;
    UINT32 pci_cfg[USBWARP_PCI_CFG_DWORDS]{};
    UINT32 bar0_mask = 0;
    bool bar0_probing = false;
    SRWLOCK pciLock = SRWLOCK_INIT;

    HANDLE section = nullptr;
    HANDLE doorbellEvent = nullptr;
    bool doorbellRegistered = false;
    void *shm_base = nullptr;
    uint32_t shm_size = 0;
    std::atomic<bool> hdv_started{false}, hdv_torn_down{false};
    std::atomic<bool> mem_space_on{false}, mmio_mapped{false};
    HANDLE memSpaceEvent = nullptr;

    SRWLOCK lifecycleLock = SRWLOCK_INIT;
    std::atomic<bool> shutdown_requested{false};
    std::atomic<DWORD> fatalError{ERROR_SUCCESS}, lastSessionError{ERROR_SUCCESS};
    std::atomic<uint32_t> stage{USBWARP_SESSION_INITIALIZING};
    HANDLE stopEvent = nullptr, sessionStopEvent = nullptr, driverFaultEvent = nullptr;
    HANDLE sessionThread = nullptr, heartbeatThread = nullptr;
    HANDLE targetChangedEvent = nullptr;
    bool serviceMode = false;

    // Selection/cache are separate from immutable transport configuration.
    SRWLOCK targetLock = SRWLOCK_INIT;
    warp_target selectedTarget{};
    uint64_t targetGeneration = 1, attachedGeneration = 0, vmListGeneration = 0;
    std::vector<warp_vm> vms;
    DWORD vmDiscoveryError = ERROR_NOT_READY, targetError = ERROR_NOT_READY;
    uint32_t targetMatches = 0;
    WCHAR resolvedVmId[256]{}, attachedVmId[256]{};
    WCHAR openingVmId[256]{}; // SessionWorker exclusively owns this value

    SRWLOCK driverLock = SRWLOCK_INIT;
    uint8_t msixTable[16]{}; // protected by pciLock; initially masked
    bool msixPresent = false; // immutable for an HDV instance, including teardown
    uint64_t irqNonce = 0, irqRevision = 0;
    SRWLOCK captureLock = SRWLOCK_INIT;
    HANDLE hDriver = nullptr; // opened OVERLAPPED; all access under driverLock
    bool driverRegistered = false, driverShmSetup = false;
    uint32_t driverCaps = 0;
    std::atomic<uint32_t> driverState{USBWARP_STATE_INIT};

    HANDLE pipeThread = nullptr, pipeListener = INVALID_HANDLE_VALUE;
    WCHAR pipeName[256]{}; // default protocol name; tests may supply a private name
    warp_pipe_client clients[WARP_PIPE_CLIENTS];
    UINT64 start_time_ns = 0;
    std::atomic<int> cfg_reads{0}, cfg_writes{0};
};
extern warp_session g_sess;

bool ParseArgOverrides(int argc, WCHAR *argv[], warp_config *cfg, bool install = false);
DWORD ValidateConfig(warp_config *cfg);
DWORD LoadConfig(int argc, WCHAR *argv[], warp_config *cfg, bool install = false);
DWORD SaveRegistryConfig(const warp_config *cfg);
DWORD DecodeRegistryConfig(const BYTE *data, DWORD bytes, warp_config *cfg);
DWORD RemoveRegistryConfig();
void PrintUsage();

BOOL ShmCreate(warp_session *s);
void ShmDestroy(warp_session *s);
BOOL HdvSessionOpen(warp_session *s);
BOOL HdvSessionMapMmio(warp_session *s);
BOOL HdvSessionClose(warp_session *s);

BOOL DrvClientOpen(warp_session *s);
DWORD DrvClientClose(warp_session *s); // teardown even a registered, unmapped session
BOOL DrvClientRegister(warp_session *s);
BOOL DrvClientSetupShm(warp_session *s);
BOOL DrvClientHeartbeat(warp_session *s);
BOOL DrvClientActive(warp_session *s);
BOOL DrvClientConfigureIrq(warp_session *s);
BOOL DrvClientQueryStatus(warp_session *s, USBWARP_QUERY_STATUS_OUT *out);
BOOL DrvClientEnumDevices(warp_session *s, USBWARP_ENUM_DEVICES_OUT *out);
BOOL DrvClientBindGuid(warp_session *s, const uint8_t guid[16], const WCHAR *path,
                       uint8_t speed, uint32_t *index, uint16_t *vid, uint16_t *pid);
BOOL DrvClientUnbindGuid(warp_session *s, const uint8_t guid[16]);
BOOL DeviceBind(warp_session *s, const uint8_t guid[16], const WCHAR *path, uint8_t speed,
                uint32_t *index, uint16_t *vid, uint16_t *pid);
BOOL DeviceUnbind(warp_session *s, const uint8_t guid[16]);
BOOL DeviceRecover(warp_session *s);
DWORD DeviceCloseSession(warp_session *s);
void DeviceReconcile(warp_session *s);
DWORD DeviceCapturePending(bool *pending);

BOOL PipeServerStart(warp_session *s);
void PipeServerStop(warp_session *s); // joins every thread before returning
DWORD WINAPI SessionWorker(void *context);
void RequestStop(warp_session *s, DWORD error = ERROR_SUCCESS);
bool StopRequested(const warp_session *s);
void JoinServiceThread(warp_session *s, HANDLE thread);
DWORD ServiceRun(warp_session *s, bool serviceMode, int argc, WCHAR *argv[]);

int SvcInstall(int argc, WCHAR *argv[]);
int SvcUninstall();
int RunServiceDispatcher();
void SvcCheckpoint();
void SvcRunning();
void SvcStopping();

BOOL LogInit(bool serviceMode);
void LogCleanup();
void LogInfo(const char *fmt, ...);
void LogWarn(const char *fmt, ...);
void LogError(const char *fmt, ...);

static inline UINT64 NowNs()
{
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    UINT64 q = (UINT64)ctr.QuadPart, f = (UINT64)freq.QuadPart;
    return (q / f) * 1000000000ULL + ((q % f) * 1000000000ULL) / f;
}
#endif
