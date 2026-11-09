/* SPDX-License-Identifier: GPL-2.0-only
 * Compile the actual guest ring implementation with RAM/lock stand-ins.
 * gcc -std=c11 -O2 -Wall -Wextra -Werror -pthread -Itests/stubs -Iinclude
 *     tests/ring_guest_test.c -o ring_guest_test
 */
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usbwarp_iso.h"

#define USBWARP_GUEST_H
#define __iomem
#define __force
typedef uint8_t u8;
typedef pthread_mutex_t spinlock_t;
struct usbwarp_ring {
    struct usbwarp_ring_header *hdr;
    void *data;
    void *doorbell;
    uint32_t data_size, data_mask;
    spinlock_t produce_lock;
};
#define spin_lock_init(p) assert(pthread_mutex_init(p, NULL) == 0)
#define spin_lock_irqsave(p, f) do { (f) = 0; assert(pthread_mutex_lock(p) == 0); } while (0)
#define spin_unlock_irqrestore(p, f) do { (void)(f); assert(pthread_mutex_unlock(p) == 0); } while (0)
#define ioread32(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define iowrite32(v, p) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
static unsigned doorbell_count;
#define writeq(v, p) do { __atomic_store_n((uint64_t *)(p), (v), __ATOMIC_RELEASE); __atomic_add_fetch(&doorbell_count,1,__ATOMIC_RELAXED); } while(0)
#define rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define memcpy_toio memcpy
#define memcpy_fromio memcpy
#define memset_io memset
#include "../usbwarp_module/usbwarp_ring.c"

#define PRODUCERS 4
#define MESSAGES 10000
static struct usbwarp_ring ring;

static void *produce(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    unsigned char *data = malloc(USBWARP_MAX_MESSAGE_SIZE);
    assert(data);
    for (uint32_t n = 1; n <= MESSAGES; ++n) {
        uint32_t length = (n % 31 == 0) ? USBWARP_MAX_MESSAGE_SIZE :
                          64 + ((n * 37 + id) % 1024);
        struct usbwarp_msg_header *h = (void *)data;
        memset(data, (unsigned char)n, length);
        h->magic = USBWARP_MSG_MAGIC;
        h->message_length = length;
        h->device_id = id;
        h->transaction_id = n;
        int ret;
        while ((ret = usbwarp_ring_produce(&ring, data, length)) == -ENOSPC)
            sched_yield();
        assert(ret == 0);
    }
    free(data);
    return NULL;
}

int main(void)
{
    struct usbwarp_ring_header header = { 0 };
    uint32_t seen[PRODUCERS] = { 0 }, len;
    unsigned char *storage = calloc(1, 65536), *out = malloc(USBWARP_MAX_MESSAGE_SIZE);
    pthread_t threads[PRODUCERS];
    uint64_t bell=0;
    assert(storage && out);
    header.data_size = 65536;
    header.data_size_mask = 65535;
    header.max_message_size = USBWARP_MAX_MESSAGE_SIZE;
    /* Force uint32 index rollover and ring-end splits immediately. */
    header.producer_index = header.consumer_index = UINT32_MAX - 63;
    usbwarp_ring_init(&ring, &header, storage, 65536);
    header.data_size = header.data_size_mask = header.max_message_size = UINT32_MAX;
    assert(ring.data_size==65536 && ring.data_mask==65535);
    ring.doorbell=&bell;
    assert(usbwarp_ring_produce(&ring, out, USBWARP_MAX_MESSAGE_SIZE + 1) == -EMSGSIZE);
    assert(usbwarp_ring_produce(&ring, out, 0) == -EMSGSIZE);
    for (uintptr_t i = 0; i < PRODUCERS; ++i)
        assert(pthread_create(&threads[i], NULL, produce, (void *)i) == 0);
    for (unsigned n = 0; n < PRODUCERS * MESSAGES;) {
        int ret = usbwarp_ring_consume(&ring, out, USBWARP_MAX_MESSAGE_SIZE, &len);
        if (ret == -EAGAIN) { sched_yield(); continue; }
        assert(ret == 0);
        struct usbwarp_msg_header *h = (void *)out;
        assert(h->device_id < PRODUCERS && h->message_length == len);
        assert(h->transaction_id == ++seen[h->device_id]);
        for (unsigned j = sizeof(*h); j < len; ++j)
            assert(out[j] == (unsigned char)h->transaction_id);
        ++n;
    }
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(seen[i] == MESSAGES);
    }
    assert(usbwarp_ring_consume(&ring, out, USBWARP_MAX_MESSAGE_SIZE, &len) == -EAGAIN);
    assert(doorbell_count==PRODUCERS*MESSAGES && bell==1);
    /* Receiver capacity errors consume the whole message, preserving framing. */
    memset(out, 0, USBWARP_MAX_MESSAGE_SIZE);
    ((struct usbwarp_msg_header *)out)->magic = USBWARP_MSG_MAGIC;
    ((struct usbwarp_msg_header *)out)->message_length = USBWARP_MAX_MESSAGE_SIZE;
    assert(usbwarp_ring_produce(&ring, out, USBWARP_MAX_MESSAGE_SIZE) == 0);
    ring.hdr->producer_flags = USBWARP_RING_FLAG_NEED_SPACE;
    unsigned before_credit = doorbell_count;
    assert(usbwarp_ring_consume(&ring, out, 4096, &len) == -EMSGSIZE);
    assert(doorbell_count == before_credit + 1); /* oversized-message discard releases credits too */
    assert(usbwarp_ring_produce(&ring, out, USBWARP_MAX_MESSAGE_SIZE) == 0);
    before_credit = doorbell_count;
    assert(usbwarp_ring_consume(&ring, out, USBWARP_MAX_MESSAGE_SIZE, &len) == 0);
    assert(doorbell_count == before_credit + 1);
    ring.hdr->producer_flags = 0;
    assert(usbwarp_ring_consume(&ring, out, USBWARP_MAX_MESSAGE_SIZE, &len) == -EAGAIN);
    memset(out, 0x5a, 8);
    assert(usbwarp_ring_produce(&ring, out, 8) == -EMSGSIZE);
    assert(usbwarp_ring_inject_raw(&ring, out, 8) == 0);
    assert(usbwarp_ring_consume(&ring, out, USBWARP_MAX_MESSAGE_SIZE, &len) == -EPROTO);
    header.consumer_index=0; header.producer_index=64;
    memset(storage,0,64);
    ((struct usbwarp_msg_header *)storage)->magic=USBWARP_MSG_MAGIC;
    ((struct usbwarp_msg_header *)storage)->message_length=UINT32_MAX-62;
    len=999;
    assert(usbwarp_ring_consume(&ring,out,USBWARP_MAX_MESSAGE_SIZE,&len)==-EPROTO && len==0);
    header.producer_index=65536+64;
    assert(usbwarp_ring_consume(&ring,out,USBWARP_MAX_MESSAGE_SIZE,&len)==-EPROTO);
    header.producer_index=65;
    assert(usbwarp_ring_consume(&ring,out,USBWARP_MAX_MESSAGE_SIZE,&len)==-EPROTO);
    assert(usbwarp_ring_produce(&ring,out,32)==-EPROTO);
    pthread_mutex_destroy(&ring.produce_lock);
    free(out); free(storage);
    puts("Guest production ring: 40000 concurrent messages, ISO maximum, rollover and backpressure passed");
    return 0;
}
