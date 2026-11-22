// InstallShield.cpp -- 7zfm-qt: InstallShield cabinets (data1.hdr with
// data1.cab, data2.cab ...; versions 5 to 2015 and later), after unshield
// (David Eriksson and contributors, MIT license; see LICENSE.unshield),
// its reading rewritten for 7-Zip's interfaces:
//
//   "ISc(", a version, the cabinet descriptor's offset. The descriptor
//   gives the file table: the directories' and files' name offsets (up to
//   version 5), or (6 and later) an array of 0x57-byte file records: flags
//   (split, obfuscated, compressed, invalid), sizes, data offset, MD5,
//   name, directory, links to an identical file, and the volume (cabinet
//   number) holding the data. Each cabinet starts with a volume header that
//   says where the first and last files it holds a part of are.
//   A compressed file is blocks of a 16-bit length and raw deflate data, or
//   up to version 5 one raw deflate stream (unshield -O; InstallShield 5's
//   own cabinets, version 0x01000004); an obfuscated one is XORed with a
//   running key (unshield_deobfuscate). Up to version 5 the file table
//   doesn't say which cabinet holds a file: the volume headers do.
//
// The cabinet opened may be the .hdr or any .cab of the set; the header
// comes from data1.hdr when there is one, the data from dataN.cab.

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"

#include "Windows/PropVariant.h"

#include "7zip/Common/ProgressUtils.h"
#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamObjects.h"
#include "7zip/Common/StreamUtils.h"

#include "C/Md5.h"

#include "../common/ItemArchive.h"

#include <zlib.h>

#include <string.h>

using namespace NWindows;

namespace NArchive {
namespace NInstallShield {

using NItemArc::GetUi16;
using NItemArc::GetUi32;
using NItemArc::GetUi64;
using NItemArc::ReadAt;
using NItemArc::GetStreamSize;

static const UInt32 kSignature = 0x28635349;    // "ISc("
static const unsigned kMaxGroups = 71;

static const unsigned kFile_Split = 1;
static const unsigned kFile_Obfuscated = 2;
static const unsigned kFile_Compressed = 4;
static const unsigned kFile_Invalid = 8;

struct CFile
{
  UString Path;
  UInt32 Flags;
  UInt64 Size;
  UInt64 PackSize;
  UInt64 Offset;
  Byte Md5[16];
  unsigned Volume;
  UInt32 Index;         // in the file table, as the volume headers count
};

class CHandler Z7_final:
  public IInArchive,
  public IInArchiveGetStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(IInArchive, IInArchiveGetStream)
public:
  CObjectVector<CFile> _files;
  CObjectVector<CMyComPtr<IInStream> > _volumes;   // [n]: dataN.cab, NULL if missing
  unsigned _major;
  UInt32 _fileCount;     // in the file table
  UInt64 _phySize;
  UString _comment;
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback);
};

// the version to read the tables by (unshield's major_version)
static unsigned MajorVersion(UInt32 v)
{
  if ((v >> 24) == 1)
    return (v >> 12) & 0xF;
  if ((v >> 24) == 2 || (v >> 24) == 4)
  {
    const unsigned m = v & 0xFFFF;
    return m ? m / 100 : 0;
  }
  return 0;
}

static UString ReadName(const Byte *p, size_t size, bool unicode)
{
  UString s;
  if (unicode)
  {
    for (size_t i = 0; i + 1 < size; i += 2)
    {
      const wchar_t c = (wchar_t)GetUi16(p + i);
      if (c == 0)
        break;
      s += c;
    }
    return s;
  }
  AString a;
  for (size_t i = 0; i < size && p[i]; i++)
    a += (char)p[i];
  return MultiByteToUnicodeString(a, CP_ACP);
}

