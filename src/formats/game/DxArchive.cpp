// DxArchive.cpp -- 7zfm-qt: DxLib archives (.dxa), which WOLF RPG Editor
// games keep their data in (Data.wolf, or .wolf files in Data/), after
// GARbro's DxLib reader (MIT license, see LICENSE.garbro).
//
//   The whole archive is XORed with a 12-byte key, from the archive's
//   start (the header and data) or the index's (the index); from version 6
//   a file's data from its size instead. The key comes from the game's
//   password, but where the header's fields have known values it can be
//   read back from the header itself, as here: the "DX" signature and
//   version, and (version 6) the data's offset 0x30 and the upper half of
//   the index's offset, which is 0; (versions 1-4) the data's offset 0x18
//   or 0x1C and an index that runs to the end of the file. If that fails,
//   DxLib's default key (no password) and then the user's password are
//   tried.
//
//   Header after "DX" and the version (16-bit): the index's size, the
//   data's offset, the index's offset, and in the index the file table's
//   and directory table's offsets; 32-bit up to version 4, 64-bit from 6,
//   where the code page follows (932, Shift-JIS, before that). The
//   directory table: per directory its own entry in the file table, its
//   parent's, its file count and its files' offset in the file table. A
//   file table entry: the name's offset in the name table, attributes
//   (0x10 a directory, whose offset is then its directory table entry),
//   created, accessed and written FILETIMEs, the data's offset from the
//   data's start, its size, and (from version 2) the packed size or -1. A
//   name entry: its length / 4 (16-bit), a hash, then an upper-case copy
//   and the name.
//
//   Packed data: the unpacked and packed sizes (32-bit, the packed with
//   this header), a control byte, then bytes; the control byte, unless
//   doubled, starts a copy: the byte after it (less 1 if above the control
//   byte) holds the length - 4 in bits 3-7 (bit 2: 8 more bits in the next
//   byte) and in bits 0-1 the distance's size (1 to 3 bytes); the distance
//   is that + 1.
//
// Version 5 and DxLib 3's version 8 (WOLF RPG Editor 3) are different
// layouts and not read.

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"
#include "Common/UTFConvert.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "../common/ItemArchive.h"

#include <iconv.h>
#include <stdio.h>
#include <string.h>

