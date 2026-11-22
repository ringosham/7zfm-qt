// InnoSetup.cpp -- 7zfm-qt: Inno Setup installers (setup.exe, with GOG's
// setup-N.bin slices). A port of innoextract's reading of them (Daniel
// Scharrer, zlib license; see LICENSE.innoextract): src/loader/offsets.cpp,
// src/setup/*.cpp, src/stream/{block,chunk,slice}.cpp and exefilter.hpp,
// reduced to what listing and extracting the files needs, and changed to
// 7-Zip's interfaces.
//
//   The setup loader (an offset table at 0x30 in old versions, else the
//   RCDATA resource 11111) gives where the setup data (setup-0) and the
//   files (setup-1, or external slices when it is 0) are. setup-0 is a
//   version string ("Inno Setup Setup Data (5.5.7) (u)"), then two blocks
//   (4 KB chunks with CRC-32s, zlib or LZMA compressed): the header and
//   all entry lists, then the data entries (where each file is: its chunk,
//   offset, size, checksum). Every entry list must be read to get past it;
//   their fields depend on the version, as innoextract knows them.
//   The files are in chunks ("zlb\x1A"), each compressed as one stream
//   (zlib, bzip2, LZMA, LZMA2), possibly ARC4-encrypted, holding files one
//   after another; executables may have their x86 call addresses encoded.
//
// Paths: "{app}\bin\x.exe" becomes "app/bin/x.exe", as innoextract
// writes it. A chunk is decoded once for all the files extracted from it.
// Not done: Inno 6.4's XChaCha20 encryption (reported as unsupported).

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"
#include "Common/UTFConvert.h"

#include "Windows/PropVariant.h"
#include "Windows/PropVariantUtils.h"

#include "7zip/Common/ProgressUtils.h"
#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamObjects.h"
#include "7zip/Common/StreamUtils.h"
#include "7zip/IPassword.h"

#include "C/7zCrc.h"
#include "C/Alloc.h"
#include "C/Lzma2Dec.h"
#include "C/LzmaDec.h"
#include "C/Md5.h"
#include "C/Sha1.h"
#include "C/Sha256.h"

#include "../common/ItemArchive.h"

#include <bzlib.h>
#include <zlib.h>

#include <algorithm>
#include <string.h>
#include <vector>

using namespace NWindows;

