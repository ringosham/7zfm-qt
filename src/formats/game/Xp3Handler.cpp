// Xp3Handler.cpp -- 7zfm-qt: KiriKiri (TVP) archives, .xp3, also inside a
// game's executable. After GARbro's reader (ArcFormats/KiriKiri/ArcXP3.cs,
// MIT license):
//
//   "XP3\r\n \n\x1A\x8B\x67\x01", the offset of the index (KiriKiri Z: of a
//   0x80 marker, then the index's offset 9 bytes on). The index: a type
//   byte (0 plain: its size; 1 zlib: packed and unpacked sizes), then
//   chunks of a 4-byte name and an 8-byte size. A "File" chunk holds
//   sections: "info" (flags, nonzero if encrypted; size; packed size; the
//   name as a 16-bit length and UTF-16), "segm" (the parts of the file,
//   28 bytes each: flags, bit 0 zlib; offset; size; packed size), "adlr"
//   (Adler-32 of the file), "time" (a FILETIME). Other chunks of KiriKiri Z
//   games ("eliF", "hnfn"...) give names for the Adler-32 values of
//   entries whose own name is a hash.
//
// Many commercial games encrypt their files, each its own way (GARbro
// knows a hundred and more); those come out as they are stored, marked
// encrypted.

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "../common/ItemArchive.h"

#include <zlib.h>

#include <map>
#include <string.h>

namespace NArchive {
namespace NXp3 {

using namespace NItemArc;

static const Byte kSignature[] = { 'X', 'P', '3', 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01 };

static UInt32 Sig(const char *s) { return GetUi32((const Byte *)s); }

class CHandler Z7_final: public CHandlerBase
{
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
};

static bool ReadUtf16Name(const Byte *p, size_t size, size_t &pos, UString &name)
{
  if (size - pos < 2)
    return false;
  const unsigned len = GetUi16(p + pos);
  pos += 2;
  if ((size - pos) / 2 < len)
    return false;
  name.Empty();
  for (unsigned i = 0; i < len; i++)
    name += (wchar_t)GetUi16(p + pos + i * 2);
  pos += (size_t)len * 2;
  return true;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *)
{
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))
  Byte h[32];
  if (fileSize < sizeof(kSignature) + 8)
    return S_FALSE;
  RINOK(ReadAt(stream, 0, h, sizeof(kSignature) + 8))
  if (memcmp(h, kSignature, sizeof(kSignature)) != 0)
    return S_FALSE;
  UInt64 dirOffset = GetUi64(h + sizeof(kSignature));
  if (dirOffset < 0x13 || dirOffset >= fileSize)
    return S_FALSE;
  RINOK(ReadAt(stream, dirOffset, h, 4))
  if (GetUi32(h) == 0x80)
  {
    RINOK(ReadAt(stream, dirOffset + 9, h, 8))
    dirOffset = GetUi64(h);
    if (dirOffset < 0x13 || dirOffset >= fileSize)
      return S_FALSE;
  }
  const size_t avail = (size_t)MyMin(fileSize - dirOffset, (UInt64)17);
  RINOK(ReadAt(stream, dirOffset, h, avail))
  CByteBuffer index;
  if (h[0] == 0)
  {
    const UInt64 size = GetUi64(h + 1);
    if (size > fileSize - dirOffset - 9 || size > ((UInt64)1 << 30))
      return S_FALSE;
    index.Alloc((size_t)size);
    RINOK(ReadAt(stream, dirOffset + 9, index, (size_t)size))
  }
  else if (h[0] == 1 && avail == 17)
  {
    const UInt64 packed = GetUi64(h + 1);
    const UInt64 size = GetUi64(h + 9);
    if (packed > fileSize - dirOffset - 17 || size > ((UInt64)1 << 30))
      return S_FALSE;
    CByteBuffer p((size_t)packed);
    RINOK(ReadAt(stream, dirOffset + 17, p, (size_t)packed))
    index.Alloc((size_t)size);
    if (CHandlerBase::DecodeMethod(kMethod_Zlib, 0, p, p.Size(), index, index.Size()) != S_OK)
      return S_FALSE;
  }
  else
    return S_FALSE;

