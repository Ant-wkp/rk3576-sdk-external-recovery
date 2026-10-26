/*
 * Copyright (C) 2023 Rockchip Electronics Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>
#include "log.h"
#include "rktools.h"
#include "download.h"
#include "../mtdutils/mtdutils.h"



#define LOCAL_VERSION_PATH "/etc/version"
#define DOWNLOAD_VERSION_PATH "/tmp/version"

bool getVersionFromfile(const char * filepath, char *version, int maxLength)
{
    if (version == NULL || filepath == NULL) {
        LOGE("getLocalVersion is error, version == null.\n");
        return false;
    }
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        LOGE("open %s failed, error is %s.\n", filepath, strerror(errno));
        return false;
    }

    char *line = NULL;
    size_t len = 0;
    size_t read;
    while ((read = getline(&line, &len, fp)) != -1) {
        if (read == 0 || line[0] == '#') {
            continue;
        }
        char *pline = strstr(line, "RK_VERSION");
        if (pline != NULL && (pline = strstr(pline, "=")) != NULL) {
            pline++; //过滤掉等于号
            //过滤掉空格
            while (*pline == ' ') {
                pline++;
            }
            int pline_len = strlen(pline) - 1;
            int version_len = (pline_len > maxLength ? maxLength : pline_len);
            memcpy(version, pline, version_len);
            LOGI("version = %s.\n", version);
            break;
        }
    }
    free(line);
    fclose(fp);
    return true;
}

//下载服务器版本号文件
bool getRemoteVersion(char *url, char *version, int maxLength)
{
    if (url == NULL) {
        LOGE("getRemoteVersion url is null.\n");
        return false;
    }

    if (download_file(url, DOWNLOAD_VERSION_PATH) == -1) {
        LOGE("getRemoteVersion failed, url is %s.\n", url);
        return false;
    }

    return getVersionFromfile(DOWNLOAD_VERSION_PATH, version, maxLength);
}

//获取本地版本号
bool getLocalVersion(char *version, int maxLength)
{
    return getVersionFromfile(LOCAL_VERSION_PATH, version, maxLength);
}

//判断是MTD还是block 设备
bool isMtdDevice()
{
    char param[2048];
    int fd, ret;
    char *s = NULL;
    fd = open("/proc/cmdline", O_RDONLY);
    ret = read(fd, (char*)param, 2048);
    close(fd);
    s = strstr(param, "storagemedia");
    if (s == NULL) {
        LOGI("no found storagemedia in cmdline, default is not MTD.\n");
        return false;
    } else {
        s = strstr(s, "=");
        if (s == NULL) {
            LOGI("no found storagemedia in cmdline, default is not MTD.\n");
            return false;
        }

        s++;
        while (*s == ' ') {
            s++;
        }

        if (strncmp(s, "mtd", 3) == 0 ) {
            LOGI("Now is MTD.\n");
            return true;
        } else if (strncmp(s, "sd", 2) == 0) {
            LOGI("Now is SD.\n");
            if ( !access(MTD_PATH, F_OK) ) {
                fd = open(MTD_PATH, O_RDONLY);
                ret = read(fd, (char*)param, 2048);
                close(fd);

                s = strstr(param, "mtd");
                if (s == NULL) {
                    LOGI("no found mtd.\n");
                    return false;
                }
                LOGI("Now is MTD.\n");
                return true;
            }
        }
    }
    LOGI("Current device is not MTD\n");
    return false;
}

/**
 * 从cmdline 获取从哪里引导
 * 返回值：
 *     0: a分区
 *     1: b分区
 *    -1: recovery 模式
 */
int rk_parse_slot(const char *cmdline)
{
    static const char *keys[] = { "android_slotsufix=", "androidboot.slot_suffix=" };
    int slot = -1;
    const char *p = cmdline;
    if (!p)
        return -1;
    while (*p) {
        p += strspn(p, " \t\r\n");
        size_t len = strcspn(p, " \t\r\n");
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            size_t keylen = strlen(keys[i]);
            if (len >= keylen && !memcmp(p, keys[i], keylen)) {
                if (len != keylen + 2 || p[keylen] != '_' ||
                    (p[keylen + 1] != 'a' && p[keylen + 1] != 'b'))
                    return -1;
                int found = p[keylen + 1] - 'a';
                if (slot != -1 && slot != found)
                    return -1;
                slot = found;
            }
        }
        p += len;
    }
    return slot;
}

