// MainWindow.cpp

#include "MainWindow.h"

#include "Common/Common.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/resource.h"
#include "7zip/UI/FileManager/ViewSettings.h"

#include "Panel.h"
#include "CompressCall.h"

bool OptionsDialog();
#include "QtConv.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QDir>
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
  {
    QIcon icon;
    for (const char *f : { ":/icons/16.png", ":/icons/32.png", ":/icons/48.png", ":/icons/7zfm-qt.svg" })
      icon.addFile(QString::fromLatin1(f));
    setWindowIcon(icon);
    QApplication::setWindowIcon(icon);
  }
  _splitter = new QSplitter(Qt::Horizontal, this);
  _splitter->setChildrenCollapsible(false);
  for (unsigned i = 0; i < 2; i++)
  {
    _panels[i] = new Panel(_splitter);
    _splitter->addWidget(_panels[i]);
  }
  setCentralWidget(_splitter);
  _statusLabel = new QLabel;
  statusBar()->addWidget(_statusLabel, 1);

  QSettings s;
  // upstream CWindowInfo numPanels / currentPanel (default: one panel)
  _numPanels = s.value(QStringLiteral("MainWindow/NumPanels"), 1).toUInt() == 2 ? 2 : 1;
  _lastFocused = s.value(QStringLiteral("MainWindow/CurrentPanel"), 0).toUInt() == 1 ? 1 : 0;

  CreateMenus();
  CreateToolBar();

  for (unsigned i = 0; i < 2; i++)
  {
    Panel *panel = _panels[i];
    // upstream CPanelCallbackImp
    connect(panel, &Panel::StatusChanged, this, [this, i]() {
      if (i == _lastFocused)
        UpdateStatus();
    });
    connect(panel, &Panel::FolderChanged, this, [this, i](const QString &path) {
      _panels[1 - i]->AddHistoryItem(path);
      if (i == _lastFocused)
        UpdateTitle(path);
    });
    connect(panel, &Panel::TabPressed, this, [this, i]() {
      if (_numPanels != 1)
        _panels[1 - i]->FocusList();
    });
    connect(panel, &Panel::SetSameFolderRequested, this, [this, i]() {
      // upstream CApp::OnSetSameFolder
      if (_numPanels <= 1)
        return;
      _panels[1 - i]->BindToPathAndRefresh(_panels[i]->CurrentFolderPrefix());
    });
    connect(panel, &Panel::SetSubFolderRequested, this, [this, i]() {
      if (_numPanels <= 1)
        return;
      _panels[i]->OpenSubFolderIn(*_panels[1 - i]);
    });
  }
  // upstream PanelWasFocused
  connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
    for (unsigned i = 0; i < 2; i++)
      if (now && _panels[i]->isAncestorOf(now))
        SetFocusedPanel(i);
  });

  if (!restoreGeometry(s.value(QStringLiteral("MainWindow/Geometry")).toByteArray()))
    resize(1000, 650);
  if (!_splitter->restoreState(s.value(QStringLiteral("MainWindow/Splitter")).toByteArray()))
    _splitter->setSizes({ 1, 1 });
  {
    UStringVector folders;
    ReadFolderHistory(folders);
    QStringList l;
    FOR_VECTOR (i, folders)
      l << ToQ(folders[i]);
    CListMode listMode;
    listMode.Read();
    for (unsigned i = 0; i < 2; i++)
    {
      _panels[i]->SetHistory(l);
      _panels[i]->SetViewMode(listMode.Panels[i] <= 3 ? (int)listMode.Panels[i] : 3);
    }
  }
  _panels[1 - _lastFocused]->setVisible(_numPanels > 1);
  _twoPanelsAction->setChecked(_numPanels > 1);
  _viewActions[GetPanel()->ViewMode()]->setChecked(true);
}

void MainWindow::BindPanelToSavedPath(unsigned index)
{
  // upstream CApp::CreateOnePanel with an empty path
  UString path;
  if (!ReadPanelPath(index, path) || path.IsEmpty())
    path = ToU(QDir::homePath() + QLatin1Char('/'));
  _panelBound[index] = true;
  _panels[index]->BindToStartPath(path);
}

void MainWindow::OpenStartPaths(const QString &start)
{
  // binding a panel can move the keyboard focus (and so _lastFocused) for
  // a moment, e.g. while its list is rebuilt
  const unsigned focused = _lastFocused;
  for (unsigned i = 0; i < 2; i++)
  {
    if (i != focused && _numPanels == 1)
      continue;  // bound when it is shown (F9)
    if (i == focused && !start.isEmpty())
    {
      _panelBound[i] = true;
      _panels[i]->BindToStartPath(ToU(start));
    }
    else
      BindPanelToSavedPath(i);
  }
  _panels[focused]->FocusList();
  SetFocusedPanel(focused);
}

