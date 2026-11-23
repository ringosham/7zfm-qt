// UnrealPak.cpp -- 7zfm-qt: Unreal Engine 3-5 .pak files, after repak
// (MIT / Apache-2.0 licenses):
//
//   At the end, a footer: from version 7 the encryption key's GUID, from 4
//   an "index encrypted" byte, the magic 0x5A6F12E1, the version, the
//   index's offset and size, its SHA-1, a "frozen" byte (version 9), and
//   from 8 the names of the compression methods used ("Zlib", "Oodle"...).
//   The index: the mount point, the entry count, then up to version 9 each
//   file's path and record; from 10 a path hash seed, an index of path
//   hashes and a full directory index (directories, their files, and where
//   each file's record is), then the records, most bit-packed ("encoded").
//   A record: offset, sizes, compression, SHA-1, the compression blocks,
//   flags (bit 0 encrypted), block size. The data has a copy of its record
//   in front.
//
// Encrypted indexes and files are AES-256 (ECB); the key is asked, as 64
// hex digits or its base64. Oodle blocks are decoded by Oodle.cpp.
// IoStore containers (.utoc/.ucas) are a different format.

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"
#include "Common/UTFConvert.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "C/Aes.h"

#include "../common/ItemArchive.h"

#include "Oodle.h"

#include <string.h>

