// Resource strings. Upstream code built without Z7_LANG resolves
// LangString(id) to NWindows::MyLoadString(id); we answer from the English
// table that tools/gen-lang.py extracts from upstream's .rc files.

#include "compat/Win32Compat.h"

#include "Common/Common.h"
#include "Windows/ResourceString.h"

#include <algorithm>

namespace {
struct CLangPair { UInt32 Id; const wchar_t *Text; };
const CLangPair kTable[] =
{
#include "LangTable.inc"
};
}

namespace NWindows {

UString MyLoadString(UINT resourceID)
{
  const CLangPair *end = kTable + Z7_ARRAY_SIZE(kTable);
  const CLangPair *p = std::lower_bound(kTable, end, (UInt32)resourceID,
      [](const CLangPair &a, UInt32 id) { return a.Id < id; });
  if (p != end && p->Id == resourceID)
    return UString(p->Text);
  return UString();
}

void MyLoadString(UINT resourceID, UString &dest) { dest = MyLoadString(resourceID); }
void MyLoadString(HINSTANCE, UINT resourceID, UString &dest) { dest = MyLoadString(resourceID); }

}
