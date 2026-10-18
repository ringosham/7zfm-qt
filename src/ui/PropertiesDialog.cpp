// PropertiesDialog.cpp

#include "PropertiesDialog.h"

#include "PanelProperties.h"
#include "QtConv.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

void ShowPropertiesDialog(QWidget *parent, const QString &title, const CPropList &list)
{
  QDialog d(parent ? parent->window() : nullptr);
  d.setWindowTitle(title);
  auto *tree = new QTreeWidget;
  tree->setColumnCount(2);
  tree->setHeaderHidden(true);
  tree->setRootIsDecorated(false);
  tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree->setAlternatingRowColors(true);
  QStringList text;
  for (unsigned i = 0; i < list.Strings.Size(); i++)
  {
    const QString name = ToQ(list.Strings[i]);
    const QString value = i < list.Values.Size() ? ToQ(list.Values[i]) : QString();
    auto *item = new QTreeWidgetItem(tree, QStringList() << name << value);
    if (value.isEmpty() && name.startsWith(QLatin1Char('-')))
    {
      item->setFlags(Qt::NoItemFlags);
      item->setText(0, QString());
      item->setFirstColumnSpanned(true);
    }
    item->setToolTip(1, value);
    text << (value.isEmpty() ? name : name + QLatin1String(": ") + value);
  }
  tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  tree->header()->setStretchLastSection(true);
  tree->setMinimumSize(560, 420);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  QPushButton *copy = buttons->addButton(QObject::tr("Copy"), QDialogButtonBox::ActionRole);
  QObject::connect(copy, &QPushButton::clicked, &d, [text]() {
    QApplication::clipboard()->setText(text.join(QLatin1Char('\n')));
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  auto *lay = new QVBoxLayout(&d);
  lay->addWidget(tree);
  lay->addWidget(buttons);
  d.resize(640, 520);
  d.exec();
}
