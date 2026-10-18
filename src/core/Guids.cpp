// Instantiates every COM interface ID once (the role upstream's
// Console/Main.cpp and FileManager/ClassDefs.cpp play in their builds).

#include "Common/Common.h"
#include "Common/MyInitGuid.h"

#include "7zip/ICoder.h"
#include "7zip/IPassword.h"
#include "7zip/IProgress.h"
#include "7zip/IStream.h"
#include "7zip/Archive/IArchive.h"
#include "7zip/UI/Common/IFileExtractCallback.h"
#include "7zip/UI/FileManager/IFolder.h"
#include "Agent.h"
#include "IFolderArchive.h"