// "data1.hdr" -> "data", or the name before the first digit or dot
static UString VolumePrefix(const UString &name)
{
  UString s = name;
  for (unsigned i = 0; i < s.Len(); i++)
    if (s[i] == L'.' || (s[i] >= L'0' && s[i] <= L'9'))
    {
      s.DeleteFrom(i);
      break;
    }
  return s;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))
  Byte h[20];
  if (fileSize < 20)
    return S_FALSE;
  RINOK(ReadAt(stream, 0, h, 20))
  if (GetUi32(h) != kSignature)
    return S_FALSE;

  const UString arcName = NItemArc::GetArcName(callback);
  const UString prefix = VolumePrefix(arcName);
  // the header: data1.hdr if there is one, else the cabinet itself
  CMyComPtr<IInStream> hdr;
  if (!prefix.IsEmpty())
  {
    RINOK(NItemArc::OpenSiblingFile(callback, prefix + L"1.hdr", hdr))
    if (!hdr && arcName.Len() > 4 && StringsAreEqualNoCase_Ascii(arcName.RightPtr(4), ".hdr"))
      hdr = stream;
  }
  if (!hdr)
  {
    // the first cabinet holds the descriptor
    RINOK(NItemArc::OpenSiblingFile(callback, prefix + L"1.cab", hdr))
    if (!hdr)
      hdr = stream;
  }
  UInt64 hSize = 0;
  RINOK(GetStreamSize(hdr, hSize))
  if (hSize > ((UInt64)1 << 28) || hSize < 20)
    return S_FALSE;
  CByteBuffer d((size_t)hSize);
  RINOK(ReadAt(hdr, 0, d, d.Size()))
  if (GetUi32(d) != kSignature)
    return S_FALSE;
  const UInt32 version = GetUi32(d + 4);
  const UInt32 descOffset = GetUi32(d + 12);
  const UInt32 descSize = GetUi32(d + 16);
  if (descSize == 0 || descOffset > hSize || hSize - descOffset < 0x30)
  {
    // a cabinet without the descriptor: the header is elsewhere
    return S_FALSE;
  }
  _major = MajorVersion(version);
  const bool unicode = _major >= 17;
  const Byte *desc = d + descOffset;
  const UInt32 tableOffset = GetUi32(desc + 0x0C);
  const UInt32 dirCount = GetUi32(desc + 0x1C);
  const UInt32 fileCount = GetUi32(desc + 0x28);
  _fileCount = fileCount;
  const UInt32 tableOffset2 = GetUi32(desc + 0x2C);
  const UInt64 tableStart = (UInt64)descOffset + tableOffset;
  const UInt64 count = (UInt64)dirCount + fileCount;
  if (count == 0 || count > (1 << 22) || tableStart > hSize || count * 4 > hSize - tableStart)
    return S_FALSE;
  const Byte *table = d + (size_t)tableStart;
  auto nameAt = [&](UInt64 off) -> UString
  {
    const UInt64 at = tableStart + off;
    if (at >= hSize)
      return UString();
    return ReadName(d + (size_t)at, (size_t)(hSize - at), unicode);
  };
  UStringVector dirs;
  for (UInt32 i = 0; i < dirCount; i++)
  {
    UString dn = nameAt(GetUi32(table + i * 4));
    dn.Replace(L'\\', L'/');
    dirs.Add(dn);
  }

  struct CRaw { UInt32 Flags; UInt64 Size, Pack, Offset; Byte Md5[16]; UInt32 Name, Dir, LinkPrev, LinkFlags, Volume; };
  CObjectVector<CRaw> raw;
  for (UInt32 i = 0; i < fileCount; i++)
  {
    CRaw f;
    memset(f.Md5, 0, 16);
    f.LinkPrev = 0;
    f.LinkFlags = 0;
    if (_major <= 5)
    {
      const UInt64 at = tableStart + GetUi32(table + (dirCount + i) * 4);
      if (at + 0x3A > hSize)
        return S_FALSE;
      const Byte *p = d + (size_t)at;
      f.Name = GetUi32(p);
      f.Dir = GetUi16(p + 4);
      f.Flags = GetUi16(p + 8);
      f.Size = GetUi32(p + 10);
      f.Pack = GetUi32(p + 14);
      f.Offset = GetUi32(p + 18 + 0x14);
      if (_major == 5)
        memcpy(f.Md5, p + 0x2A, 16);
      f.Volume = 1;
    }
    else
    {
      const UInt64 at = tableStart + tableOffset2 + (UInt64)i * 0x57;
      if (at + 0x57 > hSize)
        return S_FALSE;
      const Byte *p = d + (size_t)at;
      f.Flags = GetUi16(p);
      f.Size = GetUi64(p + 2);
      f.Pack = GetUi64(p + 10);
      f.Offset = GetUi64(p + 18);
      memcpy(f.Md5, p + 26, 16);
      f.Name = GetUi32(p + 58);
      f.Dir = GetUi16(p + 62);
      f.LinkPrev = GetUi32(p + 76);
      f.LinkFlags = p[84];
      f.Volume = GetUi16(p + 85);
    }
    raw.Add(f);
  }

  for (UInt32 i = 0; i < fileCount; i++)
  {
    CRaw f = raw[i];
    if ((f.Flags & kFile_Invalid) || f.Name == 0)
      continue;
    UInt32 dataIndex = i;
    // a link to an earlier identical file, which may be a link itself:
    // the data of the first of them
    for (UInt32 n = 0; (raw[dataIndex].LinkFlags & 1) && raw[dataIndex].LinkPrev < fileCount && n < fileCount; n++)
      dataIndex = raw[dataIndex].LinkPrev;
    if (dataIndex != i)
    {
      const CRaw &src = raw[dataIndex];
      f.Flags = src.Flags;
      f.Size = src.Size;
      f.Pack = src.Pack;
      f.Offset = src.Offset;
      memcpy(f.Md5, src.Md5, 16);
      f.Volume = src.Volume;
    }
    if (f.Offset == 0)
      continue;
    CFile file;
    UString name = nameAt(f.Name);
    name.Replace(L'\\', L'/');
    file.Path = (f.Dir < dirs.Size() && !dirs[f.Dir].IsEmpty()) ? dirs[f.Dir] + L'/' + name : name;
    file.Flags = f.Flags;
    file.Size = f.Size;
    file.PackSize = f.Pack;
    file.Offset = f.Offset;
    memcpy(file.Md5, f.Md5, 16);
    file.Volume = f.Volume ? f.Volume : 1;
    file.Index = dataIndex;
    _files.Add(file);
  }

  // the cabinets: dataN.cab, as many as the files name; up to version 5
  // the files don't say, so all there are
  unsigned maxVolume = 1;
  FOR_VECTOR (i, _files)
    maxVolume = MyMax(maxVolume, _files[i].Volume);
  _volumes.Add(CMyComPtr<IInStream>());
  for (unsigned v = 1; v < 256; v++)
  {
    CMyComPtr<IInStream> s;
    UString name = prefix;
    name.Add_UInt32(v);
    name += L".cab";
    if (StringsAreEqualNoCase(name, arcName))
      s = stream;
    else
      RINOK(NItemArc::OpenSiblingFile(callback, name, s))
    if (!s && v > maxVolume + 1 && (_major > 5 || v > 1))
      break;
    _volumes.Add(s);
  }
  _phySize = fileSize;
  _comment = "InstallShield ";
  _comment.Add_UInt32(_major);
  return S_OK;
}

