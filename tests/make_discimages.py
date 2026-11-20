#!/usr/bin/env python3
# Writes disc images in the formats of src/formats/disc for the tests, from
# (payload), a file system image used as the data track or the ISO, and the
# CD made by make_cdimage.py in the same folder (cd.bin, cd-track2.pcm,
# cd-track3.pcm): a data track of (payload) and two audio tracks.
#   make_discimages.py <payload> <outdir>
# Writes: zso.zso, cso.cso, gc.ciso, isz.isz, ecm.bin.ecm (+ ecm.bin, the
# expected output), nrg.nrg, cdi.cdi, mds.mds + mds.mdf, gdi.gdi + gdi
# track files, ccd.ccd + ccd.img, multi.cue + multi track files, and
# file.zisofs (text and zeros, as file.plain).
import bz2
import os
import struct
import sys
import zlib

RAW = 2352
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


def edc_table():
    t = []
    for i in range(256):
        e = i
        for _ in range(8):
            e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
        t.append(e)
    return t


EDC_T = edc_table()
ECC_F = [((i << 1) ^ (0x11D if i & 0x80 else 0)) & 0xFF for i in range(256)]
ECC_B = [0] * 256
for i in range(256):
    ECC_B[i ^ ECC_F[i]] = i


def edc(b):
    e = 0
    for x in b:
        e = (e >> 8) ^ EDC_T[(e ^ x) & 0xFF]
    return e


def ecc_block(src, major_count, minor_count, major_mult, minor_inc):
    size = major_count * minor_count
    out = bytearray(major_count * 2)
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            t = src[index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= t
            b ^= t
            a = ECC_F[a]
        a = ECC_B[ECC_F[a] ^ b]
        out[major] = a
        out[major + major_count] = a ^ b
    return bytes(out)


def ecc(sector):
    s = bytearray(sector)
    if s[15] == 2:
        s[12:16] = b"\0\0\0\0"
    p = ecc_block(s[12:], 86, 24, 2, 86)
    s[0x81C:0x81C + 172] = p
    q = ecc_block(s[12:], 52, 43, 86, 88)
    return p + q


def bcd(n):
    return ((n // 10) << 4) | (n % 10)


def header(lba, mode):
    a = lba + 150
    return SYNC + bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75), mode])


def mode1_sector(lba, data):
    s = bytearray(header(lba, 1) + data + b"\0" * 288)
    s[0x810:0x814] = struct.pack("<I", edc(s[:0x810]))
    s[0x81C:0x930] = ecc(s)
    return bytes(s)


def mode2_sector(lba, data, form2):
    sub = bytes([0, 0, 0x28 if form2 else 0x08, 0]) * 2
    s = bytearray(header(lba, 2) + sub + data)
    if form2:
        s += b"\0" * (2352 - len(s))
        s[0x92C:0x930] = struct.pack("<I", edc(s[16:0x92C]))
    else:
        s += b"\0" * (2352 - len(s))
        s[0x818:0x81C] = struct.pack("<I", edc(s[16:0x818]))
        s[0x81C:0x930] = ecc(s)
    return bytes(s)


def lz4_literals(data):
    # an LZ4 block of literals only
    n = len(data)
    out = bytearray()
    if n < 15:
        out.append(n << 4)
    else:
        out.append(0xF0)
        r = n - 15
        while r >= 255:
            out.append(255)
            r -= 255
        out.append(r)
    return bytes(out) + data


def blocked(payload, bs, compress, flag_plain_every=0):
    """CSO/ZSO: (index table, data) with (compress) per block"""
    n = (len(payload) + bs - 1) // bs
    data = bytearray()
    index = []
    base = 0x18 + (n + 1) * 4
    for i in range(n):
        blk = payload[i * bs:(i + 1) * bs]
        pos = base + len(data)
        if flag_plain_every and i % flag_plain_every == 0:
            index.append(pos | 0x80000000)
            data += blk
        else:
            index.append(pos)
            data += compress(blk)
    index.append(base + len(data))
    return index, bytes(data)


