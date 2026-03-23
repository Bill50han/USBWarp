/*
 * usbwarp_drv.h — UsbWarp Windows Kernel Driver internal definitions.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Private to UsbWarp.sys.  Not exported to user mode.
 * This driver follows the Non-PnP control device pattern from Microsoft's
 * general/ioctl/kmdf/sys/nonpnp sample.
 */

#ifndef USBWARP_DRV_H
#define USBWARP_DRV_H

#include <ntddk.h>
#include <wdf.h>
#include <wdmsec.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <initguid.h>

/* MmSectionObjectType is exported by ntoskrnl but not declared in all
 * WDK headers.  Declare it explicitly for ObReferenceObjectByHandle. */
extern POBJECT_TYPE *MmSectionObjectType;

#include "../include/usbwarp_shared.h"
#include "../include/usbwarp_ioctl.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define USBWARP_DRIVER_TAG         'pWsU'   /* 'UsWp' in little-endian    */
#define USBWARP_NTDEVICE_NAME      L"\\Device\\UsbWarp"
#define USBWARP_SYMBOLIC_LINK      L"\\DosDevices\\UsbWarp"

#define USBWARP_HEARTBEAT_TIMEOUT_MS     6000   /* 6s: spec §06 §9.2     */
#define USBWARP_ORPHAN_GRACE_PERIOD_MS  30000   /* 30s: spec §06 §9.2    */
#define USBWARP_POLL_MSG_BUF_SIZE        4096
#define USBWARP_SHM_MIN_SIZE       (4u << 20)   /* 4 MB                  */
#define USBWARP_SHM_MAX_SIZE     (256u << 20)   /* 256 MB                */
#define USBWARP_URB_TIMEOUT_MS          5000
#define USBWARP_CTRL_TIMEOUT_MS        10000

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Token bucket (per-device rate limiter)
 *
 *   Capacity 200 tokens, refill rate 100/sec (spec §05 §3.1 Layer 6,
 *   spec §03 §7.3).
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USBWARP_TOKEN_BUCKET {
    LONG             Tokens;              /* current tokens (interlocked)  */
    LONG             Capacity;            /* max tokens                    */
    LONG             RefillRate;          /* tokens per second             */
    LARGE_INTEGER    LastRefill;          /* QPC timestamp                 */
} USBWARP_TOKEN_BUCKET, *PUSBWARP_TOKEN_BUCKET;

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Sliding window error counter (spec §06 §2.3)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ERROR_WINDOW_SLOTS  256

typedef struct _USBWARP_ERROR_WINDOW {
    ULONG            Timestamps[ERROR_WINDOW_SLOTS];  /* seconds since boot  */
    ULONG            Head;
    ULONG            Count;
    KSPIN_LOCK       Lock;
    ULONG            WindowSeconds;       /* 60                            */
    ULONG            Threshold;           /* trip when Count >= Threshold  */
} USBWARP_ERROR_WINDOW, *PUSBWARP_ERROR_WINDOW;

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Circuit breaker (per-device, spec §06 §2.1)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum _USBWARP_CB_STATE {
    CB_CLOSED    = 0,
    CB_OPEN      = 1,
    CB_HALF_OPEN = 2,
} USBWARP_CB_STATE, *PUSBWARP_CB_STATE;

typedef struct _USBWARP_CIRCUIT_BREAKER {
    LONG                 State;           /* USBWARP_CB_STATE (interlocked) */
    USBWARP_ERROR_WINDOW HwErrors;        /* URB HW errors: threshold 10   */
    USBWARP_ERROR_WINDOW ProtoErrors;     /* protocol violations: thresh 50*/
    USBWARP_ERROR_WINDOW RateErrors;      /* rate-limit hits: threshold 100*/
    LONG                 ConsecErrors;    /* consecutive URB errors         */
    LARGE_INTEGER        OpenTimestamp;   /* when breaker opened            */
    ULONG                CooldownMs;      /* 30000                          */

    /* P1#4: HALF_OPEN probe tracking (spec §06 §2.1) */
    LONG                 ProbeCount;      /* probes allowed in HALF_OPEN    */
    LONG                 ProbeSuccesses;  /* consecutive probe successes    */
    LONG                 ProbeFailures;   /* any failure → re-OPEN          */
} USBWARP_CIRCUIT_BREAKER, *PUSBWARP_CIRCUIT_BREAKER;

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  URB context (per in-flight URB, spec §02 §6.4.1)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum _USBWARP_URB_STATE {
    UURB_STATE_ACTIVE    = 0,
    UURB_STATE_CANCELING = 1,
    UURB_STATE_DEAD      = 2,
} USBWARP_URB_STATE, *PUSBWARP_URB_STATE;

