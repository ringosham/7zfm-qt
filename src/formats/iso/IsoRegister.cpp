// IsoRegister.cpp -- 7zfm-qt changes: registered with IOutArchive

#include "Common/Common.h"

#include "7zip/Common/RegisterArc.h"

#include "IsoHandler.h"

namespace NArchive {
namespace NIso {

static const Byte k_Signature[] = { 'C', 'D', '0', '0', '1' };

// 7zfm-qt: with IOutArchive (adding and removing files in an image)
REGISTER_ARC_IO(
  "Iso", "iso img", NULL, 0xE7,
  k_Signature,
  NArchive::NIso::kStartPos + 1,
  NArcInfoFlags::kMTime | NArcInfoFlags::kMTime_Default,
  TIME_PREC_TO_ARC_FLAGS_MASK (NFileTimeType::kUnix)
  | TIME_PREC_TO_ARC_FLAGS_TIME_DEFAULT (NFileTimeType::kUnix),
  NULL)

}}
