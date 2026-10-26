/* SPDX-License-Identifier: Apache-2.0 */
/* Link substitutes only: the actual main.c is tested without device access. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "bootloader.h"
#include "update_engine/update.h"
bool is_sdboot, is_usbboot;
static int action(const char *name)
{
    const char *a=getenv("TEST_ACTION");return a&&!strcmp(a,name);
}
void Log(const char *f,int l,LogPriority p,const char *fmt,...) {}
int getCurrentSlot(void) { return 0; }
bool isMtdDevice(void) { return false; }
int rk_boot_identity_slot(void) { return getenv("TEST_BAD_ROOT") ? -1 : 0; }
int rk_ab_operation_lock(void) { assert(action("boot")||action("update")||action("confirm"));return dup(0); }
int rk_boot_check(int fd) { assert(action("boot"));puts("BOOT_CHECK");return 0; }
int rk_ab_read_verified(struct AvbABData *a)
{
    memset(a,0,sizeof(*a));a->slots[0].priority=15;
    a->slots[0].successful_boot=getenv("TEST_TRIAL")?0:1;return 0;
}
int setSlotSucceed(void) { assert(action("confirm"));puts("CONFIRM");return getenv("TEST_CONFIRM_FAIL")?-1:0; }
int setSlotActivity(void) { assert(action("update"));puts("ACTIVATE");return getenv("TEST_ACTIVATE_FAIL")?-1:0; }
void RK_ota_set_url(char *u,char *p) { assert(action("update"));puts("DOWNLOAD"); }
bool RK_ota_set_partition(int p) { assert(action("update"));return true; }
void RK_ota_start(RK_upgrade_callback cb,RK_print_callback print)
{
    assert(action("update"));cb(NULL,RK_UPGRADE_FINISHED);
}
bool RK_ota_check_version(char *u) { assert(0);return false; }
void miscDisplay(void) { assert(0); }
int wipe_userdata(int r) { assert(0);return -1; }
int set_bootloader_message(const struct bootloader_message *m) { assert(0);return -1; }
int readCustomMiscCmdline(void) { assert(0);return -1; }
int writeCustomMiscCmdline(void) { assert(0);return -1; }
int cleanCustomMiscCmdline(void) { assert(0);return -1; }
int system(const char *cmd) { assert(0);return -1; }
void sync(void) { assert(0); }
