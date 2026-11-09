/*
 * usbwarp_filter_ioctl.h — IOCTL interface between UsbWarp.sys and UsbWarpFilter.sys
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * UsbWarp.sys (Non-PnP control device) cannot submit URBs directly to
 * USB device stacks because it is not a registered USB client.
 * UsbWarpFilter.sys sits inside the USB device stack, holds a valid
 * USBD_HANDLE, and proxies URB submissions on behalf of UsbWarp.sys.
 *
 * Communication: UsbWarp.sys opens the filter's device interface and
 * sends these custom IOCTLs.  The filter translates them into proper
 * IOCTL_INTERNAL_USB_SUBMIT_URB requests using USBD_UrbAllocate.
 */

#ifndef USBWARP_FILTER_IOCTL_H
#define USBWARP_FILTER_IOCTL_H

#include "usbwarp_iso.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Device interface GUID
 *
 *   UsbWarpFilter.sys registers this interface for each USB device it
 *   attaches to.  UsbWarp.sys enumerates interfaces with this GUID to
 *   find the filter instance corresponding to a specific USB device.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* {7E3A8C1F-4B2D-4F6E-9A1C-3D5E7F8B2A4C} */
DEFINE_GUID(GUID_USBWARP_FILTER_INTERFACE,
    0x7e3a8c1f, 0x4b2d, 0x4f6e,
    0x9a, 0x1c, 0x3d, 0x5e, 0x7f, 0x8b, 0x2a, 0x4c);

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  IOCTL codes
 *
 *   FILE_DEVICE_UNKNOWN, METHOD_BUFFERED, FILE_ANY_ACCESS.
 *   Function codes 0x800+ to avoid conflict with standard USB IOCTLs.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define IOCTL_USBWARP_FILTER_SUBMIT_URB \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_USBWARP_FILTER_ABORT_PIPE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_USBWARP_FILTER_GET_PIPES \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_USBWARP_FILTER_SELECT_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_USBWARP_FILTER_GET_DESCRIPTOR \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_USBWARP_FILTER_SUBMIT_ISO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBWARP_FILTER_SELECT_INTERFACE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBWARP_FILTER_STREAMS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBWARP_FILTER_RESET_PIPE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* A capture is a single kernel file object's lease on one PnP incarnation.
 * Closing that file retires the incarnation; another lease requires a port
 * cycle. This handshake deliberately rejects the old transparent filter. */
#define USBWARP_CAPTURE_VERSION 1u
#define IOCTL_USBWARP_FILTER_CLAIM \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)
typedef struct _USBWARP_FILTER_CLAIM {
    uint32_t Version;
    uint32_t Reserved;
} USBWARP_FILTER_CLAIM;

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  SUBMIT_URB request / response
 *
 *   UsbWarp.sys sends this to proxy a URB through the filter.
 *   Input:  USBWARP_FILTER_URB_REQUEST  + optional inline data
 *   Output: USBWARP_FILTER_URB_RESPONSE + optional inline data (IN xfers)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Transfer types (same as usbwarp_shared.h enum values). */
#define USBWARP_FXFER_CONTROL      0
#define USBWARP_FXFER_ISO          1
#define USBWARP_FXFER_BULK         2
#define USBWARP_FXFER_INTERRUPT    3

typedef struct _USBWARP_FILTER_URB_REQUEST {
    uint32_t TransferType;         /* USBWARP_FXFER_xxx                  */
    uint32_t Endpoint;             /* endpoint number 0-15               */
    uint32_t Direction;            /* 0=OUT, 1=IN                        */
    uint32_t TransferLength;       /* requested byte count               */
    uint32_t TransferFlags;        /* USBD_TRANSFER_DIRECTION_IN | ...   */
    uint8_t  SetupPacket[8];       /* control transfers only             */

    /* Transfer buffer options:
     * a) KernelVa != NULL → direct kernel pointer (SHM zero-copy path)
     * b) KernelVa == NULL → inline data follows this struct           */
    uint64_t KernelVa;            /* system VA of transfer buffer, or 0 */
    uint32_t InlineDataOffset;    /* bytes from start of this struct    */
    uint32_t InlineDataLength;    /* bytes of inline data (≤ TransferLength) */

    /* Pipe handle from SELECT_CONFIG response.
     * Required for BULK/INTERRUPT transfers.  64-bit to avoid truncation
     * of USBD_PIPE_HANDLE on 64-bit systems. */
    uint64_t PipeHandle;          /* USBD_PIPE_HANDLE as uint64_t       */
    uint32_t StreamId;           /* 0=ordinary pipe, otherwise 1..N */
    uint32_t Reserved;
} USBWARP_FILTER_URB_REQUEST;

