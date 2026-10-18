// MainWindow.h -- 7zFM's main window (upstream App.cpp, MyLoadMenu.cpp).

#ifndef Z7QT_MAIN_WINDOW_H
#define Z7QT_MAIN_WINDOW_H

#include <QMainWindow>

class QAction;
class QLabel;
class Panel;

class MainWindow : public QMainWindow
{
  Q_OBJECT
public:
  MainWindow();
  Panel *GetPanel() const { return _panel; }

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

  Panel *_panel;
  QLabel *_statusLabel;
  QAction *_viewActions[4];
  QMenu *_favoritesMenu;
};

#endif
