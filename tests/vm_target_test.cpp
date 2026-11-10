// SPDX-License-Identifier: GPL-2.0-only
#include "../USBWarpService/warp_service.h"
#include <stdio.h>
#include <string>
#include <thread>

static unsigned checks, saves, closes, waits;
static DWORD saveError, pendingError;
static bool active, pending;
static uint32_t bound;
static warp_config saved;
static const WCHAR *document=L"[]";
bool StopRequested(const warp_session *s) { return s->shutdown_requested; }
BOOL DrvClientActive(warp_session *) { return active; }
BOOL DrvClientEnumDevices(warp_session *, USBWARP_ENUM_DEVICES_OUT *out)
{ *out={};out->DeviceCount=bound;return TRUE; }
DWORD DeviceCapturePending(bool *out) { *out=pending;return pendingError; }
DWORD SaveRegistryConfig(const warp_config *cfg) { ++saves;if(!saveError)saved=*cfg;return saveError; }
static HCS_OPERATION WINAPI Create(void *,void *) {return (HCS_OPERATION)1;}
static HRESULT WINAPI Enumerate(const WCHAR *,HCS_OPERATION) {return S_OK;}
static HRESULT WINAPI Wait(HCS_OPERATION,DWORD ms,PWSTR *result)
{
    if(ms!=100) return E_INVALIDARG;
    *result=nullptr;
    if(++waits<3) return HCS_E_OPERATION_TIMEOUT;
    size_t bytes=(wcslen(document)+1)*sizeof(WCHAR);
    *result=(PWSTR)LocalAlloc(LMEM_FIXED,bytes);
    if(!*result)return E_OUTOFMEMORY;
    memcpy(*result,document,bytes);return S_OK;
}
static void WINAPI Close(HCS_OPERATION) {++closes;}
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}} while(0)
static warp_target Name(const WCHAR *name,const WCHAR *owner=L"")
{warp_target t{};t.kind=USBWARP_TARGET_NAME;wcscpy_s(t.value,name);wcscpy_s(t.owner,owner);return t;}

