/*
 * usb_enum.c — Host USB device enumeration via SetupDi API.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * FIX #2/#3: Use real DEVPKEY_Device_ContainerId for device identification.
 * FIX #6:    Filter composite device children — only show physical devices.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "usbwarp_cli.h"

#include <setupapi.h>
#include <initguid.h>   /* must precede devpkey.h to define DEVPKEY GUIDs */
#include <devpkey.h>
#include <cfgmgr32.h>

#pragma comment(lib, "setupapi.lib")

static const GUID GUID_DEVINTERFACE_USB_DEVICE_LOCAL =
    { 0xA5DCBF10L, 0x6530, 0x11D2,
      { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  VID/PID parser
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ParseVidPid(const char *hwId, uint16_t *vid, uint16_t *pid)
{
    const char *p;
    *vid = 0;
    *pid = 0;
    p = strstr(hwId, "VID_");
    if (p) *vid = (uint16_t)strtoul(p + 4, NULL, 16);
    p = strstr(hwId, "PID_");
    if (p) *pid = (uint16_t)strtoul(p + 4, NULL, 16);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Bus-ID from USB port topology
 *
 *   Try to extract a "hub-port" style bus-id from the instance path.
 *   USB instance paths often look like:
 *     USB\VID_046D&PID_C534\5&2f5e3a07&0&3
 *   The last segment "3" is the port number.  We combine with the
 *   parent hub to create a "X-Y" style identifier.
 *
 *   Fallback: hash of instance path for uniqueness.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void MakeBusId(const char *instanceId, char *busId, int bufLen)
{
    /* Try to extract port number from instance path.
     * Format: USB\VID_xxxx&PID_xxxx\<serial_or_location>
     * The last part after the last '\' often contains port info. */
    const char *lastSeg = strrchr(instanceId, '\\');
    if (lastSeg) {
        lastSeg++;
        /* If it contains '&', extract the port part after last '&'. */
        const char *port = strrchr(lastSeg, '&');
        if (port && port[1] >= '0' && port[1] <= '9') {
            snprintf(busId, bufLen, "1-%s", port + 1);
            return;
        }
    }

    /* Fallback: 8-char hash of instance ID. */
    unsigned int hash = 0;
    const char *p;
    for (p = instanceId; *p; p++)
        hash = hash * 31 + (unsigned char)*p;
    snprintf(busId, bufLen, "%08x", hash);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Check if device is a USB composite child (#6)
 *
 *   Composite USB devices expose multiple interfaces.  SetupDi enumerates
 *   both the physical device AND each interface.  We want to show only
 *   the physical device (the one whose service is "usbccgp" or whose
 *   instance path starts with "USB\" rather than "USB\COMPOSITE\").
 *
 *   Strategy: check if the instance path contains "MI_" (multi-interface
 *   suffix), which indicates a composite child interface.
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool IsCompositeChild(const char *instanceId)
{
    /* Composite children have paths like:
     * USB\VID_xxxx&PID_xxxx&MI_00\...  (MI_ = interface index) */
    if (strstr(instanceId, "&MI_") != NULL)
        return true;
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Get real container ID (fix #3)
 *
 *   DEVPKEY_Device_ContainerId returns a GUID that identifies the
 *   physical device.  All interfaces of a composite device share the
 *   same container ID.  This is the correct identifier for bind/unbind.
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool GetContainerId(HDEVINFO devInfo, PSP_DEVINFO_DATA devData,
                           GUID *out)
{
    DEVPROPTYPE propType;
    GUID        containerId;
    DWORD       propSize = 0;

    if (SetupDiGetDevicePropertyW(
            devInfo, devData,
            &DEVPKEY_Device_ContainerId,
            &propType,
            (PBYTE)&containerId, sizeof(containerId),
            &propSize, 0)) {
        if (propType == DEVPROP_TYPE_GUID && propSize == sizeof(GUID)) {
            *out = containerId;
            return true;
        }
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Enumerate USB devices
 * ═══════════════════════════════════════════════════════════════════════════ */

int UsbEnumerate(USB_DEVICE_LIST *list)
{
    HDEVINFO        devInfo;
    SP_DEVINFO_DATA devData;
    DWORD           idx;
    DWORD           propType;
    char            propBuf[512];

    list->count = 0;

    devInfo = SetupDiGetClassDevsA(
        &GUID_DEVINTERFACE_USB_DEVICE_LOCAL,
        NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (devInfo == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Error: SetupDiGetClassDevs failed (%lu).\n",
                GetLastError());
        return -1;
    }

    for (idx = 0; list->count < MAX_USB_DEVICES; idx++) {
        USB_HOST_DEVICE *dev = &list->devices[list->count];
        memset(dev, 0, sizeof(*dev));

        devData.cbSize = sizeof(devData);
        if (!SetupDiEnumDeviceInfo(devInfo, idx, &devData))
            break;

        /* Get instance ID. */
        if (!SetupDiGetDeviceInstanceIdA(devInfo, &devData,
                dev->instanceId, sizeof(dev->instanceId), NULL))
            continue;

        /* Get device interface path (\\?\USB#...#{GUID}) for CreateFile.
         * This is needed by the driver's WdfIoTargetOpen. */
        {
            SP_DEVICE_INTERFACE_DATA ifData;
            ifData.cbSize = sizeof(ifData);

            if (SetupDiEnumDeviceInterfaces(devInfo, &devData,
                    &GUID_DEVINTERFACE_USB_DEVICE_LOCAL, 0, &ifData)) {
                /* Get required buffer size. */
                DWORD detailSize = 0;
                SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData,
                    NULL, 0, &detailSize, NULL);

                if (detailSize > 0 && detailSize < 2048) {
                    PSP_DEVICE_INTERFACE_DETAIL_DATA_A pDetail =
                        (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)
                        HeapAlloc(GetProcessHeap(), 0, detailSize);
                    if (pDetail) {
                        pDetail->cbSize =
                            sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
                        if (SetupDiGetDeviceInterfaceDetailA(
                                devInfo, &ifData, pDetail,
                                detailSize, NULL, NULL)) {
                            strncpy(dev->devicePath, pDetail->DevicePath,
                                    sizeof(dev->devicePath) - 1);
                        }
                        HeapFree(GetProcessHeap(), 0, pDetail);
                    }
                }
            }
        }

        /* Skip root hubs. */
        if (strstr(dev->instanceId, "ROOT_HUB") != NULL)
            continue;

        /* FIX #6: Skip composite children (MI_ interfaces). */
        if (IsCompositeChild(dev->instanceId)) {
            dev->isCompositeChild = true;
            continue;
        }

        /* Get hardware ID for VID/PID. */
        if (SetupDiGetDeviceRegistryPropertyA(devInfo, &devData,
                SPDRP_HARDWAREID, &propType,
                (PBYTE)propBuf, sizeof(propBuf), NULL))
            ParseVidPid(propBuf, &dev->vendorId, &dev->productId);

        /* Skip entries without VID/PID. */
        if (dev->vendorId == 0 && dev->productId == 0)
            continue;

        /* Get friendly name / description. */
        if (!SetupDiGetDeviceRegistryPropertyA(devInfo, &devData,
                SPDRP_FRIENDLYNAME, &propType,
                (PBYTE)dev->description, sizeof(dev->description), NULL)) {
            if (!SetupDiGetDeviceRegistryPropertyA(devInfo, &devData,
                    SPDRP_DEVICEDESC, &propType,
                    (PBYTE)dev->description, sizeof(dev->description), NULL)) {
                snprintf(dev->description, sizeof(dev->description),
                         "USB Device %04x:%04x",
                         dev->vendorId, dev->productId);
            }
        }

        /* Get device class. */
        {
            char classBuf[64] = {0};
            if (SetupDiGetDeviceRegistryPropertyA(devInfo, &devData,
                    SPDRP_CLASS, &propType,
                    (PBYTE)classBuf, sizeof(classBuf), NULL)) {
                if (_stricmp(classBuf, "HIDClass") == 0)
                    dev->deviceClass = 0x03;
                else if (_stricmp(classBuf, "DiskDrive") == 0)
                    dev->deviceClass = 0x08;
                else if (_stricmp(classBuf, "Printer") == 0)
                    dev->deviceClass = 0x07;
                else if (_stricmp(classBuf, "Image") == 0)
                    dev->deviceClass = 0x06;
            }
        }

        /* FIX #3: Get real container ID (DEVPKEY_Device_ContainerId).
         * Falls back to hash-based GUID if the property is unavailable. */
        if (!GetContainerId(devInfo, &devData, &dev->containerId)) {
            /* Fallback: generate GUID from instance path hash. */
            unsigned int hash = 0;
            const char *p;
            for (p = dev->instanceId; *p; p++)
                hash = hash * 31 + (unsigned char)*p;
            dev->containerId.Data1 = hash;
            dev->containerId.Data2 = (unsigned short)(hash >> 16);
            dev->containerId.Data3 = (unsigned short)(hash & 0xFFFF);
            memset(dev->containerId.Data4, 0, 8);
        }

        MakeBusId(dev->instanceId, dev->busId, sizeof(dev->busId));

        list->count++;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Utility functions
 * ═══════════════════════════════════════════════════════════════════════════ */

bool ParseGuid(const char *str, GUID *out)
{
    unsigned long d1;
    unsigned int d2, d3, d4[8];
    int n = sscanf(str,
        "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        &d1, &d2, &d3,
        &d4[0], &d4[1], &d4[2], &d4[3],
        &d4[4], &d4[5], &d4[6], &d4[7]);
    if (n != 11) return false;
    out->Data1 = d1;
    out->Data2 = (unsigned short)d2;
    out->Data3 = (unsigned short)d3;
    for (int i = 0; i < 8; i++)
        out->Data4[i] = (unsigned char)d4[i];
    return true;
}

void FormatGuid(const GUID *g, char *buf, int bufLen)
{
    snprintf(buf, bufLen,
        "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

void FormatBytes(uint64_t bytes, char *buf, int bufLen)
{
    if (bytes >= 1073741824ULL)
        snprintf(buf, bufLen, "%.2f GB", (double)bytes / 1073741824.0);
    else if (bytes >= 1048576ULL)
        snprintf(buf, bufLen, "%.2f MB", (double)bytes / 1048576.0);
    else if (bytes >= 1024ULL)
        snprintf(buf, bufLen, "%.2f KB", (double)bytes / 1024.0);
    else
        snprintf(buf, bufLen, "%llu B", (unsigned long long)bytes);
}

void FormatDuration(uint64_t seconds, char *buf, int bufLen)
{
    uint64_t h = seconds / 3600;
    uint64_t m = (seconds % 3600) / 60;
    uint64_t s = seconds % 60;
    if (h > 0)
        snprintf(buf, bufLen, "%lluh %llum %llus",
                 (unsigned long long)h, (unsigned long long)m,
                 (unsigned long long)s);
    else if (m > 0)
        snprintf(buf, bufLen, "%llum %llus",
                 (unsigned long long)m, (unsigned long long)s);
    else
        snprintf(buf, bufLen, "%llus", (unsigned long long)s);
}
