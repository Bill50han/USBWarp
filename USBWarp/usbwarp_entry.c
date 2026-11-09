/*
 * usbwarp_entry.c — UsbWarp Windows Kernel Driver entry point.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Non-PnP control device pattern (cf. Microsoft general/ioctl/kmdf/sys/nonpnp).
 * UsbWarp.sys is not bound to a physical PCI device.  Instead, the Service
 * passes a Section handle via IOCTL, and the driver maps the shared memory
 * for Ring-protocol communication with the Linux guest.
 */

#include "usbwarp_drv.h"

/* Forward declarations */
EVT_WDF_DRIVER_UNLOAD                   UsbWarpEvtDriverUnload;
EVT_WDF_DEVICE_SHUTDOWN_NOTIFICATION    UsbWarpEvtShutdown;

static EVT_WDF_OBJECT_CONTEXT_CLEANUP   UsbWarpEvtDeviceCleanup;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, UsbWarpDeviceAdd)
#pragma alloc_text(PAGE, UsbWarpEvtDriverUnload)
#pragma alloc_text(PAGE, UsbWarpEvtDeviceCleanup)
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  DriverEntry
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS                status;
    WDF_DRIVER_CONFIG       config;
    WDFDRIVER               hDriver = NULL;
    PWDFDEVICE_INIT         pInit   = NULL;
    WDF_OBJECT_ATTRIBUTES   attributes;

    KdPrint(("UsbWarp: DriverEntry (protocol %u.%u)\n",
             USBWARP_PROTOCOL_VERSION_MAJOR,
             USBWARP_PROTOCOL_VERSION_MINOR));

    /* ── Create framework driver object (Non-PnP) ──────────────────────── */
    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    config.EvtDriverUnload  = UsbWarpEvtDriverUnload;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &attributes,
                             &config,
                             &hDriver);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: WdfDriverCreate failed 0x%x\n", status));
        return status;
    }

    /* ── Allocate control device init structure ─────────────────────────── */
    pInit = WdfControlDeviceInitAllocate(
                hDriver,
                &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (pInit == NULL) {
        KdPrint(("UsbWarp: WdfControlDeviceInitAllocate failed\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* ── Create control device ──────────────────────────────────────────── */
    status = UsbWarpDeviceAdd(hDriver, pInit);
    if (!NT_SUCCESS(status)) {
        /* pInit freed inside UsbWarpDeviceAdd on failure */
        KdPrint(("UsbWarp: UsbWarpDeviceAdd failed 0x%x\n", status));
    }

    return status;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  UsbWarpDeviceAdd — create the control device object
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS
UsbWarpDeviceAdd(
    _In_ WDFDRIVER       Driver,
    _In_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS                status;
    WDF_OBJECT_ATTRIBUTES   devAttributes;
    WDF_IO_QUEUE_CONFIG     queueConfig;
    WDFDEVICE               controlDevice = NULL;
    PUSBWARP_GLOBAL_CONTEXT ctx;
    DECLARE_CONST_UNICODE_STRING(ntDeviceName,   USBWARP_NTDEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(symbolicLink,   USBWARP_SYMBOLIC_LINK);
    ULONG                   i;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    /* Exclusive: only one opener at a time (the Service). */
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    {
        WDF_FILEOBJECT_CONFIG files;
        WDF_OBJECT_ATTRIBUTES fileAttributes;
        WDF_FILEOBJECT_CONFIG_INIT(&files, WDF_NO_EVENT_CALLBACK,
                                  WDF_NO_EVENT_CALLBACK, UsbWarpEvtFileCleanup);
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, USBWARP_FILE_CONTEXT);
        WdfDeviceInitSetFileObjectConfig(DeviceInit, &files, &fileAttributes);
    }

    status = WdfDeviceInitAssignName(DeviceInit, &ntDeviceName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: AssignName failed 0x%x\n", status));
        goto End;
    }

    /* Shutdown notification — for orderly cleanup. */
    WdfControlDeviceInitSetShutdownNotification(
        DeviceInit, UsbWarpEvtShutdown, WdfDeviceShutdown);

    /* Device context + cleanup callback.
     * EvtCleanupCallback on the WDFDEVICE fires when the device is
     * being deleted (during sc stop / driver unload).  This is where
     * we run EmergencyShutdown + KeFlushQueuedDpcs to guarantee all
     * in-flight DPCs complete before driver code is unloaded.
     *
     * Using WDFDEVICE (not WDFDRIVER) means the callback parameter
     * IS the device — we can call UsbWarpGetContext directly. */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttributes,
                                            USBWARP_GLOBAL_CONTEXT);
    devAttributes.EvtCleanupCallback = UsbWarpEvtDeviceCleanup;
    devAttributes.ExecutionLevel = WdfExecutionLevelPassive;

    status = WdfDeviceCreate(&DeviceInit, &devAttributes, &controlDevice);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: WdfDeviceCreate failed 0x%x\n", status));
        goto End;
    }

    /* Symbolic link for user-mode access. */
    status = WdfDeviceCreateSymbolicLink(controlDevice, &symbolicLink);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: CreateSymbolicLink failed 0x%x\n", status));
        goto End;
    }

    /* ── Initialise global context ──────────────────────────────────────── */
    ctx = UsbWarpGetContext(controlDevice);
    RtlZeroMemory(ctx, sizeof(*ctx));

    ctx->ControlDevice = controlDevice;

    KeInitializeSpinLock(&ctx->DeviceLock);
    KeInitializeSpinLock(&ctx->ServiceLock);
    KeInitializeMutex(&ctx->SessionMutex, 0);
    ctx->SessionSyncInitialized = TRUE;
    ctx->PublishedState = USBWARP_STATE_INIT;
    ctx->MaxDevices = USBWARP_MAX_DEVICES_LIMIT;
    {
        WDF_WORKITEM_CONFIG work;
        WDF_OBJECT_ATTRIBUTES workAttributes;
        WDF_WORKITEM_CONFIG_INIT(&work, UsbWarpEvtSessionCleanup);
        work.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&workAttributes);
        workAttributes.ParentObject = controlDevice;
        status = WdfWorkItemCreate(&work, &workAttributes, &ctx->SessionCleanupWork);
        if (!NT_SUCCESS(status)) goto End;
    }
    InitializeListHead(&ctx->Completions);
    KeInitializeSpinLock(&ctx->CompletionLock);
    KeInitializeEvent(&ctx->PollStopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&ctx->PollWakeEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&ctx->GuestShutdownAckEvent, NotificationEvent, FALSE);

    for (i = 0; i < USBWARP_MAX_DEVICES_LIMIT; i++) {
        InitializeListHead(&ctx->Devices[i].PendingUrbs);
        KeInitializeSpinLock(&ctx->Devices[i].PendingLock);
        KeInitializeEvent(&ctx->Devices[i].DrainEvent,
                          NotificationEvent, TRUE);  /* initially signalled */
    }

    ctx->LastServiceHeartbeat.QuadPart = (LONGLONG)KeQueryInterruptTime();

    /* Heartbeats have an independent parallel path. Slow management stays
     * serialized, without delaying liveness checks behind a USB bind. */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoDeviceControl = UsbWarpEvtManagementControl;
    status = WdfIoQueueCreate(controlDevice, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->ManagementQueue);
    if (!NT_SUCCESS(status)) goto End;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoDeviceControl = UsbWarpEvtIoDeviceControl;
    status = WdfIoQueueCreate(controlDevice, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->DefaultQueue);
    if (!NT_SUCCESS(status)) goto End;

    /* Finish initialising the control device — I/O now accepted. */
    WdfControlFinishInitializing(controlDevice);

    KdPrint(("UsbWarp: control device created\n"));

End:
    if (DeviceInit != NULL) {
        WdfDeviceInitFree(DeviceInit);
    }
    return status;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Shutdown notification
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpEvtShutdown(
    _In_ WDFDEVICE Device
    )
{
    PUSBWARP_GLOBAL_CONTEXT ctx = UsbWarpGetContext(Device);

    KdPrint(("UsbWarp: system shutdown notification\n"));

    /* Perform emergency shutdown to release all resources. */
    if (ctx->SessionSyncInitialized) {
        KeWaitForSingleObject(&ctx->SessionMutex, Executive, KernelMode, FALSE, NULL);
        UsbWarpEmergencyShutdown(ctx);
        KeReleaseMutex(&ctx->SessionMutex, FALSE);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Driver unload
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpEvtDriverUnload(
    _In_ WDFDRIVER Driver
    )
{
    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    KdPrint(("UsbWarp: driver unloading\n"));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Device cleanup (WDFDEVICE being deleted — last chance before unload)
 *
 *   This fires when WDF deletes the control device during driver unload.
 *   It is the FINAL callback before driver code is unloaded from memory.
 *
 *   If the Service disconnected and orphan mode hasn't completed
 *   EmergencyShutdown yet, it runs here.  If it already ran (via
 *   ran (via orphan timeout or system shutdown), the EmergencyMode flag
 *   makes it a no-op.
 *
 *   KeFlushQueuedDpcs at the end is our absolute guarantee that no
 *   DPC references UsbWarp.sys code when we return.
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpEvtDeviceCleanup(
    _In_ WDFOBJECT DeviceObject
    )
{
    PUSBWARP_GLOBAL_CONTEXT ctx;

    PAGED_CODE();

    KdPrint(("UsbWarp: device cleanup (final)\n"));

    ctx = UsbWarpGetContext((WDFDEVICE)DeviceObject);
    if (!ctx)
        return;

    /* Run EmergencyShutdown if not already done (idempotent). */
    if (ctx->SessionSyncInitialized) {
        KeWaitForSingleObject(&ctx->SessionMutex, Executive, KernelMode, FALSE, NULL);
        UsbWarpEmergencyShutdown(ctx);
        KeReleaseMutex(&ctx->SessionMutex, FALSE);
    }

    /* Final DPC flush — absolute last chance before code unloads. */
    KeFlushQueuedDpcs();

    KdPrint(("UsbWarp: device cleanup complete — safe to unload\n"));
}
