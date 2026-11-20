// ExFatHandler.cpp -- 7zfm-qt: exFAT, which upstream's FAT handler doesn't
// read. After Microsoft's exFAT specification (published 2019): a boot
// sector, one or two FATs, and a cluster heap whose directories hold entry
// sets (a file entry, a stream extension, then name entries). A file's
// clusters are a FAT chain, or a contiguous run when its stream extension
// says NoFatChain. Read only; the upcase table and the allocation bitmap
// aren't needed for that.
//
// Items are as upstream's FAT handler gives them: a parent index
// (IArchiveGetRawProps) instead of whole paths, and a seekable stream per
// file, so that a disk image on an exFAT volume opens in place.

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "Common/StringConvert.h"

#include "Windows/PropVariant.h"
#include "Windows/PropVariantUtils.h"
#include "Windows/TimeUtils.h"

#include "7zip/Archive/IArchive.h"
#include "7zip/Common/ProgressUtils.h"
#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"
#include "7zip/Compress/CopyCoder.h"
#include "7zip/PropID.h"

#include <string.h>

#include <memory>
#include <vector>

using namespace NWindows;

namespace NArchive {
namespace NExFat {

static UInt32 Get16(const Byte *p) { return (UInt32)p[0] | ((UInt32)p[1] << 8); }
static UInt32 Get32(const Byte *p) { return Get16(p) | (Get16(p + 2) << 16); }
static UInt64 Get64(const Byte *p) { return Get32(p) | ((UInt64)Get32(p + 4) << 32); }

static const UInt32 kFatEnd = 0xFFFFFFFF;
static const unsigned kMaxDepth = 256;

enum
{
  kType_Bitmap = 0x81,
  kType_Upcase = 0x82,
  kType_Label = 0x83,
  kType_File = 0x85,
  kType_Stream = 0xC0,
  kType_Name = 0xC1
};

struct CItem
{
  UString Name;
  CByteBuffer Utf16Name;   // zero-terminated, for GetRawProp(kpidName)
  int Parent;
  UInt32 Attrib;
  UInt64 Size;             // DataLength
  UInt64 ValidSize;        // ValidDataLength: zeros after it
  UInt32 Cluster;
  bool NoFatChain;
  UInt32 CTime, MTime, ATime;
  Byte CTime10ms, MTime10ms;
  Byte CTimeUtc, MTimeUtc, ATimeUtc;

  bool IsDir() const { return (Attrib & 0x10) != 0; }
};

struct CRun
{
  UInt64 Offset;           // in the item's data
  UInt64 Cluster;
  UInt64 Count;
};

class CVolume
{
public:
  CMyComPtr<IInStream> Stream;
  unsigned SectorShift, ClusterShift;
  UInt64 HeapOffset;       // bytes
  UInt32 ClusterCount;
  UInt32 RootCluster;
  UInt64 VolumeBytes;
  std::vector<UInt32> Fat;
  UString Label;
  UInt32 Serial;
  bool Error;              // a broken chain or entry set was skipped

  UInt64 ClusterSize() const { return (UInt64)1 << ClusterShift; }
  bool IsValidCluster(UInt32 c) const { return c >= 2 && c - 2 < ClusterCount; }
  UInt64 ClusterOffset(UInt32 c) const { return HeapOffset + ((UInt64)(c - 2) << ClusterShift); }

  // the runs of contiguous clusters holding (size) bytes from (cluster)
  bool GetRuns(UInt32 cluster, UInt64 size, bool noFatChain, std::vector<CRun> &runs) const
  {
    runs.clear();
    if (size == 0)
      return true;
    const UInt64 numClusters = (size + ClusterSize() - 1) >> ClusterShift;
    if (!IsValidCluster(cluster))
      return false;
    if (noFatChain)
    {
      if ((UInt64)(cluster - 2) + numClusters > ClusterCount)
        return false;
      runs.push_back(CRun{0, cluster, numClusters});
      return true;
    }
    UInt64 done = 0;
    while (done < numClusters)
    {
      if (!IsValidCluster(cluster))
        return false;
      if (!runs.empty() && runs.back().Cluster + runs.back().Count == cluster)
        runs.back().Count++;
      else
        runs.push_back(CRun{done << ClusterShift, cluster, 1});
      done++;
      if (done == numClusters)
        break;
      cluster = Fat[cluster];
    }
    return true;
  }

