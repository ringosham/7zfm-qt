// PropertiesDialog.h -- the two-column list 7zFM shows for Alt+Enter
// (upstream CListViewDialog).

#ifndef Z7QT_PROPERTIES_DIALOG_H
#define Z7QT_PROPERTIES_DIALOG_H

#include <QString>

class QWidget;
struct CPropList;

void ShowPropertiesDialog(QWidget *parent, const QString &title, const CPropList &list);

#endif
