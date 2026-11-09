// SPDX-License-Identifier: GPL-2.0
#include "usbwarp_guest.h"
#include <linux/io.h>

/* Copy an arbitrary URB range without treating MMIO as ordinary RAM. This
 * handles storage/UAS scatterlists and ISO packet offsets in atomic context. */
size_t usbwarp_copy_urb_data(struct urb *urb, u32 offset, void *memory,
                           void __iomem *mmio, u32 length, bool to_urb)
{
    struct sg_mapping_iter iter;
    size_t copied = 0;
    if (!length) return 0;
    if (!urb->num_sgs) {
        void *data = urb->transfer_buffer + offset;
        if (mmio) {
            if (to_urb) memcpy_fromio(data, mmio, length);
            else memcpy_toio(mmio, data, length);
        } else if (to_urb) memcpy(data, memory, length);
        else memcpy(memory, data, length);
        return length;
    }
    sg_miter_start(&iter, urb->sg, urb->num_sgs,
        SG_MITER_ATOMIC | (to_urb ? SG_MITER_TO_SG : SG_MITER_FROM_SG));
    if (!sg_miter_skip(&iter, offset)) goto done;
    while (copied < length && sg_miter_next(&iter)) {
        size_t n = min_t(size_t, iter.length, length - copied);
        if (mmio) {
            if (to_urb) memcpy_fromio(iter.addr, mmio + copied, n);
            else memcpy_toio(mmio + copied, iter.addr, n);
        } else if (to_urb) memcpy(iter.addr, memory + copied, n);
        else memcpy(memory + copied, iter.addr, n);
        iter.consumed = n;
        copied += n;
    }
done:
    sg_miter_stop(&iter);
    return copied;
}