namespace NArchive {
namespace NInno {

using NItemArc::GetUi16;
using NItemArc::GetUi32;
using NItemArc::GetUi64;
using NItemArc::ReadAt;
using NItemArc::GetStreamSize;

#define V(a, b, c) (((UInt32)(a) << 24) | ((UInt32)(b) << 16) | ((UInt32)(c) << 8))
#define VX(a, b, c, d) (V(a, b, c) | (UInt32)(d))

struct CVersion
{
  UInt32 Value;
  bool Unicode;
  bool Isx;
  bool Bits16;
  bool operator>=(UInt32 v) const { return Value >= v; }
  bool operator<(UInt32 v) const { return Value < v; }
  bool operator==(UInt32 v) const { return Value == v; }
  unsigned Bits() const { return Bits16 ? 16 : 32; }
};

enum ECompression { kComp_Stored, kComp_Zlib, kComp_BZip2, kComp_Lzma1, kComp_Lzma2, kComp_Unknown };
enum EChecksum { kSum_None, kSum_Adler32, kSum_Crc32, kSum_Md5, kSum_Sha1, kSum_Sha256 };
enum EFilter { kFilter_None, kFilter_4108, kFilter_5200, kFilter_5309 };
enum EEncryption { kEnc_None, kEnc_Arc4Md5, kEnc_Arc4Sha1, kEnc_XChaCha20 };

// ---------- the version string ----------

static bool ParseVersion(const Byte *p, size_t size, CVersion &v)
{
  v.Unicode = v.Isx = v.Bits16 = false;
  v.Value = 0;
  // legacy: "i1.2.10--16\x1A" / "i1.2.10--32\x1A"
  if (size >= 12 && p[0] == 'i' && p[11] == 0x1A)
  {
    if (p[2] != '.' || p[4] != '.' || p[7] != '-' || p[8] != '-')
      return false;
    v.Bits16 = (p[9] == '1' && p[10] == '6');
    v.Value = V(p[1] - '0', p[3] - '0', (p[5] - '0') * 10 + (p[6] - '0'));
    return true;
  }
  AString s;
  for (size_t i = 0; i < size && p[i]; i++)
    s += (char)p[i];
  if (s.Find("Inno Setup") < 0)
    return false;
  // the highest "(a.b.c[.d])" in it
  for (int pos = s.Find('('); pos >= 0; pos = s.Find('(', (unsigned)pos + 1))
  {
    const char *q = s.Ptr((unsigned)pos + 1);
    UInt32 parts[4] = { 0, 0, 0, 0 };
    unsigned n = 0;
    for (; n < 4; n++)
    {
      if (!(*q >= '0' && *q <= '9'))
        break;
      UInt32 x = 0;
      while (*q >= '0' && *q <= '9')
        x = x * 10 + (UInt32)(*q++ - '0');
      parts[n] = x;
      if (*q == 'a')
        q++;
      if (*q != '.')
      {
        n++;
        break;
      }
      q++;
    }
    if (n >= 3)
    {
      const UInt32 val = VX(parts[0], parts[1], parts[2], parts[3]);
      if (val > v.Value)
        v.Value = val;
    }
  }
  if (v.Value == 0)
    return false;
  if (s.Find("(5.5.8) (u)") >= 0)
    v.Value = V(5, 5, 7);    // unofficial
  v.Unicode = v.Value >= V(6, 3, 0) || s.Find("(u)") >= 0 || s.Find("(U)") >= 0;
  v.Isx = s.Find("My Inno Setup Extensions") >= 0 || s.Find("with ISX") >= 0;
  return true;
}

// the other reading of versions some modified builds also write
static bool NextAmbiguous(UInt32 &v)
{
  static const UInt32 k[][2] =
  {
    { V(1, 3, 21), V(1, 3, 24) }, { V(2, 0, 1), V(2, 0, 2) }, { V(3, 0, 3), V(3, 0, 4) },
    { V(4, 2, 3), V(4, 2, 4) }, { V(5, 3, 10), VX(5, 3, 10, 1) }, { V(5, 4, 2), VX(5, 4, 2, 1) },
    { V(5, 5, 0), VX(5, 5, 0, 1) }, { V(5, 5, 7), VX(5, 5, 7, 1) }
  };
  for (unsigned i = 0; i < Z7_ARRAY_SIZE(k); i++)
    if (v == k[i][0])
    {
      v = k[i][1];
      return true;
    }
  return false;
}

// ---------- reading the header stream ----------

struct CR
{
  const Byte *P;
  size_t Size, Pos;
  bool Error;
  const CVersion &Ver;
  CR(const Byte *p, size_t size, const CVersion &ver): P(p), Size(size), Pos(0), Error(false), Ver(ver) {}
  bool Need(size_t n) { if (Size - Pos < n) { Error = true; Pos = Size; return false; } return true; }
  UInt32 U8() { if (!Need(1)) return 0; return P[Pos++]; }
  UInt32 U16() { if (!Need(2)) return 0; const UInt32 v = GetUi16(P + Pos); Pos += 2; return v; }
  UInt32 U32() { if (!Need(4)) return 0; const UInt32 v = GetUi32(P + Pos); Pos += 4; return v; }
  UInt64 U64() { if (!Need(8)) return 0; const UInt64 v = GetUi64(P + Pos); Pos += 8; return v; }
  // a count: 16 bits in 16-bit setups
  UInt32 Count() { return Ver.Bits16 ? U16() : U32(); }
  void Skip(size_t n) { if (Need(n)) Pos += n; }
  void Str() { const UInt32 n = U32(); Skip(n); }
  void Str(CByteBuffer &b)
  {
    const UInt32 n = U32();
    if (!Need(n)) return;
    b.CopyFrom(P + Pos, n);
    Pos += n;
  }
  // stored_flag_reader: (n) flags, a byte per 8, 3 bytes padded to 4
  void Flags(unsigned n, unsigned padBits = 32)
  {
    const unsigned bytes = (n + 7) / 8;
    Skip(bytes + ((bytes == 3 && padBits == 32) ? 1 : 0));
  }
  // reads the flags, returning them as bits
  UInt64 FlagBits(unsigned n, unsigned padBits = 32)
  {
    UInt64 r = 0;
    const unsigned bytes = (n + 7) / 8;
    for (unsigned i = 0; i < bytes; i++)
      r |= (UInt64)U8() << (8 * i);
    if (bytes == 3 && padBits == 32)
      U8();
    return r;
  }
  void WinVer() { Skip(Ver >= V(1, 3, 19) ? 20 : 8); }
  void Condition()
  {
    if (Ver >= V(2, 0, 0) || (Ver.Isx && Ver >= V(1, 3, 8))) Str();
    if (Ver >= V(2, 0, 0) || (Ver.Isx && Ver >= V(1, 3, 17))) Str();
    if (Ver >= V(4, 0, 1)) Str();
    if (Ver >= V(4, 0, 0) || (Ver.Isx && Ver >= V(1, 3, 24))) Str();
    if (Ver >= V(4, 1, 0)) { Str(); Str(); }
  }
};

struct CHeader
{
  UInt32 Languages, Messages, Permissions, Types, Components, Tasks, Directories, Files;
  UInt32 DataEntries, Icons, Inis, Registry, Deletes, UninstallDeletes, Runs, UninstallRuns;
  int Compression;
  bool EncryptionUsed;
  bool PasswordUsed;
  int PasswordType;        // kSum_*
  Byte PasswordHash[32];
  CByteBuffer Salt;
  UInt32 SlicesPerDisk;
  CByteBuffer BaseFilename;
};

static int CompressionFrom(const CVersion &v, unsigned e)
{
  if (v >= V(5, 3, 9))
  {
    static const int k[] = { kComp_Stored, kComp_Zlib, kComp_BZip2, kComp_Lzma1, kComp_Lzma2 };
    return e < 5 ? k[e] : kComp_Unknown;
  }
  if (v >= V(4, 2, 6))
  {
    static const int k[] = { kComp_Stored, kComp_Zlib, kComp_BZip2, kComp_Lzma1 };
    return e < 4 ? k[e] : kComp_Unknown;
  }
  if (v >= V(4, 2, 5))
  {
    static const int k[] = { kComp_Stored, kComp_BZip2, kComp_Lzma1 };
    return e < 3 ? k[e] : kComp_Unknown;
  }
  static const int k[] = { kComp_Zlib, kComp_BZip2, kComp_Lzma1 };
  return e < 3 ? k[e] : kComp_Unknown;
}

static bool ReadHeader(CR &r, CHeader &h)
{
  const CVersion &v = r.Ver;
  if (v < V(1, 3, 0)) r.U32();
  r.Str(); r.Str();                                     // app name, versioned name
  if (v >= V(1, 3, 0)) r.Str();                         // app id
  r.Str();                                              // copyright
  if (v >= V(1, 3, 0)) { r.Str(); r.Str(); }            // publisher, url
  if (v >= V(5, 1, 13)) r.Str();                        // support phone
  if (v >= V(1, 3, 0)) { r.Str(); r.Str(); r.Str(); }   // support url, updates url, version
  r.Str(); r.Str();                                     // default dir, group
  if (v < V(3, 0, 0)) r.Str();                          // uninstall icon name
  r.Str(h.BaseFilename);
  if (v >= V(1, 3, 0) && v < V(5, 2, 5)) { r.Str(); r.Str(); r.Str(); }
  if (v >= V(1, 3, 3)) r.Str();
  if (v >= V(1, 3, 6)) { r.Str(); r.Str(); }
  if (v >= V(1, 3, 14)) r.Str();
  if (v >= V(3, 0, 0)) { r.Str(); r.Str(); }
  if (v >= V(4, 0, 0) || (v.Isx && v >= VX(3, 0, 6, 1))) r.Str();
  if ((v >= V(4, 0, 0) && v < V(5, 2, 5)) || (v.Isx && v >= V(1, 3, 24))) r.Str();
  if (v >= V(4, 2, 4)) { r.Str(); r.Str(); r.Str(); r.Str(); }
  if (v >= V(5, 3, 8)) r.Str();
  if (v >= V(5, 3, 10)) r.Str();
  if (v >= V(5, 5, 0)) r.Str();
  if (v >= V(5, 5, 6)) r.Str();
  if (v >= V(5, 6, 1)) { r.Str(); r.Str(); }
  if (v >= V(6, 3, 0)) { r.Str(); r.Str(); }
  if (v >= V(5, 2, 5)) { r.Str(); r.Str(); r.Str(); }
  if (v >= V(5, 2, 1) && v < V(5, 3, 10)) r.Str();
  if (v >= V(5, 2, 5)) r.Str();
  if (v >= V(2, 0, 6) && !v.Unicode) r.Skip(32);       // lead bytes
  h.Languages = (v >= V(4, 0, 0)) ? r.U32() : (v >= V(2, 0, 1) ? 1 : 0);
  h.Messages = (v >= V(4, 2, 1)) ? r.U32() : 0;
  h.Permissions = (v >= V(4, 1, 0)) ? r.U32() : 0;
  h.Types = h.Components = 0;
  if (v >= V(2, 0, 0) || v.Isx) { h.Types = r.U32(); h.Components = r.U32(); }
  h.Tasks = (v >= V(2, 0, 0) || (v.Isx && v >= V(1, 3, 17))) ? r.U32() : 0;
  h.Directories = r.Count();
  h.Files = r.Count();
  h.DataEntries = r.Count();
  h.Icons = r.Count();
  h.Inis = r.Count();
  h.Registry = r.Count();
  h.Deletes = r.Count();
  h.UninstallDeletes = r.Count();
  h.Runs = r.Count();
  h.UninstallRuns = r.Count();
  Int32 licenseSize = 0, beforeSize = 0, afterSize = 0;
  if (v < V(1, 3, 0))
  {
    licenseSize = (Int32)r.Count(); beforeSize = (Int32)r.Count(); afterSize = (Int32)r.Count();
  }
  r.WinVer();
  if (v < VX(6, 4, 0, 1)) r.U32();                      // back colour
  if (v >= V(1, 3, 3) && v < VX(6, 4, 0, 1)) r.U32();
  if (v < V(5, 5, 7)) r.U32();
  if ((v >= V(2, 0, 0) && v < V(5, 0, 4)) || v.Isx) r.U32();
  if (v >= V(6, 0, 0)) { r.U8(); r.U32(); r.U32(); }    // wizard style, resize
  if (v >= V(5, 5, 7)) r.U8();                          // image alpha
  memset(h.PasswordHash, 0, sizeof(h.PasswordHash));
  if (v >= V(6, 4, 0)) { r.Skip(4); h.PasswordType = kSum_Sha256; }
  else if (v >= V(5, 3, 9)) { if (r.Need(20)) memcpy(h.PasswordHash, r.P + r.Pos, 20); r.Skip(20); h.PasswordType = kSum_Sha1; }
  else if (v >= V(4, 2, 0)) { if (r.Need(16)) memcpy(h.PasswordHash, r.P + r.Pos, 16); r.Skip(16); h.PasswordType = kSum_Md5; }
  else { const UInt32 c = r.U32(); memcpy(h.PasswordHash, &c, 4); h.PasswordType = kSum_Crc32; }
  h.Salt.Free();
  if (v >= V(6, 4, 0)) { if (r.Need(44)) h.Salt.CopyFrom(r.P + r.Pos, 44); r.Skip(44); }
  else if (v >= V(4, 2, 2)) { if (r.Need(8)) h.Salt.CopyFrom(r.P + r.Pos, 8); r.Skip(8); }
  if (v >= V(4, 0, 0)) { r.U64(); h.SlicesPerDisk = r.U32(); }
  else { r.U32(); h.SlicesPerDisk = 1; }
  if ((v >= V(2, 0, 0) && v < V(5, 0, 0)) || (v.Isx && v >= V(1, 3, 4))) r.U8();
  if (v >= V(1, 3, 0)) r.U8();
  if (v >= V(5, 0, 0)) {}
  else if (v >= V(2, 0, 0) || (v.Isx && v >= V(1, 3, 13))) r.U8();
  if (v >= V(1, 3, 6)) r.U8();
  if (v.Isx && v >= V(2, 0, 10) && v < V(3, 0, 0)) r.U32();
  if (v >= V(3, 0, 0) && v < V(3, 0, 3)) r.U8();
  if (v >= V(5, 3, 7)) r.U8();
  else if (v >= V(3, 0, 4) || (v.Isx && v >= V(3, 0, 3))) r.U8();
  if (v >= V(5, 7, 0)) r.Flags(2);
  if (v >= V(4, 0, 10)) { r.U8(); r.U8(); }
  h.Compression = kComp_Zlib;
  if (v >= V(4, 1, 5))
    h.Compression = CompressionFrom(v, r.U8());
  if (v >= V(6, 3, 0)) {}
  else if (v >= V(5, 6, 0)) { r.Flags(5); r.Flags(5); }
  else if (v >= V(5, 1, 0)) { r.Flags(4); r.Flags(4); }
  if (v >= V(5, 2, 1) && v < V(5, 3, 10)) { r.U32(); r.U32(); }
  if (v >= V(5, 3, 3)) { r.U8(); r.U8(); }
  if (v >= V(5, 5, 0)) r.U64();
  else if (v >= V(5, 3, 6)) r.U32();
  if (v == VX(5, 3, 10, 1) || v == VX(5, 4, 2, 1) || v == VX(5, 5, 0, 1))
    r.U8();

  // the header flags, in innoextract's order: where Password, BzipUsed and
  // EncryptionUsed fall depends on the version
  unsigned n = 0;
  int passwordBit = -1, bzipBit = -1, encBit = -1;
  n++;                                                                   // DisableStartupPrompt
  if (v < V(5, 3, 10)) n++;
  n++;                                                                   // CreateAppDir
  if (v < V(5, 3, 3)) n++;
  if (v < V(1, 3, 6)) n++;
  if (v < V(5, 3, 3)) n++;
  n++;                                                                   // AllowNoIcons
  if (v < V(3, 0, 0) || v >= V(3, 0, 3)) n++;
  if (v < V(1, 3, 3)) n++;
  n++;                                                                   // AlwaysUsePersonalGroup
  if (v < VX(6, 4, 0, 1)) n += 4;
  n++;                                                                   // EnableDirDoesntExistWarning
  if (v < V(4, 1, 2)) n++;
  passwordBit = (int)n++;                                                // Password
  if (v >= V(1, 2, 6)) n++;
  if (v >= V(1, 2, 14)) n++;
  if (v.Bits() != 16)
  {
    if (v < V(3, 0, 4)) n++;
    if (v < V(3, 0, 0)) n++;
    if (v < V(1, 3, 6)) n++;
    if (v < V(5, 6, 1)) n++;
  }
  if (v >= V(1, 3, 0) && v < V(5, 3, 8)) n++;
  if (v >= V(1, 3, 1)) n++;
  if (v >= V(1, 3, 3) && v < VX(6, 4, 0, 1)) n++;
  if (v >= V(1, 3, 10)) n++;
  if (v >= V(1, 3, 20)) n++;
  if (v >= V(2, 0, 0) || (v.Isx && v >= V(1, 3, 10))) n++;
  if (v >= V(2, 0, 0)) n += 6;
  if (v >= V(2, 0, 7)) n += 2;
  if (v >= V(2, 0, 17) && v < V(4, 1, 5)) bzipBit = (int)n++;
  if (v >= V(2, 0, 18)) n++;
  if (v >= V(3, 0, 0)) n += 2;
  if (v >= V(3, 0, 1)) n++;
  if (v >= V(3, 0, 3)) n++;
  if (v >= V(4, 0, 0) || (v.Isx && v >= V(3, 0, 3))) n++;
  if (v >= V(4, 0, 0) && v < V(4, 0, 10)) n++;
  if (v >= V(4, 0, 1) && v < V(4, 0, 10)) n++;
  if (v >= V(4, 0, 9)) n++;
  if (v >= V(4, 1, 3)) n++;
  if (v >= V(4, 1, 8)) n += 2;
  if (v >= V(4, 2, 2)) encBit = (int)n++;
  if (v >= V(5, 0, 4) && v < V(5, 6, 1)) n++;
  if (v >= V(5, 1, 7) && !v.Unicode) n++;
  if (v >= V(5, 1, 13)) n++;
  if (v >= V(5, 2, 1)) n++;
  if (v >= V(5, 3, 8)) n++;
  if (v >= V(5, 3, 9)) n++;
  if (v >= V(5, 5, 0)) n += 3;
  if (v >= V(5, 5, 7)) n++;
  if (v >= V(6, 0, 0)) n += 3;
  if (v >= V(6, 3, 0)) n++;
  // up to 64 flags as bits; more is not expected
  if (n > 64)
    return false;
  const UInt64 flags = r.FlagBits(n, v.Bits());
  h.PasswordUsed = (flags >> passwordBit) & 1;
  h.EncryptionUsed = encBit >= 0 && ((flags >> encBit) & 1);
  if (v < V(4, 1, 5))
    h.Compression = (bzipBit >= 0 && ((flags >> bzipBit) & 1)) ? kComp_BZip2 : kComp_Zlib;
  if (v < V(1, 3, 0))
    r.Skip((size_t)MyMax(licenseSize, 0) + (size_t)MyMax(beforeSize, 0) + (size_t)MyMax(afterSize, 0));
  return !r.Error;
}

static UInt32 g_LanguageCodepage;

static void SkipLanguage(CR &r)
{
  const CVersion &v = r.Ver;
  if (v >= V(4, 0, 0)) r.Str();
  r.Str();
  if (v == VX(5, 5, 7, 1)) r.Str();
  r.Str(); r.Str(); r.Str(); r.Str();
  if (v >= V(4, 0, 0)) r.Str();
  if (v >= V(4, 0, 1)) { r.Str(); r.Str(); r.Str(); }
  r.U32();                                             // language id
  if (v < V(4, 2, 2)) {}
  else if (!v.Unicode) g_LanguageCodepage = r.U32();
  else if (v < V(5, 3, 0)) r.U32();
  r.U32();
  if (v < V(4, 1, 0)) r.U32();
  r.U32(); r.U32(); r.U32();
  if (v == VX(5, 5, 7, 1)) r.U32();
  if (v >= V(5, 2, 3)) r.U8();
}

static void SkipWizard(CR &r, const CHeader &h)
{
  const CVersion &v = r.Ver;
  for (int k = 0; k < 2; k++)
  {
    if (k == 1 && !(v >= V(2, 0, 0) || v.Isx))
      break;
    const UInt32 count = (v >= V(5, 6, 0)) ? r.U32() : 1;
    for (UInt32 i = 0; i < count && !r.Error; i++)
      r.Str();
  }
  if (h.Compression == kComp_BZip2 || (h.Compression == kComp_Lzma1 && v == V(4, 1, 5))
      || (h.Compression == kComp_Zlib && v >= V(4, 2, 6)))
    r.Str();
  if (h.EncryptionUsed && v < V(6, 4, 0))
    r.Str();
}

struct CFileEntry
{
  CByteBuffer Destination;
  CByteBuffer Languages;
  CByteBuffer Components;
  UInt32 Location;
};

static bool ReadEntries(CR &r, const CHeader &h, CObjectVector<CFileEntry> &files)
{
  const CVersion &v = r.Ver;
  const unsigned pad = v.Bits();
  #define LOOP(count) for (UInt32 i_ = 0; i_ < (count) && !r.Error; i_++)
  if (v < V(4, 0, 0))
    SkipWizard(r, h);
  LOOP(h.Messages) { r.Str(); r.Str(); r.U32(); }
  LOOP(h.Permissions) { r.Str(); }
  LOOP(h.Types)
  {
    r.Str(); r.Str();
    if (v >= V(4, 0, 1)) r.Str();
    if (v >= V(4, 0, 0) || (v.Isx && v >= V(1, 3, 24))) r.Str();
    r.WinVer();
    r.Flags(1);
    if (v >= V(4, 0, 3)) r.U8();
    if (v >= V(4, 0, 0)) r.U64(); else r.U32();
  }
  LOOP(h.Components)
  {
    r.Str(); r.Str(); r.Str();
    if (v >= V(4, 0, 1)) r.Str();
    if (v >= V(4, 0, 0) || (v.Isx && v >= V(1, 3, 24))) r.Str();
    if (v >= V(4, 0, 0)) r.U64(); else r.U32();
    if (v >= V(4, 0, 0) || (v.Isx && v >= V(3, 0, 3))) r.U32();
    if (v >= V(4, 0, 0) || (v.Isx && v >= V(3, 0, 4))) r.U8();
    r.WinVer();
    r.Flags(5);
    if (v >= V(4, 0, 0)) r.U64();
    else if (v >= V(2, 0, 0) || (v.Isx && v >= V(1, 3, 24))) r.U32();
  }
  LOOP(h.Tasks)
  {
    r.Str(); r.Str(); r.Str(); r.Str();
    if (v >= V(4, 0, 1)) r.Str();
    if (v >= V(4, 0, 0) || (v.Isx && v >= V(1, 3, 24))) r.Str();
    if (v >= V(4, 0, 0) || (v.Isx && v >= V(3, 0, 3))) r.U32();
    if (v >= V(4, 0, 0) || (v.Isx && v >= V(3, 0, 4))) r.U8();
    r.WinVer();
    r.Flags(2 + (v >= V(2, 0, 5)) + (v >= V(2, 0, 6)) + (v >= V(4, 2, 3)));
  }
  LOOP(h.Directories)
  {
    if (v < V(1, 3, 0)) r.U32();
    r.Str();
    r.Condition();
    if (v >= V(4, 0, 11) && v < V(4, 1, 0)) r.Str();
    if (v >= V(2, 0, 11)) r.U32();
    r.WinVer();
    if (v >= V(4, 1, 0)) r.U16();
    r.Flags(5);
  }
  LOOP(h.Files)
  {
    CFileEntry f;
    if (v < V(1, 3, 0)) r.U32();
    r.Str();                                           // source
    r.Str(f.Destination);
    r.Str();                                           // font name
    if (v >= V(5, 2, 5)) r.Str();
    // the condition, keeping components and languages
    if (v >= V(2, 0, 0) || (v.Isx && v >= V(1, 3, 8))) r.Str(f.Components);
    if (v >= V(2, 0, 0) || (v.Isx && v >= V(1, 3, 17))) r.Str();
    if (v >= V(4, 0, 1)) r.Str(f.Languages);
    if (v >= V(4, 0, 0) || (v.Isx && v >= V(1, 3, 24))) r.Str();
    if (v >= V(4, 1, 0)) { r.Str(); r.Str(); }
    r.WinVer();
    f.Location = r.U32();
    r.U32();                                           // attributes
    if (v >= V(4, 0, 0)) r.U64(); else r.U32();
    if (v < V(3, 0, 5)) r.U8();
    if (v >= V(4, 1, 0)) r.U16();
    unsigned n = 4;
    if (v.Bits() != 16) n += 3;
    if (v < V(2, 0, 0) && !v.Isx) n++;
    n += 2;
    if (v >= V(1, 2, 5)) n++;
    if (v >= V(1, 2, 6)) n++;
    if (v >= V(1, 3, 21)) n += 2;
    if (v >= V(1, 3, 25)) n++;
    if (v >= V(2, 0, 5)) n++;
    if (v >= V(3, 0, 1)) n++;
    if (v >= V(3, 0, 5)) n += 3;
    if (v >= V(4, 0, 0) || (v.Isx && v >= VX(3, 0, 6, 1))) n++;
    if (v >= V(4, 0, 5)) n++;
    if (v >= V(4, 1, 8)) n++;
    if (v >= V(4, 2, 1)) n++;
    if (v >= V(4, 2, 5)) n++;
    if (v >= V(5, 0, 3)) n++;
    if (v >= V(5, 1, 0)) n++;
    if (v >= V(5, 1, 2)) n += 2;
    if (v >= V(5, 2, 0)) n += 3;
    if (v >= V(5, 2, 5)) n++;
    r.Flags(n, pad);
    r.U8();                                            // file type
    files.Add(f);
  }
  LOOP(h.Icons)
  {
    if (v < V(1, 3, 0)) r.U32();
    r.Str(); r.Str(); r.Str(); r.Str(); r.Str(); r.Str();
    r.Condition();
    if (v >= V(5, 3, 5)) r.Str();
    if (v >= V(6, 1, 0)) r.Skip(16);
    r.WinVer();
    r.U32();
    if (v >= V(1, 3, 24)) r.U32();
    if (v >= V(1, 3, 15)) r.U8();
    if (v >= V(2, 0, 7)) r.U16();
    unsigned n = 2 + (v < V(1, 3, 26)) + (v.Bits() != 16) + (v >= V(5, 0, 3) && v < V(6, 3, 0))
        + (v >= V(5, 4, 2)) + (v >= V(5, 5, 0)) + (v >= V(6, 1, 0));
    r.Flags(n, pad);
  }
  LOOP(h.Inis)
  {
    if (v < V(1, 3, 0)) r.U32();
    r.Str(); r.Str(); r.Str(); r.Str();
    r.Condition();
    r.WinVer();
    r.Flags(5, pad);
  }
  LOOP(h.Registry)
  {
    if (v < V(1, 3, 0)) r.U32();
    r.Str();
    if (v.Bits() != 16) r.Str();
    r.Str();
    r.Condition();
    if (v >= V(4, 0, 11) && v < V(4, 1, 0)) r.Str();
    r.WinVer();
    if (v.Bits() != 16) r.U32();
    if (v >= V(4, 1, 0)) r.U16();
    r.U8();
    unsigned n = (v.Bits() != 16 ? 2 : 0) + 3 + (v >= V(1, 2, 6)) + (v >= V(1, 3, 9) ? 2 : 0)
        + (v >= V(1, 3, 12)) + (v >= V(1, 3, 16)) + (v >= V(5, 1, 0) ? 2 : 0);
    r.Flags(n, pad);
  }
  for (int k = 0; k < 2; k++)
    LOOP(k == 0 ? h.Deletes : h.UninstallDeletes)
    {
      if (v < V(1, 3, 0)) r.U32();
      r.Str();
      r.Condition();
      r.WinVer();
      r.U8();
    }
  for (int k = 0; k < 2; k++)
    LOOP(k == 0 ? h.Runs : h.UninstallRuns)
    {
      if (v < V(1, 3, 0)) r.U32();
      r.Str(); r.Str(); r.Str();
      if (v >= V(1, 3, 9)) r.Str();
      if (v >= V(2, 0, 2)) r.Str();
      if (v >= V(5, 1, 13)) r.Str();
      if (v >= V(2, 0, 0) || v.Isx) r.Str();
      r.Condition();
      r.WinVer();
      if (v >= V(1, 3, 24)) r.U32();
      r.U8();
      unsigned n = (v >= V(1, 2, 3)) + (v >= V(1, 3, 9) || (v.Isx && v >= V(1, 3, 8)))
          + (v >= V(2, 0, 0) ? 4 : 0) + (v >= V(2, 0, 8)) + (v >= V(5, 1, 10) ? 2 : 0)
          + (v >= V(5, 2, 0)) + (v >= V(6, 1, 0)) + (v >= V(6, 3, 0));
      r.Flags(n, pad);
    }
  #undef LOOP
  return !r.Error;
}

struct CDataEntry
{
  UInt32 FirstSlice, LastSlice;
  UInt32 ChunkOffset;
  UInt64 FileOffset, FileSize, ChunkSize;
  int SumType;
  Byte Sum[32];
  FILETIME MTime;
  bool MTimeDefined;
  int Compression;
  int Encryption;
  int Filter;
};

static bool ReadDataEntry(CR &r, const CHeader &h, CDataEntry &d)
{
  const CVersion &v = r.Ver;
  d.FirstSlice = r.U32();
  d.LastSlice = r.U32();
  if (v < V(4, 0, 0))
  {
    if (d.FirstSlice < 1 || d.LastSlice < 1)
      return false;
    d.FirstSlice--; d.LastSlice--;
  }
  d.ChunkOffset = r.U32();
  d.FileOffset = (v >= V(4, 0, 1)) ? r.U64() : 0;
  if (v >= V(4, 0, 0)) { d.FileSize = r.U64(); d.ChunkSize = r.U64(); }
  else { d.FileSize = r.U32(); d.ChunkSize = r.U32(); }
  memset(d.Sum, 0, sizeof(d.Sum));
  if (v >= V(6, 4, 0)) { if (r.Need(32)) memcpy(d.Sum, r.P + r.Pos, 32); r.Skip(32); d.SumType = kSum_Sha256; }
  else if (v >= V(5, 3, 9)) { if (r.Need(20)) memcpy(d.Sum, r.P + r.Pos, 20); r.Skip(20); d.SumType = kSum_Sha1; }
  else if (v >= V(4, 2, 0)) { if (r.Need(16)) memcpy(d.Sum, r.P + r.Pos, 16); r.Skip(16); d.SumType = kSum_Md5; }
  else { const UInt32 c = r.U32(); memcpy(d.Sum, &c, 4); d.SumType = (v >= V(4, 0, 1)) ? kSum_Crc32 : kSum_Adler32; }
  d.MTimeDefined = false;
  if (v.Bits16)
    r.U32();
  else
  {
    const UInt64 t = r.U64();
    d.MTime.dwLowDateTime = (DWORD)t;
    d.MTime.dwHighDateTime = (DWORD)(t >> 32);
    d.MTimeDefined = t != 0;
  }
  r.U32(); r.U32();                                    // file version
  // flags: VersionInfoValid, VersionInfoNotValid, then by version
  unsigned n = 2;
  int bzipped = -1, callOpt = -1, encrypted = -1, compressed = -1;
  if (v >= V(2, 0, 17) && v < V(4, 0, 1)) bzipped = (int)n++;
  if (v >= V(4, 0, 10)) n++;
  if (v >= V(4, 1, 0)) n++;
  if (v >= V(4, 1, 8)) callOpt = (int)n++;
  if (v >= V(4, 2, 0)) n++;
  if (v >= V(4, 2, 2)) encrypted = (int)n++;
  if (v >= V(4, 2, 5)) compressed = (int)n++;
  if (v >= V(5, 1, 13)) n++;
  if (v >= V(5, 5, 7) && v < V(6, 3, 0)) n += 2;
  const UInt64 flags = r.FlagBits(n, v.Bits());
  if (v >= V(6, 3, 0)) r.U8();
  const bool isCompressed = compressed < 0 || ((flags >> compressed) & 1);
  d.Compression = isCompressed ? h.Compression : kComp_Stored;
  if (bzipped >= 0 && ((flags >> bzipped) & 1))
    d.Compression = kComp_BZip2;
  d.Encryption = kEnc_None;
  if (encrypted >= 0 && ((flags >> encrypted) & 1))
    d.Encryption = (v >= V(6, 4, 0)) ? kEnc_XChaCha20 : (v >= V(5, 3, 9)) ? kEnc_Arc4Sha1 : kEnc_Arc4Md5;
  d.Filter = kFilter_None;
  if (callOpt >= 0 && ((flags >> callOpt) & 1))
    d.Filter = (v < V(5, 2, 0)) ? kFilter_4108 : (v < V(5, 3, 9)) ? kFilter_5200 : kFilter_5309;
  return !r.Error;
}

// ---------- blocks of setup-0 ----------

static HRESULT ReadAllInflate(const Byte *src, size_t srcSize, CByteBuffer &out)
{
  z_stream z;
  memset(&z, 0, sizeof(z));
  if (inflateInit(&z) != Z_OK)
    return E_OUTOFMEMORY;
  size_t cap = srcSize * 4 + 4096, done = 0;
  out.Alloc(cap);
  z.next_in = (Bytef *)src;
  z.avail_in = (uInt)srcSize;
  int r = Z_OK;
  while (r == Z_OK)
  {
    if (done == cap)
    {
      cap *= 2;
      out.ChangeSize_KeepData(cap, done);
    }
    z.next_out = (Byte *)out + done;
    z.avail_out = (uInt)(cap - done);
    r = inflate(&z, Z_NO_FLUSH);
    done = cap - z.avail_out;
  }
  inflateEnd(&z);
  out.ChangeSize_KeepData(done, done);
  return r == Z_STREAM_END ? S_OK : S_FALSE;
}

static HRESULT ReadAllLzma(const Byte *src, size_t srcSize, CByteBuffer &out)
{
  if (srcSize < LZMA_PROPS_SIZE)
    return S_FALSE;
  CLzmaDec dec;
  LzmaDec_CONSTRUCT(&dec)
  if (LzmaDec_Allocate(&dec, src, LZMA_PROPS_SIZE, &g_Alloc) != SZ_OK)
    return S_FALSE;
  LzmaDec_Init(&dec);
  size_t cap = srcSize * 4 + 4096, done = 0, inPos = LZMA_PROPS_SIZE;
  out.Alloc(cap);
  HRESULT res = S_OK;
  for (;;)
  {
    if (done == cap)
    {
      cap *= 2;
      out.ChangeSize_KeepData(cap, done);
    }
    SizeT outLen = cap - done, inLen = srcSize - inPos;
    ELzmaStatus status;
    const SRes sres = LzmaDec_DecodeToBuf(&dec, (Byte *)out + done, &outLen, src + inPos, &inLen, LZMA_FINISH_ANY, &status);
    done += outLen;
    inPos += inLen;
    if (sres != SZ_OK) { res = S_FALSE; break; }
    if (status == LZMA_STATUS_FINISHED_WITH_MARK || (inLen == 0 && outLen == 0))
      break;
  }
  LzmaDec_Free(&dec, &g_Alloc);
  out.ChangeSize_KeepData(done, done);
  return res;
}

// a block at (pos): CRC-32, sizes, 4 KB parts each behind a CRC-32
static HRESULT ReadBlock(IInStream *stream, UInt64 &pos, const CVersion &v, CByteBuffer &out)
{
  Byte h[13];
  const unsigned hSize = (v >= V(4, 0, 9)) ? 9 : 12;
  RINOK(ReadAt(stream, pos, h, 4 + hSize - 4 + 0))
  const UInt32 crc = GetUi32(h);
  if (CrcCalc(h + 4, hSize - 4) != crc)
    return S_FALSE;
  UInt32 storedSize;
  int comp;
  if (v >= V(4, 0, 9))
  {
    storedSize = GetUi32(h + 4);
    comp = h[8] ? (v >= V(4, 1, 6) ? kComp_Lzma1 : kComp_Zlib) : kComp_Stored;
  }
  else
  {
    const UInt32 c = GetUi32(h + 4), u = GetUi32(h + 8);
    if (c == 0xFFFFFFFF) { storedSize = u; comp = kComp_Stored; }
    else { storedSize = c; comp = kComp_Zlib; }
    storedSize += (UInt32)(((UInt64)storedSize + 4095) / 4096 * 4);
  }
  pos += hSize;
  if (storedSize > (1 << 28))
    return S_FALSE;
  CByteBuffer raw(storedSize);
  RINOK(ReadAt(stream, pos, raw, storedSize))
  pos += storedSize;
  // the parts without their CRC-32s
  CByteBuffer data(storedSize);
  size_t d = 0;
  for (size_t p = 0; p < storedSize;)
  {
    if (storedSize - p < 5)
      return S_FALSE;
    const UInt32 partCrc = GetUi32(raw + p);
    const size_t len = MyMin((size_t)4096, (size_t)storedSize - p - 4);
    if (CrcCalc(raw + p + 4, len) != partCrc)
      return S_FALSE;
    memcpy((Byte *)data + d, raw + p + 4, len);
    d += len;
    p += 4 + len;
  }
  if (comp == kComp_Stored)
  {
    out.CopyFrom(data, d);
    return S_OK;
  }
  if (comp == kComp_Zlib)
    return ReadAllInflate(data, d, out);
  return ReadAllLzma(data, d, out);
}

// ---------- the setup loader ----------

// a PE's resource (type, id) of any language: its offset and size
static bool FindResource(IInStream *stream, UInt32 type, UInt32 id, UInt64 &offset, UInt32 &size)
{
  Byte h[64];
  if (ReadAt(stream, 0, h, 64) != S_OK || h[0] != 'M' || h[1] != 'Z')
    return false;
  const UInt32 pe = GetUi32(h + 0x3C);
  Byte coff[24 + 240];
  if (ReadAt(stream, pe, coff, 24) != S_OK || memcmp(coff, "PE\0\0", 4) != 0)
    return false;
  const unsigned numSections = GetUi16(coff + 6);
  const unsigned optSize = GetUi16(coff + 20);
  if (optSize < 96 || optSize > 240 || numSections > 96)
    return false;
  Byte opt[240];
  if (ReadAt(stream, pe + 24, opt, optSize) != S_OK)
    return false;
  const bool pe64 = GetUi16(opt) == 0x20B;
  const unsigned dirs = pe64 ? 112 : 96;
  if (optSize < dirs + 3 * 8)
    return false;
  const UInt32 resRva = GetUi32(opt + dirs + 2 * 8);
  CByteBuffer secs((size_t)numSections * 40);
  if (ReadAt(stream, pe + 24 + optSize, secs, secs.Size()) != S_OK)
    return false;
  auto toFile = [&](UInt32 rva, UInt64 &off) -> bool
  {
    for (unsigned i = 0; i < numSections; i++)
    {
      const Byte *s = secs + i * 40;
      const UInt32 va = GetUi32(s + 12), vs = MyMax(GetUi32(s + 8), GetUi32(s + 16)), raw = GetUi32(s + 20);
      if (rva >= va && rva < va + vs)
      {
        off = (UInt64)raw + (rva - va);
        return true;
      }
    }
    return false;
  };
  UInt64 resOff;
  if (resRva == 0 || !toFile(resRva, resOff))
    return false;
  // three levels: type, name, language
  UInt64 dir = resOff;
  const UInt32 want[2] = { type, id };
  for (unsigned level = 0; level < 3; level++)
  {
    Byte d[16];
    if (ReadAt(stream, dir, d, 16) != S_OK)
      return false;
    const unsigned num = GetUi16(d + 12) + GetUi16(d + 14);
    bool found = false;
    for (unsigned i = 0; i < num && i < 4096; i++)
    {
      Byte e[8];
      if (ReadAt(stream, dir + 16 + i * 8, e, 8) != S_OK)
        return false;
      const UInt32 name = GetUi32(e), target = GetUi32(e + 4);
      if (level < 2 && ((name & 0x80000000) || name != want[level]))
        continue;
      if (level < 2)
      {
        if (!(target & 0x80000000))
          return false;
        dir = resOff + (target & 0x7FFFFFFF);
      }
      else
      {
        if (target & 0x80000000)
          return false;
        Byte de[8];
        if (ReadAt(stream, resOff + target, de, 8) != S_OK)
          return false;
        size = GetUi32(de + 4);
        return toFile(GetUi32(de), offset);
      }
      found = true;
      break;
    }
    if (!found)
      return false;
  }
  return false;
}

static const Byte kLoaderMagics[][12] =
{
  { 'r', 'D', 'l', 'P', 't', 'S', '0', '2', 0x87, 'e', 'V', 'x' },
  { 'r', 'D', 'l', 'P', 't', 'S', '0', '4', 0x87, 'e', 'V', 'x' },
  { 'r', 'D', 'l', 'P', 't', 'S', '0', '5', 0x87, 'e', 'V', 'x' },
  { 'r', 'D', 'l', 'P', 't', 'S', '0', '6', 0x87, 'e', 'V', 'x' },
  { 'r', 'D', 'l', 'P', 't', 'S', '0', '7', 0x87, 'e', 'V', 'x' },
  { 'r', 'D', 'l', 'P', 't', 'S', 0xCD, 0xE6, 0xD7, '{', 0x0B, '*' },
  { 'n', 'S', '5', 'W', '7', 'd', 'T', 0x83, 0xAA, 0x1B, 0x0F, 'j' }
};
static const UInt32 kLoaderVersions[] =
  { V(1, 2, 10), V(4, 0, 0), V(4, 0, 3), V(4, 0, 10), V(4, 1, 6), V(5, 1, 5), V(5, 1, 5) };

// where setup-0 (header) and setup-1 (data; 0 if in slice files) are
static bool LoadOffsets(IInStream *stream, UInt64 &headerOffset, UInt64 &dataOffset)
{
  UInt64 table = 0;
  Byte h[12];
  if (ReadAt(stream, 0x30, h, 12) == S_OK && GetUi32(h) == 0x6F6E6E49 && GetUi32(h + 4) == ~GetUi32(h + 8))
    table = GetUi32(h + 4);
  else
  {
    UInt32 size;
    if (!FindResource(stream, 10, 11111, table, size))
      return false;
  }
  Byte t[64];
  if (ReadAt(stream, table, t, 64) != S_OK)
    return false;
  UInt32 lv = 0xFFFFFFFF;
  for (unsigned i = 0; i < Z7_ARRAY_SIZE(kLoaderMagics); i++)
    if (memcmp(t, kLoaderMagics[i], 12) == 0)
      lv = kLoaderVersions[i];
  size_t p = 12;
  if (lv >= V(5, 1, 5)) p += 4;                        // revision
  p += 4;                                              // (unused)
  p += 4;                                              // exe offset
  if (lv < V(4, 1, 6)) p += 4;                         // exe compressed size
  p += 4;                                              // exe uncompressed size
  p += 4;                                              // exe checksum
  if (lv < V(4, 0, 0)) p += 4;                         // message offset
  headerOffset = GetUi32(t + p);
  dataOffset = GetUi32(t + p + 4);
  return headerOffset != 0;
}

// ---------- reading the files ----------

static void Arc4Init(Byte *state, const Byte *key, size_t keyLen)
{
  for (unsigned i = 0; i < 256; i++)
    state[i] = (Byte)i;
  unsigned j = 0;
  for (unsigned i = 0; i < 256; i++)
  {
    j = (j + state[i] + key[i % keyLen]) & 0xFF;
    const Byte t = state[i]; state[i] = state[j]; state[j] = t;
  }
  state[256] = state[257] = 0;
}

static void Arc4Crypt(Byte *state, Byte *data, size_t size)
{
  unsigned i = state[256], j = state[257];
  for (size_t k = 0; k < size; k++)
  {
    i = (i + 1) & 0xFF;
    j = (j + state[i]) & 0xFF;
    const Byte t = state[i]; state[i] = state[j]; state[j] = t;
    if (data)
      data[k] ^= state[(state[i] + state[j]) & 0xFF];
  }
  state[256] = (Byte)i;
  state[257] = (Byte)j;
}

class CHandler;

// one chunk, decoded in order
class CChunkReader
{
  CHandler *_arc;
  const CDataEntry *_d;
  UInt32 _slice;
  UInt64 _slicePos, _sliceEnd;
  CMyComPtr<IInStream> _sliceStream;
  UInt64 _rawLeft;
  Byte _arc4[258];
  bool _arc4On;
  // decoders
  CLzmaDec _lzma;
  CLzma2Dec _lzma2;
  bool _lzmaInit, _lzma2Init;
  z_stream _z;
  bool _zInit;
  bz_stream _bz;
  bool _bzInit;
  CByteBuffer _in;
  size_t _inPos, _inLen;
  bool _end;
  HRESULT ReadRaw(Byte *data, size_t size, size_t &got);
  HRESULT FillIn();
public:
  UInt64 Pos;              // in the decoded chunk
  Int32 OpRes;             // after a failure
  CChunkReader(CHandler *arc): _arc(arc), _d(NULL), _lzmaInit(false), _lzma2Init(false), _zInit(false), _bzInit(false), Pos(0), OpRes(0) {}
  ~CChunkReader() { Free(); }
  void Free();
  HRESULT Open(const CDataEntry &d);
  // (size) decoded bytes, fewer only at the end
  HRESULT Read(Byte *data, size_t size, size_t &got);
};

class CHandler Z7_final:
  public IInArchive,
  public IInArchiveGetStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(IInArchive, IInArchiveGetStream)
public:
  struct CItem
  {
    UString Path;
    unsigned Data;
  };
  CVersion _ver;
  CHeader _header;
  CObjectVector<CItem> _items;
  CRecordVector<CDataEntry> _data;
  CMyComPtr<IInStream> _stream;
  CObjectVector<CMyComPtr<IInStream> > _slices;
  UString _sliceBase;
  UInt64 _dataOffset;
  UInt64 _phySize;
  AString _password;          // as Inno hashes it: UTF-16LE in Unicode setups
  bool _passwordDefined;
  UString _comment;

