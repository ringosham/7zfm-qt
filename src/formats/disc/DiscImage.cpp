// DiscImage.cpp -- 7zfm-qt: see DiscImage.h.

#include "DiscImage.h"

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"

#include "Windows/PropVariant.h"

#include "7zip/Common/ProgressUtils.h"
#include "7zip/Common/StreamObjects.h"
#include "7zip/Common/StreamUtils.h"
#include "7zip/Compress/CopyCoder.h"
#include "7zip/PropID.h"

#include <zlib.h>

#include <stdio.h>
#include <string.h>

using namespace NWindows;

namespace NArchive {
namespace NDisc {

const Byte kSync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

UInt32 GetBe16(const Byte *p) { return ((UInt32)p[0] << 8) | p[1]; }
UInt32 GetBe32(const Byte *p) { return ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) | ((UInt32)p[2] << 8) | p[3]; }
UInt64 GetBe64(const Byte *p) { return ((UInt64)GetBe32(p) << 32) | GetBe32(p + 4); }
UInt32 GetLe16(const Byte *p) { return (UInt32)p[0] | ((UInt32)p[1] << 8); }
UInt32 GetLe32(const Byte *p) { return (UInt32)p[0] | ((UInt32)p[1] << 8) | ((UInt32)p[2] << 16) | ((UInt32)p[3] << 24); }
UInt64 GetLe64(const Byte *p) { return GetLe32(p) | ((UInt64)GetLe32(p + 4) << 32); }
void SetLe32(Byte *p, UInt32 v) { p[0] = (Byte)v; p[1] = (Byte)(v >> 8); p[2] = (Byte)(v >> 16); p[3] = (Byte)(v >> 24); }

unsigned SectorFormatSize(ESectorFormat f)
{
  switch (f)
  {
    case kRaw2352: return 2352;
    case kCooked2048: return 2048;
    case kMode2_2336: return 2336;
    case kForm2_2324: return 2324;
    case kWhole: return 2352;
  }
  return 2352;
}

unsigned CTrack::DataOffset() const
{
  switch (Format)
  {
    case kRaw2352: return Mode == kMode2 ? 24 : 16;
    case kMode2_2336: return 8;
    default: return 0;
  }
}

// ---------- EDC and ECC (ECMA-130 annexes A and B) ----------

static UInt32 g_EdcTable[256];
static Byte g_EccF[256];
static Byte g_EccB[256];

static struct CTablesInit
{
  CTablesInit()
  {
    for (UInt32 i = 0; i < 256; i++)
    {
      UInt32 e = i;
      for (int j = 0; j < 8; j++)
        e = (e >> 1) ^ ((e & 1) ? 0xD8018001 : 0);
      g_EdcTable[i] = e;
      const UInt32 f = (i << 1) ^ ((i & 0x80) ? 0x11D : 0);
      g_EccF[i] = (Byte)f;
      g_EccB[i ^ f] = (Byte)i;
    }
  }
} g_TablesInit;

UInt32 Edc(const Byte *p, size_t size, UInt32 e)
{
  for (size_t i = 0; i < size; i++)
    e = (e >> 8) ^ g_EdcTable[(e ^ p[i]) & 0xFF];
  return e;
}

bool EdcIsOk(const Byte *s, ETrackMode mode)
{
  if (memcmp(s, kSync, 12) != 0)
    return true; // not a data sector after all (a pregap of zeros)
  if (mode == kMode1)
    return Edc(s, 2064) == GetLe32(s + 2064);
  if (mode == kMode2)
  {
    if (s[18] & 0x20) // form 2: EDC is optional (zero when absent)
    {
      const UInt32 stored = GetLe32(s + 2348);
      return stored == 0 || Edc(s + 16, 2332) == stored;
    }
    return Edc(s + 16, 2056) == GetLe32(s + 2072);
  }
  return true;
}

static void EccBlock(const Byte *src, UInt32 majorCount, UInt32 minorCount,
    UInt32 majorMult, UInt32 minorInc, Byte *dest)
{
  const UInt32 size = majorCount * minorCount;
  for (UInt32 major = 0; major < majorCount; major++)
  {
    UInt32 index = (major >> 1) * majorMult + (major & 1);
    Byte a = 0, b = 0;
    for (UInt32 minor = 0; minor < minorCount; minor++)
    {
      const Byte t = src[index];
      index += minorInc;
      if (index >= size)
        index -= size;
      a ^= t;
      b ^= t;
      a = g_EccF[a];
    }
    a = g_EccB[g_EccF[a] ^ b];
    dest[major] = a;
    dest[major + majorCount] = (Byte)(a ^ b);
  }
}

void EccGenerate(Byte *s)
{
  // mode 2 form 1: computed with the header as zeros
  Byte header[4];
  const bool mode2 = s[15] == 2;
  if (mode2)
  {
    memcpy(header, s + 12, 4);
    memset(s + 12, 0, 4);
  }
  EccBlock(s + 12, 86, 24, 2, 86, s + 0x81C);
  EccBlock(s + 12, 52, 43, 86, 88, s + 0x8C8);
  if (mode2)
    memcpy(s + 12, header, 4);
}

void EdcEccGenerate(Byte *s)
{
  if (s[15] == 1)
  {
    SetLe32(s + 0x810, Edc(s, 0x810));
    memset(s + 0x814, 0, 8);
    EccGenerate(s);
  }
  else if (s[15] == 2)
  {
    if (s[18] & 0x20)
      SetLe32(s + 0x92C, Edc(s + 16, 0x91C));
    else
    {
      SetLe32(s + 0x818, Edc(s + 16, 0x808));
      EccGenerate(s);
    }
  }
}

// ---------- CDisc ----------

void CDisc::NameTracks(const UString &baseName)
{
  FOR_VECTOR (i, Tracks)
  {
    CTrack &t = Tracks[i];
    const char *ext = t.Mode == kAudio ? ".wav" : t.IsData() ? ".iso" : ".bin";
    if (Tracks.Size() == 1)
      t.Name = baseName.IsEmpty() ? UString(L"image") : baseName;
    else
    {
      char s[32];
      snprintf(s, sizeof(s), "Track%02u", (unsigned)t.Number);
      t.Name = s;
    }
    t.Name += ext;
  }
}

HRESULT CDisc::ReadSector(const CTrack &t, UInt32 index, Byte *main, bool &ok)
{
  ok = false;
  if (index >= t.NumSectors)
    return S_OK;
  IInStream *s = Sources[t.Source];
  RINOK(InStream_SeekSet(s, t.Start + (UInt64)index * t.Stride))
  size_t size = t.MainSize();
  RINOK(ReadStream(s, main, &size))
  ok = (size == t.MainSize());
  return S_OK;
}

int CDisc::FindDataTrack(UInt32 lba) const
{
  FOR_VECTOR (i, Tracks)
  {
    const CTrack &t = Tracks[i];
    if (t.IsData() && lba >= t.Lba && lba - t.Lba < t.NumSectors)
      return (int)i;
  }
  return -1;
}

void CDisc::DecideViews()
{
  Byte s[kRawSectorSize];
  FOR_VECTOR (i, Tracks)
  {
    CTrack &t = Tracks[i];
    t.LbaView = false;
    if (!t.IsData() || t.Lba == 0)
      continue;
    // the primary volume descriptor, from sector 16 on
    for (UInt32 k = 16; k < 32; k++)
    {
      bool ok;
      if (ReadSector(t, k, s, ok) != S_OK || !ok)
        break;
      const Byte *d = s + t.DataOffset();
      if (memcmp(d + 1, "CD001", 5) != 0)
        break;
      if (d[0] == 0xFF)
        break;
      if (d[0] == 1)
      {
        // the root directory's extent
        t.LbaView = GetLe32(d + 156 + 2) >= t.Lba;
        break;
      }
    }
  }
}

// an .iso view of a data track not at LBA 0 runs to the end of the disc's
// last data track
static UInt64 IsoViewSectors(const CDisc &d, const CTrack &t)
{
  if (!t.LbaView)
    return t.NumSectors;
  UInt64 end = (UInt64)t.Lba + t.NumSectors;
  FOR_VECTOR (i, d.Tracks)
  {
    const CTrack &o = d.Tracks[i];
    if (o.IsData() && o.Lba >= t.Lba)
      end = MyMax(end, (UInt64)o.Lba + o.NumSectors);
  }
  return end;
}

UInt64 CDisc::ItemSize(unsigned i) const
{
  const CTrack &t = Tracks[i];
  if (t.Mode == kAudio)
    return 44 + (UInt64)t.NumSectors * kRawSectorSize;
  if (t.IsData())
    return IsoViewSectors(*this, t) * kDataSize;
  return (UInt64)t.NumSectors * t.MainSize();
}

// ---------- the stream of a track ----------

Z7_CLASS_IMP_IInStream(CTrackStream)
public:
  std::shared_ptr<CDisc> Disc;
  unsigned TrackIndex;
  bool CheckEdc;
  bool EdcError;
  bool ReadError;
private:
  Byte _header[44];
  Byte _sector[kRawSectorSize];
  UInt64 _sectorIndex;    // of the item's unit in _sector, or (UInt64)-1
  unsigned _unit;         // bytes of item data per sector
  unsigned _unitOffset;   // where they are in a sector's main bytes
  unsigned _headerSize;
  UInt64 _pos;
  UInt64 _size;
  // a run of raw sectors, read at once
  CByteBuffer _run;
  UInt32 _runFirst, _runNum;
  static const UInt32 kRunSectors = 32;

