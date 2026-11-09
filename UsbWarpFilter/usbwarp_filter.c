/* SPDX-License-Identifier: GPL-2.0-only
 * UsbWarpFilter is the dedicated USB capture function driver. The historical
 * binary name is retained, but this is NOT a USB class filter. SetupAPI installs
 * it on exactly one selected physical device instance. KMDF owns PnP, power,
 * queues and cancellation; UsbWarp.sys owns the single kernel file lease.
 * The original Windows function driver is absent for the entire capture.
 */
#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <usb.h>
#include <devpkey.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <ntstrsafe.h>
#include <wdmsec.h>

/* Include shared IOCTL header — needs INITGUID for DEFINE_GUID. */
#include <initguid.h>
#include "../include/usbwarp_filter_ioctl.h"
#include "../include/usbwarp_usb3.h"

#define PRINT_LOG 0
#if !(PRINT_LOG)
#undef KdPrint
#define KdPrint(_x_) 
#endif

#define FILTER_TAG  'tFwU'   /* UwFt */

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Filter device extension
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _FILTER_EXT {
    WDFDEVICE Device;
    WDFQUEUE Queue;
    WDFFILEOBJECT Owner;
    BOOLEAN Retired;
    PDEVICE_OBJECT  Self;          /* KMDF capture FDO                   */
    PDEVICE_OBJECT  LowerDevice;   /* device below us (FDO or PDO)       */
    PDEVICE_OBJECT  Pdo;           /* physical device object (bottom)     */
    USBD_HANDLE     UsbdHandle;    /* registered USB client handle        */
    BOOLEAN         Started;       /* IRP_MN_START_DEVICE completed       */
    BOOLEAN         Removed;       /* IRP_MN_REMOVE_DEVICE received       */
    BOOLEAN         ConfigSelected;/* SELECT_CONFIG done, pipes cached    */

    /* Cached pipe map from last SELECT_CONFIGURATION. */
    USBWARP_FILTER_PIPES_RESPONSE CachedPipes;
    USBD_CONFIGURATION_HANDLE ConfigurationHandle;
    PUSB_CONFIGURATION_DESCRIPTOR Configuration;
    KMUTEX ConfigMutex;
    LONG ActiveByInterface[256];
    LONG ActiveTransfers;
    LONG ActiveByPipe[32];
    USHORT MaxStreams;
    struct {
        ULONG Count;
        USBD_PIPE_HANDLE PipeHandle;
        PUSBD_STREAM_INFORMATION Info;
        UCHAR InterfaceNumber;
    } Streams[32];
} FILTER_EXT, *PFILTER_EXT;

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Forward declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILTER_EXT, CaptureGetContext);
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD CaptureAddDevice;
EVT_WDF_DEVICE_PREPARE_HARDWARE CapturePrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE CaptureRelease;
EVT_WDF_DEVICE_D0_ENTRY CaptureD0Entry;
EVT_WDF_DEVICE_D0_EXIT CaptureD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL CaptureIoctl;
EVT_WDF_IO_QUEUE_IO_STOP CaptureIoStop;
EVT_WDF_DEVICE_FILE_CREATE CaptureCreate;
EVT_WDF_FILE_CLEANUP CaptureCleanup;
EVT_WDF_REQUEST_COMPLETION_ROUTINE UrbCompletionRoutine;
static NTSTATUS SubmitSyncUrb(PFILTER_EXT Ext, PURB Urb);

typedef struct _FILTER_URB_CONTEXT {
    PURB Urb;
    PVOID TransferBuffer;
    BOOLEAN FreeBuffer;
    BOOLEAN Iso;
    BOOLEAN DirectionIn;
    ULONG InterfaceNumber;
    ULONG EndpointIndex;
    PFILTER_EXT Ext;
    ULONG TransferLength;
    ULONG NumberOfPackets;
    ULONG PacketLengths[1];
} FILTER_URB_CONTEXT, *PFILTER_URB_CONTEXT;

/* Forward the ORIGINAL IRP. Its cancellation now belongs to the USB stack;
 * there is no second IRP or cancel routine that can race a freed context. */
VOID
UrbCompletionRoutine(WDFREQUEST Request, WDFIOTARGET Target, PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    PIRP Irp = WdfRequestWdmGetIrp(Request);
    PFILTER_URB_CONTEXT ctx = Context;
    PURB urb = ctx->Urb;
    PVOID out = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS nt = Params ? Params->IoStatus.Status : WdfRequestGetStatus(Request);
    UNREFERENCED_PARAMETER(Target);

    if (ctx->Iso) {
        USBWARP_FILTER_ISO_RESPONSE *resp = out;
        struct usbwarp_iso_packet_result *results = (void *)(resp + 1);
        resp->NtStatus = nt;
        resp->UsbdStatus = urb->UrbHeader.Status;
        resp->NumberOfPackets = ctx->NumberOfPackets;
        resp->StartFrame = urb->UrbIsochronousTransfer.StartFrame;
        for (ULONG i = 0; i < ctx->NumberOfPackets; ++i) {
            USBD_STATUS ps = urb->UrbIsochronousTransfer.IsoPacket[i].Status;
            ULONG length = ctx->DirectionIn ? urb->UrbIsochronousTransfer.IsoPacket[i].Length :
                           (USBD_SUCCESS(ps) ? ctx->PacketLengths[i] : 0);
            if (length > ctx->PacketLengths[i]) {
                length = 0;
                ps = USBD_STATUS_DATA_OVERRUN;
            }
            /* Cancellation may follow already completed packets. Preserve
             * their data; only unscheduled packets inherit cancellation. */
            if (nt == STATUS_CANCELLED && ps == USBD_STATUS_NOT_ACCESSED)
                ps = USBD_STATUS_CANCELED;
            results[i].actual_length = length;
            results[i].status = ps;
        }
        Irp->IoStatus.Information = sizeof(*resp) + ctx->NumberOfPackets * sizeof(*results);
    } else {
        USBWARP_FILTER_URB_RESPONSE *resp = out;
        ULONG actual = urb->UrbHeader.Function == URB_FUNCTION_CONTROL_TRANSFER ?
            urb->UrbControlTransfer.TransferBufferLength :
            urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
        if (!NT_SUCCESS(nt) || !USBD_SUCCESS(urb->UrbHeader.Status))
            actual = 0;
        actual = min(actual, ctx->TransferLength);
        resp->NtStatus = nt;
        resp->UsbdStatus = urb->UrbHeader.Status;
        resp->ActualLength = actual;
        resp->Reserved = 0;
        Irp->IoStatus.Information = sizeof(*resp);
        if (ctx->DirectionIn && ctx->FreeBuffer && actual) {
            RtlCopyMemory(resp + 1, ctx->TransferBuffer, actual);
            Irp->IoStatus.Information += actual;
        }
    }
    /* The IOCTL succeeded; the response retains USB/transport status. */
    Irp->IoStatus.Status = STATUS_SUCCESS;
    if (ctx->FreeBuffer)
        ExFreePoolWithTag(ctx->TransferBuffer, FILTER_TAG);
    USBD_UrbFree(ctx->Ext->UsbdHandle, ctx->Urb);
    if (ctx->InterfaceNumber != MAXULONG)
        InterlockedDecrement(&ctx->Ext->ActiveByInterface[ctx->InterfaceNumber]);
    if (ctx->EndpointIndex != MAXULONG)
        InterlockedDecrement(&ctx->Ext->ActiveByPipe[ctx->EndpointIndex]);
    InterlockedDecrement(&ctx->Ext->ActiveTransfers);
    ExFreePoolWithTag(ctx, FILTER_TAG);
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Irp->IoStatus.Information);
}