  unsigned _missingSlices;
  HRESULT GetSlice(UInt32 index, CMyComPtr<IInStream> &s, UInt64 &start, UInt64 &end);
  HRESULT OpenSlices(IArchiveOpenCallback *callback);
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback);
  HRESULT GetPassword(IUnknown *cb);
  bool CheckPassword(const AString &pw) const;
};

HRESULT CHandler::GetSlice(UInt32 index, CMyComPtr<IInStream> &s, UInt64 &start, UInt64 &end)
{
  s.Release();
  if (_dataOffset != 0)
  {
    if (index != 0)
      return S_FALSE;
    s = _stream;
    start = _dataOffset;
    UInt64 size;
    RINOK(GetStreamSize(_stream, size))
    end = size;
    return S_OK;
  }
  if (index >= _slices.Size() || !_slices[index])
    return S_FALSE;
  s = _slices[index];
  Byte h[12];
  RINOK(ReadAt(s, 0, h, 12))
  if (memcmp(h, "idska16\x1a", 8) != 0 && memcmp(h, "idska32\x1a", 8) != 0)
    return S_FALSE;
  start = 0;
  end = GetUi32(h + 8);
  return S_OK;
}

// the slice files the data entries need, next to the setup
HRESULT CHandler::OpenSlices(IArchiveOpenCallback *callback)
{
  UInt32 count = 0;
  FOR_VECTOR (i, _data)
    count = MyMax(count, _data[i].LastSlice + 1);
  for (UInt32 index = 0; index < count && index < 4096; index++)
  {
    // base-1.bin, or base-1a.bin with several slices a disk
    UString name = _sliceBase;
    name += L'-';
    const UInt32 perDisk = MyMax(_header.SlicesPerDisk, (UInt32)1);
    name.Add_UInt32(index / perDisk + 1);
    if (perDisk > 1)
      name += (wchar_t)(L'a' + index % perDisk);
    name += L".bin";
    CMyComPtr<IInStream> f;
    RINOK(NItemArc::OpenSiblingFile(callback, name, f))
    _slices.Add(f);
    if (!f)
      _missingSlices++;
  }
  return S_OK;
}

