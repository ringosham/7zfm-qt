// Q32.h -- a small user32 on top of Qt.
//
// Force-included into the upstream Win32 GUI sources we compile unchanged
// (Windows/Window.cpp, Windows/Control/Dialog.cpp, GUI/CompressDialog.cpp,
// ...). It declares the Win32 types, constants and functions those use;
// Q32.cpp implements them with Qt widgets. HWND is a QWidget*. Dialog
// layouts come from upstream's .rc files (tools/gen-dialogs.py).
//
// Only what upstream 7-Zip's dialogs use is provided.

#ifndef Z7QT_Q32_H
#define Z7QT_Q32_H

#include "compat/Win32Compat.h"

#include "Common/MyWindows.h"

#include <stddef.h>
#include <stdint.h>

#ifndef _WIN32

// INT_PTR, UINT_PTR, LONG_PTR, DWORD_PTR come from C/7zTypes.h
typedef unsigned long ULONG_PTR;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef void *HANDLE;
typedef void *PVOID;
typedef void *HMENU;
typedef void *HICON;
typedef void *HBITMAP;
typedef void *HFONT;
typedef void *HBRUSH;
typedef void *HDC;
typedef void *HIMAGELIST;
typedef void *HMONITOR;
typedef void *HACCEL;
typedef void *HGDIOBJ;
typedef HICON HCURSOR;
typedef const void *LPCVOID;
typedef WCHAR *LPWSTR;
typedef CHAR *LPSTR;
typedef UINT *LPUINT;
typedef WORD ATOM;
typedef DWORD COLORREF;
#ifndef CONST
#define CONST const
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef WINUSERAPI
#define WINUSERAPI
#endif
#ifndef DECLARE_HANDLE
#define DECLARE_HANDLE(n) typedef void *n
#endif

typedef struct tagPOINT { LONG x; LONG y; } POINT, *LPPOINT;
typedef struct tagSIZE { LONG cx; LONG cy; } SIZE, *LPSIZE;
typedef RECT *LPRECT;
typedef const RECT *LPCRECT;

typedef struct tagMSG
{
  HWND hwnd;
  UINT message;
  WPARAM wParam;
  LPARAM lParam;
  DWORD time;
  POINT pt;
} MSG, *LPMSG;

typedef struct tagNMHDR
{
  HWND hwndFrom;
  UINT_PTR idFrom;
  UINT code;
} NMHDR, *LPNMHDR;

typedef struct tagWINDOWPLACEMENT
{
  UINT length;
  UINT flags;
  UINT showCmd;
  POINT ptMinPosition;
  POINT ptMaxPosition;
  RECT rcNormalPosition;
} WINDOWPLACEMENT;

typedef struct tagMONITORINFO
{
  DWORD cbSize;
  RECT rcMonitor;
  RECT rcWork;
  DWORD dwFlags;
} MONITORINFO, *LPMONITORINFO;
#define MONITOR_DEFAULTTONULL       0x00000000
#define MONITOR_DEFAULTTOPRIMARY    0x00000001
#define MONITOR_DEFAULTTONEAREST    0x00000002

inline BOOL BoolToBOOL(bool v) { return (v ? TRUE: FALSE); }

typedef struct tagCOMBOBOXEXITEMW
{
  UINT mask;
  INT_PTR iItem;
  LPWSTR pszText;
  int cchTextMax;
  int iImage;
  int iSelectedImage;
  int iOverlay;
  int iIndent;
  LPARAM lParam;
} COMBOBOXEXITEMW;
typedef struct tagCOMBOBOXEXITEMA
{
  UINT mask;
  INT_PTR iItem;
  LPSTR pszText;
  int cchTextMax;
  int iImage;
  int iSelectedImage;
  int iOverlay;
  int iIndent;
  LPARAM lParam;
} COMBOBOXEXITEMA, COMBOBOXEXITEM;
#define CBEIF_TEXT 0x1
#define CBEIF_IMAGE 0x2
#define CBEIF_SELECTEDIMAGE 0x4
#define CBEIF_INDENT 0x10
#define CBEIF_LPARAM 0x20

