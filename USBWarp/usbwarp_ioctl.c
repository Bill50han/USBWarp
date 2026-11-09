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
static NTSTATUS HandleEnumDevices(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
                                  _In_ WDFREQUEST Request,
                                  _In_ size_t OutLen);

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Top-level IOCTL dispatch
 * ═══════════════════════════════════════════════════════════════════════════ */

BOOLEAN UsbWarpRequestOwnsSession(PUSBWARP_GLOBAL_CONTEXT Ctx, WDFREQUEST Request)
{
    WDFFILEOBJECT file = WdfRequestGetFileObject(Request);
    BOOLEAN owns;
    KIRQL irql;
    KeAcquireSpinLock(&Ctx->ServiceLock, &irql);
    owns = file && Ctx->ServiceRegistered && !Ctx->ServiceDisconnected &&
           Ctx->ServiceFile == WdfFileObjectWdmGetFileObject(file);
    KeReleaseSpinLock(&Ctx->ServiceLock, irql);
    return owns;
}

VOID UsbWarpEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen,
                               size_t InLen, ULONG Code)
{
    PUSBWARP_GLOBAL_CONTEXT ctx = UsbWarpGetContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status;
    if (Code == IOCTL_USBWARP_HEARTBEAT) {
        status = HandleHeartbeat(ctx, Request, InLen, OutLen);
        WdfRequestComplete(Request, status);
        return;
    }
    status = WdfRequestForwardToIoQueue(Request, ctx->ManagementQueue);
    if (!NT_SUCCESS(status)) WdfRequestComplete(Request, status);
}