void CChunkReader::Free()
{
  if (_lzmaInit) { LzmaDec_Free(&_lzma, &g_Alloc); _lzmaInit = false; }
  if (_lzma2Init) { Lzma2Dec_Free(&_lzma2, &g_Alloc); _lzma2Init = false; }
  if (_zInit) { inflateEnd(&_z); _zInit = false; }
  if (_bzInit) { BZ2_bzDecompressEnd(&_bz); _bzInit = false; }
}

HRESULT CChunkReader::ReadRaw(Byte *data, size_t size, size_t &got)
{
  got = 0;
  while (size != 0 && _rawLeft != 0)
  {
    if (_slicePos >= _sliceEnd)
    {
      // on into the next slice, after its 12-byte head
      UInt64 start, end;
      if (_arc->GetSlice(++_slice, _sliceStream, start, end) != S_OK)
        return S_FALSE;
      _slicePos = 12;
      _sliceEnd = end;
      continue;
    }
    const size_t n = (size_t)MyMin(MyMin((UInt64)size, _rawLeft), _sliceEnd - _slicePos);
    RINOK(ReadAt(_sliceStream, _slicePos, data, n))
    if (_arc4On)
      Arc4Crypt(_arc4, data, n);
    _slicePos += n;
    _rawLeft -= n;
    data += n;
    size -= n;
    got += n;
  }
  return S_OK;
}

