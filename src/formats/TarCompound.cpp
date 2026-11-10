// TarCompound.cpp -- 7zfm-qt: compressed tar archives in one step, as the
// formats "tgz" (.tar.gz), "tbz2" (.tar.bz2), "txz" (.tar.xz) and "tzst"
// (.tar.zst). Upstream 7-Zip writes gz/bz2/xz only around a single file, so
// a folder takes two steps there (tar, then compress the tar).
//
// Writing: upstream's tar handler writes the tar stream on a thread of its
// own into a CStreamBinder, and upstream's gzip / bzip2 / xz handler
// compresses what comes out of it, as its single item. Upstream ships no
// zstd encoder (ZstdHandler.cpp has it behind Z7_USE_ZSTD_COMPRESSION,
// without the sources), so zstd uses the system libzstd.
//
// Updating an existing archive: Open() decompresses the tar into a temporary
// file with upstream's handler and opens that with upstream's tar handler,
// which then adds and replaces items as it does in a plain .tar.
//
// Browsing stays with upstream's handlers (gz, then the tar inside): these
// formats open only when asked for by name (-ttgz, or the compress dialog),
// through kByExtOnlyOpen and an extension that never matches a file's.

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/MyCom.h"
#include "Common/MyString.h"

#include "Windows/FileDir.h"
#include "Windows/PropVariant.h"
#include "Windows/System.h"
#include "Windows/TimeUtils.h"

#include "7zip/Archive/IArchive.h"
#include "7zip/Common/FileStreams.h"
#include "7zip/Common/MethodProps.h"
#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamBinder.h"
#include "7zip/Common/StreamUtils.h"
#include "7zip/PropID.h"
#include "7zip/UI/Common/LoadCodecs.h"

#include <zstd.h>

#include <thread>

using namespace NWindows;

namespace NArchive {
namespace NTarCompound {

enum ECompressor { kGzip, kBzip2, kXz, kZstd };

// upstream's format of each compressor
static const char * const kCompressorNames[] = { "gzip", "bzip2", "xz", "zstd" };

struct CProp
{
  UString Name;
  NCOM::CPropVariant Value;
};

// upstream handlers, by name, from the registered formats
static HRESULT FindUpstreamFormat(const char *name, CMyComPtr<IUnknown> &codecsRef, CCodecs *&codecs, unsigned &index)
{
  codecs = new CCodecs;
  codecsRef = (IUnknown *)(ICompressCodecsInfo *)codecs;
  RINOK(codecs->Load())
  const int i = codecs->FindFormatForArchiveType(UString(name));
  if (i < 0)
    return E_NOTIMPL;
  index = (unsigned)i;
  return S_OK;
}

static HRESULT CreateUpstreamOut(const char *name, CMyComPtr<IOutArchive> &archive)
{
  CMyComPtr<IUnknown> ref;
  CCodecs *codecs;
  unsigned index;
  RINOK(FindUpstreamFormat(name, ref, codecs, index))
  return codecs->CreateOutArchive(index, archive);
}

static HRESULT CreateUpstreamIn(const char *name, CMyComPtr<IInArchive> &archive)
{
  CMyComPtr<IUnknown> ref;
  CCodecs *codecs;
  unsigned index;
  RINOK(FindUpstreamFormat(name, ref, codecs, index))
  return codecs->CreateInArchive(index, archive);
}

// the compressor's update callback: one file, the tar stream
Z7_CLASS_IMP_COM_1(
  CTarItemCallback
  , IArchiveUpdateCallback
)
  Z7_IFACE_COM7_IMP(IProgress)
public:
  CMyComPtr<ISequentialInStream> Stream;
  UInt64 SizeEstimate;
  FILETIME MTime;
};

Z7_COM7F_IMF(CTarItemCallback::SetTotal(UInt64 /* total */)) { return S_OK; }
Z7_COM7F_IMF(CTarItemCallback::SetCompleted(const UInt64 * /* completeValue */)) { return S_OK; }

Z7_COM7F_IMF(CTarItemCallback::GetUpdateItemInfo(UInt32 /* index */,
    Int32 *newData, Int32 *newProps, UInt32 *indexInArchive))
{
  if (newData) *newData = 1;
  if (newProps) *newProps = 1;
  if (indexInArchive) *indexInArchive = (UInt32)(Int32)-1;
  return S_OK;
}

Z7_COM7F_IMF(CTarItemCallback::GetProperty(UInt32 /* index */, PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidIsDir: prop = false; break;
    // the writers want a size; it only sizes their buffers and threads
    case kpidSize: prop = SizeEstimate; break;
    case kpidMTime: prop = MTime; break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CTarItemCallback::GetStream(UInt32 /* index */, ISequentialInStream **inStream))
{
  *inStream = Stream.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CTarItemCallback::SetOperationResult(Int32 /* operationResult */)) { return S_OK; }


// the decompressor's extract callback: its single item, into a file
Z7_CLASS_IMP_COM_1(
  CToStreamCallback
  , IArchiveExtractCallback
)
  Z7_IFACE_COM7_IMP(IProgress)
public:
  CMyComPtr<ISequentialOutStream> Stream;
  Int32 Result;
  CToStreamCallback(): Result(NExtract::NOperationResult::kDataError) {}
};

Z7_COM7F_IMF(CToStreamCallback::SetTotal(UInt64 /* total */)) { return S_OK; }
Z7_COM7F_IMF(CToStreamCallback::SetCompleted(const UInt64 * /* completeValue */)) { return S_OK; }

Z7_COM7F_IMF(CToStreamCallback::GetStream(UInt32 /* index */, ISequentialOutStream **outStream, Int32 askExtractMode))
{
  *outStream = NULL;
  if (askExtractMode == NExtract::NAskMode::kExtract)
    *outStream = Stream.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CToStreamCallback::PrepareOperation(Int32 /* askExtractMode */)) { return S_OK; }

Z7_COM7F_IMF(CToStreamCallback::SetOperationResult(Int32 opRes))
{
  Result = opRes;
  return S_OK;
}


class CHandler Z7_final :
  public IInArchive,
  public IOutArchive,
  public ISetProperties,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_3(IInArchive, IOutArchive, ISetProperties)
  Z7_IFACE_COM7_IMP(IInArchive)
  Z7_IFACE_COM7_IMP(IOutArchive)
  Z7_IFACE_COM7_IMP(ISetProperties)

