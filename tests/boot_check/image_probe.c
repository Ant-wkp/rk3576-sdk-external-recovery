/* SPDX-License-Identifier: Apache-2.0 */
/* Read-only use of the actual SDK parser and MD5 implementation. */
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "update_engine/rkimage.h"
#include "update_engine/log.h"

static const char *image;
int __real_open(const char *, int, ...);
FILE *__real_fopen(const char *, const char *);
int __wrap_open(const char *path, int flags, ...)
{
    assert(!strcmp(path,image) && (flags&O_ACCMODE)==O_RDONLY);
    return __real_open(path,flags);
}
FILE *__wrap_fopen(const char *path,const char *mode)
{
    assert(!strcmp(path,image) && !strcmp(mode,"rb"));
    return __real_fopen(path,mode);
}
void Log(const char *f,int line,LogPriority level,const char *fmt,...)
{
    if(level!=LOG_ERROR)return;
    va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);
}
int main(int argc,char **argv)
{
    _Static_assert(sizeof(DWORD)==4,"native image DWORD must be 32 bit");
    RKIMAGE_HDR header;struct stat st;
    if(argc!=2)return 2;
    image=argv[1];
    if(stat(image,&st)||!S_ISREG(st.st_mode)||st.st_size<4096||st.st_size>=0x100000000LL)return 2;
    if(analyticImage(image,&header)||header.item_count<0||header.item_count>MAX_PACKAGE_FILES)return 2;
    for(int i=0;i<header.item_count;i++) {
        RKIMAGE_ITEM *item=&header.item[i];
        if(!memchr(item->name,0,sizeof(item->name))||item->file[50]=='H'||item->file[55]=='H'||
           (unsigned long long)item->offset+item->size>(unsigned long long)st.st_size)return 2;
        printf("RK_ITEM\t%s\t%u\t%u\n",item->name,item->offset,item->size);
    }
    return 0;
}