HRESULT CChunkReader::FillIn()
{
  if (_inPos < _inLen)
    return S_OK;
  _inPos = 0;
  return ReadRaw(_in, _in.Size(), _inLen);
}

HRESULT CChunkReader::Open(const CDataEntry &d)
{
  Free();
  _d = &d;
  Pos = 0;
  OpRes = NExtract::NOperationResult::kOK;
  _end = false;
  _arc4On = false;
  _slice = d.FirstSlice;
  UInt64 start, end;
  if (_arc->GetSlice(_slice, _sliceStream, start, end) != S_OK)
  {
    OpRes = NExtract::NOperationResult::kUnavailable;
    return S_FALSE;
  }
  _slicePos = start + d.ChunkOffset;
  _sliceEnd = end;
  _rawLeft = 4;
  Byte magic[4];
  size_t got;
  RINOK(ReadRaw(magic, 4, got))
  if (got != 4 || memcmp(magic, "zlb\x1a", 4) != 0)
  {
    OpRes = NExtract::NOperationResult::kDataError;
    return S_FALSE;
  }
  _rawLeft = d.ChunkSize;
  if (d.Encryption == kEnc_XChaCha20 || (d.Encryption != kEnc_None && !_arc->_passwordDefined))
  {
    OpRes = d.Encryption == kEnc_XChaCha20 ? NExtract::NOperationResult::kUnsupportedMethod
        : NExtract::NOperationResult::kWrongPassword;
    return S_FALSE;
  }
  if (d.Encryption != kEnc_None)
  {
    Byte salt[8];
    RINOK(ReadRaw(salt, 8, got))
    Byte key[20];
    size_t keyLen;
    const AString &pw = _arc->_password;
    if (d.Encryption == kEnc_Arc4Sha1)
    {
      CSha1 s;
      Sha1_Init(&s);
      Sha1_Update(&s, salt, 8);
      Sha1_Update(&s, (const Byte *)pw.Ptr(), pw.Len());
      Sha1_Final(&s, key);
      keyLen = 20;
    }
    else
    {
      CMd5 s;
      Md5_Init(&s);
      Md5_Update(&s, salt, 8);
      Md5_Update(&s, (const Byte *)pw.Ptr(), pw.Len());
      Md5_Final(&s, key);
      keyLen = 16;
    }
    Arc4Init(_arc4, key, keyLen);
    Arc4Crypt(_arc4, NULL, 1000);
    _arc4On = true;
  }
  _in.Alloc(1 << 16);
  _inPos = _inLen = 0;
  switch (d.Compression)
  {
    case kComp_Stored: break;
    case kComp_Zlib:
      memset(&_z, 0, sizeof(_z));
      if (inflateInit(&_z) != Z_OK) return E_OUTOFMEMORY;
      _zInit = true;
      break;
    case kComp_BZip2:
      memset(&_bz, 0, sizeof(_bz));
      if (BZ2_bzDecompressInit(&_bz, 0, 0) != BZ_OK) return E_OUTOFMEMORY;
      _bzInit = true;
      break;
    case kComp_Lzma1:
    {
      Byte props[5];
      RINOK(ReadRaw(props, 5, got))
      LzmaDec_CONSTRUCT(&_lzma)
      if (got != 5 || LzmaDec_Allocate(&_lzma, props, 5, &g_Alloc) != SZ_OK)
      {
        OpRes = NExtract::NOperationResult::kDataError;
        return S_FALSE;
      }
      LzmaDec_Init(&_lzma);
      _lzmaInit = true;
      break;
    }
    case kComp_Lzma2:
    {
      Byte prop;
      RINOK(ReadRaw(&prop, 1, got))
      Lzma2Dec_CONSTRUCT(&_lzma2)
      if (got != 1 || Lzma2Dec_Allocate(&_lzma2, prop, &g_Alloc) != SZ_OK)
      {
        OpRes = NExtract::NOperationResult::kDataError;
        return S_FALSE;
      }
      Lzma2Dec_Init(&_lzma2);
      _lzma2Init = true;
      break;
    }
    default:
      OpRes = NExtract::NOperationResult::kUnsupportedMethod;
      return S_FALSE;
  }
  return S_OK;
}

