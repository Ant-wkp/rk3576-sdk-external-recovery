/* SPDX-License-Identifier: Apache-2.0 */
/* Boot-only protection. No background feeder and no production fault hooks. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "boot_check.h"
#include "../bootloader.h"
#include "rktools.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/watchdog.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK_LIMIT 120
#define CHECK_INTERVAL 5
#define STABLE_TIME 30
#define REBOOT_GRACE 15
#define STATE_FILE "/run/updateEngine-boot-check.state"
#define RELEASE_FILE "/etc/rk-ota-release"

struct release {
    char id[64], kernel[256], uboot[256], dtb[64], layout[64], abi[64];
};

static int text_file(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t used = 0;
    while (used < cap - 1) {
        ssize_t n = read(fd, out + used, cap - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return -1; }
        if (!n) break;
        used += n;
    }
    int rc = close(fd);
    if (rc || !used || used == cap - 1) return -1;
    while (used && (out[used-1] == '\n' || out[used-1] == '\r' || !out[used-1])) used--;
    if (!used || memchr(out, 0, used)) return -1;
    out[used] = 0;
    return 0;
}

static int chosen(const char *key, char *out, size_t cap)
{
    char path[256];
    int n = snprintf(path, sizeof(path), "/sys/firmware/devicetree/base/chosen/100ask,%s", key);
    return n < 0 || (size_t)n >= sizeof(path) ? -1 : text_file(path, out, cap);
}

static int load_release(struct release *r)
{
    char data[2048];
    struct stat st;
    if (lstat(RELEASE_FILE, &st) || !S_ISREG(st.st_mode) || st.st_uid ||
        (st.st_mode & 022) || text_file(RELEASE_FILE, data, sizeof(data))) return -1;
    char *keys[] = { "release", "kernel", "uboot", "dtb", "layout", "spl_abi" };
    char *values[] = { r->id, r->kernel, r->uboot, r->dtb, r->layout, r->abi };
    size_t caps[] = { sizeof(r->id), sizeof(r->kernel), sizeof(r->uboot),
                      sizeof(r->dtb), sizeof(r->layout), sizeof(r->abi) };
    unsigned seen = 0;
    char *save, *line = strtok_r(data, "\n", &save);
    while (line) {
        char *equal = strchr(line, '=');
        if (!equal || equal == line || !equal[1]) return -1;
        *equal++ = 0;
        unsigned i;
        for (i = 0; i < 6 && strcmp(line, keys[i]); i++);
        if (i == 6 || (seen & (1U << i)) || strlen(equal) >= caps[i]) return -1;
        strcpy(values[i], equal); seen |= 1U << i;
        line = strtok_r(NULL, "\n", &save);
    }
    return seen == 63 ? 0 : -1;
}

/* Match mountinfo major:minor, never infer the root from /dev/root text. */
static int mounted_device(const char *point, dev_t wanted, const char *fstype, int require_rw)
{
    FILE *f = fopen("/proc/self/mountinfo", "re");
    if (!f) return -1;
    char line[8192], root[4096], mount[4096], options[1024], type[64];
    unsigned major, minor; int result = -1;
    while (fgets(line, sizeof(line), f)) {
        if (!strchr(line, '\n')) break;
        char *sep = strstr(line, " - ");
        if (!sep || sscanf(line, "%*u %*u %u:%u %4095s %4095s %1023s",
                           &major, &minor, root, mount, options) != 5 ||
            sscanf(sep + 3, "%63s", type) != 1) break;
        if (strcmp(point, mount)) continue;
        char *save; int rw = 0;
        for (char *o = strtok_r(options, ",", &save); o; o = strtok_r(NULL, ",", &save))
            if (!strcmp(o, "rw")) rw = 1;
        result = wanted == makedev(major, minor) && (!require_rw || rw) &&
                 (!fstype || !strcmp(fstype, type)) ? 0 : -1;
        break;
    }
    if (ferror(f)) result = -1;
    fclose(f); return result;
}

static int partition(const char *name, dev_t *dev, char parent[PATH_MAX])
{
    char path[128], sys[128], real[PATH_MAX]; struct stat st;
    int n = snprintf(path, sizeof(path), "/dev/block/by-name/%s", name);
    if (n < 0 || (size_t)n >= sizeof(path) ||
        stat(path, &st) || !S_ISBLK(st.st_mode)) return -1;
    *dev = st.st_rdev;
    snprintf(sys, sizeof(sys), "/sys/dev/block/%u:%u", major(*dev), minor(*dev));
    if (!realpath(sys, real)) return -1;
    char *slash = strrchr(real, '/');
    if (!slash) return -1;
    *slash = 0; strcpy(parent, real);
    /* Restrict automatic management to an eMMC partition, not MTD or USB. */
    slash = strrchr(parent, '/');
    unsigned disk; char extra;
    return !slash || sscanf(slash + 1, "mmcblk%u%c", &disk, &extra) != 1 ? -1 : 0;
}

