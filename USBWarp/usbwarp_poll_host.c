/*
 * usbwarp_poll_host.c — Host polling thread and G2H message dispatch.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * The poll thread is a system thread that:
 *   1. Consumes G2H ring messages (URB submits, cancels, negotiate, heartbeat)
 *   2. Applies the seven-layer validation from spec §05 §3.1
 *   3. Dispatches valid messages to the appropriate handler
 *   4. Monitors Service heartbeat and enters orphan mode on timeout
 *   5. Periodically writes host heartbeat to Control Block
 */

#include "usbwarp_drv.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * §0  Ring produce with retry for critical messages
 *
 *   negotiate_resp and device_added/removed are critical — if the Ring is
 *   full and we drop them, the Guest will never enter RUNNING or see the
 *   device.  Retry with short delay (runs on poll thread at PASSIVE_LEVEL).
 * ═══════════════════════════════════════════════════════════════════════════ */

static NTSTATUS
RingProduceRetry(
    _In_ PUSBWARP_HOST_RING Ring,
    _In_ const VOID *Msg,
    _In_ ULONG MsgLen,
    _In_ ULONG MaxRetries
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;
    LARGE_INTEGER delay;

    for (i = 0; i <= MaxRetries; i++) {
        status = UsbWarpRingProduce(Ring, Msg, MsgLen);
        if (NT_SUCCESS(status))
            return status;
        if (i < MaxRetries) {
            delay.QuadPart = -10000;  /* 1 ms */
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }
    }
    KdPrint(("UsbWarp: RingProduceRetry FAILED after %u retries\n",
             MaxRetries));
    return status;
}

/* Forward declarations. */
static VOID DispatchMessage(_In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
                            _In_ PVOID MsgBuf, _In_ ULONG MsgLen);

static VOID FillMsgHeader(_Out_ struct usbwarp_msg_header *Hdr,
                           USHORT Type, ULONG DevId,
                           ULONG TxnId, ULONG TotalLen);

/* ═══════════════════════════════════════════════════════════════════════════
 * §0b  Ring corruption recovery
 *
 *   When RingConsume returns STATUS_DATA_ERROR (bad magic, impossible
 *   message_length, etc.), the consumer is stuck — it can't determine
 *   the corrupted message's length to skip past it.
 *
 *   Recovery algorithm:
 *     1. From current ci, scan forward one cacheline (64B) at a time
 *     2. At each position, peek the header
 *     3. If it looks valid (magic OK + length reasonable + type in range),
 *        advance ci to that position and resume normal consumption
 *     4. If no valid header found within max scan distance, reset ci = pi
 *        (drop all pending data) and log the event
 *
 *   All messages are cacheline-aligned, so the next valid message MUST
 *   start on a cacheline boundary.  The triple-check (magic + length +
 *   type range) makes false positive probability ~10^-15.
 *
 *   Max scan: data_size / cacheline_size cachelines (one full ring).
 *   This runs at PASSIVE_LEVEL (poll thread) so it's safe.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Check if a header looks plausibly valid. */
static BOOLEAN
LooksLikeValidHeader(
    _In_ const struct usbwarp_msg_header *Hdr,
    _In_ ULONG UsedBytes,
    _In_ ULONG MaxMessageSize
    )
{
    ULONG aligned;

    /* 1. Magic must match exactly. */
    if (Hdr->magic != USBWARP_MSG_MAGIC)
        return FALSE;

    /* 2. Length must be at least header size and not exceed max. */
    if (Hdr->message_length < USBWARP_MSG_HEADER_SIZE)
        return FALSE;
    if (Hdr->message_length > MaxMessageSize)
        return FALSE;

    /* 3. Aligned length must fit in available data. */
    aligned = USBWARP_ALIGN_CACHELINE(Hdr->message_length);
    if (aligned > UsedBytes)
        return FALSE;

    /* 4. Message type must be in a known range.
     *    Valid G2H types: 1 (NEGOTIATE), 5 (ADD_ACK), 6 (REMOVE_ACK),
     *    20 (HEARTBEAT), 31 (GUEST_SHUTDOWN), 32 (SHUTDOWN_ACK),
     *    100 (URB_SUBMIT), 102 (URB_CANCEL), 200 (ISO_SUBMIT),
     *    300-301 (CIRCUIT_BREAKER), 400-401 (STATS).
     *    We use a broad range check rather than exhaustive enum match
     *    to avoid false negatives from future message types. */
    if (Hdr->message_type == 0 || Hdr->message_type > 500)
        return FALSE;

    return TRUE;
}

