// LinkDialog.h -- File > Link... (upstream FileManager/LinkDialog). Upstream
// creates NTFS hard links, symbolic links, junctions and WSL links through
// reparse points; on Linux: hard links and symbolic links (relative or
// absolute).

#ifndef Z7QT_LINK_DIALOG_H
#define Z7QT_LINK_DIALOG_H

class QWidget;
class QString;

// (target): the focused item; (dir): the current folder. Returns true if a
// link was created.
bool RunLinkDialog(QWidget *parent, const QString &target, const QString &dir);

#endif
