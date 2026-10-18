// main.cpp -- 7zfm-qt entry point.

#include "Common/Common.h"
#include "Common/StringConvert.h"

#include "Agent.h"
#include "7zip/UI/FileManager/ViewSettings.h"

#include "ui/MainWindow.h"
#include "ui/Panel.h"
#include "QtConv.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMessageBox>
#include <QTimer>

int main(int argc, char *argv[])
{
  QApplication app(argc, argv);
  QApplication::setOrganizationName(QStringLiteral("7zfm-qt"));
  QApplication::setApplicationName(QStringLiteral("7zfm-qt"));
  QApplication::setApplicationVersion(QStringLiteral(Z7QT_VERSION));
  QApplication::setDesktopFileName(QStringLiteral("7zfm-qt"));

  // like 7zz: pick up the user's locale; file names are UTF-8
  MY_SetLocale();

  if (LoadGlobalCodecs() != S_OK)
  {
    QMessageBox::critical(nullptr, QStringLiteral("7-Zip"), QStringLiteral("Cannot load codecs"));
    return 1;
  }

  MainWindow w;
  w.show();

  QString start;
  QString screenshot; // development aid: --screenshot FILE grabs the window and quits
  QStringList keys;   // with --screenshot: --keys "Ctrl+PgDown,Down,Return" replays keys first
  const QStringList args = app.arguments();
  for (int i = 1; i < args.size(); i++)
  {
    if (args[i] == QLatin1String("--screenshot") && i + 1 < args.size())
      screenshot = args[++i];
    else if (args[i] == QLatin1String("--keys") && i + 1 < args.size())
      keys = args[++i].split(QLatin1Char(','), Qt::SkipEmptyParts);
    else if (start.isEmpty())
      start = QFileInfo(args[i]).absoluteFilePath();
  }
  if (start.isEmpty())
  {
    // like upstream: reopen the last folder
    UString last;
    if (ReadPanelPath(0, last) && !last.IsEmpty())
      start = ToQ(last);
    else
      start = QDir::homePath() + QLatin1Char('/');
  }
  w.GetPanel()->BindToPathAndRefresh(ToU(start));
  w.GetPanel()->FocusList();

  if (!screenshot.isEmpty())
  {
    QTimer::singleShot(300, &w, [&w, screenshot, keys]() {
      for (const QString &k : keys)
      {
        const QKeySequence seq(k);
        if (seq.isEmpty())
          continue;
        const QKeyCombination kc = seq[0];
        QWidget *target = QApplication::focusWidget() ? QApplication::focusWidget() : &w;
        QKeyEvent press(QEvent::KeyPress, kc.key(), kc.keyboardModifiers());
        QApplication::sendEvent(target, &press);
        QKeyEvent release(QEvent::KeyRelease, kc.key(), kc.keyboardModifiers());
        QApplication::sendEvent(target, &release);
        QApplication::processEvents();
      }
      QTimer::singleShot(200, &w, [&w, screenshot]() {
        w.grab().save(screenshot);
        QApplication::quit();
      });
    });
  }
  return app.exec();
}