  HRESULT LoadTrackSector(const CTrack &t, UInt32 index, Byte *dest, bool &ok);
public:
  void Init(const std::shared_ptr<CDisc> &disc, unsigned trackIndex)
  {
    Disc = disc;
    TrackIndex = trackIndex;
    CheckEdc = false;
    EdcError = false;
    ReadError = false;
    _sectorIndex = (UInt64)(Int64)-1;
    _pos = 0;
    _runFirst = _runNum = 0;
    const CTrack &t = disc->Tracks[trackIndex];
    _size = disc->ItemSize(trackIndex);
    _headerSize = 0;
    if (t.Mode == kAudio)
    {
      _unit = kRawSectorSize;
      _unitOffset = 0;
      _headerSize = 44;
      const UInt32 dataSize = (UInt32)((UInt64)t.NumSectors * kRawSectorSize);
      Byte *h = _header;
      memcpy(h, "RIFF", 4); SetLe32(h + 4, dataSize + 36);
      memcpy(h + 8, "WAVEfmt ", 8); SetLe32(h + 16, 16);
      h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
      SetLe32(h + 24, 44100); SetLe32(h + 28, 44100 * 4);
      h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
      memcpy(h + 36, "data", 4); SetLe32(h + 40, dataSize);
    }
    else if (t.IsData())
    {
      _unit = kDataSize;
      _unitOffset = t.DataOffset();
    }
    else
    {
      _unit = t.MainSize();
      _unitOffset = 0;
    }
  }
private:
  HRESULT LoadUnit(UInt64 index);
};

