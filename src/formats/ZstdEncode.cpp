// ZstdEncode.cpp -- 7zfm-qt: see ZstdEncode.h

#include "Common/MyBuffer.h"
#include "Common/MyWindows.h"

#include "7zip/Common/StreamUtils.h"

#include "ZstdEncode.h"

#include <zstd.h>

namespace NZ7Qt {

static int ZstdLevel(UInt32 level)
{
  static const Byte kLevels[] = { 1, 1, 2, 2, 3, 3, 6, 12, 15, 19 };
  if (level < Z7_ARRAY_SIZE(kLevels))
    return kLevels[level];
  return (int)MyMin(level, (UInt32)ZSTD_maxCLevel());
}

HRESULT ZstdCompress(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    UInt32 level, UInt32 numThreads, const UInt64 *contentSize,
    ICompressProgressInfo *progress)
{
  ZSTD_CCtx *const cctx = ZSTD_createCCtx();
  if (!cctx)
    return E_OUTOFMEMORY;
  struct CFree { ZSTD_CCtx *c; ~CFree() { ZSTD_freeCCtx(c); } } freeCtx = { cctx };
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, ZstdLevel(level));
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1);
  // a libzstd built without threads keeps 0 workers: single-threaded
  if (numThreads > 1)
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, (int)numThreads);
  if (contentSize)
    ZSTD_CCtx_setPledgedSrcSize(cctx, *contentSize);

  const size_t inSize = ZSTD_CStreamInSize();
  const size_t outSize = ZSTD_CStreamOutSize();
  CByteBuffer inBuf(inSize), outBuf(outSize);
  UInt64 inTotal = 0, outTotal = 0;
  for (;;)
  {
    size_t read = inSize;
    RINOK(ReadStream(inStream, inBuf, &read))
    inTotal += read;
    const ZSTD_EndDirective mode = read < inSize ? ZSTD_e_end : ZSTD_e_continue;
    ZSTD_inBuffer in = { inBuf, read, 0 };
    for (;;)
    {
      ZSTD_outBuffer out = { outBuf, outSize, 0 };
      const size_t rem = ZSTD_compressStream2(cctx, &out, &in, mode);
      // a pledged size the input doesn't match (the file changed) ends here
      if (ZSTD_isError(rem))
        return E_FAIL;
      RINOK(WriteStream(outStream, outBuf, out.pos))
      outTotal += out.pos;
      if (mode == ZSTD_e_end ? rem == 0 : in.pos == in.size)
        break;
    }
    if (progress)
      RINOK(progress->SetRatioInfo(&inTotal, &outTotal))
    if (mode == ZSTD_e_end)
      return S_OK;
  }
}

}
