# Test images

Images that no script here can write; `tests/run_smoke.sh` checks what they
hold. The CHDs were made by MAME's `chdman` 0.276 (Debian `mame-tools`).

- `cd.chd`: `chdman createcd` of `make_cdimage.py`'s `cd.cue`, with a 4 MiB
  FAT image holding `HDFILE.TXT` ("hello from a CHD hard disk") as the data
  track (codecs cdlz, cdzl, cdfl).
- `dvd.chd`, `dvd-zstd.chd`: `chdman createdvd` of an ISO holding
  `numbers.txt` (`seq 1 100000`) and `hello.txt` (lzma, zlib, huff, flac;
  and zstd).
- `hd.chd`: `chdman createhd` of the same FAT image.
- `exfat.img.xz`: an 8 MiB exFAT volume (`mkfs.exfat -c 4K -L TESTVOL`)
  filled through Linux's exfat driver with guestfish: `numbers.txt`
  (`seq 1 30000`), nested folders, 200 files in `many/`, an empty file and
  folder, names longer than 15 characters, in CJK and with an emoji,
  `frag1.bin` and `frag2.bin` appended to in turn (so in FAT chains), and
  `prealloc.bin` (fallocate 100000, then "start" written: zeros past its
  valid data length). `exfat.sha256` lists every file's checksum, made from
  the source files, not from this reader.
- `lzip/`: made by lzip 1.25 (Debian `lzip`). `seq.txt.lz` is `seq 1 30000`
  at `-9`; `multi.txt.lz` is two members (its first 100000 bytes, then the
  rest with a 64 KiB dictionary) concatenated; `empty.lz` an empty file;
  `hello.tar.lz` a tar of `lzip-hello.txt` ("hello from a tar.lz").