  const ECompressor _compressor;
  CObjectVector<CProp> _tarProps;
  CObjectVector<CProp> _compressorProps;
  // zstd
  UInt32 _level;
  UInt32 _numThreads;

  // an opened archive: its tar, decompressed into (_tempFile)
  CMyComPtr<IInArchive> _tar;
  NFile::NDir::CTempFile _tempFile;
  NCOM::CPropVariant _phySize; // of the compressed file, not of the tar

  HRESULT GetTarOut(CMyComPtr<IOutArchive> &tar);
  HRESULT ZstdCompress(ISequentialInStream *inStream, ISequentialOutStream *outStream);
public:
  CHandler(ECompressor c): _compressor(c) { InitProps(); }
  void InitProps()
  {
    _tarProps.Clear();
    _compressorProps.Clear();
    _level = 5;
    _numThreads = NSystem::GetNumberOfProcessors();
  }
};

// ---- IInArchive: the tar inside, for updating it ----

Z7_COM7F_IMF(CHandler::Open(IInStream *stream, const UInt64 *maxCheckStartPosition, IArchiveOpenCallback *openCallback))
{
  COM_TRY_BEGIN
  Close();

  CMyComPtr<IInArchive> outer;
  RINOK(CreateUpstreamIn(kCompressorNames[_compressor], outer))
  RINOK(outer->Open(stream, maxCheckStartPosition, openCallback))
  UInt32 numItems = 0;
  RINOK(outer->GetNumberOfItems(&numItems))
  if (numItems != 1)
    return S_FALSE;
  RINOK(outer->GetArchiveProperty(kpidPhySize, &_phySize))

  {
    CMyComPtr2_Create<ISequentialOutStream, COutFileStream> tempOut;
    if (!_tempFile.CreateRandomInTempFolder(FTEXT("7zfm-qt-tar"), &tempOut->File))
      return GetLastError_noZero_HRESULT();
    CMyComPtr2_Create<IArchiveExtractCallback, CToStreamCallback> extract;
    extract->Stream = tempOut;
    const UInt32 index = 0;
    RINOK(outer->Extract(&index, 1, 0, extract))
    if (extract->Result != NExtract::NOperationResult::kOK)
      return S_FALSE;
  }
  outer->Close();

  CMyComPtr2_Create<IInStream, CInFileStream> tempIn;
  if (!tempIn->Open(_tempFile.GetPath()))
    return GetLastError_noZero_HRESULT();
  CMyComPtr<IInArchive> tar;
  RINOK(CreateUpstreamIn("tar", tar))
  const UInt64 maxCheck = 0;
  RINOK(tar->Open(tempIn, &maxCheck, openCallback))
  _tar = tar;
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::Close())
{
  if (_tar)
    _tar->Close();
  _tar.Release();
  _tempFile.Remove();
  _phySize.Clear();
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = 0;
  return _tar ? _tar->GetNumberOfItems(numItems) : S_OK;
}

