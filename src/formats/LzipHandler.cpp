// LzipHandler.cpp -- 7zfm-qt: lzip (.lz, .tar.lz, .tlz), which upstream
// doesn't read. After the lzip manual's file format section: one or more
// members, each a 6-byte header ("LZIP", version 1, coded dictionary
// size), an LZMA stream (lc 3, lp 0, pb 2, with an end marker, no LZMA
// header) and a 20-byte trailer (CRC-32, data size, member size).
//
// Like upstream's gzip and xz handlers: one item, named after the archive
// (".tlz" becomes ".tar"), read sequentially. The sizes come from the
// trailers, read from the end of the file, so a .tar.lz knows its tar's
// size before decoding. Decoding is upstream's LZMA decoder.

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/MyCom.h"
#include "Common/MyString.h"

#include "Windows/PropVariant.h"

#include "7zip/Archive/IArchive.h"
#include "7zip/Common/ProgressUtils.h"
#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"
#include "7zip/PropID.h"

#include "../C/7zCrc.h"
#include "../C/Alloc.h"
#include "../C/LzmaDec.h"

#include <stdio.h>
#include <string.h>

using namespace NWindows;

namespace NArchive {
namespace NLzip {

static const unsigned kHeaderSize = 6;
static const unsigned kTrailerSize = 20;
static const Byte kLzmaProps = (2 * 5 + 0) * 9 + 3; // pb 2, lp 0, lc 3

static UInt32 Get32(const Byte *p) { return (UInt32)p[0] | ((UInt32)p[1] << 8) | ((UInt32)p[2] << 16) | ((UInt32)p[3] << 24); }
static UInt64 Get64(const Byte *p) { return Get32(p) | ((UInt64)Get32(p + 4) << 32); }

// the dictionary size of a member header; 0 if it isn't one
static UInt32 DictSize(const Byte *h)
{
  if (memcmp(h, "LZIP", 4) != 0 || h[4] != 1)
    return 0;
  const unsigned shift = h[5] & 0x1F;
  if (shift < 12 || shift > 29)
    return 0;
  UInt32 size = (UInt32)1 << shift;
  size -= (size / 16) * ((h[5] >> 5) & 7);
  return size >= (1 << 12) ? size : 0;
}

Z7_CLASS_IMP_CHandler_IInArchive_0

