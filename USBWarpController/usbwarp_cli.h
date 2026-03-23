/*
 * usbwarp_cli.h — UsbWarp Controller CLI internal declarations.
 *
 * Copyright (c) 2026 UsbWarp Project
 */

#ifndef USBWARP_CLI_H
#define USBWARP_CLI_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/usbwarp_shared.h"
#include "../include/usbwarp_pipe.h"
#include "../USBWarpService/warp_service.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  USB device enumeration (host-side, SetupDi)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USB_HOST_DEVICE {
    char     busId[32];              /* short stable ID for CLI use       */
    char     instanceId[256];        /* device instance path              */
    char     devicePath[512];        /* \\?\USB#...#{GUID} for CreateFile */
    char     description[256];       /* friendly name / description       */
    uint16_t vendorId;
    uint16_t productId;
    uint8_t  deviceClass;
    uint8_t  deviceSubclass;
    uint8_t  deviceProtocol;
    bool     bound;                  /* currently bound to Guest          */
    bool     isCompositeChild;       /* skip: child of USB composite      */
    GUID     containerId;            /* real DEVPKEY_Device_ContainerId   */
} USB_HOST_DEVICE;

#define MAX_USB_DEVICES  128

typedef struct _USB_DEVICE_LIST {
    USB_HOST_DEVICE devices[MAX_USB_DEVICES];
    int             count;
} USB_DEVICE_LIST;

int  UsbEnumerate(USB_DEVICE_LIST *list);
bool ParseGuid(const char *str, GUID *out);
void FormatGuid(const GUID *g, char *buf, int bufLen);
void FormatBytes(uint64_t bytes, char *buf, int bufLen);
void FormatDuration(uint64_t seconds, char *buf, int bufLen);

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Pipe client
 * ═══════════════════════════════════════════════════════════════════════════ */

/* #8: Larger buffer for future extensibility. */
#define PIPE_BUF_SIZE  65536

typedef struct _PIPE_CLIENT {
    HANDLE   hPipe;
    uint32_t nextSeqId;
} PIPE_CLIENT;

int  PipeConnect(PIPE_CLIENT *pc);
void PipeDisconnect(PIPE_CLIENT *pc);
int  PipeSendCommand(PIPE_CLIENT *pc,
                     uint32_t command,
                     const void *payload, uint32_t payloadLen,
                     void *respBuf, uint32_t respBufSize,
                     uint32_t *respLen);

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline const char *SessionStateName(uint32_t state)
{
    switch (state) {
    case 0: return "INITIALIZING";
    case 1: return "HDV_STARTED";
    case 2: return "RUNNING";
    default: return "UNKNOWN";
    }
}

static inline bool GuidsEqual(const GUID *a, const uint8_t b[16])
{
    return memcmp(a, b, 16) == 0;
}

#endif /* USBWARP_CLI_H */
