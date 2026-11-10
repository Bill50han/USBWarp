// SPDX-License-Identifier: GPL-2.0-only
#include "../USBWarpController/vm_commands.c"
#include <vector>
int PipeConnect(PIPE_CLIENT *) {return -1;}
void PipeDisconnect(PIPE_CLIENT *) {}
int PipeSendCommand(PIPE_CLIENT *,uint32_t,const void *,uint32_t,void *,uint32_t,uint32_t *) {return -1;}
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main()
{
    WCHAR wsl[]=L"--wsl", name[]=L"--name", owner[]=L"--owner", id[]=L"--id";
    WCHAR label[]=L"开发 VM", vmms[]=L"VMMS", guid[]=L"11111111-1111-1111-1111-111111111111";
    WCHAR *a[]={name,label,owner,vmms};usbwarp_pipe_target target{};
    CHECK(VmBuildTarget(4,a,&target) && target.kind==USBWARP_TARGET_NAME && !strcmp(target.owner,"VMMS"));
    WCHAR *b[]={wsl};CHECK(VmBuildTarget(1,b,&target) && target.kind==USBWARP_TARGET_WSL);
    WCHAR *c[]={wsl,name,label};CHECK(!VmBuildTarget(3,c,&target));
    WCHAR *d[]={id,guid};CHECK(VmBuildTarget(2,d,&target) && target.kind==USBWARP_TARGET_ID);
    d[1]=label;CHECK(!VmBuildTarget(2,d,&target));
    WCHAR *e[]={owner,vmms,wsl};CHECK(!VmBuildTarget(3,e,&target));
    CHECK(!VmBuildTarget(1,a,&target));
    WCHAR bad[]=L"bad\x1b[31m";a[1]=bad;CHECK(!VmBuildTarget(4,a,&target));
    usbwarp_pipe_target_status status{};
    CHECK(VmStatusValid((BYTE *)&status,sizeof(status)));
    CHECK(!VmStatusValid((BYTE *)&status,sizeof(status)-1));
    status.selected.kind=99;CHECK(!VmStatusValid((BYTE *)&status,sizeof(status)));status={};
    status.reserved=1;CHECK(!VmStatusValid((BYTE *)&status,sizeof(status)));status={};
    memset(status.resolved_id,'x',sizeof(status.resolved_id));CHECK(!VmStatusValid((BYTE *)&status,sizeof(status)));
    std::vector<BYTE> bytes(sizeof(usbwarp_pipe_vm_list)+sizeof(usbwarp_pipe_vm_entry),0);
    auto page=(usbwarp_pipe_vm_list *)bytes.data();page->generation=4;page->count=page->total_count=1;
    auto vm=(usbwarp_pipe_vm_entry *)(page+1);strcpy_s(vm->id,"id");
    CHECK(VmPageValid(bytes.data(),(uint32_t)bytes.size(),0,0));
    CHECK(!VmPageValid(bytes.data(),(uint32_t)bytes.size()-1,0,0));
    CHECK(!VmPageValid(bytes.data(),(uint32_t)bytes.size(),0,3));
    page->total_count=1025;CHECK(!VmPageValid(bytes.data(),(uint32_t)bytes.size(),0,0));page->total_count=1;
    page->count=UINT32_MAX;CHECK(!VmPageValid(bytes.data(),(uint32_t)bytes.size(),0,0));page->count=1;
    vm->name[3]='x';CHECK(!VmPageValid(bytes.data(),(uint32_t)bytes.size(),0,0));
    printf("VM controller Unicode/protocol: %u checks passed\n",checks);return 0;
}
