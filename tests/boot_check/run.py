#!/usr/bin/env python3
"""Compile actual SDK functions against link-only fakes; no device access."""
import pathlib
import os
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="rk-boot-check-") as tmp:
    obj = pathlib.Path(tmp) / "rktools.o"
    exe = pathlib.Path(tmp) / "metadata"
    common = ["cc", "-D_GNU_SOURCE", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0",
              "-g", "-O1", "-ffunction-sections", "-fdata-sections", "-I", str(ROOT)]
    subprocess.run(common + ["-DisMtdDevice=rk_unused_mtd", "-c", str(ROOT / "update_engine/rktools.c"), "-o", str(obj)], check=True)
    subprocess.run(common + ["-DSUCCESSFUL_BOOT", str(ROOT / "tests/boot_check/metadata_test.c"), str(obj),
                   "-Wl,--gc-sections", "-Wl,--wrap=open,--wrap=fopen,--wrap=stat,--wrap=pread,--wrap=pwrite,--wrap=fsync",
                   "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
    state = pathlib.Path(tmp) / "state"
    subprocess.run(common + ["-DSUCCESSFUL_BOOT", str(ROOT / "tests/boot_check/state_test.c"),
                   "-Wl,--wrap=open,--wrap=fopen,--wrap=stat,--wrap=lstat,--wrap=fstat,--wrap=realpath",
                   "-Wl,--wrap=unlink,--wrap=mkostemp,--wrap=clock_gettime,--wrap=sleep,--wrap=uname",
                   "-Wl,--wrap=ioctl,--wrap=fork,--wrap=waitpid,--wrap=execl,--wrap=pause", "-o", str(state)], check=True)
    fake = pathlib.Path(tmp) / "fake"
    for name in ["dev", "etc", "run", "userdata", "proc/self", "proc/sys/kernel/random",
                 "sys/firmware/devicetree/base/chosen", "sys/class/watchdog/watchdog0"]:
        (fake / name).mkdir(parents=True, exist_ok=True)
    subprocess.run([str(state), str(fake)], check=True, timeout=15)
    cli = pathlib.Path(tmp) / "cli"
    subprocess.run(common + ["-I", str(ROOT / "tests/boot_check/include"),
                   str(ROOT / "update_engine/main.c"), str(ROOT / "tests/boot_check/cli_stubs.c"),
                   "-o", str(cli)], check=True)
    cases = [(["--boot-check"], "boot", {}, 0, "BOOT_CHECK"),
             (["--update", "--image_url=/fixture.img"], "update", {}, 0, "ACTIVATE"),
             (["--update", "--image_url=/fixture.img"], "update", {"TEST_TRIAL":"1"}, 2, ""),
             (["--update", "--image_url=/fixture.img"], "update", {"TEST_ACTIVATE_FAIL":"1"}, 2, "ACTIVATE"),
             (["--misc=now"], "confirm", {}, 0, "CONFIRM"),
             (["--misc=now"], "confirm", {"TEST_CONFIRM_FAIL":"1"}, 2, "CONFIRM"),
             (["--misc=now"], "confirm", {"TEST_BAD_ROOT":"1"}, 2, "")]
    for extra in [["--update"], ["--misc=now"], ["--reboot"], ["--image_url=x"],
                  ["--boot-check"], ["unexpected"], ["--unknown"]]:
        cases.append((["--boot-check"]+extra,"none",{},2,""))
    cases.append((["--update","--boot-check"],"none",{},2,""))
    for args, action, extra, expected, marker in cases:
        env = dict(os.environ, TEST_ACTION=action, **extra)
        result = subprocess.run([str(cli)]+args, env=env, capture_output=True, text=True, timeout=5)
        assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
        assert marker in result.stdout
        if expected and not marker:
            assert not result.stdout, (args,result.stdout)
    print(f"native CLI tests passed ({len(cases)} cases)")
    for definitions in [[], ["-DSUCCESSFUL_BOOT", "-DRETRY_BOOT"]]:
        mode = pathlib.Path(tmp) / "mode"
        subprocess.run(common + definitions + [str(ROOT / "tests/boot_check/mode_test.c"),
                       "-Wl,--gc-sections,--wrap=open", "-o", str(mode)], check=True)
        subprocess.run([str(mode)], check=True, timeout=5)
    print("unsupported confirmation mode tests passed (no watchdog access)")
