// SPDX-License-Identifier: GPL-2.0-only
// Recoverable, class-independent target capture. The registry is an undo
// journal only; binding ownership is ALWAYS obtained from UsbWarp.sys.
#include "warp_service.h"
#include "capture_pnp.h"
#include <vector>
#include <string>

namespace {
constexpr WCHAR CaptureKey[]=USBWARP_SERVICE_REG_KEY L"\\DeviceCapture";
using capture::Record;
using capture::Phase;
struct Lock {
    SRWLOCK *value;
    explicit Lock(SRWLOCK *p):value(p) { AcquireSRWLockExclusive(value); }
    ~Lock() { ReleaseSRWLockExclusive(value); }
};
struct Key { HKEY value=nullptr; ~Key() { if(value) RegCloseKey(value); } };
std::wstring Name(const uint8_t guid[16])
{
    GUID id; memcpy(&id,guid,16); WCHAR name[40];
    StringFromGUID2(id,name,_countof(name)); return name;
}
DWORD Names(std::vector<std::wstring> &names)
{
    Key key;
    DWORD error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,CaptureKey,0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key.value);
    if(error==ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if(error) return error;
    for(DWORD i=0;;++i) {
        WCHAR name[40]; DWORD length=_countof(name);
        error=RegEnumValueW(key.value,i,name,&length,nullptr,nullptr,nullptr,nullptr);
        if(error==ERROR_NO_MORE_ITEMS) return ERROR_SUCCESS;
        if(error) return error;
        GUID id;
        if(FAILED(CLSIDFromString(name,&id))) return ERROR_INVALID_DATA;
        names.emplace_back(name,length);
    }
}
DWORD ReadRecord(const std::wstring &name,Record &record)
{
    Key key; DWORD error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,CaptureKey,0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key.value);
    if(error) return error;
    DWORD bytes=0;
    error=RegGetValueW(key.value,nullptr,name.c_str(),RRF_RT_REG_MULTI_SZ,nullptr,nullptr,&bytes);
    if(error) return error;
    if(bytes<4 || bytes>65536 || bytes%2) return ERROR_INVALID_DATA;
    std::vector<WCHAR> data(bytes/2);
    error=RegGetValueW(key.value,nullptr,name.c_str(),RRF_RT_REG_MULTI_SZ,nullptr,data.data(),&bytes);
    if(error) return error;
    if(data.back() || data[data.size()-2]) return ERROR_INVALID_DATA;
    unsigned fields=0,version=0;
    record=Record{};
    for(size_t offset=0;offset+1<data.size() && data[offset];) {
        size_t length=wcsnlen_s(data.data()+offset,data.size()-offset);
        if(length>=1024 || offset+length>=data.size()) return ERROR_INVALID_DATA;
        std::wstring v(data.data()+offset,length); offset+=length+1;
        unsigned field=0;
        if(v==L"version=2" || v==L"version=3") { field=1; version=(unsigned)(v.back()-L'0'); }
        else if(v==L"binding=none" || v==L"binding=driver") {
            field=128;
            record.binding=v==L"binding=none" ? capture::OriginalBinding::None : capture::OriginalBinding::Driver;
        }
        else if(v.compare(0,3,L"id=")==0) { field=2; record.instance=v.substr(3); }
        else if(v.compare(0,4,L"inf=")==0) { field=4; record.inf=v.substr(4); }
        else if(v.compare(0,8,L"section=")==0) { field=8; record.section=v.substr(8); }
        else if(v.compare(0,8,L"service=")==0) { field=16; record.service=v.substr(8); }
        else if(v.compare(0,6,L"model=")==0) { field=64; record.model=v.substr(6); }
        else if(v.size()==7 && v.compare(0,6,L"phase=")==0 && v[6]>=L'0' && v[6]<=L'4') {
            field=32; record.phase=(Phase)(v[6]-L'0');
        } else if(v.compare(0,2,L"u=")==0 && v.size()>2) record.upper.push_back(v.substr(2));
        else if(v.compare(0,2,L"l=")==0 && v.size()>2) record.lower.push_back(v.substr(2));
        else return ERROR_INVALID_DATA;
        if(fields & field) return ERROR_INVALID_DATA;
        fields|=field;
    }
    if(fields!=(version==2 ? 127u : 255u) || record.instance.empty() ||
        FAILED(CLSIDFromString(name.c_str(),&record.container))) return ERROR_INVALID_DATA;
    if(record.binding==capture::OriginalBinding::None) {
        if(!record.inf.empty() || !record.section.empty() || !record.service.empty() || !record.model.empty()) return ERROR_INVALID_DATA;
    } else if(record.inf.empty() || record.section.empty() || record.model.empty() ||
              (version==2 && record.service.empty())) return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}