VOID
UsbWarpEvtManagementControl(
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
    /* A stale cleanup callback must finish before a new generation is published. */
    if (IoControlCode == IOCTL_USBWARP_REGISTER_SERVICE)
        WdfWorkItemFlush(ctx->SessionCleanupWork);
    KeWaitForSingleObject(&ctx->SessionMutex, Executive, KernelMode, FALSE, NULL);
    if (WdfRequestIsCanceled(Request)) {
        status = STATUS_CANCELLED;
        goto complete;
    }
    if (IoControlCode != IOCTL_USBWARP_REGISTER_SERVICE && !UsbWarpRequestOwnsSession(ctx, Request)) {
        status = STATUS_ACCESS_DENIED;
        goto complete;
    }

    switch (IoControlCode) {

    case IOCTL_USBWARP_REGISTER_SERVICE:
        status = HandleRegisterService(ctx, Request,
                                       InputBufferLength, OutputBufferLength);
        break;

    case IOCTL_USBWARP_SETUP_SHM:
        status = HandleSetupShm(ctx, Request,
                                InputBufferLength, OutputBufferLength);
        break;

    case IOCTL_USBWARP_CONFIGURE_IRQ:
        status = UsbWarpIrqConfigure(ctx, Request);
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

    case IOCTL_USBWARP_ENUM_DEVICES:
        status = HandleEnumDevices(ctx, Request, OutputBufferLength);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        KdPrint(("UsbWarp: unknown IOCTL 0x%x\n", IoControlCode));
        break;
    }

complete:
    KeReleaseMutex(&ctx->SessionMutex, FALSE);
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

        WDFFILEOBJECT file = WdfRequestGetFileObject(Request);
        PEPROCESS process;
        KIRQL irql;
        if (!file || savedPid != IoGetRequestorProcessId(WdfRequestWdmGetIrp(Request)))
            return STATUS_ACCESS_DENIED;
        status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)savedPid, &process);
        if (!NT_SUCCESS(status)) return status;
        PFILE_OBJECT owner = WdfFileObjectWdmGetFileObject(file);
        ObReferenceObject(owner); // keeps the control device/driver alive until cleanup
        InterlockedExchange(&Ctx->EmergencyMode, FALSE);
        InterlockedExchange(&Ctx->ShuttingDown, FALSE);
        InterlockedExchange(&Ctx->GlobalBreakerOpen, FALSE);
        InterlockedExchange(&Ctx->OrphanState, ORPHAN_NONE);
        InterlockedExchange(&Ctx->PublishedState, USBWARP_STATE_INIT);
        Ctx->Negotiated = FALSE;
        Ctx->NegotiatedCaps = 0;
        Ctx->NegotiatedVersion = 0;
        KeAcquireSpinLock(&Ctx->ServiceLock, &irql);
        /* CLEANUP may race REGISTER before the session has been published. */
        if (UsbWarpGetFileContext(file)->CleanedUp || WdfRequestIsCanceled(Request)) {
            KeReleaseSpinLock(&Ctx->ServiceLock, irql);
            ObDereferenceObject(process);
            ObDereferenceObject(owner);
            return STATUS_FILE_CLOSED;
        }
        Ctx->ServiceFile = owner;
        Ctx->ServiceProcess = process;
        Ctx->ServiceProcessId = savedPid;
        Ctx->HeartbeatIntervalMs = savedHeartbeat;
        Ctx->ServiceDisconnected = FALSE;
        ++Ctx->SessionGeneration;
        Ctx->ServiceRegistered = TRUE;
        Ctx->LastServiceHeartbeat.QuadPart = (LONGLONG)KeQueryInterruptTime();
        KeReleaseSpinLock(&Ctx->ServiceLock, irql);

        /* Fill output (OVERWRITES input buffer for METHOD_BUFFERED). */
        RtlZeroMemory(out, sizeof(*out));
        out->Version       = USBWARP_IOCTL_STRUCT_VERSION;
        out->DriverVersion = USBWARP_MAKE_VERSION(
                                 USBWARP_PROTOCOL_VERSION_MAJOR,
                                 USBWARP_PROTOCOL_VERSION_MINOR);
        out->MaxDevices    = USBWARP_MAX_DEVICES_LIMIT;
        out->Capabilities = USBWARP_DRIVER_CAP_G2H_DOORBELL;
        if (UsbWarpIrqSupported()) out->Capabilities |= USBWARP_DRIVER_CAP_H2G_IRQ;

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
    if (!USBWARP_IS_POWER_OF_2(in->ExpectedSize) ||
        !USBWARP_IS_POWER_OF_2(in->RingSizePerDirection) ||
        in->RingSizePerDirection < USBWARP_RING_DATA_SIZE_MIN ||
        in->RingSizePerDirection > USBWARP_RING_DATA_SIZE_MAX)
        return STATUS_INVALID_PARAMETER;

    if (!USBWARP_IS_POWER_OF_2(in->BufferSize) ||
        in->BufferSize < USBWARP_BUFFER_SIZE_MIN ||
        in->BufferSize > USBWARP_BUFFER_SIZE_MAX)
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
    if (in->MaxDevices > USBWARP_MAX_DEVICES_LIMIT) return STATUS_INVALID_PARAMETER;
    Ctx->MaxDevices = in->MaxDevices ? in->MaxDevices : USBWARP_MAX_DEVICES_LIMIT;
    hSection = (HANDLE)(ULONG_PTR)in->SectionHandle;

    {
        KAPC_STATE attach;
        KeStackAttachProcess((PRKPROCESS)Ctx->ServiceProcess, &attach);
        status = ObReferenceObjectByHandle(
                 hSection,
                 SECTION_MAP_READ | SECTION_MAP_WRITE,
                 *MmSectionObjectType,
                 UserMode,            /* ← critical for security */
                 &sectionObj,
                 NULL);
        KeUnstackDetachProcess(&attach);
    }
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
    if (viewSize != (SIZE_T)in->ExpectedSize) {
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
                status = GetExceptionCode();
                IoFreeMdl(mdl);
                goto setup_shm_fail;
            }
        } else {
            KdPrint(("UsbWarp: IoAllocateMdl failed for SHM lock\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto setup_shm_fail;
        }
    }

    /* ── Verify Control Block magic ─────────────────────────────────────── */
    cb = (volatile struct usbwarp_control_block *)shmBase;

    if (cb->magic != USBWARP_CONTROL_BLOCK_MAGIC || cb->shm_total_size != in->ExpectedSize ||
        cb->protocol_version_major != USBWARP_PROTOCOL_VERSION_MAJOR) {
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
    if ((ULONG64)g2hOff + g2hSz > viewSize || (ULONG64)h2gOff + h2gSz > viewSize ||
        (ULONG64)drOff + drSz > viewSize) {
        KdPrint(("UsbWarp: SHM layout extends beyond mapping\n"));
        status = STATUS_INVALID_PARAMETER;
        goto setup_shm_fail;
    }

    if (g2hOff != USBWARP_CONTROL_BLOCK_SIZE ||
        g2hSz != USBWARP_RING_HEADER_SIZE + in->RingSizePerDirection || h2gSz != g2hSz ||
        h2gOff != g2hOff + g2hSz || drOff != USBWARP_ALIGN_PAGE(h2gOff + h2gSz) ||
        drSz != in->ExpectedSize - drOff || bufSz != in->BufferSize ||
        bufCnt != drSz / bufSz || bufCnt < USBWARP_BUFFER_QUOTA_MIN) {
        KdPrint(("UsbWarp: invalid SHM layout parameters\n"));
        status = STATUS_INVALID_PARAMETER;
        goto setup_shm_fail;
    }

    Ctx->DataRegion     = (volatile UCHAR *)shmBase + drOff;
    Ctx->DataRegionSize = drSz;
    Ctx->BufferSize     = bufSz;
    Ctx->BufferCount    = bufCnt;

    /* Validate the initialized headers before any worker may consume them. */
    for (ULONG direction = 0; direction < 2; ++direction) {
        volatile struct usbwarp_ring_header *ring =
            (volatile struct usbwarp_ring_header *)((PUCHAR)shmBase + (direction ? h2gOff : g2hOff));
        if (ring->data_size != in->RingSizePerDirection ||
            ring->data_size_mask != in->RingSizePerDirection - 1 ||
            ring->max_message_size != USBWARP_MAX_MESSAGE_SIZE ||
            ring->producer_index || ring->consumer_index) {
            status = STATUS_INVALID_PARAMETER;
            goto setup_shm_fail;
        }
    }

    /* ── Initialise ring wrappers ───────────────────────────────────────── */
    UsbWarpRingInit(&Ctx->G2hRing,
                    (volatile struct usbwarp_ring_header *)
                        ((PUCHAR)shmBase + g2hOff),
                    (volatile UCHAR *)shmBase + g2hOff +
                        USBWARP_RING_HEADER_SIZE, in->RingSizePerDirection);

    UsbWarpRingInit(&Ctx->H2gRing,
                    (volatile struct usbwarp_ring_header *)
                        ((PUCHAR)shmBase + h2gOff),
                    (volatile UCHAR *)shmBase + h2gOff +
                        USBWARP_RING_HEADER_SIZE, in->RingSizePerDirection);

    if (!!in->DoorbellEventHandle != !!(cb->flags & USBWARP_FLAG_G2H_DOORBELL)) {
        status = STATUS_INVALID_PARAMETER;
        goto setup_shm_fail;
    }
    if (in->DoorbellEventHandle) {
        KAPC_STATE attach;
        KeStackAttachProcess((PRKPROCESS)Ctx->ServiceProcess, &attach);
        status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)in->DoorbellEventHandle,
            SYNCHRONIZE, *ExEventObjectType, UserMode, (PVOID *)&Ctx->G2hDoorbell, NULL);
        KeUnstackDetachProcess(&attach);
        if (!NT_SUCCESS(status)) goto setup_shm_fail;
    }

    /* ── Update Control Block host state ────────────────────────────────── */
    cb->host_state = USBWARP_STATE_READY;

    Ctx->ShmEstablished = TRUE;
    InterlockedExchange(&Ctx->PublishedState, USBWARP_STATE_READY);

    /* ── Start poll thread ──────────────────────────────────────────────── */
    UsbWarpIrqInitialize(Ctx);
    status = UsbWarpPollStart(Ctx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: poll start failed 0x%x\n", status));
        cb->host_state = USBWARP_STATE_ERROR;
        Ctx->ShmEstablished = FALSE;
        goto setup_shm_fail;
    }

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
    UsbWarpIrqDestroy(Ctx);
    if (Ctx->G2hDoorbell) { ObDereferenceObject(Ctx->G2hDoorbell); Ctx->G2hDoorbell = NULL; }
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
    Ctx->DataRegion = NULL;
    Ctx->DataRegionSize = 0;
    Ctx->ShmSize = 0;
    Ctx->G2hRing.Hdr = Ctx->H2gRing.Hdr = NULL;
    Ctx->G2hRing.Data = Ctx->H2gRing.Data = NULL;
    Ctx->ShmEstablished = FALSE;
    InterlockedExchange(&Ctx->PublishedState, USBWARP_STATE_INIT);
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
    KIRQL oldIrql;
    PUSBWARP_DEVICE_CONTEXT devCtx;
    GUID  bindContainerId = { 0 };
    BOOLEAN bindHaveContainerId = FALSE;

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
    if (!Ctx->ShmEstablished || !Ctx->Negotiated || Ctx->GlobalBreakerOpen || Ctx->ShuttingDown)
        return STATUS_INVALID_DEVICE_STATE;

    /* Orphan mode: refuse new binds. */
    if (InterlockedCompareExchange(&Ctx->OrphanState, 0, 0) != ORPHAN_NONE)
        return STATUS_DEVICE_NOT_CONNECTED;

    /* Validate instance path length.  Leave room for our terminator and
     * require a whole number of WCHARs before treating it as UNICODE_STRING. */
    if (in->InstancePathLength == 0 ||
        in->InstancePathLength >= sizeof(in->InstancePath) ||
        (in->InstancePathLength % sizeof(WCHAR)) != 0)
        return STATUS_INVALID_PARAMETER;

    /* Find a free device slot (1-based indexing; slot 0 unused) and reject
     * a duplicate DeviceGuid — the GUID is the unbind key, so it must stay
     * unique across bound slots.  DeviceLock keeps the scan consistent
     * with concurrent hot-remove teardown on the PnP callback thread. */
    slot = 0;
    KeAcquireSpinLock(&Ctx->DeviceLock, &oldIrql);
    for (ULONG i = 0; i < Ctx->MaxDevices; i++) {
        if (!Ctx->Devices[i].InUse) {
            if (slot == 0)
                slot = i + 1;  /* device_id = 1-based */
            continue;
        }
        if (RtlCompareMemory(Ctx->Devices[i].DeviceGuid,
                             in->DeviceGuid, 16) == 16) {
            KeReleaseSpinLock(&Ctx->DeviceLock, oldIrql);
            KdPrint(("UsbWarp: DUPLICATE BIND rejected — "
                     "GUID already bound in slot %u\n", i + 1));
            return STATUS_ALREADY_REGISTERED;
        }
    }
    KeReleaseSpinLock(&Ctx->DeviceLock, oldIrql);
    if (slot == 0)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* ── Duplicate detection via Container ID ────────────────────────
     *
     * Before touching devCtx or calling UsbWarpDeviceOpen (which sends
     * SELECT_CONFIG and would invalidate existing pipe handles), open
     * a lightweight temporary IoTarget to query the PDO's Container ID.
     *
     * Container ID is a PnP GUID that groups all devnodes of the same
     * physical device — perfect for detecting double-bind.
     */
    {
        WDFIOTARGET          tempTarget = NULL;
        WDF_IO_TARGET_OPEN_PARAMS openParams;
        UNICODE_STRING       devPath;
        WCHAR                pathBuf[256];
        PDEVICE_OBJECT       targetDO, pdo;
        DEVPROPTYPE          propType;
        ULONG                requiredSize = 0;
        NTSTATUS             cidStatus;

        /* Build path from input buffer (devCtx not initialized yet). */
        RtlZeroMemory(pathBuf, sizeof(pathBuf));
        RtlCopyMemory(pathBuf, in->InstancePath, in->InstancePathLength);
        RtlInitUnicodeString(&devPath, pathBuf);

        cidStatus = WdfIoTargetCreate(Ctx->ControlDevice,
                                       WDF_NO_OBJECT_ATTRIBUTES, &tempTarget);
        if (NT_SUCCESS(cidStatus)) {
            WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
                &openParams, &devPath, GENERIC_READ | GENERIC_WRITE);
            openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;

            cidStatus = WdfIoTargetOpen(tempTarget, &openParams);
        }

        if (NT_SUCCESS(cidStatus)) {
            targetDO = WdfIoTargetWdmGetTargetDeviceObject(tempTarget);
            if (targetDO) {
                pdo = IoGetDeviceAttachmentBaseRef(targetDO);
                if (pdo) {
                    cidStatus = IoGetDevicePropertyData(
                        pdo, &DEVPKEY_Device_ContainerId,
                        LOCALE_NEUTRAL, 0,
                        sizeof(GUID), &bindContainerId,
                        &requiredSize, &propType);
                    ObDereferenceObject(pdo);

                    if (NT_SUCCESS(cidStatus) &&
                        propType == DEVPROP_TYPE_GUID)
                        bindHaveContainerId = TRUE;
                }
            }
            WdfIoTargetClose(tempTarget);
        }

        if (tempTarget)
            WdfObjectDelete(tempTarget);

        /* Check for duplicate if we got a Container ID. */
        if (bindHaveContainerId) {
            for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; i++) {
                if (!Ctx->Devices[i].InUse) continue;
                if (!Ctx->Devices[i].ContainerIdValid) continue;
                if (RtlEqualMemory(&Ctx->Devices[i].ContainerId,
                                    &bindContainerId, sizeof(GUID))) {
                    KdPrint(("UsbWarp: DUPLICATE BIND rejected — "
                             "same device already in slot %u\n", i + 1));
                    return STATUS_ALREADY_REGISTERED;
                }
            }
        }
    }

    /* ── Initialize device context ───────────────────────────────────── */
    devCtx = &Ctx->Devices[slot - 1];

    /* Publish the slot under DeviceLock so a concurrent ENUM snapshot
     * never observes a half-initialized entry.  The zeroing is safe: the
     * slot was free, nothing references it. */
    KeAcquireSpinLock(&Ctx->DeviceLock, &oldIrql);
    RtlZeroMemory(devCtx, sizeof(*devCtx));
    devCtx->DeviceIndex = slot;
    do { devCtx->Epoch = (ULONG)InterlockedIncrement(&Ctx->DeviceEpochCounter); } while (!devCtx->Epoch);
    KeReleaseSpinLock(&Ctx->DeviceLock, oldIrql);
    RtlCopyMemory(devCtx->DeviceGuid, in->DeviceGuid, 16);
    InitializeListHead(&devCtx->PendingUrbs);
    KeInitializeSpinLock(&devCtx->PendingLock);
    ExInitializeRundownProtection(&devCtx->SubmitRundown);

    /* Store instance path. */
    RtlCopyMemory(devCtx->InstancePathBuf, in->InstancePath,
                   in->InstancePathLength);
    RtlInitUnicodeString(&devCtx->InstancePath, devCtx->InstancePathBuf);
    devCtx->InstancePathBuf[in->InstancePathLength / sizeof(WCHAR)] = L'\0';

    /* Initialise rate limiter and circuit breaker. */
    UsbWarpTokenBucketInit(&devCtx->TokenBucket, 16000, 16000);
    UsbWarpBreakerInit(&devCtx->Breaker);

    /* Store Container ID from pre-open query. */
    if (bindHaveContainerId) {
        devCtx->ContainerId      = bindContainerId;
        devCtx->ContainerIdValid = TRUE;
    }

    /* Open the USB device via WDF I/O target.
     * Safe now — we know this isn't a duplicate. */
    status = UsbWarpDeviceOpen(Ctx, devCtx, &devCtx->InstancePath);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: device open failed 0x%x for slot %u\n",
                 status, slot));
        KeAcquireSpinLock(&Ctx->DeviceLock, &oldIrql);
        devCtx->InUse = FALSE;
        KeReleaseSpinLock(&Ctx->DeviceLock, oldIrql);
        return status;
    }

    if (WdfRequestIsCanceled(Request) || Ctx->GlobalBreakerOpen || Ctx->ShuttingDown) {
        UsbWarpDeviceClose(Ctx, devCtx);
        return STATUS_INVALID_DEVICE_STATE;
    }

    /* Speed from the controller's hub query beats the bulk-maxpacket
     * heuristic: it is correct for devices without bulk endpoints
     * (UVC/UAC) and knows about low-speed.  0 (not queried) keeps the
     * heuristic result from UsbWarpDeviceOpen. */
    if (!devCtx->SpeedQueried && in->Speed >= USBWARP_SPEED_LOW && in->Speed <= USBWARP_SPEED_MAX)
        devCtx->Speed = (UCHAR)in->Speed;

    /* Device IDs are independent of root-hub ports. Enforce the negotiated
     * bus type and root-port capacity before publishing the binding. */
    {
        BOOLEAN ss = usbwarp_speed_is_super(devCtx->Speed) ? TRUE : FALSE;
        ULONG used = 0;
        if (ss && !(Ctx->NegotiatedCaps & USBWARP_CAP_USB3)) {
            UsbWarpDeviceClose(Ctx, devCtx);
            return STATUS_NOT_SUPPORTED;
        }
        KeAcquireSpinLock(&Ctx->DeviceLock, &oldIrql);
        for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; ++i)
            if (Ctx->Devices[i].InUse && !!usbwarp_speed_is_super(Ctx->Devices[i].Speed) == !!ss)
                ++used;
        KeReleaseSpinLock(&Ctx->DeviceLock, oldIrql);
        if (used >= (ss ? USBWARP_USB3_PORTS : USBWARP_USB2_PORTS)) {
            UsbWarpDeviceClose(Ctx, devCtx);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /* Publish and enqueue DEVICE_ADDED under the same lock as PnP removal,
     * keeping ADDED before REMOVED even if unplug happens during bind. */
    KeAcquireSpinLock(&Ctx->DeviceLock, &oldIrql);
    if (devCtx->Removing || Ctx->GlobalBreakerOpen || Ctx->ShuttingDown) {
        KeReleaseSpinLock(&Ctx->DeviceLock, oldIrql);
        UsbWarpDeviceClose(Ctx, devCtx);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    devCtx->InUse = TRUE;
    InterlockedIncrement(&Ctx->ActiveDeviceCount);
    status = UsbWarpSendDeviceAdded(Ctx, devCtx);
    if (!NT_SUCCESS(status)) devCtx->Removing = 1;
    KeReleaseSpinLock(&Ctx->DeviceLock, oldIrql);

    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: DEVICE_ADDED send failed 0x%x — rolling back slot %u\n",
                 status, slot));
        UsbWarpDeviceClose(Ctx, devCtx);
        KeAcquireSpinLock(&Ctx->DeviceLock, &oldIrql);
        devCtx->InUse = FALSE;
        KeReleaseSpinLock(&Ctx->DeviceLock, oldIrql);
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
    KIRQL oldIrql;
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
    KeAcquireSpinLock(&Ctx->DeviceLock, &oldIrql);
    for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; i++) {
        if (Ctx->Devices[i].InUse && !Ctx->Devices[i].Removing &&
            RtlCompareMemory(Ctx->Devices[i].DeviceGuid,
                             in->DeviceGuid, 16) == 16) {
            devCtx = &Ctx->Devices[i];
            devCtx->Removing = 1;
            break;
        }
    }
    KeReleaseSpinLock(&Ctx->DeviceLock, oldIrql);
    if (!devCtx)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    /* Stop physical I/O before the guest can recycle shared buffers. */
    UsbWarpDeviceClose(Ctx, devCtx);

    /* Notify Guest after closing device.  If Ring is full, log warning
     * but proceed with close — we cannot leave a device open after the
     * user explicitly requested unbind. */
    status = UsbWarpSendDeviceRemoved(Ctx, devCtx, 0 /* user-requested */);
    if (!NT_SUCCESS(status)) {
        KdPrint(("UsbWarp: DEVICE_REMOVED send failed 0x%x for slot %u "
                 "(proceeding with close)\n", status, devCtx->DeviceIndex));
    }


    KeAcquireSpinLock(&Ctx->DeviceLock, &oldIrql);
    if (devCtx->InUse) {
        devCtx->InUse = FALSE;
        InterlockedDecrement(&Ctx->ActiveDeviceCount);
    }
    InterlockedExchange(&devCtx->Removing, 0);
    KeReleaseSpinLock(&Ctx->DeviceLock, oldIrql);

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

    {
        WDFFILEOBJECT file = WdfRequestGetFileObject(Request);
        KIRQL irql;
        KeAcquireSpinLock(&Ctx->ServiceLock, &irql);
        if (!file || !Ctx->ServiceRegistered || Ctx->ServiceDisconnected ||
            Ctx->ServiceFile != WdfFileObjectWdmGetFileObject(file)) {
            KeReleaseSpinLock(&Ctx->ServiceLock, irql);
            return STATUS_ACCESS_DENIED;
        }
        Ctx->LastServiceHeartbeat.QuadPart = (LONGLONG)KeQueryInterruptTime();
        InterlockedCompareExchange(&Ctx->OrphanState, ORPHAN_NONE, ORPHAN_GRACE);
        KeReleaseSpinLock(&Ctx->ServiceLock, irql);
        KeAcquireSpinLock(&Ctx->DeviceLock, &irql);
        for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; ++i)
            if (Ctx->Devices[i].InUse) totalPending += Ctx->Devices[i].PendingCount;
        KeReleaseSpinLock(&Ctx->DeviceLock, irql);
    }

    /* Fill output. */
    RtlZeroMemory(out, sizeof(*out));
    out->Version         = USBWARP_IOCTL_STRUCT_VERSION;
    out->DriverState = (ULONG)InterlockedCompareExchange(&Ctx->PublishedState, 0, 0);
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
    out->DriverState = (ULONG)InterlockedCompareExchange(&Ctx->PublishedState, 0, 0);
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

