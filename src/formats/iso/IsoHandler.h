// IsoHandler.h -- 7zfm-qt changes: IOutArchive

#ifndef ZIP7_INC_ISO_HANDLER_H
#define ZIP7_INC_ISO_HANDLER_H

#include "Common/MyCom.h"

#include "7zip/Archive/IArchive.h"

#include "IsoIn.h"

namespace NArchive {
namespace NIso {

// 7zfm-qt: IOutArchive, to add, remove and rename files (IsoUpdate.cpp)
Z7_CLASS_IMP_CHandler_IInArchive_2(
  IInArchiveGetStream,
  IOutArchive
)
  CMyComPtr<IInStream> _stream;
  CInArchive _archive;
};

}}

#endif
