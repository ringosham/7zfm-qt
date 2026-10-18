// FmProgress.cpp -- CProgressThreadVirt::Process() copied from upstream
// FileManager/ProgressDialog2.cpp; Create() runs it under the Qt dialog.

#include "FmProgress.h"

#include "Common/StringConvert.h"

#include "Messages.h"

using namespace NWindows;

IUiHooks *g_UiHooks = NULL;

HRESULT CProgressThreadVirt::Create(const UString &title, HWND /* parentWindow */)
{
  UString t = MainAddTitle;
  t += title;
  if (!Ui)
    return E_FAIL;
  Ui->RunWithProgress(Sync, title, [this]() { Process(); }, WaitMode);
  ProcessWasFinished_GuiVirt();
  return S_OK;
}

static void AddMessageToString(UString &dest, const UString &src)
{
  if (!src.IsEmpty())
  {
    if (!dest.IsEmpty())
      dest.Add_LF();
    dest += src;
  }
}

void CProgressThreadVirt::Process()
{
  UString m;
  try { Result = ProcessVirt(); }
  catch(const wchar_t *s) { m = s; }
  catch(const UString &s) { m = s; }
  catch(const char *s) { m = GetUnicodeString(s); }
  catch(int v)
  {
    m = "Error #";
    m.Add_UInt32((unsigned)v);
  }
  catch(...) { m = "Error"; }
  if (Result != E_ABORT)
  {
    if (m.IsEmpty() && Result != S_OK)
      m = HResultToMessage(Result);
  }
  AddMessageToString(m, FinalMessage.ErrorMessage.Message);

  {
    FOR_VECTOR(i, ErrorPaths)
    {
      if (i >= 32)
        break;
      AddMessageToString(m, fs2us(ErrorPaths[i]));
    }
  }

  CProgressSync &sync = Sync;
  NSynchronization::CCriticalSectionLock lock(sync._cs);
  if (m.IsEmpty())
  {
    if (!FinalMessage.OkMessage.Message.IsEmpty())
      sync.FinalMessage.OkMessage = FinalMessage.OkMessage;
  }
  else
  {
    sync.FinalMessage.ErrorMessage.Message = m;
    if (Result == S_OK)
      Result = E_FAIL;
  }
  ThreadFinishedOK = true;
}
