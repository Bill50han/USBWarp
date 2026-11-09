/*
 * usbwarp_device.c — USB device management via UsbWarpFilter.sys proxy.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * ARCHITECTURE:
 *   UsbWarp.sys (Non-PnP control device) cannot directly submit URBs to
 *   USB device stacks.  All USB I/O is proxied through UsbWarpFilter.sys,
 *   the selected device's dedicated KMDF function driver, which holds a
 *   registered USBD_HANDLE and one kernel file lease.
 *
 *   UsbWarp.sys opens the new capture interface after the service completes
 *   PnP driver replacement and port reset, then claims its lease:
 *     - IOCTL_USBWARP_FILTER_GET_DESCRIPTOR → proxy descriptor reads
 *     - IOCTL_USBWARP_FILTER_SELECT_CONFIG  → configure + return pipe map
 *     - IOCTL_USBWARP_FILTER_SUBMIT_URB     → proxy URB submissions
 *
 *   Zero-copy is preserved: the filter passes our SHM kernel VA directly
 *   as the URB TransferBuffer to the USB stack.
 */

#include "usbwarp_drv.h"
#include "../include/usbwarp_filter_ioctl.h"

static PUSBWARP_COMPLETION AllocateCompletion(ULONG Length)
{
    PUSBWARP_COMPLETION reply = ExAllocatePool2(POOL_FLAG_NON_PAGED,
        FIELD_OFFSET(USBWARP_COMPLETION, Data) + Length, USBWARP_DRIVER_TAG);
    if (reply) reply->Length = Length;
    return reply;
}

static VOID QueueMessage(PUSBWARP_GLOBAL_CONTEXT Ctx, const VOID *Message, ULONG Length)
{
    PUSBWARP_COMPLETION reply = AllocateCompletion(Length);
    if (!reply && Ctx->ErrorReply) {
        reply = Ctx->ErrorReply;
        Ctx->ErrorReply = NULL;
        reply->Length = Length;
    }
    if (!reply) {
        /* The small admission-error reply can still use already mapped ring
         * storage under pool pressure. Stop admitting work if even it is full. */
        if (!NT_SUCCESS(UsbWarpRingProduce(&Ctx->H2gRing, Message, Length)))
            InterlockedExchange(&Ctx->GlobalBreakerOpen, TRUE);
        return;
    }
    RtlCopyMemory(reply->Data, Message, Length);
    UsbWarpQueueCompletion(Ctx, reply);
}

static EVT_WDF_REQUEST_COMPLETION_ROUTINE UrbProxyCompletion;

/* PnP hot-plug removal callbacks */
static EVT_WDF_IO_TARGET_QUERY_REMOVE    UsbWarpEvtTargetQueryRemove;
static EVT_WDF_IO_TARGET_REMOVE_CANCELED UsbWarpEvtTargetRemoveCanceled;
static EVT_WDF_IO_TARGET_REMOVE_COMPLETE UsbWarpEvtTargetRemoveComplete;

static VOID SendUrbComplete(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ PUSBWARP_URB_CONTEXT UrbCtx,
    _In_ int32_t ProtoStatus,
    _In_ ULONG ActualLength);

static VOID SendUrbSubmitError(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ const struct usbwarp_msg_urb_submit *Msg,
    _In_ int32_t ProtoStatus);

static VOID DecrementPending(PUSBWARP_DEVICE_CONTEXT DevCtx)
{
    LONG newCount = InterlockedDecrement(&DevCtx->PendingCount);
    if (newCount == 0)
        KeSetEvent(&DevCtx->DrainEvent, IO_NO_INCREMENT, FALSE);
}