static NTSTATUS
ForwardUrb(PFILTER_EXT Ext, WDFREQUEST Request, PFILTER_URB_CONTEXT Ctx, PBOOLEAN Forwarded)
{
    IO_STACK_LOCATION stack = {0};
    Ctx->Ext = Ext;
    stack.MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack.Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack.Parameters.Others.Argument1 = Ctx->Urb;
    WdfRequestWdmFormatUsingStackLocation(Request, &stack);
    WdfRequestSetCompletionRoutine(Request, UrbCompletionRoutine, Ctx);
    InterlockedIncrement(&Ext->ActiveTransfers);
    if (Ctx->InterfaceNumber != MAXULONG)
        InterlockedIncrement(&Ext->ActiveByInterface[Ctx->InterfaceNumber]);
    if (Ctx->EndpointIndex != MAXULONG)
        InterlockedIncrement(&Ext->ActiveByPipe[Ctx->EndpointIndex]);
    *Forwarded = TRUE;
    KeReleaseMutex(&Ext->ConfigMutex, FALSE);
    /* The default target is the USB bus PDO. No Windows class driver remains
     * in this stack. KMDF forwards cancellation and owns the original IRP. */
    if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(Ext->Device), NULL))
        UrbCompletionRoutine(Request, NULL, NULL, Ctx);
    return STATUS_PENDING; /* ownership transferred even for inline completion */
}

static USBWARP_FILTER_PIPE_INFO *
LookupPipe(PFILTER_EXT Ext, ULONG Endpoint, ULONG Direction, ULONG Type)
{
    for (ULONG i = 0; i < Ext->CachedPipes.NumPipes; ++i) {
        USBWARP_FILTER_PIPE_INFO *p = &Ext->CachedPipes.Pipes[i];
        if (p->EndpointAddress == (Endpoint | (Direction ? 0x80 : 0)) && p->PipeType == Type)
            return p;
    }
    return NULL;
}

