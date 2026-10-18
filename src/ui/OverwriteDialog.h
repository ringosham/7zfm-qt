// OverwriteDialog.h -- Qt port of upstream FileManager/OverwriteDialog.

#ifndef Z7QT_OVERWRITE_DIALOG_H
#define Z7QT_OVERWRITE_DIALOG_H

#include "UiHooks.h"

class QWidget;

EOverwriteAnswer RunOverwriteDialog(QWidget *parent, const COverwriteFileInfo &oldFile, const COverwriteFileInfo &newFile);
bool RunMemDialog(QWidget *parent, CMemUseQuestion &q);

#endif
