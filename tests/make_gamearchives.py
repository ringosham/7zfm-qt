#!/usr/bin/env python3
"""Writes the game archive fixtures in tests/data/game (committed, so the
tests don't need this script's modules: 'cryptography' for the encrypted
ones). Each format is written as its engine writes it; see the handlers in
src/formats/game. Run: python3 -I tests/make_gamearchives.py tests/data/game

Every archive holds the same small tree (FILES below), so a test extracts it
and compares with that.
"""
import hashlib
import os
import struct
import sys
import zlib

FILES = [
    ("readme.txt", b"7zfm-qt game archive fixture\n"),
    ("data/numbers.bin", bytes(range(256)) * 40),
    ("data/sub/text.txt", ("line %d\n" % 1).encode() * 1 + b"".join(b"line %d\n" % i for i in range(2, 400))),
    ("empty.dat", b""),
]

# the key the encrypted fixtures use (tests type it in)
GODOT_KEY = bytes(range(32))


def write(out, name, data):
    with open(os.path.join(out, name), "wb") as f:
        f.write(data)


def tree(out):
    """the files themselves, to compare extractions with"""
    for name, data in FILES:
        p = os.path.join(out, "tree", name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)


# ---------- Godot .pck (core/io/pck_packer.cpp) ----------

def godot_encrypt(data):
    """FileAccessEncrypted: MD5 of the data, its length, IV, AES-256-CFB of the data padded to 16"""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
    try:
        from cryptography.hazmat.decrepit.ciphers.modes import CFB
    except ImportError:
        from cryptography.hazmat.primitives.ciphers.modes import CFB
    iv = bytes(range(100, 116))
    padded = data + b"\0" * (-len(data) % 16)
    enc = Cipher(algorithms.AES(GODOT_KEY), CFB(iv)).encryptor()
    body = enc.update(padded) + enc.finalize()
    return hashlib.md5(data).digest() + struct.pack("<Q", len(data)) + iv + body


def godot_pck(version, encrypt_dir=False, encrypt_files=False, removal=False, res_prefix=True):
    files = list(FILES)
    header = b"GDPC" + struct.pack("<IIII", version, 4 if version > 1 else 3, 3, 0)
    flags = (1 if encrypt_dir else 0) | (2 if version >= 2 else 0)
    fixed = len(header) + (12 if version >= 2 else 0) + (8 if version >= 3 else 0) + 64

    # file data, then (version 3) the directory at the end
    def entries(file_base_rel):
        d = struct.pack("<I", len(files) + (1 if removal else 0))
        offset = 0
        blobs = []
        for name, data in files:
            path = ("res://" if res_prefix else "") + name
            pb = path.encode()
            pb += b"\0" * (-len(pb) % 4)
            blob = godot_encrypt(data) if encrypt_files else data
            d += struct.pack("<I", len(pb)) + pb + struct.pack("<QQ", offset, len(data)) + hashlib.md5(data).digest()
            if version >= 2:
                d += struct.pack("<I", 1 if encrypt_files else 0)
            blobs.append(blob)
            offset += len(blob)
            offset += -offset % 16
            blobs.append(b"\0" * (-len(blob) % 16))
        if removal:
            pb = b"res://gone.txt\0\0"
            d += struct.pack("<I", len(pb)) + pb + struct.pack("<QQ", 0, 0) + b"\0" * 16 + struct.pack("<I", 2)
        return d, b"".join(blobs)

    d, blobs = entries(True)
    if encrypt_dir:
        d = godot_encrypt(d)
    if version >= 3:
        file_base = fixed
        dir_offset = file_base + len(blobs)
        head = header + struct.pack("<IQQ", flags, file_base, dir_offset) + b"\0" * 64
        return head + blobs + d
    if version == 2:
        file_base = fixed + len(d)
        file_base += -file_base % 16
        head = header + struct.pack("<IQ", flags, file_base) + b"\0" * 64 + d
        return head + b"\0" * (file_base - len(head)) + blobs
    # version 1: offsets from the start of the pack
    file_base = fixed + len(d)
    d1 = struct.pack("<I", len(files))
    offset = file_base
    for name, data in files:
        pb = ("res://" + name).encode()
        pb += b"\0" * (-len(pb) % 4)
        d1 += struct.pack("<I", len(pb)) + pb + struct.pack("<QQ", offset, len(data)) + hashlib.md5(data).digest()
        offset += len(data)
    return header + b"\0" * 64 + d1 + b"".join(data for _, data in files)


def godot(out):
    write(out, "godot3.pck", godot_pck(1))
    write(out, "godot4.pck", godot_pck(2))
    write(out, "godot44.pck", godot_pck(3, removal=True, res_prefix=False))
    write(out, "godot4-enc.pck", godot_pck(2, encrypt_dir=True, encrypt_files=True))
    # an exported game: the pack after the executable, then its size and "GDPC"
    pck = godot_pck(2)
    exe = b"MZ" + b"\x90" * 4094
    write(out, "godot4-game.exe", exe + pck + struct.pack("<Q", len(pck)) + b"GDPC")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tests/data/game"
    os.makedirs(out, exist_ok=True)
    tree(out)
    godot(out)


if __name__ == "__main__":
    main()
