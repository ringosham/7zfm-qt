# ACE fixtures

Made with ACE32.EXE 2.6 (ACE Compression Software, from ace26.exe as once
published on winace.com) under wine, from the files in `../game/tree` and
generated media whose SHA-256 is in `media.sha256`:

| archive | made with | covers |
|---|---|---|
| `ace10.ace` | `a -r -s- -c2- -m3` | ACE 1.0 LZ77, not solid |
| `solid.ace` | `a -r -s -m5` | ACE 2.0, solid |
| `media.ace` | `a -s -m5 -c2` | delta, x86 call and picture modes |
| `sound.ace` | `a -s -m5 -c2` | 8-bit and 16-bit sound modes |
| `pass.ace`, `pass-solid.ace` | `-psecret` | Blowfish encryption |

The media: tone.wav (16-bit stereo), picture.bmp (24-bit), calls.exe (x86-like
calls), table.dat (a table of 32-bit numbers), mono8.wav and mono16.wav.
