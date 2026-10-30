// main.cpp -- 7zfm-qt entry point.

#include "Common/Common.h"
#include "Common/StringConvert.h"

#include "Agent.h"
#include "7zip/UI/FileManager/ViewSettings.h"

#include "ui/MainWindow.h"
#include "ui/Panel.h"
#include "ui/UiHooksQt.h"
#include "ui/DndExtract.h"
#include "FmProgress.h"
#include "CompressCall.h"
#include "QtConv.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMimeData>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMessageBox>
#include <QTimer>
#include <QtTest/QTest>

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
  // open the start path from the event loop: the window appears first, and
  // a slow open (or its password prompt) runs with the usual progress UI.
  // Without one, each panel reopens its last folder, like upstream.
  QTimer::singleShot(0, &w, [&w, start]() { w.OpenStartPaths(start); });

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
        if (keys[i] == QLatin1String("@wait"))
          return;
        if (keys[i].startsWith(QLatin1String("@type:")))
        {
          QWidget *target = QApplication::focusWidget();
          if (target)
            for (const QChar ch : keys[i].mid(6))
            {
              QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch));
              QApplication::sendEvent(target, &press);
            }
          return;
        }
        if (keys[i].startsWith(QLatin1String("@action:")))
        {
          const QString a = keys[i].mid(8);
          if (a == QLatin1String("extract")) w.GetPanel()->ExtractArchives();
          else if (a == QLatin1String("add")) w.GetPanel()->AddToArchive();
          else if (a == QLatin1String("split")) w.GetPanel()->Split();
          else if (a.startsWith(QLatin1String("hash:"))) w.GetPanel()->CalculateCrc(qstrdup(a.mid(5).toLatin1().constData()));
          else if (a == QLatin1String("benchmark")) Benchmark(false);
          else if (a == QLatin1String("copy")) w.OnCopy(false, false);
          else if (a == QLatin1String("move")) w.OnCopy(true, false);
          else if (a == QLatin1String("flat")) w.GetPanel()->SetFlatMode(true);
          else if (a == QLatin1String("options")) { extern bool OptionsDialog(); OptionsDialog(); }
          else if (a == QLatin1String("combine")) w.GetPanel()->Combine();
          else if (a.startsWith(QLatin1String("rename:"))) w.GetPanel()->RenameFocused(a.mid(7));
          else if (a.startsWith(QLatin1String("drop:"))) w.GetPanel()->DropFiles(a.mid(5).split(QLatin1Char(';')), false);
          else if (a == QLatin1String("test")) w.GetPanel()->TestArchives();
          else if (a == QLatin1String("properties")) w.GetPanel()->Properties();
          else if (a.startsWith(QLatin1String("dnd:")))
          {
            // a drop on Dolphin: KIO's DropJob calls the Ark DnD object
            std::unique_ptr<QMimeData> mime(w.GetPanel()->CreateDragData());
            // nothing may be extracted before the drop
            if (mime && !w.GetPanel()->DragItemsExtracted()
                && mime->hasFormat(QLatin1String(DndExtract::ServiceMimeType())))
            {
              QDBusMessage m = QDBusMessage::createMethodCall(
                  QString::fromUtf8(mime->data(QLatin1String(DndExtract::ServiceMimeType()))),
                  QString::fromUtf8(mime->data(QLatin1String(DndExtract::PathMimeType()))),
                  QStringLiteral("org.kde.ark.DndExtract"), QStringLiteral("extractSelectedFilesTo"));
              m.setArguments({a.mid(4)});
              QDBusConnection::sessionBus().asyncCall(m);
            }
          }
          else if (a.startsWith(QLatin1String("dnduri:")))
          {
            // a drop on a target that reads the URLs: extracted on request
            std::unique_ptr<QMimeData> mime(w.GetPanel()->CreateDragData());
            if (mime && !w.GetPanel()->DragItemsExtracted())
              for (const QUrl &u : mime->urls())
                QFile::copy(u.toLocalFile(), a.mid(7) + QLatin1Char('/') + u.fileName());
          }
          return;
        }
        const QKeySequence seq(keys[i]);
        if (!seq.isEmpty())
        {
          const QKeyCombination kc = seq[0];
          // keys go to the active modal dialog if there is one
          QWidget *target = nullptr;
          if (QWidget *modal = QApplication::activeModalWidget())
            target = modal->focusWidget() ? modal->focusWidget() : modal;
          if (!target)
            target = QApplication::focusWidget() ? QApplication::focusWidget() : &w;
          // QTest goes through shortcut processing like a real key press
          QTest::keyClick(target, kc.key(), kc.keyboardModifiers());
        }
        return;
      }
      QWidget *top = QApplication::activeModalWidget();
      if (!top)
        top = &w;
      top->grab().save(screenshot);
      // and the text of an open dialog, for tests to check messages
      if (top != &w)
      {
        QFile f(screenshot + QStringLiteral(".txt"));
        if (f.open(QIODevice::WriteOnly))
          for (const QLabel *l : top->findChildren<QLabel *>())
            if (l->isVisible() && !l->text().isEmpty())
              f.write((l->text() + QLatin1Char('\n')).toUtf8());
      }
      QApplication::exit(0);
    };
    QTimer::singleShot(300, qApp, [step]() { (*step)(0); });
  }
  return app.exec();
}
