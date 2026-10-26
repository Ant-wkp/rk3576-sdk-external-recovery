/* SPDX-License-Identifier: Apache-2.0 */
/* Real SDK functions, all device access replaced at the test link layer. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "update_engine/rkbootloader.c"

static FILE *disk, *command;
static int sync_error, short_write, corrupt_read, writes;
static unsigned char original[4096];
static const char *translate(const char *path, char out[64])
{
    if (!strcmp(path, MISC_PARTITION_NAME_BLOCK))
        snprintf(out, 64, "/proc/self/fd/%d", fileno(disk));
    else if (!strcmp(path, "/proc/cmdline"))
        snprintf(out, 64, "/proc/self/fd/%d", fileno(command));
    else { assert(strncmp(path, "/dev/", 5)); return path; }
    return out;
}
int __real_open(const char *, int, ...);
FILE *__real_fopen(const char *, const char *);
int __real_stat(const char *, struct stat *);
ssize_t __real_pwrite(int, const void *, size_t, off_t);
ssize_t __real_pread(int, void *, size_t, off_t);
int __real_fsync(int);
int __wrap_open(const char *path, int flags, ...)
{
    char out[64]; assert(!(flags & O_CREAT));
    return __real_open(translate(path, out), flags);
}
FILE *__wrap_fopen(const char *path, const char *mode)
{
    char out[64]; return __real_fopen(translate(path, out), mode);
}
int __wrap_stat(const char *path, struct stat *st)
{
    char out[64]; return __real_stat(translate(path, out), st);
}
ssize_t __wrap_pwrite(int fd, const void *p, size_t n, off_t off)
{
    writes++;
    return __real_pwrite(fd, p, short_write ? n - 1 : n, off);
}
ssize_t __wrap_pread(int fd, void *p, size_t n, off_t off)
{
    ssize_t rc = __real_pread(fd, p, n, off);
    if (corrupt_read && writes && rc > 0) ((unsigned char *)p)[0] ^= 1;
    return rc;
}
int __wrap_fsync(int fd)
{
    if (sync_error) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
void Log(const char *f, int line, LogPriority p, const char *fmt, ...) {}
bool isMtdDevice(void) { return false; }
/* An accidental MTD branch must fail loudly, never touch real devices. */
int mtd_scan_partitions(void) { abort(); }
const MtdPartition *mtd_find_partition_by_name(const char *n) { abort(); }
int mtd_partition_info(const MtdPartition *p, size_t *a, size_t *b, size_t *c) { abort(); }
MtdReadContext *mtd_read_partition(const MtdPartition *p) { abort(); }
ssize_t mtd_read_data(MtdReadContext *p, char *b, size_t n) { abort(); }
void mtd_read_close(MtdReadContext *p) { abort(); }
MtdWriteContext *mtd_write_partition(const MtdPartition *p) { abort(); }
ssize_t mtd_write_data(MtdWriteContext *p, const char *b, size_t n) { abort(); }
int mtd_write_close(MtdWriteContext *p) { abort(); }

static void fixture(int current)
{
    struct AvbABData a = { .magic = AVB_AB_MAGIC, .version_major = 1 };
    a.slots[current] = (AvbABSlotData){15, 0, 0, {0}}; /* last trial 1 -> 0 */
    a.slots[1-current] = (AvbABSlotData){14, 0, 1, {0}};
    a.last_boot = 1-current;
    AvbABData_update_crc(&a);
    memset(original, 0xa5, sizeof(original));
    memcpy(original + MISC_OFFSET, &a, sizeof(a));
    assert(__real_pwrite(fileno(disk), original, sizeof(original), 0) == sizeof(original));
    assert(ftruncate(fileno(command), 0) == 0);
    char cmd[64]; int n = snprintf(cmd, sizeof(cmd), "root=x androidboot.slot_suffix=_%c\n", 'a'+current);
    assert(__real_pwrite(fileno(command), cmd, n, 0) == n);
    writes = sync_error = short_write = corrupt_read = 0;
}
int main(void)
{
    disk = tmpfile(); command = tmpfile(); assert(disk && command);
    assert(rk_parse_slot("android_slotsufix=_a androidboot.slot_suffix=_a") == 0);
    assert(rk_parse_slot("android_slotsufix=_a androidboot.slot_suffix=_b") == -1);
    assert(rk_parse_slot("xandroid_slotsufix=_a") == -1);
    assert(rk_parse_slot("androidboot.slot_suffix=_bad") == -1);
    assert(rk_parse_slot("androidboot.slot_suffix=") == -1);
    for (int slot = 0; slot < 2; slot++) {
        struct AvbABData a; unsigned char after[4096];
        fixture(slot); assert(getCurrentSlot() == slot);
        assert(setSlotSucceed() == 0); assert(rk_ab_read_verified(&a) == 0);
        assert(a.slots[slot].successful_boot && !a.slots[slot].tries_remaining);
        assert(a.last_boot == slot && a.slots[1-slot].successful_boot);
        int count = writes; assert(setSlotSucceed() == 0 && writes == count);
        assert(__real_pread(fileno(disk), after, sizeof(after), 0) == sizeof(after));
        assert(!memcmp(original, after, MISC_OFFSET));
        assert(!memcmp(original+MISC_OFFSET+32, after+MISC_OFFSET+32, sizeof(after)-MISC_OFFSET-32));
        fixture(slot); assert(rk_ab_fail_current(slot) == 0);
        assert(rk_ab_read_verified(&a) == 0);
        assert(!a.slots[slot].priority && !a.slots[slot].successful_boot);
        assert(a.slots[1-slot].priority == 14 && a.slots[1-slot].successful_boot);
        assert(a.last_boot == 1-slot);
        fixture(slot); assert(rk_ab_fail_current(1-slot) != 0 && !writes);
        fixture(slot); sync_error=1; assert(setSlotSucceed() != 0);
        fixture(slot); short_write=1; assert(setSlotSucceed() != 0);
        fixture(slot); corrupt_read=1; assert(setSlotSucceed() != 0);
        fixture(slot); original[MISC_OFFSET+28] ^= 1;
        assert(__real_pwrite(fileno(disk), original, sizeof(original), 0) == sizeof(original));
        assert(setSlotSucceed() != 0 && !writes);
        fixture(slot); original[MISC_OFFSET] ^= 1;
        assert(__real_pwrite(fileno(disk), original, sizeof(original), 0) == sizeof(original));
        assert(setSlotSucceed() != 0 && !writes);
        fixture(slot); memcpy(&a, original+MISC_OFFSET, 32);
        a.slots[1-slot].successful_boot=0; AvbABData_update_crc(&a);
        assert(__real_pwrite(fileno(disk), &a, 32, MISC_OFFSET) == 32);
        assert(rk_ab_fail_current(slot) != 0 && !writes);
    }
    fclose(disk); fclose(command);
    puts("metadata/slot tests passed (both directions, errors, sector preservation)");
    return 0;
}