static VOID
SendUrbSubmitError(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ const struct usbwarp_msg_urb_submit *Msg,
    _In_ int32_t ProtoStatus
    )
{
    if (Msg->hdr.message_type == USBWARP_MSG_ISO_SUBMIT) {
        struct usbwarp_msg_iso_complete comp = { 0 };
        comp.hdr = Msg->hdr;
        comp.hdr.message_type = USBWARP_MSG_ISO_COMPLETE;
        comp.hdr.message_length = sizeof(comp);
        comp.device_id = Msg->hdr.device_id;
        comp.endpoint = Msg->endpoint;
        comp.status = ProtoStatus;
        QueueMessage(Ctx, &comp, sizeof(comp));
    } else {
        struct usbwarp_msg_urb_complete comp = { 0 };
        comp.hdr = Msg->hdr;
        comp.hdr.message_type = USBWARP_MSG_URB_COMPLETE;
        comp.hdr.message_length = USBWARP_MSG_URB_COMPLETE_BASE_SIZE;
        comp.device_id = Msg->hdr.device_id;
        comp.endpoint = Msg->endpoint;
        comp.status = ProtoStatus;
        QueueMessage(Ctx, &comp, comp.hdr.message_length);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Synchronous filter IOCTL helper
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
SendFilterIoctlSyncEx(
    _In_ WDFIOTARGET Target,
    _In_ ULONG IoControlCode,
    _In_opt_ PVOID InputBuffer,
    _In_ ULONG InputLength,
    _Out_writes_bytes_opt_(OutputLength) PVOID OutputBuffer,
    _In_ ULONG OutputLength,
    _Out_opt_ PULONG BytesReturned,
    BOOLEAN AllowStopped
    )
{
    NTSTATUS    status;
    WDFREQUEST  request = NULL;
    WDFMEMORY   inMem = NULL, outMem = NULL;

    if (BytesReturned) *BytesReturned = 0;
    if (OutputBuffer && OutputLength) RtlZeroMemory(OutputBuffer, OutputLength);

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
    WDF_REQUEST_SEND_OPTIONS_INIT(&opts, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS |
        (AllowStopped ? WDF_REQUEST_SEND_OPTION_IGNORE_TARGET_STATE : 0));
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

static NTSTATUS SendFilterIoctlSync(WDFIOTARGET Target, ULONG Code, PVOID Input, ULONG InputLength,
                                    PVOID Output, ULONG OutputLength, PULONG Returned)
{
    return SendFilterIoctlSyncEx(Target, Code, Input, InputLength, Output, OutputLength, Returned, FALSE);
}

/* Resolve the physical port by parent devnode and driver key. Unlike a CLI
 * speed hint this works when the caller cannot open hub interfaces, and never
 * confuses a device's bcdUSB capability with its currently negotiated link. */
static UCHAR QueryConnectionSpeed(PUSBWARP_GLOBAL_CONTEXT Ctx, WDFIOTARGET DeviceTarget)
{
    static const GUID hubGuid = { 0xf18a0e88, 0xc30c, 0x11d0,
        {0x88,0x15,0,0xa0,0xc9,0x06,0xbe,0xd8} };
    WCHAR parentId[256] = {0}, driverKey[256] = {0};
    PDEVICE_OBJECT object = WdfIoTargetWdmGetTargetDeviceObject(DeviceTarget), pdo;
    PWSTR links = NULL;
    DEVPROPTYPE type;
    ULONG required;
    UCHAR speed = 0;
    UNICODE_STRING parent, key;
    if (!object) return 0;
    pdo = IoGetDeviceAttachmentBaseRef(object);
    if (!pdo) return 0;
    if (!NT_SUCCESS(IoGetDevicePropertyData(pdo, &DEVPKEY_Device_Parent, LOCALE_NEUTRAL, 0,
            sizeof(parentId), parentId, &required, &type)) || type != DEVPROP_TYPE_STRING ||
        !NT_SUCCESS(IoGetDevicePropertyData(pdo, &DEVPKEY_Device_Driver, LOCALE_NEUTRAL, 0,
            sizeof(driverKey), driverKey, &required, &type)) || type != DEVPROP_TYPE_STRING) {
        ObDereferenceObject(pdo);
        return 0;
    }
    ObDereferenceObject(pdo);
    parentId[255] = driverKey[255] = 0;
    RtlInitUnicodeString(&parent, parentId);
    RtlInitUnicodeString(&key, driverKey);
    if (!NT_SUCCESS(IoGetDeviceInterfaces(&hubGuid, NULL, 0, &links))) return 0;
    for (PWSTR path = links; *path && !speed; path += wcslen(path) + 1) {
        WDFIOTARGET hub;
        WDF_IO_TARGET_OPEN_PARAMS options;
        UNICODE_STRING name, id;
        WCHAR hubId[256] = {0};
        USB_HUB_INFORMATION_EX hubInfo;
        ULONG bytes = 0;
        BOOLEAN match = FALSE;
        if (!NT_SUCCESS(WdfIoTargetCreate(Ctx->ControlDevice, WDF_NO_OBJECT_ATTRIBUTES, &hub))) continue;
        RtlInitUnicodeString(&name, path);
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&options, &name, GENERIC_READ | GENERIC_WRITE);
        options.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
        if (!NT_SUCCESS(WdfIoTargetOpen(hub, &options))) { WdfObjectDelete(hub); continue; }
        object = WdfIoTargetWdmGetTargetDeviceObject(hub);
        pdo = object ? IoGetDeviceAttachmentBaseRef(object) : NULL;
        if (pdo) {
            if (NT_SUCCESS(IoGetDevicePropertyData(pdo, &DEVPKEY_Device_InstanceId, LOCALE_NEUTRAL, 0,
                    sizeof(hubId), hubId, &required, &type)) && type == DEVPROP_TYPE_STRING) {
                hubId[255] = 0;
                RtlInitUnicodeString(&id, hubId);
                match = RtlEqualUnicodeString(&id, &parent, TRUE);
            }
            ObDereferenceObject(pdo);
        }
        if (match && NT_SUCCESS(SendFilterIoctlSync(hub, IOCTL_USB_GET_HUB_INFORMATION_EX,
                NULL, 0, &hubInfo, sizeof(hubInfo), &bytes)) && bytes >= sizeof(hubInfo)) {
            for (ULONG port = 1; port <= hubInfo.HighestPortNumber && port <= 255; ++port) {
                USB_NODE_CONNECTION_DRIVERKEY_NAME in = {0};
                struct { USB_NODE_CONNECTION_DRIVERKEY_NAME key; WCHAR extra[256]; } out;
                UNICODE_STRING connectedKey;
                USB_NODE_CONNECTION_INFORMATION_EX ci = {0}, ciOut;
                USB_NODE_CONNECTION_INFORMATION_EX_V2 v2 = {0}, v2Out;
                in.ConnectionIndex = port;
                if (!NT_SUCCESS(SendFilterIoctlSync(hub, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME,
                        &in, sizeof(in), &out, sizeof(out), &bytes)) ||
                    bytes < FIELD_OFFSET(USB_NODE_CONNECTION_DRIVERKEY_NAME, DriverKeyName) + sizeof(WCHAR) ||
                    out.key.ActualLength > sizeof(out)) continue;
                out.extra[255] = 0;
                RtlInitUnicodeString(&connectedKey, out.key.DriverKeyName);
                if (!RtlEqualUnicodeString(&connectedKey, &key, TRUE)) continue;
                ci.ConnectionIndex = port;
                if (NT_SUCCESS(SendFilterIoctlSync(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                        &ci, sizeof(ci), &ciOut, sizeof(ciOut), &bytes)) && bytes >= sizeof(ciOut)) {
                    if (ciOut.Speed <= 2) speed = (UCHAR)(ciOut.Speed + 1);
                    else if (ciOut.Speed == 3) speed = USBWARP_SPEED_SUPER;
                    else if (ciOut.Speed == 4) speed = USBWARP_SPEED_SUPER_PLUS;
                }
                v2.ConnectionIndex = port;
                v2.Length = sizeof(v2);
                v2.SupportedUsbProtocols.Usb300 = 1;
                if (NT_SUCCESS(SendFilterIoctlSync(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2,
                        &v2, sizeof(v2), &v2Out, sizeof(v2Out), &bytes)) && bytes >= sizeof(v2Out)) {
                    if (v2Out.Flags.DeviceIsOperatingAtSuperSpeedPlusOrHigher) speed = USBWARP_SPEED_SUPER_PLUS;
                    else if (v2Out.Flags.DeviceIsOperatingAtSuperSpeedOrHigher) speed = USBWARP_SPEED_SUPER;
                }
                if (usbwarp_speed_is_super(speed)) {
                    USB_NODE_CONNECTION_SUPERSPEEDPLUS_INFORMATION plus = {0}, plusOut;
                    plus.ConnectionIndex = port;
                    plus.Length = sizeof(plus);
                    if (NT_SUCCESS(SendFilterIoctlSync(hub, IOCTL_USB_GET_NODE_CONNECTION_SUPERSPEEDPLUS_INFORMATION,
                            &plus, sizeof(plus), &plusOut, sizeof(plusOut), &bytes)) && bytes >= sizeof(plusOut)) {
                        uint32_t mode = usbwarp_usb3_decode_link(plusOut.RxSuperSpeedPlus.AsUlong32,
                            plusOut.RxLaneCount, plusOut.TxSuperSpeedPlus.AsUlong32, plusOut.TxLaneCount);
                        if (mode) speed = (UCHAR)mode;
                    }
                }
                break;
            }
        }
        WdfIoTargetClose(hub);
        WdfObjectDelete(hub);
    }
    ExFreePool(links);
    return speed;
}

static VOID CloseTargetStreams(WDFIOTARGET Target)
{
    USBWARP_FILTER_STREAMS_REQUEST req = { 0xfffefffeu, 0 };
    USBWARP_FILTER_STREAMS_RESPONSE resp;
    SendFilterIoctlSyncEx(Target, IOCTL_USBWARP_FILTER_STREAMS, &req, sizeof(req),
                         &resp, sizeof(resp), NULL, TRUE);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §1b  PnP hot-plug removal callbacks
 *
 *   When the physical USB device is unplugged while bound, WDF invokes
 *   these callbacks on the IoTarget.  This enables automatic unbind
 *   without user intervention.
 *
 *   Flow: unplug → PnP SURPRISE_REMOVAL → WDF detects target gone
 *         → QueryRemove → RemoveComplete → we auto-unbind
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
UsbWarpEvtTargetQueryRemove(WDFIOTARGET Target)
{
    /* PnP is asking "can this device be removed?"  Always agree —
     * we're a passthrough consumer, not a storage volume. */
    KdPrint(("UsbWarp: PnP QueryRemove for target %p\n", Target));
    WdfIoTargetCloseForQueryRemove(Target);
    return STATUS_SUCCESS;
}

static VOID UsbWarpEvtTargetRemoveComplete(WDFIOTARGET Target);

static VOID
UsbWarpEvtTargetRemoveCanceled(WDFIOTARGET Target)
{
    /* QueryRemove already closed the capture lease. It cannot be revived:
     * remove the guest device and let the service reset/restore Windows. */
    UsbWarpEvtTargetRemoveComplete(Target);
}

static VOID
UsbWarpEvtTargetRemoveComplete(WDFIOTARGET Target)
{
    PUSBWARP_TARGET_CONTEXT tgtCtx = UsbWarpGetTargetContext(Target);
    PUSBWARP_GLOBAL_CONTEXT ctx;
    PUSBWARP_DEVICE_CONTEXT devCtx;
    ULONG idx;

    if (!tgtCtx || !tgtCtx->GlobalCtx)
        return;

    ctx = tgtCtx->GlobalCtx;
    idx = tgtCtx->DeviceIndex;

    if (idx == 0 || idx > USBWARP_MAX_DEVICES_LIMIT)
        return;

    devCtx = &ctx->Devices[idx - 1];

    {
        KIRQL irql;
        BOOLEAN opening;
        KeAcquireSpinLock(&ctx->DeviceLock, &irql);
        if (devCtx->Removing || devCtx->Epoch != tgtCtx->Epoch || devCtx->FilterTarget != Target) {
            KeReleaseSpinLock(&ctx->DeviceLock, irql);
            return;
        }
        devCtx->Removing = 1;
        opening = !devCtx->InUse;
        KeReleaseSpinLock(&ctx->DeviceLock, irql);
        if (opening) {
            /* Bind owns the object until it publishes InUse. Leave Removing
             * set so it aborts instead of exporting an already-removed device. */
            KeWaitForSingleObject(&tgtCtx->StateMutex, Executive, KernelMode, FALSE, NULL);
            WdfIoTargetClose(Target);
            KeReleaseMutex(&tgtCtx->StateMutex, FALSE);
            return;
        }
    }

    KdPrint(("UsbWarp: HOT-REMOVE auto-unbind slot=%u VID=%04x PID=%04x\n",
             idx, devCtx->VendorId, devCtx->ProductId));

    UsbWarpDeviceClose(ctx, devCtx);
    UsbWarpSendDeviceRemoved(ctx, devCtx, 1 /* hot_remove: physical I/O drained */);

    /* Publish the now-closed slot to ENUM and the bind allocator. */
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&ctx->DeviceLock, &oldIrql);
        if (devCtx->InUse) {
            devCtx->InUse = FALSE;
            InterlockedDecrement(&ctx->ActiveDeviceCount);
        }
        InterlockedExchange(&devCtx->Removing, 0);
        KeReleaseSpinLock(&ctx->DeviceLock, oldIrql);
    }

    KdPrint(("UsbWarp: hot-remove complete, slot %u freed\n", idx));
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
     * USB device stack. UsbWarpFilter.sys (dedicated capture FDO) handles
     * our custom IOCTLs; normal IOCTLs pass through to the function driver. */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, USBWARP_TARGET_CONTEXT);
    attrs.ParentObject = Ctx->ControlDevice;

    status = WdfIoTargetCreate(Ctx->ControlDevice, &attrs, &DevCtx->FilterTarget);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: WdfIoTargetCreate failed 0x%x\n", status));
        return status;
    }

    /* Store back-pointer so PnP callbacks can find the device context. */
    {
        PUSBWARP_TARGET_CONTEXT tgtCtx =
            UsbWarpGetTargetContext(DevCtx->FilterTarget);
        tgtCtx->GlobalCtx   = Ctx;
        tgtCtx->DeviceIndex = DevCtx->DeviceIndex;
        tgtCtx->Epoch = DevCtx->Epoch;
        KeInitializeMutex(&tgtCtx->StateMutex, 0);
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &openParams, &DevCtx->InstancePath,
        GENERIC_READ | GENERIC_WRITE);
    openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;

    /* Register PnP removal callbacks for hot-plug auto-unbind.
     * When the physical USB device is unplugged, WDF invokes these
     * instead of leaving the IoTarget in a silently broken state. */
    openParams.EvtIoTargetQueryRemove    = UsbWarpEvtTargetQueryRemove;
    openParams.EvtIoTargetRemoveCanceled = UsbWarpEvtTargetRemoveCanceled;
    openParams.EvtIoTargetRemoveComplete = UsbWarpEvtTargetRemoveComplete;

    status = WdfIoTargetOpen(DevCtx->FilterTarget, &openParams);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: WdfIoTargetOpen failed 0x%x (filter not installed?)\n",
                 status));
        WdfObjectDelete(DevCtx->FilterTarget);
        DevCtx->FilterTarget = NULL;
        return status;
    }

    KdPrint(("UsbWarp: filter target opened for %wZ\n", &DevCtx->InstancePath));

    /* One kernel file lease per PnP incarnation. An obsolete transparent
     * class filter must never be usable for binding. */
    {
        USBWARP_FILTER_CLAIM claim = { USBWARP_CAPTURE_VERSION, 0 };
        status = SendFilterIoctlSync(DevCtx->FilterTarget, IOCTL_USBWARP_FILTER_CLAIM,
            &claim, sizeof(claim), NULL, 0, NULL);
        if (!NT_SUCCESS(status)) {
            UsbWarpDeviceClose(Ctx, DevCtx);
            return status;
        }
    }

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
            UsbWarpDeviceClose(Ctx, DevCtx);
            return NT_SUCCESS(status) ? STATUS_DEVICE_DATA_ERROR : status;
        }
    }

    /* ── Step 2: SELECT_CONFIG via filter → get pipe map ───────────────── */
    {
        USBWARP_FILTER_SELECT_CONFIG_REQUEST req;
        USBWARP_FILTER_PIPES_RESPONSE resp;
        ULONG returned = 0;

        RtlZeroMemory(&req, sizeof(req));
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
            for (ULONG i = 0; i < resp.NumPipes && i < USBWARP_FILTER_MAX_PIPES; i++) {
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
            UsbWarpDeviceClose(Ctx, DevCtx);
            return NT_SUCCESS(status) ? STATUS_DEVICE_DATA_ERROR : status;
        }
    }

    {
        UCHAR actualSpeed = QueryConnectionSpeed(Ctx, DevCtx->FilterTarget);
        if (actualSpeed) {
            DevCtx->Speed = actualSpeed;
            DevCtx->SpeedQueried = TRUE;
        }
    }

    if (DevCtx->Removing || WdfIoTargetGetState(DevCtx->FilterTarget) != WdfIoTargetStarted) {
        UsbWarpDeviceClose(Ctx, DevCtx);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    KdPrint(("UsbWarp: device opened slot=%u VID=%04x PID=%04x pipes=%u\n",
             DevCtx->DeviceIndex, DevCtx->VendorId, DevCtx->ProductId,
             DevCtx->NumPipes));

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Device close
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID UsbWarpPurgeTarget(WDFIOTARGET Target)
{
    PUSBWARP_TARGET_CONTEXT tc = UsbWarpGetTargetContext(Target);
    KeWaitForSingleObject(&tc->StateMutex, Executive, KernelMode, FALSE, NULL);
    WdfIoTargetPurge(Target, WdfIoTargetPurgeIoAndWait);
    CloseTargetStreams(Target);
    KeReleaseMutex(&tc->StateMutex, FALSE);
}

VOID
UsbWarpDeviceClose(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx, _In_ PUSBWARP_DEVICE_CONTEXT DevCtx)
{
    WDFIOTARGET target;
    PUSBWARP_TARGET_CONTEXT tc;
    KIRQL irql;
    BOOLEAN owned = FALSE;
    InterlockedExchange(&DevCtx->Removing, 1);
    KeAcquireSpinLock(&Ctx->DeviceLock, &irql);
    target = DevCtx->FilterTarget;
    if (target) WdfObjectReference(target);
    KeReleaseSpinLock(&Ctx->DeviceLock, irql);
    if (!target) return;
    tc = UsbWarpGetTargetContext(target);
    KeWaitForSingleObject(&tc->StateMutex, Executive, KernelMode, FALSE, NULL);
    /* Another teardown may have closed this referenced target while we waited. */
    if (DevCtx->FilterTarget == target) {
        ExWaitForRundownProtectionRelease(&DevCtx->SubmitRundown);
        WdfIoTargetPurge(target, WdfIoTargetPurgeIoAndWait);
        if (InterlockedCompareExchange(&DevCtx->PendingCount, 0, 0) > 0)
            KeWaitForSingleObject(&DevCtx->DrainEvent, Executive, KernelMode, FALSE, NULL);
        CloseTargetStreams(target);
        WdfIoTargetClose(target);
        KeAcquireSpinLock(&Ctx->DeviceLock, &irql);
        DevCtx->FilterTarget = NULL;
        DevCtx->NumPipes = 0;
        KeReleaseSpinLock(&Ctx->DeviceLock, irql);
        owned = TRUE;
    }
    KeReleaseMutex(&tc->StateMutex, FALSE);
    if (owned) WdfObjectDelete(target);
    WdfObjectDereference(target);
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

static int32_t ProtoStatus(NTSTATUS Nt, USBD_STATUS Usb)
{
    if (Nt == STATUS_NOT_SUPPORTED) return USBWARP_STATUS_NOT_SUPPORTED;
    if (Nt == STATUS_CANCELLED || Usb == USBD_STATUS_CANCELED) return USBWARP_STATUS_CANCELLED;
    if (Nt == STATUS_DEVICE_NOT_CONNECTED || Nt == STATUS_NO_SUCH_DEVICE) return USBWARP_STATUS_DISCONNECTED;
    if (Nt == STATUS_INSUFFICIENT_RESOURCES || Usb == USBD_STATUS_INSUFFICIENT_RESOURCES)
        return USBWARP_STATUS_NO_RESOURCE;
    if (Nt == STATUS_IO_TIMEOUT || Usb == USBD_STATUS_TIMEOUT) return USBWARP_STATUS_TIMEOUT;
    if (Usb == USBD_STATUS_STALL_PID || Usb == USBD_STATUS_ENDPOINT_HALTED) return USBWARP_STATUS_STALL;
    if (Usb == USBD_STATUS_DATA_OVERRUN || Usb == USBD_STATUS_BABBLE_DETECTED) return USBWARP_STATUS_OVERFLOW;
    if (Usb == USBD_STATUS_CRC) return USBWARP_STATUS_CRC;
    if (Usb == USBD_STATUS_NOT_ACCESSED || Usb == USBD_STATUS_ISO_NOT_ACCESSED_BY_HW ||
        Usb == USBD_STATUS_ISO_NA_LATE_USBPORT || Usb == USBD_STATUS_ISO_NOT_ACCESSED_LATE ||
        Usb == USBD_STATUS_BAD_START_FRAME) return USBWARP_STATUS_ISO_MISSED;
    return NT_SUCCESS(Nt) && USBD_SUCCESS(Usb) ? USBWARP_STATUS_SUCCESS : USBWARP_STATUS_PROTOCOL_ERROR;
}

static VOID FreeProxy(PUSBWARP_URB_CONTEXT U)
{
    if (U->WdfRequest) WdfObjectDelete(U->WdfRequest);
    if (U->OutMemory) WdfObjectDelete(U->OutMemory);
    if (U->UrbMemory) WdfObjectDelete(U->UrbMemory);
    if (U->FilterOutBuf) ExFreePoolWithTag(U->FilterOutBuf, USBWARP_DRIVER_TAG);
    if (U->FilterReqBuf) ExFreePoolWithTag(U->FilterReqBuf, USBWARP_DRIVER_TAG);
    if (U->Completion) ExFreePoolWithTag(U->Completion, USBWARP_DRIVER_TAG);
    ExFreePoolWithTag(U, USBWARP_DRIVER_TAG);
}

static VOID SendUrbComplete(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx, _In_ PUSBWARP_URB_CONTEXT U,
                           _In_ int32_t Status, _In_ ULONG Actual)
{
    struct usbwarp_msg_urb_complete *c = (void *)U->Completion->Data;
    ULONG len = USBWARP_MSG_URB_COMPLETE_BASE_SIZE;
    RtlZeroMemory(c, sizeof(*c));
    c->hdr.magic = USBWARP_MSG_MAGIC;
    c->hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
    c->hdr.message_type = USBWARP_MSG_URB_COMPLETE;
    c->hdr.transaction_id = U->TransactionId;
    c->hdr.flags = U->Epoch;
    c->hdr.device_id = c->device_id = U->DeviceIndex;
    c->endpoint = U->Endpoint;
    c->status = Status;
    c->actual_length = Actual;
    c->data_mode = U->DataMode;
    c->buffer_offset = U->BufferOffset;
    if (U->Direction && U->DataMode == USBWARP_DATA_INLINE && Actual) {
        RtlCopyMemory(c->inline_data, U->InlineData, Actual);
        len += Actual;
    }
    c->hdr.message_length = len;
    U->Completion->Length = len;
    UsbWarpQueueCompletion(Ctx, U->Completion);
    U->Completion = NULL;
}

static VOID SendIsoComplete(PUSBWARP_URB_CONTEXT U, int32_t Status,
                           const USBWARP_FILTER_ISO_RESPONSE *Response)
{
    struct usbwarp_msg_iso_complete *c = (void *)U->Completion->Data;
    struct usbwarp_iso_packet_result *out = usbwarp_iso_results(c);
    const struct usbwarp_iso_packet_result *in = Response ? (const void *)(Response + 1) : NULL;
    RtlZeroMemory(c, sizeof(*c));
    c->hdr.magic = USBWARP_MSG_MAGIC;
    c->hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
    c->hdr.message_type = USBWARP_MSG_ISO_COMPLETE;
    c->hdr.message_length = usbwarp_iso_message_size(U->NumberOfPackets);
    c->hdr.transaction_id = U->TransactionId;
    c->hdr.flags = U->Epoch;
    c->hdr.device_id = c->device_id = U->DeviceIndex;
    c->endpoint = U->Endpoint;
    c->status = Status;
    c->number_of_packets = U->NumberOfPackets;
    c->data_mode = USBWARP_DATA_BUFFER;
    c->buffer_offset = U->BufferOffset;
    c->start_frame = Response ? Response->StartFrame : 0;
    for (ULONG i = 0; i < U->NumberOfPackets; ++i) {
        out[i].status = in ? ProtoStatus(STATUS_SUCCESS, in[i].status) : Status;
        out[i].actual_length = in ? in[i].actual_length : 0;
        if (out[i].status) ++c->error_count;
    }
    U->Completion->Length = c->hdr.message_length;
    UsbWarpQueueCompletion(U->GlobalCtx, U->Completion);
    U->Completion = NULL;
}

static VOID UrbProxyCompletion(WDFREQUEST Request, WDFIOTARGET Target,
                              PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    PUSBWARP_URB_CONTEXT u = Context;
    PUSBWARP_GLOBAL_CONTEXT ctx = u->GlobalCtx;
    PUSBWARP_DEVICE_CONTEXT dev = &ctx->Devices[u->DeviceIndex - 1];
    ULONG actual = 0;
    int32_t status = ProtoStatus(Params->IoStatus.Status, USBD_STATUS_INTERNAL_HC_ERROR);
    KIRQL irql;
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);
    /* Stop cancel lookup before deleting request objects. */
    KeAcquireSpinLock(&dev->PendingLock, &irql);
    InterlockedExchange(&u->State, UURB_STATE_DEAD);
    RemoveEntryList(&u->ListEntry);
    KeReleaseSpinLock(&dev->PendingLock, irql);
    if (u->NumberOfPackets) {
        USBWARP_FILTER_ISO_RESPONSE *resp = u->FilterOutBuf;
        BOOLEAN valid = NT_SUCCESS(Params->IoStatus.Status) &&
            Params->IoStatus.Information >= u->FilterOutSize &&
            resp->NumberOfPackets == u->NumberOfPackets;
        if (valid) {
            status = ProtoStatus(resp->NtStatus, resp->UsbdStatus);
            /* ISO packet errors belong to their descriptors, including an
             * URB where every packet missed its service interval. */
            if (resp->UsbdStatus == USBD_STATUS_ISOCH_REQUEST_FAILED && resp->NtStatus != STATUS_CANCELLED)
                status = USBWARP_STATUS_SUCCESS;
            const struct usbwarp_iso_packet_result *r = (const void *)(resp + 1);
            for (ULONG i = 0; i < u->NumberOfPackets; ++i) actual += r[i].actual_length;
        }
        SendIsoComplete(u, status, valid ? resp : NULL);
        InterlockedDecrement(&ctx->IsoPending);
    } else {
        USBWARP_FILTER_URB_RESPONSE *resp = u->FilterOutBuf;
        if (NT_SUCCESS(Params->IoStatus.Status) && Params->IoStatus.Information >= sizeof(*resp)) {
            status = ProtoStatus(resp->NtStatus, resp->UsbdStatus);
            actual = min(resp->ActualLength, u->TransferLength);
            if (u->Direction && u->DataMode == USBWARP_DATA_INLINE && actual) {
                if (actual > USBWARP_INLINE_DATA_SIZE || Params->IoStatus.Information < sizeof(*resp) + actual) {
                    actual = 0;
                    status = USBWARP_STATUS_PROTOCOL_ERROR;
                } else RtlCopyMemory(u->InlineData, resp + 1, actual);
            }
        }
        SendUrbComplete(ctx, u, status, actual);
    }
    if (status == USBWARP_STATUS_SUCCESS) UsbWarpBreakerRecordSuccess(&dev->Breaker);
    else if (status != USBWARP_STATUS_CANCELLED && status != USBWARP_STATUS_ISO_MISSED)
        UsbWarpBreakerRecordHwError(&dev->Breaker);
    InterlockedIncrement64(&dev->TotalUrbs);
    InterlockedIncrement64(&ctx->TotalUrbsProcessed);
    InterlockedAdd64(&dev->TotalBytes, actual);
    InterlockedAdd64(&ctx->TotalBytesTransferred, actual);
    FreeProxy(u);
    DecrementPending(dev);
}

static VOID ApplyPipeMap(PUSBWARP_DEVICE_CONTEXT Dev, const USBWARP_FILTER_PIPES_RESPONSE *Map)
{
    Dev->NumPipes = (UCHAR)min(Map->NumPipes, USBWARP_FILTER_MAX_PIPES);
    for (ULONG i = 0; i < Dev->NumPipes; ++i) {
        Dev->PipeHandles[i] = (USBD_PIPE_HANDLE)(ULONG_PTR)Map->Pipes[i].PipeHandle;
        Dev->PipeEndpoints[i] = Map->Pipes[i].EndpointAddress;
    }
}

static VOID ProcessTransfer(PUSBWARP_GLOBAL_CONTEXT Ctx,
    const struct usbwarp_msg_urb_submit *Msg, const struct usbwarp_msg_iso_submit *Iso)
{
    PUSBWARP_DEVICE_CONTEXT dev = &Ctx->Devices[Msg->device_id - 1];
    PUSBWARP_URB_CONTEXT u = NULL;
    PVOID transfer = NULL;
    NTSTATUS status;
    KIRQL irql;
    ULONG reqSize, outSize, inlineLen = 0;
    if (Ctx->GlobalBreakerOpen || Ctx->ShuttingDown || !UsbWarpBreakerAllows(&dev->Breaker)) {
        SendUrbSubmitError(Ctx, Msg, USBWARP_STATUS_SHUTDOWN); return;
    }
    if (!UsbWarpTokenBucketConsume(&dev->TokenBucket) ||
        InterlockedCompareExchange(&dev->PendingCount, 0, 0) >= USBWARP_PENDING_URBS_DEFAULT) {
        SendUrbSubmitError(Ctx, Msg, USBWARP_STATUS_NO_RESOURCE); return;
    }
    if (Msg->data_mode == USBWARP_DATA_BUFFER) {
        status = UsbWarpSafeGetTransferVa(Ctx, Msg->buffer_offset, max(Msg->transfer_length, 1u), &transfer);
        if (!NT_SUCCESS(status)) { SendUrbSubmitError(Ctx, Msg, USBWARP_STATUS_INVALID_PARAM); return; }
    }
    if (!Iso && Msg->transfer_type == USBWARP_XFER_CONTROL) {
        UCHAR bm = Msg->setup_packet[0], request = Msg->setup_packet[1];
        USHORT value = Msg->setup_packet[2] | (Msg->setup_packet[3] << 8);
        USHORT index = Msg->setup_packet[4] | (Msg->setup_packet[5] << 8);
        USHORT length = Msg->setup_packet[6] | (Msg->setup_packet[7] << 8);
        if (((bm & 0x60) == 0 && request == USB_REQUEST_SET_ADDRESS) ||
            length != Msg->transfer_length || (length && (ULONG)(bm >> 7) != Msg->direction)) {
            SendUrbSubmitError(Ctx, Msg, USBWARP_STATUS_INVALID_PARAM); return;
        }
        if (bm == 2 && request == USB_REQUEST_CLEAR_FEATURE && value == USB_FEATURE_ENDPOINT_STALL && !length) {
            ULONG endpoint = index;
            status = SendFilterIoctlSync(dev->FilterTarget, IOCTL_USBWARP_FILTER_RESET_PIPE,
                &endpoint, sizeof(endpoint), NULL, 0, NULL);
            SendUrbSubmitError(Ctx, Msg, NT_SUCCESS(status) ? USBWARP_STATUS_SUCCESS : USBWARP_STATUS_STALL);
            return;
        }
        if ((bm == 0 && request == USB_REQUEST_SET_CONFIGURATION) ||
            (bm == 1 && request == USB_REQUEST_SET_INTERFACE)) {
            USBWARP_FILTER_PIPES_RESPONSE pipes;
            ULONG returned = 0;
            if (length || value > 255 || index > 255) {
                SendUrbSubmitError(Ctx, Msg, USBWARP_STATUS_INVALID_PARAM); return;
            }
            if (request == USB_REQUEST_SET_CONFIGURATION) {
                USBWARP_FILTER_SELECT_CONFIG_REQUEST req = { 0 };
                req.ByValue = 1;
                req.ConfigurationValue = value;
                status = SendFilterIoctlSync(dev->FilterTarget, IOCTL_USBWARP_FILTER_SELECT_CONFIG,
                    &req, sizeof(req), &pipes, sizeof(pipes), &returned);
            } else {
                USBWARP_FILTER_SELECT_INTERFACE_REQUEST req = { index, value };
                status = SendFilterIoctlSync(dev->FilterTarget, IOCTL_USBWARP_FILTER_SELECT_INTERFACE,
                    &req, sizeof(req), &pipes, sizeof(pipes), &returned);
            }
            if (NT_SUCCESS(status) && returned >= sizeof(pipes)) {
                ApplyPipeMap(dev, &pipes);
                SendUrbSubmitError(Ctx, Msg, USBWARP_STATUS_SUCCESS);
            } else SendUrbSubmitError(Ctx, Msg, USBWARP_STATUS_STALL);
            return;
        }
    }
    if (Iso && !usbwarp_iso_packet_count_valid(Iso->number_of_packets, dev->Speed, Iso->interval)) {
        SendUrbSubmitError(Ctx, Msg, USBWARP_STATUS_INVALID_PARAM); return;
    }
    u = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*u), USBWARP_DRIVER_TAG);
    if (!u) { SendUrbSubmitError(Ctx, Msg, USBWARP_STATUS_NO_RESOURCE); return; }
    u->GlobalCtx = Ctx;
    u->TransactionId = Msg->hdr.transaction_id;
    u->Epoch = Msg->hdr.flags;
    u->DeviceIndex = Msg->device_id;
    u->Endpoint = (UCHAR)Msg->endpoint;
    u->Direction = (UCHAR)Msg->direction;
    u->TransferType = (UCHAR)Msg->transfer_type;
    u->DataMode = (UCHAR)Msg->data_mode;
    u->TransferLength = Msg->transfer_length;
    u->BufferOffset = Msg->buffer_offset;
    u->NumberOfPackets = Iso ? Iso->number_of_packets : 0;
    u->State = UURB_STATE_ACTIVE;
    u->Completion = AllocateCompletion(Iso ? usbwarp_iso_message_size(Iso->number_of_packets) :
                                      sizeof(struct usbwarp_msg_urb_complete));
    if (!u->Completion) goto Fail;
    if (Iso) {
        reqSize = sizeof(USBWARP_FILTER_ISO_REQUEST) + Iso->number_of_packets * 8;
        outSize = sizeof(USBWARP_FILTER_ISO_RESPONSE) + Iso->number_of_packets * 8;
    } else {
        inlineLen = Msg->data_mode == USBWARP_DATA_INLINE && !Msg->direction ? Msg->transfer_length : 0;
        reqSize = sizeof(USBWARP_FILTER_URB_REQUEST) + inlineLen;
        /* BUFFER mode already writes directly to SHM: no payload-sized IOCTL copy. */
        outSize = sizeof(USBWARP_FILTER_URB_RESPONSE) +
            (Msg->data_mode == USBWARP_DATA_INLINE && Msg->direction ? Msg->transfer_length : 0);
    }
    u->FilterReqBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, reqSize, USBWARP_DRIVER_TAG);
    u->FilterOutBuf = ExAllocatePool2(POOL_FLAG_NON_PAGED, outSize, USBWARP_DRIVER_TAG);
    u->FilterOutSize = outSize;
    if (!u->FilterReqBuf || !u->FilterOutBuf) goto Fail;
    if (Iso) {
        USBWARP_FILTER_ISO_REQUEST *req = u->FilterReqBuf;
        req->KernelVa = (ULONG_PTR)transfer;
        req->Endpoint = Iso->endpoint;
        req->Direction = Iso->direction;
        req->TransferLength = Iso->total_length;
        req->NumberOfPackets = Iso->number_of_packets;
        req->Speed = dev->Speed;
        req->Interval = Iso->interval;
        RtlCopyMemory(req + 1, usbwarp_iso_packets_const(Iso), Iso->number_of_packets * 8);
    } else {
        USBWARP_FILTER_URB_REQUEST *req = u->FilterReqBuf;
        req->TransferType = Msg->transfer_type;
        req->Endpoint = Msg->endpoint;
        req->Direction = Msg->direction;
        req->TransferLength = Msg->transfer_length;
        req->StreamId = Msg->stream_id;
        req->TransferFlags = Msg->transfer_flags;
        req->KernelVa = (ULONG_PTR)transfer;
        req->PipeHandle = (ULONG_PTR)FindPipe(dev, u->Endpoint, u->Direction);
        RtlCopyMemory(req->SetupPacket, Msg->setup_packet, 8);
        req->InlineDataOffset = sizeof(*req);
        req->InlineDataLength = inlineLen;
        if (inlineLen) RtlCopyMemory(req + 1, Msg->inline_data, inlineLen);
    }
    status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, dev->FilterTarget, &u->WdfRequest);
    if (!NT_SUCCESS(status)) goto Fail;
    status = WdfMemoryCreatePreallocated(WDF_NO_OBJECT_ATTRIBUTES, u->FilterReqBuf, reqSize, &u->UrbMemory);
    if (!NT_SUCCESS(status)) goto Fail;
    status = WdfMemoryCreatePreallocated(WDF_NO_OBJECT_ATTRIBUTES, u->FilterOutBuf, outSize, &u->OutMemory);
    if (!NT_SUCCESS(status)) goto Fail;
    status = WdfIoTargetFormatRequestForIoctl(dev->FilterTarget, u->WdfRequest,
        Iso ? IOCTL_USBWARP_FILTER_SUBMIT_ISO : IOCTL_USBWARP_FILTER_SUBMIT_URB,
        u->UrbMemory, NULL, u->OutMemory, NULL);
    if (!NT_SUCCESS(status)) goto Fail;
    WdfRequestSetCompletionRoutine(u->WdfRequest, UrbProxyCompletion, u);
    KeAcquireSpinLock(&dev->PendingLock, &irql);
    KeClearEvent(&dev->DrainEvent);
    InsertTailList(&dev->PendingUrbs, &u->ListEntry);
    InterlockedIncrement(&dev->PendingCount);
    KeReleaseSpinLock(&dev->PendingLock, irql);
    if (Iso) InterlockedIncrement(&Ctx->IsoPending);
    if (WdfRequestSend(u->WdfRequest, dev->FilterTarget, NULL)) return;
    /* On FALSE the completion callback is not called. */
    KeAcquireSpinLock(&dev->PendingLock, &irql);
    RemoveEntryList(&u->ListEntry);
    KeReleaseSpinLock(&dev->PendingLock, irql);
    if (Iso) InterlockedDecrement(&Ctx->IsoPending);
    DecrementPending(dev);
