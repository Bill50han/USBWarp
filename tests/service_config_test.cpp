// SPDX-License-Identifier: GPL-2.0-only
#include "../USBWarpService/warp_service.h"
#include <stdio.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    WCHAR exe[] = L"service.exe", install[] = L"--install", id[] = L"--id";
    WCHAR vm[] = L"11111111-1111-1111-1111-111111111111", iso[] = L"--iso";
    WCHAR shm[] = L"--shm-size", ring[] = L"--ring-size", maximum[] = L"--max-devices";
    WCHAR good[] = L"64", negative[] = L"-1", overflow[] = L"4294967296", bad[] = L"12junk";
    WCHAR *args[] = {exe, install, id, vm, iso, shm, good};
    warp_config cfg;
    warp_config_defaults(&cfg);
    CHECK(ParseArgOverrides(_countof(args), args, &cfg, true));
    CHECK(ValidateConfig(&cfg) == ERROR_SUCCESS && cfg.enable_iso && cfg.shm_size == 64u * 1024 * 1024);
    CHECK(!ParseArgOverrides(_countof(args), args, &cfg, false));
    WCHAR *broken[] = {exe, shm, negative};
    CHECK(!ParseArgOverrides(3, broken, &cfg));
    broken[2] = overflow; CHECK(!ParseArgOverrides(3, broken, &cfg));
    broken[2] = bad; CHECK(!ParseArgOverrides(3, broken, &cfg));
    CHECK(!ParseArgOverrides(2, broken, &cfg));
    WCHAR zero[] = L"0";
    WCHAR *zeroDevices[] = {exe, maximum, zero};
    CHECK(ParseArgOverrides(3, zeroDevices, &cfg));
    CHECK(ValidateConfig(&cfg) != ERROR_SUCCESS);
    cfg.max_devices = 8;
    WCHAR odd[] = L"65";
    WCHAR *oddRing[] = {exe, ring, odd};
    CHECK(ParseArgOverrides(3, oddRing, &cfg));
    CHECK(ValidateConfig(&cfg) != ERROR_SUCCESS);
    cfg.ring_data_size = 256 * 1024;
    wcscpy_s(cfg.vm_id, L"not-a-vm-id");
    CHECK(ValidateConfig(&cfg) != ERROR_SUCCESS);
    wcscpy_s(cfg.vm_id, vm);
    CHECK(ValidateConfig(&cfg) == ERROR_SUCCESS);
    CHECK(usbwarp_binding_matches(USBWARP_CAP_DEVICE_EPOCH, 9, 9));
    CHECK(!usbwarp_binding_matches(USBWARP_CAP_DEVICE_EPOCH, 8, 9));
    UINT64 before = NowNs(); Sleep(2); CHECK(NowNs() > before);
    printf("Service configuration: %u checks passed\n", checks);
    return 0;
}
