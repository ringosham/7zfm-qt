// ZstdEncode.h -- 7zfm-qt: zstd compression with the system libzstd, for
// .zst (ZstdHandler.cpp) and .tar.zst (TarCompound.cpp). Upstream ships
// only the decoder.

#ifndef Z7QT_ZSTD_ENCODE_H
#define Z7QT_ZSTD_ENCODE_H

#include "7zip/ICoder.h"
#include "7zip/IStream.h"

namespace NZ7Qt {

// level is 7-Zip's (Store 0 .. Ultra 9, mapped on zstd's 1..19); above 9
// it is a zstd level itself, up to 22. contentSize, when not NULL, goes in
// the frame header as zstd's command line writes it, and the input must be
// exactly that long. progress (optional) gets input and output sizes.
HRESULT ZstdCompress(ISequentialInStream *inStream, ISequentialOutStream *outStream,
    UInt32 level, UInt32 numThreads, const UInt64 *contentSize,
    ICompressProgressInfo *progress);

}

#endif
