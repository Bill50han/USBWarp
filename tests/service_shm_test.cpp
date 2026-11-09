// SPDX-License-Identifier: GPL-2.0-only
#include "../USBWarpService/warp_service.h"
#include <stdio.h>
void LogInfo(const char *, ...) {}
void LogWarn(const char *, ...) {}
void LogError(const char *, ...) {}
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main()
{
    warp_session s; warp_config_defaults(&s.cfg);
    DWORD before, after;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for (unsigned i=0; i<8; ++i) {
        CHECK(ShmCreate(&s));
        auto cb=(const usbwarp_control_block *)s.shm_base;
        CHECK(cb->magic==USBWARP_CONTROL_BLOCK_MAGIC && cb->shm_total_size==s.shm_size);
        CHECK(cb->g2h_ring_offset>=sizeof(*cb) &&
              cb->g2h_ring_offset+cb->g2h_ring_size<=cb->h2g_ring_offset &&
              cb->h2g_ring_offset+cb->h2g_ring_size<=cb->data_region_offset);
        CHECK((uint64_t)cb->data_region_offset+cb->data_region_size==s.shm_size &&
              (uint64_t)cb->buffer_count*cb->buffer_size<=cb->data_region_size);
        HANDLE section=s.section; void *view=s.shm_base;
        CHECK(!ShmCreate(&s) && GetLastError()==ERROR_INVALID_STATE);
        CHECK(s.section==section && s.shm_base==view && cb->magic==USBWARP_CONTROL_BLOCK_MAGIC);
        ShmDestroy(&s); CHECK(!s.section && !s.shm_base && !s.shm_size);
        ShmDestroy(&s); // repeated teardown is harmless
    }
    s.cfg.ring_data_size=1;
    CHECK(!ShmCreate(&s) && GetLastError()==ERROR_INVALID_PARAMETER && !s.section);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && after==before);
    printf("SHM ownership/layout: %u checks passed\n",checks);
    return 0;
}
