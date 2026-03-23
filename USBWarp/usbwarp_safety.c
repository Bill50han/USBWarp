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
 *   Capacity 200 tokens, refill rate 100/sec (spec §03 §7.3).
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
        Hdr->message_length > MsgLen) {
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
    case USBWARP_MSG_URB_COMPLETE:
    case USBWARP_MSG_URB_CANCEL:
    case USBWARP_MSG_URB_CANCEL_ACK:
    case USBWARP_MSG_DEVICE_ADDED:
    case USBWARP_MSG_DEVICE_REMOVED:
    case USBWARP_MSG_DEVICE_ADD_ACK:
    case USBWARP_MSG_DEVICE_REMOVE_ACK:
    case USBWARP_MSG_HEARTBEAT:
    case USBWARP_MSG_HOST_SHUTDOWN:
    case USBWARP_MSG_SHUTDOWN_ACK:
    case USBWARP_MSG_CIRCUIT_BREAKER_TRIP:
    case USBWARP_MSG_CIRCUIT_BREAKER_RESET:
        break;
    default:
        KdPrint(("UsbWarp: L2 unknown type %u\n", Hdr->message_type));
        return STATUS_DATA_ERROR;
    }

    /* ── Layer 3: Device authorisation ──────────────────────────────────── */
    /* device_id 0 is valid for system-level messages (negotiate, heartbeat).
     * For device-specific messages, verify device is bound. */
    if (Hdr->message_type == USBWARP_MSG_URB_SUBMIT ||
        Hdr->message_type == USBWARP_MSG_URB_CANCEL) {
        if (Hdr->device_id == 0 ||
            Hdr->device_id > USBWARP_MAX_DEVICES_LIMIT) {
            KdPrint(("UsbWarp: L3 device_id out of range %u\n",
                     Hdr->device_id));
            return STATUS_ACCESS_DENIED;
        }
        if (!Ctx->Devices[Hdr->device_id - 1].InUse) {
            KdPrint(("UsbWarp: L3 device_id %u not bound\n",
                     Hdr->device_id));
            return STATUS_ACCESS_DENIED;
        }
    }

    /* ── Layer 4: Payload field validation ──────────────────────────────── */
    if (Hdr->message_type == USBWARP_MSG_URB_SUBMIT) {
        const struct usbwarp_msg_urb_submit *sub =
            (const struct usbwarp_msg_urb_submit *)Hdr;

        if (MsgLen < USBWARP_MSG_URB_SUBMIT_BASE_SIZE)
            return STATUS_DATA_ERROR;

        /* endpoint: 0–15 */
        if (sub->endpoint > 15) {
            KdPrint(("UsbWarp: L4 endpoint %u > 15\n", sub->endpoint));
            return STATUS_DATA_ERROR;
        }

        /* direction: 0 or 1 */
        if (sub->direction > 1)
            return STATUS_DATA_ERROR;

        /* transfer_type: known values */
        if (sub->transfer_type > USBWARP_XFER_INTERRUPT)
            return STATUS_DATA_ERROR;

        /* data_mode: known values */
        if (sub->data_mode > USBWARP_DATA_INLINE)
            return STATUS_DATA_ERROR;

        /* transfer_length reasonable limit (64 MB) */
        if (sub->transfer_length > 64 * 1024 * 1024)
            return STATUS_DATA_ERROR;

        /* ── Layer 5: Buffer bounds check ───────────────────────────────── */
        if (sub->data_mode == USBWARP_DATA_BUFFER &&
            sub->transfer_length > 0) {
            ULONG64 drOffset = Ctx->ControlBlock->data_region_offset;
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
    SIZE_T size = Ctx->ShmSize;

    *OutVa = NULL;

    if (base == NULL || size == 0)
        return STATUS_INVALID_DEVICE_STATE;

    /* Overflow check. */
    if (BufferOffset >= (ULONG)size)
        return STATUS_BUFFER_OVERFLOW;

    if (TransferLength > (ULONG)(size - BufferOffset))
        return STATUS_BUFFER_OVERFLOW;

    *OutVa = base + BufferOffset;
    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Orphan mode (spec §06 §9)
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpEnterOrphanMode(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    ULONG Reason
    )
{
    LONG old;

    UNREFERENCED_PARAMETER(Reason);

    old = InterlockedCompareExchange(&Ctx->OrphanState,
                                          ORPHAN_GRACE, ORPHAN_NONE);
    if (old == ORPHAN_NONE) {
        KeQuerySystemTime(&Ctx->OrphanGraceStart);
        KdPrint(("UsbWarp: ORPHAN MODE entered reason=%u "
                 "(grace period %u ms)\n",
                 Reason, USBWARP_ORPHAN_GRACE_PERIOD_MS));
    }
}

VOID
UsbWarpCheckOrphanTimeout(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx
    )
{
    LARGE_INTEGER now;

    if (!Ctx->ServiceRegistered)
        return;

    KeQuerySystemTime(&now);

    /* Compute timeout dynamically: 3x the registered heartbeat interval,
     * clamped to [3s, 30s].  Falls back to 6s if not yet registered. */
    ULONG timeoutMs = Ctx->HeartbeatIntervalMs > 0 ?
                      Ctx->HeartbeatIntervalMs * 3 :
                      USBWARP_HEARTBEAT_TIMEOUT_MS;
    if (timeoutMs < 3000)  timeoutMs = 3000;
    if (timeoutMs > 30000) timeoutMs = 30000;

    LONGLONG elapsed = (now.QuadPart - Ctx->LastServiceHeartbeat.QuadPart);
    LONGLONG timeoutTicks = (LONGLONG)timeoutMs * 10000LL;

    if (elapsed > timeoutTicks) {
        LONG orphanState = InterlockedCompareExchange(&Ctx->OrphanState, 0, 0);

        if (orphanState == ORPHAN_NONE) {
            UsbWarpEnterOrphanMode(Ctx, 0 /* HEARTBEAT_TIMEOUT */);
        }
        else if (orphanState == ORPHAN_GRACE) {
            /* Check if grace period expired. */
            LONGLONG graceElapsed =
                (now.QuadPart - Ctx->OrphanGraceStart.QuadPart);
            LONGLONG graceTicks =
                (LONGLONG)USBWARP_ORPHAN_GRACE_PERIOD_MS * 10000LL;

            if (graceElapsed > graceTicks) {
                InterlockedExchange(&Ctx->OrphanState, ORPHAN_EXPIRED);
                KdPrint(("UsbWarp: orphan grace period EXPIRED — "
                         "initiating emergency shutdown\n"));

                /* Trip the global breaker. */
                InterlockedExchange(&Ctx->GlobalBreakerOpen, TRUE);

                /* Update Control Block. */
                if (Ctx->ControlBlock) {
                    Ctx->ControlBlock->host_state = USBWARP_STATE_ERROR;
                }
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Process notify context (P1#5: atomic pointer)
 * ═══════════════════════════════════════════════════════════════════════════ */

PUSBWARP_GLOBAL_CONTEXT g_NotifyContext = NULL;

static VOID SetNotifyContext(PUSBWARP_GLOBAL_CONTEXT ctx)
{
    InterlockedExchangePointer((PVOID *)&g_NotifyContext, ctx);
}

static VOID ClearNotifyContext(void)
{
    InterlockedExchangePointer((PVOID *)&g_NotifyContext, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Emergency shutdown (spec §06 §8.3)
 *
 *   Priority 1: Stop DMA (cancel all pending USB requests)
 *   Priority 2: Stop poll thread
 *   Priority 3: Unmap shared memory
 *   Priority 4: Release Section reference
 *   Priority 5: Free kernel objects
 * ═══════════════════════════════════════════════════════════════════════════ */

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

    InterlockedExchange(&Ctx->ShuttingDown, TRUE);
    InterlockedExchange(&Ctx->GlobalBreakerOpen, TRUE);

    /* ── Priority 1: Stop DMA — cancel all pending USB requests ─────────── */
    for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; i++) {
        if (Ctx->Devices[i].InUse && Ctx->Devices[i].FilterTarget) {
            WdfIoTargetStop(Ctx->Devices[i].FilterTarget,
                            WdfIoTargetCancelSentIo);
        }
    }

    /* P0#3: Wait for each device's pending URBs to drain (max 10s each).
     * This ensures all DMA is complete before we unmap shared memory. */
    timeout.QuadPart = -100000000LL;  /* 10 seconds */
    for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; i++) {
        if (Ctx->Devices[i].InUse &&
            InterlockedCompareExchange(&Ctx->Devices[i].PendingCount, 0, 0) > 0) {
            KeClearEvent(&Ctx->Devices[i].DrainEvent);
            KeWaitForSingleObject(&Ctx->Devices[i].DrainEvent,
                                  Executive, KernelMode, FALSE, &timeout);
        }
    }

    /* ── Priority 2: Stop poll thread ───────────────────────────────────── */
    UsbWarpPollStop(Ctx);

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

    /* ── Priority 5: Free kernel objects ────────────────────────────────── */
    for (ULONG i = 0; i < USBWARP_MAX_DEVICES_LIMIT; i++) {
        if (Ctx->Devices[i].InUse) {
            UsbWarpDeviceClose(Ctx, &Ctx->Devices[i]);
            Ctx->Devices[i].InUse = FALSE;
        }
    }
    Ctx->ActiveDeviceCount = 0;

    /* Unregister process notification callback. */
    if (Ctx->ProcessNotifyRegistered) {
        PsSetCreateProcessNotifyRoutineEx(
            UsbWarpProcessNotifyCallback, TRUE);
        Ctx->ProcessNotifyRegistered = NULL;
        ClearNotifyContext();  /* P1#5 */
    }

    Ctx->ServiceRegistered = FALSE;

    KdPrint(("UsbWarp: emergency shutdown COMPLETE\n"));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Process termination notify (spec §06 §9.2)
 *
 *   Registered via PsSetCreateProcessNotifyRoutineEx.
 *   Fires when ANY process exits.  We check if it's our Service.
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpProcessNotifyCallback(
    _Inout_  PEPROCESS              Process,
    _In_     HANDLE                 ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    PUSBWARP_GLOBAL_CONTEXT ctx;

    UNREFERENCED_PARAMETER(Process);

    if (CreateInfo != NULL)
        return;  /* only care about termination */

    /* P1#5: Read pointer atomically, validate before use. */
    ctx = (PUSBWARP_GLOBAL_CONTEXT)InterlockedCompareExchangePointer(
              (PVOID *)&g_NotifyContext, NULL, NULL);
    if (!ctx)
        return;

    if (!ctx->ServiceRegistered)
        return;

    if ((ULONG)(ULONG_PTR)ProcessId == ctx->ServiceProcessId) {
        KdPrint(("UsbWarp: Service process %u TERMINATED\n",
                 ctx->ServiceProcessId));
        UsbWarpEnterOrphanMode(ctx, 1 /* PROCESS_TERMINATED */);
    }
}