namespace NArchive {
namespace NUnrealPak {

using namespace NItemArc;

static const UInt32 kMagic = 0x5A6F12E1;

enum ECompression { kComp_None, kComp_Zlib, kComp_Gzip, kComp_Oodle, kComp_Zstd, kComp_Lz4, kComp_Unknown };

// what a piece is: compression and whether it is encrypted
static const unsigned kMethod_Pak = kMethod_Custom;

// the versions, newest first, and their footer sizes (repak's Version)
struct CVersionInfo { UInt32 Major; unsigned CompNames; };
static const CVersionInfo kVersions[] =
{
  { 11, 5 }, { 10, 5 }, { 9, 5 }, { 8, 5 }, { 8, 4 }, { 7, 0 }, { 6, 0 }, { 5, 0 },
  { 4, 0 }, { 3, 0 }, { 2, 0 }, { 1, 0 }
};

static unsigned FooterSize(const CVersionInfo &v)
{
  unsigned size = 4 + 4 + 8 + 8 + 20;
  if (v.Major >= 7) size += 16;
  if (v.Major >= 4) size += 1;
  if (v.Major == 9) size += 1;
  size += 32 * v.CompNames;
  return size;
}

class CHandler Z7_final: public CHandlerBase
{
  CByteBuffer _key;
  bool _keyDefined;
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
  void Close2() Z7_override { _keyDefined = false; }
  HRESULT GetKey(IArchiveOpenCallback *callback);
public:
  CHandler(): _keyDefined(false) {}
  bool Decrypt(Byte *data, size_t size) const;
  HRESULT DecodePiece(const CItem &item, const CPiece &piece, CByteBuffer &packed, Byte *dest) Z7_override;
};

static int HexValue(wchar_t c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int Base64Value(wchar_t c)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

HRESULT CHandler::GetKey(IArchiveOpenCallback *callback)
{
  if (_keyDefined)
    return S_OK;
  UString s;
  RINOK(AskPassword(callback, s))
  s.RemoveChar(L' ');
  if (s.IsPrefixedBy(L"0x") || s.IsPrefixedBy(L"0X"))
    s.DeleteFrontal(2);
  _key.Alloc(32);
  if (s.Len() == 64)
  {
    for (unsigned i = 0; i < 32; i++)
    {
      const int hi = HexValue(s[i * 2]), lo = HexValue(s[i * 2 + 1]);
      if (hi < 0 || lo < 0)
        return S_FALSE;
      _key[i] = (Byte)(hi * 16 + lo);
    }
  }
  else if (s.Len() == 44 && s.Back() == L'=')
  {
    UInt32 acc = 0;
    unsigned bits = 0, n = 0;
    for (unsigned i = 0; i < 43; i++)
    {
      const int v = Base64Value(s[i]);
      if (v < 0)
        return S_FALSE;
      acc = (acc << 6) | (UInt32)v;
      bits += 6;
      if (bits >= 8)
      {
        bits -= 8;
        if (n < 32)
          _key[n++] = (Byte)(acc >> bits);
      }
    }
    if (n != 32)
      return S_FALSE;
  }
  else
    return S_FALSE;
  _keyDefined = true;
  return S_OK;
}

// AES-256 ECB: CBC decoding with a zero IV, then each block's predecessor
// XORed back out
bool CHandler::Decrypt(Byte *data, size_t size) const
{
  if (!_keyDefined)
    return false;
  size &= ~(size_t)15;
  if (size == 0)
    return true;
  CByteBuffer cipher(size);
  memcpy(cipher, data, size);
  UInt32 aesBuf[AES_NUM_IVMRK_WORDS + 3];
  UInt32 *aes = (UInt32 *)(((size_t)aesBuf + 15) & ~(size_t)15);
  Aes_SetKey_Dec(aes + 4, _key, 32);
  Byte iv[16];
  memset(iv, 0, 16);
  AesCbc_Init(aes, iv);
  AesCbc_Decode(aes, data, size / 16);
  for (size_t i = 16; i < size; i++)
    data[i] ^= cipher[i - 16];
  return true;
}

HRESULT CHandler::DecodePiece(const CItem &item, const CPiece &piece, CByteBuffer &packed, Byte *dest)
{
  if (piece.Method != kMethod_Pak)
    return CHandlerBase::DecodePiece(item, piece, packed, dest);
  const unsigned comp = (unsigned)(piece.Aux & 0xFF);
  const bool encrypted = (piece.Aux & 0x100) != 0;
  if (encrypted && !Decrypt(packed, packed.Size()))
    return k_Hr_WrongPassword;
  const size_t inSize = (size_t)(piece.Aux >> 16);   // without the AES padding
  const Byte *src = packed;
  const size_t srcSize = MyMin(inSize, packed.Size());
  switch (comp)
  {
    case kComp_None: return DecodeMethod(kMethod_Stored, 0, src, srcSize, dest, (size_t)piece.Size);
    case kComp_Zlib:
    {
      // zlib, or gzip when the header says (some games mislabel)
      if (srcSize >= 2 && src[0] == 0x1F && src[1] == 0x8B)
        break;
      return DecodeMethod(kMethod_Zlib, 0, src, srcSize, dest, (size_t)piece.Size);
    }
    case kComp_Gzip: break;
    case kComp_Zstd: return DecodeMethod(kMethod_Zstd, 0, src, srcSize, dest, (size_t)piece.Size);
    case kComp_Lz4: return DecodeMethod(kMethod_Lz4Block, 0, src, srcSize, dest, (size_t)piece.Size);
    case kComp_Oodle: return NOodle::Decode(src, srcSize, dest, (size_t)piece.Size) ? S_OK : k_Hr_DataError;
    default: return k_Hr_Unsupported;
  }
  // gzip: zlib's inflate with windowBits 31; strip the 10-byte header ourselves
  if (srcSize < 18 || src[0] != 0x1F || src[1] != 0x8B || src[2] != 8)
    return k_Hr_DataError;
  size_t pos = 10;
  const Byte flg = src[3];
  if (flg & 4) { if (srcSize < pos + 2) return k_Hr_DataError; pos += 2 + GetUi16(src + pos); }
  if (flg & 8) { while (pos < srcSize && src[pos]) pos++; pos++; }
  if (flg & 16) { while (pos < srcSize && src[pos]) pos++; pos++; }
  if (flg & 2) pos += 2;
  if (pos >= srcSize)
    return k_Hr_DataError;
  return DecodeMethod(kMethod_Deflate, 0, src + pos, srcSize - pos, dest, (size_t)piece.Size);
}

// ---------- reading the index ----------

struct CBuf
{
  const Byte *P;
  size_t Size, Pos;
  bool Error;
  CBuf(const Byte *p, size_t size): P(p), Size(size), Pos(0), Error(false) {}
  bool Need(size_t n) { if (Size - Pos < n) { Error = true; return false; } return true; }
  UInt32 U32() { if (!Need(4)) return 0; const UInt32 v = GetUi32(P + Pos); Pos += 4; return v; }
  UInt64 U64() { if (!Need(8)) return 0; const UInt64 v = GetUi64(P + Pos); Pos += 8; return v; }
  Byte U8() { if (!Need(1)) return 0; return P[Pos++]; }
  void Skip(size_t n) { if (Need(n)) Pos += n; }
  // an FString: length with the terminator, negative for UTF-16
  bool Str(UString &s)
  {
    s.Empty();
    const Int32 len = (Int32)U32();
    if (Error)
      return false;
    if (len == 0)
      return true;
    if (len > 0)
    {
      if (len > 65536 || !Need((size_t)len))
        return false;
      AString a;
      a.SetFrom_CalcLen((const char *)P + Pos, (unsigned)len);
      Pos += (size_t)len;
      if (!ConvertUTF8ToUnicode(a, s))
        s = GetUnicodeString(a);
      return true;
    }
    const size_t n = (size_t)(-(Int64)len);
    if (n > 65536 || !Need(n * 2))
      return false;
    for (size_t i = 0; i < n; i++)
    {
      const wchar_t c = (wchar_t)GetUi16(P + Pos + i * 2);
      if (c == 0)
        break;
      s += c;
    }
    Pos += n * 2;
    return true;
  }
};

struct CEntry
{
  UInt64 Offset;
  UInt64 Compressed;
  UInt64 Uncompressed;
  UInt32 Slot;          // 0: none, else 1-based into the footer's methods
  bool Encrypted;
  UInt32 BlockSize;
  CRecordVector<UInt64> BlockStart, BlockEnd;
  UInt64 Timestamp;
};

// the size of a record as written in front of a file's data
static UInt64 RecordSize(UInt32 major, bool v8a, UInt32 slot, unsigned numBlocks)
{
  UInt64 size = 8 + 8 + 8 + (v8a ? 1 : 4) + (major == 1 ? 8 : 0) + 20;
  if (slot != 0)
    size += 4 + 16 * (UInt64)numBlocks;
  if (major >= 3)
    size += 1 + 4;
  return size;
}

static bool ReadEntry(CBuf &b, UInt32 major, bool v8a, CEntry &e)
{
  e.Offset = b.U64();
  e.Compressed = b.U64();
  e.Uncompressed = b.U64();
  e.Slot = v8a ? b.U8() : b.U32();
  e.Timestamp = (major == 1) ? b.U64() : 0;
  b.Skip(20);
  e.Encrypted = false;
  e.BlockSize = 0;
  if (major >= 3)
  {
    if (e.Slot != 0)
    {
      const UInt32 n = b.U32();
      if (n > (1 << 24))
        return false;
      for (UInt32 i = 0; i < n && !b.Error; i++)
      {
        e.BlockStart.Add(b.U64());
        e.BlockEnd.Add(b.U64());
      }
    }
    e.Encrypted = (b.U8() & 1) != 0;
    e.BlockSize = b.U32();
  }
  return !b.Error;
}

static bool ReadEncodedEntry(CBuf &b, UInt32 major, CEntry &e)
{
  const UInt32 bits = b.U32();
  e.Slot = (bits >> 23) & 0x3F;
  e.Encrypted = (bits & (1 << 22)) != 0;
  const UInt32 numBlocks = (bits >> 6) & 0xFFFF;
  UInt32 blockSize = bits & 0x3F;
  e.BlockSize = (blockSize == 0x3F) ? b.U32() : (blockSize << 11);
  e.Offset = (bits & (1u << 31)) ? b.U32() : b.U64();
  e.Uncompressed = (bits & (1 << 30)) ? b.U32() : b.U64();
  e.Compressed = (e.Slot == 0) ? e.Uncompressed : ((bits & (1 << 29)) ? b.U32() : b.U64());
  e.Timestamp = 0;
  UInt64 base = RecordSize(major, false, e.Slot, numBlocks);
  if (numBlocks == 1 && !e.Encrypted)
  {
    e.BlockStart.Add(base);
    e.BlockEnd.Add(base + e.Compressed);
  }
  else
    for (UInt32 i = 0; i < numBlocks && !b.Error; i++)
    {
      UInt64 size = b.U32();
      e.BlockStart.Add(base);
      e.BlockEnd.Add(base + size);
      if (e.Encrypted)
        size = (size + 15) & ~(UInt64)15;
      base += size;
    }
  return !b.Error;
}

static UString CleanMount(const UString &mount)
{
  UString m = mount;
  m.Replace(L'\\', L'/');
  while (m.IsPrefixedBy(L"../"))
    m.DeleteFrontal(3);
  while (m.IsPrefixedBy(L"/"))
    m.DeleteFrontal(1);
  if (!m.IsEmpty() && m.Back() != L'/')
    m += L'/';
  return m;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))

