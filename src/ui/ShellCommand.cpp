// ShellCommand.cpp -- the commands of upstream's Explorer context menu
// (Explorer/ContextMenu.cpp: CZipContextMenu::QueryContextMenu builds each
// command's folder and archive name, InvokeCommandCommon runs it), for the
// file managers' menu entries. Upstream's menu starts 7zG.exe for these;
// here they run in this process, with only their own dialogs on screen.

#include "Common/MyException.h"
#include "Common/StringConvert.h"

#include "Windows/FileFind.h"

#include "7zip/UI/Common/ArchiveName.h"
#include "7zip/UI/Common/EnumDirItems.h"
#include "7zip/UI/Common/ZipRegistry.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/resource.h"

#include "CompressCall.h"
#include "HashGUI.h"
#include "Messages.h"
#include "QtConv.h"
#include "ShellCommand.h"

#include <QFileInfo>

#include <cstdio>

UString GetSubFolderNameForExtract(const UString &arcName);  // PanelCopy.cpp

static const char * const kCommands[] =
{
  "--extract", "--extract-here", "--extract-to", "--test",
  "--add", "--add-7z", "--add-zip", "--hash", "--hash-file"
};

bool IsShellCommand(const QString &arg)
{
  for (const char *c : kCommands)
    if (arg == QLatin1String(c))
      return true;
  return false;
}

static void ShowError(const UString &message)
{
  if (g_UiHooks)
    g_UiHooks->ShowError(message);
}

// upstream CalcChecksum (UI/Common/CompressCall2.cpp), without the
// (arcFileName) case: --hash-file goes through CompressFiles() as there
static void CalcChecksum(const UStringVector &paths, const UString &methodName)
{
  try
  {
    NWildcard::CCensor censor;
    FOR_VECTOR (i, paths)
      censor.AddPreItem_NoWildcard(paths[i]);
    censor.AddPathsToCensor(NWildcard::k_RelatPath);
    CHashOptions options;
    options.Methods.Add(methodName);
    bool messageWasDisplayed = false;
    const HRESULT res = HashCalcGUI(censor, options, messageWasDisplayed);
    if (res != S_OK && res != E_ABORT && !messageWasDisplayed)
      ShowError(HResultToMessage(res));
  }
  catch (const CSystemException &e) { ShowError(HResultToMessage(e.ErrorCode)); }
  catch (const UString &s) { ShowError(s); }
  catch (...) { ShowError(UString("Error")); }
}

bool RunShellCommand(const QStringList &args, int &exitCode)
{
  if (args.isEmpty() || !IsShellCommand(args[0]))
    return false;
  const QString cmd = args[0];
  exitCode = 2;

  int first = 1;
  UString method;
  if (cmd == QLatin1String("--hash"))
  {
    if (args.size() < 2)
    {
      fprintf(stderr, "7zfm-qt: --hash needs a method\n");
      return true;
    }
    method = ToU(args[1]);
    first = 2;
  }
  if (args.size() <= first)
  {
    fprintf(stderr, "7zfm-qt: %s needs files\n", qPrintable(cmd));
    return true;
  }

  // file managers pass absolute paths; keep symbolic links as they are
  UStringVector paths;
  bool haveDirs = false;
  for (int i = first; i < args.size(); i++)
  {
    const QFileInfo fi(args[i]);
    if (!fi.exists() && !fi.isSymLink())
    {
      ShowError(ToU(QStringLiteral("Cannot find file: ") + args[i]));
      return true;
    }
    if (fi.isDir())
      haveDirs = true;
    paths.Add(ToU(fi.absoluteFilePath()));
  }

  NWindows::NFile::NFind::CFileInfo fi0;
  if (!fi0.Find(us2fs(paths[0])))
  {
    ShowError(UString("Cannot find file: ") + paths[0]);
    return true;
  }
  UString folderPrefix = paths[0];
  folderPrefix.DeleteFrom((unsigned)(folderPrefix.ReverseFind_PathSepar() + 1));

  exitCode = 0;

  if (cmd == QLatin1String("--extract") || cmd == QLatin1String("--extract-here")
      || cmd == QLatin1String("--extract-to"))
  {
    if (haveDirs)
    {
      ShowError(LangString(IDS_SELECT_FILES));
      exitCode = 2;
      return true;
    }
    UString specFolder ('*');
    if (paths.Size() == 1)
      specFolder = GetSubFolderNameForExtract(fs2us(fi0.Name));
    specFolder.Add_PathSepar();
    CContextMenuInfo ci;
    ci.Load();
    const bool here = (cmd == QLatin1String("--extract-here"));
    const bool to = (cmd == QLatin1String("--extract-to"));
    ExtractArchives(paths, here ? folderPrefix : folderPrefix + specFolder,
        !here && !to,           // showDialog: only "Extract files..."
        to && ci.ElimDup.Val,   // elimDup
        ci.WriteZone);
  }
  else if (cmd == QLatin1String("--test"))
    TestArchives(paths);
  else if (cmd == QLatin1String("--add") || cmd == QLatin1String("--add-7z")
      || cmd == QLatin1String("--add-zip"))
  {
    UString arcName_base;
    UString arcName = CreateArchiveName(paths,
        false, // isHash
        paths.Size() == 1 ? &fi0 : NULL,
        arcName_base);
    UString arcType;
    if (cmd != QLatin1String("--add"))
    {
      arcType = (cmd == QLatin1String("--add-7z")) ? "7z" : "zip";
      arcName.Add_Dot();
      arcName += arcType;
    }
    const bool showDialog = arcType.IsEmpty();
    if (CompressFiles(folderPrefix, arcName, arcType,
        showDialog, // addExtension
        paths,
        false, // email
        showDialog,
        true) != S_OK)
      exitCode = 1;
  }
  else if (cmd == QLatin1String("--hash"))
    CalcChecksum(paths, method);
  else if (cmd == QLatin1String("--hash-file"))
  {
    UString arcName_base;
    UString name = CreateArchiveName(paths,
        true, // isHash
        paths.Size() == 1 ? &fi0 : NULL,
        arcName_base);
    name += ".sha256";
    if (CompressFiles(folderPrefix, name, UString("hash"),
        false, // addExtension
        paths,
        false, // email
        false, // showDialog
        true) != S_OK)
      exitCode = 1;
  }
  return true;
}
