// F-22 Lightning II native rewrite – launcher (Windows).
//
// Sits next to f22native.exe. It finds the game files (F22.EXE, RESOURCE.RES, the optional video folder), lets the
// player change the settings without opening the game's menus, starts the game and runs the texture export.
//  - Game settings go into the game's own file, f22native.sav ("opt" line: sfx music texDist skyHigh joy fullscreen
//    gEffects hdTex); every other line of that file (pilots, career, statistics) is kept as it is.
//  - Launcher-only choices (game folder, window size, skip intro, original AI) go into f22launcher.ini and become
//    command-line switches of f22native.exe (--res, --exe, --size, --nointro, --aivm).
// No external libraries: user32, gdi32, comctl32, shell32, ole32.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

enum Id {
  IdPlay = 100, IdExit, IdBrowse, IdAuto, IdHd, IdFull, IdIntro, IdAiVm, IdSize, IdSfx, IdMusic, IdSfxVal, IdMusicVal,
  IdExport, IdTexFolder, IdReadme, IdTitle, IdSub, IdGroupGame, IdGroupSet, IdGroupTex, IdFolder, IdStatus, IdSizeLabel,
  IdSfxLabel, IdMusicLabel, IdTexHint
};

struct Opt { int sfx = 100, music = 80, texDist = 100, skyHigh = 1, joy = 1, full = 0, gfx = 1, hd = 0; };

std::wstring gHere;             // folder of the launcher and f22native.exe, ends with '\'
std::wstring gGameDir;          // folder holding RESOURCE.RES and F22.EXE, ends with '\' (empty: not found)
bool gGameDirManual = false;    // chosen with "Browse" (otherwise found automatically)
Opt gOpt;
HWND gWnd;
HFONT gFont, gBold, gBig;
std::map<int, HWND> gCtl;
int gDpi = 96;
HANDLE gJob = nullptr;          // the running texture export

const wchar_t* SIZES[] = {L"1024 x 768", L"1280 x 720", L"1280 x 1024", L"1600 x 900", L"1920 x 1080", L"2560 x 1440"};
const int NSIZES = sizeof SIZES / sizeof SIZES[0];

