// FolderModel.h -- table model over an IFolderFolder, the Qt counterpart of
// 7zFM's virtual list view: cells are read from the folder on demand.

#ifndef Z7QT_FOLDER_MODEL_H
#define Z7QT_FOLDER_MODEL_H

#include "Common/Common.h"
#include "Common/MyCom.h"
#include "7zip/UI/FileManager/IFolder.h"

#include "ItemSort.h"

#include <QAbstractTableModel>
#include <QVector>

class FolderModel : public QAbstractTableModel
{
  Q_OBJECT
public:
  struct CColumn
  {
    PROPID ID;
    QString Name;
    bool IsRaw;
  };

  explicit FolderModel(QObject *parent = nullptr);

  // (folder) must have LoadItems() done. Keeps the current sort column if
  // the new folder has it.
  void SetFolder(IFolderFolder *folder);
  void Reload();   // after folder->LoadItems()
  void Clear();

  int rowCount(const QModelIndex &parent = QModelIndex()) const override;
  int columnCount(const QModelIndex &parent = QModelIndex()) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
  Qt::ItemFlags flags(const QModelIndex &index) const override;

  // 7zFM ordering: folders first, then by column; same click toggles.
  void SortByColumn(int column);
  void SortByPropID(PROPID propID, bool ascending);
  int SortColumn() const;
  bool SortAscending() const { return _sort.Ascending; }
  PROPID SortPropID() const { return _sort.SortID; }

  UInt32 RealIndex(int row) const { return _rows[row]; }
  int RowOfRealIndex(UInt32 realIndex) const;
  int RowOfName(const UString &name) const;
  UString ItemName(int row) const { return _sort.GetItemName(_rows[row]); }
  bool IsFolder(int row) const { return _sort.IsItem_Folder(_rows[row]); }
  UInt64 ItemSize(int row) const;
  const QVector<CColumn> &Columns() const { return _columns; }
  int ColumnOfPropID(PROPID id) const;

  enum { kIsFolderRole = Qt::UserRole + 1, kRealIndexRole };

private:
  void LoadColumns();
  void Sort();

  CMyComPtr<IFolderFolder> _folder;
  CSortContext _sort;
  QVector<CColumn> _columns;
  QVector<UInt32> _rows;
  bool _isDevicesFolder = false;
};

#endif
