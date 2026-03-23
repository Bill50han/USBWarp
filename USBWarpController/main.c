/*
 * main.c — UsbWarp Controller CLI entry point.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * FIX #1: Cross-reference Service bound list with local enumeration.
 * FIX #2: Use containerId (DEVPKEY_Device_ContainerId) as the unique
 *         device identifier throughout bind/unbind/list.
 * FIX #7: Bounds-check detail_length in all response parsers.
 */

#include "usbwarp_cli.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Safe detail string extraction (#7)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ExtractDetail(const struct usbwarp_pipe_response *resp,
                          uint32_t availableBytes,
                          char *out, int outLen)
{
    out[0] = '\0';
    if (resp->detail_length == 0)
        return;

    /* FIX #7: Clamp detail_length to what's actually available in
     * the buffer after the response struct.  Prevents OOB read. */
    uint32_t maxDetail = availableBytes > sizeof(*resp) ?
                         availableBytes - sizeof(*resp) : 0;
    uint32_t copyLen = resp->detail_length;
    if (copyLen > maxDetail) copyLen = maxDetail;
    if (copyLen >= (uint32_t)outLen) copyLen = (uint32_t)(outLen - 1);

    memcpy(out, (const uint8_t *)resp + sizeof(*resp), copyLen);
    out[copyLen] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  'list' command — enumerate + cross-reference bound status (#1)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int CmdList(int argc, char *argv[])
{
    USB_DEVICE_LIST devList;
    PIPE_CLIENT     pc;
    bool            serviceOnline = false;
    int             i;

    (void)argc; (void)argv;

    if (UsbEnumerate(&devList) < 0) {
        fprintf(stderr, "Error: USB device enumeration failed.\n");
        return 1;
    }

    /* FIX #1: Query bound devices from Service and cross-reference. */
    if (PipeConnect(&pc) == 0) {
        serviceOnline = true;

        uint8_t *respBuf = (uint8_t *)HeapAlloc(GetProcessHeap(), 0,
                                                  PIPE_BUF_SIZE);
        if (respBuf) {
            uint32_t respLen = 0;

            if (PipeSendCommand(&pc, USBWARP_PIPE_CMD_LIST_DEVICES,
                                NULL, 0, respBuf, PIPE_BUF_SIZE,
                                &respLen) == 0) {
                uint32_t minResp = sizeof(struct usbwarp_pipe_header) +
                                   sizeof(struct usbwarp_pipe_response) +
                                   sizeof(struct usbwarp_pipe_device_list);

                if (respLen >= minResp) {
                    const uint8_t *p = respBuf +
                        sizeof(struct usbwarp_pipe_header) +
                        sizeof(struct usbwarp_pipe_response);
                    const struct usbwarp_pipe_device_list *dl =
                        (const struct usbwarp_pipe_device_list *)p;

                    const uint8_t *cursor = p + sizeof(*dl);
                    uint32_t remaining = respLen - (uint32_t)(cursor - respBuf);

                    for (uint32_t d = 0; d < dl->device_count; d++) {
                        if (remaining < sizeof(struct usbwarp_pipe_device_entry))
                            break;

                        const struct usbwarp_pipe_device_entry *entry =
                            (const struct usbwarp_pipe_device_entry *)cursor;

                        /* Cross-reference: match containerId with local
                         * enumerated devices. */
                        for (i = 0; i < devList.count; i++) {
                            if (GuidsEqual(&devList.devices[i].containerId,
                                           entry->device_guid)) {
                                devList.devices[i].bound = true;
                                break;
                            }
                        }

                        cursor += sizeof(struct usbwarp_pipe_device_entry) +
                                  entry->description_length;
                        remaining = (uint32_t)(respLen -
                            (uint32_t)(cursor - respBuf));
                    }
                }
            }
            HeapFree(GetProcessHeap(), 0, respBuf);
        }

        PipeDisconnect(&pc);
    }

    /* Print device list. */
    printf("\n");
    printf("  %-10s  %-9s  %-8s  %s\n",
           "BUSID", "VID:PID", "STATE", "DESCRIPTION");
    printf("  %-10s  %-9s  %-8s  %s\n",
           "----------", "---------", "--------",
           "-------------------------------------------");

    if (devList.count == 0) {
        printf("  (no USB devices found)\n");
    }

    for (i = 0; i < devList.count; i++) {
        USB_HOST_DEVICE *d = &devList.devices[i];
        const char *state;

        if (d->bound)
            state = "Attached";
        else if (serviceOnline)
            state = "Free";
        else
            state = "Unknown";

        printf("  %-10s  %04x:%04x  %-8s  %s\n",
               d->busId,
               d->vendorId, d->productId,
               state,
               d->description);
    }

    printf("\n");

    if (!serviceOnline) {
        printf("  Note: Service not running — device state unknown.\n");
        printf("  Start the service: UsbWarpService.exe --id <VM-GUID>\n\n");
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  'status' command (#7: bounds-checked detail parsing)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int CmdStatus(int argc, char *argv[])
{
    PIPE_CLIENT pc;
    uint8_t     respBuf[4096];
    uint32_t    respLen = 0;

    (void)argc; (void)argv;

    if (PipeConnect(&pc) != 0) return 1;

    if (PipeSendCommand(&pc, USBWARP_PIPE_CMD_QUERY_STATUS,
                        NULL, 0, respBuf, sizeof(respBuf), &respLen) != 0) {
        fprintf(stderr, "Error: failed to query status.\n");
        PipeDisconnect(&pc);
        return 1;
    }
    PipeDisconnect(&pc);

    uint32_t minLen = sizeof(struct usbwarp_pipe_header) +
                      sizeof(struct usbwarp_pipe_response) +
                      sizeof(struct usbwarp_pipe_status);
    if (respLen < minLen) {
        fprintf(stderr, "Error: status response too short.\n");
        return 1;
    }

    const struct usbwarp_pipe_response *resp =
        (const struct usbwarp_pipe_response *)
        (respBuf + sizeof(struct usbwarp_pipe_header));

    if (resp->status != 0) {
        char detail[256];
        ExtractDetail(resp, respLen - sizeof(struct usbwarp_pipe_header),
                      detail, sizeof(detail));
        fprintf(stderr, "Error: service returned status %d: %s\n",
                resp->status, detail);
        return 1;
    }

    const struct usbwarp_pipe_status *st =
        (const struct usbwarp_pipe_status *)
        ((const uint8_t *)resp + sizeof(*resp));

    char uptimeBuf[64], bytesBuf[64];
    FormatDuration(st->uptime_seconds, uptimeBuf, sizeof(uptimeBuf));
    FormatBytes(st->total_bytes_transferred, bytesBuf, sizeof(bytesBuf));

    printf("\n");
    printf("  UsbWarp Service Status\n");
    printf("  ----------------------\n");
    printf("  Session:        %s\n", SessionStateName(st->session_state));
    printf("  Uptime:         %s\n", uptimeBuf);
    printf("  Bound devices:  %u\n", st->bound_device_count);
    printf("  Active devices: %u\n", st->active_device_count);
    printf("  Orphan mode:    %s\n", st->orphan_mode ? "YES" : "no");
    printf("  URBs processed: %llu\n",
           (unsigned long long)st->total_urbs_processed);
    printf("  Bytes xferred:  %s\n", bytesBuf);
    printf("\n");

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  'stats' command
 * ═══════════════════════════════════════════════════════════════════════════ */

static int CmdStats(int argc, char *argv[])
{
    PIPE_CLIENT pc;
    uint8_t     respBuf[4096];
    uint32_t    respLen = 0;

    (void)argc; (void)argv;

    if (PipeConnect(&pc) != 0) return 1;

    if (PipeSendCommand(&pc, USBWARP_PIPE_CMD_QUERY_STATS,
                        NULL, 0, respBuf, sizeof(respBuf), &respLen) != 0) {
        fprintf(stderr, "Error: failed to query stats.\n");
        PipeDisconnect(&pc);
        return 1;
    }
    PipeDisconnect(&pc);

    uint32_t minLen = sizeof(struct usbwarp_pipe_header) +
                      sizeof(struct usbwarp_pipe_response) +
                      sizeof(struct usbwarp_pipe_stats_response);
    if (respLen < minLen) {
        fprintf(stderr, "Error: stats response too short.\n");
        return 1;
    }

    const struct usbwarp_pipe_response *resp =
        (const struct usbwarp_pipe_response *)
        (respBuf + sizeof(struct usbwarp_pipe_header));

    if (resp->status != 0) {
        char detail[256];
        ExtractDetail(resp, respLen - sizeof(struct usbwarp_pipe_header),
                      detail, sizeof(detail));
        fprintf(stderr, "Error: status %d: %s\n", resp->status, detail);
        return 1;
    }

    const struct usbwarp_pipe_stats_response *sr =
        (const struct usbwarp_pipe_stats_response *)
        ((const uint8_t *)resp + sizeof(*resp));

    char outBuf[64], inBuf[64];
    FormatBytes(sr->bytes_out, outBuf, sizeof(outBuf));
    FormatBytes(sr->bytes_in, inBuf, sizeof(inBuf));

    printf("\n");
    printf("  UsbWarp Performance Statistics\n");
    printf("  ------------------------------\n");
    printf("  URB submits:     %llu\n",
           (unsigned long long)sr->urb_submit_count);
    printf("  URB completes:   %llu\n",
           (unsigned long long)sr->urb_complete_count);
    printf("  URB cancels:     %llu\n",
           (unsigned long long)sr->urb_cancel_count);
    printf("  URB errors:      %llu\n",
           (unsigned long long)sr->urb_error_count);
    printf("  Bytes OUT:       %s\n", outBuf);
    printf("  Bytes IN:        %s\n", inBuf);
    printf("  Ring usage:      %u%%\n", sr->ring_usage_percent);
    printf("  Buffer usage:    %u%%\n", sr->buffer_usage_percent);
    printf("  Rate limit hits: %u\n", sr->rate_limit_hits);
    printf("  CB trips:        %u\n", sr->circuit_breaker_trips);
    printf("\n");

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  'bind' command (#2: containerId as identifier)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int CmdBind(int argc, char *argv[])
{
    USB_DEVICE_LIST devList;
    PIPE_CLIENT     pc;
    const char     *target;
    USB_HOST_DEVICE *found = NULL;
    int              i;

    if (argc < 1) {
        fprintf(stderr, "Usage: usbwarp bind <BUSID>\n");
        fprintf(stderr, "  Use 'usbwarp list' to see available devices.\n");
        return 1;
    }

    target = argv[0];

    if (UsbEnumerate(&devList) < 0) {
        fprintf(stderr, "Error: USB device enumeration failed.\n");
        return 1;
    }

    for (i = 0; i < devList.count; i++) {
        if (_stricmp(devList.devices[i].busId, target) == 0) {
            found = &devList.devices[i];
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "Error: device '%s' not found.\n", target);
        fprintf(stderr, "  Use 'usbwarp list' to see available devices.\n");
        return 1;
    }

    printf("Binding %04x:%04x (%s) ...\n",
           found->vendorId, found->productId, found->description);

    if (PipeConnect(&pc) != 0) return 1;

    /* Build bind request: fixed header + variable-length device path.
     * We send the device interface path (\\?\USB#...#{GUID}) which
     * the driver needs for WdfIoTargetOpen. */
    {
        const char *pathToSend = found->devicePath[0] ?
                                 found->devicePath : found->instanceId;
        struct usbwarp_pipe_bind_request req;
        uint32_t pathLen = (uint32_t)strlen(pathToSend);
        uint32_t payloadLen = sizeof(req) + pathLen;
        uint8_t *payload = (uint8_t *)HeapAlloc(GetProcessHeap(), 0,
                                                 payloadLen);
        if (!payload) {
            fprintf(stderr, "Error: out of memory.\n");
            PipeDisconnect(&pc);
            return 1;
        }

        memset(&req, 0, sizeof(req));
        memcpy(req.device_guid, &found->containerId, 16);
        req.instance_path_length = (uint16_t)pathLen;

        memcpy(payload, &req, sizeof(req));
        memcpy(payload + sizeof(req), pathToSend, pathLen);

        uint8_t  respBuf[4096];
        uint32_t respLen = 0;

        int rc = PipeSendCommand(&pc, USBWARP_PIPE_CMD_BIND_DEVICE,
                                 payload, payloadLen,
                                 respBuf, sizeof(respBuf), &respLen);
        HeapFree(GetProcessHeap(), 0, payload);
        PipeDisconnect(&pc);

        if (rc != 0) {
            fprintf(stderr, "Error: pipe communication failed.\n");
            return 1;
        }

        if (respLen < sizeof(struct usbwarp_pipe_header) +
                      sizeof(struct usbwarp_pipe_response)) {
            fprintf(stderr, "Error: bind response too short.\n");
            return 1;
        }

        const struct usbwarp_pipe_response *resp =
            (const struct usbwarp_pipe_response *)
            (respBuf + sizeof(struct usbwarp_pipe_header));

        char detail[256];
        ExtractDetail(resp, respLen - sizeof(struct usbwarp_pipe_header),
                      detail, sizeof(detail));

        if (resp->status == 0) {
            printf("Success: %s\n", detail[0] ? detail : "device bound");
        } else {
            fprintf(stderr, "Error: bind failed (status=%d): %s\n",
                    resp->status, detail[0] ? detail : "unknown error");
            return 1;
        }
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  'unbind' command
 * ═══════════════════════════════════════════════════════════════════════════ */

static int CmdUnbind(int argc, char *argv[])
{
    USB_DEVICE_LIST devList;
    PIPE_CLIENT     pc;
    const char     *target;
    USB_HOST_DEVICE *found = NULL;
    int              i;

    if (argc < 1) {
        fprintf(stderr, "Usage: usbwarp unbind <BUSID>\n");
        return 1;
    }

    target = argv[0];

    if (UsbEnumerate(&devList) < 0) {
        fprintf(stderr, "Error: USB device enumeration failed.\n");
        return 1;
    }

    for (i = 0; i < devList.count; i++) {
        if (_stricmp(devList.devices[i].busId, target) == 0) {
            found = &devList.devices[i];
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "Error: device '%s' not found.\n", target);
        return 1;
    }

    printf("Unbinding %04x:%04x (%s) ...\n",
           found->vendorId, found->productId, found->description);

    if (PipeConnect(&pc) != 0) return 1;

    struct usbwarp_pipe_bind_request req;
    memcpy(req.device_guid, &found->containerId, 16);

    uint8_t  respBuf[4096];
    uint32_t respLen = 0;

    if (PipeSendCommand(&pc, USBWARP_PIPE_CMD_UNBIND_DEVICE,
                        &req, sizeof(req),
                        respBuf, sizeof(respBuf), &respLen) != 0) {
        fprintf(stderr, "Error: pipe communication failed.\n");
        PipeDisconnect(&pc);
        return 1;
    }
    PipeDisconnect(&pc);

    if (respLen < sizeof(struct usbwarp_pipe_header) +
                  sizeof(struct usbwarp_pipe_response)) {
        fprintf(stderr, "Error: unbind response too short.\n");
        return 1;
    }

    const struct usbwarp_pipe_response *resp =
        (const struct usbwarp_pipe_response *)
        (respBuf + sizeof(struct usbwarp_pipe_header));

    char detail[256];
    ExtractDetail(resp, respLen - sizeof(struct usbwarp_pipe_header),
                  detail, sizeof(detail));

    if (resp->status == 0) {
        printf("Success: device unbound.\n");
    } else {
        fprintf(stderr, "Error: unbind failed (status=%d): %s\n",
                resp->status, detail[0] ? detail : "unknown error");
        return 1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  Usage and entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

static void PrintUsage(void)
{
    fprintf(stderr,
        "\n"
        "UsbWarp — USB passthrough for WSL2 via Hyper-V HDV\n"
        "\n"
        "Usage:\n"
        "  usbwarp list                   List available USB devices\n"
        "  usbwarp bind   <BUSID>         Bind device to WSL2 guest\n"
        "  usbwarp unbind <BUSID>         Unbind device from guest\n"
        "  usbwarp status                 Show session status\n"
        "  usbwarp stats                  Show performance statistics\n"
        "\n"
        "Examples:\n"
        "  usbwarp list\n"
        "  usbwarp bind 1-3\n"
        "  usbwarp unbind 1-3\n"
        "\n"
        "The BUSID is shown in the first column of 'usbwarp list'.\n"
        "The UsbWarp Service must be running for bind/unbind/status.\n"
        "\n");
}

int main(int argc, char *argv[])
{
    const char *cmd;

    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    cmd = argv[1];

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        PrintUsage();
        return 0;
    }
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        printf("UsbWarp Controller v%u.%u (protocol %u.%u)\n",
               0, 1,
               USBWARP_PROTOCOL_VERSION_MAJOR,
               USBWARP_PROTOCOL_VERSION_MINOR);
        return 0;
    }

    if (strcmp(cmd, "list") == 0)   return CmdList(argc - 2, argv + 2);
    if (strcmp(cmd, "bind") == 0 ||
        strcmp(cmd, "attach") == 0) return CmdBind(argc - 2, argv + 2);
    if (strcmp(cmd, "unbind") == 0 ||
        strcmp(cmd, "detach") == 0) return CmdUnbind(argc - 2, argv + 2);
    if (strcmp(cmd, "status") == 0) return CmdStatus(argc - 2, argv + 2);
    if (strcmp(cmd, "stats") == 0)  return CmdStats(argc - 2, argv + 2);

    fprintf(stderr, "Error: unknown command '%s'.\n", cmd);
    PrintUsage();
    return 1;
}