typedef struct _USBWARP_FILTER_STREAMS_REQUEST {
    uint32_t EndpointMask;
    uint32_t NumberOfStreams;
} USBWARP_FILTER_STREAMS_REQUEST;
typedef struct _USBWARP_FILTER_STREAMS_RESPONSE {
    uint32_t NumberOfStreams;
} USBWARP_FILTER_STREAMS_RESPONSE;

typedef struct _USBWARP_FILTER_URB_RESPONSE {
    int32_t  NtStatus;            /* NTSTATUS from USB stack             */
    int32_t  UsbdStatus;          /* USBD_STATUS from USB stack          */
    uint32_t ActualLength;        /* bytes actually transferred          */
    uint32_t Reserved;
    /* For IN transfers: data follows if KernelVa was NULL (inline path) */
} USBWARP_FILTER_URB_RESPONSE;

typedef struct _USBWARP_FILTER_ISO_REQUEST {
    uint64_t KernelVa;
    uint64_t PipeHandle;
    uint32_t Endpoint;
    uint32_t Direction;
    uint32_t TransferLength;
    uint32_t NumberOfPackets;
    uint32_t Speed;
    uint32_t Interval;
    /* struct usbwarp_iso_packet_desc packets[NumberOfPackets] follows */
} USBWARP_FILTER_ISO_REQUEST;

typedef struct _USBWARP_FILTER_ISO_RESPONSE {
    int32_t NtStatus;
    int32_t UsbdStatus;
    uint32_t NumberOfPackets;
    uint32_t StartFrame;
    /* usbwarp_iso_packet_result follows; status is raw USBD_STATUS here */
} USBWARP_FILTER_ISO_RESPONSE;

typedef struct _USBWARP_FILTER_SELECT_INTERFACE_REQUEST {
    uint32_t InterfaceNumber;
    uint32_t AlternateSetting;
} USBWARP_FILTER_SELECT_INTERFACE_REQUEST;

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  GET_PIPES response
 *
 *   Returns the pipe map after SELECT_CONFIGURATION.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define USBWARP_FILTER_MAX_PIPES  32

typedef struct _USBWARP_FILTER_PIPE_INFO {
    uint8_t  EndpointAddress;     /* e.g. 0x81 = EP1 IN                 */
    uint8_t  PipeType;            /* UsbdPipeTypeBulk/Interrupt/...      */
    uint16_t MaxPacketSize;
    uint8_t  InterfaceNumber;
    uint8_t  AlternateSetting;
    uint8_t  Interval;
    uint8_t  Reserved;
    uint64_t PipeHandle;          /* USBD_PIPE_HANDLE as uint64_t       */
} USBWARP_FILTER_PIPE_INFO;

typedef struct _USBWARP_FILTER_PIPES_RESPONSE {
    uint32_t NumPipes;
    uint32_t Reserved;
    USBWARP_FILTER_PIPE_INFO Pipes[USBWARP_FILTER_MAX_PIPES];
} USBWARP_FILTER_PIPES_RESPONSE;

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  GET_DESCRIPTOR request / response
 *
 *   Proxies USB GET_DESCRIPTOR.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USBWARP_FILTER_GET_DESC_REQUEST {
    uint8_t  DescriptorType;      /* USB_DEVICE_DESCRIPTOR_TYPE etc.     */
    uint8_t  Index;
    uint16_t LanguageId;
    uint32_t BufferLength;        /* max bytes to return                 */
} USBWARP_FILTER_GET_DESC_REQUEST;

/* Response: raw descriptor bytes in output buffer. */

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  SELECT_CONFIG request / response
 *
 *   Triggers configuration selection and pipe enumeration.
 *   Input:  configuration index (0 = first config).
 *   Output: USBWARP_FILTER_PIPES_RESPONSE.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct _USBWARP_FILTER_SELECT_CONFIG_REQUEST {
    uint32_t ConfigIndex;         /* usually 0                           */
    uint32_t ConfigurationValue;  /* 0=unconfigure; ignored if ByValue=0 */
    uint32_t ByValue;             /* standard SET_CONFIGURATION request */
} USBWARP_FILTER_SELECT_CONFIG_REQUEST;

/* Response: USBWARP_FILTER_PIPES_RESPONSE */

#endif /* USBWARP_FILTER_IOCTL_H */
