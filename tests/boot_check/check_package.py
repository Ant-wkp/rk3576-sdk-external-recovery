#!/usr/bin/env python3
"""Read-only acceptance of this board's native full OTA package and payloads.

Not a production updater, signature verifier, or a general hostile-image parser.
The original SDK parser is run only on a locally generated, size-bounded package.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def digest(path, offset=0, size=None):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        stream.seek(offset)
        left = path.stat().st_size-offset if size is None else size
        while left:
            data = stream.read(min(left, 1024*1024))
            if not data:
                raise ValueError("short payload read")
            result.update(data)
            left -= len(data)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=pathlib.Path, required=True)
    parser.add_argument("--images", type=pathlib.Path, required=True)
    args = parser.parse_args()
    package = args.package.resolve(strict=True)
    if not package.is_file() or package.stat().st_size >= 2**32:
        raise ValueError("expected a local regular OTA file smaller than 4 GiB")
    with tempfile.TemporaryDirectory(prefix="rk-native-image-") as name:
        exe = pathlib.Path(name) / "probe"
        command = ["cc", "-D_GNU_SOURCE", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0",
                   "-O2", "-ffunction-sections", "-fdata-sections", "-I", str(ROOT)]
        command += [str(ROOT / path) for path in ["tests/boot_check/image_probe.c",
                    "update_engine/rkimage.c", "update_engine/md5sum.c", "update_engine/md5.c"]]
        command += ["-Wl,--gc-sections,--wrap=open,--wrap=fopen", "-o", str(exe)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(exe), str(package)], check=True, capture_output=True, text=True, timeout=60)
    items = {}
    for line in result.stdout.splitlines():
        if not line.startswith("RK_ITEM\t"):
            continue  # Original SDK MD5 routines also print their digest.
        fields = line.split("\t")[1:]
        if len(fields) != 3 or fields[0] in items:
            raise ValueError(f"invalid/duplicate native parser output: {line}")
        items[fields[0]] = (int(fields[1]), int(fields[2]))
    expected = {"package-file", "parameter", "uboot_a", "boot_a", "system_a"}
    if set(items) != expected:
        raise ValueError(f"unexpected package entries: {set(items)}")
    payloads = {}
    ranges = []
    for key, filename in [("uboot_a", "uboot.img"), ("boot_a", "boot.img"), ("system_a", "rootfs.img")]:
        offset, size = items[key]
        source = args.images / filename
        if size != source.stat().st_size or size <= 0:
            raise ValueError(f"payload size mismatch: {key}")
        actual, wanted = digest(package, offset, size), digest(source)
        if actual != wanted:
            raise ValueError(f"payload SHA mismatch: {key}")
        ranges.append((offset, offset+size))
        payloads[key] = {"offset": offset, "size": size, "sha256": actual}
    ranges.sort()
    if any(a[1] > b[0] for a, b in zip(ranges, ranges[1:])):
        raise ValueError("overlapping component payloads")
    print(json.dumps({"package": str(package), "sha256": digest(package),
                      "native_md5_parser_passed": True, "signature_authentication": False,
                      "entries": sorted(items), "payloads": payloads}, indent=2))


if __name__ == "__main__":
    main()