int main()
{
    warp_target none{}, wsl{};wsl.kind=USBWARP_TARGET_WSL;
    CHECK(ValidateTarget(&none)==0 && ValidateTarget(&wsl)==0);
    warp_target id{};id.kind=USBWARP_TARGET_ID;wcscpy_s(id.value,L"{aaaaaaaa-1111-2222-3333-444444444444}");
    CHECK(ValidateTarget(&id)==0 && !wcscmp(id.value,L"AAAAAAAA-1111-2222-3333-444444444444"));
    usbwarp_pipe_target wire{};warp_target decoded{};
    auto name=Name(L"开发 VM",L"VMMS");
    CHECK(TargetToWire(name,&wire)==0 && TargetFromWire(wire,&decoded)==0 && !memcmp(&decoded,&name,sizeof(name)));
    wire.reserved=1;CHECK(TargetFromWire(wire,&decoded)==ERROR_INVALID_DATA);wire.reserved=0;
    memset(wire.value,'x',sizeof(wire.value));CHECK(TargetFromWire(wire,&decoded)==ERROR_INVALID_DATA);
    CHECK(TargetToWire(name,&wire)==0);wire.value[sizeof(wire.value)-1]='x';CHECK(TargetFromWire(wire,&decoded)==ERROR_INVALID_DATA);
    wire={};wire.kind=USBWARP_TARGET_NAME;memcpy(wire.value,"\xc0\xaf",2);CHECK(TargetFromWire(wire,&decoded)==ERROR_INVALID_DATA);
    wire={};wire.kind=USBWARP_TARGET_WSL;strcpy_s(wire.value,"Ubuntu");CHECK(TargetFromWire(wire,&decoded)==ERROR_INVALID_DATA);
    name=Name(L"bad\x1b[31m");CHECK(ValidateTarget(&name)!=0);
    name=Name(L"bad");name.value[3]=0xd800;CHECK(ValidateTarget(&name)!=0);

    std::vector<warp_vm> vms;
    const WCHAR *json=LR"([{"Id":"11111111-1111-1111-1111-111111111111","Owner":"WSL","SystemType":"VirtualMachine","State":"Running"},
        {"Id":"22222222-2222-2222-2222-222222222222","Name":"\u5f00\u53d1 VM","Owner":"VMMS","SystemType":"VirtualMachine"},
        {"Id":"container","Name":"开发 VM","SystemType":"Container"},
        {"Id":"template","Name":"开发 VM","Owner":"VMMS","SystemType":"VirtualMachine","State":"SavedAsTemplate"}])";
    CHECK(ParseVmDocument(json,vms)==0 && vms.size()==3);
    WCHAR resolved[256]{};uint32_t matches=0;
    CHECK(MatchVm(wsl,vms,resolved,&matches)==0 && matches==1 && !wcscmp(resolved,L"11111111-1111-1111-1111-111111111111"));
    name=Name(L"开发 vm",L"vmms");
    CHECK(MatchVm(name,vms,resolved,&matches)==0 && matches==1 && !wcscmp(resolved,L"22222222-2222-2222-2222-222222222222"));
    warp_vm duplicate=vms[0];wcscpy_s(duplicate.id,L"33333333-3333-3333-3333-333333333333");vms.push_back(duplicate);
    CHECK(MatchVm(wsl,vms,resolved,&matches)==ERROR_DUP_NAME && matches==2 && !resolved[0]);
    CHECK(MatchVm(Name(L"absent"),vms,resolved,&matches)==ERROR_NOT_FOUND && !matches && !resolved[0]);
    CHECK(ParseVmDocument(LR"([{"SystemType":"VirtualMachine","Id":"x","Name":17}])",vms)==ERROR_INVALID_DATA && vms.empty());
    CHECK(ParseVmDocument(LR"([{"SystemType":"VirtualMachine","Id":"x","Name":"bad\u0000hidden"}])",vms)==ERROR_INVALID_DATA);
    CHECK(ParseVmDocument(LR"([{"SystemType":"VirtualMachine","Id":"x"},{"SystemType":"VirtualMachine","Id":"X"}])",vms)==ERROR_INVALID_DATA);
    CHECK(ParseVmDocument(L"[",vms)==ERROR_INVALID_DATA);
    CHECK(ParseVmDocument(L"{}",vms)==ERROR_INVALID_DATA);
    CHECK(ParseVmDocument(json,vms)==0);

    warp_session s;warp_config_defaults(&s.cfg);
    s.targetChangedEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);CHECK(s.targetChangedEvent);
    s.api.HcsCreateOp=Create;s.api.HcsEnumerate=Enumerate;s.api.HcsWait=Wait;s.api.HcsCloseOp=Close;
    document=json;VmRefresh(&s);
    CHECK(waits==3 && closes==1 && s.vmDiscoveryError==0 && s.vms.size()==3);
    auto listGeneration=s.vmListGeneration;
    VmRefresh(&s);CHECK(s.vmListGeneration==listGeneration && closes==2);
    document=L"[]";VmRefresh(&s);CHECK(s.vmListGeneration!=listGeneration && s.vms.empty());
    document=json;VmRefresh(&s);CHECK(s.vms.size()==3);
    CHECK(VmSelect(&s,wsl)==0 && saves==1 && saved.target.kind==USBWARP_TARGET_WSL);
    CHECK(s.cfg.target.kind==USBWARP_TARGET_NONE); // immutable transport/startup config
    CHECK(WaitForSingleObject(s.targetChangedEvent,0)==WAIT_OBJECT_0);
    uint64_t generation=VmSelection(&s,&decoded);
    CHECK(VmResolve(&s,decoded,generation)==0 && VmAttached(&s,generation));
    CHECK(VmStage(&s,generation,USBWARP_SESSION_RUNNING) && VmCanBind(&s));
    active=true;bound=1;CHECK(VmSelect(&s,none)==ERROR_BUSY && saves==1 && VmCurrent(&s,generation));
    CHECK(VmSelect(&s,wsl)==0 && saves==1); // idempotent selection while bound
    bound=0;
    HANDLE selecting=CreateEventW(nullptr,FALSE,FALSE,nullptr);CHECK(selecting);
    std::atomic<DWORD> raced{ERROR_IO_PENDING};
    AcquireSRWLockExclusive(&s.captureLock); // a bind already admitted, before kernel commit
    std::thread switcher([&]{SetEvent(selecting);raced=VmSelect(&s,none);});
    CHECK(WaitForSingleObject(selecting,1000)==WAIT_OBJECT_0);
    bound=1; // kernel binding commits before the capture operation releases its lock
    ReleaseSRWLockExclusive(&s.captureLock);switcher.join();CloseHandle(selecting);
    CHECK(raced==ERROR_BUSY && VmCurrent(&s,generation));
    bound=0;pending=true;CHECK(VmSelect(&s,none)==ERROR_BUSY);pending=false;
    pendingError=ERROR_ACCESS_DENIED;CHECK(VmSelect(&s,none)==ERROR_ACCESS_DENIED);pendingError=0;
    saveError=ERROR_ACCESS_DENIED;CHECK(VmSelect(&s,none)==ERROR_ACCESS_DENIED && VmCurrent(&s,generation));saveError=0;
    CHECK(VmSelect(&s,none)==0 && !VmCurrent(&s,generation) && !VmCanBind(&s));
    CHECK(!VmAttached(&s,generation) && !VmStage(&s,generation,USBWARP_SESSION_RUNNING));
    s.stage=USBWARP_SESSION_RUNNING;CHECK(!VmCanBind(&s)); // stale worker cannot admit a bind
    VmDetached(&s);CHECK(!s.attachedVmId[0] && !s.attachedGeneration);
    CHECK(VmSelect(&s,wsl)==0);generation=VmSelection(&s,&decoded);
    s.vmDiscoveryError=ERROR_ACCESS_DENIED;
    CHECK(VmResolve(&s,decoded,generation)==ERROR_ACCESS_DENIED && !s.resolvedVmId[0]);
    CHECK(VmSelect(&s,id)==0);generation=VmSelection(&s,&decoded);
    CHECK(VmResolve(&s,decoded,generation)==0 && !wcscmp(s.openingVmId,id.value));

    s.vms.clear();s.vmDiscoveryError=0;
    for(unsigned i=0;i<35;++i){warp_vm vm{};swprintf_s(vm.id,L"id-%u",i);wcscpy_s(vm.name,L"test");s.vms.push_back(vm);}
    usbwarp_pipe_vm_list_request request{};std::vector<BYTE> reply;
    CHECK(VmList(&s,request,reply)==0);
    auto page=(usbwarp_pipe_vm_list *)reply.data();
    CHECK(page->count==32 && page->total_count==35 && reply.size()==sizeof(*page)+32*sizeof(usbwarp_pipe_vm_entry));
    request.offset=32;request.generation=page->generation;
    CHECK(VmList(&s,request,reply)==0 && ((usbwarp_pipe_vm_list *)reply.data())->count==3);
    ++s.vmListGeneration;CHECK(VmList(&s,request,reply)==ERROR_RETRY);
    request={};request.offset=1;CHECK(VmList(&s,request,reply)==ERROR_INVALID_DATA);
    request={};request.reserved=1;CHECK(VmList(&s,request,reply)==ERROR_INVALID_DATA);
    CloseHandle(s.targetChangedEvent);
    printf("VM discovery/selection: %u checks passed\n",checks);return 0;
}
