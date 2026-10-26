/* SPDX-License-Identifier: Apache-2.0 */
/* Actual native target selection/dispatch; no device I/O or real flashing. */
#include <assert.h>
#include <stdarg.h>
#include "update_engine/update.c"

static int target_slot, writes_seen, comparisons, final_status, source_both;
static long long offsets[3] = { 0x10000, 0x20000, 0x30000 };
void Log(const char *f,int l,LogPriority p,const char *fmt,...) {}
bool isMtdDevice(void) { return false; }
int mtd_scan_partitions(void) { abort(); }
const MtdPartition *mtd_find_partition_by_name(const char *n) { abort(); }
void getFlashPoint(char *p) { abort(); }
int download_file(char *url, const char *save) { return 1; }
int analyticImage(const char *path, PRKIMAGE_HDR header)
{
    const char *names[] = {"parameter","uboot_a","boot_a","system_a","uboot_b","boot_b","system_b"};
    memset(header,0,sizeof(*header));header->item_count=source_both?7:4;
    for(int i=0;i<header->item_count;i++) {
        strcpy(header->item[i].name,names[i]);
        header->item[i].offset=i?offsets[(i-1)%3]+(i>3?0x100000:0):0;
        header->item[i].size=4096;
    }
    return 0;
}
int flash_bootloader(char *source, void *command) { abort(); }
int flash_parameter(char *source, void *command) { abort(); }
int flash_register_partition_data(PSTRUCT_PARAM_ITEM p,long long *backup) { return 0; }
int flash_normal(char *source, void *command)
{
    UPDATE_CMD *c=command;char expected[96];
    const char *names[]={"uboot","boot","system"};
    assert(writes_seen<3);
    snprintf(expected,sizeof(expected),"/dev/block/by-name/%s_%c",names[writes_seen],'a'+target_slot);
    assert(!strcmp(c->dest_path,expected));
    assert(c->size==4096 && c->flash_offset==0);
    assert(c->offset==offsets[writes_seen]+(source_both&&target_slot?0x100000:0));
    assert(!c->skip_verify);
    writes_seen++;return 0;
}
bool comparefile(const char *dest, const char *source, long long dest_offset, long long source_offset, long long size)
{
    assert(strstr(dest,target_slot?"_b":"_a") && size==4096);
    comparisons++;return true;
}
void sync(void) {}
int __wrap_unlink(const char *path)
{
    assert(!strcmp(path,"/fixture/update.img.done"));return 0;
}
static void callback(void *data,RK_Upgrade_Status_t status) { final_status=status; }
static void print_callback(char *p) {}
int main(void)
{
    UPDATE_CMD original[16];memcpy(original,update_cmd,sizeof(original));
    for(source_both=0;source_both<2;source_both++)for(target_slot=0;target_slot<2;target_slot++) {
        memcpy(update_cmd,original,sizeof(original));
        writes_seen=comparisons=0;final_status=-1;
        RK_ota_set_url("/fixture/update.img","/fixture/update.img");
        /* Same masks as native main.c: running A selects B, running B selects A. */
        int mask=0xFC00 & (target_slot ? 0x155ff : 0x1a9ff);
        assert(RK_ota_set_partition(mask));
        RK_ota_start(callback,print_callback);
        assert(final_status==RK_UPGRADE_FINISHED && writes_seen==3 && comparisons==3);
    }
    puts("native update dispatch passed (both targets, same/cross-slot source, GPT excluded)");
    return 0;
}
