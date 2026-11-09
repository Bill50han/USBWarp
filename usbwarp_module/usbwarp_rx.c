// SPDX-License-Identifier: GPL-2.0-only
#include "usbwarp_guest.h"

static bool irq_softirq = true;
module_param(irq_softirq, bool, 0644);
MODULE_PARM_DESC(irq_softirq, "Drain data completions in the IRQ CPU's softirq; false uses the worker");

/* NAPI-style deferred IRQ re-enable. Disabled until explicitly requested:
 * timer accuracy and the appropriate interval depend on the hypervisor. */
static unsigned int rx_repoll_us;
static unsigned int rx_repoll_count = 1;
static int set_repoll_bound(const char *value, const struct kernel_param *kp, unsigned int max)
{
    unsigned int n;
    int ret = kstrtouint(value, 0, &n);
    if (ret) return ret;
    if (n > max) return -EINVAL;
    WRITE_ONCE(*(unsigned int *)kp->arg, n);
    return 0;
}
static int set_repoll_us(const char *value, const struct kernel_param *kp)
{ return set_repoll_bound(value, kp, 250); }
static int set_repoll_count(const char *value, const struct kernel_param *kp)
{ return set_repoll_bound(value, kp, 16); }
static const struct kernel_param_ops repoll_us_ops = { .set=set_repoll_us, .get=param_get_uint };
static const struct kernel_param_ops repoll_count_ops = { .set=set_repoll_count, .get=param_get_uint };
module_param_cb(rx_repoll_us, &repoll_us_ops, &rx_repoll_us, 0644);
module_param_cb(rx_repoll_count, &repoll_count_ops, &rx_repoll_count, 0644);
MODULE_PARM_DESC(rx_repoll_us, "Deferred IRQ repoll interval us (0=off, 1..250)");
MODULE_PARM_DESC(rx_repoll_count, "Maximum empty repolls (0..16), also bounded by 1 ms since progress");

/* Experimental bounded busy polling, independent of the deferred-IRQ timer.
 * USB transfer type and length are conservative hints, not device protocols.
 * No Interrupt/ISO submission starts a window; no in-flight count keeps it
 * alive. Keeping IRQs armed between drains lets softirq make progress if the
 * poller is preempted. rx_lock serializes both consumers for each drain. */
static unsigned int rx_busy_us;
static unsigned int rx_busy_bytes = 128;
static int set_busy_us(const char *value, const struct kernel_param *kp)
{ return set_repoll_bound(value, kp, 1000); }
static int set_busy_bytes(const char *value, const struct kernel_param *kp)
{ return set_repoll_bound(value, kp, USBWARP_MAX_TRANSFER_SIZE); }
static const struct kernel_param_ops busy_us_ops = { .set=set_busy_us, .get=param_get_uint };
static const struct kernel_param_ops busy_bytes_ops = { .set=set_busy_bytes, .get=param_get_uint };
module_param_cb(rx_busy_us, &busy_us_ops, &rx_busy_us, 0644);
module_param_cb(rx_busy_bytes, &busy_bytes_ops, &rx_busy_bytes, 0644);
MODULE_PARM_DESC(rx_busy_us, "Busy-poll window after short Control/Bulk publication, us (0=off, max 1000)");
MODULE_PARM_DESC(rx_busy_bytes, "Maximum Control/Bulk request bytes eligible for busy polling (default 128)");

static void busy_extend(struct usbwarp_hcd *w, u64 now, unsigned int us)
{
    u64 old = atomic64_read(&w->rx_busy_until), until = now + (u64)us * 1000;
    while (old < until) {
        u64 observed = atomic64_cmpxchg(&w->rx_busy_until, old, until);
        if (observed == old) break;
        old = observed;
    }
    atomic64_inc(&w->rx_busy_kicks);
    if (old <= now) wake_up(&w->irq_wait);
}

