// UiHooks.h -- what the ported FileManager logic (src/fm) needs from the UI.
// Upstream calls Win32 dialogs directly; here src/ui implements this
// interface with Qt, so src/fm stays free of widget code.

#ifndef Z7QT_UI_HOOKS_H
#define Z7QT_UI_HOOKS_H

#include "Common/Common.h"
#include "Common/MyString.h"

#include <functional>

class CProgressSync;

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
};

#endif
