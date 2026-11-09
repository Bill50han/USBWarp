// SPDX-License-Identifier: GPL-2.0
/* USB2 and USB3.2 virtual root hubs. Port-change bits follow USB hub semantics. */
#include "usbwarp_guest.h"
#include <linux/usb/ch9.h>
#include <linux/usb/ch11.h>
#include <asm/unaligned.h>

int usbwarp_hub_status_data(struct usb_hcd *hcd, char *buf)
{
    struct usbwarp_hcd *w = hcd_to_warp(hcd);
    struct usbwarp_roothub *rh = hcd_to_rh(hcd);
    unsigned long flags;
    unsigned int i, bytes = DIV_ROUND_UP(rh->num_ports + 1, 8);
    bool changed = false;
    memset(buf, 0, bytes);
    spin_lock_irqsave(&w->dev_lock, flags);
    for (i = 0; i < rh->num_ports; ++i) {
        /* Change bits remain asserted until ClearPortFeature acknowledges them. */
        if (rh->port_status[i] >> 16) {
            buf[(i + 1) / 8] |= 1u << ((i + 1) % 8);
            changed = true;
        }
    }
    spin_unlock_irqrestore(&w->dev_lock, flags);
    if (changed && hcd->state == HC_STATE_SUSPENDED)
        usb_hcd_resume_root_hub(hcd);
    return changed ? bytes : 0;
}