bool usbwarp_rx_submit(struct usbwarp_hcd *w, u32 type, u32 length)
{
    unsigned int us = READ_ONCE(rx_busy_us);
    if (!us || !READ_ONCE(irq_softirq) || !usbwarp_irq_ready(w) ||
        READ_ONCE(w->rx_stopping) || READ_ONCE(w->quiescing) || READ_ONCE(w->shutting_down) ||
        atomic_read(&w->iso_pending) || length > READ_ONCE(rx_busy_bytes) ||
        (type != USBWARP_XFER_CONTROL && type != USBWARP_XFER_BULK)) return false;
    atomic64_inc(&w->rx_busy_submits);
    busy_extend(w, ktime_get_ns(), us);
    return true;
}

void usbwarp_rx_progress(struct usbwarp_hcd *w, bool eligible, u64 published_ns, int status)
{
    unsigned int us = READ_ONCE(rx_busy_us);
    u64 now;
    if (!eligible || status || !us || !READ_ONCE(irq_softirq) ||
        !usbwarp_irq_ready(w) || READ_ONCE(w->rx_stopping) || READ_ONCE(w->quiescing) ||
        READ_ONCE(w->shutting_down) || atomic_read(&w->iso_pending)) return;
    now = ktime_get_ns();
    /* A small receive that waited seconds is not evidence of a fast stream.
     * Failed/cancelled requests never renew a window. */
    if (!published_ns || now < published_ns || now - published_ns > (u64)us * 1000) {
        atomic64_inc(&w->rx_busy_slow);
        return;
    }
    atomic64_inc(&w->rx_busy_fast);
    busy_extend(w, now, us);
}

bool usbwarp_rx_busy_pending(struct usbwarp_hcd *w)
{
    return READ_ONCE(rx_busy_us) && READ_ONCE(irq_softirq) && READ_ONCE(w->rx_fast) &&
        !READ_ONCE(w->rx_stopping) && !READ_ONCE(w->rx_control_pending) &&
        !READ_ONCE(w->quiescing) && !READ_ONCE(w->shutting_down) &&
        !READ_ONCE(w->irq_failed) && !atomic_read(&w->iso_pending) &&
        (u64)atomic64_read(&w->rx_busy_until) > ktime_get_ns();
}

/* rx_lock protects policy state. The hard timer callback never takes it:
 * it can interrupt the tasklet on the same CPU while the lock is held. */
static void cancel_repoll(struct usbwarp_hcd *w)
{
    if (w->rx_repoll_active) {
        w->rx_repoll_active = false;
        hrtimer_try_to_cancel(&w->rx_repoll_timer);
    }
}

static void reset_repoll(struct usbwarp_hcd *w)
{
    cancel_repoll(w);
    w->rx_repoll_left = 0;
    w->rx_repoll_until = 0;
}

static void repoll_progress(struct usbwarp_hcd *w, unsigned int type)
{
    unsigned int us = READ_ONCE(rx_repoll_us), count = READ_ONCE(rx_repoll_count);
    if (!us || !count || type == USBWARP_MSG_ISO_COMPLETE || atomic_read(&w->iso_pending)) {
        reset_repoll(w);
        return;
    }
    w->rx_repoll_left = count;
    w->rx_repoll_until = ktime_get_ns() + (u64)min(us * count, 1000u) * 1000;
}

static bool defer_irq(struct usbwarp_hcd *w)
{
    unsigned int us = READ_ONCE(rx_repoll_us);
    u64 now, delay;
    if (w->rx_busy_active || !us || !READ_ONCE(rx_repoll_count) || !w->rx_repoll_left ||
        atomic_read(&w->iso_pending) ||
        !usbwarp_irq_ready(w)) {
        reset_repoll(w);
        return false;
    }
    now = ktime_get_ns();
    if (now >= w->rx_repoll_until) { reset_repoll(w); return false; }
    delay = min_t(u64, (u64)us * 1000, w->rx_repoll_until - now);
    --w->rx_repoll_left;
    w->rx_repoll_active = true;
    /* Pair with ISO admission: either it sees active and wakes the worker,
     * or this recheck cancels suppression before starting a timer. */
    smp_mb();
    if (atomic_read(&w->iso_pending)) { reset_repoll(w); return false; }
    WRITE_ONCE(w->rx_repoll_due, now + delay);
    WRITE_ONCE(w->rx_repoll_started, now);
    ++w->rx_repoll_arms;
    hrtimer_start(&w->rx_repoll_timer, ns_to_ktime(delay), HRTIMER_MODE_REL_PINNED);
    return true;
}

