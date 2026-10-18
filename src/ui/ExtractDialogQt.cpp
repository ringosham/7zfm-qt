// ExtractDialogQt.cpp -- Qt version of upstream GUI/ExtractDialog.cpp;
// OnInit()/OnOK() logic is kept: path history, "split destination"
// (extract into a subfolder named after the archive), modes, password.

#include "ExtractDialogQt.h"

#include "Common/Common.h"
#include "Common/Wildcard.h"
#include "Windows/FileName.h"

#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/GUI/ExtractDialogRes.h"
#include "7zip/UI/GUI/ExtractRes.h"

#include "../fm/ExtractDialog.h"
#include "QtConv.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

using namespace NWindows;
using namespace NFile;
using namespace NName;

static const UInt32 kPathMode_IDs[] =
{
  IDS_EXTRACT_PATHS_FULL,
  IDS_EXTRACT_PATHS_NO,
  IDS_EXTRACT_PATHS_ABS
};

static const UInt32 kOverwriteMode_IDs[] =
{
  IDS_EXTRACT_OVERWRITE_ASK,
  IDS_EXTRACT_OVERWRITE_WITHOUT_PROMPT,
  IDS_EXTRACT_OVERWRITE_SKIP_EXISTING,
  IDS_EXTRACT_OVERWRITE_RENAME,
  IDS_EXTRACT_OVERWRITE_RENAME_EXISTING
};

static const int kPathModeButtonsVals[] =
{
  NExtract::NPathMode::kFullPaths,
  NExtract::NPathMode::kNoPaths,
  NExtract::NPathMode::kAbsPaths
};

static const int kOverwriteButtonsVals[] =
{
  NExtract::NOverwriteMode::kAsk,
  NExtract::NOverwriteMode::kOverwrite,
  NExtract::NOverwriteMode::kSkip,
  NExtract::NOverwriteMode::kRename,
  NExtract::NOverwriteMode::kRenameExisting
};

static const unsigned kHistorySize = 16;

static QString Lq(UInt32 id, const char *fallback = "")
{
  const UString s = LangString(id);
  return s.IsEmpty() ? QString::fromLatin1(fallback) : ToQ(s);
}

static void AddComboItems(QComboBox *combo, const UInt32 *langIDs, unsigned numItems, const int *values, int curVal)
{
  int curSel = 0;
  for (unsigned i = 0; i < numItems; i++)
  {
    QString s = Lq(langIDs[i]);
    s.remove(QLatin1Char('&'));
    combo->addItem(s);
    if (values[i] == curVal)
      curSel = (int)i;
  }
  combo->setCurrentIndex(curSel);
}

static bool GetBoolsVal(const CBoolPair &b1, const CBoolPair &b2)
{
  if (b1.Def) return b1.Val;
  if (b2.Def) return b2.Val;
  return b1.Val;
}

static void GetButton_Bools(bool val, CBoolPair &b1, CBoolPair &b2)
{
  const bool oldVal = GetBoolsVal(b1, b2);
  if (val != oldVal)
    b1.Def = b2.Def = true;
  b1.Val = b2.Val = val;
}

static void AddUniqueString(UStringVector &list, const UString &s)
{
  FOR_VECTOR (i, list)
    if (s.IsEqualTo_NoCase(list[i]))
      return;
  list.Add(s);
}