typedef struct _USBWARP_URB_CONTEXT {
    LIST_ENTRY           ListEntry;       /* per-device pending list        */
    ULONG                TransactionId;
    ULONG                DeviceIndex;

    /* URB parameters (from Ring message) */
    UCHAR                Endpoint;
    UCHAR                Direction;       /* 0=OUT, 1=IN                   */
    UCHAR                TransferType;    /* enum usbwarp_transfer_type    */
    UCHAR                DataMode;        /* NONE/BUFFER/INLINE            */

    ULONG                TransferLength;
    ULONG                BufferOffset;    /* into shared memory            */
    PVOID                TransferVa;      /* kernel VA in SHM mapping      */

    UCHAR                SetupPacket[8];  /* control transfers             */

    /* Inline data copy (for INLINE mode) */
    UCHAR                InlineData[USBWARP_INLINE_DATA_SIZE];
    ULONG                InlineDataLen;

    /* Windows USB submission */
    URB                  Urb;             /* built for submission           */
    WDFREQUEST           WdfRequest;
    WDFMEMORY            UrbMemory;       /* input memory for filter IOCTL  */
    WDFMEMORY            OutMemory;       /* output memory for filter IOCTL */

    /* Filter proxy buffers — stored here so completion routine can
     * access them directly without WdfRequestRetrieveOutputBuffer
     * (which may fail when IRP was completed by a lower WDM driver). */
    PVOID                FilterOutBuf;    /* heap alloc for response+data   */
    ULONG                FilterOutSize;
    PVOID                FilterReqBuf;    /* heap alloc for request struct  */

    /* State */
    LONG                 State;           /* USBWARP_URB_STATE (interlocked)*/

    /* Timestamps */
    LARGE_INTEGER        SubmitTime;
    LARGE_INTEGER        GuestSubmitTime;
} USBWARP_URB_CONTEXT, *PUSBWARP_URB_CONTEXT;

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Per-device context (spec §02 §6.4)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USBWARP_DEVICE_CONTEXT {
    BOOLEAN              InUse;
    ULONG                DeviceIndex;     /* 1-based, = device_id in Ring  */
    UCHAR                DeviceGuid[16];

    /* USB target — via UsbWarpFilter.sys proxy */
    WDFIOTARGET          FilterTarget;    /* I/O target to UsbWarpFilter  */
    UNICODE_STRING       InstancePath;
    WCHAR                InstancePathBuf[256];

    /* Pipe handle map — indexed by endpoint address (P0#1)
     * Populated via URB_FUNCTION_SELECT_CONFIGURATION.
     * Entry [addr] where addr = endpoint | (dir ? 0x80 : 0). */
    USBD_PIPE_HANDLE     PipeHandles[32];  /* raw USBD pipe handles        */
    UCHAR                PipeEndpoints[32]; /* endpoint addresses           */
    UCHAR                NumPipes;

    /* Descriptors */
    USHORT               VendorId;
    USHORT               ProductId;
    UCHAR                Speed;           /* enum usbwarp_usb_speed        */

    /* Pending URBs */
    LIST_ENTRY           PendingUrbs;
    KSPIN_LOCK           PendingLock;
    LONG                 PendingCount;
    KEVENT               DrainEvent;      /* P0#2: signalled when PendingCount==0 */

    /* Rate limiter & circuit breaker */
    USBWARP_TOKEN_BUCKET TokenBucket;
    USBWARP_CIRCUIT_BREAKER Breaker;

    /* Statistics */
    LONG64               TotalUrbs;
    LONG64               TotalBytes;
} USBWARP_DEVICE_CONTEXT, *PUSBWARP_DEVICE_CONTEXT;

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Host-side ring wrapper
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USBWARP_HOST_RING {
    volatile struct usbwarp_ring_header *Hdr;
    volatile UCHAR                      *Data;
    ULONG                                DataSize;
    ULONG                                DataMask;
} USBWARP_HOST_RING, *PUSBWARP_HOST_RING;

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Orphan mode (spec §01 §8.4, §06 §9)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum _USBWARP_ORPHAN_STATE {
    ORPHAN_NONE       = 0,    /* Service alive and registered            */
    ORPHAN_GRACE      = 1,    /* Service lost, grace period running      */
    ORPHAN_EXPIRED    = 2,    /* Grace period expired, shutting down     */
} USBWARP_ORPHAN_STATE, *PUSBWARP_ORPHAN_STATE;

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Global driver context
 *
 *   Stored as WDF device context on the control device.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USBWARP_GLOBAL_CONTEXT {
    /* ── Driver / device ────────────────────────────────────────────────── */
    WDFDEVICE            ControlDevice;
    WDFQUEUE             DefaultQueue;

    /* ── Service registration ───────────────────────────────────────────── */
    BOOLEAN              ServiceRegistered;
    ULONG                ServiceProcessId;
    ULONG                HeartbeatIntervalMs;      /* from REGISTER_SERVICE  */
    HANDLE               ServiceProcessHandle;
    PVOID                ProcessNotifyRegistered;  /* flag for cleanup      */
    KEVENT               ServiceLostEvent;

    /* ── Shared memory ──────────────────────────────────────────────────── */
    BOOLEAN              ShmEstablished;
    PVOID                SectionObject;    /* kernel ref from ObRef         */
    PVOID                ShmKernelBase;    /* MmMapViewInSystemSpace result */
    SIZE_T               ShmSize;
    PMDL                 ShmMdl;           /* locked pages for DISPATCH access */

    volatile struct usbwarp_control_block *ControlBlock;
    USBWARP_HOST_RING    G2hRing;         /* Guest→Host: we consume        */
    USBWARP_HOST_RING    H2gRing;         /* Host→Guest: we produce        */
    volatile UCHAR      *DataRegion;
    ULONG                DataRegionSize;
    ULONG                BufferSize;
    ULONG                BufferCount;

    /* ── Devices ────────────────────────────────────────────────────────── */
    USBWARP_DEVICE_CONTEXT  Devices[USBWARP_MAX_DEVICES_LIMIT];
    LONG                 ActiveDeviceCount;
    KSPIN_LOCK           DeviceLock;

    /* ── Negotiation ────────────────────────────────────────────────────── */
    BOOLEAN              Negotiated;
    USHORT               NegotiatedVersion;
    ULONG                NegotiatedCaps;

    /* ── Poll thread ────────────────────────────────────────────────────── */
    PETHREAD             PollThread;
    KEVENT               PollStopEvent;
    LONG                 PollRunning;

    /* ── Heartbeat / orphan ─────────────────────────────────────────────── */
    LARGE_INTEGER        LastServiceHeartbeat;    /* QPC                    */
    LONG                 OrphanState;             /* USBWARP_ORPHAN_STATE   */
    LARGE_INTEGER        OrphanGraceStart;

    /* ── Global circuit breaker ─────────────────────────────────────────── */
    LONG                 GlobalBreakerOpen;       /* 0=CLOSED, 1=OPEN       */

    /* ── Emergency ──────────────────────────────────────────────────────── */
    LONG                 EmergencyMode;           /* interlocked flag       */
    LONG                 ShuttingDown;

    /* ── Statistics ─────────────────────────────────────────────────────── */
    LONG64               TotalUrbsProcessed;
    LONG64               TotalBytesTransferred;

    /* ── Transaction counter ────────────────────────────────────────────── */
    LONG                 TxnCounter;

} USBWARP_GLOBAL_CONTEXT, *PUSBWARP_GLOBAL_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USBWARP_GLOBAL_CONTEXT, UsbWarpGetContext)

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  Function declarations — usbwarp_entry.c
 * ═══════════════════════════════════════════════════════════════════════════ */

