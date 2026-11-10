// SPDX-License-Identifier: GPL-2.0-only
// Exercise the production capture transaction and journal with in-memory
// registry/PnP/kernel adapters. Never touches a real device or registry key.
#include "../USBWarpService/warp_service.h"
#include "../USBWarpService/capture_pnp.h"
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#pragma comment(lib,"ole32.lib")
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static std::map<std::wstring,std::vector<BYTE>> journal;
static int writes,failWrite,flushes,failFlush;
static bool present,native,bound,badIdentity,enumFails,bindReplyFails,unbindFails,closeFails;
static bool originallyDriverless;
static int installs,restores,cycles;
static DWORD installFailure,cycleFailure;
static DWORD preflightFailure;
static GUID testGuid{0x12345678,0x1234,0x5678,{1,2,3,4,5,6,7,8}};
static std::vector<std::string> events;
static LSTATUS WINAPI OpenKey(HKEY,LPCWSTR path,DWORD,REGSAM,PHKEY key) {
    if(wcsstr(path,L"StorageCapture")) return ERROR_FILE_NOT_FOUND;
    *key=(HKEY)1; return ERROR_SUCCESS;
}
static LSTATUS WINAPI CreateKey(HKEY,LPCWSTR,DWORD,LPWSTR,DWORD,REGSAM,const SECURITY_ATTRIBUTES *,PHKEY key,LPDWORD) { *key=(HKEY)1; return 0; }
static LSTATUS WINAPI CloseKey(HKEY) { return 0; }
static LSTATUS WINAPI Flush(HKEY) { ++flushes; return flushes==failFlush ? ERROR_WRITE_FAULT : 0; }
static LSTATUS WINAPI SetValue(HKEY,LPCWSTR name,DWORD,DWORD type,const BYTE *data,DWORD bytes) {
    CHECK(type==REG_MULTI_SZ); ++writes;
    if(writes==failWrite) return ERROR_WRITE_FAULT;
    journal[name]=std::vector<BYTE>(data,data+bytes); return 0;
}
static LSTATUS WINAPI GetValue(HKEY,LPCWSTR,LPCWSTR name,DWORD,LPDWORD,void *out,LPDWORD bytes) {
    auto it=journal.find(name); if(it==journal.end()) return ERROR_FILE_NOT_FOUND;
    DWORD needed=(DWORD)it->second.size();
    if(!out) { *bytes=needed; return 0; }
    if(*bytes<needed) { *bytes=needed; return ERROR_MORE_DATA; }
    memcpy(out,it->second.data(),needed); *bytes=needed; return 0;
}
static LSTATUS WINAPI DeleteValue(HKEY,LPCWSTR name) { return journal.erase(name) ? 0 : ERROR_FILE_NOT_FOUND; }
static LSTATUS WINAPI EnumValue(HKEY,DWORD index,LPWSTR name,LPDWORD length,LPDWORD,LPDWORD,LPBYTE,LPDWORD) {
    if(index>=journal.size()) return ERROR_NO_MORE_ITEMS;
    auto it=journal.begin(); std::advance(it,index);
    if(*length<=it->first.size()) return ERROR_MORE_DATA;
    wcscpy_s(name,*length,it->first.c_str()); *length=(DWORD)it->first.size(); return 0;
}
static ULONGLONG WINAPI Tick() { static ULONGLONG now=0; return now+=10000; }
static VOID WINAPI NoSleep(DWORD) {}
#define RegOpenKeyExW OpenKey
#define RegCreateKeyExW CreateKey
#define RegCloseKey CloseKey
#define RegFlushKey Flush
#define RegSetValueExW SetValue
#define RegGetValueW GetValue
#define RegDeleteValueW DeleteValue
#define RegEnumValueW EnumValue
#define GetTickCount64 Tick
#define Sleep NoSleep
#include "../USBWarpService/device_capture.cpp"
bool VmCanBind(warp_session *) { return true; } // target admission is tested separately
#undef RegOpenKeyExW
#undef RegCreateKeyExW
#undef RegCloseKey
#undef RegFlushKey
#undef RegSetValueExW
#undef RegGetValueW
#undef RegDeleteValueW
#undef RegEnumValueW
#undef GetTickCount64
#undef Sleep
void LogWarn(const char *,...) {}
void LogError(const char *,...) {}
namespace capture {
DWORD Preflight() { return preflightFailure; }
DWORD Resolve(const uint8_t *guid,const WCHAR *,Record &r) {
    CHECK(!memcmp(guid,&testGuid,16));
    r.instance=L"USB\\VID_1234&PID_5678\\SERIAL"; r.inf=L"C:\\Windows\\INF\\original.inf";
    r.section=L"Original"; r.service=L"Original"; r.model=L"USB\\Class_08&SubClass_06&Prot_50"; r.container=testGuid;
    if(originallyDriverless) {
        r.binding=OriginalBinding::None;
        r.inf.clear(); r.section.clear(); r.service.clear(); r.model.clear();
    }
    r.upper={L"OriginalUpper"}; r.lower={L"OriginalLower"}; return 0;
}
DWORD Install(Record &) { ++installs; CHECK(!bound); native=false; events.push_back("capture"); return installFailure; }
DWORD RestoreDriver(const Record &r) {
    ++restores; CHECK(!bound); CHECK(r.upper==std::vector<std::wstring>{L"OriginalUpper"});
    CHECK(r.lower==std::vector<std::wstring>{L"OriginalLower"});
    if(originallyDriverless) {
        CHECK(r.binding==OriginalBinding::None);
        CHECK(r.inf.empty() && r.section.empty() && r.service.empty() && r.model.empty());
    } else {
        CHECK(r.binding==OriginalBinding::Driver && r.section==L"Original");
        CHECK(r.model==L"USB\\Class_08&SubClass_06&Prot_50");
        CHECK(r.inf==L"C:\\Windows\\INF\\original.inf");
    }
    native=true; events.push_back("restore"); return 0;
}
DWORD Cycle(const std::wstring &) {
    ++cycles; CHECK(!bound); events.push_back("reset");
    DWORD error=cycleFailure; cycleFailure=0; return error;
}
DWORD Verify(const Record &,bool captured,std::wstring *path) {
    if(!present) return ERROR_DEVICE_NOT_CONNECTED;
    if(badIdentity) return ERROR_INVALID_DATA;
    if(captured==native) return ERROR_NOT_READY;
    if(path) *path=L"\\??\\USB#captured";
    return 0;
}
DWORD Present(const std::wstring &) { return present ? 0 : ERROR_DEVICE_NOT_CONNECTED; }
DWORD Identity(const Record &) { return badIdentity ? ERROR_INVALID_DATA : (present ? 0 : ERROR_DEVICE_NOT_CONNECTED); }
}
BOOL DrvClientEnumDevices(warp_session *,USBWARP_ENUM_DEVICES_OUT *out) {
    if(enumFails) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    *out={}; out->DeviceCount=bound ? 1 : 0;
    if(bound) memcpy(out->Devices[0].DeviceGuid,&testGuid,16); return TRUE;
}
BOOL DrvClientBindGuid(warp_session *,const uint8_t *,const WCHAR *path,uint8_t,uint32_t *,uint16_t *,uint16_t *) {
    CHECK(!native); CHECK(cycles>=1); CHECK(wcscmp(path,L"\\??\\USB#captured")==0);
    bound=true; events.push_back("bind"); SetLastError(ERROR_INVALID_DATA); return !bindReplyFails;
}
BOOL DrvClientUnbindGuid(warp_session *,const uint8_t *) {
    events.push_back("drain"); if(unbindFails) { SetLastError(ERROR_BUSY); return FALSE; }
    bound=false; return TRUE;
}
DWORD DrvClientClose(warp_session *) {
    if(closeFails) return ERROR_BUSY;
    bound=false; events.push_back("close"); return 0;
}
static void Reset() {
    journal.clear(); events.clear(); writes=flushes=failWrite=failFlush=0;
    present=native=true; bound=badIdentity=enumFails=bindReplyFails=unbindFails=closeFails=false;
    originallyDriverless=false;
    installs=restores=cycles=0; installFailure=cycleFailure=preflightFailure=0;
}
int main() {
    warp_session s{}; const auto guid=(const uint8_t *)&testGuid;
    auto bind=[&] { return DeviceBind(&s,guid,L"\\??\\USB#native",3,nullptr,nullptr,nullptr); };
    Reset(); CHECK(bind()); CHECK(bound && !native && journal.size()==1);
    CHECK(events==std::vector<std::string>({"capture","reset","bind"}));
    CHECK(!bind()); CHECK(GetLastError()==ERROR_ALREADY_EXISTS); CHECK(installs==1);
    DeviceReconcile(&s); CHECK(bound && restores==0);
    CHECK(DeviceUnbind(&s,guid)); CHECK(native && !bound && journal.empty());
    CHECK(events==std::vector<std::string>({"capture","reset","bind","drain","reset","restore","reset"}));
    CHECK(DeviceUnbind(&s,guid)); CHECK(restores==1);

    // Crash at each durable boundary. Recovery has no in-memory record.
    for(int phase=0;phase<=4;++phase) {
        Reset(); Record record; capture::Resolve(guid,L"",record); record.phase=(Phase)phase;
        native=phase==0 || phase==4;
        CHECK(!SaveRecord(Name(guid),record)); CHECK(DeviceRecover(&s));
        CHECK(journal.empty() && native); CHECK(restores==(phase>0 && phase<4 ? 1 : 0));
    }
    // Each registry write/flush may fail before or after a PnP mutation.
    for(int i=1;i<=3;++i) {
        Reset(); failWrite=i; CHECK(!bind()); CHECK(!bound && native && journal.empty());
        Reset(); failFlush=i; CHECK(!bind()); CHECK(!bound && native && journal.empty());
    }
    Reset(); installFailure=ERROR_INSTALL_FAILURE; CHECK(!bind()); CHECK(native && journal.empty() && restores==1);
    Reset(); preflightFailure=ERROR_FILE_NOT_FOUND; CHECK(!bind()); CHECK(native && journal.empty() && installs==0 && restores==0 && cycles==0);
    Reset(); cycleFailure=ERROR_GEN_FAILURE; CHECK(!bind()); CHECK(native && journal.empty());
    Reset(); bindReplyFails=true; CHECK(!bind()); CHECK(!bound && native && journal.empty());

    Reset(); CHECK(bind()); unbindFails=true; CHECK(!DeviceUnbind(&s,guid));
    CHECK(bound && !native && restores==0 && journal.size()==1);
    unbindFails=false; CHECK(DeviceUnbind(&s,guid));
    Reset(); CHECK(bind()); s.driverRegistered=true; closeFails=true;
    CHECK(DeviceCloseSession(&s)==ERROR_BUSY); CHECK(bound && !native && journal.size()==1);
    closeFails=false; CHECK(!DeviceCloseSession(&s)); CHECK(native && journal.empty());

    Reset(); CHECK(bind()); bound=false; present=false;
    CHECK(DeviceRecover(&s)); CHECK(journal.size()==1 && restores==0);
    bool pending=false; CHECK(!DeviceCapturePending(&pending) && pending);
    present=true; DeviceReconcile(&s); CHECK(native && journal.empty());
    Reset(); CHECK(bind()); bound=false; badIdentity=true;
    CHECK(!DeviceRecover(&s)); CHECK(restores==0 && journal.size()==1);
    badIdentity=false; CHECK(DeviceRecover(&s));
    Reset(); CHECK(bind()); bound=false; enumFails=true; DeviceReconcile(&s);
    CHECK(!native && restores==0 && journal.size()==1);
    enumFails=false; DeviceReconcile(&s); CHECK(native && journal.empty());

    Reset(); Record record; capture::Resolve(guid,L"",record); CHECK(!SaveRecord(Name(guid),record));
    auto original=journal.begin()->second;
    for(size_t size=0;size<original.size();++size) {
        journal.begin()->second=std::vector<BYTE>(original.begin(),original.begin()+size);
        Record decoded; CHECK(ReadRecord(Name(guid),decoded)!=ERROR_SUCCESS);
    }
    journal.begin()->second=original;
    Record decoded; CHECK(!ReadRecord(Name(guid),decoded)); CHECK(decoded.upper==record.upper && decoded.lower==record.lower);

    // An explicit driverless destination survives serialization and every
    // restart boundary. Return reset happens BEFORE removing capture, because
    // there is no native function driver whose START event could be awaited.
    Reset(); originallyDriverless=true; CHECK(bind());
    CHECK(!ReadRecord(Name(guid),decoded)); CHECK(decoded.binding==capture::OriginalBinding::None);
    CHECK(!decoded.HasFunctionDriver() && decoded.inf.empty() && decoded.service.empty());
    CHECK(DeviceUnbind(&s,guid)); CHECK(native && !bound && journal.empty());
    CHECK(events==std::vector<std::string>({"capture","reset","bind","drain","reset","restore"}));
    for(int phase=0;phase<=4;++phase) {
        Reset(); originallyDriverless=true; Record noDriver; capture::Resolve(guid,L"",noDriver);
        noDriver.phase=(Phase)phase; native=phase==0 || phase==4;
        CHECK(!SaveRecord(Name(guid),noDriver)); CHECK(DeviceRecover(&s)); CHECK(native && journal.empty());
        CHECK(cycles==(phase>0 && phase<4 ? 1 : 0));
    }
    for(int i=1;i<=3;++i) {
        Reset(); originallyDriverless=true; failWrite=i;
        CHECK(!bind()); CHECK(!bound && native && journal.empty());
        Reset(); originallyDriverless=true; failFlush=i;
        CHECK(!bind()); CHECK(!bound && native && journal.empty());
    }
    Reset(); originallyDriverless=true; bindReplyFails=true;
    CHECK(!bind()); CHECK(!bound && native && journal.empty());
    Reset(); originallyDriverless=true; installFailure=ERROR_INSTALL_FAILURE;
    CHECK(!bind()); CHECK(!bound && native && journal.empty());
    Reset(); originallyDriverless=true; cycleFailure=ERROR_GEN_FAILURE;
    CHECK(!bind()); CHECK(!bound && native && journal.empty());
    Reset(); originallyDriverless=true; CHECK(bind()); bound=false; present=false;
    CHECK(DeviceRecover(&s)); CHECK(journal.size()==1 && restores==0);
    present=true; DeviceReconcile(&s); CHECK(native && journal.empty());
    Reset(); originallyDriverless=true; CHECK(bind()); s.driverRegistered=true;
    CHECK(!DeviceCloseSession(&s)); CHECK(native && journal.empty());

    auto replaceRecord=[&](std::vector<std::wstring> values) {
        std::vector<WCHAR> text;
        for(const auto &v:values) { text.insert(text.end(),v.begin(),v.end()); text.push_back(0); }
        text.push_back(0);
        journal[Name(guid)]=std::vector<BYTE>((BYTE *)text.data(),(BYTE *)(text.data()+text.size()));
    };
    std::vector<std::wstring> legacy={L"version=2",L"id=USB\\VID_1234&PID_5678\\SERIAL",L"inf=C:\\Windows\\INF\\original.inf",
        L"section=Original",L"service=Original",L"model=USB\\Class_08&SubClass_06&Prot_50",L"phase=2",
        L"u=OriginalUpper",L"l=OriginalLower"};
    Reset(); replaceRecord(legacy); CHECK(!ReadRecord(Name(guid),decoded));
    CHECK(decoded.binding==capture::OriginalBinding::Driver && decoded.HasFunctionDriver());
    native=false; CHECK(DeviceRecover(&s)); CHECK(native && journal.empty());
    legacy[0]=L"version=3"; replaceRecord(legacy); CHECK(ReadRecord(Name(guid),decoded)==ERROR_INVALID_DATA);
    legacy.push_back(L"binding=none"); replaceRecord(legacy); CHECK(ReadRecord(Name(guid),decoded)==ERROR_INVALID_DATA);
    legacy.back()=L"binding=driver"; replaceRecord(legacy); CHECK(!ReadRecord(Name(guid),decoded));
    legacy[3]=L"section="; replaceRecord(legacy); CHECK(ReadRecord(Name(guid),decoded)==ERROR_INVALID_DATA);
    printf("Capture transaction: %u checks passed\n",checks);
}
