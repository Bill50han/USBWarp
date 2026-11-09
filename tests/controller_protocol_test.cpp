// SPDX-License-Identifier: GPL-2.0-only
// Production CLI parsers, with no real enumeration or pipe/device access.
#include "../USBWarpController/usbwarp_cli.h"
#include <vector>
#include <cassert>
static std::vector<BYTE> wire;
static bool truncated;
static bool async_read, async_write, timed_out, cancelled, drained, pending_write;
static void *pending_buffer;
static DWORD pending_size;
static unsigned writes, checks;
static BOOL WINAPI Write(HANDLE, LPCVOID buffer, DWORD size, LPDWORD bytes, LPOVERLAPPED)
{
    ++writes;
    if(async_write){pending_buffer=(void *)buffer;pending_size=size;pending_write=true;*bytes=0;SetLastError(ERROR_IO_PENDING);return FALSE;}
    *bytes=size; return TRUE;
}
static BOOL WINAPI Read(HANDLE, LPVOID buffer, DWORD size, LPDWORD bytes, LPOVERLAPPED)
{
    if(async_read){pending_buffer=buffer;pending_size=size;pending_write=false;*bytes=0;SetLastError(ERROR_IO_PENDING);return FALSE;}
    *bytes=(DWORD)wire.size(); if (*bytes>size) *bytes=size;
    memcpy(buffer,wire.data(),*bytes);
    if (truncated) { SetLastError(ERROR_MORE_DATA); return FALSE; }
    return TRUE;
}
static BOOL WINAPI Close(HANDLE) { return TRUE; }
static HANDLE WINAPI Event(LPSECURITY_ATTRIBUTES,BOOL,BOOL,LPCWSTR) { return (HANDLE)2; }
static DWORD WINAPI Wait(HANDLE,DWORD ms) { assert(ms<=60000); return timed_out?WAIT_TIMEOUT:WAIT_OBJECT_0; }
static BOOL WINAPI Cancel(HANDLE,LPOVERLAPPED) { cancelled=true;return TRUE; }
static BOOL WINAPI Complete(HANDLE,LPOVERLAPPED,LPDWORD bytes,BOOL wait)
{
    assert(wait); drained=true;
    if(cancelled){*bytes=0;SetLastError(ERROR_OPERATION_ABORTED);return FALSE;}
    if(pending_write){assert(((usbwarp_pipe_header *)pending_buffer)->magic==USBWARP_PIPE_MAGIC);*bytes=pending_size;return TRUE;}
    *bytes=(DWORD)wire.size(); if(*bytes>pending_size)*bytes=pending_size;
    memcpy(pending_buffer,wire.data(),*bytes); return TRUE;
}
#define WriteFile Write
#define ReadFile Read
#define CloseHandle Close
#define CreateEventW Event
#define WaitForSingleObject Wait
#define CancelIoEx Cancel
#define GetOverlappedResult Complete
#include "../USBWarpController/pipe_client.c"
#undef WriteFile
#undef ReadFile
#undef CloseHandle
#undef CreateEventW
#undef WaitForSingleObject
#undef CancelIoEx
#undef GetOverlappedResult
#define main ControllerEntry
#include "../USBWarpController/main.c"
#undef main
int UsbEnumerate(USB_DEVICE_LIST *) { return -1; }
bool ParseGuid(const char *, GUID *) { return false; }
void FormatGuid(const GUID *, char *, int) {}
void FormatBytes(uint64_t, char *, int) {}
void FormatDuration(uint64_t, char *, int) {}