namespace NArchive {
namespace NDxArchive {

using namespace NItemArc;

static const unsigned kKeySize = 12;
static const UInt32 kMaxIndex = (UInt32)1 << 28;

class CHandler Z7_final: public CHandlerBase
{
  Byte _key[kKeySize];
  unsigned _version;
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
  bool TryKey(IInStream *stream, UInt64 fileSize, unsigned version, const Byte *key);
  bool ReadIndex(const Byte *index, size_t size, unsigned version, UInt64 base, UInt64 tableOffset,
      UInt64 fileTable, UInt64 dirTable, unsigned codePage, const UString &root, unsigned depth);
public:
  void Xor(UInt64 pos, Byte *data, size_t size) const
  {
    unsigned k = (unsigned)(pos % kKeySize);
    for (size_t i = 0; i < size; i++)
    {
      data[i] ^= _key[k];
      if (++k == kKeySize)
        k = 0;
    }
  }
  void TransformStored(const CItem &item, unsigned, UInt64 pos, Byte *data, size_t size) Z7_override
    { Xor(item.Aux + pos, data, size); }
  HRESULT DecodePiece(const CItem &item, const CPiece &piece, CByteBuffer &packed, Byte *dest) Z7_override;
};

// ---------- the key ----------

static Byte RotR(Byte b, unsigned n) { return (Byte)((b >> n) | (b << (8 - n))); }
static Byte RotL(Byte b, unsigned n) { return (Byte)((b << n) | (b >> (8 - n))); }

// the key DxLib makes of a password (in the game's code page): its first
// 12 bytes, repeated to fill 12, or 0xAA for none, then scrambled
static void KeyFromPassword(const AString &password, Byte *key)
{
  if (password.IsEmpty())
    memset(key, 0xAA, kKeySize);
  else
  {
    const unsigned len = MyMin(password.Len(), kKeySize);
    for (unsigned i = 0; i < kKeySize; i++)
      key[i] = (Byte)password[i % len];
  }
  key[0] ^= 0xFF;
  key[1] = RotR(key[1], 4);
  key[2] ^= 0x8A;
  key[3] = (Byte)~RotR(key[3], 4);
  key[4] ^= 0xFF;
  key[5] ^= 0xAC;
  key[6] ^= 0xFF;
  key[7] = (Byte)~RotR(key[7], 3);
  key[8] = RotL(key[8], 3);
  key[9] ^= 0x7F;
  key[10] = (Byte)(RotR(key[10], 4) ^ 0xD6);
  key[11] ^= 0xCC;
}

static void SetUi32(Byte *p, UInt32 v) { for (unsigned i = 0; i < 4; i++) p[i] = (Byte)(v >> (8 * i)); }

// ---------- names ----------

static UString FromCodePage(const AString &a, unsigned codePage)
{
  UString u;
  if (codePage == 65001 && ConvertUTF8ToUnicode(a, u))
    return u;
  bool ascii = true;
  for (unsigned i = 0; i < a.Len(); i++)
    if ((Byte)a[i] >= 0x80)
      ascii = false;
  if (!ascii)
  {
    char cp[16];
    snprintf(cp, sizeof(cp), "CP%u", codePage);
    const iconv_t cd = iconv_open("UTF-8", cp);
    if (cd != (iconv_t)-1)
    {
      CByteBuffer out((size_t)a.Len() * 4 + 4);
      char *in = const_cast<char *>(a.Ptr());
      size_t inLeft = a.Len();
      char *o = (char *)(Byte *)out;
      size_t outLeft = out.Size() - 1;
      const size_t r = iconv(cd, &in, &inLeft, &o, &outLeft);
      iconv_close(cd);
      if (r != (size_t)-1 && inLeft == 0)
      {
        *o = 0;
        AString utf((const char *)(const Byte *)out);
        if (ConvertUTF8ToUnicode(utf, u))
          return u;
      }
    }
  }
  return MultiByteToUnicodeString(a, CP_ACP);
}

// ---------- the index ----------

static UInt64 Get(const Byte *p, bool wide) { return wide ? GetUi64(p) : GetUi32(p); }

// the name at (offset) in the name table: length / 4, hash, upper-case copy, name
static bool GetName(const Byte *index, size_t size, UInt64 offset, AString &name)
{
  if (offset >= size || size - offset < 4)
    return false;
  const UInt64 start = offset + GetUi16(index + offset) * 4 + 4;
  if (start >= size)
    return false;
  const Byte *p = index + start;
  const size_t max = size - (size_t)start;
  size_t len = 0;
  while (len < max && p[len] != 0)
    len++;
  if (len == max || len == 0)
    return false;
  name.SetFrom_CalcLen((const char *)p, (unsigned)len);
  return true;
}

bool CHandler::ReadIndex(const Byte *index, size_t size, unsigned version, UInt64 base, UInt64 tableOffset,
    UInt64 fileTable, UInt64 dirTable, unsigned codePage, const UString &rootIn, unsigned depth)
{
  if (depth > 64)
    return false;
  const bool wide = version >= 6;
  const unsigned w = wide ? 8 : 4;
  const UInt64 dirPos = dirTable + tableOffset;
  if (dirPos >= size || size - dirPos < 4 * w)
    return false;
  const Byte *d = index + dirPos;
  const UInt64 dirOffset = Get(d, wide), parent = Get(d + w, wide);
  const UInt64 fileCount = Get(d + 2 * w, wide), files = Get(d + 3 * w, wide);
  const UInt64 none = wide ? (UInt64)(Int64)-1 : 0xFFFFFFFF;
  UString root = rootIn;
  if (dirOffset != none && parent != none)
  {
    const UInt64 e = fileTable + dirOffset;
    if (e >= size || size - e < w)
      return false;
    AString name;
    if (!GetName(index, size, Get(index + e, wide), name))
      return false;
    root += FromCodePage(name, codePage);
    root += L'/';
  }
  const unsigned entrySize = wide ? 0x40 : (version >= 2 ? 0x2C : 0x28);
  UInt64 pos = fileTable + files;
  if (fileCount > size / entrySize)
    return false;
  for (UInt64 i = 0; i < fileCount; i++, pos += entrySize)
  {
    if (pos >= size || size - pos < entrySize)
      return false;
    const Byte *e = index + pos;
    const UInt64 nameOffset = Get(e, wide);
    const UInt32 attrib = (UInt32)Get(e + w, wide);
    const Byte *times = e + 2 * w;
    const Byte *rest = times + 0x18;
    const UInt64 offset = Get(rest, wide);
    if (attrib & 0x10)
    {
      if (offset == 0 || offset == tableOffset)
        return false;
      if (!ReadIndex(index, size, version, base, offset, fileTable, dirTable, codePage, root, depth + 1))
        return false;
      continue;
    }
    const UInt64 fileSize = Get(rest + w, wide);
    UInt64 packed = none;
    if (version >= 2)
      packed = Get(rest + 2 * w, wide);
    AString name;
    if (!GetName(index, size, nameOffset, name))
      return false;
    CItem item;
    item.Path = root + FromCodePage(name, codePage);
    const UInt64 dataPos = base + offset;
    // the key runs from the data's offset, or from version 6 its size
    item.Aux = wide ? fileSize : dataPos;
    if (packed != none)
    {
      item.AddPiece(dataPos, packed, fileSize, kMethod_Custom, item.Aux);
      item.Method = "DX XOR";
    }
    else
    {
      item.SetStored(dataPos, fileSize);
      item.Method = "XOR";
    }
    const UInt64 mtime = GetUi64(times + 0x10);
    if (mtime != 0)
    {
      item.MTime.dwLowDateTime = (DWORD)mtime;
      item.MTime.dwHighDateTime = (DWORD)(mtime >> 32);
      item.MTime_Defined = true;
    }
    _items.Add(item);
  }
  return true;
}

bool CHandler::TryKey(IInStream *stream, UInt64 fileSize, unsigned version, const Byte *key)
{
  memcpy(_key, key, kKeySize);
  _items.Clear();
  const bool wide = version >= 6;
  const unsigned hsize = wide ? 0x30 : 0x1C;
  if (fileSize < hsize)
    return false;
  Byte h[0x30];
  if (ReadAt(stream, 0, h, hsize) != S_OK)
    return false;
  Xor(0, h, hsize);
  if (h[0] != 'D' || h[1] != 'X' || GetUi16(h + 2) != version)
    return false;
  const UInt32 indexSize = GetUi32(h + 4);
  const UInt64 base = wide ? GetUi64(h + 8) : GetUi32(h + 8);
  const UInt64 indexOffset = wide ? GetUi64(h + 16) : GetUi32(h + 12);
  const UInt64 fileTable = wide ? GetUi64(h + 24) : GetUi32(h + 16);
  const UInt64 dirTable = wide ? GetUi64(h + 32) : GetUi32(h + 20);
  const unsigned codePage = wide ? GetUi32(h + 40) : 932;
  if (indexSize > kMaxIndex || indexOffset > fileSize || indexSize > fileSize - indexOffset
      || base > fileSize || fileTable >= indexSize || dirTable >= indexSize)
    return false;
  CByteBuffer index(indexSize);
  if (ReadAt(stream, indexOffset, index, indexSize) != S_OK)
    return false;
  // the index's key runs from the index's start from version 6, else from its offset
  Xor(wide ? 0 : indexOffset, index, indexSize);
  if (!ReadIndex(index, indexSize, version, base, 0, fileTable, dirTable, codePage, UString(), 0))
  {
    _items.Clear();
    return false;
  }
  _version = version;
  _phySize = fileSize;
  _comment = "DxLib archive version ";
  _comment.Add_UInt32(version);
  _comment += ", code page ";
  _comment.Add_UInt32(codePage);
  return true;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))
  if (fileSize < 0x1C)
    return S_FALSE;
  Byte h[0x30];
  memset(h, 0, sizeof(h));
  RINOK(ReadAt(stream, 0, h, (size_t)MyMin(fileSize, (UInt64)sizeof(h))))

