/* Test-only LD_PRELOAD fixture. Never linked/installed into production firmware. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int mode_is(const char *value)
{
    const char *mode = getenv("UE_BOARD_TEST_MODE");
    return mode && !strcmp(mode, value);
}

static void event(const char *value)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(stderr, "BOARD_FIXTURE %ld.%03ld %s\n", now.tv_sec, now.tv_nsec / 1000000, value);
    fflush(stderr);
}

static void snapshot(const char *phase)
{
    const char *tag = getenv("UE_BOARD_TEST_TAG");
    if (!tag || !*tag || strspn(tag, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(tag)) return;
    char path[256]; unsigned char data[512];
    int in = open("/dev/block/by-name/misc", O_RDONLY | O_CLOEXEC);
    if (in < 0) return;
    ssize_t got = pread(in, data, sizeof(data), 2048);
    close(in);
    if (got != sizeof(data)) return;
    snprintf(path, sizeof(path), "/userdata/native-boot-check-fixtures/%s-%s.bin", tag, phase);
    int out = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out >= 0) {
        if (write(out, data, sizeof(data)) == sizeof(data)) fsync(out);
        close(out);
    }
}

int uname(struct utsname *value)
{
    int (*real_uname)(struct utsname *) = dlsym(RTLD_NEXT, "uname");
    int rc = real_uname(value);
    if (!rc && (mode_is("identity") || mode_is("reboot-fails"))) {
        strcpy(value->release, "board-test-intentional-identity-mismatch");
        event("identity mismatch injected only in checker");
    }
    return rc;
}

int ioctl(int fd, unsigned long request, ...)
{
    int (*real_ioctl)(int, unsigned long, ...) = dlsym(RTLD_NEXT, "ioctl");
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    int rc = real_ioctl(fd, request, arg);
    static int first = 1;
    if (!rc && request == WDIOC_KEEPALIVE && first) {
        first = 0;
        snapshot("adopt");
    }
    if (!rc && request == WDIOC_KEEPALIVE && mode_is("hang")) {
        event("adopted real WDT; SIGSTOP checker, no more keepalive");
        raise(SIGSTOP);
    }
    return rc;
}

int execl(const char *path, const char *arg, ...)
{
    snapshot("before-reboot");
    if (!strcmp(path, "/sbin/reboot") && mode_is("reboot-fails")) {
        event("normal reboot request deliberately failed, WDT must reset");
        errno = EACCES;
        return -1;
    }
    /* This fixture's only exec caller is the checker's no-argument reboot. */
    if (strcmp(path, "/sbin/reboot") || strcmp(arg, "reboot")) {
        errno = EINVAL;
        return -1;
    }
    char *argv[] = { (char *)arg, NULL };
    extern char **environ;
    return execve(path, argv, environ);
}

size_t fread(void *ptr, size_t size, size_t count, FILE *stream)
{
    size_t (*real_fread)(void *, size_t, size_t, FILE *) = dlsym(RTLD_NEXT, "fread");
    size_t rc = real_fread(ptr, size, count, stream);
    if (mode_is("metadata-crc") && size == 1 && count == 32 && rc == 32) {
        char link[64], path[256];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fileno(stream));
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n > 0) {
            path[n] = 0;
            if (!strcmp(path, "/dev/mmcblk0p2")) {
                ((unsigned char *)ptr)[28] ^= 1;
                event("corrupted metadata read only; disk unchanged");
            }
        }
    }
    return rc;
}
