// PanelProperties.cpp -- the content of 7zFM's Properties window (Alt+Enter),
// copied from upstream FileManager/PanelMenu.cpp (CPanel::Properties and its
// helpers). Upstream fills a CListViewDialog; here we fill CPropList and the
// Qt dialog shows it. NtSecurity descriptors are shown as hex.

#include "PanelProperties.h"

#include "Common/IntToString.h"
#include "Common/MyCom.h"
#include "Common/StringConvert.h"
#include "Windows/PropVariant.h"
#include "Windows/PropVariantConv.h"

#include "7zip/PropID.h"
#include "7zip/Archive/IArchive.h"
#include "7zip/UI/Common/OpenArchive.h"
#include "7zip/UI/Common/PropIDUtils.h"
#include "7zip/UI/FileManager/FormatUtils.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/PropertyName.h"
#include "7zip/UI/FileManager/resource.h"

#include "ListFormat.h"
#include "Messages.h"

using namespace NWindows;

static UInt64 GetItem_UInt64Prop(IFolderFolder *folder, UInt32 index, PROPID propID)
{
  NCOM::CPropVariant prop;
  if (folder->GetProperty(index, propID, &prop) != S_OK)
    return 0;
  UInt64 val = 0;
  if (ConvertPropVariantToUInt64(prop, val))
    return val;
  return 0;
}

static bool IsItem_Folder(IFolderFolder *folder, UInt32 index)
{
  NCOM::CPropVariant prop;
  if (folder->GetProperty(index, kpidIsDir, &prop) != S_OK)
    return false;
  return prop.vt == VT_BOOL && prop.boolVal != VARIANT_FALSE;
}

static const char * const kSeparator = "------------------------";
static const char * const kSeparatorSmall = "----------------";



static void AddListAscii(CPropList &dialog, const char *s)
{
  dialog.Strings.Add((UString)s);
  dialog.Values.AddNew();
}

static void AddSeparator(CPropList &dialog)
{
  AddListAscii(dialog, kSeparator);
}

static void AddSeparatorSmall(CPropList &dialog)
{
  AddListAscii(dialog, kSeparatorSmall);
}

static void AddPropertyPair(const UString &name, const UString &val, CPropList &dialog)
{
  dialog.Strings.Add(name);
  dialog.Values.Add(val);
}


static void AddPropertyString(PROPID propID, const wchar_t *nameBSTR,
    const NCOM::CPropVariant &prop, CPropList &dialog)
{
  if (prop.vt != VT_EMPTY)
  {
    UString val;

    if (propID == kpidErrorFlags ||
        propID == kpidWarningFlags)
    {
      UInt32 flags = GetOpenArcErrorFlags(prop);
      if (flags == 0)
        return;
      if (flags != 0)
        val = GetOpenArcErrorMessage(flags);
    }

    if (val.IsEmpty())
    {
      if ((prop.vt == VT_UI8 || prop.vt == VT_UI4 || prop.vt == VT_UI2) && IsSizeProp(propID))
      {
        UInt64 v = 0;
        ConvertPropVariantToUInt64(prop, v);
        val = ConvertSizeToString(v);
      }
      else
        ConvertPropertyToString2(val, prop, propID, 9); // we send 9 - is ns precision
    }

    if (!val.IsEmpty())
    {
      if (propID == kpidErrorType)
      {
        AddPropertyPair(L"Open WARNING:", L"Cannot open the file as expected archive type", dialog);
      }
      AddPropertyPair(GetNameOfProperty(propID, nameBSTR), val, dialog);
    }
  }
}


static void AddPropertyString(PROPID propID, UInt64 val, CPropList &dialog)
{
  NCOM::CPropVariant prop = val;
  AddPropertyString(propID, NULL, prop, dialog);
}


static const Byte kSpecProps[] =
{
  kpidPath,
  kpidType,
  kpidErrorType,
  kpidError,
  kpidErrorFlags,
  kpidWarning,
  kpidWarningFlags,
  kpidOffset,
  kpidPhySize,
  kpidTailSize
};


