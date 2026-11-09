// SPDX-License-Identifier: GPL-2.0-only
// Device-instance PnP transitions. No knowledge of disks, volumes, HID, or the
// Linux transport. Both directions replace the WHOLE physical USB function.
#include <windows.h>
#include "warp_service.h"
#include <initguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <newdev.h>
#include <usbioctl.h>
#include "../include/usbwarp_filter_ioctl.h"
#include "capture_pnp.h"
#include <cwctype>
#pragma comment(lib,"cfgmgr32.lib")
#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"newdev.lib")

namespace capture {
namespace {
DWORD Error(CONFIGRET cr) { return CM_MapCrToWin32Err(cr,ERROR_GEN_FAILURE); }
struct Info {
    HDEVINFO set=INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA device{sizeof(SP_DEVINFO_DATA)};
    ~Info() { if(set!=INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set); }
    DWORD Open(const std::wstring &id) {
        set=SetupDiCreateDeviceInfoList(nullptr,nullptr);
        if(set==INVALID_HANDLE_VALUE) return GetLastError();
        if(!SetupDiOpenDeviceInfoW(set,id.c_str(),nullptr,0,&device)) return GetLastError();
        SP_DEVINSTALL_PARAMS_W p{sizeof(p)};
        if(!SetupDiGetDeviceInstallParamsW(set,&device,&p)) return GetLastError();
        p.Flags|=DI_QUIETINSTALL;
        p.FlagsEx|=DI_FLAGSEX_NOUIONQUERYREMOVE;
        return SetupDiSetDeviceInstallParamsW(set,&device,&p) ? ERROR_SUCCESS : GetLastError();
    }
};
DWORD Property(DEVINST node,const DEVPROPKEY &key,std::wstring &out,bool optional=false)
{
    WCHAR text[1024]; DEVPROPTYPE type; ULONG size=sizeof(text);
    CONFIGRET cr=CM_Get_DevNode_PropertyW(node,&key,&type,(PBYTE)text,&size,0);
    if(optional && cr==CR_NO_SUCH_VALUE) { out.clear(); return ERROR_SUCCESS; }
    if(cr!=CR_SUCCESS) return Error(cr);
    if(type!=DEVPROP_TYPE_STRING || size<2 || size%2 || size>sizeof(text) || text[size/2-1]) return ERROR_INVALID_DATA;
    out.assign(text); return ERROR_SUCCESS;
}
DWORD Filters(Info &info,DWORD property,std::vector<std::wstring> &values)
{
    DWORD bytes=0,type;
    if(!SetupDiGetDeviceRegistryPropertyW(info.set,&info.device,property,&type,nullptr,0,&bytes)) {
        DWORD error=GetLastError();
        if(error==ERROR_INVALID_DATA) return ERROR_SUCCESS; // absent optional value
        if(error!=ERROR_INSUFFICIENT_BUFFER) return error;
    }
    if(bytes<4 || bytes>65536 || bytes%2) return ERROR_INVALID_DATA;
    std::vector<WCHAR> buffer(bytes/2);
    if(!SetupDiGetDeviceRegistryPropertyW(info.set,&info.device,property,&type,(PBYTE)buffer.data(),bytes,nullptr)) return GetLastError();
    if(type!=REG_MULTI_SZ || buffer.back() || buffer[buffer.size()-2]) return ERROR_INVALID_DATA;
    for(size_t i=0; i+1<buffer.size() && buffer[i];) {
        size_t n=wcsnlen_s(buffer.data()+i,buffer.size()-i);
        if(n>=256 || n>=buffer.size()-i) return ERROR_INVALID_DATA;
        values.emplace_back(buffer.data()+i,n); i+=n+1;
    }
    return ERROR_SUCCESS;
}
DWORD SetFilters(Info &info,DWORD property,const std::vector<std::wstring> &values)
{
    if(values.empty()) {
        DWORD bytes=0;
        if(!SetupDiGetDeviceRegistryPropertyW(info.set,&info.device,property,nullptr,nullptr,0,&bytes) &&
            GetLastError()==ERROR_INVALID_DATA) return ERROR_SUCCESS;
    }
    std::vector<WCHAR> buffer;
    for(const auto &v:values) { buffer.insert(buffer.end(),v.begin(),v.end()); buffer.push_back(0); }
    if(!buffer.empty()) buffer.push_back(0);
    return SetupDiSetDeviceRegistryPropertyW(info.set,&info.device,property,
        buffer.empty() ? nullptr : (BYTE *)buffer.data(),(DWORD)(buffer.size()*2)) ? ERROR_SUCCESS : GetLastError();
}
DWORD DriverDetail(Info &info,DWORD kind,SP_DRVINFO_DATA_W &driver,std::wstring &inf,std::wstring &section,
                   const std::wstring *model=nullptr,bool *modelMatches=nullptr,std::wstring *primaryModel=nullptr)
{
    auto device=info.device.DevInst ? &info.device : nullptr;
    DWORD bytes=0;
    SetupDiGetDriverInfoDetailW(info.set,device,&driver,nullptr,0,&bytes);
    if(bytes<sizeof(SP_DRVINFO_DETAIL_DATA_W) || bytes>65536) return ERROR_INVALID_DATA;
    std::vector<BYTE> buffer(bytes);
    auto detail=(SP_DRVINFO_DETAIL_DATA_W *)buffer.data(); detail->cbSize=sizeof(*detail);
    if(!SetupDiGetDriverInfoDetailW(info.set,device,&driver,detail,bytes,nullptr)) return GetLastError();
    inf=detail->InfFileName; section=detail->SectionName;
    if(primaryModel) {
        size_t chars=(bytes-FIELD_OFFSET(SP_DRVINFO_DETAIL_DATA_W,HardwareID))/sizeof(WCHAR);
        if(wcsnlen_s(detail->HardwareID,chars)>=chars) return ERROR_INVALID_DATA;
        *primaryModel=detail->HardwareID;
    }
    if(model && modelMatches) {
        *modelMatches=false;
        size_t count=(bytes-FIELD_OFFSET(SP_DRVINFO_DETAIL_DATA_W,HardwareID))/sizeof(WCHAR);
        for(size_t i=0;i<count && detail->HardwareID[i];) {
            size_t n=wcsnlen_s(detail->HardwareID+i,count-i);
            if(n>=count-i) return ERROR_INVALID_DATA;
            if(!_wcsicmp(detail->HardwareID+i,model->c_str())) *modelMatches=true;
            i+=n+1;
        }
    }
    (void)kind; return ERROR_SUCCESS;
}
DWORD Select(Info &info,const std::wstring &inf,const std::wstring &section,const std::wstring &model,SP_DRVINFO_DATA_W &selected)
{
    auto device=info.device.DevInst ? &info.device : nullptr;
    SP_DEVINSTALL_PARAMS_W p{sizeof(p)};
    if(!SetupDiGetDeviceInstallParamsW(info.set,device,&p)) return GetLastError();
    p.Flags|=DI_ENUMSINGLEINF;
    p.FlagsEx|=DI_FLAGSEX_ALLOWEXCLUDEDDRVS|DI_FLAGSEX_NO_CLASSLIST_NODE_MERGE;
    if(wcscpy_s(p.DriverPath,inf.c_str())) return ERROR_FILENAME_EXCED_RANGE;
    if(!SetupDiSetDeviceInstallParamsW(info.set,device,&p) ||
        !SetupDiBuildDriverInfoList(info.set,device,SPDIT_CLASSDRIVER)) return GetLastError();
    for(DWORD i=0;i<4096;++i) {
        SP_DRVINFO_DATA_W driver{sizeof(driver)};
        if(!SetupDiEnumDriverInfoW(info.set,device,SPDIT_CLASSDRIVER,i,&driver)) return GetLastError();
        std::wstring candidateInf,candidateSection;
        bool modelMatches=false;
        DWORD error=DriverDetail(info,SPDIT_CLASSDRIVER,driver,candidateInf,candidateSection,&model,&modelMatches);
        if(error) return error;
        if(!modelMatches) continue;
        // SetupAPI may return the platform-decorated installation section.
        // Resolve the requested base section using the INF's own decoration
        // rules rather than accepting an arbitrary prefix or another model.
        bool match=!_wcsicmp(candidateSection.c_str(),section.c_str());
        HINF file=SetupOpenInfFileW(inf.c_str(),nullptr,INF_STYLE_WIN4,nullptr);
        if(!match && file!=INVALID_HANDLE_VALUE) {
            WCHAR actual[256],candidateActual[256];
            if(SetupDiGetActualSectionToInstallW(file,section.c_str(),actual,_countof(actual),nullptr,nullptr) &&
               SetupDiGetActualSectionToInstallW(file,candidateSection.c_str(),candidateActual,_countof(candidateActual),nullptr,nullptr))
                match=!_wcsicmp(candidateActual,actual);
        }
        if(file!=INVALID_HANDLE_VALUE) SetupCloseInfFile(file);
        if(match && modelMatches) { selected=driver; return ERROR_SUCCESS; }
    }
    return ERROR_NO_DRIVER_SELECTED;
}
DWORD InstalledModel(Info &device,Record &record)
{
    SP_DEVINSTALL_PARAMS_W params{sizeof(params)};
    if(!SetupDiGetDeviceInstallParamsW(device.set,&device.device,&params)) return GetLastError();
    params.FlagsEx|=DI_FLAGSEX_INSTALLEDDRIVER|DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    if(!SetupDiSetDeviceInstallParamsW(device.set,&device.device,&params) ||
       !SetupDiBuildDriverInfoList(device.set,&device.device,SPDIT_CLASSDRIVER)) return GetLastError();
    SP_DRVINFO_DATA_W installed{sizeof(installed)};
    if(!SetupDiEnumDriverInfoW(device.set,&device.device,SPDIT_CLASSDRIVER,0,&installed)) return GetLastError();
    DWORD error=DriverDetail(device,SPDIT_CLASSDRIVER,installed,record.inf,record.section,nullptr,nullptr,&record.model);
    if(!error && (record.inf.empty() || record.section.empty() || record.model.empty())) return ERROR_INVALID_DATA;
    return error;
}
bool MissingDriverProblem(ULONG problem)
{
    return problem==0 || problem==CM_PROB_NOT_CONFIGURED || problem==CM_PROB_FAILED_INSTALL;
}
bool OriginalStateUsable(const Record &record,ULONG status,ULONG problem)
{
    if(record.binding==OriginalBinding::None) return MissingDriverProblem(problem);
    // A valid NULL-service INF can intentionally leave the function absent.
    // An installed package with an error must not masquerade as no driver.
    return !problem && (record.service.empty() || (status & DN_STARTED));
}
// Absence must be established, not inferred from a failed property/API read.
// A service/INF that exists but failed to start is not a driverless device.
DWORD ReadBinding(Info &device,Record &record)
{
    std::wstring inf;
    DWORD error=Property(device.device.DevInst,DEVPKEY_Device_Service,record.service,true);
    if(!error) error=Property(device.device.DevInst,DEVPKEY_Device_DriverInfPath,inf,true);
    if(error) return error;
    // CLASSDRIVER + INSTALLEDDRIVER also identifies manually forced drivers;
    // COMPATDRIVER and MatchingDeviceId can omit them entirely.
    error=InstalledModel(device,record);
    if(record.service.empty() && inf.empty() &&
       (error==ERROR_NO_MORE_ITEMS || error==ERROR_NO_DRIVER_SELECTED)) {
        record.binding=OriginalBinding::None;
        record.inf.clear(); record.section.clear(); record.model.clear();
        return ERROR_SUCCESS;
    }
    if(error) return error;
    if(inf.empty()) return ERROR_INVALID_DATA;
    record.binding=OriginalBinding::Driver;
    return ERROR_SUCCESS;
}
DWORD SetNullDriver(const Record &record)
{
    Info device; DWORD error=device.Open(record.instance); if(error) return error;
    BOOL reboot=FALSE;
    // Without INSTALLNULLDRIVER, a null DriverInfoData asks Windows to select
    // a matching driver, which would NOT restore the original no-driver state.
    if(!DiInstallDevice(nullptr,device.set,&device.device,nullptr,
        DIIDFLAG_INSTALLNULLDRIVER|DIIDFLAG_NOFINISHINSTALLUI,&reboot)) return GetLastError();
    if(reboot) return ERROR_SUCCESS_REBOOT_REQUIRED;
    return ERROR_SUCCESS;
}
DWORD SetDriver(const Record &record,const std::wstring &inf,const std::wstring &section,bool native)
{
    const std::wstring model=native ? record.model : L"USBWarp\\Capture";
    // Validate the package/model before changing anything. Driver selection is
    // per DEVINST; never UpdateDriverForPlugAndPlayDevices (all matching IDs).
    { Info probe; SP_DRVINFO_DATA_W selected{sizeof(selected)};
      // Validate independently of the CURRENT setup class. A USBDevice capture
      // INF need not belong to the target's original HID/USB/etc setup class.
      probe.set=SetupDiCreateDeviceInfoList(nullptr,nullptr);
      if(probe.set==INVALID_HANDLE_VALUE) return GetLastError();
      DWORD e=Select(probe,inf,section,model,selected); if(e) return e; }
    BOOL reboot=FALSE;
    DWORD clear=SetNullDriver(record); if(clear) return clear;
    // Explicitly inherit the validated global class list. NULL-driver removal
    // may leave the previous setup class on the devnode; rebuilding a class
    // list on that element would silently exclude the destination class.
    Info device;
    device.set=SetupDiCreateDeviceInfoList(nullptr,nullptr);
    if(device.set==INVALID_HANDLE_VALUE) return GetLastError();
    SP_DRVINFO_DATA_W selected{sizeof(selected)};
    DWORD error=Select(device,inf,section,model,selected); if(error) return error;
    if(!SetupDiSetSelectedDriverW(device.set,nullptr,&selected) ||
       !SetupDiOpenDeviceInfoW(device.set,record.instance.c_str(),nullptr,DIOD_INHERIT_CLASSDRVS,&device.device) ||
       !SetupDiGetSelectedDriverW(device.set,&device.device,&selected)) return GetLastError();
    SP_DEVINSTALL_PARAMS_W params{sizeof(params)};
    if(!SetupDiGetDeviceInstallParamsW(device.set,&device.device,&params)) return GetLastError();
    params.Flags|=DI_QUIETINSTALL; params.FlagsEx|=DI_FLAGSEX_NOUIONQUERYREMOVE;
    if(!SetupDiSetDeviceInstallParamsW(device.set,&device.device,&params)) return GetLastError();
    error=SetFilters(device,SPDRP_UPPERFILTERS,native ? record.upper : std::vector<std::wstring>{});
    if(!error) error=SetFilters(device,SPDRP_LOWERFILTERS,native ? record.lower : std::vector<std::wstring>{});
    if(error) return error;
    if(!DiInstallDevice(nullptr,device.set,&device.device,&selected,DIIDFLAG_NOFINISHINSTALLUI,&reboot)) return GetLastError();
    if(reboot) return ERROR_SUCCESS_REBOOT_REQUIRED;
    // INF actions may replace optional per-device filters. Restore exactly the
    // original settings before the final port cycle rebuilds the native stack.
    if(native) {
        error=SetFilters(device,SPDRP_UPPERFILTERS,record.upper);
        if(!error) error=SetFilters(device,SPDRP_LOWERFILTERS,record.lower);
    }
    return error;
}
DWORD Interfaces(const GUID &guid,const std::wstring &id,std::vector<WCHAR> &paths)
{
    for(unsigned retry=0;retry<3;++retry) {
        ULONG chars=0;
        CONFIGRET cr=CM_Get_Device_Interface_List_SizeW(&chars,const_cast<GUID *>(&guid),const_cast<WCHAR *>(id.c_str()),CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if(cr!=CR_SUCCESS) return Error(cr);
        if(chars<2 || chars>65536) return ERROR_NOT_FOUND;
        paths.resize(chars);
        cr=CM_Get_Device_Interface_ListW(const_cast<GUID *>(&guid),const_cast<WCHAR *>(id.c_str()),paths.data(),chars,CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if(cr==CR_SUCCESS) return paths[0] ? ERROR_SUCCESS : ERROR_NOT_FOUND;
        if(cr!=CR_BUFFER_SMALL) return Error(cr);
    }
    return ERROR_RETRY;
}
struct Port {
    HANDLE hub=INVALID_HANDLE_VALUE; ULONG index=0;
    ~Port() { if(hub!=INVALID_HANDLE_VALUE) CloseHandle(hub); }
    DWORD Open(const std::wstring &instance) {
        DEVINST device,parent;
        CONFIGRET cr=CM_Locate_DevNodeW(&device,const_cast<WCHAR *>(instance.c_str()),CM_LOCATE_DEVNODE_NORMAL);
        if(cr!=CR_SUCCESS) return ERROR_DEVICE_NOT_CONNECTED;
        std::wstring driver;
        DWORD error=Property(device,DEVPKEY_Device_Driver,driver,true); if(error) return error;
        // For a USB PDO, the bus-reported Address is its parent hub's port
        // number (DEVICE_CAPABILITIES.Address), even without a function driver.
        ULONG address=0,bytes=sizeof(address); DEVPROPTYPE type;
        cr=CM_Get_DevNode_PropertyW(device,&DEVPKEY_Device_Address,&type,(PBYTE)&address,&bytes,0);
        if(cr!=CR_SUCCESS) return Error(cr);
        if(type!=DEVPROP_TYPE_UINT32 || bytes!=sizeof(address) || !address || address>255) return ERROR_INVALID_DATA;
        cr=CM_Get_Parent(&parent,device,0); if(cr!=CR_SUCCESS) return Error(cr);
        WCHAR parentId[MAX_DEVICE_ID_LEN];
        cr=CM_Get_Device_IDW(parent,parentId,_countof(parentId),0); if(cr!=CR_SUCCESS) return Error(cr);
        std::vector<WCHAR> paths;
        error=Interfaces(GUID_DEVINTERFACE_USB_HUB,parentId,paths); if(error) return error;
        hub=CreateFileW(paths.data(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(hub==INVALID_HANDLE_VALUE) return GetLastError();
        USB_NODE_INFORMATION node{}; node.NodeType=UsbHub; DWORD returned;
        if(!DeviceIoControl(hub,IOCTL_USB_GET_NODE_INFORMATION,&node,sizeof(node),&node,sizeof(node),&returned,nullptr)) return GetLastError();
        if(address>node.u.HubInformation.HubDescriptor.bNumberOfPorts) return ERROR_INVALID_DATA;
        // A present driver key is an additional identity check. Never search
        // for an empty key: multiple unconfigured ports can report one.
        if(!driver.empty()) {
            alignas(8) BYTE buffer[4096]{};
            auto key=(USB_NODE_CONNECTION_DRIVERKEY_NAME *)buffer; key->ConnectionIndex=address;
            if(!DeviceIoControl(hub,IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME,buffer,sizeof(buffer),buffer,sizeof(buffer),&returned,nullptr)) return GetLastError();
            if(key->ActualLength>sizeof(buffer) || key->ActualLength<FIELD_OFFSET(USB_NODE_CONNECTION_DRIVERKEY_NAME,DriverKeyName)+2 ||
               wcsnlen_s(key->DriverKeyName,(sizeof(buffer)-FIELD_OFFSET(USB_NODE_CONNECTION_DRIVERKEY_NAME,DriverKeyName))/2)>=512 ||
               _wcsicmp(key->DriverKeyName,driver.c_str())) return ERROR_DEVICE_NOT_CONNECTED;
        }
        index=address;
        alignas(8) BYTE connectionBuffer[sizeof(USB_NODE_CONNECTION_INFORMATION_EX)+32*sizeof(USB_PIPE_INFO)]{};
        auto connection=(USB_NODE_CONNECTION_INFORMATION_EX *)connectionBuffer; connection->ConnectionIndex=index;
        if(!DeviceIoControl(hub,IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,connection,sizeof(connectionBuffer),connection,sizeof(connectionBuffer),&returned,nullptr)) return GetLastError();
        if(connection->ConnectionStatus!=DeviceConnected) return ERROR_DEVICE_NOT_CONNECTED;
        unsigned vendor=0,product=0;
        std::wstring upper=instance; for(auto &ch:upper) ch=towupper(ch);
        if(swscanf_s(upper.c_str(),L"USB\\VID_%4x&PID_%4x",&vendor,&product)!=2 ||
           connection->DeviceDescriptor.idVendor!=vendor || connection->DeviceDescriptor.idProduct!=product) return ERROR_DEVICE_NOT_CONNECTED;
        // Capturing a hub would take every attached device away from Windows.
        if(connection->DeviceIsHub || connection->DeviceDescriptor.bDeviceClass==USB_DEVICE_CLASS_HUB) return ERROR_NOT_SUPPORTED;
        return ERROR_SUCCESS;
    }
};
}

DWORD Present(const std::wstring &instance)
{
    DEVINST node;
    return CM_Locate_DevNodeW(&node,const_cast<WCHAR *>(instance.c_str()),CM_LOCATE_DEVNODE_NORMAL)==CR_SUCCESS ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD Identity(const Record &record)
{
    DEVINST node;
    if(CM_Locate_DevNodeW(&node,const_cast<WCHAR *>(record.instance.c_str()),CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS) return ERROR_DEVICE_NOT_CONNECTED;
    GUID container{}; ULONG bytes=sizeof(container); DEVPROPTYPE type;
    if(CM_Get_DevNode_PropertyW(node,&DEVPKEY_Device_ContainerId,&type,(PBYTE)&container,&bytes,0)!=CR_SUCCESS ||
        type!=DEVPROP_TYPE_GUID || bytes!=sizeof(container) || !IsEqualGUID(container,record.container)) return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}
DWORD Resolve(const uint8_t guid[16],const WCHAR *target,Record &record)
{
    std::wstring path=target;
    WCHAR id[MAX_DEVICE_ID_LEN]; DEVPROPTYPE type; ULONG bytes=sizeof(id);
    CONFIGRET cr;
    if(!_wcsnicmp(path.c_str(),L"USB\\VID_",8)) {
        if(path.size()>=MAX_DEVICE_ID_LEN) return ERROR_INVALID_NAME;
        record.instance=path; // a present devnode may have no enabled interface
    } else {
        if(path.size()<8 || path.substr(0,4)!=L"\\??\\") return ERROR_INVALID_NAME;
        path[1]=L'\\';
        cr=CM_Get_Device_Interface_PropertyW(path.c_str(),&DEVPKEY_Device_InstanceId,&type,(PBYTE)id,&bytes,0);
        if(cr!=CR_SUCCESS) return Error(cr);
        if(type!=DEVPROP_TYPE_STRING || bytes<2 || bytes>sizeof(id) || bytes%2 || id[bytes/2-1]) return ERROR_INVALID_DATA;
        record.instance=id;
    }
    // Only whole physical devices. A composite interface is never a target.
    std::wstring upper=record.instance;
    for(auto &ch:upper) ch=towupper(ch);
    if(upper.compare(0,8,L"USB\\VID_") || upper.find(L"&MI_")!=std::wstring::npos) return ERROR_NOT_SUPPORTED;
    Info device; DWORD error=device.Open(record.instance); if(error) return error;
    GUID container{}; bytes=sizeof(container);
    cr=CM_Get_DevNode_PropertyW(device.device.DevInst,&DEVPKEY_Device_ContainerId,&type,(PBYTE)&container,&bytes,0);
    if(cr!=CR_SUCCESS || type!=DEVPROP_TYPE_GUID || bytes!=16 || memcmp(guid,&container,16)) return ERROR_INVALID_PARAMETER;
    record.container=container;
    ULONG status,problem;
    cr=CM_Get_DevNode_Status(&status,&problem,device.device.DevInst,0);
    if(cr!=CR_SUCCESS) return Error(cr);
    error=ReadBinding(device,record); if(error) return error;
    if(!OriginalStateUsable(record,status,problem)) return ERROR_DEVICE_NOT_AVAILABLE;
    if(!(status & DN_DISABLEABLE)) return ERROR_NOT_SUPPORTED;
    // The OS's PnP query-remove handles vetoes for every function class.
    Port port; error=port.Open(record.instance); if(error) return error;
    if(!_wcsicmp(record.service.c_str(),L"UsbWarpFilter")) return ERROR_BUSY;
    error=Filters(device,SPDRP_UPPERFILTERS,record.upper);
    if(!error) error=Filters(device,SPDRP_LOWERFILTERS,record.lower);
    return error;
}
static DWORD PackagePath(std::wstring &path)
{
    WCHAR exe[MAX_PATH]; DWORD length=GetModuleFileNameW(nullptr,exe,_countof(exe));
    if(!length || length>=_countof(exe)) return ERROR_BAD_PATHNAME;
    path=exe; auto slash=path.find_last_of(L"\\/");
    if(slash==std::wstring::npos) return ERROR_BAD_PATHNAME;
    path.resize(slash+1); path+=L"capture\\UsbWarpFilter.inf";
    return ERROR_SUCCESS;
}
DWORD Preflight()
{
    std::wstring path; DWORD error=PackagePath(path); if(error) return error;
    Info info; info.set=SetupDiCreateDeviceInfoList(nullptr,nullptr);
    if(info.set==INVALID_HANDLE_VALUE) return GetLastError();
    SP_DRVINFO_DATA_W driver{sizeof(driver)};
    return Select(info,path,L"Capture",L"USBWarp\\Capture",driver);
}
DWORD Install(Record &record)
{
    std::wstring path; DWORD error=PackagePath(path); if(error) return error;
    // The signed package is staged by the installer; this also checks that the
    // selected local package is still available before touching the device.
    return SetDriver(record,path,L"Capture",false);
}
DWORD RestoreDriver(const Record &record) {
    DWORD error=Identity(record);
    if(error) return error;
    if(record.binding==OriginalBinding::Driver) return SetDriver(record,record.inf,record.section,true);
    error=SetNullDriver(record); if(error) return error;
    Info device; error=device.Open(record.instance);
    if(!error) error=SetFilters(device,SPDRP_UPPERFILTERS,record.upper);
    if(!error) error=SetFilters(device,SPDRP_LOWERFILTERS,record.lower);
    return error;
}
DWORD Cycle(const std::wstring &instance)
{
    Port port; DWORD error=port.Open(instance); if(error) return error;
    struct Reset {
        HCMNOTIFICATION notification=nullptr;
        HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        volatile LONG removed=0;
        ~Reset() { if(notification) CM_Unregister_Notification(notification); if(ready) CloseHandle(ready); }
        static DWORD CALLBACK Notify(HCMNOTIFICATION,void *context,CM_NOTIFY_ACTION action,
                                      PCM_NOTIFY_EVENT_DATA,DWORD) {
            auto state=(Reset *)context;
            if(action==CM_NOTIFY_ACTION_DEVICEINSTANCEREMOVED) InterlockedExchange(&state->removed,1);
            if(action==CM_NOTIFY_ACTION_DEVICEINSTANCESTARTED && InterlockedCompareExchange(&state->removed,0,0))
                SetEvent(state->ready);
            return ERROR_SUCCESS;
        }
    } reset;
    if(!reset.ready) return GetLastError();
    CM_NOTIFY_FILTER filter{}; filter.cbSize=sizeof(filter);
    filter.FilterType=CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE;
    if(wcscpy_s(filter.u.DeviceInstance.InstanceId,instance.c_str())) return ERROR_INVALID_NAME;
    CONFIGRET cr=CM_Register_Notification(&filter,&reset,Reset::Notify,&reset.notification);
    if(cr!=CR_SUCCESS) return Error(cr);
    USB_CYCLE_PORT_PARAMS cycle{}; cycle.ConnectionIndex=port.index; DWORD returned;
    if(!DeviceIoControl(port.hub,IOCTL_USB_HUB_CYCLE_PORT,&cycle,sizeof(cycle),&cycle,sizeof(cycle),&returned,nullptr)) return GetLastError();
    if(cycle.StatusReturned) return ERROR_GEN_FAILURE;
    // Hub IOCTL completion only means the cycle was requested. Wait for this
    // exact instance to disappear AND start again; an old enabled interface
    // must never be mistaken for the new PnP incarnation.
    DWORD wait=WaitForSingleObject(reset.ready,15000);
    if(wait==WAIT_OBJECT_0) return ERROR_SUCCESS;
    if(wait==WAIT_FAILED) return GetLastError();
    return Present(instance) ? ERROR_DEVICE_NOT_CONNECTED : ERROR_TIMEOUT;
}
DWORD Verify(const Record &record,bool captured,std::wstring *path)
{
    DEVINST node;
    if(CM_Locate_DevNodeW(&node,const_cast<WCHAR *>(record.instance.c_str()),CM_LOCATE_DEVNODE_NORMAL)!=CR_SUCCESS) return ERROR_DEVICE_NOT_CONNECTED;
    GUID container{}; ULONG bytes=sizeof(container); DEVPROPTYPE type;
    if(CM_Get_DevNode_PropertyW(node,&DEVPKEY_Device_ContainerId,&type,(PBYTE)&container,&bytes,0)!=CR_SUCCESS ||
        type!=DEVPROP_TYPE_GUID || bytes!=sizeof(container) || !IsEqualGUID(container,record.container)) return ERROR_INVALID_DATA;
    ULONG status,problem; CONFIGRET cr=CM_Get_DevNode_Status(&status,&problem,node,0);
    if(cr!=CR_SUCCESS) return Error(cr);
    if(captured) {
        if(problem || !(status & DN_STARTED)) return ERROR_NOT_READY;
    } else if(!OriginalStateUsable(record,status,problem)) return ERROR_NOT_READY;
    std::wstring service; DWORD error=Property(node,DEVPKEY_Device_Service,service,true); if(error) return error;
    if(_wcsicmp(service.c_str(),captured ? L"UsbWarpFilter" : record.service.c_str())) return ERROR_NOT_READY;
    if(captured) {
        std::vector<WCHAR> paths; error=Interfaces(GUID_USBWARP_FILTER_INTERFACE,record.instance,paths); if(error) return error;
        if(path) { *path=paths.data(); (*path)[1]=L'?'; } // Win32 link -> NT link
    } else {
        Info info; Record installed;
        error=info.Open(record.instance);
        if(!error) error=ReadBinding(info,installed);
        if(!error) error=Filters(info,SPDRP_UPPERFILTERS,installed.upper);
        if(!error) error=Filters(info,SPDRP_LOWERFILTERS,installed.lower);
        if(error) return error;
        if(installed.binding!=record.binding || _wcsicmp(installed.inf.c_str(),record.inf.c_str()) ||
           _wcsicmp(installed.section.c_str(),record.section.c_str()) ||
           _wcsicmp(installed.model.c_str(),record.model.c_str()) ||
           installed.upper!=record.upper || installed.lower!=record.lower) return ERROR_NOT_READY;
    }
    return ERROR_SUCCESS;
}
}