HRESULT CChunkReader::Read(Byte *data, size_t size, size_t &got)
{
  got = 0;
  while (size != 0 && !_end)
  {
    size_t n = 0;
    if (_d->Compression == kComp_Stored)
    {
      RINOK(ReadRaw(data, size, n))
      if (n == 0)
        _end = true;
    }
    else
    {
      RINOK(FillIn())
      const size_t avail = _inLen - _inPos;
      const Byte *in = _in + _inPos;
      size_t used = 0;
      bool finished = false;
      if (_zInit)
      {
        _z.next_in = (Bytef *)in;
        _z.avail_in = (uInt)avail;
        _z.next_out = data;
        _z.avail_out = (uInt)MyMin(size, (size_t)1 << 30);
        const size_t outAvail = _z.avail_out;
        const int r = inflate(&_z, Z_NO_FLUSH);
        used = avail - _z.avail_in;
        n = outAvail - _z.avail_out;
        if (r == Z_STREAM_END) finished = true;
        else if (r != Z_OK && !(r == Z_BUF_ERROR && avail == 0)) { OpRes = NExtract::NOperationResult::kDataError; return S_FALSE; }
      }
      else if (_bzInit)
      {
        _bz.next_in = (char *)in;
        _bz.avail_in = (unsigned)avail;
        _bz.next_out = (char *)data;
        _bz.avail_out = (unsigned)MyMin(size, (size_t)1 << 30);
        const size_t outAvail = _bz.avail_out;
        const int r = BZ2_bzDecompress(&_bz);
        used = avail - _bz.avail_in;
        n = outAvail - _bz.avail_out;
        if (r == BZ_STREAM_END) finished = true;
        else if (r != BZ_OK) { OpRes = NExtract::NOperationResult::kDataError; return S_FALSE; }
      }
      else
      {
        SizeT outLen = size, inLen = avail;
        ELzmaStatus status;
        const SRes r = _lzmaInit
            ? LzmaDec_DecodeToBuf(&_lzma, data, &outLen, in, &inLen, LZMA_FINISH_ANY, &status)
            : Lzma2Dec_DecodeToBuf(&_lzma2, data, &outLen, in, &inLen, LZMA_FINISH_ANY, &status);
        used = inLen;
        n = outLen;
        if (r != SZ_OK) { OpRes = NExtract::NOperationResult::kDataError; return S_FALSE; }
        if (status == LZMA_STATUS_FINISHED_WITH_MARK) finished = true;
      }
      _inPos += used;
      if (finished || (n == 0 && used == 0 && avail == 0))
        _end = true;
    }
    data += n;
    size -= n;
    got += n;
    Pos += n;
  }
  return S_OK;
}

// x86 call addresses (innoextract's exefilter.hpp), over a file's bytes in order
struct CFilter
{
  int Type;
  UInt32 Addr, AddrOffset;
  unsigned AddrLeft;
  UInt32 Offset;
  Byte Pending[4];
  unsigned NumPending;     // 5200: bytes of an address still to come
  void Init(int type) { Type = type; Addr = 0; AddrOffset = 5; AddrLeft = 0; Offset = 0; NumPending = 0; }
  // filters (data) in place; (out) gets the bytes ready, which may lag
  // the input by up to 4 bytes for 5200 / 5309
  void Run(Byte *data, size_t size, CByteBuffer &out, size_t &outLen);
  void Finish(CByteBuffer &out, size_t &outLen)
  {
    // a cut-short address at the end stays as it is: the bytes seen of it
    if (NumPending != 0)
      for (unsigned i = 0; i < 4 - NumPending; i++)
        out[outLen++] = Pending[i];
    NumPending = 0;
  }
};

void CFilter::Run(Byte *data, size_t size, CByteBuffer &out, size_t &outLen)
{
  outLen = 0;
  if (out.Size() < size + 8)
    out.Alloc(size + 8);
  if (Type == kFilter_4108)
  {
    for (size_t i = 0; i < size; i++, AddrOffset++)
    {
      Byte b = data[i];
      if (AddrLeft == 0)
      {
        if (b == 0xE8 || b == 0xE9)
        {
          Addr = ~AddrOffset + 1;
          AddrLeft = 4;
        }
      }
      else
      {
        Addr += b;
        b = (Byte)Addr;
        Addr >>= 8;
        AddrLeft--;
      }
      out[outLen++] = b;
    }
    return;
  }
  const bool flip = (Type == kFilter_5309);
  for (size_t i = 0; i < size; i++)
  {
    const Byte b = data[i];
    Offset++;
    if (NumPending != 0)
    {
      Pending[4 - NumPending] = b;
      if (--NumPending == 0)
      {
        if (Pending[3] == 0x00 || Pending[3] == 0xFF)
        {
          const UInt32 addr = Offset & 0xFFFFFF;
          UInt32 rel = Pending[0] | ((UInt32)Pending[1] << 8) | ((UInt32)Pending[2] << 16);
          rel -= addr;
          Pending[0] = (Byte)rel; Pending[1] = (Byte)(rel >> 8); Pending[2] = (Byte)(rel >> 16);
          if (flip && (rel & 0x800000))
            Pending[3] = (Byte)~Pending[3];
        }
        memcpy((Byte *)out + outLen, Pending, 4);
        outLen += 4;
      }
      continue;
    }
    out[outLen++] = b;
    if (b != 0xE8 && b != 0xE9)
      continue;
    const UInt32 left = 0x10000 - ((Offset - 1) % 0x10000);
    if (left < 5)
      continue;
    NumPending = 4;
  }
  // an address cut by the end of (data) waits in Pending for the rest
}