/* ═══════════════════════════════════════════════════════════════════════════
 * §8b  ENUM_DEVICES
 *
 *   Authoritative per-device snapshot.  DeviceLock also guards the
 *   InUse/ActiveDeviceCount transitions in bind/unbind and the hot-remove
 *   PnP callback, so the scan cannot observe a half-torn slot.
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
HandleEnumDevices(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ WDFREQUEST Request,
    _In_ size_t OutLen
    )
{
    NTSTATUS status;
    PUSBWARP_ENUM_DEVICES_OUT out = NULL;
    size_t bufLen;
    KIRQL oldIrql;
    ULONG count = 0;

    if (OutLen < sizeof(*out))
        return STATUS_BUFFER_TOO_SMALL;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out),
                                            (PVOID *)&out, &bufLen);
    if (!NT_SUCCESS(status)) return status;

    RtlZeroMemory(out, sizeof(*out));

    KeAcquireSpinLock(&Ctx->DeviceLock, &oldIrql);
    for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; i++) {
        PUSBWARP_DEVICE_CONTEXT devCtx = &Ctx->Devices[i];
        PUSBWARP_ENUM_DEVICE_ENTRY entry;

        if (!devCtx->InUse)
            continue;

        entry            = &out->Devices[count++];
        entry->DeviceIndex = devCtx->DeviceIndex;
        RtlCopyMemory(entry->DeviceGuid, devCtx->DeviceGuid, 16);
        entry->VendorId     = devCtx->VendorId;
        entry->ProductId   = devCtx->ProductId;
        entry->Speed       = devCtx->Speed;
        entry->Removing    = (InterlockedCompareExchange(&devCtx->Removing,
                                                          0, 0) != 0);
        entry->BreakerState = (UCHAR)InterlockedCompareExchange(
                                    &devCtx->Breaker.State, 0, 0);
        entry->PendingCount = (ULONG)devCtx->PendingCount;
        entry->TotalUrbs   = (ULONG64)devCtx->TotalUrbs;
        entry->TotalBytes  = (ULONG64)devCtx->TotalBytes;
    }
    KeReleaseSpinLock(&Ctx->DeviceLock, oldIrql);

    out->Version     = USBWARP_IOCTL_STRUCT_VERSION;
    out->DeviceCount = count;

    WdfRequestSetInformation(Request, sizeof(*out));

    return STATUS_SUCCESS;
}
