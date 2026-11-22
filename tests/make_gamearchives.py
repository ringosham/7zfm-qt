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


# ---------- Ren'Py .rpa (launcher/game/archiver.rpy) ----------

def rpa(version, protocol=2, prefix=0, split=False):
    """RPA-3.0 with an XOR key, RPA-2.0, or RPA-1.0 (returns (rpi, rpa))"""
    import pickle
    key = 0x42424242 if version == 3 else 0
    header_len = 34 if version == 3 else (25 if version == 2 else 0)
    data = bytearray(b"Made with Ren'Py." if version > 1 else b"")
    data += b"\0" * (header_len - len(data)) if len(data) < header_len else b""
    index = {}
    for name, content in FILES:
        pre = content[:prefix] if version == 3 else b""
        rest = content[len(pre):]
        entries = []
        chunks = [rest[:len(rest) // 2], rest[len(rest) // 2:]] if split and len(rest) > 1 else [rest]
        for n, chunk in enumerate(chunks):
            off = header_len + len(data) - header_len if False else len(data)
            data += chunk
            if version == 3:
                entries.append((off ^ key, len(chunk) ^ key, pre if n == 0 else b""))
            else:
                entries.append((off, len(chunk)))
        index[name] = entries
    idx = zlib.compress(pickle.dumps(index, protocol))
    if version == 1:
        return idx, bytes(data)
    offset = len(data)
    if version == 3:
        head = b"RPA-3.0 %016x %08x\n" % (offset, key)
    else:
        head = b"RPA-2.0 %016x\n" % offset
    data[0:len(head)] = head
    return bytes(data) + idx


def renpy(out):
    write(out, "renpy3.rpa", rpa(3, protocol=2, prefix=5))
    write(out, "renpy3-p5.rpa", rpa(3, protocol=5, prefix=0, split=True))
    write(out, "renpy2.rpa", rpa(2, protocol=2))
    rpi, data = rpa(1, protocol=2)
    write(out, "renpy1.rpi", rpi)
    write(out, "renpy1.rpa", data)


# ---------- RPG Maker ----------

def rgss_xor(data, key):
    out = bytearray(data)
    for i in range(len(out)):
        out[i] ^= (key >> (8 * (i % 4))) & 0xFF
        if i % 4 == 3:
            key = (key * 7 + 3) & 0xFFFFFFFF
    return bytes(out)


def rgssad_v1():
    """XP / VX: Game.rgssad"""
    key = 0xDEADCAFE
    out = bytearray(b"RGSSAD\0\x01")
    step = lambda k: (k * 7 + 3) & 0xFFFFFFFF
    for name, data in FILES:
        n = name.replace("/", "\\").encode()
        out += struct.pack("<I", len(n) ^ key); key = step(key)
        for c in n:
            out.append(c ^ (key & 0xFF)); key = step(key)
        out += struct.pack("<I", len(data) ^ key); key = step(key)
        out += rgss_xor(data, key)
    return bytes(out)


def rgssad_v3():
    """VX Ace: Game.rgss3a"""
    seed = 0x1234567
    key = (seed * 9 + 3) & 0xFFFFFFFF
    names = [n.replace("/", "\\").encode() for n, _ in FILES]
    table = 12 + sum(16 + len(n) for n in names) + 16
    out = bytearray(b"RGSSAD\0\x03" + struct.pack("<I", seed))
    blobs = b""
    offset = table
    for (name, data), n in zip(FILES, names):
        fkey = (0x5EED0000 + len(blobs)) & 0xFFFFFFFF
        out += struct.pack("<IIII", offset ^ key, len(data) ^ key, fkey ^ key, len(n) ^ key)
        out += bytes(c ^ ((key >> (8 * (i % 4))) & 0xFF) for i, c in enumerate(n))
        blobs += rgss_xor(data, fkey)
        offset += len(data)
    out += struct.pack("<IIII", 0 ^ key, 0, 0, 0)
    assert len(out) == table
    return bytes(out) + blobs


PNG = (b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0)
       + struct.pack(">I", zlib.crc32(b"IHDR" + struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0)))
       + struct.pack(">I", 10) + b"IDAT" + zlib.compress(b"\0\0")[:10] + b"\0\0\0\0"
       + struct.pack(">I", 0) + b"IEND" + struct.pack(">I", zlib.crc32(b"IEND")))
OGG = b"OggS\0\x02" + b"\0" * 8 + b"\x01\x02\x03\x04" + b"fake vorbis data" * 8
MV_KEY = bytes.fromhex("d41d8cd98f00b204e9800998ecf8427e")


def rpgmv(data, key=MV_KEY):
    head = b"RPGMV\0\0\0\0\x03\x01\0\0\0\0\0"
    enc = bytes(b ^ key[i] for i, b in enumerate(data[:16]))
    return head + enc + data[16:]


def rpgmaker(out):
    write(out, "Game.rgssad", rgssad_v1())
    write(out, "Game.rgss3a", rgssad_v3())
    mv = os.path.join(out, "mv", "www")
    for d in ("data", "img/pictures", "audio/bgm"):
        os.makedirs(os.path.join(mv, d), exist_ok=True)
    with open(os.path.join(mv, "data", "System.json"), "w") as f:
        f.write('{"gameTitle":"fixture","hasEncryptedImages":true,"encryptionKey":"%s"}' % MV_KEY.hex())
    write(os.path.join(mv, "img", "pictures"), "pic.rpgmvp", rpgmv(PNG))
    write(os.path.join(mv, "audio", "bgm"), "song.rpgmvo", rpgmv(OGG))
    nokey = os.path.join(out, "mv-nokey")
    os.makedirs(nokey, exist_ok=True)
    write(nokey, "pic.png_", rpgmv(PNG))
    write(nokey, "song.ogg_", rpgmv(OGG))
    write(out, "tree-png.png", PNG)
    write(out, "tree-ogg.ogg", OGG)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tests/data/game"
    os.makedirs(out, exist_ok=True)
    tree(out)
    godot(out)
    renpy(out)
    rpgmaker(out)


if __name__ == "__main__":
    main()
