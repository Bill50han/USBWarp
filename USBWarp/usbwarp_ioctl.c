/*
 * usbwarp_ioctl.c — UsbWarp IOCTL dispatch for Service ↔ Kernel Driver.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Every IOCTL follows the validation sequence from spec §05 §3.2:
 *   1. Buffer size check
 *   2. Version field check
 *   3. Value range check
 *   4. State precondition check
 *   5. Handle validation (ObReferenceObjectByHandle with UserMode)
 */

#include "usbwarp_drv.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, UsbWarpEvtIoDeviceControl)
#endif

/* Defined in usbwarp_safety.c — global pointer for process notify callback. */
extern PUSBWARP_GLOBAL_CONTEXT g_NotifyContext;

/* Forward declarations for IOCTL handlers. */
static NTSTATUS HandleRegisterService(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
                                      _In_ WDFREQUEST Request,
                                      _In_ size_t InLen, _In_ size_t OutLen);
static NTSTATUS HandleSetupShm(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
                               _In_ WDFREQUEST Request,
                               _In_ size_t InLen, _In_ size_t OutLen);
static NTSTATUS HandleTeardownShm(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
                                  _In_ WDFREQUEST Request,
                                  _In_ size_t InLen);
static NTSTATUS HandleBindDevice(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
                                 _In_ WDFREQUEST Request,
                                 _In_ size_t InLen, _In_ size_t OutLen);
static NTSTATUS HandleUnbindDevice(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
                                   _In_ WDFREQUEST Request,
                                   _In_ size_t InLen);
static NTSTATUS HandleHeartbeat(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
                                _In_ WDFREQUEST Request,
                                _In_ size_t InLen, _In_ size_t OutLen);
