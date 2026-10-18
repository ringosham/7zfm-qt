// FileFolderPluginOpen.h -- port of upstream FileManager/FileFolderPluginOpen.h.

#ifndef Z7QT_FILE_FOLDER_PLUGIN_OPEN_H
#define Z7QT_FILE_FOLDER_PLUGIN_OPEN_H

#include "Common/MyCom.h"
#include "7zip/UI/FileManager/IFolder.h"

#include "UiHooks.h"

struct CFfpOpen
{
  Z7_CLASS_NO_COPY(CFfpOpen)
public:
  // in/out:
  bool Encrypted;
  UString Password;

  CMyComPtr<IFolderFolder> Folder;
  UString ErrorMessage;

  CFfpOpen(): Encrypted(false) {}

  HRESULT OpenFileFolderPlugin(IInStream *inStream,
      const FString &path, const UString &arcFormat, IUiHooks *ui);
};

void GetFolderError(CMyComPtr<IFolderFolder> &folder, UString &open_Errors, UString &nonOpen_Errors);

#endif
