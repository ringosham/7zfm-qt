// Q32.cpp -- user32 on Qt; see Q32.h.

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QFont>
#include <QFontMetrics>
#include <QGroupBox>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QSlider>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTabWidget>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QWindow>

#include <map>
#include <unistd.h>

#include "Q32.h"

HINSTANCE g_hInstance = NULL;  // upstream Dialog.cpp refers to it
bool g_IsNT = true;            // upstream uses the wide (W) API paths

namespace {
#include "DialogTemplates.inc"
}

const CDlgTemplate *Q32_FindDialogTemplate(int id)
{
  for (const CDlgTemplate &t : k_DialogTemplates)
    if (t.Id == id)
      return &t;
  return nullptr;
}

// ---------- helpers ----------

static inline QWidget *W(HWND h) { return static_cast<QWidget *>(h); }
static inline HWND H(QWidget *w) { return static_cast<HWND>(w); }

static QString FromW(LPCWSTR s) { return s ? QString::fromWCharArray(s) : QString(); }
static QString FromA(LPCSTR s) { return s ? QString::fromUtf8(s) : QString(); }

// copies (s) into a wchar_t / char buffer of (maxCount) units; returns length
static int CopyOutW(const QString &s, LPWSTR dest, int maxCount)
{
  if (!dest || maxCount <= 0)
    return 0;
  std::wstring w = s.toStdWString();
  int n = (int)w.size();
  if (n > maxCount - 1)
    n = maxCount - 1;
  wmemcpy(dest, w.c_str(), (size_t)n);
  dest[n] = 0;
  return n;
}

static int CopyOutA(const QString &s, LPSTR dest, int maxCount)
{
  if (!dest || maxCount <= 0)
    return 0;
  QByteArray a = s.toUtf8();
  int n = (int)a.size();
  if (n > maxCount - 1)
    n = maxCount - 1;
  memcpy(dest, a.constData(), (size_t)n);
  dest[n] = 0;
  return n;
}

// Win32 '&' mnemonics: Qt uses the same character, "&&" for a literal '&'.
static QString MnemonicText(const QString &s, bool noPrefix)
{
  if (noPrefix)
    return QString(s).replace(QLatin1Char('&'), QLatin1String("&&"));
  return s;
}

class Q32Dialog;
static Q32Dialog *DialogOf(QWidget *w);

static const char *kId = "q32_id";
static const char *kStyle = "q32_style";
static const char *kUserData = "q32_userdata";
static const char *kItemData = "q32_itemdata";  // combo items: Qt::UserRole

// the "up-down" control next to an edit
class Q32Spin : public QWidget
{
public:
  QPointer<QLineEdit> Buddy;
  int Min = 0, Max = 100;
  bool SetBuddyInt = true;
  explicit Q32Spin(QWidget *parent): QWidget(parent)
  {
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto *up = new QToolButton;
    up->setArrowType(Qt::UpArrow);
    auto *down = new QToolButton;
    down->setArrowType(Qt::DownArrow);
    for (QToolButton *b : { up, down })
    {
      b->setAutoRepeat(true);
      b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
      lay->addWidget(b);
    }
    connect(up, &QToolButton::clicked, this, [this]() { Step(1); });
    connect(down, &QToolButton::clicked, this, [this]() { Step(-1); });
  }
  int Pos() const { return Buddy ? Buddy->text().toInt() : 0; }
  void SetPos(int v)
  {
    v = qBound(qMin(Min, Max), v, qMax(Min, Max));
    if (Buddy && SetBuddyInt)
      Buddy->setText(QString::number(v));
  }
  void Step(int d) { SetPos(Pos() + (Min <= Max ? d : -d)); }
};

class Q32Dialog : public QDialog
{
public:
  DLGPROC Proc = nullptr;
  LPARAM InitParam = 0;
  LONG_PTR UserData = 0;
  LONG_PTR MsgResult = 0;
  INT_PTR Result = 0;
  bool Ended = false;
  bool Initialized = false;
  bool Modeless = false;
  double BaseX = 6, BaseY = 13;
  std::map<int, QWidget *> Items;
  std::map<UINT_PTR, QTimer *> Timers;
  UInt32 Style = 0;

  explicit Q32Dialog(QWidget *parent): QDialog(parent) {}

  LRESULT Call(UINT msg, WPARAM w, LPARAM l)
  {
    if (!Proc)
      return 0;
    return (LRESULT)Proc(H(this), msg, w, l);
  }
  void Command(int id, int code, QWidget *ctrl)
  {
    if (Initialized)
      Call(WM_COMMAND, MAKEWPARAM(id, code), (LPARAM)H(ctrl));
  }
  int PxX(int du) const { return (int)(du * BaseX / 4 + 0.5); }
  int PxY(int du) const { return (int)(du * BaseY / 8 + 0.5); }

  void reject() override { Command(IDCANCEL, BN_CLICKED, nullptr); }  // Esc
  void accept() override { Command(IDOK, BN_CLICKED, nullptr); }
  void closeEvent(QCloseEvent *e) override
  {
    e->ignore();
    Command(IDCANCEL, BN_CLICKED, nullptr);
  }
  void resizeEvent(QResizeEvent *e) override
  {
    QDialog::resizeEvent(e);
    if (Initialized)
      Call(WM_SIZE, 0, MAKELPARAM(width(), height()));
  }
};

static Q32Dialog *DialogOf(QWidget *w)
{
  for (; w; w = w->parentWidget())
    if (auto *d = dynamic_cast<Q32Dialog *>(w))
      return d;
  return nullptr;
}

static void ComputeBaseUnits(Q32Dialog *d)
{
  const QFontMetrics fm(d->font());
  static const QString kAlphabet = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
  d->BaseX = (double)fm.horizontalAdvance(kAlphabet) / 52.0;
  // Qt widgets are taller than classic Win32 ones: make a 14-unit row fit
  // a combo box.
  QComboBox probe;
  const int comboH = probe.sizeHint().height();
  d->BaseY = qMax((double)fm.height(), comboH * 8.0 / 13.0);
}

// ---------- building dialogs from templates ----------

