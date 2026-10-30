// IconProvider.h -- desktop-theme icons by file name, cached per extension
// (upstream uses the Windows system image list).

#ifndef Z7QT_ICON_PROVIDER_H
#define Z7QT_ICON_PROVIDER_H

#include <QHash>
#include <QIcon>
#include <QMimeDatabase>

class IconProvider
{
public:
  QIcon Folder();
  QIcon ForFile(const QString &name);
  QIcon ForDevice();
  // base with the theme's link emblem in the bottom-right corner, as Dolphin
  // draws symbolic links (upstream gets the shortcut arrow from the shell)
  QIcon WithLinkEmblem(const QIcon &base);
private:
  QMimeDatabase _db;
  QHash<QString, QIcon> _byExt;
  QIcon _folder;
  QIcon _file;
  QIcon _linkEmblem;
  QHash<qint64, QIcon> _linked;  // base cacheKey -> icon with the emblem
};

IconProvider &GetIconProvider();

#endif