  // DxLib 3's version 8, not encrypted at the start
  if (h[0] == 'D' && h[1] == 'X' && GetUi16(h + 2) == 8)
  {
    _phySize = fileSize;
    _comment = "DxLib archive version 8";
    _error = "DxLib archive version 8 (WOLF RPG Editor 3) is not supported";
    return S_OK;
  }

  Byte key[kKeySize];
  // version 6: "DX", 6, the data at 0x30 and the index offset's upper half 0
  {
    Byte k[0x30];
    memcpy(k, h, sizeof(k));
    k[0] ^= 'D'; k[1] ^= 'X'; k[2] ^= 6;
    k[8] ^= 0x30;
    const UInt32 key0 = GetUi32(k);
    if (fileSize >= 0x30 && (GetUi32(k + 12) ^ key0) == 0)
    {
      SetUi32(key, key0);
      SetUi32(key + 4, GetUi32(k + 0x1C));
      SetUi32(key + 8, GetUi32(k + 8));
      if (TryKey(stream, fileSize, 6, key))
        return S_OK;
    }
  }
  // versions 4 to 1: "DX", the version, the data at 0x1C (0x18 before 4)
  // and the index running to the end
  for (unsigned version = 4; version >= 1; version--)
  {
    memcpy(key, h, kKeySize);
    key[0] ^= 'D'; key[1] ^= 'X'; key[2] ^= (Byte)version;
    const unsigned dataStart = version > 3 ? 0x1C : 0x18;
    key[8] ^= (Byte)dataStart;
    const UInt32 indexOffset = GetUi32(h + 12) ^ GetUi32(key);
    if (indexOffset <= dataStart || indexOffset >= fileSize)
      continue;
    const UInt32 indexSize = (UInt32)(fileSize - indexOffset);
    if (indexSize > 0xFFFFFF)
      continue;
    key[4] ^= (Byte)indexSize;
    key[5] ^= (Byte)(indexSize >> 8);
    key[6] ^= (Byte)(indexSize >> 16);
    if (TryKey(stream, fileSize, version, key))
      return S_OK;
  }
  // DxLib's own key (no password), then the game's password
  for (unsigned attempt = 0; attempt < 2; attempt++)
  {
    AString password;
    if (attempt == 1)
    {
      UString u;
      const HRESULT res = AskPassword(callback, u);
      if (res != S_OK)
        return res == E_ABORT ? E_ABORT : S_FALSE;
      password = UnicodeStringToMultiByte(u, CP_ACP);
    }
    KeyFromPassword(password, key);
    const unsigned version = (h[2] ^ key[2]) | ((unsigned)(h[3] ^ key[3]) << 8);
    if ((h[0] ^ key[0]) != 'D' || (h[1] ^ key[1]) != 'X')
      continue;
    if ((version >= 1 && version <= 4) || version == 6)
      if (TryKey(stream, fileSize, version, key))
        return S_OK;
  }
  return S_FALSE;
}