  // the footer: try each version's size, newest first
  const unsigned kMaxFooter = 4 + 4 + 8 + 8 + 20 + 16 + 1 + 1 + 32 * 5;
  if (fileSize < 44)
    return S_FALSE;
  const size_t tailSize = (size_t)MyMin(fileSize, (UInt64)kMaxFooter);
  Byte tail[kMaxFooter];
  RINOK(ReadAt(stream, fileSize - tailSize, tail, tailSize))
  const CVersionInfo *ver = NULL;
  const Byte *f = NULL;
  for (unsigned i = 0; i < Z7_ARRAY_SIZE(kVersions); i++)
  {
    const unsigned size = FooterSize(kVersions[i]);
    if (size > tailSize)
      continue;
    const Byte *p = tail + tailSize - size;
    unsigned pos = (kVersions[i].Major >= 7 ? 16 : 0) + (kVersions[i].Major >= 4 ? 1 : 0);
    if (GetUi32(p + pos) == kMagic && GetUi32(p + pos + 4) == kVersions[i].Major)
    {
      ver = &kVersions[i];
      f = p;
      break;
    }
  }
  if (!ver)
    return S_FALSE;
  const UInt32 major = ver->Major;
  const bool v8a = (major == 8 && ver->CompNames == 4);
  unsigned pos = (major >= 7) ? 16 : 0;
  const bool indexEncrypted = (major >= 4) && f[pos++] != 0;
  pos += 8;
  const UInt64 indexOffset = GetUi64(f + pos);
  const UInt64 indexSize = GetUi64(f + pos + 8);
  pos += 16 + 20 + (major == 9 ? 1 : 0);
  // the compression methods by slot
  int methods[8];
  unsigned numMethods = 0;
  if (major >= 8)
    for (unsigned i = 0; i < ver->CompNames; i++)
    {
      char name[33];
      memcpy(name, f + pos + 32 * i, 32);
      name[32] = 0;
      int m = kComp_Unknown;
      if (name[0] == 0) m = kComp_Unknown;
      else if (StringsAreEqualNoCase_Ascii(name, "Zlib")) m = kComp_Zlib;
      else if (StringsAreEqualNoCase_Ascii(name, "Gzip")) m = kComp_Gzip;
      else if (StringsAreEqualNoCase_Ascii(name, "Oodle")) m = kComp_Oodle;
      else if (StringsAreEqualNoCase_Ascii(name, "Zstd")) m = kComp_Zstd;
      else if (StringsAreEqualNoCase_Ascii(name, "LZ4")) m = kComp_Lz4;
      methods[numMethods++] = m;
    }
  else
  {
    // older versions: flags 1 zlib, 2 gzip, 4 custom (Oodle)
    methods[0] = kComp_Zlib; methods[1] = kComp_Gzip; methods[2] = kComp_Unknown; methods[3] = kComp_Oodle;
    numMethods = 4;
  }

