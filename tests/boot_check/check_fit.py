#!/usr/bin/env python3
"""Read-only comparison of RK two-copy FIT trust payloads; no signing/OTP."""
import argparse
import gzip
import hashlib
import json
import pathlib
import struct


def parse(blob):
    if len(blob) < 40:
        raise ValueError("short FDT")
    magic, total, off, strings, _, _, _, _, ssize, tsize = struct.unpack_from(">10I", blob)
    if magic != 0xD00DFEED or total > len(blob) or off+tsize > total or strings+ssize > total:
        raise ValueError("invalid FIT header")
    pos, stack, nodes, ended = off, [], {}, False
    while pos < off+tsize:
        token, = struct.unpack_from(">I", blob, pos)
        pos += 4
        if token == 1:
            end = blob.index(0, pos, off+tsize)
            stack.append(blob[pos:end].decode("ascii"))
            nodes["/"+"/".join(stack[1:])] = {}
            pos = (end+4) & ~3
        elif token == 2:
            if not stack:
                raise ValueError("unbalanced FIT")
            stack.pop()
        elif token == 3:
            size, name = struct.unpack_from(">2I", blob, pos)
            pos += 8
            if name >= ssize or pos+size > off+tsize or not stack:
                raise ValueError("invalid property")
            end = blob.index(0, strings+name, strings+ssize)
            key = blob[strings+name:end].decode("ascii")
            nodes["/"+"/".join(stack[1:])][key] = blob[pos:pos+size]
            pos = (pos+size+3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            ended = True
            break
        else:
            raise ValueError("invalid structure token")
    if not ended or stack:
        raise ValueError("incomplete FIT")
    # SDK ITS templates retain algo/key-name placeholders in unsigned images.
    # A value property, not the placeholder node name, identifies a signature.
    if any("value" in props for path, props in nodes.items()
           if any(part.startswith("signature") for part in path.split("/"))):
        raise ValueError("signed FIT is outside this deployment scope")
    return total, nodes


def integer(data):
    if len(data) not in (4, 8):
        raise ValueError("invalid FIT integer")
    return int.from_bytes(data, "big")


def inspect(path):
    blob = path.read_bytes()
    if len(blob) != 4*1024*1024 or blob[:2*1024*1024] != blob[2*1024*1024:]:
        raise ValueError("expected identical two 2-MiB FIT copies")
    total, nodes = parse(blob[:2*1024*1024])
    trust = {}
    for name in ["atf-1", "atf-2", "atf-3", "optee"]:
        p = nodes["/images/"+name]
        if "data" in p:
            data = p["data"]
        else:
            size = integer(p["data-size"])
            position = integer(p["data-position"]) if "data-position" in p else (
                (total+3)&~3)+integer(p["data-offset"])
            if position < total or position+size > 2*1024*1024:
                raise ValueError("trust payload outside FIT copy")
            data = blob[position:position+size]
        compression = p.get("compression", b"none\0").rstrip(b"\0")
        if compression == b"gzip":
            data = gzip.decompress(data)
        elif compression != b"none":
            raise ValueError("unsupported trust compression")
        trust[name] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    return {"image": str(path), "sha256": hashlib.sha256(blob).hexdigest(),
            "identical_fit_copies": True, "signature_value_present": False, "trust_payloads": trust}


if __name__ == "__main__":
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("--reference", type=pathlib.Path, required=True)
    args.add_argument("--image", type=pathlib.Path, required=True)
    options = args.parse_args()
    reference, actual = inspect(options.reference), inspect(options.image)
    if reference["trust_payloads"] != actual["trust_payloads"]:
        raise ValueError("TF-A/OP-TEE payload changed")
    print(json.dumps({"reference": reference, "image": actual, "unchanged_trust": True}, indent=2))
