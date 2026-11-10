/*
 * pipe_client.c — Named Pipe client for Controller → Service communication.
 *
 * Copyright (c) 2026 UsbWarp Project
 *
 * FIX #4: Check SetNamedPipeHandleState return value.
 * FIX #5: Better ERROR_MORE_DATA handling with warning.
 * FIX #8: Use PIPE_BUF_SIZE (64KB) for send buffer, heap-allocate.
 */

#include "usbwarp_cli.h"

/* Bound the entire request/reply exchange, including a stalled or spoofed
 * local server. Cancellation must complete before OVERLAPPED/buffers die. */
#ifndef PIPE_COMMAND_TIMEOUT_MS
#define PIPE_COMMAND_TIMEOUT_MS 60000u
#endif
static BOOL PipeTransfer(HANDLE pipe, void *buffer, DWORD size, DWORD *bytes,
                         BOOL write, ULONGLONG deadline)
{
    OVERLAPPED ov = { 0 };
    DWORD error;
    BOOL ok;
    *bytes = 0;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return FALSE;
    ok = write ? WriteFile(pipe, buffer, size, bytes, &ov) : ReadFile(pipe, buffer, size, bytes, &ov);
    error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_PENDING) {
        ULONGLONG now = GetTickCount64();
        DWORD remaining = now >= deadline ? 0 : (DWORD)(deadline - now);
        DWORD wait = WaitForSingleObject(ov.hEvent, remaining);
        DWORD waitError = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        if (wait != WAIT_OBJECT_0) CancelIoEx(pipe, &ov);
        ok = GetOverlappedResult(pipe, &ov, bytes, TRUE);
        error = ok ? ERROR_SUCCESS : GetLastError();
        if (wait != WAIT_OBJECT_0) { ok = FALSE; error = waitError; }
    }
    CloseHandle(ov.hEvent);
    SetLastError(error);
    return ok;
}

int PipeConnect(PIPE_CLIENT *pc)
{
    DWORD mode;
    BOOL  ok;

    pc->hPipe = INVALID_HANDLE_VALUE;
    pc->nextSeqId = 1;

    if (!WaitNamedPipeA(USBWARP_PIPE_NAME, 5000)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) {
            fprintf(stderr, "Error: UsbWarp Service is not running.\n");
            fprintf(stderr, "Start the service first:\n");
            fprintf(stderr, "  sc.exe start UsbWarpService\n");
            fprintf(stderr, "  Then select a target with: USBWarpController vm use --wsl\n");
        } else if (err == ERROR_SEM_TIMEOUT) {
            fprintf(stderr, "Error: Service pipe is busy.\n");
        } else {
            fprintf(stderr, "Error: Cannot connect to service pipe (%lu).\n",
                    err);
        }
        return -1;
    }

    pc->hPipe = CreateFileA(
        USBWARP_PIPE_NAME,
        FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
        0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);

    if (pc->hPipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Error: Cannot open service pipe (%lu).\n",
                GetLastError());
        return -1;
    }

    /* FIX #4: Set pipe to message-read mode and check result. */
    mode = PIPE_READMODE_MESSAGE;
    ok = SetNamedPipeHandleState(pc->hPipe, &mode, NULL, NULL);
    if (!ok) {
        fprintf(stderr, "Error: SetNamedPipeHandleState failed (%lu).\n", GetLastError());
        PipeDisconnect(pc);
        return -1;
    }

    return 0;
}

void PipeDisconnect(PIPE_CLIENT *pc)
{
    if (pc->hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pc->hPipe);
        pc->hPipe = INVALID_HANDLE_VALUE;
    }
}

int PipeSendCommand(PIPE_CLIENT *pc,
                    uint32_t command,
                    const void *payload, uint32_t payloadLen,
                    void *respBuf, uint32_t respBufSize,
                    uint32_t *respLen)
{
    struct usbwarp_pipe_header hdr;
    DWORD  written, bytesRead;
    BOOL   ok;
    BYTE  *sendBuf;
    uint32_t totalSend;
    ULONGLONG deadline;

    if (!respLen) return -1;
    *respLen = 0;
    if (!pc || pc->hPipe == INVALID_HANDLE_VALUE || !pc->hPipe ||
        payloadLen > PIPE_BUF_SIZE - sizeof(hdr) || (payloadLen && !payload) ||
        !respBuf || respBufSize < sizeof(hdr) + sizeof(struct usbwarp_pipe_response))
        return -1;

    /* Build request. */
    hdr.magic          = USBWARP_PIPE_MAGIC;
    hdr.command        = command;
    hdr.payload_length = payloadLen;
    hdr.sequence_id    = pc->nextSeqId++;

    totalSend = sizeof(hdr) + payloadLen;

    /* FIX #8: Heap-allocate send buffer for large payloads. */
    sendBuf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, totalSend);
    if (!sendBuf) {
        fprintf(stderr, "Error: out of memory.\n");
        return -1;
    }

    memcpy(sendBuf, &hdr, sizeof(hdr));
    if (payloadLen > 0 && payload)
        memcpy(sendBuf + sizeof(hdr), payload, payloadLen);

    deadline = GetTickCount64() + PIPE_COMMAND_TIMEOUT_MS;
    ok = PipeTransfer(pc->hPipe, sendBuf, totalSend, &written, TRUE, deadline);
    HeapFree(GetProcessHeap(), 0, sendBuf);

    if (!ok || written != totalSend) {
        fprintf(stderr, "Error: pipe write failed (%lu).\n", GetLastError());
        PipeDisconnect(pc);
        return -1;
    }

    /* Read response. */
    ok = PipeTransfer(pc->hPipe, respBuf, respBufSize, &bytesRead, FALSE, deadline);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_MORE_DATA) {
            fprintf(stderr, "Error: response exceeds the receive buffer.\n");
            PipeDisconnect(pc);
            return -1;
        }
        fprintf(stderr, "Error: pipe read failed (%lu).\n", err);
        PipeDisconnect(pc);
        return -1;
    }

    /* Validate response header. */
    if (bytesRead < sizeof(struct usbwarp_pipe_header) + sizeof(struct usbwarp_pipe_response)) {
        fprintf(stderr, "Error: response too short (%lu bytes).\n", bytesRead);
        PipeDisconnect(pc);
        return -1;
    }

    struct usbwarp_pipe_header *respHdr = (struct usbwarp_pipe_header *)respBuf;
    if (respHdr->magic != USBWARP_PIPE_MAGIC || respHdr->command != USBWARP_PIPE_CMD_RESPONSE ||
        respHdr->sequence_id != hdr.sequence_id || respHdr->payload_length != bytesRead - sizeof(hdr)) {
        fprintf(stderr, "Error: invalid response envelope.\n");
        PipeDisconnect(pc);
        return -1;
    }

    const struct usbwarp_pipe_response *response =
        (const struct usbwarp_pipe_response *)((const BYTE *)respBuf + sizeof(hdr));
    if (response->detail_length != respHdr->payload_length - sizeof(*response)) {
        fprintf(stderr, "Error: invalid response payload length.\n");
        PipeDisconnect(pc);
        return -1;
    }
    *respLen = bytesRead;

    return 0;
}
