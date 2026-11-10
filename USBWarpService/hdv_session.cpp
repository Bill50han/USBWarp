/*
 * hdv_session.cpp — HDV / HCS lifecycle for UsbWarp Service.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * FIX #4: HcsWaitForOperationResult — log resultDoc content.
 * FIX #5: Ignore writes to PCI read-only registers (VID/DID/Rev/Class).
 * FIX #6: Memory Space disable — documented behaviour (mapping persists).
 * FIX #9: Signal memSpaceEvent when MemSpace is enabled.
 */

#include "warp_service.h"
#include <stdio.h>
#include <string.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Dynamic API loading (unchanged)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LOAD_OR_FAIL(lib, name, type, field) do {         \
    api->field = (type)GetProcAddress(lib, name);         \
    if (!api->field) {                                    \
        LogError("GetProcAddress(%s) failed", name);      \
        return FALSE;                                     \
    }                                                     \
} while (0)

BOOL HdvLoadApis(struct usbwarp_hdv_api *api)
{
    memset(api, 0, sizeof(*api));
    api->hComputeCore  = LoadLibraryExW(L"computecore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    api->hVmDeviceHost = LoadLibraryExW(L"vmdevicehost.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!api->hComputeCore) {
        LogError("Failed to load computecore.dll (%lu)", GetLastError());
        return FALSE;
    }

    HMODULE cc = api->hComputeCore;
    LOAD_OR_FAIL(cc, "HcsOpenComputeSystem",       T_HcsOpenComputeSystem,       HcsOpen);
    LOAD_OR_FAIL(cc, "HcsEnumerateComputeSystems", T_HcsEnumerateComputeSystems, HcsEnumerate);
    LOAD_OR_FAIL(cc, "HcsCreateOperation",         T_HcsCreateOperation,         HcsCreateOp);
    LOAD_OR_FAIL(cc, "HcsModifyComputeSystem",     T_HcsModifyComputeSystem,     HcsModify);
    LOAD_OR_FAIL(cc, "HcsWaitForOperationResult",  T_HcsWaitForOperationResult,  HcsWait);
    LOAD_OR_FAIL(cc, "HcsCloseOperation",          T_HcsCloseOperation,          HcsCloseOp);
    LOAD_OR_FAIL(cc, "HcsCloseComputeSystem",      T_HcsCloseComputeSystem,      HcsCloseSys);

    HMODULE hdv = api->hVmDeviceHost ? api->hVmDeviceHost : cc;
    LOAD_OR_FAIL(hdv, "HdvInitializeDeviceHost",         T_HdvInitializeDeviceHost,         HdvInitHost);
    LOAD_OR_FAIL(hdv, "HdvTeardownDeviceHost",           T_HdvTeardownDeviceHost,           HdvTeardownHost);
    LOAD_OR_FAIL(hdv, "HdvCreateDeviceInstance",         T_HdvCreateDeviceInstance,         HdvCreateDevice);
    LOAD_OR_FAIL(hdv, "HdvCreateSectionBackedMmioRange", T_HdvCreateSectionBackedMmioRange, HdvCreateMmio);
    LOAD_OR_FAIL(hdv, "HdvDestroySectionBackedMmioRange", T_HdvDestroySectionBackedMmioRange, HdvDestroyMmio);
    api->HdvRegisterDoorbell = (T_HdvRegisterDoorbell)GetProcAddress(hdv,"HdvRegisterDoorbell");
    api->HdvUnregisterDoorbell = (T_HdvUnregisterDoorbell)GetProcAddress(hdv,"HdvUnregisterDoorbell");

    LogInfo("HCS/HDV APIs loaded (computecore=%p  vmdevicehost=%p)",
            (void *)cc, (void *)api->hVmDeviceHost);
    return TRUE;
}

#undef LOAD_OR_FAIL