  // a directory's bytes: its whole FAT chain if (size) isn't known
  HRESULT ReadDirData(UInt32 cluster, UInt64 size, bool noFatChain, CByteBuffer &buf)
  {
    if (size == 0)
    {
      // the root: follow the chain to its end
      UInt64 n = 0;
      for (UInt32 c = cluster; IsValidCluster(c) && n <= ClusterCount; c = Fat[c])
        n++;
      if (n == 0 || n > ClusterCount)
        return S_FALSE;
      size = n << ClusterShift;
    }
    if (size > ((UInt64)256 << 20))
      return S_FALSE; // the specification's limit is 256 MiB
    std::vector<CRun> runs;
    if (!GetRuns(cluster, size, noFatChain, runs))
      return S_FALSE;
    buf.Alloc((size_t)size);
    for (const CRun &r : runs)
    {
      const UInt64 len = MyMin(r.Count << ClusterShift, size - r.Offset);
      RINOK(InStream_SeekSet(Stream, ClusterOffset((UInt32)r.Cluster)))
      RINOK(ReadStream_FALSE(Stream, buf + (size_t)r.Offset, (size_t)len))
    }
    return S_OK;
  }
};

// ---------- a file's data ----------

Z7_CLASS_IMP_IInStream(CItemStream)
public:
  std::shared_ptr<CVolume> Vol;
  std::vector<CRun> Runs;
  UInt64 Size, ValidSize;
private:
  UInt64 _pos;
  size_t _run;
public:
  void Init() { _pos = 0; _run = 0; }
};

Z7_COM7F_IMF(CItemStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (_pos >= Size || size == 0)
    return S_OK;
  if (size > Size - _pos)
    size = (UInt32)(Size - _pos);
  if (_pos >= ValidSize)
  {
    // past ValidDataLength: zeros
    memset(data, 0, size);
    _pos += size;
    if (processedSize)
      *processedSize = size;
    return S_OK;
  }
  if (size > ValidSize - _pos)
    size = (UInt32)(ValidSize - _pos);
  // the run holding _pos
  if (_run >= Runs.size() || _pos < Runs[_run].Offset
      || _pos >= Runs[_run].Offset + (Runs[_run].Count << Vol->ClusterShift))
  {
    size_t lo = 0, hi = Runs.size();
    while (hi - lo > 1)
    {
      const size_t mid = (lo + hi) / 2;
      if (Runs[mid].Offset <= _pos)
        lo = mid;
      else
        hi = mid;
    }
    _run = lo;
  }
  const CRun &r = Runs[_run];
  const UInt64 inRun = _pos - r.Offset;
  const UInt64 left = (r.Count << Vol->ClusterShift) - inRun;
  if (size > left)
    size = (UInt32)left;
  RINOK(InStream_SeekSet(Vol->Stream, Vol->ClusterOffset((UInt32)r.Cluster) + inRun))
  UInt32 got = 0;
  RINOK(Vol->Stream->Read(data, size, &got))
  _pos += got;
  if (processedSize)
    *processedSize = got;
  return S_OK;
}

Z7_COM7F_IMF(CItemStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  switch (seekOrigin)
  {
    case STREAM_SEEK_SET: break;
    case STREAM_SEEK_CUR: offset += (Int64)_pos; break;
    case STREAM_SEEK_END: offset += (Int64)Size; break;
    default: return STG_E_INVALIDFUNCTION;
  }
  if (offset < 0)
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  _pos = (UInt64)offset;
  if (newPosition)
    *newPosition = _pos;
  return S_OK;
}

// ---------- the handler ----------

Z7_class_CHandler_final:
  public IInArchive,
  public IArchiveGetRawProps,
  public IInArchiveGetStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_3(IInArchive, IArchiveGetRawProps, IInArchiveGetStream)

