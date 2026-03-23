/*
 * usbwarp_device.c — USB device management via UsbWarpFilter.sys proxy.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * ARCHITECTURE:
 *   UsbWarp.sys (Non-PnP control device) cannot directly submit URBs to
 *   USB device stacks.  All USB I/O is proxied through UsbWarpFilter.sys,
 *   a USB class upper filter driver that sits in each USB device stack
 *   and holds a legitimate USBD_HANDLE.
 *
 *   UsbWarp.sys opens the USB device interface path (the same path the
 *   Service passes during bind).  Since UsbWarpFilter.sys is a class
 *   upper filter, it intercepts our custom IOCTLs:
 *     - IOCTL_USBWARP_FILTER_GET_DESCRIPTOR → proxy descriptor reads
 *     - IOCTL_USBWARP_FILTER_SELECT_CONFIG  → configure + return pipe map
 *     - IOCTL_USBWARP_FILTER_SUBMIT_URB     → proxy URB submissions
 *
 *   Zero-copy is preserved: the filter passes our SHM kernel VA directly
 *   as the URB TransferBuffer to the USB stack.
 */

#include "usbwarp_drv.h"
#include "../include/usbwarp_filter_ioctl.h"

static EVT_WDF_REQUEST_COMPLETION_ROUTINE UrbProxyCompletion;

static VOID SendUrbComplete(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_opt_ PUSBWARP_URB_CONTEXT UrbCtx,
    _In_ int32_t ProtoStatus,
    _In_ ULONG ActualLength);

