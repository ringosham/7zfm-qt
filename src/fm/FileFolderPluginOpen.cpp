// FileFolderPluginOpen.cpp -- port of upstream FileManager/FileFolderPluginOpen.cpp.
// The error-reporting helpers are copied verbatim; OpenFileFolderPlugin()
// keeps upstream's flow but runs the open through IUiHooks (Qt progress
// dialog + worker thread) instead of CProgressDialog + CThread.

#include "FileFolderPluginOpen.h"

#include "Windows/FileName.h"
#include "Windows/PropVariant.h"

#include "Agent.h"
#include "7zip/UI/FileManager/FormatUtils.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/PropertyName.h"
#include "7zip/UI/FileManager/resourceGui.h"
#include "7zip/UI/GUI/ExtractRes.h"

#include "Messages.h"
#include "OpenCallback.h"

using namespace NWindows;

static void SplitNameToPureNameAndExtension(const FString &fullName,
    FString &pureName, FString &extensionDelimiter, FString &extension)
{
  const int index = fullName.ReverseFind_Dot();
  if (index < 0)
  {
    pureName = fullName;
    extensionDelimiter.Empty();
    extension.Empty();
  }
  else
  {
    pureName.SetFrom(fullName, (unsigned)index);
    extensionDelimiter = '.';
    extension = fullName.Ptr((unsigned)index + 1);
  }
}


struct CArcLevelInfo
{
  UString Error;
  UString Path;
  UString Type;
  UString ErrorType;
  UString ErrorFlags;
};


struct CArcLevelsInfo
{
  CObjectVector<CArcLevelInfo> Levels; // LastLevel Is NON-OPEN
};




static void GetFolderLevels(CMyComPtr<IFolderFolder> &folder, CArcLevelsInfo &levels)
{
  levels.Levels.Clear();

  CMyComPtr<IGetFolderArcProps> getFolderArcProps;
  folder.QueryInterface(IID_IGetFolderArcProps, &getFolderArcProps);
  
  if (!getFolderArcProps)
    return;
  CMyComPtr<IFolderArcProps> arcProps;
  getFolderArcProps->GetFolderArcProps(&arcProps);
  if (!arcProps)
    return;

  UInt32 numLevels;
  if (arcProps->GetArcNumLevels(&numLevels) != S_OK)
    numLevels = 0;
  
  for (UInt32 level = 0; level <= numLevels; level++)
  {
    const PROPID propIDs[] = { kpidError, kpidPath, kpidType, kpidErrorType };

    CArcLevelInfo lev;
    
    for (Int32 i = 0; i < 4; i++)
    {
      CMyComBSTR name;
      NCOM::CPropVariant prop;
      if (arcProps->GetArcProp(level, propIDs[i], &prop) != S_OK)
        continue;
      if (prop.vt != VT_EMPTY)
      {
        UString *s = NULL;
        switch (propIDs[i])
        {
          case kpidError: s = &lev.Error; break;
          case kpidPath: s = &lev.Path; break;
          case kpidType: s = &lev.Type; break;
          case kpidErrorType: s = &lev.ErrorType; break;
        }
        *s = (prop.vt == VT_BSTR) ? prop.bstrVal : L"?";
      }
    }

    {
      NCOM::CPropVariant prop;
      if (arcProps->GetArcProp(level, kpidErrorFlags, &prop) == S_OK)
      {
        UInt32 flags = GetOpenArcErrorFlags(prop);
        if (flags != 0)
          lev.ErrorFlags = GetOpenArcErrorMessage(flags);
      }
    }

    levels.Levels.Add(lev);
  }
}
    
static UString GetBracedType(const wchar_t *type)
{
  UString s ('[');
  s += type;
  s.Add_Char(']');
  return s;
}

