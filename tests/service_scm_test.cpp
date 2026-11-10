// SPDX-License-Identifier: GPL-2.0-only
// Real SCM and named pipes; the service executable uses fixture backends only.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <stdexcept>
#include "../include/usbwarp_pipe.h"
#pragma comment(lib, "advapi32.lib")

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { char e[256]; snprintf(e,sizeof(e),"line %d: %s (error %lu)",__LINE__,#x,GetLastError()); throw std::runtime_error(e); } } while(0)
static const WCHAR *Name = USBWARP_SERVICE_NAME;
static std::wstring Root = USBWARP_TEST_DIRECTORY;

static ULONGLONG log_size()
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW((Root+L"\\Logs\\service.log").c_str(),GetFileExInfoStandard,&attributes)) {
        CHECK(GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND);
        return 0;
    }
    return ((ULONGLONG)attributes.nFileSizeHigh<<32)|attributes.nFileSizeLow;
}

static void marker(const WCHAR *name, bool create)
{
    std::wstring path = Root + L"\\" + name;
    if (!create) { DeleteFileW(path.c_str()); return; }
    HANDLE h = CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
    CHECK(h != INVALID_HANDLE_VALUE); CloseHandle(h);
}
static DWORD run(const std::wstring &exe, const WCHAR *args)
{
    std::wstring command = L"\"" + exe + L"\" " + args;
    STARTUPINFOW start{}; start.cb = sizeof(start); start.dwFlags = STARTF_USESHOWWINDOW; start.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    CHECK(CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &start, &process));
    CloseHandle(process.hThread);
    DWORD wait = WaitForSingleObject(process.hProcess,30000), exit = 0;
    CHECK(wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess,&exit));
    CloseHandle(process.hProcess); return exit;
}
static SERVICE_STATUS_PROCESS state(SC_HANDLE service)
{
    SERVICE_STATUS_PROCESS status{}; DWORD needed;
    CHECK(QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(BYTE *)&status,sizeof(status),&needed));
    return status;
}
static SERVICE_STATUS_PROCESS wait_state(SC_HANDLE service, DWORD wanted, DWORD timeout=15000)
{
    ULONGLONG end = GetTickCount64()+timeout;
    for (;;) {
        auto current=state(service);
        if (current.dwCurrentState==wanted) return current;
        CHECK(GetTickCount64()<end);
        Sleep(50);
    }
}
static HANDLE connect_pipe()
{
    CHECK(WaitNamedPipeA(USBWARP_PIPE_NAME,5000));
    HANDLE h=CreateFileA(USBWARP_PIPE_NAME,FILE_READ_DATA|FILE_WRITE_DATA|FILE_READ_ATTRIBUTES|
        FILE_WRITE_ATTRIBUTES|SYNCHRONIZE,0,nullptr,OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
    CHECK(h!=INVALID_HANDLE_VALUE);
    DWORD mode=PIPE_READMODE_MESSAGE; CHECK(SetNamedPipeHandleState(h,&mode,nullptr,nullptr));
    return h;
}
static bool transfer(HANDLE pipe, void *data, DWORD size, DWORD *bytes, bool write)
{
    OVERLAPPED ov{}; ov.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    CHECK(ov.hEvent);
    BOOL ok=write?WriteFile(pipe,data,size,bytes,&ov):ReadFile(pipe,data,size,bytes,&ov);
    if (!ok && GetLastError()==ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent,5000)!=WAIT_OBJECT_0) CancelIoEx(pipe,&ov);
        ok=GetOverlappedResult(pipe,&ov,bytes,TRUE);
    }
    CloseHandle(ov.hEvent); return ok!=FALSE;
}
static std::vector<BYTE> call(uint32_t command, const void *payload=nullptr, uint32_t size=0)
{
    HANDLE pipe=connect_pipe();
    std::vector<BYTE> request(sizeof(usbwarp_pipe_header)+size), reply(65536);
    usbwarp_pipe_header header{USBWARP_PIPE_MAGIC,command,size,17};
    memcpy(request.data(),&header,sizeof(header)); if (size) memcpy(request.data()+sizeof(header),payload,size);
    DWORD bytes=0;
    bool ok=transfer(pipe,request.data(),(DWORD)request.size(),&bytes,true) && bytes==request.size();
    if (ok) ok=transfer(pipe,reply.data(),(DWORD)reply.size(),&bytes,false);
    CloseHandle(pipe); CHECK(ok); reply.resize(bytes);
    CHECK(reply.size()>=sizeof(header)+sizeof(usbwarp_pipe_response));
    auto response=(const usbwarp_pipe_header *)reply.data();
    CHECK(response->magic==USBWARP_PIPE_MAGIC && response->sequence_id==17);
    return reply;
}
static int32_t status_of(const std::vector<BYTE> &reply)
{ return ((const usbwarp_pipe_response *)(reply.data()+sizeof(usbwarp_pipe_header)))->status; }
static usbwarp_pipe_status status()
{
    auto reply=call(USBWARP_PIPE_CMD_QUERY_STATUS);
    CHECK(!status_of(reply) && reply.size()>=sizeof(usbwarp_pipe_header)+sizeof(usbwarp_pipe_response)+sizeof(usbwarp_pipe_status));
    usbwarp_pipe_status value;
    memcpy(&value,reply.data()+sizeof(usbwarp_pipe_header)+sizeof(usbwarp_pipe_response),sizeof(value));
    return value;
}
static void wait_stage(uint32_t wanted, DWORD timeout=15000)
{
    ULONGLONG end=GetTickCount64()+timeout;
    while (status().session_state!=wanted) { CHECK(GetTickCount64()<end); Sleep(100); }
}
static usbwarp_pipe_target_status target_status()
{
    auto reply=call(USBWARP_PIPE_CMD_QUERY_TARGET);
    CHECK(!status_of(reply) && reply.size()==sizeof(usbwarp_pipe_header)+sizeof(usbwarp_pipe_response)+sizeof(usbwarp_pipe_target_status));
    usbwarp_pipe_target_status target;
    memcpy(&target,reply.data()+sizeof(usbwarp_pipe_header)+sizeof(usbwarp_pipe_response),sizeof(target));
    return target;
}
static void select_wsl()
{
    usbwarp_pipe_target target{};target.kind=USBWARP_TARGET_WSL;
    CHECK(status_of(call(USBWARP_PIPE_CMD_SET_TARGET,&target,sizeof(target)))==0);
}
static void wait_target(const char *id, DWORD error=ERROR_SUCCESS)
{
    ULONGLONG end=GetTickCount64()+20000;
    for(;;) {auto target=target_status();if(!strcmp(target.attached_id,id) && target.error==error)return;CHECK(GetTickCount64()<end);Sleep(100);}
}
static std::vector<BYTE> bind_payload()
{
    const char path[]="\\\\?\\USB#VID_1234&PID_ABCD#SELFTEST#{a5dcbf10-6530-11d2-901f-00c04fb951ed}";
    usbwarp_pipe_bind_request bind{}; bind.device_guid[0]=1; bind.instance_path_length=sizeof(path)-1; bind.speed=6;
    std::vector<BYTE> payload(sizeof(bind)+sizeof(path)-1);
    memcpy(payload.data(),&bind,sizeof(bind)); memcpy(payload.data()+sizeof(bind),path,sizeof(path)-1);
    return payload;
}

