// UiHooks.h -- what the ported FileManager logic (src/fm) needs from the UI.
// Upstream calls Win32 dialogs directly; here src/ui implements this
// interface with Qt, so src/fm stays free of widget code.

#ifndef Z7QT_UI_HOOKS_H
#define Z7QT_UI_HOOKS_H

#include "Common/Common.h"
#include "Common/MyString.h"
#include "Common/MyVector.h"

#include <functional>

class CProgressSync;
class CExtractDialog;

// upstream NOverwriteDialog::CFileInfo
struct COverwriteFileInfo
{
  bool Size_IsDefined = false;
  bool Time_IsDefined = false;
  bool Is_FileSystemFile = false;
  UInt64 Size = 0;
  FILETIME Time;
  UString Path;

  void SetTime2(const FILETIME *t) { Time_IsDefined = (t != NULL); if (t) Time = *t; }
  void SetSize2(const UInt64 *size) { Size_IsDefined = (size != NULL); if (size) Size = *size; }
};

// answers of upstream COverwriteDialog
enum EOverwriteAnswer
{
  kOverwrite_Cancel,
  kOverwrite_Yes,
  kOverwrite_No,
  kOverwrite_YesToAll,
  kOverwrite_NoToAll,
  kOverwrite_AutoRename
};

// fields of upstream CMemDialog
struct CMemUseQuestion
{
  UInt32 Limit_GB = 0;
  UInt32 Required_GB = 0;
  bool TestMode = false;
  bool ShowRemember = false;
  UString ArcPath;
  UString FilePath;
  // out:
  bool NeedSave = false;
  bool Remember = false;
  bool SkipArc = false;

  // upstream CMemDialog::AddInfoMessage_To_String (Messages.cpp)
  void AddInfoMessage_To_String(UString &s, const UInt32 *ramSize_GB = NULL) const;
};

struct IUiHooks
{
  virtual ~IUiHooks() {}

  // Runs (work) on a worker thread while a progress dialog shows (sync).
  // Returns after (work) finished; the UI keeps processing events meanwhile.
  // waitMode: show the dialog only if the work takes noticeable time
  // (upstream CProgressDialog::WaitMode).
  virtual void RunWithProgress(CProgressSync &sync, const UString &title,
      const std::function<void()> &work, bool waitMode) = 0;

  // May be called from the worker thread; blocks until the user answers.
  virtual bool AskPassword(UString &password) = 0;

  virtual void ShowError(const UString &message) = 0;

  // May be called from the worker thread.
  virtual EOverwriteAnswer AskOverwrite(const COverwriteFileInfo &oldFile, const COverwriteFileInfo &newFile) = 0;
  // false: stop the operation
  virtual bool AskMemoryUse(CMemUseQuestion &q) = 0;

  // upstream CExtractDialog: true = OK
  virtual bool RunExtractDialog(CExtractDialog &dialog) = 0;

  // upstream CListViewDialog (hash results, properties)
  virtual void ShowPropertyList(const UString &title, const UStringVector &names, const UStringVector &values) = 0;
};

#endif
