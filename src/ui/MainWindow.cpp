// MainWindow.cpp

#include "MainWindow.h"

#include "Common/Common.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/resource.h"
#include "7zip/UI/FileManager/ViewSettings.h"

#include "Panel.h"
#include "CompressCall.h"
#include "QtConv.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>

// Menu text from upstream's resources, with Windows' "\tShortcut" removed
// (Qt shows the shortcut itself).
static QString M(UInt32 id, const char *fallback = "")
{
  const UString s = LangString(id);
  QString q = s.IsEmpty() ? QString::fromLatin1(fallback) : ToQ(s);
  const int tab = q.indexOf(QLatin1Char('\t'));
  if (tab >= 0)
    q.truncate(tab);
  return q;
}

static QIcon ThemeIcon(const char *name)
{
  return QIcon::fromTheme(QString::fromLatin1(name));
}

MainWindow::MainWindow()
{
  setWindowTitle(QStringLiteral("7-Zip"));
  setWindowIcon(QIcon::fromTheme(QStringLiteral("package-x-generic")));
  _panel = new Panel(this);
  setCentralWidget(_panel);
  _statusLabel = new QLabel;
  statusBar()->addWidget(_statusLabel, 1);

  CreateMenus();
  CreateToolBar();

  connect(_panel, &Panel::StatusChanged, this, &MainWindow::UpdateStatus);
  connect(_panel, &Panel::FolderChanged, this, &MainWindow::UpdateTitle);

  QSettings s;
  if (!restoreGeometry(s.value(QStringLiteral("MainWindow/Geometry")).toByteArray()))
    resize(1000, 650);
  {
    UStringVector folders;
    ReadFolderHistory(folders);
    QStringList l;
    FOR_VECTOR (i, folders)
      l << ToQ(folders[i]);
    _panel->SetHistory(l);
  }
  _panel->SetViewMode(s.value(QStringLiteral("MainWindow/ViewMode"), 3).toInt());
  _viewActions[_panel->ViewMode()]->setChecked(true);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
  QSettings s;
  s.setValue(QStringLiteral("MainWindow/Geometry"), saveGeometry());
  s.setValue(QStringLiteral("MainWindow/ViewMode"), _panel->ViewMode());
  _panel->SaveListViewInfo();
  {
    UStringVector folders;
    for (const QString &p : _panel->History())
      folders.Add(ToU(p));
    SaveFolderHistory(folders);
  }
  SavePanelPath(0, _panel->CurrentFolderPrefix());
  QMainWindow::closeEvent(e);
}

void MainWindow::UpdateStatus()
{
  _statusLabel->setText(_panel->StatusText());
}

void MainWindow::UpdateTitle(const QString &path)
{
  setWindowTitle(path.isEmpty() ? QStringLiteral("7-Zip") : path);
}

void MainWindow::OpenInsideAction(const wchar_t *type)
{
  _panel->OpenFocusedItemAsInternal(type);
}