// reads a file's stored bytes across cabinets, as unshield's reader does
class CReader
{
  CHandler *_arc;
  const CFile *_f;
  unsigned _volume;
  CMyComPtr<IInStream> _s;
  UInt64 _pos;
  UInt64 _left;            // in this volume
  unsigned _seed;
  bool OpenVolume(unsigned v);
public:
  CReader(CHandler *arc, const CFile *f): _arc(arc), _f(f), _volume(0), _pos(0), _left(0), _seed(0) {}
  bool Open();
  bool Read(Byte *data, size_t size);
};

bool CReader::OpenVolume(unsigned v)
{
  if (v >= _arc->_volumes.Size() || !_arc->_volumes[v])
    return false;
  _s = _arc->_volumes[v];
  _volume = v;
  Byte h[20 + 64];
  const bool v5 = _arc->_major <= 5;
  if (ReadAt(_s, 0, h, 20 + (v5 ? 40 : 64)) != S_OK || GetUi32(h) != kSignature)
    return false;
  const Byte *p = h + 20;
  UInt32 firstIndex, lastIndex;
  UInt64 firstOffset, firstSize, firstPack, lastOffset, lastSize, lastPack;
  if (v5)
  {
    firstIndex = GetUi32(p + 8); lastIndex = GetUi32(p + 12);
    firstOffset = GetUi32(p + 16); firstSize = GetUi32(p + 20); firstPack = GetUi32(p + 24);
    lastOffset = GetUi32(p + 28); lastSize = GetUi32(p + 32); lastPack = GetUi32(p + 36);
    if (lastOffset == 0)
      lastOffset = 0x7FFFFFFF;
  }
  else
  {
    firstIndex = GetUi32(p + 8); lastIndex = GetUi32(p + 12);
    firstOffset = GetUi64(p + 16); firstSize = GetUi64(p + 24); firstPack = GetUi64(p + 32);
    lastOffset = GetUi64(p + 40); lastSize = GetUi64(p + 48); lastPack = GetUi64(p + 56);
  }
  UInt32 flags = _f->Flags;
  if (v5)
  {
    if (_f->Index + 1 < _arc->_fileCount && _f->Index == lastIndex && lastPack != _f->PackSize)
      flags |= kFile_Split;
    else if (_f->Index > 0 && _f->Index == firstIndex && firstPack != _f->PackSize)
      flags |= kFile_Split;
  }
  const bool compressed = (_f->Flags & kFile_Compressed) != 0;
  if (flags & kFile_Split)
  {
    if (_f->Index == lastIndex && lastOffset != 0x7FFFFFFF)
    {
      _pos = lastOffset;
      _left = compressed ? lastPack : lastSize;
    }
    else if (_f->Index == firstIndex)
    {
      _pos = firstOffset;
      _left = compressed ? firstPack : firstSize;
    }
    else
      return true;
  }
  else
  {
    _pos = _f->Offset;
    _left = compressed ? _f->PackSize : _f->Size;
  }
  // version 5: the file may start in a later cabinet than recorded
  if (v5 && _f->Index > lastIndex && !(flags & kFile_Split))
    return OpenVolume(v + 1);
  return true;
}

