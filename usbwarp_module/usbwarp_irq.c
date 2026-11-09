// SPDX-License-Identifier: GPL-2.0-only
#include "usbwarp_guest.h"
#include <linux/interrupt.h>
#include <linux/hyperv.h>
#include <linux/irq.h>
#ifdef CONFIG_X86_64
#include <asm/msr.h>
#endif

static bool h2g_irq;
module_param(h2g_irq, bool, 0644);
MODULE_PARM_DESC(h2g_irq, "Opt-in MSI-X allocation/waits; default uses adaptive polling");

static bool irq_affinity = true;
module_param(irq_affinity, bool, 0644);
MODULE_PARM_DESC(irq_affinity, "Follow effective MSI-X CPU affinity on the completion worker");

/* A wakeup on another Hyper-V VP can itself require a virtual IPI. Keep
 * the draining worker with its interrupt, like a threaded IRQ handler.
 * This runs only on that worker, outside all driver locks; changing CPU
 * affinity may sleep. IRQ descriptors remain owned until poll_stop joins.
 * Polling/failure fallback restores the worker's original allowed CPUs. */
#ifdef CONFIG_X86_64
static void follow_irq_affinity(struct usbwarp_hcd *w)
{
    const struct cpumask *mask;
    bool enabled;

    if (!w->irq_requested) return;
    if (!w->irq_affinity_saved) {
        cpumask_copy(&w->irq_original_cpus, current->cpus_ptr);
        w->irq_affinity_saved = true;
    }
    enabled = READ_ONCE(irq_affinity) && READ_ONCE(h2g_irq) &&
        !READ_ONCE(w->irq_failed) &&
        ioread32(&w->cb->irq_host_state) == USBWARP_IRQ_READY;
    if (enabled) {
        mask = irq_get_effective_affinity_mask(w->irq_vector);
        if (mask && cpumask_intersects(mask, cpu_active_mask)) {
            if (w->irq_affinity_applied && cpumask_equal(current->cpus_ptr, mask)) return;
            if (cpumask_equal(current->cpus_ptr, mask) || !set_cpus_allowed_ptr(current, mask)) {
                w->irq_affinity_applied = true;
                WRITE_ONCE(w->irq_affinity_cpu, cpumask_weight(mask) == 1 ? (int)cpumask_first(mask) : -1);
            }
            return;
        }
    }
    if (w->irq_affinity_applied && !set_cpus_allowed_ptr(current, &w->irq_original_cpus)) {
        w->irq_affinity_applied = false;
        WRITE_ONCE(w->irq_affinity_cpu, -1);
    }
}
#endif

/* GuestOsId is VM-wide. Only a short-lived discovery marker is written;
 * retain the Linux OS/version fields and never disable the hypercall page.
 * Ownership is protected across devices AND the independent watchdog. */
static DEFINE_MUTEX(marker_lock);
static struct usbwarp_hcd *marker_owner;

u64 usbwarp_irq_os_id(void)
{
    u64 value = 0;
#ifdef CONFIG_X86_64
    if (rdmsrl_safe(0x40000000, &value)) return 0;
#endif
    return value;
}

static int restore_locked(struct usbwarp_hcd *w)
{
    int ret = 0;
#ifdef CONFIG_X86_64
    u64 observed;
    if (w->marker_owned) {
        ret = rdmsrl_safe(0x40000000, &observed);
        if (!ret && observed != w->marker_last && observed != w->marker_original)
            ret = -EBUSY;
        if (!ret && observed != w->marker_original)
            ret = wrmsrl_safe(0x40000000, w->marker_original);
        if (!ret) {
            ret = rdmsrl_safe(0x40000000, &observed);
            if (!ret && observed != w->marker_original) ret = -EIO;
        }
        if (ret) dev_err(&w->pdev->dev, "GuestOsId restore failed: %d\n", ret);
    }
#endif
    w->marker_owned = false;
    if (marker_owner == w) marker_owner = NULL;
    return ret;
}

void usbwarp_irq_restore(struct usbwarp_hcd *w)
{
    if (!w->irq_requested) return;
    cancel_delayed_work_sync(&w->marker_watchdog);
    mutex_lock(&marker_lock);
    restore_locked(w);
    mutex_unlock(&marker_lock);
}

static void marker_expired(struct work_struct *work)
{
    struct usbwarp_hcd *w = container_of(to_delayed_work(work), struct usbwarp_hcd, marker_watchdog);
    mutex_lock(&marker_lock);
    if (w->marker_owned) {
        restore_locked(w);
        w->irq_failed = true;
        iowrite32(USBWARP_IRQ_FAILED, &w->cb->irq_guest_state);
        wake_up(&w->irq_wait);
        dev_warn(&w->pdev->dev, "GuestOsId marker timed out; restored, using polling\n");
    }
    mutex_unlock(&marker_lock);
}