static enum hrtimer_restart usbwarp_repoll_timer(struct hrtimer *timer)
{
    struct usbwarp_hcd *w = container_of(timer, struct usbwarp_hcd, rx_repoll_timer);
    u64 now = ktime_get_ns(), due = READ_ONCE(w->rx_repoll_due);
    u64 late = now > due ? now - due : 0;
    u64 started = READ_ONCE(w->rx_repoll_started);
    ++w->rx_repoll_fires;
    w->rx_repoll_late_ns += late;
    if (now > started) w->rx_repoll_delay_ns += now - started;
    if (late > w->rx_repoll_late_max) w->rx_repoll_late_max = late;
    if (READ_ONCE(w->rx_fast) && !READ_ONCE(w->rx_stopping) && !READ_ONCE(w->rx_control_pending)) {
        atomic_set(&w->rx_repoll_pending, 1);
        tasklet_schedule(&w->rx_tasklet);
    }
    return HRTIMER_NORESTART;
}

/* Only these handlers are atomic: bounded metadata validation, SG_MITER_ATOMIC
 * copies, spinlocks, and HCD_BH giveback. Management stays on the worker. */
static bool dispatch_completion(struct usbwarp_hcd *w, const void *message, u32 length)
{
    const struct usbwarp_msg_header *hdr = message;
    if (hdr->message_type == USBWARP_MSG_URB_COMPLETE) {
        if (length >= USBWARP_MSG_URB_COMPLETE_BASE_SIZE)
            usbwarp_complete_urb(w, message);
        return true;
    }
    if (hdr->message_type == USBWARP_MSG_ISO_COMPLETE) {
        if (length >= USBWARP_MSG_ISO_COMPLETE_BASE_SIZE)
            usbwarp_complete_iso_urb(w, message, length);
        return true;
    }
    return false;
}

static bool rx_has_data(struct usbwarp_hcd *w)
{
    return ioread32(&w->h2g.hdr->producer_index) != ioread32(&w->h2g.hdr->consumer_index);
}

/* Caller holds rx_lock with bottom halves disabled, or is the tasklet. */
static void rx_drain_locked(struct usbwarp_hcd *w)
{
    u64 began = ktime_get_ns(), sequence;
    unsigned int budget = 32;
    bool timer_poll, found = false, overdue;

    /* Hard IRQs stay enabled while copying data. The hard handler never
     * acquires rx_lock; it only schedules this tasklet or wakes the worker. */
    timer_poll = atomic_xchg(&w->rx_repoll_pending, 0);
    /* After a large scheduling delay, do not extend suppression merely
     * because work accumulated while this timer was unable to run. */
    overdue = w->rx_repoll_active && began > w->rx_repoll_until + 1000000;
    cancel_repoll(w);
    if (!w->rx_fast || w->rx_stopping || w->rx_control_pending || READ_ONCE(w->shutting_down))
        goto out;
    ++w->rx_batches;
    usbwarp_irq_disarm(w);
    while (budget--) {
        u32 length = 0;
        int ret = usbwarp_ring_consume(&w->h2g, w->rx_buffer, USBWARP_MAX_MESSAGE_SIZE, &length);
        if (ret == -EAGAIN) {
            if (defer_irq(w)) goto out;
            if (!usbwarp_irq_arm(w, false, &sequence)) goto out;
            usbwarp_irq_disarm(w);
            continue;
        }
        if (ret) {
            /* A malformed ring must not spin indefinitely in softirq. Let
             * process-context liveness/error handling retire the session. */
            WRITE_ONCE(w->irq_failed, true);
            w->rx_error = ret;
            w->rx_fast = false;
            reset_repoll(w);
            wake_up(&w->irq_wait);
            goto out;
        }
        if (usbwarp_validate_h2g_message(w, w->rx_buffer, length)) {
            found = true;
            if (dispatch_completion(w, w->rx_buffer, length)) {
                ++w->rx_completions;
                if (!overdue) repoll_progress(w, ((struct usbwarp_msg_header *)w->rx_buffer)->message_type);
                else reset_repoll(w);
            } else {
                /* Preserve wire ordering even when the control handler
                 * sleeps. No consumer can pass this message until it ends. */
                memcpy(w->poll_buffer, w->rx_buffer, length);
                w->rx_control_length = length;
                w->rx_control_pending = true;
                reset_repoll(w);
                ++w->rx_deferrals;
                wake_up(&w->irq_wait);
                goto out;
            }
        }
        if (ktime_get_ns() - began >= 100000) break; /* 100 us, checked between URBs */
    }
    /* A large copy may use the time budget but empty the ring. Re-arm now
     * instead of delaying the next completion behind an empty tasklet. */
    if (!rx_has_data(w)) {
        if (defer_irq(w) || !usbwarp_irq_arm(w, false, &sequence)) goto out;
    }
    usbwarp_irq_disarm(w);
    /* Like EHB, leave notification disarmed while known work is scheduled.
     * The softirq budget can defer a sustained stream to ksoftirqd. */
    ++w->rx_reschedules;
    tasklet_schedule(&w->rx_tasklet);
out:
    if (timer_poll) {
        if (found) ++w->rx_repoll_hits;
        else ++w->rx_repoll_empty;
    }
    if (atomic_read(&w->tx_pending)) wake_up(&w->irq_wait);
}

