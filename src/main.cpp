// main.cpp -- 7zfm-qt entry point.

#include "Common/Common.h"
#include "Common/StringConvert.h"

#include "Agent.h"
#include "7zip/UI/FileManager/ViewSettings.h"

#include "ui/MainWindow.h"
#include "ui/Panel.h"
#include "ui/UiHooksQt.h"
#include "FmProgress.h"
#include "QtConv.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMessageBox>
#include <QTimer>

#include <functional>
#include <memory>

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
  UiHooksQt hooks(&w);
  g_UiHooks = &hooks;
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
      keys += args[++i].split(QLatin1Char(','), Qt::SkipEmptyParts);
    else if (args[i] == QLatin1String("--select") && i + 1 < args.size())
      keys.prepend(QStringLiteral("@select:") + args[++i]);
    else if (args[i] == QLatin1String("--action") && i + 1 < args.size())
      keys.append(QStringLiteral("@action:") + args[++i]);
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
    // replay keys one by one (so modal dialogs can receive them), then grab
    // the active window
    auto step = std::make_shared<std::function<void(int)>>();
    *step = [&w, screenshot, keys, step](int i) {
      if (i < keys.size())
      {
        // schedule the next step first: this key may open a modal dialog
        QTimer::singleShot(400, qApp, [step, i]() { (*step)(i + 1); });
        if (keys[i].startsWith(QLatin1String("@select:")))
        {
          w.GetPanel()->SelectByName(keys[i].mid(8));
          return;
        }
        if (keys[i].startsWith(QLatin1String("@action:")))
        {
          const QString a = keys[i].mid(8);
          if (a == QLatin1String("extract")) w.GetPanel()->ExtractArchives();
          else if (a == QLatin1String("test")) w.GetPanel()->TestArchives();
          else if (a == QLatin1String("properties")) w.GetPanel()->Properties();
          return;
        }
        const QKeySequence seq(keys[i]);
        if (!seq.isEmpty())
        {
          const QKeyCombination kc = seq[0];
          QWidget *target = QApplication::focusWidget();
          if (!target)
            target = QApplication::activeModalWidget() ? QApplication::activeModalWidget() : &w;
          QKeyEvent press(QEvent::KeyPress, kc.key(), kc.keyboardModifiers());
          QApplication::sendEvent(target, &press);
          QKeyEvent release(QEvent::KeyRelease, kc.key(), kc.keyboardModifiers());
          QApplication::sendEvent(target, &release);
        }
        return;
      }
      QWidget *top = QApplication::activeModalWidget();
      if (!top)
        top = &w;
      top->grab().save(screenshot);
      QApplication::exit(0);
    };
    QTimer::singleShot(300, qApp, [step]() { (*step)(0); });
  }
  return app.exec();
}
