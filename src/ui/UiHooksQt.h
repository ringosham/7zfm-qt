// UiHooksQt.h -- IUiHooks implemented with Qt widgets.

#ifndef Z7QT_UI_HOOKS_QT_H
#define Z7QT_UI_HOOKS_QT_H

#include "UiHooks.h"

#include <QPointer>
#include <QWidget>

#include <functional>

class UiHooksQt : public IUiHooks
{
  QPointer<QWidget> _parent;
public:
  explicit UiHooksQt(QWidget *parent): _parent(parent) {}

  void RunWithProgress(CProgressSync &sync, const UString &title,
      const std::function<void()> &work, bool waitMode) override;
  bool AskPassword(UString &password) override;
  void ShowError(const UString &message) override;

  QWidget *DialogParent() const;
};

// Runs (f) on the GUI thread and waits for it (direct call if already there).
void RunOnGuiThread(const std::function<void()> &f);

#endif
