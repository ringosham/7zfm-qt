// SplitPanel.h -- the CPanel members upstream PanelSplitFile.cpp uses, as a
// Win32-free interface the Qt panel implements.

#ifndef Z7QT_SPLIT_PANEL_H
#define Z7QT_SPLIT_PANEL_H

#include "Common/MyString.h"
#include "Common/MyVector.h"

struct ISplitPanel
{
  virtual ~ISplitPanel() {}
  virtual bool Is_IO_FS_Folder() const = 0;
  virtual bool IsFSFolder() const = 0;
  virtual void Get_ItemIndices_Operated(CRecordVector<UInt32> &indices) const = 0;
  virtual bool IsItem_Folder(unsigned index) const = 0;
  virtual UString GetItemName(unsigned index) const = 0;
  virtual UString GetItemRelPath(unsigned index) const = 0;
  virtual UString GetFsPath() const = 0;
  virtual void MessageBox_Error(const UString &message) = 0;
};

void SplitFile(ISplitPanel &panel);     // upstream CApp::Split
void CombineFiles(ISplitPanel &panel);  // upstream CApp::Combine

#endif
