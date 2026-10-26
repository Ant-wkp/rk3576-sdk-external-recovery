/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the real state machine; every OS/device operation is link-wrapped. */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include "update_engine/boot_check.c"

static char sandbox[PATH_MAX];
static int slot, wfd, confirmations, rollbacks, reboots, feeds, stops;
static int bad_identity, bad_metadata, no_fallback, confirmed, stop_error, confirm_error, feed_error;
static int foreign_misc;
static int64_t tick, reboot_tick;
static jmp_buf reset_jump;
static struct AvbABData metadata;

int __real_open(const char *, int, ...);
FILE *__real_fopen(const char *, const char *);
int __real_stat(const char *, struct stat *);
int __real_lstat(const char *, struct stat *);
int __real_fstat(int, struct stat *);
int __real_unlink(const char *);
static const char *map(const char *path, char out[PATH_MAX])
{
    if (!strncmp(path, sandbox, strlen(sandbox))) return path;
    assert(path[0] == '/');
    assert(snprintf(out, PATH_MAX, "%s%s", sandbox, path) < PATH_MAX);
    return out;
}
static void put(const char *path, const char *value)
{
    char buf[PATH_MAX]; int fd = __real_open(map(path, buf), O_WRONLY|O_CREAT|O_TRUNC, 0600);
    assert(fd >= 0); assert(write(fd, value, strlen(value)) == strlen(value)); assert(!close(fd));
}
int __wrap_open(const char *path, int flags, ...)
{
    char buf[PATH_MAX]; mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    int fd = __real_open(map(path, buf), flags, mode);
    if (!strcmp(path, "/dev/watchdog0")) wfd = fd;
    return fd;
}
FILE *__wrap_fopen(const char *path, const char *mode)
{
    char buf[PATH_MAX]; return __real_fopen(map(path, buf), mode);
}
int __wrap_stat(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    if (!strncmp(path, "/dev/block/by-name/", 19)) {
        const char *name = path + 19;
        int part = !strcmp(name,"system_a") ? 7 : !strcmp(name,"system_b") ? 8 :
                   !strcmp(name,"userdata") ? 9 : !strcmp(name,"misc") ? 4 : -1;
        assert(part > 0); st->st_mode = S_IFBLK|0600; st->st_rdev=makedev(179,part); return 0;
    }
    char buf[PATH_MAX]; return __real_stat(map(path, buf), st);
}
int __wrap_lstat(const char *path, struct stat *st)
{
    char buf[PATH_MAX]; int rc=__real_lstat(map(path,buf),st); st->st_uid=0; return rc;
}
int __wrap_fstat(int fd, struct stat *st)
{
    int rc=__real_fstat(fd,st); st->st_uid=0; return rc;
}
char *__wrap_realpath(const char *path, char *out)
{
    unsigned ma,mi; assert(sscanf(path,"/sys/dev/block/%u:%u",&ma,&mi)==2 && ma==179);
    if (foreign_misc && mi==4)
        snprintf(out,PATH_MAX,"/sys/devices/mmc_host/mmc1/mmc1:0001/block/mmcblk1/mmcblk1p4");
    else
        snprintf(out,PATH_MAX,"/sys/devices/mmc_host/mmc0/mmc0:0001/block/mmcblk0/mmcblk0p%u",mi);
    return out;
}
int __wrap_unlink(const char *path)
{
    char buf[PATH_MAX]; return __real_unlink(map(path,buf));
}
int __wrap_mkostemp(char *path, int flags)
{
    strcpy(path,"/userdata/probe"); char buf[PATH_MAX];
    return __real_open(map(path,buf),O_RDWR|O_CREAT|O_EXCL|flags,0600);
}
int __wrap_clock_gettime(clockid_t id, struct timespec *t)
{
    assert(id==CLOCK_MONOTONIC); t->tv_sec=tick; t->tv_nsec=0; return 0;
}
unsigned __wrap_sleep(unsigned n) { tick+=n; return 0; }
int __wrap_uname(struct utsname *u) { memset(u,0,sizeof(*u));strcpy(u->release,"6.1.75-abtest-v1");return 0; }
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    assert(fd==wfd && fd>=0);
    va_list ap;va_start(ap,request);void *arg=va_arg(ap,void *);va_end(ap);
    switch(request) {
    case WDIOC_GETSUPPORT: memset(arg,0,sizeof(struct watchdog_info));return 0;
    case WDIOC_GETTIMEOUT: *(int *)arg=89;return 0;
    case WDIOC_KEEPALIVE: feeds++;return feed_error?-1:0;
    case WDIOC_SETOPTIONS:
        assert(*(int *)arg==WDIOS_DISABLECARD && confirmations && !rollbacks);
        stops++;if(stop_error)return -1;
        put("/sys/class/watchdog/watchdog0/state","inactive\n");return 0;
    default: assert(0);return -1;
    }
}
pid_t __wrap_fork(void) { reboots++;reboot_tick=tick;return 12345; }
pid_t __wrap_waitpid(pid_t pid,int *status,int flags) { assert(pid==12345&&flags==WNOHANG);*status=0;return pid; }
int __wrap_execl(const char *p,const char *arg,...) { assert(0);return -1; }
int __wrap_pause(void) { longjmp(reset_jump,1); }
int getCurrentSlot(void) { return slot; }
bool isMtdDevice(void) { return false; }
int rk_ab_read_verified(struct AvbABData *a) { *a=metadata;return bad_metadata?-1:0; }
int setSlotSucceed(void)
{
    confirmations++;if(confirm_error)return -1;
    metadata.slots[slot].successful_boot=1;metadata.slots[slot].tries_remaining=0;return 0;
}
int rk_ab_fail_current(int expected)
{
    assert(expected==slot && !metadata.slots[slot].successful_boot);
    if(no_fallback)return -1;
    rollbacks++;metadata.slots[slot].priority=0;return 0;
}
static void setup(void)
{
    if(wfd>=0)close(wfd);
    slot=0;wfd=-1;confirmations=rollbacks=reboots=feeds=stops=0;tick=reboot_tick=0;
    bad_identity=bad_metadata=no_fallback=confirmed=stop_error=confirm_error=feed_error=0;
    foreign_misc=0;
    memset(&metadata,0,sizeof(metadata));metadata.slots[0].priority=15;
    metadata.slots[1].priority=14;metadata.slots[1].successful_boot=1;
    put("/proc/sys/kernel/random/boot_id","11111111-2222-3333-4444-555555555555\n");
    put(STATE_FILE,"not-complete\n");put("/dev/watchdog0","");
    put("/sys/class/watchdog/watchdog0/state","active\n");
    put(RELEASE_FILE,"release=v1\nkernel=6.1.75-abtest-v1\nuboot=U-Boot test-abtest-v1\ndtb=v1\nlayout=rk3576-ab-v2\nspl_abi=abctx2\n");
    put("/etc/ab-test-version","v1\n");
    put("/sys/firmware/devicetree/base/chosen/100ask,uboot-ab-slot","_a");
    put("/sys/firmware/devicetree/base/chosen/100ask,uboot-version","U-Boot test-abtest-v1");
    put("/sys/firmware/devicetree/base/chosen/100ask,ab-test-version","v1");
    put("/sys/firmware/devicetree/base/chosen/100ask,ab-layout","rk3576-ab-v2");
    put("/sys/firmware/devicetree/base/chosen/100ask,spl-ab-abi","abctx2");
    put("/proc/self/mountinfo","1 0 179:7 / / rw - ext4 /dev/root rw\n2 1 179:9 / /userdata rw - ext4 /dev/mmcblk0p9 rw\n");
}
int main(int argc,char **argv)
{
    assert(argc==2);strcpy(sandbox,argv[1]);wfd=-1;
    setup();
    { struct release r; dev_t dev;char parent[PATH_MAX],v[32];
      assert(!chosen("uboot-ab-slot",v,sizeof(v)) && !strcmp(v,"_a"));
      assert(!partition("system_a",&dev,parent));
      assert(!mounted_device("/",dev,"ext4",1));
      assert(rk_boot_identity_slot()==0);assert(!load_release(&r));assert(!identity(0,&r)); }
    assert(rk_boot_check(-1)==0);assert(confirmations==1&&stops==1&&!rollbacks&&!reboots&&tick==30);
    int oldfeeds=feeds;assert(rk_boot_check(-1)==0 && feeds==oldfeeds && stops==1);
    setup();put("/sys/firmware/devicetree/base/chosen/100ask,ab-test-version","v2");
    if(!setjmp(reset_jump)) { rk_boot_check(-1);assert(0); }
    assert(rollbacks==1&&reboots==1&&reboot_tick==0&&!confirmations&&!stops&&tick==15);
    setup();metadata.slots[0].successful_boot=1;
    put("/sys/firmware/devicetree/base/chosen/100ask,ab-test-version","v2");
    if(!setjmp(reset_jump)) { rk_boot_check(-1);assert(0); }
    assert(!rollbacks&&reboots==1&&!confirmations&&!stops);
    setup();bad_metadata=1;
    if(!setjmp(reset_jump)) { rk_boot_check(-1);assert(0); }
    assert(!rollbacks&&!reboots&&!confirmations&&!stops);
    setup();no_fallback=1;put("/sys/firmware/devicetree/base/chosen/100ask,ab-test-version","v2");
    if(!setjmp(reset_jump)) { rk_boot_check(-1);assert(0); }
    assert(!rollbacks&&!reboots&&!stops);
    setup();confirm_error=1;
    if(!setjmp(reset_jump)) { rk_boot_check(-1);assert(0); }
    assert(confirmations==1&&!rollbacks&&!reboots&&!stops);
    setup();stop_error=1;
    if(!setjmp(reset_jump)) { rk_boot_check(-1);assert(0); }
    assert(confirmations==1&&stops==1&&!rollbacks&&reboots==1&&reboot_tick==30&&tick==45);
    setup();put("/proc/self/mountinfo","1 0 179:7 / / rw - ext4 /dev/root rw\n");
    if(!setjmp(reset_jump)) { rk_boot_check(-1);assert(0); }
    assert(rollbacks==1&&reboot_tick==120&&!stops&&!confirmations);
    setup();put("/proc/self/mountinfo","1 0 179:7 / / ro - ext4 /dev/root ro\n2 1 179:9 / /userdata rw - ext4 /dev/mmcblk0p9 rw\n");
    assert(rk_boot_identity_slot()==-1 && root_slot(0)==0);
    if(!setjmp(reset_jump)) { rk_boot_check(-1);assert(0); }
    assert(rollbacks==1&&reboots==1&&reboot_tick==0&&!confirmations&&!stops);
    setup();put("/proc/self/mountinfo","1 0 179:8 / / rw - ext4 /dev/root rw\n2 1 179:9 / /userdata rw - ext4 /dev/mmcblk0p9 rw\n");
    if(!setjmp(reset_jump)) { rk_boot_check(-1);assert(0); }
    assert(!rollbacks&&!reboots&&!confirmations&&!stops);
    setup();foreign_misc=1;
    if(!setjmp(reset_jump)) { rk_boot_check(-1);assert(0); }
    assert(!rollbacks&&!reboots&&!confirmations&&!stops);
    setup();slot=-1;
    if(!setjmp(reset_jump)) { rk_boot_check(-1);assert(0); }
    assert(!rollbacks&&!reboots&&!confirmations&&!stops);
    if(wfd>=0)close(wfd);
    puts("boot-check state tests passed (success, immediate rollback, WDT fallback, timeout)");
    return 0;
}
