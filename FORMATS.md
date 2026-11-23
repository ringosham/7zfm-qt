# Additional supported formats

Below are the list of additional formats supported that differs from the original 7-Zip. All of them are extract only unless otherwise specified.

## Archives

| Format | Name | Notes |
| ------ | ---- | ----------- |
| zstd | ZStandard compression | Supports compression of .tar.zst as well |
| lzip | Lzip decompression | |
| ACE | WinAce ACE 1.0 / 2.0 | Solid, self-extracting and encrypted (Blowfish) archives; .c00 volumes |


## Filesystems

| Format | Name | Notes |
| ------ | ---- | ----------- |
| exFAT | exFAT filesystem | Expecting raw partition files or block device |

## Disk images

| Format | Name | Notes |
| ------ | ---- | ----------- |
| CHD | MAME CHD (Compressed Hunks of Data) | |
| CSO | PlayStation Portable CSO (Compressed ISO) | |
| ISZ | UltraISO ISZ (Inflated ISO) | |
| MDS/MDF | Alcohol 120% MDS/MDF (Master Data Sector/Mini Disc Format) | |
| DAA | PowerISO DAA (Direct Access Archive) | Versions 0x100 and 0x110, split .partNN.daa volumes, password protected |
| GBI | gBurner image | Same format as DAA, also split and password protected |
| UIF | MagicISO UIF (Universal Image Format) | ISO images, and mixed-mode CD images (BIN/CUE, MDS, NRG) as the image file with its sheet. Also password protected |
| CISO | GameCube / Wii compact ISO (.ciso) | Equivalent to CSO. Opens at the disc image; its GameCube / Wii file system isn't read |
| CHD (GameCube / Wii) | GameCube / Wii discs in a CHD (chdman createdvd) | Equivalent to CHD. Opens at the disc image; its GameCube / Wii file system isn't read |


## Executables and setup files

| Format | Name | Notes |
| ------ | ---- | ----------- |
| Inno Setup | Inno Setup installers (setup.exe) | Versions 1.2.10 to 6.x, GOG's setup-N.bin slices, passwords (not Inno 6.4's encryption) |
| InstallShield | InstallShield cabinets (data1.hdr, dataN.cab) | Versions 5 to 2015 and later |
| InstallShield 3 | InstallShield 3 archives (.z, _SETUP.LIB, DATA.1 ...) | Also disk sets split over DATA.1, DATA.2 ... |
| InstallShield setup | Single-file InstallShield setup.exe, Developer 7 and later | The .msi, data1.cab and other files it carries, which open in turn. Also InstallShield 10.5's layout without a signature |
| UPX | UPX-packed executables (.exe, .dll, ELF) | Requires `upx` to be  installed. Extraction is called in separate process. |

## Game engine archives

| Format | Name | Notes |
| ------ | ---- | ----------- |
| Unity | Unity bundles (.unity3d, .bundle) and serialized files (.assets, level0) | Objects by type: textures as TGA/DDS, TextAssets as text, sounds as FSB banks, the rest raw |
| Unreal pak | Unreal Engine .pak | Versions 1 to 11. Hex key required for AES decryption. Oodle-compressed files (Kraken, Mermaid, Selkie, Leviathan, LZNA, Bitknit, LZB16) too. IoStore (.utoc/.ucas) isn't read |
| Godot pck | Godot engine packs (.pck), also inside a game's .exe | Hex key required for decryption |
| RPA | Ren'Py archives (.rpa, .rpi) | RPA-3.0, 2.0 and 1.0 |
| RGSS | RPG Maker XP / VX / VX Ace (.rgssad, .rgss2a, .rgss3a) | |
| RPGMV | RPG Maker MV / MZ encrypted files (.rpgmvp, .png_ ...) | Images decrypt by themselves; audio with a key found before, or asked |
| DxArchive | WOLF RPG Editor (.wolf) and other DxLib games (.dxa) | Versions 1 to 4 and 6; usually no password needed, else asked. Not WOLF RPG Editor 3's version 8 |
| XP3 | KiriKiri archives (.xp3), also inside a game's .exe | Files a game encrypts its own way come out as stored |
| BSA / BA2 | Bethesda archives, Morrowind to Starfield | BA2 textures get their DDS header back, as DirectXTex writes it |

These already open as the formats they are made of: id Tech 3 `.pk3`, LÖVE `.love` and zip-based `.phar` (zip); Unity `.unitypackage` and `.ipk` (gzip, then the tar inside).

