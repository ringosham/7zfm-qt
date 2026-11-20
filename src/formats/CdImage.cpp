// CdImage.cpp -- 7zfm-qt: raw CD images (.bin, with or without a .cue), as
// the format "CD". Upstream's ISO handler reads only 2048-byte sectors, the
// layout of an .iso; a .bin keeps whole 2352-byte sectors: a sync pattern
// and header, the 2048 bytes of data, and error-detection codes (EDC/ECC).
// Audio tracks have no sectors of that kind, only 16-bit stereo samples.
//
// Each track is an item. A data track reads as its 2048-byte payloads, which
// upstream's ISO (or UDF, HFS) handler opens in place. An audio track reads
// as a WAV file. With a single data track, kpidMainSubfile makes upstream's
// CArchiveLink open the file system right away, as it does for a VHD.
//
// The tracks come from the .cue next to the .bin (same name, through
// IArchiveOpenVolumeCallback, as split archives find their volumes). Without
// one, the data track ends at the last sector with a sync pattern, and the
// rest is one audio item.
//
// Extract and Test check each data sector's EDC, and report a CRC error for
// any that doesn't match (Test is how to check a .bin for damage).

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "Common/StringToInt.h"
#include "Common/UTFConvert.h"

#include "Windows/PropVariant.h"

#include "7zip/Archive/IArchive.h"
#include "7zip/Common/ProgressUtils.h"
#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"
#include "7zip/Compress/CopyCoder.h"
#include "7zip/PropID.h"

#include <stdio.h>
#include <string.h>

using namespace NWindows;

namespace NArchive {
namespace NCdImage {

static const unsigned kRawSectorSize = 2352;
static const unsigned kDataSize = 2048;
static const unsigned kWavHeaderSize = 44;
static const UInt32 kFramesPerSecond = 75;

static const Byte kSync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

enum ETrackMode
{
  kAudio,
  kMode1,      // MODE1/2352
  kMode2,      // MODE2/2352 (XA form 1 data)
  kMode1Data,  // MODE1/2048: payloads only, as in an .iso
  kMode2Raw    // MODE2/2336: no sync and header
};

struct CTrack
{
  UInt32 Number;
  ETrackMode Mode;
  UInt64 Start;       // byte offset of INDEX 01 in the .bin
  UInt32 NumSectors;
  UString Name;

