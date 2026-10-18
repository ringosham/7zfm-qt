// CompressCall2.cpp -- CompressFiles() from upstream FileManager/CompressCall2.cpp
// (in-process: UpdateGUI shows upstream's CompressDialog through Q32).
// ExtractArchives()/TestArchives() of the same upstream file live in
// src/fm/CompressCall.cpp.

#include "Common/MyException.h"

#include "7zip/UI/Common/EnumDirItems.h"
#include "7zip/UI/Common/HashCalc.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/GUI/ExtractRes.h"
#include "7zip/UI/GUI/UpdateGUI.h"

#include "CompressCall.h"
#include "Messages.h"

static HWND g_HWND = NULL;

#define MY_TRY_BEGIN  HRESULT result; try {
#define MY_TRY_FINISH } \
  catch(CSystemException &e) { result = e.ErrorCode; } \
  catch(UString &s) { ErrorMessage(s); result = E_FAIL; } \
  catch(...) { result = E_FAIL; } \
  if (result != S_OK && result != E_ABORT) \
    ErrorMessageHRESULT(result);

static void ThrowException_if_Error(HRESULT res)
{
  if (res != S_OK)
    throw CSystemException(res);
}

#define CREATE_CODECS \
  CCodecs *codecs = new CCodecs; \
  CMyComPtr<IUnknown> compressCodecsInfo = codecs; \
  ThrowException_if_Error(codecs->Load()); \
  Codecs_AddHashArcHandler(codecs);

static void ErrorMessage(LPCWSTR message)
{
  MessageBoxW(g_HWND, message, L"7-Zip", MB_ICONERROR);
}

static void ErrorMessageHRESULT(HRESULT res)
{
  ErrorMessage(HResultToMessage(res));
}

static void ErrorLangMessage(UINT resourceID)
{
  ErrorMessage(LangString(resourceID));
}

HRESULT CompressFiles(
    const UString &arcPathPrefix,
    const UString &arcName,
    const UString &arcType,
    bool addExtension,
    const UStringVector &names,
    bool email, bool showDialog, bool /* waitFinish */)
{
  MY_TRY_BEGIN

  CREATE_CODECS

  CUpdateCallbackGUI callback;

  callback.Init();

  CUpdateOptions uo;
  uo.EMailMode = email;
  uo.SetActionCommand_Add();

  uo.ArcNameMode = (addExtension ? k_ArcNameMode_Add : k_ArcNameMode_Exact);

  CObjectVector<COpenType> formatIndices;
  if (!ParseOpenTypes(*codecs, arcType, formatIndices))
  {
    ErrorLangMessage(IDS_UNSUPPORTED_ARCHIVE_TYPE);
    return E_FAIL;
  }
  const UString arcPath = arcPathPrefix + arcName;
  if (!uo.InitFormatIndex(codecs, formatIndices, arcPath) ||
      !uo.SetArcPath(codecs, arcPath))
  {
    ErrorLangMessage(IDS_UPDATE_NOT_SUPPORTED);
    return E_FAIL;
  }

  NWildcard::CCensor censor;
  FOR_VECTOR (i, names)
  {
    censor.AddPreItem_NoWildcard(names[i]);
  }

  bool messageWasDisplayed = false;

  result = UpdateGUI(codecs,
      formatIndices, arcPath,
      censor, uo, showDialog, messageWasDisplayed, &callback, g_HWND);

  if (result != S_OK)
  {
    if (result != E_ABORT && messageWasDisplayed)
      return E_FAIL;
    throw CSystemException(result);
  }
  if (callback.FailedFiles.Size() > 0)
  {
    if (!messageWasDisplayed)
      throw CSystemException(E_FAIL);
    return E_FAIL;
  }

  MY_TRY_FINISH
  return S_OK;
}
