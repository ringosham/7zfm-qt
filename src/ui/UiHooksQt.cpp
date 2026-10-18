// UiHooksQt.cpp

#include "UiHooksQt.h"

#include "../fm/ExtractDialog.h"
#include "ExtractDialogQt.h"
#include "OverwriteDialog.h"
#include "PanelProperties.h"
#include "FolderModel.h"
#include "PasswordDialog.h"
#include "PropertiesDialog.h"
#include "ProgressDialog.h"
#include "QtConv.h"

#include <QApplication>
#include <QMessageBox>
#include <QThread>

void RunOnGuiThread(const std::function<void()> &f)
{
  if (QThread::currentThread() == qApp->thread())
    f();
  else
    QMetaObject::invokeMethod(qApp, f, Qt::BlockingQueuedConnection);
}

QWidget *UiHooksQt::DialogParent() const
{
  QWidget *w = QApplication::activeModalWidget();
  if (!w)
    w = QApplication::activeWindow();
  if (!w)
    w = _parent;
  return w;
}

void UiHooksQt::RunWithProgress(CProgressSync &sync, const UString &title,
    const std::function<void()> &work, bool waitMode)
{
  FolderAccess_Block();
  ProgressDialog::Run(_parent, sync, ToQ(title), work, waitMode);
  FolderAccess_Unblock();
}

bool UiHooksQt::AskPassword(UString &password)
{
  bool ok = false;
  RunOnGuiThread([&]() {
    PasswordDialog d(DialogParent());
    ok = (d.exec() == QDialog::Accepted);
    if (ok)
      password = ToU(d.Password());
  });
  return ok;
}

void UiHooksQt::ShowError(const UString &message)
{
  RunOnGuiThread([&]() {
    QMessageBox::critical(DialogParent(), QStringLiteral("7-Zip"), ToQ(message));
  });
}

EOverwriteAnswer UiHooksQt::AskOverwrite(const COverwriteFileInfo &oldFile, const COverwriteFileInfo &newFile)
{
  EOverwriteAnswer a = kOverwrite_Cancel;
  RunOnGuiThread([&]() { a = RunOverwriteDialog(DialogParent(), oldFile, newFile); });
  return a;
}

bool UiHooksQt::AskMemoryUse(CMemUseQuestion &q)
{
  bool ok = false;
  RunOnGuiThread([&]() { ok = RunMemDialog(DialogParent(), q); });
  return ok;
}

void UiHooksQt::ShowPropertyList(const UString &title, const UStringVector &names, const UStringVector &values)
{
  RunOnGuiThread([&]() {
    CPropList list;
    list.Strings = names;
    list.Values = values;
    ShowPropertiesDialog(DialogParent(), ToQ(title), list);
  });
}

bool UiHooksQt::RunExtractDialog(CExtractDialog &dialog)
{
  bool ok = false;
  RunOnGuiThread([&]() { ok = ::RunExtractDialog(DialogParent(), dialog); });
  return ok;
}

// upstream CExtractDialog::Create() shows the dialog
INT_PTR CExtractDialog::Create(HWND)
{
  if (!g_UiHooks)
    return IDCANCEL;
  return g_UiHooks->RunExtractDialog(*this) ? IDOK : IDCANCEL;
}
