// ItemArchive.cpp -- 7zfm-qt: see ItemArchive.h.

#include "ItemArchive.h"

#include "Common/ComTry.h"
#include "Common/StringConvert.h"

#include "Windows/PropVariantUtils.h"
#include "Windows/TimeUtils.h"

#include "7zip/Common/ProgressUtils.h"
#include "7zip/Common/StreamUtils.h"
#include "7zip/ICoder.h"
#include "7zip/IPassword.h"
#include "7zip/PropID.h"

#include "C/7zCrc.h"
#include "C/Alloc.h"
#include "C/LzmaDec.h"

#include <bzlib.h>
#include <lz4.h>
#include <lz4frame.h>
#include <zlib.h>
#include <zstd.h>

#include <string.h>

using namespace NWindows;

namespace NArchive {
namespace NItemArc {

UInt32 GetUi16(const Byte *p) { return (UInt32)p[0] | ((UInt32)p[1] << 8); }
UInt32 GetUi32(const Byte *p) { return GetUi16(p) | (GetUi16(p + 2) << 16); }
UInt64 GetUi64(const Byte *p) { return GetUi32(p) | ((UInt64)GetUi32(p + 4) << 32); }
UInt32 GetBe32(const Byte *p) { return ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) | ((UInt32)p[2] << 8) | p[3]; }

// a piece decoded whole must fit in memory; anything claiming more is a
// broken index
static const UInt64 kMaxPieceSize = (UInt64)1 << 32;

void CItem::AddPiece(UInt64 offset, UInt64 packSize, UInt64 size, Byte method, UInt64 aux, Byte stream)
{
  CPiece p;
  p.Offset = offset;
  p.PackSize = packSize;
  p.Size = size;
  p.Method = method;
  p.Stream = stream;
  p.Aux = aux;
  Pieces.Add(p);
  Size += size;
  if (method != kMethod_Inline)
    PackSize += packSize;
}

void CItem::SetMTime_Unix(Int64 t)
{
  NTime::UnixTime64_To_FileTime(t, MTime);
  MTime_Defined = true;
}

const char *MethodName(Byte method)
{
  switch (method)
  {
    case kMethod_Stored:
    case kMethod_Inline: return "Copy";
    case kMethod_Zlib: return "Deflate";
    case kMethod_Deflate: return "Deflate";
    case kMethod_Lz4Block:
    case kMethod_Lz4Frame: return "LZ4";
    case kMethod_Zstd: return "ZSTD";
    case kMethod_Lzma:
    case kMethod_LzmaRaw: return "LZMA";
    case kMethod_Bzip2: return "BZip2";
    default: return NULL;
  }
}

HRESULT ReadAt(IInStream *stream, UInt64 offset, void *data, size_t size)
{
  RINOK(InStream_SeekSet(stream, offset))
  size_t processed = size;
  RINOK(ReadStream(stream, data, &processed))
  return processed == size ? S_OK : S_FALSE;
}

HRESULT GetStreamSize(IInStream *stream, UInt64 &size)
{
  return InStream_GetSize_SeekToEnd(stream, size);
}

UString GetArcName(IArchiveOpenCallback *callback)
{
  UString name;
  if (!callback)
    return name;
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

HRESULT OpenSiblingFile(IArchiveOpenCallback *callback, const UString &name, CMyComPtr<IInStream> &stream)
{
  stream.Release();
  if (!callback)
    return S_OK;
  Z7_DECL_CMyComPtr_QI_FROM(IArchiveOpenVolumeCallback, volumeCallback, callback)
  if (!volumeCallback)
    return S_OK;
  CMyComPtr<IInStream> s;
  const HRESULT res = volumeCallback->GetStream(name, &s);
  if (res == S_FALSE || !s)
    return S_OK;
  RINOK(res)
  stream = s;
  return S_OK;
}

HRESULT AskPassword(IArchiveOpenCallback *callback, UString &password)
{
  password.Empty();
  if (!callback)
    return S_FALSE;
  Z7_DECL_CMyComPtr_QI_FROM(ICryptoGetTextPassword, getPassword, callback)
  if (!getPassword)
    return S_FALSE;
  CMyComBSTR bstr;
  RINOK(getPassword->CryptoGetTextPassword(&bstr))
  if (bstr)
    password = (const wchar_t *)bstr;
  return S_OK;
}

static HRESULT Zlib(const Byte *src, size_t srcSize, Byte *dest, size_t destSize, int windowBits)
{
  z_stream z;
  memset(&z, 0, sizeof(z));
  if (inflateInit2(&z, windowBits) != Z_OK)
    return E_OUTOFMEMORY;
  size_t outDone = 0;
  int r = Z_OK;
  // zlib counts in uInt: feed and drain in steps
  while (r == Z_OK)
  {
    const size_t inStep = MyMin(srcSize, (size_t)1 << 30);
    const size_t outStep = MyMin(destSize - outDone, (size_t)1 << 30);
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)inStep;
    z.next_out = dest + outDone;
    z.avail_out = (uInt)outStep;
    r = inflate(&z, Z_NO_FLUSH);
    const size_t used = inStep - z.avail_in;
    src += used;
    srcSize -= used;
    outDone += outStep - z.avail_out;
    if (r == Z_OK && used == 0 && outStep == z.avail_out)
      break;
  }
  inflateEnd(&z);
  return (r == Z_STREAM_END && outDone == destSize) ? S_OK : k_Hr_DataError;
}

static HRESULT Lzma(const Byte *props, const Byte *src, size_t srcSize, Byte *dest, size_t destSize)
{
  SizeT outLen = destSize;
  SizeT inLen = srcSize;
  ELzmaStatus status;
  const SRes res = LzmaDecode(dest, &outLen, src, &inLen, props, LZMA_PROPS_SIZE,
      LZMA_FINISH_ANY, &status, &g_Alloc);
  if (res == SZ_ERROR_UNSUPPORTED)
    return k_Hr_Unsupported;
  return (res == SZ_OK && outLen == destSize) ? S_OK : k_Hr_DataError;
}

HRESULT CHandlerBase::DecodeMethod(Byte method, UInt64 aux, const Byte *src, size_t srcSize, Byte *dest, size_t destSize)
{
  switch (method)
  {
    case kMethod_Stored:
      if (srcSize < destSize)
        return k_Hr_DataError;
      memcpy(dest, src, destSize);
      return S_OK;
    case kMethod_Zlib: return Zlib(src, srcSize, dest, destSize, 15);
    case kMethod_Deflate: return Zlib(src, srcSize, dest, destSize, -15);
    case kMethod_Lz4Block:
    {
      if (srcSize > 0x7E000000 || destSize > 0x7E000000)
        return k_Hr_Unsupported;
      const int n = LZ4_decompress_safe((const char *)src, (char *)dest, (int)srcSize, (int)destSize);
      return (n >= 0 && (size_t)n == destSize) ? S_OK : k_Hr_DataError;
    }
    case kMethod_Lz4Frame:
    {
      LZ4F_dctx *ctx = NULL;
      if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION)))
        return E_OUTOFMEMORY;
      size_t inDone = 0, outDone = 0;
      size_t r = 1;
      while (r != 0 && outDone < destSize + 1)
      {
        size_t inLen = srcSize - inDone;
        size_t outLen = destSize - outDone;
        r = LZ4F_decompress(ctx, dest + outDone, &outLen, src + inDone, &inLen, NULL);
        if (LZ4F_isError(r))
          break;
        inDone += inLen;
        outDone += outLen;
        if (inLen == 0 && outLen == 0)
          break;
      }
      LZ4F_freeDecompressionContext(ctx);
      return (r == 0 && outDone == destSize) ? S_OK : k_Hr_DataError;
    }
    case kMethod_Zstd:
    {
      const size_t r = ZSTD_decompress(dest, destSize, src, srcSize);
      return (!ZSTD_isError(r) && r == destSize) ? S_OK : k_Hr_DataError;
    }
    case kMethod_Lzma:
      if (srcSize < LZMA_PROPS_SIZE)
        return k_Hr_DataError;
      return Lzma(src, src + LZMA_PROPS_SIZE, srcSize - LZMA_PROPS_SIZE, dest, destSize);
    case kMethod_LzmaRaw:
    {
      Byte props[LZMA_PROPS_SIZE];
      props[0] = (Byte)aux;
      const UInt32 dict = (UInt32)(aux >> 8);
      for (unsigned i = 0; i < 4; i++)
        props[1 + i] = (Byte)(dict >> (8 * i));
      return Lzma(props, src, srcSize, dest, destSize);
    }
    case kMethod_Bzip2:
    {
      if (srcSize > 0xFFFFFFFF || destSize > 0xFFFFFFFF)
        return k_Hr_Unsupported;
      unsigned out = (unsigned)destSize;
      const int r = BZ2_bzBuffToBuffDecompress((char *)dest, &out, (char *)src, (unsigned)srcSize, 0, 0);
      return (r == BZ_OK && out == destSize) ? S_OK : k_Hr_DataError;
    }
    default:
      return k_Hr_Unsupported;
  }
}