  std::shared_ptr<CVolume> _vol;
  CObjectVector<CItem> _items;
  UInt64 _phySize;

  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback);
  HRESULT ReadDir(UInt32 cluster, UInt64 size, bool noFatChain, int parent, unsigned depth,
      IArchiveOpenCallback *callback);
  void GetPath(unsigned index, UString &path) const;
  HRESULT GetItemStream(unsigned index, CItemStream **stream);
};

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  Byte b[512];
  RINOK(InStream_SeekToBegin(stream))
  RINOK(ReadStream_FALSE(stream, b, sizeof(b)))
  if (memcmp(b + 3, "EXFAT   ", 8) != 0 || b[510] != 0x55 || b[511] != 0xAA)
    return S_FALSE;
  for (unsigned i = 11; i < 64; i++)
    if (b[i] != 0)
      return S_FALSE;
  std::shared_ptr<CVolume> v = std::make_shared<CVolume>();
  v->Stream = stream;
  v->SectorShift = b[108];
  v->ClusterShift = b[108] + b[109];
  if (v->SectorShift < 9 || v->SectorShift > 12 || v->ClusterShift > 25)
    return S_FALSE;
  const UInt64 volumeSectors = Get64(b + 72);
  const UInt32 fatOffset = Get32(b + 80);
  const UInt32 fatLength = Get32(b + 84);
  const UInt32 heapOffset = Get32(b + 88);
  v->ClusterCount = Get32(b + 92);
  v->RootCluster = Get32(b + 96);
  v->Serial = Get32(b + 100);
  const unsigned numFats = b[110];
  const bool secondFat = (Get16(b + 106) & 1) != 0 && numFats == 2;
  v->HeapOffset = (UInt64)heapOffset << v->SectorShift;
  v->VolumeBytes = volumeSectors << v->SectorShift;
  v->Error = false;
  if (numFats < 1 || numFats > 2 || v->ClusterCount == 0 || v->ClusterCount > 0xFFFFFFF5
      || ((UInt64)fatLength << v->SectorShift) < ((UInt64)v->ClusterCount + 2) * 4
      || !v->IsValidCluster(v->RootCluster))
    return S_FALSE;

  // the active FAT
  {
    const size_t fatBytes = ((size_t)v->ClusterCount + 2) * 4;
    CByteBuffer fat(fatBytes);
    const UInt64 fatPos = ((UInt64)fatOffset + (secondFat ? fatLength : 0)) << v->SectorShift;
    RINOK(InStream_SeekSet(stream, fatPos))
    RINOK(ReadStream_FALSE(stream, fat, fatBytes))
    v->Fat.resize((size_t)v->ClusterCount + 2);
    for (size_t i = 0; i < v->Fat.size(); i++)
      v->Fat[i] = Get32(fat + i * 4);
  }
  _vol = v;
  _phySize = v->VolumeBytes;
  RINOK(ReadDir(v->RootCluster, 0, false, -1, 0, callback))
  return S_OK;
}

static void SetName(CItem &item, const std::vector<UInt16> &name)
{
  item.Utf16Name.Alloc((name.size() + 1) * 2);
  for (size_t i = 0; i < name.size(); i++)
  {
    item.Utf16Name[i * 2] = (Byte)name[i];
    item.Utf16Name[i * 2 + 1] = (Byte)(name[i] >> 8);
  }
  item.Utf16Name[name.size() * 2] = 0;
  item.Utf16Name[name.size() * 2 + 1] = 0;
  // UTF-16 to wchar_t, with surrogate pairs
  UString s;
  for (size_t i = 0; i < name.size(); i++)
  {
    UInt32 c = name[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < name.size() && name[i + 1] >= 0xDC00 && name[i + 1] < 0xE000)
    {
      c = 0x10000 + ((c - 0xD800) << 10) + (name[i + 1] - 0xDC00);
      i++;
    }
    s += (wchar_t)c;
  }
  item.Name = s;
}

HRESULT CHandler::ReadDir(UInt32 cluster, UInt64 size, bool noFatChain, int parent, unsigned depth,
    IArchiveOpenCallback *callback)
{
  if (depth > kMaxDepth)
  {
    _vol->Error = true;
    return S_OK;
  }
  CByteBuffer buf;
  const HRESULT res = _vol->ReadDirData(cluster, size, noFatChain, buf);
  if (res == S_FALSE)
  {
    _vol->Error = true;
    return parent < 0 ? S_FALSE : S_OK;
  }
  RINOK(res)
  const size_t numEntries = buf.Size() / 32;
  std::vector<unsigned> subDirs;
  for (size_t i = 0; i < numEntries; i++)
  {
    const Byte *e = buf + i * 32;
    const Byte type = e[0];
    if (type == 0)
      break; // end of directory
    if (type == kType_Label && parent < 0)
    {
      const unsigned len = MyMin((unsigned)e[1], 11u);
      std::vector<UInt16> n;
      for (unsigned k = 0; k < len; k++)
        n.push_back((UInt16)Get16(e + 2 + k * 2));
      CItem tmp;
      SetName(tmp, n);
      _vol->Label = tmp.Name;
      continue;
    }
    if (type != kType_File)
      continue;
    const unsigned secondary = e[1];
    const Byte *st = e + 32;
    if (secondary < 2 || i + secondary >= numEntries || st[0] != kType_Stream)
    {
      _vol->Error = true;
      continue;
    }
    CItem item;
    item.Parent = parent;
    item.Attrib = Get16(e + 4);
    item.CTime = Get32(e + 8);
    item.MTime = Get32(e + 12);
    item.ATime = Get32(e + 16);
    item.CTime10ms = e[20];
    item.MTime10ms = e[21];
    item.CTimeUtc = e[22];
    item.MTimeUtc = e[23];
    item.ATimeUtc = e[24];
    item.NoFatChain = (st[1] & 2) != 0;
    const unsigned nameLen = st[3];
    item.ValidSize = Get64(st + 8);
    item.Cluster = Get32(st + 20);
    item.Size = Get64(st + 24);
    if (item.ValidSize > item.Size)
      item.ValidSize = item.Size;
    std::vector<UInt16> name;
    for (unsigned k = 2; k <= secondary && name.size() < nameLen; k++)
    {
      const Byte *ne = e + k * 32;
      if (ne[0] != kType_Name)
        break;
      for (unsigned c = 0; c < 15 && name.size() < nameLen; c++)
        name.push_back((UInt16)Get16(ne + 2 + c * 2));
    }
    i += secondary;
    if (name.size() != nameLen || nameLen == 0)
    {
      _vol->Error = true;
      continue;
    }
    SetName(item, name);
    const unsigned index = _items.Add(item);
    if (item.IsDir())
      subDirs.push_back(index);
  }
  if (callback && (_items.Size() & 0xFFF) == 0)
  {
    const UInt64 numFiles = _items.Size();
    RINOK(callback->SetCompleted(&numFiles, NULL))
  }
  for (unsigned index : subDirs)
  {
    const CItem &d = _items[index];
    if (d.Size == 0)
      continue;
    RINOK(ReadDir(d.Cluster, d.Size, d.NoFatChain, (int)index, depth + 1, callback))
  }
  return S_OK;
}