  CMyComPtr<IInStream> _stream;
  UInt32 _dictSize;         // of the first member
  UInt64 _phySize;          // of the members found from the end
  UInt64 _unpackSize;
  UInt32 _numMembers;
  bool _sizesKnown;         // the members cover the whole file
};

static const Byte kProps[] =
{
  kpidSize,
  kpidPackSize,
  kpidMethod
};

static const Byte kArcProps[] =
{
  kpidNumStreams,
  kpidMethod
};

IMP_IInArchive_Props
IMP_IInArchive_ArcProps

static void MethodString(UInt32 dict, NCOM::CPropVariant &prop)
{
  char s[32];
  if ((dict & ((1 << 20) - 1)) == 0)
    snprintf(s, sizeof(s), "LZMA:%um", (unsigned)(dict >> 20));
  else if ((dict & ((1 << 10) - 1)) == 0)
    snprintf(s, sizeof(s), "LZMA:%uk", (unsigned)(dict >> 10));
  else
    snprintf(s, sizeof(s), "LZMA:%u", (unsigned)dict);
  prop = s;
}

Z7_COM7F_IMF(CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPhySize: if (_sizesKnown) prop = _phySize; break;
    case kpidNumStreams: if (_sizesKnown) prop = _numMembers; break;
    case kpidMethod: MethodString(_dictSize, prop); break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = 1;
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetProperty(UInt32, PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidSize: if (_sizesKnown) prop = _unpackSize; break;
    case kpidPackSize: if (_sizesKnown) prop = _phySize; break;
    case kpidMethod: MethodString(_dictSize, prop); break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CHandler::Open(IInStream *stream, const UInt64 *, IArchiveOpenCallback *))
{
  COM_TRY_BEGIN
  Close();
  Byte h[kHeaderSize];
  RINOK(InStream_SeekToBegin(stream))
  RINOK(ReadStream_FALSE(stream, h, kHeaderSize))
  _dictSize = DictSize(h);
  if (_dictSize == 0)
    return S_FALSE;
  _stream = stream;

  // the members, from the last trailer back to the first header
  UInt64 fileSize;
  RINOK(InStream_GetSize_SeekToEnd(stream, fileSize))
  UInt64 pos = fileSize, unpack = 0;
  UInt32 members = 0;
  bool ok = true;
  while (pos != 0)
  {
    if (pos < kHeaderSize + kTrailerSize)
    {
      ok = false;
      break;
    }
    Byte t[kTrailerSize];
    RINOK(InStream_SeekSet(stream, pos - kTrailerSize))
    RINOK(ReadStream_FALSE(stream, t, kTrailerSize))
    const UInt64 memberSize = Get64(t + 12);
    if (memberSize < kHeaderSize + kTrailerSize || memberSize > pos)
    {
      ok = false;
      break;
    }
    Byte mh[kHeaderSize];
    RINOK(InStream_SeekSet(stream, pos - memberSize))
    RINOK(ReadStream_FALSE(stream, mh, kHeaderSize))
    if (DictSize(mh) == 0)
    {
      ok = false;
      break;
    }
    unpack += Get64(t + 4);
    members++;
    pos -= memberSize;
  }
  // trailing data after the last member: sizes unknown until decoded
  _sizesKnown = ok;
  if (ok)
  {
    _phySize = fileSize;
    _unpackSize = unpack;
    _numMembers = members;
  }
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::Close())
{
  _stream.Release();
  _dictSize = 0;
  _phySize = 0;
  _unpackSize = 0;
  _numMembers = 0;
  _sizesKnown = false;
  return S_OK;
}

// decodes every member of (in) to (out); (opRes) says how it went
static HRESULT Decode(ISequentialInStream *in, ISequentialOutStream *out,
    ICompressProgressInfo *progress, Int32 &opRes)
{
  opRes = NExtract::NOperationResult::kOK;
  const size_t kBufSize = 1 << 20;
  CByteBuffer buf(kBufSize);
  size_t bufPos = 0, bufLen = 0;
  bool eof = false;
  UInt64 inTotal = 0, outTotal = 0;

  // keeps at least (need) bytes in the buffer unless the input ends
  auto fill = [&](size_t need) -> HRESULT {
    if (bufLen - bufPos >= need || eof)
      return S_OK;
    memmove(buf, buf + bufPos, bufLen - bufPos);
    bufLen -= bufPos;
    bufPos = 0;
    while (bufLen < need && !eof)
    {
      size_t n = kBufSize - bufLen;
      RINOK(ReadStream(in, buf + bufLen, &n))
      if (n == 0)
        eof = true;
      bufLen += n;
    }
    return S_OK;
  };

  CLzmaDec dec;
  LzmaDec_CONSTRUCT(&dec)
  UInt32 allocatedDict = 0;
  HRESULT result = S_OK;

  for (unsigned member = 0; ; member++)
  {
    RINOK(fill(kHeaderSize))
    if (bufLen - bufPos == 0 && member != 0)
      break; // the end
    if (bufLen - bufPos < kHeaderSize)
    {
      opRes = member == 0 ? NExtract::NOperationResult::kUnexpectedEnd : NExtract::NOperationResult::kOK;
      break;
    }
    const UInt32 dict = DictSize(buf + bufPos);
    if (dict == 0)
    {
      // trailing data after a member is allowed, but not a broken first one
      if (member == 0)
        opRes = NExtract::NOperationResult::kDataError;
      break;
    }
    bufPos += kHeaderSize;
    const UInt64 memberStart = inTotal;
    inTotal += kHeaderSize;

    if (dict != allocatedDict)
    {
      if (allocatedDict)
        LzmaDec_Free(&dec, &g_Alloc);
      allocatedDict = 0;
      Byte props[5] = { kLzmaProps, (Byte)dict, (Byte)(dict >> 8), (Byte)(dict >> 16), (Byte)(dict >> 24) };
      if (LzmaDec_Allocate(&dec, props, 5, &g_Alloc) != SZ_OK)
      {
        result = E_OUTOFMEMORY;
        break;
      }
      allocatedDict = dict;
    }
    LzmaDec_Init(&dec);
    dec.dicPos = 0;
    UInt32 crc = CRC_INIT_VAL;
    UInt64 memberOut = 0;
    bool finished = false;
    while (!finished)
    {
      RINOK(fill(1))
      const size_t dicStart = dec.dicPos;
      SizeT inProcessed = bufLen - bufPos;
      ELzmaStatus status;
      const SRes res = LzmaDec_DecodeToDic(&dec, dec.dicBufSize, buf + bufPos, &inProcessed, LZMA_FINISH_ANY, &status);
      bufPos += inProcessed;
      inTotal += inProcessed;
      const size_t produced = dec.dicPos - dicStart;
      if (produced != 0)
      {
        crc = CrcUpdate(crc, dec.dic + dicStart, produced);
        if (out)
          RINOK(WriteStream(out, dec.dic + dicStart, produced))
        memberOut += produced;
        outTotal += produced;
        if (progress)
          RINOK(progress->SetRatioInfo(&inTotal, &outTotal))
      }
      if (dec.dicPos == dec.dicBufSize)
        dec.dicPos = 0;
      if (res != SZ_OK)
      {
        opRes = NExtract::NOperationResult::kDataError;
        break;
      }
      if (status == LZMA_STATUS_FINISHED_WITH_MARK)
        finished = true;
      else if (inProcessed == 0 && produced == 0)
      {
        if (eof && bufPos == bufLen)
        {
          opRes = NExtract::NOperationResult::kUnexpectedEnd;
          break;
        }
        if (status != LZMA_STATUS_NEEDS_MORE_INPUT)
        {
          opRes = NExtract::NOperationResult::kDataError;
          break;
        }
      }
    }
    if (!finished)
      break;
    RINOK(fill(kTrailerSize))
    if (bufLen - bufPos < kTrailerSize)
    {
      opRes = NExtract::NOperationResult::kUnexpectedEnd;
      break;
    }
    const Byte *t = buf + bufPos;
    bufPos += kTrailerSize;
    inTotal += kTrailerSize;
    if (Get32(t) != CRC_GET_DIGEST(crc))
      opRes = NExtract::NOperationResult::kCRCError;
    else if (Get64(t + 4) != memberOut || Get64(t + 12) != inTotal - memberStart)
      opRes = NExtract::NOperationResult::kDataError;
    if (opRes != NExtract::NOperationResult::kOK)
      break;
  }
  if (allocatedDict)
    LzmaDec_Free(&dec, &g_Alloc);
  return result;
}

Z7_COM7F_IMF(CHandler::Extract(const UInt32 *indices, UInt32 numItems,
    Int32 testMode, IArchiveExtractCallback *extractCallback))
{
  COM_TRY_BEGIN
  if (numItems == 0)
    return S_OK;
  if (numItems != (UInt32)(Int32)-1 && (numItems != 1 || indices[0] != 0))
    return E_INVALIDARG;
  if (_sizesKnown)
    RINOK(extractCallback->SetTotal(_phySize))
  CMyComPtr<ISequentialOutStream> outStream;
  const Int32 askMode = testMode ?
      NExtract::NAskMode::kTest :
      NExtract::NAskMode::kExtract;
  RINOK(extractCallback->GetStream(0, &outStream, askMode))
  if (!testMode && !outStream)
    return S_OK;
  RINOK(extractCallback->PrepareOperation(askMode))
  CMyComPtr2_Create<ICompressProgressInfo, CLocalProgress> lps;
  lps->Init(extractCallback, true);
  RINOK(InStream_SeekToBegin(_stream))
  Int32 opRes;
  RINOK(Decode(_stream, outStream, lps, opRes))
  outStream.Release();
  return extractCallback->SetOperationResult(opRes);
  COM_TRY_END
}

static const Byte k_Signature[] = { 'L', 'Z', 'I', 'P', 1 };

REGISTER_ARC_I(
  "lzip", "lz tlz", "* .tar", 0xA3,
  k_Signature,
  0,
  NArcInfoFlags::kKeepName,
  NULL)

}}
