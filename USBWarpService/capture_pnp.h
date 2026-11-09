// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <string>
#include <vector>

namespace capture {
// This is an undo record, not a second source of binding state. The core
// driver remains authoritative about whether a Linux guest owns the device.
enum class Phase { Prepared, Switching, Captured, Restoring, Native };
enum class OriginalBinding { Driver, None };
struct Record {
    std::wstring instance, inf, section, service, model;
    GUID container{};
    std::vector<std::wstring> upper, lower;
    Phase phase=Phase::Prepared;
    OriginalBinding binding=OriginalBinding::Driver;
    bool HasFunctionDriver() const { return binding==OriginalBinding::Driver && !service.empty(); }
};
DWORD Resolve(const uint8_t guid[16], const WCHAR *path, Record &record);
DWORD Preflight();
DWORD Install(Record &record);
DWORD RestoreDriver(const Record &record);
DWORD Cycle(const std::wstring &instance);
DWORD Verify(const Record &record, bool captured, std::wstring *path=nullptr);
DWORD Present(const std::wstring &instance);
DWORD Identity(const Record &record);
}
