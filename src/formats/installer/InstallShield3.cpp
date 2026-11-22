// InstallShield3.cpp -- 7zfm-qt: InstallShield 3 archives: the .z files of
// InstallShield 3 setups (_SETUP.LIB and the like) and the DATA.1, DATA.2
// ... sets a setup spans over its disks. Written from the layout below, as
// unshieldv3 (Wolfgang Frisch) and stix (Veit Kannegieser) describe it, with
// the split files worked out from InstallShield 3.0.116's own disk sets; no
// code of theirs is used. The files are PKWARE DCL ("implode") data, which
// Mark Adler's blast (zlib's contrib/blast, zlib license; blast.c, blast.h)
// decodes.
//
//   Each volume starts with: 13 5D 65 8C 3A 01 02 00, u16, u16 multi-volume,
//   u16 number of files in the set, DOS time and date, packed and unpacked
//   size, u32, u8 number of volumes (in the first only), u8 this volume's
//   number (from 1), u8, u32 start of the file that goes on in the next
//   volume, u32 end of the one that came from the previous, u32 offset of
//   the table, u32, u16 number of folders, u32, u32. File data starts at
//   0xFF.
//
//   The table: a record per folder (u16 number of files, u16 record size,
//   u16 name length, its name: "" or "SUB\DIR"), then the folders' files in
//   that order, a record each: u8 last volume, u16 folder, u32 size, u32
//   packed size, u32 offset, u32 DOS date and time, u32, u16 record size,
//   u8 flags (0x10: stored), u8 split, u8, u8 first volume, u8 name length,
//   the name.
//
// Each volume's table lists all the files of the set, those of volumes not
// written yet as in volume 0; the first volume's, written last, is whole.
// A split file runs from its offset to the table in its first volume, on
// from 0xFF to the table in any volume between, and from 0xFF for what is
// left of it in its last volume.

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"

#include "Windows/TimeUtils.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "../common/ItemArchive.h"

extern "C" {
#include "blast.h"
}

#include <string.h>

#include <map>
#include <string>

