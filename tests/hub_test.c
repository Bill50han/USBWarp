/* SPDX-License-Identifier: GPL-2.0-only
 * Compile the production root-hub implementation against Linux UAPI USB
 * descriptors, with host-controller locks/notifications replaced by test hooks.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/usb/ch9.h>
#include <linux/usb/ch11.h>
#include "usbwarp_usb3.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define USBWARP_GUEST_H
#define USBWARP_MAX_PORTS USBWARP_USB2_PORTS
#define HC_STATE_RUNNING 1
#define HC_STATE_SUSPENDED 2
#define DIV_ROUND_UP(a,b) (((a) + (b) - 1) / (b))
#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define cpu_to_le16(x) ((uint16_t)(x))
#define spin_lock_irqsave(p, f) do { (void)(p); (f) = 0; } while (0)
#define spin_unlock_irqrestore(p, f) do { (void)(p); (void)(f); } while (0)
#define fallthrough __attribute__((fallthrough))
#define DeviceRequest ((USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE) << 8)
#define HUB_REQ(d,t,r) ((((d) | (t)) << 8) | (r))
#define ClearHubFeature HUB_REQ(USB_DIR_OUT, USB_RT_HUB, USB_REQ_CLEAR_FEATURE)
#define ClearPortFeature HUB_REQ(USB_DIR_OUT, USB_RT_PORT, USB_REQ_CLEAR_FEATURE)
#define GetHubDescriptor HUB_REQ(USB_DIR_IN, USB_RT_HUB, USB_REQ_GET_DESCRIPTOR)
#define GetHubStatus HUB_REQ(USB_DIR_IN, USB_RT_HUB, USB_REQ_GET_STATUS)
#define GetPortStatus HUB_REQ(USB_DIR_IN, USB_RT_PORT, USB_REQ_GET_STATUS)
#define SetHubFeature HUB_REQ(USB_DIR_OUT, USB_RT_HUB, USB_REQ_SET_FEATURE)
#define SetPortFeature HUB_REQ(USB_DIR_OUT, USB_RT_PORT, USB_REQ_SET_FEATURE)
#define SetHubDepth HUB_REQ(USB_DIR_OUT, USB_RT_HUB, HUB_SET_DEPTH)
#define GetPortErrorCount HUB_REQ(USB_DIR_IN, USB_RT_PORT, HUB_GET_PORT_ERR_COUNT)
struct usbwarp_hcd { int dev_lock; struct { u8 speed; } devices[32]; };
struct usbwarp_roothub { u32 port_status[31], device_id[31]; unsigned num_ports; };
struct usb_hcd { struct usbwarp_hcd *w; struct usbwarp_roothub *rh; bool ss; int state; unsigned polls; };
static struct usbwarp_hcd *hcd_to_warp(struct usb_hcd *h) { return h->w; }
static struct usbwarp_roothub *hcd_to_rh(struct usb_hcd *h) { return h->rh; }
static bool usbwarp_is_usb3(struct usb_hcd *h) { return h->ss; }
static void usb_hcd_poll_rh_status(struct usb_hcd *h) { ++h->polls; }
static void usb_hcd_resume_root_hub(struct usb_hcd *h) { h->state = HC_STATE_RUNNING; }
static void put_unaligned_le16(uint16_t v, void *p) { uint8_t *b = p; b[0] = v; b[1] = v >> 8; }
static void put_unaligned_le32(uint32_t v, void *p) { uint8_t *b = p; for (unsigned i=0;i<4;++i) b[i] = v >> (8*i); }
static uint32_t le32(const void *p) { const uint8_t *b=p; return b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24); }
#include "../usbwarp_module/usbwarp_hub.c"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)

int main(void)
{
    struct usbwarp_hcd w = {0};
    struct usbwarp_roothub usb2 = {.num_ports=31}, usb3 = {.num_ports=15};
    struct usb_hcd h2 = {&w, &usb2, false, HC_STATE_RUNNING, 0};
    struct usb_hcd h3 = {&w, &usb3, true, HC_STATE_RUNNING, 0};
    unsigned char data[128];
    char changes[4];
    unsigned pos, descriptors, speeds;
    /* Both descriptor types, including short header reads and guard bytes. */
    for (unsigned n=0;n<=64;++n) {
        memset(data,0xa5,sizeof(data));
        CHECK(usbwarp_hub_control(&h2,GetHubDescriptor,USB_DT_HUB<<8,0,(char *)data,n)==(int)(n<15?n:15));
        if(n) CHECK(data[0]==15);
        for(unsigned i=n<15?n:15;i<sizeof(data);++i) CHECK(data[i]==0xa5);
        memset(data,0xa5,sizeof(data));
        CHECK(usbwarp_hub_control(&h3,GetHubDescriptor,USB_DT_SS_HUB<<8,0,(char *)data,n)==(int)(n<12?n:12));
        if(n) CHECK(data[0]==12);
        for(unsigned i=n<12?n:12;i<sizeof(data);++i) CHECK(data[i]==0xa5);
        memset(data,0xa5,sizeof(data));
        CHECK(usbwarp_hub_control(&h3,DeviceRequest|USB_REQ_GET_DESCRIPTOR,USB_DT_BOS<<8,0,
                                 (char *)data,n)==(int)(n<51?n:51));
        for(unsigned i=n<51?n:51;i<sizeof(data);++i) CHECK(data[i]==0xa5);
    }
    CHECK(usbwarp_usb3_bos[2]==sizeof(usbwarp_usb3_bos));
    pos=5; descriptors=0; speeds=0;
    while(pos<sizeof(usbwarp_usb3_bos)) {
        unsigned len=usbwarp_usb3_bos[pos];
        CHECK(len>=3 && pos+len<=sizeof(usbwarp_usb3_bos));
        CHECK(usbwarp_usb3_bos[pos+1]==USB_DT_DEVICE_CAPABILITY);
        if(usbwarp_usb3_bos[pos+2]==USB_SSP_CAP_TYPE) {
            unsigned entries=(le32(usbwarp_usb3_bos+pos+4)&31)+1;
            CHECK(len==12+4*entries);
            for(unsigned i=0;i<entries;++i) {
                u32 attr=le32(usbwarp_usb3_bos+pos+12+i*4);
                CHECK(((attr>>4)&3)==3);
                CHECK((attr>>16)==5 || (attr>>16)==10);
                speeds|=1u<<(attr&15);
            }
        }
        pos+=len; ++descriptors;
    }
    CHECK(descriptors==usbwarp_usb3_bos[4] && speeds==7);

    /* Same port number on the two root hubs routes independently. */
    usb2.device_id[0]=32; usb3.device_id[0]=1;
    usb2.port_status[0]=USB_PORT_STAT_CONNECTION|USB_PORT_STAT_POWER|(USB_PORT_STAT_C_CONNECTION<<16);
    usb3.port_status[0]=USB_PORT_STAT_CONNECTION|USB_PORT_STAT_ENABLE|USB_SS_PORT_STAT_POWER|(USB_PORT_STAT_C_CONNECTION<<16);
    for(unsigned speed=USBWARP_SPEED_SUPER;speed<=USBWARP_SPEED_MAX;++speed) {
        w.devices[0].speed=speed;
        CHECK(usbwarp_hub_control(&h3,GetPortStatus,HUB_EXT_PORT_STATUS,1,(char *)data,8)==8);
        u32 ext=le32(data+4), id=ext&15, lane=(ext>>8)&15;
        CHECK(id==((speed==USBWARP_SPEED_SUPER)?0:(speed==USBWARP_SPEED_SUPER_GEN1_2X)?2:1));
        CHECK(lane==((speed==USBWARP_SPEED_SUPER_PLUS_2X||speed==USBWARP_SPEED_SUPER_GEN1_2X)?1:0));
        CHECK(((ext>>4)&15)==id && ((ext>>12)&15)==lane);
    }
    CHECK(usbwarp_hub_status_data(&h3,changes)==2 && (changes[0]&2));
    CHECK(usbwarp_hub_status_data(&h3,changes)==2); /* not cleared by status polling */
    CHECK(!usbwarp_hub_control(&h3,ClearPortFeature,USB_PORT_FEAT_C_CONNECTION,1,(char *)data,0));
    CHECK(usbwarp_hub_status_data(&h3,changes)==0);
    CHECK(usbwarp_hub_status_data(&h2,changes)==4); /* sibling unchanged */
    CHECK(!usbwarp_hub_control(&h3,SetPortFeature,USB_PORT_FEAT_BH_PORT_RESET,1,(char *)data,0));
    CHECK(usb3.port_status[0]&(USB_PORT_STAT_C_BH_RESET<<16));
    CHECK(!(usb3.port_status[0]&(USB_PORT_STAT_C_RESET<<16)));
    CHECK(!usbwarp_hub_control(&h3,ClearPortFeature,USB_PORT_FEAT_C_BH_PORT_RESET,1,(char *)data,0));
    CHECK(!(usb3.port_status[0]&(USB_PORT_STAT_C_BH_RESET<<16)));
    CHECK(!usbwarp_hub_control(&h3,SetPortFeature,USB_PORT_FEAT_LINK_STATE,(3<<8)|1,(char *)data,0));
    CHECK((usb3.port_status[0]&USB_PORT_STAT_LINK_STATE)==USB_SS_PORT_LS_U3);
    CHECK(!usbwarp_hub_control(&h3,SetPortFeature,USB_PORT_FEAT_LINK_STATE,1,(char *)data,0));
    CHECK((usb3.port_status[0]&USB_PORT_STAT_LINK_STATE)==USB_SS_PORT_LS_U0);
    CHECK(usbwarp_hub_control(&h2,SetPortFeature,USB_PORT_FEAT_BH_PORT_RESET,1,(char *)data,0)==-EPIPE);
    CHECK(usbwarp_hub_control(&h3,SetPortFeature,USB_PORT_FEAT_SUSPEND,1,(char *)data,0)==-EPIPE);
    for(unsigned index=0;index<256;++index) {
        int result=usbwarp_hub_control(&h3,GetPortStatus,0,index,(char *)data,4);
        CHECK(result==((index>=1&&index<=15)?4:-EPIPE));
    }
    for(unsigned n=0;n<4;++n)
        CHECK(usbwarp_hub_control(&h3,GetPortStatus,0,1,(char *)data,n)==-EPIPE);
    CHECK(usbwarp_hub_control(&h2,GetPortStatus,HUB_EXT_PORT_STATUS,1,(char *)data,8)==-EPIPE);
    CHECK(usbwarp_hub_control(&h3,GetPortErrorCount,0,1,(char *)data,2)==2);
    CHECK(usbwarp_hub_control(&h3,GetHubStatus,0,0,(char *)data,4)==4);
    CHECK(usbwarp_hub_control(&h2,GetHubDescriptor,USB_DT_SS_HUB<<8,0,(char *)data,12)==-EPIPE);
    CHECK(!usbwarp_hub_control(&h3,SetHubDepth,0,0,(char *)data,0));

    CHECK(usbwarp_usb3_decode_link(5u<<16|0x30,0,5u<<16|0xb0,0)==USBWARP_SPEED_SUPER);
    CHECK(usbwarp_usb3_decode_link(10u<<16|0x4030,0,10u<<16|0x40b0,0)==USBWARP_SPEED_SUPER_PLUS);
    CHECK(usbwarp_usb3_decode_link(10u<<16|0x4030,1,10u<<16|0x40b0,1)==USBWARP_SPEED_SUPER_PLUS_2X);
    CHECK(usbwarp_usb3_decode_link(5000u<<16|0x4020,1,5000u<<16|0x40a0,1)==USBWARP_SPEED_SUPER_GEN1_2X);
    CHECK(!usbwarp_usb3_decode_link(20u<<16|0x4030,0,20u<<16|0x40b0,0)); /* USB4 excluded */
    CHECK(!usbwarp_usb3_decode_link(10u<<16|0x4030,0,10u<<16|0x40b0,1));
    CHECK(usbwarp_endpoint_mask(0x81)==(1u<<17));
    CHECK(usbwarp_endpoint_mask(0x0f)==(1u<<15));
    CHECK(!usbwarp_endpoint_mask(0x80) && !usbwarp_endpoint_mask(0x91));
    CHECK(usbwarp_stream_request_valid((1u<<1)|(1u<<18),32));
    CHECK(usbwarp_stream_request_valid(1u<<31,0));
    CHECK(!usbwarp_stream_request_valid(0,1));
    CHECK(!usbwarp_stream_request_valid(1u<<16,1));
    CHECK(!usbwarp_stream_request_valid(2,33));
    printf("Production USB2/USB3.2 root hubs: %u checks passed\n",checks);
    return 0;
}