static NTSTATUS
HandleSubmitUrb(PFILTER_EXT Ext, WDFREQUEST Request, PBOOLEAN Forwarded)
{
    PIRP OrigIrp = WdfRequestWdmGetIrp(Request);
    PIO_STACK_LOCATION  irpSp = IoGetCurrentIrpStackLocation(OrigIrp);
    PVOID               inBuf = OrigIrp->AssociatedIrp.SystemBuffer;
    ULONG               inLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    NTSTATUS            status;
    PURB                urb = NULL;
    PFILTER_URB_CONTEXT ctx = NULL;
    PVOID               transferBuf = NULL;
    BOOLEAN             freeTransferBuf = FALSE;

    if (inLen < sizeof(USBWARP_FILTER_URB_REQUEST))
        return STATUS_BUFFER_TOO_SMALL;

    USBWARP_FILTER_URB_REQUEST *req = (USBWARP_FILTER_URB_REQUEST *)inBuf;
    if ((req->StreamId && (req->TransferType != USBWARP_FXFER_BULK || req->StreamId > USBWARP_MAX_STREAMS)) ||
        req->Direction > 1 || req->Endpoint > 15 || req->TransferLength > USBWARP_MAX_TRANSFER_SIZE ||
        irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(USBWARP_FILTER_URB_RESPONSE) +
        ((!req->KernelVa && req->Direction) ? req->TransferLength : 0))
        return STATUS_INVALID_PARAMETER;
    if (req->InlineDataLength &&
        (req->InlineDataOffset < sizeof(*req) || req->InlineDataOffset > inLen ||
         req->InlineDataLength > inLen - req->InlineDataOffset))
        return STATUS_INVALID_PARAMETER;


    /* Trace every URB for debugging. */
    if (req->TransferType == USBWARP_FXFER_CONTROL) {
        KdPrint(("UsbWarpFilter: SUBMIT CTRL setup=%02x %02x "
                 "wVal=%04x wIdx=%04x wLen=%04x "
                 "xferLen=%u dir=%u KVa=%p inlOff=%u inlLen=%u\n",
                 req->SetupPacket[0], req->SetupPacket[1],
                 (USHORT)(req->SetupPacket[2] | (req->SetupPacket[3] << 8)),
                 (USHORT)(req->SetupPacket[4] | (req->SetupPacket[5] << 8)),
                 (USHORT)(req->SetupPacket[6] | (req->SetupPacket[7] << 8)),
                 req->TransferLength, req->Direction,
                 (PVOID)(ULONG_PTR)req->KernelVa,
                 req->InlineDataOffset, req->InlineDataLength));
    } else {
        KdPrint(("UsbWarpFilter: SUBMIT %s ep=0x%02x dir=%u len=%u "
                 "KVa=%p pipe=%p\n",
                 req->TransferType == USBWARP_FXFER_BULK ? "BULK" : "INTR",
                 req->Endpoint | (req->Direction ? 0x80 : 0),
                 req->Direction, req->TransferLength,
                 (PVOID)(ULONG_PTR)req->KernelVa,
                 (PVOID)(ULONG_PTR)req->PipeHandle));
    }

    if (!Ext->UsbdHandle || !Ext->Started)
        return STATUS_INVALID_DEVICE_STATE;

    /* ── Defense-in-depth: pre-validate BEFORE any allocation. ─────────
     * These checks duplicate UsbWarp.sys Layer 4.  The filter is the
     * LAST line of defense — if the main driver has a bug or is running
     * an old binary, we must not send dangerous URBs to the USB stack. */
    if (req->TransferType == USBWARP_FXFER_CONTROL) {
        UCHAR bmReq = req->SetupPacket[0];
        UCHAR bReq  = req->SetupPacket[1];
        USHORT wLen = (USHORT)(req->SetupPacket[6] |
                               (req->SetupPacket[7] << 8));

        /* Block SET_ADDRESS — sends to xHCI cause fatal errors. */
        if ((bmReq & 0x60) == 0x00 && bReq == 0x05) {
            KdPrint(("UsbWarpFilter: BLOCKED SET_ADDRESS\n"));
            return STATUS_INVALID_PARAMETER;
        }

        /* wLength vs TransferLength mismatch. */
        if (wLen != req->TransferLength) {
            KdPrint(("UsbWarpFilter: BLOCKED wLen=%u != xferLen=%u\n",
                     wLen, req->TransferLength));
            return STATUS_INVALID_PARAMETER;
        }

        /* These requests must update USBD's pipe/alternate-setting state.
         * UsbWarp.sys routes them through the selection IOCTLs. */
        if ((bmReq == 0 && bReq == USB_REQUEST_SET_CONFIGURATION) ||
            (bmReq == 1 && bReq == USB_REQUEST_SET_INTERFACE))
            return STATUS_INVALID_PARAMETER;

        /* Direction mismatch (only if data phase exists). */
        if (wLen > 0) {
            UCHAR setupDir = (bmReq & 0x80) ? 1 : 0;
            if (setupDir != req->Direction) {
                KdPrint(("UsbWarpFilter: BLOCKED dir mismatch setup=%u req=%u\n",
                         setupDir, req->Direction));
                return STATUS_INVALID_PARAMETER;
            }
        }
    }

    /* Fix #3: Validate inline data length. */
    if (req->InlineDataLength > req->TransferLength) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Determine transfer buffer. */
    if (req->KernelVa != 0) {
        /* Zero-copy path: direct SHM pointer from UsbWarp.sys. */
        transferBuf = (PVOID)(ULONG_PTR)req->KernelVa;
    } else if (req->InlineDataLength > 0 &&
               inLen >= req->InlineDataOffset + req->InlineDataLength) {
        /* Inline path: data follows the request struct.
         * Must copy because the system buffer is shared in/out. */
        transferBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                       req->TransferLength, FILTER_TAG);
        if (!transferBuf) return STATUS_INSUFFICIENT_RESOURCES;
        freeTransferBuf = TRUE;

        /* Copy OUT data (for OUT transfers). */
        if (req->Direction == 0 && req->InlineDataLength > 0) {
            RtlCopyMemory(transferBuf,
                          (PUCHAR)inBuf + req->InlineDataOffset,
                          min(req->InlineDataLength, req->TransferLength));
        }
    } else if (req->TransferLength > 0) {
        /* Need a buffer for IN transfers. */
        transferBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                       req->TransferLength, FILTER_TAG);
        if (!transferBuf) return STATUS_INSUFFICIENT_RESOURCES;
        freeTransferBuf = TRUE;
    }

    /* Allocate URB using USBD_UrbAllocate — embeds our USBD_HANDLE. */
    status = USBD_UrbAllocate(Ext->UsbdHandle, &urb);
    if (!NT_SUCCESS(status) || !urb) {
        if (freeTransferBuf) ExFreePoolWithTag(transferBuf, FILTER_TAG);
        return status;
    }

    /* Build URB based on transfer type. */
    switch (req->TransferType) {
    case USBWARP_FXFER_BULK:
    case USBWARP_FXFER_INTERRUPT:
        urb->UrbHeader.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
        urb->UrbHeader.Length   = sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
        urb->UrbBulkOrInterruptTransfer.TransferBuffer = transferBuf;
        urb->UrbBulkOrInterruptTransfer.TransferBufferLength = req->TransferLength;
        urb->UrbBulkOrInterruptTransfer.TransferBufferMDL = NULL;
        urb->UrbBulkOrInterruptTransfer.TransferFlags =
            (req->Direction ? USBD_TRANSFER_DIRECTION_IN : 0) |
            USBD_SHORT_TRANSFER_OK;
        urb->UrbBulkOrInterruptTransfer.UrbLink = NULL;

        /* Use real USBD_PIPE_HANDLE from SELECT_CONFIG response.
         * The main driver passes it in req->PipeHandle (64-bit). */
        urb->UrbBulkOrInterruptTransfer.PipeHandle =
            (USBD_PIPE_HANDLE)(ULONG_PTR)req->PipeHandle;

        break;

    case USBWARP_FXFER_CONTROL: {
        UCHAR bmReqType = req->SetupPacket[0];
        UCHAR bRequest  = req->SetupPacket[1];

        /* Block SET_ADDRESS — managed by USB core/HCD. */
        if ((bmReqType & 0x60) == 0x00 && bRequest == 0x05) {
            status = STATUS_INVALID_PARAMETER;
            goto Cleanup;
        }

        /* ALL control transfers use URB_FUNCTION_CONTROL_TRANSFER with
         * the raw 8-byte setup packet passed through verbatim.
         *
         * Rationale: the Guest's Linux USB Core has already constructed
         * the complete, correct setup packet (bmRequestType, bRequest,
         * wValue, wIndex, wLength).  Our job is to relay it unchanged.
         *
         * The alternative (URB_FUNCTION_VENDOR_DEVICE, CLASS_INTERFACE,
         * etc.) are convenience wrappers that RE-CONSTRUCT the setup
         * packet from their fields.  This is wrong for us because:
         *   1. They overwrite bmRequestType based on URB function type
         *   2. Standard requests (GET_DESCRIPTOR etc.) would be sent
         *      as vendor requests → device STALLs
         *   3. We'd need to reverse-engineer the Guest's intent from
         *      the setup packet just to have Windows re-encode it
         *
         * URB_FUNCTION_CONTROL_TRANSFER is the raw, universal path.
         * It works for standard, class, AND vendor requests equally.
         * PipeHandle=NULL + USBD_DEFAULT_PIPE_TRANSFER flag = use EP0.
         */
        urb->UrbHeader.Function = URB_FUNCTION_CONTROL_TRANSFER;
        urb->UrbHeader.Length   = sizeof(struct _URB_CONTROL_TRANSFER);
        urb->UrbControlTransfer.PipeHandle = NULL;
        urb->UrbControlTransfer.TransferBuffer = transferBuf;
        urb->UrbControlTransfer.TransferBufferLength = req->TransferLength;
        urb->UrbControlTransfer.TransferBufferMDL = NULL;
        urb->UrbControlTransfer.TransferFlags =
            (req->Direction ? USBD_TRANSFER_DIRECTION_IN : 0) |
            USBD_SHORT_TRANSFER_OK | USBD_DEFAULT_PIPE_TRANSFER;
        urb->UrbControlTransfer.UrbLink = NULL;
        RtlCopyMemory(urb->UrbControlTransfer.SetupPacket,
                      req->SetupPacket, 8);
        break;
    }

    default:
        status = STATUS_NOT_SUPPORTED;
        goto Cleanup;
    }

    ctx = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ctx), FILTER_TAG);
    if (!ctx) { status = STATUS_INSUFFICIENT_RESOURCES; goto Cleanup; }
    ctx->Urb = urb;
    ctx->TransferBuffer = transferBuf;
    ctx->FreeBuffer = freeTransferBuf;
    ctx->DirectionIn = req->Direction != 0;
    ctx->TransferLength = req->TransferLength;
    ctx->InterfaceNumber = MAXULONG;
    ctx->EndpointIndex = MAXULONG;
    KeWaitForSingleObject(&Ext->ConfigMutex, Executive, KernelMode, FALSE, NULL);
    if (req->TransferType != USBWARP_FXFER_CONTROL) {
        USBWARP_FILTER_PIPE_INFO *pipe = LookupPipe(Ext, req->Endpoint, req->Direction,
            req->TransferType == USBWARP_FXFER_BULK ? UsbdPipeTypeBulk : UsbdPipeTypeInterrupt);
        if (!pipe) {
            KeReleaseMutex(&Ext->ConfigMutex, FALSE);
            status = STATUS_INVALID_PARAMETER;
            goto Cleanup;
        }
        /* Resolve under the same lock as alternate-setting changes. Never
         * trust a cached pointer received from another driver. */
        urb->UrbBulkOrInterruptTransfer.PipeHandle = (USBD_PIPE_HANDLE)(ULONG_PTR)pipe->PipeHandle;
        ctx->InterfaceNumber = pipe->InterfaceNumber;
        ctx->EndpointIndex = req->Endpoint + (req->Direction ? 16 : 0);
        if (Ext->Streams[ctx->EndpointIndex].Count) {
            USBD_PIPE_HANDLE stream = NULL;
            for (ULONG i = 0; i < Ext->Streams[ctx->EndpointIndex].Count; ++i) {
                PUSBD_STREAM_INFORMATION info = &Ext->Streams[ctx->EndpointIndex].Info[i];
                if (info->StreamID == req->StreamId) {
                    if (!info->MaximumTransferSize || req->TransferLength <= info->MaximumTransferSize)
                        stream = info->PipeHandle;
                    break;
                }
            }
            if (!stream) {
                KeReleaseMutex(&Ext->ConfigMutex, FALSE);
                status = STATUS_INVALID_PARAMETER;
                goto Cleanup;
            }
            urb->UrbBulkOrInterruptTransfer.PipeHandle = stream;
        } else if (req->StreamId) {
            KeReleaseMutex(&Ext->ConfigMutex, FALSE);
            status = STATUS_INVALID_PARAMETER;
            goto Cleanup;
        }

    }
    return ForwardUrb(Ext, Request, ctx, Forwarded);
Cleanup:
    if (ctx) ExFreePoolWithTag(ctx, FILTER_TAG);
    if (urb) USBD_UrbFree(Ext->UsbdHandle, urb);
    if (freeTransferBuf && transferBuf)
        ExFreePoolWithTag(transferBuf, FILTER_TAG);
    return status;
}

