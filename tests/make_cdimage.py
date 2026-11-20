#!/usr/bin/env python3
# Writes raw (2352-byte sector) CD images for the CD format tests
# (src/formats/CdImage.cpp), with (payload) as the data track:
#   cd.bin + cd.cue  MODE1 data, then two audio tracks; track 2 has a
#                    pregap (INDEX 00) in the file. cd-track2.pcm and
#                    cd-track3.pcm hold the audio tracks' samples.
#   mode2.bin        MODE2 (XA form 1) data only, no .cue
#   bad.bin          MODE1 data then audio, no .cue, one data byte damaged
#   make_cdimage.py <payload> <outdir>
import os
import sys

SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


def edc_table():
    t = []
    for i in range(256):
        e = i
        for _ in range(8):
            e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
        t.append(e)
    return t


EDC = edc_table()


def edc(b):
    e = 0
    for x in b:
        e = (e >> 8) ^ EDC[(e ^ x) & 0xFF]
    return e.to_bytes(4, "little")


def bcd(n):
    return ((n // 10) << 4) | (n % 10)


def header(lba, mode):
    a = lba + 150
    return SYNC + bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75), mode])


def data_sectors(payload, mode, first_lba=0):
    payload += b"\0" * (-len(payload) % 2048)
    out = bytearray()
    for i in range(len(payload) // 2048):
        d = payload[i * 2048:(i + 1) * 2048]
        h = header(first_lba + i, mode)
        if mode == 1:
            s = h + d
            s += edc(s) + b"\0" * 8 + b"\0" * 276  # no ECC: not checked
        else:
            sub = bytes([0, 0, 0x08, 0]) * 2  # form 1, data
            s = h + sub + d
            s += edc(s[16:]) + b"\0" * 276
        assert len(s) == 2352
        out += s
    return bytes(out)


def audio(sectors, seed):
    return bytes((i * seed + (i >> 8)) & 0xFF for i in range(sectors * 2352))


def msf(sector):
    return "%02d:%02d:%02d" % (sector // 4500, sector // 75 % 60, sector % 75)


def main():
    payload = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    p = lambda name: os.path.join(out, name)

    data = data_sectors(payload, 1)
    n_data = len(data) // 2352
    pregap = b"\0" * (150 * 2352)
    t2, t3 = audio(100, 7), audio(50, 13)
    with open(p("cd.bin"), "wb") as f:
        f.write(data + pregap + t2 + t3)
    with open(p("cd.cue"), "w") as f:
        f.write('FILE "CD.BIN" BINARY\r\n'
                '  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n'
                '  TRACK 02 AUDIO\r\n    INDEX 00 %s\r\n    INDEX 01 %s\r\n'
                '  TRACK 03 AUDIO\r\n    INDEX 01 %s\r\n'
                % (msf(n_data), msf(n_data + 150), msf(n_data + 250)))
    open(p("cd-track2.pcm"), "wb").write(t2)
    open(p("cd-track3.pcm"), "wb").write(t3)

    open(p("mode2.bin"), "wb").write(data_sectors(payload, 2))

    bad = bytearray(data + audio(40, 3))
    bad[5 * 2352 + 16 + 100] ^= 0x01
    open(p("bad.bin"), "wb").write(bytes(bad))


main()
