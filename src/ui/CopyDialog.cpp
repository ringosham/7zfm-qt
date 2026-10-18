// CopyDialog.cpp

#include "CopyDialog.h"

#include "QtConv.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

bool RunCopyDialog(QWidget *parent, CCopyDialog &cd)
{
  QDialog d(parent ? parent->window() : nullptr);
  QString title = ToQ(cd.Title);
  title.remove(QLatin1Char('&'));
  d.setWindowTitle(title);
  auto *label = new QLabel(ToQ(cd.Static));
  auto *combo = new QComboBox;
  combo->setEditable(true);
  combo->setInsertPolicy(QComboBox::NoInsert);
  combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  FOR_VECTOR (i, cd.Strings)
    combo->addItem(ToQ(cd.Strings[i]));
  combo->setCurrentIndex(-1);
  combo->setEditText(ToQ(cd.Value));
  label->setBuddy(combo);
  auto *browse = new QPushButton(QStringLiteral("..."));
  browse->setFixedWidth(40);
  QObject::connect(browse, &QPushButton::clicked, &d, [&]() {
    const QString dir = QFileDialog::getExistingDirectory(&d, title, combo->currentText());
    if (!dir.isEmpty())
      combo->setEditText(dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/'));
  });
  auto *info = new QLabel(ToQ(cd.Info));
  info->setTextFormat(Qt::PlainText);
  info->setAlignment(Qt::AlignTop | Qt::AlignLeft);

  auto *row = new QHBoxLayout;
  row->addWidget(combo);
  row->addWidget(browse);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  auto *lay = new QVBoxLayout(&d);
  lay->addWidget(label);
  lay->addLayout(row);
  lay->addWidget(info, 1);
  lay->addWidget(buttons);
  d.resize(600, d.sizeHint().height());
  if (d.exec() != QDialog::Accepted)
    return false;
  cd.Value = ToU(combo->currentText().trimmed());
  return true;
}