DWORD SaveRecord(const std::wstring &name,const Record &record)
{
    std::vector<WCHAR> data;
    auto append=[&](const std::wstring &v) { data.insert(data.end(),v.begin(),v.end()); data.push_back(0); };
    append(L"version=3");
    append(record.binding==capture::OriginalBinding::None ? L"binding=none" : L"binding=driver");
    append(L"id="+record.instance); append(L"inf="+record.inf);
    append(L"section="+record.section); append(L"service="+record.service);
    append(L"model="+record.model);
    append(L"phase="+std::to_wstring((int)record.phase));
    for(const auto &v:record.upper) append(L"u="+v);
    for(const auto &v:record.lower) append(L"l="+v);
    data.push_back(0);
    if(data.size()*2>65536) return ERROR_BUFFER_OVERFLOW;
    Key key;
    DWORD error=RegCreateKeyExW(HKEY_LOCAL_MACHINE,CaptureKey,0,nullptr,0,
        KEY_SET_VALUE|KEY_QUERY_VALUE|KEY_WOW64_64KEY,nullptr,&key.value,nullptr);
    if(!error) error=RegSetValueExW(key.value,name.c_str(),0,REG_MULTI_SZ,(const BYTE *)data.data(),(DWORD)(data.size()*2));
    // Flush before each irreversible PnP transition: process death/power loss
    // must leave sufficient information to reinstall the exact original INF.
    if(!error) error=RegFlushKey(key.value);
    return error;
}
DWORD Forget(const std::wstring &name)
{
    Key key; DWORD error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,CaptureKey,0,KEY_SET_VALUE|KEY_WOW64_64KEY,&key.value);
    if(!error) error=RegDeleteValueW(key.value,name.c_str());
    return error==ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
}
DWORD WaitDriver(const Record &record,bool captured,std::wstring *path=nullptr)
{
    ULONGLONG deadline=GetTickCount64()+15000;
    DWORD error;
    do {
        error=capture::Verify(record,captured,path);
        if(!error || error==ERROR_INVALID_DATA) return error;
        Sleep(100);
    } while(GetTickCount64()<deadline);
    return error;
}
DWORD Restore(const std::wstring &name)
{
    Record record; DWORD error=ReadRecord(name,record);
    if(error==ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if(error) return error;
    if(record.phase==Phase::Prepared) return Forget(name); // no PnP mutations yet
    error=capture::Identity(record); if(error) return error;
    if(record.phase!=Phase::Native) {
        record.phase=Phase::Restoring;
        error=SaveRecord(name,record); if(error) return error;
        // Reset while the capture driver still owns the function, BEFORE the
        // original function driver attempts configuration or class commands.
        if(!capture::Verify(record,true)) {
            error=capture::Cycle(record.instance); if(error) return error;
            error=WaitDriver(record,true); if(error) return error;
        }
        error=capture::RestoreDriver(record);
        if(error) { LogWarn("Restoring original USB device state failed: %lu",error); return error; }
        record.phase=Phase::Native;
        error=SaveRecord(name,record); if(error) return error;
    }
    error=WaitDriver(record,false); if(error) return error;
    // A no-function-driver destination was already reset while the capture
    // driver was present. Do not wait for a function driver that does not exist
    // or trigger automatic driver matching after restoring the null binding.
    if(record.HasFunctionDriver()) {
        error=capture::Cycle(record.instance); if(error) return error;
        error=WaitDriver(record,false); if(error) return error;
    }
    return Forget(name);
}
bool Listed(const USBWARP_ENUM_DEVICES_OUT &list,const uint8_t guid[16])
{
    for(uint32_t i=0;i<list.DeviceCount;++i) if(!memcmp(list.Devices[i].DeviceGuid,guid,16)) return true;
    return false;
}
DWORD RestoreAll(const USBWARP_ENUM_DEVICES_OUT *active)
{
    std::vector<std::wstring> names; DWORD error=Names(names),first=error;
    if(error) return error;
    for(const auto &name:names) {
        GUID guid; if(FAILED(CLSIDFromString(name.c_str(),&guid))) return ERROR_INVALID_DATA;
        if(active && Listed(*active,(uint8_t *)&guid)) continue;
        error=Restore(name);
        // An unplugged instance keeps its undo record and is restored after
        // reconnection. It must not prevent other devices starting a session.
        if(error && error!=ERROR_DEVICE_NOT_CONNECTED && !first) first=error;
    }
    return first;
}
DWORD CheckLegacyJournal()
{
    Key key; DWORD error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,USBWARP_SERVICE_REG_KEY L"\\StorageCapture",0,
        KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key.value);
    if(error==ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if(error) return error;
    DWORD count=0;
    error=RegQueryInfoKeyW(key.value,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&count,nullptr,nullptr,nullptr,nullptr);
    if(!error && count) {
        LogError("Legacy storage capture is still pending; restore devices with the previous service before upgrading");
        return ERROR_REVISION_MISMATCH;
    }
    return error;
}
}

