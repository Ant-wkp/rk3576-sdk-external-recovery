#!/usr/bin/env python3
"""Build-time identity and actual kernel stop-function checks; no hardware."""
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SDK = ROOT.parents[1]


def test_kernel_stop(tmp):
    source = (SDK / "kernel-6.1/drivers/watchdog/dw_wdt.c").read_text()
    start = source.index("static int dw_wdt_stop(")
    end = source.index("\n}\n", start) + 3
    body = source[start:end]
    harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
struct dw_wdt { void *rst; int enabled; } hw;
struct watchdog_device { unsigned long status; } device;
int aerr,derr,stuck,asserts,deasserts;
#define WDOG_HW_RUNNING 1
#define set_bit(n,p) (*(p) |= 1UL << (n))
static struct dw_wdt *to_dw_wdt(struct watchdog_device *p) { return &hw; }
static int dw_wdt_is_enabled(struct dw_wdt *p) { return p->enabled; }
static int reset_control_assert(void *p) { asserts++;return aerr; }
static int reset_control_deassert(void *p) { deasserts++;if(!derr&&!stuck)hw.enabled=0;return derr; }
'''
    harness += body + r'''
static void reset(void) { hw.rst=&hw;hw.enabled=1;aerr=derr=stuck=asserts=deasserts=0; }
int main(void) {
 reset();assert(dw_wdt_stop(&device)==0 && asserts==1 && deasserts==1 && !hw.enabled);
 reset();hw.enabled=0;assert(dw_wdt_stop(&device)==0 && !asserts);
 reset();hw.rst=0;assert(dw_wdt_stop(&device)==-EOPNOTSUPP && hw.enabled && !asserts);
 reset();aerr=-EIO;assert(dw_wdt_stop(&device)==-EIO && !deasserts);
 reset();derr=-ETIMEDOUT;assert(dw_wdt_stop(&device)==-ETIMEDOUT);
 reset();stuck=1;assert(dw_wdt_stop(&device)==-EIO && hw.enabled);
 puts("actual BSP watchdog stop tests passed (6 cases)");return 0;
}
'''
    c = tmp / "stop.c"
    c.write_text(harness)
    exe = tmp / "stop"
    subprocess.run(["cc", "-Wall", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def test_release(tmp):
    fake = tmp / "sdk"
    scripts = fake / "device/rockchip/.chips/rk3576/scripts"
    scripts.mkdir(parents=True)
    original = SDK / "device/rockchip/rk3576/scripts/gen-ab-release.sh"
    generator = scripts / original.name
    shutil.copyfile(original, generator)
    target = fake / "target"
    for d in ["target/etc", "u-boot", "kernel/include/config", "kernel/arch/arm64/boot/dts/rockchip"]:
        (fake / d).mkdir(parents=True, exist_ok=True)
    (target / "etc/ab-test-version").write_text("v1\n")
    (fake / "u-boot/u-boot.bin").write_bytes(b"\x00U-Boot 2017.09 test-abtest-v1\x00")
    (fake / "kernel/include/config/kernel.release").write_text("6.1.75-abtest-v1\n")
    dtb = fake / "kernel/arch/arm64/boot/dts/rockchip/rk3576-100ask-dshanpi-a1-ab-v1.dtb"
    subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(dtb)],
                   input='/dts-v1/; / { chosen { 100ask,ab-test-version = "v1"; }; };', text=True, check=True)
    subprocess.run(["sh", str(generator), str(target), "v1"], check=True)
    stamp = (target / "etc/rk-ota-release").read_text()
    assert "release=v1\n" in stamp and "uboot=U-Boot 2017.09 test-abtest-v1\n" in stamp
    (fake / "kernel/include/config/kernel.release").write_text("6.1.75-abtest-v2\n")
    result = subprocess.run(["sh", str(generator), str(target), "v1"], capture_output=True)
    assert result.returncode and (target / "etc/rk-ota-release").read_text() == stamp
    print("release generator tests passed (matching artifacts, stale/mixed rejection)")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="rk-boot-integration-") as name:
        tmp = pathlib.Path(name)
        test_kernel_stop(tmp)
        test_release(tmp)