HRESULT CTrackStream::LoadTrackSector(const CTrack &t, UInt32 index, Byte *dest, bool &ok)
{
  ok = false;
  if (index >= t.NumSectors)
    return S_OK;
  const unsigned main = t.MainSize();
  // the item's own track goes through a run buffer, for sequential reads
  if (&t == &Disc->Tracks[TrackIndex])
  {
    if (index < _runFirst || index >= _runFirst + _runNum)
    {
      UInt32 num = MyMin(kRunSectors, t.NumSectors - index);
      if (_run.Size() < (size_t)kRunSectors * t.Stride)
        _run.Alloc((size_t)kRunSectors * t.Stride);
      _runNum = 0;
      IInStream *s = Disc->Sources[t.Source];
      RINOK(InStream_SeekSet(s, t.Start + (UInt64)index * t.Stride))
      size_t size = (size_t)num * t.Stride;
      RINOK(ReadStream(s, _run, &size))
      // the last sector of a source may lack its subchannel bytes
      UInt32 got = (UInt32)(size / t.Stride);
      if (got < num && size - (size_t)got * t.Stride >= main)
        got++;
      _runFirst = index;
      _runNum = got;
      if (got != num)
        ReadError = true;
      if (CheckEdc && t.Format == kRaw2352)
        for (UInt32 i = 0; i < got; i++)
          if (!EdcIsOk(_run + (size_t)i * t.Stride, t.Mode))
            EdcError = true;
    }
    if (index >= _runFirst + _runNum)
      return S_OK;
    memcpy(dest, _run + (size_t)(index - _runFirst) * t.Stride, main);
    ok = true;
    return S_OK;
  }
  return Disc->ReadSector(t, index, dest, ok);
}

HRESULT CTrackStream::LoadUnit(UInt64 index)
{
  if (index == _sectorIndex)
    return S_OK;
  _sectorIndex = (UInt64)(Int64)-1;
  const CTrack &t = Disc->Tracks[TrackIndex];
  bool ok = false;
  if (t.IsData() && t.LbaView)
  {
    // the disc from LBA 0, with this track's head in front
    if (index < t.Lba)
    {
      RINOK(LoadTrackSector(t, (UInt32)index, _sector, ok))
    }
    else
    {
      const int d = Disc->FindDataTrack((UInt32)index);
      if (d >= 0)
      {
        const CTrack &dt = Disc->Tracks[(unsigned)d];
        RINOK(LoadTrackSector(dt, (UInt32)index - dt.Lba, _sector, ok))
        if (ok && &dt != &t)
        {
          // another track's sector: its data where this track's would be
          const unsigned from = dt.DataOffset();
          if (from != _unitOffset)
            memmove(_sector + _unitOffset, _sector + from, kDataSize);
        }
      }
    }
    if (!ok)
      memset(_sector, 0, sizeof(_sector));
  }
  else
  {
    RINOK(LoadTrackSector(t, (UInt32)index, _sector, ok))
    if (!ok)
      return S_FALSE;
  }
  _sectorIndex = index;
  return S_OK;
}