void MainWindow::CreateMenus()
{
  // ---- File (upstream IDM_FILE) ----
  QMenu *file = menuBar()->addMenu(M(IDM_FILE, "&File"));
  QAction *a;
  a = file->addAction(M(IDM_OPEN, "&Open"), this, [this]() { _panel->OpenSelectedItems(true); });
  a->setShortcutContext(Qt::WidgetShortcut);
  a = file->addAction(M(IDM_OPEN_INSIDE, "Open &Inside"), this, [this]() { OpenInsideAction(NULL); });
  a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_PageDown));
  a->setShortcutContext(Qt::WidgetShortcut); // handled by the panel's key filter
  file->addAction(M(IDM_OPEN_INSIDE_ONE, "Open Inside *"), this, [this]() { OpenInsideAction(L"*"); });
  file->addAction(M(IDM_OPEN_INSIDE_PARSER, "Open Inside #"), this, [this]() { OpenInsideAction(L"#"); });
  a = file->addAction(M(IDM_OPEN_OUTSIDE, "Open O&utside"), this, [this]() { _panel->OpenFocusedItemOutside(); });
  a->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Return));
  a->setShortcutContext(Qt::WidgetShortcut);
  a = file->addAction(M(IDM_FILE_VIEW, "&View"), this, [this]() { _panel->EditItem(false); });
  a->setShortcut(QKeySequence(Qt::Key_F3));
  a->setShortcutContext(Qt::WidgetShortcut);
  a = file->addAction(M(IDM_FILE_EDIT, "&Edit"), this, [this]() { _panel->EditItem(true); });
  a->setShortcut(QKeySequence(Qt::Key_F4));
  a->setShortcutContext(Qt::WidgetShortcut);
  file->addSeparator();
  a = file->addAction(M(IDM_RENAME, "Rena&me"), this, [this]() { _panel->RenameFile(); });
  a->setShortcut(QKeySequence(Qt::Key_F2));
  a->setShortcutContext(Qt::WidgetShortcut);
  a = file->addAction(M(IDM_COPY_TO, "&Copy To..."), this, [this]() { _panel->CopyToDialog(false); });
  a->setShortcut(QKeySequence(Qt::Key_F5));
  a->setShortcutContext(Qt::WidgetShortcut);
  a = file->addAction(M(IDM_MOVE_TO, "&Move To..."), this, [this]() { _panel->CopyToDialog(true); });
  a->setShortcut(QKeySequence(Qt::Key_F6));
  a->setShortcutContext(Qt::WidgetShortcut);
  a = file->addAction(M(IDM_DELETE, "&Delete"), this, [this]() { _panel->DeleteItems(true); });
  a->setShortcut(QKeySequence(Qt::Key_Delete));
  a->setShortcutContext(Qt::WidgetShortcut);
  file->addSeparator();
  file->addAction(M(IDM_LINK, "&Link..."), this, [this]() { _panel->Link(); });
  file->addAction(M(IDM_SPLIT, "&Split file..."), this, [this]() { _panel->Split(); });
  file->addAction(M(IDM_COMBINE, "Com&bine files..."), this, [this]() { _panel->Combine(); });
  file->addSeparator();
  a = file->addAction(M(IDM_COMMENT, "Comme&nt..."), this, [this]() { _panel->ChangeComment(); });
  a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z));
  a->setShortcutContext(Qt::WidgetShortcut);
  {
    QMenu *crc = file->addMenu(QStringLiteral("CRC"));
    static const char *const kMethods[] = { "CRC32", "CRC64", "XXH64", "MD5", "SHA1", "SHA256",
        "SHA384", "SHA512", "SHA3-256", "BLAKE2sp", "*" };
    static const char *const kNames[] = { "CRC-32", "CRC-64", "XXH64", "MD5", "SHA-1", "SHA-256",
        "SHA-384", "SHA-512", "SHA3-256", "BLAKE2sp", "*" };
    for (unsigned i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
    {
      const char *m = kMethods[i];
      crc->addAction(QString::fromLatin1(kNames[i]), this, [this, m]() { _panel->CalculateCrc(m); });
    }
  }
  file->addSeparator();
  a = file->addAction(M(IDM_CREATE_FOLDER, "Create Folder"), this, [this]() { _panel->CreateFolder(); });
  a->setShortcut(QKeySequence(Qt::Key_F7));
  a->setShortcutContext(Qt::WidgetShortcut);
  a = file->addAction(M(IDM_CREATE_FILE, "Create File"), this, [this]() { _panel->CreateFile(); });
  a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_N));
  a->setShortcutContext(Qt::WidgetShortcut);
  file->addSeparator();
  a = file->addAction(M(IDM_PROPERTIES, "P&roperties"), this, [this]() { _panel->Properties(); });
  a->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
  file->addSeparator();
  a = file->addAction(tr("E&xit"), this, &QWidget::close);
  a->setShortcut(QKeySequence(Qt::ALT | Qt::Key_F4));

  // ---- Edit ----
  QMenu *edit = menuBar()->addMenu(M(IDM_EDIT, "&Edit"));
  a = edit->addAction(M(IDM_SELECT_ALL, "Select &All"), this, [this]() { _panel->SelectAll(true); });
  a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_A));
  edit->addAction(M(IDM_DESELECT_ALL, "Deselect All"), this, [this]() { _panel->SelectAll(false); });
  edit->addAction(M(IDM_INVERT_SELECTION, "&Invert Selection"), this, [this]() { _panel->InvertSelection(); });
  edit->addAction(M(IDM_SELECT, "Select..."), this, [this]() { _panel->SelectByMask(true); });
  edit->addAction(M(IDM_DESELECT, "Deselect..."), this, [this]() { _panel->SelectByMask(false); });

  // ---- View ----
  QMenu *view = menuBar()->addMenu(M(IDM_VIEW, "&View"));
  auto *group = new QActionGroup(this);
  const UInt32 ids[4] = { IDM_VIEW_LARGE_ICONS, IDM_VIEW_SMALL_ICONS, IDM_VIEW_LIST, IDM_VIEW_DETAILS };
  const char *fall[4] = { "Lar&ge Icons", "S&mall Icons", "&List", "&Details" };
  for (int i = 0; i < 4; i++)
  {
    _viewActions[i] = view->addAction(M(ids[i], fall[i]), this, [this, i]() { _panel->SetViewMode(i); });
    _viewActions[i]->setCheckable(true);
    _viewActions[i]->setShortcut(QKeySequence(Qt::CTRL | (Qt::Key_1 + i)));
    group->addAction(_viewActions[i]);
  }
  view->addSeparator();
  a = view->addAction(M(IDM_OPEN_ROOT_FOLDER, "Open Root Folder"), this, [this]() { _panel->OpenRootFolder(); });
  a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Backslash));
  a = view->addAction(M(IDM_OPEN_PARENT_FOLDER, "Up One Level"), this, [this]() { _panel->OpenParentFolder(); });
  a->setShortcutContext(Qt::WidgetShortcut); // Backspace handled by the panel
  view->addSeparator();
  a = view->addAction(M(IDM_VIEW_REFRESH, "R&efresh"), this, [this]() { _panel->RefreshListCtrl_SaveFocused(); });
  a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
  a = view->addAction(tr("Go to path"), this, [this]() { _panel->FocusAddressBar(); });
  a->setShortcuts({ QKeySequence(Qt::CTRL | Qt::Key_L), QKeySequence(Qt::ALT | Qt::Key_D) });

  // ---- Favorites (upstream IDM_FAVORITES, AppState FastFolders) ----
  _favoritesMenu = menuBar()->addMenu(M(IDM_FAVORITES, "F&avorites"));
  RebuildFavoritesMenu();

  // ---- Tools ----
  QMenu *tools = menuBar()->addMenu(M(IDM_TOOLS, "&Tools"));
  tools->addAction(tr("&Add to archive..."), this, [this]() { _panel->AddToArchive(); });
  tools->addAction(tr("E&xtract..."), this, [this]() { _panel->ExtractArchives(); });
  tools->addAction(tr("&Test"), this, [this]() { _panel->TestArchives(); });
  tools->addSeparator();
  tools->addAction(M(IDM_BENCHMARK, "&Benchmark"), this, []() { Benchmark(false); });
  tools->addAction(M(IDM_BENCHMARK2, "Benchmark 2"), this, []() { Benchmark(true); });

  // ---- Help ----
  QMenu *help = menuBar()->addMenu(M(IDM_HELP, "&Help"));
  help->addAction(M(IDM_ABOUT, "&About 7-Zip..."), this, &MainWindow::About);
}

