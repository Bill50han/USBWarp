/* SPDX-License-Identifier: GPL-2.0-only */
#include "usbwarp_cli.h"
#include <shellapi.h>
#include <wchar.h>
#include <stddef.h>
#pragma comment(lib, "shell32.lib")

#define VM_MAX_ENTRIES 1024u
static bool VmText(const char *text, size_t capacity)
{
    const char *end = (const char *)memchr(text, 0, capacity);
    WCHAR decoded[1024];
    if (!end || end - text >= (ptrdiff_t)_countof(decoded)) return false;
    for (const char *p = end; p != text + capacity; ++p) if (*p) return false;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text,
        (int)(end - text + 1), decoded, _countof(decoded));
    if (!n) return false;
    for (int i = 0; i + 1 < n; ++i)
        if (decoded[i] < 32 || (decoded[i] >= 127 && decoded[i] <= 159)) return false;
    return true;
}
static bool VmTargetValid(const struct usbwarp_pipe_target *target)
{
    if (target->reserved || target->kind > USBWARP_TARGET_WSL ||
        !VmText(target->value, sizeof(target->value)) || !VmText(target->owner, sizeof(target->owner))) return false;
    if (target->kind == USBWARP_TARGET_NONE || target->kind == USBWARP_TARGET_WSL)
        return !target->value[0] && !target->owner[0];
    return target->value[0] && (target->kind == USBWARP_TARGET_NAME || !target->owner[0]);
}
static bool VmEntryValid(const struct usbwarp_pipe_vm_entry *entry)
{
    return entry->id[0] && VmText(entry->id, sizeof(entry->id)) &&
        VmText(entry->name, sizeof(entry->name)) && VmText(entry->owner, sizeof(entry->owner)) &&
        VmText(entry->state, sizeof(entry->state));
}
static bool VmPageValid(const BYTE *data, uint32_t size, uint32_t offset, uint64_t generation)
{
    if (size < sizeof(struct usbwarp_pipe_vm_list)) return false;
    const struct usbwarp_pipe_vm_list *page = (const struct usbwarp_pipe_vm_list *)data;
    if (page->reserved || page->count > USBWARP_VM_PAGE_SIZE || page->total_count > VM_MAX_ENTRIES ||
        offset > page->total_count || page->count > page->total_count - offset ||
        size != sizeof(*page) + page->count * sizeof(struct usbwarp_pipe_vm_entry) ||
        (generation && generation != page->generation) || (!page->count && offset < page->total_count)) return false;
    const struct usbwarp_pipe_vm_entry *entries = (const struct usbwarp_pipe_vm_entry *)(page + 1);
    for (uint32_t i = 0; i < page->count; ++i) if (!VmEntryValid(entries + i)) return false;
    return true;
}
static bool VmStatusValid(const BYTE *data, uint32_t size)
{
    if (size != sizeof(struct usbwarp_pipe_target_status)) return false;
    const struct usbwarp_pipe_target_status *s = (const struct usbwarp_pipe_target_status *)data;
    return !s->reserved && s->switching <= 1 && s->matches <= VM_MAX_ENTRIES &&
        VmTargetValid(&s->selected) && VmText(s->resolved_id, sizeof(s->resolved_id)) &&
        VmText(s->attached_id, sizeof(s->attached_id));
}
static void VmError(DWORD error)
{
    WCHAR message[512] = {0};
    char utf8[2048] = {0};
    if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, error, 0, message, _countof(message), NULL)) {
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, message, -1, utf8, sizeof(utf8), NULL, NULL);
        fprintf(stderr, "Error %lu (0x%08lX): %s", error, error, utf8);
    } else fprintf(stderr, "Error %lu (0x%08lX).\n", error, error);
}
/* Returns a Windows error, or ERROR_INVALID_DATA for malformed replies. */
static DWORD VmExchange(PIPE_CLIENT *client, uint32_t command, const void *request, uint32_t bytes,
                        BYTE *buffer, const BYTE **payload, uint32_t *length)
{
    uint32_t size = 0;
    *payload = NULL; *length = 0;
    if (PipeSendCommand(client, command, request, bytes, buffer, PIPE_BUF_SIZE, &size)) return ERROR_BROKEN_PIPE;
    uint32_t prefix = sizeof(struct usbwarp_pipe_header) + sizeof(struct usbwarp_pipe_response);
    if (size < prefix) return ERROR_INVALID_DATA;
    const struct usbwarp_pipe_response *response = (const struct usbwarp_pipe_response *)(buffer + sizeof(struct usbwarp_pipe_header));
    if (response->detail_length != size - prefix) return ERROR_INVALID_DATA;
    if (response->status) {
        if (response->status > 0 || response->status == INT32_MIN) return ERROR_INVALID_DATA;
        return (DWORD)-response->status;
    }
    *payload = buffer + prefix; *length = size - prefix;
    return ERROR_SUCCESS;
}
static int VmListCommand(PIPE_CLIENT *client, BYTE *buffer)
{
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        struct usbwarp_pipe_vm_list_request request = {0};
        struct usbwarp_pipe_vm_entry *all = NULL;
        uint32_t total = 0;
        DWORD error = ERROR_SUCCESS;
        do {
            const BYTE *data; uint32_t size;
            error = VmExchange(client, USBWARP_PIPE_CMD_LIST_VMS, &request, sizeof(request), buffer, &data, &size);
            if (error) break;
            if (!VmPageValid(data, size, request.offset, request.generation)) { error = ERROR_INVALID_DATA; break; }
            const struct usbwarp_pipe_vm_list *page = (const struct usbwarp_pipe_vm_list *)data;
            if (page->error) { error = page->error; break; }
            if (!page->generation) { error = ERROR_NOT_READY; break; }
            if (!request.offset) {
                total = page->total_count;
                if (total) {
                    all = (struct usbwarp_pipe_vm_entry *)calloc(total, sizeof(*all));
                    if (!all) { error = ERROR_NOT_ENOUGH_MEMORY; break; }
                }
                request.generation = page->generation;
            } else if (total != page->total_count) { error = ERROR_INVALID_DATA; break; }
            if (page->count) memcpy(all + request.offset, page + 1, page->count * sizeof(*all));
            request.offset += page->count;
        } while (request.offset < total);
        if (!error) {
            printf("NAME | OWNER | STATE | CURRENT HCS ID\n");
            for (uint32_t i = 0; i < total; ++i)
                printf("%s | %s | %s | %s\n", all[i].name[0] ? all[i].name : "(unnamed)",
                    all[i].owner[0] ? all[i].owner : "-", all[i].state[0] ? all[i].state : "(not reported)", all[i].id);
            if (!total) printf("No HCS virtual machines are currently visible to the service.\n");
            printf("WSL: vm use --wsl. Named VM: vm use --name NAME [--owner OWNER].\n");
        }
        free(all);
        if (error == ERROR_RETRY) continue;
        if (error) VmError(error);
        return error ? 1 : 0;
    }
    fprintf(stderr, "VM list changed repeatedly; retry vm list.\n");
    return 1;
}
static int VmStatusCommand(PIPE_CLIENT *client, BYTE *buffer)
{
    const BYTE *data; uint32_t size;
    DWORD error = VmExchange(client, USBWARP_PIPE_CMD_QUERY_TARGET, NULL, 0, buffer, &data, &size);
    if (!error && !VmStatusValid(data, size)) error = ERROR_INVALID_DATA;
    if (error) { VmError(error); return 1; }
    const struct usbwarp_pipe_target_status *s = (const struct usbwarp_pipe_target_status *)data;
    printf("Selected target: ");
    switch (s->selected.kind) {
    case USBWARP_TARGET_NONE: printf("none (use vm use to select one)\n"); break;
    case USBWARP_TARGET_WSL: printf("WSL (automatic ID; unique running WSL VM visible to service)\n"); break;
    case USBWARP_TARGET_ID: printf("fixed ID %s\n", s->selected.value); break;
    case USBWARP_TARGET_NAME:
        printf("name '%s', owner '%s'\n", s->selected.value, s->selected.owner[0] ? s->selected.owner : "any"); break;
    }
    printf("Resolved ID:     %s\n", s->resolved_id[0] ? s->resolved_id : "-");
    printf("Attached ID:     %s\n", s->attached_id[0] ? s->attached_id : "-");
    if (s->switching) printf("Target change is draining the previous session.\n");
    if (s->error == ERROR_DUP_NAME) printf("Ambiguous target: %u matches. Specify --owner or a fixed --id.\n", s->matches);
    else if (s->error == ERROR_NOT_FOUND || s->error == ERROR_NOT_READY)
        printf("Waiting for the selected VM; start it normally. The service will retry.\n");
    else if (s->error) VmError(s->error);
    error = VmExchange(client, USBWARP_PIPE_CMD_QUERY_STATUS, NULL, 0, buffer, &data, &size);
    if (error || size != sizeof(struct usbwarp_pipe_status)) {
        if (!error) error = ERROR_INVALID_DATA;
        VmError(error); return 1;
    }
    printf("USB session:     %s\n", SessionStateName(((const struct usbwarp_pipe_status *)data)->session_state));
    return 0;
}
static bool VmBuildTarget(int argc, WCHAR **argv, struct usbwarp_pipe_target *target)
{
    memset(target, 0, sizeof(*target));
    const WCHAR *value = NULL, *owner = NULL;
    bool chosen = false;
    for (int i = 0; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--wsl")) {
            if (chosen) return false;
            chosen = true; target->kind = USBWARP_TARGET_WSL;
        } else if (!_wcsicmp(argv[i], L"--name") || !_wcsicmp(argv[i], L"--id")) {
            if (chosen || i + 1 >= argc) return false;
            chosen = true;
            target->kind = !_wcsicmp(argv[i], L"--name") ? USBWARP_TARGET_NAME : USBWARP_TARGET_ID;
            value = argv[++i];
            if (!*value || wcslen(value) >= 256) return false;
        } else if (!_wcsicmp(argv[i], L"--owner")) {
            if (owner || ++i == argc || !*argv[i] || wcslen(argv[i]) >= 64) return false;
            owner = argv[i];
        } else return false;
    }
    if (!chosen || (owner && target->kind != USBWARP_TARGET_NAME)) return false;
    if (value && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, target->value, sizeof(target->value), NULL, NULL)) return false;
    if (owner && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, owner, -1, target->owner, sizeof(target->owner), NULL, NULL)) return false;
    if (target->kind == USBWARP_TARGET_ID) {
        WCHAR braced[40] = {0}; GUID id;
        if (wcslen(value) == 36) swprintf_s(braced, _countof(braced), L"{%ls}", value);
        else if (wcslen(value) == 38) wcscpy_s(braced, _countof(braced), value);
        else return false;
        if (FAILED(CLSIDFromString(braced, &id))) return false;
    }
    return VmTargetValid(target);
}
static void VmUsage(void)
{
    fprintf(stderr, "Usage:\n"
        "  USBWarpController vm list\n"
        "  USBWarpController vm status\n"
        "  USBWarpController vm use --wsl\n"
        "  USBWarpController vm use --name NAME [--owner OWNER]\n"
        "  USBWarpController vm use --id GUID\n"
        "  USBWarpController vm clear\n"
        "Selection persists; no service reinstall/restart is required.\n"
        "Unbind USB devices before changing target. WSL distributions share a VM.\n");
}
int CmdVm(void)
{
    int argc = 0, result = 1;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    PIPE_CLIENT client;
    BYTE *buffer = NULL;
    struct usbwarp_pipe_target target = {0};
    const WCHAR *verb = NULL;
    bool use = false, clear = false, list = false, status = false;
    UINT previousCodePage = GetConsoleOutputCP();
    client.hPipe = INVALID_HANDLE_VALUE;
    if (previousCodePage) SetConsoleOutputCP(CP_UTF8);
    if (!argv || argc < 3) { VmUsage(); goto done; }
    verb = argv[2];
    if (!_wcsicmp(verb, L"--help") && argc == 3) { VmUsage(); result = 0; goto done; }
    use = !_wcsicmp(verb, L"use"); clear = !_wcsicmp(verb, L"clear");
    list = !_wcsicmp(verb, L"list"); status = !_wcsicmp(verb, L"status");
    if ((!use && !clear && !list && !status) || (!use && argc != 3) ||
        (use && !VmBuildTarget(argc - 3, argv + 3, &target))) { VmUsage(); goto done; }
    buffer = (BYTE *)malloc(PIPE_BUF_SIZE);
    if (!buffer) { VmError(ERROR_NOT_ENOUGH_MEMORY); goto done; }
    if (PipeConnect(&client)) goto done;
    if (list) result = VmListCommand(&client, buffer);
    else if (status) result = VmStatusCommand(&client, buffer);
    else {
        const BYTE *data; uint32_t size;
        DWORD error = VmExchange(&client, USBWARP_PIPE_CMD_SET_TARGET, &target, sizeof(target), buffer, &data, &size);
        if (error) {
            if (error == ERROR_BUSY) fprintf(stderr, "Unbind all USB devices and finish pending restoration before changing target.\n");
            VmError(error);
        } else {
            printf("Target %s. Use 'vm status' to see discovery and connection state.\n", clear ? "cleared" : "saved");
            result = 0;
        }
    }
done:
    if (client.hPipe != INVALID_HANDLE_VALUE) PipeDisconnect(&client);
    free(buffer);
    if (argv) LocalFree(argv);
    fflush(stdout); fflush(stderr);
    if (previousCodePage) SetConsoleOutputCP(previousCodePage);
    return result;
}
