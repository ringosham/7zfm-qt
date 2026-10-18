// Seams for upstream Win32 GUI helpers that the compiled dialogs call
// (FileManager/BrowseDialog, Explorer/MyMessages, FileManager/HelpUtils).

#include <QApplication>
#include <QDesktopServices>
#include <QFileDialog>
#include <QMessageBox>
#include <QUrl>

#include "Q32.h"

#include "Common/StringConvert.h"
#include "Windows/ErrorMsg.h"
#include "Windows/FileIO.h"
#include "7zip/UI/FileManager/LangUtils.h"

static QString FromU(const UString &s) { return QString::fromWCharArray(s.Ptr(), (qsizetype)s.Len()); }

static UString ToU2(const QString &s)
{
  UString u;
  const std::wstring w = s.toStdWString();
  u = w.c_str();
  return u;
}

// FileManager/BrowseDialog.h
bool MyBrowseForFolder(HWND owner, LPCWSTR title, LPCWSTR path, UString &resultPath)
{
  const QString dir = QFileDialog::getExistingDirectory(static_cast<QWidget *>(owner),
      QString::fromWCharArray(title), QString::fromWCharArray(path));
  if (dir.isEmpty())
    return false;
  resultPath = ToU2(dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/'));
  return true;
}

bool MyBrowseForFile(HWND owner, LPCWSTR title, LPCWSTR path, LPCWSTR /* filterDescription */,
    LPCWSTR /* filter */, UString &resultPath);
bool MyBrowseForFile(HWND owner, LPCWSTR title, LPCWSTR path, LPCWSTR, LPCWSTR, UString &resultPath)
{
  const QString file = QFileDialog::getSaveFileName(static_cast<QWidget *>(owner),
      QString::fromWCharArray(title), QString::fromWCharArray(path), QString(), nullptr,
      QFileDialog::DontConfirmOverwrite);
  if (file.isEmpty())
    return false;
  resultPath = ToU2(file);
  return true;
}

// FileManager/BrowseDialog.h: file "Save as" browser of the Add dialog
#include "7zip/UI/FileManager/BrowseDialog.h"

bool CBrowseInfo::BrowseForFile(const CObjectVector<CBrowseFilterInfo> &filters)
{
  QStringList nameFilters;
  FOR_VECTOR (i, filters)
  {
    const CBrowseFilterInfo &f = filters[i];
    QStringList masks;
    FOR_VECTOR (k, f.Masks)
      masks << FromU(f.Masks[k]);
    nameFilters << FromU(f.Description) + QLatin1String(" (") + masks.join(QLatin1Char(' ')) + QLatin1Char(')');
  }
  QFileDialog dlg(static_cast<QWidget *>(hwndOwner), lpstrTitle ? QString::fromWCharArray(lpstrTitle) : QString());
  dlg.setAcceptMode(SaveMode ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
  dlg.setFileMode(SaveMode ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
  dlg.setOption(QFileDialog::DontConfirmOverwrite, true);
  dlg.setNameFilters(nameFilters);
  if (FilterIndex >= 0 && FilterIndex < nameFilters.size())
    dlg.selectNameFilter(nameFilters[FilterIndex]);
  if (!FilePath.IsEmpty())
    dlg.selectFile(FromU(FilePath));
  if (dlg.exec() != QDialog::Accepted || dlg.selectedFiles().isEmpty())
    return false;
  FilePath = ToU2(dlg.selectedFiles().first());
  FilterIndex = nameFilters.indexOf(dlg.selectedNameFilter());
  return true;
}

// From upstream GUI/ExtractDialog.cpp (not linked: the extract dialog is a
// Qt port), used by CompressDialog.cpp.
#include "Windows/Control/ComboBox.h"

void AddComboItems(NWindows::NControl::CComboBox &combo, const UInt32 *langIDs, unsigned numItems, const int *values, int curVal);
void AddComboItems(NWindows::NControl::CComboBox &combo, const UInt32 *langIDs, unsigned numItems, const int *values, int curVal)
{
  unsigned curSel = 0;
  for (unsigned i = 0; i < numItems; i++)
  {
    UString s = LangString(langIDs[i]);
    s.RemoveChar(L'&');
    combo.AddString_SetItemData(s, (LPARAM)i);
    if (values[i] == curVal)
      curSel = i;
  }
  combo.SetCurSel(curSel);
}

void AddUniqueString(UStringVector &list, const UString &s);
void AddUniqueString(UStringVector &list, const UString &s)
{
  FOR_VECTOR (i, list)
    if (s.IsEqualTo_NoCase(list[i]))
      return;
  list.Add(s);
}

// Explorer/MyMessages.h
void ShowErrorMessage(HWND window, LPCWSTR message)
{
  MessageBoxW(window, message, L"7-Zip", MB_OK | MB_ICONSTOP);
}

void ShowErrorMessageHwndRes(HWND window, UInt32 langID)
{
  ShowErrorMessage(window, LangString(langID));
}

void ShowErrorMessageRes(UInt32 langID)
{
  ShowErrorMessageHwndRes(NULL, langID);
}

void ShowLastErrorMessage(HWND window)
{
  ShowErrorMessage(window, NWindows::NError::MyFormatMessage(GetLastError_noZero_HRESULT()));
}

void LangString_OnlyFromLangFile(UInt32, UString &dest) { dest.Empty(); }

// FileManager/HelpUtils.h: upstream opens the CHM help file
void ShowHelpWindow(LPCSTR topicFile)
{
  QDesktopServices::openUrl(QUrl(QStringLiteral("https://7-zip.opensource.jp/chm/") + QString::fromUtf8(topicFile)));
}