bool CReader::Open()
{
  _seed = 0;
  return OpenVolume(_f->Volume);
}

bool CReader::Read(Byte *data, size_t size)
{
  Byte *start = data;
  const size_t total = size;
  while (size != 0)
  {
    const size_t n = (size_t)MyMin((UInt64)size, _left);
    if (n == 0)
    {
      if (!OpenVolume(_volume + 1))
        return false;
      continue;
    }
    if (ReadAt(_s, _pos, data, n) != S_OK)
      return false;
    _pos += n;
    _left -= n;
    data += n;
    size -= n;
  }
  if (_f->Flags & kFile_Obfuscated)
  {
    for (size_t i = 0; i < total; i++, _seed++)
    {
      const Byte b = (Byte)(start[i] ^ 0xD5);
      start[i] = (Byte)((Byte)((b >> 2) | (b << 6)) - (_seed % 0x47));
    }
  }
  return true;
}

// whether a file's data starts as blocks of a 16-bit length and raw
// deflate data; up to version 5 it may be one deflate stream instead
static bool FirstBlockOk(CHandler *arc, const CFile &f)
{
  CReader reader(arc, &f);
  Byte lb[2];
  if (f.PackSize < 2 || !reader.Open() || !reader.Read(lb, 2))
    return true;   // fails as before
  const unsigned len = GetUi16(lb);
  if (len == 0 || len > f.PackSize - 2)
    return false;
  CByteBuffer in(len + 1), outBuf((size_t)1 << 17);
  if (!reader.Read(in, len))
    return true;
  in[len] = 0;
  z_stream z;
  memset(&z, 0, sizeof(z));
  if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
    return true;
  z.next_in = in;
  z.avail_in = len + 1;
  z.next_out = outBuf;
  z.avail_out = (uInt)outBuf.Size();
  const int r = inflate(&z, Z_FINISH);
  inflateEnd(&z);
  return r == Z_STREAM_END;
}