static void rejected_frames()
{
    for (unsigned kind=0; kind<3; ++kind) {
        HANDLE pipe=connect_pipe();
        std::vector<BYTE> frame(kind==0 ? 70000 : sizeof(usbwarp_pipe_header)+1,0);
        usbwarp_pipe_header header{USBWARP_PIPE_MAGIC,USBWARP_PIPE_CMD_QUERY_STATUS,0,23};
        if (kind==1) header.magic=0;
        memcpy(frame.data(),&header,sizeof(header));
        DWORD bytes; transfer(pipe,frame.data(),(DWORD)frame.size(),&bytes,true);
        BYTE reply[512]; BOOL responded=transfer(pipe,reply,sizeof(reply),&bytes,false);
        CloseHandle(pipe); CHECK(!responded); // no prefix dispatch, even for a query
        CHECK(status().session_state==USBWARP_SESSION_RUNNING);
    }
}

static HANDLE start_console()
{
    std::wstring exe=Root+L"\\service_fixture.exe";
    std::wstring command=L"\""+exe+L"\" --id 11111111-1111-1111-1111-111111111111";
    STARTUPINFOW start{}; start.cb=sizeof(start); start.dwFlags=STARTF_USESHOWWINDOW; start.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{};
    CHECK(CreateProcessW(exe.c_str(),&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
                         nullptr,nullptr,&start,&process));
    CloseHandle(process.hThread);
    ULONGLONG end=GetTickCount64()+10000;
    while (!WaitNamedPipeA(USBWARP_PIPE_NAME,100)) {
        CHECK(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT && GetTickCount64()<end);
        Sleep(20);
    }
    return process.hProcess;
}
static void stop_console(HANDLE process)
{
    marker(L"stop_service",true);
    CHECK(WaitForSingleObject(process,10000)==WAIT_OBJECT_0);
    DWORD exit; CHECK(GetExitCodeProcess(process,&exit) && !exit);
    CloseHandle(process); marker(L"stop_service",false);
}
static void nonadmin_denied(const std::vector<BYTE> &binding)
{
    usbwarp_pipe_target target{};
    BYTE sid[SECURITY_MAX_SID_SIZE]; DWORD size=sizeof(sid);
    CHECK(CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,sid,&size));
    BOOL admin=FALSE; CHECK(CheckTokenMembership(nullptr,sid,&admin));
    if (!admin) {
        CHECK(status_of(call(USBWARP_PIPE_CMD_SET_TARGET,&target,sizeof(target)))==-ERROR_ACCESS_DENIED);
        CHECK(status_of(call(USBWARP_PIPE_CMD_QUERY_TARGET))==0);
        CHECK(status_of(call(USBWARP_PIPE_CMD_BIND_DEVICE,binding.data(),(uint32_t)binding.size()))==-ERROR_ACCESS_DENIED);
        CHECK(status_of(call(USBWARP_PIPE_CMD_QUERY_STATUS))==0);
        return;
    }
    HANDLE token=nullptr, restricted=nullptr;
    CHECK(OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE|TOKEN_QUERY,&token));
    SID_AND_ATTRIBUTES disabled{sid,0};
    CHECK(CreateRestrictedToken(token,DISABLE_MAX_PRIVILEGE,1,&disabled,0,nullptr,0,nullptr,&restricted));
    CloseHandle(token);
    CHECK(ImpersonateLoggedOnUser(restricted));
    try {
        CHECK(status_of(call(USBWARP_PIPE_CMD_SET_TARGET,&target,sizeof(target)))==-ERROR_ACCESS_DENIED);
        CHECK(status_of(call(USBWARP_PIPE_CMD_QUERY_TARGET))==0);
        CHECK(status_of(call(USBWARP_PIPE_CMD_BIND_DEVICE,binding.data(),(uint32_t)binding.size()))==-ERROR_ACCESS_DENIED);
        CHECK(status_of(call(USBWARP_PIPE_CMD_QUERY_STATUS))==0);
    } catch (...) { RevertToSelf(); CloseHandle(restricted); throw; }
    CHECK(RevertToSelf()); CloseHandle(restricted);
}
static int console_test()
{
    HANDLE process=nullptr, idle=INVALID_HANDLE_VALUE;
    int result=1;
    try {
        // Refuse to attach to another test run or service.
        CHECK(!WaitNamedPipeA(USBWARP_PIPE_NAME,0) && GetLastError()==ERROR_FILE_NOT_FOUND);
        marker(L"vm_up",false); marker(L"driver_down",true); marker(L"device_gone",false);
        marker(L"stop_service",false); marker(L"backend_bound",false);
        process=start_console(); wait_stage(USBWARP_SESSION_WAIT_DRIVER);
        marker(L"driver_down",false); wait_stage(USBWARP_SESSION_WAIT_VM);
        nonadmin_denied(bind_payload());
        idle=connect_pipe(); CHECK(status().session_state==USBWARP_SESSION_WAIT_VM);
        stop_console(process); process=nullptr; CloseHandle(idle); idle=INVALID_HANDLE_VALUE;
        process=start_console(); wait_stage(USBWARP_SESSION_WAIT_VM);
        marker(L"vm_up",true); wait_stage(USBWARP_SESSION_RUNNING);
        marker(L"backend_bound",true); CHECK(status().bound_device_count==1);
        auto listed=call(USBWARP_PIPE_CMD_LIST_DEVICES); CHECK(status_of(listed)==0);
        CHECK(((usbwarp_pipe_device_list *)(listed.data()+sizeof(usbwarp_pipe_header)+sizeof(usbwarp_pipe_response)))->device_count==1);
        uint8_t guid[16]={1}, global[16]={0};
        CHECK(status_of(call(USBWARP_PIPE_CMD_QUERY_DEVICE,guid,sizeof(guid)))==0);
        CHECK(status_of(call(USBWARP_PIPE_CMD_QUERY_STATS))==0); // legacy empty request
        CHECK(status_of(call(USBWARP_PIPE_CMD_QUERY_STATS,global,sizeof(global)))==0);
        CHECK(status_of(call(USBWARP_PIPE_CMD_QUERY_STATS,guid,3))==-ERROR_INVALID_DATA);
        marker(L"device_gone",true); CHECK(status().bound_device_count==0);
        CHECK(status_of(call(USBWARP_PIPE_CMD_QUERY_DEVICE,guid,sizeof(guid)))==-ERROR_NOT_FOUND);
        marker(L"backend_bound",false); marker(L"device_gone",false);
        marker(L"vm_up",false); wait_stage(USBWARP_SESSION_WAIT_VM);
        CHECK(WaitForSingleObject(process,0)==WAIT_TIMEOUT);
        marker(L"vm_up",true); wait_stage(USBWARP_SESSION_RUNNING);
        CHECK(status().bound_device_count==0);
        nonadmin_denied(bind_payload());
        marker(L"driver_down",true); wait_stage(USBWARP_SESSION_WAIT_DRIVER);
        marker(L"driver_down",false); wait_stage(USBWARP_SESSION_RUNNING);
        rejected_frames();
        // A client that never reads its replies cannot delay service shutdown.
        idle=connect_pipe();
        usbwarp_pipe_header query{USBWARP_PIPE_MAGIC,USBWARP_PIPE_CMD_QUERY_STATUS,0,1};
        DWORD sent;
        // Replies exceed the server's 64 KiB output quota while the small
        // requests still fit its input quota, leaving an actual write pending.
        for (unsigned i=0; i<1200; ++i) CHECK(transfer(idle,&query,sizeof(query),&sent,true));
        stop_console(process); process=nullptr; CloseHandle(idle); idle=INVALID_HANDLE_VALUE;
        result=0;
    } catch (const std::exception &error) { fprintf(stderr,"Console regression failed: %s\n",error.what()); }
    if (idle!=INVALID_HANDLE_VALUE) CloseHandle(idle);
    if (process) {
        marker(L"stop_service",true);
        if (WaitForSingleObject(process,10000)!=WAIT_OBJECT_0) TerminateProcess(process,1);
        WaitForSingleObject(process,5000); CloseHandle(process);
    }
    marker(L"vm_up",false); marker(L"driver_down",false); marker(L"device_gone",false);
    marker(L"stop_service",false); marker(L"backend_bound",false);
    if (!result) printf("Console/IPC/session regression: %u checks passed (fixture backends; no real USB/VM I/O)\n",checks);
    return result;
}

