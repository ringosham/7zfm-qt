// Messages.cpp -- HResultToMessage (upstream ProgressDialog2.cpp) and
// GetOpenArcErrorMessage (upstream ExtractCallback.cpp), copied verbatim.

#include "Messages.h"

#include "Common/IntToString.h"
#include "Windows/ErrorMsg.h"

#include "7zip/Archive/IArchive.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/resourceGui.h"
#include "7zip/UI/GUI/ExtractRes.h"
#include "7zip/UI/FileManager/MemDialogRes.h"

#include "UiHooks.h"

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

// from upstream FileManager/MemDialog.cpp
static void AddSize_GB(UString &s, UInt32 size_GB, UInt32 id)
{
  s.Add_LF();
  s += "    ";
  s.Add_UInt32(size_GB);
  s += " GB : ";
  AddLangString(s, id);
}

void CMemUseQuestion::AddInfoMessage_To_String(UString &s, const UInt32 *ramSize_GB) const
{
  AddLangString(s, IDS_MEM_REQUIRES_BIG_MEM);
  AddSize_GB(s, Required_GB, IDS_MEM_REQUIRED_MEM_SIZE);
  AddSize_GB(s, Limit_GB, IDS_MEM_CURRENT_MEM_LIMIT);
  if (ramSize_GB)
    AddSize_GB(s, *ramSize_GB, IDS_MEM_RAM_SIZE);
  if (!FilePath.IsEmpty())
  {
    s.Add_LF();
    s += "File: ";
    s += FilePath;
  }
}

// from upstream FileManager/OverwriteDialog.cpp
#include "7zip/UI/FileManager/FormatUtils.h"
#include "7zip/UI/FileManager/OverwriteDialogRes.h"

void AddSizeValue(UString &s, UInt64 value);
void AddSizeValue(UString &s, UInt64 value)
{
  {
    wchar_t sz[32];
    ConvertUInt64ToString(value, sz);
    s += MyFormatNew(IDS_FILE_SIZE, sz);
  }
  if (value >= (1 << 10))
  {
    char c;
          if (value >= ((UInt64)10 << 30)) { value >>= 30; c = 'G'; }
    else  if (value >=         (10 << 20)) { value >>= 20; c = 'M'; }
    else                                   { value >>= 10; c = 'K'; }
    s += " : ";
    s.Add_UInt64(value);
    s.Add_Space();
    s.Add_Char(c);
    s += "iB";
  }
}