BOOL DeviceBind(warp_session *s,const uint8_t guid[16],const WCHAR *path,uint8_t speed,
                uint32_t *index,uint16_t *vid,uint16_t *pid)
{
    Lock lock(&s->captureLock);
    if (!VmCanBind(s)) { SetLastError(ERROR_NOT_READY); return FALSE; }
    USBWARP_ENUM_DEVICES_OUT active{};
    if(!DrvClientEnumDevices(s,&active)) return FALSE;
    if(Listed(active,guid)) { SetLastError(ERROR_ALREADY_EXISTS); return FALSE; }
    std::wstring name=Name(guid),capturedPath;
    Record record;
    const char *stage="restore previous capture";
    DWORD error=Restore(name);
    if(!error) { stage="resolve target"; error=capture::Resolve(guid,path,record); }
    if(!error) { stage="validate capture package"; error=capture::Preflight(); }
    if(!error) error=SaveRecord(name,record);
    if(!error) { record.phase=Phase::Switching; error=SaveRecord(name,record); }
    if(!error) { stage="install capture driver"; error=capture::Install(record); }
    if(!error) { stage="start capture driver"; error=WaitDriver(record,true); }
    if(!error) { stage="reset for capture"; error=capture::Cycle(record.instance); }
    if(!error) { stage="wait for reset"; error=WaitDriver(record,true,&capturedPath); }
    if(!error) { record.phase=Phase::Captured; error=SaveRecord(name,record); }
    if(!error) {
        stage="claim kernel capture lease";
        if(DrvClientBindGuid(s,guid,capturedPath.c_str(),speed,index,vid,pid)) return TRUE;
    }
    if(!error) error=GetLastError();
    LogWarn("USB capture failed at %s: %lu; restoring original Windows device state",stage,error);
    // A malformed success reply may still represent a committed binding.
    // Never return endpoints to Windows until kernel ownership is gone.
    DrvClientUnbindGuid(s,guid);
    if(DrvClientEnumDevices(s,&active) && !Listed(active,guid)) {
        DWORD restore=Restore(name);
        if(restore) LogError("USB driver restoration pending: %lu",restore);
    }
    SetLastError(error); return FALSE;
}
BOOL DeviceUnbind(warp_session *s,const uint8_t guid[16])
{
    Lock lock(&s->captureLock);
    if(!DrvClientUnbindGuid(s,guid)) {
        DWORD error=GetLastError(); USBWARP_ENUM_DEVICES_OUT active{};
        if(!DrvClientEnumDevices(s,&active)) return FALSE;
        if(Listed(active,guid)) { SetLastError(error); return FALSE; }
    }
    DWORD error=Restore(Name(guid)); SetLastError(error); return !error;
}
BOOL DeviceRecover(warp_session *s)
{
    Lock lock(&s->captureLock);
    DWORD error=CheckLegacyJournal();
    if(!error) error=RestoreAll(nullptr);
    SetLastError(error); return !error;
}
DWORD DeviceCloseSession(warp_session *s)
{
    Lock lock(&s->captureLock);
    AcquireSRWLockShared(&s->driverLock); bool owned=s->driverRegistered; ReleaseSRWLockShared(&s->driverLock);
    DWORD error=DrvClientClose(s);
    if(!error && owned) error=RestoreAll(nullptr);
    return error;
}
void DeviceReconcile(warp_session *s)
{
    if(!TryAcquireSRWLockExclusive(&s->captureLock)) return;
    std::vector<std::wstring> names;
    if(!Names(names) && !names.empty()) {
        USBWARP_ENUM_DEVICES_OUT active{};
        if(DrvClientEnumDevices(s,&active)) {
            DWORD error=RestoreAll(&active);
            if(error) LogWarn("Windows USB driver restoration pending: %lu",error);
        }
    }
    ReleaseSRWLockExclusive(&s->captureLock);
}
DWORD DeviceCapturePending(bool *pending)
{
    std::vector<std::wstring> names;
    DWORD error=CheckLegacyJournal(); if(!error) error=Names(names);
    *pending=!names.empty(); return error;
}