bool RunExtractDialog(QWidget *parent, CExtractDialog &dlg)
{
  NExtract::CInfo info;
  info.Load();

  if (info.PathMode == NExtract::NPathMode::kCurPaths)
    info.PathMode = NExtract::NPathMode::kFullPaths;
  if (!dlg.PathMode_Force && info.PathMode_Force)
    dlg.PathMode = info.PathMode;
  if (!dlg.OverwriteMode_Force && info.OverwriteMode_Force)
    dlg.OverwriteMode = info.OverwriteMode;

  QDialog d(parent);
  QString title = Lq(IDD_EXTRACT, "Extract");
  if (!dlg.ArcPath.IsEmpty())
    title += QLatin1String(" : ") + ToQ(dlg.ArcPath);
  d.setWindowTitle(title);

  auto *pathCombo = new QComboBox;
  pathCombo->setEditable(true);
  pathCombo->setInsertPolicy(QComboBox::NoInsert);
  pathCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto *browse = new QPushButton(QStringLiteral("..."));
  browse->setFixedWidth(40);
  auto *nameEnable = new QCheckBox;
  auto *nameEdit = new QLineEdit;

  UString pathPrefix = dlg.DirPath;
  if (info.SplitDest.Val)
  {
    nameEnable->setChecked(true);
    UString pathName;
    SplitPathToParts_Smart(dlg.DirPath, pathPrefix, pathName);
    if (pathPrefix.IsEmpty())
      pathPrefix = pathName;
    else
      nameEdit->setText(ToQ(pathName));
  }
  else
    nameEdit->setVisible(false);
  for (unsigned i = 0; i < info.Paths.Size() && i < kHistorySize; i++)
    pathCombo->addItem(ToQ(info.Paths[i]));
  pathCombo->setCurrentIndex(-1);
  pathCombo->setEditText(ToQ(pathPrefix));

  QObject::connect(nameEnable, &QCheckBox::toggled, nameEdit, &QWidget::setVisible);
  QObject::connect(browse, &QPushButton::clicked, &d, [&]() {
    const QString dir = QFileDialog::getExistingDirectory(&d, Lq(IDS_EXTRACT_SET_FOLDER, "Specify a location for extracted files."),
        pathCombo->currentText());
    if (!dir.isEmpty())
    {
      pathCombo->setCurrentIndex(-1);
      pathCombo->setEditText(dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/'));
    }
  });

  auto *pathMode = new QComboBox;
  auto *overwriteMode = new QComboBox;
  AddComboItems(pathMode, kPathMode_IDs, Z7_ARRAY_SIZE(kPathMode_IDs), kPathModeButtonsVals, dlg.PathMode);
  AddComboItems(overwriteMode, kOverwriteMode_IDs, Z7_ARRAY_SIZE(kOverwriteMode_IDs), kOverwriteButtonsVals, dlg.OverwriteMode);
  auto *elimDup = new QCheckBox(Lq(IDX_EXTRACT_ELIM_DUP, "Eliminate duplication of root folder"));
  elimDup->setChecked(GetBoolsVal(dlg.ElimDup, info.ElimDup));

  auto *password = new QLineEdit(ToQ(dlg.Password));
  auto *showPassword = new QCheckBox(Lq(IDX_PASSWORD_SHOW, "&Show Password"));
  showPassword->setChecked(info.ShowPassword.Val);
  password->setEchoMode(showPassword->isChecked() ? QLineEdit::Normal : QLineEdit::Password);
  QObject::connect(showPassword, &QCheckBox::toggled, password, [password](bool on) {
    password->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
  });

  auto *lay = new QVBoxLayout(&d);
  auto *toLabel = new QLabel(Lq(IDT_EXTRACT_EXTRACT_TO, "E&xtract to:"));
  toLabel->setBuddy(pathCombo);
  lay->addWidget(toLabel);
  auto *pathRow = new QHBoxLayout;
  pathRow->addWidget(pathCombo);
  pathRow->addWidget(browse);
  lay->addLayout(pathRow);
  auto *nameRow = new QHBoxLayout;
  nameRow->addWidget(nameEnable);
  nameRow->addWidget(nameEdit);
  lay->addLayout(nameRow);

  auto *cols = new QHBoxLayout;
  auto *left = new QFormLayout;
  left->addRow(Lq(IDT_EXTRACT_PATH_MODE, "Path mode:"), pathMode);
  left->addRow(Lq(IDT_EXTRACT_OVERWRITE_MODE, "Overwrite mode:"), overwriteMode);
  left->addRow(elimDup);
  cols->addLayout(left);
  auto *pwBox = new QGroupBox(Lq(IDG_PASSWORD, "Password"));
  auto *pwLay = new QVBoxLayout(pwBox);
  pwLay->addWidget(password);
  pwLay->addWidget(showPassword);
  pwLay->addStretch();
  cols->addWidget(pwBox);
  lay->addLayout(cols);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  lay->addWidget(buttons);
  d.resize(620, d.sizeHint().height());

  if (d.exec() != QDialog::Accepted)
    return false;

  // ---- upstream CExtractDialog::OnOK() ----
  const int pathMode2 = kPathModeButtonsVals[pathMode->currentIndex()];
  if (dlg.PathMode != NExtract::NPathMode::kCurPaths ||
      pathMode2 != NExtract::NPathMode::kFullPaths)
    dlg.PathMode = (NExtract::NPathMode::EEnum)pathMode2;
  dlg.OverwriteMode = (NExtract::NOverwriteMode::EEnum)kOverwriteButtonsVals[overwriteMode->currentIndex()];
  dlg.Password = ToU(password->text());

  GetButton_Bools(elimDup->isChecked(), dlg.ElimDup, info.ElimDup);

  if (showPassword->isChecked() != info.ShowPassword.Val)
  {
    info.ShowPassword.Def = true;
    info.ShowPassword.Val = showPassword->isChecked();
  }
  if (info.PathMode != pathMode2)
  {
    info.PathMode_Force = true;
    info.PathMode = (NExtract::NPathMode::EEnum)pathMode2;
  }
  if (!dlg.OverwriteMode_Force && info.OverwriteMode != dlg.OverwriteMode)
    info.OverwriteMode_Force = true;
  info.OverwriteMode = dlg.OverwriteMode;

  UString s = ToU(pathCombo->currentText());
  s.Trim();
  NName::NormalizeDirPathPrefix(s);
  const bool splitDest = nameEnable->isChecked();
  if (splitDest)
  {
    UString pathName = ToU(nameEdit->text());
    pathName.Trim();
    s += pathName;
    NName::NormalizeDirPathPrefix(s);
  }
  if (splitDest != info.SplitDest.Val)
  {
    info.SplitDest.Def = true;
    info.SplitDest.Val = splitDest;
  }
  dlg.DirPath = s;

  info.Paths.Clear();
  AddUniqueString(info.Paths, s);
  for (int i = 0; i < pathCombo->count(); i++)
  {
    UString t = ToU(pathCombo->itemText(i));
    t.Trim();
    AddUniqueString(info.Paths, t);
  }
  info.Save();
  return true;
}
