// SPDX-License-Identifier: GPL-2.0-only
#include "warp_service.h"
#include <stdio.h>
#include <stdarg.h>
#include <sddl.h>
#include <aclapi.h>
#include <io.h>
#include <fcntl.h>
#pragma comment(lib, "advapi32.lib")

namespace {
SRWLOCK logLock = SRWLOCK_INIT;
FILE *sink;
HANDLE eventSource;
bool asService;

FILE *OpenLogFile()
{
    WCHAR directory[1024], path[1100];
#ifdef USBWARP_LOG_DIRECTORY
    wcscpy_s(directory, USBWARP_LOG_DIRECTORY);
#else
    WCHAR base[900];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", base, _countof(base));
    if (!n || n >= _countof(base)) return nullptr;
    swprintf_s(directory, L"%s\\UsbWarp", base);
#endif
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GR;;;AU)",
            SDDL_REVISION_1, &descriptor, nullptr)) return nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    BOOL created = CreateDirectoryW(directory, &attributes);
    if (!created && GetLastError() != ERROR_ALREADY_EXISTS) { LocalFree(descriptor); return nullptr; }
    // Protecting the DACL also requires reading the existing descriptor.
    HANDLE dir = CreateFileW(directory, FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    FILE_ATTRIBUTE_TAG_INFO tag{};
    BOOL present, defaulted;
    PACL acl;
    FILE *file = nullptr;
    if (dir != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandleEx(dir, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) && present &&
        SetSecurityInfo(dir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS) {
        swprintf_s(path, L"%s\\service.log", directory);
        HANDLE h = CreateFileW(path, GENERIC_WRITE | FILE_READ_ATTRIBUTES | WRITE_DAC,
            FILE_SHARE_READ, &attributes, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (h != INVALID_HANDLE_VALUE) {
            if (GetFileInformationByHandle(h, &info) && info.nNumberOfLinks == 1 &&
                !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
                SetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                    nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS) {
                int fd = _open_osfhandle((intptr_t)h, _O_APPEND | _O_TEXT);
                if (fd >= 0) { file = _fdopen(fd, "a"); if (!file) _close(fd); h = INVALID_HANDLE_VALUE; }
            }
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    }
    if (dir != INVALID_HANDLE_VALUE) CloseHandle(dir);
    LocalFree(descriptor);
    if (file) setvbuf(file, nullptr, _IONBF, 0);
    return file;
}

void LogV(WORD eventType, const char *level, const char *format, va_list args)
{
    char message[4096];
    vsnprintf(message, sizeof(message), format, args);
    AcquireSRWLockExclusive(&logLock);
    if (sink || !asService) {
        SYSTEMTIME now;
        GetLocalTime(&now);
        fprintf(sink ? sink : stderr, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s %s\n",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds, level, message);
    } else if (eventSource) {
        const char *strings[] = {message};
        ReportEventA(eventSource, eventType, 0, 0, nullptr, 1, 0, strings, nullptr);
    }
    ReleaseSRWLockExclusive(&logLock);
}
}

BOOL LogInit(bool serviceMode)
{
    AcquireSRWLockExclusive(&logLock);
    asService = serviceMode;
    if (serviceMode) {
        sink = OpenLogFile();
        if (!sink) eventSource = RegisterEventSourceW(nullptr, USBWARP_SERVICE_NAME);
    }
    BOOL ok = !serviceMode || sink || eventSource;
    ReleaseSRWLockExclusive(&logLock);
    return ok;
}
void LogCleanup()
{
    AcquireSRWLockExclusive(&logLock);
    if (sink) fclose(sink);
    if (eventSource) DeregisterEventSource(eventSource);
    sink = nullptr; eventSource = nullptr;
    ReleaseSRWLockExclusive(&logLock);
}
void LogInfo(const char *fmt, ...) { va_list ap; va_start(ap,fmt); LogV(EVENTLOG_INFORMATION_TYPE,"[INFO]",fmt,ap); va_end(ap); }
void LogWarn(const char *fmt, ...) { va_list ap; va_start(ap,fmt); LogV(EVENTLOG_WARNING_TYPE,"[WARN]",fmt,ap); va_end(ap); }
void LogError(const char *fmt, ...) { va_list ap; va_start(ap,fmt); LogV(EVENTLOG_ERROR_TYPE,"[ERROR]",fmt,ap); va_end(ap); }
