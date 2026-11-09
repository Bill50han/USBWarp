// SPDX-License-Identifier: GPL-2.0-only
#include "usbwarp_drv.h"
#if defined(_AMD64_)
#include <intrin.h>
#endif

/* 0x44/0x47 are TLFS partition discovery calls. Device assertion 0x80 is
 * the native WinHvAssertDeviceInterrupt ABI observed on 26100/26200.
 * It is NOT the APIC assertion call 0x94: HDV MSI values are opaque keys.
 * Restrict this optional backend to verified builds, validate a real IRQ,
 * and retain polling. See docs/kernel-interrupts.txt for contract limits. */
BOOLEAN UsbWarpIrqSupported(VOID)
{
#if defined(_AMD64_)
    int c[4];
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"HvlInvokeHypercall");
    __cpuid(c, 0x40000000);
    if (c[0] < 0x40000003 || c[1] != 0x7263694d ||
        c[2] != 0x666f736f || c[3] != 0x76482074) return FALSE;
    __cpuid(c, 0x40000001);
    if (c[0] != 0x31237648) return FALSE;
    __cpuid(c, 0x40000003);
    if (!(c[1] & 1)) return FALSE; /* CreatePartitions: parent only. */
    __cpuid(c, 0x40000002);
    if (c[0] != 26100 && c[0] != 26200) return FALSE;
    return MmGetSystemRoutineAddress(&name) != NULL;
#else
    return FALSE;
#endif
}

VOID UsbWarpIrqInitialize(PUSBWARP_GLOBAL_CONTEXT ctx)
{
    USBWARP_IRQ_HOST *q = &ctx->Irq;
    PHYSICAL_ADDRESS highest;
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"HvlInvokeHypercall");
    RtlZeroMemory(q, sizeof(*q));
    KeInitializeSpinLock(&q->Lock);
    if (!UsbWarpIrqSupported()) return;
    q->Invoke = (USBWARP_HYPERCALL)MmGetSystemRoutineAddress(&name);
    highest.QuadPart = MAXLONGLONG;
    q->Pages = MmAllocateContiguousMemory(3 * PAGE_SIZE, highest);
    if (!q->Pages) return;
    RtlZeroMemory(q->Pages, 3 * PAGE_SIZE);
    q->InputPa = MmGetPhysicalAddress(q->Pages).QuadPart;
    q->OutputPa = MmGetPhysicalAddress((PUCHAR)q->Pages + PAGE_SIZE).QuadPart;
    q->NotifyPa = MmGetPhysicalAddress((PUCHAR)q->Pages + 2 * PAGE_SIZE).QuadPart;
    q->Cb = ctx->ControlBlock;
    ctx->H2gRing.NotifyContext = ctx;
}

VOID UsbWarpIrqDestroy(PUSBWARP_GLOBAL_CONTEXT ctx)
{
    /* Caller has joined the poll thread and drained all ring producers. */
    PVOID pages = ctx->Irq.Pages;
    ctx->H2gRing.NotifyContext = NULL;
    if (ctx->Irq.Cb) ctx->Irq.Cb->irq_host_state = USBWARP_IRQ_FAILED;
    ctx->Irq.Cb = NULL;
    ctx->Irq.Pages = NULL;
    ctx->Irq.Configured = FALSE;
    if (pages) MmFreeContiguousMemory(pages);
}

