// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <windows.h>
#include <vector>
#include "../include/usbwarp_pipe.h"

struct warp_target {
    uint32_t kind = USBWARP_TARGET_NONE;
    WCHAR value[256]{};
    WCHAR owner[64]{};
};
struct warp_vm {
    WCHAR id[256]{};
    WCHAR name[256]{};
    WCHAR owner[64]{};
    WCHAR state[32]{};
};
struct warp_session;
DWORD ValidateTarget(warp_target *target);
DWORD TargetFromWire(const usbwarp_pipe_target &wire, warp_target *target);
DWORD TargetToWire(const warp_target &target, usbwarp_pipe_target *wire);
DWORD ParseVmDocument(const WCHAR *json, std::vector<warp_vm> &vms);
DWORD MatchVm(const warp_target &target, const std::vector<warp_vm> &vms,
              WCHAR (&id)[256], uint32_t *matches);
DWORD EnumerateVms(warp_session *s, std::vector<warp_vm> &vms);
void VmRefresh(warp_session *s);
DWORD VmList(warp_session *s, const usbwarp_pipe_vm_list_request &request, std::vector<BYTE> &reply);
void VmTargetStatus(warp_session *s, usbwarp_pipe_target_status *status);
DWORD VmSelect(warp_session *s, const warp_target &target);
uint64_t VmSelection(warp_session *s, warp_target *target);
bool VmCurrent(warp_session *s, uint64_t generation);
bool VmCanBind(warp_session *s);
bool VmStage(warp_session *s, uint64_t generation, uint32_t stage);
DWORD VmResolve(warp_session *s, const warp_target &target, uint64_t generation);
void VmDetached(warp_session *s);
bool VmAttached(warp_session *s, uint64_t generation);
void VmFailure(warp_session *s, uint64_t generation, DWORD error);
bool VmAttached(warp_session *s, uint64_t generation);