static NTSTATUS
HandleSubmitIso(PFILTER_EXT Ext, WDFREQUEST Request, PBOOLEAN Forwarded)
{
    PIRP Irp = WdfRequestWdmGetIrp(Request);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    USBWARP_FILTER_ISO_REQUEST *req = Irp->AssociatedIrp.SystemBuffer;
    const struct usbwarp_iso_packet_desc *packets;
    USBWARP_FILTER_PIPE_INFO *pipe;
    PFILTER_URB_CONTEXT ctx;
    PURB urb = NULL;
    NTSTATUS status;
    ULONG n, expectedInterval;
    if (sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(*req))
        return STATUS_BUFFER_TOO_SMALL;
    n = req->NumberOfPackets;
    if (!usbwarp_iso_packet_count_valid(n, req->Speed, req->Interval) ||
        req->Endpoint == 0 || req->Endpoint > 15 || req->Direction > 1 || !req->KernelVa ||
        sp->Parameters.DeviceIoControl.InputBufferLength != sizeof(*req) + n * 8 ||
        sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(USBWARP_FILTER_ISO_RESPONSE) + n * 8)
        return STATUS_INVALID_PARAMETER;
    packets = (const void *)(req + 1);
    if (!usbwarp_iso_layout_valid(packets, n, req->TransferLength))
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&Ext->ConfigMutex, Executive, KernelMode, FALSE, NULL);
    pipe = LookupPipe(Ext, req->Endpoint, req->Direction, UsbdPipeTypeIsochronous);
    if (!pipe || pipe->Interval == 0 || pipe->Interval > 16) {
        status = STATUS_INVALID_PARAMETER;
        goto Unlock;
    }
    expectedInterval = req->Speed == USBWARP_SPEED_FULL ? 1 : 1u << (pipe->Interval - 1);
    if (req->Interval != expectedInterval) { status = STATUS_INVALID_PARAMETER; goto Unlock; }
    for (ULONG i = 0; i < n; ++i) {
        if (packets[i].length > pipe->MaxPacketSize) { status = STATUS_INVALID_PARAMETER; goto Unlock; }
    }
    status = USBD_IsochUrbAllocate(Ext->UsbdHandle, n, &urb);
    if (!NT_SUCCESS(status)) goto Unlock;
    ctx = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ctx) + (n - 1) * sizeof(ULONG), FILTER_TAG);
    if (!ctx) {
        USBD_UrbFree(Ext->UsbdHandle, urb);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Unlock;
    }
    ctx->Urb = urb;
    ctx->Iso = TRUE;
    ctx->NumberOfPackets = n;
    ctx->DirectionIn = req->Direction != 0;
    ctx->InterfaceNumber = pipe->InterfaceNumber;
    ctx->EndpointIndex = req->Endpoint + (req->Direction ? 16 : 0);
    ctx->TransferLength = req->TransferLength;
    urb->UrbHeader.Function = URB_FUNCTION_ISOCH_TRANSFER;
    urb->UrbHeader.Length = (USHORT)GET_ISO_URB_SIZE(n);
    urb->UrbIsochronousTransfer.PipeHandle = (USBD_PIPE_HANDLE)(ULONG_PTR)pipe->PipeHandle;
    urb->UrbIsochronousTransfer.TransferFlags = USBD_START_ISO_TRANSFER_ASAP |
        (req->Direction ? USBD_TRANSFER_DIRECTION_IN : USBD_TRANSFER_DIRECTION_OUT);
    urb->UrbIsochronousTransfer.TransferBuffer = (PVOID)(ULONG_PTR)req->KernelVa;
    urb->UrbIsochronousTransfer.TransferBufferMDL = NULL;
    urb->UrbIsochronousTransfer.TransferBufferLength = req->TransferLength;
    urb->UrbIsochronousTransfer.NumberOfPackets = n;
    urb->UrbIsochronousTransfer.StartFrame = 0;
    urb->UrbIsochronousTransfer.ErrorCount = 0;
    urb->UrbIsochronousTransfer.UrbLink = NULL;
    for (ULONG i = 0; i < n; ++i) {
        urb->UrbIsochronousTransfer.IsoPacket[i].Offset = packets[i].offset;
        urb->UrbIsochronousTransfer.IsoPacket[i].Length = 0;
        urb->UrbIsochronousTransfer.IsoPacket[i].Status = USBD_STATUS_NOT_ACCESSED;
        ctx->PacketLengths[i] = packets[i].length;
    }
    return ForwardUrb(Ext, Request, ctx, Forwarded);
Unlock:
    KeReleaseMutex(&Ext->ConfigMutex, FALSE);
    return status;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  GET_DESCRIPTOR handler
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
HandleGetDescriptor(PFILTER_EXT Ext, PIRP Irp)
{
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    PVOID inBuf  = Irp->AssociatedIrp.SystemBuffer;
    ULONG inLen  = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status;

    if (inLen < sizeof(USBWARP_FILTER_GET_DESC_REQUEST))
        return STATUS_BUFFER_TOO_SMALL;

    USBWARP_FILTER_GET_DESC_REQUEST *req =
        (USBWARP_FILTER_GET_DESC_REQUEST *)inBuf;

    ULONG descLen = min(req->BufferLength, outLen);
    if (descLen == 0) return STATUS_BUFFER_TOO_SMALL;

    PVOID descBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, descLen, FILTER_TAG);
    if (!descBuf) return STATUS_INSUFFICIENT_RESOURCES;

    /* Build and submit GET_DESCRIPTOR URB synchronously. */
    PURB urb = NULL;
    status = USBD_UrbAllocate(Ext->UsbdHandle, &urb);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(descBuf, FILTER_TAG);
        return status;
    }

    UsbBuildGetDescriptorRequest(
        urb, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
        req->DescriptorType, req->Index, req->LanguageId,
        descBuf, NULL, descLen, NULL);

    status = SubmitSyncUrb(Ext, urb);

    if (NT_SUCCESS(status)) {
        ULONG actual = urb->UrbControlDescriptorRequest.TransferBufferLength;
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, descBuf,
                       min(actual, outLen));
        Irp->IoStatus.Information = min(actual, outLen);
    }

    USBD_UrbFree(Ext->UsbdHandle, urb);
    ExFreePoolWithTag(descBuf, FILTER_TAG);
    return status;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  SELECT_CONFIG handler
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Synchronous configuration requests run on the host poll thread. A timeout
 * cancels USB I/O, then waits for ownership to return before freeing the URB. */
static NTSTATUS SubmitSyncUrb(PFILTER_EXT Ext, PURB Urb)
{
    WDFREQUEST request;
    WDF_REQUEST_SEND_OPTIONS options;
    IO_STACK_LOCATION stack = {0};
    WDFIOTARGET target = WdfDeviceGetIoTarget(Ext->Device);
    NTSTATUS status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, target, &request);
    if (!NT_SUCCESS(status)) return status;
    stack.MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack.Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack.Parameters.Others.Argument1 = Urb;
    WdfRequestWdmFormatUsingStackLocation(request, &stack);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(10));
    WdfRequestSend(request, target, &options);
    status = WdfRequestGetStatus(request);
    if (NT_SUCCESS(status) && !USBD_SUCCESS(Urb->UrbHeader.Status))
        status = STATUS_DEVICE_DATA_ERROR;
    WdfObjectDelete(request);
    return status;
}

