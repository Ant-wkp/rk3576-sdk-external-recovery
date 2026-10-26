/* SPDX-License-Identifier: Apache-2.0 */
/* Unsupported confirmation modes must return before any device access. */
#include <assert.h>
#include <stdlib.h>
#include "update_engine/boot_check.c"
int __wrap_open(const char *path, int flags, ...) { abort(); }
int main(void)
{
    assert(rk_boot_check(-1) == 2);
    return 0;
}