static VOID DecrementPending(PUSBWARP_DEVICE_CONTEXT DevCtx)
{
    LONG newCount = InterlockedDecrement(&DevCtx->PendingCount);
    if (newCount == 0)
        KeSetEvent(&DevCtx->DrainEvent, IO_NO_INCREMENT, FALSE);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Synchronous filter IOCTL helper
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
SendFilterIoctlSync(
    _In_ WDFIOTARGET Target,
    _In_ ULONG IoControlCode,
    _In_opt_ PVOID InputBuffer,
    _In_ ULONG InputLength,
    _Out_opt_ PVOID OutputBuffer,
    _In_ ULONG OutputLength,
    _Out_opt_ PULONG BytesReturned
    )
{
    NTSTATUS    status;
    WDFREQUEST  request = NULL;
    WDFMEMORY   inMem = NULL, outMem = NULL;

    if (BytesReturned) *BytesReturned = 0;

    status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, Target, &request);
    if (!NT_SUCCESS(status)) return status;

    if (InputBuffer && InputLength > 0) {
        status = WdfMemoryCreatePreallocated(WDF_NO_OBJECT_ATTRIBUTES,
                                              InputBuffer, InputLength, &inMem);
        if (!NT_SUCCESS(status)) { WdfObjectDelete(request); return status; }
    }

    if (OutputBuffer && OutputLength > 0) {
        status = WdfMemoryCreatePreallocated(WDF_NO_OBJECT_ATTRIBUTES,
                                              OutputBuffer, OutputLength, &outMem);
        if (!NT_SUCCESS(status)) {
            if (inMem) WdfObjectDelete(inMem);
            WdfObjectDelete(request);
            return status;
        }
    }

    status = WdfIoTargetFormatRequestForIoctl(
                 Target, request, IoControlCode,
                 inMem, NULL, outMem, NULL);
    if (!NT_SUCCESS(status)) {
        if (outMem) WdfObjectDelete(outMem);
        if (inMem) WdfObjectDelete(inMem);
        WdfObjectDelete(request);
        return status;
    }

    WDF_REQUEST_SEND_OPTIONS opts;
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&opts, WDF_REL_TIMEOUT_IN_SEC(10));

    if (!WdfRequestSend(request, Target, &opts))
        status = WdfRequestGetStatus(request);

    if (NT_SUCCESS(status) && BytesReturned) {
        WDF_REQUEST_COMPLETION_PARAMS params;
        WDF_REQUEST_COMPLETION_PARAMS_INIT(&params);
        WdfRequestGetCompletionParams(request, &params);
        *BytesReturned = (ULONG)params.IoStatus.Information;
    }

    if (outMem) WdfObjectDelete(outMem);
    if (inMem) WdfObjectDelete(inMem);
    WdfObjectDelete(request);
    return status;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Device open — via UsbWarpFilter proxy
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS
UsbWarpDeviceOpen(
    _In_  PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_  PUSBWARP_DEVICE_CONTEXT DevCtx,
    _In_  PCUNICODE_STRING        InstancePath
    )
{
    NTSTATUS                  status;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    WDF_OBJECT_ATTRIBUTES     attrs;

    UNREFERENCED_PARAMETER(InstancePath);

    KeInitializeEvent(&DevCtx->DrainEvent, NotificationEvent, TRUE);
    DevCtx->NumPipes    = 0;
    DevCtx->FilterTarget = NULL;
    RtlZeroMemory(DevCtx->PipeHandles, sizeof(DevCtx->PipeHandles));
    RtlZeroMemory(DevCtx->PipeEndpoints, sizeof(DevCtx->PipeEndpoints));

    /* Open the USB device interface path.  This goes to the TOP of the
     * USB device stack.  UsbWarpFilter.sys (class upper filter) intercepts
     * our custom IOCTLs; normal IOCTLs pass through to the function driver. */
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = Ctx->ControlDevice;

    status = WdfIoTargetCreate(Ctx->ControlDevice, &attrs, &DevCtx->FilterTarget);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: WdfIoTargetCreate failed 0x%x\n", status));
        return status;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &openParams, &DevCtx->InstancePath,
        GENERIC_READ | GENERIC_WRITE);
    openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;

    status = WdfIoTargetOpen(DevCtx->FilterTarget, &openParams);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: WdfIoTargetOpen failed 0x%x (filter not installed?)\n",
                 status));
        WdfObjectDelete(DevCtx->FilterTarget);
        DevCtx->FilterTarget = NULL;
        return status;
    }

    KdPrint(("UsbWarp: filter target opened for %wZ\n", &DevCtx->InstancePath));

    /* ── Step 1: Get device descriptor via filter ──────────────────────── */
    {
        USBWARP_FILTER_GET_DESC_REQUEST req;
        USB_DEVICE_DESCRIPTOR devDesc;
        ULONG returned = 0;

        req.DescriptorType = USB_DEVICE_DESCRIPTOR_TYPE;
        req.Index          = 0;
        req.LanguageId     = 0;
        req.BufferLength   = sizeof(devDesc);

        status = SendFilterIoctlSync(
                     DevCtx->FilterTarget,
                     IOCTL_USBWARP_FILTER_GET_DESCRIPTOR,
                     &req, sizeof(req),
                     &devDesc, sizeof(devDesc), &returned);

        if (NT_SUCCESS(status) && returned >= sizeof(devDesc)) {
            DevCtx->VendorId  = devDesc.idVendor;
            DevCtx->ProductId = devDesc.idProduct;
            /* NOTE: bcdUSB is the USB spec version the device supports,
             * NOT the actual connection speed.  A full-speed device can
             * declare bcdUSB=0x0200.  Actual speed is inferred from
             * bulk endpoint maxpacket size after SELECT_CONFIG below. */
            DevCtx->Speed = USBWARP_SPEED_FULL; /* default, updated below */
            KdPrint(("UsbWarp: VID=%04x PID=%04x bcdUSB=%04x\n",
                     devDesc.idVendor, devDesc.idProduct, devDesc.bcdUSB));
        } else {
            KdPrint(("UsbWarp: GET_DESCRIPTOR failed 0x%x (ret=%u)\n",
                     status, returned));
            DevCtx->VendorId  = 0;
            DevCtx->ProductId = 0;
            DevCtx->Speed     = USBWARP_SPEED_HIGH;
        }
    }

    /* ── Step 2: SELECT_CONFIG via filter → get pipe map ───────────────── */
    {
        USBWARP_FILTER_SELECT_CONFIG_REQUEST req;
        USBWARP_FILTER_PIPES_RESPONSE resp;
        ULONG returned = 0;

        req.ConfigIndex = 0;
        RtlZeroMemory(&resp, sizeof(resp));

        status = SendFilterIoctlSync(
                     DevCtx->FilterTarget,
                     IOCTL_USBWARP_FILTER_SELECT_CONFIG,
                     &req, sizeof(req),
                     &resp, sizeof(resp), &returned);

        if (NT_SUCCESS(status) && returned >= sizeof(resp)) {
            for (ULONG i = 0; i < resp.NumPipes && i < 32; i++) {
                DevCtx->PipeHandles[i] =
                    (USBD_PIPE_HANDLE)(ULONG_PTR)resp.Pipes[i].PipeHandle;
                DevCtx->PipeEndpoints[i] = resp.Pipes[i].EndpointAddress;
                KdPrint(("UsbWarp: pipe[%u] ep=0x%02x type=%u maxPkt=%u\n",
                         i, resp.Pipes[i].EndpointAddress,
                         resp.Pipes[i].PipeType,
                         resp.Pipes[i].MaxPacketSize));
            }
            DevCtx->NumPipes = (UCHAR)min(resp.NumPipes, 32);

            /* Infer actual connection speed from bulk endpoint maxpacket.
             *   Full-speed:  maxpacket ≤ 64
             *   High-speed:  maxpacket = 512
             *   Super-speed: maxpacket = 1024
             * This is reliable because USB spec mandates these values.
             * bcdUSB is NOT reliable — it's the spec version, not speed. */
            for (ULONG i = 0; i < resp.NumPipes; i++) {
                if (resp.Pipes[i].PipeType == 2 /* UsbdPipeTypeBulk */) {
                    if (resp.Pipes[i].MaxPacketSize >= 1024)
                        DevCtx->Speed = USBWARP_SPEED_SUPER;
                    else if (resp.Pipes[i].MaxPacketSize >= 512)
                        DevCtx->Speed = USBWARP_SPEED_HIGH;
                    else
                        DevCtx->Speed = USBWARP_SPEED_FULL;
                    KdPrint(("UsbWarp: speed inferred from bulk maxPkt=%u → %s\n",
                             resp.Pipes[i].MaxPacketSize,
                             DevCtx->Speed == USBWARP_SPEED_SUPER ? "super" :
                             DevCtx->Speed == USBWARP_SPEED_HIGH ? "high" : "full"));
                    break;
                }
            }
        } else {
            KdPrint(("UsbWarp: SELECT_CONFIG failed 0x%x\n", status));
            DevCtx->NumPipes = 0;
        }
    }

    KdPrint(("UsbWarp: device opened slot=%u VID=%04x PID=%04x pipes=%u\n",
             DevCtx->DeviceIndex, DevCtx->VendorId, DevCtx->ProductId,
             DevCtx->NumPipes));

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Device close
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpDeviceClose(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ PUSBWARP_DEVICE_CONTEXT DevCtx
    )
{
    LARGE_INTEGER timeout;
    UNREFERENCED_PARAMETER(Ctx);

    if (!DevCtx->FilterTarget) goto done;

    WdfIoTargetStop(DevCtx->FilterTarget, WdfIoTargetCancelSentIo);

    /* Wait for pending URBs to drain. */
    if (InterlockedCompareExchange(&DevCtx->PendingCount, 0, 0) > 0) {
        KeClearEvent(&DevCtx->DrainEvent);
        timeout.QuadPart = -100000000LL;  /* 10 seconds */
        KeWaitForSingleObject(&DevCtx->DrainEvent,
                              Executive, KernelMode, FALSE, &timeout);
    }

    WdfIoTargetClose(DevCtx->FilterTarget);
    WdfObjectDelete(DevCtx->FilterTarget);
    DevCtx->FilterTarget = NULL;
    DevCtx->NumPipes = 0;

done:
    KdPrint(("UsbWarp: device closed slot=%u\n", DevCtx->DeviceIndex));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Pipe handle lookup
 * ═══════════════════════════════════════════════════════════════════════════ */

static USBD_PIPE_HANDLE
FindPipe(
    _In_ PUSBWARP_DEVICE_CONTEXT DevCtx,
    _In_ UCHAR Endpoint,
    _In_ UCHAR Direction
    )
{
    UCHAR targetAddr = Endpoint | (Direction ? 0x80 : 0x00);
    for (int i = 0; i < (int)DevCtx->NumPipes; i++) {
        if (DevCtx->PipeEndpoints[i] == targetAddr)
            return DevCtx->PipeHandles[i];
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  URB proxy completion callback
 *
 *   Called when the filter completes IOCTL_USBWARP_FILTER_SUBMIT_URB.
 *   Parses the response and writes MSG_URB_COMPLETE to the Ring.
 * ═══════════════════════════════════════════════════════════════════════════ */

static VOID
UrbProxyCompletion(
    _In_ WDFREQUEST                     Request,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context
    )
{
    PUSBWARP_URB_CONTEXT    urbCtx = (PUSBWARP_URB_CONTEXT)Context;
    PUSBWARP_GLOBAL_CONTEXT ctx;
    PUSBWARP_DEVICE_CONTEXT devCtx;
    int32_t                 protoStatus;
    ULONG                   actualLength = 0;
    KIRQL                   oldIrql;

    UNREFERENCED_PARAMETER(Request);

    /* Parse filter response directly from our stored output buffer.
     * Do NOT use WdfRequestRetrieveOutputBuffer here — when the filter
     * completes the underlying WDM IRP directly via IoCompleteRequest,
     * WDF's internal memory tracking may not see the output data,
     * causing a NULL dereference in FxRequest::GetMemoryObject. */
    if (urbCtx->FilterOutBuf &&
        urbCtx->FilterOutSize >= sizeof(USBWARP_FILTER_URB_RESPONSE)) {
        USBWARP_FILTER_URB_RESPONSE *resp =
            (USBWARP_FILTER_URB_RESPONSE *)urbCtx->FilterOutBuf;

        if (NT_SUCCESS(resp->NtStatus) && USBD_SUCCESS(resp->UsbdStatus)) {
            protoStatus = USBWARP_STATUS_SUCCESS;
        } else if (resp->UsbdStatus == USBD_STATUS_CANCELED) {
            protoStatus = USBWARP_STATUS_CANCELLED;
        } else if (resp->UsbdStatus == USBD_STATUS_STALL_PID ||
                   resp->UsbdStatus == USBD_STATUS_ENDPOINT_HALTED) {
            protoStatus = USBWARP_STATUS_STALL;
        } else if (resp->UsbdStatus == USBD_STATUS_TIMEOUT) {
            protoStatus = USBWARP_STATUS_TIMEOUT;
        } else {
            protoStatus = USBWARP_STATUS_PROTOCOL_ERROR;
        }
        actualLength = resp->ActualLength;

        /* For IN inline transfers, the filter appends the received data
         * after the response header in FilterOutBuf.  Copy it back to
         * urbCtx->InlineData so SendUrbComplete can include it in the
         * Ring message.  Without this, the Guest receives stale data. */
        if (urbCtx->Direction == 1 &&
            urbCtx->DataMode == USBWARP_DATA_INLINE &&
            actualLength > 0 &&
            actualLength <= USBWARP_INLINE_DATA_SIZE &&
            urbCtx->FilterOutSize >= sizeof(USBWARP_FILTER_URB_RESPONSE) + actualLength) {
            RtlCopyMemory(urbCtx->InlineData,
                          (PUCHAR)urbCtx->FilterOutBuf + sizeof(USBWARP_FILTER_URB_RESPONSE),
                          actualLength);
            urbCtx->InlineDataLen = actualLength;
        }
    } else {
        protoStatus = NT_SUCCESS(Params->IoStatus.Status) ?
                      USBWARP_STATUS_PROTOCOL_ERROR : USBWARP_STATUS_NO_RESOURCE;
    }

    ctx = UsbWarpGetContext(WdfIoTargetGetDevice(Target));

    if (urbCtx->DeviceIndex == 0 ||
        urbCtx->DeviceIndex > USBWARP_MAX_DEVICES_LIMIT)
        goto FreeOnly;

    devCtx = &ctx->Devices[urbCtx->DeviceIndex - 1];

    /* Record breaker status. */
    if (protoStatus == USBWARP_STATUS_SUCCESS)
        UsbWarpBreakerRecordSuccess(&devCtx->Breaker);
    else if (protoStatus != USBWARP_STATUS_CANCELLED)
        UsbWarpBreakerRecordHwError(&devCtx->Breaker);

    /* Statistics. */
    InterlockedIncrement64(&devCtx->TotalUrbs);
    InterlockedIncrement64(&ctx->TotalUrbsProcessed);
    if (actualLength > 0) {
        InterlockedAdd64(&devCtx->TotalBytes, (LONG64)actualLength);
        InterlockedAdd64(&ctx->TotalBytesTransferred, (LONG64)actualLength);
    }

    SendUrbComplete(ctx, urbCtx, protoStatus, actualLength);

    KeAcquireSpinLock(&devCtx->PendingLock, &oldIrql);
    RemoveEntryList(&urbCtx->ListEntry);
    KeReleaseSpinLock(&devCtx->PendingLock, oldIrql);
    DecrementPending(devCtx);

FreeOnly:
    if (urbCtx->FilterOutBuf)
        ExFreePoolWithTag(urbCtx->FilterOutBuf, USBWARP_DRIVER_TAG);
    if (urbCtx->FilterReqBuf)
        ExFreePoolWithTag(urbCtx->FilterReqBuf, USBWARP_DRIVER_TAG);
    if (urbCtx->OutMemory)  WdfObjectDelete(urbCtx->OutMemory);
    if (urbCtx->UrbMemory)  WdfObjectDelete(urbCtx->UrbMemory);
    if (urbCtx->WdfRequest) WdfObjectDelete(urbCtx->WdfRequest);
    ExFreePoolWithTag(urbCtx, USBWARP_DRIVER_TAG);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  URB submit — proxy through filter
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpProcessUrbSubmit(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ const struct usbwarp_msg_urb_submit *Msg
    )
{
    PUSBWARP_DEVICE_CONTEXT  devCtx;
    PUSBWARP_URB_CONTEXT     urbCtx = NULL;
    NTSTATUS                 status;
    ULONG                    devIdx;
    KIRQL                    oldIrql;

    devIdx = Msg->device_id;
    if (devIdx == 0 || devIdx > USBWARP_MAX_DEVICES_LIMIT) {
        SendUrbComplete(Ctx, NULL, USBWARP_STATUS_ACCESS_DENIED, 0);
        return;
    }

    devCtx = &Ctx->Devices[devIdx - 1];
    if (!devCtx->InUse || !devCtx->FilterTarget) {
        SendUrbComplete(Ctx, NULL, USBWARP_STATUS_DISCONNECTED, 0);
        return;
    }

    /* Layer 6: Rate limit */
    if (!UsbWarpTokenBucketConsume(&devCtx->TokenBucket)) {
        UsbWarpErrorWindowRecord(&devCtx->Breaker.RateErrors);
        SendUrbComplete(Ctx, NULL, USBWARP_STATUS_NO_RESOURCE, 0);
        return;
    }

    /* Layer 7: Circuit breaker */
    if (!UsbWarpBreakerAllows(&devCtx->Breaker)) {
        SendUrbComplete(Ctx, NULL, USBWARP_STATUS_SHUTDOWN, 0);
        return;
    }

    /* Layer 5: Buffer bounds (BUFFER mode) */
    PVOID transferVa = NULL;
    if (Msg->data_mode == USBWARP_DATA_BUFFER && Msg->transfer_length > 0) {
        status = UsbWarpSafeGetTransferVa(Ctx, Msg->buffer_offset,
                                           Msg->transfer_length, &transferVa);
        if (!NT_SUCCESS(status)) {
            UsbWarpBreakerRecordProtoError(&devCtx->Breaker);
            SendUrbComplete(Ctx, NULL, USBWARP_STATUS_PROTOCOL_ERROR, 0);
            return;
        }
    }

    /* Allocate URB context. */
    urbCtx = (PUSBWARP_URB_CONTEXT)ExAllocatePool2(
                 POOL_FLAG_NON_PAGED, sizeof(USBWARP_URB_CONTEXT),
                 USBWARP_DRIVER_TAG);
    if (!urbCtx) {
        SendUrbComplete(Ctx, NULL, USBWARP_STATUS_NO_RESOURCE, 0);
        return;
    }

    RtlZeroMemory(urbCtx, sizeof(*urbCtx));
    urbCtx->TransactionId  = Msg->hdr.transaction_id;
    urbCtx->DeviceIndex    = devIdx;
    urbCtx->Endpoint       = (UCHAR)Msg->endpoint;
    urbCtx->Direction      = (UCHAR)Msg->direction;
    urbCtx->TransferType   = (UCHAR)Msg->transfer_type;
    urbCtx->DataMode       = (UCHAR)Msg->data_mode;
    urbCtx->TransferLength = Msg->transfer_length;
    urbCtx->BufferOffset   = Msg->buffer_offset;
    urbCtx->TransferVa     = transferVa;
    InterlockedExchange(&urbCtx->State, UURB_STATE_ACTIVE);
    KeQuerySystemTime(&urbCtx->SubmitTime);
    urbCtx->GuestSubmitTime.QuadPart = (LONGLONG)Msg->hdr.timestamp;

    if (Msg->transfer_type == USBWARP_XFER_CONTROL)
        RtlCopyMemory(urbCtx->SetupPacket, Msg->setup_packet, 8);

    if (Msg->data_mode == USBWARP_DATA_INLINE && Msg->transfer_length > 0) {
        ULONG copyLen = min(Msg->transfer_length, USBWARP_INLINE_DATA_SIZE);
        RtlCopyMemory(urbCtx->InlineData, Msg->inline_data, copyLen);
        urbCtx->InlineDataLen = copyLen;
    }

    /* Add to pending list. */
    KeClearEvent(&devCtx->DrainEvent);
    KeAcquireSpinLock(&devCtx->PendingLock, &oldIrql);
    InsertTailList(&devCtx->PendingUrbs, &urbCtx->ListEntry);
    InterlockedIncrement(&devCtx->PendingCount);
    KeReleaseSpinLock(&devCtx->PendingLock, oldIrql);

    /* ── Build filter IOCTL request ─────────────────────────────────────── */

    /* Allocate the IOCTL input buffer (FILTER_URB_REQUEST + optional inline data). */
    ULONG reqSize = sizeof(USBWARP_FILTER_URB_REQUEST);
    ULONG inlineLen = 0;
    if (Msg->data_mode == USBWARP_DATA_INLINE && Msg->direction == 0 &&
        urbCtx->InlineDataLen > 0) {
        inlineLen = urbCtx->InlineDataLen;
        reqSize += inlineLen;
    }

    PUCHAR reqBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                              reqSize, USBWARP_DRIVER_TAG);
    if (!reqBuf) goto SubmitFail;

    {
        USBWARP_FILTER_URB_REQUEST *req = (USBWARP_FILTER_URB_REQUEST *)reqBuf;
        RtlZeroMemory(req, sizeof(*req));

        req->TransferType   = Msg->transfer_type;
        req->Endpoint       = Msg->endpoint;
        req->Direction      = Msg->direction;
        req->TransferLength = Msg->transfer_length;
        req->TransferFlags  = (Msg->direction ? USBD_TRANSFER_DIRECTION_IN : 0) |
                              USBD_SHORT_TRANSFER_OK;

        /* Pass real pipe handle for BULK/INTERRUPT. */
        if (Msg->transfer_type == USBWARP_XFER_BULK ||
            Msg->transfer_type == USBWARP_XFER_INTERRUPT) {
            USBD_PIPE_HANDLE ph = FindPipe(devCtx, (UCHAR)Msg->endpoint,
                                            (UCHAR)Msg->direction);
            req->PipeHandle = (uint64_t)(ULONG_PTR)ph;
        }

        if (Msg->transfer_type == USBWARP_XFER_CONTROL)
            RtlCopyMemory(req->SetupPacket, Msg->setup_packet, 8);

        /* Zero-copy path: pass SHM kernel VA directly.
         * The filter will use this as the URB TransferBuffer. */
        if (Msg->data_mode == USBWARP_DATA_BUFFER && transferVa) {
            req->KernelVa = (ULONG_PTR)transferVa;
        } else if (inlineLen > 0) {
            req->InlineDataOffset = sizeof(*req);
            req->InlineDataLength = inlineLen;
            RtlCopyMemory(reqBuf + sizeof(*req), urbCtx->InlineData, inlineLen);
        }
    }

    /* Create WDF request. */
    status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES,
                              devCtx->FilterTarget, &urbCtx->WdfRequest);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(reqBuf, USBWARP_DRIVER_TAG);
        goto SubmitFail;
    }

    /* Create input memory. */
    status = WdfMemoryCreatePreallocated(WDF_NO_OBJECT_ATTRIBUTES,
                                          reqBuf, reqSize, &urbCtx->UrbMemory);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(reqBuf, USBWARP_DRIVER_TAG);
        goto SubmitFail;
    }
    /* NOTE: WdfMemoryCreatePreallocated does NOT free the buffer on delete.
     * Store the pointer for cleanup in completion/error path. */
    urbCtx->FilterReqBuf = reqBuf;

    /* Create output memory for response. */
    ULONG outSize = sizeof(USBWARP_FILTER_URB_RESPONSE) +
                    (Msg->direction ? Msg->transfer_length : 0);
    PVOID outBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, outSize,
                                    USBWARP_DRIVER_TAG);
    if (!outBuf) goto SubmitFail;

    /* Store in urbCtx so completion routine can read directly. */
    urbCtx->FilterOutBuf  = outBuf;
    urbCtx->FilterOutSize = outSize;

    status = WdfMemoryCreatePreallocated(WDF_NO_OBJECT_ATTRIBUTES,
                                          outBuf, outSize, &urbCtx->OutMemory);
    if (!NT_SUCCESS(status)) {
        urbCtx->FilterOutBuf = NULL;  /* prevent double free */
        ExFreePoolWithTag(outBuf, USBWARP_DRIVER_TAG);
        goto SubmitFail;
    }

    /* Format the IOCTL request. */
    status = WdfIoTargetFormatRequestForIoctl(
                 devCtx->FilterTarget, urbCtx->WdfRequest,
                 IOCTL_USBWARP_FILTER_SUBMIT_URB,
                 urbCtx->UrbMemory, NULL, urbCtx->OutMemory, NULL);
    if (!NT_SUCCESS(status)) {
        goto SubmitFail;
    }

    WdfRequestSetCompletionRoutine(urbCtx->WdfRequest,
                                   UrbProxyCompletion, urbCtx);

    if (!WdfRequestSend(urbCtx->WdfRequest, devCtx->FilterTarget, NULL)) {
        status = WdfRequestGetStatus(urbCtx->WdfRequest);
        goto SubmitFail;
    }

    return;