int usbwarp_hub_control(struct usb_hcd *hcd, u16 request, u16 value,
                               u16 index, char *buf, u16 length)
{
    struct usbwarp_hcd *w = hcd_to_warp(hcd);
    struct usbwarp_roothub *rh = hcd_to_rh(hcd);
    bool ss = usbwarp_is_usb3(hcd), changed = false;
    unsigned int port = (index & 0xff) - 1;
    unsigned long flags;
    u32 *status, mask, ext = 0, id;
    int ret = 0;
    if (request == GetHubDescriptor) {
        struct usb_hub_descriptor desc = { 0 };
        unsigned int size;
        if (value != (ss ? USB_DT_SS_HUB : USB_DT_HUB) << 8) return -EPIPE;
        desc.bNbrPorts = rh->num_ports;
        desc.wHubCharacteristics = cpu_to_le16(HUB_CHAR_INDV_PORT_LPSM | HUB_CHAR_NO_OCPM);
        desc.bPwrOn2PwrGood = 1;
        if (ss) {
            if (value != USB_DT_SS_HUB << 8) return -EPIPE;
            desc.bDescriptorType = USB_DT_SS_HUB;
            size = USB_DT_SS_HUB_SIZE;
            desc.u.ss.DeviceRemovable = 0;
        } else {
            unsigned int width = DIV_ROUND_UP(rh->num_ports + 1, 8);
            desc.bDescriptorType = USB_DT_HUB;
            size = USB_DT_HUB_NONVAR_SIZE + 2 * width;
            memset(desc.u.hs.DeviceRemovable + width, 0xff, width);
        }
        desc.bDescLength = size;
        size = min_t(unsigned int, length, size);
        memcpy(buf, &desc, size);
        return size;
    }
    if (request == (DeviceRequest | USB_REQ_GET_DESCRIPTOR)) {
        if (!ss || value >> 8 != USB_DT_BOS) return -EPIPE;
        ret = min_t(unsigned int, length, sizeof(usbwarp_usb3_bos));
        memcpy(buf, usbwarp_usb3_bos, ret);
        return ret;
    }
    if (request == GetHubStatus) {
        if (length < 4) return -EPIPE;
        memset(buf, 0, 4);
        return 4;
    }
    if (request == ClearHubFeature || request == SetHubFeature)
        return value <= C_HUB_OVER_CURRENT ? 0 : -EPIPE;
    if (request == SetHubDepth)
        return ss && value == 0 ? 0 : -EPIPE;
    if (port >= rh->num_ports) return -EPIPE;
    spin_lock_irqsave(&w->dev_lock, flags);
    status = &rh->port_status[port];
    switch (request) {
    case GetPortStatus:
        if (length < 4 || (value && (!ss || value != HUB_EXT_PORT_STATUS || length < 8))) {
            ret = -EPIPE; break;
        }
        put_unaligned_le32(*status, buf);
        ret = 4;
        if (ss && value == HUB_EXT_PORT_STATUS) {
            id = rh->device_id[port];
            if (id) ext = usbwarp_usb3_port_speed(w->devices[id - 1].speed);
            put_unaligned_le32(ext, buf + 4);
            ret = 8;
        }
        break;
    case GetPortErrorCount:
        if (!ss || length < 2) { ret = -EPIPE; break; }
        put_unaligned_le16(0, buf);
        ret = 2;
        break;
    case SetPortFeature:
        switch (value) {
        case USB_PORT_FEAT_POWER:
            *status |= ss ? USB_SS_PORT_STAT_POWER : USB_PORT_STAT_POWER;
            break;
        case USB_PORT_FEAT_BH_PORT_RESET:
            if (!ss) { ret = -EPIPE; break; }
            *status |= USB_PORT_STAT_C_BH_RESET << 16;
            fallthrough;
        case USB_PORT_FEAT_RESET:
            if (!(*status & USB_PORT_STAT_CONNECTION)) break;
            *status &= ~USB_PORT_STAT_RESET;
            *status |= USB_PORT_STAT_ENABLE;
            if (value != USB_PORT_FEAT_BH_PORT_RESET)
                *status |= USB_PORT_STAT_C_RESET << 16;
            if (ss) *status &= ~USB_PORT_STAT_LINK_STATE;
            changed = true;
            break;
        case USB_PORT_FEAT_SUSPEND:
            if (ss) ret = -EPIPE;
            else *status |= USB_PORT_STAT_SUSPEND;
            break;
        case USB_PORT_FEAT_LINK_STATE:
            if (!ss || (index >> 8) > 15) { ret = -EPIPE; break; }
            *status = (*status & ~USB_PORT_STAT_LINK_STATE) | ((index >> 8) << 5);
            *status |= USB_PORT_STAT_C_LINK_STATE << 16;
            changed = true;
            break;
        case USB_PORT_FEAT_U1_TIMEOUT:
        case USB_PORT_FEAT_U2_TIMEOUT:
        case USB_PORT_FEAT_REMOTE_WAKE_MASK:
            if (!ss) ret = -EPIPE;
            break; /* Virtual link policy; Windows owns physical link power. */
        default: ret = -EPIPE; break;
        }
        break;
    case ClearPortFeature:
        mask = 0;
        switch (value) {
        case USB_PORT_FEAT_C_CONNECTION: mask = USB_PORT_STAT_C_CONNECTION; break;
        case USB_PORT_FEAT_C_ENABLE: mask = USB_PORT_STAT_C_ENABLE; break;
        case USB_PORT_FEAT_C_SUSPEND: mask = USB_PORT_STAT_C_SUSPEND; break;
        case USB_PORT_FEAT_C_OVER_CURRENT: mask = USB_PORT_STAT_C_OVERCURRENT; break;
        case USB_PORT_FEAT_C_RESET: mask = USB_PORT_STAT_C_RESET; break;
        case USB_PORT_FEAT_C_BH_PORT_RESET: mask = USB_PORT_STAT_C_BH_RESET; break;
        case USB_PORT_FEAT_C_PORT_LINK_STATE: mask = USB_PORT_STAT_C_LINK_STATE; break;
        case USB_PORT_FEAT_C_PORT_CONFIG_ERROR: mask = USB_PORT_STAT_C_CONFIG_ERROR; break;
        case USB_PORT_FEAT_ENABLE: *status &= ~USB_PORT_STAT_ENABLE; break;
        case USB_PORT_FEAT_POWER:
            *status &= ~(ss ? USB_SS_PORT_STAT_POWER : USB_PORT_STAT_POWER); break;
        case USB_PORT_FEAT_SUSPEND:
            if (ss) { ret = -EPIPE; break; }
            *status &= ~USB_PORT_STAT_SUSPEND;
            *status |= USB_PORT_STAT_C_SUSPEND << 16;
            changed = true;
            break;
        default: ret = -EPIPE; break;
        }
        *status &= ~(mask << 16);
        break;
    default: ret = -EPIPE; break;
    }
    spin_unlock_irqrestore(&w->dev_lock, flags);
    if (changed) usb_hcd_poll_rh_status(hcd);
    return ret;
}
