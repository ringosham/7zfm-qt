// OptionsDialog.cpp -- upstream FileManager/OptionsDialog.cpp. The System
// (file associations) and 7-Zip (Explorer context menu) pages configure
// Windows shell integration and are left out; Language needs lang files.

#include "Windows/Control/Dialog.h"
#include "Windows/Control/PropertyPage.h"

#include "7zip/UI/FileManager/EditPage.h"
#include "7zip/UI/FileManager/EditPageRes.h"
#include "7zip/UI/FileManager/FoldersPage.h"
#include "7zip/UI/FileManager/FoldersPageRes.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/SettingsPage.h"
#include "7zip/UI/FileManager/SettingsPageRes.h"
#include "7zip/UI/FileManager/resource.h"

using namespace NWindows;

// returns true if the user pressed OK or Apply
bool OptionsDialog();
bool OptionsDialog()
{
  CFoldersPage foldersPage;
  CEditPage editPage;
  CSettingsPage settingsPage;

  CObjectVector<NControl::CPageInfo> pages;
  const UINT pageIDs[] = {
      IDD_SETTINGS,
      IDD_FOLDERS,
      IDD_EDIT };
  NControl::CPropertyPage *pagePointers[] = { &settingsPage, &foldersPage, &editPage };

  for (unsigned i = 0; i < Z7_ARRAY_SIZE(pageIDs); i++)
  {
    NControl::CPageInfo &page = pages.AddNew();
    page.ID = pageIDs[i];
    page.Page = pagePointers[i];
  }
  const INT_PTR res = NControl::MyPropertySheet(pages, NULL, LangString(IDS_OPTIONS));
  return res != -1 && res != 0;
}

// FileManager/FM.cpp: large pages need privileges on Windows; 7zfm-qt does
// not use them.
bool IsLargePageSupported();
bool IsLargePageSupported() { return false; }
