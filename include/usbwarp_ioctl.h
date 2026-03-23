/*
 * usbwarp_ioctl.h — UsbWarp Service ↔ Kernel Driver IOCTL Interface
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Defines IOCTL control codes, input/output structures, and the device
 * interface GUID used by UsbWarpService.exe to communicate with UsbWarp.sys.
 *
 * Windows-only header.  Requires <windows.h> or <ntddk.h> before inclusion.
 */

#ifndef USBWARP_IOCTL_H
#define USBWARP_IOCTL_H

#include "usbwarp_shared.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Device interface GUID
 *
 *   Service opens the driver via SetupDiGetClassDevs + CreateFile on this
 *   interface GUID.
 *
 *   {B3E7A5D1-4C2F-4D8E-9A1B-6F3E2D1C0A9B}
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef DEFINE_GUID  /* available when windows.h or ntddk.h is included */

DEFINE_GUID(GUID_USBWARP_CONTROL_INTERFACE,
    0xb3e7a5d1, 0x4c2f, 0x4d8e,
    0x9a, 0x1b, 0x6f, 0x3e, 0x2d, 0x1c, 0x0a, 0x9b);

#endif /* DEFINE_GUID */

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  IOCTL codes
 *
 *   Device type:  FILE_DEVICE_UNKNOWN
 *   Method:       METHOD_BUFFERED  (all IOCTLs)
 *   Access:       FILE_WRITE_ACCESS for mutating ops,
 *                 FILE_READ_ACCESS  for queries
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef CTL_CODE   /* allow standalone compilation for unit tests */
  #define CTL_CODE(t,f,m,a) (((t)<<16)|((a)<<14)|((f)<<2)|(m))
  #define FILE_DEVICE_UNKNOWN   0x00000022
  #define METHOD_BUFFERED       0
  #define FILE_READ_ACCESS      0x0001
  #define FILE_WRITE_ACCESS     0x0002
#endif

#define USBWARP_DEVICE_TYPE  FILE_DEVICE_UNKNOWN

