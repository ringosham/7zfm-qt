// IconProvider.cpp

#include "IconProvider.h"

#include <QApplication>
#include <QStyle>

IconProvider &GetIconProvider()
{
  static IconProvider p;
  return p;
}

QIcon IconProvider::Folder()
{
  if (_folder.isNull())
    _folder = QIcon::fromTheme(QStringLiteral("folder"), qApp->style()->standardIcon(QStyle::SP_DirIcon));
  return _folder;
}

QIcon IconProvider::ForDevice()
{
  return QIcon::fromTheme(QStringLiteral("drive-harddisk"), qApp->style()->standardIcon(QStyle::SP_DriveHDIcon));
}

QIcon IconProvider::ForFile(const QString &name)
{
  const int dot = name.lastIndexOf(QLatin1Char('.'));
  const QString ext = dot > 0 ? name.mid(dot + 1).toLower() : QString();
  auto it = _byExt.constFind(ext);
  if (it != _byExt.constEnd())
    return *it;
  QIcon icon;
  if (!ext.isEmpty())
  {
    const QMimeType mt = _db.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    if (mt.isValid() && !mt.isDefault())
    {
      icon = QIcon::fromTheme(mt.iconName());
      if (icon.isNull())
        icon = QIcon::fromTheme(mt.genericIconName());
    }
  }
  if (icon.isNull())
  {
    if (_file.isNull())
      _file = QIcon::fromTheme(QStringLiteral("text-x-generic"), qApp->style()->standardIcon(QStyle::SP_FileIcon));
    icon = _file;
  }
  _byExt.insert(ext, icon);
  return icon;
}
