// SPDX-License-Identifier: GPL-2.0-only
#include "warp_service.h"
#include <wchar.h>

namespace {
bool Text(const WCHAR *value, size_t capacity)
{
    size_t length = wcsnlen_s(value, capacity);
    if (length == capacity) return false;
    for (size_t i = 0; i < length; ++i) {
        if (value[i] < 32 || (value[i] >= 127 && value[i] <= 159)) return false;
        if (value[i] >= 0xd800 && value[i] <= 0xdbff) {
            if (++i >= length || value[i] < 0xdc00 || value[i] > 0xdfff) return false;
        } else if (value[i] >= 0xdc00 && value[i] <= 0xdfff) return false;
    }
    for (size_t i = length; i < capacity; ++i) if (value[i]) return false;
    return true;
}
bool Same(const WCHAR *a, const WCHAR *b)
{ return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL; }
bool Decode(const char *source, size_t bytes, WCHAR *out, int capacity)
{
    const char *end = (const char *)memchr(source, 0, bytes);
    if (!end) return false;
    for (const char *p = end; p != source + bytes; ++p) if (*p) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source,
        (int)(end - source + 1), out, capacity) != 0;
}
}

DWORD ValidateTarget(warp_target *target)
{
    if (!target || !Text(target->value, _countof(target->value)) ||
        !Text(target->owner, _countof(target->owner))) return ERROR_INVALID_PARAMETER;
    switch (target->kind) {
    case USBWARP_TARGET_NONE:
    case USBWARP_TARGET_WSL:
        return !target->value[0] && !target->owner[0] ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
    case USBWARP_TARGET_NAME:
        return target->value[0] ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
    case USBWARP_TARGET_ID: {
        if (target->owner[0]) return ERROR_INVALID_PARAMETER;
        size_t length = wcslen(target->value);
        WCHAR braced[40]{}; GUID guid{};
        if (length == 36) swprintf_s(braced, L"{%ls}", target->value);
        else if (length == 38) wcscpy_s(braced, target->value);
        else return ERROR_INVALID_PARAMETER;
        if (FAILED(CLSIDFromString(braced, &guid))) return ERROR_INVALID_PARAMETER;
        StringFromGUID2(guid, braced, _countof(braced));
        memset(target->value, 0, sizeof(target->value));
        memcpy(target->value, braced + 1, 36 * sizeof(WCHAR));
        return ERROR_SUCCESS;
    }
    default: return ERROR_INVALID_PARAMETER;
    }
}

DWORD TargetFromWire(const usbwarp_pipe_target &wire, warp_target *target)
{
    warp_target parsed{};
    parsed.kind = wire.kind;
    if (wire.reserved || !Decode(wire.value, sizeof(wire.value), parsed.value, _countof(parsed.value)) ||
        !Decode(wire.owner, sizeof(wire.owner), parsed.owner, _countof(parsed.owner)) || ValidateTarget(&parsed))
        return ERROR_INVALID_DATA;
    *target = parsed;
    return ERROR_SUCCESS;
}
DWORD TargetToWire(const warp_target &target, usbwarp_pipe_target *wire)
{
    memset(wire, 0, sizeof(*wire));
    wire->kind = target.kind;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, target.value, -1,
            wire->value, sizeof(wire->value), nullptr, nullptr) ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, target.owner, -1,
            wire->owner, sizeof(wire->owner), nullptr, nullptr)) return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}
DWORD MatchVm(const warp_target &target, const std::vector<warp_vm> &vms,
              WCHAR (&id)[256], uint32_t *matches)
{
    id[0] = 0; *matches = 0;
    if (target.kind == USBWARP_TARGET_NONE) return ERROR_NOT_READY;
    for (const auto &vm : vms) {
        // VMMS can omit State for a running VM; paused/stopped/templates are
        // never chosen automatically. An actual open remains authoritative.
        if (vm.state[0] && !Same(vm.state, L"Running")) continue;
        bool match = target.kind == USBWARP_TARGET_ID ? Same(target.value, vm.id) :
            target.kind == USBWARP_TARGET_WSL ? Same(vm.owner, L"WSL") :
            target.kind == USBWARP_TARGET_NAME && Same(target.value, vm.name) &&
                (!target.owner[0] || Same(target.owner, vm.owner));
        if (match) { ++*matches; wcscpy_s(id, vm.id); }
    }
    if (*matches == 1) return ERROR_SUCCESS;
    id[0] = 0;
    return *matches ? ERROR_DUP_NAME : ERROR_NOT_FOUND;
}