void MainWindow::SwitchOnOffOnePanel()
{
  // upstream CApp::SwitchOnOffOnePanel
  const unsigned focused = _lastFocused;
  const unsigned other = 1 - focused;
  if (_numPanels == 1)
  {
    _numPanels++;
    _panels[other]->show();
    if (!_panelBound[other])
      BindPanelToSavedPath(other);
  }
  else
  {
    _numPanels--;
    _panels[other]->hide();
  }
  _twoPanelsAction->setChecked(_numPanels > 1);
  _panels[focused]->FocusList();
  SetFocusedPanel(focused);
}

void MainWindow::SetFocusedPanel(unsigned index)
{
  if (index == _lastFocused)
    return;
  _lastFocused = index;
  _viewActions[GetPanel()->ViewMode()]->setChecked(true);
  _flatAction->setChecked(GetPanel()->FlatMode());
  UpdateStatus();
  UpdateTitle(ToQ(GetPanel()->CurrentFolderPrefix()));
}

void MainWindow::OnCopy(bool move, bool copyToSame)
{
  // upstream CApp::OnCopy: destination is the other panel when there are two
  Panel *src = GetPanel();
  Panel *dest = (_numPanels <= 1) ? src : _panels[1 - _lastFocused];
  src->CopyToPanel(*dest, move, copyToSame);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
  QSettings s;
  s.setValue(QStringLiteral("MainWindow/Geometry"), saveGeometry());
  s.setValue(QStringLiteral("MainWindow/Splitter"), _splitter->saveState());
  s.setValue(QStringLiteral("MainWindow/NumPanels"), _numPanels);
  s.setValue(QStringLiteral("MainWindow/CurrentPanel"), _lastFocused);
  s.remove(QStringLiteral("MainWindow/ViewMode"));  // before per-panel ListMode
  CListMode listMode;
  for (unsigned i = 0; i < 2; i++)
  {
    listMode.Panels[i] = (UInt32)_panels[i]->ViewMode();
    _panels[i]->SaveListViewInfo();
    if (_panelBound[i])
      SavePanelPath(i, _panels[i]->CurrentFolderPrefix());
  }
  listMode.Save();
  {
    UStringVector folders;
    for (const QString &p : GetPanel()->History())
      folders.Add(ToU(p));
    SaveFolderHistory(folders);
  }
  QMainWindow::closeEvent(e);
}

void MainWindow::UpdateStatus()
{
  _statusLabel->setText(GetPanel()->StatusText());
}

void MainWindow::UpdateTitle(const QString &path)
{
  setWindowTitle(path.isEmpty() ? QStringLiteral("7-Zip") : path);
}

void MainWindow::OpenInsideAction(const wchar_t *type)
{
  GetPanel()->OpenFocusedItemAsInternal(type);
}