static NTSTATUS HandleQueryStatus(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
                                  _In_ WDFREQUEST Request,
                                  _In_ size_t OutLen);

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Top-level IOCTL dispatch
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    PUSBWARP_GLOBAL_CONTEXT ctx;
    NTSTATUS status;

    PAGED_CODE();

    ctx = UsbWarpGetContext(WdfIoQueueGetDevice(Queue));

    switch (IoControlCode) {

    case IOCTL_USBWARP_REGISTER_SERVICE:
        status = HandleRegisterService(ctx, Request,
                                       InputBufferLength, OutputBufferLength);
        break;

    case IOCTL_USBWARP_SETUP_SHM:
        status = HandleSetupShm(ctx, Request,
                                InputBufferLength, OutputBufferLength);
        break;

    case IOCTL_USBWARP_TEARDOWN_SHM:
        status = HandleTeardownShm(ctx, Request, InputBufferLength);
        break;

    case IOCTL_USBWARP_BIND_DEVICE:
        status = HandleBindDevice(ctx, Request,
                                  InputBufferLength, OutputBufferLength);
        break;

    case IOCTL_USBWARP_UNBIND_DEVICE:
        status = HandleUnbindDevice(ctx, Request, InputBufferLength);
        break;

    case IOCTL_USBWARP_HEARTBEAT:
        status = HandleHeartbeat(ctx, Request,
                                 InputBufferLength, OutputBufferLength);
        break;

    case IOCTL_USBWARP_QUERY_STATUS:
        status = HandleQueryStatus(ctx, Request, OutputBufferLength);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        KdPrint(("UsbWarp: unknown IOCTL 0x%x\n", IoControlCode));
        break;
    }

    WdfRequestComplete(Request, status);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  REGISTER_SERVICE
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
HandleRegisterService(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ size_t InLen,
    _In_ size_t OutLen
    )
{
    NTSTATUS status;
    PUSBWARP_REGISTER_SERVICE_IN  in  = NULL;
    PUSBWARP_REGISTER_SERVICE_OUT out = NULL;
    size_t bufLen;

    /* §3.2 step 1: buffer size */
    if (InLen < sizeof(*in) || OutLen < sizeof(*out))
        return STATUS_BUFFER_TOO_SMALL;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in),
                                           (PVOID *)&in, &bufLen);
    if (!NT_SUCCESS(status)) return status;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                            (PVOID *)&out, &bufLen);
    if (!NT_SUCCESS(status)) return status;

    /* §3.2 step 2: version */
    if (in->Version != USBWARP_IOCTL_STRUCT_VERSION)
        return STATUS_REVISION_MISMATCH;

    /* §3.2 step 3: value range */
    if (in->HeartbeatIntervalMs < 500 || in->HeartbeatIntervalMs > 10000)
        return STATUS_INVALID_PARAMETER;

    if (in->ProcessId == 0)
        return STATUS_INVALID_PARAMETER;

    /* CRITICAL: For METHOD_BUFFERED IOCTLs, in and out share the same
     * system buffer.  Save all input values BEFORE writing the output,
     * otherwise RtlZeroMemory(out) destroys the input data. */
    {
        ULONG savedPid       = in->ProcessId;
        ULONG savedHeartbeat = in->HeartbeatIntervalMs;

        /* §3.2 step 4: state — must not already be registered */
        if (Ctx->ServiceRegistered)
            return STATUS_ALREADY_REGISTERED;

        /* Record registration. */
        Ctx->ServiceProcessId      = savedPid;
        Ctx->HeartbeatIntervalMs   = savedHeartbeat;
        Ctx->ServiceRegistered     = TRUE;
        KeQuerySystemTime(&Ctx->LastServiceHeartbeat);

        /* Register process termination notify. */
        InterlockedExchangePointer((PVOID *)&g_NotifyContext, Ctx);
        status = PsSetCreateProcessNotifyRoutineEx(
                     UsbWarpProcessNotifyCallback, FALSE);
        if (NT_SUCCESS(status)) {
            Ctx->ProcessNotifyRegistered = (PVOID)(ULONG_PTR)1;
        } else {
            KdPrint(("UsbWarp: PsSetCreateProcessNotifyRoutineEx failed 0x%x "
                     "(orphan detection relies on heartbeat)\n", status));
        }

        /* Fill output (OVERWRITES input buffer for METHOD_BUFFERED). */
        RtlZeroMemory(out, sizeof(*out));
        out->Version       = USBWARP_IOCTL_STRUCT_VERSION;
        out->DriverVersion = USBWARP_MAKE_VERSION(
                                 USBWARP_PROTOCOL_VERSION_MAJOR,
                                 USBWARP_PROTOCOL_VERSION_MINOR);
        out->MaxDevices    = USBWARP_MAX_DEVICES_LIMIT;

        WdfRequestSetInformation(Request, sizeof(*out));

        KdPrint(("UsbWarp: Service registered PID=%u heartbeat=%ums\n",
                 savedPid, savedHeartbeat));
    }

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  SETUP_SHM
 *
 *   This is the most security-sensitive IOCTL.
 *
 *   The Service passes a user-mode Section handle.  The driver:
 *     1. Validates the handle with ObReferenceObjectByHandle(UserMode).
 *     2. Maps the Section into system space via MmMapViewInSystemSpace.
 *     3. Verifies Control Block magic.
 *     4. Parses layout and initialises ring wrappers.
 *     5. Starts the poll thread.
 *
 *   UserMode access mode in ObReferenceObjectByHandle ensures the caller
 *   can only pass handles to Sections they legitimately own.
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
HandleSetupShm(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ size_t InLen,
    _In_ size_t OutLen
    )
{
    NTSTATUS                status;
    PUSBWARP_SETUP_SHM_IN  in  = NULL;
    PUSBWARP_SETUP_SHM_OUT out = NULL;
    size_t                  bufLen;
    HANDLE                  hSection;
    PVOID                   sectionObj = NULL;
    PVOID                   shmBase    = NULL;
    SIZE_T                  viewSize   = 0;
    volatile struct usbwarp_control_block *cb;
    ULONG                   g2hOff, g2hSz, h2gOff, h2gSz;
    ULONG                   drOff, drSz, bufSz, bufCnt;

    /* Step 1: buffer size */
    if (InLen < sizeof(*in) || OutLen < sizeof(*out))
        return STATUS_BUFFER_TOO_SMALL;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in),
                                           (PVOID *)&in, &bufLen);
    if (!NT_SUCCESS(status)) return status;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                            (PVOID *)&out, &bufLen);
    if (!NT_SUCCESS(status)) return status;

    /* Step 2: version */
    if (in->Version != USBWARP_IOCTL_STRUCT_VERSION)
        return STATUS_REVISION_MISMATCH;

    /* Step 3: value range */
    if (in->ExpectedSize < USBWARP_SHM_MIN_SIZE ||
        in->ExpectedSize > USBWARP_SHM_MAX_SIZE)
        return STATUS_INVALID_PARAMETER;

    /* Power-of-two checks */
    if (in->RingSizePerDirection == 0 ||
        (in->RingSizePerDirection & (in->RingSizePerDirection - 1)) != 0)
        return STATUS_INVALID_PARAMETER;

    if (in->BufferSize == 0 ||
        (in->BufferSize & (in->BufferSize - 1)) != 0)
        return STATUS_INVALID_PARAMETER;

    /* Step 4: state precondition */
    if (!Ctx->ServiceRegistered)
        return STATUS_INVALID_DEVICE_STATE;

    if (Ctx->ShmEstablished)
        return STATUS_ALREADY_REGISTERED;

    /* Step 5: handle validation — ObReferenceObjectByHandle with UserMode.
     *
     * CRITICAL SECURITY: UserMode access mode ensures the Service process
     * can only pass Section handles it legitimately possesses.  A malicious
     * Service cannot reference arbitrary kernel objects.
     */
    hSection = (HANDLE)(ULONG_PTR)in->SectionHandle;

    status = ObReferenceObjectByHandle(
                 hSection,
                 SECTION_MAP_READ | SECTION_MAP_WRITE,
                 *MmSectionObjectType,
                 UserMode,            /* ← critical for security */
                 &sectionObj,
                 NULL);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: ObReferenceObjectByHandle failed 0x%x\n", status));
        return status;
    }

    /* Map the Section into system space (kernel VA). */
    viewSize = 0;  /* map entire section */
    status = MmMapViewInSystemSpace(sectionObj, &shmBase, &viewSize);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: MmMapViewInSystemSpace failed 0x%x\n", status));
        ObDereferenceObject(sectionObj);
        return status;
    }

    /* Verify expected size vs actual mapping size. */
    if (viewSize < (SIZE_T)in->ExpectedSize) {
        KdPrint(("UsbWarp: Section size mismatch: mapped=%llu expected=%llu\n",
                 (ULONGLONG)viewSize, (ULONGLONG)in->ExpectedSize));
        MmUnmapViewInSystemSpace(shmBase);
        ObDereferenceObject(sectionObj);
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Store in context — independent reference for orphan resilience.
     * Even if Service crashes, this reference keeps the Section alive. */
    Ctx->SectionObject = sectionObj;
    Ctx->ShmKernelBase = shmBase;
    Ctx->ShmSize       = viewSize;

    /* Lock SHM pages into physical memory.  URB completions arrive via
     * xHCI DPC at DISPATCH_LEVEL and write to the H2G Ring.  Pagefile-
     * backed sections mapped with MmMapViewInSystemSpace CAN be paged
     * out.  Without locking, a page fault at DISPATCH_LEVEL → BSOD
     * (DRIVER_IRQL_NOT_LESS_OR_EQUAL on Ring write). */
    {
        PMDL mdl = IoAllocateMdl(shmBase, (ULONG)viewSize, FALSE, FALSE, NULL);
        if (mdl) {
            __try {
                MmProbeAndLockPages(mdl, KernelMode, IoModifyAccess);
                Ctx->ShmMdl = mdl;
                KdPrint(("UsbWarp: SHM pages locked (%u pages)\n",
                         (ULONG)(viewSize / PAGE_SIZE)));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                KdPrint(("UsbWarp: MmProbeAndLockPages failed 0x%x\n",
                         GetExceptionCode()));
                IoFreeMdl(mdl);
                /* Non-fatal — system may still work if pages stay resident,
                 * but BSOD is possible under memory pressure. */
            }
        } else {
            KdPrint(("UsbWarp: IoAllocateMdl failed for SHM lock\n"));
        }
    }

    /* ── Verify Control Block magic ─────────────────────────────────────── */
    cb = (volatile struct usbwarp_control_block *)shmBase;

    if (cb->magic != USBWARP_CONTROL_BLOCK_MAGIC) {
        KdPrint(("UsbWarp: Control Block magic mismatch: 0x%x\n", cb->magic));
        goto setup_shm_fail;
    }

    Ctx->ControlBlock = cb;

    /* ── Parse layout ───────────────────────────────────────────────────── */
    g2hOff = cb->g2h_ring_offset;
    g2hSz  = cb->g2h_ring_size;
    h2gOff = cb->h2g_ring_offset;
    h2gSz  = cb->h2g_ring_size;
    drOff  = cb->data_region_offset;
    drSz   = cb->data_region_size;
    bufSz  = cb->buffer_size;
    bufCnt = cb->buffer_count;

    /* Boundary checks: all offsets must fall within the mapping. */
    if (g2hOff + g2hSz > viewSize || h2gOff + h2gSz > viewSize ||
        drOff + drSz > viewSize) {
        KdPrint(("UsbWarp: SHM layout extends beyond mapping\n"));
        status = STATUS_INVALID_PARAMETER;
        goto setup_shm_fail;
    }

    if (g2hSz < USBWARP_RING_HEADER_SIZE || h2gSz < USBWARP_RING_HEADER_SIZE ||
        bufCnt == 0 || bufSz == 0) {
        KdPrint(("UsbWarp: invalid SHM layout parameters\n"));
        status = STATUS_INVALID_PARAMETER;
        goto setup_shm_fail;
    }

    Ctx->DataRegion     = (volatile UCHAR *)shmBase + drOff;
    Ctx->DataRegionSize = drSz;
    Ctx->BufferSize     = bufSz;
    Ctx->BufferCount    = bufCnt;

    /* ── Initialise ring wrappers ───────────────────────────────────────── */
    UsbWarpRingInit(&Ctx->G2hRing,
                    (volatile struct usbwarp_ring_header *)
                        ((PUCHAR)shmBase + g2hOff),
                    (volatile UCHAR *)shmBase + g2hOff +
                        USBWARP_RING_HEADER_SIZE);

    UsbWarpRingInit(&Ctx->H2gRing,
                    (volatile struct usbwarp_ring_header *)
                        ((PUCHAR)shmBase + h2gOff),
                    (volatile UCHAR *)shmBase + h2gOff +
                        USBWARP_RING_HEADER_SIZE);

    /* ── Update Control Block host state ────────────────────────────────── */
    cb->host_state = USBWARP_STATE_INIT;

    Ctx->ShmEstablished = TRUE;

    /* ── Start poll thread ──────────────────────────────────────────────── */
    status = UsbWarpPollStart(Ctx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: poll start failed 0x%x\n", status));
        cb->host_state = USBWARP_STATE_ERROR;
        Ctx->ShmEstablished = FALSE;
        goto setup_shm_fail;
    }

    cb->host_state = USBWARP_STATE_RUNNING;

    /* ── Fill output ────────────────────────────────────────────────────── */
    RtlZeroMemory(out, sizeof(*out));
    out->Version          = USBWARP_IOCTL_STRUCT_VERSION;
    out->BufferCount      = bufCnt;
    out->DataRegionOffset = drOff;
    out->DataRegionSize   = drSz;

    WdfRequestSetInformation(Request, sizeof(*out));

    KdPrint(("UsbWarp: SHM established: base=%p size=%llu buffers=%u×%uK\n",
             shmBase, (ULONGLONG)viewSize, bufCnt, bufSz / 1024));

    return STATUS_SUCCESS;

setup_shm_fail:
    if (Ctx->ShmMdl) {
        MmUnlockPages(Ctx->ShmMdl);
        IoFreeMdl(Ctx->ShmMdl);
        Ctx->ShmMdl = NULL;
    }
    if (Ctx->ShmKernelBase) {
        MmUnmapViewInSystemSpace(Ctx->ShmKernelBase);
        Ctx->ShmKernelBase = NULL;
    }
    if (Ctx->SectionObject) {
        ObDereferenceObject(Ctx->SectionObject);
        Ctx->SectionObject = NULL;
    }
    Ctx->ControlBlock   = NULL;
    Ctx->ShmEstablished = FALSE;
    return NT_SUCCESS(status) ? STATUS_DATA_ERROR : status;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  TEARDOWN_SHM
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
HandleTeardownShm(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ size_t InLen
    )
{
    PUSBWARP_TEARDOWN_SHM_IN in = NULL;
    size_t bufLen;
    NTSTATUS status;

    if (InLen < sizeof(*in))
        return STATUS_BUFFER_TOO_SMALL;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in),
                                           (PVOID *)&in, &bufLen);
    if (!NT_SUCCESS(status)) return status;

    if (in->Version != USBWARP_IOCTL_STRUCT_VERSION)
        return STATUS_REVISION_MISMATCH;

    if (!Ctx->ShmEstablished)
        return STATUS_INVALID_DEVICE_STATE;

    /* Perform full shutdown. */
    UsbWarpEmergencyShutdown(Ctx);

    KdPrint(("UsbWarp: SHM torn down (flags=0x%x)\n", in->Flags));
    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  BIND_DEVICE
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
HandleBindDevice(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ size_t InLen,
    _In_ size_t OutLen
    )
{
    NTSTATUS status;
    PUSBWARP_BIND_DEVICE_IN  in  = NULL;
    PUSBWARP_BIND_DEVICE_OUT out = NULL;
    size_t bufLen;
    ULONG slot;
    PUSBWARP_DEVICE_CONTEXT devCtx;

    if (InLen < sizeof(*in) || OutLen < sizeof(*out))
        return STATUS_BUFFER_TOO_SMALL;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in),
                                           (PVOID *)&in, &bufLen);
    if (!NT_SUCCESS(status)) return status;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                            (PVOID *)&out, &bufLen);
    if (!NT_SUCCESS(status)) return status;

    if (in->Version != USBWARP_IOCTL_STRUCT_VERSION)
        return STATUS_REVISION_MISMATCH;

    /* State: SHM must be established. */
    if (!Ctx->ShmEstablished)
        return STATUS_INVALID_DEVICE_STATE;

    /* Orphan mode: refuse new binds. */
    if (InterlockedCompareExchange(&Ctx->OrphanState, 0, 0) != ORPHAN_NONE)
        return STATUS_DEVICE_NOT_CONNECTED;

    /* Validate instance path length. */
    if (in->InstancePathLength == 0 ||
        in->InstancePathLength > sizeof(in->InstancePath))
        return STATUS_INVALID_PARAMETER;

    /* Find a free device slot (1-based indexing; slot 0 unused). */
    slot = 0;
    for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; i++) {
        if (!Ctx->Devices[i].InUse) {
            slot = i + 1;  /* device_id = 1-based */
            break;
        }
    }
    if (slot == 0)
        return STATUS_INSUFFICIENT_RESOURCES;

    devCtx = &Ctx->Devices[slot - 1];

    /* Initialise device context. */
    RtlZeroMemory(devCtx, sizeof(*devCtx));
    devCtx->InUse       = TRUE;
    devCtx->DeviceIndex = slot;
    RtlCopyMemory(devCtx->DeviceGuid, in->DeviceGuid, 16);
    InitializeListHead(&devCtx->PendingUrbs);
    KeInitializeSpinLock(&devCtx->PendingLock);

    /* Store instance path. */
    RtlCopyMemory(devCtx->InstancePathBuf, in->InstancePath,
                   in->InstancePathLength);
    RtlInitUnicodeString(&devCtx->InstancePath, devCtx->InstancePathBuf);
    /* Ensure NUL termination. */
    devCtx->InstancePathBuf[in->InstancePathLength / sizeof(WCHAR)] = L'\0';

    /* Initialise rate limiter and circuit breaker. */
    UsbWarpTokenBucketInit(&devCtx->TokenBucket, 200, 100);
    UsbWarpBreakerInit(&devCtx->Breaker);

    /* Open the USB device via WDF I/O target. */
    status = UsbWarpDeviceOpen(Ctx, devCtx, &devCtx->InstancePath);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: device open failed 0x%x for slot %u\n",
                 status, slot));
        devCtx->InUse = FALSE;
        return status;
    }

    InterlockedIncrement(&Ctx->ActiveDeviceCount);

    /* Notify Guest via Ring message.
     * If the Ring is full, roll back: close device, clear slot. */
    status = UsbWarpSendDeviceAdded(Ctx, devCtx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: DEVICE_ADDED send failed 0x%x — rolling back slot %u\n",
                 status, slot));
        UsbWarpDeviceClose(Ctx, devCtx);
        devCtx->InUse = FALSE;
        InterlockedDecrement(&Ctx->ActiveDeviceCount);
        return status;
    }

    /* Fill output. */
    RtlZeroMemory(out, sizeof(*out));
    out->Version     = USBWARP_IOCTL_STRUCT_VERSION;
    out->DeviceIndex = slot;
    out->VendorId    = devCtx->VendorId;
    out->ProductId   = devCtx->ProductId;

    WdfRequestSetInformation(Request, sizeof(*out));

    KdPrint(("UsbWarp: device bound slot=%u VID=%04x PID=%04x\n",
             slot, devCtx->VendorId, devCtx->ProductId));

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  UNBIND_DEVICE
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
HandleUnbindDevice(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ size_t InLen
    )
{
    NTSTATUS status;
    PUSBWARP_UNBIND_DEVICE_IN in = NULL;
    size_t bufLen;
    PUSBWARP_DEVICE_CONTEXT devCtx = NULL;

    if (InLen < sizeof(*in))
        return STATUS_BUFFER_TOO_SMALL;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in),
                                           (PVOID *)&in, &bufLen);
    if (!NT_SUCCESS(status)) return status;

    if (in->Version != USBWARP_IOCTL_STRUCT_VERSION)
        return STATUS_REVISION_MISMATCH;

    if (!Ctx->ShmEstablished)
        return STATUS_INVALID_DEVICE_STATE;

    /* Find device by GUID. */
    for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; i++) {
        if (Ctx->Devices[i].InUse &&
            RtlCompareMemory(Ctx->Devices[i].DeviceGuid,
                             in->DeviceGuid, 16) == 16) {
            devCtx = &Ctx->Devices[i];
            break;
        }
    }
    if (!devCtx)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    /* Notify Guest BEFORE closing device.  If Ring is full, log warning
     * but proceed with close — we cannot leave a device open after the
     * user explicitly requested unbind. */
    status = UsbWarpSendDeviceRemoved(Ctx, devCtx, 0 /* user-requested */);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: DEVICE_REMOVED send failed 0x%x for slot %u "
                 "(proceeding with close)\n", status, devCtx->DeviceIndex));
    }

    /* Close USB device (cancels pending URBs). */
    UsbWarpDeviceClose(Ctx, devCtx);

    devCtx->InUse = FALSE;
    InterlockedDecrement(&Ctx->ActiveDeviceCount);

    KdPrint(("UsbWarp: device unbound slot=%u\n", devCtx->DeviceIndex));
    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  HEARTBEAT
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
HandleHeartbeat(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ size_t InLen,
    _In_ size_t OutLen
    )
{
    NTSTATUS status;
    PUSBWARP_HEARTBEAT_IN  in  = NULL;
    PUSBWARP_HEARTBEAT_OUT out = NULL;
    size_t bufLen;
    LONG totalPending = 0;

    if (InLen < sizeof(*in) || OutLen < sizeof(*out))
        return STATUS_BUFFER_TOO_SMALL;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in),
                                           (PVOID *)&in, &bufLen);
    if (!NT_SUCCESS(status)) return status;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                            (PVOID *)&out, &bufLen);
    if (!NT_SUCCESS(status)) return status;

    if (in->Version != USBWARP_IOCTL_STRUCT_VERSION)
        return STATUS_REVISION_MISMATCH;

    if (!Ctx->ServiceRegistered)
        return STATUS_INVALID_DEVICE_STATE;

    /* Update last heartbeat timestamp. */
    KeQuerySystemTime(&Ctx->LastServiceHeartbeat);

    /* If we were in orphan grace period, recover. */
    if (InterlockedCompareExchange(&Ctx->OrphanState,
                                   ORPHAN_NONE, ORPHAN_GRACE) == ORPHAN_GRACE) {
        KdPrint(("UsbWarp: recovered from orphan grace period\n"));
    }

    /* Count total pending URBs. */
    for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; i++) {
        if (Ctx->Devices[i].InUse)
            totalPending += Ctx->Devices[i].PendingCount;
    }

    /* Fill output. */
    RtlZeroMemory(out, sizeof(*out));
    out->Version         = USBWARP_IOCTL_STRUCT_VERSION;
    out->DriverState     = Ctx->ShmEstablished ?
                               USBWARP_STATE_RUNNING : USBWARP_STATE_INIT;
    out->PendingUrbCount = (ULONG)totalPending;

    if (InterlockedCompareExchange(&Ctx->GlobalBreakerOpen, 0, 0))
        out->DriverState = USBWARP_STATE_ERROR;

    WdfRequestSetInformation(Request, sizeof(*out));

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  QUERY_STATUS
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
HandleQueryStatus(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ size_t OutLen
    )
{
    NTSTATUS status;
    PUSBWARP_QUERY_STATUS_OUT out = NULL;
    size_t bufLen;
    LONG totalPending = 0;

    if (OutLen < sizeof(*out))
        return STATUS_BUFFER_TOO_SMALL;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                            (PVOID *)&out, &bufLen);
    if (!NT_SUCCESS(status)) return status;

    RtlZeroMemory(out, sizeof(*out));
    out->Version           = USBWARP_IOCTL_STRUCT_VERSION;
    out->DriverState       = USBWARP_STATE_INIT;
    out->ShmEstablished    = Ctx->ShmEstablished ? 1 : 0;
    out->ServiceRegistered = Ctx->ServiceRegistered ? 1 : 0;
    out->OrphanMode        = (InterlockedCompareExchange(&Ctx->OrphanState,
                                                          0, 0) != ORPHAN_NONE) ? 1 : 0;
    out->ActiveDeviceCount = (ULONG)InterlockedCompareExchange(
                                 &Ctx->ActiveDeviceCount, 0, 0);

    for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; i++) {
        if (Ctx->Devices[i].InUse)
            totalPending += Ctx->Devices[i].PendingCount;
    }
    out->TotalPendingUrbs = (ULONG)totalPending;

    out->TotalUrbsProcessed   = (ULONG64)InterlockedCompareExchange64(
                                    &Ctx->TotalUrbsProcessed, 0, 0);
    out->TotalBytesTransferred = (ULONG64)InterlockedCompareExchange64(
                                     &Ctx->TotalBytesTransferred, 0, 0);

    if (Ctx->ShmEstablished) {
        out->DriverState         = USBWARP_STATE_RUNNING;
        out->RingG2HUsagePercent = UsbWarpRingUsagePercent(&Ctx->G2hRing);
        out->RingH2GUsagePercent = UsbWarpRingUsagePercent(&Ctx->H2gRing);
    }

    if (InterlockedCompareExchange(&Ctx->GlobalBreakerOpen, 0, 0))
        out->DriverState = USBWARP_STATE_ERROR;

    /* Health score: simple heuristic. */
    out->HealthScore = 100;
    if (out->OrphanMode)        out->HealthScore -= 50;
    if (out->DriverState == USBWARP_STATE_ERROR) out->HealthScore = 0;

    WdfRequestSetInformation(Request, sizeof(*out));

    return STATUS_SUCCESS;
}
