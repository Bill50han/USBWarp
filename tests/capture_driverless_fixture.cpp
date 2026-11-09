// SPDX-License-Identifier: GPL-2.0-only
// VM-only fixture: create a real NULL-driver state on an explicitly identified
// USB leaf. A separate durable journal retains its original driver for finally
// cleanup. Mutations require both production services to be stopped.
#define USBWARP_SERVICE_REG_KEY L"SOFTWARE\\UsbWarp\\SelfTest\\Driverless"
#include "../USBWarpService/capture_pnp.cpp"
#include "../USBWarpService/device_capture.cpp"
#include <cstdio>
#include <cstdarg>
#pragma comment(lib,"ole32.lib")
void LogWarn(const char *format,...) { va_list args; va_start(args,format); vprintf(format,args); puts(""); va_end(args); }
void LogError(const char *format,...) { va_list args; va_start(args,format); vprintf(format,args); puts(""); va_end(args); }
BOOL DrvClientEnumDevices(warp_session *,USBWARP_ENUM_DEVICES_OUT *) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
BOOL DrvClientBindGuid(warp_session *,const uint8_t *,const WCHAR *,uint8_t,uint32_t *,uint16_t *,uint16_t *) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
BOOL DrvClientUnbindGuid(warp_session *,const uint8_t *) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
DWORD DrvClientClose(warp_session *) { return ERROR_NOT_SUPPORTED; }
static DWORD Stopped()
{
    SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if(!manager) return GetLastError();
    DWORD error=0;
    for(const auto name:{L"UsbWarpService",L"USBWarp"}) {
        SC_HANDLE service=OpenServiceW(manager,name,SERVICE_QUERY_STATUS);
        if(!service) { error=GetLastError(); break; }
        SERVICE_STATUS status{};
        if(!QueryServiceStatus(service,&status)) error=GetLastError();
        else if(status.dwCurrentState!=SERVICE_STOPPED) error=ERROR_BUSY;
        CloseServiceHandle(service);
        if(error) break;
    }
    CloseServiceHandle(manager); return error;
}
int wmain(int argc,WCHAR **argv)
{
    if(argc!=4) return ERROR_INVALID_PARAMETER;
    capture::Record identity; identity.instance=argv[2];
    if(FAILED(CLSIDFromString(argv[3],&identity.container))) return ERROR_INVALID_PARAMETER;
    DWORD error=capture::Identity(identity); if(error) return (int)error;
    auto guid=(const uint8_t *)&identity.container;
    auto name=Name(guid);
    bool code28=!wcscmp(argv[1],L"prepare-code28");
    if(!wcscmp(argv[1],L"prepare") || code28) {
        error=Stopped(); if(error) return (int)error;
        capture::Record existing;
        error=ReadRecord(name,existing);
        if(error!=ERROR_FILE_NOT_FOUND) return error ? (int)error : ERROR_ALREADY_EXISTS;
        error=capture::Resolve(guid,argv[2],existing); if(error) return (int)error;
        if(!existing.HasFunctionDriver()) return ERROR_INVALID_STATE;
        existing.phase=capture::Phase::Switching;
        error=SaveRecord(name,existing); if(error) return (int)error;
        error=capture::SetNullDriver(existing);
        if(error) { DWORD rollback=Restore(name); printf("prepare=%lu rollback=%lu\n",error,rollback); return (int)error; }
        if(code28) {
            DEVINST node;
            CONFIGRET cr=CM_Locate_DevNodeW(&node,argv[2],CM_LOCATE_DEVNODE_NORMAL);
            if(cr==CR_SUCCESS) cr=CM_Set_DevNode_Problem(node,CM_PROB_FAILED_INSTALL,0);
            if(cr!=CR_SUCCESS) return (int)CM_MapCrToWin32Err(cr,ERROR_GEN_FAILURE);
        }
        puts("Fixture: original driver journaled; NULL driver installed on exact USB instance.");
    } else if(!wcscmp(argv[1],L"restore")) {
        error=Stopped(); if(error) return (int)error;
        error=Restore(name);
        printf("Fixture: original Windows driver restoration=%lu\n",error);
        return (int)error;
    } else if(wcscmp(argv[1],L"verify-none")) return ERROR_INVALID_PARAMETER;
    identity.binding=capture::OriginalBinding::None;
    error=capture::Verify(identity,false); if(error) { printf("no-driver verification=%lu\n",error); return (int)error; }
    capture::Record resolved;
    error=capture::Resolve(guid,argv[2],resolved);
    if(error) { printf("driverless Resolve=%lu\n",error); return (int)error; }
    if(resolved.binding!=capture::OriginalBinding::None) return ERROR_INVALID_DATA;
    capture::Port port; error=port.Open(identity.instance);
    if(error) return (int)error;
    DEVINST node; ULONG status,problem;
    if(CM_Locate_DevNodeW(&node,argv[2],CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS ||
       CM_Get_DevNode_Status(&status,&problem,node,0)!=CR_SUCCESS) return ERROR_DEVICE_NOT_CONNECTED;
    printf("Verified: binding=none, no INF/service/model, hub port=%lu, PnP problem=%lu, started=%d\n",
        port.index,problem,(status & DN_STARTED)!=0);
    return 0;
}