Fail:
    if (u->Completion) {
        if (Iso) SendIsoComplete(u, USBWARP_STATUS_NO_RESOURCE, NULL);
        else SendUrbComplete(Ctx, u, USBWARP_STATUS_NO_RESOURCE, 0);
    } else SendUrbSubmitError(Ctx, Msg, USBWARP_STATUS_NO_RESOURCE);
    FreeProxy(u);
}

static BOOLEAN AcquireDeviceIo(PUSBWARP_GLOBAL_CONTEXT Ctx,
                               PUSBWARP_DEVICE_CONTEXT Dev, ULONG Epoch)
{
    KIRQL irql;
    BOOLEAN acquired = FALSE;
    /* Slot reset/publication and acquiring the rundown reference must be
     * atomic with respect to each other. A preliminary unlocked InUse check
     * can otherwise acquire a freshly reinitialized slot's rundown object. */
    KeAcquireSpinLock(&Ctx->DeviceLock, &irql);
    if (Dev->InUse && !Dev->Removing &&
        usbwarp_binding_matches(Ctx->NegotiatedCaps, Epoch, Dev->Epoch))
        acquired = ExAcquireRundownProtection(&Dev->SubmitRundown);
    KeReleaseSpinLock(&Ctx->DeviceLock, irql);
    return acquired;
}