  unsigned RawSize() const
  {
    switch (Mode)
    {
      case kMode1Data: return kDataSize;
      case kMode2Raw: return 2336;
      default: return kRawSectorSize;
    }
  }
  // where the 2048 data bytes are in a raw sector
  unsigned DataOffset() const
  {
    switch (Mode)
    {
      case kMode1: return 16;
      case kMode2: return 24;
      case kMode2Raw: return 8;
      default: return 0;
    }
  }
  unsigned ItemDataSize() const { return Mode == kAudio ? kRawSectorSize : kDataSize; }
  unsigned HeaderSize() const { return Mode == kAudio ? kWavHeaderSize : 0; }
  UInt64 Size() const { return HeaderSize() + (UInt64)NumSectors * ItemDataSize(); }
  UInt64 PackSize() const { return (UInt64)NumSectors * RawSize(); }
  const char *MethodName() const
  {
    switch (Mode)
    {
      case kAudio: return "AUDIO";
      case kMode1: return "MODE1/2352";
      case kMode2: return "MODE2/2352";
      case kMode1Data: return "MODE1/2048";
      case kMode2Raw: return "MODE2/2336";
    }
    return "";
  }
};

// ---------- EDC (ECMA-130 annex A: CRC-32 over x^32+x^31+x^16+x^15+x^4+x^3+x+1) ----------

static UInt32 g_EdcTable[256];

static struct CEdcTableInit
{
  CEdcTableInit()
  {
    for (UInt32 i = 0; i < 256; i++)
    {
      UInt32 e = i;
      for (int j = 0; j < 8; j++)
        e = (e >> 1) ^ ((e & 1) ? 0xD8018001 : 0);
      g_EdcTable[i] = e;
    }
  }
} g_EdcTableInit;

static UInt32 Edc(const Byte *p, size_t size)
{
  UInt32 e = 0;
  for (size_t i = 0; i < size; i++)
    e = (e >> 8) ^ g_EdcTable[(e ^ p[i]) & 0xFF];
  return e;
}

static UInt32 GetUi32(const Byte *p)
{
  return (UInt32)p[0] | ((UInt32)p[1] << 8) | ((UInt32)p[2] << 16) | ((UInt32)p[3] << 24);
}

static void SetUi32(Byte *p, UInt32 v)
{
  p[0] = (Byte)v; p[1] = (Byte)(v >> 8); p[2] = (Byte)(v >> 16); p[3] = (Byte)(v >> 24);
}

// false only for a sector whose stored EDC doesn't match its data
static bool EdcIsOk(const Byte *s, ETrackMode mode)
{
  if (mode == kMode1)
    return Edc(s, 2064) == GetUi32(s + 2064);
  if (mode == kMode2)
  {
    if (s[18] & 0x20) // form 2: EDC is optional (zero when absent)
    {
      const UInt32 stored = GetUi32(s + 2348);
      return stored == 0 || Edc(s + 16, 2332) == stored;
    }
    return Edc(s + 16, 2056) == GetUi32(s + 2072);
  }
  return true;
}

// ---------- a track's contents as a stream ----------

Z7_CLASS_IMP_IInStream(CTrackStream)
public:
  CMyComPtr<IInStream> Base;
  CTrack Track;
  bool CheckEdc;
  bool EdcError;
  bool ReadError;
private:
  Byte _header[kWavHeaderSize];
  CByteBuffer _cache;
  UInt32 _cacheFirst;  // first sector in _cache
  UInt32 _cacheNum;
  UInt64 _pos;
  UInt64 _size;
  static const UInt32 kCacheSectors = 64;
public:
  void Init(IInStream *base, const CTrack &track)
  {
    Base = base;
    Track = track;
    CheckEdc = false;
    EdcError = false;
    ReadError = false;
    _cacheFirst = 0;
    _cacheNum = 0;
    _pos = 0;
    _size = track.Size();
    if (track.Mode == kAudio)
    {
      // RIFF/WAVE, PCM, 2 channels, 44100 Hz, 16 bits
      const UInt32 dataSize = (UInt32)((UInt64)track.NumSectors * kRawSectorSize);
      Byte *h = _header;
      memcpy(h, "RIFF", 4); SetUi32(h + 4, dataSize + 36);
      memcpy(h + 8, "WAVEfmt ", 8); SetUi32(h + 16, 16);
      h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
      SetUi32(h + 24, 44100); SetUi32(h + 28, 44100 * 4);
      h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
      memcpy(h + 36, "data", 4); SetUi32(h + 40, dataSize);
    }
  }
private:
  HRESULT LoadSector(UInt32 sector)
  {
    if (sector >= _cacheFirst && sector < _cacheFirst + _cacheNum)
      return S_OK;
    const unsigned raw = Track.RawSize();
    UInt32 num = Track.NumSectors - sector;
    if (num > kCacheSectors)
      num = kCacheSectors;
    if (_cache.Size() < (size_t)kCacheSectors * raw)
      _cache.Alloc((size_t)kCacheSectors * raw);
    _cacheNum = 0;
    RINOK(InStream_SeekSet(Base, Track.Start + (UInt64)sector * raw))
    size_t size = (size_t)num * raw;
    RINOK(ReadStream(Base, _cache, &size))
    _cacheFirst = sector;
    _cacheNum = (UInt32)(size / raw);
    if (_cacheNum != num)
      ReadError = true;
    if (CheckEdc)
      for (UInt32 i = 0; i < _cacheNum; i++)
        if (!EdcIsOk(_cache + (size_t)i * raw, Track.Mode))
          EdcError = true;
    return S_OK;
  }
};

Z7_COM7F_IMF(CTrackStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  UInt32 done = 0;
  Byte *dest = (Byte *)data;
  const unsigned header = Track.HeaderSize();
  while (size != 0 && _pos < _size)
  {
    UInt32 cur;
    if (_pos < header)
    {
      cur = MyMin(size, (UInt32)(header - _pos));
      memcpy(dest, _header + _pos, cur);
    }
    else
    {
      const unsigned itemData = Track.ItemDataSize();
      const UInt64 p = _pos - header;
      const UInt32 sector = (UInt32)(p / itemData);
      const unsigned offset = (unsigned)(p % itemData);
      RINOK(LoadSector(sector))
      if (sector >= _cacheFirst + _cacheNum)
        break; // the .bin is shorter than the track
      cur = MyMin(size, (UInt32)(itemData - offset));
      memcpy(dest, _cache + (size_t)(sector - _cacheFirst) * Track.RawSize()
          + Track.DataOffset() + offset, cur);
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

// ---------- .cue sheets ----------

struct CCueTrack
{
  UInt32 Number;
  ETrackMode Mode;
  Int64 Index0;  // frames from the start of the .bin; -1 if none
  Int64 Index1;
};

static bool ParseMsf(const AString &s, Int64 &frames)
{
  // mm:ss:ff
  const char *p = s.Ptr();
  const char *end;
  const UInt32 m = ConvertStringToUInt32(p, &end);
  if (end == p || *end != ':')
    return false;
  p = end + 1;
  const UInt32 sec = ConvertStringToUInt32(p, &end);
  if (end == p || *end != ':' || sec >= 60)
    return false;
  p = end + 1;
  const UInt32 f = ConvertStringToUInt32(p, &end);
  if (end == p || *end != 0 || f >= kFramesPerSecond)
    return false;
  frames = ((Int64)m * 60 + sec) * kFramesPerSecond + f;
  return true;
}

// splits a line into words; a "quoted string" is one word
static void SplitCueLine(const AString &line, AStringVector &words)
{
  words.Clear();
  unsigned i = 0;
  while (i < line.Len())
  {
    while (i < line.Len() && (line[i] == ' ' || line[i] == '\t'))
      i++;
    if (i >= line.Len())
      break;
    AString w;
    if (line[i] == '"')
    {
      i++;
      while (i < line.Len() && line[i] != '"')
        w += line[i++];
      i++;
    }
    else
      while (i < line.Len() && line[i] != ' ' && line[i] != '\t')
        w += line[i++];
    words.Add(w);
  }
}

static AString FileNameOf(const AString &path)
{
  int pos = MyMax(path.ReverseFind('/'), path.ReverseFind('\\'));
  return AString(path.Ptr((unsigned)(pos + 1)));
}

struct CCueFile
{
  AString Name;
  CRecordVector<CCueTrack> Tracks;
};

// the tracks of (binName) in the .cue; false if it has none or isn't one
static bool ParseCue(const AString &text, const AString &binName, CRecordVector<CCueTrack> &tracks)
{
  CObjectVector<CCueFile> files;
  AStringVector words;
  unsigned pos = 0;
  while (pos < text.Len())
  {
    int eol = text.Find('\n', pos);
    if (eol < 0)
      eol = (int)text.Len();
    AString line;
    line.SetFrom(text.Ptr(pos), (unsigned)eol - pos);
    pos = (unsigned)eol + 1;
    line.RemoveChar('\r');
    SplitCueLine(line, words);
    if (words.IsEmpty())
      continue;
    AString cmd = words[0];
    cmd.MakeLower_Ascii();
    if (cmd == "file" && words.Size() >= 2)
      files.AddNew().Name = FileNameOf(words[1]);
    else if (cmd == "track" && words.Size() >= 3 && !files.IsEmpty())
    {
      CCueTrack t;
      t.Number = ConvertStringToUInt32(words[1], NULL);
      t.Index0 = t.Index1 = -1;
      AString mode = words[2];
      mode.MakeLower_Ascii();
      if (mode == "audio") t.Mode = kAudio;
      else if (mode == "mode1/2352") t.Mode = kMode1;
      else if (mode == "mode2/2352") t.Mode = kMode2;
      else if (mode == "mode1/2048") t.Mode = kMode1Data;
      else if (mode == "mode2/2336") t.Mode = kMode2Raw;
      else
        return false; // CDG, CDI...: not ours to guess
      files.Back().Tracks.Add(t);
    }
    else if (cmd == "index" && words.Size() >= 3 && !files.IsEmpty() && !files.Back().Tracks.IsEmpty())
    {
      Int64 frames;
      if (!ParseMsf(words[2], frames))
        return false;
      const UInt32 n = ConvertStringToUInt32(words[1], NULL);
      if (n == 0)
        files.Back().Tracks.Back().Index0 = frames;
      else if (n == 1)
        files.Back().Tracks.Back().Index1 = frames;
    }
  }
  // with a single FILE, it is this .bin even if one of them was renamed
  int found = files.Size() == 1 ? 0 : -1;
  FOR_VECTOR (i, files)
    if (found < 0 && StringsAreEqualNoCase_Ascii(files[i].Name, binName))
      found = (int)i;
  if (found < 0)
    return false;
  tracks = files[(unsigned)found].Tracks;
  FOR_VECTOR (i, tracks)
    if (tracks[i].Index1 < 0)
      return false;
  return !tracks.IsEmpty();
}

// ---------- the handler ----------

Z7_class_CHandler_final:
  public IInArchive,
  public IInArchiveGetStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(IInArchive, IInArchiveGetStream)

  CMyComPtr<IInStream> _stream;
  CObjectVector<CTrack> _tracks;
  UInt64 _phySize;
  bool _fromCue;

  HRESULT ReadSectorHeader(UInt64 pos, Byte *header);
  bool IsDataSector(UInt64 pos, ETrackMode mode);
  HRESULT TracksFromCue(IInStream *stream, IArchiveOpenCallback *callback, const UString &name);
  HRESULT TracksFromSectors();
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback);
};

static const Byte kProps[] =
{
  kpidPath,
  kpidSize,
  kpidPackSize,
  kpidMethod
};

static const Byte kArcProps[] =
{
  kpidComment
};

IMP_IInArchive_Props
IMP_IInArchive_ArcProps

HRESULT CHandler::ReadSectorHeader(UInt64 pos, Byte *header)
{
  RINOK(InStream_SeekSet(_stream, pos))
  return ReadStream_FALSE(_stream, header, 16);
}

bool CHandler::IsDataSector(UInt64 pos, ETrackMode mode)
{
  Byte h[16];
  if (ReadSectorHeader(pos, h) != S_OK)
    return false;
  if (memcmp(h, kSync, 12) != 0)
    return false;
  return h[15] == (mode == kMode2 ? 2 : 1);
}

HRESULT CHandler::TracksFromCue(IInStream *stream, IArchiveOpenCallback *callback, const UString &name)
{
  Z7_DECL_CMyComPtr_QI_FROM(IArchiveOpenVolumeCallback, volumeCallback, callback)
  if (!volumeCallback || name.IsEmpty())
    return S_FALSE;
  UStringVector cueNames;
  {
    const int dot = name.ReverseFind_Dot();
    const UString base = dot > 0 ? name.Left((unsigned)dot) : name;
    cueNames.Add(base + L".cue");
    cueNames.Add(base + L".CUE");
    cueNames.Add(name + L".cue");
  }
  CMyComPtr<IInStream> cue;
  FOR_VECTOR (i, cueNames)
  {
    if (cueNames[i] == name)
      continue;
    cue.Release();
    const HRESULT res = volumeCallback->GetStream(cueNames[i], &cue);
    if (res == S_OK && cue)
      break;
    cue.Release();
  }
  if (!cue)
    return S_FALSE;

  UInt64 cueSize;
  RINOK(InStream_GetSize_SeekToBegin(cue, cueSize))
  if (cueSize == 0 || cueSize > (1 << 16))
    return S_FALSE;
  CByteBuffer buf((size_t)cueSize);
  RINOK(ReadStream_FALSE(cue, buf, (size_t)cueSize))
  AString text;
  text.SetFrom_CalcLen((const char *)(const Byte *)buf, (unsigned)cueSize);

  CRecordVector<CCueTrack> cueTracks;
  AString binName;
  ConvertUnicodeToUTF8(name, binName);
  if (!ParseCue(text, binName, cueTracks))
    return S_FALSE;

  // each track runs from its INDEX 01 to the next track's first index;
  // a pregap (INDEX 00) has the sector size of the track it precedes
  UInt64 regionStart = 0;   // byte offset of the current track's first index
  Int64 regionFrame = 0;
  CObjectVector<CTrack> tracks;
  FOR_VECTOR (i, cueTracks)
  {
    const CCueTrack &c = cueTracks[i];
    CTrack t;
    t.Number = c.Number;
    t.Mode = c.Mode;
    const unsigned raw = t.RawSize();
    const Int64 first = c.Index0 >= 0 ? c.Index0 : c.Index1;
    if (first < regionFrame || c.Index1 < first)
      return S_FALSE;
    if (i != 0)
    {
      const CTrack &prev = tracks.Back();
      regionStart += (UInt64)(first - regionFrame) * prev.RawSize();
    }
    else
      regionStart = (UInt64)first * raw;
    regionFrame = first;
    t.Start = regionStart + (UInt64)(c.Index1 - first) * raw;
    t.NumSectors = 0;
    tracks.Add(t);
  }
  for (unsigned i = 0; i < tracks.Size(); i++)
  {
    CTrack &t = tracks[i];
    UInt64 end = _phySize;
    if (i + 1 < tracks.Size())
    {
      const CCueTrack &next = cueTracks[i + 1];
      const Int64 nextFirst = next.Index0 >= 0 ? next.Index0 : next.Index1;
      const Int64 frame1 = cueTracks[i].Index1;
      end = t.Start + (UInt64)(nextFirst - frame1) * t.RawSize();
    }
    if (end > _phySize)
      end = _phySize;
    if (t.Start > end)
      return S_FALSE;
    t.NumSectors = (UInt32)((end - t.Start) / t.RawSize());
    // the .cue has to describe this .bin: data tracks start with a sector
    if ((t.Mode == kMode1 || t.Mode == kMode2) && t.NumSectors != 0
        && !IsDataSector(t.Start, t.Mode))
      return S_FALSE;
  }
  _tracks = tracks;
  _fromCue = true;
  return S_OK;
}

HRESULT CHandler::TracksFromSectors()
{
  Byte h[16];
  RINOK(ReadSectorHeader(0, h))
  const ETrackMode mode = h[15] == 2 ? kMode2 : kMode1;
  const UInt32 n = (UInt32)(_phySize / kRawSectorSize);
  // the data track ends at the last sector with a sync pattern: binary
  // search, as a data track can't be interrupted by audio
  UInt32 dataEnd = n;
  if (!IsDataSector((UInt64)(n - 1) * kRawSectorSize, mode))
  {
    UInt32 lo = 0, hi = n - 1; // lo: data, hi: not
    while (hi - lo > 1)
    {
      const UInt32 mid = lo + (hi - lo) / 2;
      if (IsDataSector((UInt64)mid * kRawSectorSize, mode))
        lo = mid;
      else
        hi = mid;
    }
    dataEnd = hi;
  }
  CTrack &t = _tracks.AddNew();
  t.Number = 1;
  t.Mode = mode;
  t.Start = 0;
  t.NumSectors = dataEnd;
  if (dataEnd < n)
  {
    CTrack &a = _tracks.AddNew();
    a.Number = 2;
    a.Mode = kAudio;
    a.Start = (UInt64)dataEnd * kRawSectorSize;
    a.NumSectors = n - dataEnd;
  }
  return S_OK;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  _stream = stream;
  RINOK(InStream_GetSize_SeekToEnd(stream, _phySize))
  if (_phySize < kRawSectorSize)
    return S_FALSE;
  {
    Byte h[16];
    RINOK(ReadSectorHeader(0, h))
    if (memcmp(h, kSync, 12) != 0 || (h[15] != 1 && h[15] != 2))
      return S_FALSE;
  }

  UString name;
  {
    Z7_DECL_CMyComPtr_QI_FROM(IArchiveOpenVolumeCallback, volumeCallback, callback)
    if (volumeCallback)
    {
      NCOM::CPropVariant prop;
      if (volumeCallback->GetProperty(kpidName, &prop) == S_OK && prop.vt == VT_BSTR)
        name = prop.bstrVal;
    }
  }
  {
    const int slash = name.ReverseFind_PathSepar();
    if (slash >= 0)
      name.DeleteFrontal((unsigned)slash + 1);
  }

  const HRESULT res = TracksFromCue(stream, callback, name);
  if (res != S_OK)
  {
    if (res != S_FALSE)
      return res;
    _tracks.Clear();
    RINOK(TracksFromSectors())
  }

  UString base = name;
  {
    const int dot = base.ReverseFind_Dot();
    if (dot > 0)
      base.DeleteFrom((unsigned)dot);
  }
  if (base.IsEmpty())
    base = L"image";
  FOR_VECTOR (i, _tracks)
  {
    CTrack &t = _tracks[i];
    const char *ext = t.Mode == kAudio ? ".wav" : ".iso";
    if (_tracks.Size() == 1)
      t.Name = base;
    else
    {
      char s[32];
      snprintf(s, sizeof(s), "Track%02u", (unsigned)t.Number);
      t.Name = s;
    }
    t.Name += ext;
  }
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
  _stream.Release();
  _tracks.Clear();
  _phySize = 0;
  _fromCue = false;
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _tracks.Size();
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPhySize: prop = _phySize; break;
    case kpidComment:
      prop = _fromCue ? "Tracks from the .cue file" : "No .cue file: tracks detected from the sectors";
      break;
    // a single data track: open its file system right away
    case kpidMainSubfile:
      if (_tracks.Size() == 1 && _tracks[0].Mode != kAudio)
        prop = (UInt32)0;
      break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  const CTrack &t = _tracks[index];
  switch (propID)
  {
    case kpidPath: prop = t.Name; break;
    case kpidSize: prop = t.Size(); break;
    case kpidPackSize: prop = t.PackSize(); break;
    case kpidMethod: prop = t.MethodName(); break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::GetStream(UInt32 index, ISequentialInStream **stream))
{
  COM_TRY_BEGIN
  *stream = NULL;
  if (index >= _tracks.Size())
    return E_INVALIDARG;
  CMyComPtr2<IInStream, CTrackStream> s;
  s.Create_if_Empty();
  s->Init(_stream, _tracks[index]);
  *stream = s.Detach();
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::Extract(const UInt32 *indices, UInt32 numItems,
    Int32 testMode, IArchiveExtractCallback *extractCallback))
{
  COM_TRY_BEGIN
  const bool allFilesMode = (numItems == (UInt32)(Int32)-1);
  if (allFilesMode)
    numItems = _tracks.Size();
  if (numItems == 0)
    return S_OK;
  UInt64 totalSize = 0;
  for (UInt32 i = 0; i < numItems; i++)
    totalSize += _tracks[allFilesMode ? i : indices[i]].Size();
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
    const CTrack &t = _tracks[index];
    CMyComPtr<ISequentialOutStream> outStream;
    const Int32 askMode = testMode ?
        NExtract::NAskMode::kTest :
        NExtract::NAskMode::kExtract;
    RINOK(extractCallback->GetStream(index, &outStream, askMode))
    if (!testMode && !outStream)
    {
      done += t.Size();
      continue;
    }
    RINOK(extractCallback->PrepareOperation(askMode))

    CMyComPtr2_Create<IInStream, CTrackStream> in;
    in->Init(_stream, t);
    in->CheckEdc = true;
    // CLocalProgress adds the item's progress to InSize, the items before it
    RINOK(copyCoder.Code(in, outStream, NULL, lps))
    outStream.Release();
    done += t.Size();

    Int32 opRes = NExtract::NOperationResult::kOK;
    if (copyCoder.TotalSize != t.Size() || in->ReadError)
      opRes = NExtract::NOperationResult::kUnexpectedEnd;
    else if (in->EdcError)
      opRes = NExtract::NOperationResult::kCRCError;
    RINOK(extractCallback->SetOperationResult(opRes))
  }
  lps->InSize = lps->OutSize = done;
  return lps->SetCur();
  COM_TRY_END
}

static const Byte k_Signature[] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

REGISTER_ARC_I(
  "CD", "bin", NULL, 0x94,
  k_Signature,
  0,
  0,
  NULL)

}}