HRESULT CHandlerBase::DecodePiece(const CItem &, const CPiece &piece, CByteBuffer &packed, Byte *dest)
{
  return DecodeMethod(piece.Method, piece.Aux, packed, packed.Size(), dest, (size_t)piece.Size);
}

// ---------- item stream ----------

class CItemStream Z7_final:
  public IInStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(ISequentialInStream, IInStream)
  CMyComPtr<IInArchive> _arcRef;
  CHandlerBase *_arc;
  const CItem *_item;
  CMyComPtr<IInStream> _stream;
  CRecordVector<UInt64> _starts;   // of the pieces in the item
  UInt64 _pos;
  int _cachedPiece;
  CByteBuffer _cache;
  CByteBuffer _packed;
public:
  HRESULT Error;                    // a DecodePiece result, once it failed

  void Init(CHandlerBase *arc, const CItem *item, IInStream *stream)
  {
    _arc = arc;
    _arcRef = arc;
    _item = item;
    _stream = stream;
    UInt64 start = 0;
    FOR_VECTOR (i, item->Pieces)
    {
      _starts.Add(start);
      start += item->Pieces[i].Size;
    }
    _pos = 0;
    _cachedPiece = -1;
    Error = S_OK;
  }
};

Z7_COM7F_IMF(CItemStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (size == 0 || _pos >= _item->Size)
    return S_OK;
  if (Error != S_OK)
    return E_FAIL;
  // the piece holding _pos
  unsigned lo = 0, hi = _starts.Size();
  while (hi - lo > 1)
  {
    const unsigned mid = (lo + hi) / 2;
    if (_starts[mid] <= _pos)
      lo = mid;
    else
      hi = mid;
  }
  const CPiece &piece = _item->Pieces[lo];
  const UInt64 inPiece = _pos - _starts[lo];
  if (inPiece >= piece.Size)
    return S_OK;
  const UInt64 rem = piece.Size - inPiece;
  if (size > rem)
    size = (UInt32)rem;

  IInStream *pieceStream = _arc->PieceStream(piece);
  if (!pieceStream && piece.Method != kMethod_Inline)
  {
    Error = k_Hr_DataError;
    return E_FAIL;
  }
  if (piece.Method == kMethod_Stored)
  {
    RINOK(InStream_SeekSet(pieceStream, piece.Offset + inPiece))
    UInt32 got = 0;
    RINOK(pieceStream->Read(data, size, &got))
    if (got == 0)
      return S_OK;
    _arc->TransformStored(*_item, lo, inPiece, (Byte *)data, got);
    size = got;
  }
  else if (piece.Method == kMethod_Inline)
  {
    if (piece.Offset + piece.Size > _item->Inline.Size())
    {
      Error = k_Hr_DataError;
      return E_FAIL;
    }
    memcpy(data, (const Byte *)_item->Inline + piece.Offset + inPiece, size);
  }
  else
  {
    if (_cachedPiece != (int)lo)
    {
      _cachedPiece = -1;
      if (piece.Size > kMaxPieceSize || piece.PackSize > kMaxPieceSize)
      {
        Error = k_Hr_DataError;
        return E_FAIL;
      }
      _packed.Alloc((size_t)piece.PackSize);
      const HRESULT r = ReadAt(pieceStream, piece.Offset, _packed, (size_t)piece.PackSize);
      if (r != S_OK)
      {
        Error = (r == S_FALSE) ? k_Hr_DataError : r;
        return E_FAIL;
      }
      _cache.Alloc((size_t)piece.Size);
      const HRESULT d = _arc->DecodePiece(*_item, piece, _packed, _cache);
      if (d != S_OK)
      {
        Error = d;
        return E_FAIL;
      }
      _cachedPiece = (int)lo;
    }
    memcpy(data, (const Byte *)_cache + (size_t)inPiece, size);
  }
  _pos += size;
  if (processedSize)
    *processedSize = size;
  return S_OK;
}

