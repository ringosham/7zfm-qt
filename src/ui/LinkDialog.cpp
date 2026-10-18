// LinkDialog.cpp

#include "LinkDialog.h"

#include "Common/Common.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/LinkDialogRes.h"

#include "QtConv.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include <errno.h>
#include <string.h>
#include <unistd.h>

static QString Lq(UInt32 id, const char *fallback)
{
  const UString s = LangString(id);
  return s.IsEmpty() ? QString::fromLatin1(fallback) : ToQ(s);
}

bool RunLinkDialog(QWidget *parent, const QString &target, const QString &dir)
{
  QDialog d(parent ? parent->window() : nullptr);
  d.setWindowTitle(Lq(IDD_LINK, "Link"));

  // upstream: "Link from" is the new link, "Link to" the existing target
  auto *from = new QLineEdit(QDir(dir).filePath(QFileInfo(target).fileName() + QStringLiteral(".link")));
  auto *to = new QLineEdit(target);
  auto browse = [&d](QLineEdit *e, bool save) {
    const QString f = save
        ? QFileDialog::getSaveFileName(&d, QString(), e->text(), QString(), nullptr, QFileDialog::DontConfirmOverwrite)
        : QFileDialog::getOpenFileName(&d, QString(), e->text());
    if (!f.isEmpty())
      e->setText(f);
  };
  auto *fromB = new QPushButton(QStringLiteral("..."));
  auto *toB = new QPushButton(QStringLiteral("..."));
  fromB->setFixedWidth(40);
  toB->setFixedWidth(40);
  QObject::connect(fromB, &QPushButton::clicked, &d, [&]() { browse(from, true); });
  QObject::connect(toB, &QPushButton::clicked, &d, [&]() { browse(to, false); });

  auto *type = new QGroupBox(Lq(IDG_LINK_TYPE, "Link Type"));
  auto *hard = new QRadioButton(Lq(IDR_LINK_TYPE_HARD, "Hard Link"));
  auto *sym = new QRadioButton(QObject::tr("Symbolic Link"));
  auto *relative = new QCheckBox(QObject::tr("Relative path"));
  relative->setChecked(true);
  auto *group = new QButtonGroup(&d);
  group->addButton(hard);
  group->addButton(sym);
  const bool isDir = QFileInfo(target).isDir();
  hard->setEnabled(!isDir);
  (isDir ? sym : hard)->setChecked(true);
  QObject::connect(sym, &QRadioButton::toggled, relative, &QWidget::setEnabled);
  relative->setEnabled(sym->isChecked());
  auto *tl = new QVBoxLayout(type);
  tl->addWidget(hard);
  tl->addWidget(sym);
  tl->addWidget(relative);

  auto *lay = new QVBoxLayout(&d);
  lay->addWidget(new QLabel(Lq(IDT_LINK_PATH_FROM, "Link from:")));
  auto *r1 = new QHBoxLayout;
  r1->addWidget(from);
  r1->addWidget(fromB);
  lay->addLayout(r1);
  lay->addWidget(new QLabel(Lq(IDT_LINK_PATH_TO, "Link to:")));
  auto *r2 = new QHBoxLayout;
  r2->addWidget(to);
  r2->addWidget(toB);
  lay->addLayout(r2);
  lay->addWidget(type);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
  buttons->addButton(Lq(IDB_LINK_LINK, "Link"), QDialogButtonBox::AcceptRole);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  lay->addWidget(buttons);
  d.resize(560, d.sizeHint().height());

  for (;;)
  {
    if (d.exec() != QDialog::Accepted)
      return false;
    const QString linkPath = QFileInfo(from->text()).isRelative() ? QDir(dir).filePath(from->text()) : from->text();
    QString targetPath = QFileInfo(to->text()).isRelative() ? QDir(dir).filePath(to->text()) : to->text();
    int res;
    if (hard->isChecked())
      res = link(QFile::encodeName(targetPath).constData(), QFile::encodeName(linkPath).constData());
    else
    {
      if (relative->isChecked())
        targetPath = QFileInfo(linkPath).absoluteDir().relativeFilePath(targetPath);
      res = symlink(QFile::encodeName(targetPath).constData(), QFile::encodeName(linkPath).constData());
    }
    if (res == 0)
      return true;
    QMessageBox::critical(&d, QStringLiteral("7-Zip"), QString::fromLocal8Bit(strerror(errno)) + QLatin1Char('\n') + linkPath);
  }
}