  if (indexOffset > fileSize || indexSize > fileSize - indexOffset || indexSize > ((UInt64)1 << 30))
    return S_FALSE;
  CByteBuffer index((size_t)indexSize);
  RINOK(ReadAt(stream, indexOffset, index, index.Size()))
  if (indexEncrypted)
  {
    const HRESULT k = GetKey(callback);
    if (k != S_OK)
      return k == E_ABORT ? E_ABORT : S_FALSE;
    Decrypt(index, index.Size());
  }
  CBuf b(index, index.Size());
  UString mount;
  if (!b.Str(mount) || mount.Len() > 1024)
  {
    _keyDefined = false;
    return S_FALSE;
  }
  const UString prefix = CleanMount(mount);
  const UInt32 count = b.U32();
  if (b.Error || count > (1 << 24))
    return S_FALSE;

  CObjectVector<CEntry> entries;
  UStringVector paths;
  if (major < 10)
  {
    for (UInt32 i = 0; i < count; i++)
    {
      UString path;
      CEntry e;
      if (!b.Str(path) || !ReadEntry(b, major, v8a, e))
        return S_FALSE;
      paths.Add(path);
      entries.Add(e);
    }
  }
  else
  {
    b.U64();                                // path hash seed
    if (b.U32() != 0)
      b.Skip(8 + 8 + 20);                   // path hash index: not needed with the full one
    CByteBuffer fdi;
    if (b.U32() != 0)
    {
      const UInt64 off = b.U64();
      const UInt64 size = b.U64();
      b.Skip(20);
      if (off > fileSize || size > fileSize - off || size > ((UInt64)1 << 30))
        return S_FALSE;
      fdi.Alloc((size_t)size);
      RINOK(ReadAt(stream, off, fdi, fdi.Size()))
      if (indexEncrypted)
        Decrypt(fdi, fdi.Size());
    }
    else
      return S_FALSE;                       // no names without it
    const UInt32 encSize = b.U32();
    if (b.Error || !b.Need(encSize))
      return S_FALSE;
    CBuf encoded(b.P + b.Pos, encSize);
    b.Pos += encSize;
    const UInt32 numPlain = b.U32();
    CObjectVector<CEntry> plain;
    for (UInt32 i = 0; i < numPlain && !b.Error && i < (1 << 24); i++)
    {
      CEntry e;
      if (!ReadEntry(b, major, false, e))
        return S_FALSE;
      plain.Add(e);
    }
    CBuf d(fdi, fdi.Size());
    const UInt32 numDirs = d.U32();
    for (UInt32 i = 0; i < numDirs && !d.Error; i++)
    {
      UString dir;
      if (!d.Str(dir))
        return S_FALSE;
      while (dir.IsPrefixedBy(L"/"))
        dir.DeleteFrontal(1);
      const UInt32 numFiles = d.U32();
      for (UInt32 k = 0; k < numFiles && !d.Error; k++)
      {
        UString name;
        if (!d.Str(name))
          return S_FALSE;
        const Int32 at = (Int32)d.U32();
        if (at == (Int32)0x80000000)
          continue;
        CEntry e;
        if (at >= 0)
        {
          encoded.Pos = (size_t)at;
          encoded.Error = false;
          if (!ReadEncodedEntry(encoded, major, e))
            return S_FALSE;
        }
        else
        {
          const UInt32 idx = (UInt32)(-(Int64)at) - 1;
          if (idx >= plain.Size())
            return S_FALSE;
          e = plain[idx];
        }
        paths.Add(dir + name);
        entries.Add(e);
      }
    }
    if (d.Error)
      return S_FALSE;
  }