Z7_COM7F_IMF(CItemStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  switch (seekOrigin)
  {
    case STREAM_SEEK_SET: break;
    case STREAM_SEEK_CUR: offset += (Int64)_pos; break;
    case STREAM_SEEK_END: offset += (Int64)_item->Size; break;
    default: return STG_E_INVALIDFUNCTION;
  }
  if (offset < 0)
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  _pos = (UInt64)offset;
  if (newPosition)
    *newPosition = _pos;
  return S_OK;
}

HRESULT CHandlerBase::GetItemStream(unsigned index, CMyComPtr<IInStream> &stream)
{
  stream.Release();
  CItemStream *s = new CItemStream;
  CMyComPtr<IInStream> ref = s;
  s->Init(this, &_items[index], DataStream());
  stream = ref;
  return S_OK;
}

// ---------- IInArchive ----------

static const Byte kProps[] =
{
  kpidPath,
  kpidIsDir,
  kpidSize,
  kpidPackSize,
  kpidMTime,
  kpidCRC,
  kpidMethod,
  kpidEncrypted
};

static const Byte kArcProps[] =
{
  kpidComment
};

// upstream's macros name the class CHandler
#define CHandler CHandlerBase
IMP_IInArchive_Props
IMP_IInArchive_ArcProps
#undef CHandler

