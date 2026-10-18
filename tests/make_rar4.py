#!/usr/bin/env python3
# Writes a store-only (method 0x30) RAR 2.9 / RAR4 archive of a directory:
#   make_rar4.py <out.rar> <dir>
# rar 7.x can no longer create RAR4 archives (-ma4 was removed), so this
# covers the RAR4 handler. The RAR3 LZ decoder is not exercised by it.
import os, struct, sys, time, zlib


def block(htype, flags, body, data=b""):
    size = 7 + len(body)
    hdr = struct.pack("<BHH", htype, flags, size) + body
    return struct.pack("<H", zlib.crc32(hdr) & 0xFFFF) + hdr + data


def dostime(t):
    lt = time.localtime(t)
    return ((lt.tm_year - 1980) << 25 | lt.tm_mon << 21 | lt.tm_mday << 16
            | lt.tm_hour << 11 | lt.tm_min << 5 | lt.tm_sec // 2)


def file_block(name, path):
    st = os.stat(path)
    is_dir = os.path.isdir(path)
    data = b"" if is_dir else open(path, "rb").read()
    flags = 0x8000 | (0x00E0 if is_dir else 0)  # LONG_BLOCK; dict bits 111 = dir
    bname = name.replace("/", "\\").encode()
    body = struct.pack("<IIBIIBBHI", len(data), len(data), 3,  # host OS: Unix
                       zlib.crc32(data), dostime(st.st_mtime), 20, 0x30,
                       len(bname), st.st_mode) + bname
    return block(0x74, flags, body, data)


def main():
    out, root = sys.argv[1], sys.argv[2]
    parts = [b"Rar!\x1a\x07\x00", block(0x73, 0, b"\0" * 6)]
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for n in sorted(dirs) + sorted(files):
            p = os.path.join(d, n)
            parts.append(file_block(os.path.relpath(p, root), p))
    parts.append(block(0x7B, 0x4000, b""))
    with open(out, "wb") as f:
        f.write(b"".join(parts))


main()
