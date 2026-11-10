// GUI.cpp -- 7zG: upstream GUI/GUI.cpp (Main2 and WinMain's error handling)
// for "7zfm-qt <command> ...". The command line is 7-Zip's own
// (CArcCmdLineParser, as in 7z and 7zG); the commands run with their dialogs
// and no file manager window. 7zfm-qt: no Win32 startup, no external
// codecs, and the arguments come from argv instead of GetCommandLineW().

#include "Common/MyException.h"
#include "Common/StringConvert.h"

#include "7zip/UI/Common/ArchiveCommandLine.h"
#include "7zip/UI/Common/ExitCode.h"

#include "7zip/UI/FileManager/LangUtils.h"

#include "7zip/UI/GUI/BenchmarkDialog.h"
#include "7zip/UI/GUI/UpdateGUI.h"

#include "7zip/UI/GUI/ExtractRes.h"

#include "ExtractGUI.h"
#include "HashGUI.h"
#include "SevenZipG.h"

using namespace NWindows;

extern bool g_DisableUserQuestions;

static void ErrorMessage(LPCWSTR message)
{
  if (!g_DisableUserQuestions)
    MessageBoxW(NULL, message, L"7-Zip", MB_ICONERROR | MB_OK);
}

static void ErrorMessage(const char *s)
{
  ErrorMessage(GetUnicodeString(s));
}

static void ErrorLangMessage(UINT resourceID)
{
  ErrorMessage(LangString(resourceID));
}

static const char * const kNoFormats = "7-Zip cannot find the code that works with archives.";

static int ShowMemErrorMessage()
{
  ErrorLangMessage(IDS_MEM_ERROR);
  return NExitCode::kMemoryError;
}

static int ShowSysErrorMessage(HRESULT errorCode)
{
  if (errorCode == E_OUTOFMEMORY)
    return ShowMemErrorMessage();
  ErrorMessage(HResultToMessage(errorCode));
  return NExitCode::kFatalError;
}

static void ThrowException_if_Error(HRESULT res)
{
  if (res != S_OK)
    throw CSystemException(res);
}