// decodes file (index) into (out), checking its MD5 from version 6
static HRESULT DecodeFile(CHandler *arc, const CFile &f, ISequentialOutStream *out, Int32 &opRes)
{
  opRes = NExtract::NOperationResult::kOK;
  CReader reader(arc, &f);
  if (!reader.Open())
  {
    opRes = NExtract::NOperationResult::kUnavailable;
    return S_OK;
  }
  CMd5 md5;
  Md5_Init(&md5);
  UInt64 written = 0;
  const size_t kBuf = 1 << 16;
  CByteBuffer in(kBuf + 1), outBuf(kBuf * 2);
  if ((f.Flags & kFile_Compressed) && arc->_major <= 5 && !FirstBlockOk(arc, f))
  {
    // the old layout (unshield -O): one raw deflate stream
    CReader r2(arc, &f);
    if (!r2.Open())
    {
      opRes = NExtract::NOperationResult::kUnavailable;
      return S_OK;
    }
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
      return E_OUTOFMEMORY;
    UInt64 left = f.PackSize;
    bool padded = false;
    int ret = Z_OK;
    HRESULT hr = S_OK;
    while (ret == Z_OK)
    {
      if (z.avail_in == 0)
      {
        const size_t n = (size_t)MyMin(left, (UInt64)kBuf);
        if (n != 0)
        {
          if (!r2.Read(in, n))
          {
            ret = Z_BUF_ERROR;
            break;
          }
          left -= n;
          z.avail_in = (uInt)n;
        }
        else if (!padded)
        {
          // as unshield: inflate may want a byte more
          in[0] = 0;
          z.avail_in = 1;
          padded = true;
        }
        else
          break;
        z.next_in = in;
      }
      z.next_out = outBuf;
      z.avail_out = (uInt)outBuf.Size();
      ret = inflate(&z, Z_NO_FLUSH);
      const size_t n = outBuf.Size() - z.avail_out;
      if (ret == Z_BUF_ERROR && n == 0 && z.avail_in != 0)
        break;
      if (ret == Z_BUF_ERROR)
        ret = Z_OK;
      if (written + n > f.Size)
      {
        ret = Z_DATA_ERROR;
        break;
      }
      Md5_Update(&md5, outBuf, n);
      if (out && n != 0)
      {
        hr = WriteStream(out, outBuf, n);
        if (hr != S_OK)
          break;
      }
      written += n;
    }
    inflateEnd(&z);
    RINOK(hr)
    if (ret != Z_STREAM_END && !(ret == Z_OK && padded && written == f.Size))
    {
      opRes = NExtract::NOperationResult::kDataError;
      return S_OK;
    }
  }
  else if (f.Flags & kFile_Compressed)
  {
    UInt64 left = f.PackSize;
    while (left > 0)
    {
      Byte lb[2];
      if (left < 2 || !reader.Read(lb, 2))
      {
        opRes = NExtract::NOperationResult::kUnexpectedEnd;
        return S_OK;
      }
      const unsigned len = GetUi16(lb);
      left -= 2;
      if (len == 0 || len > left || !reader.Read(in, len))
      {
        opRes = NExtract::NOperationResult::kDataError;
        return S_OK;
      }
      left -= len;
      in[len] = 0;   // as unshield: inflate wants a byte more
      z_stream z;
      memset(&z, 0, sizeof(z));
      if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
        return E_OUTOFMEMORY;
      z.next_in = in;
      z.avail_in = len + 1;
      z.next_out = outBuf;
      z.avail_out = (uInt)outBuf.Size();
      const int r = inflate(&z, Z_FINISH);
      const size_t n = outBuf.Size() - z.avail_out;
      inflateEnd(&z);
      if (r != Z_STREAM_END)
      {
        opRes = NExtract::NOperationResult::kDataError;
        return S_OK;
      }
      Md5_Update(&md5, outBuf, n);
      if (out)
        RINOK(WriteStream(out, outBuf, n))
      written += n;
    }
  }
  else
  {
    UInt64 left = f.Size;
    while (left > 0)
    {
      const size_t n = (size_t)MyMin(left, (UInt64)kBuf);
      if (!reader.Read(outBuf, n))
      {
        opRes = NExtract::NOperationResult::kUnexpectedEnd;
        return S_OK;
      }
      Md5_Update(&md5, outBuf, n);
      if (out)
        RINOK(WriteStream(out, outBuf, n))
      written += n;
      left -= n;
    }
  }
  if (written != f.Size)
    opRes = NExtract::NOperationResult::kDataError;
  else if (arc->_major >= 6)
  {
    Byte digest[16];
    Md5_Final(&md5, digest);
    if (memcmp(digest, f.Md5, 16) != 0)
      opRes = NExtract::NOperationResult::kCRCError;
  }
  return S_OK;
}

