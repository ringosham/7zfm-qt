#!/usr/bin/env python3
"""Writes the game archive fixtures in tests/data/game (committed, so the
tests don't need this script's modules: 'cryptography' for the encrypted
ones). Each format is written as its engine writes it; see the handlers in
src/formats/game. Run: python3 -I tests/make_gamearchives.py tests/data/game

Every archive holds the same small tree (FILES below), so a test extracts it
and compares with that. The Unreal paks (unreal-*.pak) are made from that
tree by repak (https://github.com/trumank/repak), not by this script:
  repak pack --version V11 --compression Zlib tree unreal-v11-zlib.pak
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


# ---------- KiriKiri .xp3 ----------

XP3_SIG = b"XP3\r\n \n\x1a\x8b\x67\x01"


def xp3(packed_index=True, cushion=False, hashed_names=False):
    body = bytearray()
    entries = b""
    names = b""
    pos = len(XP3_SIG) + 8 + (17 if cushion else 0)
    for n, (name, data) in enumerate(FILES):
        segs = b""
        # numbers.bin in two segments, one stored and one zlib; the rest zlib
        parts = [data[:1000], data[1000:]] if name.endswith("numbers.bin") else [data]
        for k, part in enumerate(parts):
            compress = not (name.endswith("numbers.bin") and k == 0)
            stored = zlib.compress(part) if compress else part
            segs += struct.pack("<IQQQ", 1 if compress else 0, pos + len(body), len(part), len(stored))
            body += stored
        adler = zlib.adler32(data)
        shown = ("%08x" % adler) if hashed_names else name
        nm = shown.encode("utf-16-le")
        info = struct.pack("<IQQ", 0, len(data), sum(struct.unpack_from("<Q", segs, 20 + 28 * i)[0] for i in range(len(parts))))
        info += struct.pack("<H", len(shown)) + nm
        chunk = (b"info" + struct.pack("<Q", len(info)) + info
                 + b"segm" + struct.pack("<Q", len(segs)) + segs
                 + b"adlr" + struct.pack("<Q", 4) + struct.pack("<I", adler)
                 + b"time" + struct.pack("<Q", 8) + struct.pack("<Q", 133000000000000000 + n))
        entries += b"File" + struct.pack("<Q", len(chunk)) + chunk
        if hashed_names:
            real = name.encode("utf-16-le")
            ch = struct.pack("<IH", adler, len(name)) + real
            names += b"eliF" + struct.pack("<Q", len(ch)) + ch
    index = names + entries
    index_off = pos + len(body)
    if packed_index:
        z = zlib.compress(index)
        idx = b"\x01" + struct.pack("<QQ", len(z), len(index)) + z
    else:
        idx = b"\x00" + struct.pack("<Q", len(index)) + index
    if cushion:
        head = XP3_SIG + struct.pack("<Q", len(XP3_SIG) + 8) + struct.pack("<I5xQ", 0x80, index_off)
        assert len(head) == pos
    else:
        head = XP3_SIG + struct.pack("<Q", index_off)
    return head + bytes(body) + idx


def kirikiri(out):
    write(out, "kirikiri.xp3", xp3())
    write(out, "kirikiri-plain.xp3", xp3(packed_index=False))
    write(out, "kirikiriz.xp3", xp3(cushion=True, hashed_names=True))
    write(out, "kirikiri-game.exe", b"MZ" + b"\x90" * 8190 + xp3())


# ---------- Bethesda .bsa / .ba2 ----------

def lz4_block(data):
    """an LZ4 block of literals only (valid, not compressed)"""
    n = len(data)
    if n < 15:
        return bytes([n << 4]) + data
    out = bytearray([0xF0])
    rest = n - 15
    while rest >= 255:
        out.append(255)
        rest -= 255
    out.append(rest)
    return bytes(out) + data


def lz4_frame(data):
    """an LZ4 frame with uncompressed blocks"""
    out = bytearray(b"\x04\x22\x4d\x18")
    flg, bd = 0x60, 0x40          # version 1, independent blocks; 64 KB blocks
    out += bytes([flg, bd, (xxh32(bytes([flg, bd])) >> 8) & 0xFF])
    for i in range(0, len(data), 65536):
        blk = data[i:i + 65536]
        out += struct.pack("<I", len(blk) | 0x80000000) + blk
    out += b"\0\0\0\0"
    return bytes(out)


def xxh32(data, seed=0):
    P1, P2, P3, P4, P5 = 2654435761, 2246822519, 3266489917, 668265263, 374761393
    M = 0xFFFFFFFF
    rotl = lambda x, r: ((x << r) | (x >> (32 - r))) & M
    n = len(data)
    i = 0
    if n >= 16:
        v = [(seed + P1 + P2) & M, (seed + P2) & M, seed, (seed - P1) & M]
        while i + 16 <= n:
            for k in range(4):
                v[k] = (rotl((v[k] + struct.unpack_from("<I", data, i + 4 * k)[0] * P2) & M, 13) * P1) & M
            i += 16
        h = (rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18)) & M
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while i + 4 <= n:
        h = (rotl((h + struct.unpack_from("<I", data, i)[0] * P3) & M, 17) * P4) & M
        i += 4
    while i < n:
        h = (rotl((h + data[i] * P5) & M, 11) * P1) & M
        i += 1
    h ^= h >> 15; h = (h * P2) & M; h ^= h >> 13; h = (h * P3) & M; h ^= h >> 16
    return h


def bsa(version, compress=True, embed=False):
    folders = {}
    for name, data in FILES:
        d, _, f = name.rpartition("/")
        folders.setdefault(d, []).append((f, data))
    flags = 1 | 2 | (4 if compress else 0) | (0x100 if embed else 0)
    rec = 24 if version == 105 else 16
    names = b"".join(f.encode() + b"\0" for d in folders for f, _ in folders[d])
    dirnames_len = sum(len(d) + 1 for d in folders)
    head_len = 36
    blocks_len = sum(1 + len(d.replace("/", "\\")) + 1 + 16 * len(folders[d]) for d in folders)
    data_start = head_len + rec * len(folders) + blocks_len + len(names)
    body = bytearray()
    recs = b""
    blocks = b""
    for d in folders:
        dn = d.replace("/", "\\").encode() + b"\0"
        off = head_len + rec * len(folders) + len(blocks) + dirnames_len  # informational
        if version == 105:
            recs += struct.pack("<QIIQ", 0, len(folders[d]), 0, off)
        else:
            recs += struct.pack("<QII", 0, len(folders[d]), off)
        blocks += bytes([len(dn)]) + dn
        for k, (f, data) in enumerate(folders[d]):
            payload = b""
            if embed:
                full = ((d + "/") if d else "") + f
                full = full.replace("/", "\\").encode()
                payload += bytes([len(full)]) + full
            # the second file of each folder flips the archive's compression
            flip = (k == 1)
            comp = compress != flip
            if comp:
                packed = lz4_frame(data) if version == 105 else zlib.compress(data)
                payload += struct.pack("<I", len(data)) + packed
            else:
                payload += data
            size = len(payload) | (0x40000000 if flip else 0)
            blocks += struct.pack("<QII", 0, size, data_start + len(body))
            body += payload
    head = b"BSA\0" + struct.pack("<IIIIIIII", version, head_len, flags, len(folders),
                                     len(FILES), dirnames_len, len(names), 0)
    out = head + recs + blocks + names
    assert len(out) == data_start, (len(out), data_start)
    return out + bytes(body)


def bsa_morrowind():
    n = len(FILES)
    names = [name.replace("/", "\\").encode() + b"\0" for name, _ in FILES]
    name_offsets = []
    o = 0
    for nm in names:
        name_offsets.append(o)
        o += len(nm)
    hash_offset = n * 8 + n * 4 + o
    recs = b""
    data = b""
    for _, d in FILES:
        recs += struct.pack("<II", len(d), len(data))
        data += d
    return (struct.pack("<III", 0x100, hash_offset, n) + recs
            + b"".join(struct.pack("<I", x) for x in name_offsets) + b"".join(names)
            + b"\0" * (8 * n) + data)


def ba2_gnrl(version):
    n = len(FILES)
    head_len = 24 + (8 if version in (2, 3) else 0) + (4 if version == 3 else 0)
    data_start = head_len + 36 * n
    recs = b""
    body = b""
    for name, data in FILES:
        ext = name.rpartition(".")[2].encode()[:4].ljust(4, b"\0")
        if version == 3:
            packed = lz4_block(data) if data else b""
        else:
            packed = zlib.compress(data) if len(data) > 100 else b""
        stored = packed if packed else data
        recs += struct.pack("<I4sIIQIII", 0, ext, 0, 0, data_start + len(body), len(packed), len(data), 0xBAADF00D)
        body += stored
    names = b"".join(struct.pack("<H", len(nm)) + nm for nm in (name.replace("/", "\\").encode() for name, _ in FILES))
    head = b"BTDX" + struct.pack("<I4sIQ", version, b"GNRL", n, data_start + len(body))
    if version in (2, 3):
        head += struct.pack("<II", 0, 0)
    if version == 3:
        head += struct.pack("<I", 3)
    return head + recs + body + names


def ba2_dx10():
    """one BC1 (DXT1) 8x8 texture with 4 mips, in 2 chunks, and its DDS"""
    mips = [bytes((i * 7 + m) & 0xFF for i in range(max(1, (8 >> m) // 4) ** 2 * 8)) for m in range(4)]
    chunk1 = mips[0]
    chunk2 = b"".join(mips[1:])
    head_len = 24
    rec_len = 24 + 2 * 24
    data_start = head_len + rec_len
    c1 = zlib.compress(chunk1)
    body = c1 + chunk2
    rec = struct.pack("<I4sIBBHHHBBH", 0, b"dds\0", 0, 0, 2, 24, 8, 8, 4, 71, 0)
    rec += struct.pack("<QIIHHI", data_start, len(c1), len(chunk1), 0, 0, 0xBAADF00D)
    rec += struct.pack("<QIIHHI", data_start + len(c1), 0, len(chunk2), 1, 3, 0xBAADF00D)
    name = b"textures\\stone.dds"
    names = struct.pack("<H", len(name)) + name
    head = b"BTDX" + struct.pack("<I4sIQ", 1, b"DX10", 1, data_start + len(body))
    # the DDS the handler must give
    dds = bytearray(4 + 124)
    dds[0:4] = b"DDS "
    struct.pack_into("<IIIIIII", dds, 4, 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, 8, 8, 32, 0, 4)
    struct.pack_into("<II4s", dds, 4 + 72, 32, 4, b"DXT1")
    struct.pack_into("<I", dds, 4 + 104, 0x1000 | 0x8 | 0x400000)
    return head + rec + body + names, bytes(dds) + chunk1 + chunk2


def bethesda(out):
    write(out, "oblivion.bsa", bsa(103))
    write(out, "skyrim.bsa", bsa(104, embed=True))
    write(out, "skyrim-stored.bsa", bsa(104, compress=False))
    write(out, "skyrimse.bsa", bsa(105))
    write(out, "morrowind.bsa", bsa_morrowind())
    write(out, "fallout4.ba2", ba2_gnrl(1))
    write(out, "starfield.ba2", ba2_gnrl(3))
    ba2, dds = ba2_dx10()
    write(out, "fallout4-textures.ba2", ba2)
    write(out, "tree-stone.dds", dds)


# ---------- Unity (AssetStudio's readers) ----------

def ustr(s):
    b = s.encode()
    return struct.pack("<I", len(b)) + b + b"\0" * (-len(b) % 4)


def serialized_file(objects, res_path):
    """a SerializedFile, format 22 (Unity 2021.3): objects are (classID, bytes)"""
    classes = sorted(set(c for c, _ in objects))
    meta = b"2021.3.0f1\0" + struct.pack("<i", 19) + b"\0"           # version, platform, no type trees
    meta += struct.pack("<i", len(classes))
    for c in classes:
        meta += struct.pack("<i", c) + b"\0" + struct.pack("<h", -1) + b"\0" * 16
    meta += struct.pack("<i", len(objects))
    body = b""
    offsets = []
    for c, data in objects:
        body += b"\0" * (-len(body) % 8)
        offsets.append(len(body))
        body += data
    for n, ((c, data), off) in enumerate(zip(objects, offsets)):
        meta += b"\0" * (-(48 + len(meta)) % 4)
        meta += struct.pack("<qqIi", n + 1, off, len(data), classes.index(c))
    meta += struct.pack("<iii", 0, 0, 0) + b"\0"                      # scripts, externals, ref types, user info
    data_offset = 48 + len(meta)
    data_offset += -data_offset % 16
    total = data_offset + len(body)
    head = struct.pack(">IIII", 0, 0, 22, 0) + b"\0\0\0\0" + struct.pack(">IQQQ", len(meta), total, data_offset, 0)
    return head + meta + b"\0" * (data_offset - 48 - len(meta)) + body


def unity_objects(res_path, pixels):
    text = FILES[0][1]
    textasset = ustr("readme") + struct.pack("<I", len(text)) + text + b"\0" * (-len(text) % 4)
    tex = ustr("pixels")
    tex += struct.pack("<iBB", 0, 0, 0) + b"\0\0"                     # fallback format, downscale, alpha optional, align
    tex += struct.pack("<iiiiii", 2, 2, len(pixels), 0, 4, 1)          # w, h, image size, mips stripped, RGBA32, mips
    tex += bytes([0, 0, 0, 0]) + struct.pack("<i", 0)                  # readable, preprocessed, ignore limit, streaming; priority
    tex += struct.pack("<ii", 1, 2) + b"\0" * 24 + struct.pack("<ii", 0, 1)
    tex += struct.pack("<I", 0)                                       # platform blob
    tex += struct.pack("<I", 0) + struct.pack("<QI", 0, len(pixels)) + ustr(res_path)
    return [(49, textasset), (28, tex)]


PIXELS = bytes([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 10, 20, 30, 40])   # RGBA, bottom row first


def unity_tga():
    h = bytearray(18)
    h[2] = 2
    struct.pack_into("<HHBB", h, 12, 2, 2, 32, 8)
    px = bytearray(PIXELS)
    for i in range(0, len(px), 4):
        px[i], px[i + 2] = px[i + 2], px[i]
    return bytes(h) + bytes(px)


def unityfs(lzma_blocks):
    import lzma
    cab = "CAB-0123456789abcdef"
    sf = serialized_file(unity_objects("archive:/%s/%s.resS" % (cab, cab), PIXELS), None)
    res = PIXELS
    data = sf + res
    nodes = [(0, len(sf), 4, cab), (len(sf), len(res), 0, cab + ".resS")]
    blocks = []
    payload = b""
    for i in range(0, len(data), 4096):
        chunk = data[i:i + 4096]
        if lzma_blocks:
            filt = [{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 16, "lc": 3, "lp": 0, "pb": 2}]
            packed = b"\x5d" + struct.pack("<I", 1 << 16) + lzma.compress(chunk, format=lzma.FORMAT_RAW, filters=filt)
            blocks.append((len(chunk), len(packed), 1))
        else:
            packed = lz4_block(chunk)
            blocks.append((len(chunk), len(packed), 2))
        payload += packed
    info = b"\0" * 16 + struct.pack(">i", len(blocks))
    for u, c, f in blocks:
        info += struct.pack(">IIH", u, c, f)
    info += struct.pack(">i", len(nodes))
    for off, size, flags, path in nodes:
        info += struct.pack(">qqI", off, size, flags) + path.encode() + b"\0"
    cinfo = lz4_block(info)
    head = b"UnityFS\0" + struct.pack(">I", 7) + b"5.x.x\0" + b"2021.3.0f1\0"
    size_pos = len(head)
    head += struct.pack(">qIII", 0, len(cinfo), len(info), 0x42)      # LZ4 block info, directory combined
    head += b"\0" * (-len(head) % 16)
    total = len(head) + len(cinfo) + len(payload)
    head = head[:size_pos] + struct.pack(">q", total) + head[size_pos + 8:]
    return head + cinfo + payload


def unity(out):
    write(out, "unity-lz4.unity3d", unityfs(False))
    write(out, "unity-lzma.unity3d", unityfs(True))
    d = os.path.join(out, "unity")
    os.makedirs(d, exist_ok=True)
    write(d, "sharedassets0.assets", serialized_file(unity_objects("sharedassets0.assets.resS", PIXELS), None))
    write(d, "sharedassets0.assets.resS", PIXELS)
    write(out, "tree-pixels.tga", unity_tga())


# ---------- Inno Setup 6.0.0 (u), as innoextract reads it ----------

def inno_block(data):
    """setup-0 block: CRC-32, size, LZMA flag, then 4 KB parts behind CRC-32s"""
    import lzma
    filt = [{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 16, "lc": 3, "lp": 0, "pb": 2}]
    packed = b"\x5d" + struct.pack("<I", 1 << 16) + lzma.compress(data, format=lzma.FORMAT_RAW, filters=filt)
    body = b""
    for i in range(0, len(packed), 4096):
        part = packed[i:i + 4096]
        body += struct.pack("<I", zlib.crc32(part)) + part
    head = struct.pack("<IB", len(body), 1)
    return struct.pack("<I", zlib.crc32(head)) + head + body


def inno_setup():
    u = lambda t: (lambda b: struct.pack("<I", len(b)) + b)(t.encode("utf-16-le"))
    a = lambda t: struct.pack("<I", len(t)) + t
    files = FILES
    h = b""
    h += u("Fixture") + u("Fixture 1.0") + u("fixture") + u("") + u("") + u("") + u("")  # name .. support phone
    h += u("") + u("") + u("1.0")                       # support url, updates url, version
    h += u("{pf}\\Fixture") + u("Fixture")             # default dir, group
    h += u("setup")                                     # base filename
    h += u("{app}") + u("") + u("")                     # uninstall files dir, name, icon
    h += u("") + u("") + u("") + u("")                  # mutex, user name, organisation, serial
    h += u("") + u("") + u("") + u("")                  # readme, contact, comments, modify path
    h += u("yes") + u("yes") + u("") + u("") + u("") + u("")   # uninstall key, uninstallable, close filter, setup mutex, env, assoc
    h += a(b"") + a(b"") + a(b"")                       # license, info before, info after
    h += a(b"")                                         # compiled code
    counts = [1, 0, 0, 0, 0, 0, 0, len(files), len(files), 0, 0, 0, 0, 0, 0, 0]
    h += struct.pack("<16I", *counts)
    h += b"\0" * 20                                     # windows versions
    h += struct.pack("<II", 0, 0)                       # back colours
    h += struct.pack("<BII", 0, 100, 100) + b"\0"       # wizard style, resize; alpha format
    h += b"\0" * 20 + b"\0" * 8                         # password hash, salt
    h += struct.pack("<qI", 0, 1)                       # extra disk space, slices per disk
    h += bytes([0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0]) + struct.pack("<Q", 0)
    # log mode, dir warning, privileges, privilege overrides (5.7+), language dialog, detection,
    # LZMA, architectures x2, dir page, group page; display size
    h += b"\0" * 6                                      # 48 header flags in 6.0.0
    lang = u("default") + u("English") + u("") + u("") + u("") + u("") + a(b"") + a(b"") + a(b"") + a(b"")
    lang += struct.pack("<IIIIIB", 0x409, 8, 12, 12, 8, 0)
    h += lang
    for name, data in files:
        e = u("") + u("{app}\\" + name.replace("/", "\\")) + u("") + u("")
        e += u("") + u("") + u("") + u("") + u("") + u("")   # condition
        e += b"\0" * 20
        e += struct.pack("<IIQh", files.index((name, data)), 0, 0, -1)
        e += b"\0" * 4 + b"\0"                          # 32 file flags, file type
        h += e
    h += struct.pack("<I", 0) + struct.pack("<I", 0)    # wizard images, small images
    # the data: files 0-2 in one LZMA chunk, file 3 in a stored one
    import lzma
    filt = [{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 16, "lc": 3, "lp": 0, "pb": 2}]
    solid = b"".join(d for _, d in files[:3])
    chunk1 = b"zlb\x1a" + b"\x5d" + struct.pack("<I", 1 << 16) + lzma.compress(solid, format=lzma.FORMAT_RAW, filters=filt)
    chunk2 = b"zlb\x1a" + files[3][1]
    data2 = b""
    off = 0
    for n, (name, d) in enumerate(files):
        if n < 3:
            chunk_off, file_off, chunk_size, comp = 0, off, len(chunk1) - 4, 1
            off += len(d)
        else:
            chunk_off, file_off, chunk_size, comp = len(chunk1), 0, len(chunk2) - 4, 0
        data2 += struct.pack("<IIIQQQ", 0, 0, chunk_off, file_off, len(d), chunk_size)
        data2 += hashlib.sha1(d).digest() + struct.pack("<q", 132000000000000000) + struct.pack("<II", 0, 0)
        # flags: VersionInfoValid, NotValid, TimeStampInUTC, IsUninstallerExe, CallInstructionOptimized,
        # Touch, ChunkEncrypted, ChunkCompressed (bit 7), SolidBreak, Sign, SignOnce
        data2 += struct.pack("<H", (1 << 7) if comp else 0)
    setup0 = b"Inno Setup Setup Data (6.0.0) (u)".ljust(64, b"\0") + inno_block(h) + inno_block(data2)
    exe = bytearray(b"MZ" + b"\0" * 0x7E)
    table_off = len(exe)
    header_off = table_off + 64
    data_off = header_off + len(setup0)
    table = bytes([ord("r"), ord("D"), ord("l"), ord("P"), ord("t"), ord("S"), 0xCD, 0xE6, 0xD7, ord("{"), 0x0B, ord("*")])
    table += struct.pack("<IIIIIII", 1, 0, 0, 0, 0, header_off, data_off)
    table += struct.pack("<I", zlib.crc32(table))
    struct.pack_into("<III", exe, 0x30, 0x6F6E6E49, table_off, table_off ^ 0xFFFFFFFF)
    exe += table.ljust(64, b"\0")
    return bytes(exe) + setup0 + chunk1 + chunk2


def inno(out):
    write(out, "inno6-setup.exe", inno_setup())


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tests/data/game"
    os.makedirs(out, exist_ok=True)
    tree(out)
    godot(out)
    renpy(out)
    rpgmaker(out)
    kirikiri(out)
    bethesda(out)
    unity(out)
    inno(out)


if __name__ == "__main__":
    main()
