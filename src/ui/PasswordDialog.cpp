// PasswordDialog.cpp

#include "PasswordDialog.h"

#include "Common/Common.h"
#include "7zip/UI/Common/ZipRegistry.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/PasswordDialogRes.h"

#include "QtConv.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

static QString LangQ(UInt32 id, const char *fallback)
{
  const UString s = LangString(id);
  return s.IsEmpty() ? QString::fromLatin1(fallback) : ToQ(s);
}

PasswordDialog::PasswordDialog(QWidget *parent): QDialog(parent)
{
  setWindowTitle(LangQ(IDD_PASSWORD, "Enter password"));
  auto *label = new QLabel(LangQ(IDT_PASSWORD_ENTER, "&Enter password:"));
  _edit = new QLineEdit;
  label->setBuddy(_edit);
  _show = new QCheckBox(LangQ(IDX_PASSWORD_SHOW, "&Show password"));
  _show->setChecked(NExtract::Read_ShowPassword());
  _edit->setEchoMode(_show->isChecked() ? QLineEdit::Normal : QLineEdit::Password);
  connect(_show, &QCheckBox::toggled, this, [this](bool on) {
    _edit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
  });
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  auto *lay = new QVBoxLayout(this);
  lay->addWidget(label);
  lay->addWidget(_edit);
  lay->addWidget(_show);
  lay->addWidget(buttons);
  setMinimumWidth(360);
}

QString PasswordDialog::Password() const { return _edit->text(); }

void PasswordDialog::accept()
{
  if (_show->isChecked() != NExtract::Read_ShowPassword())
    NExtract::Save_ShowPassword(_show->isChecked());
  QDialog::accept();
}
