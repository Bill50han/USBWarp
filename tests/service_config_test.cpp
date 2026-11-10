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
    CHECK(ValidateConfig(&cfg) == ERROR_SUCCESS && cfg.target.kind == USBWARP_TARGET_NONE);
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
    memset(cfg.target.value, 0, sizeof(cfg.target.value));
    wcscpy_s(cfg.target.value, L"not-a-vm-id");
    CHECK(ValidateConfig(&cfg) != ERROR_SUCCESS);
    memset(cfg.target.value, 0, sizeof(cfg.target.value));
    wcscpy_s(cfg.target.value, vm);
    CHECK(ValidateConfig(&cfg) == ERROR_SUCCESS);
    struct Legacy { DWORD magic, version, size; WCHAR vm[256]; DWORD shm, ring, buffer, max, flags; } legacy{};
    legacy.magic=0x43425755; legacy.version=1; legacy.size=sizeof(legacy);
    wcscpy_s(legacy.vm,vm); legacy.shm=cfg.shm_size; legacy.ring=cfg.ring_data_size;
    legacy.buffer=cfg.buffer_size; legacy.max=cfg.max_devices; legacy.flags=3;
    warp_config migrated{};
    CHECK(DecodeRegistryConfig((const BYTE *)&legacy,sizeof(legacy),&migrated)==ERROR_SUCCESS);
    CHECK(migrated.target.kind==USBWARP_TARGET_ID && !wcscmp(migrated.target.value,vm) && migrated.enable_iso && migrated.debug);
    legacy.vm[37]=L'}';CHECK(DecodeRegistryConfig((const BYTE *)&legacy,sizeof(legacy),&migrated)==ERROR_SUCCESS);
    CHECK(!migrated.target.value[37]);legacy.vm[37]=0;
    legacy.flags=4; CHECK(DecodeRegistryConfig((const BYTE *)&legacy,sizeof(legacy),&migrated)==ERROR_INVALID_DATA);
    legacy.flags=0; legacy.vm[255]=L'x'; CHECK(DecodeRegistryConfig((const BYTE *)&legacy,sizeof(legacy),&migrated)==ERROR_INVALID_DATA);
    WCHAR wsl[]=L"--wsl", name[]=L"--vm-name", owner[]=L"--vm-owner", label[]=L"Ubuntu Test", vmms[]=L"VMMS";
    WCHAR *wslArgs[]={exe,install,wsl};
    warp_config_defaults(&cfg);
    CHECK(ParseArgOverrides(3,wslArgs,&cfg,true) && ValidateConfig(&cfg)==0 && cfg.target.kind==USBWARP_TARGET_WSL);
    WCHAR *nameArgs[]={exe,owner,vmms,name,label};
    CHECK(ParseArgOverrides(5,nameArgs,&cfg) && ValidateConfig(&cfg)==0);
    CHECK(cfg.target.kind==USBWARP_TARGET_NAME && !wcscmp(cfg.target.value,label) && !wcscmp(cfg.target.owner,vmms));
    WCHAR *conflict[]={exe,wsl,id,vm}; CHECK(!ParseArgOverrides(4,conflict,&cfg));
    WCHAR *badOwner[]={exe,wsl,owner,vmms}; CHECK(!ParseArgOverrides(4,badOwner,&cfg));
    CHECK(usbwarp_binding_matches(USBWARP_CAP_DEVICE_EPOCH, 9, 9));
    CHECK(!usbwarp_binding_matches(USBWARP_CAP_DEVICE_EPOCH, 8, 9));
    UINT64 before = NowNs(); Sleep(2); CHECK(NowNs() > before);
    printf("Service configuration: %u checks passed\n", checks);
    return 0;
}