void CHandler::GetPath(unsigned index, UString &path) const
{
  path = _items[index].Name;
  unsigned guard = 0;
  for (int p = _items[index].Parent; p >= 0 && guard < kMaxDepth + 2; p = _items[(unsigned)p].Parent, guard++)
  {
    UString s = _items[(unsigned)p].Name;
    s.Add_PathSepar();
    s += path;
    path = s;
  }
}

HRESULT CHandler::GetItemStream(unsigned index, CItemStream **stream)
{
  *stream = NULL;
  const CItem &item = _items[index];
  CItemStream *s = new CItemStream;
  CMyComPtr<IInStream> ref = s;
  s->Vol = _vol;
  s->Size = item.Size;
  s->ValidSize = item.ValidSize;
  if (!_vol->GetRuns(item.Cluster, item.ValidSize, item.NoFatChain, s->Runs))
    return S_FALSE;
  s->Init();
  *stream = s;
  ref.Detach();
  return S_OK;
}

static const Byte kProps[] =
{
  kpidPath,
  kpidIsDir,
  kpidSize,
  kpidPackSize,
  kpidMTime,
  kpidCTime,
  kpidATime,
  kpidAttrib
};

static const Byte kArcProps[] =
{
  kpidVolumeName,
  kpidClusterSize,
  kpidSectorSize,
  kpidId
};

IMP_IInArchive_Props
IMP_IInArchive_ArcProps

