// SPDX-License-Identifier: GPL-2.0-only
#include "warp_service.h"
#include <stdio.h>
#include <errno.h>
#include <wchar.h>

#ifndef USBWARP_CONFIG_ROOT
#define USBWARP_CONFIG_ROOT HKEY_LOCAL_MACHINE
#endif

namespace {
struct StoredConfigV1 {
    DWORD magic, version, size;
    WCHAR vm_id[256];
    DWORD shm, ring, buffer, max_devices, flags;
};
struct StoredConfig {
    DWORD magic, version, size;
    warp_target target;
    DWORD shm, ring, buffer, max_devices, flags;
};
static_assert(sizeof(StoredConfigV1) == 544, "legacy config size");
static_assert(sizeof(StoredConfig) == 676, "config v2 size");
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
    BYTE saved[sizeof(StoredConfig)]{};
    DWORD size = sizeof(saved);
    error = RegGetValueW(key, nullptr, L"Config", RRF_RT_REG_BINARY, nullptr, saved, &size);
    if (!error) {
        error = DecodeRegistryConfig(saved, size, cfg);
    } else if (error == ERROR_FILE_NOT_FOUND) {
        // Read old installations once; new writes use one atomic REG_BINARY value.
        size = sizeof(cfg->target.value);
        error = RegGetValueW(key, nullptr, L"VmId", RRF_RT_REG_SZ, nullptr, cfg->target.value, &size);
        if (error == ERROR_FILE_NOT_FOUND) error = ERROR_SUCCESS;
        if (!error && cfg->target.value[0]) cfg->target.kind = USBWARP_TARGET_ID;
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

DWORD DecodeRegistryConfig(const BYTE *data, DWORD bytes, warp_config *cfg)
{
    if (!data || !cfg || bytes < 3 * sizeof(DWORD)) return ERROR_INVALID_DATA;
    DWORD header[3]; memcpy(header, data, sizeof(header));
    if (header[0] != ConfigMagic || header[2] != bytes) return ERROR_INVALID_DATA;
    StoredConfig saved{};
    if (header[1] == 1 && bytes == sizeof(StoredConfigV1)) {
        StoredConfigV1 old; memcpy(&old, data, sizeof(old));
        saved.target.kind = USBWARP_TARGET_ID;
        // v1 normalized {GUID} in place without clearing the old trailing '}'.
        // Migrate the bounded string, not its obsolete padding bytes.
        if (old.vm_id[255]) return ERROR_INVALID_DATA;
        wcscpy_s(saved.target.value, old.vm_id);
        saved.shm = old.shm; saved.ring = old.ring; saved.buffer = old.buffer;
        saved.max_devices = old.max_devices; saved.flags = old.flags;
    } else if (header[1] == 2 && bytes == sizeof(saved)) {
        memcpy(&saved, data, sizeof(saved));
    } else return ERROR_INVALID_DATA;
    if (saved.flags & ~3u) return ERROR_INVALID_DATA;
    warp_config candidate{};
    candidate.target = saved.target;
    candidate.shm_size = saved.shm; candidate.ring_data_size = saved.ring;
    candidate.buffer_size = saved.buffer; candidate.max_devices = saved.max_devices;
    candidate.enable_iso = (saved.flags & 1) != 0;
    candidate.debug = (saved.flags & 2) != 0;
    if (ValidateConfig(&candidate)) return ERROR_INVALID_DATA;
    *cfg = candidate;
    return ERROR_SUCCESS;
}

bool ParseArgOverrides(int argc, WCHAR *argv[], warp_config *cfg, bool install)
{
    bool sawInstall = false, sawTarget = false;
    const WCHAR *owner = nullptr;
    for (int i = 1; i < argc; ++i) {
        const WCHAR *arg = argv[i];
        if (install && !_wcsicmp(arg, L"--install") && !sawInstall) { sawInstall = true; continue; }
        if (!_wcsicmp(arg, L"--iso")) { cfg->enable_iso = true; continue; }
        if (!_wcsicmp(arg, L"--no-iso")) { cfg->enable_iso = false; continue; }
        if (!_wcsicmp(arg, L"--debug")) { cfg->debug = true; continue; }
        if (!_wcsicmp(arg, L"--no-debug")) { cfg->debug = false; continue; }
        if (!_wcsicmp(arg, L"--wsl") || !_wcsicmp(arg, L"--no-vm")) {
            if (sawTarget) return false;
            sawTarget = true;
            cfg->target = warp_target{};
            cfg->target.kind = !_wcsicmp(arg, L"--wsl") ? USBWARP_TARGET_WSL : USBWARP_TARGET_NONE;
            continue;
        }
        if (!_wcsicmp(arg, L"--id") || !_wcsicmp(arg, L"--vm-name")) {
            if (sawTarget || ++i == argc || wcslen(argv[i]) >= _countof(cfg->target.value)) return false;
            sawTarget = true;
            cfg->target = warp_target{};
            cfg->target.kind = !_wcsicmp(arg, L"--id") ? USBWARP_TARGET_ID : USBWARP_TARGET_NAME;
            wcscpy_s(cfg->target.value, argv[i]);
            continue;
        }
        if (!_wcsicmp(arg, L"--vm-owner")) {
            if (owner || ++i == argc || !argv[i][0] || wcslen(argv[i]) >= _countof(cfg->target.owner)) return false;
            owner = argv[i];
            continue;
        }
        uint32_t *out = nullptr, scale = 1;
        if (!_wcsicmp(arg, L"--shm-size")) { out = &cfg->shm_size; scale = 1024 * 1024; }
        else if (!_wcsicmp(arg, L"--ring-size")) { out = &cfg->ring_data_size; scale = 1024; }
        else if (!_wcsicmp(arg, L"--buffer-size")) { out = &cfg->buffer_size; scale = 1024; }
        else if (!_wcsicmp(arg, L"--max-devices")) out = &cfg->max_devices;
        if (!out || ++i == argc || !Number(argv[i], scale, out)) return false;
    }
    if (owner) {
        if (cfg->target.kind != USBWARP_TARGET_NAME) return false;
        memset(cfg->target.owner, 0, sizeof(cfg->target.owner));
        wcscpy_s(cfg->target.owner, owner);
    }
    return true;
}

DWORD ValidateConfig(warp_config *cfg)
{
    if (ValidateTarget(&cfg->target)) return ERROR_INVALID_PARAMETER;
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
    saved.magic = ConfigMagic; saved.version = 2; saved.size = sizeof(saved);
    saved.target = copy.target;
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
    fprintf(stderr, "USBWarpService [--iso|--no-iso] [--debug|--no-debug]\n"
        "  --wsl | --vm-name NAME [--vm-owner OWNER] | --id GUID | --no-vm\n"
        "  --shm-size MB --ring-size KB --buffer-size KB --max-devices N\n"
        "  --install [options]    install/update the Windows service (administrator)\n"
        "  --uninstall            stop and remove the Windows service\n"
        "  --version              print the product version\n"
        "Sizes must be powers of two. A target is optional during installation.\n"
        "Use USBWarpController vm list/use/status/clear to manage the target.\n");
}