Z7_COM7F_IMF(CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  return _tar ? _tar->GetProperty(index, propID, value) : E_INVALIDARG;
}

Z7_COM7F_IMF(CHandler::Extract(const UInt32 *indices, UInt32 numItems, Int32 testMode, IArchiveExtractCallback *extractCallback))
{
  return _tar ? _tar->Extract(indices, numItems, testMode, extractCallback) : E_FAIL;
}

Z7_COM7F_IMF(CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  // the tar's own size would be taken as the file's, which is smaller
  if (propID == kpidPhySize)
  {
    NCOM::CPropVariant prop = _phySize;
    prop.Detach(value);
    return S_OK;
  }
  return _tar ? _tar->GetArchiveProperty(propID, value) : S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfProperties(UInt32 *numProps))
{
  *numProps = 0;
  return _tar ? _tar->GetNumberOfProperties(numProps) : S_OK;
}

Z7_COM7F_IMF(CHandler::GetPropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
{
  return _tar ? _tar->GetPropertyInfo(index, name, propID, varType) : E_INVALIDARG;
}

Z7_COM7F_IMF(CHandler::GetNumberOfArchiveProperties(UInt32 *numProps))
{
  *numProps = 0;
  return _tar ? _tar->GetNumberOfArchiveProperties(numProps) : S_OK;
}

Z7_COM7F_IMF(CHandler::GetArchivePropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
{
  return _tar ? _tar->GetArchivePropertyInfo(index, name, propID, varType) : E_INVALIDARG;
}

// ---- properties: tar's go to tar, the rest to the compressor ----

static bool IsTarProp(const UString &name, const PROPVARIANT &value)
{
  // tar's own switches (Tar/TarHandler.cpp SetProperties): code page,
  // -m=gnu|pax|posix, and the timestamps (tm, tc, ta, tp)
  if (name.IsEqualTo("cp"))
    return true;
  if (name.IsEqualTo("m") && value.vt == VT_BSTR)
  {
    const UString s = value.bstrVal;
    return s.IsEqualTo_Ascii_NoCase("gnu") || s.IsEqualTo_Ascii_NoCase("pax")
        || s.IsEqualTo_Ascii_NoCase("posix");
  }
  return name.Len() == 2 && name[0] == 't';
}

static HRESULT SetHandlerProps(IUnknown *handler, const CObjectVector<CProp> &props)
{
  if (props.IsEmpty())
    return S_OK;
  CMyComPtr<ISetProperties> setProps;
  handler->QueryInterface(IID_ISetProperties, (void **)&setProps);
  if (!setProps)
    return E_INVALIDARG;
  CRecordVector<const wchar_t *> names;
  CRecordVector<PROPVARIANT> values;
  FOR_VECTOR (i, props)
  {
    names.Add(props[i].Name);
    values.Add(props[i].Value);
  }
  return setProps->SetProperties(names.ConstData(), values.ConstData(), props.Size());
}

Z7_COM7F_IMF(CHandler::SetProperties(const wchar_t * const *names, const PROPVARIANT *values, UInt32 numProps))
{
  COM_TRY_BEGIN
  InitProps();
  for (UInt32 i = 0; i < numProps; i++)
  {
    UString name = names[i];
    name.MakeLower_Ascii();
    if (name.IsEmpty())
      return E_INVALIDARG;
    const PROPVARIANT &value = values[i];
    CProp p;
    p.Name = name;
    p.Value = value;
    if (IsTarProp(name, value))
    {
      _tarProps.Add(p);
      continue;
    }
    if (_compressor != kZstd)
    {
      _compressorProps.Add(p);
      continue;
    }
    if (name[0] == 'x')
    {
      UInt32 level = 5;
      RINOK(ParsePropToUInt32(name.Ptr(1), value, level))
      _level = level;
    }
    else if (name.IsPrefixedBy_Ascii_NoCase("mt"))
    {
      RINOK(ParseMtProp(name.Ptr(2), value, NSystem::GetNumberOfProcessors(), _numThreads))
    }
    else if (name.IsPrefixedBy_Ascii_NoCase("memuse"))
    {
    }
    else if (name.IsEqualTo("m"))
    {
      // the only method
      if (value.vt != VT_BSTR || !UString(value.bstrVal).IsEqualTo_Ascii_NoCase("zstd"))
        return E_INVALIDARG;
    }
    else
      return E_INVALIDARG;
  }
  return S_OK;
  COM_TRY_END
}

// ---- IOutArchive ----

// the opened archive's tar handler, or a new one
HRESULT CHandler::GetTarOut(CMyComPtr<IOutArchive> &tar)
{
  if (_tar)
  {
    RINOK(_tar.QueryInterface(IID_IOutArchive, &tar))
  }
  else
  {
    RINOK(CreateUpstreamOut("tar", tar))
  }
  return SetHandlerProps(tar, _tarProps);
}

Z7_COM7F_IMF(CHandler::GetFileTimeType(UInt32 *type))
{
  CMyComPtr<IOutArchive> tar;
  RINOK(GetTarOut(tar))
  return tar->GetFileTimeType(type);
}

// 7-Zip's levels (Store 0 .. Ultra 9) on zstd's 1..19; -mx above 9 is a
// zstd level itself (up to 22)
static int ZstdLevel(UInt32 level)
{
  static const Byte kLevels[] = { 1, 1, 2, 2, 3, 3, 6, 12, 15, 19 };
  if (level < Z7_ARRAY_SIZE(kLevels))
    return kLevels[level];
  return (int)MyMin(level, (UInt32)ZSTD_maxCLevel());
}

HRESULT CHandler::ZstdCompress(ISequentialInStream *inStream, ISequentialOutStream *outStream)
{
  ZSTD_CCtx *const cctx = ZSTD_createCCtx();
  if (!cctx)
    return E_OUTOFMEMORY;
  struct CFree { ZSTD_CCtx *c; ~CFree() { ZSTD_freeCCtx(c); } } freeCtx = { cctx };
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, ZstdLevel(_level));
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1);
  // a libzstd built without threads keeps 0 workers: single-threaded
  if (_numThreads > 1)
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, (int)_numThreads);

  const size_t inSize = ZSTD_CStreamInSize();
  const size_t outSize = ZSTD_CStreamOutSize();
  CByteBuffer inBuf(inSize), outBuf(outSize);
  for (;;)
  {
    size_t read = inSize;
    RINOK(ReadStream(inStream, inBuf, &read))
    const ZSTD_EndDirective mode = read < inSize ? ZSTD_e_end : ZSTD_e_continue;
    ZSTD_inBuffer in = { inBuf, read, 0 };
    for (;;)
    {
      ZSTD_outBuffer out = { outBuf, outSize, 0 };
      const size_t rem = ZSTD_compressStream2(cctx, &out, &in, mode);
      if (ZSTD_isError(rem))
        return E_FAIL;
      RINOK(WriteStream(outStream, outBuf, out.pos))
      if (mode == ZSTD_e_end ? rem == 0 : in.pos == in.size)
        break;
    }
    if (mode == ZSTD_e_end)
      return S_OK;
  }
}

