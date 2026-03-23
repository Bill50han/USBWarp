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
    api->hComputeCore  = LoadLibraryA("computecore.dll");
    api->hVmDeviceHost = LoadLibraryA("vmdevicehost.dll");

    if (!api->hComputeCore) {
        LogError("Failed to load computecore.dll (%lu)", GetLastError());
        return FALSE;
    }

    HMODULE cc = api->hComputeCore;
    LOAD_OR_FAIL(cc, "HcsOpenComputeSystem",       T_HcsOpenComputeSystem,       HcsOpen);
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

    /* 0x04: Status bit 4 (Capabilities List) = 1 */
    s->pci_cfg[0x04 / 4] = 0x00100000;
    s->pci_cfg[0x0C / 4] = 0x00000000;
    s->pci_cfg[0x34 / 4] = 0x00000000;
    s->pci_cfg[0x3C / 4] = 0x00000100;
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
    bars[0] = s->bar0_mask;

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
    (void)ctx;
    LogInfo("[HDV] Stop");
}

/* ── Config-space read (unchanged) ─────────────────────────────────────── */

static HRESULT CALLBACK Cb_ReadConfigSpace(void *ctx, UINT32 offset,
                                           UINT32 *value)
{
    struct warp_session *s = (struct warp_session *)ctx;

    if (offset < USBWARP_PCI_CFG_SIZE)
        *value = s->pci_cfg[offset / 4];
    else
        *value = 0;

    s->cfg_reads++;
    if (s->cfg_reads <= 60 || s->cfg_reads % 100 == 0)
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

    if (offset < USBWARP_PCI_CFG_SIZE)
        old = s->pci_cfg[offset / 4];

    /* FIX #5: Ignore writes to read-only registers per PCI spec. */
    if (IsPciReadOnly(offset)) {
        if (s->cfg.debug)
            LogInfo("[HDV] CfgWr [0x%02X] ignored (read-only)", offset);
        return S_OK;
    }

    /* ── BAR0 (offset 0x10): handle probe and address assignment ───────── */
    if (offset == 0x10) {
        if (value == 0xFFFFFFFF) {
            s->pci_cfg[0x10 / 4] = s->bar0_mask;
            s->bar0_probing = true;
            LogInfo("[HDV] CfgWr [0x10] BAR0 probe → mask 0x%08X", s->bar0_mask);
        } else {
            s->pci_cfg[0x10 / 4] = value & s->bar0_mask;
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
    return S_OK;
}

/* ── MMIO callbacks (should never fire once section-backed range exists) ─ */

static HRESULT CALLBACK Cb_ReadMmio(void *ctx, HDV_PCI_BAR_SELECTOR bar,
                                    UINT64 offset, UINT64 length, BYTE *value)
{
    (void)ctx;
    LogWarn("[HDV] ReadMmio BAR%d off=0x%llX len=%llu — unexpected!",
            (int)bar, (unsigned long long)offset, (unsigned long long)length);
    memset(value, 0, (size_t)length);
    return S_OK;
}

static HRESULT CALLBACK Cb_WriteMmio(void *ctx, HDV_PCI_BAR_SELECTOR bar,
                                     UINT64 offset, UINT64 length,
                                     const BYTE *value)
{
    (void)ctx; (void)value;
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

BOOL HdvSessionOpen(struct warp_session *s)
{
    HRESULT hr;
    const struct usbwarp_hdv_api *a = &s->api;

    s->hdv_started   = false;
    s->hdv_torn_down = false;
    s->mem_space_on  = false;
    s->mmio_mapped   = false;
    s->cfg_reads     = 0;
    s->cfg_writes    = 0;

    InitConfigSpace(s);

    /* Create memSpaceEvent (#9). */
    s->memSpaceEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    /* Step 1: open VM. */
    hr = a->HcsOpen(s->cfg.vm_id, GENERIC_ALL, &s->hcs_system);
    if (FAILED(hr)) {
        LogError("HcsOpenComputeSystem failed: 0x%08lX", (unsigned long)hr);
        return FALSE;
    }
    LogInfo("VM opened");

    /* Step 2: init HDV device host. */
    hr = a->HdvInitHost(s->hcs_system, &s->hdv_host);
    if (FAILED(hr)) {
        LogError("HdvInitializeDeviceHost failed: 0x%08lX", (unsigned long)hr);
        a->HcsCloseSys(s->hcs_system);
        s->hcs_system = NULL;
        return FALSE;
    }
    LogInfo("HDV device host initialised");

    /* Step 3: create PCI device instance. */
    CoCreateGuid(&s->class_id);
    CoCreateGuid(&s->instance_id);
    HDV_PCI_DEVICE_INTERFACE iface = MakePciInterface();

    hr = a->HdvCreateDevice(s->hdv_host, HdvDeviceTypePCI,
                            &s->class_id, &s->instance_id,
                            &iface, s, &s->hdv_device);
    if (FAILED(hr)) {
        LogError("HdvCreateDeviceInstance failed: 0x%08lX", (unsigned long)hr);
        a->HdvTeardownHost(s->hdv_host);
        a->HcsCloseSys(s->hcs_system);
        s->hdv_host = NULL;
        s->hcs_system = NULL;
        return FALSE;
    }
    LogInfo("PCI device instance created");

    /* Step 4: hot-add to VM. */
    WCHAR cs[80], is[80];
    GuidBare(&s->class_id,    cs, 80);
    GuidBare(&s->instance_id, is, 80);

    WCHAR json[1024];
    swprintf_s(json, 1024,
        L"{\"ResourcePath\":\"VirtualMachine/Devices/FlexibleIov/%s\","
        L"\"RequestType\":\"Add\","
        L"\"Settings\":{"
            L"\"EmulatorId\":\"%s\","
            L"\"HostingModel\":\"External\","
            L"\"Configuration\":[]"
        L"}}",
        is, cs);

    HCS_OPERATION op = a->HcsCreateOp(NULL, NULL);
    hr = a->HcsModify(s->hcs_system, op, json, NULL);

    PWSTR resultDoc = NULL;
    hr = a->HcsWait(op, 15000, &resultDoc);
    a->HcsCloseOp(op);

    /* FIX #4: Always log resultDoc content for diagnostics. */
    if (resultDoc) {
        if (resultDoc[0] != L'\0')
            LogInfo("[HCS] OperationResult: %ls", resultDoc);
        LocalFree(resultDoc);
    }

    if (FAILED(hr)) {
        LogError("Hot-add failed: 0x%08lX", (unsigned long)hr);
        a->HdvTeardownHost(s->hdv_host);
        a->HcsCloseSys(s->hcs_system);
        s->hdv_host = NULL;
        s->hcs_system = NULL;
        return FALSE;
    }

    if (!s->hdv_started) {
        LogError("Hot-add returned success but Start callback did not fire");
        a->HdvTeardownHost(s->hdv_host);
        a->HcsCloseSys(s->hcs_system);
        s->hdv_host = NULL;
        s->hcs_system = NULL;
        return FALSE;
    }

    LogInfo("Hot-add complete — PCI device live in guest");
    LogInfo("Waiting for guest to enable Memory Space ...");
    return TRUE;
}

BOOL HdvSessionMapMmio(struct warp_session *s)
{
    if (s->mmio_mapped) return TRUE;
    if (!s->mem_space_on) return FALSE;
    if (!s->section) {
        LogError("Cannot map MMIO: Section not created");
        return FALSE;
    }

    UINT64 page_count = s->shm_size / USBWARP_PAGE_SIZE;

    LogInfo("Mapping Section to BAR0: %llu pages (%u MB), R/W",
            (unsigned long long)page_count,
            s->shm_size / (1024 * 1024));

    HRESULT hr = s->api.HdvCreateMmio(
        s->hdv_device, HDV_PCI_BAR0,
        0, page_count,
        (HDV_MMIO_MAPPING_FLAGS)(HdvMmioMappingFlagWriteable),
        s->section, 0);

    if (FAILED(hr)) {
        LogError("HdvCreateSectionBackedMmioRange failed: 0x%08lX",
                 (unsigned long)hr);
        return FALSE;
    }

    s->mmio_mapped = true;
    s->cb->host_state = USBWARP_STATE_READY;
    LogInfo("Section-backed MMIO established — zero-copy SHM active");
    return TRUE;
}

void HdvSessionClose(struct warp_session *s)
{
    const struct usbwarp_hdv_api *a = &s->api;
    LogInfo("Closing HDV session");

    if (s->hdv_host) {
        a->HdvTeardownHost(s->hdv_host);
        s->hdv_host   = NULL;
        s->hdv_device = NULL;
    }
    if (s->hcs_system) {
        a->HcsCloseSys(s->hcs_system);
        s->hcs_system = NULL;
    }

    if (s->memSpaceEvent) {
        CloseHandle(s->memSpaceEvent);
        s->memSpaceEvent = NULL;
    }

    s->hdv_started  = false;
    s->mem_space_on = false;
    s->mmio_mapped  = false;
}