static VOID SubmitWithRundown(PUSBWARP_GLOBAL_CONTEXT Ctx,
    const struct usbwarp_msg_urb_submit *Msg, const struct usbwarp_msg_iso_submit *Iso)
{
    PUSBWARP_DEVICE_CONTEXT dev;
    if (!Msg->device_id || Msg->device_id > USBWARP_MAX_DEVICES_LIMIT) return;
    dev = &Ctx->Devices[Msg->device_id - 1];
    if (!AcquireDeviceIo(Ctx, dev, Msg->hdr.flags)) {
        SendUrbSubmitError(Ctx, Msg, USBWARP_STATUS_DISCONNECTED); return;
    }
    if (dev->FilterTarget) ProcessTransfer(Ctx, Msg, Iso);
    else SendUrbSubmitError(Ctx, Msg, USBWARP_STATUS_DISCONNECTED);
    ExReleaseRundownProtection(&dev->SubmitRundown);
}

VOID UsbWarpProcessUrbSubmit(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx, _In_ const struct usbwarp_msg_urb_submit *Msg)
{
    SubmitWithRundown(Ctx, Msg, NULL);
}

VOID UsbWarpProcessIsoSubmit(PUSBWARP_GLOBAL_CONTEXT Ctx, const struct usbwarp_msg_iso_submit *Msg)
{
    struct usbwarp_msg_urb_submit normalized = { 0 };
    normalized.hdr = Msg->hdr;
    normalized.device_id = Msg->device_id;
    normalized.endpoint = Msg->endpoint;
    normalized.direction = Msg->direction;
    normalized.transfer_length = Msg->total_length;
    normalized.transfer_type = USBWARP_FXFER_ISO;
    normalized.data_mode = USBWARP_DATA_BUFFER;
    normalized.buffer_offset = Msg->buffer_offset;
    SubmitWithRundown(Ctx, &normalized, Msg);
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
    if (!AcquireDeviceIo(Ctx, devCtx, Msg->hdr.flags)) return;

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
                if (reqToCancel) WdfObjectReference(reqToCancel);
            }
            break;
        }
    }
    KeReleaseSpinLock(&devCtx->PendingLock, oldIrql);

    /* Cancel OUTSIDE the lock.  WdfRequestCancelSentRequest may
     * synchronously complete the request, which calls UrbProxyCompletion,
     * which acquires PendingLock.  If we held the lock here → deadlock. */
    if (reqToCancel) {
        WdfRequestCancelSentRequest(reqToCancel);
        WdfObjectDereference(reqToCancel);
    }

    RtlZeroMemory(&ack, sizeof(ack));
    ack.hdr.magic            = USBWARP_MSG_MAGIC;
    ack.hdr.message_type     = USBWARP_MSG_URB_CANCEL_ACK;
    ack.hdr.protocol_version = USBWARP_PROTOCOL_VERSION;
    ack.hdr.message_length   = sizeof(ack);
    ack.hdr.transaction_id   = txnId;
    ack.hdr.flags = Msg->hdr.flags;
    ack.hdr.device_id        = devIdx;
    ack.status = found ? 0 : (int32_t)USBWARP_STATUS_ACCESS_DENIED;

    UsbWarpRingProduce(&Ctx->H2gRing, &ack, sizeof(ack));
    ExReleaseRundownProtection(&devCtx->SubmitRundown);
}