// exFAT timestamps: DOS date and time, local at a UTC offset (in 15 minute
// steps) when bit 7 of that byte says it is valid, plus 10 ms steps
static void SetTime(NCOM::CPropVariant &prop, UInt32 dosTime, Byte ms10, Byte utc)
{
  if (dosTime == 0)
    return;
  FILETIME ft;
  if (!NTime::DosTime_To_FileTime(dosTime, ft))
    return;
  UInt64 v = ((UInt64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  v += (UInt64)ms10 * 100000;
  if (utc & 0x80)
  {
    Int32 quarters = utc & 0x7F;
    if (quarters & 0x40)
      quarters -= 0x80;
    v -= (Int64)quarters * 15 * 60 * 10000000;
    ft.dwLowDateTime = (DWORD)v;
    ft.dwHighDateTime = (DWORD)(v >> 32);
    prop.SetAsTimeFrom_FT_Prec(ft, k_PropVar_TimePrec_Base + 2);
  }
  else
  {
    // local time, as a FAT handler gives it
    FILETIME utcFt;
    ft.dwLowDateTime = (DWORD)v;
    ft.dwHighDateTime = (DWORD)(v >> 32);
    if (LocalFileTimeToFileTime(&ft, &utcFt))
      prop.SetAsTimeFrom_FT_Prec(utcFt, k_PropVar_TimePrec_Base + 2);
  }
}

Z7_COM7F_IMF(CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  if (_vol)
    switch (propID)
    {
      case kpidPhySize: prop = _phySize; break;
      case kpidVolumeName: if (!_vol->Label.IsEmpty()) prop = _vol->Label; break;
      case kpidClusterSize: prop = (UInt32)_vol->ClusterSize(); break;
      case kpidSectorSize: prop = (UInt32)1 << _vol->SectorShift; break;
      case kpidId: prop = _vol->Serial; break;
      case kpidErrorFlags:
        if (_vol->Error)
          prop = (UInt32)kpv_ErrorFlags_HeadersError;
        break;
      default: break;
    }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  const CItem &item = _items[index];
  switch (propID)
  {
    case kpidPath: { UString s; GetPath(index, s); prop = s; break; }
    case kpidName: prop = item.Name; break;
    case kpidIsDir: prop = item.IsDir(); break;
    case kpidSize: if (!item.IsDir()) prop = item.Size; break;
    case kpidPackSize:
      if (!item.IsDir())
        prop = (item.Size + _vol->ClusterSize() - 1) & ~(_vol->ClusterSize() - 1);
      break;
    case kpidMTime: SetTime(prop, item.MTime, item.MTime10ms, item.MTimeUtc); break;
    case kpidCTime: SetTime(prop, item.CTime, item.CTime10ms, item.CTimeUtc); break;
    case kpidATime: SetTime(prop, item.ATime, 0, item.ATimeUtc); break;
    case kpidAttrib: prop = item.Attrib; break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::GetNumRawProps(UInt32 *numProps))
{
  *numProps = 0;
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetRawPropInfo(UInt32, BSTR *name, PROPID *propID))
{
  *name = NULL;
  *propID = 0;
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetParent(UInt32 index, UInt32 *parent, UInt32 *parentType))
{
  *parentType = NParentType::kDir;
  *parent = (UInt32)(Int32)(index < _items.Size() ? _items[index].Parent : -1);
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetRawProp(UInt32 index, PROPID propID, const void **data, UInt32 *dataSize, UInt32 *propType))
{
  *data = NULL;
  *dataSize = 0;
  *propType = 0;
  if (index < _items.Size() && propID == kpidName)
  {
    const CByteBuffer &b = _items[index].Utf16Name;
    *data = (const Byte *)b;
    *dataSize = (UInt32)b.Size();
    *propType = NPropDataType::kUtf16z;
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
  _vol.reset();
  _items.Clear();
  _phySize = 0;
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _items.Size();
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetStream(UInt32 index, ISequentialInStream **stream))
{
  COM_TRY_BEGIN
  *stream = NULL;
  if (index >= _items.Size() || _items[index].IsDir())
    return S_FALSE;
  CItemStream *s;
  RINOK(GetItemStream(index, &s))
  CMyComPtr<IInStream> ref = s;
  *stream = ref.Detach();
  return S_OK;
  COM_TRY_END
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
  UInt64 totalSize = 0;
  for (UInt32 i = 0; i < numItems; i++)
  {
    const CItem &item = _items[allFilesMode ? i : indices[i]];
    if (!item.IsDir())
      totalSize += item.Size;
  }
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
    const CItem &item = _items[index];
    CMyComPtr<ISequentialOutStream> outStream;
    const Int32 askMode = testMode ?
        NExtract::NAskMode::kTest :
        NExtract::NAskMode::kExtract;
    RINOK(extractCallback->GetStream(index, &outStream, askMode))
    if (item.IsDir())
    {
      RINOK(extractCallback->PrepareOperation(askMode))
      RINOK(extractCallback->SetOperationResult(NExtract::NOperationResult::kOK))
      continue;
    }
    done += item.Size;
    if (!testMode && !outStream)
      continue;
    RINOK(extractCallback->PrepareOperation(askMode))
    Int32 opRes = NExtract::NOperationResult::kOK;
    CItemStream *s = NULL;
    const HRESULT res = GetItemStream(index, &s);
    if (res == S_FALSE)
      opRes = NExtract::NOperationResult::kDataError;
    else
    {
      RINOK(res)
      CMyComPtr<IInStream> in = s;
      RINOK(copyCoder.Code(in, outStream, NULL, lps))
      if (copyCoder.TotalSize != item.Size)
        opRes = NExtract::NOperationResult::kUnexpectedEnd;
    }
    outStream.Release();
    RINOK(extractCallback->SetOperationResult(opRes))
  }
  lps->InSize = lps->OutSize = done;
  return lps->SetCur();
  COM_TRY_END
}

static const Byte k_Signature[] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };

REGISTER_ARC_I(
  "exFAT", "exfat", NULL, 0xA2,
  k_Signature,
  3,
  0,
  NULL)

}}