static irqreturn_t usbwarp_interrupt(int irq, void *context)
{
    struct usbwarp_hcd *w = context;
    WRITE_ONCE(w->irq_last_ns, ktime_get_mono_fast_ns());
    writeq(atomic64_inc_return(&w->irq_count), &w->cb->irq_received);
    atomic_set(&w->irq_pending, 1);
    if (!usbwarp_rx_interrupt(w)) wake_up(&w->irq_wait);
    return IRQ_HANDLED;
}

void usbwarp_irq_init(struct usbwarp_hcd *w)
{
    struct device *parent;
    int ret;
    usbwarp_rx_init(w);
    init_waitqueue_head(&w->irq_wait);
    INIT_DELAYED_WORK(&w->marker_watchdog, marker_expired);
    atomic_set(&w->irq_pending, 0);
    atomic64_set(&w->irq_count, 0);
    w->irq_vector = -1;
    w->irq_last_ns = ktime_get_mono_fast_ns();
    w->irq_affinity_cpu = -1;
    if (!IS_ENABLED(CONFIG_X86_64) || !h2g_irq ||
        !(ioread32(&w->cb->flags) & USBWARP_FLAG_H2G_IRQ)) return;
    /* Validate the same instance GUID from which pci-hyperv builds the
     * logical device ID. Do not guess from the PCI domain or USB identity. */
    for (parent = w->pdev->dev.parent; parent; parent = parent->parent)
        if (parent->bus && !strcmp(parent->bus->name, "vmbus")) break;
    if (!parent || PCI_FUNC(w->pdev->devfn)) return;
    memcpy_toio(w->cb->irq_instance_id, container_of(parent, struct hv_device, device)->dev_instance.b, 16);
    iowrite32(PCI_FUNC(w->pdev->devfn), &w->cb->irq_pci_function);
    ret = pci_alloc_irq_vectors(w->pdev, 1, 1, PCI_IRQ_MSIX);
    if (ret < 0) goto failed;
    w->irq_vector = pci_irq_vector(w->pdev, 0);
    ret = request_irq(w->irq_vector, usbwarp_interrupt, 0, "usbwarp", w);
    if (ret) { pci_free_irq_vectors(w->pdev); w->irq_vector = -1; goto failed; }
    w->irq_requested = true;
    wmb();
    iowrite32(USBWARP_IRQ_BEGIN, &w->cb->irq_guest_state);
    return;
failed:
    dev_warn(&w->pdev->dev, "MSI-X allocation failed: %d; using polling\n", ret);
}

void usbwarp_irq_cleanup(struct usbwarp_hcd *w)
{
    usbwarp_rx_stop(w);
    if (!w->irq_requested) return;
    usbwarp_irq_restore(w);
    writeq(0, &w->cb->irq_arm_token);
    iowrite32(USBWARP_IRQ_FAILED, &w->cb->irq_guest_state);
    free_irq(w->irq_vector, w);
    pci_free_irq_vectors(w->pdev);
    w->irq_requested = false;
}

void usbwarp_irq_step(struct usbwarp_hcd *w)
{
#ifdef CONFIG_X86_64
    u32 request, state;
    u64 observed, tag;
    int ret = 0;
    follow_irq_affinity(w);
    if (!w->irq_requested || READ_ONCE(w->irq_failed)) return;
    request = ioread32(&w->cb->irq_request_seq);
    rmb();
    if (request == w->irq_request) return;
    state = ioread32(&w->cb->irq_host_state);
    mutex_lock(&marker_lock);
    if (state == USBWARP_IRQ_BEGIN) {
        if (marker_owner && marker_owner != w) { mutex_unlock(&marker_lock); return; }
        ret = rdmsrl_safe(0x40000000, &observed);
        if (!ret && (observed >> 56) != 0x81) ret = -EINVAL;
        if (!ret) {
            w->marker_original = w->marker_last = observed;
            marker_owner = w;
            w->marker_owned = true;
            writeq(observed, &w->cb->irq_original_os_id);
            schedule_delayed_work(&w->marker_watchdog, msecs_to_jiffies(3000));
        }
    } else if (state == USBWARP_IRQ_TAG) {
        tag = readq(&w->cb->irq_challenge);
        if (!w->marker_owned || marker_owner != w || tag > 0xffff) ret = -EINVAL;
        else {
            ret = rdmsrl_safe(0x40000000, &observed);
            if (!ret && observed != w->marker_last) ret = -EBUSY;
            if (!ret) {
                u64 next = (w->marker_original & ~0xffffULL) | tag;
                ret = wrmsrl_safe(0x40000000, next);
                if (!ret) {
                    w->marker_last = next;
                    ret = rdmsrl_safe(0x40000000, &observed);
                    if (!ret && observed != next) ret = -EIO;
                }
            }
        }
    } else if (state == USBWARP_IRQ_RESTORE || state == USBWARP_IRQ_FAILED) {
        ret = restore_locked(w);
        if (state == USBWARP_IRQ_FAILED) ret = -ENOTSUPP;
    } else if (state != USBWARP_IRQ_TEST && state != USBWARP_IRQ_READY) ret = -EPROTO;
    if (ret) {
        restore_locked(w);
        w->irq_failed = true;
        iowrite32(USBWARP_IRQ_FAILED, &w->cb->irq_guest_state);
    } else iowrite32(state, &w->cb->irq_guest_state);
    iowrite32(ret, &w->cb->irq_marker_result);
    wmb();
    iowrite32(request, &w->cb->irq_ack_request);
    w->irq_request = request;
    mutex_unlock(&marker_lock);
    if (w->doorbell) writeq(1, w->doorbell);
    if (state == USBWARP_IRQ_READY && !ret)
        dev_info(&w->pdev->dev, "native MSI-X notification active, IRQ %d\n", w->irq_vector);
#endif
}