// ---------- IInArchive ----------

static const Byte kProps[] = { kpidPath, kpidSize, kpidMTime, kpidMethod, kpidEncrypted };
static const Byte kArcProps[] = { kpidComment };
IMP_IInArchive_Props
IMP_IInArchive_ArcProps

Z7_COM7F_IMF(CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPhySize: if (_phySize) prop = _phySize; break;
    case kpidComment: if (!_comment.IsEmpty()) prop = _comment; break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

static const char * const kCompNames[] = { "Copy", "Deflate", "BZip2", "LZMA", "LZMA2", "?" };

Z7_COM7F_IMF(CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  const CItem &item = _items[index];
  const CDataEntry &d = _data[item.Data];
  switch (propID)
  {
    case kpidPath: prop = item.Path; break;
    case kpidSize: prop = d.FileSize; break;
    case kpidMTime: if (d.MTimeDefined) prop = d.MTime; break;
    case kpidEncrypted: prop = d.Encryption != kEnc_None; break;
    case kpidMethod:
    {
      AString s (kCompNames[MyMin(d.Compression, (int)kComp_Unknown)]);
      if (d.Filter != kFilter_None) s += " x86";
      if (d.Encryption != kEnc_None) s += d.Encryption == kEnc_XChaCha20 ? " XChaCha20" : " ARC4";
      prop = s;
      break;
    }
    default: break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

// Inno's file names: "{app}\x" -> "app/x"; ANSI ones in Windows-1252
static UString DecodeString(const CByteBuffer &b, bool unicode)
{
  UString s;
  if (unicode)
  {
    for (size_t i = 0; i + 1 < b.Size(); i += 2)
      s += (wchar_t)GetUi16(b + i);
    return s;
  }
  static const UInt16 k1252[32] =
  {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178
  };
  AString a;
  a.SetFrom((const char *)(const Byte *)b, (unsigned)b.Size());
  if (ConvertUTF8ToUnicode(a, s) && g_LanguageCodepage == 65001)
    return s;
  s.Empty();
  for (size_t i = 0; i < b.Size(); i++)
  {
    const Byte c = b[i];
    s += (wchar_t)((c >= 0x80 && c < 0xA0) ? k1252[c - 0x80] : c);
  }
  return s;
}

static UString MakePath(const UString &dest)
{
  UString r;
  for (unsigned i = 0; i < dest.Len(); i++)
  {
    const wchar_t c = dest[i];
    if (c == L'{')
    {
      if (i + 1 < dest.Len() && dest[i + 1] == L'{')   // "{{" is a brace
      {
        r += L'{';
        i++;
        continue;
      }
      const int end = dest.Find(L'}', i);
      if (end < 0)
        break;
      UString name = dest.Mid(i + 1, (unsigned)end - i - 1);
      const int colon = name.Find(L':');
      if (colon >= 0)
        name.DeleteFrom((unsigned)colon);
      if (name.IsPrefixedBy(L"#") || name.IsPrefixedBy(L"%"))
        name.DeleteFrontal(1);
      r += name;
      i = (unsigned)end;
      continue;
    }
    r += (c == L'\\') ? L'/' : c;
  }
  while (r.IsPrefixedBy(L"/"))
    r.DeleteFrontal(1);
  return r;
}

HRESULT CHandler::GetPassword(IUnknown *cb)
{
  if (_passwordDefined)
    return S_OK;
  Z7_DECL_CMyComPtr_QI_FROM(ICryptoGetTextPassword, getPassword, cb)
  if (!getPassword)
    return S_FALSE;
  for (int attempt = 0; attempt < 3; attempt++)
  {
    CMyComBSTR bstr;
    RINOK(getPassword->CryptoGetTextPassword(&bstr))
    const UString u = bstr ? (const wchar_t *)bstr : L"";
    AString pw;
    if (_ver.Unicode)
      for (unsigned i = 0; i < u.Len(); i++)
      {
        pw += (char)(Byte)u[i];
        pw += (char)(Byte)(u[i] >> 8);
      }
    else
      pw = UnicodeStringToMultiByte(u, CP_ACP);
    if (CheckPassword(pw))
    {
      _password = pw;
      _passwordDefined = true;
      return S_OK;
    }
    break;
  }
  return S_FALSE;
}

bool CHandler::CheckPassword(const AString &pw) const
{
  const Byte *p = (const Byte *)pw.Ptr();
  if (_header.PasswordType == kSum_Sha1)
  {
    CSha1 s;
    Sha1_Init(&s);
    Sha1_Update(&s, (const Byte *)"PasswordCheckHash", 17);
    Sha1_Update(&s, _header.Salt, _header.Salt.Size());
    Sha1_Update(&s, p, pw.Len());
    Byte d[20];
    Sha1_Final(&s, d);
    return memcmp(d, _header.PasswordHash, 20) == 0;
  }
  if (_header.PasswordType == kSum_Md5)
  {
    CMd5 s;
    Md5_Init(&s);
    if (_header.Salt.Size() != 0)
    {
      Md5_Update(&s, (const Byte *)"PasswordCheckHash", 17);
      Md5_Update(&s, _header.Salt, _header.Salt.Size());
    }
    Md5_Update(&s, p, pw.Len());
    Byte d[16];
    Md5_Final(&s, d);
    return memcmp(d, _header.PasswordHash, 16) == 0;
  }
  if (_header.PasswordType == kSum_Crc32)
  {
    const UInt32 c = CrcCalc(p, pw.Len());
    return memcmp(&c, _header.PasswordHash, 4) == 0;
  }
  return false;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  UInt64 sigPos = 0;
  RINOK(InStream_GetPos(stream, sigPos))
  UInt64 headerOffset = 0, dataOffset = 0;
  if (!LoadOffsets(stream, headerOffset, dataOffset))
  {
    // setup-0 alone, where it was found
    headerOffset = sigPos;
    dataOffset = 0;
  }
  Byte vs[64];
  RINOK(ReadAt(stream, headerOffset, vs, 64))
  CVersion v;
  if (!ParseVersion(vs, 64, v))
    return S_FALSE;
  const bool legacy = (vs[0] == 'i' && vs[11] == 0x1A);
  for (;;)
  {
    _ver = v;
    UInt64 pos = headerOffset + (legacy ? 12 : 64);
    CByteBuffer b1, b2;
    g_LanguageCodepage = 1252;
    bool ok = ReadBlock(stream, pos, v, b1) == S_OK && ReadBlock(stream, pos, v, b2) == S_OK;
    CObjectVector<CFileEntry> files;
    _data.Clear();
    if (ok)
    {
      CR r(b1, b1.Size(), _ver);
      ok = ReadHeader(r, _header);
      for (UInt32 i = 0; ok && i < _header.Languages; i++)
        SkipLanguage(r);
      ok = ok && !r.Error && ReadEntries(r, _header, files);
      if (ok && _ver >= V(4, 0, 0))
        SkipWizard(r, _header);
      ok = ok && !r.Error && r.Pos == r.Size;
      CR r2(b2, b2.Size(), _ver);
      for (UInt32 i = 0; ok && i < _header.DataEntries; i++)
      {
        CDataEntry d;
        ok = ReadDataEntry(r2, _header, d);
        _data.Add(d);
      }
      ok = ok && r2.Pos == r2.Size;
    }
    if (ok)
    {
      _phySize = pos;
      // the items: files with data, named after their destination
      _items.Clear();
      UStringVector paths;
      FOR_VECTOR (i, files)
      {
        const CFileEntry &f = files[i];
        if (f.Location >= _data.Size())
          continue;
        CItem item;
        item.Data = f.Location;
        item.Path = MakePath(DecodeString(f.Destination, _ver.Unicode));
        // the same name again (per language or component): mark which
        if (paths.FindInSorted(item.Path) >= 0)
        {
          UString tag = DecodeString(f.Languages, _ver.Unicode);
          if (tag.IsEmpty())
            tag = DecodeString(f.Components, _ver.Unicode);
          UString p = item.Path;
          const int dot = p.ReverseFind_Dot();
          const int slash = p.ReverseFind(L'/');
          const unsigned at = (dot > slash) ? (unsigned)dot : p.Len();
          UString suffix = UString(L"@") + (tag.IsEmpty() ? UString(L"2") : tag);
          suffix.Replace(L'/', L'_');
          suffix.Replace(L' ', L'_');
          p.Insert(at, suffix);
          for (unsigned k = 3; paths.FindInSorted(p) >= 0 && k < 1000; k++)
          {
            p = item.Path;
            UString s2 = suffix;
            s2 += L'_';
            s2.Add_UInt32(k);
            p.Insert(at, s2);
          }
          item.Path = p;
        }
        paths.AddToUniqueSorted(item.Path);
        _items.Add(item);
      }
      break;
    }
    UInt32 next = v.Value;
    if (!NextAmbiguous(next))
      return S_FALSE;
    v.Value = next;
  }

  _stream = stream;
  _dataOffset = dataOffset;
  _missingSlices = 0;
  if (dataOffset == 0)
  {
    UString name = NItemArc::GetArcName(callback);
    const int dot = name.ReverseFind_Dot();
    if (dot > 0)
      name.DeleteFrom((unsigned)dot);
    _sliceBase = name.IsEmpty() ? UString(L"setup") : name;
    RINOK(OpenSlices(callback))
  }
  _passwordDefined = false;
  if (_header.EncryptionUsed || _header.PasswordUsed)
  {
    const HRESULT r = GetPassword(callback);
    if (r == E_ABORT)
      return E_ABORT;
  }
  AString c ("Inno Setup ");
  c.Add_UInt32(_ver.Value >> 24); c.Add_Dot();
  c.Add_UInt32((_ver.Value >> 16) & 0xFF); c.Add_Dot();
  c.Add_UInt32((_ver.Value >> 8) & 0xFF);
  if (_ver.Value & 0xFF) { c.Add_Dot(); c.Add_UInt32(_ver.Value & 0xFF); }
  if (_ver.Unicode) c += " (Unicode)";
  _comment = GetUnicodeString(c);
  return S_OK;
}

Z7_COM7F_IMF(CHandler::Open(IInStream *stream, const UInt64 *, IArchiveOpenCallback *callback))
{
  COM_TRY_BEGIN
  Close();
  const HRESULT res = Open2(stream, callback);
  if (res != S_OK)
    Close();
  return res;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::Close())
{
  _items.Clear();
  _data.Clear();
  _slices.Clear();
  _stream.Release();
  _phySize = 0;
  _passwordDefined = false;
  _comment.Empty();
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _items.Size();
  return S_OK;
}

// the checksum of a file as it is read
struct CSum
{
  int Type;
  UInt32 Crc;
  uLong Adler;
  CMd5 Md5;
  CSha1 Sha1;
  CSha256 Sha256;
  void Init(int type)
  {
    Type = type;
    Crc = CRC_INIT_VAL;
    Adler = adler32(0, NULL, 0);
    if (type == kSum_Md5) Md5_Init(&Md5);
    if (type == kSum_Sha1) Sha1_Init(&Sha1);
    if (type == kSum_Sha256) Sha256_Init(&Sha256);
  }
  void Update(const Byte *p, size_t n)
  {
    switch (Type)
    {
      case kSum_Crc32: Crc = CrcUpdate(Crc, p, n); break;
      case kSum_Adler32: Adler = adler32(Adler, p, (uInt)n); break;
      case kSum_Md5: Md5_Update(&Md5, p, n); break;
      case kSum_Sha1: Sha1_Update(&Sha1, p, n); break;
      case kSum_Sha256: Sha256_Update(&Sha256, p, n); break;
      default: break;
    }
  }
  bool Check(const Byte *want)
  {
    Byte d[32];
    switch (Type)
    {
      case kSum_Crc32: { const UInt32 c = CRC_GET_DIGEST(Crc); return memcmp(&c, want, 4) == 0; }
      case kSum_Adler32: { const UInt32 a = (UInt32)Adler; return memcmp(&a, want, 4) == 0; }
      case kSum_Md5: Md5_Final(&Md5, d); return memcmp(d, want, 16) == 0;
      case kSum_Sha1: Sha1_Final(&Sha1, d); return memcmp(d, want, 20) == 0;
      case kSum_Sha256: Sha256_Final(&Sha256, d); return memcmp(d, want, 32) == 0;
      default: return true;
    }
  }
};

// decodes the file of (d) from (reader), which is open on its chunk at or
// before it, into (out) (may be NULL); the operation result
static HRESULT CopyFile(CChunkReader &reader, const CDataEntry &d, ISequentialOutStream *out,
    CLocalProgress *lps, UInt64 doneBefore, Int32 &opRes)
{
  opRes = NExtract::NOperationResult::kOK;
  const size_t kBuf = 1 << 18;
  CByteBuffer buf(kBuf), filtered;
  // skip to the file
  while (reader.Pos < d.FileOffset)
  {
    size_t got;
    const HRESULT r = reader.Read(buf, (size_t)MyMin((UInt64)kBuf, d.FileOffset - reader.Pos), got);
    if (r != S_OK || got == 0)
    {
      opRes = reader.OpRes ? reader.OpRes : NExtract::NOperationResult::kUnexpectedEnd;
      return r == S_FALSE ? S_OK : r;
    }
  }
  CFilter filter;
  filter.Init(d.Filter);
  CSum sum;
  sum.Init(d.SumType);
  UInt64 left = d.FileSize;
  while (left != 0)
  {
    size_t got;
    const HRESULT r = reader.Read(buf, (size_t)MyMin((UInt64)kBuf, left), got);
    if (r != S_OK || got == 0)
    {
      opRes = reader.OpRes ? reader.OpRes : NExtract::NOperationResult::kUnexpectedEnd;
      return r == S_FALSE ? S_OK : r;
    }
    left -= got;
    const Byte *p = buf;
    size_t n = got;
    if (d.Filter != kFilter_None)
    {
      filter.Run(buf, got, filtered, n);
      if (left == 0)
        filter.Finish(filtered, n);
      p = filtered;
    }
    sum.Update(p, n);
    if (out)
      RINOK(WriteStream(out, p, n))
    if (lps)
    {
      lps->InSize = lps->OutSize = doneBefore + (d.FileSize - left);
      RINOK(lps->SetCur())
    }
  }
  if (!sum.Check(d.Sum))
    opRes = NExtract::NOperationResult::kCRCError;
  return S_OK;
}

Z7_COM7F_IMF(CHandler::Extract(const UInt32 *indices, UInt32 numItems,
    Int32 testMode, IArchiveExtractCallback *extractCallback))
{
  COM_TRY_BEGIN
  const bool allFilesMode = (numItems == (UInt32)(Int32)-1);
  if (allFilesMode)
    numItems = _items.Size();
  if (numItems == 0)
    return S_OK;
  // in chunk order, each chunk read once
  CRecordVector<UInt32> order;
  UInt64 total = 0;
  for (UInt32 i = 0; i < numItems; i++)
  {
    const UInt32 index = allFilesMode ? i : indices[i];
    order.Add(index);
    total += _data[_items[index].Data].FileSize;
  }
  std::sort(&order[0], &order[0] + order.Size(), [this](UInt32 a, UInt32 b)
  {
    const CDataEntry &x = _data[_items[a].Data], &y = _data[_items[b].Data];
    if (x.FirstSlice != y.FirstSlice) return x.FirstSlice < y.FirstSlice;
    if (x.ChunkOffset != y.ChunkOffset) return x.ChunkOffset < y.ChunkOffset;
    return x.FileOffset < y.FileOffset;
  });
  RINOK(extractCallback->SetTotal(total))
  if (!_passwordDefined)
  {
    bool encrypted = false;
    FOR_VECTOR (i, order)
      if (_data[_items[order[i]].Data].Encryption != kEnc_None)
        encrypted = true;
    if (encrypted)
    {
      const HRESULT r = GetPassword(extractCallback);
      if (r == E_ABORT)
        return r;
    }
  }
  CMyComPtr2_Create<ICompressProgressInfo, CLocalProgress> lps;
  lps->Init(extractCallback, false);
  CChunkReader reader(this);
  const CDataEntry *open = NULL;
  UInt64 done = 0;
  FOR_VECTOR (i, order)
  {
    const UInt32 index = order[i];
    const CDataEntry &d = _data[_items[index].Data];
    CMyComPtr<ISequentialOutStream> out;
    const Int32 askMode = testMode ? NExtract::NAskMode::kTest : NExtract::NAskMode::kExtract;
    RINOK(extractCallback->GetStream(index, &out, askMode))
    if (!testMode && !out)
    {
      done += d.FileSize;
      continue;
    }
    RINOK(extractCallback->PrepareOperation(askMode))
    // the same chunk, and not behind: go on; else open it again
    const bool same = open && open->FirstSlice == d.FirstSlice && open->ChunkOffset == d.ChunkOffset
        && reader.Pos <= d.FileOffset && reader.OpRes == 0;
    Int32 opRes = NExtract::NOperationResult::kOK;
    if (!same)
    {
      open = &d;
      const HRESULT r = reader.Open(d);
      if (r != S_OK)
      {
        if (r != S_FALSE)
          return r;
        opRes = reader.OpRes;
        open = NULL;
      }
    }
    if (opRes == NExtract::NOperationResult::kOK)
    {
      RINOK(CopyFile(reader, d, out, lps.ClsPtr(), done, opRes))
      if (opRes != NExtract::NOperationResult::kOK)
        open = NULL;
    }
    done += d.FileSize;
    out.Release();
    RINOK(extractCallback->SetOperationResult(opRes))
  }
  lps->InSize = lps->OutSize = done;
  return lps->SetCur();
  COM_TRY_END
}

// in place: the file decoded into memory (up to 256 MB)
Z7_COM7F_IMF(CHandler::GetStream(UInt32 index, ISequentialInStream **stream))
{
  COM_TRY_BEGIN
  *stream = NULL;
  if (index >= _items.Size())
    return S_FALSE;
  const CDataEntry &d = _data[_items[index].Data];
  if (d.FileSize > ((UInt64)1 << 28) || d.Encryption != kEnc_None)
    return S_FALSE;
  CChunkReader reader(this);
  if (reader.Open(d) != S_OK)
    return S_FALSE;
  CBufPtrSeqOutStream *spec = new CBufPtrSeqOutStream;
  CMyComPtr<ISequentialOutStream> outRef = spec;
  CReferenceBuf *ref = new CReferenceBuf;
  CMyComPtr<IUnknown> refRef = ref;
  ref->Buf.Alloc((size_t)d.FileSize);
  spec->Init(ref->Buf, ref->Buf.Size());
  Int32 opRes;
  RINOK(CopyFile(reader, d, outRef, NULL, 0, opRes))
  if (opRes != NExtract::NOperationResult::kOK || spec->GetPos() != d.FileSize)
    return S_FALSE;
  CBufInStream *in = new CBufInStream;
  CMyComPtr<ISequentialInStream> inRef = in;
  in->Init(ref);
  *stream = inRef.Detach();
  return S_OK;
  COM_TRY_END
}

static const Byte k_Signature[] =
  { 'I', 'n', 'n', 'o', ' ', 'S', 'e', 't', 'u', 'p', ' ', 'S', 'e', 't', 'u', 'p', ' ', 'D', 'a', 't', 'a', ' ', '(' };

REGISTER_ARC_I(
  "Inno", "exe", NULL, 0xA4,
  k_Signature,
  0,
  NArcInfoFlags::kFindSignature | NArcInfoFlags::kUseGlobalOffset,
  NULL)

}}