static NTSTATUS ReadDescriptor(PFILTER_EXT Ext, UCHAR Type, UCHAR Index, PVOID Buffer, ULONG Length)
{
    PURB urb;
    NTSTATUS status = USBD_UrbAllocate(Ext->UsbdHandle, &urb);
    if (!NT_SUCCESS(status)) return status;
    UsbBuildGetDescriptorRequest(urb, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
        Type, Index, 0, Buffer, NULL, Length, NULL);
    status = SubmitSyncUrb(Ext, urb);
    if (NT_SUCCESS(status) && urb->UrbControlDescriptorRequest.TransferBufferLength < Length)
        status = STATUS_DEVICE_DATA_ERROR;
    USBD_UrbFree(Ext->UsbdHandle, urb);
    return status;
}

static VOID ForgetStreams(PFILTER_EXT Ext, int InterfaceNumber)
{
    /* A successful SELECT_CONFIG/INTERFACE or device removal invalidates the
     * old handles in the USB stack. Only their local bookkeeping remains. */
    for (ULONG i = 0; i < 32; ++i) {
        if (Ext->Streams[i].Info &&
            (InterfaceNumber < 0 || Ext->Streams[i].InterfaceNumber == InterfaceNumber)) {
            ExFreePoolWithTag(Ext->Streams[i].Info, FILTER_TAG);
            RtlZeroMemory(&Ext->Streams[i], sizeof(Ext->Streams[i]));
        }
    }
}

static ULONG EndpointStreamLimit(PFILTER_EXT Ext, const USBWARP_FILTER_PIPE_INFO *Pipe)
{
    PUSB_INTERFACE_DESCRIPTOR iface;
    PUCHAR end, p;
    if (!Ext->Configuration) return 0;
    iface = USBD_ParseConfigurationDescriptorEx(Ext->Configuration, Ext->Configuration,
        Pipe->InterfaceNumber, Pipe->AlternateSetting, -1, -1, -1);
    if (!iface) return 0;
    end = (PUCHAR)Ext->Configuration + Ext->Configuration->wTotalLength;
    p = (PUCHAR)iface + iface->bLength;
    while (p + 2 <= end && p[0] >= 2 && p[0] <= end - p) {
        if (p[1] == USB_INTERFACE_DESCRIPTOR_TYPE) break;
        if (p[1] == USB_ENDPOINT_DESCRIPTOR_TYPE && p[0] >= 7 && p[2] == Pipe->EndpointAddress) {
            PUCHAR companion = p + p[0];
            if (companion + 6 <= end && companion[0] >= 6 && companion[1] == 0x30) {
                ULONG exponent = companion[3] & 31;
                if (exponent && exponent <= 16)
                    return (1u << exponent) - 1; /* stream 0 is reserved */
            }
            return 0;
        }
        p += p[0];
    }
    return 0;
}

static NTSTATUS CloseStreams(PFILTER_EXT Ext, ULONG Index)
{
    PURB urb;
    NTSTATUS status;
    if (!Ext->Streams[Index].Count) return STATUS_SUCCESS;
    status = USBD_UrbAllocate(Ext->UsbdHandle, &urb);
    if (!NT_SUCCESS(status)) return status;
    urb->UrbHeader.Function = URB_FUNCTION_CLOSE_STATIC_STREAMS;
    urb->UrbHeader.Length = sizeof(struct _URB_PIPE_REQUEST);
    urb->UrbPipeRequest.PipeHandle = Ext->Streams[Index].PipeHandle;
    status = SubmitSyncUrb(Ext, urb);
    USBD_UrbFree(Ext->UsbdHandle, urb);
    if (NT_SUCCESS(status)) {
        ExFreePoolWithTag(Ext->Streams[Index].Info, FILTER_TAG);
        RtlZeroMemory(&Ext->Streams[Index], sizeof(Ext->Streams[Index]));
    }
    return status;
}

static NTSTATUS HandleStreams(PFILTER_EXT Ext, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    USBWARP_FILTER_STREAMS_REQUEST req;
    USBWARP_FILTER_PIPE_INFO *pipes[32] = { 0 };
    ULONG count, opened = 0;
    NTSTATUS status = STATUS_SUCCESS;
    if (sp->Parameters.DeviceIoControl.InputBufferLength != sizeof(req) ||
        sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(USBWARP_FILTER_STREAMS_RESPONSE))
        return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(&req, Irp->AssociatedIrp.SystemBuffer, sizeof(req));
    if (!usbwarp_stream_request_valid(req.EndpointMask, req.NumberOfStreams))
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&Ext->ConfigMutex, Executive, KernelMode, FALSE, NULL);
    count = min(req.NumberOfStreams, Ext->MaxStreams);
    if (req.NumberOfStreams && !count) { status = STATUS_NOT_SUPPORTED; goto Done; }
    /* Validate every endpoint before mutating any pipe. */
    for (ULONG i = 0; i < 32; ++i) {
        if (!(req.EndpointMask & (1u << i))) continue;
        if (!req.NumberOfStreams && !Ext->Streams[i].Count) continue;
        pipes[i] = LookupPipe(Ext, i & 15, i >> 4, UsbdPipeTypeBulk);
        if (!pipes[i]) { status = STATUS_INVALID_PARAMETER; goto Done; }
        if (InterlockedCompareExchange(&Ext->ActiveByPipe[i], 0, 0)) {
            status = STATUS_DEVICE_BUSY; goto Done;
        }
        if (req.NumberOfStreams) {
            if (Ext->Streams[i].Count) { status = STATUS_INVALID_DEVICE_STATE; goto Done; }
            count = min(count, EndpointStreamLimit(Ext, pipes[i]));
        }
    }
    if (req.NumberOfStreams && !count) { status = STATUS_NOT_SUPPORTED; goto Done; }
    for (ULONG i = 0; i < 32; ++i) {
        PURB urb;
        PUSBD_STREAM_INFORMATION info;
        ULONGLONG seen = 0;
        if (!pipes[i]) continue;
        if (!req.NumberOfStreams) {
            NTSTATUS closeStatus = CloseStreams(Ext, i);
            if (!NT_SUCCESS(closeStatus)) status = closeStatus;
            continue;
        }
        info = ExAllocatePool2(POOL_FLAG_NON_PAGED, count * sizeof(*info), FILTER_TAG);
        if (!info) { status = STATUS_INSUFFICIENT_RESOURCES; goto Rollback; }
        status = USBD_UrbAllocate(Ext->UsbdHandle, &urb);
        if (!NT_SUCCESS(status)) { ExFreePoolWithTag(info, FILTER_TAG); goto Rollback; }
        UsbBuildOpenStaticStreamsRequest(urb, (USBD_PIPE_HANDLE)(ULONG_PTR)pipes[i]->PipeHandle,
                                        (USHORT)count, info);
        status = SubmitSyncUrb(Ext, urb);
        USBD_UrbFree(Ext->UsbdHandle, urb);
        if (!NT_SUCCESS(status)) { ExFreePoolWithTag(info, FILTER_TAG); goto Rollback; }
        Ext->Streams[i].Count = count;
        Ext->Streams[i].PipeHandle = (USBD_PIPE_HANDLE)(ULONG_PTR)pipes[i]->PipeHandle;
        Ext->Streams[i].InterfaceNumber = pipes[i]->InterfaceNumber;
        Ext->Streams[i].Info = info;
        opened |= 1u << i;
        for (ULONG n = 0; n < count; ++n) {
            if (!info[n].StreamID || info[n].StreamID > count || !info[n].PipeHandle ||
                (seen & (1ULL << info[n].StreamID))) { status = STATUS_DEVICE_DATA_ERROR; goto Rollback; }
            seen |= 1ULL << info[n].StreamID;
        }
    }
    if (NT_SUCCESS(status)) {
        ((USBWARP_FILTER_STREAMS_RESPONSE *)Irp->AssociatedIrp.SystemBuffer)->NumberOfStreams = count;
        Irp->IoStatus.Information = sizeof(USBWARP_FILTER_STREAMS_RESPONSE);
    }
    goto Done;
