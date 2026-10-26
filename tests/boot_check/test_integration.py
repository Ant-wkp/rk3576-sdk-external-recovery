#!/usr/bin/env python3
"""Build-time identity and actual kernel stop-function checks; no hardware."""
import pathlib
import os
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
    (fake / "u-boot/u-boot.bin").write_bytes(b"\x00U-Boot 2017.09\x00U-Boot 2017.09 (test)-abtest-v1\x00")
    (fake / "kernel/include/config/kernel.release").write_text("6.1.75-abtest-v1\n")
    dtb = fake / "kernel/arch/arm64/boot/dts/rockchip/rk3576-100ask-dshanpi-a1-ab-v1.dtb"
    subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(dtb)],
                   input='/dts-v1/; / { chosen { 100ask,ab-test-version = "v1"; }; };', text=True, check=True)
    subprocess.run(["sh", str(generator), str(target), "v1"], check=True)
    stamp = (target / "etc/rk-ota-release").read_text()
    assert "release=v1\n" in stamp and "uboot=U-Boot 2017.09 (test)-abtest-v1\n" in stamp
    (fake / "kernel/include/config/kernel.release").write_text("6.1.75-abtest-v2\n")
    result = subprocess.run(["sh", str(generator), str(target), "v1"], capture_output=True)
    assert result.returncode and (target / "etc/rk-ota-release").read_text() == stamp
    print("release generator tests passed (matching artifacts, stale/mixed rejection)")


def test_install_gate(tmp):
    fake = tmp / "install-sdk"
    target = fake / "buildroot/output/board/target"
    (target / "usr/bin").mkdir(parents=True)
    (target / "etc/init.d").mkdir(parents=True)
    gate = fake / "buildroot/package/rockchip/recovery/check-boot-install.sh"
    gate.parent.mkdir(parents=True)
    shutil.copyfile(SDK / "buildroot/package/rockchip/recovery/check-boot-install.sh", gate)
    source = fake / "external/recovery"
    build = target.parent / "build/recovery-develop"
    inputs = ["Makefile", "bootloader.h", "update_engine/main.c", "update_engine/boot_check.c",
              "update_engine/boot_check.h", "update_engine/rktools.c", "update_engine/rktools.h",
              "update_engine/rkbootloader.c", "update_engine/S99_bootcontrol"]
    for name in inputs:
        for directory in [source, build]:
            file = directory / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text("fixture input\n")
            os.utime(file, (1000, 1000))
    (build / "updateEngine").write_text("fixture executable\n")
    def run():
        return subprocess.run(["sh", str(gate), str(target)], capture_output=True).returncode
    assert run() != 0
    for name in ["usr/bin/updateEngine", "etc/init.d/S99_bootcontrol"]:
        file = target / name
        file.write_text("#!/bin/sh\n# --boot-check\n")
        file.chmod(0o755)
    assert run() != 0  # Missing release must fail even if vendor logging hid it.
    stamp = target / "etc/rk-ota-release"
    stamp.write_text("release=v1\nkernel=k\nuboot=u\ndtb=v1\nlayout=l\nspl_abi=a\n")
    assert run() == 0
    (source / "update_engine/boot_check.c").write_text("changed source\n")
    assert run() != 0
    shutil.copyfile(source / "update_engine/boot_check.c", build / "update_engine/boot_check.c")
    os.utime(build / "updateEngine", (1000, 1000))
    assert run() != 0  # Synced source is not evidence of recompilation.
    os.utime(build / "updateEngine", None)
    assert run() == 0
    old = target / "etc/init.d/S99ab-watchdog"
    old.write_text("old feeder\n")
    assert run() != 0
    old.unlink()
    (target / "etc/init.d/S99_bootcontrol").write_text("# --boot-check\nupdateEngine --misc=now\n")
    assert run() != 0
    print("rootfs install gate tests passed (missing files, old feeder, unsafe confirmation)")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="rk-boot-integration-") as name:
        tmp = pathlib.Path(name)
        test_kernel_stop(tmp)
        test_release(tmp)
        test_install_gate(tmp)
