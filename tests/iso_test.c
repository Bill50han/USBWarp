/* SPDX-License-Identifier: GPL-2.0-only
 * Tests the validation functions used by both kernel drivers and the filter.
 * gcc -std=c11 -Wall -Wextra -Werror -Iinclude tests/iso_test.c -o iso_test
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usbwarp_iso.h"

static unsigned checks;
static void check(int condition, const char *expression, int line)
{
    ++checks;
    if (!condition) {
        fprintf(stderr, "line %d: %s\n", line, expression);
        exit(1);
    }
}
#define CHECK(x) check((x), #x, __LINE__)

static struct usbwarp_msg_iso_submit *make_submit(unsigned count)
{
    unsigned i, len = usbwarp_iso_message_size(count);
    struct usbwarp_msg_iso_submit *m = calloc(1, len);
    struct usbwarp_iso_packet_desc *p;
    assert(m);
    m->hdr.message_length = len;
    m->hdr.device_id = m->device_id = 1;
    m->endpoint = 1;
    m->direction = 1;
    m->number_of_packets = count;
    m->interval = 1;
    m->total_length = count * 192;
    p = usbwarp_iso_packets(m);
    for (i = 0; i < count; ++i) {
        p[i].offset = i * 192;
        p[i].length = 192;
    }
    return m;
}

int main(void)
{
    unsigned i, len;
    struct usbwarp_msg_iso_submit *m = make_submit(1024);
    struct usbwarp_iso_packet_desc *p = usbwarp_iso_packets(m);
    struct usbwarp_msg_iso_complete *c;
    uint32_t saved, seed = 0x918ef9b1;
    len = m->hdr.message_length;
    CHECK(len == 8256 && len == USBWARP_MAX_MESSAGE_SIZE);
    CHECK((unsigned char *)p - (unsigned char *)m == 64);
    CHECK(offsetof(struct usbwarp_msg_iso_submit, buffer_offset) == 0x34);
    CHECK(offsetof(struct usbwarp_msg_iso_submit, total_length) == 0x38);
    CHECK(usbwarp_iso_submit_valid(m, len));
    m->hdr.flags = 19; CHECK(usbwarp_iso_submit_valid(m, len)); m->hdr.flags = 0;
    for (i = 0; i < len; ++i)
        CHECK(!usbwarp_iso_submit_valid(m, i));
    m->number_of_packets = UINT32_MAX;
    CHECK(!usbwarp_iso_submit_valid(m, len));
    m->number_of_packets = 0;
    CHECK(!usbwarp_iso_submit_valid(m, len));
    m->number_of_packets = 1024;
    p[1].offset++;
    CHECK(!usbwarp_iso_submit_valid(m, len));
    p[1].offset -= 2;
    CHECK(!usbwarp_iso_submit_valid(m, len));
    p[1].offset++;
    p[0].length = UINT32_MAX;
    CHECK(!usbwarp_iso_submit_valid(m, len));
    p[0].length = 192;
    m->endpoint = 0; CHECK(!usbwarp_iso_submit_valid(m, len));
    m->endpoint = 16; CHECK(!usbwarp_iso_submit_valid(m, len));
    m->endpoint = 1;
    m->direction = 2; CHECK(!usbwarp_iso_submit_valid(m, len)); m->direction = 1;
    m->start_frame = 123; CHECK(!usbwarp_iso_submit_valid(m, len)); m->start_frame = 0;
    m->device_id = 2; CHECK(!usbwarp_iso_submit_valid(m, len)); m->device_id = 1;
    m->interval = 3; CHECK(!usbwarp_iso_submit_valid(m, len)); m->interval = 1;
    ++m->total_length; CHECK(!usbwarp_iso_submit_valid(m, len)); --m->total_length;
    /* Randomly corrupted offsets and lengths cannot escape the data region. */
    for (i = 0; i < 100000; ++i) {
        unsigned n = i % 1024;
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        saved = p[n].length;
        p[n].length = seed;
        CHECK(usbwarp_iso_submit_valid(m, len) == (seed == saved));
        p[n].length = saved;
    }
    free(m);
    /* Variable-size audio OUT packets, including a zero-length packet. */
    {
        struct usbwarp_iso_packet_desc packets[] = {{0, 176}, {176, 0}, {176, 180}};
        CHECK(usbwarp_iso_layout_valid(packets, 3, 356));
        packets[2].offset = 192; /* a Linux buffer gap must be packed first */
        CHECK(!usbwarp_iso_layout_valid(packets, 3, 372));
    }
    CHECK(usbwarp_iso_packet_count_valid(255, USBWARP_SPEED_FULL, 1));
    CHECK(!usbwarp_iso_packet_count_valid(256, USBWARP_SPEED_FULL, 1));
    CHECK(!usbwarp_iso_packet_count_valid(1, USBWARP_SPEED_LOW, 1));
    CHECK(usbwarp_iso_packet_count_valid(1024, USBWARP_SPEED_HIGH, 1));
    CHECK(!usbwarp_iso_packet_count_valid(1025, USBWARP_SPEED_HIGH, 1));
    CHECK(!usbwarp_iso_packet_count_valid(7, USBWARP_SPEED_HIGH, 1));
    CHECK(usbwarp_iso_packet_count_valid(4, USBWARP_SPEED_HIGH, 2));
    CHECK(usbwarp_iso_packet_count_valid(2, USBWARP_SPEED_HIGH, 4));
    CHECK(usbwarp_iso_packet_count_valid(1, USBWARP_SPEED_HIGH, 8));
    for (i = USBWARP_SPEED_SUPER; i <= USBWARP_SPEED_MAX; ++i) {
        CHECK(usbwarp_iso_packet_count_valid(1024, i, 1));
        CHECK(!usbwarp_iso_packet_count_valid(7, i, 1));
        CHECK(usbwarp_iso_packet_count_valid(1, i, 8));
    }
    CHECK(!usbwarp_iso_packet_count_valid(8, USBWARP_SPEED_MAX + 1, 1));
    c = calloc(1, USBWARP_MAX_MESSAGE_SIZE);
    assert(c);
    c->hdr.message_length = USBWARP_MAX_MESSAGE_SIZE;
    c->device_id = c->hdr.device_id = 1;
    c->endpoint = 1;
    c->number_of_packets = 1024;
    c->data_mode = USBWARP_DATA_BUFFER;
    CHECK(usbwarp_iso_complete_valid(c, USBWARP_MAX_MESSAGE_SIZE));
    for (i = 0; i < USBWARP_MAX_MESSAGE_SIZE; ++i)
        CHECK(!usbwarp_iso_complete_valid(c, i));
    c->number_of_packets = UINT32_MAX;
    CHECK(!usbwarp_iso_complete_valid(c, USBWARP_MAX_MESSAGE_SIZE));
    c->number_of_packets = 0;
    c->hdr.message_length = 64;
    CHECK(!usbwarp_iso_complete_valid(c, 64));
    c->status = USBWARP_STATUS_NO_RESOURCE;
    c->data_mode = USBWARP_DATA_NONE;
    CHECK(usbwarp_iso_complete_valid(c, 64));
    c->data_mode = USBWARP_DATA_INLINE;
    CHECK(!usbwarp_iso_complete_valid(c, 64));
    free(c);
    printf("ISO protocol: %u checks passed\n", checks);
    return 0;
}