int main(int argc, char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--console")) return console_test();
    std::wstring directory=Root+L"\\Install With Spaces", executable=directory+L"\\service_fixture.exe";
    SC_HANDLE manager=nullptr, service=nullptr;
    bool installed=false;
    HANDLE idle=INVALID_HANDLE_VALUE;
    int result=1;
    try {
        manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT|SC_MANAGER_CREATE_SERVICE);
        CHECK(manager); // requires an elevated test shell
        SC_HANDLE existing=OpenServiceW(manager,Name,SERVICE_QUERY_STATUS);
        if (existing) { CloseServiceHandle(existing); throw std::runtime_error("Private test service already exists; refusing to replace it"); }
        CHECK(GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST);
        CreateDirectoryW(directory.c_str(),nullptr);
        CHECK(CopyFileW((Root+L"\\service_fixture.exe").c_str(),executable.c_str(),FALSE));
        marker(L"vm_up",false); marker(L"driver_down",false); marker(L"device_gone",false);
        marker(L"stop_service",false); marker(L"backend_bound",false);
        marker(L"vm_new_id",false);marker(L"vm_ambiguous",false);marker(L"discovery_error",false);
        CHECK(run(executable,L"--install")==0);
        installed=true;
        service=OpenServiceW(manager,Name,SERVICE_START|SERVICE_STOP|SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG);
        CHECK(service);
        DWORD needed=0; QueryServiceConfigW(service,nullptr,0,&needed);
        std::vector<BYTE> config(needed);
        CHECK(QueryServiceConfigW(service,(QUERY_SERVICE_CONFIGW *)config.data(),needed,&needed));
        auto settings=(QUERY_SERVICE_CONFIGW *)config.data();
        CHECK(settings->dwServiceType==SERVICE_WIN32_OWN_PROCESS);
        CHECK(settings->lpBinaryPathName[0]==L'"' && wcsstr(settings->lpBinaryPathName,L"Install With Spaces"));

        ULONGLONG previousLogSize=log_size();
        marker(L"forbid_shm",true);
        CHECK(StartServiceW(service,0,nullptr));
        auto first=wait_state(service,SERVICE_RUNNING);
        wait_stage(USBWARP_SESSION_WAIT_TARGET);
        CHECK(target_status().selected.kind==USBWARP_TARGET_NONE);
        ULONGLONG idleUntil=GetTickCount64()+6000;
        while(GetTickCount64()<idleUntil){CHECK(status().session_state==USBWARP_SESSION_WAIT_TARGET);Sleep(100);}
        usbwarp_pipe_vm_list_request listRequest{};
        CHECK(status_of(call(USBWARP_PIPE_CMD_LIST_VMS,&listRequest,sizeof(listRequest)))==0);
        marker(L"forbid_shm",false);
        select_wsl();wait_stage(USBWARP_SESSION_WAIT_VM);
        auto binding=bind_payload();
        CHECK(status_of(call(USBWARP_PIPE_CMD_BIND_DEVICE,binding.data(),(uint32_t)binding.size()))==-ERROR_NOT_READY);
        idle=connect_pipe(); // idle client must neither monopolize IPC nor delay stop
        CHECK(status().session_state==USBWARP_SESSION_WAIT_VM);
        SERVICE_STATUS ignored;
        ULONGLONG began=GetTickCount64(); CHECK(ControlService(service,SERVICE_CONTROL_STOP,&ignored));
        wait_state(service,SERVICE_STOPPED,10000); CHECK(GetTickCount64()-began<10000);
        CHECK(log_size()>previousLogSize);
        CloseHandle(idle); idle=INVALID_HANDLE_VALUE;

        CHECK(StartServiceW(service,0,nullptr));
        first=wait_state(service,SERVICE_RUNNING); wait_stage(USBWARP_SESSION_WAIT_VM);
        marker(L"vm_up",true); wait_stage(USBWARP_SESSION_RUNNING);
        nonadmin_denied(binding);
        CHECK(status_of(call(USBWARP_PIPE_CMD_BIND_DEVICE,binding.data(),(uint32_t)binding.size()))==0);
        CHECK(status().bound_device_count==1);
        usbwarp_pipe_target clearTarget{};
        CHECK(status_of(call(USBWARP_PIPE_CMD_SET_TARGET,&clearTarget,sizeof(clearTarget)))==-ERROR_BUSY);
        CHECK(target_status().selected.kind==USBWARP_TARGET_WSL);
        marker(L"device_gone",true); CHECK(status().bound_device_count==0); // backend truth, no mirror
        marker(L"device_gone",false);
        // Oversized message with an otherwise valid prefix must never execute.
        HANDLE oversized=connect_pipe();
        std::vector<BYTE> bad(70000,0);
        usbwarp_pipe_header badHeader{USBWARP_PIPE_MAGIC,USBWARP_PIPE_CMD_BIND_DEVICE,(uint32_t)binding.size(),19};
        memcpy(bad.data(),&badHeader,sizeof(badHeader)); memcpy(bad.data()+sizeof(badHeader),binding.data(),binding.size());
        DWORD sent; transfer(oversized,bad.data(),(DWORD)bad.size(),&sent,true); CloseHandle(oversized);
        CHECK(state(service).dwCurrentState==SERVICE_RUNNING);

        marker(L"vm_up",false); wait_stage(USBWARP_SESSION_WAIT_VM);
        CHECK(state(service).dwProcessId==first.dwProcessId); // no process exit on VM loss
        marker(L"vm_new_id",true); // WSL creates a different HCS system after shutdown
        marker(L"vm_up",true); wait_stage(USBWARP_SESSION_RUNNING);
        wait_target("22222222-2222-2222-2222-222222222222");
        CHECK(status().bound_device_count==0);
        marker(L"vm_up",false);wait_stage(USBWARP_SESSION_WAIT_VM);
        marker(L"vm_ambiguous",true);marker(L"vm_up",true);
        wait_target("",ERROR_DUP_NAME);CHECK(target_status().matches==2);
        CHECK(status().session_state==USBWARP_SESSION_WAIT_VM);
        marker(L"vm_ambiguous",false);wait_stage(USBWARP_SESSION_RUNNING);
        CHECK(status_of(call(USBWARP_PIPE_CMD_SET_TARGET,&clearTarget,sizeof(clearTarget)))==0);
        wait_stage(USBWARP_SESSION_WAIT_TARGET);wait_target("");
        select_wsl();wait_stage(USBWARP_SESSION_RUNNING);
        CHECK(state(service).dwProcessId==first.dwProcessId);

        // Verify the SCM failure action using only the fixture process ID.
        HANDLE process=OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE,FALSE,first.dwProcessId);
        CHECK(process); CHECK(TerminateProcess(process,99)); WaitForSingleObject(process,5000); CloseHandle(process);
        ULONGLONG deadline=GetTickCount64()+20000;
        for (;;) {
            auto current=state(service);
            if (current.dwCurrentState==SERVICE_RUNNING && current.dwProcessId!=first.dwProcessId) break;
            CHECK(GetTickCount64()<deadline); Sleep(100);
        }
        wait_stage(USBWARP_SESSION_RUNNING);
        CHECK(target_status().selected.kind==USBWARP_TARGET_WSL);
        wait_target("22222222-2222-2222-2222-222222222222");
        result=0;
    } catch (const std::exception &error) { fprintf(stderr,"SCM regression failed: %s\n",error.what()); }
    if (idle!=INVALID_HANDLE_VALUE) CloseHandle(idle);
    if (service) CloseServiceHandle(service);
    if (manager) CloseServiceHandle(manager);
    if (installed) {
        try { if (run(executable,L"--uninstall")!=0) result=1; }
        catch (...) { result=1; }
    }
    marker(L"vm_up",false); marker(L"driver_down",false); marker(L"device_gone",false);
    marker(L"vm_new_id",false);marker(L"vm_ambiguous",false);marker(L"discovery_error",false);
    marker(L"forbid_shm",false);
    if (!result) printf("SCM/IPC/session regression: %u checks passed (fixture backends; no real USB/VM I/O)\n",checks);
    return result;
}