Z7_COM7F_IMF(CHandler::UpdateItems(ISequentialOutStream *outStream, UInt32 numItems,
    IArchiveUpdateCallback *updateCallback))
{
  COM_TRY_BEGIN

  // an upper bound of the tar's size: headers, long-name and pax records,
  // data padded to 512 bytes, and the end of archive
  UInt64 sizeEstimate = 10240;
  for (UInt32 i = 0; i < numItems; i++)
  {
    Int32 newData = 0, newProps = 0;
    UInt32 indexInArchive = 0;
    RINOK(updateCallback->GetUpdateItemInfo(i, &newData, &newProps, &indexInArchive))
    NCOM::CPropVariant prop;
    if (IntToBool(newData))
    {
      RINOK(updateCallback->GetProperty(i, kpidSize, &prop))
    }
    else if (_tar)
    {
      RINOK(_tar->GetProperty(indexInArchive, kpidSize, &prop))
    }
    UInt64 size = 0;
    if (prop.vt == VT_UI8)
      size = prop.uhVal.QuadPart;
    sizeEstimate += 4096 + ((size + 511) & ~(UInt64)511);
  }

  CMyComPtr<IOutArchive> tar;
  RINOK(GetTarOut(tar))

  CMyComPtr<IOutArchive> compressor;
  if (_compressor != kZstd)
  {
    RINOK(CreateUpstreamOut(kCompressorNames[_compressor], compressor))
    RINOK(SetHandlerProps(compressor, _compressorProps))
  }

  CStreamBinder binder;
  RINOK(binder.Create_ReInit())
  CMyComPtr<ISequentialInStream> tarIn;
  CMyComPtr<ISequentialOutStream> tarOut;
  binder.CreateStreams2(tarIn, tarOut);

  // tar writes on its own thread; releasing (tarOut) ends the stream for
  // the reader, releasing (tarIn) stops the writer
  HRESULT tarRes = S_OK;
  std::thread tarThread([&tarRes, &tar, &tarOut, numItems, updateCallback]() {
    try
    {
      tarRes = tar->UpdateItems(tarOut, numItems, updateCallback);
    }
    catch (...)
    {
      tarRes = E_FAIL;
    }
    tarOut.Release();
  });

  HRESULT res;
  try
  {
    if (compressor)
    {
      CMyComPtr2_Create<IArchiveUpdateCallback, CTarItemCallback> item;
      item->Stream = tarIn;
      item->SizeEstimate = sizeEstimate;
      NTime::GetCurUtcFileTime(item->MTime);
      tarIn.Release();
      res = compressor->UpdateItems(outStream, 1, item);
    }
    else
      res = ZstdCompress(tarIn, outStream);
  }
  catch (...)
  {
    res = E_FAIL;
  }
  tarIn.Release();
  tarThread.join();

  // a compressor error makes tar's writes fail: report the cause
  if (res != S_OK)
    return res;
  return tarRes;

  COM_TRY_END
}

}}