static VOID
TryRingRecovery(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx
    )
{
    PUSBWARP_HOST_RING ring = &Ctx->G2hRing;
    ULONG ci = ring->Hdr->consumer_index;
    ULONG pi = ReadULongAcquire(
                   (volatile ULONG *)&ring->Hdr->producer_index);
    ULONG used = usbwarp_ring_used(pi, ci, ring->DataMask);
    ULONG maxScan = ring->DataSize / USBWARP_CACHELINE_SIZE;
    ULONG scanned = 0;
    ULONG probeIndex;
    struct usbwarp_msg_header hdr;
    NTSTATUS status;

    if (used == 0)
        return;  /* nothing to recover */

    KdPrint(("UsbWarp: Ring corruption detected at ci=%u pi=%u used=%u, "
             "entering recovery scan\n", ci, pi, used));

    /* Scan forward one cacheline at a time. */
    probeIndex = ci + USBWARP_CACHELINE_SIZE;

    while (scanned < maxScan) {
        scanned++;

        /* Check if we've caught up with the producer. */
        used = usbwarp_ring_used(pi, probeIndex, ring->DataMask);
        if (used < sizeof(struct usbwarp_msg_header))
            break;  /* no more data to scan */

        status = UsbWarpRingPeekHeaderAt(ring, probeIndex, &hdr);
        if (NT_SUCCESS(status) &&
            LooksLikeValidHeader(&hdr, used, ring->Hdr->max_message_size)) {
            /* Found a valid-looking header.  Advance ci to this position. */
            ULONG skippedBytes = probeIndex - ci;
            WriteULongRelease(
                (volatile ULONG *)&ring->Hdr->consumer_index,
                probeIndex);

            InterlockedIncrement64(&Ctx->RingRecoveryCount);
            InterlockedAdd64(&Ctx->RingCachelinesSkipped, (LONG64)scanned);

            KdPrint(("UsbWarp: Ring RECOVERED — skipped %u bytes "
                     "(%u cachelines) to valid message type=%u len=%u\n",
                     skippedBytes, scanned,
                     hdr.message_type, hdr.message_length));
            return;
        }

        probeIndex += USBWARP_CACHELINE_SIZE;
    }

    /* No valid header found — drop all pending data. */
    WriteULongRelease(
        (volatile ULONG *)&ring->Hdr->consumer_index, pi);

    InterlockedIncrement64(&Ctx->RingRecoveryResets);
    InterlockedAdd64(&Ctx->RingCachelinesSkipped, (LONG64)scanned);

    KdPrint(("UsbWarp: Ring RESET — scanned %u cachelines, no valid header "
             "found. ci advanced from %u to %u (dropped %u bytes)\n",
             scanned, ci, pi, usbwarp_ring_used(pi, ci, ring->DataMask)));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Poll thread main function
 * ═══════════════════════════════════════════════════════════════════════════ */

static VOID
UsbWarpPollThreadProc(
    _In_ PVOID Context
    )
{
    PUSBWARP_GLOBAL_CONTEXT ctx = (PUSBWARP_GLOBAL_CONTEXT)Context;
    PUCHAR                  msgBuf;
    LARGE_INTEGER           heartbeatInterval;
    LARGE_INTEGER           lastHeartbeat;
    LARGE_INTEGER           now;
    LARGE_INTEGER           waitTimeout;
    NTSTATUS                waitStatus;
    ULONG                   idleCount = 0;

    KdPrint(("UsbWarp: poll thread started\n"));

    msgBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                      USBWARP_POLL_MSG_BUF_SIZE,
                                      USBWARP_DRIVER_TAG);
    if (!msgBuf) {
        KdPrint(("UsbWarp: poll thread: cannot alloc msg buffer\n"));
        PsTerminateSystemThread(STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    heartbeatInterval.QuadPart = -10000LL * 1000;  /* 1 second in 100ns */
    KeQuerySystemTime(&lastHeartbeat);

    InterlockedExchange(&ctx->PollRunning, TRUE);

    while (!InterlockedCompareExchange(&ctx->ShuttingDown, 0, 0)) {

        ULONG   msgLen = 0;
        NTSTATUS consumeStatus;
        BOOLEAN  gotMsg = FALSE;

        /* ── Try to consume one G2H message ─────────────────────────────── */
        consumeStatus = UsbWarpRingConsume(
                            &ctx->G2hRing, msgBuf,
                            USBWARP_POLL_MSG_BUF_SIZE, &msgLen);

        if (NT_SUCCESS(consumeStatus) && msgLen > 0) {

            /* Seven-layer validation (spec §05 §3.1). */
            NTSTATUS valStatus = UsbWarpValidateMessage(
                                     ctx,
                                     (const struct usbwarp_msg_header *)msgBuf,
                                     msgLen);
            if (NT_SUCCESS(valStatus)) {
                DispatchMessage(ctx, msgBuf, msgLen);
            } else {
                KdPrint(("UsbWarp: message validation failed 0x%x type=%u\n",
                         valStatus,
                         ((const struct usbwarp_msg_header *)msgBuf)->message_type));
            }
            gotMsg = TRUE;

        } else if (consumeStatus == STATUS_DATA_ERROR) {
            /* Ring corruption: bad magic, impossible length, or
             * length exceeds available data.  The consumer is stuck
             * because it can't determine the corrupted message's size.
             * Scan forward to find the next valid message. */
            TryRingRecovery(ctx);
            gotMsg = TRUE;  /* don't enter idle — retry immediately */
        }

        /* ── Adaptive poll timing ───────────────────────────────────────── */
        if (gotMsg) {
            idleCount = 0;
        } else {
            idleCount++;

            if (idleCount < 100) {
                YieldProcessor();
            } else if (idleCount < 1000) {
                /* Light sleep: 10 µs */
                waitTimeout.QuadPart = -100;  /* 10 µs in 100ns units */
                KeWaitForSingleObject(&ctx->PollStopEvent, Executive,
                                      KernelMode, FALSE, &waitTimeout);
            } else {
                /* Idle sleep: 1 ms */
                waitTimeout.QuadPart = -10000;  /* 1 ms */
                waitStatus = KeWaitForSingleObject(
                                 &ctx->PollStopEvent, Executive,
                                 KernelMode, FALSE, &waitTimeout);
                if (waitStatus == STATUS_SUCCESS)
                    break;  /* stop event signalled */
            }
        }

        /* ── Periodic host heartbeat ────────────────────────────────────── */
        KeQuerySystemTime(&now);
        if ((now.QuadPart - lastHeartbeat.QuadPart) >= heartbeatInterval.QuadPart) {

            /* Update Control Block host heartbeat. */
            if (ctx->ControlBlock) {
                ctx->ControlBlock->host_heartbeat_ts = (uint64_t)now.QuadPart;
                ctx->ControlBlock->host_state        = USBWARP_STATE_RUNNING;
            }

            /* Check Service heartbeat timeout (orphan detection). */
            UsbWarpCheckOrphanTimeout(ctx);

            lastHeartbeat = now;
        }
    }

    ExFreePoolWithTag(msgBuf, USBWARP_DRIVER_TAG);
    InterlockedExchange(&ctx->PollRunning, FALSE);

    KdPrint(("UsbWarp: poll thread stopped\n"));
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Start / stop
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS
UsbWarpPollStart(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx
    )
{
    NTSTATUS status;
    HANDLE   threadHandle;
    OBJECT_ATTRIBUTES oa;

    KeClearEvent(&Ctx->PollStopEvent);
    InterlockedExchange(&Ctx->ShuttingDown, FALSE);

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    status = PsCreateSystemThread(&threadHandle, THREAD_ALL_ACCESS,
                                  &oa, NULL, NULL,
                                  UsbWarpPollThreadProc, Ctx);
    if (!NT_SUCCESS(status))
        return status;

    /* Get PETHREAD reference for later KeWaitForSingleObject. */
    status = ObReferenceObjectByHandle(threadHandle, THREAD_ALL_ACCESS,
                                       *PsThreadType, KernelMode,
                                       (PVOID *)&Ctx->PollThread, NULL);
    ZwClose(threadHandle);

    /* P1#6: Elevate poll thread priority for low-latency USB devices.
     * LOW_REALTIME_PRIORITY (16) ensures we preempt normal threads
     * but don't interfere with critical system threads. */
    if (NT_SUCCESS(status) && Ctx->PollThread) {
        KeSetPriorityThread((PKTHREAD)Ctx->PollThread, LOW_REALTIME_PRIORITY);
    }

    return status;
}

VOID
UsbWarpPollStop(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx
    )
{
    LARGE_INTEGER timeout;

    if (!Ctx->PollThread)
        return;

    InterlockedExchange(&Ctx->ShuttingDown, TRUE);
    KeSetEvent(&Ctx->PollStopEvent, IO_NO_INCREMENT, FALSE);

    timeout.QuadPart = -50000000LL;  /* 5 seconds */
    KeWaitForSingleObject(Ctx->PollThread, Executive,
                          KernelMode, FALSE, &timeout);

    ObDereferenceObject(Ctx->PollThread);
    Ctx->PollThread = NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Message dispatch
 * ═══════════════════════════════════════════════════════════════════════════ */

static VOID
DispatchMessage(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ PVOID  MsgBuf,
    _In_ ULONG  MsgLen
    )
{
    const struct usbwarp_msg_header *hdr =
        (const struct usbwarp_msg_header *)MsgBuf;

    switch (hdr->message_type) {

    case USBWARP_MSG_NEGOTIATE: {
        const struct usbwarp_msg_negotiate *neg =
            (const struct usbwarp_msg_negotiate *)MsgBuf;
        if (MsgLen < sizeof(*neg))
            break;
        UsbWarpSendNegotiateResp(Ctx, neg);
        break;
    }

    case USBWARP_MSG_URB_SUBMIT: {
        const struct usbwarp_msg_urb_submit *sub =
            (const struct usbwarp_msg_urb_submit *)MsgBuf;
        if (MsgLen < USBWARP_MSG_URB_SUBMIT_BASE_SIZE)
            break;
        UsbWarpProcessUrbSubmit(Ctx, sub);
        break;
    }

    case USBWARP_MSG_URB_CANCEL: {
        const struct usbwarp_msg_urb_cancel *can =
            (const struct usbwarp_msg_urb_cancel *)MsgBuf;
        if (MsgLen < sizeof(*can))
            break;
        UsbWarpProcessUrbCancel(Ctx, can);
        break;
    }

    case USBWARP_MSG_HEARTBEAT: {
        /* Guest heartbeat: update Control Block guest state. */
        break;
    }

    case USBWARP_MSG_DEVICE_ADD_ACK:
    case USBWARP_MSG_DEVICE_REMOVE_ACK:
        /* Acknowledgements from Guest — informational. */
        break;

    case USBWARP_MSG_SHUTDOWN_ACK:
        KdPrint(("UsbWarp: guest acknowledged shutdown\n"));
        break;

    default:
        KdPrint(("UsbWarp: unknown G2H message type %u\n",
                 hdr->message_type));
        break;
    }
}

static VOID
FillMsgHeader(
    _Out_ struct usbwarp_msg_header *Hdr,
    USHORT Type, ULONG DevId,
    ULONG TxnId, ULONG TotalLen
    )
{
    RtlZeroMemory(Hdr, sizeof(*Hdr));
    Hdr->magic            = USBWARP_MSG_MAGIC;
    Hdr->message_type     = Type;
    Hdr->protocol_version = USBWARP_PROTOCOL_VERSION;
    Hdr->message_length   = TotalLen;
    Hdr->transaction_id   = TxnId;
    Hdr->device_id        = DevId;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Negotiate response
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpSendNegotiateResp(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ const struct usbwarp_msg_negotiate *NegMsg
    )
{
    struct usbwarp_msg_negotiate_resp resp;
    USHORT version;

    RtlZeroMemory(&resp, sizeof(resp));

    /* Reject re-negotiation when already in RUNNING state.
     * A fuzz or misbehaving Guest could downgrade capabilities (e.g.
     * caps=0 disables inline data) or set max_pending_urbs=0, breaking
     * all subsequent transfers.  Once negotiated, the session is locked. */
    if (Ctx->Negotiated) {
        KdPrint(("UsbWarp: negotiate REJECTED — already in RUNNING state\n"));
        FillMsgHeader(&resp.hdr, USBWARP_MSG_NEGOTIATE_RESP, 0, 0,
                      sizeof(resp));
        resp.status = (int32_t)STATUS_ALREADY_REGISTERED;
        /* Return current params so Guest can resync. */
        resp.negotiated_version  = Ctx->NegotiatedVersion;
        resp.capabilities        = Ctx->NegotiatedCaps;
        resp.max_pending_urbs    = USBWARP_PENDING_URBS_DEFAULT;
        resp.max_devices         = USBWARP_MAX_DEVICES_LIMIT;
        RingProduceRetry(&Ctx->H2gRing, &resp, sizeof(resp), 10);
        return;
    }

    /* Choose the highest mutually-supported version. */
    version = USBWARP_PROTOCOL_VERSION;
    if (version < NegMsg->min_version || version > NegMsg->max_version) {
        /* Version mismatch — reject. */
        FillMsgHeader(&resp.hdr, USBWARP_MSG_NEGOTIATE_RESP, 0, 0,
                      sizeof(resp));
        resp.status = (int32_t)STATUS_NOT_SUPPORTED;
        RingProduceRetry(&Ctx->H2gRing, &resp, sizeof(resp), 10);
        KdPrint(("UsbWarp: negotiate rejected — version mismatch\n"));
        return;
    }

    /* Accept negotiation. */
    Ctx->NegotiatedVersion = version;
    Ctx->NegotiatedCaps    = USBWARP_CAP_INLINE_DATA |
                             USBWARP_CAP_BATCH_SUBMIT |
                             USBWARP_CAP_STATS;
    /* Intersect with guest capabilities. */
    Ctx->NegotiatedCaps &= NegMsg->capabilities;
    Ctx->Negotiated = TRUE;

    FillMsgHeader(&resp.hdr, USBWARP_MSG_NEGOTIATE_RESP, 0, 0,
                  sizeof(resp));
    resp.status              = 0;
    resp.negotiated_version  = version;
    resp.capabilities        = Ctx->NegotiatedCaps;
    resp.max_pending_urbs    = USBWARP_PENDING_URBS_DEFAULT;
    resp.max_devices         = USBWARP_MAX_DEVICES_LIMIT;

    RingProduceRetry(&Ctx->H2gRing, &resp, sizeof(resp), 50);

    KdPrint(("UsbWarp: negotiate OK ver=%u.%u caps=0x%x\n",
             USBWARP_VERSION_MAJOR(version),
             USBWARP_VERSION_MINOR(version),
             Ctx->NegotiatedCaps));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Device added / removed notifications (Host → Guest)
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS
UsbWarpSendDeviceAdded(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ PUSBWARP_DEVICE_CONTEXT DevCtx
    )
{
    struct usbwarp_msg_device_added msg;

    RtlZeroMemory(&msg, sizeof(msg));
    FillMsgHeader(&msg.hdr, USBWARP_MSG_DEVICE_ADDED,
                  DevCtx->DeviceIndex, 0, sizeof(msg));
    msg.device_id = DevCtx->DeviceIndex;
    msg.vendor_id = DevCtx->VendorId;
    msg.product_id = DevCtx->ProductId;
    msg.speed     = DevCtx->Speed;

    return RingProduceRetry(&Ctx->H2gRing, &msg, sizeof(msg), 20);
}

NTSTATUS
UsbWarpSendDeviceRemoved(
    _In_ PUSBWARP_GLOBAL_CONTEXT Ctx,
    _In_ PUSBWARP_DEVICE_CONTEXT DevCtx,
    _In_ ULONG Reason
    )
{
    struct usbwarp_msg_device_removed msg;

    RtlZeroMemory(&msg, sizeof(msg));
    FillMsgHeader(&msg.hdr, USBWARP_MSG_DEVICE_REMOVED,
                  DevCtx->DeviceIndex, 0, sizeof(msg));
    msg.device_id = DevCtx->DeviceIndex;
    msg.reason    = Reason;

    return RingProduceRetry(&Ctx->H2gRing, &msg, sizeof(msg), 20);
}
