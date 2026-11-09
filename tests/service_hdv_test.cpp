// SPDX-License-Identifier: GPL-2.0-only
// Exercises the production HCS/HDV lifecycle through a fake native API table.
#include "../USBWarpService/warp_service.h"
#include <stdio.h>
#include <assert.h>

void LogInfo(const char *, ...) {}
void LogWarn(const char *, ...) {}
void LogError(const char *, ...) {}
bool StopRequested(const warp_session *s) { return s->shutdown_requested.load(); }
static unsigned irqConfigs;
BOOL DrvClientConfigureIrq(warp_session *) { ++irqConfigs; return TRUE; }

static warp_session *session;
static const HDV_PCI_DEVICE_INTERFACE *callbacks;
static HRESULT openError, initError, createError, addError, teardownError;
static unsigned checks, operations, waits, maps, unmaps, bells, unbells, closes, teardowns, addTimeouts;
static bool adding, completed, stopDuringAdd;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)

static HRESULT WINAPI Open(const WCHAR *, DWORD, HCS_SYSTEM *out)
{ if (FAILED(openError)) return openError; *out=(HCS_SYSTEM)1; return S_OK; }
static HCS_OPERATION WINAPI CreateOp(void *, void *)
{ ++operations; completed=false; return CreateEventW(nullptr,TRUE,FALSE,nullptr); }
static HRESULT WINAPI Modify(HCS_SYSTEM, HCS_OPERATION, const WCHAR *json, HANDLE)
{
    adding=wcsstr(json,L"\"Add\"")!=nullptr;
    assert(adding); // Windows FlexibleIov has no Remove or Update operation.
    if (adding && FAILED(addError)) { completed=true; return addError; }
    return S_OK;
}
static HRESULT WINAPI Wait(HCS_OPERATION, DWORD milliseconds, PWSTR *document)
{
    assert(milliseconds==100); ++waits;
    *document=(PWSTR)LocalAlloc(LPTR,3*sizeof(WCHAR));
    wcscpy_s(*document,3,L"{}");
    if (adding && addTimeouts) {
        --addTimeouts;
        callbacks->Start(session);
        callbacks->WriteConfigSpace(session,4,2);
        if (stopDuringAdd) session->shutdown_requested=true;
        return HCS_E_OPERATION_TIMEOUT;
    }
    completed=true; return S_OK;
}
static void WINAPI CloseOp(HCS_OPERATION op)
{ assert(completed); --operations; CloseHandle(op); }
static void WINAPI CloseSystem(HCS_SYSTEM) { ++closes; }
static HRESULT WINAPI Init(HCS_SYSTEM, HDV_HOST *out)
{ if (FAILED(initError)) return initError; *out=(HDV_HOST)2; return S_OK; }
static HRESULT WINAPI Teardown(HDV_HOST)
{
    ++teardowns;
    if (FAILED(teardownError)) return teardownError;
    if (callbacks) callbacks->Teardown(session);
    return S_OK;
}
static HRESULT WINAPI CreateDevice(HDV_HOST, HDV_DEVICE_TYPE, const GUID *, const GUID *,
    const void *iface, void *context, HDV_DEVICE *out)
{
    if (FAILED(createError)) return createError;
    callbacks=(const HDV_PCI_DEVICE_INTERFACE *)iface; session=(warp_session *)context;
    *out=(HDV_DEVICE)3;
    return callbacks->Initialize(context);
}
static HRESULT WINAPI Map(HDV_DEVICE, HDV_PCI_BAR_SELECTOR bar, UINT64 offset, UINT64 pages,
                          HDV_MMIO_MAPPING_FLAGS flags, HANDLE section, UINT64 sectionOffset)
{
    assert(bar==HDV_PCI_BAR0 && !offset && !sectionOffset && pages==session->shm_size/4096);
    assert(flags==HdvMmioMappingFlagWriteable && section==session->section);
    ++maps; return S_OK;
}
static HRESULT WINAPI Unmap(HDV_DEVICE, HDV_PCI_BAR_SELECTOR bar, UINT64 offset)
{ assert(bar==HDV_PCI_BAR0 && !offset); ++unmaps; return S_OK; }
static HRESULT WINAPI RegisterBell(HDV_DEVICE,HDV_PCI_BAR_SELECTOR bar,UINT64 offset,UINT64 trigger,UINT64 flags,HANDLE event)
{ assert(bar==HDV_PCI_BAR2 && !offset && trigger==1 && flags==4 && event==session->doorbellEvent); ++bells; return S_OK; }
static HRESULT WINAPI UnregisterBell(HDV_DEVICE,HDV_PCI_BAR_SELECTOR bar,UINT64 offset,UINT64 trigger,UINT64 flags)
{ assert(bar==HDV_PCI_BAR2 && !offset && trigger==1 && flags==4); ++unbells; return S_OK; }
static void configure(warp_session &s)
{
    warp_config_defaults(&s.cfg);
    s.section=(HANDLE)4; s.shm_size=s.cfg.shm_size;
    s.api.HcsOpen=Open; s.api.HcsCreateOp=CreateOp; s.api.HcsModify=Modify;
    s.api.HcsWait=Wait; s.api.HcsCloseOp=CloseOp; s.api.HcsCloseSys=CloseSystem;
    s.api.HdvInitHost=Init; s.api.HdvCreateDevice=CreateDevice;
    s.api.HdvTeardownHost=Teardown; s.api.HdvCreateMmio=Map;
    s.api.HdvDestroyMmio=Unmap;
    s.api.HdvRegisterDoorbell=RegisterBell; s.api.HdvUnregisterDoorbell=UnregisterBell;
    s.doorbellEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    session=&s;
}
static bool empty(const warp_session &s)
{ return !s.hcs_system && !s.hdv_host && !s.hdv_device && !s.memSpaceEvent && !operations; }

