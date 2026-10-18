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
private:
  QMimeDatabase _db;
  QHash<QString, QIcon> _byExt;
  QIcon _folder;
  QIcon _file;
};

IconProvider &GetIconProvider();

#endif
