/*
 * hdv_defs.h — Hyper-V Device Virtualization (HDV) & Host Compute Service
 *              (HCS) API type definitions.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * These types are extracted from Microsoft headers and the validated ShmTest
 * proof-of-concept.  We load all functions at runtime from computecore.dll
 * and vmdevicehost.dll so the Service binary does not link against them.
 */

#ifndef USBWARP_HDV_DEFS_H
#define USBWARP_HDV_DEFS_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <combaseapi.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Opaque handle types
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef void *HCS_SYSTEM;
typedef void *HCS_OPERATION;
typedef void *HDV_HOST;
typedef void *HDV_DEVICE;

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Enumerations
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    HdvDeviceTypePCI = 1,
} HDV_DEVICE_TYPE;

typedef enum {
    HDV_PCI_BAR0 = 0, HDV_PCI_BAR1, HDV_PCI_BAR2,
    HDV_PCI_BAR3, HDV_PCI_BAR4, HDV_PCI_BAR5,
} HDV_PCI_BAR_SELECTOR;

#define HDV_PCI_BAR_COUNT  6

typedef enum {
    HdvPciDeviceInterfaceVersion1 = 1,
} HDV_PCI_INTERFACE_VERSION;

typedef enum {
    HdvMmioMappingFlagNone      = 0,
    HdvMmioMappingFlagWriteable = 1,
} HDV_MMIO_MAPPING_FLAGS;

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  PCI PnP identity structure
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    UINT16 VendorID;
    UINT16 DeviceID;
    UINT8  RevisionID;
    UINT8  ProgIf;
    UINT8  SubClass;
    UINT8  BaseClass;
    UINT16 SubVendorID;
    UINT16 SubSystemID;
} HDV_PCI_PNP_ID;

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  PCI device interface callback typedefs
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef HRESULT (CALLBACK *PFN_HDV_PCI_INITIALIZE)(void *ctx);
typedef void    (CALLBACK *PFN_HDV_PCI_TEARDOWN)(void *ctx);
typedef HRESULT (CALLBACK *PFN_HDV_PCI_SET_CONFIG)(void *ctx, UINT32 count,
                                                   const PCWSTR *values);
typedef HRESULT (CALLBACK *PFN_HDV_PCI_GET_DETAILS)(void *ctx,
                                                    HDV_PCI_PNP_ID *pnp,
                                                    UINT32 barCount,
                                                    UINT32 *bars);
typedef HRESULT (CALLBACK *PFN_HDV_PCI_START)(void *ctx);
typedef void    (CALLBACK *PFN_HDV_PCI_STOP)(void *ctx);
typedef HRESULT (CALLBACK *PFN_HDV_PCI_READ_CFG)(void *ctx, UINT32 offset,
                                                 UINT32 *value);
typedef HRESULT (CALLBACK *PFN_HDV_PCI_WRITE_CFG)(void *ctx, UINT32 offset,
                                                  UINT32 value);
typedef HRESULT (CALLBACK *PFN_HDV_PCI_READ_MMIO)(void *ctx,
                                                  HDV_PCI_BAR_SELECTOR bar,
                                                  UINT64 offset, UINT64 length,
                                                  BYTE *value);
typedef HRESULT (CALLBACK *PFN_HDV_PCI_WRITE_MMIO)(void *ctx,
                                                   HDV_PCI_BAR_SELECTOR bar,
                                                   UINT64 offset, UINT64 length,
                                                   const BYTE *value);

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  PCI device interface table
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    HDV_PCI_INTERFACE_VERSION Version;
    PFN_HDV_PCI_INITIALIZE    Initialize;
    PFN_HDV_PCI_TEARDOWN      Teardown;
    PFN_HDV_PCI_SET_CONFIG    SetConfiguration;
    PFN_HDV_PCI_GET_DETAILS   GetDetails;
    PFN_HDV_PCI_START         Start;
    PFN_HDV_PCI_STOP          Stop;
    PFN_HDV_PCI_READ_CFG      ReadConfigSpace;
    PFN_HDV_PCI_WRITE_CFG     WriteConfigSpace;
    PFN_HDV_PCI_READ_MMIO     ReadInterceptedMemory;
    PFN_HDV_PCI_WRITE_MMIO    WriteInterceptedMemory;
} HDV_PCI_DEVICE_INTERFACE;

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  HCS / HDV API function pointer typedefs
 * ═══════════════════════════════════════════════════════════════════════════ */