/* Rollback must still identify a known root that has remounted read-only.
 * Do not confuse health (rw ext4) with trustworthy block-device identity. */
static int root_slot(int require_healthy)
{
    int slot = getCurrentSlot();
    char suffix[16], name[32], parent[PATH_MAX], misc_parent[PATH_MAX]; dev_t dev, misc;
    if (slot < 0 || slot > 1 || chosen("uboot-ab-slot", suffix, sizeof(suffix)) ||
        strcmp(suffix, slot ? "_b" : "_a")) return -1;
    snprintf(name, sizeof(name), "system_%c", 'a' + slot);
    return partition(name, &dev, parent) || partition("misc", &misc, misc_parent) ||
           dev == misc || strcmp(parent, misc_parent) ||
           mounted_device("/", dev, require_healthy ? "ext4" : NULL, require_healthy) ? -1 : slot;
}

int rk_boot_identity_slot(void)
{
    return root_slot(1);
}

static int identity(int slot, const struct release *r)
{
    struct utsname u; char value[256];
    if (rk_boot_identity_slot() != slot || uname(&u) || strcmp(u.release, r->kernel) ||
        chosen("uboot-version", value, sizeof(value)) || strcmp(value, r->uboot) ||
        chosen("ab-test-version", value, sizeof(value)) || strcmp(value, r->dtb) ||
        chosen("ab-layout", value, sizeof(value)) || strcmp(value, r->layout) ||
        chosen("spl-ab-abi", value, sizeof(value)) || strcmp(value, r->abi) ||
        text_file("/etc/ab-test-version", value, sizeof(value)) || strcmp(value, r->id))
        return -1;
    return 0;
}

static int storage_ready(int slot)
{
    char name[32], root_parent[PATH_MAX], parent[PATH_MAX]; dev_t root, data, misc;
    snprintf(name, sizeof(name), "system_%c", 'a' + slot);
    if (partition(name, &root, root_parent) || partition("userdata", &data, parent) ||
        strcmp(root_parent, parent) || data == root || mounted_device("/userdata", data, NULL, 1) ||
        partition("misc", &misc, parent) || strcmp(root_parent, parent) || misc == data || misc == root)
        return -1;
    char path[] = "/userdata/.updateEngine-check-XXXXXX";
    int fd = mkostemp(path, O_CLOEXEC);
    if (fd < 0) return -1;
    const char sample[] = "updateEngine boot storage check\n"; char verify[sizeof(sample)];
    int rc = write(fd, sample, sizeof(sample)) != sizeof(sample) || fsync(fd) ||
             pread(fd, verify, sizeof(verify), 0) != sizeof(verify) ||
             memcmp(sample, verify, sizeof(sample));
    if (close(fd)) rc = 1;
    if (unlink(path)) rc = 1;
    return rc ? -1 : 0;
}

static int64_t now_seconds(void)
{
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : t.tv_sec;
}