  bool anyEncrypted = false;
  FOR_VECTOR (i, entries)
  {
    const CEntry &e = entries[i];
    CItem item;
    UString p = paths[i];
    p.Replace(L'\\', L'/');
    item.Path = prefix + p;
    if (major == 1 && e.Timestamp != 0)
    {
      item.MTime.dwLowDateTime = (DWORD)e.Timestamp;
      item.MTime.dwHighDateTime = (DWORD)(e.Timestamp >> 32);
      item.MTime_Defined = true;
    }
    int comp = kComp_None;
    if (e.Slot != 0)
      comp = (e.Slot - 1 < numMethods) ? methods[e.Slot - 1] : kComp_Unknown;
    if (e.Encrypted)
    {
      item.Encrypted = true;
      anyEncrypted = true;
    }
    const UInt64 dataStart = e.Offset + RecordSize(major, v8a, e.Slot, e.BlockStart.Size());
    if (comp == kComp_None)
    {
      if (!e.Encrypted)
        item.SetStored(dataStart, e.Uncompressed);
      else
        item.AddPiece(dataStart, (e.Uncompressed + 15) & ~(UInt64)15, e.Uncompressed, kMethod_Pak,
            0x100 | ((e.Uncompressed) << 16));
    }
    else
    {
      // blocks: relative to the record from version 5, absolute before
      UInt64 left = e.Uncompressed;
      FOR_VECTOR (k, e.BlockStart)
      {
        const UInt64 start = (major >= 5) ? e.Offset + e.BlockStart[k] : e.BlockStart[k];
        const UInt64 inSize = e.BlockEnd[k] - e.BlockStart[k];
        const UInt64 packSize = e.Encrypted ? ((inSize + 15) & ~(UInt64)15) : inSize;
        const UInt64 outSize = (e.BlockSize != 0) ? MyMin(left, (UInt64)e.BlockSize) : left;
        item.AddPiece(start, packSize, outSize, kMethod_Pak,
            (UInt64)comp | (e.Encrypted ? 0x100 : 0) | (inSize << 16));
        left -= outSize;
      }
      if (left != 0)
        _errorFlags |= kpv_ErrorFlags_HeadersError;
      switch (comp)
      {
        case kComp_Zlib: item.Method = "Zlib"; break;
        case kComp_Gzip: item.Method = "Gzip"; break;
        case kComp_Oodle: item.Method = "Oodle"; break;
        case kComp_Zstd: item.Method = "Zstd"; break;
        case kComp_Lz4: item.Method = "LZ4"; break;
        default: item.Method = "Unknown"; break;
      }
    }
    if (e.Encrypted)
      item.Method += item.Method.IsEmpty() ? "AES-256" : " AES-256";
    if (e.Offset > fileSize)
      _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
    _items.Add(item);
  }
  if (anyEncrypted && !_keyDefined)
  {
    const HRESULT k = GetKey(callback);
    if (k == E_ABORT)
      return E_ABORT;
  }
  _phySize = fileSize;
  _comment = "Unreal pak version ";
  _comment.Add_UInt32(major);
  if (!mount.IsEmpty())
  {
    _comment += ", mount point ";
    _comment += mount;
  }
  return S_OK;
}

REGISTER_ARC_I_NO_SIG(
  "UnrealPak", "pak", NULL, 0xAB,
  0,
  NArcInfoFlags::kByExtOnlyOpen,
  NULL)

}}
