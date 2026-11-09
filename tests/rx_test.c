// SPDX-License-Identifier: GPL-2.0-only
/* Compile the production receive consumer with deterministic kernel stand-ins.
 * Checks ownership, sleeping-control barriers, mode switches and retirement. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usbwarp_shared.h"
#include "usbwarp_iso.h"
#define USBWARP_GUEST_H
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t atomic64_t;
typedef pthread_mutex_t spinlock_t;
#define module_param(...)
#define module_param_cb(n,ops,arg,mode) static const void *__attribute__((used)) test_param_##n = (ops)
#define MODULE_PARM_DESC(...)
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(type,a,b) min((type)(a),(type)(b))
struct kernel_param { void *arg; };
struct kernel_param_ops { int (*set)(const char *,const struct kernel_param *); int (*get)(char *,const struct kernel_param *); };
static int param_get_uint(char *p,const struct kernel_param *kp) { return sprintf(p,"%u",*(unsigned *)kp->arg); }
static int kstrtouint(const char *p,unsigned base,unsigned *out) {
    char *end; unsigned long value;
    if(*p=='-') return -EINVAL;
    errno=0; value=strtoul(p,&end,base);
    if(errno || *end || value>UINT_MAX) return -EINVAL;
    *out=(unsigned)value; return 0;
}
#define READ_ONCE(x) __atomic_load_n(&(x),__ATOMIC_ACQUIRE)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x),(v),__ATOMIC_RELEASE)
#define spin_lock_init(p) assert(!pthread_mutex_init((p),NULL))
#define spin_lock(p) assert(!pthread_mutex_lock(p))
#define spin_unlock(p) assert(!pthread_mutex_unlock(p))
static _Thread_local unsigned bh_depth;
#define spin_lock_bh(p) do { ++bh_depth; spin_lock(p); } while(0)
#define spin_unlock_bh(p) do { spin_unlock(p); assert(bh_depth); --bh_depth; } while(0)
#define ioread32(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define iowrite32(v,p) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define readq(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define writeq(v,p) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define atomic_read(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define atomic_set(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define atomic_xchg(p,v) __atomic_exchange_n((p),(v),__ATOMIC_ACQ_REL)
#define atomic64_read atomic_read
#define atomic64_set atomic_set
#define atomic64_inc(p) ((void)__atomic_add_fetch((p),1,__ATOMIC_ACQ_REL))
static u64 atomic64_cmpxchg(atomic64_t *p,u64 old,u64 value)
{ __atomic_compare_exchange_n(p,&old,value,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE); return old; }
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define wake_up(p) ((void)__atomic_add_fetch((p),1,__ATOMIC_RELAXED))
#define dev_err(...) ((void)0)
#define USBWARP_REMOVE_HOST_SHUTDOWN 2
static u64 clock_ns, clock_step;
static u64 ktime_get_ns(void) { u64 n=clock_ns; clock_ns+=clock_step; return n; }
static u64 ktime_get_mono_fast_ns(void) { return clock_ns; }
static bool test_need_resched,test_stop;
static void (*relax_hook)(void);
static bool need_resched(void) { return test_need_resched; }
static bool kthread_should_stop(void) { return test_stop; }
static void cpu_relax(void) { if(relax_hook) relax_hook(); }
enum hrtimer_restart { HRTIMER_NORESTART };
#define HRTIMER_MODE_REL_PINNED 1
struct hrtimer { enum hrtimer_restart (*function)(struct hrtimer *); u64 due; bool active; };
static void hrtimer_init(struct hrtimer *t,int clock,int mode) { (void)clock; (void)mode; t->active=false; }
static int hrtimer_try_to_cancel(struct hrtimer *t) { bool active=t->active; t->active=false; return active; }
static int hrtimer_cancel(struct hrtimer *t) { return hrtimer_try_to_cancel(t); }
static u64 ns_to_ktime(u64 ns) { return ns; }
static void hrtimer_start(struct hrtimer *t,u64 delta,int mode) { (void)mode; t->due=clock_ns+delta; t->active=true; }
#define container_of(p,type,field) ((type *)((char *)(p)-offsetof(type,field)))
struct tasklet_struct { void (*callback)(struct tasklet_struct *); int scheduled; };
#define from_tasklet(v,t,m) ((struct usbwarp_hcd *)((char *)(t)-offsetof(struct usbwarp_hcd,m)))
static void tasklet_setup(struct tasklet_struct *t,void (*f)(struct tasklet_struct *)) { t->callback=f; }
static void tasklet_schedule(struct tasklet_struct *t) { WRITE_ONCE(t->scheduled,1); }
static void tasklet_kill(struct tasklet_struct *t) { WRITE_ONCE(t->scheduled,0); }
static void synchronize_irq(int vector) { (void)vector; }
struct message { struct usbwarp_msg_header hdr; unsigned char padding[USBWARP_MAX_MESSAGE_SIZE-sizeof(struct usbwarp_msg_header)]; };
struct fake_ring { struct usbwarp_ring_header *hdr; struct message messages[128]; bool corrupt; };
struct usbwarp_hcd {
    spinlock_t rx_lock;
    struct tasklet_struct rx_tasklet;
    struct hrtimer rx_repoll_timer;
    int rx_repoll_pending,iso_pending;
    bool rx_repoll_active;
    u32 rx_repoll_left;
    u64 rx_repoll_until,rx_repoll_due,rx_repoll_started,rx_repoll_arms,rx_repoll_fires,rx_repoll_hits,rx_repoll_empty;
    u64 rx_repoll_late_ns,rx_repoll_late_max,rx_repoll_delay_ns;
    atomic64_t rx_busy_until,rx_busy_kicks,rx_busy_submits,rx_busy_fast,rx_busy_slow;
    bool rx_busy_active;
    u64 rx_busy_windows,rx_busy_ns,rx_busy_polls,rx_busy_completions,rx_busy_yields;
    bool rx_fast,rx_stopping,rx_control_pending,shutting_down,quiescing,irq_failed,irq_requested,ready;
    u32 rx_control_length;
    int rx_error,irq_vector,irq_pending,irq_wait,tx_pending;
    u64 irq_token,irq_last_ns,rx_batches,rx_completions,rx_deferrals,rx_reschedules;
    void *rx_buffer,*poll_buffer;
    struct usbwarp_control_block *cb;
    struct fake_ring h2g;
    unsigned count,order[128],retired;
    int block_control,control_entered,release_control;
};
static _Thread_local bool atomic_context;
static bool usbwarp_irq_ready(struct usbwarp_hcd *w) { return w->ready && !w->irq_failed; }
static void usbwarp_irq_disarm(struct usbwarp_hcd *w) { w->irq_token&=~1ULL; w->irq_pending=0; }
static bool usbwarp_irq_arm(struct usbwarp_hcd *w,bool test,u64 *seq)
{
    (void)test; *seq=w->cb->irq_event_seq;
    w->irq_token=(w->irq_token+2)|1;
    return w->h2g.hdr->producer_index!=w->h2g.hdr->consumer_index;
}
static int usbwarp_ring_consume(struct fake_ring *r,void *out,u32 size,u32 *length)
{
    struct message *m;
    if(r->corrupt) return -EPROTO;
    if(r->hdr->consumer_index==r->hdr->producer_index) return -EAGAIN;
    m=&r->messages[r->hdr->consumer_index++];
    *length=m->hdr.message_length;
    assert(*length<=size);
    memcpy(out,m,*length);
    return 0;
}
static bool usbwarp_validate_h2g_message(struct usbwarp_hcd *w,const void *p,u32 n)
{ (void)w; return n>=sizeof(struct usbwarp_msg_header) && ((const struct usbwarp_msg_header *)p)->magic==USBWARP_MSG_MAGIC; }
static void record(struct usbwarp_hcd *w,const struct usbwarp_msg_header *hdr)
{ assert(w->count<128); w->order[w->count++]=hdr->transaction_id; }
static void usbwarp_complete_urb(struct usbwarp_hcd *w,const void *p)
{ assert(atomic_context || bh_depth || !w->rx_fast); record(w,p); }
static void usbwarp_complete_iso_urb(struct usbwarp_hcd *w,const void *p,u32 n)
{ (void)n; usbwarp_complete_urb(w,p); }
static void usbwarp_dispatch_message(struct usbwarp_hcd *w,const void *p,u32 n)
{
    const struct usbwarp_msg_header *h=p;
    (void)n;
    assert(!atomic_context);
    if(h->message_type==USBWARP_MSG_DEVICE_ADDED && READ_ONCE(w->block_control)) {
        WRITE_ONCE(w->control_entered,1);
        while(!READ_ONCE(w->release_control)) sched_yield();
    }
    record(w,h);
}
static void usbwarp_shutdown_all_devices(struct usbwarp_hcd *w,unsigned reason)
{ (void)reason; w->shutting_down=true; }
static void usbwarp_retire_hcd(struct usbwarp_hcd *w) { ++w->retired; }
#include "../usbwarp_module/usbwarp_rx.c"

static struct usbwarp_hcd *create(void)
{
    struct usbwarp_hcd *w=calloc(1,sizeof(*w)); assert(w);
    w->cb=calloc(1,sizeof(*w->cb)); w->h2g.hdr=calloc(1,sizeof(*w->h2g.hdr));
    w->poll_buffer=calloc(1,USBWARP_MAX_MESSAGE_SIZE); w->rx_buffer=calloc(1,USBWARP_MAX_MESSAGE_SIZE);
    assert(w->cb && w->h2g.hdr && w->poll_buffer && w->rx_buffer);
    w->irq_requested=w->ready=true; w->irq_vector=29;
    usbwarp_rx_init(w); irq_softirq=true; clock_step=0; clock_ns=20000000;
    rx_repoll_us=0; rx_repoll_count=1;
    rx_busy_us=0; rx_busy_bytes=128; test_need_resched=test_stop=false; relax_hook=NULL;
    return w;
}
static void destroy(struct usbwarp_hcd *w)
{
    usbwarp_rx_stop(w); pthread_mutex_destroy(&w->rx_lock);
    free(w->cb); free(w->h2g.hdr); free(w->poll_buffer); free(w->rx_buffer); free(w);
}
static void push(struct usbwarp_hcd *w,u32 type,u32 id)
{
    struct message *m=&w->h2g.messages[w->h2g.hdr->producer_index++];
    m->hdr.magic=USBWARP_MSG_MAGIC; m->hdr.message_type=type; m->hdr.transaction_id=id;
    m->hdr.message_length=type==USBWARP_MSG_URB_COMPLETE ? USBWARP_MSG_URB_COMPLETE_BASE_SIZE : sizeof(m->hdr);
    ++w->cb->irq_event_seq;
}
static void pump_once(struct usbwarp_hcd *w)
{
    if(__atomic_exchange_n(&w->rx_tasklet.scheduled,0,__ATOMIC_ACQ_REL)) {
        atomic_context=true; w->rx_tasklet.callback(&w->rx_tasklet); atomic_context=false;
    }
}
static void pump(struct usbwarp_hcd *w)
{ unsigned n=0; while(READ_ONCE(w->rx_tasklet.scheduled)) { assert(++n<128); pump_once(w); } }
static void *worker(void *p) { assert(usbwarp_rx_worker(p)); return NULL; }
static void fire_timer(struct usbwarp_hcd *w,u64 late)
{
    struct hrtimer *t=&w->rx_repoll_timer;
    assert(t->active); clock_ns=t->due+late; t->active=false;
    assert(t->function(t)==HRTIMER_NORESTART); pump(w);
}

static struct usbwarp_hcd *busy_target;
static void arrive_during_busy(void)
{ relax_hook=NULL; push(busy_target,USBWARP_MSG_URB_COMPLETE,1); }
static void irq_during_busy(void)
{
    assert(busy_target->irq_token&1); /* IRQ fallback stays enabled */
    arrive_during_busy(); usbwarp_rx_interrupt(busy_target); pump(busy_target);
}
static void disable_busy(void) { relax_hook=NULL; rx_busy_us=0; }
static void stop_during_busy(void) { relax_hook=NULL; usbwarp_rx_stop(busy_target); }

