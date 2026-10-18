// ExtractDialog.h -- the data side of upstream GUI/ExtractDialog.h. The
// fields are upstream's; Create() shows the Qt dialog (ui/ExtractDialog.cpp)
// through g_UiHooks, so the copied ExtractGUI.cpp is unchanged.

#ifndef Z7QT_EXTRACT_DIALOG_H
#define Z7QT_EXTRACT_DIALOG_H

#include "Common/MyString.h"
#include "Common/MyTypes.h"

#include "7zip/UI/Common/ExtractMode.h"
#include "7zip/UI/Common/ZipRegistry.h"

#include "FmProgress.h"

#ifndef IDOK
#define IDOK 1
#define IDCANCEL 2
#endif

class CExtractDialog
{
public:
  UString DirPath;
  UString ArcPath;
  UString Password;
  bool PathMode_Force;
  bool OverwriteMode_Force;
  NExtract::NPathMode::EEnum PathMode;
  NExtract::NOverwriteMode::EEnum OverwriteMode;
  CBoolPair NtSecurity;
  CBoolPair ElimDup;

  CExtractDialog():
    PathMode_Force(false),
    OverwriteMode_Force(false),
    PathMode(NExtract::NPathMode::kFullPaths),
    OverwriteMode(NExtract::NOverwriteMode::kAsk)
  {
    ElimDup.Val = true;
  }

  INT_PTR Create(HWND parent = NULL);
};

#endif