void GetFolderProperties(IFolderFolder *_folderPtr, const CRecordVector<UInt32> &operatedIndices, CPropList &list)
{
  CMyComPtr<IFolderFolder> _folder = _folderPtr;

  CMyComPtr<IGetFolderArcProps> getFolderArcProps;
  _folder.QueryInterface(IID_IGetFolderArcProps, &getFolderArcProps);
  CMyComPtr<IArchiveGetRawProps> _folderRawProps;
  _folder.QueryInterface(IID_IArchiveGetRawProps, &_folderRawProps);
  {
    CPropList &message = list;
    
    if (operatedIndices.Size() == 1)
    {
      UInt32 index = operatedIndices[0];
      // message += "Item:\n");
      UInt32 numProps;
      if (_folder->GetNumberOfProperties(&numProps) == S_OK)
      {
        for (UInt32 i = 0; i < numProps; i++)
        {
          CMyComBSTR name;
          PROPID propID;
          VARTYPE varType;
          
          if (_folder->GetPropertyInfo(i, &name, &propID, &varType) != S_OK)
            continue;
          
          NCOM::CPropVariant prop;
          if (_folder->GetProperty(index, propID, &prop) != S_OK)
            continue;
          AddPropertyString(propID, name, prop, message);
        }
      }


      if (_folderRawProps)
      {
        _folderRawProps->GetNumRawProps(&numProps);
        for (UInt32 i = 0; i < numProps; i++)
        {
          CMyComBSTR name;
          PROPID propID;
          if (_folderRawProps->GetRawPropInfo(i, &name, &propID) != S_OK)
            continue;

          const void *data;
          UInt32 dataSize;
          UInt32 propType;
          if (_folderRawProps->GetRawProp(index, propID, &data, &dataSize, &propType) != S_OK)
            continue;

          if (dataSize != 0)
          {
            AString s;
            if (propID == kpidNtSecure)
              ConvertNtSecureToString((const Byte *)data, dataSize, s);
            else
            {
              const unsigned kMaxDataSize = 1 << 8;
              if (dataSize > kMaxDataSize)
              {
                s += "data:";
                s.Add_UInt32(dataSize);
              }
              else
              {
                char temp[kMaxDataSize * 2 + 2];
                if (dataSize <= 8 && (propID == kpidCRC || propID == kpidChecksum))
                  ConvertDataToHex_Upper(temp, (const Byte *)data, dataSize);
                else
                  ConvertDataToHex_Lower(temp, (const Byte *)data, dataSize);
                s += temp;
              }
            }
            AddPropertyPair(GetNameOfProperty(propID, name), (UString)s.Ptr(), message);
          }
        }
      }

      AddSeparator(message);
    }
    else if (operatedIndices.Size() >= 1)
    {
      UInt64 packSize = 0;
      UInt64 unpackSize = 0;
      UInt64 numFiles = 0;
      UInt64 numDirs = 0;

      FOR_VECTOR (i, operatedIndices)
      {
        const UInt32 index = operatedIndices[i];
        unpackSize += GetItem_UInt64Prop(_folder, index, kpidSize);
        packSize += GetItem_UInt64Prop(_folder, index, kpidPackSize);
        if (IsItem_Folder(_folder, index))
        {
          numDirs++;
          numDirs += GetItem_UInt64Prop(_folder, index, kpidNumSubDirs);
          numFiles += GetItem_UInt64Prop(_folder, index, kpidNumSubFiles);
        }
        else
          numFiles++;
      }
      {
        wchar_t temp[32];
        ConvertUInt32ToString(operatedIndices.Size(), temp);
        AddPropertyPair(L"", MyFormatNew(LangString(IDS_N_SELECTED_ITEMS), temp), message);
      }

      if (numDirs != 0)
        AddPropertyString(kpidNumSubDirs, numDirs, message);
      if (numFiles != 0)
        AddPropertyString(kpidNumSubFiles, numFiles, message);
      AddPropertyString(kpidSize, unpackSize, message);
      AddPropertyString(kpidPackSize, packSize, message);

      AddSeparator(message);
    }

        
    /*
    AddLangString(message, IDS_PROP_FILE_TYPE);
    message += kPropValueSeparator;
    message += GetFolderTypeID();
    message.Add_LF();
    */

    {
      NCOM::CPropVariant prop;
      if (_folder->GetFolderProperty(kpidPath, &prop) == S_OK)
      {
        AddPropertyString(kpidName, L"Path", prop, message);
      }
    }

    CMyComPtr<IFolderProperties> folderProperties;
    _folder.QueryInterface(IID_IFolderProperties, &folderProperties);
    if (folderProperties)
    {
      UInt32 numProps;
      if (folderProperties->GetNumberOfFolderProperties(&numProps) == S_OK)
      {
        for (UInt32 i = 0; i < numProps; i++)
        {
          CMyComBSTR name;
          PROPID propID;
          VARTYPE vt;
          if (folderProperties->GetFolderPropertyInfo(i, &name, &propID, &vt) != S_OK)
            continue;
          NCOM::CPropVariant prop;
          if (_folder->GetFolderProperty(propID, &prop) != S_OK)
            continue;
          AddPropertyString(propID, name, prop, message);
        }
      }
    }

    if (getFolderArcProps)
    {
      CMyComPtr<IFolderArcProps> getProps;
      getFolderArcProps->GetFolderArcProps(&getProps);
      if (getProps)
      {
        UInt32 numLevels;
        if (getProps->GetArcNumLevels(&numLevels) != S_OK)
          numLevels = 0;
        for (UInt32 level2 = 0; level2 < numLevels; level2++)
        {
          {
            UInt32 level = numLevels - 1 - level2;
            UInt32 numProps;
            if (getProps->GetArcNumProps(level, &numProps) == S_OK)
            {
              const int kNumSpecProps = Z7_ARRAY_SIZE(kSpecProps);

              AddSeparator(message);
              
              for (Int32 i = -(int)kNumSpecProps; i < (Int32)numProps; i++)
              {
                CMyComBSTR name;
                PROPID propID;
                VARTYPE vt;
                if (i < 0)
                  propID = kSpecProps[i + kNumSpecProps];
                else if (getProps->GetArcPropInfo(level, (UInt32)i, &name, &propID, &vt) != S_OK)
                  continue;
                NCOM::CPropVariant prop;
                if (getProps->GetArcProp(level, propID, &prop) != S_OK)
                  continue;
                AddPropertyString(propID, name, prop, message);
              }
            }
          }
          
          if (level2 < numLevels - 1)
          {
            const UInt32 level = numLevels - 1 - level2;
            UInt32 numProps;
            if (getProps->GetArcNumProps2(level, &numProps) == S_OK)
            {
              AddSeparatorSmall(message);
              for (UInt32 i = 0; i < numProps; i++)
              {
                CMyComBSTR name;
                PROPID propID;
                VARTYPE vt;
                if (getProps->GetArcPropInfo2(level, i, &name, &propID, &vt) != S_OK)
                  continue;
                NCOM::CPropVariant prop;
                if (getProps->GetArcProp2(level, propID, &prop) != S_OK)
                  continue;
                AddPropertyString(propID, name, prop, message);
              }
            }
          }
        }
        
        {
          // we ERROR message for NonOpen level
              bool needSep = true;
              const int kNumSpecProps = Z7_ARRAY_SIZE(kSpecProps);
              for (Int32 i = -(int)kNumSpecProps; i < 0; i++)
              {
                CMyComBSTR name;
                const PROPID propID = kSpecProps[i + kNumSpecProps];
                NCOM::CPropVariant prop;
                if (getProps->GetArcProp(numLevels, propID, &prop) != S_OK)
                  continue;
                if (needSep)
                {
                  AddSeparator(message);
                  AddSeparator(message);
                  needSep = false;
                }
                AddPropertyString(propID, name, prop, message);
              }
        }

      }
    }

  }
}