DRIVER_INITIALIZE DriverEntry;

NTSTATUS
UsbWarpDeviceAdd(
    _In_ WDFDRIVER       Driver,
    _In_ PWDFDEVICE_INIT DeviceInit
    );

EVT_WDF_DRIVER_UNLOAD          UsbWarpEvtDriverUnload;
EVT_WDF_OBJECT_CONTEXT_CLEANUP UsbWarpEvtDriverCleanup;
EVT_WDF_DEVICE_SHUTDOWN_NOTIFICATION UsbWarpEvtShutdown;

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Function declarations — usbwarp_ioctl.c
 * ═══════════════════════════════════════════════════════════════════════════ */

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL UsbWarpEvtIoDeviceControl;

/* ═══════════════════════════════════════════════════════════════════════════
 * §12  Function declarations — usbwarp_ring_host.c
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpRingInit(
    _Out_ PUSBWARP_HOST_RING Ring,
    _In_  volatile struct usbwarp_ring_header *Hdr,
    _In_  volatile UCHAR *Data
    );

NTSTATUS
UsbWarpRingProduce(
    _In_  PUSBWARP_HOST_RING Ring,
    _In_  const VOID        *Msg,
    _In_  ULONG              MsgLen
    );

NTSTATUS
UsbWarpRingConsume(
    _In_    PUSBWARP_HOST_RING Ring,
    _Out_   PVOID              MsgOut,
    _In_    ULONG              BufSize,
    _Out_   PULONG             MsgLenOut
    );

ULONG
UsbWarpRingUsagePercent(
    _In_ const PUSBWARP_HOST_RING Ring
    );

/* ═══════════════════════════════════════════════════════════════════════════
 * §13  Function declarations — usbwarp_poll_host.c
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS UsbWarpPollStart(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx);
VOID     UsbWarpPollStop(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx);

VOID
UsbWarpSendNegotiateResp(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ const struct usbwarp_msg_negotiate *NegMsg
    );

NTSTATUS
UsbWarpSendDeviceAdded(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ PUSBWARP_DEVICE_CONTEXT DevCtx
    );

NTSTATUS
UsbWarpSendDeviceRemoved(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ PUSBWARP_DEVICE_CONTEXT DevCtx,
    _In_ ULONG Reason
    );

/* ═══════════════════════════════════════════════════════════════════════════
 * §14  Function declarations — usbwarp_device.c
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS
UsbWarpDeviceOpen(
    _In_  PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_  PUSBWARP_DEVICE_CONTEXT DevCtx,
    _In_  PCUNICODE_STRING        InstancePath
    );

VOID
UsbWarpDeviceClose(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ PUSBWARP_DEVICE_CONTEXT DevCtx
    );

VOID
UsbWarpProcessUrbSubmit(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ const struct usbwarp_msg_urb_submit *Msg
    );

VOID
UsbWarpProcessUrbCancel(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ const struct usbwarp_msg_urb_cancel *Msg
    );

/* ═══════════════════════════════════════════════════════════════════════════
 * §15  Function declarations — usbwarp_safety.c
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Token bucket */
VOID    UsbWarpTokenBucketInit(_Out_ PUSBWARP_TOKEN_BUCKET Tb,
                               LONG Capacity, LONG RefillRate);
