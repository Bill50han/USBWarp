/*
 * usbwarp_ring_host.c — Host-side SPSC Ring produce/consume.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * The host (Kernel Driver) is:
 *   - Producer for the H2G ring  (writes completion messages to Guest)
 *   - Consumer for the G2H ring  (reads URB submits from Guest)
 *
 * All shared-memory accesses use volatile pointers.  Memory ordering
 * relies on KeMemoryBarrier/ReadBarrier/WriteBarrier (x86-64 has
 * strong ordering, but we insert explicit barriers for correctness
 * on potential ARM64 ports).
 *
 * The consumer copies messages into a caller-provided buffer to prevent
 * TOCTOU attacks: the Guest could modify the ring data after our
 * validation checks (spec §05 §3.1, defence in depth).
 */

#include "usbwarp_drv.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Initialisation
 * ═══════════════════════════════════════════════════════════════════════════ */

VOID
UsbWarpRingInit(
    _Out_ PUSBWARP_HOST_RING Ring,
    _In_  volatile struct usbwarp_ring_header *Hdr,
    _In_  volatile UCHAR *Data
    )
{
    Ring->Hdr      = Hdr;
    Ring->Data     = Data;
    Ring->DataSize = Hdr->data_size;
    Ring->DataMask = Hdr->data_size_mask;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Produce  (Host writes to H2G ring — completions to Guest)
 *
 *   Single producer.  The poll thread is the only writer.
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS
UsbWarpRingProduce(
    _In_ PUSBWARP_HOST_RING Ring,
    _In_ const VOID        *Msg,
    _In_ ULONG              MsgLen
    )
{
    ULONG aligned = USBWARP_ALIGN_CACHELINE(MsgLen);
    ULONG pi = Ring->Hdr->producer_index;
    ULONG ci;
    ULONG avail, pos, first;

    /* Read consumer_index with acquire semantics. */
    ci = ReadULongAcquire((volatile ULONG *)&Ring->Hdr->consumer_index);

    avail = usbwarp_ring_available(pi, ci, Ring->DataMask);
    if (avail < aligned)
        return STATUS_INSUFFICIENT_RESOURCES;  /* ring full */

    /* Write message data, handling wrap-around. */
    pos   = pi & Ring->DataMask;
    first = Ring->DataSize - pos;

    if (first >= aligned) {
        RtlCopyMemory((PVOID)(Ring->Data + pos), Msg, MsgLen);
        if (aligned > MsgLen)
            RtlZeroMemory((PVOID)(Ring->Data + pos + MsgLen),
                          aligned - MsgLen);
    } else {
        /* Wrap: first part to end, rest at start. */
        if (first >= MsgLen) {
            RtlCopyMemory((PVOID)(Ring->Data + pos), Msg, MsgLen);
            if (first > MsgLen)
                RtlZeroMemory((PVOID)(Ring->Data + pos + MsgLen),
                              first - MsgLen);
            if (aligned > first)
                RtlZeroMemory((PVOID)Ring->Data, aligned - first);
        } else {
            RtlCopyMemory((PVOID)(Ring->Data + pos), Msg, first);
            RtlCopyMemory((PVOID)Ring->Data,
                          (const UCHAR *)Msg + first,
                          MsgLen - first);
            if (aligned > MsgLen)
                RtlZeroMemory((PVOID)(Ring->Data + MsgLen - first),
                              aligned - MsgLen);
        }
    }

    /* Write barrier: data visible before index update. */
    KeMemoryBarrier();

    /* Store-release producer_index. */
    WriteULongRelease((volatile ULONG *)&Ring->Hdr->producer_index, pi + aligned);

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Consume  (Host reads from G2H ring — URB submits from Guest)
 *
 *   Single consumer (poll thread).
 *
 *   TOCTOU defence: all data is copied to a local buffer before being
 *   used by the caller.  The Guest could concurrently modify the ring
 *   data area, so we must not validate-in-place then use-in-place.
 * ═══════════════════════════════════════════════════════════════════════════ */

NTSTATUS
UsbWarpRingConsume(
    _In_    PUSBWARP_HOST_RING Ring,
    _Out_   PVOID              MsgOut,
    _In_    ULONG              BufSize,
    _Out_   PULONG             MsgLenOut
    )
{
    ULONG ci = Ring->Hdr->consumer_index;
    ULONG pi;
    ULONG used, pos, first;
    struct usbwarp_msg_header hdrCopy;
    ULONG msgLen, aligned;

    *MsgLenOut = 0;

    /* Load producer_index with acquire semantics. */
    pi = ReadULongAcquire((volatile ULONG *)&Ring->Hdr->producer_index);

    used = usbwarp_ring_used(pi, ci, Ring->DataMask);
    if (used < sizeof(struct usbwarp_msg_header))
        return STATUS_NO_MORE_ENTRIES;  /* empty */

    /* Peek at the header to learn message_length.
     * We read into a local copy (TOCTOU defence). */
    pos   = ci & Ring->DataMask;
    first = Ring->DataSize - pos;

    if (first >= sizeof(hdrCopy)) {
        RtlCopyMemory(&hdrCopy, (const VOID *)(Ring->Data + pos),
                       sizeof(hdrCopy));
    } else {
        RtlCopyMemory(&hdrCopy, (const VOID *)(Ring->Data + pos), first);
        RtlCopyMemory((PUCHAR)&hdrCopy + first,
                       (const VOID *)Ring->Data,
                       sizeof(hdrCopy) - first);
    }

    /* Basic header sanity — Layer 2 of seven-layer validation. */
    if (hdrCopy.magic != USBWARP_MSG_MAGIC)
        return STATUS_DATA_ERROR;

    msgLen = hdrCopy.message_length;
    if (msgLen < sizeof(struct usbwarp_msg_header))
        return STATUS_DATA_ERROR;

    aligned = USBWARP_ALIGN_CACHELINE(msgLen);
    if (aligned > used)
        return STATUS_DATA_ERROR;  /* length exceeds what's in ring */

    if (msgLen > BufSize) {
        /* Skip to avoid wedging the ring. */
        WriteULongRelease((volatile ULONG *)&Ring->Hdr->consumer_index, ci + aligned);
        return STATUS_BUFFER_OVERFLOW;
    }

    /* Copy full message into caller's local buffer. */
    pos   = ci & Ring->DataMask;
    first = Ring->DataSize - pos;

    if (first >= msgLen) {
        RtlCopyMemory(MsgOut, (const VOID *)(Ring->Data + pos), msgLen);
    } else {
        RtlCopyMemory(MsgOut, (const VOID *)(Ring->Data + pos), first);
        RtlCopyMemory((PUCHAR)MsgOut + first,
                       (const VOID *)Ring->Data,
                       msgLen - first);
    }

    /* Read barrier (pairs with producer's write barrier). */
    KeMemoryBarrier();

    /* Advance consumer_index. */
    WriteULongRelease((volatile ULONG *)&Ring->Hdr->consumer_index, ci + aligned);

    *MsgLenOut = msgLen;
    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Usage query
 * ═══════════════════════════════════════════════════════════════════════════ */

ULONG
UsbWarpRingUsagePercent(
    _In_ const PUSBWARP_HOST_RING Ring
    )
{
    ULONG pi = Ring->Hdr->producer_index;
    ULONG ci = Ring->Hdr->consumer_index;
    ULONG used = usbwarp_ring_used(pi, ci, Ring->DataMask);

    if (Ring->DataSize == 0)
        return 0;
    return (ULONG)((ULONG64)used * 100 / Ring->DataSize);
}
