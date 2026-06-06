// SPDX-License-Identifier: GPL-2.0-only
/*
 * ring_fuzz.c — Comprehensive fuzz test for UsbWarp Host driver (Layer 2)
 *
 * Generates malformed Ring messages and injects them into the G2H Ring
 * via the debugfs interface (/sys/kernel/debug/usbwarp/inject_g2h).
 * The Host driver (UsbWarp.sys) must handle ALL test vectors without
 * crashing (BSOD).
 *
 * Prerequisites:
 *   1. usbwarp.ko loaded with debugfs support (usbwarp_debugfs.c)
 *   2. UsbWarp.sys + UsbWarpFilter.sys running on Host
 *   3. System connected and negotiated (gs=2 in service log)
 *   4. Optionally: a USB device bound (for URB-related tests)
 *
 * Build:   gcc -O2 -Wall -Wextra -I../include -o ring_fuzz ring_fuzz.c
 * Run:     sudo ./ring_fuzz [-v] [-n COUNT] [-d DEVICE_PATH]
 *
 * Exit code 0 = all vectors injected (check Host for crashes).
 * The test PASSES if the Host is still alive after completion.
 *
 * IMPORTANT: Run with WinDbg attached to capture any Host-side panics.
 *            After the test completes, verify:
 *              - No BSOD occurred
 *              - WinDbg shows no unhandled exceptions
 *              - Service is still running (poll log continues)
 *              - dmesg shows no kernel oops on Guest side
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

/* Pull in protocol definitions. */
#include "usbwarp_shared.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

#define INJECT_PATH  "/sys/kernel/debug/usbwarp/inject_g2h"
#define STATUS_PATH  "/sys/kernel/debug/usbwarp/ring_status"
#define MAX_MSG_SIZE 4096
#define INTER_MSG_DELAY_US  1000   /* 1ms between injections */
#define INTER_GROUP_DELAY_US 50000 /* 50ms between test groups */

static int  g_fd = -1;
static int  g_verbose = 0;
static int  g_injected = 0;
static int  g_failed = 0;
static int  g_skipped = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 * Injection helper
 * ═══════════════════════════════════════════════════════════════════════════ */

static void inject(const char *name, const void *data, size_t len)
{
    if (g_verbose)
        printf("  [FUZZ] %-50s (%3zu bytes) ", name, len);

    ssize_t ret = write(g_fd, data, len);
    if (ret == (ssize_t)len) {
        g_injected++;
        if (g_verbose) printf("\033[32mINJECTED\033[0m\n");
    } else if (ret < 0 && errno == ENOSPC) {
        /* Ring full — wait and retry once. */
        usleep(10000);
        ret = write(g_fd, data, len);
        if (ret == (ssize_t)len) {
            g_injected++;
            if (g_verbose) printf("\033[33mRETRY OK\033[0m\n");
        } else {
            g_failed++;
            if (g_verbose) printf("\033[31mFAILED (ring full)\033[0m\n");
        }
    } else {
        g_failed++;
        if (g_verbose) printf("\033[31mFAILED (%s)\033[0m\n", strerror(errno));
    }

    usleep(INTER_MSG_DELAY_US);
}