#define IOCTL_USBWARP_REGISTER_SERVICE \
    CTL_CODE(USBWARP_DEVICE_TYPE, 0x806, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_USBWARP_SETUP_SHM \
    CTL_CODE(USBWARP_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_USBWARP_TEARDOWN_SHM \
    CTL_CODE(USBWARP_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_USBWARP_BIND_DEVICE \
    CTL_CODE(USBWARP_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_USBWARP_UNBIND_DEVICE \
    CTL_CODE(USBWARP_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_USBWARP_HEARTBEAT \
    CTL_CODE(USBWARP_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_USBWARP_QUERY_STATUS \
    CTL_CODE(USBWARP_DEVICE_TYPE, 0x805, METHOD_BUFFERED, FILE_READ_ACCESS)

/* Debug-only fault injection (gated behind DBG build) */
#define IOCTL_USBWARP_DBG_INJECT_ERROR \
    CTL_CODE(USBWARP_DEVICE_TYPE, 0x900, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Structure version
 * ═══════════════════════════════════════════════════════════════════════════ */

#define USBWARP_IOCTL_STRUCT_VERSION  1u

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  IOCTL_USBWARP_REGISTER_SERVICE
 *
 *   First call after Service starts.  Establishes identity, heartbeat
 *   monitoring, and process-termination notification in the driver.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USBWARP_REGISTER_SERVICE_IN {
    uint32_t Version;                    /* USBWARP_IOCTL_STRUCT_VERSION      */
    uint32_t ProcessId;                  /* Service PID                       */
    uint32_t HeartbeatIntervalMs;        /* recommended 2000; range 500–10000 */
    uint32_t Reserved[4];                /* must be zero                      */
} USBWARP_REGISTER_SERVICE_IN, * PUSBWARP_REGISTER_SERVICE_IN;

typedef struct _USBWARP_REGISTER_SERVICE_OUT {
    uint32_t Version;
    uint32_t DriverVersion;              /* USBWARP_MAKE_VERSION              */
    uint32_t MaxDevices;
    uint32_t Reserved[4];
} USBWARP_REGISTER_SERVICE_OUT, * PUSBWARP_REGISTER_SERVICE_OUT;

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  IOCTL_USBWARP_SETUP_SHM
 *
 *   Passes a Section handle to the driver.  The driver duplicates the
 *   reference via ObReferenceObjectByHandle(UserMode) and maps the section
 *   into system-space.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USBWARP_SETUP_SHM_IN {
    uint32_t Version;
    uint64_t SectionHandle;              /* user-mode HANDLE, cast to uint64  */
    uint64_t ExpectedSize;               /* must match section size           */
    uint32_t RingSizePerDirection;       /* bytes, power of 2, incl. header   */
    uint32_t BufferSize;                 /* per-slot, power of 2              */
    uint32_t Flags;                      /* reserved, 0                       */
    uint32_t Reserved[2];
} USBWARP_SETUP_SHM_IN, * PUSBWARP_SETUP_SHM_IN;

typedef struct _USBWARP_SETUP_SHM_OUT {
    uint32_t Version;
    uint32_t BufferCount;                /* Data Region slot count            */
    uint64_t DataRegionOffset;
    uint64_t DataRegionSize;
    uint32_t Reserved[4];
} USBWARP_SETUP_SHM_OUT, * PUSBWARP_SETUP_SHM_OUT;

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  IOCTL_USBWARP_TEARDOWN_SHM
 * ═══════════════════════════════════════════════════════════════════════════ */

#define USBWARP_TEARDOWN_FLAG_GRACEFUL  0x00000001u
#define USBWARP_TEARDOWN_FLAG_FORCE     0x00000002u

typedef struct _USBWARP_TEARDOWN_SHM_IN {
    uint32_t Version;
    uint32_t Flags;                      /* GRACEFUL or FORCE                 */
    uint32_t Reserved[2];
} USBWARP_TEARDOWN_SHM_IN, * PUSBWARP_TEARDOWN_SHM_IN;

/* No output buffer. */

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  IOCTL_USBWARP_BIND_DEVICE
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USBWARP_BIND_DEVICE_IN {
    uint32_t Version;
    uint8_t  DeviceGuid[16];             /* binary GUID, Service-assigned     */
    uint32_t InstancePathLength;         /* bytes including NUL terminator    */
    uint16_t InstancePath[256];          /* UTF-16 device instance path       */
    uint32_t Flags;                      /* reserved, 0                       */
    uint32_t Reserved[2];
} USBWARP_BIND_DEVICE_IN, * PUSBWARP_BIND_DEVICE_IN;

typedef struct _USBWARP_BIND_DEVICE_OUT {
    uint32_t Version;
    uint32_t DeviceIndex;                /* driver-internal index (device_id) */
    uint16_t VendorId;
    uint16_t ProductId;
    uint32_t Reserved[2];
} USBWARP_BIND_DEVICE_OUT, * PUSBWARP_BIND_DEVICE_OUT;

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  IOCTL_USBWARP_UNBIND_DEVICE
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USBWARP_UNBIND_DEVICE_IN {
    uint32_t Version;
    uint8_t  DeviceGuid[16];
    uint32_t Flags;                      /* GRACEFUL or FORCE                 */
    uint32_t Reserved[2];
} USBWARP_UNBIND_DEVICE_IN, * PUSBWARP_UNBIND_DEVICE_IN;

/* No output buffer. */

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  IOCTL_USBWARP_HEARTBEAT
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USBWARP_HEARTBEAT_IN {
    uint32_t Version;
    uint32_t _pad0;
    uint64_t Timestamp;                  /* QPC-based                         */
    uint32_t ServiceState;               /* Service session-state enum        */
    uint32_t Reserved;
} USBWARP_HEARTBEAT_IN, * PUSBWARP_HEARTBEAT_IN;

typedef struct _USBWARP_HEARTBEAT_OUT {
    uint32_t Version;
    uint32_t DriverState;                /* enum usbwarp_state                */
    uint32_t PendingUrbCount;
    uint32_t Reserved;
} USBWARP_HEARTBEAT_OUT, * PUSBWARP_HEARTBEAT_OUT;

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  IOCTL_USBWARP_QUERY_STATUS
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USBWARP_QUERY_STATUS_OUT {
    uint32_t Version;
    uint32_t DriverState;                /* enum usbwarp_state                */
    uint8_t  ShmEstablished;             /* boolean                           */
    uint8_t  ServiceRegistered;          /* boolean                           */
    uint8_t  OrphanMode;                 /* boolean                           */
    uint8_t  _pad0;
    uint32_t ActiveDeviceCount;
    uint32_t TotalPendingUrbs;
    uint64_t TotalUrbsProcessed;
    uint64_t TotalBytesTransferred;
    uint32_t RingG2HUsagePercent;
    uint32_t RingH2GUsagePercent;
    uint32_t BufferPoolUsagePercent;
    uint32_t HealthScore;                /* 0–100                             */
    uint32_t Reserved[4];
} USBWARP_QUERY_STATUS_OUT, * PUSBWARP_QUERY_STATUS_OUT;

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  IOCTL_USBWARP_DBG_INJECT_ERROR  (debug builds only)
 * ═══════════════════════════════════════════════════════════════════════════ */

enum usbwarp_dbg_error_type {
    USBWARP_DBG_MSG_CORRUPTION         = 1,
    USBWARP_DBG_URB_TIMEOUT            = 2,
    USBWARP_DBG_RING_FULL              = 3,
    USBWARP_DBG_BUFFER_EXHAUST         = 4,
    USBWARP_DBG_SERVICE_LOST           = 5,
    USBWARP_DBG_HEARTBEAT_STOP         = 6,
    USBWARP_DBG_DEVICE_UNPLUG          = 7,
};

typedef struct _USBWARP_DBG_INJECT_ERROR_IN {
    uint32_t ErrorType;                  /* enum usbwarp_dbg_error_type       */
    uint32_t DeviceId;                   /* 0 = global                        */
    uint32_t Count;                      /* repetitions                       */
} USBWARP_DBG_INJECT_ERROR_IN, * PUSBWARP_DBG_INJECT_ERROR_IN;

#endif /* USBWARP_IOCTL_H */