void MainWindow::CreateMenus()
{
  // ---- File (upstream IDM_FILE) ----
  QMenu *file = menuBar()->addMenu(M(IDM_FILE, "&File"));
  QAction *a;
  a = file->addAction(M(IDM_OPEN, "&Open"), this, [this]() { GetPanel()->OpenSelectedItems(true); });
  a->setShortcutContext(Qt::WidgetShortcut);
  a = file->addAction(M(IDM_OPEN_INSIDE, "Open &Inside"), this, [this]() { OpenInsideAction(NULL); });
  a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_PageDown));
  a->setShortcutContext(Qt::WidgetShortcut); // handled by the panel's key filter
  file->addAction(M(IDM_OPEN_INSIDE_ONE, "Open Inside *"), this, [this]() { OpenInsideAction(L"*"); });
  file->addAction(M(IDM_OPEN_INSIDE_PARSER, "Open Inside #"), this, [this]() { OpenInsideAction(L"#"); });
  a = file->addAction(M(IDM_OPEN_OUTSIDE, "Open O&utside"), this, [this]() { GetPanel()->OpenFocusedItemOutside(); });
  a->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Return));
  a->setShortcutContext(Qt::WidgetShortcut);
  a = file->addAction(M(IDM_FILE_VIEW, "&View"), this, [this]() { GetPanel()->EditItem(false); });
  a->setShortcut(QKeySequence(Qt::Key_F3));
  a = file->addAction(M(IDM_FILE_EDIT, "&Edit"), this, [this]() { GetPanel()->EditItem(true); });
  a->setShortcut(QKeySequence(Qt::Key_F4));
  file->addSeparator();
  a = file->addAction(M(IDM_RENAME, "Rena&me"), this, [this]() { GetPanel()->RenameFile(); });
  a->setShortcut(QKeySequence(Qt::Key_F2));
  // Shift+F5 / Shift+F6: copy to the same folder under a new name (upstream PanelKey.cpp)
  a = file->addAction(M(IDM_COPY_TO, "&Copy To..."), this, [this]() {
    OnCopy(false, QApplication::keyboardModifiers() & Qt::ShiftModifier);
  });
  a->setShortcuts({ QKeySequence(Qt::Key_F5), QKeySequence(Qt::SHIFT | Qt::Key_F5) });
  a = file->addAction(M(IDM_MOVE_TO, "&Move To..."), this, [this]() {
    OnCopy(true, QApplication::keyboardModifiers() & Qt::ShiftModifier);
  });
  a->setShortcuts({ QKeySequence(Qt::Key_F6), QKeySequence(Qt::SHIFT | Qt::Key_F6) });
  a = file->addAction(M(IDM_DELETE, "&Delete"), this, [this]() {
    GetPanel()->DeleteItems(!(QApplication::keyboardModifiers() & Qt::ShiftModifier));
  });
  a->setShortcuts({ QKeySequence(Qt::Key_Delete), QKeySequence(Qt::SHIFT | Qt::Key_Delete) });
  file->addSeparator();
  file->addAction(M(IDM_LINK, "&Link..."), this, [this]() { GetPanel()->Link(); });
  file->addAction(M(IDM_SPLIT, "&Split file..."), this, [this]() { GetPanel()->Split(); });
  file->addAction(M(IDM_COMBINE, "Com&bine files..."), this, [this]() { GetPanel()->Combine(); });
  file->addSeparator();
  a = file->addAction(M(IDM_COMMENT, "Comme&nt..."), this, [this]() { GetPanel()->ChangeComment(); });
  a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z));
  {
    QMenu *crc = file->addMenu(QStringLiteral("CRC"));
    static const char *const kMethods[] = { "CRC32", "CRC64", "XXH64", "MD5", "SHA1", "SHA256",
        "SHA384", "SHA512", "SHA3-256", "BLAKE2sp", "*" };
    static const char *const kNames[] = { "CRC-32", "CRC-64", "XXH64", "MD5", "SHA-1", "SHA-256",
        "SHA-384", "SHA-512", "SHA3-256", "BLAKE2sp", "*" };
    for (unsigned i = 0; i < sizeof(kMethods) / sizeof(kMethods[0]); i++)
    {
      const char *m = kMethods[i];
      crc->addAction(QString::fromLatin1(kNames[i]), this, [this, m]() { GetPanel()->CalculateCrc(m); });
    }
  }
  file->addSeparator();
  a = file->addAction(M(IDM_CREATE_FOLDER, "Create Folder"), this, [this]() { GetPanel()->CreateFolder(); });
  a->setShortcut(QKeySequence(Qt::Key_F7));
  a = file->addAction(M(IDM_CREATE_FILE, "Create File"), this, [this]() { GetPanel()->CreateFile(); });
  a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_N));
  file->addSeparator();
  a = file->addAction(M(IDM_PROPERTIES, "P&roperties"), this, [this]() { GetPanel()->Properties(); });
  a->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
  file->addSeparator();
  a = file->addAction(tr("E&xit"), this, &QWidget::close);
  a->setShortcut(QKeySequence(Qt::ALT | Qt::Key_F4));

  // ---- Edit ----
  QMenu *edit = menuBar()->addMenu(M(IDM_EDIT, "&Edit"));
  a = edit->addAction(M(IDM_SELECT_ALL, "Select &All"), this, [this]() { GetPanel()->SelectAll(true); });
  a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_A));
  edit->addAction(M(IDM_DESELECT_ALL, "Deselect All"), this, [this]() { GetPanel()->SelectAll(false); });
  edit->addAction(M(IDM_INVERT_SELECTION, "&Invert Selection"), this, [this]() { GetPanel()->InvertSelection(); });
  edit->addAction(M(IDM_SELECT, "Select..."), this, [this]() { GetPanel()->SelectByMask(true); });
  edit->addAction(M(IDM_DESELECT, "Deselect..."), this, [this]() { GetPanel()->SelectByMask(false); });

  // ---- View ----
  QMenu *view = menuBar()->addMenu(M(IDM_VIEW, "&View"));
  auto *group = new QActionGroup(this);
  const UInt32 ids[4] = { IDM_VIEW_LARGE_ICONS, IDM_VIEW_SMALL_ICONS, IDM_VIEW_LIST, IDM_VIEW_DETAILS };
  const char *fall[4] = { "Lar&ge Icons", "S&mall Icons", "&List", "&Details" };
  for (int i = 0; i < 4; i++)
  {
    _viewActions[i] = view->addAction(M(ids[i], fall[i]), this, [this, i]() { GetPanel()->SetViewMode(i); });
    _viewActions[i]->setCheckable(true);
    _viewActions[i]->setShortcut(QKeySequence(Qt::CTRL | (Qt::Key_1 + i)));
    group->addAction(_viewActions[i]);
  }
  view->addSeparator();
  _flatAction = view->addAction(M(IDM_VIEW_FLAT_VIEW, "&Flat View"), this, [this](bool on) { GetPanel()->SetFlatMode(on); });
  _flatAction->setCheckable(true);
  _twoPanelsAction = view->addAction(M(IDM_VIEW_TWO_PANELS, "&2 Panels"), this, [this]() { SwitchOnOffOnePanel(); });
  _twoPanelsAction->setCheckable(true);
  _twoPanelsAction->setShortcut(QKeySequence(Qt::Key_F9));
  view->addSeparator();
  a = view->addAction(M(IDM_OPEN_ROOT_FOLDER, "Open Root Folder"), this, [this]() { GetPanel()->OpenRootFolder(); });
  a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Backslash));
  a = view->addAction(M(IDM_OPEN_PARENT_FOLDER, "Up One Level"), this, [this]() { GetPanel()->OpenParentFolder(); });
  a->setShortcutContext(Qt::WidgetShortcut); // Backspace handled by the panel
  view->addSeparator();
  a = view->addAction(M(IDM_VIEW_REFRESH, "R&efresh"), this, [this]() { GetPanel()->RefreshListCtrl_SaveFocused(); });
  a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
  a = view->addAction(tr("Go to path"), this, [this]() { GetPanel()->FocusAddressBar(); });
  a->setShortcuts({ QKeySequence(Qt::CTRL | Qt::Key_L), QKeySequence(Qt::ALT | Qt::Key_D) });

  // ---- Favorites (upstream IDM_FAVORITES, AppState FastFolders) ----
  _favoritesMenu = menuBar()->addMenu(M(IDM_FAVORITES, "F&avorites"));
  RebuildFavoritesMenu();

  // ---- Tools ----
  QMenu *tools = menuBar()->addMenu(M(IDM_TOOLS, "&Tools"));
  tools->addAction(tr("&Add to archive..."), this, [this]() { GetPanel()->AddToArchive(); });
  tools->addAction(tr("E&xtract..."), this, [this]() { GetPanel()->ExtractArchives(); });
  tools->addAction(tr("&Test"), this, [this]() { GetPanel()->TestArchives(); });
  tools->addSeparator();
  tools->addAction(M(IDM_OPTIONS, "&Options..."), this, [this]() {
    if (OptionsDialog())
      for (Panel *p : _panels)
        p->ApplySettings();
  });
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
  tb->addAction(ThemeIcon("go-up"), M(IDM_OPEN_PARENT_FOLDER, "Up"), this, [this]() { GetPanel()->OpenParentFolder(); });
  tb->addAction(ThemeIcon("computer"), M(IDM_OPEN_ROOT_FOLDER, "Root"), this, [this]() { GetPanel()->OpenRootFolder(); });
  tb->addSeparator();
  tb->addAction(ThemeIcon("archive-insert"), tr("Add"), this, [this]() { GetPanel()->AddToArchive(); });
  tb->addAction(ThemeIcon("archive-extract"), tr("Extract"), this, [this]() { GetPanel()->ExtractArchives(); });
  tb->addAction(ThemeIcon("dialog-ok-apply"), tr("Test"), this, [this]() { GetPanel()->TestArchives(); });
  tb->addAction(ThemeIcon("edit-copy"), tr("Copy"), this, [this]() { OnCopy(false, false); });
  tb->addAction(ThemeIcon("go-jump"), tr("Move"), this, [this]() { OnCopy(true, false); });
  tb->addAction(ThemeIcon("edit-delete"), tr("Delete"), this, [this]() { GetPanel()->DeleteItems(true); });
  tb->addSeparator();
  tb->addAction(ThemeIcon("document-properties"), tr("Info"), this, [this]() { GetPanel()->Properties(); });
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
      f[(unsigned)i] = GetPanel()->CurrentFolderPrefix();
      SaveFastFolders(f);
      RebuildFavoritesMenu();
    });
    set->setShortcut(QKeySequence(Qt::ALT | Qt::SHIFT | (Qt::Key_0 + i)));
    QAction *go = _favoritesMenu->addAction(QStringLiteral("%1  %2").arg(i).arg(path.isEmpty() ? QStringLiteral("-") : path),
        this, [this, path]() {
      if (!path.isEmpty())
      {
        GetPanel()->BindToPathAndRefresh(ToU(path));
        GetPanel()->FocusList();
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