// ---- registration: one format per compressor ----

#define Z7QT_REGISTER_TAR_COMPOUND(ns, name, ext, id, compressor) \
  namespace NArchive { namespace NTarCompound { namespace ns { \
  static IInArchive *CreateArc() { return new CHandler(compressor); } \
  static IOutArchive *CreateArcOut() { return new CHandler(compressor); } \
  REGISTER_ARC_R(name, ext, NULL, id, 0, NULL, 0, \
      NArcInfoFlags::kByExtOnlyOpen \
    | NArcInfoFlags::kSymLinks \
    | NArcInfoFlags::kHardLinks \
    | NArcInfoFlags::kMTime \
    | NArcInfoFlags::kMTime_Default \
    , TIME_PREC_TO_ARC_FLAGS_MASK (NFileTimeType::kWindows) \
    | TIME_PREC_TO_ARC_FLAGS_MASK (NFileTimeType::kUnix) \
    | TIME_PREC_TO_ARC_FLAGS_MASK (NFileTimeType::k1ns) \
    | TIME_PREC_TO_ARC_FLAGS_TIME_DEFAULT (NFileTimeType::kUnix) \
    , CreateArc, CreateArcOut, NULL) \
  }}}

// the extensions have a dot, so that opening never picks these by name
Z7QT_REGISTER_TAR_COMPOUND(NGz,   "tgz",  "tar.gz",  0x90, kGzip)
Z7QT_REGISTER_TAR_COMPOUND(NBz2,  "tbz2", "tar.bz2", 0x91, kBzip2)
Z7QT_REGISTER_TAR_COMPOUND(NXz,   "txz",  "tar.xz",  0x92, kXz)
Z7QT_REGISTER_TAR_COMPOUND(NZstd, "tzst", "tar.zst", 0x93, kZstd)