def raw_deflate(b):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(b) + c.flush()


def write_cso(path, payload, magic, compress):
    bs = 2048
    index, data = blocked(payload, bs, compress, flag_plain_every=5)
    with open(path, "wb") as f:
        f.write(magic + struct.pack("<IQIBBH", 0x18, len(payload), bs, 1, 0, 0))
        f.write(b"".join(struct.pack("<I", x) for x in index))
        f.write(data)


def write_gc_ciso(path, payload):
    bs = 0x8000
    n = (len(payload) + bs - 1) // bs
    used = []
    body = bytearray()
    for i in range(n):
        blk = payload[i * bs:(i + 1) * bs].ljust(bs, b"\0")
        if blk.count(0) == bs and i != n - 1:
            used.append(0)
        else:
            used.append(1)
            body += blk
    m = bytes(used).ljust(0x8000 - 8, b"\0")
    with open(path, "wb") as f:
        f.write(b"CISO" + struct.pack("<I", bs) + m + body)


def write_isz(path, payload):
    sect, chunk = 2048, 65536
    n = (len(payload) + chunk - 1) // chunk
    ptr_len = 3
    chunks, ptrs = bytearray(), []
    for i in range(n):
        c = payload[i * chunk:(i + 1) * chunk]
        if c.count(0) == len(c):
            t, d = 0, b""
        elif i % 3 == 0:
            t, d = 2, zlib.compress(c, 9)
        elif i % 3 == 1:
            t, d = 3, bz2.compress(c, 9)
        else:
            t, d = 1, c
        ptrs.append(len(d) | (t << (8 * ptr_len - 2)))
        chunks += d
    table = bytearray(b"".join(p.to_bytes(ptr_len, "little") for p in ptrs))
    key = [(~c) & 0xFF for c in b"IsZ!"]
    for i in range(len(table)):
        table[i] ^= key[i % 4]
    hdr_size = 64
    chunk_offs = hdr_size
    data_offs = chunk_offs + len(table)
    h = b"IsZ!" + struct.pack("<BBIHIBQIIBBIIIB", hdr_size, 1, 0, sect, len(payload) // sect,
                              0, 0, n, chunk, ptr_len, 0, chunk_offs, 0, data_offs, 0)
    h = h.ljust(hdr_size, b"\0")
    with open(path, "wb") as f:
        f.write(h + table + chunks)


def ecm_count(type_, count):
    count -= 1
    out = bytearray()
    out.append(((count & 0x1F) << 2) | type_ | (0x80 if count >> 5 else 0))
    count >>= 5
    while count:
        out.append((count & 0x7F) | (0x80 if count >> 7 else 0))
        count >>= 7
    return bytes(out)


def write_ecm(path, expected_path, payload):
    # mode 1 sectors, mode 2 form 1 and form 2 ones (2336 bytes after the
    # sync and header, as ECM stores them), and literal bytes between
    secs = [mode1_sector(i, payload[i * 2048:(i + 1) * 2048]) for i in range(40)]
    m2f1 = [mode2_sector(40 + i, payload[i * 2048:(i + 1) * 2048], False) for i in range(10)]
    m2f2 = [mode2_sector(50 + i, (payload[i * 2324:(i + 1) * 2324]).ljust(2324, b"\0"), True) for i in range(10)]
    literal = b"literal bytes between sectors" * 3
    expected = b"".join(secs) + literal
    out = bytearray(b"ECM\0")
    out += ecm_count(1, len(secs))
    for s in secs:
        out += s[12:15] + s[16:16 + 2048]
    out += ecm_count(0, len(literal)) + literal
    # mode 1 sectors after the literal bytes: (expected) has them unaligned,
    # so a test sees their regenerated EDC and ECC as raw bytes
    more = [mode1_sector(60 + i, payload[(10 + i) * 2048:(11 + i) * 2048]) for i in range(5)]
    out += ecm_count(1, len(more))
    for s in more:
        out += s[12:15] + s[16:16 + 2048]
    expected += b"".join(more)
    for kind, group in ((2, m2f1), (3, m2f2)):
        # the sync and header as literal bytes, then the sector body
        for s in group:
            out += ecm_count(0, 16) + s[:16]
            out += ecm_count(kind, 1)
            out += s[16:20] + (s[24:24 + 2048] if kind == 2 else s[24:24 + 2324])
            expected += s
    out += bytes([0xFC, 0xFF, 0xFF, 0xFF, 0x3F])  # end marker: count 0xFFFFFFFF
    out += struct.pack("<I", 0)
    open(path, "wb").write(bytes(out))
    open(expected_path, "wb").write(expected)