Rollback:
    for (ULONG i = 0; i < 32; ++i)
        if (opened & (1u << i)) CloseStreams(Ext, i);
Done:
    KeReleaseMutex(&Ext->ConfigMutex, FALSE);
    return status;
}

static NTSTATUS HandleResetPipe(PFILTER_EXT Ext, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG endpoint, index;
    USBWARP_FILTER_PIPE_INFO *pipe = NULL;
    PURB urb;
    NTSTATUS status;
    if (sp->Parameters.DeviceIoControl.InputBufferLength != sizeof(endpoint))
        return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(&endpoint, Irp->AssociatedIrp.SystemBuffer, sizeof(endpoint));
    if (!usbwarp_endpoint_mask(endpoint)) return STATUS_INVALID_PARAMETER;
    index = (endpoint & 15) + ((endpoint & 0x80) ? 16 : 0);
    KeWaitForSingleObject(&Ext->ConfigMutex, Executive, KernelMode, FALSE, NULL);
    for (ULONG i = 0; i < Ext->CachedPipes.NumPipes; ++i)
        if (Ext->CachedPipes.Pipes[i].EndpointAddress == endpoint)
            pipe = &Ext->CachedPipes.Pipes[i];
    if (!pipe || pipe->PipeType == UsbdPipeTypeIsochronous) { status = STATUS_INVALID_PARAMETER; goto Done; }
    status = USBD_UrbAllocate(Ext->UsbdHandle, &urb);
    if (!NT_SUCCESS(status)) goto Done;
    urb->UrbHeader.Length = sizeof(struct _URB_PIPE_REQUEST);
    urb->UrbPipeRequest.PipeHandle = (USBD_PIPE_HANDLE)(ULONG_PTR)pipe->PipeHandle;
    if (InterlockedCompareExchange(&Ext->ActiveByPipe[index], 0, 0)) {
        urb->UrbHeader.Function = URB_FUNCTION_ABORT_PIPE;
        status = SubmitSyncUrb(Ext, urb);
    }
    if (NT_SUCCESS(status)) {
        urb->UrbHeader.Function = URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL;
        status = SubmitSyncUrb(Ext, urb);
    }
    USBD_UrbFree(Ext->UsbdHandle, urb);
Done:
    KeReleaseMutex(&Ext->ConfigMutex, FALSE);
    return status;
}

static VOID CacheInterface(USBWARP_FILTER_PIPES_RESPONSE *Map, PUSBD_INTERFACE_INFORMATION Interface)
{
    for (ULONG i = 0; i < Interface->NumberOfPipes && Map->NumPipes < USBWARP_FILTER_MAX_PIPES; ++i) {
        PUSBD_PIPE_INFORMATION p = &Interface->Pipes[i];
        USBWARP_FILTER_PIPE_INFO *out = &Map->Pipes[Map->NumPipes++];
        out->EndpointAddress = p->EndpointAddress;
        out->PipeType = (UCHAR)p->PipeType;
        out->MaxPacketSize = p->MaximumPacketSize;
        out->PipeHandle = (uint64_t)(ULONG_PTR)p->PipeHandle;
        out->InterfaceNumber = Interface->InterfaceNumber;
        out->AlternateSetting = Interface->AlternateSetting;
        out->Interval = p->Interval;
    }
}

static NTSTATUS HandleSelectConfig(PFILTER_EXT Ext, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    USBWARP_FILTER_SELECT_CONFIG_REQUEST req;
    USB_CONFIGURATION_DESCRIPTOR hdr = { 0 };
    USB_DEVICE_DESCRIPTOR device;
    PUSB_CONFIGURATION_DESCRIPTOR cfg = NULL;
    PUSBD_INTERFACE_LIST_ENTRY interfaces = NULL;
    PURB urb = NULL;
    ULONG index, count = 0, endpoints = 0, off;
    NTSTATUS status;
    if (sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(req) ||
        sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(Ext->CachedPipes))
        return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(&req, Irp->AssociatedIrp.SystemBuffer, sizeof(req));
    if (req.ConfigIndex > 255 || req.ConfigurationValue > 255 || req.ByValue > 1)
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&Ext->ConfigMutex, Executive, KernelMode, FALSE, NULL);
    if (InterlockedCompareExchange(&Ext->ActiveTransfers, 0, 0)) {
        status = STATUS_DEVICE_BUSY;
        goto Done;
    }
    if (req.ByValue && req.ConfigurationValue == 0) {
        status = USBD_UrbAllocate(Ext->UsbdHandle, &urb);
        if (!NT_SUCCESS(status)) goto Done;
        urb->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;
        urb->UrbHeader.Length = sizeof(struct _URB_SELECT_CONFIGURATION);
        urb->UrbSelectConfiguration.ConfigurationDescriptor = NULL;
        status = SubmitSyncUrb(Ext, urb);
        if (NT_SUCCESS(status)) {
            ForgetStreams(Ext, -1);
            Ext->ConfigSelected = FALSE;
            Ext->ConfigurationHandle = NULL;
            if (Ext->Configuration) ExFreePoolWithTag(Ext->Configuration, FILTER_TAG);
            Ext->Configuration = NULL;
            RtlZeroMemory(&Ext->CachedPipes, sizeof(Ext->CachedPipes));
        }
        goto Reply;
    }
    status = ReadDescriptor(Ext, USB_DEVICE_DESCRIPTOR_TYPE, 0, &device, sizeof(device));
    if (!NT_SUCCESS(status)) goto Done;
    for (index = 0; index < device.bNumConfigurations; ++index) {
        if (!req.ByValue && index != req.ConfigIndex) continue;
        status = ReadDescriptor(Ext, USB_CONFIGURATION_DESCRIPTOR_TYPE, (UCHAR)index, &hdr, sizeof(hdr));
        if (!NT_SUCCESS(status)) goto Done;
        if (!req.ByValue || hdr.bConfigurationValue == req.ConfigurationValue) break;
    }
    if (index == device.bNumConfigurations || hdr.wTotalLength < sizeof(hdr)) {
        status = STATUS_INVALID_PARAMETER;
        goto Done;
    }
    cfg = ExAllocatePool2(POOL_FLAG_NON_PAGED, hdr.wTotalLength, FILTER_TAG);
    if (!cfg) { status = STATUS_INSUFFICIENT_RESOURCES; goto Done; }
    status = ReadDescriptor(Ext, USB_CONFIGURATION_DESCRIPTOR_TYPE, (UCHAR)index, cfg, hdr.wTotalLength);
    if (!NT_SUCCESS(status)) goto Done;
    if (cfg->wTotalLength != hdr.wTotalLength || cfg->bLength < sizeof(*cfg) ||
        cfg->bLength > cfg->wTotalLength || cfg->bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE) {
        status = STATUS_DEVICE_DATA_ERROR; goto Done;
    }
    interfaces = ExAllocatePool2(POOL_FLAG_NON_PAGED,
        (cfg->bNumInterfaces + 1) * sizeof(*interfaces), FILTER_TAG);
    if (!interfaces) { status = STATUS_INSUFFICIENT_RESOURCES; goto Done; }
    /* Interface numbers need not be contiguous. Validate every descriptor
     * before handing pointers to USBD's configuration builder. */
    for (off = cfg->bLength; off < cfg->wTotalLength;) {
        PUSB_COMMON_DESCRIPTOR d = (void *)((PUCHAR)cfg + off);
        if (cfg->wTotalLength - off < sizeof(*d) || d->bLength < sizeof(*d) ||
            d->bLength > cfg->wTotalLength - off) { status = STATUS_DEVICE_DATA_ERROR; goto Done; }
        if (d->bDescriptorType == USB_ENDPOINT_DESCRIPTOR_TYPE && d->bLength < sizeof(USB_ENDPOINT_DESCRIPTOR)) {
            status = STATUS_DEVICE_DATA_ERROR; goto Done;
        }
        if (d->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE) {
            PUSB_INTERFACE_DESCRIPTOR intf = (void *)d;
            if (d->bLength < sizeof(*intf)) { status = STATUS_DEVICE_DATA_ERROR; goto Done; }
            if (!intf->bAlternateSetting) {
                if (count >= cfg->bNumInterfaces) { status = STATUS_DEVICE_DATA_ERROR; goto Done; }
                for (ULONG j = 0; j < count; ++j)
                    if (interfaces[j].InterfaceDescriptor->bInterfaceNumber == intf->bInterfaceNumber) {
                        status = STATUS_DEVICE_DATA_ERROR; goto Done;
                    }
                interfaces[count++].InterfaceDescriptor = intf;
                endpoints += intf->bNumEndpoints;
            }
        }
        off += d->bLength;
    }
    if (count != cfg->bNumInterfaces || endpoints > USBWARP_FILTER_MAX_PIPES) {
        status = STATUS_DEVICE_DATA_ERROR; goto Done;
    }
    status = USBD_SelectConfigUrbAllocateAndBuild(Ext->UsbdHandle, cfg, interfaces, &urb);
    if (!NT_SUCCESS(status)) goto Done;
    status = SubmitSyncUrb(Ext, urb);
    if (NT_SUCCESS(status)) {
        ForgetStreams(Ext, -1);
        RtlZeroMemory(&Ext->CachedPipes, sizeof(Ext->CachedPipes));
        for (ULONG i = 0; i < count; ++i)
            CacheInterface(&Ext->CachedPipes, interfaces[i].Interface);
        Ext->ConfigurationHandle = urb->UrbSelectConfiguration.ConfigurationHandle;
        if (Ext->Configuration) ExFreePoolWithTag(Ext->Configuration, FILTER_TAG);
        Ext->Configuration = cfg;
        cfg = NULL;
        Ext->ConfigSelected = TRUE;
    }