void usbwarp_irq_disarm(struct usbwarp_hcd *w)
{
    if (!w->irq_requested) return;
    w->irq_token &= ~1ULL;
    writeq(w->irq_token, &w->cb->irq_arm_token);
    atomic_set(&w->irq_pending, 0);
}

bool usbwarp_irq_ready(struct usbwarp_hcd *w)
{
    return w->irq_requested && READ_ONCE(h2g_irq) && !READ_ONCE(w->irq_failed) &&
        ioread32(&w->cb->irq_host_state) == USBWARP_IRQ_READY;
}

/* Caller serializes both arm-token writers with rx_lock. As on an xHCI
 * event ring, publishing readiness must precede the final empty check. */
bool usbwarp_irq_arm(struct usbwarp_hcd *w, bool test, u64 *sequence)
{
    *sequence = readq(&w->cb->irq_event_seq);
    writeq(test ? *sequence - 1 : *sequence, &w->cb->irq_ack_seq);
    w->irq_token = (w->irq_token + 2) | 1;
    writeq(w->irq_token, &w->cb->irq_arm_token);
    mb();
    return atomic_read(&w->irq_pending) ||
        ioread32(&w->h2g.hdr->producer_index) != ioread32(&w->h2g.hdr->consumer_index) ||
        readq(&w->cb->irq_event_seq) != *sequence;
}

bool usbwarp_irq_wait(struct usbwarp_hcd *w)
{
    u32 state;
    u64 sequence;
    unsigned long timeout = max(1UL, msecs_to_jiffies(10)), deadline;
    if (!w->irq_requested || !READ_ONCE(h2g_irq) || READ_ONCE(w->irq_failed) || atomic_read(&w->tx_pending)) return false;
    state = ioread32(&w->cb->irq_host_state);
    if (state != USBWARP_IRQ_READY && state != USBWARP_IRQ_TEST) return false;
    if (READ_ONCE(w->rx_fast)) {
        deadline = jiffies + timeout;
        wait_event_timeout(w->irq_wait,
            READ_ONCE(w->rx_control_pending) || !READ_ONCE(w->rx_fast) ||
            atomic_read(&w->tx_pending) || usbwarp_rx_priority_pending(w) || usbwarp_rx_busy_pending(w) ||
            kthread_should_stop() || READ_ONCE(w->shutting_down) ||
            !usbwarp_irq_ready(w), timeout);
        if (time_after_eq(jiffies, deadline)) usbwarp_rx_watchdog(w);
        return true;
    }
    spin_lock_bh(&w->rx_lock);
    if (usbwarp_irq_arm(w, state == USBWARP_IRQ_TEST, &sequence)) {
        spin_unlock_bh(&w->rx_lock);
        return true;
    }
    spin_unlock_bh(&w->rx_lock);
    if (state == USBWARP_IRQ_TEST && w->doorbell) writeq(1, w->doorbell);
    /* Publish arm before rechecking work. Producer publication before arm
     * is observed here; publication after arm must cause an interrupt. */
    if (ioread32(&w->h2g.hdr->producer_index) != ioread32(&w->h2g.hdr->consumer_index) ||
        readq(&w->cb->irq_event_seq) != sequence) return true;
    deadline = jiffies + timeout;
    wait_event_timeout(w->irq_wait,
        atomic_read(&w->irq_pending) || kthread_should_stop() || READ_ONCE(w->shutting_down) ||
        atomic_read(&w->tx_pending) || readq(&w->cb->irq_event_seq) != sequence || READ_ONCE(w->irq_failed),
        timeout);
    /* wait_event_timeout returns 1, not 0, if its condition becomes true
     * at timeout. Count timer-discovered work even in that case. */
    if (time_after_eq(jiffies, deadline) && !atomic_read(&w->irq_pending) &&
        ioread32(&w->h2g.hdr->producer_index) != ioread32(&w->h2g.hdr->consumer_index))
        writeq(readq(&w->cb->irq_watchdog) + 1, &w->cb->irq_watchdog);
    return true;
}
