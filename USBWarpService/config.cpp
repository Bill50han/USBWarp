// SPDX-License-Identifier: GPL-2.0-only
#include "warp_service.h"
#include <stdio.h>
#include <errno.h>
#include <wchar.h>

#ifndef USBWARP_CONFIG_ROOT
#define USBWARP_CONFIG_ROOT HKEY_LOCAL_MACHINE
#endif

namespace {
struct StoredConfig {
    DWORD magic, version, size;
    WCHAR vm_id[256];
    DWORD shm, ring, buffer, max_devices, flags;
};
constexpr DWORD ConfigMagic = 0x43425755;

bool Number(const WCHAR *text, uint32_t scale, uint32_t *result)
{
    if (!text || !*text) return false;
    for (const WCHAR *p = text; *p; ++p) if (*p < L'0' || *p > L'9') return false;
    errno = 0;
    WCHAR *end;
    unsigned long long n = _wcstoui64(text, &end, 10);
    if (errno == ERANGE || *end || n > UINT32_MAX / scale) return false;
    *result = (uint32_t)n * scale;
    return true;
}

DWORD ReadDword(HKEY key, const WCHAR *name, DWORD *value)
{
    DWORD size = sizeof(*value);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, value, &size);
}

DWORD ReadConfig(warp_config *cfg)
{
    HKEY key;
    DWORD error = RegOpenKeyExW(USBWARP_CONFIG_ROOT, USBWARP_SERVICE_REG_KEY, 0,
                               KEY_READ | KEY_WOW64_64KEY, &key);
    if (error == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (error) return error;
    StoredConfig saved{};
    DWORD size = sizeof(saved);
    error = RegGetValueW(key, nullptr, L"Config", RRF_RT_REG_BINARY, nullptr, &saved, &size);
    if (!error) {
        if (size != sizeof(saved) || saved.magic != ConfigMagic || saved.version != 1 ||
            saved.size != sizeof(saved) || saved.vm_id[255] || (saved.flags & ~3u)) {
            error = ERROR_INVALID_DATA;
        } else {
            memcpy(cfg->vm_id, saved.vm_id, sizeof(cfg->vm_id));
            cfg->shm_size = saved.shm; cfg->ring_data_size = saved.ring;
            cfg->buffer_size = saved.buffer; cfg->max_devices = saved.max_devices;
            cfg->enable_iso = (saved.flags & 1) != 0;
            cfg->debug = (saved.flags & 2) != 0;
        }
    } else if (error == ERROR_FILE_NOT_FOUND) {
        // Read old installations once; new writes use one atomic REG_BINARY value.
        size = sizeof(cfg->vm_id);
        error = RegGetValueW(key, nullptr, L"VmId", RRF_RT_REG_SZ, nullptr, cfg->vm_id, &size);
        if (error == ERROR_FILE_NOT_FOUND) error = ERROR_SUCCESS;
        const WCHAR *names[] = {L"ShmSizeMb", L"RingSizeKb", L"BufferSizeKb", L"MaxDevices"};
        uint32_t *outputs[] = {&cfg->shm_size, &cfg->ring_data_size, &cfg->buffer_size, &cfg->max_devices};
        const uint32_t scales[] = {1024u * 1024u, 1024, 1024, 1};
        for (unsigned i = 0; !error && i < 4; ++i) {
            DWORD value, e = ReadDword(key, names[i], &value);
            if (e == ERROR_FILE_NOT_FOUND) continue;
            if (e || value > UINT32_MAX / scales[i]) { error = e ? e : ERROR_INVALID_DATA; break; }
            *outputs[i] = value * scales[i];
        }
        DWORD value, e = ReadDword(key, L"EnableIso", &value);
        if (!e) cfg->enable_iso = value != 0;
        else if (!error && e != ERROR_FILE_NOT_FOUND) error = e;
    }
    RegCloseKey(key);
    return error;
}
}

bool ParseArgOverrides(int argc, WCHAR *argv[], warp_config *cfg, bool install)
{
    bool sawInstall = false;
    for (int i = 1; i < argc; ++i) {
        const WCHAR *arg = argv[i];
        if (install && !_wcsicmp(arg, L"--install") && !sawInstall) { sawInstall = true; continue; }
        if (!_wcsicmp(arg, L"--iso")) { cfg->enable_iso = true; continue; }
        if (!_wcsicmp(arg, L"--no-iso")) { cfg->enable_iso = false; continue; }
        if (!_wcsicmp(arg, L"--debug")) { cfg->debug = true; continue; }
        if (!_wcsicmp(arg, L"--no-debug")) { cfg->debug = false; continue; }
        if (!_wcsicmp(arg, L"--id")) {
            if (++i == argc || wcslen(argv[i]) >= _countof(cfg->vm_id)) return false;
            wcscpy_s(cfg->vm_id, argv[i]);
            continue;
        }
        uint32_t *out = nullptr, scale = 1;
        if (!_wcsicmp(arg, L"--shm-size")) { out = &cfg->shm_size; scale = 1024 * 1024; }
        else if (!_wcsicmp(arg, L"--ring-size")) { out = &cfg->ring_data_size; scale = 1024; }
        else if (!_wcsicmp(arg, L"--buffer-size")) { out = &cfg->buffer_size; scale = 1024; }
        else if (!_wcsicmp(arg, L"--max-devices")) out = &cfg->max_devices;
        if (!out || ++i == argc || !Number(argv[i], scale, out)) return false;
    }
    return true;
}