// ---------- packed data ----------

static bool Unpack(const Byte *src, size_t srcSize, Byte *dest, size_t destSize)
{
  if (srcSize < 9 || GetUi32(src) != destSize)
    return false;
  const size_t packed = GetUi32(src + 4);
  if (packed < 9 || packed > srcSize)
    return false;
  const Byte code = src[8];
  size_t p = 9, d = 0;
  while (p < packed)
  {
    Byte b = src[p++];
    if (b != code)
    {
      if (d >= destSize)
        return false;
      dest[d++] = b;
      continue;
    }
    if (p >= packed)
      return false;
    b = src[p++];
    if (b == code)
    {
      if (d >= destSize)
        return false;
      dest[d++] = b;
      continue;
    }
    if (b > code)
      b--;
    size_t count = b >> 3;
    if (b & 4)
    {
      if (p >= packed)
        return false;
      count |= (size_t)src[p++] << 5;
    }
    count += 4;
    const unsigned n = (b & 3) + 1;
    if (n == 4 || packed - p < n)
      return false;
    size_t dist = 0;
    for (unsigned i = 0; i < n; i++)
      dist |= (size_t)src[p + i] << (8 * i);
    p += n;
    dist++;
    if (dist > d || count > destSize - d)
      return false;
    for (size_t i = 0; i < count; i++, d++)
      dest[d] = dest[d - dist];
  }
  return d == destSize;
}

HRESULT CHandler::DecodePiece(const CItem &item, const CPiece &piece, CByteBuffer &packed, Byte *dest)
{
  if (piece.Method != kMethod_Custom)
    return CHandlerBase::DecodePiece(item, piece, packed, dest);
  Xor(piece.Aux, packed, packed.Size());
  return Unpack(packed, packed.Size(), dest, (size_t)piece.Size) ? S_OK : k_Hr_DataError;
}

REGISTER_ARC_I_NO_SIG(
  "DxArchive", "wolf dxa", NULL, 0xB6,
  0,
  NArcInfoFlags::kByExtOnlyOpen,
  NULL)

}}