static const Byte kProps[] = { kpidPath, kpidSize, kpidPackSize, kpidMethod };
static const Byte kArcProps[] = { kpidComment };
IMP_IInArchive_Props
IMP_IInArchive_ArcProps

Z7_COM7F_IMF(CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPhySize: prop = _phySize; break;
    case kpidComment: prop = _comment; break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  const CFile &f = _files[index];
  switch (propID)
  {
    case kpidPath: prop = f.Path; break;
    case kpidSize: prop = f.Size; break;
    case kpidPackSize: prop = f.PackSize; break;
    case kpidMethod:
    {
      AString s ((f.Flags & kFile_Compressed) ? "Deflate" : "Copy");
      if (f.Flags & kFile_Obfuscated)
        s += " Obfuscated";
      prop = s;
      break;
    }
    default: break;
  }
  prop.Detach(value);
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
  _files.Clear();
  _volumes.Clear();
  _phySize = 0;
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _files.Size();
  return S_OK;
}

Z7_COM7F_IMF(CHandler::Extract(const UInt32 *indices, UInt32 numItems,
    Int32 testMode, IArchiveExtractCallback *extractCallback))
{
  COM_TRY_BEGIN
  const bool allFilesMode = (numItems == (UInt32)(Int32)-1);
  if (allFilesMode)
    numItems = _files.Size();
  UInt64 total = 0;
  for (UInt32 i = 0; i < numItems; i++)
    total += _files[allFilesMode ? i : indices[i]].Size;
  RINOK(extractCallback->SetTotal(total))
  UInt64 done = 0;
  for (UInt32 i = 0; i < numItems; i++)
  {
    RINOK(extractCallback->SetCompleted(&done))
    const UInt32 index = allFilesMode ? i : indices[i];
    const CFile &f = _files[index];
    CMyComPtr<ISequentialOutStream> out;
    const Int32 askMode = testMode ? NExtract::NAskMode::kTest : NExtract::NAskMode::kExtract;
    RINOK(extractCallback->GetStream(index, &out, askMode))
    done += f.Size;
    if (!testMode && !out)
      continue;
    RINOK(extractCallback->PrepareOperation(askMode))
    Int32 opRes;
    RINOK(DecodeFile(this, f, out, opRes))
    out.Release();
    RINOK(extractCallback->SetOperationResult(opRes))
  }
  return extractCallback->SetCompleted(&done);
  COM_TRY_END
}

// in place: decoded into memory (up to 256 MB)
Z7_COM7F_IMF(CHandler::GetStream(UInt32 index, ISequentialInStream **stream))
{
  COM_TRY_BEGIN
  *stream = NULL;
  if (index >= _files.Size() || _files[index].Size > ((UInt64)1 << 28))
    return S_FALSE;
  const CFile &f = _files[index];
  CReferenceBuf *ref = new CReferenceBuf;
  CMyComPtr<IUnknown> refRef = ref;
  ref->Buf.Alloc((size_t)f.Size);
  CBufPtrSeqOutStream *spec = new CBufPtrSeqOutStream;
  CMyComPtr<ISequentialOutStream> out = spec;
  spec->Init(ref->Buf, ref->Buf.Size());
  Int32 opRes;
  RINOK(DecodeFile(this, f, out, opRes))
  if (opRes != NExtract::NOperationResult::kOK)
    return S_FALSE;
  CBufInStream *in = new CBufInStream;
  CMyComPtr<ISequentialInStream> inRef = in;
  in->Init(ref);
  *stream = inRef.Detach();
  return S_OK;
  COM_TRY_END
}

static const Byte k_Signature[] = { 'I', 'S', 'c', '(' };

REGISTER_ARC_I(
  "InstallShield", "cab hdr", NULL, 0xA5,
  k_Signature,
  0,
  0,
  NULL)

}}