static int write_state(const char *boot, const char *state, int slot)
{
    char data[512]; int n = snprintf(data, sizeof(data), "%s\n%s\nslot=%d\n", boot, state, slot);
    int fd = open(STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int rc = n <= 0 || (size_t)n >= sizeof(data) || write(fd, data, n) != n;
    if (close(fd)) rc = 1;
    return rc ? -1 : 0;
}

static void record_result(const char *boot, const char *reason, int slot, int data_ready)
{
    fprintf(stderr, "AB_CHECK slot=%d result=%s\n", slot, reason); fflush(stderr);
    write_state(boot, reason, slot);
    /* Only persist after proving userdata really is mounted. Bounded last-result. */
    if (!data_ready) return;
    int dir = open("/userdata", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) return;
    int fd = openat(dir, ".updateEngine-boot-result", O_WRONLY | O_CREAT | O_TRUNC |
                    O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        char line[512]; int n = snprintf(line, sizeof(line), "boot=%s\nslot=%d\nresult=%s\n", boot, slot, reason);
        if (n > 0 && (size_t)n < sizeof(line) && write(fd, line, n) == n) fsync(fd);
        close(fd); fsync(dir);
    }
    close(dir);
}

int rk_ab_operation_lock(void)
{
    int fd = open("/run/updateEngine-ab.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode & 022) ||
        flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -1; }
    return fd;
}

static void await_reset(void)
{
    fprintf(stderr, "AB_CHECK waiting for hardware reset; no further keepalive\n");
    fflush(stderr);
    /* Never disable, magic-close, or resume feeding on an error path. */
    for (;;) pause();
}

static void reboot_now(int watchdog)
{
    fprintf(stderr, "AB_CHECK requesting normal /sbin/reboot now\n"); fflush(stderr);
    pid_t pid = fork();
    if (!pid) { execl("/sbin/reboot", "reboot", (char *)NULL); _exit(127); }
    if (pid < 0) await_reset();
    int64_t until = now_seconds() + REBOOT_GRACE;
    for (;;) {
        int status; pid_t done = pid > 0 ? waitpid(pid, &status, WNOHANG) : 0;
        if (done == pid && (!WIFEXITED(status) || WEXITSTATUS(status))) break;
        if (done == pid) pid = -1;
        int64_t now = now_seconds();
        if (now < 0 || now >= until) break;
        if (ioctl(watchdog, WDIOC_KEEPALIVE, NULL)) break;
        sleep(1);
    }
    await_reset();
}

int rk_boot_check(int operation_lock)
{
#if !defined(SUCCESSFUL_BOOT) || defined(RETRY_BOOT)
    fprintf(stderr, "AB_CHECK requires SUCCESSFUL_BOOT without RETRY_BOOT\n");
    return 2;
#endif
    struct release r; struct AvbABData a;
    char boot[64], previous[512], expected[96]; int watchdog = -1, data_ready = 0;
    int slot = getCurrentSlot();
    if (text_file("/proc/sys/kernel/random/boot_id", boot, sizeof(boot))) return 2;
    snprintf(expected, sizeof(expected), "%s\ncomplete\n", boot);
    if (!text_file(STATE_FILE, previous, sizeof(previous)) &&
        !strncmp(previous, expected, strlen(expected))) return 0;
    int64_t start = now_seconds(), stable = -1;
    if (start < 0 || write_state(boot, "checking", slot)) return 2;
    watchdog = open("/dev/watchdog0", O_WRONLY | O_CLOEXEC);
    if (watchdog < 0) {
        record_result(boot, "watchdog-open-failed", slot, 0);
        return 2; /* Existing finite kernel takeover remains the fallback. */
    }
    struct watchdog_info support; int timeout;
    if (ioctl(watchdog, WDIOC_GETSUPPORT, &support) ||
        ioctl(watchdog, WDIOC_GETTIMEOUT, &timeout) || timeout < 10 ||
        ioctl(watchdog, WDIOC_KEEPALIVE, NULL)) {
        record_result(boot, "watchdog-adoption-failed", slot, 0); await_reset();
    }
    fprintf(stderr, "AB_CHECK started slot=%d watchdog_timeout=%d limit=%d stable=%d\n",
            slot, timeout, CHECK_LIMIT, STABLE_TIME); fflush(stderr);
    const char *reason = "check-timeout";
    if (slot < 0 || slot > 1 || isMtdDevice() || rk_ab_read_verified(&a) || load_release(&r)) {
        reason = "invalid-slot-metadata-or-release"; goto failed;
    }
    for (;;) {
        if (rk_ab_read_verified(&a) || !a.slots[slot].priority) { reason = "invalid-metadata"; goto failed; }
        /* Missing mounts may settle. Conflicting identities cannot become healthy. */
        if (identity(slot, &r)) { reason = "boot-identity-mismatch"; goto failed; }
        data_ready = storage_ready(slot) == 0;
        int64_t now = now_seconds();
        if (now < 0 || now - start >= CHECK_LIMIT) goto failed;
        if (data_ready) {
            if (stable < 0) stable = now;
            if (now - stable >= STABLE_TIME) break;
        } else stable = -1;
        if (ioctl(watchdog, WDIOC_KEEPALIVE, NULL)) { reason = "keepalive-failed"; goto failed; }
        sleep(CHECK_INTERVAL);
    }
    if (rk_boot_identity_slot() != slot || setSlotSucceed() || rk_ab_read_verified(&a) ||
        !a.slots[slot].successful_boot || a.slots[slot].tries_remaining) {
        record_result(boot, "confirmation-uncertain", slot, data_ready); await_reset();
    }
    int options = WDIOS_DISABLECARD;
    char state[32];
    if (ioctl(watchdog, WDIOC_SETOPTIONS, &options) ||
        text_file("/sys/class/watchdog/watchdog0/state", state, sizeof(state)) || strcmp(state, "inactive")) {
        record_result(boot, "confirmed-but-watchdog-stop-failed", slot, data_ready);
        /* A failed reset deassert may have left hardware OFF. Request reboot
         * too: blindly waiting for a watchdog that may be stopped can hang. */
        if (operation_lock >= 0) close(operation_lock);
        reboot_now(watchdog);
    }
    if (close(watchdog)) {
        record_result(boot, "confirmed-watchdog-stopped-close-error", slot, data_ready); return 2;
    }
    record_result(boot, "complete", slot, data_ready);
    return 0;
failed:
    record_result(boot, reason, slot, data_ready);
    /* Never select a fallback from corrupt metadata or an unknown actual root. */
    if (slot < 0 || slot > 1 || root_slot(0) != slot || rk_ab_read_verified(&a)) await_reset();
    if (!a.slots[slot].successful_boot) {
        if (rk_ab_fail_current(slot)) {
            record_result(boot, "rollback-not-persisted-or-no-confirmed-fallback", slot, data_ready);
            await_reset();
        }
        record_result(boot, "rollback-persisted", slot, data_ready);
    }
    /* No metadata operations after releasing the lock for normal shutdown. */
    if (operation_lock >= 0) close(operation_lock);
    reboot_now(watchdog);
    return 2;
}