int S(int v) { return MulDiv(v, gDpi, 96); }
bool isFile(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
bool isDir(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
std::wstring slash(std::wstring p) { if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\'; return p; }
std::wstring parent(std::wstring p) {
  while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
  size_t c = p.find_last_of(L"\\/");
  return c == std::wstring::npos ? std::wstring() : p.substr(0, c + 1);
}
bool hasGame(const std::wstring& d) { return isFile(d + L"RESOURCE.RES") && isFile(d + L"F22.EXE"); }

// The same search as f22native.exe: the program's folder and up to 4 parent folders.
std::wstring findGameDir() {
  std::wstring d = gHere;
  for (int i = 0; i < 5 && !d.empty(); i++, d = parent(d)) if (hasGame(d)) return d;
  return {};
}

// ---- f22launcher.ini (UTF-16 via the profile API) ----
std::wstring ini() { return gHere + L"f22launcher.ini"; }
std::wstring iniGet(const wchar_t* k, const wchar_t* def) {
  wchar_t b[1024];
  GetPrivateProfileStringW(L"launcher", k, def, b, 1024, ini().c_str());
  return b;
}
void iniSet(const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"launcher", k, v.c_str(), ini().c_str()); }

// ---- f22native.sav: only the "opt" line belongs to the launcher ----
std::wstring savPath() { return gHere + L"f22native.sav"; }
std::vector<std::string> readLines(const std::wstring& p) {
  std::vector<std::string> v;
  std::ifstream f(p.c_str());
  for (std::string s; std::getline(f, s);) { if (!s.empty() && s.back() == '\r') s.pop_back(); v.push_back(s); }
  return v;
}
void loadOpt() {
  for (auto& s : readLines(savPath())) {
    if (s.compare(0, 4, "opt ") != 0) continue;
    Opt o;
    if (std::sscanf(s.c_str() + 4, "%d %d %d %d %d %d %d %d", &o.sfx, &o.music, &o.texDist, &o.skyHigh, &o.joy, &o.full, &o.gfx, &o.hd) >= 5) gOpt = o;
  }
}
bool saveOpt() {
  std::vector<std::string> v = readLines(savPath());
  char line[128];
  std::snprintf(line, sizeof line, "opt %d %d %d %d %d %d %d %d", gOpt.sfx, gOpt.music, gOpt.texDist, gOpt.skyHigh, gOpt.joy, gOpt.full, gOpt.gfx, gOpt.hd);
  bool put = false;
  for (auto& s : v) if (s.compare(0, 4, "opt ") == 0) { s = line; put = true; }
  if (!put) v.push_back(line);
  std::ofstream f(savPath().c_str(), std::ios::trunc);
  if (!f) return false;
  for (auto& s : v) f << s << "\n";
  return bool(f);
}

// ---- controls ----
HWND add(int id, const wchar_t* cls, const wchar_t* text, DWORD style, HFONT font = nullptr) {
  HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, gWnd, HMENU(INT_PTR(id)), nullptr, nullptr);
  SendMessageW(h, WM_SETFONT, WPARAM(font ? font : gFont), TRUE);
  gCtl[id] = h;
  return h;
}
void place(int id, int x, int y, int w, int h) { MoveWindow(gCtl[id], S(x), S(y), S(w), S(h), TRUE); }
void setText(int id, const std::wstring& t) { SetWindowTextW(gCtl[id], t.c_str()); }
bool checked(int id) { return SendMessageW(gCtl[id], BM_GETCHECK, 0, 0) == BST_CHECKED; }
void check(int id, bool on) { SendMessageW(gCtl[id], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

void layout() {
  place(IdTitle, 16, 12, 440, 30);
  place(IdSub, 16, 42, 440, 18);
  place(IdGroupGame, 10, 66, 452, 104);
  place(IdFolder, 22, 88, 330, 22);
  place(IdBrowse, 358, 87, 92, 24);
  place(IdStatus, 22, 116, 330, 48);
  place(IdAuto, 358, 116, 92, 24);
  place(IdGroupSet, 10, 176, 452, 196);
  place(IdHd, 22, 198, 210, 20);
  place(IdFull, 240, 198, 210, 20);
  place(IdIntro, 22, 222, 210, 20);
  place(IdAiVm, 240, 222, 216, 20);
  place(IdSizeLabel, 22, 254, 110, 20);
  place(IdSize, 140, 250, 150, 200);
  place(IdSfxLabel, 22, 290, 110, 20);
  place(IdSfx, 136, 286, 270, 28);
  place(IdSfxVal, 410, 290, 44, 20);
  place(IdMusicLabel, 22, 326, 110, 20);
  place(IdMusic, 136, 322, 270, 28);
  place(IdMusicVal, 410, 326, 44, 20);
  place(IdGroupTex, 10, 378, 452, 82);
  place(IdTexHint, 22, 398, 430, 30);
  place(IdExport, 22, 428, 140, 24);
  place(IdTexFolder, 170, 428, 140, 24);
  place(IdReadme, 318, 428, 132, 24);
  place(IdPlay, 10, 470, 330, 38);
  place(IdExit, 348, 470, 114, 38);
}

void showVol() {
  setText(IdSfxVal, std::to_wstring(SendMessageW(gCtl[IdSfx], TBM_GETPOS, 0, 0)) + L"%");
  setText(IdMusicVal, std::to_wstring(SendMessageW(gCtl[IdMusic], TBM_GETPOS, 0, 0)) + L"%");
}

void refreshStatus() {
  if (gGameDir.empty()) {
    setText(IdFolder, L"(game folder not found)");
    setText(IdStatus, L"F22.EXE and RESOURCE.RES were not found next to this program.\nChoose the F-22 game folder with \"Browse\".");
  } else {
    setText(IdFolder, gGameDir);
    std::wstring s = L"F22.EXE and RESOURCE.RES found";
    s += gGameDirManual ? L" (chosen folder)." : L" (found automatically).";
    bool vid = isDir(gGameDir + L"video") || isDir(gHere + L"video");
    s += vid ? L"\nVideos: found." : L"\nVideos: none (optional, see README).";
    setText(IdStatus, s);
  }
  bool prog = isFile(gHere + L"f22native.exe");
  if (!prog) setText(IdStatus, L"f22native.exe is missing: put the launcher next to it.");
  bool ok = prog && !gGameDir.empty() && !gJob;
  EnableWindow(gCtl[IdPlay], ok);
  EnableWindow(gCtl[IdExport], ok);
  EnableWindow(gCtl[IdAuto], gGameDirManual);
}

void loadAll() {
  loadOpt();
  std::wstring d = iniGet(L"gamedir", L"");
  if (!d.empty() && hasGame(slash(d))) { gGameDir = slash(d); gGameDirManual = true; }
  else gGameDir = findGameDir();
  check(IdHd, gOpt.hd != 0);
  check(IdFull, gOpt.full != 0);
  check(IdIntro, iniGet(L"skipintro", L"0") == L"1");
  check(IdAiVm, iniGet(L"aivm", L"0") == L"1");
  std::wstring sz = iniGet(L"size", L"1280 x 720");
  int sel = 1;
  for (int i = 0; i < NSIZES; i++) if (sz == SIZES[i]) sel = i;
  SendMessageW(gCtl[IdSize], CB_SETCURSEL, sel, 0);
  SendMessageW(gCtl[IdSfx], TBM_SETPOS, TRUE, gOpt.sfx);
  SendMessageW(gCtl[IdMusic], TBM_SETPOS, TRUE, gOpt.music);
  showVol();
  refreshStatus();
}

bool saveAll() {
  gOpt.hd = checked(IdHd);
  gOpt.full = checked(IdFull);
  gOpt.sfx = int(SendMessageW(gCtl[IdSfx], TBM_GETPOS, 0, 0));
  gOpt.music = int(SendMessageW(gCtl[IdMusic], TBM_GETPOS, 0, 0));
  iniSet(L"gamedir", gGameDirManual ? gGameDir : L"");
  iniSet(L"skipintro", checked(IdIntro) ? L"1" : L"0");
  iniSet(L"aivm", checked(IdAiVm) ? L"1" : L"0");
  int sel = int(SendMessageW(gCtl[IdSize], CB_GETCURSEL, 0, 0));
  iniSet(L"size", SIZES[sel >= 0 && sel < NSIZES ? sel : 1]);
  if (!saveOpt()) {
    MessageBoxW(gWnd, (L"Could not write " + savPath()).c_str(), L"F-22 Lightning II", MB_ICONWARNING);
    return false;
  }
  return true;
}

std::wstring gameArgs() {
  std::wstring a;
  if (gGameDirManual) a += L" --res \"" + gGameDir + L"RESOURCE.RES\" --exe \"" + gGameDir + L"F22.EXE\"";
  return a;
}

bool run(const std::wstring& args, bool hidden, HANDLE* proc) {
  std::wstring cmd = L"\"" + gHere + L"f22native.exe\"" + args;
  STARTUPINFOW si = {sizeof si};
  if (hidden) { si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; }
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, gHere.c_str(), &si, &pi)) {
    MessageBoxW(gWnd, L"Could not start f22native.exe.", L"F-22 Lightning II", MB_ICONERROR);
    return false;
  }
  CloseHandle(pi.hThread);
  if (proc) *proc = pi.hProcess; else CloseHandle(pi.hProcess);
  return true;
}

void play() {
  if (!saveAll()) return;
  std::wstring a = gameArgs();
  int sel = int(SendMessageW(gCtl[IdSize], CB_GETCURSEL, 0, 0));
  int w = 1280, h = 720;
  if (sel >= 0 && sel < NSIZES) std::swscanf(SIZES[sel], L"%d x %d", &w, &h);
  a += L" --size " + std::to_wstring(w) + L"x" + std::to_wstring(h);
  if (checked(IdIntro)) a += L" --nointro";
  if (checked(IdAiVm)) a += L" --aivm";
  if (run(a, false, nullptr)) DestroyWindow(gWnd);
}

DWORD WINAPI waitJob(LPVOID) {
  WaitForSingleObject(gJob, INFINITE);
  PostMessageW(gWnd, WM_APP, 0, 0);
  return 0;
}

void exportTextures() {
  if (!saveAll()) return;
  if (!run(gameArgs() + L" --dumptex", true, &gJob)) return;
  setText(IdExport, L"Exporting...");
  refreshStatus();
  CloseHandle(CreateThread(nullptr, 0, waitJob, nullptr, 0, nullptr));
}

void exportDone() {
  DWORD rc = 1;
  GetExitCodeProcess(gJob, &rc);
  CloseHandle(gJob);
  gJob = nullptr;
  setText(IdExport, L"Export textures");
  refreshStatus();
  if (rc == 0) {
    if (MessageBoxW(gWnd, L"The original textures are in the textures folder as PNG files.\n"
                          L"Edit them and keep the names; existing files were not overwritten.\n\nOpen the folder now?",
                    L"Export textures", MB_ICONINFORMATION | MB_YESNO) == IDYES)
      ShellExecuteW(gWnd, L"open", (gHere + L"textures").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  } else {
    MessageBoxW(gWnd, L"The texture export failed (see dumptex.txt).", L"Export textures", MB_ICONWARNING);
  }
}

void browse() {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
  DWORD o = 0;
  dlg->GetOptions(&o);
  dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(L"Choose the F-22 Lightning II game folder (with F22.EXE and RESOURCE.RES)");
  if (SUCCEEDED(dlg->Show(gWnd))) {
    IShellItem* it = nullptr;
    if (SUCCEEDED(dlg->GetResult(&it))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
        std::wstring d = slash(p);
        CoTaskMemFree(p);
        if (hasGame(d)) { gGameDir = d; gGameDirManual = true; iniSet(L"gamedir", d); }
        else MessageBoxW(gWnd, L"This folder does not hold both F22.EXE and RESOURCE.RES.", L"F-22 Lightning II", MB_ICONWARNING);
      }
      it->Release();
    }
  }
  dlg->Release();
  refreshStatus();
}

void build() {
  add(IdTitle, L"STATIC", L"F-22 Lightning II", SS_LEFT, gBig);
  add(IdSub, L"STATIC", L"native rewrite for Windows – launcher", SS_LEFT);
  add(IdGroupGame, L"BUTTON", L"Game files", BS_GROUPBOX, gBold);
  add(IdFolder, L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_BORDER);
  add(IdBrowse, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP);
  add(IdStatus, L"STATIC", L"", SS_LEFT);
  add(IdAuto, L"BUTTON", L"Automatic", BS_PUSHBUTTON | WS_TABSTOP);
  add(IdGroupSet, L"BUTTON", L"Settings", BS_GROUPBOX, gBold);
  add(IdHd, L"BUTTON", L"HD textures", BS_AUTOCHECKBOX | WS_TABSTOP);
  add(IdFull, L"BUTTON", L"Fullscreen (Alt+Enter)", BS_AUTOCHECKBOX | WS_TABSTOP);
  add(IdIntro, L"BUTTON", L"Skip the intro videos", BS_AUTOCHECKBOX | WS_TABSTOP);
  add(IdAiVm, L"BUTTON", L"Original AI scripts (experimental)", BS_AUTOCHECKBOX | WS_TABSTOP);
  add(IdSizeLabel, L"STATIC", L"Window size:", SS_LEFT);
  HWND cb = add(IdSize, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
  for (int i = 0; i < NSIZES; i++) SendMessageW(cb, CB_ADDSTRING, 0, LPARAM(SIZES[i]));
  add(IdSfxLabel, L"STATIC", L"Sound effects:", SS_LEFT);
  add(IdSfx, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP);
  add(IdSfxVal, L"STATIC", L"", SS_RIGHT);
  add(IdMusicLabel, L"STATIC", L"Music:", SS_LEFT);
  add(IdMusic, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP);
  add(IdMusicVal, L"STATIC", L"", SS_RIGHT);
  for (int id : {IdSfx, IdMusic}) { SendMessageW(gCtl[id], TBM_SETRANGE, TRUE, MAKELPARAM(0, 100)); SendMessageW(gCtl[id], TBM_SETPAGESIZE, 0, 10); }
  add(IdGroupTex, L"BUTTON", L"Textures", BS_GROUPBOX, gBold);
  add(IdTexHint, L"STATIC", L"Replaced textures: PNG or BMP files with the original names in the textures folder.", SS_LEFT);
  add(IdExport, L"BUTTON", L"Export textures", BS_PUSHBUTTON | WS_TABSTOP);
  add(IdTexFolder, L"BUTTON", L"Open textures folder", BS_PUSHBUTTON | WS_TABSTOP);
  add(IdReadme, L"BUTTON", L"Read me", BS_PUSHBUTTON | WS_TABSTOP);
  add(IdPlay, L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, gBig);
  add(IdExit, L"BUTTON", L"Exit", BS_PUSHBUTTON | WS_TABSTOP);
  layout();
  loadAll();
}

void command(int id) {
  switch (id) {
    case IdPlay: play(); break;
    case IdExit: SendMessageW(gWnd, WM_CLOSE, 0, 0); break;
    case IdBrowse: browse(); break;
    case IdAuto: gGameDirManual = false; gGameDir = findGameDir(); iniSet(L"gamedir", L""); refreshStatus(); break;
    case IdExport: exportTextures(); break;
    case IdTexFolder:
      CreateDirectoryW((gHere + L"textures").c_str(), nullptr);
      ShellExecuteW(gWnd, L"open", (gHere + L"textures").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      break;
    case IdReadme: {
      const wchar_t* names[] = {L"README.md", L"README.txt", L"INSTALL.txt"};
      for (auto n : names) if (isFile(gHere + n)) { ShellExecuteW(gWnd, L"open", L"notepad.exe", (L"\"" + gHere + n + L"\"").c_str(), nullptr, SW_SHOWNORMAL); return; }
      MessageBoxW(gWnd, L"README.md is not next to the launcher.", L"F-22 Lightning II", MB_ICONINFORMATION);
      break;
    }
  }
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: gWnd = h; build(); return 0;
    case WM_COMMAND: command(LOWORD(wp)); return 0;
    case WM_HSCROLL: showVol(); return 0;
    case WM_APP: exportDone(); return 0;
    case WM_CTLCOLORSTATIC: {
      HDC dc = HDC(wp);
      SetBkMode(dc, TRANSPARENT);
      if (HWND(lp) == gCtl[IdFolder]) return LRESULT(GetSysColorBrush(COLOR_WINDOW));
      return LRESULT(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_CLOSE:
      if (gJob) { MessageBoxW(h, L"Wait until the texture export is finished.", L"F-22 Lightning II", MB_ICONINFORMATION); return 0; }
      saveAll();
      DestroyWindow(h);
      return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, wp, lp);
}

HFONT font(int pt, int weight) {
  return CreateFontW(-MulDiv(pt, gDpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  wchar_t p[MAX_PATH];
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  gHere = parent(p);
  HDC sdc = GetDC(nullptr);
  gDpi = GetDeviceCaps(sdc, LOGPIXELSY);
  ReleaseDC(nullptr, sdc);
  gFont = font(9, FW_NORMAL);
  gBold = font(9, FW_SEMIBOLD);
  gBig = font(15, FW_SEMIBOLD);

  WNDCLASSW wc = {};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.lpszClassName = L"F22Launcher";
  RegisterClassW(&wc);
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r = {0, 0, S(472), S(518)};
  AdjustWindowRect(&r, style, FALSE);
  int w = r.right - r.left, h = r.bottom - r.top;
  HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"F-22 Lightning II – Launcher", style,
                             (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                             nullptr, nullptr, inst, nullptr);
  ShowWindow(wnd, show);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (!IsDialogMessageW(wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  }
  CoUninitialize();
  return 0;
}
