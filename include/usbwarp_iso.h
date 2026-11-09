/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USBWARP_ISO_H
#define USBWARP_ISO_H

#include "usbwarp_shared.h"

/* Trailing arrays use accessors to keep the same ABI in C, C++ and WDK C. */
static inline struct usbwarp_iso_packet_desc *
usbwarp_iso_packets(struct usbwarp_msg_iso_submit *msg)
{
    return (struct usbwarp_iso_packet_desc *)(msg + 1);
}

static inline const struct usbwarp_iso_packet_desc *
usbwarp_iso_packets_const(const struct usbwarp_msg_iso_submit *msg)
{
    return (const struct usbwarp_iso_packet_desc *)(msg + 1);
}

static inline struct usbwarp_iso_packet_result *
usbwarp_iso_results(struct usbwarp_msg_iso_complete *msg)
{
    return (struct usbwarp_iso_packet_result *)(msg + 1);
}

static inline const struct usbwarp_iso_packet_result *
usbwarp_iso_results_const(const struct usbwarp_msg_iso_complete *msg)
{
    return (const struct usbwarp_iso_packet_result *)(msg + 1);
}

static inline uint32_t usbwarp_iso_message_size(uint32_t packets)
{
    return USBWARP_MSG_ISO_SUBMIT_BASE_SIZE + packets * 8u;
}

/* Packed offsets are essential: Windows derives OUT packet lengths from
 * adjacent offsets, ignoring IsoPacket[].Length. Linux buffer gaps must not
 * become USB payload. Use subtraction to avoid integer overflow. */
static inline int usbwarp_iso_layout_valid(
    const struct usbwarp_iso_packet_desc *packets, uint32_t count,
    uint32_t total_length)
{
    uint32_t i, end = 0;
    if (!count || count > USBWARP_ISO_MAX_PACKETS ||
        total_length > USBWARP_MAX_TRANSFER_SIZE)
        return 0;
    for (i = 0; i < count; ++i) {
        if (packets[i].offset != end || packets[i].length > total_length - end)
            return 0;
        end += packets[i].length;
    }
    return end == total_length;
}

static inline int usbwarp_iso_submit_valid(
    const struct usbwarp_msg_iso_submit *msg, uint32_t bytes)
{
    if (bytes < USBWARP_MSG_ISO_SUBMIT_BASE_SIZE ||
        !msg->number_of_packets || msg->number_of_packets > USBWARP_ISO_MAX_PACKETS ||
        bytes != usbwarp_iso_message_size(msg->number_of_packets) ||
        msg->hdr.message_length != bytes || msg->device_id != msg->hdr.device_id ||
        !msg->device_id || msg->device_id > USBWARP_MAX_DEVICES_LIMIT ||
        msg->endpoint == 0 || msg->endpoint > 15 ||
        msg->direction > 1 || msg->start_frame != 0 ||
        !USBWARP_IS_POWER_OF_2(msg->interval) || msg->interval > 32768)
        return 0;
    return usbwarp_iso_layout_valid(usbwarp_iso_packets_const(msg),
                                   msg->number_of_packets, msg->total_length);
}

static inline int usbwarp_iso_complete_valid(
    const struct usbwarp_msg_iso_complete *msg, uint32_t bytes)
{
    if (bytes < USBWARP_MSG_ISO_COMPLETE_BASE_SIZE ||
        msg->number_of_packets > USBWARP_ISO_MAX_PACKETS ||
        bytes != usbwarp_iso_message_size(msg->number_of_packets) ||
        msg->hdr.message_length != bytes || msg->device_id != msg->hdr.device_id ||
        !msg->device_id || msg->device_id > USBWARP_MAX_DEVICES_LIMIT ||
        msg->endpoint == 0 || msg->endpoint > 15 ||
        (msg->data_mode != USBWARP_DATA_NONE && msg->data_mode != USBWARP_DATA_BUFFER))
        return 0;
    /* A zero-result response is an error before the host submitted USB I/O. */
    return msg->number_of_packets != 0 || msg->status != USBWARP_STATUS_SUCCESS;
}

/* Windows limits full-speed URBs to 255 packets. High-speed URBs must span
 * whole frames (eight microframes), accounting for the endpoint interval. */
static inline int usbwarp_iso_packet_count_valid(uint32_t count,
                                               uint32_t speed, uint32_t interval)
{
    uint32_t per_frame;
    if (!count || !USBWARP_IS_POWER_OF_2(interval) || interval > 32768)
        return 0;
    if (speed == USBWARP_SPEED_FULL)
        return count <= 255 && interval == 1;
    if (speed != USBWARP_SPEED_HIGH && !usbwarp_speed_is_super(speed))
        return 0;
    per_frame = interval < 8 ? 8 / interval : 1;
    return count <= USBWARP_ISO_MAX_PACKETS && count % per_frame == 0;
}

#endif