Reply:
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &Ext->CachedPipes, sizeof(Ext->CachedPipes));
        Irp->IoStatus.Information = sizeof(Ext->CachedPipes);
    }
Done:
    if (urb) USBD_UrbFree(Ext->UsbdHandle, urb);
    if (interfaces) ExFreePoolWithTag(interfaces, FILTER_TAG);
    if (cfg) ExFreePoolWithTag(cfg, FILTER_TAG);
    KeReleaseMutex(&Ext->ConfigMutex, FALSE);
    return status;
}

static NTSTATUS HandleSelectInterface(PFILTER_EXT Ext, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    USBWARP_FILTER_SELECT_INTERFACE_REQUEST req;
    USBWARP_FILTER_PIPES_RESPONSE map = { 0 };
    USBD_INTERFACE_LIST_ENTRY entry = { 0 };
    PURB urb = NULL;
    NTSTATUS status;
    if (sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(req) ||
        sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(map))
        return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(&req, Irp->AssociatedIrp.SystemBuffer, sizeof(req));
    if (req.InterfaceNumber > 255 || req.AlternateSetting > 255)
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&Ext->ConfigMutex, Executive, KernelMode, FALSE, NULL);
    if (!Ext->Configuration || !Ext->ConfigSelected) { status = STATUS_DEVICE_NOT_READY; goto Done; }
    if (InterlockedCompareExchange(&Ext->ActiveByInterface[req.InterfaceNumber], 0, 0)) {
        status = STATUS_DEVICE_BUSY; goto Done;
    }
    entry.InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(Ext->Configuration,
        Ext->Configuration, req.InterfaceNumber, req.AlternateSetting, -1, -1, -1);
    if (!entry.InterfaceDescriptor) { status = STATUS_INVALID_PARAMETER; goto Done; }
    for (ULONG i = 0; i < Ext->CachedPipes.NumPipes; ++i)
        if (Ext->CachedPipes.Pipes[i].InterfaceNumber != req.InterfaceNumber)
            map.Pipes[map.NumPipes++] = Ext->CachedPipes.Pipes[i];
    if (map.NumPipes + entry.InterfaceDescriptor->bNumEndpoints > USBWARP_FILTER_MAX_PIPES) {
        status = STATUS_INSUFFICIENT_RESOURCES; goto Done;
    }
    status = USBD_SelectInterfaceUrbAllocateAndBuild(Ext->UsbdHandle,
        Ext->ConfigurationHandle, &entry, &urb);
    if (!NT_SUCCESS(status)) goto Done;
    status = SubmitSyncUrb(Ext, urb);
    if (NT_SUCCESS(status)) {
        ForgetStreams(Ext, (int)req.InterfaceNumber);
        CacheInterface(&map, &urb->UrbSelectInterface.Interface);
        Ext->CachedPipes = map;
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &map, sizeof(map));
        Irp->IoStatus.Information = sizeof(map);
    }
Done:
    if (urb) USBD_UrbFree(Ext->UsbdHandle, urb);
    KeReleaseMutex(&Ext->ConfigMutex, FALSE);
    return status;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  IRP_MJ_DEVICE_CONTROL — custom IOCTL handler
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID CaptureIoctl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength,
                  size_t InLength, ULONG Control)
{
    PFILTER_EXT ext = CaptureGetContext(WdfIoQueueGetDevice(Queue));
    PIRP irp = WdfRequestWdmGetIrp(Request);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    BOOLEAN forwarded = FALSE;
    irp->IoStatus.Information = 0;
    if (WdfRequestGetRequestorMode(Request) != KernelMode) {
        status = STATUS_ACCESS_DENIED;
        goto Complete;
    }
    KeWaitForSingleObject(&ext->ConfigMutex, Executive, KernelMode, FALSE, NULL);
    if (!ext->Started || ext->Removed || ext->Retired || !ext->UsbdHandle)
        status = STATUS_DEVICE_NOT_CONNECTED;
    else if (Control == IOCTL_USBWARP_FILTER_CLAIM) {
        USBWARP_FILTER_CLAIM *claim = irp->AssociatedIrp.SystemBuffer;
        if (InLength != sizeof(*claim) || OutLength || !claim ||
            claim->Version != USBWARP_CAPTURE_VERSION || claim->Reserved)
            status = STATUS_REVISION_MISMATCH;
        else if (!WdfRequestGetFileObject(Request)) status = STATUS_INVALID_HANDLE;
        else if (ext->Owner) status = STATUS_SHARING_VIOLATION;
        else {
            ext->Owner = WdfRequestGetFileObject(Request);
            status = STATUS_SUCCESS;
        }
    } else if (!ext->Owner || ext->Owner != WdfRequestGetFileObject(Request))
        status = STATUS_ACCESS_DENIED;
    else status = STATUS_SUCCESS;
    KeReleaseMutex(&ext->ConfigMutex, FALSE);
    if (!NT_SUCCESS(status) || Control == IOCTL_USBWARP_FILTER_CLAIM) goto Complete;
    switch (Control) {
    case IOCTL_USBWARP_FILTER_SUBMIT_URB:
        status = HandleSubmitUrb(ext, Request, &forwarded);
        if (forwarded) return;
        break;
    case IOCTL_USBWARP_FILTER_SUBMIT_ISO:
        status = HandleSubmitIso(ext, Request, &forwarded);
        if (forwarded) return;
        break;
    case IOCTL_USBWARP_FILTER_RESET_PIPE: status = HandleResetPipe(ext, irp); break;
    case IOCTL_USBWARP_FILTER_STREAMS: status = HandleStreams(ext, irp); break;
    case IOCTL_USBWARP_FILTER_GET_DESCRIPTOR: status = HandleGetDescriptor(ext, irp); break;
    case IOCTL_USBWARP_FILTER_SELECT_CONFIG: status = HandleSelectConfig(ext, irp); break;
    case IOCTL_USBWARP_FILTER_SELECT_INTERFACE: status = HandleSelectInterface(ext, irp); break;
    case IOCTL_USBWARP_FILTER_GET_PIPES:
        if (OutLength < sizeof(ext->CachedPipes)) status = STATUS_BUFFER_TOO_SMALL;
        else {
            KeWaitForSingleObject(&ext->ConfigMutex, Executive, KernelMode, FALSE, NULL);
            RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &ext->CachedPipes, sizeof(ext->CachedPipes));
            irp->IoStatus.Information = sizeof(ext->CachedPipes);
            KeReleaseMutex(&ext->ConfigMutex, FALSE);
            status = STATUS_SUCCESS;
        }
        break;
    default: status = STATUS_INVALID_DEVICE_REQUEST; break;
    }