BOOLEAN UsbWarpTokenBucketConsume(_Inout_ PUSBWARP_TOKEN_BUCKET Tb);

/* Sliding window */
VOID    UsbWarpErrorWindowInit(_Out_ PUSBWARP_ERROR_WINDOW W,
                               ULONG WindowSec, ULONG Threshold);
VOID    UsbWarpErrorWindowRecord(_Inout_ PUSBWARP_ERROR_WINDOW W);
BOOLEAN UsbWarpErrorWindowTripped(_In_ const PUSBWARP_ERROR_WINDOW W);

/* Circuit breaker */
VOID    UsbWarpBreakerInit(_Out_ PUSBWARP_CIRCUIT_BREAKER Cb);
BOOLEAN UsbWarpBreakerAllows(_Inout_ PUSBWARP_CIRCUIT_BREAKER Cb);
VOID    UsbWarpBreakerRecordHwError(_Inout_ PUSBWARP_CIRCUIT_BREAKER Cb);
VOID    UsbWarpBreakerRecordProtoError(_Inout_ PUSBWARP_CIRCUIT_BREAKER Cb);
VOID    UsbWarpBreakerRecordSuccess(_Inout_ PUSBWARP_CIRCUIT_BREAKER Cb);
VOID    UsbWarpBreakerTrip(_Inout_ PUSBWARP_CIRCUIT_BREAKER Cb);

/* Seven-layer message validation (spec §05 §3.1) */
NTSTATUS
UsbWarpValidateMessage(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ const struct usbwarp_msg_header *Hdr,
    _In_ ULONG MsgLen
    );

/* Orphan mode */
VOID    UsbWarpEnterOrphanMode(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx, ULONG Reason);
VOID    UsbWarpCheckOrphanTimeout(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx);

/* Emergency shutdown (spec §06 §8.3) */
VOID    UsbWarpEmergencyShutdown(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx);

/* Buffer bounds (spec §05 §4.2) */
NTSTATUS
UsbWarpSafeGetTransferVa(
    _In_  PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_  ULONG BufferOffset,
    _In_  ULONG TransferLength,
    _Out_ PVOID *OutVa
    );

/* Process notify callback */
VOID
UsbWarpProcessNotifyCallback(
    _Inout_  PEPROCESS              Process,
    _In_     HANDLE                 ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    );

#endif /* USBWARP_DRV_H */