void GetFolderError(CMyComPtr<IFolderFolder> &folder, UString &open_Errors, UString &nonOpen_Errors)
{
  CArcLevelsInfo levs;
  GetFolderLevels(folder, levs);
  open_Errors.Empty();
  nonOpen_Errors.Empty();

  FOR_VECTOR (i, levs.Levels)
  {
    bool isNonOpenLevel = (i == 0);
    const CArcLevelInfo &lev = levs.Levels[levs.Levels.Size() - 1 - i];

    UString m;
    
    if (!lev.ErrorType.IsEmpty())
    {
      m = MyFormatNew(IDS_CANT_OPEN_AS_TYPE, GetBracedType(lev.ErrorType));
      if (!isNonOpenLevel)
      {
        m.Add_LF();
        m += MyFormatNew(IDS_IS_OPEN_AS_TYPE, GetBracedType(lev.Type));
      }
    }

    if (!lev.Error.IsEmpty())
    {
      if (!m.IsEmpty())
        m.Add_LF();
      m += GetBracedType(lev.Type);
      m += " : ";
      m += GetNameOfProperty(kpidError, L"Error");
      m += " : ";
      m += lev.Error;
    }

    if (!lev.ErrorFlags.IsEmpty())
    {
      if (!m.IsEmpty())
        m.Add_LF();
      m += GetNameOfProperty(kpidErrorFlags, L"Errors");
      m += ": ";
      m += lev.ErrorFlags;
    }
    
    if (!m.IsEmpty())
    {
      if (isNonOpenLevel)
      {
        UString &s = nonOpen_Errors;
        s += lev.Path;
        s.Add_LF();
        s += m;
      }
      else
      {
        UString &s = open_Errors;
        if (!s.IsEmpty())
          s += "--------------------\n";
        s += lev.Path;
        s.Add_LF();
        s += m;
      }
    }
  }
}


HRESULT CFfpOpen::OpenFileFolderPlugin(IInStream *inStream,
    const FString &path, const UString &arcFormat, IUiHooks *ui)
{
  FString extension, name, pureName, dot;

  const int slashPos = path.ReverseFind_PathSepar();
  FString dirPrefix;
  FString fileName;
  if (slashPos >= 0)
  {
    dirPrefix.SetFrom(path, (unsigned)(slashPos + 1));
    fileName = path.Ptr((unsigned)(slashPos + 1));
  }
  else
    fileName = path;

  SplitNameToPureNameAndExtension(fileName, pureName, dot, extension);

  ErrorMessage.Empty();

  CMyComPtr<IFolderManager> folderManager = new CArchiveFolderManager;

  COpenArchiveCallback openCallbackSpec_loc;
  COpenArchiveCallback *openCallback = &openCallbackSpec_loc;
  openCallback->PasswordIsDefined = Encrypted;
  openCallback->Password = Password;
  openCallback->Ui = ui;

  /* COpenCallbackImp object will exist after Open stage for multivolume archives */
  COpenCallbackImp *openCallbackSpec = new COpenCallbackImp;
  CMyComPtr<IProgress> openCallbackProgress = openCallbackSpec;
  // we set pointer without reference counter:
  openCallbackSpec->Callback = openCallback;

  if (inStream)
    openCallbackSpec->SetSubArchiveName(fs2us(fileName));
  else
  {
    RINOK(openCallbackSpec->Init2(dirPrefix, fileName))
  }

  const UString fullPath = fs2us(path);
  CMyComPtr<IFolderFolder> folder;
  HRESULT result = E_FAIL;

  ui->RunWithProgress(openCallback->Sync, LangString(IDS_OPENNING), [&]()
  {
    try
    {
      result = folderManager->OpenFolderFile(inStream, fullPath, arcFormat, &folder, openCallbackProgress);
    }
    catch(...) { result = E_FAIL; }
  }, true);

  /*
    if archive is multivolume:
    COpenCallbackImp object will exist after Open stage.
    COpenCallbackImp object will be deleted when last reference
    from each volume object (CInFileStreamVol) will be closed (when archive will be closed).
  */
  openCallbackProgress.Release();

  if (result != S_FALSE && result != S_OK)
    return result;

  if (folder)
  {
    UString open_Errors, nonOpen_Errors;
    GetFolderError(folder, open_Errors, nonOpen_Errors);
    if (!nonOpen_Errors.IsEmpty())
      ErrorMessage = nonOpen_Errors;
  }

  Encrypted = openCallback->PasswordIsDefined;
  Password = openCallback->Password;

  if (result == S_OK)
    Folder = folder;

  return result;
}