VOID UsbWarpProcessStreams(PUSBWARP_GLOBAL_CONTEXT Ctx, const struct usbwarp_msg_streams_request *Msg)
{
    struct usbwarp_msg_streams_response reply = { 0 };
    PUSBWARP_DEVICE_CONTEXT dev = &Ctx->Devices[Msg->hdr.device_id - 1];
    USBWARP_FILTER_STREAMS_REQUEST req = { Msg->endpoint_mask, Msg->num_streams };
    USBWARP_FILTER_STREAMS_RESPONSE resp = { 0 };
    ULONG returned = 0;
    NTSTATUS status;
    reply.hdr = Msg->hdr;
    reply.hdr.message_type = USBWARP_MSG_STREAMS_RESPONSE;
    reply.hdr.message_length = sizeof(reply);
    reply.status = USBWARP_STATUS_DISCONNECTED;
    if (AcquireDeviceIo(Ctx, dev, Msg->hdr.flags)) {
        if (Ctx->GlobalBreakerOpen || Ctx->ShuttingDown) reply.status = USBWARP_STATUS_SHUTDOWN;
        else if (!usbwarp_speed_is_super(dev->Speed)) reply.status = USBWARP_STATUS_NOT_SUPPORTED;
        else {
            status = SendFilterIoctlSync(dev->FilterTarget, IOCTL_USBWARP_FILTER_STREAMS,
                &req, sizeof(req), &resp, sizeof(resp), &returned);
            reply.status = ProtoStatus(status, NT_SUCCESS(status) ? USBD_STATUS_SUCCESS : USBD_STATUS_INTERNAL_HC_ERROR);
            if (!reply.status && returned == sizeof(resp)) reply.num_streams = resp.NumberOfStreams;
            else if (!reply.status) reply.status = USBWARP_STATUS_PROTOCOL_ERROR;
        }
        ExReleaseRundownProtection(&dev->SubmitRundown);
    }
    QueueMessage(Ctx, &reply, sizeof(reply));
}