static void usbwarp_rx_tasklet(struct tasklet_struct *tasklet)
{
    struct usbwarp_hcd *w = from_tasklet(w, tasklet, rx_tasklet);
    spin_lock(&w->rx_lock);
    rx_drain_locked(w);
    spin_unlock(&w->rx_lock);
}

bool usbwarp_rx_busy_poll(struct usbwarp_hcd *w)
{
    u64 began, deadline, sequence;
    bool yielded = false;
    if (!usbwarp_rx_busy_pending(w) || !usbwarp_irq_ready(w)) return false;
    began = ktime_get_ns();
    deadline = began + (u64)READ_ONCE(rx_busy_us) * 1000;
    spin_lock_bh(&w->rx_lock);
    if (!usbwarp_rx_busy_pending(w)) { spin_unlock_bh(&w->rx_lock); return false; }
    reset_repoll(w);
    w->rx_busy_active = true;
    ++w->rx_busy_windows;
    /* Re-enable notification before the CPU starts checking an empty ring.
     * Completion can be handled by softirq while this thread is descheduled. */
    if (usbwarp_irq_arm(w, false, &sequence)) tasklet_schedule(&w->rx_tasklet);
    spin_unlock_bh(&w->rx_lock);
    while (usbwarp_rx_busy_pending(w) && ktime_get_ns() < deadline && !kthread_should_stop()) {
        if (need_resched()) { yielded = true; break; }
        ++w->rx_busy_polls;
        if (rx_has_data(w)) {
            spin_lock_bh(&w->rx_lock);
            if (w->rx_fast && !w->rx_stopping && !w->rx_control_pending) {
                u64 before = w->rx_completions;
                rx_drain_locked(w);
                w->rx_busy_completions += w->rx_completions - before;
            }
            spin_unlock_bh(&w->rx_lock);
        } else cpu_relax();
    }
    spin_lock_bh(&w->rx_lock);
    w->rx_busy_active = false;
    w->rx_busy_ns += ktime_get_ns() - began; /* elapsed time, not CPU time */
    if (yielded) ++w->rx_busy_yields;
    if (w->rx_fast && !w->rx_stopping && !w->rx_control_pending && usbwarp_irq_ready(w)) {
        if (usbwarp_irq_arm(w, false, &sequence)) tasklet_schedule(&w->rx_tasklet);
    }
    spin_unlock_bh(&w->rx_lock);
    return true;
}

