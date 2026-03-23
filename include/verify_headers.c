/*
 * verify_headers.c — Compile-time layout verification for UsbWarp headers.
 *
 * Build:  gcc -std=c11 -Wall -Wextra -Werror -I../include -c verify_headers.c
 *         (zero warnings = pass)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "usbwarp_shared.h"
#include "usbwarp_ioctl.h"
#include "usbwarp_pipe.h"

/* ── Additional compile-time checks not in the headers ────────────────────── */

/* Control Block must occupy exactly 1 page */
USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_control_block) == USBWARP_CONTROL_BLOCK_SIZE, "");

/* Ring header must be exactly 3 cache lines */
USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_ring_header) == 3 * USBWARP_CACHELINE_SIZE, "");

/* Message header fits in half a cache line */
USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_msg_header) == USBWARP_MSG_HEADER_SIZE, "");

/* URB messages are exactly 3 cache lines */
USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_msg_urb_submit)   == 3 * USBWARP_CACHELINE_SIZE, "");
USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_msg_urb_complete)  == 3 * USBWARP_CACHELINE_SIZE, "");

/* URB base size (without inline) is 96 */
USBWARP_STATIC_ASSERT(USBWARP_MSG_URB_SUBMIT_BASE_SIZE   == 96, "");
USBWARP_STATIC_ASSERT(USBWARP_MSG_URB_COMPLETE_BASE_SIZE  == 96, "");

/* Inline data starts at exactly byte 96 of the message (= base size) */
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_submit,  inline_data) ==
               USBWARP_MSG_URB_SUBMIT_BASE_SIZE, "");
USBWARP_STATIC_ASSERT(offsetof(struct usbwarp_msg_urb_complete, inline_data) ==
               USBWARP_MSG_URB_COMPLETE_BASE_SIZE, "");

/* Alignment macro round-trip */
USBWARP_STATIC_ASSERT(USBWARP_ALIGN_CACHELINE(1)   == 64,  "");
USBWARP_STATIC_ASSERT(USBWARP_ALIGN_CACHELINE(64)  == 64,  "");
USBWARP_STATIC_ASSERT(USBWARP_ALIGN_CACHELINE(65)  == 128, "");
USBWARP_STATIC_ASSERT(USBWARP_ALIGN_CACHELINE(180) == 192, "");
USBWARP_STATIC_ASSERT(USBWARP_ALIGN_CACHELINE(192) == 192, "");

/* Control messages fit in 1 cache-line ring slot */
USBWARP_STATIC_ASSERT(USBWARP_ALIGN_CACHELINE(sizeof(struct usbwarp_msg_negotiate))       == 64, "");
USBWARP_STATIC_ASSERT(USBWARP_ALIGN_CACHELINE(sizeof(struct usbwarp_msg_negotiate_resp))  == 64, "");
USBWARP_STATIC_ASSERT(USBWARP_ALIGN_CACHELINE(sizeof(struct usbwarp_msg_heartbeat))       == 64, "");
USBWARP_STATIC_ASSERT(USBWARP_ALIGN_CACHELINE(sizeof(struct usbwarp_msg_shutdown))        == 64, "");
USBWARP_STATIC_ASSERT(USBWARP_ALIGN_CACHELINE(sizeof(struct usbwarp_msg_urb_cancel))      == 64, "");
USBWARP_STATIC_ASSERT(USBWARP_ALIGN_CACHELINE(sizeof(struct usbwarp_msg_urb_cancel_ack))  == 64, "");
USBWARP_STATIC_ASSERT(USBWARP_ALIGN_CACHELINE(sizeof(struct usbwarp_msg_circuit_breaker)) == 64, "");

/* Pipe header */
USBWARP_STATIC_ASSERT(sizeof(struct usbwarp_pipe_header) == 16, "");