Z7_COM7F_IMF(CTrackStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  UInt32 done = 0;
  Byte *dest = (Byte *)data;
  const CTrack &t = Disc->Tracks[TrackIndex];
  while (size != 0 && _pos < _size)
  {
    UInt32 cur;
    if (_pos < _headerSize)
    {
      cur = MyMin(size, (UInt32)(_headerSize - _pos));
      memcpy(dest, _header + _pos, cur);
    }
    else
    {
      const UInt64 p = _pos - _headerSize;
      const UInt64 index = p / _unit;
      const unsigned offset = (unsigned)(p % _unit);
      const HRESULT res = LoadUnit(index);
      if (res == S_FALSE)
        break; // the source is shorter than the track
      RINOK(res)
      cur = MyMin(size, (UInt32)(_unit - offset));
      const Byte *src = _sector + _unitOffset + offset;
      if (t.Mode == kAudio && t.SwapAudio)
      {
        // big-endian samples; (offset) and (cur) may split a sample
        for (UInt32 i = 0; i < cur; i++)
          dest[i] = _sector[(offset + i) ^ 1];
      }
      else
        memcpy(dest, src, cur);
    }
    dest += cur;
    size -= cur;
    done += cur;
    _pos += cur;
  }
  if (processedSize)
    *processedSize = done;
  return S_OK;
}

Z7_COM7F_IMF(CTrackStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  switch (seekOrigin)
  {
    case STREAM_SEEK_SET: break;
    case STREAM_SEEK_CUR: offset += (Int64)_pos; break;
    case STREAM_SEEK_END: offset += (Int64)_size; break;
    default: return STG_E_INVALIDFUNCTION;
  }
  if (offset < 0)
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  _pos = (UInt64)offset;
  if (newPosition)
    *newPosition = _pos;
  return S_OK;
}

CTrackStream *NewTrackStream(const std::shared_ptr<CDisc> &disc, unsigned trackIndex)
{
  CTrackStream *s = new CTrackStream;
  s->Init(disc, trackIndex);
  return s;
}
bool TrackStream_EdcError(const CTrackStream *s) { return s->EdcError; }
bool TrackStream_ReadError(const CTrackStream *s) { return s->ReadError; }
void TrackStream_SetCheckEdc(CTrackStream *s) { s->CheckEdc = true; }

// ---------- CTrackHandler ----------

static const Byte kTrackProps[] = { kpidPath, kpidSize, kpidPackSize, kpidMethod };
static const Byte kTrackArcProps[] = { kpidComment, kpidNumBlocks };