NTSTATUS UsbWarpIrqConfigure(PUSBWARP_GLOBAL_CONTEXT ctx, WDFREQUEST request)
{
    USBWARP_CONFIGURE_IRQ_IN *in;
    USBWARP_IRQ_HOST *q = &ctx->Irq;
    KIRQL level;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(request, sizeof(*in), (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (in->Version != USBWARP_IOCTL_STRUCT_VERSION) return STATUS_REVISION_MISMATCH;
    if (!ctx->ShmEstablished || !q->Pages) return STATUS_NOT_SUPPORTED;
    if (!in->Nonce || !in->Revision || in->Reserved || (in->Flags & ~3u))
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock, &level);
    if (q->Configured && (in->Nonce != q->Config.Nonce ||
        RtlCompareMemory(in->InstanceId, q->Config.InstanceId, 16) != 16)) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else if (!q->Configured || in->Revision > q->Config.Revision) {
        q->Config = *in;
        q->Configured = TRUE;
    }
    KeReleaseSpinLock(&q->Lock, level);
    KeSetEvent(&ctx->PollWakeEvent, IO_NO_INCREMENT, FALSE);
    return status;
}

VOID UsbWarpIrqProduced(PUSBWARP_GLOBAL_CONTEXT ctx)
{
    /* Ring publication precedes this counter. No hypercall under ring lock. */
    InterlockedIncrement64((volatile LONG64 *)&ctx->Irq.Cb->irq_event_seq);
    KeSetEvent(&ctx->PollWakeEvent, IO_NO_INCREMENT, FALSE);
}

static ULONG Query(USBWARP_IRQ_HOST *q, USHORT code, ULONGLONG a, ULONGLONG b, ULONGLONG *value)
{
    KIRQL level;
    ULONGLONG result;
    ULONGLONG *in = q->Pages, *out = (PVOID)((PUCHAR)q->Pages + PAGE_SIZE);
    RtlZeroMemory(in, 32);
    *out = 0;
    in[0] = a; in[1] = b;
    KeRaiseIrql(DISPATCH_LEVEL, &level);
    result = q->Invoke(code, q->InputPa, q->OutputPa);
    KeLowerIrql(level);
    *value = *out;
    return (ULONG)(result & 0xffff);
}

static BOOLEAN Match(USBWARP_IRQ_HOST *q, ULONGLONG expected, ULONGLONG *match)
{
    ULONGLONG previous = 0, next, value, found = 0;
    ULONG i;
    for (i = 0; i < 4096; ++i) {
        if (Query(q, 0x47, MAXULONGLONG, previous, &next)) return FALSE;
        if (!next) { *match = found; return found != 0; }
        if (next == previous || next == MAXULONGLONG) return FALSE;
        if (Query(q, 0x44, next, 0x70000, &value)) return FALSE;
        if (value == expected) {
            if (found) return FALSE; /* ambiguous, never choose first match */
            found = next;
        }
        previous = next;
    }
    return FALSE; /* incomplete enumeration cannot establish uniqueness */
}

static VOID Request(USBWARP_IRQ_HOST *q, ULONG state, ULONGLONG challenge)
{
    q->State = state; q->Challenge = challenge;
    q->Deadline = KeQueryInterruptTime() + 20000000ULL;
    q->Cb->irq_challenge = challenge;
    q->Cb->irq_host_state = state;
    WriteULongRelease((volatile ULONG *)&q->Cb->irq_request_seq, ++q->Request);
}

static VOID Fail(USBWARP_IRQ_HOST *q, ULONG status)
{
    q->Cb->irq_last_status = status;
    ++q->Cb->irq_failed;
    Request(q, USBWARP_IRQ_FAILED, 0);
    KdPrint(("UsbWarp: native IRQ disabled, status=%x; using polling\n", status));
}

static VOID Tag(USBWARP_IRQ_HOST *q)
{
    ULONGLONG tag = (q->Config.Nonce >> (q->Round * 16)) & 0xffff;
    /* Never leave the marker equal to the normal OS ID. */
    if (tag == (q->Original & 0xffff)) tag ^= 0x8000;
    Request(q, USBWARP_IRQ_TAG, tag);
}

static VOID Notify(USBWARP_IRQ_HOST *q)
{
    /* MSI table writes, masking and delivery share this lock. The service
     * never participates in the completion path. */
    KIRQL level;
    ULONGLONG token, seq, result;
    struct input {
        ULONGLONG Partition, Device;
        ULONG Source, Reserved, Address, Data;
    } *in;
    C_ASSERT(sizeof(struct input) == 32);
    KeAcquireSpinLock(&q->Lock, &level);
    token = q->Cb->irq_arm_token;
    seq = q->Cb->irq_event_seq;
    KeMemoryBarrier();
    if ((token & 1) && token != q->LastToken && seq != q->Cb->irq_ack_seq)
        q->Cb->irq_pending = 1;
    if (!q->Cb->irq_pending || !(q->Config.Flags & USBWARP_IRQ_CONFIG_ENABLED) ||
        (q->Config.Flags & USBWARP_IRQ_CONFIG_MASKED)) goto out;
    if (q->Config.MsiAddress >> 32) {
        KeReleaseSpinLock(&q->Lock, level);
        Fail(q, (ULONG)STATUS_NOT_SUPPORTED); return;
    }
    in = (PVOID)((PUCHAR)q->Pages + 2 * PAGE_SIZE);
    RtlZeroMemory(in, sizeof(*in));
    in->Partition = q->Partition;
    in->Device = ((ULONG)q->Config.InstanceId[5] << 24) |
                 ((ULONG)q->Config.InstanceId[4] << 16) |
                 ((ULONG)q->Config.InstanceId[7] << 8) |
                 (q->Config.InstanceId[6] & 0xf8);
    in->Source = 1;
    in->Address = (ULONG)q->Config.MsiAddress;
    in->Data = q->Config.MsiData;
    KeMemoryBarrier();
    result = q->Invoke(0x80, q->NotifyPa, 0);
    q->Cb->irq_last_status = (ULONG)(result & 0xffff);
    if (!(result & 0xffff)) {
        q->LastToken = token;
        q->Cb->irq_pending = 0;
        ++q->Cb->irq_sent;
    }
    KeReleaseSpinLock(&q->Lock, level);
    if (result & 0xffff) Fail(q, (ULONG)(result & 0xffff));
    return;
out:
    KeReleaseSpinLock(&q->Lock, level);
}

VOID UsbWarpIrqPoll(PUSBWARP_GLOBAL_CONTEXT ctx)
{
    USBWARP_IRQ_HOST *q = &ctx->Irq;
    volatile struct usbwarp_control_block *cb = q->Cb;
    ULONGLONG match, value;
    KIRQL level;
    BOOLEAN configured;
    if (!cb) return;
    KeAcquireSpinLock(&q->Lock, &level);
    configured = q->Configured;
    KeReleaseSpinLock(&q->Lock, level);
    if (!configured || q->State == USBWARP_IRQ_FAILED) return;
    if (q->State == USBWARP_IRQ_READY) {
        if (cb->irq_guest_state == USBWARP_IRQ_FAILED) { Fail(q, (ULONG)STATUS_CANCELLED); return; }
        Notify(q); return;
    }
    if (!q->State) {
        if (!ctx->Negotiated || cb->irq_guest_state != USBWARP_IRQ_BEGIN) return;
        KeMemoryBarrier();
        if (cb->irq_pci_function || RtlCompareMemory((const void *)cb->irq_instance_id,
            q->Config.InstanceId, 16) != 16) { Fail(q, (ULONG)STATUS_OBJECT_TYPE_MISMATCH); return; }
        cb->irq_generation = q->Config.Nonce;
        Request(q, USBWARP_IRQ_BEGIN, 0);
        return;
    }
    if (KeQueryInterruptTime() > q->Deadline) { Fail(q, (ULONG)STATUS_IO_TIMEOUT); return; }
    if (q->State == USBWARP_IRQ_TEST) {
        Notify(q);
        if (q->State == USBWARP_IRQ_TEST && cb->irq_received != q->TestReceived) {
            Request(q, USBWARP_IRQ_READY, 0);
            KdPrint(("UsbWarp: native MSI-X ready, partition=%llu\n", q->Partition));
        }
        return;
    }
    if (ReadULongAcquire((volatile ULONG *)&cb->irq_ack_request) != q->Request) return;
    if (cb->irq_marker_result) { Fail(q, (ULONG)STATUS_UNSUCCESSFUL); return; }
    if (q->State == USBWARP_IRQ_BEGIN) {
        q->Original = cb->irq_original_os_id;
        if ((q->Original >> 56) != 0x81) { Fail(q, (ULONG)STATUS_INVALID_PARAMETER); return; }
        q->Round = 0;
        Tag(q);
    } else if (q->State == USBWARP_IRQ_TAG) {
        if (!Match(q, (q->Original & ~0xffffULL) | q->Challenge, &match) ||
            (q->Round && q->Partition != match)) { Fail(q, (ULONG)STATUS_NOT_FOUND); return; }
        q->Partition = match;
        if (++q->Round == 4) Request(q, USBWARP_IRQ_RESTORE, 0);
        else Tag(q);
    } else if (q->State == USBWARP_IRQ_RESTORE) {
        if (Query(q, 0x44, q->Partition, 0x70000, &value) || value != q->Original) {
            Fail(q, (ULONG)STATUS_DATA_ERROR); return;
        }
        q->TestReceived = cb->irq_received;
        cb->irq_partition_id = q->Partition;
        Request(q, USBWARP_IRQ_TEST, 0);
        InterlockedIncrement64((volatile LONG64 *)&cb->irq_event_seq);
    }
}