void HdvUnloadApis(struct usbwarp_hdv_api *api)
{
    if (api->hVmDeviceHost) { FreeLibrary(api->hVmDeviceHost); }
    if (api->hComputeCore)  { FreeLibrary(api->hComputeCore);  }
    memset(api, 0, sizeof(*api));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  PCI config-space initialisation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void InitConfigSpace(struct warp_session *s)
{
    memset(s->pci_cfg, 0, sizeof(s->pci_cfg));
    s->bar0_mask    = ~(s->cfg.shm_size - 1u);
    s->bar0_probing = false;

    // A 64-bit BAR avoids the tiny low-MMIO aperture in WSL utility VMs.
    s->pci_cfg[0x10 / 4] = 0xc; // 64-bit, prefetchable coherent RAM
    s->pci_cfg[0x18 / 4] = s->doorbellEvent ? 0x4 : 0;
    s->pci_cfg[0x0C / 4] = 0x00000000;
    s->pci_cfg[0x34 / 4] = 0x00000000;
    s->pci_cfg[0x3C / 4] = 0x00000100;
    memset(s->msixTable, 0, sizeof(s->msixTable));
    s->msixTable[12] = 1;
    s->irqRevision = 0;
    s->msixPresent = (s->driverCaps & USBWARP_DRIVER_CAP_H2G_IRQ) != 0;
    if (s->msixPresent) {
        s->pci_cfg[0x04 / 4] = 0x00100000;
        s->pci_cfg[0x34 / 4] = 0x50;
        s->pci_cfg[0x50 / 4] = 0x00000011; // MSI-X, one vector
        s->pci_cfg[0x54 / 4] = 4;          // table: BAR4, offset 0
        s->pci_cfg[0x58 / 4] = 0x804;      // PBA: BAR4, offset 0x800
        s->pci_cfg[0x20 / 4] = 4;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  PCI device callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */

static HRESULT CALLBACK Cb_Initialize(void *ctx)
{
    (void)ctx;
    LogInfo("[HDV] Initialize");
    return S_OK;
}

static void CALLBACK Cb_Teardown(void *ctx)
{
    struct warp_session *s = (struct warp_session *)ctx;
    LogInfo("[HDV] Teardown");
    s->hdv_torn_down = true;
    if (s->memSpaceEvent) SetEvent(s->memSpaceEvent);
}

static HRESULT CALLBACK Cb_SetConfiguration(void *ctx, UINT32 count,
                                            const PCWSTR *values)
{
    (void)ctx; (void)values;
    LogInfo("[HDV] SetConfiguration (count=%u)", count);
    return S_OK;
}

static HRESULT CALLBACK Cb_GetDetails(void *ctx, HDV_PCI_PNP_ID *pnp,
                                      UINT32 barCount, UINT32 *bars)
{
    struct warp_session *s = (struct warp_session *)ctx;
    if (!bars || barCount == 0) return E_INVALIDARG;

    pnp->VendorID    = USBWARP_PCI_VENDOR_ID;
    pnp->DeviceID    = USBWARP_PCI_DEVICE_ID;
    pnp->RevisionID  = USBWARP_PCI_REVISION_ID;
    pnp->ProgIf      = USBWARP_PCI_PROG_IF;
    pnp->SubClass    = USBWARP_PCI_SUB_CLASS;
    pnp->BaseClass   = USBWARP_PCI_BASE_CLASS;
    pnp->SubVendorID = USBWARP_PCI_VENDOR_ID;
    pnp->SubSystemID = 0x0001;

    UINT32 n = (barCount < HDV_PCI_BAR_COUNT) ? barCount : HDV_PCI_BAR_COUNT;
    memset(bars, 0, sizeof(UINT32) * n);
    if (n < 2) return E_INVALIDARG;
    bars[0] = s->bar0_mask | 0xc;
    bars[1] = 0xffffffffu;
    if (s->doorbellEvent && n>=4) { bars[2]=0xfffff004u; bars[3]=0xffffffffu; }
    if (s->msixPresent && n >= 6) {
        bars[4] = 0xfffff004u; bars[5] = 0xffffffffu;
    }

    LogInfo("[HDV] GetDetails: VID=0x%04X DID=0x%04X BAR0=0x%08X (%u MB)",
            pnp->VendorID, pnp->DeviceID, bars[0],
            s->cfg.shm_size / (1024 * 1024));
    return S_OK;
}

static HRESULT CALLBACK Cb_Start(void *ctx)
{
    struct warp_session *s = (struct warp_session *)ctx;
    LogInfo("[HDV] Start — device is live in guest");
    s->hdv_started = true;
    return S_OK;
}

static void CALLBACK Cb_Stop(void *ctx)
{
    auto s = (warp_session *)ctx;
    s->hdv_started = false;
    if (s->memSpaceEvent) SetEvent(s->memSpaceEvent);
    LogInfo("[HDV] Stop");
}

/* ── Config-space read (unchanged) ─────────────────────────────────────── */

static HRESULT CALLBACK Cb_ReadConfigSpace(void *ctx, UINT32 offset,
                                           UINT32 *value)
{
    struct warp_session *s = (struct warp_session *)ctx;

    if (!value || (offset & 3)) return E_INVALIDARG;
    AcquireSRWLockShared(&s->pciLock);
    *value = offset < USBWARP_PCI_CFG_SIZE ? s->pci_cfg[offset / 4] : 0xffffffffu;
    ReleaseSRWLockShared(&s->pciLock);

    s->cfg_reads++;
    if (s->cfg.debug && (s->cfg_reads <= 60 || s->cfg_reads % 100 == 0))
        LogInfo("[HDV] CfgRd [0x%02X] → 0x%08X", offset, *value);

    return S_OK;
}

/* ── Config-space write ────────────────────────────────────────────────── */

/* FIX #5: PCI read-only registers — ignore writes silently. */
static bool IsPciReadOnly(UINT32 offset)
{
    switch (offset) {
    case 0x00:   /* Vendor ID / Device ID */
    case 0x08:   /* Revision ID / Class Code */
    case 0x2C:   /* Subsystem Vendor ID / Subsystem ID */
    case 0x34:
    case 0x54:
    case 0x58:
        return true;
    default:
        return false;
    }
}

static HRESULT CALLBACK Cb_WriteConfigSpace(void *ctx, UINT32 offset,
                                            UINT32 value)
{
    struct warp_session *s = (struct warp_session *)ctx;
    UINT32 old = 0;
    if (offset & 3) return E_INVALIDARG;
    struct Lock {
        SRWLOCK *lock;
        explicit Lock(SRWLOCK *p) : lock(p) { AcquireSRWLockExclusive(lock); }
        void unlock() { if (lock) { ReleaseSRWLockExclusive(lock); lock = nullptr; } }
        ~Lock() { unlock(); }
    } lock(&s->pciLock);

    if (offset < USBWARP_PCI_CFG_SIZE)
        old = s->pci_cfg[offset / 4];

    /* FIX #5: Ignore writes to read-only registers per PCI spec. */
    if (IsPciReadOnly(offset)) {
        if (s->cfg.debug)
            LogInfo("[HDV] CfgWr [0x%02X] ignored (read-only)", offset);
        return S_OK;
    }

    /* ── BAR0 (offset 0x10): handle probe and address assignment ───────── */
    if (offset == 0x18) {
        s->pci_cfg[0x18 / 4] = !s->doorbellEvent ? 0 :
            (value == 0xffffffffu ? 0xfffff004u : (value & 0xfffff000u) | 4);
        goto done;
    }
    if (offset == 0x50) {
        s->pci_cfg[offset / 4] = s->msixPresent ?
            0x11 | (value & 0xc0000000u) : 0;
        goto done;
    }
    if (offset == 0x20 && s->msixPresent) {
        s->pci_cfg[offset / 4] = (value & 0xfffff000u) | 4;
        goto done;
    }
    if (((offset==0x20 || offset==0x24) && !s->msixPresent) ||
        (offset==0x1c && !s->doorbellEvent)) {
        s->pci_cfg[offset/4]=0;
        goto done;
    }
    if (offset == 0x10) {
        if (value == 0xFFFFFFFF) {
            s->pci_cfg[0x10 / 4] = s->bar0_mask | 0xc;
            s->bar0_probing = true;
            LogInfo("[HDV] CfgWr [0x10] BAR0 probe → mask 0x%08X", s->bar0_mask);
        } else {
            s->pci_cfg[0x10 / 4] = (value & s->bar0_mask) | 0xc;
            s->bar0_probing = false;
            LogInfo("[HDV] CfgWr [0x10] BAR0 assigned → 0x%08X",
                    s->pci_cfg[0x10 / 4]);
        }
        goto done;
    }

    /* ── Command register (offset 0x04): detect Memory Space enable ──── */
    if (offset == 0x04) {
        /* Preserve Status bits (upper 16); only Command bits (lower 16)
         * are writable.  Merge: keep old status, accept new command. */
        s->pci_cfg[0x04 / 4] = (old & 0xFFFF0000u) | (value & 0x0000FFFFu);

        bool was_mem = (old   & 0x0002) != 0;
        bool now_mem = (value & 0x0002) != 0;

        if (now_mem && !was_mem) {
            LogInfo("[HDV] CfgWr [0x04] Memory Space ENABLED (Cmd=0x%04X)",
                    value & 0xFFFF);
            s->mem_space_on = true;

            /* FIX #9: Signal event so main loop wakes immediately. */
            if (s->memSpaceEvent)
                SetEvent(s->memSpaceEvent);
        } else if (!now_mem && was_mem) {
            /* FIX #6: Memory Space disabled.  We intentionally keep the
             * MMIO mapping alive.  The Section-backed range remains valid
             * in Hyper-V; when the Guest re-enables MemSpace, accesses
             * resume immediately without needing to re-map.  This matches
             * real PCI hardware behaviour where BAR mappings persist. */
            LogWarn("[HDV] CfgWr [0x04] Memory Space DISABLED "
                    "(MMIO mapping retained)");
            s->mem_space_on = false;
        } else {
            LogInfo("[HDV] CfgWr [0x04] Cmd=0x%04X (MemSpace %s)",
                    value & 0xFFFF, now_mem ? "on" : "off");
        }
        goto done;
    }

    /* ── All other writable registers ──────────────────────────────────── */
    if (offset < USBWARP_PCI_CFG_SIZE)
        s->pci_cfg[offset / 4] = value;

    s->cfg_writes++;
    if (s->cfg_writes <= 30 || s->cfg_writes % 100 == 0)
        LogInfo("[HDV] CfgWr [0x%02X] 0x%08X → 0x%08X", offset, old, value);

done:
    lock.unlock();
    if (offset == 0x50 && !DrvClientConfigureIrq(s)) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

/* ── MMIO callbacks (should never fire once section-backed range exists) ─ */

static bool ValidMmio(const warp_session *s, HDV_PCI_BAR_SELECTOR bar,
                      UINT64 offset, UINT64 length, const void *value)
{
    UINT64 size = bar == HDV_PCI_BAR0 ? s->shm_size :
        bar == HDV_PCI_BAR2 && s->doorbellEvent ? 4096 :
        bar == HDV_PCI_BAR4 && s->msixPresent ? 4096 : 0;
    return value && length && offset <= size && length <= size - offset;
}

static HRESULT CALLBACK Cb_ReadMmio(void *ctx, HDV_PCI_BAR_SELECTOR bar,
                                    UINT64 offset, UINT64 length, BYTE *value)
{
    auto s = (warp_session *)ctx;
    if (!ValidMmio(s, bar, offset, length, value)) return E_INVALIDARG;
    if (bar == HDV_PCI_BAR4 && s->msixPresent) {
        if (!value || offset > 4096 || length > 4096 - offset) return E_INVALIDARG;
        memset(value, 0, (size_t)length);
        AcquireSRWLockShared(&s->pciLock);
        for (UINT64 i = 0; i < length; ++i) {
            if (offset + i < 16) value[i] = s->msixTable[offset + i];
            else if (offset + i == 0x800 && s->shm_base)
                value[i] = (BYTE)(((volatile usbwarp_control_block *)s->shm_base)->irq_pending & 1);
        }
        ReleaseSRWLockShared(&s->pciLock);
        return S_OK;
    }
    LogWarn("[HDV] ReadMmio BAR%d off=0x%llX len=%llu — unexpected!",
            (int)bar, (unsigned long long)offset, (unsigned long long)length);
    memset(value, 0, (size_t)length);
    return S_OK;
}

static HRESULT CALLBACK Cb_WriteMmio(void *ctx, HDV_PCI_BAR_SELECTOR bar,
                                     UINT64 offset, UINT64 length,
                                     const BYTE *value)
{
    auto s=(warp_session *)ctx;
    if (!ValidMmio(s, bar, offset, length, value)) return E_INVALIDARG;
    if (bar == HDV_PCI_BAR4 && s->msixPresent) {
        if (!value || offset > 4096 || length > 4096 - offset) return E_INVALIDARG;
        AcquireSRWLockExclusive(&s->pciLock);
        for (UINT64 i = 0; i < length && offset + i < 16; ++i)
            s->msixTable[offset + i] = value[i];
        s->msixTable[12] &= 1;
        s->msixTable[13] = s->msixTable[14] = s->msixTable[15] = 0;
        ReleaseSRWLockExclusive(&s->pciLock);
        if (offset < 16 && !DrvClientConfigureIrq(s)) return HRESULT_FROM_WIN32(GetLastError());
        return S_OK;
    }
    if (bar==HDV_PCI_BAR2 && offset==0 && length==sizeof(UINT64) && s->doorbellEvent) {
        SetEvent(s->doorbellEvent); // startup/fallback; registered writes signal in the kernel
        return S_OK;
    }
    (void)value;
    LogWarn("[HDV] WriteMmio BAR%d off=0x%llX len=%llu — unexpected!",
            (int)bar, (unsigned long long)offset, (unsigned long long)length);
    return S_OK;
}

/* ── Callback table ────────────────────────────────────────────────────── */

static HDV_PCI_DEVICE_INTERFACE MakePciInterface(void)
{
    HDV_PCI_DEVICE_INTERFACE iface;
    memset(&iface, 0, sizeof(iface));
    iface.Version               = HdvPciDeviceInterfaceVersion1;
    iface.Initialize            = Cb_Initialize;
    iface.Teardown              = Cb_Teardown;
    iface.SetConfiguration      = Cb_SetConfiguration;
    iface.GetDetails            = Cb_GetDetails;
    iface.Start                 = Cb_Start;
    iface.Stop                  = Cb_Stop;
    iface.ReadConfigSpace       = Cb_ReadConfigSpace;
    iface.WriteConfigSpace      = Cb_WriteConfigSpace;
    iface.ReadInterceptedMemory = Cb_ReadMmio;
    iface.WriteInterceptedMemory= Cb_WriteMmio;
    return iface;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Hot-add JSON + GUID helper
 * ═══════════════════════════════════════════════════════════════════════════ */

static void GuidBare(const GUID *g, WCHAR *buf, int sz)
{
    swprintf_s(buf, sz,
        L"%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Public API: HdvSessionOpen / MapMmio / Close
 * ═══════════════════════════════════════════════════════════════════════════ */

namespace {
// The emulation class identifies UsbWarp. Instance IDs must be fresh because
// HCS FlexibleIov supports Add only: stopped PCI instances persist until VM exit.
const GUID EmulatorClass = {0x88421827,0x8545,0x4a35,{0xb8,0xa1,0x51,0xa9,0xd9,0x1d,0xeb,0x15}};

DWORD ErrorCode(HRESULT hr)
{
    return HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : (DWORD)hr;
}

HRESULT AddDevice(warp_session *s)
{
    WCHAR id[80], cls[80], json[1024];
    GuidBare(&s->instance_id, id, _countof(id));
    GuidBare(&s->class_id, cls, _countof(cls));
    swprintf_s(json, L"{\"ResourcePath\":\"VirtualMachine/Devices/FlexibleIov/%ls\","
            L"\"RequestType\":\"Add\",\"Settings\":{\"EmulatorId\":\"%ls\","
            L"\"HostingModel\":\"External\",\"Configuration\":[]}}", id, cls);
    HCS_OPERATION op = s->api.HcsCreateOp(nullptr, nullptr);
    if (!op) return E_OUTOFMEMORY;
    HRESULT hr = s->api.HcsModify(s->hcs_system, op, json, nullptr);
    if (SUCCEEDED(hr)) {
        // HcsCancelOperation is documented as E_NOTIMPL. Keep the operation and
        // callback resources alive until completion; SCM progress is reported
        // independently by the service thread during shutdown.
        for (;;) {
            PWSTR document = nullptr;
            hr = s->api.HcsWait(op, 100, &document);
            if (document) {
                if (hr != HCS_E_OPERATION_TIMEOUT && *document && s->cfg.debug)
                    LogInfo("[HCS Add] %ls", document);
                LocalFree(document);
            }
            if (hr != HCS_E_OPERATION_TIMEOUT) break;
            // Make BAR memory available during hot-add, before guest probe's
            // timeout, even if HCS hasn't returned the add result yet.
            if (!StopRequested(s) && s->mem_space_on && !s->mmio_mapped)
                HdvSessionMapMmio(s);
        }
    }
    s->api.HcsCloseOp(op);
    return hr;
}
}

BOOL HdvSessionOpen(warp_session *s)
{
    if (s->hdv_host || s->hcs_system || s->memSpaceEvent) {
        SetLastError(ERROR_INVALID_STATE); return FALSE;
    }
    s->hdv_started = false; s->hdv_torn_down = false;
    s->mem_space_on = false; s->mmio_mapped = false;
    s->cfg_reads = 0; s->cfg_writes = 0;
    s->class_id = EmulatorClass;
    InitConfigSpace(s);
    s->memSpaceEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s->memSpaceEvent) return FALSE;
    const char *operation = "HcsOpenComputeSystem";
    HRESULT hr = s->api.HcsOpen(s->openingVmId, GENERIC_ALL, &s->hcs_system);
    if (SUCCEEDED(hr) && StopRequested(s)) hr = HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
    if (SUCCEEDED(hr)) {
        operation = "CoCreateGuid(device instance)";
        hr = CoCreateGuid(&s->instance_id);
    }
    if (SUCCEEDED(hr)) {
        operation = "HdvInitializeDeviceHost";
        hr = s->api.HdvInitHost(s->hcs_system, &s->hdv_host);
    }
    if (SUCCEEDED(hr)) {
        static const HDV_PCI_DEVICE_INTERFACE iface = MakePciInterface();
        operation = "HdvCreateDeviceInstance";
        hr = s->api.HdvCreateDevice(s->hdv_host, HdvDeviceTypePCI,
            &s->class_id, &s->instance_id, &iface, s, &s->hdv_device);
    }
    if (SUCCEEDED(hr)) {
        s->stage.store(USBWARP_SESSION_CONNECTING);
        if (s->msixPresent) {
            NTSTATUS randomStatus = BCryptGenRandom(nullptr, (PUCHAR)&s->irqNonce,
                sizeof(s->irqNonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            if (randomStatus < 0 || !s->irqNonce || !DrvClientConfigureIrq(s)) {
                LogWarn("Native interrupt backend unavailable; using guest polling");
                // No guest can access config space before AddDevice. Preserve
                // the normal device even if optional backend allocation fails.
                AcquireSRWLockExclusive(&s->driverLock);
                s->driverCaps &= ~USBWARP_DRIVER_CAP_H2G_IRQ;
                ReleaseSRWLockExclusive(&s->driverLock);
                InitConfigSpace(s);
                if (s->shm_base)
                    ((volatile usbwarp_control_block *)s->shm_base)->flags &= ~USBWARP_FLAG_H2G_IRQ;
            }
        }
    }
    if (SUCCEEDED(hr)) {
        operation = "HcsModifyComputeSystem(Add)";
        hr = AddDevice(s);
        if (SUCCEEDED(hr)) s->hdv_added = true;
    }
    if (SUCCEEDED(hr) && (!s->hdv_started || StopRequested(s))) {
        operation = "HDV Start callback";
        hr = HRESULT_FROM_WIN32(StopRequested(s) ? ERROR_OPERATION_ABORTED : ERROR_DEVICE_NOT_CONNECTED);
    }
    if (FAILED(hr)) {
        DWORD error = ErrorCode(hr);
        // Once mapped, let SessionWorker drain the kernel/guest before unmap.
        if (!s->mmio_mapped) HdvSessionClose(s);
        if (!StopRequested(s)) LogWarn("%s failed for VM %ls: 0x%08lX", operation, s->openingVmId, (unsigned long)hr);
        SetLastError(error);
        return FALSE;
    }
    WCHAR instance[80]; GuidBare(&s->instance_id, instance, _countof(instance));
    LogInfo("HDV session attached to VM %ls (instance %ls)", s->openingVmId, instance);
    return TRUE;
}

BOOL HdvSessionMapMmio(warp_session *s)
{
    if (s->mmio_mapped) return TRUE;
    if (!s->mem_space_on || !s->section || !s->hdv_device) { SetLastError(ERROR_NOT_READY); return FALSE; }
    if (s->doorbellEvent && !s->doorbellRegistered) {
        HRESULT doorbell=s->api.HdvRegisterDoorbell(s->hdv_device,HDV_PCI_BAR2,0,1,USBWARP_HDV_DOORBELL_FLAGS,s->doorbellEvent);
        if (SUCCEEDED(doorbell)) {
            s->doorbellRegistered=true;
            LogInfo("HDV G2H doorbell registered (kernel event wakeup)");
        } else {
            // Initialization only: BAR0 is not exposed to the guest yet.
            auto cb=(usbwarp_control_block *)s->shm_base;
            cb->flags &= ~USBWARP_FLAG_G2H_DOORBELL;
            LogWarn("HDV doorbell unavailable: 0x%08lX",(unsigned long)doorbell);
        }
    }
    HRESULT hr = s->api.HdvCreateMmio(s->hdv_device, HDV_PCI_BAR0, 0,
        s->shm_size / USBWARP_PAGE_SIZE, HdvMmioMappingFlagWriteable, s->section, 0);
    if (FAILED(hr)) { SetLastError(ErrorCode(hr)); return FALSE; }
    s->mmio_mapped = true;
    // Kernel SETUP_SHM has already initialized the transport and owns its state.
    return TRUE;
}

BOOL HdvSessionClose(warp_session *s)
{
    if (s->doorbellRegistered && s->hdv_device) {
        HRESULT hr=s->api.HdvUnregisterDoorbell(s->hdv_device,HDV_PCI_BAR2,0,1,USBWARP_HDV_DOORBELL_FLAGS);
        if (FAILED(hr) && !s->hdv_torn_down) LogWarn("HDV doorbell unregister: 0x%08lX",(unsigned long)hr);
        s->doorbellRegistered=false;
    }
    // The kernel shutdown handshake has quiesced guest transfers. Release the
    // BAR backing explicitly; FlexibleIov has no supported hot-remove request.
    if (s->mmio_mapped && s->hdv_device) {
        HRESULT hr = s->api.HdvDestroyMmio(s->hdv_device, HDV_PCI_BAR0, 0);
        if (SUCCEEDED(hr)) s->mmio_mapped = false;
        else if (!s->hdv_torn_down) LogWarn("HDV MMIO unmap returned 0x%08lX", (unsigned long)hr);
    }
    if (s->hdv_host) {
        HRESULT hr = s->api.HdvTeardownHost(s->hdv_host);
        if (FAILED(hr)) { SetLastError(ErrorCode(hr)); return FALSE; }
        s->hdv_host = nullptr; s->hdv_device = nullptr;
    }
    s->hdv_added = false;
    if (s->hcs_system) { s->api.HcsCloseSys(s->hcs_system); s->hcs_system = nullptr; }
    if (s->memSpaceEvent) { CloseHandle(s->memSpaceEvent); s->memSpaceEvent = nullptr; }
    s->hdv_started = false; s->mem_space_on = false; s->mmio_mapped = false;
    return TRUE;
}