SubmitFail:
    SendUrbComplete(Ctx, urbCtx, USBWARP_STATUS_NO_RESOURCE, 0);

    KeAcquireSpinLock(&devCtx->PendingLock, &oldIrql);
    RemoveEntryList(&urbCtx->ListEntry);
    KeReleaseSpinLock(&devCtx->PendingLock, oldIrql);
    DecrementPending(devCtx);

    if (urbCtx->FilterOutBuf)
        ExFreePoolWithTag(urbCtx->FilterOutBuf, USBWARP_DRIVER_TAG);
    if (urbCtx->FilterReqBuf)
        ExFreePoolWithTag(urbCtx->FilterReqBuf, USBWARP_DRIVER_TAG);
    if (urbCtx->OutMemory)  WdfObjectDelete(urbCtx->OutMemory);
    if (urbCtx->UrbMemory)  WdfObjectDelete(urbCtx->UrbMemory);
    if (urbCtx->WdfRequest) WdfObjectDelete(urbCtx->WdfRequest);
    ExFreePoolWithTag(urbCtx, USBWARP_DRIVER_TAG);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Send MSG_URB_COMPLETE to Guest
 * ═══════════════════════════════════════════════════════════════════════════ */

static VOID
SendUrbComplete(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_opt_ PUSBWARP_URB_CONTEXT UrbCtx,
    _In_ int32_t ProtoStatus,
    _In_ ULONG ActualLength
    )
{
    struct usbwarp_msg_urb_complete comp;
    ULONG msgLen;
    BOOLEAN useInline = FALSE;

    RtlZeroMemory(&comp, sizeof(comp));

    if (UrbCtx && UrbCtx->Direction == 1 && ActualLength > 0 &&
        ActualLength <= USBWARP_INLINE_DATA_SIZE &&
        UrbCtx->DataMode == USBWARP_DATA_INLINE)
        useInline = TRUE;

    msgLen = useInline ?
        USBWARP_MSG_URB_COMPLETE_BASE_SIZE + ActualLength :
        USBWARP_MSG_URB_COMPLETE_BASE_SIZE;

    comp.hdr.magic            = USBWARP_MSG_MAGIC;
    comp.hdr.message_type     = USBWARP_MSG_URB_COMPLETE;
    comp.hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
    comp.hdr.message_length   = msgLen;
    comp.hdr.transaction_id   = UrbCtx ? UrbCtx->TransactionId : 0;
    comp.hdr.device_id        = UrbCtx ? UrbCtx->DeviceIndex : 0;
    comp.device_id     = UrbCtx ? UrbCtx->DeviceIndex : 0;
    comp.endpoint      = UrbCtx ? UrbCtx->Endpoint : 0;
    comp.status        = ProtoStatus;
    comp.actual_length = ActualLength;

    if (useInline) {
        comp.data_mode = USBWARP_DATA_INLINE;
        RtlCopyMemory(comp.inline_data, UrbCtx->InlineData, ActualLength);
    } else if (UrbCtx && UrbCtx->DataMode == USBWARP_DATA_BUFFER) {
        comp.data_mode     = USBWARP_DATA_BUFFER;
        comp.buffer_offset = UrbCtx->BufferOffset;
    } else {
        comp.data_mode = USBWARP_DATA_NONE;
    }

    UsbWarpRingProduce(&Ctx->H2gRing, &comp, msgLen);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  URB cancel
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpProcessUrbCancel(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ const struct usbwarp_msg_urb_cancel *Msg
    )
{
    PUSBWARP_DEVICE_CONTEXT devCtx;
    PUSBWARP_URB_CONTEXT    urbCtx;
    PLIST_ENTRY             entry;
    ULONG                   devIdx = Msg->device_id;
    ULONG                   txnId  = Msg->hdr.transaction_id;
    KIRQL                   oldIrql;
    BOOLEAN                 found  = FALSE;
    struct usbwarp_msg_urb_cancel_ack ack;

    WDFREQUEST              reqToCancel = NULL;

    if (devIdx == 0 || devIdx > USBWARP_MAX_DEVICES_LIMIT) return;
    devCtx = &Ctx->Devices[devIdx - 1];
    if (!devCtx->InUse) return;

    KeAcquireSpinLock(&devCtx->PendingLock, &oldIrql);
    for (entry = devCtx->PendingUrbs.Flink;
         entry != &devCtx->PendingUrbs;
         entry = entry->Flink) {
        urbCtx = CONTAINING_RECORD(entry, USBWARP_URB_CONTEXT, ListEntry);
        if (urbCtx->TransactionId == txnId) {
            if (InterlockedCompareExchange(&urbCtx->State,
                    UURB_STATE_CANCELING, UURB_STATE_ACTIVE) ==
                UURB_STATE_ACTIVE) {
                found = TRUE;
                reqToCancel = urbCtx->WdfRequest;
            }
            break;
        }
    }
    KeReleaseSpinLock(&devCtx->PendingLock, oldIrql);

    /* Cancel OUTSIDE the lock.  WdfRequestCancelSentRequest may
     * synchronously complete the request, which calls UrbProxyCompletion,
     * which acquires PendingLock.  If we held the lock here → deadlock. */
    if (reqToCancel)
        WdfRequestCancelSentRequest(reqToCancel);

    RtlZeroMemory(&ack, sizeof(ack));
    ack.hdr.magic            = USBWARP_MSG_MAGIC;
    ack.hdr.message_type     = USBWARP_MSG_URB_CANCEL_ACK;
    ack.hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
    ack.hdr.message_length   = sizeof(ack);
    ack.hdr.transaction_id   = txnId;
    ack.hdr.device_id        = devIdx;
    ack.status = found ? 0 : (int32_t)USBWARP_STATUS_ACCESS_DENIED;

    UsbWarpRingProduce(&Ctx->H2gRing, &ack, sizeof(ack));
}
