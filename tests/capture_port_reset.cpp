// SPDX-License-Identifier: GPL-2.0-only
// Hardware regression helper: reset exactly one identity-checked USB leaf.
// No driver installation, file-system access, or data writes.
#include "../USBWarpService/capture_pnp.cpp"
#include <cstdio>
#pragma comment(lib,"ole32.lib")
int wmain(int argc,WCHAR **argv)
{
    if(argc!=3 && argc!=4) return ERROR_INVALID_PARAMETER;
    capture::Record record; record.instance=argv[1];
    if(FAILED(CLSIDFromString(argv[2],&record.container))) return ERROR_INVALID_PARAMETER;
    DWORD error=capture::Identity(record);
    if(!error && argc==4) {
        if(wcscmp(argv[3],L"--verify-access")) return ERROR_INVALID_PARAMETER;
        std::vector<WCHAR> paths;
        error=capture::Interfaces(GUID_USBWARP_FILTER_INTERFACE,record.instance,paths);
        if(error) return (int)error;
        HANDLE handle=CreateFileW(paths.data(),GENERIC_READ|GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(handle!=INVALID_HANDLE_VALUE) { CloseHandle(handle); return ERROR_INVALID_ACCESS; }
        error=GetLastError();
        printf("Capture user-mode open rejected: %lu\n",error);
        return error==ERROR_ACCESS_DENIED ? 0 : (int)error;
    }
    if(!error) error=capture::Cycle(record.instance);
    printf("Identity-checked USB port reset: %lu\n",error);
    return (int)error;
}