/* Build a well-formed message header. */
static void fill_hdr(struct usbwarp_msg_header *h, uint16_t type,
                     uint32_t len, uint32_t dev_id, uint32_t txn_id)
{
    memset(h, 0, sizeof(*h));
    h->magic            = USBWARP_MSG_MAGIC;
    h->message_type     = type;
    h->protocol_version = USBWARP_MAKE_VERSION(1, 0);
    h->message_length   = len;
    h->device_id        = dev_id;
    h->transaction_id   = txn_id;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Magic field fuzzing
 *
 * Host must reject messages with incorrect magic.  Must NOT crash.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_magic(void)
{
    printf("\n§1  Magic field fuzzing\n");

    uint8_t buf[64];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    uint32_t bad_magics[] = {
        0x00000000,
        0xFFFFFFFF,
        0xDEADBEEF,
        0x12345678,
        USBWARP_CONTROL_BLOCK_MAGIC,  /* CB magic, not MSG magic */
        USBWARP_MSG_MAGIC ^ 1,        /* off by one */
        USBWARP_MSG_MAGIC >> 8,        /* shifted */
        USBWARP_MSG_MAGIC & 0xFFFF,    /* truncated */
    };

    for (size_t i = 0; i < sizeof(bad_magics)/sizeof(bad_magics[0]); i++) {
        memset(buf, 0, sizeof(buf));
        fill_hdr(h, USBWARP_MSG_HEARTBEAT, 64, 0, 1);
        h->magic = bad_magics[i];

        char name[64];
        snprintf(name, sizeof(name), "bad magic 0x%08x", bad_magics[i]);
        inject(name, buf, 64);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Message length fuzzing
 *
 * message_length field vs actual bytes written to Ring.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_message_length(void)
{
    printf("\n§2  Message length fuzzing\n");

    uint8_t buf[4096];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    /* Length = 0 */
    memset(buf, 0, 64);
    fill_hdr(h, USBWARP_MSG_HEARTBEAT, 0, 0, 100);
    inject("length=0", buf, 64);

    /* Length = 1 (less than header) */
    memset(buf, 0, 64);
    fill_hdr(h, USBWARP_MSG_HEARTBEAT, 1, 0, 101);
    inject("length=1", buf, 64);

    /* Length = 31 (one less than header) */
    memset(buf, 0, 64);
    fill_hdr(h, USBWARP_MSG_HEARTBEAT, 31, 0, 102);
    inject("length=31 (header-1)", buf, 64);

    /* Length = 32 (exactly header, no payload) */
    memset(buf, 0, 64);
    fill_hdr(h, USBWARP_MSG_HEARTBEAT, 32, 0, 103);
    inject("length=32 (header only)", buf, 64);

    /* Length claims 4096 but only 64 bytes written */
    memset(buf, 0, 64);
    fill_hdr(h, USBWARP_MSG_HEARTBEAT, 4096, 0, 104);
    inject("length=4096 actual=64", buf, 64);

    /* Length = 0xFFFFFFFF */
    memset(buf, 0, 64);
    fill_hdr(h, USBWARP_MSG_HEARTBEAT, 0xFFFFFFFF, 0, 105);
    inject("length=0xFFFFFFFF", buf, 64);

    /* Length = 0x80000000 (sign bit) */
    memset(buf, 0, 64);
    fill_hdr(h, USBWARP_MSG_HEARTBEAT, 0x80000000, 0, 106);
    inject("length=0x80000000", buf, 64);

    /* Length = 33 (not cacheline-aligned) */
    memset(buf, 0, 64);
    fill_hdr(h, USBWARP_MSG_HEARTBEAT, 33, 0, 107);
    inject("length=33 (unaligned)", buf, 64);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Message type fuzzing
 *
 * Invalid and out-of-range message types.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_message_type(void)
{
    printf("\n§3  Message type fuzzing\n");

    uint8_t buf[64];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    uint16_t bad_types[] = {
        0,                          /* NONE / unset */
        0xFFFF,                     /* max uint16 */
        0x7FFF,                     /* large */
        99,                         /* undefined */
        USBWARP_MSG_NEGOTIATE_RESP, /* Host→Guest only, not Guest→Host */
        USBWARP_MSG_URB_COMPLETE,   /* Host→Guest only */
        USBWARP_MSG_DEVICE_ADDED,   /* Host→Guest only */
        USBWARP_MSG_DEVICE_REMOVED, /* Host→Guest only */
        USBWARP_MSG_URB_CANCEL_ACK, /* Host→Guest only */
        200, 201, 255,              /* undefined */
    };

    for (size_t i = 0; i < sizeof(bad_types)/sizeof(bad_types[0]); i++) {
        memset(buf, 0, sizeof(buf));
        fill_hdr(h, bad_types[i], 64, 0, 200 + i);

        char name[64];
        snprintf(name, sizeof(name), "msg_type=%u", bad_types[i]);
        inject(name, buf, 64);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Protocol version fuzzing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_protocol_version(void)
{
    printf("\n§4  Protocol version fuzzing\n");

    uint8_t buf[64];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    uint16_t bad_versions[] = {
        0x0000,                          /* 0.0 */
        0x0200,                          /* 2.0 (future) */
        0xFF00,                          /* 255.0 */
        0x00FF,                          /* 0.255 */
        0xFFFF,                          /* 255.255 */
        USBWARP_MAKE_VERSION(0, 0),      /* 0.0 */
        USBWARP_MAKE_VERSION(2, 0),      /* major mismatch */
        USBWARP_MAKE_VERSION(1, 99),     /* minor mismatch */
    };

    for (size_t i = 0; i < sizeof(bad_versions)/sizeof(bad_versions[0]); i++) {
        memset(buf, 0, sizeof(buf));
        fill_hdr(h, USBWARP_MSG_HEARTBEAT, 64, 0, 300 + i);
        h->protocol_version = bad_versions[i];

        char name[64];
        snprintf(name, sizeof(name), "version=0x%04x", bad_versions[i]);
        inject(name, buf, 64);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Device ID fuzzing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_device_id(void)
{
    printf("\n§5  Device ID fuzzing\n");

    uint8_t buf[192];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    uint32_t bad_ids[] = {
        0,                           /* global (valid for heartbeat, invalid for URB) */
        USBWARP_MAX_DEVICES_LIMIT,   /* boundary */
        USBWARP_MAX_DEVICES_LIMIT+1, /* out of range */
        99,                          /* unlikely to exist */
        0xFFFFFFFF,                  /* max uint32 */
        0x80000000,                  /* sign bit */
        0xDEAD,                      /* garbage */
    };

    for (size_t i = 0; i < sizeof(bad_ids)/sizeof(bad_ids[0]); i++) {
        /* As URB_SUBMIT — device_id must be valid. */
        memset(buf, 0, sizeof(buf));
        fill_hdr(h, USBWARP_MSG_URB_SUBMIT, 192, bad_ids[i], 400 + i);
        struct usbwarp_msg_urb_submit *sub =
            (struct usbwarp_msg_urb_submit *)buf;
        sub->device_id = bad_ids[i];
        sub->transfer_type = USBWARP_XFER_CONTROL;
        sub->endpoint = 0;
        sub->direction = 1;
        sub->transfer_length = 0;
        sub->data_mode = USBWARP_DATA_NONE;

        char name[64];
        snprintf(name, sizeof(name), "URB device_id=%u", bad_ids[i]);
        inject(name, buf, 192);

        /* As URB_CANCEL. */
        memset(buf, 0, sizeof(buf));
        fill_hdr(h, USBWARP_MSG_URB_CANCEL, 64, bad_ids[i], 500 + i);

        snprintf(name, sizeof(name), "CANCEL device_id=%u", bad_ids[i]);
        inject(name, buf, 64);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  URB_SUBMIT transfer type fuzzing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_transfer_type(void)
{
    printf("\n§6  Transfer type fuzzing\n");

    uint8_t buf[192];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    uint32_t types[] = {
        0,  /* CONTROL */
        1,  /* ISO (unsupported) */
        2,  /* BULK */
        3,  /* INTERRUPT */
        4,  /* out of range */
        99, /* garbage */
        0xFF,
        0xFFFFFFFF,
    };

    for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++) {
        memset(buf, 0, sizeof(buf));
        fill_hdr(h, USBWARP_MSG_URB_SUBMIT, 192, 1, 600 + i);
        struct usbwarp_msg_urb_submit *sub =
            (struct usbwarp_msg_urb_submit *)buf;
        sub->device_id = 1;
        sub->transfer_type = types[i];
        sub->endpoint = 0;
        sub->direction = 1;
        sub->transfer_length = 0;
        sub->data_mode = USBWARP_DATA_NONE;

        char name[64];
        snprintf(name, sizeof(name), "transfer_type=%u", types[i]);
        inject(name, buf, 192);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  URB_SUBMIT endpoint fuzzing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_endpoint(void)
{
    printf("\n§7  Endpoint fuzzing\n");

    uint8_t buf[192];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    uint32_t eps[] = {
        0,      /* EP0 control */
        1,      /* EP1 */
        15,     /* EP15 (max valid) */
        16,     /* out of range */
        127,    /* large */
        255,    /* max uint8 */
        0xFF,
    };

    for (size_t i = 0; i < sizeof(eps)/sizeof(eps[0]); i++) {
        for (uint32_t dir = 0; dir <= 2; dir++) {  /* 0=OUT, 1=IN, 2=invalid */
            memset(buf, 0, sizeof(buf));
            fill_hdr(h, USBWARP_MSG_URB_SUBMIT, 192, 1, 700 + i * 3 + dir);
            struct usbwarp_msg_urb_submit *sub =
                (struct usbwarp_msg_urb_submit *)buf;
            sub->device_id = 1;
            sub->transfer_type = USBWARP_XFER_BULK;
            sub->endpoint = eps[i];
            sub->direction = dir;
            sub->transfer_length = 0;
            sub->data_mode = USBWARP_DATA_NONE;

            char name[64];
            snprintf(name, sizeof(name), "ep=%u dir=%u", eps[i], dir);
            inject(name, buf, 192);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Transfer length fuzzing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_transfer_length(void)
{
    printf("\n§8  Transfer length fuzzing\n");

    uint8_t buf[192];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    uint32_t lengths[] = {
        0,
        1,
        64,
        65536,
        1048576,       /* 1 MB */
        16777216,      /* 16 MB (exceeds data region) */
        0x7FFFFFFF,    /* INT_MAX */
        0x80000000,    /* sign bit */
        0xFFFFFFFF,    /* max uint32 */
    };

    for (size_t i = 0; i < sizeof(lengths)/sizeof(lengths[0]); i++) {
        memset(buf, 0, sizeof(buf));
        fill_hdr(h, USBWARP_MSG_URB_SUBMIT, 192, 1, 800 + i);
        struct usbwarp_msg_urb_submit *sub =
            (struct usbwarp_msg_urb_submit *)buf;
        sub->device_id = 1;
        sub->transfer_type = USBWARP_XFER_BULK;
        sub->endpoint = 2;
        sub->direction = 0;
        sub->transfer_length = lengths[i];
        sub->data_mode = USBWARP_DATA_BUFFER;
        sub->buffer_offset = 0;  /* will be bounds-checked by Host */

        char name[64];
        snprintf(name, sizeof(name), "xfer_length=%u", lengths[i]);
        inject(name, buf, 192);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Buffer offset fuzzing (DATA_BUFFER mode)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_buffer_offset(void)
{
    printf("\n§9  Buffer offset fuzzing\n");

    uint8_t buf[192];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    struct { uint32_t offset; uint32_t length; } cases[] = {
        { 0, 64 },                  /* valid start */
        { 0xFFFFFFFF, 1 },          /* max offset */
        { 0xFFFFFFF0, 64 },         /* offset + length overflows uint32 */
        { 0x80000000, 1 },          /* sign bit */
        { 33554432, 1 },            /* exactly at SHM end (32MB) */
        { 33554433, 1 },            /* beyond SHM */
        { 0, 0xFFFFFFFF },          /* zero offset, huge length */
        { 1, 33554432 },            /* offset 1, spans entire SHM */
        { 0x82000, 65536 },         /* data region start, 1 buffer */
        { 0x82000 - 1, 2 },         /* crosses data region boundary */
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        memset(buf, 0, sizeof(buf));
        fill_hdr(h, USBWARP_MSG_URB_SUBMIT, 192, 1, 900 + i);
        struct usbwarp_msg_urb_submit *sub =
            (struct usbwarp_msg_urb_submit *)buf;
        sub->device_id = 1;
        sub->transfer_type = USBWARP_XFER_BULK;
        sub->endpoint = 2;
        sub->direction = 0;
        sub->transfer_length = cases[i].length;
        sub->data_mode = USBWARP_DATA_BUFFER;
        sub->buffer_offset = cases[i].offset;

        char name[64];
        snprintf(name, sizeof(name), "buf off=0x%x len=%u",
                 cases[i].offset, cases[i].length);
        inject(name, buf, 192);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  Data mode fuzzing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_data_mode(void)
{
    printf("\n§10 Data mode fuzzing\n");

    uint8_t buf[192];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    uint32_t modes[] = {
        USBWARP_DATA_NONE,
        USBWARP_DATA_INLINE,
        USBWARP_DATA_BUFFER,
        3,          /* undefined */
        99,         /* garbage */
        0xFF,
        0xFFFFFFFF,
    };

    for (size_t i = 0; i < sizeof(modes)/sizeof(modes[0]); i++) {
        memset(buf, 0, sizeof(buf));
        fill_hdr(h, USBWARP_MSG_URB_SUBMIT, 192, 1, 1000 + i);
        struct usbwarp_msg_urb_submit *sub =
            (struct usbwarp_msg_urb_submit *)buf;
        sub->device_id = 1;
        sub->transfer_type = USBWARP_XFER_BULK;
        sub->endpoint = 2;
        sub->direction = 0;
        sub->transfer_length = 64;
        sub->data_mode = modes[i];
        sub->buffer_offset = 0x82000;

        char name[64];
        snprintf(name, sizeof(name), "data_mode=%u", modes[i]);
        inject(name, buf, 192);
    }

    /* INLINE mode with invalid inline data sizes */
    printf("\n    Inline data edge cases\n");

    uint32_t inline_sizes[] = { 0, 1, 96, 97, 128, 255 };
    for (size_t i = 0; i < sizeof(inline_sizes)/sizeof(inline_sizes[0]); i++) {
        memset(buf, 0xAA, sizeof(buf));
        fill_hdr(h, USBWARP_MSG_URB_SUBMIT, 192, 1, 1100 + i);
        struct usbwarp_msg_urb_submit *sub =
            (struct usbwarp_msg_urb_submit *)buf;
        sub->device_id = 1;
        sub->transfer_type = USBWARP_XFER_BULK;
        sub->endpoint = 2;
        sub->direction = 0;
        sub->transfer_length = inline_sizes[i];
        sub->data_mode = USBWARP_DATA_INLINE;
        /* inline_data is at a fixed offset in the struct */

        char name[64];
        snprintf(name, sizeof(name), "inline len=%u", inline_sizes[i]);
        inject(name, buf, 192);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  Control transfer setup packet fuzzing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_setup_packet(void)
{
    printf("\n§11 Setup packet fuzzing\n");

    uint8_t buf[192];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    struct {
        const char *name;
        uint8_t setup[8];
        uint32_t xfer_len;
        uint32_t direction;
    } cases[] = {
        /* SET_ADDRESS — must be blocked */
        { "SET_ADDRESS",           {0x00, 0x05, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00}, 0, 0 },

        /* wLength/transfer_length mismatch */
        { "wLen=7 xferLen=0",      {0x21, 0x20, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00}, 0, 0 },
        { "wLen=0 xferLen=7",      {0x21, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 7, 0 },
        { "wLen=100 xferLen=1",    {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x64, 0x00}, 1, 1 },

        /* Direction mismatch: bmReqType says IN, msg says OUT */
        { "dir mismatch IN/OUT",   {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00}, 18, 0 },
        /* Direction mismatch: bmReqType says OUT, msg says IN */
        { "dir mismatch OUT/IN",   {0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}, 0, 1 },

        /* Reserved bRequest values */
        { "bRequest=0xFF",         {0x80, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00}, 8, 1 },

        /* Huge wLength */
        { "wLength=0xFFFF",        {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xFF}, 65535, 1 },

        /* All zeros */
        { "all-zero setup",        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0, 0 },

        /* All 0xFF */
        { "all-FF setup",          {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 65535, 1 },

        /* Valid GET_DESCRIPTOR but huge length */
        { "GET_DESC len=0",        {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}, 0, 1 },

        /* SET_FEATURE to device */
        { "SET_FEATURE",           {0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}, 0, 0 },

        /* CLEAR_FEATURE */
        { "CLEAR_FEATURE",         {0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0, 0 },

        /* SET_INTERFACE */
        { "SET_INTERFACE",         {0x01, 0x0B, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}, 0, 0 },

        /* Vendor request with data */
        { "vendor OUT data=64",    {0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00}, 64, 0 },
        { "vendor IN data=64",     {0xC0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00}, 64, 1 },
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        memset(buf, 0, sizeof(buf));
        fill_hdr(h, USBWARP_MSG_URB_SUBMIT, 192, 1, 1200 + i);
        struct usbwarp_msg_urb_submit *sub =
            (struct usbwarp_msg_urb_submit *)buf;
        sub->device_id = 1;
        sub->transfer_type = USBWARP_XFER_CONTROL;
        sub->endpoint = 0;
        sub->direction = cases[i].direction;
        sub->transfer_length = cases[i].xfer_len;
        sub->data_mode = USBWARP_DATA_NONE;
        memcpy(sub->setup_packet, cases[i].setup, 8);

        inject(cases[i].name, buf, 192);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §12  NEGOTIATE message fuzzing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_negotiate(void)
{
    printf("\n§12 Negotiate fuzzing\n");

    uint8_t buf[192];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    /* Double negotiate (re-negotiate after already running) */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, USBWARP_MSG_NEGOTIATE, 192, 0, 1300);
    struct usbwarp_msg_negotiate *neg = (struct usbwarp_msg_negotiate *)buf;
    neg->min_version = USBWARP_MAKE_VERSION(1, 0);
    neg->max_version = USBWARP_MAKE_VERSION(1, 0);
    neg->capabilities = USBWARP_CAP_INLINE_DATA;
    neg->max_pending_urbs = 256;
    inject("re-negotiate (already running)", buf, 192);

    /* Negotiate with version 0.0 */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, USBWARP_MSG_NEGOTIATE, 192, 0, 1301);
    neg = (struct usbwarp_msg_negotiate *)buf;
    neg->min_version = 0;
    neg->max_version = 0;
    neg->capabilities = 0;
    inject("negotiate ver=0.0", buf, 192);

    /* Negotiate with version 99.99 */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, USBWARP_MSG_NEGOTIATE, 192, 0, 1302);
    neg = (struct usbwarp_msg_negotiate *)buf;
    neg->min_version = USBWARP_MAKE_VERSION(99, 99);
    neg->max_version = USBWARP_MAKE_VERSION(99, 99);
    neg->capabilities = 0xFFFFFFFF;
    neg->max_pending_urbs = 0xFFFFFFFF;
    inject("negotiate ver=99.99 max caps", buf, 192);

    /* Negotiate with max_pending_urbs = 0 */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, USBWARP_MSG_NEGOTIATE, 192, 0, 1303);
    neg = (struct usbwarp_msg_negotiate *)buf;
    neg->min_version = USBWARP_MAKE_VERSION(1, 0);
    neg->max_version = USBWARP_MAKE_VERSION(1, 0);
    neg->max_pending_urbs = 0;
    inject("negotiate pending=0", buf, 192);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §13  Heartbeat fuzzing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_heartbeat(void)
{
    printf("\n§13 Heartbeat fuzzing\n");

    uint8_t buf[64];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    /* Heartbeat with device_id != 0 (should be global) */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, USBWARP_MSG_HEARTBEAT, 64, 999, 1400);
    inject("heartbeat device_id=999", buf, 64);

    /* Rapid heartbeat burst */
    for (int i = 0; i < 20; i++) {
        memset(buf, 0, sizeof(buf));
        fill_hdr(h, USBWARP_MSG_HEARTBEAT, 64, 0, 1410 + i);
        inject("heartbeat burst", buf, 64);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §14  URB_CANCEL fuzzing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_urb_cancel(void)
{
    printf("\n§14 URB cancel fuzzing\n");

    uint8_t buf[192];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    /* Cancel non-existent transaction */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, USBWARP_MSG_URB_CANCEL, 64, 1, 0xDEADBEEF);
    inject("cancel txn=0xDEADBEEF", buf, 64);

    /* Cancel with device_id=0 */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, USBWARP_MSG_URB_CANCEL, 64, 0, 1500);
    inject("cancel device_id=0", buf, 64);

    /* Cancel with txn_id=0 */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, USBWARP_MSG_URB_CANCEL, 64, 1, 0);
    inject("cancel txn_id=0", buf, 64);

    /* Cancel same txn_id twice */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, USBWARP_MSG_URB_CANCEL, 64, 1, 9999);
    inject("cancel txn=9999 (1st)", buf, 64);
    inject("cancel txn=9999 (2nd)", buf, 64);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §15  Raw garbage injection
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_garbage(void)
{
    printf("\n§15 Raw garbage injection\n");

    uint8_t buf[256];

    /* All zeros */
    memset(buf, 0x00, 64);
    inject("64B all-zero", buf, 64);

    /* All ones */
    memset(buf, 0xFF, 64);
    inject("64B all-FF", buf, 64);

    /* Ascending bytes */
    for (int i = 0; i < 192; i++) buf[i] = (uint8_t)i;
    inject("192B ascending", buf, 192);

    /* Random data (seeded for reproducibility) */
    srand(42);
    for (int round = 0; round < 10; round++) {
        size_t len = 32 + (rand() % 225);  /* 32..256 */
        len = (len + 63) & ~63;            /* align to cacheline */
        if (len > 256) len = 256;
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)(rand() & 0xFF);

        char name[64];
        snprintf(name, sizeof(name), "random #%d (%zu bytes)", round, len);
        inject(name, buf, len);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §16  Rapid-fire stress (Ring throughput)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_rapid_fire(void)
{
    printf("\n§16 Rapid-fire stress (200 messages, no delay)\n");

    uint8_t buf[64];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;
    int saved_delay = INTER_MSG_DELAY_US;

    for (int i = 0; i < 200; i++) {
        memset(buf, 0, sizeof(buf));
        fill_hdr(h, USBWARP_MSG_HEARTBEAT, 64, 0, 2000 + i);
        /* No delay between messages — stress the Ring and poll thread. */
        ssize_t ret = write(g_fd, buf, 64);
        if (ret == 64) g_injected++;
        else g_failed++;
    }

    printf("  [FUZZ] %-50s %s\n", "200 rapid heartbeats",
           g_failed == 0 ? "\033[32mDONE\033[0m" : "\033[33mSOME FAILED\033[0m");

    /* Let Host catch up. */
    usleep(100000);
    (void)saved_delay;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §17  Combination attacks (multiple bad fields at once)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void fuzz_combinations(void)
{
    printf("\n§17 Combination attacks\n");

    uint8_t buf[192];
    struct usbwarp_msg_header *h = (struct usbwarp_msg_header *)buf;

    /* Bad magic + bad length + bad type + bad device */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, 0xFFFF, 192, 0xFFFFFFFF, 3000);
    h->magic = 0;
    h->message_length = 0xFFFFFFFF;
    h->protocol_version = 0xFFFF;
    inject("all fields invalid", buf, 192);

    /* Valid header, URB_SUBMIT with contradictory fields */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, USBWARP_MSG_URB_SUBMIT, 192, 1, 3001);
    struct usbwarp_msg_urb_submit *sub =
        (struct usbwarp_msg_urb_submit *)buf;
    sub->device_id = 1;
    sub->transfer_type = USBWARP_XFER_CONTROL;
    sub->endpoint = 15;         /* control only uses EP0 */
    sub->direction = 1;
    sub->transfer_length = 64;
    sub->data_mode = USBWARP_DATA_BUFFER;
    sub->buffer_offset = 0xFFFFFFFF;  /* invalid offset */
    /* Setup says OUT but direction says IN */
    sub->setup_packet[0] = 0x00;  /* OUT */
    sub->setup_packet[6] = 64;    /* wLength=64 */
    inject("ctrl EP15 bad offset dir mismatch", buf, 192);

    /* BULK with DATA_INLINE but transfer_length > inline capacity */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, USBWARP_MSG_URB_SUBMIT, 192, 1, 3002);
    sub = (struct usbwarp_msg_urb_submit *)buf;
    sub->device_id = 1;
    sub->transfer_type = USBWARP_XFER_BULK;
    sub->endpoint = 2;
    sub->direction = 0;
    sub->transfer_length = 10000;  /* way more than inline can hold */
    sub->data_mode = USBWARP_DATA_INLINE;
    inject("bulk inline xferLen=10000", buf, 192);

    /* INTERRUPT with device_id that exists but wrong pipe */
    memset(buf, 0, sizeof(buf));
    fill_hdr(h, USBWARP_MSG_URB_SUBMIT, 192, 1, 3003);
    sub = (struct usbwarp_msg_urb_submit *)buf;
    sub->device_id = 1;
    sub->transfer_type = USBWARP_XFER_INTERRUPT;
    sub->endpoint = 7;  /* unlikely to exist */
    sub->direction = 1;
    sub->transfer_length = 8;
    sub->data_mode = USBWARP_DATA_NONE;
    inject("interrupt bad pipe EP7", buf, 192);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_status(void)
{
    int fd = open(STATUS_PATH, O_RDONLY);
    if (fd >= 0) {
        char buf[512];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("  %s", buf);
        }
        close(fd);
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            g_verbose = 1;
    }

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  UsbWarp Protocol Fuzz Test (Layer 2)                       ║\n");
    printf("║                                                             ║\n");
    printf("║  Target: UsbWarp.sys Host driver message parsing            ║\n");
    printf("║  Method: Raw G2H Ring injection via debugfs                 ║\n");
    printf("║  Pass:   Host is still alive after all vectors injected     ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    printf("Pre-test status:\n");
    print_status();

    g_fd = open(INJECT_PATH, O_WRONLY);
    if (g_fd < 0) {
        fprintf(stderr, "ERROR: Cannot open %s: %s\n", INJECT_PATH,
                strerror(errno));
        fprintf(stderr, "  Ensure usbwarp.ko is loaded with debugfs support.\n");
        fprintf(stderr, "  Check: ls -la %s\n", INJECT_PATH);
        return 1;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* ── Run all fuzz groups ─────────────────────────────────────────── */

    fuzz_magic();                usleep(INTER_GROUP_DELAY_US);
    fuzz_message_length();       usleep(INTER_GROUP_DELAY_US);
    fuzz_message_type();         usleep(INTER_GROUP_DELAY_US);
    fuzz_protocol_version();     usleep(INTER_GROUP_DELAY_US);
    fuzz_device_id();            usleep(INTER_GROUP_DELAY_US);
    fuzz_transfer_type();        usleep(INTER_GROUP_DELAY_US);
    fuzz_endpoint();             usleep(INTER_GROUP_DELAY_US);
    fuzz_transfer_length();      usleep(INTER_GROUP_DELAY_US);
    fuzz_buffer_offset();        usleep(INTER_GROUP_DELAY_US);
    fuzz_data_mode();            usleep(INTER_GROUP_DELAY_US);
    fuzz_setup_packet();         usleep(INTER_GROUP_DELAY_US);
    fuzz_negotiate();            usleep(INTER_GROUP_DELAY_US);
    fuzz_heartbeat();            usleep(INTER_GROUP_DELAY_US);
    fuzz_urb_cancel();           usleep(INTER_GROUP_DELAY_US);
    fuzz_garbage();              usleep(INTER_GROUP_DELAY_US);
    fuzz_rapid_fire();           usleep(INTER_GROUP_DELAY_US);
    fuzz_combinations();

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    close(g_fd);

    /* ── Wait for Host to process all messages ───────────────────────── */
    printf("\nWaiting 3s for Host to process all messages...\n");
    sleep(3);

    /* ── Results ─────────────────────────────────────────────────────── */
    printf("\n══════════════════════════════════════════════════════════════\n");
    printf("  Injected: \033[32m%d\033[0m   Failed: \033[%sm%d\033[0m   "
           "Skipped: %d   Time: %.1fs\n",
           g_injected,
           g_failed > 0 ? "33" : "32", g_failed,
           g_skipped, elapsed);
    printf("══════════════════════════════════════════════════════════════\n");

    printf("\nPost-test status:\n");
    print_status();

    printf("\n\033[1mVERDICT:\033[0m  ");
    printf("If you can read this and the Host hasn't BSODed → ");
    printf("\033[32;1mPASS\033[0m\n");
    printf("  Also check:\n");
    printf("    - WinDbg: no unhandled exceptions\n");
    printf("    - Service log: poll still running (devs=X drv=yes)\n");
    printf("    - dmesg: no kernel oops\n\n");

    return 0;
}
