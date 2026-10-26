#!/usr/bin/env python3
"""Compile actual SDK functions against link-only fakes; no device access."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="rk-boot-check-") as tmp:
    obj = pathlib.Path(tmp) / "rktools.o"
    exe = pathlib.Path(tmp) / "metadata"
    common = ["cc", "-D_GNU_SOURCE", "-g", "-O1", "-ffunction-sections", "-fdata-sections", "-I", str(ROOT)]
    subprocess.run(common + ["-DisMtdDevice=rk_unused_mtd", "-c", str(ROOT / "update_engine/rktools.c"), "-o", str(obj)], check=True)
    subprocess.run(common + ["-DSUCCESSFUL_BOOT", str(ROOT / "tests/boot_check/metadata_test.c"), str(obj),
                   "-Wl,--gc-sections", "-Wl,--wrap=open,--wrap=fopen,--wrap=stat,--wrap=pread,--wrap=pwrite,--wrap=fsync",
                   "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
