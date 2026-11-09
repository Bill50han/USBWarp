// SPDX-License-Identifier: GPL-2.0-only
// Exercise production overlapped cancellation against a real stalled pipe.
#include <windows.h>
#include <assert.h>
#include <stdio.h>
static char pipe_name[128];
#define USBWARP_PIPE_NAME pipe_name
#define PIPE_COMMAND_TIMEOUT_MS 100u
#include "../USBWarpController/pipe_client.c"
static HANDLE server, stop_event;
static DWORD WINAPI serve(void *)
{
    BOOL connected=ConnectNamedPipe(server,nullptr);
    if(!connected) assert(GetLastError()==ERROR_PIPE_CONNECTED);
    BYTE request[128]; DWORD count=0;
    assert(ReadFile(server,request,sizeof(request),&count,nullptr) && count>=sizeof(usbwarp_pipe_header));
    // A fake server receives at most an identification token from the CLI.
    assert(ImpersonateNamedPipeClient(server));
    HANDLE token=nullptr; assert(OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&token));
    SECURITY_IMPERSONATION_LEVEL level; DWORD bytes=0;
    assert(GetTokenInformation(token,TokenImpersonationLevel,&level,sizeof(level),&bytes));
    assert(level==SecurityIdentification);
    assert(RevertToSelf()); CloseHandle(token);
    assert(WaitForSingleObject(stop_event,5000)==WAIT_OBJECT_0);
    return 0;
}
int main()
{
    snprintf(pipe_name,sizeof(pipe_name),"\\\\.\\pipe\\USBWarp.Timeout.%lu.%llu",GetCurrentProcessId(),GetTickCount64());
    server=CreateNamedPipeA(pipe_name,PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,1024,1024,0,nullptr);
    assert(server!=INVALID_HANDLE_VALUE);
    stop_event=CreateEventW(nullptr,TRUE,FALSE,nullptr); assert(stop_event);
    HANDLE thread=CreateThread(nullptr,0,serve,nullptr,0,nullptr); assert(thread);
    PIPE_CLIENT client{}; client.hPipe=INVALID_HANDLE_VALUE;
    assert(PipeConnect(&client)==0);
    BYTE response[1024]; uint32_t length=999;
    ULONGLONG start=GetTickCount64();
    assert(PipeSendCommand(&client,USBWARP_PIPE_CMD_QUERY_STATUS,nullptr,0,response,sizeof(response),&length)==-1);
    assert(GetTickCount64()-start<3000 && length==0 && client.hPipe==INVALID_HANDLE_VALUE);
    SetEvent(stop_event); assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
    CloseHandle(thread); CloseHandle(stop_event); CloseHandle(server);
    puts("Real pipe: stalled reply cancelled and drained; identification-only client token preserved");
    return 0;
}