void usbwarp_rx_init(struct usbwarp_hcd *w)
{
    spin_lock_init(&w->rx_lock);
    tasklet_setup(&w->rx_tasklet, usbwarp_rx_tasklet);
    hrtimer_init(&w->rx_repoll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
    w->rx_repoll_timer.function = usbwarp_repoll_timer;
    atomic_set(&w->rx_repoll_pending, 0);
    atomic64_set(&w->rx_busy_until, 0);
    atomic64_set(&w->rx_busy_kicks, 0);
    atomic64_set(&w->rx_busy_submits, 0);
    atomic64_set(&w->rx_busy_fast, 0);
    atomic64_set(&w->rx_busy_slow, 0);
}

bool usbwarp_rx_interrupt(struct usbwarp_hcd *w)
{
    if (!READ_ONCE(w->rx_fast) || READ_ONCE(w->rx_stopping)) return false;
    tasklet_schedule(&w->rx_tasklet);
    return true;
}

void usbwarp_rx_stop(struct usbwarp_hcd *w)
{
    spin_lock_bh(&w->rx_lock);
    w->rx_stopping = true;
    w->rx_fast = false;
    atomic64_set(&w->rx_busy_until, 0);
    reset_repoll(w);
    if (w->cb) usbwarp_irq_disarm(w);
    spin_unlock_bh(&w->rx_lock);
    /* An IRQ that saw the old mode can still be about to schedule work. */
    if (w->irq_requested) synchronize_irq(w->irq_vector);
    hrtimer_cancel(&w->rx_repoll_timer);
    tasklet_kill(&w->rx_tasklet);
}

void usbwarp_rx_watchdog(struct usbwarp_hcd *w)
{
    spin_lock_bh(&w->rx_lock);
    if (w->rx_fast && !w->rx_stopping && !w->rx_control_pending && rx_has_data(w)) {
        if ((w->irq_token & 1) && !atomic_read(&w->irq_pending) &&
            ktime_get_mono_fast_ns() - READ_ONCE(w->irq_last_ns) >= 10000000)
            writeq(readq(&w->cb->irq_watchdog) + 1, &w->cb->irq_watchdog);
        tasklet_schedule(&w->rx_tasklet);
    }
    spin_unlock_bh(&w->rx_lock);
}

bool usbwarp_rx_priority_pending(struct usbwarp_hcd *w)
{
    return READ_ONCE(w->rx_repoll_active) && atomic_read(&w->iso_pending);
}

bool usbwarp_rx_worker(struct usbwarp_hcd *w)
{
    u32 length = 0;
    bool desired, got = false;
    int error = 0;
    spin_lock_bh(&w->rx_lock);
    desired = !w->rx_stopping && !READ_ONCE(w->shutting_down) &&
        READ_ONCE(irq_softirq) && usbwarp_irq_ready(w);
    if (desired != w->rx_fast) {
        reset_repoll(w);
        usbwarp_irq_disarm(w);
        w->rx_fast = desired;
        if (desired) tasklet_schedule(&w->rx_tasklet);
    }
    if (w->rx_fast && usbwarp_rx_priority_pending(w)) {
        reset_repoll(w);
        tasklet_schedule(&w->rx_tasklet);
    }
    if (w->rx_control_pending) {
        length = w->rx_control_length;
        got = true;
    } else if (!w->rx_fast && !w->rx_stopping) {
        usbwarp_irq_disarm(w);
        int ret = usbwarp_ring_consume(&w->h2g, w->poll_buffer, USBWARP_MAX_MESSAGE_SIZE, &length);
        if (!ret && length) {
            w->rx_control_pending = true;
            w->rx_control_length = length;
            got = true;
        } else if (ret != -EAGAIN) w->rx_error = ret;
    } else if (w->rx_fast && rx_has_data(w)) {
        tasklet_schedule(&w->rx_tasklet);
    }
    error = w->rx_error;
    spin_unlock_bh(&w->rx_lock);
    if (error) {
        dev_err(&w->pdev->dev, "usbwarp: invalid H2G ring (%d); retiring session\n", error);
        iowrite32(USBWARP_STATE_ERROR, &w->cb->guest_state);
        usbwarp_shutdown_all_devices(w, USBWARP_REMOVE_HOST_SHUTDOWN);
        usbwarp_retire_hcd(w);
        return true;
    }
    if (!got) return false;
    if (usbwarp_validate_h2g_message(w, w->poll_buffer, length))
        usbwarp_dispatch_message(w, w->poll_buffer, length);
    spin_lock_bh(&w->rx_lock);
    w->rx_control_pending = false;
    if (w->rx_fast && !w->rx_stopping) tasklet_schedule(&w->rx_tasklet);
    spin_unlock_bh(&w->rx_lock);
    return true;
}
