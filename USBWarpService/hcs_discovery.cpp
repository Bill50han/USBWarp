// SPDX-License-Identifier: GPL-2.0-only
#include "warp_service.h"
#include <roapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#pragma comment(lib, "windowsapp.lib")

namespace {
using namespace winrt::Windows::Data::Json;
template<size_t N> void Field(const JsonObject &object, const WCHAR *key, WCHAR (&out)[N])
{
    if (!object.HasKey(key)) return;
    auto value = object.GetNamedString(key); // wrong types are rejected
    if (value.size() >= N) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
    for (uint32_t i = 0; i < value.size(); ++i) {
        if (value[i] < 32 || (value[i] >= 127 && value[i] <= 159)) throw winrt::hresult_error(E_INVALIDARG);
    }
    memcpy(out, value.c_str(), value.size() * sizeof(WCHAR));
    out[value.size()] = 0;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, out, -1, nullptr, 0, nullptr, nullptr))
        throw winrt::hresult_error(E_INVALIDARG);
}
DWORD ErrorCode(HRESULT hr)
{ return HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : (DWORD)hr; }
}

DWORD ParseVmDocument(const WCHAR *json, std::vector<warp_vm> &vms)
{
    vms.clear();
    if (!json || wcsnlen_s(json, 4 * 1024 * 1024) == 4 * 1024 * 1024) return ERROR_INVALID_DATA;
    HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(initialized)) return ErrorCode(initialized);
    DWORD error = ERROR_SUCCESS;
    try {
        // Keep the factory reference inside this apartment's lifetime. The
        // generated static Parse caches a factory across RoUninitialize calls.
        auto factory = winrt::get_activation_factory<IJsonArrayStatics>(L"Windows.Data.Json.JsonArray");
        auto array = factory.Parse(json);
        if (array.Size() > 1024) throw winrt::hresult_error(E_INVALIDARG);
        std::vector<warp_vm> parsed;
        for (const auto &value : array) {
            auto object = value.GetObject();
            if (object.GetNamedString(L"SystemType", L"") != L"VirtualMachine") continue;
            warp_vm vm{};
            Field(object, L"Id", vm.id); Field(object, L"Name", vm.name);
            Field(object, L"Owner", vm.owner); Field(object, L"State", vm.state);
            if (!vm.id[0]) throw winrt::hresult_error(E_INVALIDARG);
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, vm.id, -1, nullptr, 0, nullptr, nullptr) > 256 ||
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, vm.state, -1, nullptr, 0, nullptr, nullptr) > 32)
                throw winrt::hresult_error(E_INVALIDARG);
            for (const auto &previous : parsed)
                if (CompareStringOrdinal(previous.id, -1, vm.id, -1, TRUE) == CSTR_EQUAL)
                    throw winrt::hresult_error(E_INVALIDARG);
            parsed.push_back(vm);
        }
        std::sort(parsed.begin(), parsed.end(), [](const warp_vm &a, const warp_vm &b) {
            return CompareStringOrdinal(a.id, -1, b.id, -1, TRUE) == CSTR_LESS_THAN;
        });
        vms.swap(parsed); // never publish a partially parsed enumeration
    } catch (const std::bad_alloc &) { error = ERROR_NOT_ENOUGH_MEMORY; }
      catch (const winrt::hresult_error &) { error = ERROR_INVALID_DATA; }
    RoUninitialize();
    return error;
}

DWORD EnumerateVms(warp_session *s, std::vector<warp_vm> &vms)
{
    if (!s->api.HcsEnumerate) return ERROR_NOT_SUPPORTED;
    HCS_OPERATION operation = s->api.HcsCreateOp(nullptr, nullptr);
    if (!operation) return ERROR_NOT_ENOUGH_MEMORY;
    PWSTR document = nullptr;
    HRESULT hr = s->api.HcsEnumerate(L"{\"Types\":[\"VirtualMachine\"]}", operation);
    if (SUCCEEDED(hr)) {
        // No callback/context or guest resources belong to this enumeration.
        // Only one operation is outstanding; keep its handle until completion.
        do {
            if (document) { LocalFree(document); document = nullptr; }
            hr = s->api.HcsWait(operation, 100, &document);
        } while (hr == HCS_E_OPERATION_TIMEOUT);
    }
    DWORD error = FAILED(hr) ? ErrorCode(hr) :
        StopRequested(s) ? ERROR_CANCELLED : ParseVmDocument(document, vms);
    if (document) LocalFree(document);
    s->api.HcsCloseOp(operation);
    return error;
}