static QWidget *CreateControl(Q32Dialog *d, const CDlgItemTemplate &t,
    QLineEdit *&lastEdit, QButtonGroup *&radioGroup, QLabel *&pendingLabel)
{
  const QString kind = QString::fromLatin1(t.Kind);
  const QString cls = QString::fromLatin1(t.Class).toLower();
  const QString text = FromW(t.Text);
  const UInt32 style = t.Style;
  QWidget *w = nullptr;
  bool focusable = true;

  auto makeLabel = [&](Qt::Alignment align) {
    auto *l = new QLabel(d);
    const bool noPrefix = (style & SS_NOPREFIX) != 0;
    l->setText(MnemonicText(text, noPrefix));
    l->setTextFormat(Qt::PlainText);
    l->setAlignment(align | Qt::AlignTop);
    l->setWordWrap((style & 0xF) != SS_LEFTNOWORDWRAP && t.CY > 10);
    if (!noPrefix && text.contains(QLatin1Char('&')))
      pendingLabel = l;
    focusable = false;
    return l;
  };
  auto makeButton = [&](bool def) {
    auto *b = new QPushButton(MnemonicText(text, false), d);
    b->setDefault(def);
    b->setAutoDefault(def);
    QObject::connect(b, &QPushButton::clicked, d, [d, b, id = t.Id]() { d->Command(id, BN_CLICKED, b); });
    return b;
  };
  auto makeCheck = [&]() {
    auto *c = new QCheckBox(MnemonicText(text, false), d);
    QObject::connect(c, &QCheckBox::clicked, d, [d, c, id = t.Id]() { d->Command(id, BN_CLICKED, c); });
    return c;
  };
  auto makeRadio = [&]() {
    auto *r = new QRadioButton(MnemonicText(text, false), d);
    r->setAutoExclusive(false);
    if ((style & WS_GROUP) || !radioGroup)
    {
      radioGroup = new QButtonGroup(d);
      radioGroup->setExclusive(true);
    }
    radioGroup->addButton(r);
    QObject::connect(r, &QRadioButton::clicked, d, [d, r, id = t.Id]() { d->Command(id, BN_CLICKED, r); });
    return r;
  };
  auto makeGroup = [&]() {
    auto *g = new QGroupBox(MnemonicText(text, false), d);
    g->setAttribute(Qt::WA_TransparentForMouseEvents);
    focusable = false;
    return g;
  };
  auto makeEdit = [&]() -> QWidget * {
    if (style & ES_MULTILINE)
    {
      auto *e = new QPlainTextEdit(d);
      e->setReadOnly(style & ES_READONLY);
      QObject::connect(e, &QPlainTextEdit::textChanged, d, [d, e, id = t.Id]() { d->Command(id, EN_CHANGE, e); });
      return e;
    }
    auto *e = new QLineEdit(d);
    if (style & ES_PASSWORD)
      e->setEchoMode(QLineEdit::Password);
    e->setReadOnly(style & ES_READONLY);
    if (style & ES_CENTER)
      e->setAlignment(Qt::AlignHCenter);
    else if (style & ES_RIGHT)
      e->setAlignment(Qt::AlignRight);
    if (style & ES_NUMBER)
      e->setValidator(new QIntValidator(0, INT_MAX, e));
    QObject::connect(e, &QLineEdit::textChanged, d, [d, e, id = t.Id]() { d->Command(id, EN_CHANGE, e); });
    lastEdit = e;
    return e;
  };
  auto makeCombo = [&]() {
    auto *c = new QComboBox(d);
    const bool editable = (style & 3) == CBS_DROPDOWN;
    c->setEditable(editable);
    if (editable)
      c->setInsertPolicy(QComboBox::NoInsert);
    c->setMaxVisibleItems(qMax(5, t.CY / 9));
    QObject::connect(c, &QComboBox::activated, d, [d, c, id = t.Id](int) { d->Command(id, CBN_SELCHANGE, c); });
    if (editable)
      QObject::connect(c->lineEdit(), &QLineEdit::textEdited, d, [d, c, id = t.Id]() { d->Command(id, CBN_EDITCHANGE, c); });
    return c;
  };

  if (kind == QLatin1String("LTEXT")) w = makeLabel(Qt::AlignLeft);
  else if (kind == QLatin1String("RTEXT")) w = makeLabel(Qt::AlignRight);
  else if (kind == QLatin1String("CTEXT")) w = makeLabel(Qt::AlignHCenter);
  else if (kind == QLatin1String("PUSHBUTTON")) w = makeButton(false);
  else if (kind == QLatin1String("DEFPUSHBUTTON")) w = makeButton(true);
  else if (kind == QLatin1String("GROUPBOX")) w = makeGroup();
  else if (kind == QLatin1String("EDITTEXT")) w = makeEdit();
  else if (kind == QLatin1String("COMBOBOX")) w = makeCombo();
  else if (kind == QLatin1String("LISTBOX"))
  {
    auto *l = new QListWidget(d);
    QObject::connect(l, &QListWidget::currentRowChanged, d, [d, l, id = t.Id](int) { d->Command(id, LBN_SELCHANGE, l); });
    w = l;
  }
  else if (kind == QLatin1String("CHECKBOX") || kind == QLatin1String("AUTOCHECKBOX")) w = makeCheck();
  else if (kind == QLatin1String("RADIOBUTTON") || kind == QLatin1String("AUTORADIOBUTTON")) w = makeRadio();
  else if (kind == QLatin1String("ICON")) { w = new QLabel(d); focusable = false; }
  else if (kind == QLatin1String("CONTROL"))
  {
    if (cls == QLatin1String("button"))
    {
      switch (style & 0xF)
      {
        case 2: case 3: case 5: case 6: w = makeCheck(); break;
        case 4: case 9: w = makeRadio(); break;
        case 7: w = makeGroup(); break;
        default: w = makeButton((style & 0xF) == BS_DEFPUSHBUTTON); break;
      }
    }
    else if (cls == QLatin1String("static"))
    {
      const UInt32 ss = style & 0xF;
      w = makeLabel(ss == SS_RIGHT ? Qt::AlignRight : ss == SS_CENTER ? Qt::AlignHCenter : Qt::AlignLeft);
    }
    else if (cls == QLatin1String("edit")) w = makeEdit();
    else if (cls == QLatin1String("combobox")) w = makeCombo();
    else if (cls == QLatin1String("msctls_updown32"))
    {
      auto *s = new Q32Spin(d);
      s->Buddy = lastEdit;
      focusable = false;
      w = s;
    }
    else if (cls == QLatin1String("msctls_progress32"))
    {
      auto *p = new QProgressBar(d);
      p->setTextVisible(false);
      focusable = false;
      w = p;
    }
    else if (cls == QLatin1String("msctls_trackbar32")) w = new QSlider(Qt::Horizontal, d);
    else if (cls == QLatin1String("syslistview32"))
    {
      auto *tv = new QTreeWidget(d);
      tv->setRootIsDecorated(false);
      tv->setUniformRowHeights(true);
      w = tv;
    }
    else
    {
      w = new QLabel(d);
      focusable = false;
    }
  }
  if (!w)
    return nullptr;

  w->setProperty(kId, t.Id);
  w->setProperty(kStyle, (uint)style);
  if (style & WS_DISABLED)
    w->setEnabled(false);
  if (focusable && pendingLabel)
  {
    pendingLabel->setBuddy(w);
    pendingLabel = nullptr;
  }

  // geometry: template rectangle in dialog units; single-line controls
  // keep their natural height, centred on the template's first row
  QRect r(d->PxX(t.X), d->PxY(t.Y), d->PxX(t.CX), d->PxY(t.CY));
  if (qobject_cast<QComboBox *>(w))
  {
    const int h = w->sizeHint().height();
    r.setHeight(h);
    r.moveTop(d->PxY(t.Y) + (d->PxY(12) - h) / 2);
  }
  else if (qobject_cast<QLineEdit *>(w) || qobject_cast<QPushButton *>(w)
      || qobject_cast<QCheckBox *>(w) || qobject_cast<QRadioButton *>(w))
  {
    const int h = qMax(r.height(), w->sizeHint().height());
    r.moveTop(r.top() + (r.height() - h) / 2);
    r.setHeight(h);
  }
  else if (auto *s = dynamic_cast<Q32Spin *>(w))
  {
    if (s->Buddy)
    {
      const QRect b = s->Buddy->geometry();
      const int sw = qMax(16, b.height() * 2 / 3);
      s->Buddy->setGeometry(b.adjusted(0, 0, -sw, 0));
      r = QRect(b.right() - sw + 1, b.top(), sw, b.height());
    }
  }
  w->setGeometry(r);
  if (qobject_cast<QGroupBox *>(w))
    w->lower();
  d->Items[t.Id] = w;
  return w;
}