def be32(v): return struct.pack(">I", v & 0xFFFFFFFF)
def be64(v): return struct.pack(">Q", v)


def write_nrg(path, cd, n_data, t2, t3):
    """NRG v2 (NER5): DAO session with CUEX and DAOX blocks"""
    data = cd[:n_data * RAW]
    pregap = cd[n_data * RAW:(n_data + 150) * RAW]
    a2 = t2
    a3 = t3
    body = data + pregap + a2 + a3
    o1, o2 = 0, len(data)
    o2s = o2 + len(pregap)
    o3 = o2s + len(a2)
    end = len(body)
    lba2 = n_data + 150
    lba3 = lba2 + len(a2) // RAW
    def cue(track, index, lba, ctl):
        return bytes([ctl, bcd(track), bcd(index), 0]) + be32(lba)
    cuex = (cue(0, 0, -150, 0x41) + cue(1, 0, -150, 0x41) + cue(1, 1, 0, 0x41)
            + cue(2, 0, n_data, 0x01) + cue(2, 1, lba2, 0x01) + cue(3, 1, lba3, 0x01)
            + bytes([0x01, 0xAA, 1, 0]) + be32(lba3 + len(a3) // RAW))
    dao = be32(0) + b"\0" * 13 + bytes([0, 0, 1, 1, 3])
    def daox(mode, start_pre, start, end_):
        return b"\0" * 12 + struct.pack(">H", RAW) + bytes([mode, 0]) + b"\0\0" + be64(start_pre) + be64(start) + be64(end_)
    dao += daox(0x05, o1, o1, o2) + daox(0x07, o2, o2s, o3) + daox(0x07, o3, o3, end)
    def block(id_, b):
        return id_ + be32(len(b)) + b
    trailer = len(body)
    desc = block(b"CUEX", cuex) + block(b"DAOX", dao) + block(b"SINF", be32(3)) + block(b"END!", b"")
    with open(path, "wb") as f:
        f.write(body + desc + b"NER5" + be64(trailer))


def cdi_header(fname):
    return (b"\xff\xff\x00\x00\x01\x00\x00\x00\xff\xff\xff\xff" + b"\0\0\0" + bytes([3, len(fname)]) + fname
            + b"\0" * 11 + b"\x02" + b"\0" * 10 + b"\x80" + struct.pack("<I", 0) + b"\0\0" + struct.pack("<H", 0x98))


def write_cdi(path, cd, n_data, t2, t3):
    """DiscJuggler: one session, three tracks with 150-sector pregaps inside"""
    fname = b"cdi.cdi"
    gap = b"\0" * (150 * RAW)
    tracks = [(1, cd[:n_data * RAW]), (0, t2), (0, t3)]
    body = b""
    desc = bytes([1])  # sessions
    desc += bytes([0, len(tracks)]) + b"\0" * 13
    for i, (mode, d) in enumerate(tracks):
        length = 150 + len(d) // RAW
        body += gap + d
        t = cdi_header(fname)
        t += struct.pack("<H", 2) + struct.pack("<II", 150, len(d) // RAW)
        t += struct.pack("<I", 0)        # CD-TEXT blocks
        t += b"\0\0"
        t += struct.pack("<I", mode) + b"\0" * 4
        t += struct.pack("<IIII", 0, i, 0, length)
        t += b"\0" * 16
        t += struct.pack("<II", 2, 4 if mode else 0)  # read mode 2: 2352
        t += b"\0" * 9 + b"\0" * 12 + struct.pack("<I", 0) + b"\0" * 99
        desc += t
    desc += bytes([0, 0]) + b"\0" * 13  # the empty session block
    desc += cdi_header(fname) + struct.pack("<I", 0) + bytes([0]) + b"\0" * 9 + b"\0" * 13 + struct.pack("<II", 0, 0) + b"\0" * 12
    total = len(desc) + 4
    with open(path, "wb") as f:
        f.write(body + desc + struct.pack("<I", total))


def write_mds(mds_path, mdf_path, cd, n_data, t2, t3):
    """Alcohol 120% v1, CD: one session, 2352-byte sectors"""
    mdf = cd[:n_data * RAW] + t2 + t3
    open(mdf_path, "wb").write(mdf)
    tracks = [(0xAA, n_data, 0, 0), (0xA9, len(t2) // RAW, n_data * RAW, n_data + 150), (0xA9, len(t3) // RAW, n_data * RAW + len(t2), n_data + 150 + len(t2) // RAW)]
    hdr_size, sess_size, track_size = 88, 24, 80
    sess_off = hdr_size
    tracks_off = sess_off + sess_size
    extra_off = tracks_off + track_size * len(tracks)
    footer_off = extra_off + 8 * len(tracks)
    fname_off = footer_off + 16
    h = bytearray(hdr_size)
    h[0:16] = b"MEDIA DESCRIPTOR"
    h[16:18] = b"\x01\x03"
    struct.pack_into("<HH", h, 18, 0, 1)
    struct.pack_into("<I", h, 80, sess_off)
    s = struct.pack("<iiHBBHHII", -150, tracks[-1][3] + tracks[-1][1], 1, len(tracks), 0, 1, len(tracks), 0, tracks_off)
    tb = b""
    eb = b""
    for i, (mode, length, offset, lba) in enumerate(tracks):
        t = bytearray(track_size)
        t[0] = mode
        t[1] = 0
        t[2] = 0x14 if mode == 0xAA else 0x10
        t[4] = i + 1
        struct.pack_into("<IH", t, 12, extra_off + 8 * i, RAW)
        struct.pack_into("<IQII", t, 36, lba, offset, 1, footer_off)
        tb += bytes(t)
        eb += struct.pack("<II", 150 if i else 0, length)
    footer = struct.pack("<IIII", fname_off, 0, 0, 0)
    open(mds_path, "wb").write(bytes(h) + s + tb + eb + footer + b"*.mdf\0")


def write_gdi(out, cd, n_data, t2, t3):
    files = [("track01.bin", cd[:n_data * RAW], 0, 4), ("track02.raw", t2, n_data + 150, 0), ("track03.raw", t3, n_data + 150 + len(t2) // RAW, 0)]
    lines = ["%d" % len(files)]
    for i, (name, d, lba, typ) in enumerate(files):
        open(os.path.join(out, name), "wb").write(d)
        lines.append("%d %d %d 2352 %s 0" % (i + 1, lba, typ, name))
    open(os.path.join(out, "gdi.gdi"), "w").write("\r\n".join(lines) + "\r\n")


def write_ccd(out, cd, n_data, t2, t3):
    img = cd[:n_data * RAW] + b"\0" * (150 * RAW) + t2 + t3
    open(os.path.join(out, "ccd.img"), "wb").write(img)
    lba2 = n_data + 150
    lba3 = lba2 + len(t2) // RAW
    leadout = lba3 + len(t3) // RAW
    entries = [(0xA0, 0, 0x04), (0xA1, 0, 0x00), (0xA2, leadout, 0x00), (1, 0, 0x04), (2, lba2, 0x00), (3, lba3, 0x00)]
    lines = ["[CloneCD]", "Version=3", "[Disc]", "TocEntries=%d" % len(entries), "Sessions=1", "DataTracksScrambled=0", "CDTextLength=0", "[Session 1]", "PreGapMode=1", "PreGapSubC=0"]
    for i, (p, plba, ctl) in enumerate(entries):
        lines += ["[Entry %d]" % i, "Session=1", "Point=0x%02x" % p, "ADR=0x01", "Control=0x%02x" % ctl, "TrackNo=0", "AMin=0", "ASec=0", "AFrame=0", "ALBA=-150", "Zero=0", "PMin=0", "PSec=0", "PFrame=0", "PLBA=%d" % plba]
    lines += ["[TRACK 1]", "MODE=1", "INDEX 1=0", "[TRACK 2]", "MODE=0", "INDEX 0=%d" % n_data, "INDEX 1=%d" % lba2, "[TRACK 3]", "MODE=0", "INDEX 1=%d" % lba3]
    open(os.path.join(out, "ccd.ccd"), "w").write("\r\n".join(lines) + "\r\n")


def write_multi_cue(out, cd, n_data, t2, t3):
    open(os.path.join(out, "multi (Track 1).bin"), "wb").write(cd[:n_data * RAW])
    open(os.path.join(out, "multi (Track 2).bin"), "wb").write(b"\0" * (150 * RAW) + t2)
    open(os.path.join(out, "multi (Track 3).bin"), "wb").write(t3)
    open(os.path.join(out, "multi.cue"), "w").write(
        'FILE "multi (Track 1).bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'
        'FILE "multi (Track 2).bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n'
        'FILE "multi (Track 3).bin" BINARY\n  TRACK 03 AUDIO\n    INDEX 01 00:00:00\n')


def write_zisofs(path, data):
    """a file as mkzftree compresses it: 32 KiB zlib blocks"""
    bs = 1 << 15
    n = (len(data) + bs - 1) // bs
    blocks = []
    for i in range(n):
        b = data[i * bs:(i + 1) * bs]
        blocks.append(b"" if b.count(0) == len(b) else zlib.compress(b, 9))
    pos = 16 + (n + 1) * 4
    ptrs = []
    for b in blocks:
        ptrs.append(pos)
        pos += len(b)
    ptrs.append(pos)
    h = bytes([0x37, 0xE4, 0x53, 0x96, 0xC9, 0xDB, 0xD6, 0x07]) + struct.pack("<IBBH", len(data), 4, 15, 0)
    open(path, "wb").write(h + b"".join(struct.pack("<I", x) for x in ptrs) + b"".join(blocks))


def main():
    payload = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    p = lambda n: os.path.join(out, n)
    cd = open(p("cd.bin"), "rb").read()
    t2 = open(p("cd-track2.pcm"), "rb").read()
    t3 = open(p("cd-track3.pcm"), "rb").read()
    n_data = (len(payload) + 2047) // 2048

    write_cso(p("cso.cso"), payload, b"CISO", raw_deflate)
    write_cso(p("zso.zso"), payload, b"ZISO", lz4_literals)
    write_gc_ciso(p("gc.ciso"), payload)
    write_isz(p("isz.isz"), payload)
    write_ecm(p("ecm.bin.ecm"), p("ecm.bin"), payload)
    write_nrg(p("nrg.nrg"), cd, n_data, t2, t3)
    write_cdi(p("cdi.cdi"), cd, n_data, t2, t3)
    write_mds(p("mds.mds"), p("mds.mdf"), cd, n_data, t2, t3)
    write_gdi(out, cd, n_data, t2, t3)
    write_ccd(out, cd, n_data, t2, t3)
    write_multi_cue(out, cd, n_data, t2, t3)
    text = "".join("%d lorem ipsum\n" % i for i in range(30000)).encode() + b"\0" * 70000
    write_zisofs(p("file.zisofs"), text)
    open(p("file.plain"), "wb").write(text)


main()
