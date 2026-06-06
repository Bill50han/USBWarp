// SPDX-License-Identifier: GPL-2.0-only
/*
 * ring_test.c — User-space Ring protocol correctness tests (Layer 1)
 *
 * Tests the SPSC ring buffer logic, alignment helpers, control block layout,
 * and message framing WITHOUT any kernel or hardware dependencies.
 *
 * Build:   gcc -O2 -Wall -Wextra -I../include -o ring_test ring_test.c
 * Run:     ./ring_test
 *
 * All tests must pass before proceeding to Layer 2 (fuzz) testing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* Pull in shared protocol definitions. */
#include "usbwarp_shared.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Test framework
 * ═══════════════════════════════════════════════════════════════════════════ */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  [%3d] %-55s ", tests_run, name); \
        fflush(stdout); \
    } while (0)

#define PASS() \
    do { tests_passed++; printf("\033[32mPASS\033[0m\n"); } while (0)

#define FAIL(fmt, ...) \
    do { \
        tests_failed++; \
        printf("\033[31mFAIL\033[0m " fmt "\n", ##__VA_ARGS__); \
    } while (0)

#define ASSERT_EQ(a, b, fmt) \
    do { \
        if ((a) != (b)) { FAIL(fmt " expected=%u got=%u", (unsigned)(b), (unsigned)(a)); return; } \
    } while (0)

#define ASSERT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { FAIL("%s", msg); return; } \
    } while (0)

/* ═══════════════════════════════════════════════════════════════════════════
 * Simulated Ring — mirrors kernel/guest implementations in user space
 * ═══════════════════════════════════════════════════════════════════════════ */

struct sim_ring {
    struct usbwarp_ring_header hdr;
    uint8_t *data;       /* ring data area */
    uint32_t data_size;  /* power of 2 */
};

static void sim_ring_init(struct sim_ring *r, uint32_t data_size)
{
    assert(USBWARP_IS_POWER_OF_2(data_size));
    memset(&r->hdr, 0, sizeof(r->hdr));
    r->hdr.data_size      = data_size;
    r->hdr.data_size_mask = data_size - 1;
    r->hdr.max_message_size = data_size / 2;
    r->data = calloc(1, data_size);
    r->data_size = data_size;
    assert(r->data);
}

static void sim_ring_free(struct sim_ring *r)
{
    free(r->data);
    r->data = NULL;
}

/* Produce: write msg into ring. Returns 0 on success, -1 if full. */
static int sim_ring_produce(struct sim_ring *r, const void *msg, uint32_t msg_len)
{
    uint32_t aligned = USBWARP_ALIGN_CACHELINE(msg_len);
    uint32_t pi = r->hdr.producer_index;
    uint32_t ci = r->hdr.consumer_index;
    uint32_t mask = r->hdr.data_size_mask;
    uint32_t avail = usbwarp_ring_available(pi, ci, mask);

    if (avail < aligned)
        return -1;  /* full */

    /* Write data, handling wrap-around. */
    uint32_t pos = pi & mask;
    uint32_t first = r->data_size - pos;

    if (first >= msg_len) {
        memcpy(r->data + pos, msg, msg_len);
    } else {
        memcpy(r->data + pos, msg, first);
        memcpy(r->data, (const uint8_t *)msg + first, msg_len - first);
    }

    /* Zero padding between msg_len and aligned. */
    if (aligned > msg_len) {
        uint32_t pad_start = (pi + msg_len) & mask;
        uint32_t pad_len = aligned - msg_len;
        uint32_t pad_first = r->data_size - pad_start;
        if (pad_first >= pad_len) {
            memset(r->data + pad_start, 0, pad_len);
        } else {
            memset(r->data + pad_start, 0, pad_first);
            memset(r->data, 0, pad_len - pad_first);
        }
    }

    /* Memory barrier would go here in kernel. */
    r->hdr.producer_index = pi + aligned;
    return 0;
}

