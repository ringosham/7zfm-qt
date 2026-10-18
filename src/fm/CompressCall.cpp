// CompressCall.cpp -- copied from upstream FileManager/CompressCall2.cpp
// (the in-process variant: ExtractGUI/UpdateGUI/HashCalcGUI run here instead
// of in a 7zG.exe child process). CompressFiles() and CalcChecksum() are
// added with the phases that bring UpdateGUI and the hash menu.

#include "Common/MyException.h"

#include "7zip/UI/Common/EnumDirItems.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/GUI/ExtractRes.h"

#include "CompressCall.h"
#include "ExtractGUI.h"
#include "HashGUI.h"
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

#define LOAD_EXTERNAL_CODECS

UString GetQuotedString(const UString &s)
{
  UString s2 ('\"');
  s2 += s;
  s2.Add_Char('\"');
  return s2;
}

static void ErrorMessage(LPCWSTR message)
{
  if (g_UiHooks)
    g_UiHooks->ShowError(UString(message));
}

static void ErrorMessageHRESULT(HRESULT res)
{
  ErrorMessage(HResultToMessage(res));
}

static void ErrorLangMessage(UINT resourceID)
{
  ErrorMessage(LangString(resourceID));
}

static HRESULT ExtractGroupCommand(const UStringVector &arcPaths,
    bool showDialog, CExtractOptions &eo, const char *kType = NULL)
{
  MY_TRY_BEGIN
  
  CREATE_CODECS

  CExtractCallbackImp *ecs = new CExtractCallbackImp;
  CMyComPtr<IFolderArchiveExtractCallback> extractCallback = ecs;
  
  ecs->Init();
  
  // eo.CalcCrc = options.CalcCrc;
  
  UStringVector arcPathsSorted;
  UStringVector arcFullPathsSorted;
  {
    NWildcard::CCensor arcCensor;
    FOR_VECTOR (i, arcPaths)
    {
      arcCensor.AddPreItem_NoWildcard(arcPaths[i]);
    }
    arcCensor.AddPathsToCensor(NWildcard::k_RelatPath);
    CDirItemsStat st;
    EnumerateDirItemsAndSort(arcCensor, NWildcard::k_RelatPath, UString(),
        arcPathsSorted, arcFullPathsSorted,
        st,
        NULL // &scan: change it!!!!
        );
  }
  
  CObjectVector<COpenType> formatIndices;
  if (kType)
  {
    if (!ParseOpenTypes(*codecs, UString(kType), formatIndices))
    {
      throw CSystemException(E_INVALIDARG);
      // ErrorLangMessage(IDS_UNSUPPORTED_ARCHIVE_TYPE);
      // return E_INVALIDARG;
    }
  }
  
  NWildcard::CCensor censor;
  {
    censor.AddPreItem_Wildcard();
  }
  
  censor.AddPathsToCensor(NWildcard::k_RelatPath);

  bool messageWasDisplayed = false;

  ecs->MultiArcMode = (arcPathsSorted.Size() > 1);

  result = ExtractGUI(codecs,
      formatIndices, CIntVector(),
      arcPathsSorted, arcFullPathsSorted,
      censor.Pairs.Front().Head, eo, NULL, showDialog, messageWasDisplayed, ecs, g_HWND);

  if (result != S_OK)
  {
    if (result != E_ABORT && messageWasDisplayed)
      return E_FAIL;
    throw CSystemException(result);
  }
  return ecs->IsOK() ? S_OK : E_FAIL;
  
  MY_TRY_FINISH
  return result;
}

void ExtractArchives(const UStringVector &arcPaths, const UString &outFolder,
    bool showDialog, bool elimDup, UInt32 writeZone)
{
  CExtractOptions eo;
  eo.OutputDir = us2fs(outFolder);
  eo.TestMode = false;
  eo.ElimDup.Val = elimDup;
  eo.ElimDup.Def = elimDup;
  if (writeZone != (UInt32)(Int32)-1)
    eo.ZoneMode = (NExtract::NZoneIdMode::EEnum)writeZone;
  ExtractGroupCommand(arcPaths, showDialog, eo);
}

void TestArchives(const UStringVector &arcPaths, bool hashMode)
{
  CExtractOptions eo;
  eo.TestMode = true;
  ExtractGroupCommand(arcPaths,
      true, // showDialog
      eo,
      hashMode ? "hash" : NULL);
}

