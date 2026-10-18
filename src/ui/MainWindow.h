// MainWindow.h -- 7zFM's main window (upstream App.cpp, MyLoadMenu.cpp).

#ifndef Z7QT_MAIN_WINDOW_H
#define Z7QT_MAIN_WINDOW_H

#include <QMainWindow>

class QAction;
class QLabel;
class QSplitter;
class Panel;

class MainWindow : public QMainWindow
{
  Q_OBJECT
public:
  MainWindow();
  // the focused panel (upstream CApp::GetFocusedPanel); menus act on it
  Panel *GetPanel() const { return _panels[_lastFocused]; }
  Panel *GetPanel(unsigned i) const { return _panels[i]; }
  unsigned NumPanels() const { return _numPanels; }
  // start path for the focused panel; the other one opens its saved folder
  void OpenStartPaths(const QString &start);
  void SwitchOnOffOnePanel();             // F9
  void OnCopy(bool move, bool copyToSame); // F5 / F6

protected:
  void closeEvent(QCloseEvent *e) override;

private:
  void CreateMenus();
  void CreateToolBar();
  void UpdateStatus();
  void UpdateTitle(const QString &path);
  void OpenInsideAction(const wchar_t *type);
  void About();
  void RebuildFavoritesMenu();
  void SetFocusedPanel(unsigned index);
  void BindPanelToSavedPath(unsigned index);

  // upstream CApp: two panels, one of them hidden with NumPanels == 1
  Panel *_panels[2];
  bool _panelBound[2] = { false, false };
  unsigned _numPanels = 1;
  unsigned _lastFocused = 0;
  QSplitter *_splitter;
  QAction *_flatAction;
  QAction *_twoPanelsAction;
  QLabel *_statusLabel;
  QAction *_viewActions[4];
  QMenu *_favoritesMenu;
};

#endif
