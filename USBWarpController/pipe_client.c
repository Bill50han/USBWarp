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
            fprintf(stderr, "  UsbWarpService.exe --id <VM-GUID>\n");
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
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL);

    if (pc->hPipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Error: Cannot open service pipe (%lu).\n",
                GetLastError());
        return -1;
    }

    /* FIX #4: Set pipe to message-read mode and check result. */
    mode = PIPE_READMODE_MESSAGE;
    ok = SetNamedPipeHandleState(pc->hPipe, &mode, NULL, NULL);
    if (!ok) {
        fprintf(stderr, "Warning: SetNamedPipeHandleState failed (%lu). "
                "Communication may be unreliable.\n", GetLastError());
        /* Continue — byte mode may still work for small messages. */
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

    *respLen = 0;

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

    ok = WriteFile(pc->hPipe, sendBuf, totalSend, &written, NULL);
    HeapFree(GetProcessHeap(), 0, sendBuf);

    if (!ok) {
        fprintf(stderr, "Error: pipe write failed (%lu).\n", GetLastError());
        return -1;
    }

    /* Read response. */
    ok = ReadFile(pc->hPipe, respBuf, respBufSize, &bytesRead, NULL);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_MORE_DATA) {
            /* FIX #5: Response truncated.  We have partial data in
             * respBuf.  Warn the user but return what we got — the
             * caller can still parse the header and status. */
            fprintf(stderr, "Warning: response truncated (%lu bytes read, "
                    "more data available).\n", bytesRead);
            *respLen = bytesRead;
            return 0;
        }
        fprintf(stderr, "Error: pipe read failed (%lu).\n", err);
        return -1;
    }

    *respLen = bytesRead;

    /* Validate response header. */
    if (bytesRead < sizeof(struct usbwarp_pipe_header)) {
        fprintf(stderr, "Error: response too short (%lu bytes).\n", bytesRead);
        return -1;
    }

    struct usbwarp_pipe_header *respHdr = (struct usbwarp_pipe_header *)respBuf;
    if (respHdr->magic != USBWARP_PIPE_MAGIC) {
        fprintf(stderr, "Error: bad response magic 0x%x.\n", respHdr->magic);
        return -1;
    }

    return 0;
}