static Q32Dialog *BuildDialog(const CDlgTemplate *t, QWidget *parent)
{
  auto *d = new Q32Dialog(parent ? parent->window() : nullptr);
  d->Style = t->Style;
  d->setWindowTitle(FromW(t->Caption));
  ComputeBaseUnits(d);
  QLineEdit *lastEdit = nullptr;
  QButtonGroup *radioGroup = nullptr;
  QLabel *pendingLabel = nullptr;
  for (unsigned i = 0; i < t->NumItems; i++)
    CreateControl(d, t->Items[i], lastEdit, radioGroup, pendingLabel);
  // the dialog must hold every control (Qt widgets may be taller)
  int right = d->PxX(t->CX), bottom = d->PxY(t->CY);
  for (QWidget *c : d->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
  {
    right = qMax(right, c->geometry().right() + 1);
    bottom = qMax(bottom, c->geometry().bottom() + 1);
  }
  d->resize(right, bottom);
  if (t->Style & WS_THICKFRAME)
    d->setMinimumSize(right / 2, bottom / 2);
  else
    d->setFixedSize(right, bottom);
  return d;
}

static void InitDialog(Q32Dialog *d, LPARAM param)
{
  d->InitParam = param;
  QWidget *first = nullptr;
  for (auto &it : d->Items)
    if (it.second->focusPolicy() != Qt::NoFocus && it.second->isEnabled() && !first)
      first = it.second;
  const LRESULT res = d->Call(WM_INITDIALOG, (WPARAM)H(first), param);
  d->Initialized = true;
  if (res && !d->Ended)
  {
    // focus the first focusable control in tab (creation) order
    for (QWidget *c : d->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
      if (c->focusPolicy() != Qt::NoFocus && c->isEnabled() && c->isVisibleTo(d))
      {
        c->setFocus();
        break;
      }
  }
}

INT_PTR DialogBoxParamW(HINSTANCE, LPCWSTR templateName, HWND parent, DLGPROC proc, LPARAM param)
{
  const CDlgTemplate *t = Q32_FindDialogTemplate((int)(ULONG_PTR)templateName);
  if (!t)
    return -1;
  QWidget *p = W(parent);
  if (!p)
    p = QApplication::activeModalWidget() ? QApplication::activeModalWidget() : QApplication::activeWindow();
  Q32Dialog *d = BuildDialog(t, p);
  d->Proc = proc;
  InitDialog(d, param);
  if (!d->Ended)
  {
    d->setModal(true);
    d->exec();
  }
  d->Call(WM_DESTROY, 0, 0);
  const INT_PTR result = d->Result;
  delete d;
  return result;
}

HWND CreateDialogParamW(HINSTANCE, LPCWSTR templateName, HWND parent, DLGPROC proc, LPARAM param)
{
  const CDlgTemplate *t = Q32_FindDialogTemplate((int)(ULONG_PTR)templateName);
  if (!t)
    return NULL;
  Q32Dialog *d = BuildDialog(t, W(parent));
  d->Modeless = true;
  d->Proc = proc;
  InitDialog(d, param);
  if (!d->Ended)
    d->show();
  return H(d);
}

BOOL EndDialog(HWND dialog, INT_PTR result)
{
  Q32Dialog *d = DialogOf(W(dialog));
  if (!d)
    return FALSE;
  d->Result = result;
  d->Ended = true;
  if (d->Modeless)
    d->hide();
  else
    d->done((int)result);
  return TRUE;
}

BOOL DestroyWindow(HWND hwnd)
{
  QWidget *w = W(hwnd);
  if (!w)
    return FALSE;
  if (auto *d = dynamic_cast<Q32Dialog *>(w))
  {
    d->Call(WM_DESTROY, 0, 0);
    d->Proc = nullptr;
    d->hide();
    d->deleteLater();
    return TRUE;
  }
  w->deleteLater();
  return TRUE;
}

// ---------- property sheets ----------

class Q32Sheet : public QDialog
{
public:
  QTabWidget *Tabs = nullptr;
  QPushButton *Apply = nullptr;
  std::vector<Q32Dialog *> Pages;
  std::vector<const PROPSHEETPAGEW *> PageInfo;
  std::vector<bool> Inited;

  explicit Q32Sheet(QWidget *parent): QDialog(parent) {}

  LRESULT Notify(Q32Dialog *page, UINT code)
  {
    PSHNOTIFY n;
    n.hdr.hwndFrom = H(this);
    n.hdr.idFrom = 0;
    n.hdr.code = code;
    n.lParam = 0;
    page->MsgResult = 0;
    page->Call(WM_NOTIFY, 0, (LPARAM)&n);
    return page->MsgResult;
  }

  void EnsureInited(int i)
  {
    if (i < 0 || Inited[(size_t)i])
      return;
    Inited[(size_t)i] = true;
    Q32Dialog *d = Pages[(size_t)i];
    // upstream MyProperyPageProcedure stores ((PROPSHEETPAGE *)lParam)->lParam
    // (the CPropertyPage*) through a 32-bit cast: see SetWindowLongPtrW
    d->InitParam = PageInfo[(size_t)i]->lParam;
    d->Call(WM_INITDIALOG, 0, (LPARAM)PageInfo[(size_t)i]);
    d->Initialized = true;
  }

  bool ApplyAll()
  {
    const int cur = Tabs->currentIndex();
    if (cur >= 0 && Inited[(size_t)cur] && Notify(Pages[(size_t)cur], PSN_KILLACTIVE) != 0)
      return false;
    for (size_t i = 0; i < Pages.size(); i++)
      if (Inited[i] && Notify(Pages[i], PSN_APPLY) != PSNRET_NOERROR)
      {
        Tabs->setCurrentIndex((int)i);
        return false;
      }
    Apply->setEnabled(false);
    return true;
  }
};

static Q32Sheet *SheetOf(QWidget *w)
{
  for (; w; w = w->parentWidget())
    if (auto *s = dynamic_cast<Q32Sheet *>(w))
      return s;
  return nullptr;
}

INT_PTR PropertySheetW(const PROPSHEETHEADERW *header)
{
  QWidget *parent = W(header->hwndParent);
  if (!parent)
    parent = QApplication::activeWindow();
  Q32Sheet sheet(parent ? parent->window() : nullptr);
  sheet.setWindowTitle(FromW(header->pszCaption));
  sheet.Tabs = new QTabWidget(&sheet);
  for (UINT i = 0; i < header->nPages; i++)
  {
    const PROPSHEETPAGEW &pi = header->ppsp[i];
    const CDlgTemplate *t = Q32_FindDialogTemplate((int)(ULONG_PTR)pi.pszTemplate);
    if (!t)
      continue;
    Q32Dialog *page = BuildDialog(t, nullptr);
    page->setWindowFlags(Qt::Widget);
    page->setMinimumSize(page->size());
    page->Proc = pi.pfnDlgProc;
    sheet.Pages.push_back(page);
    sheet.PageInfo.push_back(&pi);
    sheet.Inited.push_back(false);
    QString title = (pi.dwFlags & PSP_USETITLE) && pi.pszTitle ? FromW(pi.pszTitle) : FromW(t->Caption);
    sheet.Tabs->addTab(page, title.remove(QLatin1Char('&')));
  }
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
  sheet.Apply = buttons->button(QDialogButtonBox::Apply);
  sheet.Apply->setEnabled(false);
  INT_PTR result = 0;
  QObject::connect(buttons, &QDialogButtonBox::accepted, &sheet, [&]() {
    if (sheet.ApplyAll())
    {
      result = 1;
      sheet.done(1);
    }
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &sheet, [&]() {
    for (size_t i = 0; i < sheet.Pages.size(); i++)
      if (sheet.Inited[i])
        sheet.Notify(sheet.Pages[i], PSN_RESET);
    sheet.done(0);
  });
  QObject::connect(sheet.Apply, &QPushButton::clicked, &sheet, [&]() { if (sheet.ApplyAll()) result = 1; });
  int prev = -1;
  QObject::connect(sheet.Tabs, &QTabWidget::currentChanged, &sheet, [&](int i) {
    if (prev >= 0 && prev < (int)sheet.Pages.size() && sheet.Inited[(size_t)prev])
      if (sheet.Notify(sheet.Pages[(size_t)prev], PSN_KILLACTIVE) != 0)
      {
        QSignalBlocker b(sheet.Tabs);
        sheet.Tabs->setCurrentIndex(prev);
        return;
      }
    sheet.EnsureInited(i);
    if (i >= 0)
      sheet.Notify(sheet.Pages[(size_t)i], PSN_SETACTIVE);
    prev = i;
  });
  auto *lay = new QVBoxLayout(&sheet);
  lay->addWidget(sheet.Tabs);
  lay->addWidget(buttons);
  if (!sheet.Pages.empty())
  {
    const int start = (int)qMin<UINT>(header->nStartPage, (UINT)sheet.Pages.size() - 1);
    sheet.Tabs->setCurrentIndex(start);
    if (prev < 0)
    {
      sheet.EnsureInited(start);
      sheet.Notify(sheet.Pages[(size_t)start], PSN_SETACTIVE);
      prev = start;
    }
  }
  sheet.exec();
  for (Q32Dialog *p : sheet.Pages)
    p->Call(WM_DESTROY, 0, 0);
  return result;
}

INT_PTR PropertySheetA(const PROPSHEETHEADERA *header)
{
  // templates are resource IDs; titles are converted
  std::vector<PROPSHEETPAGEW> pages(header->nPages);
  std::vector<std::wstring> titles(header->nPages);
  for (UINT i = 0; i < header->nPages; i++)
  {
    const PROPSHEETPAGEA &a = header->ppsp[i];
    PROPSHEETPAGEW &w = pages[i];
    memset(&w, 0, sizeof(w));
    w.dwSize = sizeof(w);
    w.dwFlags = a.dwFlags;
    w.pszTemplate = (LPCWSTR)(const void *)a.pszTemplate;
    w.pfnDlgProc = a.pfnDlgProc;
    w.lParam = a.lParam;
    if (a.pszTitle)
    {
      titles[i] = QString::fromUtf8(a.pszTitle).toStdWString();
      w.pszTitle = titles[i].c_str();
    }
  }
  const std::wstring caption = header->pszCaption ? QString::fromUtf8(header->pszCaption).toStdWString() : std::wstring();
  PROPSHEETHEADERW h;
  memset(&h, 0, sizeof(h));
  h.dwSize = sizeof(h);
  h.hwndParent = header->hwndParent;
  h.pszCaption = caption.c_str();
  h.nPages = header->nPages;
  h.nStartPage = header->nStartPage;
  h.ppsp = pages.data();
  return PropertySheetW(&h);
}

// ---------- dialog items ----------

HWND GetDlgItem(HWND dialog, int id)
{
  Q32Dialog *d = dynamic_cast<Q32Dialog *>(W(dialog));
  if (!d)
    return NULL;
  auto it = d->Items.find(id);
  return it == d->Items.end() ? NULL : H(it->second);
}

int GetDlgCtrlID(HWND hwnd)
{
  return W(hwnd) ? W(hwnd)->property(kId).toInt() : 0;
}

static QString GetText(QWidget *w)
{
  if (!w) return QString();
  if (auto *l = qobject_cast<QLabel *>(w)) return l->text().replace(QLatin1String("&&"), QLatin1String("&"));
  if (auto *e = qobject_cast<QLineEdit *>(w)) return e->text();
  if (auto *e = qobject_cast<QPlainTextEdit *>(w)) return e->toPlainText().replace(QLatin1Char('\n'), QLatin1String("\r\n"));
  if (auto *c = qobject_cast<QComboBox *>(w)) return c->isEditable() ? c->currentText() : c->currentText();
  if (auto *b = qobject_cast<QAbstractButton *>(w)) return b->text();
  if (auto *g = qobject_cast<QGroupBox *>(w)) return g->title();
  if (qobject_cast<QDialog *>(w)) return w->windowTitle();
  return w->windowTitle();
}

static void SetText(QWidget *w, const QString &s)
{
  if (!w) return;
  const bool noPrefix = (w->property(kStyle).toUInt() & SS_NOPREFIX) != 0;
  if (auto *l = qobject_cast<QLabel *>(w)) l->setText(MnemonicText(s, noPrefix || !l->buddy()));
  else if (auto *e = qobject_cast<QLineEdit *>(w)) e->setText(s);
  else if (auto *e = qobject_cast<QPlainTextEdit *>(w)) e->setPlainText(QString(s).remove(QLatin1Char('\r')));
  else if (auto *c = qobject_cast<QComboBox *>(w))
  {
    if (c->isEditable())
      c->setEditText(s);
    else
      c->setCurrentIndex(c->findText(s));
  }
  else if (auto *b = qobject_cast<QAbstractButton *>(w)) b->setText(s);
  else if (auto *g = qobject_cast<QGroupBox *>(w)) g->setTitle(s);
  else w->setWindowTitle(s);
}

BOOL SetWindowTextW(HWND hwnd, LPCWSTR s) { SetText(W(hwnd), FromW(s)); return TRUE; }
BOOL SetWindowTextA(HWND hwnd, LPCSTR s) { SetText(W(hwnd), FromA(s)); return TRUE; }
int GetWindowTextW(HWND hwnd, LPWSTR s, int maxCount) { return CopyOutW(GetText(W(hwnd)), s, maxCount); }
int GetWindowTextA(HWND hwnd, LPSTR s, int maxCount) { return CopyOutA(GetText(W(hwnd)), s, maxCount); }
int GetWindowTextLengthW(HWND hwnd) { return (int)GetText(W(hwnd)).toStdWString().size(); }
int GetWindowTextLengthA(HWND hwnd) { return (int)GetText(W(hwnd)).toUtf8().size(); }

BOOL SetDlgItemTextW(HWND dialog, int id, LPCWSTR s) { return SetWindowTextW(GetDlgItem(dialog, id), s); }
BOOL SetDlgItemTextA(HWND dialog, int id, LPCSTR s) { return SetWindowTextA(GetDlgItem(dialog, id), s); }
UINT GetDlgItemTextW(HWND dialog, int id, LPWSTR s, int maxCount) { return (UINT)GetWindowTextW(GetDlgItem(dialog, id), s, maxCount); }
UINT GetDlgItemTextA(HWND dialog, int id, LPSTR s, int maxCount) { return (UINT)GetWindowTextA(GetDlgItem(dialog, id), s, maxCount); }

BOOL SetDlgItemInt(HWND dialog, int id, UINT value, BOOL isSigned)
{
  SetText(W(GetDlgItem(dialog, id)), isSigned ? QString::number((int)value) : QString::number(value));
  return TRUE;
}

UINT GetDlgItemInt(HWND dialog, int id, BOOL *translated, BOOL isSigned)
{
  bool ok = false;
  const QString s = GetText(W(GetDlgItem(dialog, id))).trimmed();
  const UINT v = isSigned ? (UINT)s.toInt(&ok) : s.toUInt(&ok);
  if (translated)
    *translated = ok ? TRUE : FALSE;
  return ok ? v : 0;
}

BOOL CheckDlgButton(HWND dialog, int id, UINT check)
{
  if (auto *b = qobject_cast<QAbstractButton *>(W(GetDlgItem(dialog, id))))
  {
    b->setChecked(check == BST_CHECKED);
    return TRUE;
  }
  return FALSE;
}

UINT IsDlgButtonChecked(HWND dialog, int id)
{
  if (auto *b = qobject_cast<QAbstractButton *>(W(GetDlgItem(dialog, id))))
    return b->isChecked() ? BST_CHECKED : BST_UNCHECKED;
  return BST_UNCHECKED;
}

BOOL CheckRadioButton(HWND dialog, int first, int last, int check)
{
  for (int id = first; id <= last; id++)
    if (auto *b = qobject_cast<QAbstractButton *>(W(GetDlgItem(dialog, id))))
      b->setChecked(id == check);
  return TRUE;
}

LRESULT SendDlgItemMessage(HWND dialog, int id, UINT msg, WPARAM w, LPARAM l)
{
  return SendMessageW(GetDlgItem(dialog, id), msg, w, l);
}

HWND GetNextDlgGroupItem(HWND, HWND control, BOOL) { return control; }

HWND GetNextDlgTabItem(HWND dialog, HWND control, BOOL previous)
{
  QWidget *w = W(control) ? W(control) : W(dialog);
  if (!w)
    return NULL;
  return H(previous ? w->previousInFocusChain() : w->nextInFocusChain());
}

BOOL MapDialogRect(HWND dialog, LPRECT rect)
{
  Q32Dialog *d = DialogOf(W(dialog));
  if (!d || !rect)
    return FALSE;
  rect->left = d->PxX(rect->left);
  rect->right = d->PxX(rect->right);
  rect->top = d->PxY(rect->top);
  rect->bottom = d->PxY(rect->bottom);
  return TRUE;
}

BOOL IsDialogMessage(HWND, LPMSG) { return FALSE; }

LONG GetDialogBaseUnits()
{
  const QFontMetrics fm(QApplication::font());
  static const QString kAlphabet = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
  const int x = (fm.horizontalAdvance(kAlphabet) + 26) / 52;
  return MAKELONG(x, fm.height());
}

// ---------- messages ----------

static LRESULT ComboMessage(QComboBox *c, UINT msg, WPARAM w, LPARAM l, bool wide)
{
  auto str = [&](LPARAM p) { return wide ? FromW((LPCWSTR)p) : FromA((LPCSTR)p); };
  const bool sorted = (c->property(kStyle).toUInt() & CBS_SORT) != 0;
  switch (msg)
  {
    case CB_RESETCONTENT: c->clear(); return 0;
    case CB_ADDSTRING:
    {
      const QString s = str(l);
      int index = c->count();
      if (sorted)
      {
        index = 0;
        while (index < c->count() && QString::compare(c->itemText(index), s, Qt::CaseInsensitive) <= 0)
          index++;
      }
      const bool keepEdit = c->isEditable();
      const QString edit = keepEdit ? c->currentText() : QString();
      const int cur = c->currentIndex();
      c->insertItem(index, s);
      if (!keepEdit && cur < 0)
        c->setCurrentIndex(-1);
      if (keepEdit && cur < 0)
      {
        c->setCurrentIndex(-1);
        c->setEditText(edit);
      }
      return index;
    }
    case CB_INSERTSTRING:
    {
      const int index = ((int)w < 0) ? c->count() : (int)w;
      c->insertItem(index, str(l));
      return index;
    }
    case CB_DELETESTRING:
      if ((int)w < 0 || (int)w >= c->count()) return CB_ERR;
      c->removeItem((int)w);
      return c->count();
    case CB_GETCOUNT: return c->count();
    case CB_GETCURSEL: return c->currentIndex();
    case CB_SETCURSEL:
      if ((int)w < 0 || (int)w >= c->count())
      {
        c->setCurrentIndex(-1);
        if (c->isEditable())
          c->setEditText(QString());
        return CB_ERR;
      }
      c->setCurrentIndex((int)w);
      return (int)w;
    case CB_GETLBTEXTLEN:
      if ((int)w < 0 || (int)w >= c->count()) return CB_ERR;
      return wide ? (LRESULT)c->itemText((int)w).toStdWString().size() : (LRESULT)c->itemText((int)w).toUtf8().size();
    case CB_GETLBTEXT:
      if ((int)w < 0 || (int)w >= c->count()) return CB_ERR;
      return wide ? CopyOutW(c->itemText((int)w), (LPWSTR)l, 1 << 20) : CopyOutA(c->itemText((int)w), (LPSTR)l, 1 << 20);
    case CB_SETITEMDATA:
      if ((int)w < 0 || (int)w >= c->count()) return CB_ERR;
      c->setItemData((int)w, QVariant::fromValue((qlonglong)l), Qt::UserRole);
      return 0;
    case CB_GETITEMDATA:
      if ((int)w < 0 || (int)w >= c->count()) return CB_ERR;
      return (LRESULT)c->itemData((int)w, Qt::UserRole).toLongLong();
    case CB_FINDSTRINGEXACT:
    {
      const QString s = str(l);
      for (int i = ((int)w < 0 ? 0 : (int)w + 1); i < c->count(); i++)
        if (QString::compare(c->itemText(i), s, Qt::CaseInsensitive) == 0)
          return i;
      return CB_ERR;
    }
    case CB_FINDSTRING:
    case CB_SELECTSTRING:
    {
      const QString s = str(l);
      for (int i = ((int)w < 0 ? 0 : (int)w + 1); i < c->count(); i++)
        if (c->itemText(i).startsWith(s, Qt::CaseInsensitive))
        {
          if (msg == CB_SELECTSTRING)
            c->setCurrentIndex(i);
          return i;
        }
      return CB_ERR;
    }
    case CB_SHOWDROPDOWN: if (w) c->showPopup(); else c->hidePopup(); return TRUE;
    case CB_GETDROPPEDSTATE: return c->view() && c->view()->isVisible();
    case CB_LIMITTEXT: if (c->lineEdit()) c->lineEdit()->setMaxLength(w ? (int)w : 32767); return TRUE;
    case CB_SETDROPPEDWIDTH: if (c->view()) c->view()->setMinimumWidth((int)w); return (LRESULT)w;
    case CB_SETEDITSEL: return TRUE;
  }
  return 0;
}

static LRESULT GenericMessage(QWidget *wd, UINT msg, WPARAM w, LPARAM l, bool wide)
{
  switch (msg)
  {
    case WM_SETTEXT: SetText(wd, wide ? FromW((LPCWSTR)l) : FromA((LPCSTR)l)); return TRUE;
    case WM_GETTEXT: return wide ? CopyOutW(GetText(wd), (LPWSTR)l, (int)w) : CopyOutA(GetText(wd), (LPSTR)l, (int)w);
    case WM_GETTEXTLENGTH: return wide ? GetWindowTextLengthW(H(wd)) : GetWindowTextLengthA(H(wd));
    case WM_SETREDRAW: wd->setUpdatesEnabled(w != 0); if (w) wd->update(); return 0;
    case WM_SETFONT:
      if (w)
        wd->setFont(*reinterpret_cast<const QFont *>(w));
      return 0;
    case WM_GETFONT: case WM_SETICON: return 0;
    case WM_CLOSE:
      if (auto *d = dynamic_cast<Q32Dialog *>(wd)) { d->Command(IDCANCEL, BN_CLICKED, nullptr); return 0; }
      wd->close();
      return 0;
  }
  return 0;
}

static LRESULT SendMessageImpl(HWND hwnd, UINT msg, WPARAM w, LPARAM l, bool wide)
{
  QWidget *wd = W(hwnd);
  if (!wd)
    return 0;
  if (msg == PSM_CHANGED || msg == PSM_UNCHANGED)
  {
    if (Q32Sheet *sheet = SheetOf(wd))
      sheet->Apply->setEnabled(msg == PSM_CHANGED);
    return 0;
  }
  if (auto *d = dynamic_cast<Q32Dialog *>(wd))
  {
    switch (msg)
    {
      case WM_NEXTDLGCTL:
        if (l)
        {
          if (W((HWND)w)) W((HWND)w)->setFocus();
        }
        else if (QWidget *f = QApplication::focusWidget())
          (w ? f->previousInFocusChain() : f->nextInFocusChain())->setFocus();
        return 0;
      case WM_SETTEXT: case WM_GETTEXT: case WM_GETTEXTLENGTH: case WM_SETREDRAW:
      case WM_SETFONT: case WM_GETFONT: case WM_SETICON: case WM_CLOSE:
        return GenericMessage(wd, msg, w, l, wide);
    }
    // custom (WM_USER / WM_APP) and other messages go to the dialog proc
    return d->Call(msg, w, l);
  }
  if (auto *c = qobject_cast<QComboBox *>(wd))
    if (msg >= CB_GETEDITSEL && msg <= CB_SETDROPPEDWIDTH)
      return ComboMessage(c, msg, w, l, wide);
  if (auto *b = qobject_cast<QAbstractButton *>(wd))
  {
    switch (msg)
    {
      case BM_GETCHECK: return b->isChecked() ? BST_CHECKED : BST_UNCHECKED;
      case BM_SETCHECK: b->setChecked(w == BST_CHECKED); return 0;
      case BM_CLICK: b->click(); return 0;
      case BM_SETSTYLE: case BM_SETIMAGE: return 0;
    }
  }
  if (auto *e = qobject_cast<QLineEdit *>(wd))
  {
    switch (msg)
    {
      case EM_SETPASSWORDCHAR: e->setEchoMode(w ? QLineEdit::Password : QLineEdit::Normal); return TRUE;
      case EM_SETREADONLY: e->setReadOnly(w != 0); return TRUE;
      case EM_LIMITTEXT: e->setMaxLength(w ? (int)w : 32767); return 0;
      case EM_SETSEL:
        if ((int)w == 0 && (int)l == -1) e->selectAll();
        else e->setSelection((int)w, (int)l - (int)w);
        return 0;
      case EM_REPLACESEL: e->insert(wide ? FromW((LPCWSTR)l) : FromA((LPCSTR)l)); return 0;
    }
  }
  if (auto *e = qobject_cast<QPlainTextEdit *>(wd))
  {
    switch (msg)
    {
      case EM_SETREADONLY: e->setReadOnly(w != 0); return TRUE;
      case EM_SETSEL: if ((int)w == 0 && (int)l == -1) e->selectAll(); return 0;
      case EM_REPLACESEL: e->insertPlainText(wide ? FromW((LPCWSTR)l) : FromA((LPCSTR)l)); return 0;
      case EM_LIMITTEXT: return 0;
    }
  }
  if (auto *p = qobject_cast<QProgressBar *>(wd))
  {
    switch (msg)
    {
      case PBM_SETRANGE32: p->setRange((int)w, (int)l); return 0;
      case PBM_SETRANGE: p->setRange(LOWORD(l), HIWORD(l)); return 0;
      case PBM_SETPOS: { const int old = p->value(); p->setValue((int)w); return old; }
      case PBM_DELTAPOS: p->setValue(p->value() + (int)w); return 0;
      case PBM_GETPOS: return p->value();
      case PBM_SETMARQUEE: if (w) p->setRange(0, 0); return TRUE;
      case PBM_SETSTATE: case PBM_SETBARCOLOR: return 0;
    }
  }
  if (auto *s = dynamic_cast<Q32Spin *>(wd))
  {
    switch (msg)
    {
      case UDM_SETRANGE32: s->Min = (int)w; s->Max = (int)l; return 0;
      case UDM_SETRANGE: s->Min = (short)HIWORD(l); s->Max = (short)LOWORD(l); return 0;
      case UDM_SETPOS32: case UDM_SETPOS: { const int old = s->Pos(); s->SetPos((int)(msg == UDM_SETPOS32 ? l : (short)LOWORD(l))); return old; }
      case UDM_GETPOS32: return s->Pos();
      case UDM_GETPOS: return s->Pos();
      case UDM_SETBUDDY: s->Buddy = qobject_cast<QLineEdit *>(W((HWND)w)); return 0;
    }
  }
  if (auto *lw = qobject_cast<QListWidget *>(wd))
  {
    switch (msg)
    {
      case LB_ADDSTRING: lw->addItem(wide ? FromW((LPCWSTR)l) : FromA((LPCSTR)l)); return lw->count() - 1;
      case LB_RESETCONTENT: lw->clear(); return 0;
      case LB_SETCURSEL: lw->setCurrentRow((int)w); return 0;
      case LB_GETCURSEL: return lw->currentRow();
      case LB_GETCOUNT: return lw->count();
    }
  }
  if (auto *sl = qobject_cast<QSlider *>(wd))
  {
    switch (msg)
    {
      case TBM_GETPOS: return sl->value();
      case TBM_SETPOS: sl->setValue((int)l); return 0;
      case TBM_SETRANGE: sl->setRange(LOWORD(l), HIWORD(l)); return 0;
    }
  }
  return GenericMessage(wd, msg, w, l, wide);
}

LRESULT SendMessageW(HWND hwnd, UINT msg, WPARAM w, LPARAM l) { return SendMessageImpl(hwnd, msg, w, l, true); }
LRESULT SendMessageA(HWND hwnd, UINT msg, WPARAM w, LPARAM l) { return SendMessageImpl(hwnd, msg, w, l, false); }

BOOL PostMessageW(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
  QPointer<QWidget> wd = W(hwnd);
  if (!wd)
    return FALSE;
  QMetaObject::invokeMethod(qApp, [wd, msg, w, l]() {
    if (wd)
      SendMessageW(H(wd.data()), msg, w, l);
  }, Qt::QueuedConnection);
  return TRUE;
}

BOOL PostMessageA(HWND hwnd, UINT msg, WPARAM w, LPARAM l) { return PostMessageW(hwnd, msg, w, l); }

// ---------- windows ----------

BOOL EnableWindow(HWND hwnd, BOOL enable)
{
  QWidget *w = W(hwnd);
  if (!w) return FALSE;
  const bool was = w->isEnabled();
  w->setEnabled(enable != FALSE);
  if (auto *s = qobject_cast<QLineEdit *>(w))
    for (QWidget *sib : w->parentWidget()->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
      if (auto *spin = dynamic_cast<Q32Spin *>(sib))
        if (spin->Buddy == s)
          spin->setEnabled(enable != FALSE);
  return was ? FALSE : TRUE;
}

BOOL IsWindowEnabled(HWND hwnd) { return W(hwnd) && W(hwnd)->isEnabled(); }

BOOL ShowWindow(HWND hwnd, int cmdShow)
{
  QWidget *w = W(hwnd);
  if (!w) return FALSE;
  const bool was = w->isVisible();
  switch (cmdShow)
  {
    case SW_HIDE: w->hide(); break;
    case SW_MINIMIZE: case SW_SHOWMINIMIZED: w->showMinimized(); break;
    case SW_MAXIMIZE: w->showMaximized(); break;
    case SW_RESTORE: w->showNormal(); break;
    default: w->show(); break;
  }
  return was;
}

BOOL IsWindowVisible(HWND hwnd) { return W(hwnd) && W(hwnd)->isVisible(); }
BOOL IsWindow(HWND hwnd) { return hwnd != NULL; }
BOOL IsZoomed(HWND hwnd) { return W(hwnd) && W(hwnd)->isMaximized(); }
HWND GetParent(HWND hwnd)
{
  QWidget *w = W(hwnd);
  if (!w)
    return NULL;
  if (dynamic_cast<Q32Dialog *>(w))
    if (Q32Sheet *sheet = SheetOf(w->parentWidget()))
      return H(sheet);  // a property page's parent is the sheet
  return H(w->parentWidget());
}
HWND SetFocus(HWND hwnd) { HWND old = H(QApplication::focusWidget()); if (W(hwnd)) W(hwnd)->setFocus(); return old; }
HWND GetFocus() { return H(QApplication::focusWidget()); }
BOOL SetForegroundWindow(HWND hwnd) { if (W(hwnd)) { W(hwnd)->raise(); W(hwnd)->activateWindow(); } return TRUE; }
BOOL UpdateWindow(HWND hwnd) { if (W(hwnd)) W(hwnd)->update(); return TRUE; }
BOOL InvalidateRect(HWND hwnd, LPCRECT, BOOL) { if (W(hwnd)) W(hwnd)->update(); return TRUE; }

BOOL MoveWindow(HWND hwnd, int x, int y, int w, int h, BOOL)
{
  QWidget *wd = W(hwnd);
  if (!wd) return FALSE;
  if (wd->isWindow())
  {
    wd->move(x, y);
    wd->resize(w, h);
  }
  else
    wd->setGeometry(x, y, w, h);
  return TRUE;
}

BOOL SetWindowPos(HWND hwnd, HWND, int x, int y, int cx, int cy, UINT flags)
{
  QWidget *wd = W(hwnd);
  if (!wd) return FALSE;
  if (!(flags & SWP_NOMOVE)) wd->move(x, y);
  if (!(flags & SWP_NOSIZE)) wd->resize(cx, cy);
  if (flags & SWP_SHOWWINDOW) wd->show();
  return TRUE;
}

BOOL GetWindowRect(HWND hwnd, LPRECT r)
{
  QWidget *w = W(hwnd);
  if (!w || !r) return FALSE;
  QRect g = w->isWindow() ? w->frameGeometry() : QRect(w->mapToGlobal(QPoint(0, 0)), w->size());
  r->left = g.left(); r->top = g.top(); r->right = g.left() + g.width(); r->bottom = g.top() + g.height();
  return TRUE;
}

BOOL GetClientRect(HWND hwnd, LPRECT r)
{
  QWidget *w = W(hwnd);
  if (!w || !r) return FALSE;
  r->left = 0; r->top = 0; r->right = w->width(); r->bottom = w->height();
  return TRUE;
}

BOOL ClientToScreen(HWND hwnd, LPPOINT p)
{
  QWidget *w = W(hwnd);
  if (!w || !p) return FALSE;
  const QPoint g = w->mapToGlobal(QPoint(p->x, p->y));
  p->x = g.x(); p->y = g.y();
  return TRUE;
}

BOOL ScreenToClient(HWND hwnd, LPPOINT p)
{
  QWidget *w = W(hwnd);
  if (!w || !p) return FALSE;
  const QPoint g = w->mapFromGlobal(QPoint(p->x, p->y));
  p->x = g.x(); p->y = g.y();
  return TRUE;
}

BOOL SetWindowPlacement(HWND hwnd, const WINDOWPLACEMENT *p)
{
  if (!W(hwnd) || !p) return FALSE;
  const RECT &r = p->rcNormalPosition;
  MoveWindow(hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
  return TRUE;
}

BOOL GetWindowPlacement(HWND hwnd, WINDOWPLACEMENT *p)
{
  if (!W(hwnd) || !p) return FALSE;
  p->showCmd = W(hwnd)->isMaximized() ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
  GetWindowRect(hwnd, &p->rcNormalPosition);
  return TRUE;
}

LONG_PTR SetWindowLongPtrW(HWND hwnd, int index, LONG_PTR v)
{
  QWidget *w = W(hwnd);
  if (!w) return 0;
  if (auto *d = dynamic_cast<Q32Dialog *>(w))
  {
    if (index == GWLP_USERDATA || index == DWLP_USER)
    {
      // Upstream Window.h casts the pointer to 32-bit LONG unless _WIN64 is
      // defined (never on Linux), so DialogProcedure stores a truncated
      // CDialog*. It always stores the WM_INITDIALOG parameter: restore it.
      if (v == (LONG_PTR)(LONG)d->InitParam)
        v = (LONG_PTR)d->InitParam;
      const LONG_PTR old = d->UserData;
      d->UserData = v;
      return old;
    }
    if (index == DWLP_MSGRESULT) { const LONG_PTR old = d->MsgResult; d->MsgResult = v; return old; }
  }
  if (index == GWL_STYLE) { const LONG_PTR old = (LONG_PTR)w->property(kStyle).toUInt(); w->setProperty(kStyle, (uint)v); return old; }
  const LONG_PTR old = (LONG_PTR)w->property(kUserData).toLongLong();
  w->setProperty(kUserData, (qlonglong)v);
  return old;
}

LONG_PTR GetWindowLongPtrW(HWND hwnd, int index)
{
  QWidget *w = W(hwnd);
  if (!w) return 0;
  if (auto *d = dynamic_cast<Q32Dialog *>(w))
  {
    if (index == GWLP_USERDATA || index == DWLP_USER) return d->UserData;
    if (index == DWLP_MSGRESULT) return d->MsgResult;
    if (index == GWL_STYLE) return (LONG_PTR)d->Style;
  }
  if (index == GWL_STYLE) return (LONG_PTR)w->property(kStyle).toUInt();
  if (index == GWLP_ID) return w->property(kId).toInt();
  return (LONG_PTR)w->property(kUserData).toLongLong();
}

LONG SetWindowLongW(HWND hwnd, int index, LONG v) { return (LONG)SetWindowLongPtrW(hwnd, index, v); }
LONG GetWindowLongW(HWND hwnd, int index) { return (LONG)GetWindowLongPtrW(hwnd, index); }

HMENU GetSystemMenu(HWND, BOOL) { return NULL; }

UINT_PTR SetTimer(HWND hwnd, UINT_PTR id, UINT elapse, TIMERPROC)
{
  Q32Dialog *d = DialogOf(W(hwnd));
  if (!d) return 0;
  QTimer *&t = d->Timers[id];
  if (!t)
  {
    t = new QTimer(d);
    QObject::connect(t, &QTimer::timeout, d, [d, id]() { d->Call(WM_TIMER, id, 0); });
  }
  t->start((int)elapse);
  return id;
}

BOOL KillTimer(HWND hwnd, UINT_PTR id)
{
  Q32Dialog *d = DialogOf(W(hwnd));
  if (!d) return FALSE;
  auto it = d->Timers.find(id);
  if (it == d->Timers.end()) return FALSE;
  it->second->stop();
  return TRUE;
}

SHORT GetKeyState(int key)
{
  const Qt::KeyboardModifiers m = QApplication::queryKeyboardModifiers();
  bool down = false;
  switch (key)
  {
    case VK_SHIFT: down = m & Qt::ShiftModifier; break;
    case VK_CONTROL: down = m & Qt::ControlModifier; break;
    case VK_MENU: down = m & Qt::AltModifier; break;
  }
  return down ? (SHORT)0x8000 : 0;
}

int GetSystemMetrics(int index)
{
  const QScreen *s = QApplication::primaryScreen();
  switch (index)
  {
    case SM_CXSCREEN: return s ? s->geometry().width() : 1920;
    case SM_CYSCREEN: return s ? s->geometry().height() : 1080;
    case SM_CXVSCROLL: case SM_CYHSCROLL: return 16;
    case SM_CYCAPTION: return 24;
    default: return 2;
  }
}

static void ScreenWorkArea(RECT *r)
{
  const QScreen *s = QApplication::primaryScreen();
  const QRect g = s ? s->availableGeometry() : QRect(0, 0, 1920, 1080);
  r->left = g.left(); r->top = g.top(); r->right = g.left() + g.width(); r->bottom = g.top() + g.height();
}

BOOL SystemParametersInfoW(UINT action, UINT, PVOID pv, UINT)
{
  if (action == SPI_GETWORKAREA && pv)
  {
    ScreenWorkArea((RECT *)pv);
    return TRUE;
  }
  return FALSE;
}

HMONITOR MonitorFromWindow(HWND, DWORD) { return (HMONITOR)1; }

BOOL GetMonitorInfoA(HMONITOR, LPMONITORINFO mi)
{
  if (!mi) return FALSE;
  ScreenWorkArea(&mi->rcWork);
  const QScreen *s = QApplication::primaryScreen();
  const QRect g = s ? s->geometry() : QRect(0, 0, 1920, 1080);
  mi->rcMonitor.left = g.left(); mi->rcMonitor.top = g.top();
  mi->rcMonitor.right = g.left() + g.width(); mi->rcMonitor.bottom = g.top() + g.height();
  mi->dwFlags = 1;
  return TRUE;
}

BOOL GetMonitorInfoW(HMONITOR m, LPMONITORINFO mi) { return GetMonitorInfoA(m, mi); }

HWND CreateWindowExW(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID) { return NULL; }
HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID) { return NULL; }
ATOM RegisterClassW(const WNDCLASSW *) { return 1; }
ATOM RegisterClassA(const WNDCLASSA *) { return 1; }
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return 0; }
LRESULT CallWindowProcW(WNDPROC proc, HWND hwnd, UINT msg, WPARAM w, LPARAM l) { return proc ? proc(hwnd, msg, w, l) : 0; }

// ---------- message boxes ----------

static int RunMessageBox(HWND hwnd, const QString &text, const QString &caption, UINT type)
{
  int result = IDOK;
  auto run = [&]() {
    QWidget *p = W(hwnd);
    if (!p)
      p = QApplication::activeModalWidget() ? QApplication::activeModalWidget() : QApplication::activeWindow();
    QMessageBox box(p);
    box.setWindowTitle(caption.isEmpty() ? QStringLiteral("7-Zip") : caption);
    box.setText(text);
    switch (type & 0xF0)
    {
      case MB_ICONERROR: box.setIcon(QMessageBox::Critical); break;
      case MB_ICONWARNING: box.setIcon(QMessageBox::Warning); break;
      case MB_ICONQUESTION: box.setIcon(QMessageBox::Question); break;
      case MB_ICONINFORMATION: box.setIcon(QMessageBox::Information); break;
    }
    QMessageBox::StandardButtons buttons;
    switch (type & 0xF)
    {
      case MB_OKCANCEL: buttons = QMessageBox::Ok | QMessageBox::Cancel; break;
      case MB_YESNOCANCEL: buttons = QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel; break;
      case MB_YESNO: buttons = QMessageBox::Yes | QMessageBox::No; break;
      case MB_RETRYCANCEL: buttons = QMessageBox::Retry | QMessageBox::Cancel; break;
      case MB_ABORTRETRYIGNORE: buttons = QMessageBox::Abort | QMessageBox::Retry | QMessageBox::Ignore; break;
      default: buttons = QMessageBox::Ok; break;
    }
    box.setStandardButtons(buttons);
    switch (box.exec())
    {
      case QMessageBox::Ok: result = IDOK; break;
      case QMessageBox::Cancel: result = IDCANCEL; break;
      case QMessageBox::Yes: result = IDYES; break;
      case QMessageBox::No: result = IDNO; break;
      case QMessageBox::Retry: result = IDRETRY; break;
      case QMessageBox::Abort: result = IDABORT; break;
      case QMessageBox::Ignore: result = IDIGNORE; break;
      default: result = IDCANCEL; break;
    }
  };
  if (QThread::currentThread() == qApp->thread())
    run();
  else
    QMetaObject::invokeMethod(qApp, run, Qt::BlockingQueuedConnection);
  return result;
}

int MessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type) { return RunMessageBox(hwnd, FromW(text), FromW(caption), type); }
int MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type) { return RunMessageBox(hwnd, FromA(text), FromA(caption), type); }
BOOL MessageBeep(UINT) { QApplication::beep(); return TRUE; }

HICON LoadIconW(HINSTANCE, LPCWSTR) { return NULL; }
HANDLE LoadImageW(HINSTANCE, LPCWSTR, UINT, int, int, UINT) { return NULL; }
BOOL DestroyIcon(HICON) { return TRUE; }

void Sleep(DWORD ms) { usleep((useconds_t)ms * 1000); }

// HFONT is a QFont*
HFONT CreateFontIndirectA(const LOGFONTA *lf)
{
  QFont *f = new QFont(QApplication::font());
  if (lf)
  {
    if ((lf->lfPitchAndFamily & 3) == FIXED_PITCH)
      f->setFamilies({ QStringLiteral("monospace") }), f->setStyleHint(QFont::Monospace), f->setFixedPitch(true);
    if (lf->lfFaceName[0])
      f->setFamily(QString::fromLatin1(lf->lfFaceName));
    if (lf->lfWeight >= FW_BOLD)
      f->setBold(true);
  }
  return (HFONT)f;
}

BOOL DeleteObject(HGDIOBJ obj)
{
  delete reinterpret_cast<QFont *>(obj);
  return TRUE;
}
