// Messages.cpp -- HResultToMessage (upstream ProgressDialog2.cpp) and
// GetOpenArcErrorMessage (upstream ExtractCallback.cpp), copied verbatim.

#include "Messages.h"

#include "Common/IntToString.h"
#include "Windows/ErrorMsg.h"

#include "7zip/Archive/IArchive.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/resourceGui.h"
#include "7zip/UI/GUI/ExtractRes.h"

using namespace NWindows;

UString HResultToMessage(HRESULT errorCode)
{
  if (errorCode == E_OUTOFMEMORY)
    return LangString(IDS_MEM_ERROR);
  else
    return NError::MyFormatMessage(errorCode);
}

static const UInt32 k_ErrorFlagsIds[] =
{
  IDS_EXTRACT_MSG_IS_NOT_ARC,
  IDS_EXTRACT_MSG_HEADERS_ERROR,
  IDS_EXTRACT_MSG_HEADERS_ERROR,
  IDS_OPEN_MSG_UNAVAILABLE_START,
  IDS_OPEN_MSG_UNCONFIRMED_START,
  IDS_EXTRACT_MSG_UEXPECTED_END,
  IDS_EXTRACT_MSG_DATA_AFTER_END,
  IDS_EXTRACT_MSG_UNSUPPORTED_METHOD,
  IDS_OPEN_MSG_UNSUPPORTED_FEATURE,
  IDS_EXTRACT_MSG_DATA_ERROR,
  IDS_EXTRACT_MSG_CRC_ERROR
};

UString GetOpenArcErrorMessage(UInt32 errorFlags)
{
  UString s;

  for (unsigned i = 0; i < Z7_ARRAY_SIZE(k_ErrorFlagsIds); i++)
  {
    const UInt32 f = (UInt32)1 << i;
    if ((errorFlags & f) == 0)
      continue;
    const UInt32 id = k_ErrorFlagsIds[i];
    UString m = LangString(id);
    if (m.IsEmpty())
      continue;
    if (f == kpv_ErrorFlags_EncryptedHeadersError)
    {
      m += " : ";
      AddLangString(m, IDS_EXTRACT_MSG_WRONG_PSW_GUESS);
    }
    if (!s.IsEmpty())
      s.Add_LF();
    s += m;
    errorFlags &= ~f;
  }

  if (errorFlags != 0)
  {
    char sz[16];
    sz[0] = '0';
    sz[1] = 'x';
    ConvertUInt32ToHex(errorFlags, sz + 2);
    if (!s.IsEmpty())
      s.Add_LF();
    s += sz;
  }

  return s;
}