int getCurrentSlot()
{
    char cmdline[CMDLINE_LENGTH + 1];
    size_t used = 0;
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    while (used < sizeof(cmdline) - 1) {
        ssize_t n = read(fd, cmdline + used, sizeof(cmdline) - 1 - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) { close(fd); return -1; }
        if (!n)
            break;
        used += n;
    }
    if (!used || used == sizeof(cmdline) - 1) { close(fd); return -1; }
    if (close(fd))
        return -1;
    cmdline[used] = '\0';
    return rk_parse_slot(cmdline);
}

void getFlashPoint(char *path)
{
    char *emmc_point = getenv(EMMC_POINT_NAME);
    LOGI("test Current device is emmc : %s.\n", emmc_point);
    if ( !access(emmc_point, F_OK) ) {
        LOGI("Current device is emmc : %s.\n", emmc_point);
        strcpy(path, emmc_point);
    } else if (strncmp("emmc", getenv("storagemedia"), 4) == 0) {
        LOGI("Current device is emmc : /dev/mmcblk0.\n");
        strcpy(path, "/dev/mmcblk0");
    } else {
        LOGI("Current device is nand : %s.\n", NAND_DRIVER_DEV_LBA);
        strcpy(path, NAND_DRIVER_DEV_LBA);
    }
}
/*
 * 获得flash 的大小，和块数
 */
int getFlashSize(char *path, long long* flash_size, long long* block_num)
{

    LOGI("[%s:%d]\n", __func__, __LINE__);

    off64_t total_size_64 = 0;
    if (isMtdDevice()) {
        size_t erase_size;
        size_t total_size;
        mtd_scan_partitions();
        const MtdPartition *part = mtd_find_partition_by_name("rk-nand");
        if ( part == NULL ) {
            part = mtd_find_partition_by_name("spi-nand0");
        }
        if (part == NULL || mtd_partition_info(part, &total_size, &erase_size, NULL)) {
            LOGE("Can't find rk-nand or spi-nand0\n");
            return -1;
        }
        total_size = total_size - (erase_size * 4);
        total_size_64 = total_size;
    } else {
        char flash_name[20];
        getFlashPoint(flash_name);
        int fd_dest = open(flash_name, O_RDWR | O_LARGEFILE);
        if (fd_dest < 0) {
            LOGE("Can't open %s\n", flash_name);
            return -2;
        }
        if ((total_size_64 = lseek64(fd_dest, 0, SEEK_END)) == -1) {
            LOGE("getFlashInfo lseek64 failed.\n");
            close(fd_dest);
            return -2;
        }
        lseek64(fd_dest, 0, SEEK_SET);
        close(fd_dest);
    }
    if ( flash_size ) {
        *flash_size = total_size_64 / 1024; //Kib
        LOGI("[%s:%d] flash size [%lld] \n", __func__, __LINE__, *flash_size);
    }
    if ( block_num ) {
        *block_num = (total_size_64 / 1024) * 2;
        LOGI("[%s:%d]  block num [%lld]\n", __func__, __LINE__, *block_num);
    }

    return 0;
}

int getFlashInfo (size_t *total_size, size_t *block_size, size_t *page_size)
{
    if (isMtdDevice()) {
        if (mtd_get_flash_info(total_size, block_size, page_size) != 0) {
            LOGE("%s-%d: get mtd info error\n", __func__, __LINE__);
            return -1;
        }
        return 0;
    } else {
        LOGI("[%s:%d]\n", __func__, __LINE__);
        if (total_size) {
            LOGE("%s-%d: get flash total size error. NOT support now.\n", __func__, __LINE__);
            return -1;
        }
        if (block_size) *block_size = 512 * 1024;
        if (page_size) *page_size = 2 * 1024;
        return 0;
    }
}
