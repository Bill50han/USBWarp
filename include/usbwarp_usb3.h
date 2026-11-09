/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USBWARP_USB3_H
#define USBWARP_USB3_H
#include "usbwarp_shared.h"

/* A USB3 hub has at most 15 ports. Device IDs are independent of port IDs. */
#define USBWARP_USB2_PORTS 31u
#define USBWARP_USB3_PORTS 15u

/* Decode the Windows hub's current SSP lane attributes (not BOS capabilities).
 * Lane counts are zero-based. USB4 rates/asymmetric links aren't guessed. */
static inline uint32_t usbwarp_usb3_decode_link(uint32_t rx, uint32_t rx_lanes,
                                               uint32_t tx, uint32_t tx_lanes)
{
    uint32_t rm = rx >> 16, tm = tx >> 16;
    uint32_t re = (rx >> 4) & 3u, te = (tx >> 4) & 3u;
    if (rx_lanes > 1 || tx_lanes != rx_lanes) return 0;
    if (((rx >> 14) & 3u) > 1 || ((tx >> 14) & 3u) > 1 ||
        (rx & 0x40u) || (tx & 0x40u)) return 0;
    while (re++ < 3) rm /= 1000;
    while (te++ < 3) tm /= 1000;
    if (rm != tm) return 0;
    if (rm == 5) return rx_lanes ? USBWARP_SPEED_SUPER_GEN1_2X : USBWARP_SPEED_SUPER;
    if (rm == 10) return rx_lanes ? USBWARP_SPEED_SUPER_PLUS_2X : USBWARP_SPEED_SUPER_PLUS;
    return 0;
}

/* Speed ID 0: SS 5G; 1: SSP 10G/lane; 2: SSP 5G/lane. Port lane fields
 * encode count minus one, as specified by USB3.2 extended GET_PORT_STATUS. */
static inline uint32_t usbwarp_usb3_port_speed(uint32_t speed)
{
    switch (speed) {
    case USBWARP_SPEED_SUPER_PLUS: return 0x0011;
    case USBWARP_SPEED_SUPER_PLUS_2X: return 0x1111;
    case USBWARP_SPEED_SUPER_GEN1_2X: return 0x1122;
    default: return 0;
    }
}

/* Root BOS: SS plus SSP capabilities, 3 speed IDs / 6 RX+TX attributes.
 * The virtual root can represent every USB3.x link mode through Gen 2x2. */
static const uint8_t usbwarp_usb3_bos[] = {
    5, 15, 51, 0, 2,
    10, 16, 3, 0, 8, 0, 3, 0, 0, 0,
    36, 16, 10, 0, 0x45, 0, 0, 0, 0, 0x11, 0, 0,
    0x30, 0x00, 5, 0, 0xb0, 0x00, 5, 0,
    0x31, 0x40, 10, 0, 0xb1, 0x40, 10, 0,
    0x32, 0x40, 5, 0, 0xb2, 0x40, 5, 0,
};
USBWARP_STATIC_ASSERT(sizeof(usbwarp_usb3_bos) == 51, "USB3.2 BOS size");

static inline uint32_t usbwarp_endpoint_mask(uint32_t address)
{
    uint32_t ep = address & 15u;
    if (!ep || (address & ~0x8fu)) return 0;
    return 1u << (ep + ((address & 0x80u) ? 16u : 0u));
}
static inline int usbwarp_stream_request_valid(uint32_t mask, uint32_t count)
{
    return mask && !(mask & 0x00010001u) && count <= USBWARP_MAX_STREAMS;
}
#endif