Z7_COM7F_IMF(CHandlerBase::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPhySize: if (_phySize != 0) prop = _phySize; break;
    case kpidErrorFlags: if (_errorFlags != 0) prop = _errorFlags; break;
    case kpidError: if (!_error.IsEmpty()) prop = _error; break;
    case kpidWarning: if (!_warning.IsEmpty()) prop = _warning; break;
    case kpidComment: if (!_comment.IsEmpty()) prop = _comment; break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandlerBase::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  const CItem &item = _items[index];
  switch (propID)
  {
    case kpidPath: prop = item.Path; break;
    case kpidIsDir: prop = item.IsDir; break;
    case kpidSize: if (!item.IsDir) prop = item.Size; break;
    case kpidPackSize: if (!item.IsDir) prop = item.PackSize; break;
    case kpidMTime: if (item.MTime_Defined) prop = item.MTime; break;
    case kpidCRC: if (item.CheckType == kCheck_Crc32) prop = item.Check; break;
    case kpidEncrypted: if (item.Encrypted) prop = true; break;
    case kpidMethod:
      if (!item.Method.IsEmpty())
        prop = item.Method;
      else if (!item.Pieces.IsEmpty())
      {
        const char *s = MethodName(item.Pieces[0].Method);
        if (s)
          prop = s;
      }
      break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandlerBase::Open(IInStream *stream, const UInt64 *, IArchiveOpenCallback *callback))
{
  COM_TRY_BEGIN
  Close();
  const HRESULT res = Open2(stream, callback);
  if (res != S_OK)
  {
    Close();
    return res;
  }
  _stream = stream;
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandlerBase::Close())
{
  _items.Clear();
  _stream.Release();
  _dataStream.Release();
  _extraStreams.Clear();
  _phySize = 0;
  _errorFlags = 0;
  _error.Empty();
  _warning.Empty();
  _comment.Empty();
  Close2();
  return S_OK;
}

Z7_COM7F_IMF(CHandlerBase::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _items.Size();
  return S_OK;
}

Z7_COM7F_IMF(CHandlerBase::GetStream(UInt32 index, ISequentialInStream **stream))
{
  COM_TRY_BEGIN
  *stream = NULL;
  if (index >= _items.Size() || _items[index].IsDir)
    return S_FALSE;
  CMyComPtr<IInStream> s;
  RINOK(GetItemStream(index, s))
  *stream = s.Detach();
  return S_OK;
  COM_TRY_END
}

static Int32 OpResOf(HRESULT hr)
{
  if (hr == k_Hr_Unsupported)
    return NExtract::NOperationResult::kUnsupportedMethod;
  if (hr == k_Hr_WrongPassword)
    return NExtract::NOperationResult::kWrongPassword;
  return NExtract::NOperationResult::kDataError;
}

Z7_COM7F_IMF(CHandlerBase::Extract(const UInt32 *indices, UInt32 numItems,
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
    if (!item.IsDir)
      totalSize += item.Size;
  }
  RINOK(extractCallback->SetTotal(totalSize))

  CMyComPtr2_Create<ICompressProgressInfo, CLocalProgress> lps;
  lps->Init(extractCallback, false);
  const size_t kBufSize = (size_t)1 << 20;
  CByteBuffer buf(kBufSize);
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
    if (item.IsDir)
    {
      RINOK(extractCallback->PrepareOperation(askMode))
      RINOK(extractCallback->SetOperationResult(NExtract::NOperationResult::kOK))
      continue;
    }
    if (!testMode && !outStream)
    {
      done += item.Size;
      continue;
    }
    RINOK(extractCallback->PrepareOperation(askMode))
    CItemStream *spec = new CItemStream;
    CMyComPtr<IInStream> in = spec;
    spec->Init(this, &item, DataStream());
    UInt32 crc = CRC_INIT_VAL;
    uLong adler = adler32(0, NULL, 0);
    UInt64 got = 0;
    Int32 opRes = NExtract::NOperationResult::kOK;
    for (;;)
    {
      UInt32 n = 0;
      const HRESULT r = in->Read(buf, (UInt32)kBufSize, &n);
      if (r != S_OK)
      {
        if (spec->Error == S_OK)
          return r;
        opRes = OpResOf(spec->Error);
        break;
      }
      if (n == 0)
        break;
      if (item.CheckType == kCheck_Crc32)
        crc = CrcUpdate(crc, buf, n);
      else if (item.CheckType == kCheck_Adler32)
        adler = adler32(adler, buf, n);
      if (outStream)
        RINOK(WriteStream(outStream, buf, n))
      got += n;
      lps->InSize = lps->OutSize = done + got;
      RINOK(lps->SetCur())
    }
    done += item.Size;
    if (opRes == NExtract::NOperationResult::kOK)
    {
      if (got != item.Size)
        opRes = NExtract::NOperationResult::kUnexpectedEnd;
      else if (item.CheckType == kCheck_Crc32 && CRC_GET_DIGEST(crc) != item.Check)
        opRes = NExtract::NOperationResult::kCRCError;
      else if (item.CheckType == kCheck_Adler32 && (UInt32)adler != item.Check)
        opRes = NExtract::NOperationResult::kCRCError;
    }
    outStream.Release();
    RINOK(extractCallback->SetOperationResult(opRes))
  }
  lps->InSize = lps->OutSize = done;
  return lps->SetCur();
  COM_TRY_END
}

}}