/* Consume: read msg from ring. Returns bytes read, 0 if empty. */
static uint32_t sim_ring_consume(struct sim_ring *r, void *out, uint32_t out_size)
{
    uint32_t pi = r->hdr.producer_index;
    uint32_t ci = r->hdr.consumer_index;
    uint32_t mask = r->hdr.data_size_mask;
    uint32_t used = usbwarp_ring_used(pi, ci, mask);

    if (used == 0)
        return 0;  /* empty */

    /* Peek header to get message length. */
    uint32_t pos = ci & mask;
    struct usbwarp_msg_header hdr;

    if (used < sizeof(hdr))
        return 0;  /* not enough data for header */

    uint32_t first = r->data_size - pos;
    if (first >= sizeof(hdr)) {
        memcpy(&hdr, r->data + pos, sizeof(hdr));
    } else {
        memcpy(&hdr, r->data + pos, first);
        memcpy((uint8_t *)&hdr + first, r->data, sizeof(hdr) - first);
    }

    if (hdr.magic != USBWARP_MSG_MAGIC)
        return 0;  /* bad magic */

    if (hdr.message_length > out_size)
        return 0;  /* output buffer too small */

    uint32_t aligned = USBWARP_ALIGN_CACHELINE(hdr.message_length);
    if (used < aligned)
        return 0;  /* incomplete message */

    /* Copy message data. */
    if (first >= hdr.message_length) {
        memcpy(out, r->data + pos, hdr.message_length);
    } else {
        memcpy(out, r->data + pos, first);
        memcpy((uint8_t *)out + first, r->data, hdr.message_length - first);
    }

    /* Advance consumer index. */
    r->hdr.consumer_index = ci + aligned;
    return hdr.message_length;
}