Z7_COM7F_IMF(CTrackHandler::GetNumberOfProperties(UInt32 *numProps))
{
  *numProps = Z7_ARRAY_SIZE(kTrackProps);
  return S_OK;
}
Z7_COM7F_IMF(CTrackHandler::GetPropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
{
  if (index >= Z7_ARRAY_SIZE(kTrackProps))
    return E_INVALIDARG;
  *propID = kTrackProps[index];
  *varType = k7z_PROPID_To_VARTYPE[(unsigned)*propID];
  *name = NULL;
  return S_OK;
}
Z7_COM7F_IMF(CTrackHandler::GetNumberOfArchiveProperties(UInt32 *numProps))
{
  *numProps = Z7_ARRAY_SIZE(kTrackArcProps);
  return S_OK;
}
Z7_COM7F_IMF(CTrackHandler::GetArchivePropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
{
  if (index >= Z7_ARRAY_SIZE(kTrackArcProps))
    return E_INVALIDARG;
  *propID = kTrackArcProps[index];
  *varType = k7z_PROPID_To_VARTYPE[(unsigned)*propID];
  *name = NULL;
  return S_OK;
}

Z7_COM7F_IMF(CTrackHandler::Open(IInStream *stream, const UInt64 *, IArchiveOpenCallback *callback))
{
  COM_TRY_BEGIN
  Close();
  _disc = std::make_shared<CDisc>();
  _stream = stream;
  HRESULT res = Open2(stream, callback);
  if (res == S_OK && _disc->Tracks.IsEmpty())
    res = S_FALSE;
  if (res == S_OK)
    _disc->DecideViews();
  if (res != S_OK)
  {
    Close();
    return res;
  }
  if (_disc->PhySize == 0)
  {
    UInt64 size = 0;
    if (StreamSize(stream, size) == S_OK)
      _disc->PhySize = size;
  }
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CTrackHandler::Close())
{
  _disc.reset();
  _stream.Release();
  return S_OK;
}

Z7_COM7F_IMF(CTrackHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _disc ? _disc->Tracks.Size() : 0;
  return S_OK;
}

Z7_COM7F_IMF(CTrackHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  if (_disc)
    switch (propID)
    {
      case kpidPhySize: prop = _disc->PhySize; break;
      case kpidComment: if (!_disc->Comment.IsEmpty()) prop = _disc->Comment; break;
      case kpidNumBlocks: prop = (UInt32)_disc->Tracks.Size(); break;
      // a single data track: open its file system right away
      case kpidMainSubfile:
        if (_disc->Tracks.Size() == 1 && (_disc->Tracks[0].IsData() || _disc->OpenSingleTrack))
          prop = (UInt32)0;
        break;
      default: break;
    }
  prop.Detach(value);
  return S_OK;
}

static const char *TrackMethod(const CTrack &t)
{
  if (t.Mode == kAudio)
    return "Audio";
  const bool m2 = t.Mode == kMode2;
  switch (t.Format)
  {
    case kRaw2352: return m2 ? "Mode2/2352" : t.Mode == kMode1 ? "Mode1/2352" : "Raw/2352";
    case kCooked2048: return m2 ? "Mode2/2048" : "Mode1/2048";
    case kMode2_2336: return "Mode2/2336";
    case kForm2_2324: return "Mode2/2324";
    case kWhole: return "Raw";
  }
  return "";
}

Z7_COM7F_IMF(CTrackHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  const CTrack &t = _disc->Tracks[index];
  switch (propID)
  {
    case kpidPath: prop = t.Name; break;
    case kpidSize: prop = _disc->ItemSize(index); break;
    case kpidPackSize: prop = (UInt64)t.NumSectors * t.Stride; break;
    case kpidMethod: prop = TrackMethod(t); break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CTrackHandler::GetStream(UInt32 index, ISequentialInStream **stream))
{
  COM_TRY_BEGIN
  *stream = NULL;
  if (!_disc || index >= _disc->Tracks.Size())
    return E_INVALIDARG;
  CMyComPtr<IInStream> s = NewTrackStream(_disc, index);
  *stream = s.Detach();
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CTrackHandler::Extract(const UInt32 *indices, UInt32 numItems,
    Int32 testMode, IArchiveExtractCallback *extractCallback))
{
  COM_TRY_BEGIN
  const bool allFilesMode = (numItems == (UInt32)(Int32)-1);
  if (allFilesMode)
    numItems = _disc->Tracks.Size();
  if (numItems == 0)
    return S_OK;
  UInt64 totalSize = 0;
  for (UInt32 i = 0; i < numItems; i++)
    totalSize += _disc->ItemSize(allFilesMode ? i : indices[i]);
  RINOK(extractCallback->SetTotal(totalSize))

  NCompress::CCopyCoderBase copyCoder;
  CMyComPtr2_Create<ICompressProgressInfo, CLocalProgress> lps;
  lps->Init(extractCallback, false);
  UInt64 done = 0;

  for (UInt32 i = 0; i < numItems; i++)
  {
    lps->InSize = lps->OutSize = done;
    RINOK(lps->SetCur())
    const UInt32 index = allFilesMode ? i : indices[i];
    const UInt64 size = _disc->ItemSize(index);
    CMyComPtr<ISequentialOutStream> outStream;
    const Int32 askMode = testMode ?
        NExtract::NAskMode::kTest :
        NExtract::NAskMode::kExtract;
    RINOK(extractCallback->GetStream(index, &outStream, askMode))
    if (!testMode && !outStream)
    {
      done += size;
      continue;
    }
    RINOK(extractCallback->PrepareOperation(askMode))
    CTrackStream *spec = NewTrackStream(_disc, index);
    CMyComPtr<IInStream> in = spec;
    TrackStream_SetCheckEdc(spec);
    *_disc->SourceError = false;
    RINOK(copyCoder.Code(in, outStream, NULL, lps))
    outStream.Release();
    done += size;
    Int32 opRes = NExtract::NOperationResult::kOK;
    if (copyCoder.TotalSize != size || TrackStream_ReadError(spec))
      opRes = NExtract::NOperationResult::kUnexpectedEnd;
    else if (TrackStream_EdcError(spec) || *_disc->SourceError)
      opRes = NExtract::NOperationResult::kCRCError;
    RINOK(extractCallback->SetOperationResult(opRes))
  }
  lps->InSize = lps->OutSize = done;
  return lps->SetCur();
  COM_TRY_END
}

// ---------- opening helpers ----------

UString GetArcName(IArchiveOpenCallback *callback)
{
  UString name;
  Z7_DECL_CMyComPtr_QI_FROM(IArchiveOpenVolumeCallback, volumeCallback, callback)
  if (volumeCallback)
  {
    NCOM::CPropVariant prop;
    if (volumeCallback->GetProperty(kpidName, &prop) == S_OK && prop.vt == VT_BSTR)
      name = prop.bstrVal;
  }
  const int slash = name.ReverseFind_PathSepar();
  if (slash >= 0)
    name.DeleteFrontal((unsigned)slash + 1);
  return name;
}

UString BaseName(const UString &name)
{
  const int dot = name.ReverseFind_Dot();
  return dot > 0 ? name.Left((unsigned)dot) : name;
}

UString ReplaceExtension(const UString &name, const char *ext)
{
  UString s = BaseName(name);
  s += L'.';
  s += ext;
  return s;
}

static void MakeUpperAscii(UString &s)
{
  for (unsigned i = 0; i < s.Len(); i++)
    if (s[i] >= 'a' && s[i] <= 'z')
      s.ReplaceOneCharAtPos(i, (wchar_t)(s[i] - 'a' + 'A'));
}

static void AddUnique(UStringVector &v, const UString &s)
{
  FOR_VECTOR (i, v)
    if (v[i] == s)
      return;
  v.Add(s);
}

HRESULT OpenSibling(IArchiveOpenCallback *callback, const UString &nameIn,
    const char *fallbackExt, CMyComPtr<IInStream> &stream)
{
  stream.Release();
  Z7_DECL_CMyComPtr_QI_FROM(IArchiveOpenVolumeCallback, volumeCallback, callback)
  if (!volumeCallback)
    return S_OK;
  UString name = nameIn;
  {
    // descriptors may name the file with a path, Windows style
    const int slash = MyMax(name.ReverseFind(L'/'), name.ReverseFind(L'\\'));
    if (slash >= 0)
      name.DeleteFrontal((unsigned)slash + 1);
  }
  UStringVector names;
  if (!name.IsEmpty())
  {
    names.Add(name);
    UString lower = name; lower.MakeLower_Ascii(); AddUnique(names, lower);
    UString upper = name; MakeUpperAscii(upper); AddUnique(names, upper);
    // "game.BIN" for "game.bin"
    const int dot = name.ReverseFind_Dot();
    if (dot > 0)
    {
      UString e = name; UString ext = name.Ptr((unsigned)dot);
      ext.MakeLower_Ascii(); e.DeleteFrom((unsigned)dot); e += ext; AddUnique(names, e);
      MakeUpperAscii(ext); e.DeleteFrom((unsigned)dot); e += ext; AddUnique(names, e);
    }
  }
  if (fallbackExt)
  {
    const UString arc = GetArcName(callback);
    if (!arc.IsEmpty())
    {
      UString lower = ReplaceExtension(arc, fallbackExt);
      names.Add(lower);
      UString upperExt;
      upperExt += fallbackExt;
      MakeUpperAscii(upperExt);
      UString upper = BaseName(arc);
      upper += L'.';
      upper += upperExt;
      names.Add(upper);
    }
  }
  const UString arcName = GetArcName(callback);
  FOR_VECTOR (i, names)
  {
    if (names[i] == arcName)
      continue;
    CMyComPtr<IInStream> s;
    const HRESULT res = volumeCallback->GetStream(names[i], &s);
    if (res == S_OK && s)
    {
      stream = s;
      return S_OK;
    }
    if (res != S_OK && res != S_FALSE)
      return res;
  }
  return S_OK;
}

HRESULT StreamSize(IInStream *stream, UInt64 &size)
{
  return InStream_GetSize_SeekToEnd(stream, size);
}

HRESULT ReadWholeStream(IInStream *stream, CByteBuffer &buf, size_t maxSize)
{
  UInt64 size;
  RINOK(InStream_GetSize_SeekToBegin(stream, size))
  if (size > maxSize)
    return S_FALSE;
  buf.Alloc((size_t)size);
  return ReadStream_FALSE(stream, buf, (size_t)size);
}

// ---------- CBlockHandler ----------

Z7_CLASS_IMP_IInStream(CBlockStream)
public:
  CMyComPtr<IInArchive> HandlerRef;
  CBlockHandler *Handler;
  bool DataError;
private:
  CByteBuffer _block;
  UInt64 _blockIndex;
  UInt64 _pos;
public:
  void Init(CBlockHandler *h)
  {
    Handler = h;
    HandlerRef = h;
    DataError = false;
    _blockIndex = (UInt64)(Int64)-1;
    _pos = 0;
    _block.Alloc(h->BlockSize());
  }
};

Z7_COM7F_IMF(CBlockStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  const UInt64 total = Handler->Size();
  const UInt32 bs = Handler->BlockSize();
  UInt32 done = 0;
  while (size != 0 && _pos < total)
  {
    const UInt64 index = _pos / bs;
    const UInt32 offset = (UInt32)(_pos % bs);
    if (index != _blockIndex)
    {
      _blockIndex = (UInt64)(Int64)-1;
      const HRESULT res = Handler->ReadBlock(index, _block);
      if (res == S_FALSE)
      {
        DataError = true;
        if (done == 0)
          return S_FALSE;
        break;
      }
      RINOK(res)
      _blockIndex = index;
    }
    UInt32 cur = MyMin(size, bs - offset);
    if (cur > total - _pos)
      cur = (UInt32)(total - _pos);
    memcpy((Byte *)data + done, _block + offset, cur);
    done += cur;
    size -= cur;
    _pos += cur;
  }
  if (processedSize)
    *processedSize = done;
  return S_OK;
}

Z7_COM7F_IMF(CBlockStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  switch (seekOrigin)
  {
    case STREAM_SEEK_SET: break;
    case STREAM_SEEK_CUR: offset += (Int64)_pos; break;
    case STREAM_SEEK_END: offset += (Int64)Handler->Size(); break;
    default: return STG_E_INVALIDFUNCTION;
  }
  if (offset < 0)
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  _pos = (UInt64)offset;
  if (newPosition)
    *newPosition = _pos;
  return S_OK;
}

static const Byte kBlockProps[] = { kpidPath, kpidSize, kpidPackSize, kpidMethod };
static const Byte kBlockArcProps[] = { kpidClusterSize };

Z7_COM7F_IMF(CBlockHandler::GetNumberOfProperties(UInt32 *numProps))
{
  *numProps = Z7_ARRAY_SIZE(kBlockProps);
  return S_OK;
}
Z7_COM7F_IMF(CBlockHandler::GetPropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
{
  if (index >= Z7_ARRAY_SIZE(kBlockProps))
    return E_INVALIDARG;
  *propID = kBlockProps[index];
  *varType = k7z_PROPID_To_VARTYPE[(unsigned)*propID];
  *name = NULL;
  return S_OK;
}
Z7_COM7F_IMF(CBlockHandler::GetNumberOfArchiveProperties(UInt32 *numProps))
{
  *numProps = Z7_ARRAY_SIZE(kBlockArcProps);
  return S_OK;
}
Z7_COM7F_IMF(CBlockHandler::GetArchivePropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
{
  if (index >= Z7_ARRAY_SIZE(kBlockArcProps))
    return E_INVALIDARG;
  *propID = kBlockArcProps[index];
  *varType = k7z_PROPID_To_VARTYPE[(unsigned)*propID];
  *name = NULL;
  return S_OK;
}

Z7_COM7F_IMF(CBlockHandler::Open(IInStream *stream, const UInt64 *, IArchiveOpenCallback *callback))
{
  COM_TRY_BEGIN
  Close();
  _stream = stream;
  const HRESULT res = Open2(stream, callback);
  if (res != S_OK || _blockSize == 0)
  {
    Close();
    return res == S_OK ? S_FALSE : res;
  }
  if (_phySize == 0)
    StreamSize(stream, _phySize);
  if (_name.IsEmpty())
    _name = L"image.iso";
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CBlockHandler::Close())
{
  Close2();
  _stream.Release();
  _size = 0;
  _blockSize = 0;
  _phySize = 0;
  _name.Empty();
  _dataError = false;
  _extras.Clear();
  return S_OK;
}

Z7_COM7F_IMF(CBlockHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _blockSize ? 1 + _extras.Size() : 0;
  return S_OK;
}

Z7_COM7F_IMF(CBlockHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPhySize: prop = _phySize; break;
    case kpidClusterSize: prop = _blockSize; break;
    case kpidMainSubfile: prop = (UInt32)0; break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CBlockHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  if (index != 0)
  {
    if (index - 1 < _extras.Size())
    {
      const CExtra &e = _extras[index - 1];
      switch (propID)
      {
        case kpidPath: prop = e.Name; break;
        case kpidSize: prop = (UInt64)e.Data.Size(); break;
        default: break;
      }
    }
    prop.Detach(value);
    return S_OK;
  }
  switch (propID)
  {
    case kpidPath: prop = _name; break;
    case kpidSize: prop = _size; break;
    case kpidPackSize: prop = _phySize; break;
    case kpidMethod: if (_method) prop = _method; break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CBlockHandler::GetStream(UInt32 index, ISequentialInStream **stream))
{
  COM_TRY_BEGIN
  *stream = NULL;
  if (index != 0)
  {
    if (index - 1 >= _extras.Size())
      return E_INVALIDARG;
    CBufInStream *in = new CBufInStream;
    CMyComPtr<ISequentialInStream> inRef = in;
    in->Init(_extras[index - 1].Data, _extras[index - 1].Data.Size(), (IInArchive *)this);
    *stream = inRef.Detach();
    return S_OK;
  }
  CBlockStream *spec = new CBlockStream;
  CMyComPtr<IInStream> s = spec;
  spec->Init(this);
  *stream = s.Detach();
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CBlockHandler::Extract(const UInt32 *indices, UInt32 numItems,
    Int32 testMode, IArchiveExtractCallback *extractCallback))
{
  COM_TRY_BEGIN
  if (numItems == 0)
    return S_OK;
  const bool allFilesMode = numItems == (UInt32)(Int32)-1;
  if (allFilesMode)
    numItems = 1 + _extras.Size();
  bool image = false;
  for (UInt32 i = 0; i < numItems; i++)
  {
    const UInt32 index = allFilesMode ? i : indices[i];
    if (index > _extras.Size())
      return E_INVALIDARG;
    if (index == 0)
      image = true;
  }
  // the files that go with the image first: they are small
  for (UInt32 i = 0; i < numItems; i++)
  {
    const UInt32 index = allFilesMode ? i : indices[i];
    if (index == 0)
      continue;
    const CExtra &e = _extras[index - 1];
    CMyComPtr<ISequentialOutStream> out;
    const Int32 askMode = testMode ? NExtract::NAskMode::kTest : NExtract::NAskMode::kExtract;
    RINOK(extractCallback->GetStream(index, &out, askMode))
    if (!testMode && !out)
      continue;
    RINOK(extractCallback->PrepareOperation(askMode))
    if (out)
      RINOK(WriteStream(out, e.Data, e.Data.Size()))
    out.Release();
    RINOK(extractCallback->SetOperationResult(NExtract::NOperationResult::kOK))
  }
  if (!image)
    return S_OK;
  RINOK(extractCallback->SetTotal(_size))
  CMyComPtr<ISequentialOutStream> outStream;
  const Int32 askMode = testMode ?
      NExtract::NAskMode::kTest :
      NExtract::NAskMode::kExtract;
  RINOK(extractCallback->GetStream(0, &outStream, askMode))
  if (!testMode && !outStream)
    return S_OK;
  RINOK(extractCallback->PrepareOperation(askMode))
  CBlockStream *spec = new CBlockStream;
  CMyComPtr<IInStream> in = spec;
  spec->Init(this);
  NCompress::CCopyCoderBase copyCoder;
  CMyComPtr2_Create<ICompressProgressInfo, CLocalProgress> lps;
  lps->Init(extractCallback, false);
  const HRESULT res = copyCoder.Code(in, outStream, NULL, lps);
  if (res != S_OK && res != S_FALSE)
    return res;
  outStream.Release();
  Int32 opRes = NExtract::NOperationResult::kOK;
  if (spec->DataError)
    opRes = NExtract::NOperationResult::kDataError;
  else if (copyCoder.TotalSize != _size)
    opRes = NExtract::NOperationResult::kUnexpectedEnd;
  return extractCallback->SetOperationResult(opRes);
  COM_TRY_END
}

// ---------- inflate ----------

HRESULT Inflate(const Byte *src, size_t srcSize, Byte *dest, size_t destSize, int windowBits, size_t *written)
{
  z_stream z;
  memset(&z, 0, sizeof(z));
  if (inflateInit2(&z, windowBits) != Z_OK)
    return E_OUTOFMEMORY;
  z.next_in = (Bytef *)src;
  z.avail_in = (uInt)srcSize;
  z.next_out = dest;
  z.avail_out = (uInt)destSize;
  const int r = inflate(&z, Z_FINISH);
  const size_t out = destSize - z.avail_out;
  inflateEnd(&z);
  if (written)
    *written = out;
  if (r != Z_STREAM_END && !(r == Z_BUF_ERROR && out == destSize && written))
    return S_FALSE;
  if (!written && out != destSize)
    return S_FALSE;
  return S_OK;
}

}}
