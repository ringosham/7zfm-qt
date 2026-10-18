// OverwriteDialog.cpp

#include "OverwriteDialog.h"

#include "Common/IntToString.h"
#include "Windows/PropVariantConv.h"
#include "Windows/System.h"

#include "7zip/UI/FileManager/FormatUtils.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/MemDialogRes.h"
#include "7zip/UI/FileManager/OverwriteDialogRes.h"
#include "7zip/UI/FileManager/ProgressDialog2Res.h"
#include "7zip/UI/FileManager/resourceGui.h"
#include "7zip/UI/GUI/ExtractRes.h"

#include "IconProvider.h"
#include "ListFormat.h"
#include "QtConv.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

void AddSizeValue(UString &s, UInt64 value);

static QString Lq(UInt32 id, const char *fallback = "")
{
  const UString s = LangString(id);
  return s.IsEmpty() ? QString::fromLatin1(fallback) : ToQ(s);
}

static QString FileInfoText(const COverwriteFileInfo &fi)
{
  QString s;
  if (fi.Size_IsDefined)
  {
    UString u;
    AddSizeValue(u, fi.Size);
    s += ToQ(u);
  }
  if (fi.Time_IsDefined)
  {
    char t[64];
    ConvertUtcFileTimeToString(fi.Time, t, kTimestampPrintLevel_SEC);
    if (!s.isEmpty())
      s += QLatin1Char('\n');
    s += QObject::tr("modified on ") + QString::fromLatin1(t);
  }
  return s;
}

EOverwriteAnswer RunOverwriteDialog(QWidget *parent, const COverwriteFileInfo &oldFile, const COverwriteFileInfo &newFile)
{
  QDialog d(parent);
  d.setWindowTitle(Lq(IDD_OVERWRITE, "Confirm File Replace"));
  auto *lay = new QVBoxLayout(&d);
  lay->addWidget(new QLabel(Lq(IDT_OVERWRITE_HEADER)));
  lay->addWidget(new QLabel(Lq(IDT_OVERWRITE_QUESTION_BEGIN)));
  auto addFile = [&](const COverwriteFileInfo &fi) {
    auto *g = new QGridLayout;
    auto *icon = new QLabel;
    icon->setPixmap(GetIconProvider().ForFile(ToQ(fi.Path)).pixmap(32, 32));
    g->addWidget(icon, 0, 0, 2, 1, Qt::AlignTop);
    auto *path = new QLabel(ToQ(fi.Path));
    path->setTextFormat(Qt::PlainText);
    path->setWordWrap(true);
    g->addWidget(path, 0, 1);
    g->addWidget(new QLabel(FileInfoText(fi)), 1, 1);
    g->setColumnStretch(1, 1);
    lay->addLayout(g);
  };
  addFile(oldFile);
  lay->addWidget(new QLabel(Lq(IDT_OVERWRITE_QUESTION_END)));
  addFile(newFile);

  auto *buttons = new QDialogButtonBox;
  EOverwriteAnswer answer = kOverwrite_Cancel;
  auto add = [&](const QString &text, EOverwriteAnswer a, QDialogButtonBox::ButtonRole role) {
    QPushButton *b = buttons->addButton(text, role);
    QObject::connect(b, &QPushButton::clicked, &d, [&d, &answer, a]() { answer = a; d.done(1); });
    return b;
  };
  add(QObject::tr("&Yes"), kOverwrite_Yes, QDialogButtonBox::YesRole)->setDefault(true);
  add(Lq(IDB_YES_TO_ALL, "Yes to &All"), kOverwrite_YesToAll, QDialogButtonBox::YesRole);
  add(QObject::tr("&No"), kOverwrite_No, QDialogButtonBox::NoRole);
  add(Lq(IDB_NO_TO_ALL, "No to A&ll"), kOverwrite_NoToAll, QDialogButtonBox::NoRole);
  add(Lq(IDB_AUTO_RENAME, "A&uto Rename"), kOverwrite_AutoRename, QDialogButtonBox::ActionRole);
  add(QObject::tr("Cancel"), kOverwrite_Cancel, QDialogButtonBox::RejectRole);
  lay->addWidget(buttons);
  d.setMinimumWidth(520);
  if (d.exec() == 0)
    return kOverwrite_Cancel;
  return answer;
}

bool RunMemDialog(QWidget *parent, CMemUseQuestion &q)
{
  // upstream CMemDialog: explain, let the user raise the limit or skip
  size_t ramSize = (size_t)sizeof(size_t) << 29;
  NWindows::NSystem::GetRamSize(ramSize);
  UInt32 ramSize_GB = (UInt32)(((UInt64)ramSize + (1u << 29)) >> 30);
  if (ramSize_GB == 0)
    ramSize_GB = 1;

  QDialog d(parent);
  d.setWindowTitle(QStringLiteral("7-Zip"));
  auto *lay = new QVBoxLayout(&d);
  UString msg;
  q.AddInfoMessage_To_String(msg, &ramSize_GB);
  if (!q.ArcPath.IsEmpty())
  {
    msg.Add_LF();
    AddLangString(msg, q.TestMode ? IDS_PROGRESS_TESTING : IDS_PROGRESS_EXTRACTING);
    msg += ": ";
    msg += q.ArcPath;
  }
  auto *text = new QLabel(ToQ(msg));
  text->setTextFormat(Qt::PlainText);
  text->setWordWrap(true);
  lay->addWidget(text);

  auto *allow = new QRadioButton(Lq(IDR_MEM_ACTION_ALLOW, "Allow archive unpacking"));
  auto *skip = new QRadioButton(Lq(IDR_MEM_ACTION_SKIP_ARC, "Skip archive unpacking"));
  allow->setChecked(!q.SkipArc);
  skip->setChecked(q.SkipArc);
  auto *spin = new QSpinBox;
  spin->setRange(1, (int)qMax<UInt32>(ramSize_GB * 4, q.Required_GB * 2));
  spin->setValue((int)qMax(q.Limit_GB, q.Required_GB));
  spin->setSuffix(QStringLiteral(" GB"));
  auto *row = new QHBoxLayout;
  row->addWidget(allow);
  row->addWidget(spin);
  lay->addLayout(row);
  lay->addWidget(skip);
  auto *save = new QCheckBox(Lq(IDX_MEM_SAVE_LIMIT, "Save the limit"));
  auto *remember = new QCheckBox(Lq(IDX_MEM_REMEMBER, "Remember choice"));
  lay->addWidget(save);
  if (q.ShowRemember)
    lay->addWidget(remember);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  lay->addWidget(buttons);
  if (d.exec() != QDialog::Accepted)
    return false;
  q.Limit_GB = (UInt32)spin->value();
  q.SkipArc = skip->isChecked();
  q.NeedSave = save->isChecked();
  q.Remember = q.ShowRemember && remember->isChecked();
  return true;
}
