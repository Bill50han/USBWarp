/*
 * usbwarp_filter.c — UsbWarpFilter.sys USB class upper filter driver.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * PURPOSE:
 *   This driver sits in the USB device stack (between the function driver
 *   and the hub PDO) and provides a legitimate URB submission path for
 *   UsbWarp.sys.  UsbWarp.sys cannot directly send IOCTL_INTERNAL_USB_-
 *   SUBMIT_URB because it is not in any USB device stack.  UsbHub3.sys
 *   requires URB senders to be registered via USBD_CreateHandle, which
 *   only works for devices IN the stack.
 *
 * ARCHITECTURE:
 *   UsbWarpFilter.sys is installed as a USB class upper filter via:
 *     HKLM\System\CCS\Control\Class\{36FC9E60-...}\UpperFilters += UsbWarpFilter
 *
 *   For each USB device:
 *     1. AddDevice: create FiDO, attach to device stack
 *     2. IRP_MN_START_DEVICE: USBD_CreateHandle, register device interface
 *     3. Custom IOCTLs from UsbWarp.sys:
 *        - GET_DESCRIPTOR:   proxy descriptor reads
 *        - SELECT_CONFIG:    select configuration, return pipe map
 *        - SUBMIT_URB:       proxy URB submissions (async)
 *        - ABORT_PIPE:       abort a pipe
 *     4. IRP_MN_REMOVE_DEVICE: USBD_CloseHandle, detach, delete
 *
 *   Normal USB traffic (from function driver above) passes through
 *   transparently — the filter only intercepts our custom IOCTLs.
 */

#include <ntddk.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <ntstrsafe.h>
#include <wdmsec.h>

/* Include shared IOCTL header — needs INITGUID for DEFINE_GUID. */
#include <initguid.h>
#include "../include/usbwarp_filter_ioctl.h"

#define FILTER_TAG  'tFwU'   /* UwFt */

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Filter device extension
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _FILTER_EXT {
    PDEVICE_OBJECT  Self;          /* our FiDO                           */
    PDEVICE_OBJECT  LowerDevice;   /* device below us (FDO or PDO)       */
    PDEVICE_OBJECT  Pdo;           /* physical device object (bottom)     */
    USBD_HANDLE     UsbdHandle;    /* registered USB client handle        */
    BOOLEAN         Started;       /* IRP_MN_START_DEVICE completed       */
    BOOLEAN         Removed;       /* IRP_MN_REMOVE_DEVICE received       */
    BOOLEAN         ConfigSelected;/* SELECT_CONFIG done, pipes cached    */
    IO_REMOVE_LOCK  RemoveLock;
    UNICODE_STRING  InterfaceLink; /* symbolic link from IoRegisterDev... */

    /* Cached pipe map from last SELECT_CONFIGURATION. */
    USBWARP_FILTER_PIPES_RESPONSE CachedPipes;
} FILTER_EXT, *PFILTER_EXT;

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Forward declarations
 * ═══════════════════════════════════════════════════════════════════════════ */

