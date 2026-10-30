// IconProvider.cpp

#include "IconProvider.h"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QStyle>

namespace {

// paints the emblem over the base at every size, so HiDPI and the list's
// icon-size setting both get crisp overlays
class EmblemIconEngine : public QIconEngine
{
  QIcon _base, _emblem;
public:
  EmblemIconEngine(const QIcon &base, const QIcon &emblem): _base(base), _emblem(emblem) {}
  QIconEngine *clone() const override { return new EmblemIconEngine(_base, _emblem); }
  QSize actualSize(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    { return _base.actualSize(size, mode, state); }
  void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
  {
    _base.paint(painter, rect, Qt::AlignCenter, mode, state);
    // KIconUtils::addOverlay: a quarter of the icon below 32 px, a third above
    const int side = rect.width() < 32 ? rect.width() / 2 : rect.width() / 3;
    const QRect er(rect.right() + 1 - side, rect.bottom() + 1 - side, side, side);
    _emblem.paint(painter, er, Qt::AlignCenter, mode, state);
  }
  QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    { return scaledPixmap(size, mode, state, 1.0); }
  QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
  {
    QPixmap pm(size * scale);
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(scale);
    QPainter p(&pm);
    paint(&p, QRect(QPoint(0, 0), size), mode, state);
    return pm;
  }
};

}

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

QIcon IconProvider::WithLinkEmblem(const QIcon &base)
{
  if (_linkEmblem.isNull())
  {
    _linkEmblem = QIcon::fromTheme(QStringLiteral("emblem-symbolic-link"));
    if (_linkEmblem.isNull())
      _linkEmblem = qApp->style()->standardIcon(QStyle::SP_FileLinkIcon);
  }
  auto it = _linked.constFind(base.cacheKey());
  if (it != _linked.constEnd())
    return *it;
  const QIcon icon(new EmblemIconEngine(base, _linkEmblem));
  _linked.insert(base.cacheKey(), icon);
  return icon;
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
