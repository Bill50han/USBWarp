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
#include "../include/usbwarp_usb3.h"

#include <setupapi.h>
#include <initguid.h>   /* must precede devpkey.h to define DEVPKEY GUIDs */
#include <devpkey.h>
#include <cfgmgr32.h>
#include <winioctl.h>   /* FILE_ANY_ACCESS, used by usbioctl.h CTL_CODEs */
#include <usbioctl.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

static const GUID GUID_DEVINTERFACE_USB_HUB_LOCAL =
    { 0xF18A0E88L, 0xC30C, 0x11D0,
      { 0x88, 0x15, 0x00, 0xA0, 0xC9, 0x06, 0xBE, 0xD8 } };

/* USB_DEVICE_SPEED values from the hub ioctl reply.  Defined locally —
 * UsbSuperSpeedPlus is missing from older SDK headers. */
#define USB_HUB_SPEED_LOW       0
#define USB_HUB_SPEED_FULL      1
#define USB_HUB_SPEED_HIGH      2
#define USB_HUB_SPEED_SUPER     3
#define USB_HUB_SPEED_SUPERPLUS 4

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
 * §4b  Query connection speed from the parent hub
 *
 *   The kernel-side fallback infers speed from bulk-EP maxpacket, which
 *   misreports devices without bulk endpoints (UVC/UAC) as full-speed and
 *   never reports low-speed.  The hub driver knows the truth — ask it via
 *   IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX.  Returns 0 (unknown) on
 *   any failure; the kernel then uses its heuristic.
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t GetDeviceSpeed(HDEVINFO devInfo, PSP_DEVINFO_DATA devData)
{
    DEVINST      parent;
    WCHAR        location[64];
    DEVPROPTYPE  propType;
    DWORD        propSize = 0;
    ULONG        port = 0;
    WCHAR        hubId[MAX_DEVICE_ID_LEN];
    WCHAR        hubIfaces[2048];
    HANDLE       hHub;
    USB_NODE_CONNECTION_INFORMATION_EX ci;
    DWORD        bytes = 0;
    uint8_t      speed = 0;

    if (CM_Get_Parent(&parent, devData->DevInst, 0) != CR_SUCCESS)
        return 0;

    /* Port number from LocationInfo, e.g. "Port_#0003.Hub_#0002". */
    if (!SetupDiGetDevicePropertyW(devInfo, devData,
            &DEVPKEY_Device_LocationInfo,
            &propType, (PBYTE)location, sizeof(location),
            &propSize, 0) ||
        propType != DEVPROP_TYPE_STRING)
        return 0;
    if (swscanf(location, L"Port_#%lu", &port) != 1 || port == 0 ||
        port > 255)
        return 0;

    if (CM_Get_Device_IDW(parent, hubId, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
        return 0;

    if (CM_Get_Device_Interface_ListW(
            (GUID *)&GUID_DEVINTERFACE_USB_HUB_LOCAL,
            hubId, hubIfaces, ARRAYSIZE(hubIfaces), 0) != CR_SUCCESS)
        return 0;
    if (!hubIfaces[0])
        return 0;

    /* Opening a hub with write access may require elevation — a plain
     * failure just means "unknown speed", which is fine. */
    hHub = CreateFileW(hubIfaces, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
    if (hHub == INVALID_HANDLE_VALUE)
        return 0;

    memset(&ci, 0, sizeof(ci));
    ci.ConnectionIndex = port;

    if (DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                        &ci, sizeof(ci), &ci, sizeof(ci), &bytes, NULL)) {
        switch (ci.Speed) {
        case USB_HUB_SPEED_LOW:       speed = USBWARP_SPEED_LOW;   break;
        case USB_HUB_SPEED_FULL:      speed = USBWARP_SPEED_FULL;  break;
        case USB_HUB_SPEED_HIGH:      speed = USBWARP_SPEED_HIGH;  break;
        case USB_HUB_SPEED_SUPER: speed = USBWARP_SPEED_SUPER; break;
        case USB_HUB_SPEED_SUPERPLUS: speed = USBWARP_SPEED_SUPER_PLUS; break;
        default:                      speed = 0;                   break;
        }
    }

    /* Legacy EX can report a USB3 link as high-speed. EX_V2 distinguishes
     * operating speed from device capability; never infer it from bcdUSB. */
    {
        USB_NODE_CONNECTION_INFORMATION_EX_V2 v2 = { 0 };
        v2.ConnectionIndex = port;
        v2.Length = sizeof(v2);
        v2.SupportedUsbProtocols.Usb300 = 1;
        if (DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2,
            &v2, sizeof(v2), &v2, sizeof(v2), &bytes, NULL) && bytes >= sizeof(v2)) {
            if (v2.Flags.DeviceIsOperatingAtSuperSpeedPlusOrHigher)
                speed = USBWARP_SPEED_SUPER_PLUS;
            else if (v2.Flags.DeviceIsOperatingAtSuperSpeedOrHigher)
                speed = USBWARP_SPEED_SUPER;
        }
    }
    if (usbwarp_speed_is_super(speed)) {
        USB_NODE_CONNECTION_SUPERSPEEDPLUS_INFORMATION plus = { 0 };
        plus.ConnectionIndex = port;
        plus.Length = sizeof(plus);
        if (DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_SUPERSPEEDPLUS_INFORMATION,
            &plus, sizeof(plus), &plus, sizeof(plus), &bytes, NULL) && bytes >= sizeof(plus)) {
            uint32_t mode = usbwarp_usb3_decode_link(plus.RxSuperSpeedPlus.AsUlong32,
                plus.RxLaneCount, plus.TxSuperSpeedPlus.AsUlong32, plus.TxLaneCount);
            if (mode) speed = (uint8_t)mode;
        }
    }

    CloseHandle(hHub);
    return speed;
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

    /* Enumerate physical USB devnodes, including unconfigured devices whose
     * function driver has not enabled any device interface. */
    devInfo = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);

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

        /* Skip root hubs. */
        if (strstr(dev->instanceId, "ROOT_HUB") != NULL)
            continue;

        /* FIX #6: Skip composite children (MI_ interfaces). */
        if (IsCompositeChild(dev->instanceId)) {
            dev->isCompositeChild = true;
            continue;
        }

        /* Hubs are infrastructure, including non-root hubs with a VID/PID. */
        {
            ULONG chars=0;
            if (CM_Get_Device_Interface_List_SizeA(&chars,
                    (GUID *)&GUID_DEVINTERFACE_USB_HUB_LOCAL,dev->instanceId,
                    CM_GET_DEVICE_INTERFACE_LIST_PRESENT)==CR_SUCCESS && chars>1 && chars<=65536) {
                char *interfaces=(char *)HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,chars);
                if (!interfaces) continue;
                BOOL hub=CM_Get_Device_Interface_ListA((GUID *)&GUID_DEVINTERFACE_USB_HUB_LOCAL,
                    dev->instanceId,interfaces,chars,CM_GET_DEVICE_INTERFACE_LIST_PRESENT)==CR_SUCCESS && interfaces[0];
                HeapFree(GetProcessHeap(),0,interfaces);
                if (hub) continue;
            }
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

        /* Connection speed from the parent hub (0 = unknown). */
        dev->speed = GetDeviceSpeed(devInfo, &devData);

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
