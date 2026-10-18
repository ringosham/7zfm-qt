// ProgressDialog.cpp

#include "ProgressDialog.h"

#include "Common/Common.h"
#include "Common/IntToString.h"

#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/ProgressDialog2Res.h"

#include "ListFormat.h"
#include "ProgressSync.h"
#include "QtConv.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

static QString L(UInt32 id, const char *fallback)
{
  const UString s = LangString(id);
  QString q = s.IsEmpty() ? QString::fromLatin1(fallback) : ToQ(s);
  q.remove(QLatin1Char('&'));
  if (q.endsWith(QLatin1Char(':')))
    q.chop(1);
  return q;
}

static QString SizeStr(UInt64 v)
{
  return ToQ(ConvertSizeToString(v));
}

static QString SizeStrMB(UInt64 v)
{
  // like upstream: "123 MB" for big values, bytes otherwise
  if (v < ((UInt64)1 << 20) * 10)
    return SizeStr(v) + QLatin1String(" B");
  return SizeStr(v >> 20) + QLatin1String(" MB");
}

static QString TimeStr(qint64 sec)
{
  const qint64 h = sec / 3600;
  const qint64 m = (sec / 60) % 60;
  const qint64 s = sec % 60;
  return QString::asprintf("%02lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
}

ProgressDialog::ProgressDialog(CProgressSync &sync, const QString &title, QWidget *parent):
    QDialog(parent),
    _sync(sync),
    _title(title)
{
  setWindowTitle(title);
  setModal(true);
  auto *grid = new QGridLayout;
  auto addPair = [&](int row, int col, const QString &name) {
    auto *n = new QLabel(name + QLatin1Char(':'));
    auto *v = new QLabel;
    v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    v->setMinimumWidth(110);
    grid->addWidget(n, row, col * 2);
    grid->addWidget(v, row, col * 2 + 1);
    return v;
  };
  _elapsedLabel = addPair(0, 0, L(IDT_PROGRESS_ELAPSED, "Elapsed time"));
  _remainingLabel = addPair(1, 0, L(IDT_PROGRESS_REMAINING, "Remaining time"));
  _filesLabel = addPair(2, 0, L(IDT_PROGRESS_FILES, "Files"));
  _ratioLabel = addPair(3, 0, L(IDT_PROGRESS_RATIO, "Compression ratio"));
  _totalLabel = addPair(0, 1, L(IDT_PROGRESS_TOTAL, "Total size"));
  _speedLabel = addPair(1, 1, L(IDT_PROGRESS_SPEED, "Speed"));
  _processedLabel = addPair(2, 1, L(IDT_PROGRESS_PROCESSED, "Processed"));
  grid->setColumnMinimumWidth(2, 24);

  _statusLabel = new QLabel;
  _pathLabel = new QLabel;
  _pathLabel->setTextFormat(Qt::PlainText);
  _pathLabel->setMinimumWidth(420);
  _pathLabel->setWordWrap(true);
  _bar = new QProgressBar;
  _bar->setRange(0, 1000);
  _bar->setTextVisible(true);

  auto *buttons = new QDialogButtonBox;
  _pauseButton = buttons->addButton(L(IDB_PAUSE, "Pause"), QDialogButtonBox::ActionRole);
  _cancelButton = buttons->addButton(QDialogButtonBox::Cancel);
  connect(_pauseButton, &QPushButton::clicked, this, &ProgressDialog::OnPause);
  connect(_cancelButton, &QPushButton::clicked, this, &ProgressDialog::reject);

  auto *lay = new QVBoxLayout(this);
  lay->addLayout(grid);
  lay->addWidget(_statusLabel);
  lay->addWidget(_pathLabel);
  lay->addWidget(_bar);
  lay->addWidget(buttons);

  _elapsed.start();
  _timer = new QTimer(this);
  connect(_timer, &QTimer::timeout, this, &ProgressDialog::UpdateUi);
  _timer->start(100);
  UpdateUi();
}

void ProgressDialog::reject()
{
  // Like upstream: Cancel / Esc / closing the window stops the operation;
  // the dialog goes away when the worker returns.
  _sync.Set_Paused(false);
  _sync.Set_Stopped(true);
  _cancelButton->setEnabled(false);
}

void ProgressDialog::OnPause()
{
  const bool paused = !_sync.Get_Paused();
  _sync.Set_Paused(paused);
  if (paused)
    _pauseStart = _elapsed.elapsed();
  else if (_pauseStart >= 0)
  {
    _pausedMs += _elapsed.elapsed() - _pauseStart;
    _pauseStart = -1;
  }
  _pauseButton->setText(paused ? L(IDS_CONTINUE, "Continue") : L(IDB_PAUSE, "Pause"));
}

void ProgressDialog::UpdateUi()
{
  UInt64 total, completed, totalFiles, curFiles, inSize, outSize;
  bool filesMode;
  UString status, path, titleFile;
  {
    NWindows::NSynchronization::CCriticalSectionLock lock(_sync._cs);
    total = _sync._totalBytes;
    completed = _sync._completedBytes;
    totalFiles = _sync._totalFiles;
    curFiles = _sync._curFiles;
    inSize = _sync._inSize;
    outSize = _sync._outSize;
    filesMode = _sync._filesProgressMode;
    status = _sync._status;
    path = _sync._filePath;
    titleFile = _sync._titleFileName;
  }

  qint64 ms = _elapsed.elapsed() - _pausedMs;
  if (_pauseStart >= 0)
    ms -= _elapsed.elapsed() - _pauseStart;
  _elapsedLabel->setText(TimeStr(ms / 1000));

  UInt64 progressTotal = filesMode ? totalFiles : total;
  UInt64 progressCur = filesMode ? curFiles : completed;
  if (progressTotal != PROGRESS_UNDEFINED_VAL && progressTotal != 0)
  {
    const double f = (double)progressCur / (double)progressTotal;
    _bar->setRange(0, 1000);
    _bar->setValue((int)(qBound(0.0, f, 1.0) * 1000));
    if (ms > 1000 && progressCur > 0)
    {
      const double remain = (double)ms * ((double)progressTotal / (double)progressCur - 1) / 1000;
      _remainingLabel->setText(TimeStr((qint64)qMax(0.0, remain)));
    }
  }
  else
  {
    _bar->setRange(0, 0); // busy indicator
    _remainingLabel->clear();
  }

  QString filesText = SizeStr(curFiles);
  if (totalFiles != PROGRESS_UNDEFINED_VAL)
    filesText += QLatin1String(" / ") + SizeStr(totalFiles);
  _filesLabel->setText(filesText);
  _totalLabel->setText(total != PROGRESS_UNDEFINED_VAL ? SizeStrMB(total) : QString());
  _processedLabel->setText(SizeStrMB(completed));
  if (ms > 0)
    _speedLabel->setText(SizeStr((UInt64)((double)completed * 1000 / (double)ms / (1 << 20))) + QLatin1String(" MB/s"));
  if (inSize != PROGRESS_UNDEFINED_VAL && outSize != PROGRESS_UNDEFINED_VAL && inSize != 0)
    _ratioLabel->setText(QString::number((int)((double)outSize * 100 / (double)inSize)) + QLatin1Char('%'));
  _statusLabel->setText(ToQ(status));
  _pathLabel->setText(ToQ(path));

  QString t;
  if (progressTotal != PROGRESS_UNDEFINED_VAL && progressTotal != 0 && _bar->maximum() > 0)
    t = QString::number(_bar->value() / 10) + QLatin1String("% ");
  if (_sync.Get_Paused())
    t += L(IDS_PROGRESS_PAUSED, "Paused") + QLatin1Char(' ');
  t += _title;
  if (!titleFile.IsEmpty())
    t += QLatin1Char(' ') + ToQ(titleFile);
  setWindowTitle(t);
}

namespace {

// Swallows user input aimed at anything but the progress dialog (and the
// prompts it raises), so nothing touches the folder objects meanwhile.
class CInputBlocker : public QObject
{
  QWidget *_allowed;
public:
  explicit CInputBlocker(QWidget *allowed): _allowed(allowed) { qApp->installEventFilter(this); }
  ~CInputBlocker() override { qApp->removeEventFilter(this); }
  bool eventFilter(QObject *obj, QEvent *ev) override
  {
    switch (ev->type())
    {
      case QEvent::MouseButtonPress:
      case QEvent::MouseButtonRelease:
      case QEvent::MouseButtonDblClick:
      case QEvent::KeyPress:
      case QEvent::KeyRelease:
      case QEvent::Wheel:
      case QEvent::Shortcut:
      case QEvent::ShortcutOverride:
      case QEvent::Drop:
      {
        QWidget *w = qobject_cast<QWidget *>(obj);
        if (!w)
          return false;
        QWidget *top = w->window();
        if (top == _allowed->window() || (top->isModal() && top != _allowed->parentWidget()))
          return false;
        return true;
      }
      default:
        return false;
    }
  }
};

}

void ProgressDialog::Run(QWidget *parent, CProgressSync &sync, const QString &title,
    const std::function<void()> &work, bool waitMode)
{
  ProgressDialog dlg(sync, title, parent ? parent->window() : nullptr);
  {
    CInputBlocker blocker(&dlg);
    QApplication::setOverrideCursor(Qt::BusyCursor);
    QThread *thread = QThread::create(work);
    QEventLoop loop;
    QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
    QTimer showTimer;
    showTimer.setSingleShot(true);
    QObject::connect(&showTimer, &QTimer::timeout, &dlg, [&]() {
      if (!thread->isFinished())
      {
        QApplication::restoreOverrideCursor();
        dlg.show();
      }
    });
    thread->start();
    showTimer.start(waitMode ? 500 : 0);
    loop.exec();
    thread->wait();
    delete thread;
    if (!dlg.isVisible())
      QApplication::restoreOverrideCursor();
    dlg.hide();
  }

  QStringList messages;
  {
    NWindows::NSynchronization::CCriticalSectionLock lock(sync._cs);
    FOR_VECTOR (i, sync.Messages)
      messages << ToQ(sync.Messages[i]);
    if (!sync.FinalMessage.ErrorMessage.Message.IsEmpty())
      messages << ToQ(sync.FinalMessage.ErrorMessage.Message);
  }
  if (!messages.isEmpty())
    ShowMessagesDialog(parent, title, messages);
}

void ShowMessagesDialog(QWidget *parent, const QString &title, const QStringList &messages)
{
  QDialog d(parent ? parent->window() : nullptr);
  d.setWindowTitle(title);
  auto *text = new QPlainTextEdit;
  text->setReadOnly(true);
  text->setPlainText(messages.join(QLatin1String("\n\n")));
  text->setMinimumSize(520, 220);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  auto *lay = new QVBoxLayout(&d);
  lay->addWidget(text);
  lay->addWidget(buttons);
  d.exec();
}