int main()
{
    warp_session s; configure(s);
    DWORD before, after;
    GUID warmup; CHECK(SUCCEEDED(CoCreateGuid(&warmup))); // initialize Windows' GUID/RPC cache
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    openError=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    for (unsigned i=0; i<128; ++i) {
        CHECK(!HdvSessionOpen(&s) && GetLastError()==ERROR_FILE_NOT_FOUND);
        CHECK(empty(s));
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && after==before);
    openError=S_OK; initError=E_FAIL;
    CHECK(!HdvSessionOpen(&s) && empty(s) && closes==1);
    initError=S_OK; createError=E_FAIL;
    CHECK(!HdvSessionOpen(&s) && empty(s) && teardowns==1);
    createError=S_OK; addError=E_ACCESSDENIED;
    unsigned priorWaits=waits;
    CHECK(!HdvSessionOpen(&s) && empty(s) && GetLastError()==ERROR_ACCESS_DENIED);
    CHECK(waits==priorWaits); // failed add must not be waited
    addError=S_OK; addTimeouts=2;
    CHECK(HdvSessionOpen(&s) && s.hdv_added && s.mmio_mapped && maps==1 && !operations);
    HDV_PCI_PNP_ID identity{}; UINT32 bars[6]{}, value;
    CHECK(SUCCEEDED(callbacks->GetDetails(&s,&identity,6,bars)) &&
          (bars[0]&0xf)==12 && bars[1]==0xffffffffu && bars[2]==0xfffff004u && bars[3]==0xffffffffu);
    CHECK(s.doorbellRegistered && bells==1);
    callbacks->WriteConfigSpace(&s,0x10,0xffffffffu);
    callbacks->ReadConfigSpace(&s,0x10,&value); CHECK(value==(s.bar0_mask|12));
    callbacks->WriteConfigSpace(&s,0x14,0xffffffffu);
    callbacks->ReadConfigSpace(&s,0x14,&value); CHECK(value==0xffffffffu);
    callbacks->WriteConfigSpace(&s,0x10,0x80000000u);
    callbacks->WriteConfigSpace(&s,0x14,0x20);
    callbacks->ReadConfigSpace(&s,0x10,&value); CHECK(value==0x8000000cu);
    callbacks->ReadConfigSpace(&s,0x14,&value); CHECK(value==0x20);
    CHECK(HdvSessionMapMmio(&s) && maps==1); // one mapping per generation
    GUID firstInstance=s.instance_id;
    CHECK(HdvSessionClose(&s) && empty(s) && unmaps==1);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && after==before);
    addTimeouts=3; stopDuringAdd=true;
    CHECK(!HdvSessionOpen(&s) && GetLastError()==ERROR_OPERATION_ABORTED && empty(s));
    CHECK(maps==1 && !addTimeouts); // stop keeps resources alive until the native add completes
    s.shutdown_requested=false; stopDuringAdd=false;
    addTimeouts=1; CHECK(HdvSessionOpen(&s));
    CHECK(memcmp(&firstInstance,&s.instance_id,sizeof(GUID))!=0);
    HANDLE savedEvent=s.memSpaceEvent; teardownError=E_FAIL;
    CHECK(!HdvSessionClose(&s) && s.hdv_host && s.hcs_system && s.memSpaceEvent==savedEvent);
    teardownError=S_OK;
    CHECK(HdvSessionClose(&s) && empty(s));
    CHECK(maps==unmaps);
    CHECK(bells==unbells && !s.doorbellRegistered);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && after==before);
    s.driverCaps = USBWARP_DRIVER_CAP_H2G_IRQ;
    usbwarp_control_block cb{};
    s.shm_base = &cb;
    addTimeouts=1; CHECK(HdvSessionOpen(&s));
    CHECK(s.irqNonce && irqConfigs);
    callbacks->GetDetails(&s,&identity,6,bars);
    CHECK(bars[4]==0xfffff004u && bars[5]==0xffffffffu);
    callbacks->ReadConfigSpace(&s,0x34,&value); CHECK(value==0x50);
    callbacks->WriteConfigSpace(&s,0x50,0xffffffffu);
    callbacks->ReadConfigSpace(&s,0x50,&value); CHECK(value==0xc0000011u);
    callbacks->WriteConfigSpace(&s,0x54,0xffffffffu);
    callbacks->ReadConfigSpace(&s,0x54,&value); CHECK(value==4);
    callbacks->WriteConfigSpace(&s,0x58,0xffffffffu);
    callbacks->ReadConfigSpace(&s,0x58,&value); CHECK(value==0x804);
    uint32_t vector[4]={0xfee00000u,0,0,0xffffffffu}, observed[4]{};
    CHECK(SUCCEEDED(callbacks->WriteInterceptedMemory(&s,HDV_PCI_BAR4,0,16,(BYTE*)vector)));
    CHECK(SUCCEEDED(callbacks->ReadInterceptedMemory(&s,HDV_PCI_BAR4,0,16,(BYTE*)observed)));
    CHECK(observed[0]==vector[0] && observed[1]==0 && observed[2]==0 && observed[3]==1);
    cb.irq_pending=1;
    uint64_t pending=0, zero=0;
    callbacks->WriteInterceptedMemory(&s,HDV_PCI_BAR4,0x800,8,(BYTE*)&zero);
    callbacks->ReadInterceptedMemory(&s,HDV_PCI_BAR4,0x800,8,(BYTE*)&pending);
    CHECK(pending==1 && cb.irq_pending==1);
    CHECK(FAILED(callbacks->ReadInterceptedMemory(&s,HDV_PCI_BAR4,4095,8,(BYTE*)&pending)));
    CHECK(FAILED(callbacks->WriteInterceptedMemory(&s,HDV_PCI_BAR4,~0ULL,8,(BYTE*)&pending)));
    CHECK(FAILED(callbacks->ReadInterceptedMemory(&s,HDV_PCI_BAR0,0,8,nullptr)));
    CHECK(FAILED(callbacks->ReadInterceptedMemory(&s,HDV_PCI_BAR0,~0ULL,8,(BYTE*)&pending)));
    CHECK(FAILED(callbacks->ReadInterceptedMemory(&s,HDV_PCI_BAR0,s.shm_size-1,8,(BYTE*)&pending)));
    CHECK(FAILED(callbacks->ReadInterceptedMemory(&s,HDV_PCI_BAR1,0,8,(BYTE*)&pending)));
    CHECK(FAILED(callbacks->WriteInterceptedMemory(&s,HDV_PCI_BAR2,0,8,nullptr)));
    CHECK(FAILED(callbacks->WriteInterceptedMemory(&s,HDV_PCI_BAR2,4095,8,(BYTE*)&pending)));
    CHECK(FAILED(callbacks->ReadInterceptedMemory(&s,HDV_PCI_BAR2,0,~0ULL,(BYTE*)&pending)));
    CHECK(FAILED(callbacks->WriteInterceptedMemory(&s,HDV_PCI_BAR4,0,0,(BYTE*)&pending)));
    s.driverCaps=0; // kernel session closed; HDV callbacks still own the PCI shell
    pending=0;
    callbacks->ReadInterceptedMemory(&s,HDV_PCI_BAR4,0x800,8,(BYTE*)&pending);
    CHECK(pending==1);
    callbacks->WriteConfigSpace(&s,0x50,0x40000000u);
    callbacks->ReadConfigSpace(&s,0x50,&value); CHECK(value==0x40000011u);
    CHECK(HdvSessionClose(&s));
    s.shm_base=nullptr;
    CloseHandle(s.doorbellEvent);
    printf("HDV lifecycle: %u checks passed\n",checks);
    return 0;
}
