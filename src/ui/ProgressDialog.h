// ProgressDialog.h -- Qt version of 7zFM's progress window
// (upstream FileManager/ProgressDialog2.cpp). Worker threads report through
// CProgressSync; this dialog polls it on a timer.

#ifndef Z7QT_PROGRESS_DIALOG_H
#define Z7QT_PROGRESS_DIALOG_H

#include <QDialog>
#include <QElapsedTimer>

#include <functional>

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
class CProgressSync;

class ProgressDialog : public QDialog
{
  Q_OBJECT
public:
  ProgressDialog(CProgressSync &sync, const QString &title, QWidget *parent);

  // Runs (work) on a worker thread. User input to other windows is blocked
  // while it runs; the dialog appears at once, or after a short delay in
  // waitMode (so quick opens don't flash a window), like upstream.
  // Errors collected in sync.Messages are shown afterwards.
  static void Run(QWidget *parent, CProgressSync &sync, const QString &title,
      const std::function<void()> &work, bool waitMode);

  void reject() override;

private slots:
  void UpdateUi();
  void OnPause();

private:
  CProgressSync &_sync;
  QString _title;
  QElapsedTimer _elapsed;
  qint64 _pausedMs = 0;
  qint64 _pauseStart = -1;
  QTimer *_timer;
  QLabel *_elapsedLabel;
  QLabel *_remainingLabel;
  QLabel *_filesLabel;
  QLabel *_speedLabel;
  QLabel *_totalLabel;
  QLabel *_processedLabel;
  QLabel *_ratioLabel;
  QLabel *_statusLabel;
  QLabel *_pathLabel;
  QProgressBar *_bar;
  QPushButton *_pauseButton;
  QPushButton *_cancelButton;
};

// Shows the list of messages a long operation produced
// (upstream MessagesDialog).
void ShowMessagesDialog(QWidget *parent, const QString &title, const QStringList &messages);

#endif
