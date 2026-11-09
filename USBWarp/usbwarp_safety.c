/*
 * usbwarp_safety.c — Safety subsystem: rate limiter, circuit breaker,
 *                    message validation, orphan mode, emergency shutdown.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * Implements the four-tier protection hierarchy from spec §06:
 *   Layer 1: per-message error handling (in validate + dispatch)
 *   Layer 2: circuit breaker (per-device + global)
 *   Layer 3: emergency shutdown
 *   Layer 4: BugCheck callback (not in this file — OS-level)
 *
 * Seven-layer message validation from spec §05 §3.1:
 *   L1: Ring integrity (in ring_host.c consume)
 *   L2: Header validation (magic, length, version, type)
 *   L3: Device authorisation
 *   L4: Payload field validation
 *   L5: Buffer bounds check
 *   L6: Rate limit (in device.c)
 *   L7: Circuit breaker (in device.c)
 */

#include "usbwarp_drv.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Token bucket (per-device rate limiter)
 *
 *   Capacity and refill are configured at bind (currently 16000 URBs/sec).
 *   Each URB submit consumes 1 token.  If tokens <= 0, request denied.
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpTokenBucketInit(
    _Out_ PUSBWARP_TOKEN_BUCKET Tb,
    LONG Capacity,
    LONG RefillRate
    )
{
    Tb->Tokens     = Capacity;
    Tb->Capacity   = Capacity;
    Tb->RefillRate = RefillRate;
    KeQuerySystemTime(&Tb->LastRefill);
}

