// SPDX-License-Identifier: GPL-2.0-only
// Real, READ-ONLY SetupAPI regression. No device instance is opened or changed.
// The inbox INF supplies many models sharing one installation section, which
// catches class-list merging and platform-decoration selection mistakes.
#include "../USBWarpService/capture_pnp.cpp"
#include <cstdio>
#include <cstdlib>
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
int wmain() {
    for(ULONG problem=0;problem<128;++problem)
        CHECK(capture::MissingDriverProblem(problem)==(problem==0 || problem==CM_PROB_NOT_CONFIGURED || problem==CM_PROB_FAILED_INSTALL));
    capture::Record state; state.service=L"Native";
    CHECK(capture::OriginalStateUsable(state,DN_STARTED,0));
    CHECK(!capture::OriginalStateUsable(state,0,0));
    CHECK(!capture::OriginalStateUsable(state,DN_STARTED,CM_PROB_FAILED_START));
    state.service.clear();
    CHECK(capture::OriginalStateUsable(state,0,0));
    CHECK(!capture::OriginalStateUsable(state,0,CM_PROB_FAILED_INSTALL));
    state.binding=capture::OriginalBinding::None;
    CHECK(capture::OriginalStateUsable(state,0,CM_PROB_FAILED_INSTALL));
    CHECK(capture::OriginalStateUsable(state,0,CM_PROB_NOT_CONFIGURED));
    CHECK(!capture::OriginalStateUsable(state,0,CM_PROB_DISABLED));
    CHECK(!capture::OriginalStateUsable(state,0,CM_PROB_FAILED_START));
    WCHAR windows[MAX_PATH]; CHECK(GetWindowsDirectoryW(windows,_countof(windows))>0);
    std::wstring inf=std::wstring(windows)+L"\\INF\\usbstor.inf";
    for(const WCHAR *section:{L"USBSTOR_BULK",L"USBSTOR_BULK.NT"}) {
        for(const WCHAR *model:{L"USB\\Class_08&SubClass_02&Prot_50",L"usb\\class_08&subclass_06&prot_50"}) {
            capture::Info info; info.set=SetupDiCreateDeviceInfoList(nullptr,nullptr);
            CHECK(info.set!=INVALID_HANDLE_VALUE);
            SP_DRVINFO_DATA_W driver{sizeof(driver)};
            CHECK(capture::Select(info,inf,section,model,driver)==ERROR_SUCCESS);
            std::wstring actualInf,actualSection,actualModel;
            CHECK(capture::DriverDetail(info,SPDIT_CLASSDRIVER,driver,actualInf,actualSection,nullptr,nullptr,&actualModel)==ERROR_SUCCESS);
            CHECK(!_wcsicmp(actualModel.c_str(),model));
        }
    }
    capture::Info info; info.set=SetupDiCreateDeviceInfoList(nullptr,nullptr);
    SP_DRVINFO_DATA_W driver{sizeof(driver)};
    CHECK(capture::Select(info,inf,L"USBSTOR_BULK",L"USB\\USBWarpMissingModel",driver)!=ERROR_SUCCESS);
    printf("SetupAPI exact model selection: %u checks passed (no PnP mutations)\n",checks);
}
