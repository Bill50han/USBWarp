// SPDX-License-Identifier: GPL-2.0-only
// Real Windows access checks on an isolated pipe; no USB or service mutation.
#include <windows.h>
#include <sddl.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <stdexcept>
#include "../include/usbwarp_pipe.h"
#pragma comment(lib,"advapi32.lib")
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(0)

static bool allowed(PSECURITY_DESCRIPTOR sd,HANDLE token,DWORD rights)
{
    GENERIC_MAPPING mapping={FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};
    MapGenericMask(&rights,&mapping);
    // Object creation maps ACE generic masks. Raw SDDL has not passed through
    // that step, so reproduce it before the user-mode AccessCheck call.
    PACL dacl=nullptr; BOOL present=FALSE,defaulted=FALSE;
    CHECK(GetSecurityDescriptorDacl(sd,&present,&dacl,&defaulted) && present && dacl);
    for(DWORD i=0;i<dacl->AceCount;i++) {
        PVOID entry=nullptr; CHECK(GetAce(dacl,i,&entry));
        auto ace=(ACCESS_ALLOWED_ACE *)entry;
        CHECK(ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE);
        MapGenericMask(&ace->Mask,&mapping);
    }
    alignas(PRIVILEGE_SET) BYTE privileges[1024]={0};
    DWORD size=sizeof(privileges),granted=0;
    BOOL access=FALSE;
    CHECK(AccessCheck(sd,token,rights,&mapping,(PPRIVILEGE_SET)privileges,&size,&granted,&access));
    return access!=FALSE;
}

int main()
{
    HANDLE primary=nullptr,restricted=nullptr,impersonation=nullptr,server=INVALID_HANDLE_VALUE,client=INVALID_HANDLE_VALUE;
    PSECURITY_DESCRIPTOR sd=nullptr,old=nullptr;
    LPWSTR userText=nullptr;
    bool impersonating=false;
    int result=1;
    try {
        CHECK(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&primary));
        DWORD bytes=0; GetTokenInformation(primary,TokenUser,nullptr,0,&bytes);
        std::vector<BYTE> user(bytes);
        CHECK(GetTokenInformation(primary,TokenUser,user.data(),bytes,&bytes));
        CHECK(ConvertSidToStringSidW(((TOKEN_USER *)user.data())->User.Sid,&userText));
        BYTE admin[SECURITY_MAX_SID_SIZE]; DWORD adminSize=sizeof(admin);
        CHECK(CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admin,&adminSize));
        BOOL elevated=FALSE; CHECK(CheckTokenMembership(nullptr,admin,&elevated));
        CHECK(elevated); // The real server requires an elevated token for its kernel device.
        SID_AND_ATTRIBUTES disabled={admin,0};
        CHECK(CreateRestrictedToken(primary,DISABLE_MAX_PRIVILEGE,1,&disabled,0,nullptr,0,nullptr,&restricted));
        CHECK(DuplicateTokenEx(restricted,TOKEN_QUERY|TOKEN_IMPERSONATE,nullptr,
            SecurityImpersonation,TokenImpersonation,&impersonation));
        std::wstring owner=std::wstring(L"O:")+userText+L"G:BA";
        CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW((owner+USBWARP_PIPE_SDDL).c_str(),SDDL_REVISION_1,&sd,nullptr));
        const DWORD io=FILE_READ_DATA|FILE_WRITE_DATA|FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES|SYNCHRONIZE;
        CHECK(allowed(sd,impersonation,io));
        CHECK(!allowed(sd,impersonation,FILE_CREATE_PIPE_INSTANCE));
        CHECK(!allowed(sd,impersonation,WRITE_DAC));
        CHECK(!allowed(sd,impersonation,WRITE_OWNER));
        // Demonstrate why the previous owner-GA entry defeated the AU mask.
        CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW((owner+L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)(A;;0x12019b;;;AU)").c_str(),SDDL_REVISION_1,&old,nullptr));
        CHECK(allowed(old,impersonation,FILE_CREATE_PIPE_INSTANCE));
        CHECK(allowed(old,impersonation,WRITE_DAC));

        WCHAR name[128]; swprintf_s(name,L"\\\\.\\pipe\\USBWarp.Security.%lu.%llu",GetCurrentProcessId(),GetTickCount64());
        SECURITY_ATTRIBUTES attributes={sizeof(attributes),sd,FALSE};
        server=CreateNamedPipeW(name,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_REJECT_REMOTE_CLIENTS,2,512,512,0,&attributes);
        CHECK(server!=INVALID_HANDLE_VALUE);
        CHECK(ImpersonateLoggedOnUser(impersonation)); impersonating=true;
        client=CreateFileW(name,io,0,nullptr,OPEN_EXISTING,SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
        DWORD clientError=GetLastError();
        HANDLE other=CreateNamedPipeW(name,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_REJECT_REMOTE_CLIENTS,2,512,512,0,nullptr);
        DWORD denied=GetLastError();
        CHECK(RevertToSelf()); impersonating=false;
        if(other!=INVALID_HANDLE_VALUE) CloseHandle(other);
        if(client==INVALID_HANDLE_VALUE) fprintf(stderr,"client open error=%lu\n",clientError);
        CHECK(client!=INVALID_HANDLE_VALUE && other==INVALID_HANDLE_VALUE && denied==ERROR_ACCESS_DENIED);
        puts("Pipe ACL: restricted owner can exchange data, but cannot add server instances or rewrite ACL; old grant reproduced");
        result=0;
    } catch(const std::exception &error) {
        fprintf(stderr,"Pipe security test failed: %s (Win32=%lu)\n",error.what(),GetLastError());
    }
    if(impersonating) RevertToSelf();
    if(client!=INVALID_HANDLE_VALUE) CloseHandle(client);
    if(server!=INVALID_HANDLE_VALUE) CloseHandle(server);
    if(sd) LocalFree(sd); if(old) LocalFree(old); if(userText) LocalFree(userText);
    if(impersonation) CloseHandle(impersonation); if(restricted) CloseHandle(restricted); if(primary) CloseHandle(primary);
    return result;
}