#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static void response(uint32_t bytes, int32_t status=0)
{
    wire.assign(sizeof(usbwarp_pipe_header)+sizeof(usbwarp_pipe_response)+bytes,0);
    usbwarp_pipe_header header{USBWARP_PIPE_MAGIC,USBWARP_PIPE_CMD_RESPONSE,(uint32_t)sizeof(usbwarp_pipe_response)+bytes,1};
    usbwarp_pipe_response reply{status,bytes};
    memcpy(wire.data(),&header,sizeof(header)); memcpy(wire.data()+sizeof(header),&reply,sizeof(reply));
}
static int receive()
{
    PIPE_CLIENT client{(HANDLE)1,1}; BYTE buffer[1024]; uint32_t length=999;
    int result=PipeSendCommand(&client,USBWARP_PIPE_CMD_QUERY_STATUS,nullptr,0,buffer,sizeof(buffer),&length);
    if ((result && length) || (truncated && client.hPipe!=INVALID_HANDLE_VALUE)) return 123;
    return result;
}
int main()
{
    response(sizeof(usbwarp_pipe_status)); CHECK(receive()==0);
    ((usbwarp_pipe_header *)wire.data())->sequence_id=2; CHECK(receive()==-1);
    response(0); ((usbwarp_pipe_header *)wire.data())->payload_length++; CHECK(receive()==-1);
    response(4); ((usbwarp_pipe_response *)(wire.data()+sizeof(usbwarp_pipe_header)))->detail_length=100;
    CHECK(receive()==-1);
    wire.resize(sizeof(usbwarp_pipe_header)); CHECK(receive()==-1);
    response(2000); truncated=true; CHECK(receive()==-1); truncated=false;
    unsigned before=writes; PIPE_CLIENT client{(HANDLE)1,1}; BYTE buffer[1024]; uint32_t length;
    CHECK(PipeSendCommand(&client,1,buffer,UINT32_MAX,buffer,sizeof(buffer),&length)==-1 && writes==before);
    CHECK(PipeSendCommand(nullptr,1,nullptr,0,buffer,sizeof(buffer),&length)==-1 && !length);
    CHECK(PipeSendCommand(&client,1,nullptr,0,buffer,sizeof(buffer),nullptr)==-1);
    response(sizeof(usbwarp_pipe_status)); async_read=true; drained=false;
    CHECK(receive()==0 && drained);
    timed_out=true; cancelled=drained=false;
    CHECK(receive()==-1 && cancelled && drained);
    async_read=false; async_write=true; cancelled=drained=false;
    CHECK(receive()==-1 && cancelled && drained);
    async_write=timed_out=cancelled=false;

    USB_DEVICE_LIST local{}; local.count=1; local.devices[0].containerId.Data1=1;
    const uint32_t prefix=sizeof(usbwarp_pipe_header)+sizeof(usbwarp_pipe_response);
    response(400,-ERROR_NOT_READY);
    memset(wire.data()+prefix,0xff,400);
    CHECK(!ApplyBoundList(&local,wire.data(),(uint32_t)wire.size()));
    response(sizeof(usbwarp_pipe_device_list)+sizeof(usbwarp_pipe_device_entry));
    auto list=(usbwarp_pipe_device_list *)(wire.data()+prefix); list->device_count=1;
    auto entry=(usbwarp_pipe_device_entry *)(list+1);
    memcpy(entry->device_guid,&local.devices[0].containerId,16); entry->bound=1; entry->speed=6;
    CHECK(ApplyBoundList(&local,wire.data(),(uint32_t)wire.size()) && local.devices[0].bound && local.devices[0].speed==6);
    local.devices[0].bound=false;
    entry->description_length=65535;
    CHECK(!ApplyBoundList(&local,wire.data(),(uint32_t)wire.size()) && !local.devices[0].bound);
    entry->description_length=0; list->device_count=2; // valid first entry, truncated second
    CHECK(!ApplyBoundList(&local,wire.data(),(uint32_t)wire.size()) && !local.devices[0].bound);
    list->device_count=UINT32_MAX; CHECK(!ApplyBoundList(&local,wire.data(),(uint32_t)wire.size()));
    list->device_count=1;
    for (uint32_t size=0; size<wire.size(); ++size) CHECK(!ApplyBoundList(&local,wire.data(),size));
    response(sizeof(usbwarp_pipe_device_list)); local.devices[0].bound=true;
    CHECK(ApplyBoundList(&local,wire.data(),(uint32_t)wire.size()) && !local.devices[0].bound);
    printf("Controller response boundaries: %u checks passed\n",checks);
    return 0;
}