namespace NArchive {
namespace NInstallShield3 {

using namespace NItemArc;
using namespace NWindows;

static const unsigned kHeaderSize = 59;
static const UInt32 kDataStart = 0xFF;
static const Byte kFlag_Stored = 0x10;
static const unsigned kMaxVolumes = 255;
static const UInt32 kMaxTableSize = (UInt32)1 << 24;

static const Byte k_Signature[] = { 0x13, 0x5D, 0x65, 0x8C, 0x3A, 0x01, 0x02, 0x00 };

struct CSegment
{
  unsigned Volume;   // its stream is _extraStreams[Volume - 1]
  UInt64 Offset;
  UInt64 Size;
};

struct CVolume
{
  bool Present;
  UInt32 Table;
  UInt64 Size;
  CVolume(): Present(false), Table(0), Size(0) {}
};

class CHandler Z7_final: public CHandlerBase
{
  CObjectVector<CRecordVector<CSegment> > _segments;   // of each compressed file, by CPiece::Aux
  CObjectVector<CVolume> _volumes;                     // [0] is volume 1

  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
  void Close2() Z7_override { _segments.Clear(); _volumes.Clear(); }
  HRESULT ReadTable(unsigned volume, std::map<std::string, bool> &folders, unsigned &missing);
public:
  HRESULT DecodePiece(const CItem &item, const CPiece &piece, CByteBuffer &packed, Byte *dest) Z7_override;
  IInStream *VolumeStream(unsigned v) const
    { return (v != 0 && v <= _extraStreams.Size()) ? (IInStream *)_extraStreams[v - 1] : NULL; }
};

static HRESULT ReadHeader(IInStream *stream, Byte *h, UInt64 &size)
{
  RINOK(GetStreamSize(stream, size))
  if (size < kDataStart)
    return S_FALSE;
  RINOK(ReadAt(stream, 0, h, kHeaderSize))
  if (memcmp(h, k_Signature, sizeof(k_Signature)) != 0)
    return S_FALSE;
  const UInt32 table = GetUi32(h + 41);
  if (table < kDataStart || table > size)
    return S_FALSE;
  return S_OK;
}

static UString Name(const Byte *p, size_t size)
{
  AString a;
  for (size_t i = 0; i < size && p[i]; i++)
    a += (char)(p[i] == '\\' ? '/' : p[i]);
  return MultiByteToUnicodeString(a, CP_ACP);
}

// reads the set's files from volume (volume)'s table
HRESULT CHandler::ReadTable(unsigned volume, std::map<std::string, bool> &folders, unsigned &missing)
{
  IInStream *s = _extraStreams[volume - 1];
  const CVolume &vol = _volumes[volume - 1];
  Byte h[kHeaderSize];
  UInt64 fileSize;
  RINOK(ReadHeader(s, h, fileSize))
  const UInt64 tableSize64 = fileSize - vol.Table;
  if (tableSize64 > kMaxTableSize)
    return S_FALSE;
  const size_t tableSize = (size_t)tableSize64;
  CByteBuffer t(tableSize);
  RINOK(ReadAt(s, vol.Table, t, tableSize))
  const unsigned numFolders = GetUi16(h + 49);

  struct CFolder { AString Name; unsigned NumFiles; };
  CObjectVector<CFolder> dirs;
  size_t pos = 0;
  for (unsigned i = 0; i < numFolders; i++)
  {
    if (pos + 6 > tableSize)
      return S_FALSE;
    const unsigned numFiles = GetUi16(t + pos);
    const unsigned recSize = GetUi16(t + pos + 2);
    const unsigned nameLen = GetUi16(t + pos + 4);
    if (recSize < 6 + nameLen || pos + recSize > tableSize)
      return S_FALSE;
    CFolder &d = dirs.AddNew();
    for (unsigned k = 0; k < nameLen && t[pos + 6 + k]; k++)
      d.Name += (char)(t[pos + 6 + k] == '\\' ? '/' : t[pos + 6 + k]);
    d.NumFiles = numFiles;
    if (!d.Name.IsEmpty())
    {
      bool &has = folders[std::string(d.Name.Ptr())];
      has = has || numFiles != 0;
    }
    pos += recSize;
  }

  FOR_VECTOR (di, dirs)
  {
    const CFolder &d = dirs[di];
    for (unsigned i = 0; i < d.NumFiles; i++)
    {
      if (pos + 30 > tableSize)
        return S_FALSE;
      const Byte *p = t + pos;
      const unsigned last = p[0];
      const UInt32 size = GetUi32(p + 3);
      const UInt32 packSize = GetUi32(p + 7);
      const UInt32 offset = GetUi32(p + 11);
      const UInt32 dosTime = GetUi32(p + 15);
      const unsigned recSize = GetUi16(p + 23);
      const Byte flags = p[25];
      const unsigned first = p[28];
      const unsigned nameLen = p[29];
      if (recSize < 30 + nameLen || pos + recSize > tableSize)
        return S_FALSE;
      pos += recSize;

      const bool multi = _volumes.Size() > 1 || first != 0;
      // single archives say volume 0
      const unsigned firstVol = multi ? first : 1;
      const unsigned lastVol = multi ? last : 1;
      if (firstVol == 0)
      {
        // in a volume after this one
        missing++;
        continue;
      }
      if (lastVol < firstVol)
        return S_FALSE;

      CRecordVector<CSegment> segs;
      UInt64 left = packSize;
      bool ok = true;
      for (unsigned v = firstVol; v <= lastVol && left != 0; v++)
      {
        if (v > _volumes.Size() || !_volumes[v - 1].Present)
        {
          ok = false;
          break;
        }
        const UInt32 start = (v == firstVol) ? offset : kDataStart;
        const UInt32 end = _volumes[v - 1].Table;
        if (start < kDataStart || start > end)
          return S_FALSE;
        CSegment seg;
        seg.Volume = v;
        seg.Offset = start;
        seg.Size = MyMin((UInt64)(end - start), left);
        if (v == lastVol && seg.Size != left)
          return S_FALSE;
        left -= seg.Size;
        segs.Add(seg);
      }
      if (!ok)
      {
        missing++;
        continue;
      }
      if (left != 0)
        return S_FALSE;

      CItem &item = _items.AddNew();
      item.Path = MultiByteToUnicodeString(d.Name, CP_ACP);
      if (!item.Path.IsEmpty())
        item.Path.Add_Char('/');
      item.Path += Name(p + 30, nameLen);
      FILETIME ft;
      if (NTime::DosTime_To_FileTime((dosTime << 16) | (dosTime >> 16), ft))
      {
        item.MTime = ft;
        item.MTime_Defined = true;
      }
      if (flags & kFlag_Stored)
      {
        if (size != packSize)
          return S_FALSE;
        FOR_VECTOR (k, segs)
          item.AddPiece(segs[k].Offset, segs[k].Size, segs[k].Size, kMethod_Stored, 0, (Byte)segs[k].Volume);
      }
      else
      {
        // a DCL stream can't grow data much more than this
        if (size > (UInt64)packSize * 1024 + (1 << 16))
          return S_FALSE;
        if (size != 0)
          item.AddPiece(0, 0, size, kMethod_Custom, _segments.Size(), (Byte)segs[0].Volume);
        item.PackSize = packSize;
        item.Method = "Implode";
        _segments.Add(segs);
      }
    }
  }
  return S_OK;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  Byte h[kHeaderSize];
  UInt64 size;
  RINOK(ReadHeader(stream, h, size))
  const bool multi = GetUi16(h + 10) != 0;
  const unsigned number = h[31];
  UInt64 phySize = size;

  // the set's name: DATA.1, DATA.2 ...
  UString base;
  if (multi)
  {
    base = GetArcName(callback);
    const int dot = base.ReverseFind_Dot();
    if (dot >= 0)
      base.DeleteFrom((unsigned)dot + 1);
    else
      base.Empty();
  }

  CMyComPtr<IInStream> first;
  unsigned total = 1;
  if (!multi || number == 1)
  {
    first = stream;
    if (multi)
      total = h[30];
  }
  else if (number != 0 && !base.IsEmpty())
  {
    UString name = base;
    name.Add_UInt32(1);
    RINOK(OpenSiblingFile(callback, name, first))
    Byte h1[kHeaderSize];
    UInt64 size1;
    if (first && ReadHeader(first, h1, size1) == S_OK && h1[31] == 1 && GetUi16(h1 + 10) != 0)
      total = h1[30];
    else
      first.Release();
  }
  if (total == 0 || (multi && total < number))
    total = number;
  if (total == 0)
    total = 1;
  if (total > kMaxVolumes)
    return S_FALSE;

  // without the first volume, the number of volumes isn't known: as many
  // as there are after this one
  const bool probe = multi && !first;
  if (probe)
    total = kMaxVolumes;
  for (unsigned v = 1; v <= total; v++)
  {
    CMyComPtr<IInStream> s;
    if (v == number || (!multi && v == 1))
      s = stream;
    else if (v == 1)
      s = first;
    else if (!base.IsEmpty())
    {
      UString name = base;
      name.Add_UInt32(v);
      RINOK(OpenSiblingFile(callback, name, s))
    }
    if (probe && !s && v > number)
      break;
    CVolume &vol = _volumes.AddNew();
    Byte hv[kHeaderSize];
    UInt64 sizeV;
    if (s && ReadHeader(s, hv, sizeV) == S_OK && (!multi || hv[31] == v))
    {
      vol.Present = true;
      vol.Table = GetUi32(hv + 41);
      vol.Size = sizeV;
    }
    else
      s.Release();
    _extraStreams.Add(s);
  }

  total = _volumes.Size();
  std::map<std::string, bool> folders;
  unsigned missing = 0;
  // the first volume's table, else the latest one there is
  unsigned source = 0;
  for (unsigned v = 1; v <= total; v++)
    if (_volumes[v - 1].Present && (source != 1))
      source = v;
  if (source == 0)
    return S_FALSE;
  RINOK(ReadTable(source, folders, missing))

  for (std::map<std::string, bool>::const_iterator it = folders.begin(); it != folders.end(); ++it)
    if (!it->second)
    {
      CItem &item = _items.AddNew();
      item.Path = MultiByteToUnicodeString(AString(it->first.c_str()), CP_ACP);
      item.IsDir = true;
    }

  _comment = "InstallShield 3";
  if (multi)
  {
    _comment += ", ";
    _comment.Add_UInt32(total);
    _comment += " volumes";
  }
  unsigned absent = 0;
  FOR_VECTOR (i, _volumes)
    if (!_volumes[i].Present)
      absent++;
  if (probe || absent != 0 || missing != 0)
  {
    _warning = "Missing volumes of the set";
    if (missing != 0)
    {
      _warning += ": ";
      _warning.Add_UInt32(missing);
      _warning += " files left out";
    }
  }
  _phySize = phySize;
  return S_OK;
}

struct CBlastIn
{
  CHandler *Handler;
  const CRecordVector<CSegment> *Segments;
  unsigned Index;
  UInt64 Pos;
  HRESULT Result;
  Byte Buf[1 << 15];
};

struct CBlastOut
{
  Byte *Dest;
  size_t Size;
  size_t Written;
};

static unsigned BlastIn(void *how, unsigned char **buf)
{
  CBlastIn &in = *(CBlastIn *)how;
  while (in.Index < in.Segments->Size())
  {
    const CSegment &seg = (*in.Segments)[in.Index];
    if (in.Pos >= seg.Size)
    {
      in.Index++;
      in.Pos = 0;
      continue;
    }
    IInStream *s = in.Handler->VolumeStream(seg.Volume);
    const size_t n = (size_t)MyMin((UInt64)sizeof(in.Buf), seg.Size - in.Pos);
    if (!s)
    {
      in.Result = S_FALSE;
      return 0;
    }
    const HRESULT r = ReadAt(s, seg.Offset + in.Pos, in.Buf, n);
    if (r != S_OK)
    {
      in.Result = r;
      return 0;
    }
    in.Pos += n;
    *buf = in.Buf;
    return (unsigned)n;
  }
  return 0;
}

static int BlastOut(void *how, unsigned char *buf, unsigned len)
{
  CBlastOut &out = *(CBlastOut *)how;
  if (len > out.Size - out.Written)
    return 1;
  memcpy(out.Dest + out.Written, buf, len);
  out.Written += len;
  return 0;
}

HRESULT CHandler::DecodePiece(const CItem &, const CPiece &piece, CByteBuffer &, Byte *dest)
{
  if (piece.Aux >= _segments.Size())
    return k_Hr_DataError;
  CByteBuffer inBuf(sizeof(CBlastIn));
  CBlastIn &in = *(CBlastIn *)(Byte *)inBuf;
  in.Handler = this;
  in.Segments = &_segments[(unsigned)piece.Aux];
  in.Index = 0;
  in.Pos = 0;
  in.Result = S_OK;
  CBlastOut out;
  out.Dest = dest;
  out.Size = (size_t)piece.Size;
  out.Written = 0;
  const int r = blast(BlastIn, &in, BlastOut, &out, NULL, NULL);
  if (in.Result != S_OK)
    return in.Result == S_FALSE ? k_Hr_DataError : in.Result;
  if (r != 0 || out.Written != out.Size)
    return k_Hr_DataError;
  return S_OK;
}

REGISTER_ARC_I(
  "InstallShield3", "z", NULL, 0xA6,
  k_Signature,
  0,
  0,
  NULL)

}}