BOOLEAN
UsbWarpTokenBucketConsume(
    _Inout_ PUSBWARP_TOKEN_BUCKET Tb
    )
{
    LARGE_INTEGER now;
    LONGLONG      elapsed100ns;
    LONG          refill;
    LONG          current;

    KeQuerySystemTime(&now);

    /* Compute refill: elapsed seconds × rate. */
    elapsed100ns = now.QuadPart - Tb->LastRefill.QuadPart;
    if (elapsed100ns > 0) {
        /* Convert 100ns units to seconds. */
        refill = (LONG)(elapsed100ns / 10000000LL) * Tb->RefillRate;
        if (refill > 0) {
            current = InterlockedAdd(&Tb->Tokens, refill);
            /* Clamp to capacity. */
            if (current > Tb->Capacity) {
                InterlockedExchange(&Tb->Tokens, Tb->Capacity);
            }
            Tb->LastRefill = now;
        }
    }

    /* Try to consume 1 token. */
    current = InterlockedDecrement(&Tb->Tokens);
    if (current < 0) {
        /* No tokens — restore and reject. */
        InterlockedIncrement(&Tb->Tokens);
        return FALSE;
    }

    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Sliding window error counter (spec §06 §2.3)
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpErrorWindowInit(
    _Out_ PUSBWARP_ERROR_WINDOW W,
    ULONG WindowSec,
    ULONG Threshold
    )
{
    RtlZeroMemory(W, sizeof(*W));
    KeInitializeSpinLock(&W->Lock);
    W->WindowSeconds = WindowSec;
    W->Threshold     = Threshold;
}

VOID
UsbWarpErrorWindowRecord(
    _Inout_ PUSBWARP_ERROR_WINDOW W
    )
{
    KIRQL         oldIrql;
    LARGE_INTEGER now;
    ULONG         nowSec;

    KeQuerySystemTime(&now);
    nowSec = (ULONG)(now.QuadPart / 10000000ULL);

    KeAcquireSpinLock(&W->Lock, &oldIrql);

    /* Expire old entries. */
    while (W->Count > 0) {
        ULONG oldest = (W->Head - W->Count) & (ERROR_WINDOW_SLOTS - 1);
        if (nowSec - W->Timestamps[oldest] > W->WindowSeconds)
            W->Count--;
        else
            break;
    }

    /* Add new entry. */
    W->Timestamps[W->Head & (ERROR_WINDOW_SLOTS - 1)] = nowSec;
    W->Head++;
    W->Count++;

    KeReleaseSpinLock(&W->Lock, oldIrql);
}

BOOLEAN
UsbWarpErrorWindowTripped(
    _In_ const PUSBWARP_ERROR_WINDOW W
    )
{
    return (W->Count >= W->Threshold);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Circuit breaker (per-device)
 *
 *   Three states: CLOSED → OPEN → HALF_OPEN → CLOSED (or back to OPEN).
 *   Trigger: sliding window threshold OR consecutive error count.
 *   Cooldown: 30 seconds (spec §06 §2.5).
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpBreakerInit(
    _Out_ PUSBWARP_CIRCUIT_BREAKER Cb
    )
{
    RtlZeroMemory(Cb, sizeof(*Cb));
    InterlockedExchange(&Cb->State, CB_CLOSED);
    Cb->CooldownMs = 30000;

    /* HW errors: 10 in 60s */
    UsbWarpErrorWindowInit(&Cb->HwErrors, 60, 10);
    /* Protocol errors: 50 in 60s */
    UsbWarpErrorWindowInit(&Cb->ProtoErrors, 60, 50);
    /* Rate-limit hits: 100 in 60s */
    UsbWarpErrorWindowInit(&Cb->RateErrors, 60, 100);
}

BOOLEAN
UsbWarpBreakerAllows(
    _Inout_ PUSBWARP_CIRCUIT_BREAKER Cb
    )
{
    LONG state = InterlockedCompareExchange(&Cb->State, 0, 0);

    if (state == CB_CLOSED)
        return TRUE;

    if (state == CB_OPEN) {
        LARGE_INTEGER now;
        KeQuerySystemTime(&now);
        LONGLONG elapsed = (now.QuadPart - Cb->OpenTimestamp.QuadPart) / 10000;
        if (elapsed >= (LONGLONG)Cb->CooldownMs) {
            /* P1#4: Transition to HALF_OPEN, reset probe counters. */
            InterlockedExchange(&Cb->ProbeCount, 0);
            InterlockedExchange(&Cb->ProbeSuccesses, 0);
            InterlockedExchange(&Cb->ProbeFailures, 0);
            InterlockedExchange(&Cb->State, CB_HALF_OPEN);
            return TRUE;
        }
        return FALSE;
    }

    /* P1#4: CB_HALF_OPEN — allow up to 3 probe requests. */
    if (InterlockedCompareExchange(&Cb->ProbeCount, 0, 0) >= 3)
        return FALSE;  /* probe limit reached, wait for verdict */

    InterlockedIncrement(&Cb->ProbeCount);
    return TRUE;
}

VOID
UsbWarpBreakerRecordHwError(
    _Inout_ PUSBWARP_CIRCUIT_BREAKER Cb
    )
{
    UsbWarpErrorWindowRecord(&Cb->HwErrors);
    InterlockedIncrement(&Cb->ConsecErrors);

    /* P1#4: If in HALF_OPEN, probe failed → back to OPEN immediately. */
    LONG state = InterlockedCompareExchange(&Cb->State, 0, 0);
    if (state == CB_HALF_OPEN) {
        InterlockedExchange(&Cb->ProbeFailures,
                            InterlockedCompareExchange(&Cb->ProbeFailures, 0, 0) + 1);
        UsbWarpBreakerTrip(Cb);  /* re-OPEN with fresh cooldown */
        return;
    }

    /* Normal CLOSED state: check trigger conditions. */
    if (UsbWarpErrorWindowTripped(&Cb->HwErrors) ||
        Cb->ConsecErrors >= 10) {
        UsbWarpBreakerTrip(Cb);
    }
}

VOID
UsbWarpBreakerRecordProtoError(
    _Inout_ PUSBWARP_CIRCUIT_BREAKER Cb
    )
{
    UsbWarpErrorWindowRecord(&Cb->ProtoErrors);
    InterlockedIncrement(&Cb->ConsecErrors);

    if (UsbWarpErrorWindowTripped(&Cb->ProtoErrors) ||
        Cb->ConsecErrors >= 10) {
        UsbWarpBreakerTrip(Cb);
    }
}

VOID
UsbWarpBreakerRecordSuccess(
    _Inout_ PUSBWARP_CIRCUIT_BREAKER Cb
    )
{
    InterlockedExchange(&Cb->ConsecErrors, 0);

    /* P1#4: In HALF_OPEN, track probe successes.
     * 3 consecutive successes → transition to CLOSED.
     * Any failure (recorded via RecordHwError) resets to OPEN. */
    LONG state = InterlockedCompareExchange(&Cb->State, 0, 0);
    if (state == CB_HALF_OPEN) {
        LONG successes = InterlockedIncrement(&Cb->ProbeSuccesses);
        if (successes >= 3) {
            InterlockedExchange(&Cb->State, CB_CLOSED);
            KdPrint(("UsbWarp: circuit breaker CLOSED (probes OK)\n"));
        }
    }
}

VOID
UsbWarpBreakerTrip(
    _Inout_ PUSBWARP_CIRCUIT_BREAKER Cb
    )
{
    LONG old = InterlockedExchange(&Cb->State, CB_OPEN);
    if (old != CB_OPEN) {
        KeQuerySystemTime(&Cb->OpenTimestamp);
        KdPrint(("UsbWarp: circuit breaker TRIPPED\n"));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Seven-layer message validation (spec §05 §3.1)
 *
 *   Layer 1 (Ring integrity) is handled in UsbWarpRingConsume.
 *   Layers 2–5 are handled here.
 *   Layers 6–7 are handled at the call site in usbwarp_device.c.
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS
UsbWarpValidateMessage(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ const struct usbwarp_msg_header *Hdr,
    _In_ ULONG MsgLen
    )
{
    /* ── Layer 2: Header validation ─────────────────────────────────────── */
    if (Hdr->magic != USBWARP_MSG_MAGIC) {
        KdPrint(("UsbWarp: L2 magic mismatch 0x%x\n", Hdr->magic));
        return STATUS_DATA_ERROR;
    }

    if (Hdr->message_length < sizeof(struct usbwarp_msg_header) ||
        Hdr->message_length != MsgLen) {
        KdPrint(("UsbWarp: L2 length invalid %u (buf=%u)\n",
                 Hdr->message_length, MsgLen));
        return STATUS_DATA_ERROR;
    }

    if (Hdr->protocol_version != USBWARP_PROTOCOL_VERSION &&
        Ctx->Negotiated) {
        /* After negotiation, version must match exactly. */
        KdPrint(("UsbWarp: L2 version mismatch %u\n",
                 Hdr->protocol_version));
        return STATUS_REVISION_MISMATCH;
    }

    /* Validate message_type is a known value. */
    switch (Hdr->message_type) {
    case USBWARP_MSG_NEGOTIATE:
    case USBWARP_MSG_NEGOTIATE_RESP:
    case USBWARP_MSG_URB_SUBMIT:
    case USBWARP_MSG_ISO_SUBMIT:
    case USBWARP_MSG_STREAMS_REQUEST:
    case USBWARP_MSG_URB_COMPLETE:
    case USBWARP_MSG_URB_CANCEL:
    case USBWARP_MSG_URB_CANCEL_ACK:
    case USBWARP_MSG_DEVICE_ADDED:
    case USBWARP_MSG_DEVICE_REMOVED:
    case USBWARP_MSG_DEVICE_ADD_ACK:
    case USBWARP_MSG_DEVICE_REMOVE_ACK:
    case USBWARP_MSG_HEARTBEAT:
    case USBWARP_MSG_HOST_SHUTDOWN:
    case USBWARP_MSG_GUEST_SHUTDOWN:
    case USBWARP_MSG_SHUTDOWN_ACK:
    case USBWARP_MSG_CIRCUIT_BREAKER_TRIP:
    case USBWARP_MSG_CIRCUIT_BREAKER_RESET:
    case USBWARP_MSG_STATS_REQUEST:
    case USBWARP_MSG_STATS_RESPONSE:
        break;
    default:
        KdPrint(("UsbWarp: L2 unknown type %u\n", Hdr->message_type));
        return STATUS_DATA_ERROR;
    }

    /* ── Layer 3: Device authorisation ──────────────────────────────────── */
    /* device_id 0 is valid for system-level messages (negotiate, heartbeat).
     * For device-specific messages, verify device is bound. */
    if (Hdr->message_type == USBWARP_MSG_URB_SUBMIT ||
        Hdr->message_type == USBWARP_MSG_ISO_SUBMIT ||
        Hdr->message_type == USBWARP_MSG_STREAMS_REQUEST ||
        Hdr->message_type == USBWARP_MSG_URB_CANCEL) {
        if (Hdr->device_id == 0 ||
            Hdr->device_id > USBWARP_MAX_DEVICES_LIMIT) {
            KdPrint(("UsbWarp: L3 device_id out of range %u\n",
                     Hdr->device_id));
            return STATUS_ACCESS_DENIED;
        }
        /* A request racing unbind must receive DISCONNECTED, rather than
         * disappear here and leave the guest holding its buffer forever. */
    }

    if (Hdr->message_type == USBWARP_MSG_STREAMS_REQUEST) {
        const struct usbwarp_msg_streams_request *req = (const void *)Hdr;
        if (!(Ctx->NegotiatedCaps & USBWARP_CAP_STREAMS) || MsgLen != sizeof(*req) ||
            !usbwarp_stream_request_valid(req->endpoint_mask, req->num_streams))
            return STATUS_DATA_ERROR;
    }

    if (Hdr->message_type == USBWARP_MSG_ISO_SUBMIT) {
        const struct usbwarp_msg_iso_submit *iso = (const void *)Hdr;
        PVOID transfer;
        if (!Ctx->Negotiated ||
            (Ctx->NegotiatedCaps & (USBWARP_CAP_ISO_TRANSFER | USBWARP_CAP_ISO_PACKED)) !=
            (USBWARP_CAP_ISO_TRANSFER | USBWARP_CAP_ISO_PACKED) ||
            !usbwarp_iso_submit_valid(iso, MsgLen))
            return STATUS_DATA_ERROR;
        return UsbWarpSafeGetTransferVa(Ctx, iso->buffer_offset,
                                       max(iso->total_length, 1u), &transfer);
    }

    /* ── Layer 4: Payload field validation ──────────────────────────────── */
    if (Hdr->message_type == USBWARP_MSG_URB_SUBMIT) {
        const struct usbwarp_msg_urb_submit *sub =
            (const struct usbwarp_msg_urb_submit *)Hdr;

        if (MsgLen < USBWARP_MSG_URB_SUBMIT_BASE_SIZE)
            return STATUS_DATA_ERROR;

        if (sub->device_id != Hdr->device_id)
            return STATUS_DATA_ERROR;

        /* endpoint: 0–15 */
        if (sub->endpoint > 15) {
            KdPrint(("UsbWarp: L4 endpoint %u > 15\n", sub->endpoint));
            return STATUS_DATA_ERROR;
        }

        /* direction: 0 or 1 */
        if (sub->direction > 1)
            return STATUS_DATA_ERROR;

        /* transfer_type: known values.  Value 1 is intentionally reserved. */
        if (sub->transfer_type != USBWARP_XFER_CONTROL &&
            sub->transfer_type != USBWARP_XFER_BULK &&
            sub->transfer_type != USBWARP_XFER_INTERRUPT)
            return STATUS_DATA_ERROR;

        if (sub->stream_id && (!(Ctx->NegotiatedCaps & USBWARP_CAP_STREAMS) ||
            sub->transfer_type != USBWARP_XFER_BULK || sub->stream_id > USBWARP_MAX_STREAMS))
            return STATUS_DATA_ERROR;

        /* data_mode: known values */
        if (sub->data_mode > USBWARP_DATA_INLINE)
            return STATUS_DATA_ERROR;
        if (sub->transfer_length && sub->data_mode == USBWARP_DATA_NONE)
            return STATUS_DATA_ERROR;
        if (sub->data_mode == USBWARP_DATA_INLINE && !sub->direction &&
            sub->transfer_length > MsgLen - USBWARP_MSG_URB_SUBMIT_BASE_SIZE)
            return STATUS_DATA_ERROR;

        /* transfer_length reasonable limit (64 MB) */
        if (sub->transfer_length > 64 * 1024 * 1024)
            return STATUS_DATA_ERROR;

        /* ── Layer 5: Buffer bounds check ───────────────────────────────── */
        if (sub->data_mode == USBWARP_DATA_BUFFER &&
            sub->transfer_length > 0) {
            ULONG64 drOffset = (ULONG64)(Ctx->DataRegion - (PUCHAR)Ctx->ShmKernelBase);
            ULONG64 drSize   = Ctx->DataRegionSize;

            /* Integer overflow check. */
            if ((ULONG64)sub->buffer_offset >
                MAXULONG64 - (ULONG64)sub->transfer_length) {
                return STATUS_INTEGER_OVERFLOW;
            }

            ULONG64 end = (ULONG64)sub->buffer_offset +
                          (ULONG64)sub->transfer_length;
            ULONG64 regionEnd = drOffset + drSize;

            if ((ULONG64)sub->buffer_offset < drOffset || end > regionEnd) {
                KdPrint(("UsbWarp: L5 buffer OOB off=0x%x len=%u "
                         "dr=[0x%llx, 0x%llx)\n",
                         sub->buffer_offset, sub->transfer_length,
                         (ULONGLONG)drOffset, (ULONGLONG)regionEnd));
                return STATUS_BUFFER_OVERFLOW;
            }
        }

        /* INLINE mode: transfer_length must fit inline buffer. */
        if (sub->data_mode == USBWARP_DATA_INLINE &&
            sub->transfer_length > USBWARP_INLINE_DATA_SIZE) {
            return STATUS_DATA_ERROR;
        }
    }

    if (Hdr->message_type == USBWARP_MSG_URB_CANCEL) {
        const struct usbwarp_msg_urb_cancel *can =
            (const struct usbwarp_msg_urb_cancel *)Hdr;

        if (MsgLen < sizeof(*can))
            return STATUS_DATA_ERROR;

        if (can->device_id != Hdr->device_id)
            return STATUS_DATA_ERROR;
    }

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Buffer bounds — secondary validation (spec §05 §4.2)
 *
 *   Called just before constructing the Windows URB.  Even though Layer 5
 *   already checked, we verify again (defence in depth).
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS
UsbWarpSafeGetTransferVa(
    _In_  PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_  ULONG BufferOffset,
    _In_  ULONG TransferLength,
    _Out_ PVOID *OutVa
    )
{
    PUCHAR base = (PUCHAR)Ctx->ShmKernelBase;
    SIZE_T size = Ctx->DataRegionSize;
    SIZE_T offset;

    *OutVa = NULL;

    if (base == NULL || size == 0 || Ctx->DataRegion == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    offset = (SIZE_T)(Ctx->DataRegion - base);

    /* Overflow check. */
    if (BufferOffset < offset || (SIZE_T)BufferOffset - offset >= size)
        return STATUS_BUFFER_OVERFLOW;

    if (TransferLength > size - ((SIZE_T)BufferOffset - offset))
        return STATUS_BUFFER_OVERFLOW;

    *OutVa = base + BufferOffset;
    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Orphan mode (spec §06 §9)
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID UsbWarpCheckOrphanTimeout(PUSBWARP_GLOBAL_CONTEXT Ctx)
{
    LARGE_INTEGER now;
    KIRQL irql;
    BOOLEAN expired = FALSE;
    now.QuadPart = (LONGLONG)KeQueryInterruptTime();
    KeAcquireSpinLock(&Ctx->ServiceLock, &irql);
    if (Ctx->ServiceRegistered) {
        ULONG milliseconds = Ctx->HeartbeatIntervalMs * 3;
        milliseconds = max(3000u, min(milliseconds, 30000u));
        if (now.QuadPart - Ctx->LastServiceHeartbeat.QuadPart > (LONGLONG)milliseconds * 10000) {
            if (Ctx->OrphanState == ORPHAN_NONE) {
                Ctx->OrphanState = ORPHAN_GRACE;
                Ctx->OrphanGraceStart = now;
            } else if (Ctx->OrphanState == ORPHAN_GRACE &&
                now.QuadPart - Ctx->OrphanGraceStart.QuadPart > (LONGLONG)USBWARP_ORPHAN_GRACE_PERIOD_MS * 10000) {
                Ctx->OrphanState = ORPHAN_EXPIRED;
                InterlockedExchange(&Ctx->GlobalBreakerOpen, TRUE);
                InterlockedExchange(&Ctx->PublishedState, USBWARP_STATE_ERROR);
                expired = TRUE;
            }
        }
    }
    KeReleaseSpinLock(&Ctx->ServiceLock, irql);
    if (expired) UsbWarpQueueSessionCleanup(Ctx);
}

/* Session cleanup is serialized against management IOCTLs by SessionMutex. */
VOID
UsbWarpEmergencyShutdown(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx
    )
{
    LONG wasEmergency;
    LARGE_INTEGER timeout;

    wasEmergency = InterlockedExchange(&Ctx->EmergencyMode, TRUE);
    if (wasEmergency)
        return;

    KdPrint(("UsbWarp: EMERGENCY SHUTDOWN initiated\n"));

    InterlockedExchange(&Ctx->GlobalBreakerOpen, TRUE);
    InterlockedExchange(&Ctx->PublishedState, USBWARP_STATE_SHUTTING_DOWN);

    if (Ctx->ControlBlock)
        Ctx->ControlBlock->host_state = USBWARP_STATE_SHUTTING_DOWN;

    for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; ++i) {
        if (Ctx->Devices[i].InUse) {
            InterlockedExchange(&Ctx->Devices[i].Removing, 1);
            UsbWarpDeviceClose(Ctx, &Ctx->Devices[i]);
        }
    }

    if (Ctx->ShmEstablished && Ctx->Negotiated && Ctx->H2gRing.Hdr) {
        NTSTATUS notifyStatus;

        notifyStatus = UsbWarpSendHostShutdown(
                           Ctx, USBWARP_SHUTDOWN_NORMAL);
        if (NT_SUCCESS(notifyStatus) && Ctx->PollThread) {
            timeout.QuadPart = -50000000LL;  /* 5 seconds */
            notifyStatus = KeWaitForSingleObject(
                               &Ctx->GuestShutdownAckEvent,
                               Executive, KernelMode, FALSE, &timeout);
            if (notifyStatus == STATUS_TIMEOUT)
                KdPrint(("UsbWarp: guest shutdown ACK timed out\n"));
        }
    }

    InterlockedExchange(&Ctx->ShuttingDown, TRUE);

    /* CRITICAL: Flush all queued DPCs across all processors.
     * UrbProxyCompletion runs as a DPC — even after DrainEvent fires,
     * the DPC may still be queued but not yet executed.  Without this
     * flush, unloading the driver while DPCs are in-flight causes
     * DRIVER_UNLOADED_WITHOUT_CANCELLING_PENDING_OPERATIONS (0xCE).
     *
     * KeFlushQueuedDpcs blocks until all queued DPCs complete.
     * Must be called at IRQL <= APC_LEVEL (we're at PASSIVE_LEVEL). */
    KdPrint(("UsbWarp: flushing queued DPCs...\n"));
    KeFlushQueuedDpcs();
    KdPrint(("UsbWarp: DPC flush complete\n"));

    /* ── Priority 2: Stop poll thread ───────────────────────────────────── */
    UsbWarpPollStop(Ctx);
    UsbWarpIrqDestroy(Ctx);
    if (Ctx->G2hDoorbell) { ObDereferenceObject(Ctx->G2hDoorbell); Ctx->G2hDoorbell = NULL; }

    /* ── Priority 3: Unmap shared memory ────────────────────────────────── */
    if (Ctx->ShmMdl) {
        MmUnlockPages(Ctx->ShmMdl);
        IoFreeMdl(Ctx->ShmMdl);
        Ctx->ShmMdl = NULL;
    }
    if (Ctx->ShmKernelBase) {
        MmUnmapViewInSystemSpace(Ctx->ShmKernelBase);
        Ctx->ShmKernelBase = NULL;
    }
    Ctx->ControlBlock   = NULL;
    Ctx->DataRegion     = NULL;
    Ctx->ShmEstablished = FALSE;

    /* ── Priority 4: Release Section reference ──────────────────────────── */
    if (Ctx->SectionObject) {
        ObDereferenceObject(Ctx->SectionObject);
        Ctx->SectionObject = NULL;
    }

    /* Publish final device state atomically with PnP cleanup callbacks. */
    {
        KIRQL irql;
        KeAcquireSpinLock(&Ctx->DeviceLock, &irql);
        for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; ++i)
            Ctx->Devices[i].InUse = FALSE;
        Ctx->ActiveDeviceCount = 0;
        KeReleaseSpinLock(&Ctx->DeviceLock, irql);
    }

    {
        KIRQL irql;
        PFILE_OBJECT file;
        PEPROCESS process;
        KeAcquireSpinLock(&Ctx->ServiceLock, &irql);
        file = Ctx->ServiceFile;
        process = Ctx->ServiceProcess;
        Ctx->ServiceFile = NULL;
        Ctx->ServiceProcess = NULL;
        Ctx->ServiceRegistered = FALSE;
        Ctx->ServiceDisconnected = FALSE;
        Ctx->ServiceProcessId = 0;
        KeReleaseSpinLock(&Ctx->ServiceLock, irql);
        Ctx->Negotiated = FALSE;
        Ctx->NegotiatedCaps = 0;
        Ctx->G2hRing.Hdr = Ctx->H2gRing.Hdr = NULL;
        Ctx->G2hRing.Data = Ctx->H2gRing.Data = NULL;
        InterlockedExchange(&Ctx->PublishedState, USBWARP_STATE_STOPPED);
        if (process) ObDereferenceObject(process);
        if (file) ObDereferenceObject(file);
    }

    KdPrint(("UsbWarp: emergency shutdown COMPLETE\n"));
}

/* A real FILE_OBJECT reference keeps the control device alive until its
 * session is drained. CLEANUP runs when the last user handle closes even
 * though that kernel reference remains; CLOSE happens after it is released. */
static VOID QueueCleanupLocked(PUSBWARP_GLOBAL_CONTEXT Ctx)
{
    if (!Ctx->ServiceRegistered || Ctx->SessionCleanupQueued) return;
    Ctx->CleanupGeneration = Ctx->SessionGeneration;
    Ctx->SessionCleanupQueued = 1;
    WdfWorkItemEnqueue(Ctx->SessionCleanupWork);
}

VOID UsbWarpQueueSessionCleanup(PUSBWARP_GLOBAL_CONTEXT Ctx)
{
    KIRQL irql;
    KeAcquireSpinLock(&Ctx->ServiceLock, &irql);
    QueueCleanupLocked(Ctx);
    KeReleaseSpinLock(&Ctx->ServiceLock, irql);
}

VOID UsbWarpEvtFileCleanup(WDFFILEOBJECT FileObject)
{
    PUSBWARP_GLOBAL_CONTEXT ctx = UsbWarpGetContext(WdfFileObjectGetDevice(FileObject));
    KIRQL irql;
    KeAcquireSpinLock(&ctx->ServiceLock, &irql);
    UsbWarpGetFileContext(FileObject)->CleanedUp = TRUE;
    if (ctx->ServiceRegistered && ctx->ServiceFile == WdfFileObjectWdmGetFileObject(FileObject)) {
        ctx->ServiceDisconnected = TRUE;
        InterlockedExchange(&ctx->GlobalBreakerOpen, TRUE);
        InterlockedExchange(&ctx->PublishedState, USBWARP_STATE_ERROR);
        QueueCleanupLocked(ctx);
    }
    KeReleaseSpinLock(&ctx->ServiceLock, irql);
}

VOID UsbWarpEvtSessionCleanup(WDFWORKITEM WorkItem)
{
    PUSBWARP_GLOBAL_CONTEXT ctx = UsbWarpGetContext((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));
    KIRQL irql;
    BOOLEAN current;
    KeWaitForSingleObject(&ctx->SessionMutex, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&ctx->ServiceLock, &irql);
    current = ctx->ServiceRegistered && ctx->CleanupGeneration == ctx->SessionGeneration;
    KeReleaseSpinLock(&ctx->ServiceLock, irql);
    if (current) UsbWarpEmergencyShutdown(ctx);
    KeAcquireSpinLock(&ctx->ServiceLock, &irql);
    ctx->SessionCleanupQueued = 0;
    KeReleaseSpinLock(&ctx->ServiceLock, irql);
    KeReleaseMutex(&ctx->SessionMutex, FALSE);
}
