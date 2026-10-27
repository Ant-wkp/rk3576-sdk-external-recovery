/* Board-only final-trial fixture: reduces exactly an activated target's 7 to 1.
 * No activation, arbitrary slot mutation or production linkage. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t crc32(const unsigned char *p, size_t n)
{
    uint32_t c = ~0U;
    while (n--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8; i++) c = (c >> 1) ^ (0xedb88320U & -(c & 1));
    }
    return ~c;
}

int main(int argc, char **argv)
{
    if (argc != 2 || strlen(argv[1]) != 1 || !strchr("ab", argv[1][0])) return 2;
    int target = argv[1][0] - 'a', current = 1 - target;
    char cmd[4096] = {0}, key[32];
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f || !fgets(cmd, sizeof(cmd), f) || fclose(f)) return 2;
    snprintf(key, sizeof(key), "android_slotsufix=_%c", 'a' + current);
    if (!strstr(cmd, key)) return 2;
    int fd = open("/dev/block/by-name/misc", O_RDWR | O_SYNC | O_CLOEXEC);
    struct stat st;
    unsigned char sector[512], verify[512];
    if (fd < 0 || fstat(fd, &st) || !S_ISBLK(st.st_mode) ||
        pread(fd, sector, 512, 2048) != 512) return 2;
    unsigned char *p = sector + current * 4 + 8, *t = sector + target * 4 + 8;
    uint32_t old = crc32(sector, 28);
    if (memcmp(sector, "\0AB0\1\0\0\0", 8) || p[0] != 14 || p[1] || p[2] != 1 ||
        t[0] != 15 || t[1] != 7 || t[2] || sector[28] != (old >> 24) ||
        sector[29] != ((old >> 16) & 255) || sector[30] != ((old >> 8) & 255) ||
        sector[31] != (old & 255)) return 2;
    t[1] = 1;
    uint32_t crc = crc32(sector, 28);
    for (unsigned i = 0; i < 4; i++) sector[28 + i] = crc >> (24 - 8 * i);
    if (pwrite(fd, sector, 512, 2048) != 512 || fsync(fd) ||
        pread(fd, verify, 512, 2048) != 512 || memcmp(verify, sector, 512) || close(fd)) return 2;
    printf("LAST_TRIAL_READY target=%c tries=1 sector-preserved-and-readback-verified\n", argv[1][0]);
    return 0;
}