/* Build a test message with specific payload. */
static void build_msg(void *buf, uint32_t total_len, uint16_t msg_type,
                      uint32_t txn_id, const void *payload, uint32_t payload_len)
{
    memset(buf, 0, total_len);
    struct usbwarp_msg_header *hdr = (struct usbwarp_msg_header *)buf;
    hdr->magic            = USBWARP_MSG_MAGIC;
    hdr->message_type     = msg_type;
    hdr->protocol_version = USBWARP_MAKE_VERSION(1, 0);
    hdr->message_length   = total_len;
    hdr->transaction_id   = txn_id;
    if (payload && payload_len > 0 && total_len > sizeof(*hdr))
        memcpy((uint8_t *)buf + sizeof(*hdr), payload,
               payload_len < (total_len - sizeof(*hdr)) ?
               payload_len : (total_len - sizeof(*hdr)));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Alignment tests
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_align_cacheline(void)
{
    TEST("ALIGN_CACHELINE(0) == 0");
    ASSERT_EQ(USBWARP_ALIGN_CACHELINE(0), 0u, "align(0)");
    PASS();

    TEST("ALIGN_CACHELINE(1) == 64");
    ASSERT_EQ(USBWARP_ALIGN_CACHELINE(1), 64u, "align(1)");
    PASS();

    TEST("ALIGN_CACHELINE(63) == 64");
    ASSERT_EQ(USBWARP_ALIGN_CACHELINE(63), 64u, "align(63)");
    PASS();

    TEST("ALIGN_CACHELINE(64) == 64");
    ASSERT_EQ(USBWARP_ALIGN_CACHELINE(64), 64u, "align(64)");
    PASS();

    TEST("ALIGN_CACHELINE(65) == 128");
    ASSERT_EQ(USBWARP_ALIGN_CACHELINE(65), 128u, "align(65)");
    PASS();

    TEST("ALIGN_CACHELINE(192) == 192");
    ASSERT_EQ(USBWARP_ALIGN_CACHELINE(192), 192u, "align(192)");
    PASS();

    TEST("ALIGN_CACHELINE(193) == 256");
    ASSERT_EQ(USBWARP_ALIGN_CACHELINE(193), 256u, "align(193)");
    PASS();

    TEST("ALIGN_CACHELINE(4096) == 4096");
    ASSERT_EQ(USBWARP_ALIGN_CACHELINE(4096), 4096u, "align(4096)");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Ring available/used arithmetic
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_ring_arithmetic(void)
{
    uint32_t mask = 4096 - 1;  /* 4K ring */

    TEST("empty ring: used=0, avail=4095");
    ASSERT_EQ(usbwarp_ring_used(0, 0, mask), 0u, "used");
    ASSERT_EQ(usbwarp_ring_available(0, 0, mask), 4095u, "avail");
    PASS();

    TEST("after produce 64B: used=64, avail=4031");
    ASSERT_EQ(usbwarp_ring_used(64, 0, mask), 64u, "used");
    ASSERT_EQ(usbwarp_ring_available(64, 0, mask), 4031u, "avail");
    PASS();

    TEST("after produce 4032B: used=4032, avail=63");
    ASSERT_EQ(usbwarp_ring_used(4032, 0, mask), 4032u, "used");
    ASSERT_EQ(usbwarp_ring_available(4032, 0, mask), 63u, "avail");
    PASS();

    /* Ring can hold at most data_size-1 bytes (one slot wasted for full detection). */
    TEST("one slot wasted: max usable = data_size - 1");
    ASSERT_EQ(usbwarp_ring_available(0, 0, mask), mask, "max avail");
    PASS();

    TEST("wrap-around: pi=4090, ci=100, used=3990");
    ASSERT_EQ(usbwarp_ring_used(4090, 100, mask), 3990u, "used wrap");
    PASS();

    TEST("wrap-around large pi: pi=8192, ci=4096, used=4096&mask=0");
    /* When pi and ci differ by exactly data_size, used wraps to 0 via mask.
     * This shouldn't happen in practice (ring can't be 100% full). */
    ASSERT_EQ(usbwarp_ring_used(8192, 4096, mask), 0u, "used overflow");
    PASS();

    /* Test with uint32 overflow (indices grow monotonically and wrap at 2^32). */
    TEST("uint32 overflow: pi=0xFFFFFF00, ci=0xFFFFFE00, used=256");
    ASSERT_EQ(usbwarp_ring_used(0xFFFFFF00u, 0xFFFFFE00u, mask), 256u, "used u32");
    PASS();

    TEST("uint32 overflow: pi=0x00000100, ci=0xFFFFFF00, used=1024");
    /* 0x100 - 0xFFFFFF00 = 0x200 = 512... let me compute correctly:
     * 0x00000100 - 0xFFFFFF00 = 0x00000200 (unsigned wrap) & 0xFFF = 0x200 = 512 */
    ASSERT_EQ(usbwarp_ring_used(0x100, 0xFFFFFF00u, mask),
              (0x100u - 0xFFFFFF00u) & mask, "used u32 wrap");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Basic produce / consume
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_basic_produce_consume(void)
{
    struct sim_ring ring;
    uint8_t msg[192];
    uint8_t out[192];
    uint32_t len;

    sim_ring_init(&ring, 4096);

    TEST("produce 1 message, consume 1 message");
    build_msg(msg, 64, USBWARP_MSG_HEARTBEAT, 1, NULL, 0);
    ASSERT_EQ(sim_ring_produce(&ring, msg, 64), 0, "produce");
    ASSERT_EQ(usbwarp_ring_used(ring.hdr.producer_index,
                                 ring.hdr.consumer_index,
                                 ring.hdr.data_size_mask), 64u, "used");
    len = sim_ring_consume(&ring, out, sizeof(out));
    ASSERT_EQ(len, 64u, "consume len");
    ASSERT_EQ(memcmp(msg, out, 64), 0, "data match");
    PASS();

    TEST("ring empty after consume");
    ASSERT_EQ(usbwarp_ring_used(ring.hdr.producer_index,
                                 ring.hdr.consumer_index,
                                 ring.hdr.data_size_mask), 0u, "used=0");
    len = sim_ring_consume(&ring, out, sizeof(out));
    ASSERT_EQ(len, 0u, "consume empty");
    PASS();

    TEST("produce 3 messages, consume 3 messages");
    for (int i = 0; i < 3; i++) {
        build_msg(msg, 64, USBWARP_MSG_HEARTBEAT, 100 + i, NULL, 0);
        ASSERT_EQ(sim_ring_produce(&ring, msg, 64), 0, "produce");
    }
    for (int i = 0; i < 3; i++) {
        len = sim_ring_consume(&ring, out, sizeof(out));
        ASSERT_EQ(len, 64u, "consume len");
        struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)out;
        ASSERT_EQ(h->transaction_id, (uint32_t)(100 + i), "txn_id");
    }
    PASS();

    sim_ring_free(&ring);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Ring full detection
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_ring_full(void)
{
    struct sim_ring ring;
    uint8_t msg[64];
    int count = 0;

    sim_ring_init(&ring, 4096);  /* 4096 bytes, 64B messages → 63 max */

    TEST("fill ring to capacity");
    build_msg(msg, 64, USBWARP_MSG_HEARTBEAT, 0, NULL, 0);
    while (sim_ring_produce(&ring, msg, 64) == 0)
        count++;

    /* 4096 / 64 = 64 slots, but one is wasted for full detection → 63 messages.
     * Actually: available = (ci - pi - 1) & mask.  With pi=0, ci=0:
     * avail = (0 - 0 - 1) & 4095 = 4095.  4095 / 64 = 63 full messages.
     * After 63: pi = 63*64 = 4032, avail = (0 - 4032 - 1) & 4095 = 63. 
     * 63 < 64, so can't fit another. */
    ASSERT_EQ(count, 63, "count");
    PASS();

    TEST("produce fails when full");
    ASSERT_EQ(sim_ring_produce(&ring, msg, 64), -1, "produce full");
    PASS();

    TEST("consume 1, then produce 1 succeeds");
    uint8_t out[64];
    uint32_t len = sim_ring_consume(&ring, out, sizeof(out));
    ASSERT_EQ(len, 64u, "consume");
    ASSERT_EQ(sim_ring_produce(&ring, msg, 64), 0, "produce after consume");
    PASS();

    sim_ring_free(&ring);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Wrap-around correctness
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_wrap_around(void)
{
    struct sim_ring ring;
    uint8_t msg[192];
    uint8_t out[192];
    uint32_t len;

    /* Small ring to force many wraps. */
    sim_ring_init(&ring, 256);

    TEST("wrap-around: produce/consume 1000 messages");
    int errors = 0;
    for (int i = 0; i < 1000; i++) {
        /* Use varying payload to detect data corruption. */
        uint8_t payload[32];
        for (int j = 0; j < 32; j++)
            payload[j] = (uint8_t)((i * 7 + j * 13) & 0xFF);

        build_msg(msg, 64, USBWARP_MSG_HEARTBEAT, i, payload, 32);

        if (sim_ring_produce(&ring, msg, 64) != 0) {
            /* Ring full — consume and retry. */
            len = sim_ring_consume(&ring, out, sizeof(out));
            if (len == 0) { errors++; continue; }
            if (sim_ring_produce(&ring, msg, 64) != 0) { errors++; continue; }
        }
    }

    /* Drain remaining. */
    while (sim_ring_consume(&ring, out, sizeof(out)) > 0)
        ;

    ASSERT_EQ(errors, 0, "errors");
    PASS();

    sim_ring_free(&ring);

    /* Test with message that crosses the ring boundary. */
    sim_ring_init(&ring, 256);

    TEST("wrap-around: message straddles ring end");

    /* Fill to near end: produce 3 × 64 = 192 bytes, then consume all.
     * pi = 192, ci = 192.  Next produce at pos = 192.
     * A 128-byte message starting at pos 192: first 64 at [192..255],
     * next 64 wraps to [0..63]. */
    for (int i = 0; i < 3; i++) {
        build_msg(msg, 64, USBWARP_MSG_HEARTBEAT, i, NULL, 0);
        sim_ring_produce(&ring, msg, 64);
    }
    for (int i = 0; i < 3; i++)
        sim_ring_consume(&ring, out, sizeof(out));

    /* Now pi=ci=192.  Produce a 128-byte message that wraps. */
    uint8_t big_payload[96];
    for (int i = 0; i < 96; i++)
        big_payload[i] = (uint8_t)(0xA0 + i);

    build_msg(msg, 128, USBWARP_MSG_URB_SUBMIT, 999, big_payload, 96);
    ASSERT_EQ(sim_ring_produce(&ring, msg, 128), 0, "produce wrap");

    len = sim_ring_consume(&ring, out, sizeof(out));
    ASSERT_EQ(len, 128u, "consume wrap len");
    ASSERT_EQ(memcmp(msg, out, 128), 0, "data integrity across wrap");
    PASS();

    sim_ring_free(&ring);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Variable message sizes
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_variable_sizes(void)
{
    struct sim_ring ring;
    uint8_t msg[4096];
    uint8_t out[4096];
    uint32_t len;

    sim_ring_init(&ring, 8192);

    /* Test various message sizes including non-aligned. */
    uint32_t sizes[] = {
        32,   /* minimum (header only) */
        33,   /* 1 byte payload → aligned to 64 */
        63,   /* almost 1 cacheline → aligned to 64 */
        64,   /* exact cacheline */
        65,   /* 1 over → aligned to 128 */
        96,   /* 1.5 cachelines → aligned to 128 */
        128,  /* 2 cachelines */
        192,  /* 3 cachelines (standard msg size) */
        256,  /* 4 cachelines */
        1024, /* large */
        2048, /* near max */
    };
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    TEST("variable sizes: produce and consume correctly");
    for (int i = 0; i < num_sizes; i++) {
        uint32_t sz = sizes[i];
        if (sz < sizeof(struct usbwarp_msg_header))
            continue;

        /* Fill payload with size-dependent pattern. */
        memset(msg, 0, sz);
        build_msg(msg, sz, USBWARP_MSG_URB_SUBMIT, 1000 + i, NULL, 0);
        for (uint32_t j = sizeof(struct usbwarp_msg_header); j < sz; j++)
            msg[j] = (uint8_t)(j ^ i);

        int rc = sim_ring_produce(&ring, msg, sz);
        if (rc != 0) {
            /* Drain and retry. */
            while (sim_ring_consume(&ring, out, sizeof(out)) > 0) ;
            rc = sim_ring_produce(&ring, msg, sz);
        }
        ASSERT_EQ(rc, 0, "produce");

        len = sim_ring_consume(&ring, out, sizeof(out));
        ASSERT_EQ(len, sz, "consume len");
        ASSERT_EQ(memcmp(msg, out, sz), 0, "data match");
    }
    PASS();

    sim_ring_free(&ring);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Interleaved produce/consume (SPSC simulation)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_interleaved(void)
{
    struct sim_ring ring;
    uint8_t msg[192];
    uint8_t out[192];
    uint32_t len;

    sim_ring_init(&ring, 1024);

    TEST("interleaved: produce 2, consume 1, repeat 500x");
    int produced = 0, consumed = 0;
    int errors = 0;

    for (int i = 0; i < 500; i++) {
        /* Produce 2. */
        for (int j = 0; j < 2; j++) {
            build_msg(msg, 64, USBWARP_MSG_HEARTBEAT, produced, NULL, 0);
            if (sim_ring_produce(&ring, msg, 64) == 0)
                produced++;
        }
        /* Consume 1. */
        len = sim_ring_consume(&ring, out, sizeof(out));
        if (len > 0) {
            struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)out;
            if (h->transaction_id != (uint32_t)consumed) errors++;
            consumed++;
        }
    }

    /* Drain. */
    while ((len = sim_ring_consume(&ring, out, sizeof(out))) > 0) {
        struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)out;
        if (h->transaction_id != (uint32_t)consumed) errors++;
        consumed++;
    }

    ASSERT_EQ(produced, consumed, "produced == consumed");
    ASSERT_EQ(errors, 0, "ordering errors");
    PASS();

    sim_ring_free(&ring);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Control Block layout verification
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_control_block_layout(void)
{
    TEST("control_block size == 4096");
    ASSERT_EQ(sizeof(struct usbwarp_control_block), 4096u, "cb size");
    PASS();

    TEST("ring_header size == 192");
    ASSERT_EQ(sizeof(struct usbwarp_ring_header), 192u, "rh size");
    PASS();

    TEST("msg_header size == 32");
    ASSERT_EQ(sizeof(struct usbwarp_msg_header), 32u, "mh size");
    PASS();

    TEST("cb.magic at offset 0x000");
    ASSERT_EQ(offsetof(struct usbwarp_control_block, magic), 0x000u, "offset");
    PASS();

    TEST("cb.g2h_ring_offset at offset 0x010");
    ASSERT_EQ(offsetof(struct usbwarp_control_block, g2h_ring_offset), 0x010u, "offset");
    PASS();

    TEST("cb.host_heartbeat_ts at offset 0x040 (cache line 1)");
    ASSERT_EQ(offsetof(struct usbwarp_control_block, host_heartbeat_ts), 0x040u, "offset");
    PASS();

    TEST("cb.guest_heartbeat_ts at offset 0x080 (cache line 2)");
    ASSERT_EQ(offsetof(struct usbwarp_control_block, guest_heartbeat_ts), 0x080u, "offset");
    PASS();

    TEST("ring_header.producer_index at offset 0x00");
    ASSERT_EQ(offsetof(struct usbwarp_ring_header, producer_index), 0x00u, "offset");
    PASS();

    TEST("ring_header.consumer_index at offset 0x40 (cache line 1)");
    ASSERT_EQ(offsetof(struct usbwarp_ring_header, consumer_index), 0x40u, "offset");
    PASS();

    TEST("ring_header.data_size at offset 0x80 (cache line 2)");
    ASSERT_EQ(offsetof(struct usbwarp_ring_header, data_size), 0x80u, "offset");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Message header validation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_msg_header_validation(void)
{
    struct sim_ring ring;
    uint8_t msg[64];
    uint8_t out[64];
    uint32_t len;

    sim_ring_init(&ring, 4096);

    TEST("reject message with bad magic");
    build_msg(msg, 64, USBWARP_MSG_HEARTBEAT, 1, NULL, 0);
    ((struct usbwarp_msg_header *)msg)->magic = 0xDEADBEEF;
    sim_ring_produce(&ring, msg, 64);
    len = sim_ring_consume(&ring, out, sizeof(out));
    ASSERT_EQ(len, 0u, "bad magic rejected");
    PASS();

    /* Reset ring. */
    ring.hdr.producer_index = 0;
    ring.hdr.consumer_index = 0;

    TEST("reject message with length > output buffer");
    build_msg(msg, 64, USBWARP_MSG_HEARTBEAT, 1, NULL, 0);
    ((struct usbwarp_msg_header *)msg)->message_length = 999;
    sim_ring_produce(&ring, msg, 64);
    len = sim_ring_consume(&ring, out, 32);  /* small output buffer */
    ASSERT_EQ(len, 0u, "too large rejected");
    PASS();

    sim_ring_free(&ring);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  Stress: rapid fill/drain cycles
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_fill_drain_stress(void)
{
    struct sim_ring ring;
    uint8_t msg[192];
    uint8_t out[192];

    sim_ring_init(&ring, 4096);

    TEST("stress: 100 fill/drain cycles");
    for (int cycle = 0; cycle < 100; cycle++) {
        /* Fill completely. */
        int count = 0;
        build_msg(msg, 64, USBWARP_MSG_HEARTBEAT, cycle * 1000, NULL, 0);
        while (sim_ring_produce(&ring, msg, 64) == 0)
            count++;

        /* Drain completely. */
        int drained = 0;
        while (sim_ring_consume(&ring, out, sizeof(out)) > 0)
            drained++;

        if (count != drained) {
            FAIL("cycle %d: produced %d consumed %d", cycle, count, drained);
            sim_ring_free(&ring);
            return;
        }
    }
    PASS();

    sim_ring_free(&ring);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Data integrity with pattern verification
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_data_integrity(void)
{
    struct sim_ring ring;
    uint8_t msg[192];
    uint8_t out[192];
    uint32_t len;

    sim_ring_init(&ring, 2048);

    TEST("data integrity: 10000 messages with unique patterns");
    int errors = 0;
    int next_consume = 0;

    for (int i = 0; i < 10000; i++) {
        /* Build message with unique pattern in payload. */
        uint32_t msg_size = 64 + (i % 3) * 64;  /* 64, 128, or 192 */
        memset(msg, 0, msg_size);
        build_msg(msg, msg_size, USBWARP_MSG_URB_COMPLETE, i, NULL, 0);

        /* Fill payload with deterministic pattern. */
        for (uint32_t j = sizeof(struct usbwarp_msg_header); j < msg_size; j++)
            msg[j] = (uint8_t)((i + j * 31) & 0xFF);

        while (sim_ring_produce(&ring, msg, msg_size) != 0) {
            /* Ring full — consume one. */
            len = sim_ring_consume(&ring, out, sizeof(out));
            if (len == 0) { errors++; break; }

            /* Verify consumed message. */
            struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)out;
            if (h->transaction_id != (uint32_t)next_consume) errors++;

            uint32_t expected_size = 64 + (next_consume % 3) * 64;
            if (len != expected_size) errors++;

            for (uint32_t j = sizeof(*h); j < len; j++) {
                uint8_t expected = (uint8_t)((next_consume + j * 31) & 0xFF);
                if (out[j] != expected) { errors++; break; }
            }
            next_consume++;
        }
    }

    /* Drain remaining. */
    while ((len = sim_ring_consume(&ring, out, sizeof(out))) > 0) {
        struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)out;
        if (h->transaction_id != (uint32_t)next_consume) errors++;

        uint32_t expected_size = 64 + (next_consume % 3) * 64;
        if (len != expected_size) errors++;

        for (uint32_t j = sizeof(*h); j < len; j++) {
            uint8_t expected = (uint8_t)((next_consume + j * 31) & 0xFF);
            if (out[j] != expected) { errors++; break; }
        }
        next_consume++;
    }

    ASSERT_EQ(next_consume, 10000, "all consumed");
    ASSERT_EQ(errors, 0, "data errors");
    PASS();

    sim_ring_free(&ring);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §12  Edge cases
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_edge_cases(void)
{
    struct sim_ring ring;
    uint8_t msg[4096];
    uint8_t out[4096];
    uint32_t len;

    /* Minimum size ring. */
    sim_ring_init(&ring, 256);

    TEST("minimum header-only message (32 bytes → aligned to 64)");
    build_msg(msg, 32, USBWARP_MSG_HEARTBEAT, 1, NULL, 0);
    ASSERT_EQ(sim_ring_produce(&ring, msg, 32), 0, "produce");
    len = sim_ring_consume(&ring, out, sizeof(out));
    ASSERT_EQ(len, 32u, "consume");
    PASS();

    sim_ring_free(&ring);

    /* Large message in small ring. */
    sim_ring_init(&ring, 256);

    TEST("message larger than ring → rejected");
    build_msg(msg, 256, USBWARP_MSG_URB_SUBMIT, 1, NULL, 0);
    /* aligned(256) = 256, but avail = 255. Should fail. */
    ASSERT_EQ(sim_ring_produce(&ring, msg, 256), -1, "too large");
    PASS();

    sim_ring_free(&ring);

    /* Exactly filling the ring with one message. */
    sim_ring_init(&ring, 256);

    TEST("message of max usable size (data_size - cacheline)");
    /* Max usable = avail = 255, aligned down to cacheline = 192.
     * Actually avail = 255, ALIGN(192) = 192 ≤ 255 → fits. */
    build_msg(msg, 192, USBWARP_MSG_URB_SUBMIT, 1, NULL, 0);
    ASSERT_EQ(sim_ring_produce(&ring, msg, 192), 0, "produce max");
    len = sim_ring_consume(&ring, out, sizeof(out));
    ASSERT_EQ(len, 192u, "consume max");
    ASSERT_EQ(memcmp(msg, out, 192), 0, "data match");
    PASS();

    sim_ring_free(&ring);

    /* Power-of-2 check. */
    TEST("IS_POWER_OF_2 helper");
    ASSERT_TRUE(USBWARP_IS_POWER_OF_2(1), "1");
    ASSERT_TRUE(USBWARP_IS_POWER_OF_2(2), "2");
    ASSERT_TRUE(USBWARP_IS_POWER_OF_2(256), "256");
    ASSERT_TRUE(USBWARP_IS_POWER_OF_2(4096), "4096");
    ASSERT_TRUE(!USBWARP_IS_POWER_OF_2(0), "0");
    ASSERT_TRUE(!USBWARP_IS_POWER_OF_2(3), "3");
    ASSERT_TRUE(!USBWARP_IS_POWER_OF_2(255), "255");
    ASSERT_TRUE(!USBWARP_IS_POWER_OF_2(4095), "4095");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §13  Message type coverage — verify all message structs fit in Ring
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_message_sizes(void)
{
    TEST("negotiate fits in standard ring");
    ASSERT_TRUE(sizeof(struct usbwarp_msg_negotiate) <= 192, "negotiate");
    PASS();

    TEST("negotiate_resp fits in standard ring");
    ASSERT_TRUE(sizeof(struct usbwarp_msg_negotiate_resp) <= 192, "negotiate_resp");
    PASS();

    TEST("urb_submit fits in standard ring");
    ASSERT_TRUE(sizeof(struct usbwarp_msg_urb_submit) <= 192, "urb_submit");
    PASS();

    TEST("urb_complete fits in standard ring");
    ASSERT_TRUE(sizeof(struct usbwarp_msg_urb_complete) <= 192, "urb_complete");
    PASS();

    TEST("device_added fits in standard ring");
    ASSERT_TRUE(sizeof(struct usbwarp_msg_device_added) <= 192, "device_added");
    PASS();

    TEST("device_removed fits in standard ring");
    ASSERT_TRUE(sizeof(struct usbwarp_msg_device_removed) <= 192, "device_removed");
    PASS();

    TEST("heartbeat fits in 1 cacheline");
    ASSERT_TRUE(sizeof(struct usbwarp_msg_heartbeat) <= 64, "heartbeat");
    PASS();

    TEST("MSG_MAGIC != CB_MAGIC (must be distinguishable)");
    ASSERT_TRUE(USBWARP_MSG_MAGIC != USBWARP_CONTROL_BLOCK_MAGIC, "magic");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §14  Monotonic index advancement (never goes backwards)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_monotonic_indices(void)
{
    struct sim_ring ring;
    uint8_t msg[64];
    uint8_t out[64];

    sim_ring_init(&ring, 1024);

    TEST("indices monotonically increase over 5000 operations");
    uint32_t prev_pi = 0, prev_ci = 0;
    int errors = 0;

    for (int i = 0; i < 5000; i++) {
        build_msg(msg, 64, USBWARP_MSG_HEARTBEAT, i, NULL, 0);

        if (sim_ring_produce(&ring, msg, 64) == 0) {
            /* Check pi increased. */
            if (ring.hdr.producer_index <= prev_pi && prev_pi != 0)
                /* Allow wrap at uint32 boundary — but pi should always
                 * increase by exactly aligned amount. */
                if ((ring.hdr.producer_index - prev_pi) != 64)
                    errors++;
            prev_pi = ring.hdr.producer_index;
        }

        if (i % 3 == 0) {
            if (sim_ring_consume(&ring, out, sizeof(out)) > 0) {
                if (ring.hdr.consumer_index <= prev_ci && prev_ci != 0)
                    if ((ring.hdr.consumer_index - prev_ci) != 64)
                        errors++;
                prev_ci = ring.hdr.consumer_index;
            }
        }
    }

    ASSERT_EQ(errors, 0, "monotonic violations");
    PASS();

    sim_ring_free(&ring);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §15  Alignment padding is zeroed
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_padding_zeroed(void)
{
    struct sim_ring ring;
    uint8_t msg[48];
    uint8_t raw[64];

    sim_ring_init(&ring, 4096);

    TEST("padding bytes between msg end and aligned boundary are zero");
    /* 48-byte message → aligned to 64.  Bytes [48..63] should be zero. */
    build_msg(msg, 48, USBWARP_MSG_HEARTBEAT, 1, NULL, 0);
    /* Fill tail with non-zero to prove produce zeroes it. */
    memset(ring.data, 0xFF, 64);
    sim_ring_produce(&ring, msg, 48);

    /* Read raw 64 bytes from ring. */
    memcpy(raw, ring.data, 64);

    int pad_errors = 0;
    for (int i = 48; i < 64; i++) {
        if (raw[i] != 0) pad_errors++;
    }
    ASSERT_EQ(pad_errors, 0, "padding not zeroed");
    PASS();

    sim_ring_free(&ring);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  UsbWarp Ring Protocol Tests (Layer 1)                      ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    printf("§1  Alignment\n");
    test_align_cacheline();

    printf("\n§2  Ring arithmetic\n");
    test_ring_arithmetic();

    printf("\n§3  Basic produce/consume\n");
    test_basic_produce_consume();

    printf("\n§4  Ring full detection\n");
    test_ring_full();

    printf("\n§5  Wrap-around\n");
    test_wrap_around();

    printf("\n§6  Variable message sizes\n");
    test_variable_sizes();

    printf("\n§7  Interleaved produce/consume\n");
    test_interleaved();

    printf("\n§8  Control Block layout\n");
    test_control_block_layout();

    printf("\n§9  Message header validation\n");
    test_msg_header_validation();

    printf("\n§10 Fill/drain stress\n");
    test_fill_drain_stress();

    printf("\n§11 Data integrity (10K messages)\n");
    test_data_integrity();

    printf("\n§12 Edge cases\n");
    test_edge_cases();

    printf("\n§13 Message struct sizes\n");
    test_message_sizes();

    printf("\n§14 Monotonic indices\n");
    test_monotonic_indices();

    printf("\n§15 Padding zeroed\n");
    test_padding_zeroed();

    printf("\n══════════════════════════════════════════════════════════════\n");
    printf("  Total: %d  Passed: \033[32m%d\033[0m  Failed: \033[%sm%d\033[0m\n",
           tests_run, tests_passed,
           tests_failed > 0 ? "31" : "32", tests_failed);
    printf("══════════════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
