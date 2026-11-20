# CHD test images

Made by MAME's `chdman` 0.276 (Debian `mame-tools`), as no Python can write
them; `tests/run_smoke.sh` checks what they hold.

- `cd.chd`: `chdman createcd` of `make_cdimage.py`'s `cd.cue`, with a 4 MiB
  FAT image holding `HDFILE.TXT` ("hello from a CHD hard disk") as the data
  track (codecs cdlz, cdzl, cdfl).
- `dvd.chd`, `dvd-zstd.chd`: `chdman createdvd` of an ISO holding
  `numbers.txt` (`seq 1 100000`) and `hello.txt` (lzma, zlib, huff, flac;
  and zstd).
- `hd.chd`: `chdman createhd` of the same FAT image.
