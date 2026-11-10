// SPDX-License-Identifier: GPL-2.0-only
#include "warp_service.h"
#include <algorithm>

namespace {
struct Exclusive {
    SRWLOCK *lock;
    explicit Exclusive(SRWLOCK *p) : lock(p) { AcquireSRWLockExclusive(p); }
    ~Exclusive() { ReleaseSRWLockExclusive(lock); }
};
bool Utf8(const WCHAR *source, char *out, int capacity)
{ return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source, -1, out, capacity, nullptr, nullptr) != 0; }
}

void VmRefresh(warp_session *s)
{
    std::vector<warp_vm> found;
    DWORD error = EnumerateVms(s, found);
    Exclusive lock(&s->targetLock);
    bool changed = !s->vmListGeneration || error != s->vmDiscoveryError || (!error &&
        (found.size() != s->vms.size() || (!found.empty() &&
            memcmp(found.data(), s->vms.data(), found.size() * sizeof(warp_vm)))));
    s->vmDiscoveryError = error;
    if (!error) s->vms.swap(found);
    // Stable snapshots survive periodic refreshes so slow pagination does not
    // starve merely because an identical enumeration completed in between.
    if (changed && !++s->vmListGeneration) ++s->vmListGeneration;
}
DWORD VmList(warp_session *s, const usbwarp_pipe_vm_list_request &request, std::vector<BYTE> &reply)
{
    if (request.reserved || (request.offset && !request.generation)) return ERROR_INVALID_DATA;
    Exclusive lock(&s->targetLock);
    if (request.generation && request.generation != s->vmListGeneration) return ERROR_RETRY;
    if (request.offset > s->vms.size()) return ERROR_INVALID_PARAMETER;
    uint32_t count = (uint32_t)std::min<size_t>(USBWARP_VM_PAGE_SIZE, s->vms.size() - request.offset);
    reply.assign(sizeof(usbwarp_pipe_vm_list) + count * sizeof(usbwarp_pipe_vm_entry), 0);
    auto header = (usbwarp_pipe_vm_list *)reply.data();
    header->generation = s->vmListGeneration; header->total_count = (uint32_t)s->vms.size();
    header->count = count; header->error = s->vmDiscoveryError;
    auto entries = (usbwarp_pipe_vm_entry *)(header + 1);
    for (uint32_t i = 0; i < count; ++i) {
        const auto &vm = s->vms[request.offset + i];
        if (!Utf8(vm.id, entries[i].id, sizeof(entries[i].id)) ||
            !Utf8(vm.name, entries[i].name, sizeof(entries[i].name)) ||
            !Utf8(vm.owner, entries[i].owner, sizeof(entries[i].owner)) ||
            !Utf8(vm.state, entries[i].state, sizeof(entries[i].state))) return ERROR_INVALID_DATA;
    }
    return ERROR_SUCCESS;
}
void VmTargetStatus(warp_session *s, usbwarp_pipe_target_status *status)
{
    Exclusive lock(&s->targetLock);
    memset(status, 0, sizeof(*status));
    TargetToWire(s->selectedTarget, &status->selected);
    Utf8(s->resolvedVmId, status->resolved_id, sizeof(status->resolved_id));
    Utf8(s->attachedVmId, status->attached_id, sizeof(status->attached_id));
    status->matches = s->targetMatches; status->error = s->targetError;
    status->switching = s->attachedGeneration && s->attachedGeneration != s->targetGeneration;
}
DWORD VmSelect(warp_session *s, const warp_target &target)
{
    warp_target checked = target;
    DWORD error = ValidateTarget(&checked);
    if (error) return error;
    // Same order as bind: capture -> target -> driver. No PnP transition may
    // race the zero-bindings check or admit a bind for the departing target.
    Exclusive capture(&s->captureLock);
    Exclusive selection(&s->targetLock);
    if (StopRequested(s)) return ERROR_OPERATION_ABORTED;
    if (!memcmp(&checked, &s->selectedTarget, sizeof(checked))) return ERROR_SUCCESS;
    if (DrvClientActive(s)) {
        USBWARP_ENUM_DEVICES_OUT devices{};
        if (!DrvClientEnumDevices(s, &devices)) return GetLastError();
        if (devices.DeviceCount) return ERROR_BUSY;
    }
    bool pending = false;
    error = DeviceCapturePending(&pending);
    if (error || pending) return error ? error : ERROR_BUSY;
    warp_config saved = s->cfg; saved.target = checked;
    error = SaveRegistryConfig(&saved);
    if (error) return error; // failed persistence leaves the running selection intact
    s->selectedTarget = checked;
    if (!++s->targetGeneration) ++s->targetGeneration;
    s->resolvedVmId[0] = 0; s->targetMatches = 0;
    s->targetError = checked.kind == USBWARP_TARGET_NONE ? ERROR_SUCCESS : ERROR_NOT_READY;
    s->stage.store(checked.kind == USBWARP_TARGET_NONE ? USBWARP_SESSION_WAIT_TARGET : USBWARP_SESSION_RETRYING);
    SetEvent(s->targetChangedEvent);
    return ERROR_SUCCESS;
}
uint64_t VmSelection(warp_session *s, warp_target *target)
{
    Exclusive lock(&s->targetLock);
    *target = s->selectedTarget;
    return s->targetGeneration;
}
bool VmCurrent(warp_session *s, uint64_t generation)
{
    Exclusive lock(&s->targetLock);
    return s->targetGeneration == generation;
}
bool VmCanBind(warp_session *s)
{
    Exclusive lock(&s->targetLock);
    return s->selectedTarget.kind != USBWARP_TARGET_NONE &&
        s->attachedGeneration == s->targetGeneration && s->stage.load() == USBWARP_SESSION_RUNNING;
}
bool VmStage(warp_session *s, uint64_t generation, uint32_t stage)
{
    Exclusive lock(&s->targetLock);
    if (s->targetGeneration != generation) return false;
    s->stage.store(stage);
    return true;
}
DWORD VmResolve(warp_session *s, const warp_target &target, uint64_t generation)
{
    Exclusive lock(&s->targetLock);
    if (generation != s->targetGeneration) return ERROR_RETRY;
    s->resolvedVmId[0] = 0; s->targetMatches = 0;
    DWORD error = s->vmDiscoveryError;
    // Fixed-ID mode retains compatibility even when enumeration is unavailable.
    // HcsOpenComputeSystem still decides whether that exact ID exists/access works.
    if (target.kind == USBWARP_TARGET_ID) {
        wcscpy_s(s->resolvedVmId, target.value);
        error = ERROR_SUCCESS;
    } else if (!error) error = MatchVm(target, s->vms, s->resolvedVmId, &s->targetMatches);
    s->targetError = target.kind == USBWARP_TARGET_NONE ? ERROR_SUCCESS : error;
    if (!error) wcscpy_s(s->openingVmId, s->resolvedVmId);
    return target.kind == USBWARP_TARGET_NONE ? ERROR_NOT_READY : error;
}
bool VmAttached(warp_session *s, uint64_t generation)
{
    Exclusive lock(&s->targetLock);
    if (s->targetGeneration != generation) return false;
    s->attachedGeneration = generation;
    wcscpy_s(s->attachedVmId, s->openingVmId);
    return true;
}
void VmFailure(warp_session *s, uint64_t generation, DWORD error)
{
    Exclusive lock(&s->targetLock);
    if (s->targetGeneration == generation) s->targetError = error;
}
void VmDetached(warp_session *s)
{
    Exclusive lock(&s->targetLock);
    s->attachedGeneration = 0;
    s->attachedVmId[0] = s->resolvedVmId[0] = 0;
    s->targetMatches = 0;
    s->targetError = s->selectedTarget.kind == USBWARP_TARGET_NONE ? ERROR_SUCCESS : ERROR_NOT_READY;
}