DWORD ValidateConfig(warp_config *cfg)
{
    size_t length = wcsnlen_s(cfg->vm_id, _countof(cfg->vm_id));
    WCHAR braced[40];
    GUID guid;
    if (length == 36) swprintf_s(braced, L"{%s}", cfg->vm_id);
    else if (length == 38) wcscpy_s(braced, cfg->vm_id);
    else return ERROR_INVALID_PARAMETER;
    if (FAILED(CLSIDFromString(braced, &guid))) return ERROR_INVALID_PARAMETER;
    StringFromGUID2(guid, braced, _countof(braced));
    memcpy(cfg->vm_id, braced + 1, 36 * sizeof(WCHAR));
    cfg->vm_id[36] = 0;
    if (!USBWARP_IS_POWER_OF_2(cfg->shm_size) || cfg->shm_size < USBWARP_SHM_SIZE_MIN ||
        cfg->shm_size > USBWARP_SHM_SIZE_MAX || !USBWARP_IS_POWER_OF_2(cfg->ring_data_size) ||
        cfg->ring_data_size < USBWARP_RING_DATA_SIZE_MIN || cfg->ring_data_size > USBWARP_RING_DATA_SIZE_MAX ||
        !USBWARP_IS_POWER_OF_2(cfg->buffer_size) || cfg->buffer_size < USBWARP_BUFFER_SIZE_MIN ||
        cfg->buffer_size > USBWARP_BUFFER_SIZE_MAX || !cfg->max_devices ||
        cfg->max_devices > USBWARP_MAX_DEVICES_LIMIT)
        return ERROR_INVALID_PARAMETER;
    uint64_t needed = USBWARP_CONTROL_BLOCK_SIZE +
        2ull * USBWARP_ALIGN_PAGE(USBWARP_RING_HEADER_SIZE + cfg->ring_data_size) +
        (uint64_t)cfg->buffer_size * USBWARP_BUFFER_QUOTA_MIN;
    return needed <= cfg->shm_size ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}

DWORD LoadConfig(int argc, WCHAR *argv[], warp_config *cfg, bool install)
{
    warp_config_defaults(cfg);
    DWORD error = ReadConfig(cfg);
    if (error) return error;
    if (!ParseArgOverrides(argc, argv, cfg, install)) return ERROR_INVALID_PARAMETER;
    return ValidateConfig(cfg);
}

DWORD SaveRegistryConfig(const warp_config *cfg)
{
    warp_config copy = *cfg;
    DWORD error = ValidateConfig(&copy);
    if (error) return error;
    StoredConfig saved{};
    saved.magic = ConfigMagic; saved.version = 1; saved.size = sizeof(saved);
    memcpy(saved.vm_id, copy.vm_id, sizeof(saved.vm_id));
    saved.shm = copy.shm_size; saved.ring = copy.ring_data_size;
    saved.buffer = copy.buffer_size; saved.max_devices = copy.max_devices;
    saved.flags = (copy.enable_iso ? 1 : 0) | (copy.debug ? 2 : 0);
    HKEY key;
    error = RegCreateKeyExW(USBWARP_CONFIG_ROOT, USBWARP_SERVICE_REG_KEY, 0, nullptr, 0,
                            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (error) return error;
    error = RegSetValueExW(key, L"Config", 0, REG_BINARY, (const BYTE *)&saved, sizeof(saved));
    RegCloseKey(key);
    return error;
}

DWORD RemoveRegistryConfig()
{
    HKEY key;
    DWORD error = RegOpenKeyExW(USBWARP_CONFIG_ROOT, USBWARP_SERVICE_REG_KEY, 0,
        DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (error == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (error) return error;
    error = RegDeleteTreeW(key, nullptr);
    RegCloseKey(key);
    if (!error) error = RegDeleteKeyExW(USBWARP_CONFIG_ROOT, USBWARP_SERVICE_REG_KEY, KEY_WOW64_64KEY, 0);
    return error;
}

void PrintUsage()
{
    fprintf(stderr, "USBWarpService [--id VM-GUID] [--iso|--no-iso] [--debug|--no-debug]\n"
        "  --shm-size MB --ring-size KB --buffer-size KB --max-devices N\n"
        "  --install [options]    install/update the Windows service (administrator)\n"
        "  --uninstall            stop and remove the Windows service\n"
        "  --version              print the product version\n"
        "Sizes must be powers of two. --id is required unless saved by --install.\n");
}
