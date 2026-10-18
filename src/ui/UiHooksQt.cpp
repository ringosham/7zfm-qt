// UiHooksQt.cpp

#include "UiHooksQt.h"

#include "PasswordDialog.h"
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
  ProgressDialog::Run(_parent, sync, ToQ(title), work, waitMode);
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