typedef INT_PTR (CALLBACK *DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void (CALLBACK *TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);

typedef struct tagWNDCLASSW
{
  UINT style;
  WNDPROC lpfnWndProc;
  int cbClsExtra;
  int cbWndExtra;
  HINSTANCE hInstance;
  HICON hIcon;
  HCURSOR hCursor;
  HBRUSH hbrBackground;
  LPCWSTR lpszMenuName;
  LPCWSTR lpszClassName;
} WNDCLASSW;

typedef struct tagWNDCLASSA
{
  UINT style;
  WNDPROC lpfnWndProc;
  int cbClsExtra;
  int cbWndExtra;
  HINSTANCE hInstance;
  HICON hIcon;
  HCURSOR hCursor;
  HBRUSH hbrBackground;
  LPCSTR lpszMenuName;
  LPCSTR lpszClassName;
} WNDCLASSA, WNDCLASS;

typedef struct tagHELPINFO { UINT cbSize; int iContextType; int iCtrlId; HWND hItemHandle; DWORD_PTR dwContextId; POINT MousePos; } HELPINFO, *LPHELPINFO;

#define MAKEINTRESOURCEW(i) ((LPCWSTR)(ULONG_PTR)((WORD)(i)))
#define MAKEINTRESOURCEA(i) ((LPCSTR)(ULONG_PTR)((WORD)(i)))
#define MAKEINTRESOURCE MAKEINTRESOURCEA
#define IS_INTRESOURCE(r) ((((ULONG_PTR)(r)) >> 16) == 0)
#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)(((WORD)(l)) | (((DWORD)(WORD)(h)) << 16)))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)(((WORD)(l)) | (((DWORD)(WORD)(h)) << 16)))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))

#ifndef MulDiv
#define MulDiv(a, b, c) ((int)(((long long)(a) * (b)) / (c)))
#endif

// dialog / button results
#ifndef IDOK
#define IDOK 1
#define IDCANCEL 2
#endif
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7
#define IDCLOSE 8
#define IDHELP 9
#ifndef IDCONTINUE
#define IDCONTINUE 11
#endif
#define IDC_STATIC (-1)

// window messages
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETREDRAW 0x000B
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_SHOWWINDOW 0x0018
#define WM_SETCURSOR 0x0020
#define WM_GETMINMAXINFO 0x0024
#define WM_NEXTDLGCTL 0x0028
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_HELP 0x0053
#define WM_NOTIFY 0x004E
#define WM_CONTEXTMENU 0x007B
#define WM_SETICON 0x0080
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_HSCROLL 0x0114
#define WM_VSCROLL 0x0115
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_DROPFILES 0x0233
#define WM_USER 0x0400
#define WM_APP 0x8000

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10

#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWLP_USERDATA (-21)
#define GWLP_ID (-12)
#define DWLP_MSGRESULT 0
#define DWLP_USER 16

#define BST_UNCHECKED 0
#define BST_CHECKED 1
#define BST_INDETERMINATE 2

#define BN_CLICKED 0
#define BN_DOUBLECLICKED 5
#define CBN_SELCHANGE 1
#define CBN_DBLCLK 2
#define CBN_SETFOCUS 3
#define CBN_KILLFOCUS 4
#define CBN_EDITCHANGE 5
#define CBN_EDITUPDATE 6
#define CBN_DROPDOWN 7
#define CBN_CLOSEUP 8
#define CBN_SELENDOK 9
#define CBN_SELENDCANCEL 10
#define EN_SETFOCUS 0x0100
#define EN_KILLFOCUS 0x0200
#define EN_CHANGE 0x0300
#define EN_UPDATE 0x0400
#define LBN_SELCHANGE 1
#define LBN_DBLCLK 2

#define BM_GETCHECK 0x00F0
#define BM_SETCHECK 0x00F1
#define BM_GETSTATE 0x00F2
#define BM_SETSTATE 0x00F3
#define BM_SETSTYLE 0x00F4
#define BM_CLICK 0x00F5
#define BM_SETIMAGE 0x00F7

#define EM_GETSEL 0x00B0
#define EM_SETSEL 0x00B1
#define EM_SCROLLCARET 0x00B7
#define EM_LIMITTEXT 0x00C5
#define EM_SETLIMITTEXT EM_LIMITTEXT
#define EM_REPLACESEL 0x00C2
#define EM_SETPASSWORDCHAR 0x00CC
#define EM_SETREADONLY 0x00CF

#define CB_ERR (-1)
#define CB_ERRSPACE (-2)
#define CB_GETEDITSEL 0x0140
#define CB_LIMITTEXT 0x0141
#define CB_SETEDITSEL 0x0142
#define CB_ADDSTRING 0x0143
#define CB_DELETESTRING 0x0144
#define CB_GETCOUNT 0x0146
#define CB_GETCURSEL 0x0147
#define CB_GETLBTEXT 0x0148
#define CB_GETLBTEXTLEN 0x0149
#define CB_INSERTSTRING 0x014A
#define CB_RESETCONTENT 0x014B
#define CB_FINDSTRING 0x014C
#define CB_SELECTSTRING 0x014D
#define CB_SETCURSEL 0x014E
#define CB_SHOWDROPDOWN 0x014F
#define CB_GETITEMDATA 0x0150
#define CB_SETITEMDATA 0x0151
#define CB_GETDROPPEDSTATE 0x0157
#define CB_FINDSTRINGEXACT 0x0158
#define CB_SETDROPPEDWIDTH 0x0160
#define CBEM_INSERTITEMW (WM_USER + 11)
#define CBEM_SETIMAGELIST (WM_USER + 2)
#define CBEM_GETEDITCONTROL (WM_USER + 7)
#define CBEM_DELETEITEM CB_DELETESTRING
#define CBEM_SETITEMW (WM_USER + 12)
#define CBEM_SETEXTENDEDSTYLE (WM_USER + 14)
#define CBEM_SETUNICODEFORMAT 0x2005
#define CBEM_INSERTITEM CBEM_INSERTITEMW
#define CBEM_SETITEM CBEM_SETITEMW

#define LB_ADDSTRING 0x0180
#define LB_RESETCONTENT 0x0184
#define LB_SETCURSEL 0x0186
#define LB_GETCURSEL 0x0188
#define LB_GETCOUNT 0x018B

#define STM_SETICON 0x0170
#define STM_SETIMAGE 0x0172
#define STM_GETICON 0x0171
#define STM_GETIMAGE 0x0173

#define PBM_SETRANGE (WM_USER + 1)
#define PBM_SETPOS (WM_USER + 2)
#define PBM_DELTAPOS (WM_USER + 3)
#define PBM_SETSTEP (WM_USER + 4)
#define PBM_STEPIT (WM_USER + 5)
#define PBM_SETRANGE32 (WM_USER + 6)
#define PBM_GETPOS (WM_USER + 8)
#define PBM_SETBARCOLOR (WM_USER + 9)
#define PBM_SETMARQUEE (WM_USER + 10)
#define PBM_SETSTATE (WM_USER + 16)
#define PBST_NORMAL 1
#define PBST_ERROR 2
#define PBST_PAUSED 3

#define UDM_SETRANGE (WM_USER + 101)
#define UDM_GETRANGE (WM_USER + 102)
#define UDM_SETPOS (WM_USER + 103)
#define UDM_GETPOS (WM_USER + 104)
#define UDM_SETBUDDY (WM_USER + 105)
#define UDM_SETRANGE32 (WM_USER + 111)
#define UDM_GETRANGE32 (WM_USER + 112)
#define UDM_SETPOS32 (WM_USER + 113)
#define UDM_GETPOS32 (WM_USER + 114)

#define TBM_GETPOS (WM_USER)
#define TBM_SETPOS (WM_USER + 5)
#define TBM_SETRANGE (WM_USER + 6)

// window styles (for GetWindowLong(GWL_STYLE) and templates)
#define WS_OVERLAPPED 0x00000000L
#define WS_POPUP 0x80000000L
#define WS_CHILD 0x40000000L
#define WS_VISIBLE 0x10000000L
#define WS_DISABLED 0x08000000L
#define WS_CAPTION 0x00C00000L
#define WS_BORDER 0x00800000L
#define WS_VSCROLL 0x00200000L
#define WS_HSCROLL 0x00100000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_SIZEBOX WS_THICKFRAME
#define WS_GROUP 0x00020000L
#define WS_TABSTOP 0x00010000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_EX_CLIENTEDGE 0x00000200L
#define DS_MODALFRAME 0x80L
#define DS_CENTER 0x0800L
#define ES_CENTER 0x0001L
#define ES_RIGHT 0x0002L
#define ES_MULTILINE 0x0004L
#define ES_PASSWORD 0x0020L
#define ES_AUTOHSCROLL 0x0080L
#define ES_READONLY 0x0800L
#define ES_NUMBER 0x2000L
#define SS_LEFT 0x0L
#define SS_CENTER 0x1L
#define SS_RIGHT 0x2L
#define SS_ICON 0x3L
#define SS_LEFTNOWORDWRAP 0xCL
#define SS_NOPREFIX 0x80L
#define BS_AUTOCHECKBOX 0x3L
#define BS_AUTORADIOBUTTON 0x9L
#define BS_DEFPUSHBUTTON 0x1L
#define CBS_DROPDOWN 0x2L
#define CBS_DROPDOWNLIST 0x3L
#define CBS_SORT 0x100L

#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_SHOWWINDOW 0x0040

#define SPI_GETWORKAREA 0x0030
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXVSCROLL 2
#define SM_CYHSCROLL 3
#define SM_CYCAPTION 4
#define SM_CXBORDER 5
#define SM_CYBORDER 6
#define SM_CXSIZEFRAME 32
#define SM_CYSIZEFRAME 33
#define SM_CXEDGE 45
#define SM_CYEDGE 46

#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_ABORTRETRYIGNORE 0x00000002L
#define MB_YESNOCANCEL 0x00000003L
#define MB_YESNO 0x00000004L
#define MB_RETRYCANCEL 0x00000005L
#define MB_ICONHAND 0x00000010L
#define MB_ICONERROR MB_ICONHAND
#define MB_ICONSTOP MB_ICONHAND
#define MB_ICONQUESTION 0x00000020L
#define MB_ICONEXCLAMATION 0x00000030L
#define MB_ICONWARNING MB_ICONEXCLAMATION
#define MB_ICONASTERISK 0x00000040L
#define MB_ICONINFORMATION MB_ICONASTERISK
#define MB_DEFBUTTON2 0x00000100L
#define MB_TASKMODAL 0x00002000L
#define MB_SETFOREGROUND 0x00010000L

#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_RETURN 0x0D

#define ICON_SMALL 0
#define ICON_BIG 1
#define IMAGE_ICON 1
#define LR_DEFAULTCOLOR 0
#define LR_SHARED 0x8000

#define HWND_TOP ((HWND)0)

#define WC_LISTVIEWW L"SysListView32"
#define UPDOWN_CLASSW L"msctls_updown32"

// ---- functions (Q32.cpp) ----
HWND GetDlgItem(HWND dialog, int id);
int GetDlgCtrlID(HWND hwnd);
BOOL SetDlgItemTextW(HWND dialog, int id, LPCWSTR s);
BOOL SetDlgItemTextA(HWND dialog, int id, LPCSTR s);
#define SetDlgItemText SetDlgItemTextA
UINT GetDlgItemTextW(HWND dialog, int id, LPWSTR s, int maxCount);
UINT GetDlgItemTextA(HWND dialog, int id, LPSTR s, int maxCount);
#define GetDlgItemText GetDlgItemTextA
BOOL SetDlgItemInt(HWND dialog, int id, UINT value, BOOL isSigned);
UINT GetDlgItemInt(HWND dialog, int id, BOOL *translated, BOOL isSigned);
BOOL CheckDlgButton(HWND dialog, int id, UINT check);
UINT IsDlgButtonChecked(HWND dialog, int id);
BOOL CheckRadioButton(HWND dialog, int first, int last, int check);
LRESULT SendDlgItemMessage(HWND dialog, int id, UINT msg, WPARAM w, LPARAM l);
HWND GetNextDlgGroupItem(HWND dialog, HWND control, BOOL previous);
HWND GetNextDlgTabItem(HWND dialog, HWND control, BOOL previous);
BOOL MapDialogRect(HWND dialog, LPRECT rect);
BOOL IsDialogMessage(HWND dialog, LPMSG msg);
BOOL EndDialog(HWND dialog, INT_PTR result);
INT_PTR DialogBoxParamW(HINSTANCE inst, LPCWSTR templateName, HWND parent, DLGPROC proc, LPARAM param);
HWND CreateDialogParamW(HINSTANCE inst, LPCWSTR templateName, HWND parent, DLGPROC proc, LPARAM param);
// templates are always resource IDs (MAKEINTRESOURCE), so A == W
inline INT_PTR DialogBoxParamA(HINSTANCE inst, LPCSTR templateName, HWND parent, DLGPROC proc, LPARAM param)
  { return DialogBoxParamW(inst, (LPCWSTR)(const void *)templateName, parent, proc, param); }
inline HWND CreateDialogParamA(HINSTANCE inst, LPCSTR templateName, HWND parent, DLGPROC proc, LPARAM param)
  { return CreateDialogParamW(inst, (LPCWSTR)(const void *)templateName, parent, proc, param); }
#define DialogBoxParam DialogBoxParamA
#define CreateDialogParam CreateDialogParamA
LONG GetDialogBaseUnits();

// A variants take UTF-8 strings, W variants wchar_t strings
LRESULT SendMessageW(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
LRESULT SendMessageA(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
#define SendMessage SendMessageA
BOOL PostMessageW(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
BOOL PostMessageA(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
#define PostMessage PostMessageA

BOOL SetWindowTextW(HWND hwnd, LPCWSTR s);
BOOL SetWindowTextA(HWND hwnd, LPCSTR s);
#define SetWindowText SetWindowTextA
int GetWindowTextW(HWND hwnd, LPWSTR s, int maxCount);
int GetWindowTextA(HWND hwnd, LPSTR s, int maxCount);
#define GetWindowText GetWindowTextA
int GetWindowTextLengthW(HWND hwnd);
int GetWindowTextLengthA(HWND hwnd);
#define GetWindowTextLength GetWindowTextLengthA

BOOL EnableWindow(HWND hwnd, BOOL enable);
BOOL IsWindowEnabled(HWND hwnd);
BOOL ShowWindow(HWND hwnd, int cmdShow);
BOOL IsWindowVisible(HWND hwnd);
BOOL IsWindow(HWND hwnd);
BOOL IsZoomed(HWND hwnd);
BOOL DestroyWindow(HWND hwnd);
HWND GetParent(HWND hwnd);
HWND SetFocus(HWND hwnd);
HWND GetFocus();
BOOL SetForegroundWindow(HWND hwnd);
BOOL UpdateWindow(HWND hwnd);
BOOL InvalidateRect(HWND hwnd, LPCRECT rect, BOOL erase);
BOOL MoveWindow(HWND hwnd, int x, int y, int w, int h, BOOL repaint);
BOOL SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags);
BOOL GetWindowRect(HWND hwnd, LPRECT rect);
BOOL GetClientRect(HWND hwnd, LPRECT rect);
BOOL ClientToScreen(HWND hwnd, LPPOINT p);
BOOL ScreenToClient(HWND hwnd, LPPOINT p);
BOOL SetWindowPlacement(HWND hwnd, const WINDOWPLACEMENT *p);
BOOL GetWindowPlacement(HWND hwnd, WINDOWPLACEMENT *p);
LONG SetWindowLongW(HWND hwnd, int index, LONG v);
LONG GetWindowLongW(HWND hwnd, int index);
#define SetWindowLong SetWindowLongW
#define GetWindowLong GetWindowLongW
LONG_PTR SetWindowLongPtrW(HWND hwnd, int index, LONG_PTR v);
LONG_PTR GetWindowLongPtrW(HWND hwnd, int index);
#define SetWindowLongPtr SetWindowLongPtrW
#define GetWindowLongPtr GetWindowLongPtrW
HMENU GetSystemMenu(HWND hwnd, BOOL revert);
UINT_PTR SetTimer(HWND hwnd, UINT_PTR id, UINT elapse, TIMERPROC proc);
BOOL KillTimer(HWND hwnd, UINT_PTR id);
SHORT GetKeyState(int key);
int GetSystemMetrics(int index);
BOOL SystemParametersInfoW(UINT action, UINT param, PVOID pv, UINT winIni);
#define SystemParametersInfo SystemParametersInfoW
HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags);
BOOL GetMonitorInfoA(HMONITOR mon, LPMONITORINFO mi);
BOOL GetMonitorInfoW(HMONITOR mon, LPMONITORINFO mi);

HWND CreateWindowExW(DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style,
    int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
HWND CreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR windowName, DWORD style,
    int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
#define CreateWindowEx CreateWindowExA
#define CreateWindowW(c, n, s, x, y, w, h, p, m, i, l) CreateWindowExW(0, c, n, s, x, y, w, h, p, m, i, l)
#define CreateWindowA(c, n, s, x, y, w, h, p, m, i, l) CreateWindowExA(0, c, n, s, x, y, w, h, p, m, i, l)
#define CreateWindow CreateWindowA
ATOM RegisterClassW(const WNDCLASSW *wc);
ATOM RegisterClassA(const WNDCLASSA *wc);
#define RegisterClass RegisterClassA
LRESULT DefWindowProcW(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
#define DefWindowProc DefWindowProcW
LRESULT CallWindowProcW(WNDPROC proc, HWND hwnd, UINT msg, WPARAM w, LPARAM l);
#define CallWindowProc CallWindowProcW

int MessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type);
int MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type);
#define MessageBox MessageBoxA
BOOL MessageBeep(UINT type);

HICON LoadIconW(HINSTANCE inst, LPCWSTR name);
#define LoadIcon LoadIconW
HANDLE LoadImageW(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT flags);
#define LoadImage LoadImageW
BOOL DestroyIcon(HICON icon);

void Sleep(DWORD ms);

// GDI fonts (BenchmarkDialog's fixed-pitch console)
typedef struct tagLOGFONTA
{
  LONG lfHeight;
  LONG lfWidth;
  LONG lfEscapement;
  LONG lfOrientation;
  LONG lfWeight;
  BYTE lfItalic;
  BYTE lfUnderline;
  BYTE lfStrikeOut;
  BYTE lfCharSet;
  BYTE lfOutPrecision;
  BYTE lfClipPrecision;
  BYTE lfQuality;
  BYTE lfPitchAndFamily;
  CHAR lfFaceName[32];
} LOGFONTA, LOGFONT;
#define FW_DONTCARE 0
#define FW_NORMAL 400
#define FW_BOLD 700
#define DEFAULT_CHARSET 1
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY 0
#define DEFAULT_PITCH 0
#define FIXED_PITCH 1
#define VARIABLE_PITCH 2
HFONT CreateFontIndirectA(const LOGFONTA *lf);
#define CreateFontIndirect CreateFontIndirectA
BOOL DeleteObject(HGDIOBJ obj);

#include <string.h>
inline LPSTR lstrcat(LPSTR a, LPCSTR b) { return strcat(a, b); }
inline LPSTR lstrcpy(LPSTR a, LPCSTR b) { return strcpy(a, b); }
inline int lstrlen(LPCSTR a) { return (int)strlen(a); }

// The dialog layouts generated from upstream .rc files.
struct CDlgItemTemplate
{
  const char *Kind;      // LTEXT, PUSHBUTTON, COMBOBOX, CONTROL, ...
  const wchar_t *Text;
  int Id;
  const char *Class;     // window class for CONTROL
  UInt32 Style;
  int X, Y, CX, CY;      // dialog units
};

struct CDlgTemplate
{
  int Id;
  const wchar_t *Caption;
  int CX, CY;
  UInt32 Style;
  const CDlgItemTemplate *Items;
  unsigned NumItems;
};

const CDlgTemplate *Q32_FindDialogTemplate(int id);

// FileManager/LangUtils.h declares this only with Z7_LANG, but
// GUI/CompressDialog.cpp calls it unconditionally. No lang files are
// loaded, so it returns an empty string (Seams.cpp).
#include "Common/MyString.h"
void LangString_OnlyFromLangFile(UInt32 langID, UString &dest);

#endif // !_WIN32

#endif