  const Byte *d = index;
  const size_t dSize = index.Size();
  std::map<UInt32, UString> names;   // Adler-32 -> name (KiriKiri Z)
  CRecordVector<UInt32> hashes;      // each item's, for the names
  size_t pos = 0;
  while (dSize - pos >= 12)
  {
    const UInt32 sig = GetUi32(d + pos);
    const UInt64 chunkSize = GetUi64(d + pos + 4);
    pos += 12;
    if (chunkSize > dSize - pos)
    {
      _errorFlags |= kpv_ErrorFlags_HeadersError;
      break;
    }
    const size_t chunkEnd = pos + (size_t)chunkSize;
    if (sig == Sig("File"))
    {
      CItem item;
      bool haveInfo = false;
      UInt32 hash = 0;
      bool haveHash = false;
      size_t s = pos;
      while (chunkEnd - s >= 12)
      {
        const UInt32 section = GetUi32(d + s);
        UInt64 sectionSize = GetUi64(d + s + 4);
        s += 12;
        if (sectionSize > chunkEnd - s)
          sectionSize = chunkEnd - s;
        const Byte *q = d + s;
        const size_t qSize = (size_t)sectionSize;
        if (section == Sig("info") && qSize >= 20)
        {
          item.Encrypted = GetUi32(q) != 0;
          size_t np = 20;
          ReadUtf16Name(q, qSize, np, item.Path);
          item.Path.Replace(L'\\', L'/');
          haveInfo = true;
        }
        else if (section == Sig("segm"))
        {
          for (size_t k = 0; k + 28 <= qSize; k += 28)
          {
            const UInt32 flags = GetUi32(q + k);
            const UInt64 offset = GetUi64(q + k + 4);
            const UInt64 size = GetUi64(q + k + 12);
            const UInt64 packed = GetUi64(q + k + 20);
            item.AddPiece(offset, packed, size, (flags & 1) ? kMethod_Zlib : kMethod_Stored);
            if (offset > fileSize || packed > fileSize - offset)
              _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
          }
        }
        else if (section == Sig("adlr") && qSize >= 4)
        {
          hash = GetUi32(q);
          haveHash = true;
        }
        else if (section == Sig("time") && qSize >= 8)
        {
          const UInt64 t = GetUi64(q);
          if (t != 0)
          {
            item.MTime.dwLowDateTime = (DWORD)t;
            item.MTime.dwHighDateTime = (DWORD)(t >> 32);
            item.MTime_Defined = true;
          }
        }
        s += qSize;
      }
      if (haveInfo && !item.Pieces.IsEmpty())
      {
        if (item.Encrypted)
          item.Method = "Encrypted";
        else if (haveHash)
        {
          item.CheckType = kCheck_Adler32;
          item.Check = hash;
        }
        hashes.Add(haveHash ? hash : 0);
        _items.Add(item);
      }
    }
    else if (chunkSize > 7)
    {
      // "eliF", "hnfn", ...: an Adler-32 and the name it stands for
      const UInt32 hash = GetUi32(d + pos);
      size_t np = pos + 4;
      UString name;
      if (ReadUtf16Name(d, chunkEnd, np, name) && !name.IsEmpty())
        names[hash] = name;
    }
    pos = chunkEnd;
  }
  if (!names.empty())
    FOR_VECTOR (i, _items)
    {
      const std::map<UInt32, UString>::const_iterator it = names.find(hashes[i]);
      if (it != names.end())
      {
        _items[i].Path = it->second;
        _items[i].Path.Replace(L'\\', L'/');
      }
    }
  if (_items.IsEmpty())
    return S_FALSE;
  _phySize = fileSize;
  return S_OK;
}

REGISTER_ARC_I(
  "XP3", "xp3", NULL, 0xAF,
  kSignature,
  0,
  NArcInfoFlags::kFindSignature,
  NULL)

}}