static int Main2(UStringVector &commandStrings)
{
  if (commandStrings.Size() == 0)
  {
    MessageBoxW(NULL, L"Specify command", L"7-Zip", 0);
    return 0;
  }

  CArcCmdLineOptions options;
  CArcCmdLineParser parser;

  parser.Parse1(commandStrings, options);
  g_DisableUserQuestions = options.YesToAll;
  parser.Parse2(options);

  CREATE_CODECS_OBJECT

  codecs->CaseSensitive_Change = options.CaseSensitive_Change;
  codecs->CaseSensitive = options.CaseSensitive;
  ThrowException_if_Error(codecs->Load());
  Codecs_AddHashArcHandler(codecs);

  const bool isExtractGroupCommand = options.Command.IsFromExtractGroup();

  if (codecs->Formats.Size() == 0 &&
        (isExtractGroupCommand
        || options.Command.IsFromUpdateGroup()))
    throw kNoFormats;

  CObjectVector<COpenType> formatIndices;
  if (!ParseOpenTypes(*codecs, options.ArcType, formatIndices))
  {
    ErrorLangMessage(IDS_UNSUPPORTED_ARCHIVE_TYPE);
    return NExitCode::kFatalError;
  }

  CIntVector excludedFormats;
  FOR_VECTOR (k, options.ExcludedArcTypes)
  {
    CIntVector tempIndices;
    if (!codecs->FindFormatForArchiveType(options.ExcludedArcTypes[k], tempIndices)
        || tempIndices.Size() != 1)
    {
      ErrorLangMessage(IDS_UNSUPPORTED_ARCHIVE_TYPE);
      return NExitCode::kFatalError;
    }
    excludedFormats.AddToUniqueSorted(tempIndices[0]);
    // excludedFormats.Sort();
  }

  if (options.Command.CommandType == NCommandType::kBenchmark)
  {
    HRESULT res = Benchmark(
        EXTERNAL_CODECS_VARS_L
        options.Properties,
        options.NumIterations_Defined ?
          options.NumIterations :
          k_NumBenchIterations_Default);
    ThrowException_if_Error(res);
  }
  else if (isExtractGroupCommand)
  {
    UStringVector ArchivePathsSorted;
    UStringVector ArchivePathsFullSorted;

    CExtractCallbackImp *ecs = new CExtractCallbackImp;
    CMyComPtr<IFolderArchiveExtractCallback> extractCallback = ecs;

    #ifndef Z7_NO_CRYPTO
    ecs->PasswordIsDefined = options.PasswordEnabled;
    ecs->Password = options.Password;
    #endif

    ecs->Init();

    CExtractOptions eo;
    (CExtractOptionsBase &)eo = options.ExtractOptions;
    eo.StdInMode = options.StdInMode;
    eo.StdOutMode = options.StdOutMode;
    eo.YesToAll = options.YesToAll;
    ecs->YesToAll = options.YesToAll;
    eo.TestMode = options.Command.IsTestCommand();
    ecs->TestMode = eo.TestMode;

    #ifndef Z7_SFX
    eo.Properties = options.Properties;
    #endif

    bool messageWasDisplayed = false;

    #ifndef Z7_SFX
    CHashBundle hb;
    CHashBundle *hb_ptr = NULL;

    if (!options.HashMethods.IsEmpty())
    {
      hb_ptr = &hb;
      ThrowException_if_Error(hb.SetMethods(EXTERNAL_CODECS_VARS_L options.HashMethods));
    }
    #endif

    {
      CDirItemsStat st;
      HRESULT hresultMain = EnumerateDirItemsAndSort(
          options.arcCensor,
          NWildcard::k_RelatPath,
          UString(), // addPathPrefix
          ArchivePathsSorted,
          ArchivePathsFullSorted,
          st,
          NULL // &scan: change it!!!!
          );
      if (hresultMain != S_OK)
        throw CSystemException(hresultMain);
    }

    ecs->MultiArcMode = (ArchivePathsSorted.Size() > 1);

    HRESULT result = ExtractGUI(
          codecs,
          formatIndices, excludedFormats,
          ArchivePathsSorted,
          ArchivePathsFullSorted,
          options.Censor.Pairs.Front().Head,
          eo,
          #ifndef Z7_SFX
          hb_ptr,
          #endif
          options.ShowDialog, messageWasDisplayed, ecs);
    if (result != S_OK)
    {
      if (result != E_ABORT && messageWasDisplayed)
        return NExitCode::kFatalError;
      throw CSystemException(result);
    }
    if (!ecs->IsOK())
      return NExitCode::kFatalError;
  }
  else if (options.Command.IsFromUpdateGroup())
  {
    #ifndef Z7_NO_CRYPTO
    bool passwordIsDefined = options.PasswordEnabled && !options.Password.IsEmpty();
    #endif

    CUpdateCallbackGUI callback;
    // callback.EnablePercents = options.EnablePercents;

    #ifndef Z7_NO_CRYPTO
    callback.PasswordIsDefined = passwordIsDefined;
    callback.AskPassword = options.PasswordEnabled && options.Password.IsEmpty();
    callback.Password = options.Password;
    #endif

    // callback.StdOutMode = options.UpdateOptions.StdOutMode;
    callback.Init();

    if (!options.UpdateOptions.InitFormatIndex(codecs, formatIndices, options.ArchiveName) ||
        !options.UpdateOptions.SetArcPath(codecs, options.ArchiveName))
    {
      ErrorLangMessage(IDS_UPDATE_NOT_SUPPORTED);
      return NExitCode::kFatalError;
    }
    bool messageWasDisplayed = false;
    HRESULT result = UpdateGUI(
        codecs, formatIndices,
        options.ArchiveName,
        options.Censor,
        options.UpdateOptions,
        options.ShowDialog,
        messageWasDisplayed,
        &callback);

    if (result != S_OK)
    {
      if (result != E_ABORT && messageWasDisplayed)
        return NExitCode::kFatalError;
      throw CSystemException(result);
    }
    if (callback.FailedFiles.Size() > 0)
    {
      if (!messageWasDisplayed)
        throw CSystemException(E_FAIL);
      return NExitCode::kWarning;
    }
  }
  else if (options.Command.CommandType == NCommandType::kHash)
  {
    bool messageWasDisplayed = false;
    HRESULT result = HashCalcGUI(EXTERNAL_CODECS_VARS_L
        options.Censor, options.HashOptions, messageWasDisplayed);

    if (result != S_OK)
    {
      if (result != E_ABORT && messageWasDisplayed)
        return NExitCode::kFatalError;
      throw CSystemException(result);
    }
  }
  else
  {
    throw "Unsupported command";
  }
  return 0;
}

// 7zfm-qt: the commands 7zG runs (Main2 above): a d u, e x t, h, b
bool Is7zGCommand(const UString &s)
{
  static const char * const kCommands[] = { "a", "b", "d", "e", "h", "t", "u", "x" };
  for (unsigned i = 0; i < Z7_ARRAY_SIZE(kCommands); i++)
    if (s.IsEqualTo_Ascii_NoCase(kCommands[i]))
      return true;
  return false;
}

// upstream WinMain, from its try block on
int Run7zG(UStringVector &commandStrings)
{
  try
  {
    return Main2(commandStrings);
  }
  catch(const CNewException &)
  {
    return ShowMemErrorMessage();
  }
  catch(const CMessagePathException &e)
  {
    ErrorMessage(e);
    return NExitCode::kUserError;
  }
  catch(const CSystemException &systemError)
  {
    if (systemError.ErrorCode == E_ABORT)
      return NExitCode::kUserBreak;
    return ShowSysErrorMessage(systemError.ErrorCode);
  }
  catch(const UString &s)
  {
    ErrorMessage(s);
    return NExitCode::kFatalError;
  }
  catch(const AString &s)
  {
    ErrorMessage(s);
    return NExitCode::kFatalError;
  }
  catch(const wchar_t *s)
  {
    ErrorMessage(s);
    return NExitCode::kFatalError;
  }
  catch(const char *s)
  {
    ErrorMessage(s);
    return NExitCode::kFatalError;
  }
  catch(int v)
  {
    AString e ("Error: ");
    e.Add_UInt32((unsigned)v);
    ErrorMessage(e);
    return NExitCode::kFatalError;
  }
  catch(...)
  {
    ErrorMessage("Unknown error");
    return NExitCode::kFatalError;
  }
}
