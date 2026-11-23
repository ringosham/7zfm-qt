// IsoUpdate.h -- 7zfm-qt: adding, removing and renaming files in an
// existing ISO 9660 image (see IsoUpdate.cpp)

#ifndef Z7QT_ISO_UPDATE_H
#define Z7QT_ISO_UPDATE_H

#include "7zip/Archive/IArchive.h"

#include "IsoIn.h"

namespace NArchive {
namespace NIso {

// Why (arc) can't be updated, or NULL if it can.
const char *GetUpdateRefusal(const CInArchive &arc);

// Writes (arc) with the changes (callback) asks for to (outStream), which
// must be seekable. (handler) gives the kept items' paths and modes.
HRESULT UpdateIso(IInStream *inStream, const CInArchive &arc, IInArchive *handler,
    ISequentialOutStream *outStream, UInt32 numItems, IArchiveUpdateCallback *callback);

}}

#endif
