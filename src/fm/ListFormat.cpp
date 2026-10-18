// ListFormat.cpp -- list-cell text, copied from upstream
// FileManager/PanelListNotify.cpp (ConvertSizeToString, IsSizeProp and the
// text part of CPanel::SetItemText).

#include "ListFormat.h"

#include "Common/IntToString.h"
#include "Common/StringConvert.h"
#include "Windows/PropVariant.h"
#include "Windows/PropVariantConv.h"

#include "7zip/PropID.h"
#include "7zip/UI/Common/PropIDUtils.h"

using namespace NWindows;

#define INT_TO_STR_SPEC(v) \
  while (v >= 10) { temp[i++] = (Byte)('0' + (unsigned)(v % 10)); v /= 10; } \
  *s++ = (Byte)('0' + (unsigned)v);

static void ConvertSizeToString(UInt64 val, wchar_t *s) throw()
{
  Byte temp[32];
  unsigned i = 0;
  
  if (val <= (UInt32)0xFFFFFFFF)
  {
    UInt32 val32 = (UInt32)val;
    INT_TO_STR_SPEC(val32)
  }
  else
  {
    INT_TO_STR_SPEC(val)
  }

  if (i < 3)
  {
    if (i != 0)
    {
      *s++ = temp[(size_t)i - 1];
      if (i == 2)
        *s++ = temp[0];
    }
    *s = 0;
    return;
  }

  unsigned r = i % 3;
  if (r != 0)
  {
    s[0] = temp[--i];
    if (r == 2)
      s[1] = temp[--i];
    s += r;
  }

  do
  {
    s[0] = ' ';
    s[1] = temp[(size_t)i - 1];
    s[2] = temp[(size_t)i - 2];
    s[3] = temp[(size_t)i - 3];
    s += 4;
  }
  while (i -= 3);
  
  *s = 0;
}

UString ConvertSizeToString(UInt64 value);
UString ConvertSizeToString(UInt64 value)
{
  wchar_t s[32];
  ConvertSizeToString(value, s);
  return s;
}

bool IsSizeProp(UINT propID) throw();
bool IsSizeProp(UINT propID) throw()
{
  switch (propID)
  {
    case kpidSize:
    case kpidPackSize:
    case kpidNumSubDirs:
    case kpidNumSubFiles:
    case kpidOffset:
    case kpidLinks:
    case kpidNumBlocks:
    case kpidNumVolumes:
    case kpidPhySize:
    case kpidHeadersSize:
    case kpidTotalSize:
    case kpidFreeSpace:
    case kpidClusterSize:
    case kpidNumErrors:
    case kpidNumStreams:
    case kpidNumAltStreams:
    case kpidAltStreamsSize:
    case kpidVirtualSize:
    case kpidUnpackSize:
    case kpidTotalPhySize:
    case kpidTailSize:
    case kpidEmbeddedStubSize:
      return true;
  }
  return false;
}


UString GetItemText(IFolderFolder *folder, IFolderGetItemName *folderGetItemName,
    UInt32 realIndex, PROPID propID, int timestampLevel)
{
  if (propID == kpidName && folderGetItemName)
  {
    const wchar_t *name = NULL;
    unsigned nameLen = 0;
    folderGetItemName->GetItemName(realIndex, &name, &nameLen);
    if (name)
    {
      // Same clean-up as upstream: runs of 4+ spaces become "... ", and
      // U+202E (RLO, used to disguise extensions) becomes '_'.
      UString text;
      for (unsigned i = 0;;)
      {
        const wchar_t c = name[i++];
        if (c == 0)
          break;
        text += c;
        if (c != ' ')
        {
          if (c == 0x202E)
            text.ReplaceOneCharAtPos(text.Len() - 1, '_');
          continue;
        }
        if (name[i] != ' ')
          continue;
        unsigned t = 1;
        for (; name[i + t] == ' '; t++);
        if (t >= 4)
        {
          text += "... ";
          i += t;
        }
      }
      if (text.IsEmpty())
        text = '_';
      return text;
    }
  }

  if (propID == kpidPrefix && folderGetItemName)
  {
    const wchar_t *name = NULL;
    unsigned nameLen = 0;
    folderGetItemName->GetItemPrefix(realIndex, &name, &nameLen);
    if (name)
      return UString(name);
  }

  NCOM::CPropVariant prop;
  const HRESULT res = folder->GetProperty(realIndex, propID, &prop);
  if (res != S_OK)
    return UString("Error: ");
  if ((prop.vt == VT_UI8 || prop.vt == VT_UI4 || prop.vt == VT_UI2) && IsSizeProp(propID))
  {
    UInt64 v = 0;
    ConvertPropVariantToUInt64(prop, v);
    return ConvertSizeToString(v);
  }
  if (prop.vt == VT_BSTR)
  {
    UString s = prop.bstrVal;
    s.Replace((wchar_t)0xA, L' ');
    s.Replace((wchar_t)0xD, L' ');
    return s;
  }
  char temp[64];
  ConvertPropertyToShortString2(temp, prop, propID, timestampLevel);
  return UString(GetUnicodeString(temp));
}
