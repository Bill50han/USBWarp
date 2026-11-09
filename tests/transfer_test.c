/* SPDX-License-Identifier: GPL-2.0-only
 * Production scatterlist-copy and ZLP scheduling code, with USB-core lists,
 * scatter mappings and the transport replaced by deterministic test doubles.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "usbwarp_shared.h"

#define USBWARP_GUEST_H
#define __iomem
typedef uint32_t u32;
typedef struct { uint32_t value; } atomic_t;
static int atomic_inc_return(atomic_t *v) { return (int)++v->value; }
static void atomic_inc(atomic_t *v) { ++v->value; }
static void atomic_dec(atomic_t *v) { assert(v->value); --v->value; }
#define spin_lock_irqsave(p,f) do { (f)=0; assert(*(p)==0); *(p)=1; } while(0)
#define spin_unlock_irqrestore(p,f) do { (void)(f); assert(*(p)==1); *(p)=0; } while(0)
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define memcpy_fromio memcpy
#define memcpy_toio memcpy
#define SG_MITER_ATOMIC 1
#define SG_MITER_TO_SG 2
#define SG_MITER_FROM_SG 4
struct scatterlist { void *data; size_t length; };
struct sg_mapping_iter {
    struct scatterlist *sg; unsigned count; size_t pos, length, consumed;
    void *addr; bool mapped;
};
static void sg_miter_start(struct sg_mapping_iter *m, struct scatterlist *sg, unsigned count, unsigned flags)
{ (void)flags; memset(m,0,sizeof(*m)); m->sg=sg; m->count=count; }
static void sg_miter_stop(struct sg_mapping_iter *m)
{ if(m->mapped) { m->pos+=m->consumed; m->mapped=false; } }
static bool sg_miter_skip(struct sg_mapping_iter *m, size_t offset)
{
    size_t total=0; for(unsigned i=0;i<m->count;++i) total+=m->sg[i].length;
    m->pos=offset; return offset<total;
}
static bool sg_miter_next(struct sg_mapping_iter *m)
{
    size_t base=0;
    sg_miter_stop(m);
    for(unsigned i=0;i<m->count;++i) {
        if(m->pos<base+m->sg[i].length) {
            size_t skip=m->pos-base;
            m->addr=(char *)m->sg[i].data+skip;
            m->length=m->sg[i].length-skip;
            m->consumed=m->length; m->mapped=true; return true;
        }
        base+=m->sg[i].length;
    }
    return false;
}
struct urb;
struct list_head { struct urb *items[4]; size_t count; };
struct usb_host_endpoint { struct list_head urb_list; };
struct urb {
    void *transfer_buffer; int num_sgs; struct scatterlist *sg;
    u32 transfer_buffer_length, stream_id, pipe;
    struct usb_host_endpoint *ep; void *hcpriv; struct list_head urb_list;
};
#define usb_pipeendpoint(p) ((p)&15)
#define list_for_each_entry(p,h,member) \
    for(size_t li=0;li<(h)->count && (((p)=(h)->items[li]),1);++li)
enum { URB_STATE_ACTIVE, URB_STATE_CANCELING, URB_STATE_DEAD };
struct usbwarp_urb_priv;
struct hash_table { struct usbwarp_urb_priv *items[8]; size_t count; };
struct hash_node { struct hash_table *table; struct usbwarp_urb_priv *owner; };
struct usbwarp_urb_priv {
    struct urb *urb; u32 transaction_id, device_id, epoch; bool need_zlp, zlp_stage, queued;
    int state; u32 saved_actual; unsigned long cancel_retry; struct hash_node hash_node;
    uint64_t published_ns; bool busy_eligible;
    struct usbwarp_msg_urb_submit wire_submit;
};
static void hash_add_impl(struct hash_table *t, struct hash_node *n)
{ assert(t->count<8); n->table=t; t->items[t->count++]=n->owner; }
static void hash_del(struct hash_node *n)
{
    struct hash_table *t=n->table;
    for(size_t i=0;i<t->count;++i) if(t->items[i]==n->owner) {
        memmove(t->items+i,t->items+i+1,(--t->count-i)*sizeof(*t->items)); return;
    }
    assert(0);
}
#define hash_add(t,n,k) do { (void)(k); hash_add_impl(&(t),(n)); } while(0)
#define hash_for_each_possible(t,p,member,key) \
    for(size_t hi=0;hi<(t).count && (((p)=(t).items[hi]),1);++hi)
#define hash_for_each(t,bkt,p,member) \
    for((bkt)=0;(bkt)<1;++(bkt)) \
        for(size_t hi=0;hi<(t).count && (((p)=(t).items[hi]),1);++hi)
struct usbwarp_hcd;
struct fake_ring { struct usbwarp_hcd *w; bool full; unsigned count; struct usbwarp_msg_urb_submit sent[8]; };
struct usbwarp_hcd {
    int urb_lock; bool quiescing, shutting_down; atomic_t txn_counter, tx_pending;
    struct hash_table urb_table; struct fake_ring g2h;
};
static bool paused;
static bool usbwarp_debugfs_g2h_paused(void) { return paused; }
static unsigned long jiffies=100;
#define msecs_to_jiffies(x) (x)
#define time_after_eq(a,b) ((long)((a)-(b))>=0)
static uint64_t ktime_get_ns(void) { return (uint64_t)jiffies*1000000; }
static unsigned policy_submits;
static bool usbwarp_rx_submit(struct usbwarp_hcd *w,u32 type,u32 length)
{ (void)w; (void)type; (void)length; ++policy_submits; return true; }
static void usbwarp_rx_progress(struct usbwarp_hcd *w,bool eligible,uint64_t stamp,int status)
{ (void)w; (void)eligible; (void)stamp; (void)status; }
static int usbwarp_ring_produce(struct fake_ring *r, const void *msg, u32 size)
{
    assert(r->w->urb_lock==1);
    if(r->full) return -ENOSPC;
    assert(r->count<8 && size<=sizeof(r->sent[0]));
    memcpy(&r->sent[r->count++],msg,size); return 0;
}
#include "../usbwarp_module/usbwarp_copy.c"
#include "../usbwarp_module/usbwarp_queue.c"

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
struct fixture {
    struct usbwarp_hcd w; struct usb_host_endpoint ep;
    struct urb urb[2]; struct usbwarp_urb_priv p[2];
};
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f));
    f->w.g2h.w=&f->w; f->w.txn_counter.value=100; f->w.tx_pending.value=1;
    f->ep.urb_list.count=2;
    for(unsigned i=0;i<2;++i) {
        struct usbwarp_urb_priv *p=&f->p[i];
        p->urb=&f->urb[i]; p->hash_node.owner=p;
        p->urb->ep=&f->ep; p->urb->hcpriv=p; p->urb->pipe=2; p->urb->transfer_buffer_length=64;
        p->device_id=1; p->epoch=9; p->transaction_id=11+i; p->need_zlp=!i; p->queued=i;
        p->wire_submit.hdr.message_length=USBWARP_MSG_URB_SUBMIT_BASE_SIZE+64;
        p->wire_submit.hdr.transaction_id=p->transaction_id; p->wire_submit.hdr.flags=p->epoch;
        p->wire_submit.data_mode=USBWARP_DATA_INLINE;
        p->wire_submit.transfer_length=64; p->wire_submit.endpoint=2;
        f->ep.urb_list.items[i]=p->urb;
        hash_add(f->w.urb_table,&p->hash_node,p->transaction_id);
    }
    paused=false;
    policy_submits=0;
}
static struct usbwarp_msg_urb_complete complete_data(void)
{
    struct usbwarp_msg_urb_complete m={0};
    m.hdr.transaction_id=11; m.hdr.flags=9; m.hdr.device_id=m.device_id=1;
    m.endpoint=2; m.actual_length=64; m.data_mode=USBWARP_DATA_INLINE;
    return m;
}
static void publish(struct fixture *f, unsigned n)
{ f->w.urb_lock=1; usbwarp_publish_queued(&f->w,&f->p[n]); f->w.urb_lock=0; }
static void retire_first(struct fixture *f)
{
    hash_del(&f->p[0].hash_node); f->urb[0].hcpriv=NULL;
    f->ep.urb_list.items[0]=&f->urb[1]; f->ep.urb_list.count=1;
}

int main(void)
{
    unsigned char pages[3][8], io[24], linear[16];
    struct scatterlist sg[]={{pages[0],3},{pages[1],5},{pages[2],8}};
    struct urb u={.sg=sg,.num_sgs=3,.transfer_buffer_length=16};
    /* Every range across three noncontiguous SG spans, both RAM and MMIO. */
    for(unsigned off=0;off<=16;++off) for(unsigned len=0;len<=16-off;++len) {
        unsigned index=0;
        for(unsigned i=0;i<3;++i) for(unsigned j=0;j<sg[i].length;++j) pages[i][j]=(unsigned char)index++;
        for(unsigned mode=0;mode<2;++mode) {
            memset(io,0xa5,sizeof(io));
            CHECK(usbwarp_copy_urb_data(&u,off,mode?NULL:io+4,mode?io+4:NULL,len,false)==len);
            for(unsigned i=0;i<len;++i) CHECK(io[4+i]==off+i);
            CHECK(io[3]==0xa5 && io[4+len]==0xa5);
        }
        memset(pages,0xcc,sizeof(pages)); memset(io,0x77,sizeof(io));
        CHECK(usbwarp_copy_urb_data(&u,off,NULL,io,len,true)==len);
        index=0;
        for(unsigned i=0;i<3;++i) for(unsigned j=0;j<sg[i].length;++j,++index)
            CHECK(pages[i][j]==((index>=off&&index<off+len)?0x77:0xcc));
    }
    CHECK(usbwarp_copy_urb_data(&u,17,io,NULL,1,false)==0);
    CHECK(usbwarp_copy_urb_data(&u,15,io,NULL,2,false)==1);
    u.num_sgs=0; u.transfer_buffer=linear;
    for(unsigned i=0;i<16;++i) linear[i]=(unsigned char)i;
    CHECK(usbwarp_copy_urb_data(&u,3,NULL,io,9,false)==9);
    CHECK(!memcmp(io,linear+3,9));
    memset(io,0xaa,9);
    CHECK(usbwarp_copy_urb_data(&u,4,io,NULL,9,true)==9);
    CHECK(linear[3]==3 && linear[4]==0xaa && linear[12]==0xaa && linear[13]==13);

    struct fixture f; struct usbwarp_msg_urb_complete m=complete_data();
    init(&f);
    CHECK(usbwarp_submission_blocked(&f.p[1]));
    CHECK(usbwarp_stage_zlp(&f.w,&m));
    CHECK(f.w.g2h.count==1 && f.w.g2h.sent[0].transfer_length==0);
    CHECK(f.w.g2h.sent[0].data_mode==USBWARP_DATA_NONE);
    CHECK(f.p[0].saved_actual==64 && f.p[0].zlp_stage && !f.p[0].queued);
    CHECK(f.p[0].transaction_id!=11 && f.w.tx_pending.value==1);
    CHECK(policy_submits==1 && f.p[0].published_ns && f.p[0].busy_eligible);
    CHECK(!usbwarp_stage_zlp(&f.w,&m)); /* duplicate first-stage response */
    publish(&f,1); CHECK(f.w.g2h.count==1); /* later data must wait for ZLP completion */
    retire_first(&f); publish(&f,1);
    CHECK(f.w.g2h.count==2 && f.w.g2h.sent[1].transfer_length==64 && !f.w.tx_pending.value);

    init(&f); f.w.g2h.full=true;
    CHECK(usbwarp_stage_zlp(&f.w,&m));
    CHECK(f.p[0].queued && f.w.tx_pending.value==2 && f.w.g2h.count==0);
    CHECK(!policy_submits && !f.p[0].published_ns && !f.p[0].busy_eligible);
    publish(&f,1); CHECK(f.w.g2h.count==0);
    f.w.g2h.full=false; publish(&f,0);
    CHECK(!f.p[0].queued && f.w.g2h.count==1 && f.w.tx_pending.value==1);
    CHECK(policy_submits==1 && f.p[0].published_ns);
    retire_first(&f); publish(&f,1); CHECK(!f.w.tx_pending.value);

    init(&f); f.urb[1].stream_id=f.p[1].wire_submit.stream_id=7;
    CHECK(!usbwarp_submission_blocked(&f.p[1])); publish(&f,1);
    CHECK(f.w.g2h.count==1 && f.w.g2h.sent[0].stream_id==7); /* independent stream */
    init(&f); f.p[0].need_zlp=false; publish(&f,1);
    CHECK(f.w.g2h.count==1); /* ordinary transfers stay pipelined */
    init(&f); f.p[0].state=URB_STATE_CANCELING;
    CHECK(!usbwarp_stage_zlp(&f.w,&m) && f.w.g2h.count==0);
    init(&f); f.w.quiescing=true;
    CHECK(usbwarp_stage_zlp(&f.w,&m) && f.p[0].queued && f.w.g2h.count==0);
    init(&f); paused=true;
    CHECK(usbwarp_stage_zlp(&f.w,&m) && f.p[0].queued && f.w.g2h.count==0);
    for(unsigned i=0;i<5;++i) {
        struct usbwarp_msg_urb_complete bad=m; init(&f);
        if(i==0) bad.status=USBWARP_STATUS_STALL;
        if(i==1) bad.actual_length=63;
        if(i==2) bad.endpoint=3;
        if(i==3) bad.actual_length=65;
        if(i==4) bad.hdr.flags=8; /* completion from an older binding */
        CHECK(!usbwarp_stage_zlp(&f.w,&bad) && f.w.g2h.count==0);
    }
    CHECK(usbwarp_binding_matches(USBWARP_CAP_DEVICE_EPOCH,9,9));
    CHECK(!usbwarp_binding_matches(USBWARP_CAP_DEVICE_EPOCH,8,9));
    CHECK(!usbwarp_binding_matches(USBWARP_CAP_DEVICE_EPOCH,0,9));
    CHECK(!usbwarp_binding_matches(USBWARP_CAP_DEVICE_EPOCH,0,0));
    CHECK(usbwarp_binding_matches(0,0,9)); /* legacy capability negotiation */

    /* A posted receive and a locally queued request may wait indefinitely.
     * Housekeeping must not turn age alone into an unlink or free either. */
    init(&f); jiffies+=60000;
    usbwarp_retry_cancels(&f.w);
    CHECK(f.w.g2h.count==0 && f.w.urb_table.count==2);
    CHECK(f.p[0].state==URB_STATE_ACTIVE && f.p[1].state==URB_STATE_ACTIVE);
    CHECK(f.urb[0].hcpriv==&f.p[0] && f.urb[1].hcpriv==&f.p[1]);
    f.p[0].state=f.p[1].state=URB_STATE_CANCELING;
    usbwarp_retry_cancels(&f.w);
    CHECK(f.w.g2h.count==1 && f.w.g2h.sent[0].hdr.message_type==USBWARP_MSG_URB_CANCEL);
    CHECK(f.w.g2h.sent[0].hdr.transaction_id==11 && f.w.g2h.sent[0].hdr.flags==9);
    CHECK(f.w.urb_table.count==2 && f.urb[0].hcpriv==&f.p[0]);
    ++jiffies; usbwarp_retry_cancels(&f.w); CHECK(f.w.g2h.count==1);
    jiffies+=99; usbwarp_retry_cancels(&f.w); CHECK(f.w.g2h.count==2);
    f.w.g2h.full=true; jiffies+=100; usbwarp_retry_cancels(&f.w);
    CHECK(f.w.g2h.count==2 && f.w.urb_table.count==2);
    f.w.g2h.full=false; jiffies+=100; usbwarp_retry_cancels(&f.w);
    CHECK(f.w.g2h.count==3 && f.p[1].queued); /* queued request never sent to host */
    printf("Production scatterlist/ZLP paths: %u checks passed\n",checks);
    return 0;
}