DRIVER_ADD_DEVICE          FilterAddDevice;
__drv_dispatchType(IRP_MJ_PNP)
DRIVER_DISPATCH            FilterPnp;
__drv_dispatchType(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH            FilterDeviceControl;
__drv_dispatchType(IRP_MJ_INTERNAL_DEVICE_CONTROL)
DRIVER_DISPATCH            FilterInternalDeviceControl;
__drv_dispatchType(IRP_MJ_CREATE)
__drv_dispatchType(IRP_MJ_CLOSE)
DRIVER_DISPATCH            FilterCreateClose;
DRIVER_DISPATCH            FilterPassThrough;
DRIVER_UNLOAD              FilterUnload;

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
SyncCompletionRoutine(PDEVICE_OBJECT DevObj, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DevObj);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
ForwardIrpSync(PFILTER_EXT Ext, PIRP Irp)
{
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, SyncCompletionRoutine, &event,
                           TRUE, TRUE, TRUE);

    NTSTATUS status = IoCallDriver(Ext->LowerDevice, Irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }
    return status;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Submit URB via USBD — the core proxy function
 *
 *   Allocates a URB using USBD_UrbAllocate (which embeds our USBD_HANDLE
 *   reference in the URB), fills it, creates a new IRP, sends it to the
 *   lower device.  The hub PDO finds our client context through the URB's
 *   embedded handle reference — no more NULL dereference.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _FILTER_URB_CONTEXT {
    PIRP    OriginalIrp;     /* the custom IOCTL IRP from UsbWarp.sys   */
    PIRP    InternalIrp;     /* our IRP sent to USB stack               */
    PURB    Urb;             /* USBD-allocated URB                       */
    PVOID   TransferBuffer;  /* if we allocated a copy                   */
    BOOLEAN FreeBuffer;      /* TRUE if TransferBuffer needs freeing     */
    PFILTER_EXT Ext;
    LONG    Completed;       /* 1 if completion already ran              */
} FILTER_URB_CONTEXT, *PFILTER_URB_CONTEXT;

static IO_COMPLETION_ROUTINE UrbCompletionRoutine;

/* Cancel routine: when UsbWarp.sys cancels the original IOCTL IRP
 * (e.g. during unbind/device close), we must also cancel the internal
 * IRP that's pending at the USB physical stack.  Without this, Bulk IN
 * reads hang forever waiting for data that will never arrive. */
static VOID
FilterCancelRoutine(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PFILTER_URB_CONTEXT ctx = (PFILTER_URB_CONTEXT)Irp->Tail.Overlay.DriverContext[0];

    UNREFERENCED_PARAMETER(DevObj);

    /* Release the cancel spin lock immediately — required by contract. */
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    /* Cancel the internal IRP at the USB stack. */
    if (ctx && ctx->InternalIrp) {
        IoCancelIrp(ctx->InternalIrp);
    }
    /* Don't complete the original IRP here — the internal IRP's
     * completion routine will fire (with STATUS_CANCELLED) and
     * complete the original IRP there. */
}

static NTSTATUS
UrbCompletionRoutine(PDEVICE_OBJECT DevObj, PIRP Irp, PVOID Context)
{
    PFILTER_URB_CONTEXT ctx = (PFILTER_URB_CONTEXT)Context;
    PIRP origIrp = ctx->OriginalIrp;
    PURB urb = ctx->Urb;

    UNREFERENCED_PARAMETER(DevObj);

    /* Prevent double completion (race between cancel and normal path). */
    if (InterlockedCompareExchange(&ctx->Completed, 1, 0) != 0) {
        /* Cancel routine or another completion already ran.
         * Just free the internal IRP — original IRP already handled. */
        IoFreeIrp(Irp);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    /* Remove cancel routine from original IRP before completing it. */
    IoSetCancelRoutine(origIrp, NULL);

    /* Build response in the original IRP's output buffer. */
    USBWARP_FILTER_URB_RESPONSE resp;
    resp.NtStatus     = Irp->IoStatus.Status;
    resp.UsbdStatus   = urb->UrbHeader.Status;
    resp.ActualLength  = 0;
    resp.Reserved      = 0;

    KdPrint(("UsbWarpFilter: COMPLETE func=%u nt=0x%x usbd=0x%x\n",
             urb->UrbHeader.Function, resp.NtStatus, resp.UsbdStatus));

    /* Extract actual length based on URB function. */
    switch (urb->UrbHeader.Function) {
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        resp.ActualLength = urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
        break;
    case URB_FUNCTION_CONTROL_TRANSFER:
        resp.ActualLength = urb->UrbControlTransfer.TransferBufferLength;
        break;
    default:
        resp.ActualLength = 0;
        break;
    }

    /* Copy response to original IRP's output buffer. */
    PIO_STACK_LOCATION origStack = IoGetCurrentIrpStackLocation(origIrp);
    ULONG outLen = origStack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID outBuf = origIrp->AssociatedIrp.SystemBuffer;

    if (outBuf && outLen >= sizeof(resp)) {
        RtlCopyMemory(outBuf, &resp, sizeof(resp));
        origIrp->IoStatus.Information = sizeof(resp);

        /* For IN transfers with inline mode (no KernelVa), copy data
         * back to the output buffer after the response header. */
        if (ctx->FreeBuffer && ctx->TransferBuffer &&
            resp.ActualLength > 0 &&
            outLen >= sizeof(resp) + resp.ActualLength) {
            RtlCopyMemory((PUCHAR)outBuf + sizeof(resp),
                          ctx->TransferBuffer, resp.ActualLength);
            origIrp->IoStatus.Information = sizeof(resp) + resp.ActualLength;
        }
    }

    origIrp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(origIrp, IO_NO_INCREMENT);

    /* Cleanup. */
    if (ctx->FreeBuffer && ctx->TransferBuffer)
        ExFreePoolWithTag(ctx->TransferBuffer, FILTER_TAG);
    USBD_UrbFree(ctx->Ext->UsbdHandle, ctx->Urb);
    IoReleaseRemoveLock(&ctx->Ext->RemoveLock, origIrp);
    IoFreeIrp(Irp);
    ExFreePoolWithTag(ctx, FILTER_TAG);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
HandleSubmitUrb(PFILTER_EXT Ext, PIRP OrigIrp)
{
    PIO_STACK_LOCATION  irpSp = IoGetCurrentIrpStackLocation(OrigIrp);
    PVOID               inBuf = OrigIrp->AssociatedIrp.SystemBuffer;
    ULONG               inLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    NTSTATUS            status;
    PURB                urb = NULL;
    PIRP                newIrp = NULL;
    PIO_STACK_LOCATION  nextSp;
    PFILTER_URB_CONTEXT ctx = NULL;
    PVOID               transferBuf = NULL;
    BOOLEAN             freeTransferBuf = FALSE;

    if (inLen < sizeof(USBWARP_FILTER_URB_REQUEST))
        return STATUS_BUFFER_TOO_SMALL;

    USBWARP_FILTER_URB_REQUEST *req = (USBWARP_FILTER_URB_REQUEST *)inBuf;

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
        if (wLen != (USHORT)req->TransferLength) {
            KdPrint(("UsbWarpFilter: BLOCKED wLen=%u != xferLen=%u\n",
                     wLen, req->TransferLength));
            return STATUS_INVALID_PARAMETER;
        }

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

        if (!urb->UrbBulkOrInterruptTransfer.PipeHandle) {
            KdPrint(("UsbWarpFilter: NULL pipe handle ep=0x%02x\n",
                     req->Endpoint | (req->Direction ? 0x80 : 0)));
            status = STATUS_INVALID_PARAMETER;
            goto Cleanup;
        }
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

    /* Allocate a new IRP for the USB stack. */
    newIrp = IoAllocateIrp(Ext->LowerDevice->StackSize, FALSE);
    if (!newIrp) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    /* Allocate completion context. */
    ctx = (PFILTER_URB_CONTEXT)ExAllocatePool2(
              POOL_FLAG_NON_PAGED, sizeof(*ctx), FILTER_TAG);
    if (!ctx) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    ctx->OriginalIrp    = OrigIrp;
    ctx->InternalIrp    = newIrp;
    ctx->Urb             = urb;
    ctx->TransferBuffer  = freeTransferBuf ? transferBuf : NULL;
    ctx->FreeBuffer      = freeTransferBuf;
    ctx->Ext             = Ext;
    ctx->Completed       = 0;

    /* Format the new IRP. */
    nextSp = IoGetNextIrpStackLocation(newIrp);
    nextSp->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    nextSp->Parameters.DeviceIoControl.IoControlCode =
        IOCTL_INTERNAL_USB_SUBMIT_URB;
    nextSp->Parameters.Others.Argument1 = urb;

    IoSetCompletionRoutine(newIrp, UrbCompletionRoutine, ctx,
                           TRUE, TRUE, TRUE);

    /* Acquire remove lock for the duration of the async URB. */
    status = IoAcquireRemoveLock(&Ext->RemoveLock, OrigIrp);
    if (!NT_SUCCESS(status)) goto Cleanup;

    /* Set cancel routine on original IRP so that when UsbWarp.sys
     * cancels its WDF request (during unbind/device close), we
     * propagate cancellation to the internal IRP at the USB stack.
     * Without this, pending Bulk IN reads hang forever. */
    OrigIrp->Tail.Overlay.DriverContext[0] = ctx;
    IoSetCancelRoutine(OrigIrp, FilterCancelRoutine);

    /* Check if IRP was already cancelled before we set the routine. */
    if (OrigIrp->Cancel) {
        /* IRP was cancelled — remove our cancel routine. If it was
         * already called (returned non-NULL), just proceed normally
         * as the cancel routine will fire. If it wasn't called yet
         * (we got it back), cancel the internal IRP ourselves. */
        if (IoSetCancelRoutine(OrigIrp, NULL) != NULL) {
            /* We removed it before it ran — cancel manually. */
            IoReleaseRemoveLock(&Ext->RemoveLock, OrigIrp);
            status = STATUS_CANCELLED;
            goto Cleanup;
        }
        /* Cancel routine already dispatched — it will call IoCancelIrp
         * on newIrp. Proceed to send normally; completion will handle it. */
    }

    /* Mark the original IRP as pending. */
    IoMarkIrpPending(OrigIrp);

    /* Send to the lower device (USB FDO or PDO). */
    IoCallDriver(Ext->LowerDevice, newIrp);

    return STATUS_PENDING;

Cleanup:
    if (newIrp) IoFreeIrp(newIrp);
    if (ctx) ExFreePoolWithTag(ctx, FILTER_TAG);
    if (urb) USBD_UrbFree(Ext->UsbdHandle, urb);
    if (freeTransferBuf && transferBuf)
        ExFreePoolWithTag(transferBuf, FILTER_TAG);
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

    /* Submit synchronously via a new IRP. */
    {
        KEVENT event;
        KeInitializeEvent(&event, NotificationEvent, FALSE);

        PIRP newIrp = IoAllocateIrp(Ext->LowerDevice->StackSize, FALSE);
        if (!newIrp) {
            USBD_UrbFree(Ext->UsbdHandle, urb);
            ExFreePoolWithTag(descBuf, FILTER_TAG);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(newIrp);
        sp->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        sp->Parameters.DeviceIoControl.IoControlCode =
            IOCTL_INTERNAL_USB_SUBMIT_URB;
        sp->Parameters.Others.Argument1 = urb;

        IoSetCompletionRoutine(newIrp, SyncCompletionRoutine, &event,
                               TRUE, TRUE, TRUE);

        status = IoCallDriver(Ext->LowerDevice, newIrp);
        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
            status = newIrp->IoStatus.Status;
        }

        IoFreeIrp(newIrp);
    }

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

static NTSTATUS
HandleSelectConfig(PFILTER_EXT Ext, PIRP Irp)
{
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status;

    if (outLen < sizeof(USBWARP_FILTER_PIPES_RESPONSE))
        return STATUS_BUFFER_TOO_SMALL;

    /* Step 1: GET_DESCRIPTOR(config header) to get wTotalLength. */
    USB_CONFIGURATION_DESCRIPTOR cfgHdr;
    PURB urb = NULL;
    KEVENT event;

    status = USBD_UrbAllocate(Ext->UsbdHandle, &urb);
    if (!NT_SUCCESS(status)) return status;

    UsbBuildGetDescriptorRequest(
        urb, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
        USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
        &cfgHdr, NULL, sizeof(cfgHdr), NULL);

    /* Sync submit. */
    {
        KeInitializeEvent(&event, NotificationEvent, FALSE);
        PIRP newIrp = IoAllocateIrp(Ext->LowerDevice->StackSize, FALSE);
        if (!newIrp) { USBD_UrbFree(Ext->UsbdHandle, urb); return STATUS_INSUFFICIENT_RESOURCES; }
        PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(newIrp);
        sp->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        sp->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
        sp->Parameters.Others.Argument1 = urb;
        IoSetCompletionRoutine(newIrp, SyncCompletionRoutine, &event, TRUE, TRUE, TRUE);
        status = IoCallDriver(Ext->LowerDevice, newIrp);
        if (status == STATUS_PENDING) { KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL); status = newIrp->IoStatus.Status; }
        IoFreeIrp(newIrp);
    }
    USBD_UrbFree(Ext->UsbdHandle, urb);

    if (!NT_SUCCESS(status) || cfgHdr.wTotalLength < sizeof(cfgHdr))
        return NT_SUCCESS(status) ? STATUS_DEVICE_DATA_ERROR : status;

    /* Step 2: GET_DESCRIPTOR(full config). */
    ULONG cfgLen = cfgHdr.wTotalLength;
    PUCHAR cfgBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, cfgLen, FILTER_TAG);
    if (!cfgBuf) return STATUS_INSUFFICIENT_RESOURCES;

    status = USBD_UrbAllocate(Ext->UsbdHandle, &urb);
    if (!NT_SUCCESS(status)) { ExFreePoolWithTag(cfgBuf, FILTER_TAG); return status; }

    UsbBuildGetDescriptorRequest(
        urb, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
        USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
        cfgBuf, NULL, cfgLen, NULL);

    {
        KeInitializeEvent(&event, NotificationEvent, FALSE);
        PIRP newIrp = IoAllocateIrp(Ext->LowerDevice->StackSize, FALSE);
        if (!newIrp) { USBD_UrbFree(Ext->UsbdHandle, urb); ExFreePoolWithTag(cfgBuf, FILTER_TAG); return STATUS_INSUFFICIENT_RESOURCES; }
        PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(newIrp);
        sp->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        sp->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
        sp->Parameters.Others.Argument1 = urb;
        IoSetCompletionRoutine(newIrp, SyncCompletionRoutine, &event, TRUE, TRUE, TRUE);
        status = IoCallDriver(Ext->LowerDevice, newIrp);
        if (status == STATUS_PENDING) { KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL); status = newIrp->IoStatus.Status; }
        IoFreeIrp(newIrp);
    }
    USBD_UrbFree(Ext->UsbdHandle, urb);

    if (!NT_SUCCESS(status)) { ExFreePoolWithTag(cfgBuf, FILTER_TAG); return status; }

    /* Step 3: SELECT_CONFIGURATION. */
    PUSB_CONFIGURATION_DESCRIPTOR cfgDesc = (PUSB_CONFIGURATION_DESCRIPTOR)cfgBuf;
    UCHAR numIf = cfgDesc->bNumInterfaces;
    PUSBD_INTERFACE_LIST_ENTRY ifList =
        (PUSBD_INTERFACE_LIST_ENTRY)ExAllocatePool2(
            POOL_FLAG_NON_PAGED,
            sizeof(USBD_INTERFACE_LIST_ENTRY) * (numIf + 1), FILTER_TAG);
    if (!ifList) { ExFreePoolWithTag(cfgBuf, FILTER_TAG); return STATUS_INSUFFICIENT_RESOURCES; }

    RtlZeroMemory(ifList, sizeof(USBD_INTERFACE_LIST_ENTRY) * (numIf + 1));
    for (UCHAR i = 0; i < numIf; i++) {
        ifList[i].InterfaceDescriptor =
            USBD_ParseConfigurationDescriptorEx(cfgDesc, cfgBuf, i, 0, -1, -1, -1);
    }
    ifList[numIf].InterfaceDescriptor = NULL;

    PURB selectUrb = USBD_CreateConfigurationRequestEx(cfgDesc, ifList);
    if (!selectUrb) {
        ExFreePoolWithTag(ifList, FILTER_TAG);
        ExFreePoolWithTag(cfgBuf, FILTER_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    {
        KeInitializeEvent(&event, NotificationEvent, FALSE);
        PIRP newIrp = IoAllocateIrp(Ext->LowerDevice->StackSize, FALSE);
        if (!newIrp) { ExFreePoolWithTag(selectUrb, 0); ExFreePoolWithTag(ifList, FILTER_TAG); ExFreePoolWithTag(cfgBuf, FILTER_TAG); return STATUS_INSUFFICIENT_RESOURCES; }
        PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(newIrp);
        sp->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        sp->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
        sp->Parameters.Others.Argument1 = selectUrb;
        IoSetCompletionRoutine(newIrp, SyncCompletionRoutine, &event, TRUE, TRUE, TRUE);
        status = IoCallDriver(Ext->LowerDevice, newIrp);
        if (status == STATUS_PENDING) { KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL); status = newIrp->IoStatus.Status; }
        IoFreeIrp(newIrp);
    }

    /* Build pipe response. */
    USBWARP_FILTER_PIPES_RESPONSE *resp =
        (USBWARP_FILTER_PIPES_RESPONSE *)Irp->AssociatedIrp.SystemBuffer;
    RtlZeroMemory(resp, sizeof(*resp));

    if (NT_SUCCESS(status)) {
        PUCHAR ptr = (PUCHAR)&selectUrb->UrbSelectConfiguration.Interface;
        for (UCHAR i = 0; i < numIf && resp->NumPipes < USBWARP_FILTER_MAX_PIPES; i++) {
            PUSBD_INTERFACE_INFORMATION ifInfo = (PUSBD_INTERFACE_INFORMATION)ptr;
            for (ULONG p = 0; p < ifInfo->NumberOfPipes &&
                              resp->NumPipes < USBWARP_FILTER_MAX_PIPES; p++) {
                USBD_PIPE_INFORMATION *pi = &ifInfo->Pipes[p];
                USBWARP_FILTER_PIPE_INFO *out = &resp->Pipes[resp->NumPipes];
                out->EndpointAddress = pi->EndpointAddress;
                out->PipeType        = (UCHAR)pi->PipeType;
                out->MaxPacketSize   = pi->MaximumPacketSize;
                out->PipeHandle      = (uint64_t)(ULONG_PTR)pi->PipeHandle;
                resp->NumPipes++;

                KdPrint(("UsbWarpFilter: pipe[%u] ep=0x%02x type=%u "
                         "handle=%p maxPkt=%u\n",
                         resp->NumPipes - 1,
                         pi->EndpointAddress, pi->PipeType,
                         pi->PipeHandle, pi->MaximumPacketSize));
            }
            ptr += ifInfo->Length;
        }
        Irp->IoStatus.Information = sizeof(*resp);

        /* Cache pipe map in filter extension for GET_PIPES. */
        RtlCopyMemory(&Ext->CachedPipes, resp, sizeof(*resp));
        Ext->ConfigSelected = TRUE;
    }

    ExFreePoolWithTag(selectUrb, 0);
    ExFreePoolWithTag(ifList, FILTER_TAG);
    ExFreePoolWithTag(cfgBuf, FILTER_TAG);
    return status;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  IRP_MJ_DEVICE_CONTROL — custom IOCTL handler
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS FilterDeviceControl(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)DevObj->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG ctl = irpSp->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS status;

    switch (ctl) {
    case IOCTL_USBWARP_FILTER_SUBMIT_URB:
        status = HandleSubmitUrb(ext, Irp);
        if (status == STATUS_PENDING)
            return status;  /* completion will happen later */
        break;

    case IOCTL_USBWARP_FILTER_GET_DESCRIPTOR:
        status = HandleGetDescriptor(ext, Irp);
        break;

    case IOCTL_USBWARP_FILTER_SELECT_CONFIG:
        status = HandleSelectConfig(ext, Irp);
        break;

    case IOCTL_USBWARP_FILTER_GET_PIPES: {
        /* Return cached pipe map from last SELECT_CONFIG.
         * Does NOT re-select configuration. */
        ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
        if (outLen < sizeof(USBWARP_FILTER_PIPES_RESPONSE)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else if (!ext->ConfigSelected) {
            status = STATUS_DEVICE_NOT_READY;
        } else {
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
                          &ext->CachedPipes, sizeof(ext->CachedPipes));
            Irp->IoStatus.Information = sizeof(ext->CachedPipes);
            status = STATUS_SUCCESS;
        }
        break;
    }

    default:
        /* Not our IOCTL — forward to lower driver (function driver). */
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->LowerDevice, Irp);
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  IRP_MJ_INTERNAL_DEVICE_CONTROL — transparent pass-through
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS FilterInternalDeviceControl(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)DevObj->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDevice, Irp);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  IRP_MJ_PNP
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS FilterPnp(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)DevObj->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    status = IoAcquireRemoveLock(&ext->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    switch (irpSp->MinorFunction) {

    case IRP_MN_START_DEVICE:
        /* Forward START to lower, wait for completion. */
        status = ForwardIrpSync(ext, Irp);
        if (NT_SUCCESS(status)) {
            /* Register as USB client. */
            status = USBD_CreateHandle(
                         ext->Self, ext->LowerDevice,
                         USBD_CLIENT_CONTRACT_VERSION_602,
                         FILTER_TAG, &ext->UsbdHandle);
            if (NT_SUCCESS(status)) {
                KdPrint(("UsbWarpFilter: USBD_CreateHandle OK %p\n",
                         ext->UsbdHandle));
            } else {
                KdPrint(("UsbWarpFilter: USBD_CreateHandle failed 0x%x\n",
                         status));
                /* Non-fatal for the device — USB still works,
                 * but our proxy won't function. */
                status = STATUS_SUCCESS;
            }

            /* Register device interface for UsbWarp.sys to find us. */
            IoRegisterDeviceInterface(
                ext->Pdo, &GUID_USBWARP_FILTER_INTERFACE,
                NULL, &ext->InterfaceLink);
            IoSetDeviceInterfaceState(&ext->InterfaceLink, TRUE);

            ext->Started = TRUE;
            KdPrint(("UsbWarpFilter: device started, interface=%wZ\n",
                     &ext->InterfaceLink));
        }
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;

    case IRP_MN_REMOVE_DEVICE:
        ext->Removed = TRUE;
        ext->Started = FALSE;

        /* Disable interface. */
        if (ext->InterfaceLink.Buffer) {
            IoSetDeviceInterfaceState(&ext->InterfaceLink, FALSE);
            RtlFreeUnicodeString(&ext->InterfaceLink);
        }

        /* Close USBD handle. */
        if (ext->UsbdHandle) {
            USBD_CloseHandle(ext->UsbdHandle);
            ext->UsbdHandle = NULL;
        }

        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);

        IoReleaseRemoveLockAndWait(&ext->RemoveLock, Irp);

        IoDetachDevice(ext->LowerDevice);
        IoDeleteDevice(ext->Self);

        KdPrint(("UsbWarpFilter: device removed\n"));
        return status;

    default:
        /* Forward all other PnP IRPs. */
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);
        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  IRP_MJ_CREATE / CLOSE — allow opens from UsbWarp.sys
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS FilterCreateClose(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)DevObj->DeviceExtension;
    /* Forward to lower driver — the function driver may need to see these. */
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDevice, Irp);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Pass-through for all other IRP types
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS FilterPassThrough(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)DevObj->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDevice, Irp);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §12  AddDevice
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS FilterAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT fido = NULL;
    PFILTER_EXT    ext;
    NTSTATUS       status;

    KdPrint(("UsbWarpFilter: AddDevice PDO=%p\n", Pdo));

    status = IoCreateDevice(
                 DriverObject,
                 sizeof(FILTER_EXT),
                 NULL,
                 FILE_DEVICE_UNKNOWN,
                 FILE_DEVICE_SECURE_OPEN,
                 FALSE,
                 &fido);
    if (!NT_SUCCESS(status))
        return status;

    ext = (PFILTER_EXT)fido->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));

    ext->Self = fido;
    ext->Pdo  = Pdo;
    IoInitializeRemoveLock(&ext->RemoveLock, FILTER_TAG, 0, 0);

    ext->LowerDevice = IoAttachDeviceToDeviceStack(fido, Pdo);
    if (!ext->LowerDevice) {
        IoDeleteDevice(fido);
        return STATUS_NO_SUCH_DEVICE;
    }

    /* Copy flags from lower device. */
    fido->DeviceType = ext->LowerDevice->DeviceType;
    fido->Characteristics = ext->LowerDevice->Characteristics;
    fido->Flags |= ext->LowerDevice->Flags &
                   (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    fido->Flags &= ~DO_DEVICE_INITIALIZING;

    KdPrint(("UsbWarpFilter: FiDO=%p attached above %p\n",
             fido, ext->LowerDevice));

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §13  DriverEntry / Unload
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID FilterUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    KdPrint(("UsbWarpFilter: unloaded\n"));
}

EXTERN_C NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    ULONG i;
    UNREFERENCED_PARAMETER(RegistryPath);

    KdPrint(("UsbWarpFilter: DriverEntry\n"));

    DriverObject->DriverUnload                         = FilterUnload;
    DriverObject->DriverExtension->AddDevice           = FilterAddDevice;

    /* Set all major functions to pass-through by default. */
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = FilterPassThrough;

    /* Override specific major functions. */
    DriverObject->MajorFunction[IRP_MJ_PNP]                      = FilterPnp;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]           = FilterDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL]  = FilterInternalDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_CREATE]                   = FilterCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]                    = FilterCreateClose;

    return STATUS_SUCCESS;
}