int main(void)
{
    struct usbwarp_hcd *w=create();
    for(unsigned i=1;i<=96;++i) push(w,USBWARP_MSG_URB_COMPLETE,i);
    assert(!usbwarp_rx_worker(w)); pump(w);
    assert(w->count==96 && w->rx_reschedules==2 && (w->irq_token&1));
    for(unsigned i=0;i<96;++i) assert(w->order[i]==i+1);
    destroy(w);

    w=create();
    push(w,USBWARP_MSG_URB_COMPLETE,1); push(w,USBWARP_MSG_DEVICE_ADDED,2); push(w,USBWARP_MSG_URB_COMPLETE,3);
    assert(!usbwarp_rx_worker(w)); pump(w);
    assert(w->count==1 && w->rx_control_pending && w->h2g.hdr->consumer_index==2);
    WRITE_ONCE(w->block_control,1);
    pthread_t thread; assert(!pthread_create(&thread,NULL,worker,w));
    while(!READ_ONCE(w->control_entered)) sched_yield();
    for(unsigned i=0;i<1000;++i) { assert(usbwarp_rx_interrupt(w)); pump(w); }
    assert(w->count==1 && w->h2g.hdr->consumer_index==2);
    assert(((struct usbwarp_msg_header *)w->poll_buffer)->transaction_id==2);
    WRITE_ONCE(w->release_control,1); assert(!pthread_join(thread,NULL)); pump(w);
    assert(w->count==3 && w->order[0]==1 && w->order[1]==2 && w->order[2]==3);
    destroy(w);

    w=create(); push(w,USBWARP_MSG_DEVICE_ADDED,1); push(w,USBWARP_MSG_URB_COMPLETE,2);
    usbwarp_rx_worker(w); pump(w); WRITE_ONCE(w->block_control,1);
    assert(!pthread_create(&thread,NULL,worker,w));
    while(!READ_ONCE(w->control_entered)) sched_yield();
    usbwarp_rx_stop(w); assert(!usbwarp_rx_interrupt(w));
    WRITE_ONCE(w->release_control,1); assert(!pthread_join(thread,NULL)); pump(w);
    assert(w->count==1 && w->order[0]==1 && !READ_ONCE(w->rx_tasklet.scheduled));
    destroy(w);

    w=create();
    for(unsigned i=1;i<=80;++i) {
        push(w,USBWARP_MSG_URB_COMPLETE,i);
        WRITE_ONCE(irq_softirq,(i&1)!=0);
        usbwarp_rx_worker(w); pump(w);
        assert(w->count==i && w->order[i-1]==i);
    }
    destroy(w);

    w=create(); clock_step=60000;
    for(unsigned i=1;i<=5;++i) push(w,USBWARP_MSG_URB_COMPLETE,i);
    usbwarp_rx_worker(w); pump_once(w);
    assert(w->count==2 && w->rx_reschedules==1); pump(w); assert(w->count==5);
    destroy(w);

    w=create(); usbwarp_rx_worker(w); pump(w);
    push(w,USBWARP_MSG_URB_COMPLETE,1);
    usbwarp_rx_watchdog(w); pump(w);
    assert(w->cb->irq_watchdog==1 && w->count==1);
    usbwarp_rx_stop(w); push(w,USBWARP_MSG_URB_COMPLETE,2);
    assert(!usbwarp_rx_interrupt(w)); assert(!usbwarp_rx_worker(w)); pump(w);
    assert(w->count==1); destroy(w);

    w=create(); w->h2g.corrupt=true;
    usbwarp_rx_worker(w); pump(w); usbwarp_rx_worker(w);
    assert(w->retired==1 && w->shutting_down && w->cb->guest_state==USBWARP_STATE_ERROR);
    destroy(w);

    w=create();
    struct kernel_param kp={&rx_repoll_us};
    assert(!set_repoll_us("20",&kp) && rx_repoll_us==20);
    assert(set_repoll_us("251",&kp)==-EINVAL && rx_repoll_us==20);
    assert(set_repoll_us("-1",&kp)==-EINVAL);
    kp.arg=&rx_repoll_count;
    assert(set_repoll_count("17",&kp)==-EINVAL);
    push(w,USBWARP_MSG_URB_COMPLETE,1); usbwarp_rx_worker(w); pump(w);
    assert(w->count==1 && w->rx_repoll_timer.active && !(w->irq_token&1));
    fire_timer(w,0);
    assert((w->irq_token&1) && !w->rx_repoll_timer.active && w->rx_repoll_empty==1);
    destroy(w);

    w=create(); rx_repoll_us=25; rx_repoll_count=2;
    push(w,USBWARP_MSG_URB_COMPLETE,1); usbwarp_rx_worker(w); pump(w);
    push(w,USBWARP_MSG_URB_COMPLETE,2); fire_timer(w,0);
    assert(w->count==2 && w->rx_repoll_hits==1 && w->rx_repoll_timer.active);
    fire_timer(w,0); fire_timer(w,0);
    assert(!w->rx_repoll_timer.active && (w->irq_token&1));
    destroy(w);

    w=create(); rx_repoll_us=50; rx_repoll_count=16;
    push(w,USBWARP_MSG_URB_COMPLETE,1); usbwarp_rx_worker(w); pump(w);
    push(w,USBWARP_MSG_URB_COMPLETE,2); fire_timer(w,5000000);
    assert(w->count==2 && !w->rx_repoll_timer.active && (w->irq_token&1));
    assert(w->rx_repoll_late_max>=5000000);
    destroy(w);

    w=create(); rx_repoll_us=20; rx_repoll_count=4; w->iso_pending=1;
    push(w,USBWARP_MSG_URB_COMPLETE,1); usbwarp_rx_worker(w); pump(w);
    assert(!w->rx_repoll_timer.active && (w->irq_token&1));
    w->iso_pending=0; push(w,USBWARP_MSG_URB_COMPLETE,2);
    usbwarp_rx_interrupt(w); pump(w); assert(w->rx_repoll_timer.active);
    w->iso_pending=1; assert(usbwarp_rx_priority_pending(w));
    usbwarp_rx_worker(w); pump(w);
    assert(!w->rx_repoll_timer.active && (w->irq_token&1));
    w->iso_pending=0; push(w,USBWARP_MSG_URB_COMPLETE,3);
    usbwarp_rx_interrupt(w); pump(w); assert(w->rx_repoll_timer.active);
    WRITE_ONCE(irq_softirq,false); usbwarp_rx_worker(w); pump(w);
    assert(!w->rx_repoll_timer.active && !w->rx_fast);
    destroy(w);

    w=create(); rx_repoll_us=20; rx_repoll_count=4;
    push(w,USBWARP_MSG_URB_COMPLETE,1); usbwarp_rx_worker(w); pump(w);
    assert(w->rx_repoll_timer.active); usbwarp_rx_stop(w);
    assert(!w->rx_repoll_timer.active && !usbwarp_rx_interrupt(w));
    destroy(w);

    w=create(); rx_repoll_us=25; rx_repoll_count=8;
    push(w,USBWARP_MSG_URB_COMPLETE,1); usbwarp_rx_worker(w); pump(w);
    push(w,USBWARP_MSG_DEVICE_ADDED,2); fire_timer(w,0);
    assert(w->rx_control_pending && !w->rx_repoll_timer.active && !w->rx_repoll_left);
    assert(usbwarp_rx_worker(w)); pump(w);
    assert(w->count==2 && !w->rx_repoll_timer.active && (w->irq_token&1));
    destroy(w);
    puts("Production RX: budgets, sleeping-control barrier, mode switches, watchdog, stop and corruption passed");
    puts("Hybrid RX: bounds, finite empty repolls, hits, overdue timers, ISO bypass and timer cancellation passed");

    w=create(); usbwarp_rx_worker(w); pump(w);
    assert(!usbwarp_rx_submit(w,USBWARP_XFER_BULK,31)); /* default disabled */
    kp.arg=&rx_busy_us;
    assert(!set_busy_us("200",&kp));
    assert(set_busy_us("1001",&kp)==-EINVAL && rx_busy_us==200);
    assert(set_busy_us("-1",&kp)==-EINVAL);
    kp.arg=&rx_busy_bytes;
    assert(set_busy_bytes("4294967295",&kp)==-EINVAL && rx_busy_bytes==128);
    assert(!usbwarp_rx_submit(w,USBWARP_XFER_INTERRUPT,8));
    assert(!usbwarp_rx_submit(w,1,8)); /* ISO uses a separate wire message */
    assert(!usbwarp_rx_submit(w,USBWARP_XFER_BULK,4096));
    assert(!usbwarp_rx_busy_pending(w));
    assert(usbwarp_rx_submit(w,USBWARP_XFER_CONTROL,8));
    assert(usbwarp_rx_submit(w,USBWARP_XFER_BULK,13));
    u64 published=clock_ns,kicks=w->rx_busy_kicks;
    clock_ns+=100000;
    usbwarp_rx_progress(w,true,published,0);
    assert(w->rx_busy_fast==1 && w->rx_busy_kicks==kicks+1);
    kicks=w->rx_busy_kicks;
    usbwarp_rx_progress(w,true,published,-ECONNRESET);
    assert(w->rx_busy_kicks==kicks);
    clock_ns+=6000000000ULL;
    usbwarp_rx_progress(w,true,published,0);
    assert(w->rx_busy_slow==1 && w->rx_busy_kicks==kicks && !usbwarp_rx_busy_pending(w));
    w->iso_pending=1;
    assert(!usbwarp_rx_submit(w,USBWARP_XFER_BULK,31));
    destroy(w);

    w=create(); usbwarp_rx_worker(w); pump(w); rx_busy_us=200;
    assert(usbwarp_rx_submit(w,USBWARP_XFER_BULK,31));
    clock_step=1000; busy_target=w; relax_hook=arrive_during_busy;
    assert(usbwarp_rx_busy_poll(w)); pump(w);
    assert(w->count==1 && w->rx_busy_completions==1 && !w->rx_busy_active && (w->irq_token&1));
    assert(w->rx_busy_ns<=210000 && !usbwarp_rx_busy_pending(w));
    destroy(w);

    w=create(); usbwarp_rx_worker(w); pump(w); rx_busy_us=200;
    usbwarp_rx_submit(w,USBWARP_XFER_BULK,31);
    clock_step=1000; busy_target=w; relax_hook=irq_during_busy;
    assert(usbwarp_rx_busy_poll(w)); pump(w);
    assert(w->count==1 && w->rx_busy_completions==0 && (w->irq_token&1));
    destroy(w);

    w=create(); usbwarp_rx_worker(w); pump(w); rx_repoll_us=25; rx_repoll_count=8;
    push(w,USBWARP_MSG_URB_COMPLETE,1); usbwarp_rx_interrupt(w); pump(w);
    assert(w->rx_repoll_timer.active && !(w->irq_token&1));
    rx_busy_us=200; usbwarp_rx_submit(w,USBWARP_XFER_BULK,31); clock_step=1000;
    assert(usbwarp_rx_busy_poll(w));
    assert(!w->rx_repoll_timer.active && !w->rx_repoll_left && (w->irq_token&1));
    destroy(w);

    w=create(); usbwarp_rx_worker(w); pump(w); rx_repoll_us=25; rx_repoll_count=8;
    push(w,USBWARP_MSG_URB_COMPLETE,1); usbwarp_rx_interrupt(w); pump(w);
    assert(w->rx_repoll_timer.active && !(w->irq_token&1));
    rx_busy_us=200; usbwarp_rx_submit(w,USBWARP_XFER_BULK,31); clock_step=1000;
    assert(usbwarp_rx_busy_poll(w));
    assert(!w->rx_repoll_timer.active && !w->rx_repoll_left && (w->irq_token&1));
    destroy(w);

    w=create(); usbwarp_rx_worker(w); pump(w); rx_busy_us=200;
    usbwarp_rx_submit(w,USBWARP_XFER_BULK,31); test_need_resched=true;
    assert(usbwarp_rx_busy_poll(w) && w->rx_busy_yields==1 && !w->rx_busy_polls);
    assert(w->irq_token&1); destroy(w);

    w=create(); usbwarp_rx_worker(w); pump(w); rx_busy_us=200;
    usbwarp_rx_submit(w,USBWARP_XFER_BULK,31); clock_step=1000; relax_hook=disable_busy;
    assert(usbwarp_rx_busy_poll(w) && !w->rx_busy_active && (w->irq_token&1));
    destroy(w);

    w=create(); usbwarp_rx_worker(w); pump(w); rx_busy_us=200;
    usbwarp_rx_submit(w,USBWARP_XFER_BULK,31); clock_step=1000;
    push(w,USBWARP_MSG_URB_COMPLETE,1); push(w,USBWARP_MSG_DEVICE_ADDED,2); push(w,USBWARP_MSG_URB_COMPLETE,3);
    assert(usbwarp_rx_busy_poll(w));
    assert(w->count==1 && w->rx_control_pending && w->h2g.hdr->consumer_index==2);
    assert(usbwarp_rx_worker(w)); pump(w);
    assert(w->count==3 && w->order[0]==1 && w->order[1]==2 && w->order[2]==3);
    destroy(w);

    w=create(); usbwarp_rx_worker(w); pump(w); rx_busy_us=200;
    usbwarp_rx_submit(w,USBWARP_XFER_BULK,31); clock_step=1000; busy_target=w; relax_hook=stop_during_busy;
    assert(usbwarp_rx_busy_poll(w) && w->rx_stopping && !w->rx_busy_active && !w->rx_fast);
    assert(!usbwarp_rx_busy_pending(w)); destroy(w);
    puts("Bounded busy RX: classification, long waits, cancellation, budgets, IRQ fallback, ownership, control and stop passed");
    return 0;
}