Complete:
    WdfRequestCompleteWithInformation(Request, status, irp->IoStatus.Information);
}

VOID CaptureIoStop(WDFQUEUE Queue, WDFREQUEST Request, ULONG Flags)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Flags);
    /* Completion owns all buffers and the final completion. Power removal
     * waits for it instead of freeing URBs still owned by the USB stack. */
    WdfRequestCancelSentRequest(Request);
}

VOID CaptureCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT File)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(File);
    WdfRequestComplete(Request, WdfRequestGetRequestorMode(Request) == KernelMode ?
        STATUS_SUCCESS : STATUS_ACCESS_DENIED);
}

VOID CaptureCleanup(WDFFILEOBJECT File)
{
    PFILTER_EXT ext = CaptureGetContext(WdfFileObjectGetDevice(File));
    BOOLEAN owner;
    KeWaitForSingleObject(&ext->ConfigMutex, Executive, KernelMode, FALSE, NULL);
    owner = ext->Owner == File;
    if (owner) { ext->Owner = NULL; ext->Retired = TRUE; }
    KeReleaseMutex(&ext->ConfigMutex, FALSE);
    /* A probe handle never changes ownership. An actual lease cannot be
     * reused after close, including service death and a cancelled PnP remove. */
    if (owner) {
        /* Stop sends first, including a dispatch racing the lease check.
         * Purging a queue alone cannot cancel unmarked forwarded requests. */
        WdfIoTargetPurge(WdfDeviceGetIoTarget(ext->Device), WdfIoTargetPurgeIoAndWait);
        WdfIoQueuePurgeSynchronously(ext->Queue);
    }
}

NTSTATUS CapturePrepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PFILTER_EXT ext = CaptureGetContext(Device);
    ULONG bytes = 0;
    USHORT streams = 0;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    status = USBD_CreateHandle(ext->Self, ext->LowerDevice,
        USBD_CLIENT_CONTRACT_VERSION_602, FILTER_TAG, &ext->UsbdHandle);
    if (!NT_SUCCESS(status)) return status;
    if (NT_SUCCESS(USBD_QueryUsbCapability(ext->UsbdHandle,
        &GUID_USB_CAPABILITY_STATIC_STREAMS, sizeof(streams), (PUCHAR)&streams, &bytes)))
        ext->MaxStreams = (USHORT)min(streams, USBWARP_MAX_STREAMS);
    ext->Removed = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS CaptureRelease(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PFILTER_EXT ext = CaptureGetContext(Device);
    UNREFERENCED_PARAMETER(Translated);
    ext->Removed = TRUE;
    ext->Started = FALSE;
    NT_ASSERT(ext->ActiveTransfers == 0);
    ForgetStreams(ext, -1);
    if (ext->UsbdHandle) USBD_CloseHandle(ext->UsbdHandle);
    ext->UsbdHandle = NULL;
    if (ext->Configuration) ExFreePoolWithTag(ext->Configuration, FILTER_TAG);
    ext->Configuration = NULL;
    ext->ConfigurationHandle = NULL;
    ext->ConfigSelected = FALSE;
    RtlZeroMemory(&ext->CachedPipes, sizeof(ext->CachedPipes));
    return STATUS_SUCCESS;
}

NTSTATUS CaptureD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    PFILTER_EXT ext = CaptureGetContext(Device);
    UNREFERENCED_PARAMETER(Previous);
    ext->Started = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS CaptureD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    PFILTER_EXT ext = CaptureGetContext(Device);
    UNREFERENCED_PARAMETER(Target);
    ext->Started = FALSE;
    /* KMDF has stopped/cancelled the queue before D0 exit. USB client/pipe
     * lifetime spans PrepareHardware -> ReleaseHardware; an ordinary power
     * transition preserves the file lease. A port cycle creates a new PDO. */
    return STATUS_SUCCESS;
}

NTSTATUS CaptureAddDevice(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_FILEOBJECT_CONFIG files;
    WDF_IO_QUEUE_CONFIG queue;
    WDFDEVICE device;
    PFILTER_EXT ext;
    NTSTATUS status;
    WCHAR service[64]; ULONG bytes; DEVPROPTYPE type;
    UNREFERENCED_PARAMETER(Driver);
    /* Fail closed if an obsolete global UpperFilters registration remains.
     * Only our dedicated function-driver installation may create a stack. */
    status = IoGetDevicePropertyData(WdfFdoInitWdmGetPhysicalDevice(Init),
        &DEVPKEY_Device_Service, LOCALE_NEUTRAL, 0, sizeof(service), service, &bytes, &type);
    if (!NT_SUCCESS(status) || type != DEVPROP_TYPE_STRING ||
        bytes > sizeof(service) || bytes < sizeof(WCHAR) || service[bytes / sizeof(WCHAR) - 1] ||
        _wcsicmp(service, L"UsbWarpFilter")) return STATUS_SUCCESS;

    WdfDeviceInitSetDeviceType(Init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetIoType(Init, WdfDeviceIoBuffered);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = CapturePrepare;
    pnp.EvtDeviceReleaseHardware = CaptureRelease;
    pnp.EvtDeviceD0Entry = CaptureD0Entry;
    pnp.EvtDeviceD0Exit = CaptureD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    WDF_FILEOBJECT_CONFIG_INIT(&files, CaptureCreate, WDF_NO_EVENT_CALLBACK, CaptureCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    attrs.SynchronizationScope = WdfSynchronizationScopeNone;
    WdfDeviceInitSetFileObjectConfig(Init, &files, &attrs);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, FILTER_EXT);
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    attrs.SynchronizationScope = WdfSynchronizationScopeNone;
    status = WdfDeviceCreate(&Init, &attrs, &device);
    if (!NT_SUCCESS(status)) return status;
    ext = CaptureGetContext(device);
    ext->Device = device;
    ext->Self = WdfDeviceWdmGetDeviceObject(device);
    ext->Pdo = WdfDeviceWdmGetPhysicalDevice(device);
    ext->LowerDevice = WdfDeviceWdmGetAttachedDevice(device);
    KeInitializeMutex(&ext->ConfigMutex, 0);
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchParallel);
    queue.EvtIoDeviceControl = CaptureIoctl;
    queue.EvtIoStop = CaptureIoStop;
    queue.PowerManaged = WdfTrue;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, &ext->Queue);
    if (!NT_SUCCESS(status)) return status;
    return WdfDeviceCreateDeviceInterface(device, &GUID_USBWARP_FILTER_INTERFACE, NULL);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, CaptureAddDevice);
    return WdfDriverCreate(Driver, Path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