/* Power-of-2 check macro */
USBWARP_STATIC_ASSERT(USBWARP_IS_POWER_OF_2(64),    "");
USBWARP_STATIC_ASSERT(USBWARP_IS_POWER_OF_2(4096),  "");
USBWARP_STATIC_ASSERT(!USBWARP_IS_POWER_OF_2(0),    "");
USBWARP_STATIC_ASSERT(!USBWARP_IS_POWER_OF_2(3),    "");
USBWARP_STATIC_ASSERT(!USBWARP_IS_POWER_OF_2(100),  "");

/* Version macros */
USBWARP_STATIC_ASSERT(USBWARP_PROTOCOL_VERSION          == 0x0100, "");
USBWARP_STATIC_ASSERT(USBWARP_VERSION_MAJOR(0x0100)     == 1,      "");
USBWARP_STATIC_ASSERT(USBWARP_VERSION_MINOR(0x0100)     == 0,      "");
USBWARP_STATIC_ASSERT(USBWARP_VERSION_MAJOR(0x0203)     == 2,      "");
USBWARP_STATIC_ASSERT(USBWARP_VERSION_MINOR(0x0203)     == 3,      "");

/* ── Runtime pretty-print (optional, helps during review) ─────────────────── */

int main(void)
{
    printf("UsbWarp header verification — all static assertions passed.\n\n");

#define SHOW(type) \
    printf("  %-44s  %4zu bytes   ring slot %3u\n", \
           #type, sizeof(type), USBWARP_ALIGN_CACHELINE((unsigned)sizeof(type)))

    printf("Shared-memory structures:\n");
    SHOW(struct usbwarp_control_block);
    SHOW(struct usbwarp_ring_header);
    printf("\n");

    printf("Ring messages:\n");
    SHOW(struct usbwarp_msg_header);
    SHOW(struct usbwarp_msg_negotiate);
    SHOW(struct usbwarp_msg_negotiate_resp);
    SHOW(struct usbwarp_msg_device_added);
    SHOW(struct usbwarp_msg_device_removed);
    SHOW(struct usbwarp_msg_device_ack);
    SHOW(struct usbwarp_msg_heartbeat);
    SHOW(struct usbwarp_msg_shutdown);
    SHOW(struct usbwarp_msg_shutdown_ack);
    SHOW(struct usbwarp_msg_urb_submit);
    SHOW(struct usbwarp_msg_urb_complete);
    SHOW(struct usbwarp_msg_urb_cancel);
    SHOW(struct usbwarp_msg_urb_cancel_ack);
    SHOW(struct usbwarp_msg_iso_submit);
    SHOW(struct usbwarp_msg_iso_complete);
    SHOW(struct usbwarp_msg_circuit_breaker);
    SHOW(struct usbwarp_msg_stats_request);
    SHOW(struct usbwarp_msg_stats_response);
    printf("\n");

    printf("IOCTL structures:\n");
    SHOW(USBWARP_REGISTER_SERVICE_IN);
    SHOW(USBWARP_REGISTER_SERVICE_OUT);
    SHOW(USBWARP_SETUP_SHM_IN);
    SHOW(USBWARP_SETUP_SHM_OUT);
    SHOW(USBWARP_TEARDOWN_SHM_IN);
    SHOW(USBWARP_BIND_DEVICE_IN);
    SHOW(USBWARP_BIND_DEVICE_OUT);
    SHOW(USBWARP_UNBIND_DEVICE_IN);
    SHOW(USBWARP_HEARTBEAT_IN);
    SHOW(USBWARP_HEARTBEAT_OUT);
    SHOW(USBWARP_QUERY_STATUS_OUT);
    printf("\n");

    printf("Pipe structures:\n");
    SHOW(struct usbwarp_pipe_header);
    SHOW(struct usbwarp_pipe_response);
    SHOW(struct usbwarp_pipe_device_entry);
    SHOW(struct usbwarp_pipe_device_list);
    SHOW(struct usbwarp_pipe_bind_request);
    SHOW(struct usbwarp_pipe_status);
    SHOW(struct usbwarp_pipe_stats_request);
    SHOW(struct usbwarp_pipe_stats_response);
    SHOW(struct usbwarp_pipe_query_device_request);
    SHOW(struct usbwarp_pipe_query_device_response);

#undef SHOW
    return 0;
}
