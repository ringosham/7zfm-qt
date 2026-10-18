// CopyDialog.h -- Qt port of upstream FileManager/CopyDialog.

#ifndef Z7QT_COPY_DIALOG_H
#define Z7QT_COPY_DIALOG_H

#include "Common/MyString.h"

class QWidget;

struct CCopyDialog
{
  UStringVector Strings;  // history
  UString Value;
  UString Title;
  UString Static;
  UString Info;
};

bool RunCopyDialog(QWidget *parent, CCopyDialog &d);

#endif
