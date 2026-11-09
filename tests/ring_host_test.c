/* SPDX-License-Identifier: GPL-2.0-only
 * Production Windows ring code with bounded RAM and lock stand-ins. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usbwarp_layout.h"
#define USBWARP_DRV_H
#define _In_
#define _Out_
#define _Inout_
typedef void VOID;
typedef void *PVOID;
typedef unsigned char UCHAR, *PUCHAR;
typedef uint32_t ULONG, *PULONG;
typedef uint64_t ULONG64;
typedef int32_t NTSTATUS;
typedef int KIRQL;
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER (-1)
#define STATUS_DATA_ERROR (-2)
#define STATUS_INSUFFICIENT_RESOURCES (-3)
#define STATUS_NO_MORE_ENTRIES (-4)
#define STATUS_BUFFER_OVERFLOW (-5)
typedef struct {
    volatile struct usbwarp_ring_header *Hdr;
    volatile UCHAR *Data;
    ULONG DataSize, DataMask;
    pthread_mutex_t ProduceLock;
    void *NotifyContext;
} USBWARP_HOST_RING, *PUSBWARP_HOST_RING;
#define KeInitializeSpinLock(p) assert(!pthread_mutex_init((p),NULL))
#define KeAcquireSpinLock(p,k) do { *(k)=0; assert(!pthread_mutex_lock(p)); } while(0)
#define KeReleaseSpinLock(p,k) do { (void)(k); assert(!pthread_mutex_unlock(p)); } while(0)
#define KeMemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define ReadULongAcquire(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define WriteULongRelease(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define RtlZeroMemory memset_zero
static void memset_zero(void *p,size_t n) { memset(p,0,n); }
static unsigned char *poison;
static void copy_memory(void *dest,const void *src,size_t n)
{
    memcpy(dest,src,n);
    if(poison && src==poison && n==sizeof(struct usbwarp_msg_header)) {
        ((struct usbwarp_msg_header *)poison)->message_length+=64;
        poison=NULL;
    }
}
#define RtlCopyMemory copy_memory
static size_t RtlCompareMemory(const void *a,const void *b,size_t n) { return memcmp(a,b,n)?0:n; }
static void UsbWarpIrqProduced(void *context) { (void)context; }
#include "../USBWarp/usbwarp_ring_host.c"

static struct usbwarp_shm_layout good_layout(void)
{
    struct usbwarp_shm_layout l={0};
    l.total_size=32u*1024*1024;
    l.g2h_offset=4096; l.g2h_size=262144+192;
    l.h2g_offset=l.g2h_offset+l.g2h_size; l.h2g_size=l.g2h_size;
    l.data_offset=USBWARP_ALIGN_PAGE(l.h2g_offset+l.h2g_size);
    l.data_size=l.total_size-l.data_offset;
    l.buffer_size=65536; l.buffer_count=l.data_size/l.buffer_size;
    return l;
}

int main(void)
{
    struct usbwarp_ring_header h={0};
    USBWARP_HOST_RING r;
    unsigned char *data=calloc(1,65536),*in=calloc(1,USBWARP_MAX_MESSAGE_SIZE),*out=malloc(USBWARP_MAX_MESSAGE_SIZE);
    assert(data && in && out);
    h.data_size=h.data_size_mask=h.max_message_size=UINT32_MAX;
    UsbWarpRingInit(&r,&h,data,65536);
    assert(r.DataSize==65536 && r.DataMask==65535);
    const unsigned sizes[]={32,64,192,USBWARP_MAX_MESSAGE_SIZE};
    for(unsigned k=0;k<4;k++) {
        h.producer_index=h.consumer_index=UINT32_MAX-63;
        memset(in,0x3a,sizes[k]);
        struct usbwarp_msg_header *message=(void *)in;
        message->magic=USBWARP_MSG_MAGIC; message->message_length=sizes[k];
        assert(UsbWarpRingProduce(&r,in,sizes[k])==0);
        ULONG length=0;
        assert(UsbWarpRingConsume(&r,out,USBWARP_MAX_MESSAGE_SIZE,&length)==0);
        assert(length==sizes[k] && !memcmp(in,out,length));
    }
    assert(UsbWarpRingProduce(&r,in,UINT32_MAX)==STATUS_INVALID_PARAMETER);
    assert(UsbWarpRingProduce(&r,in,0)==STATUS_INVALID_PARAMETER);
    assert(UsbWarpRingProduce(&r,NULL,32)==STATUS_INVALID_PARAMETER);
    const unsigned bad_lengths[]={0,31,USBWARP_MAX_MESSAGE_SIZE+1,UINT32_MAX-62,UINT32_MAX};
    for(unsigned k=0;k<sizeof(bad_lengths)/sizeof(bad_lengths[0]);k++) {
        h.consumer_index=0; h.producer_index=64;
        struct usbwarp_msg_header *message=(void *)data;
        memset(data,0,64); message->magic=USBWARP_MSG_MAGIC; message->message_length=bad_lengths[k];
        ULONG length=999;
        assert(UsbWarpRingConsume(&r,out,USBWARP_MAX_MESSAGE_SIZE,&length)==STATUS_DATA_ERROR);
        assert(!length && !h.consumer_index);
    }
    for(unsigned k=0;k<3;k++) {
        h.consumer_index=k==2?1:0;
        h.producer_index=k==0?65:k==1?65536+64:64;
        ULONG length=999, original_pi=h.producer_index;
        assert(UsbWarpRingConsume(&r,out,USBWARP_MAX_MESSAGE_SIZE,&length)==STATUS_DATA_ERROR);
        assert(!length && UsbWarpRingProduce(&r,in,32)==STATUS_DATA_ERROR);
        assert(h.producer_index==original_pi);
    }
    h.consumer_index=h.producer_index=0;
    ((struct usbwarp_msg_header *)in)->message_length=64;
    assert(UsbWarpRingProduce(&r,in,64)==0);
    poison=data;
    ULONG length=999;
    assert(UsbWarpRingConsume(&r,out,USBWARP_MAX_MESSAGE_SIZE,&length)==STATUS_DATA_ERROR);
    assert(!length && h.consumer_index==0);
    h.consumer_index=h.producer_index=0;
    assert(UsbWarpRingProduce(&r,in,64)==0);
    assert(UsbWarpRingConsume(&r,out,32,&length)==STATUS_BUFFER_OVERFLOW);
    assert(h.consumer_index==64 && !length);
    struct usbwarp_shm_layout good=good_layout();
    assert(usbwarp_layout_valid(&good,good.total_size));
    assert(!usbwarp_layout_valid(&good,4096));
    assert(!usbwarp_layout_valid(&good,1ULL<<32));
    for(unsigned field=0;field<9;field++) {
        struct usbwarp_shm_layout bad=good;
        uint32_t value=UINT32_MAX;
        memcpy((unsigned char *)&bad+field*sizeof(value),&value,sizeof(value));
        assert(!usbwarp_layout_valid(&bad,good.total_size));
    }
    struct usbwarp_shm_layout bad=good; bad.buffer_size=0;
    assert(!usbwarp_layout_valid(&bad,good.total_size));
    bad=good; bad.h2g_offset=bad.g2h_offset;
    assert(!usbwarp_layout_valid(&bad,good.total_size));
    pthread_mutex_destroy(&r.ProduceLock); free(out); free(in); free(data);
    puts("Production host ring: immutable geometry, overflow, indices, copy races and layout bounds passed");
    return 0;
}
