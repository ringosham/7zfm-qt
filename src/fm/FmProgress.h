// FmProgress.h -- stand-ins for upstream CProgressDialog / CProgressThreadVirt
// (FileManager/ProgressDialog2.h). Copied FileManager/GUI code keeps using
// them unchanged: .Sync for progress, Create() to run ProcessVirt() on a
// worker thread. The window itself is the Qt ProgressDialog, reached
// through IUiHooks.

#ifndef Z7QT_FM_PROGRESS_H
#define Z7QT_FM_PROGRESS_H

// Same guard as upstream FileManager/ProgressDialog2.h: when this header is
// force-included, upstream sources that include ProgressDialog2.h (the GUI
// update callbacks) get these stand-ins instead of the Win32 dialog.
#define ZIP7_INC_PROGRESS_DIALOG_2_H

#include "Common/MyString.h"
#include "Common/MyVector.h"

#include "ProgressSync.h"
#include "UiHooks.h"
#include "Messages.h"

#include "7zip/UI/FileManager/ProgressDialog2Res.h"

#ifndef _WIN32
typedef void *HWND;
#endif

// set by main(); used by every CProgressDialog
extern IUiHooks *g_UiHooks;

class CProgressDialog
{
public:
  CProgressSync Sync;
  IUiHooks *Ui;
  HWND MainWindow;
  UString MainTitle;
  UString MainAddTitle;
  bool WaitMode;
  bool MessagesDisplayed;
  bool CompressingMode;
  bool ShowCompressionInfo;
  int IconID;

  // upstream passes the progress window as the parent of its prompts; ours
  // are parented by IUiHooks
  operator HWND() const { return NULL; }

  CProgressDialog():
      Ui(g_UiHooks),
      MainWindow(NULL),
      WaitMode(false),
      MessagesDisplayed(false),
      CompressingMode(true),
      ShowCompressionInfo(true),
      IconID(-1)
      {}
  virtual ~CProgressDialog() {}

  void WaitCreating() {}
  void MyClose() {}
  // upstream calls this on the GUI thread once the work is done
  virtual void ProcessWasFinished_GuiVirt() {}
};

class CProgressCloser
{
public:
  explicit CProgressCloser(CProgressDialog &) {}
};

class CProgressThreadVirt: public CProgressDialog
{
protected:
  FStringVector ErrorPaths;
  CProgressFinalMessage FinalMessage;

  // error if any of HRESULT, ErrorMessage, ErrorPath
  virtual HRESULT ProcessVirt() = 0;
public:
  HRESULT Result;
  bool ThreadFinishedOK; // if there is no fatal exception

  void Process();
  void AddErrorPath(const FString &path) { ErrorPaths.Add(path); }

  HRESULT Create(const UString &title, HWND parentWindow = NULL);
  CProgressThreadVirt(): Result(E_FAIL), ThreadFinishedOK(false) {}

  CProgressMessageBoxPair &GetMessagePair(bool isError) { return isError ? FinalMessage.ErrorMessage : FinalMessage.OkMessage; }
};

#endif