void MainWindow::CreateToolBar()
{
  QToolBar *tb = addToolBar(tr("Standard"));
  tb->setObjectName(QStringLiteral("StandardToolbar"));
  tb->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  tb->setMovable(false);
  tb->addAction(ThemeIcon("go-up"), M(IDM_OPEN_PARENT_FOLDER, "Up"), this, [this]() { _panel->OpenParentFolder(); });
  tb->addAction(ThemeIcon("computer"), M(IDM_OPEN_ROOT_FOLDER, "Root"), this, [this]() { _panel->OpenRootFolder(); });
  tb->addSeparator();
  tb->addAction(ThemeIcon("archive-insert"), tr("Add"), this, [this]() { _panel->AddToArchive(); });
  tb->addAction(ThemeIcon("archive-extract"), tr("Extract"), this, [this]() { _panel->ExtractArchives(); });
  tb->addAction(ThemeIcon("dialog-ok-apply"), tr("Test"), this, [this]() { _panel->TestArchives(); });
  tb->addAction(ThemeIcon("edit-copy"), tr("Copy"), this, [this]() { _panel->CopyToDialog(false); });
  tb->addAction(ThemeIcon("go-jump"), tr("Move"), this, [this]() { _panel->CopyToDialog(true); });
  tb->addAction(ThemeIcon("edit-delete"), tr("Delete"), this, [this]() { _panel->DeleteItems(true); });
  tb->addSeparator();
  tb->addAction(ThemeIcon("document-properties"), tr("Info"), this, [this]() { _panel->Properties(); });
}

void MainWindow::RebuildFavoritesMenu()
{
  _favoritesMenu->clear();
  UStringVector fast;
  ReadFastFolders(fast);
  QMenu *add = _favoritesMenu->addMenu(M(IDM_ADD_TO_FAVORITES, "&Add folder to Favorites as"));
  _favoritesMenu->addSeparator();
  for (int i = 0; i < 10; i++)
  {
    const QString path = (unsigned)i < fast.Size() ? ToQ(fast[(unsigned)i]) : QString();
    QAction *set = add->addAction(tr("Bookmark %1").arg(i), this, [this, i]() {
      UStringVector f;
      ReadFastFolders(f);
      while (f.Size() <= (unsigned)i)
        f.Add(UString());
      f[(unsigned)i] = _panel->CurrentFolderPrefix();
      SaveFastFolders(f);
      RebuildFavoritesMenu();
    });
    set->setShortcut(QKeySequence(Qt::ALT | Qt::SHIFT | (Qt::Key_0 + i)));
    QAction *go = _favoritesMenu->addAction(QStringLiteral("%1  %2").arg(i).arg(path.isEmpty() ? QStringLiteral("-") : path),
        this, [this, path]() {
      if (!path.isEmpty())
      {
        _panel->BindToPathAndRefresh(ToU(path));
        _panel->FocusList();
      }
    });
    go->setShortcut(QKeySequence(Qt::ALT | (Qt::Key_0 + i)));
    go->setEnabled(!path.isEmpty());
  }
}

void MainWindow::About()
{
  QMessageBox::about(this, tr("About 7-Zip"),
      QStringLiteral("<b>7zfm-qt</b> %1<br><br>"
      "A Qt port of the 7-Zip File Manager.<br>"
      "7-Zip engine 26.04, Copyright (c) 1999-2026 Igor Pavlov.<br>"
      "GNU LGPL (with unRAR license restriction).").arg(QStringLiteral(Z7QT_VERSION)));
}