/* --- HCS (computecore.dll) ----------------------------------------------- */
typedef HRESULT (WINAPI *T_HcsOpenComputeSystem)(
    const WCHAR *id, DWORD requestedAccess, HCS_SYSTEM *system);

typedef HCS_OPERATION (WINAPI *T_HcsCreateOperation)(
    void *context, void *callback);

typedef HRESULT (WINAPI *T_HcsModifyComputeSystem)(
    HCS_SYSTEM system, HCS_OPERATION op, const WCHAR *config, HANDLE identity);

typedef HRESULT (WINAPI *T_HcsWaitForOperationResult)(
    HCS_OPERATION op, DWORD timeoutMs, PWSTR *resultDocument);

typedef void (WINAPI *T_HcsCloseOperation)(HCS_OPERATION op);
typedef void (WINAPI *T_HcsCloseComputeSystem)(HCS_SYSTEM system);

/* --- HDV (vmdevicehost.dll or computecore.dll) --------------------------- */
typedef HRESULT (WINAPI *T_HdvInitializeDeviceHost)(
    HCS_SYSTEM system, HDV_HOST *host);

typedef HRESULT (WINAPI *T_HdvTeardownDeviceHost)(HDV_HOST host);

typedef HRESULT (WINAPI *T_HdvCreateDeviceInstance)(
    HDV_HOST host, HDV_DEVICE_TYPE type,
    const GUID *classId, const GUID *instanceId,
    const void *iface, void *ctx, HDV_DEVICE *device);

typedef HRESULT (WINAPI *T_HdvCreateSectionBackedMmioRange)(
    HDV_DEVICE device, HDV_PCI_BAR_SELECTOR bar,
    UINT64 offsetInPages, UINT64 lengthInPages,
    HDV_MMIO_MAPPING_FLAGS flags,
    HANDLE sectionHandle, UINT64 sectionOffsetInPages);

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Global API function pointer table
 *
 *     Populated by HdvLoadApis().  All pointers NULL until loaded.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct usbwarp_hdv_api {
    /* HCS */
    T_HcsOpenComputeSystem              HcsOpen;
    T_HcsCreateOperation                HcsCreateOp;
    T_HcsModifyComputeSystem            HcsModify;
    T_HcsWaitForOperationResult         HcsWait;
    T_HcsCloseOperation                 HcsCloseOp;
    T_HcsCloseComputeSystem             HcsCloseSys;

    /* HDV */
    T_HdvInitializeDeviceHost           HdvInitHost;
    T_HdvTeardownDeviceHost             HdvTeardownHost;
    T_HdvCreateDeviceInstance           HdvCreateDevice;
    T_HdvCreateSectionBackedMmioRange   HdvCreateMmio;

    /* DLL handles (for FreeLibrary at shutdown) */
    HMODULE hComputeCore;
    HMODULE hVmDeviceHost;
};

/*
 * HdvLoadApis — Dynamically load HCS/HDV functions.
 *
 * Returns TRUE on success (all pointers populated), FALSE on failure
 * (the first missing export is printed to stderr).
 */
BOOL HdvLoadApis(struct usbwarp_hdv_api *api);

/*
 * HdvUnloadApis — Release DLL handles.
 */
void HdvUnloadApis(struct usbwarp_hdv_api *api);

#endif /* USBWARP_HDV_DEFS_H */
