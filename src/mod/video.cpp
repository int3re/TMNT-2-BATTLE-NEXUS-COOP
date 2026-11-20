#include "mod/video.h"
#include "mod/widescreen.h"

#include <windows.h>
#include <d3d9.h>
#include <mmsystem.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/camera.h"
#include "mod/game.h"
#include "mod/fixes.h"
#include "mod/input.h"
#include "mod/netplay.h"
#include "mod/overlay.h"
#include "mod/patch.h"
#include "mod/postfx.h"

#pragma intrinsic(_ReturnAddress)

namespace video {
namespace {

enum class Mode { Fullscreen, Borderless, Windowed };

Config g_cfg;
Mode g_mode = Mode::Borderless;
bool g_start_windowed = false;  // started borderless/windowed (Alt+Enter allowed)
int g_render_w = 0, g_render_h = 0;  // the frame the game renders in a window
SIZE g_windowed_client{};            // windowed client size, kept across Alt+Enter
std::atomic<HWND> g_hwnd{nullptr};
bool g_copy = false;  // D3DSWAPEFFECT_COPY: Present may target a rectangle
RECT g_dest{};
RECT g_last_client{};
int g_bar_frames = 0;
bool g_shown = false;

const char* mode_name(Mode m) {
  return m == Mode::Borderless ? "borderless" : m == Mode::Windowed ? "windowed" : "fullscreen";
}

bool windowed() { return g_mode != Mode::Fullscreen; }

int read_int(uintptr_t a) { return patch::read<int>(a); }
void write_int(uintptr_t a, int v) { *reinterpret_cast<volatile int*>(a) = v; }

MONITORINFO primary_monitor() {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  POINT origin{0, 0};
  GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
  return mi;
}

MONITORINFO monitor_of(HWND hwnd) {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
  return mi;
}

// 32-bit modes of the primary adapter (what the game's list will hold).
std::vector<SIZE> display_modes() {
  std::vector<SIZE> out;
  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return out;
  UINT n = d3d->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
  for (UINT i = 0; i < n; ++i) {
    D3DDISPLAYMODE m{};
    if (SUCCEEDED(d3d->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &m)))
      out.push_back(SIZE{(LONG)m.Width, (LONG)m.Height});
  }
  d3d->Release();
  return out;
}

bool has_mode(const std::vector<SIZE>& modes, int w, int h);

// The options page's resolution rows: this monitor's 32-bit modes instead of the game's ten
// 4:3 ones (16 and 32 bits, up to 1600x1200): the desktop's size, the others of its shape,
// 4:3 ones, then the rest, ten at most, smallest first.
void fill_resolution_rows() {
  static const int32_t kOld[game::kResRows][3] = {{640, 480, 16},  {800, 600, 16},  {1024, 768, 16}, {1280, 960, 16},
                                                  {1600, 1200, 16}, {640, 480, 32},  {800, 600, 32},  {1024, 768, 32},
                                                  {1280, 960, 32},  {1600, 1200, 32}};
  std::vector<SIZE> all;
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
    if (dm.dmBitsPerPel != 32 || dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
    SIZE s{(LONG)dm.dmPelsWidth, (LONG)dm.dmPelsHeight};
    if (!has_mode(all, s.cx, s.cy)) all.push_back(s);
  }
  MONITORINFO mi = primary_monitor();
  SIZE desk{mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top};
  if (all.empty()) return;
  std::vector<SIZE> rows;
  auto take = [&](const SIZE& s) {
    if ((int)rows.size() < game::kResRows && !has_mode(rows, s.cx, s.cy)) rows.push_back(s);
  };
  if (has_mode(all, desk.cx, desk.cy)) take(desk);
  auto by_area = [](const SIZE& a, const SIZE& b) { return a.cx * a.cy < b.cx * b.cy || (a.cx * a.cy == b.cx * b.cy && a.cx < b.cx); };
  std::sort(all.begin(), all.end(), by_area);
  // Of the desktop's shape (largest first: they matter most), then 4:3, then the rest.
  for (auto it = all.rbegin(); it != all.rend(); ++it)
    if (desk.cy > 0 && std::abs(it->cx * desk.cy - it->cy * desk.cx) <= desk.cy) take(*it);
  for (auto it = all.rbegin(); it != all.rend(); ++it)
    if (it->cx * 3 == it->cy * 4) take(*it);
  for (auto it = all.rbegin(); it != all.rend(); ++it) take(*it);
  std::sort(rows.begin(), rows.end(), by_area);
  int32_t table[game::kResRows][3];
  for (int i = 0; i < game::kResRows; ++i) {
    const SIZE& s = rows[i < (int)rows.size() ? i : rows.size() - 1];  // (few modes: the last again)
    table[i][0] = s.cx;
    table[i][1] = s.cy;
    table[i][2] = 32;
  }
  if (patch::replace("options page resolutions (this monitor's modes)", game::kResTable, kOld, table, sizeof(table))) {
    std::string list;
    for (const SIZE& s : rows) list += " " + std::to_string(s.cx) + "x" + std::to_string(s.cy);
    LOGI("video: the options page offers%s", list.c_str());
  }
}

bool has_mode(const std::vector<SIZE>& modes, int w, int h) {
  return std::any_of(modes.begin(), modes.end(), [&](const SIZE& s) { return s.cx == w && s.cy == h; });
}

// --- start-up settings: PCSetting defaults + TMNT2.ini ------------------------
using SettingInit_t = int(__fastcall*)(void* self, void* edx);
SettingInit_t oSettingInit = nullptr;

void choose_sizes() {
  MONITORINFO mi = primary_monitor();
  int mon_w = mi.rcMonitor.right - mi.rcMonitor.left, mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  int work_h = mi.rcWork.bottom - mi.rcWork.top;
  int game_w = read_int(game::kScreenWidth), game_h = read_int(game::kScreenHeight);
  int want_w = 0, want_h = 0;
  if (g_cfg.res_game) {
    want_w = game_w;
    want_h = game_h;
  } else if (g_cfg.res_w) {
    want_w = g_cfg.res_w;
    want_h = g_cfg.res_h;
  }
  LOGI("video: monitor %dx%d, TMNT2.ini asks %dx%dx%d, mode %s, aspect %s", mon_w, mon_h, game_w, game_h,
       read_int(game::kScreenDepth), mode_name(g_mode),
       g_cfg.shape == Config::Aspect::Wide ? "auto (widescreen)" : g_cfg.shape == Config::Aspect::Classic ? "4:3" : "stretch");
  // auto: the frame takes the monitor's shape; 4:3 and stretch: a 4:3 frame (shown with bars / stretched
  // over the window at Present). The cameras are the same in all three (the game's logic).
  bool wide_frame = g_cfg.shape == Config::Aspect::Wide;

  if (g_mode == Mode::Fullscreen) {
    std::vector<SIZE> modes = display_modes();
    if (!want_w) {
      if (wide_frame) {
        want_w = mon_w;
        want_h = mon_h;
      } else {  // the biggest 4:3 mode that fits the monitor
        for (const SIZE& s : modes)
          if (s.cx * 3 == s.cy * 4 && s.cy <= mon_h && s.cx * s.cy > want_w * want_h) {
            want_w = s.cx;
            want_h = s.cy;
          }
        if (!want_w) {
          want_w = mon_w;
          want_h = mon_h;
        }
      }
    }
    if (!modes.empty() && !has_mode(modes, want_w, want_h)) {
      LOGW("video: %dx%d is not a mode of this monitor, using %dx%d", want_w, want_h, mon_w, mon_h);
      want_w = mon_w;
      want_h = mon_h;
    }
    write_int(game::kScreenWidth, want_w);
    write_int(game::kScreenHeight, want_h);
    write_int(game::kScreenDepth, 32);
    write_int(game::kWindowMode, 0);
    LOGI("video: fullscreen %dx%dx32 (exclusive, the desktop resolution changes while the game runs)", want_w,
         want_h);
    return;
  }

  // Borderless / windowed: the game renders g_render_w x g_render_h.
  if (g_mode == Mode::Borderless) {
    if (want_w) {
      g_render_w = want_w;
      g_render_h = want_h;
    } else if (wide_frame) {
      g_render_w = mon_w;
      g_render_h = mon_h;
    } else {
      g_render_h = mon_h;
      g_render_w = (mon_h * 4 / 3) & ~1;
    }
  } else {
    int cw = g_cfg.window_w, ch = g_cfg.window_h;
    if (!cw) {
      ch = (work_h * 3 / 4) & ~1;
      cw = wide_frame ? (ch * mon_w / mon_h) & ~1 : (ch * 4 / 3) & ~1;
    }
    g_windowed_client = SIZE{cw, ch};
    g_render_w = want_w ? want_w : cw;
    g_render_h = want_h ? want_h : ch;
  }
  if (g_cfg.net_aspect > 0 && !g_cfg.net_wide) {
    // Netplay: the camera is part of the game's logic, so every PC renders the
    // session's aspect; a monitor of another shape gets bars. (With net_wide the
    // frame keeps the monitor's shape: camera.cpp widens each 3D draw, the logic
    // keeps the session's aspect.)
    double a = g_cfg.net_aspect;
    if (double(g_render_w) / g_render_h > a + 0.001)
      g_render_w = int(g_render_h * a + 0.5) & ~1;
    else if (double(g_render_w) / g_render_h < a - 0.001)
      g_render_h = int(g_render_w / a + 0.5) & ~1;
    if (g_mode == Mode::Windowed && !g_cfg.window_w) g_windowed_client = SIZE{g_render_w, g_render_h};
  }
  // The game's frame buffer is min(RenderWare's window video mode = the desktop,
  // ScreenWidth): a frame larger than the desktop would fill only part of it.
  if (g_render_w > mon_w || g_render_h > mon_h) {
    double s = std::min(double(mon_w) / g_render_w, double(mon_h) / g_render_h);
    int w = int(g_render_w * s) & ~1, h = int(g_render_h * s) & ~1;
    LOGW("video: %dx%d is larger than the desktop; the game renders %dx%d (scaled into the window)", g_render_w,
         g_render_h, w, h);
    g_render_w = w;
    g_render_h = h;
  }
  write_int(game::kScreenWidth, g_render_w);
  write_int(game::kScreenHeight, g_render_h);
  write_int(game::kScreenDepth, 32);
  write_int(game::kWindowMode, 1);
  LOGI("video: %s, the game renders %dx%d (window mode of the game switched on)", mode_name(g_mode), g_render_w,
       g_render_h);
}

int g_ini_w = 640, g_ini_h = 480, g_ini_depth = 32;  // what TMNT2.ini held at start

int __fastcall hkSettingInit(void* self, void* edx) {
  int r = oSettingInit(self, edx);
  g_ini_w = read_int(game::kScreenWidth);
  g_ini_h = read_int(game::kScreenHeight);
  g_ini_depth = read_int(game::kScreenDepth);
  choose_sizes();
  return r;
}

// A size the unmodified game can start with: its list keeps only 4:3 modes from
// 640x480 to 1024x768 at 16/32 bits; anything else stops it at start.
bool stock_mode(int w, int h, int depth) {
  return w >= 640 && h >= 480 && w <= 1024 && h <= 768 && w * 3 == h * 4 && (depth == 16 || depth == 32);
}

// The game writes the current size to TMNT2.ini when it quits. Without the mod
// the next start must still work, so TMNT2.ini only ever gets a stock size.
using SettingSave_t = int(__fastcall*)(void* self, void* edx);
SettingSave_t oSettingSave = nullptr;

int __fastcall hkSettingSave(void* self, void* edx) {
  int w = read_int(game::kScreenWidth), h = read_int(game::kScreenHeight), d = read_int(game::kScreenDepth);
  if (stock_mode(w, h, d)) return oSettingSave(self, edx);
  int sw = g_ini_w, sh = g_ini_h, sd = g_ini_depth;
  if (!stock_mode(sw, sh, sd)) {
    sw = 640;
    sh = 480;
    sd = 32;
  }
  LOGI("video: TMNT2.ini keeps %dx%dx%d (the game runs %dx%d, which the game without tmnt2net cannot start with)", sw,
       sh, sd, w, h);
  write_int(game::kScreenWidth, sw);
  write_int(game::kScreenHeight, sh);
  write_int(game::kScreenDepth, sd);
  int r = oSettingSave(self, edx);
  write_int(game::kScreenWidth, w);
  write_int(game::kScreenHeight, h);
  write_int(game::kScreenDepth, d);
  return r;
}

// --- CPCGraphicsDevice::FindMode (vtable +0x58) ---------------------------------
// The window mode still asks for an exclusive mode of the render size and stops
// with "Video Initialization Failed" when the monitor has no such mode. In a
// window any size works: answer with the current adapter.
using FindMode_t = int(__fastcall*)(uint8_t* self, void* edx, int w, int h, int depth, int current_only);
const FindMode_t oFindMode = reinterpret_cast<FindMode_t>(game::kFindMode);

int __fastcall hkFindMode(uint8_t* self, void* edx, int w, int h, int depth, int current_only) {
  int r = oFindMode(self, edx, w, h, depth, current_only);
  if (!windowed()) return r;
  int count = patch::read<int>(reinterpret_cast<uintptr_t>(self) + game::kVideoAdapterCount);
  int cur = patch::read<int>(reinterpret_cast<uintptr_t>(self) + game::kVideoAdapterIndex);
  int adapter = r >= 0 ? r : (cur >= 0 && cur < count ? cur : 0);
  // The adapter must offer RenderWare's window mode (recorded by the list builder).
  uint8_t* adapters = patch::read<uint8_t*>(reinterpret_cast<uintptr_t>(self) + 0x20);
  if (adapters && count > 0 && patch::read<int>(reinterpret_cast<uintptr_t>(adapters) + adapter * 0xA8 + 0x64) == 0) {
    LOGE("video: the adapter has no window mode; falling back to fullscreen");
    g_mode = Mode::Fullscreen;
    write_int(game::kWindowMode, 0);
    write_int(reinterpret_cast<uintptr_t>(self) + 0x2C, 1);  // CPCGraphicsDevice: fullscreen
    return r;
  }
  if (r < 0) {
    static bool logged = false;
    if (!logged) LOGI("video: %dx%d is not a monitor mode; fine in a window (adapter %d)", w, h, adapter);
    logged = true;
  }
  return adapter;
}

// --- CPCGraphicsDevice::ScreenWidth/Height (vtable +0x24/+0x28) ------------------
// The original returns the size of the selected fullscreen mode; CScreen, the
// 2D layer and the cameras take their size from it. In a window that mode is a
// leftover (640x480 at start), so the frame size is reported instead.
using ScreenSize_t = int(__fastcall*)(void* self, void* edx);
const ScreenSize_t oScreenWidth = reinterpret_cast<ScreenSize_t>(game::kScreenWidthFn);
const ScreenSize_t oScreenHeight = reinterpret_cast<ScreenSize_t>(game::kScreenHeightFn);

int __fastcall hkScreenWidth(void* self, void* edx) {
  return windowed() && g_render_w > 0 ? g_render_w : oScreenWidth(self, edx);
}
int __fastcall hkScreenHeight(void* self, void* edx) {
  return windowed() && g_render_h > 0 ? g_render_h : oScreenHeight(self, edx);
}

// --- window --------------------------------------------------------------------
LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
WNDPROC g_game_proc = nullptr;

void ensure_subclass(HWND hwnd) {
  if (!hwnd || g_game_proc) return;
  g_game_proc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&subclass_proc)));
  g_hwnd = hwnd;
}

DWORD windowed_style() {
  DWORD s = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  if (g_cfg.window_resize) s |= WS_THICKFRAME | WS_MAXIMIZEBOX;
  return s;
}

void show(HWND hwnd) {
  if (g_shown) return;
  g_shown = true;
  if (g_cfg.start_minimized) {
    ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
  } else if (g_cfg.no_activate) {
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  } else {
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    // Windows may keep a new window behind the one in front (its process started the game and kept the
    // focus, or the player clicked elsewhere while the game loaded): the game then waits unseen for a
    // click and has no keyboard. Once, the documented way round: joined to the input of the thread in
    // front for the call.
    HWND front = GetForegroundWindow();
    if (front != hwnd) {
      DWORD other = front ? GetWindowThreadProcessId(front, nullptr) : 0, me = GetCurrentThreadId();
      if (other && other != me && AttachThreadInput(me, other, TRUE)) {
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        AttachThreadInput(me, other, FALSE);
      }
    }
    LOGI("video: the game's window is %s", GetForegroundWindow() == hwnd ? "in front" : "NOT in front (click it: the game waits)");
  }
}

// [debug] no_activate: the game's own SetForegroundWindow calls do nothing.
using SetForegroundWindow_t = BOOL(WINAPI*)(HWND);
SetForegroundWindow_t oSetForegroundWindow = nullptr;
BOOL WINAPI hkSetForegroundWindow(HWND) { return TRUE; }

void apply(HWND hwnd) {
  if (!hwnd || !windowed()) return;
  MONITORINFO mi = monitor_of(hwnd);
  if (g_mode == Mode::Borderless) {
    const RECT& m = mi.rcMonitor;
    SetWindowLongA(hwnd, GWL_STYLE, WS_POPUP | (GetWindowLongA(hwnd, GWL_STYLE) & WS_VISIBLE));
    SetWindowLongA(hwnd, GWL_EXSTYLE, 0);
    SetWindowPos(hwnd, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    LOGI("video: borderless window %ldx%ld at %ld,%ld", m.right - m.left, m.bottom - m.top, m.left, m.top);
  } else {
    const RECT& work = mi.rcWork;
    int ww = work.right - work.left, wh = work.bottom - work.top;
    int cw = g_windowed_client.cx ? g_windowed_client.cx : g_render_w;
    int ch = g_windowed_client.cy ? g_windowed_client.cy : g_render_h;
    DWORD style = windowed_style();
    RECT r{0, 0, cw, ch};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = work.left + std::max(0, (ww - w) / 2), y = work.top + std::max(0, (wh - h) / 2);
    SetWindowLongA(hwnd, GWL_STYLE, style | (GetWindowLongA(hwnd, GWL_STYLE) & WS_VISIBLE));
    SetWindowLongA(hwnd, GWL_EXSTYLE, 0);
    SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_FRAMECHANGED);
    LOGI("video: window, client %dx%d at %d,%d%s", cw, ch, x, y, g_cfg.window_resize ? ", resizable" : "");
  }
  if (g_cfg.offscreen) {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64;
    // No taskbar button either: test copies stay out of the owner's way.
    SetWindowLongA(hwnd, GWL_EXSTYLE, (GetWindowLongA(hwnd, GWL_EXSTYLE) & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW);
    SetWindowPos(hwnd, nullptr, x, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    LOGI("video: [debug] offscreen, window moved to x=%d, no taskbar button", x);
  }
  g_last_client = RECT{};
  show(hwnd);
}

// CPCGraphicsDevice::PlaceWindow (vtable +0x5C), __thiscall(w, h).
using Place_t = void(__fastcall*)(void* self, void* edx, int w, int h);
const Place_t oPlace = reinterpret_cast<Place_t>(game::kPlaceWindow);

void __fastcall hkPlace(void* self, void* edx, int w, int h) {
  HWND hwnd = patch::read<HWND>(game::kHwnd);
  ensure_subclass(hwnd);
  if (!windowed()) {
    oPlace(self, edx, w, h);
    return;
  }
  // At start-up the game passes the size of a fullscreen mode; later calls come
  // from its own resolution menu and set the new frame size.
  if (_ReturnAddress() != reinterpret_cast<void*>(game::kPlaceFromInit) && w > 0 && h > 0 &&
      (w != g_render_w || h != g_render_h)) {
    LOGI("video: the game switches to %dx%d", w, h);
    g_render_w = w;
    g_render_h = h;
    if (g_mode == Mode::Windowed) g_windowed_client = SIZE{w, h};
  }
  apply(hwnd);
}

// RenderWare compares the window's client size with its frame on every camera
// update and rebuilds the frame (or stops) when they differ. TMNT2.exe's own
// GetClientRect therefore reports the frame size for the game window; D3D9
// and Windows still see the real window.
using GetClientRect_t = BOOL(WINAPI*)(HWND, LPRECT);
GetClientRect_t oGetClientRect = nullptr;

BOOL WINAPI hkGetClientRect(HWND hwnd, LPRECT r) {
  BOOL ok = oGetClientRect(hwnd, r);
  if (ok && r && windowed() && g_render_w > 0 && hwnd == patch::read<HWND>(game::kHwnd))
    *r = RECT{0, 0, g_render_w, g_render_h};
  return ok;
}

void keep_aspect(WPARAM edge, RECT* r) {
  if (g_render_w <= 0 || g_render_h <= 0) return;
  RECT frame{0, 0, 0, 0};
  AdjustWindowRectEx(&frame, windowed_style(), FALSE, 0);
  int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
  int cw = (r->right - r->left) - fw, ch = (r->bottom - r->top) - fh;
  double aspect = double(g_render_w) / g_render_h;
  bool by_height = edge == WMSZ_TOP || edge == WMSZ_BOTTOM;
  if (by_height)
    cw = (int)(ch * aspect + 0.5);
  else
    ch = (int)(cw / aspect + 0.5);
  if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
    r->top = r->bottom - (ch + fh);
  else
    r->bottom = r->top + ch + fh;
  if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
    r->left = r->right - (cw + fw);
  else
    r->right = r->left + cw + fw;
}

bool filter(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
  switch (msg) {
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_CLOSE) LOGI("video: close asked from the system menu / taskbar / Alt+F4");
      return false;
    case WM_CLOSE:  // the game ignores it in fullscreen and leaves a window-less process in a window
      LOGI("video: window closed (Alt+F4 / close button), quitting%s",
           (InSendMessageEx(nullptr) & ISMEX_SEND) ? " (sent by another thread or program)" : "");
      fixes::request_quit();
      *result = 0;
      return true;
    case WM_SYSKEYDOWN:  // Alt+Enter: windowed <-> borderless
      if (g_start_windowed && wp == VK_RETURN && (lp & (1 << 29)) && !(lp & (1 << 30))) {
        if (g_mode == Mode::Windowed) {
          RECT c{};
          GetClientRect(hwnd, &c);
          if (IsZoomed(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
          else if (c.right > 0)
            g_windowed_client = SIZE{c.right, c.bottom};
        }
        g_mode = g_mode == Mode::Windowed ? Mode::Borderless : Mode::Windowed;
        LOGI("video: Alt+Enter -> %s", mode_name(g_mode));
        apply(hwnd);
        *result = 0;
        return true;
      }
      return false;
    case WM_SYSCHAR:  // no beep for Alt+Enter
      if (wp == VK_RETURN) {
        *result = 0;
        return true;
      }
      return false;
    case WM_SETCURSOR:  // the game hides the cursor everywhere, title bar included
      if (LOWORD(lp) != HTCLIENT || (windowed() && !g_cfg.hide_cursor)) {
        *result = DefWindowProcA(hwnd, msg, wp, lp);
        return true;
      }
      return false;
    case WM_SIZING:
      if (g_mode == Mode::Windowed && g_cfg.window_resize) {
        keep_aspect(wp, reinterpret_cast<RECT*>(lp));
        *result = TRUE;
        return true;
      }
      return false;
    case WM_SIZE:
      g_last_client = RECT{};
      return false;
    case WM_MOUSEWHEEL:  // the stage camera's zoom (the game has no use for the wheel)
      camera::wheel(GET_WHEEL_DELTA_WPARAM(wp));
      return false;
    case WM_MBUTTONDOWN:
      camera::reset_zoom();
      return false;
    case WM_ACTIVATE: {  // the game pauses while inactive ([app+0x74] = 0)
      static int said = 0;
      if (said++ < 30)
        LOGI("video: the game's window is %s", LOWORD(wp) == WA_INACTIVE ? "not in front any more" : "in front again");
    }
      if ((g_cfg.background_run || netplay::active()) && windowed() && LOWORD(wp) == WA_INACTIVE) {
        LOGD("video: window inactive, the game keeps running");
        *result = DefWindowProcA(hwnd, msg, wp, lp);
        return true;
      }
      return false;
    default:
      return false;
  }
}

LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  LRESULT r = 0;
  if (filter(hwnd, msg, wp, lp, &r)) return r;
  return CallWindowProcA(g_game_proc, hwnd, msg, wp, lp);
}

// --- Direct3D 9 ----------------------------------------------------------------
using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using CreateDevice_t = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                        IDirect3DDevice9**);
using Reset_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present_t = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
Direct3DCreate9_t oDirect3DCreate9 = nullptr;
CreateDevice_t oCreateDevice = nullptr;
Reset_t oReset = nullptr;
Present_t oPresent = nullptr;

IDirect3D9* g_d3d = nullptr;  // CreateDevice's: multisampling checks for Reset too
UINT g_adapter = 0;
HWND g_device_hwnd = nullptr;
D3DMULTISAMPLE_TYPE g_msaa = D3DMULTISAMPLE_NONE;

// Antialiasing (MSAA) needs the DISCARD swap effect, which presents the whole back buffer onto the
// whole client area: only when the frame fills the window (no bars, no scaling).
D3DMULTISAMPLE_TYPE msaa_for(const D3DPRESENT_PARAMETERS& p) {
  if (g_cfg.antialiasing < 2 || !g_d3d || !p.Windowed) return D3DMULTISAMPLE_NONE;
  RECT c{};
  if (!g_device_hwnd || !GetClientRect(g_device_hwnd, &c) || (UINT)c.right != p.BackBufferWidth ||
      (UINT)c.bottom != p.BackBufferHeight) {
    LOGI("video: antialiasing off: the frame %ux%u does not fill the window (%ldx%ld)", p.BackBufferWidth,
         p.BackBufferHeight, c.right, c.bottom);
    return D3DMULTISAMPLE_NONE;
  }
  for (int n = g_cfg.antialiasing; n >= 2; n /= 2) {
    auto t = (D3DMULTISAMPLE_TYPE)n;
    D3DFORMAT bb = p.BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : p.BackBufferFormat;
    if (FAILED(g_d3d->CheckDeviceMultiSampleType(g_adapter, D3DDEVTYPE_HAL, bb, TRUE, t, nullptr))) continue;
    if (p.EnableAutoDepthStencil &&
        FAILED(g_d3d->CheckDeviceMultiSampleType(g_adapter, D3DDEVTYPE_HAL, p.AutoDepthStencilFormat, TRUE, t,
                                                 nullptr)))
      continue;
    return t;
  }
  LOGI("video: antialiasing not supported by this video card");
  return D3DMULTISAMPLE_NONE;
}

bool adjust(const D3DPRESENT_PARAMETERS& game, D3DPRESENT_PARAMETERS& out) {
  if (!windowed() && g_cfg.vsync < 0) return false;
  out = game;
  if (windowed()) {
    out.Windowed = TRUE;
    out.FullScreen_RefreshRateInHz = 0;
    if (g_render_w > 0) {
      out.BackBufferWidth = g_render_w;
      out.BackBufferHeight = g_render_h;
    }
    // Present into a rectangle (bars, any window size) needs the COPY swap
    // effect: one back buffer, no multisampling.
    g_copy = game.MultiSampleType == D3DMULTISAMPLE_NONE;
    g_msaa = g_copy && game.EnableAutoDepthStencil ? msaa_for(out) : D3DMULTISAMPLE_NONE;
    if (g_msaa != D3DMULTISAMPLE_NONE) {  // the whole frame onto the whole window, smoothed edges
      g_copy = false;
      out.MultiSampleType = g_msaa;
      out.MultiSampleQuality = 0;
      out.SwapEffect = D3DSWAPEFFECT_DISCARD;
      out.BackBufferCount = 1;
      out.Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    } else if (g_copy) {
      out.SwapEffect = D3DSWAPEFFECT_COPY;
      out.BackBufferCount = 1;
    }
  }
  if (g_cfg.vsync >= 0)
    out.PresentationInterval = g_cfg.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
  return true;
}

void log_params(const char* what, const D3DPRESENT_PARAMETERS& p) {
  LOGI("d3d9 %s: %ux%u fmt %d, %s, swap %d x%u, interval 0x%X, msaa %d, depth %s %d, flags 0x%X", what,
       p.BackBufferWidth, p.BackBufferHeight, (int)p.BackBufferFormat, p.Windowed ? "windowed" : "fullscreen",
       (int)p.SwapEffect, p.BackBufferCount, p.PresentationInterval, (int)p.MultiSampleType,
       p.EnableAutoDepthStencil ? "auto" : "own", (int)p.AutoDepthStencilFormat, (unsigned)p.Flags);
}

bool g_repeat_present = false;  // present_now: the frame on screen again, not a new one

HRESULT WINAPI hkPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty);
HRESULT WINAPI hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp);

IDirect3DDevice9* g_dev = nullptr;  // the game's device (one for the whole run)

// Anisotropic filtering: the game's bilinear minification becomes anisotropic (sharper floors and
// walls at an angle), as far as the card allows.
using SetSamplerState_t = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
SetSamplerState_t oSetSamplerState = nullptr;
DWORD g_aniso = 0;  // 0: off

HRESULT WINAPI hkSetSamplerState(IDirect3DDevice9* dev, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
  if (g_aniso > 1 && type == D3DSAMP_MINFILTER && value == D3DTEXF_LINEAR) {
    oSetSamplerState(dev, sampler, D3DSAMP_MAXANISOTROPY, g_aniso);
    value = D3DTEXF_ANISOTROPIC;
  }
  return oSetSamplerState(dev, sampler, type, value);
}

// A game that joined a running session replays it from its first frame. The game's code must run for
// every one of those frames - that is what makes the game the same on every PC - but none of them is
// ever looked at: the draw calls themselves are left out (the card and the driver are most of what such
// a frame costs), and the window shows a loading screen (hkPresent). tmnt2net's own drawing goes on.
bool g_own_draw = false;  // hkPresent is drawing: tmnt2net's own calls
bool skip_draw() { return netplay::catching_up() && !g_own_draw; }

using DrawPrimitive_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitive_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUP_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUP_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
                                                  D3DFORMAT, const void*, UINT);
DrawPrimitive_t oDrawPrimitive = nullptr;
DrawIndexedPrimitive_t oDrawIndexedPrimitive = nullptr;
DrawPrimitiveUP_t oDrawPrimitiveUP = nullptr;
DrawIndexedPrimitiveUP_t oDrawIndexedPrimitiveUP = nullptr;

HRESULT WINAPI hkDrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT count) {
  return skip_draw() ? D3D_OK : oDrawPrimitive(d, t, start, count);
}
HRESULT WINAPI hkDrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT min, UINT num, UINT start,
                                      UINT count) {
  return skip_draw() ? D3D_OK : oDrawIndexedPrimitive(d, t, base, min, num, start, count);
}
HRESULT WINAPI hkDrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride) {
  return skip_draw() ? D3D_OK : oDrawPrimitiveUP(d, t, count, data, stride);
}
HRESULT WINAPI hkDrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT min, UINT num, UINT count,
                                        const void* index, D3DFORMAT fmt, const void* data, UINT stride) {
  return skip_draw() ? D3D_OK : oDrawIndexedPrimitiveUP(d, t, min, num, count, index, fmt, data, stride);
}

void hook_device(IDirect3DDevice9* dev) {
  g_dev = dev;
  if (oPresent) return;
  void** vt = *reinterpret_cast<void***>(dev);
  if (g_cfg.anisotropy > 1) {
    D3DCAPS9 caps{};
    if (SUCCEEDED(dev->GetDeviceCaps(&caps)) && (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) &&
        caps.MaxAnisotropy > 1) {
      g_aniso = std::min<DWORD>((DWORD)g_cfg.anisotropy, caps.MaxAnisotropy);
      patch::hook("IDirect3DDevice9::SetSamplerState (anisotropic filtering)", reinterpret_cast<uintptr_t>(vt[69]),
                  (void*)&hkSetSamplerState, (void**)&oSetSamplerState);
      LOGI("video: anisotropic filtering x%u", (unsigned)g_aniso);
    }
  }
  if (!g_cfg.replay_draws) {
    patch::hook("IDirect3DDevice9::DrawPrimitive (not while a session is replayed)", reinterpret_cast<uintptr_t>(vt[81]),
                (void*)&hkDrawPrimitive, (void**)&oDrawPrimitive);
    patch::hook("IDirect3DDevice9::DrawIndexedPrimitive", reinterpret_cast<uintptr_t>(vt[82]),
                (void*)&hkDrawIndexedPrimitive, (void**)&oDrawIndexedPrimitive);
    patch::hook("IDirect3DDevice9::DrawPrimitiveUP", reinterpret_cast<uintptr_t>(vt[83]), (void*)&hkDrawPrimitiveUP,
                (void**)&oDrawPrimitiveUP);
    patch::hook("IDirect3DDevice9::DrawIndexedPrimitiveUP", reinterpret_cast<uintptr_t>(vt[84]),
                (void*)&hkDrawIndexedPrimitiveUP, (void**)&oDrawIndexedPrimitiveUP);
  }
  patch::hook("IDirect3DDevice9::Reset", reinterpret_cast<uintptr_t>(vt[16]), (void*)&hkReset, (void**)&oReset);
  patch::hook("IDirect3DDevice9::Present", reinterpret_cast<uintptr_t>(vt[17]), (void*)&hkPresent,
              (void**)&oPresent);
}

HRESULT WINAPI hkCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                              D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
  D3DPRESENT_PARAMETERS mine{};
  D3DPRESENT_PARAMETERS* use = pp;
  g_d3d = d3d;
  g_adapter = adapter;
  g_device_hwnd = pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus;
  if (pp) {
    log_params("CreateDevice (game)", *pp);
    if (adjust(*pp, mine)) {
      use = &mine;
      log_params("CreateDevice (tmnt2net)", mine);
    }
  }
  if (g_cfg.no_activate && use && !use->Windowed) {
    // Test runs must never switch the owner's desktop to another resolution.
    LOGE("debug: no_activate and the game asks for a fullscreen device; ending the test run");
    TerminateProcess(GetCurrentProcess(), 4);
  }
  HRESULT hr = oCreateDevice(d3d, adapter, type, focus, flags, use, out);
  if (FAILED(hr) && use != pp) {
    LOGE("d3d9 CreateDevice failed (0x%08X) with tmnt2net's parameters; retrying with the game's", (unsigned)hr);
    g_copy = false;
    g_msaa = D3DMULTISAMPLE_NONE;
    hr = oCreateDevice(d3d, adapter, type, focus, flags, pp, out);
  } else if (SUCCEEDED(hr) && use != pp) {
    pp->BackBufferWidth = mine.BackBufferWidth;
    pp->BackBufferHeight = mine.BackBufferHeight;
    pp->BackBufferFormat = mine.BackBufferFormat;
  }
  if (FAILED(hr)) {
    LOGE("d3d9 CreateDevice failed: 0x%08X", (unsigned)hr);
    return hr;
  }
  HWND hwnd = pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus;
  ensure_subclass(hwnd);
  if (out && *out) hook_device(*out);
  return hr;
}

HRESULT WINAPI hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
  D3DPRESENT_PARAMETERS mine{};
  postfx::release();  // (its copy of the frame is a default-pool resource: gone before a Reset)
  if (pp && adjust(*pp, mine)) {
    log_params("Reset (tmnt2net)", mine);
    HRESULT hr = oReset(dev, &mine);
    if (SUCCEEDED(hr)) {
      pp->BackBufferWidth = mine.BackBufferWidth;
      pp->BackBufferHeight = mine.BackBufferHeight;
      pp->BackBufferFormat = mine.BackBufferFormat;
      return hr;
    }
    LOGE("d3d9 Reset failed (0x%08X) with tmnt2net's parameters; retrying with the game's", (unsigned)hr);
    g_copy = false;
    g_msaa = D3DMULTISAMPLE_NONE;
  }
  return oReset(dev, pp);
}

// The frame fitted into the client area with its aspect kept; bars painted black.
const RECT* fit(HWND hwnd) {
  RECT c{};
  if (!GetClientRect(hwnd, &c) || c.right <= 0 || c.bottom <= 0 || g_render_w <= 0) return nullptr;
  double s = std::min(double(c.right) / g_render_w, double(c.bottom) / g_render_h);
  int w = (int)(g_render_w * s + 0.5), h = (int)(g_render_h * s + 0.5);
  if (std::abs(w - c.right) <= 1 && std::abs(h - c.bottom) <= 1) {
    g_dest = c;
    return &g_dest;
  }
  g_dest = RECT{(c.right - w) / 2, (c.bottom - h) / 2, 0, 0};
  g_dest.right = g_dest.left + w;
  g_dest.bottom = g_dest.top + h;
  bool changed = std::memcmp(&c, &g_last_client, sizeof(c)) != 0;
  if (changed || ++g_bar_frames >= 60) {
    g_bar_frames = 0;
    g_last_client = c;
    if (HDC dc = GetDC(hwnd)) {
      HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
      RECT bars[4] = {{0, 0, c.right, g_dest.top},
                      {0, g_dest.bottom, c.right, c.bottom},
                      {0, g_dest.top, g_dest.left, g_dest.bottom},
                      {g_dest.right, g_dest.top, c.right, g_dest.bottom}};
      for (const RECT& b : bars)
        if (b.right > b.left && b.bottom > b.top) FillRect(dc, &b, black);
      ReleaseDC(hwnd, dc);
    }
    if (changed) LOGD("video: frame %dx%d shown at %ld,%ld size %dx%d", g_render_w, g_render_h, g_dest.left, g_dest.top, w, h);
  }
  return &g_dest;
}

// --- frame limiter ------------------------------------------------------------------
// The game advances its logic once per frame and paces itself with Sleep() to a
// frame time rounded down to 16 ms (62.5 Hz). With Windows' default 15.6 ms timer
// that overshoots (about 53 fps here, the game ran slow); with a 1 ms timer it runs
// 4% fast. timeBeginPeriod(1) plus an exact limit at Present gives 60.
void limit_fps() {
  static LARGE_INTEGER freq{}, next{};
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LONGLONG period = freq.QuadPart / g_cfg.fps_limit;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!next.QuadPart || now.QuadPart - next.QuadPart > period * 4) next = now;  // start / long stall
  for (;;) {
    QueryPerformanceCounter(&now);
    LONGLONG left = next.QuadPart - now.QuadPart;
    if (left <= 0) break;
    if (left * 1000 / freq.QuadPart >= 2)
      Sleep(1);
    else
      YieldProcessor();
  }
  next.QuadPart += period;
}

// --- [debug] shots: the frame from the back buffer, never the desktop -------------
std::vector<unsigned> g_shot_frames;
unsigned g_frames = 0;
std::atomic<int> g_shot_tag{-1};  // tenths of a second, -1 = none

void save_shot(IDirect3DDevice9* dev, unsigned frame, const wchar_t* name_fmt = L"\\tmnt2net_f%05u.bmp") {
  IDirect3DSurface9* bb = nullptr;
  IDirect3DSurface9* sys = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  D3DSURFACE_DESC d{};
  bb->GetDesc(&d);
  IDirect3DSurface9* src = bb;
  IDirect3DSurface9* resolved = nullptr;  // a multisampled back buffer is read through a plain copy
  if (d.MultiSampleType != D3DMULTISAMPLE_NONE &&
      SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) &&
      SUCCEEDED(dev->StretchRect(bb, nullptr, resolved, nullptr, D3DTEXF_NONE)))
    src = resolved;
  bool ok = (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) &&
            SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
            SUCCEEDED(dev->GetRenderTargetData(src, sys));
  if (resolved) resolved->Release();
  D3DLOCKED_RECT lr{};
  if (ok && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    std::wstring dir = module_dir() + L"screenshots";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf(name, 64, name_fmt, frame);
    std::wstring path = dir + name;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = (LONG)d.Width;
    ih.biHeight = -(LONG)d.Height;  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    DWORD row = d.Width * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + row * d.Height;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD w = 0;
      WriteFile(f, &fh, sizeof(fh), &w, nullptr);
      WriteFile(f, &ih, sizeof(ih), &w, nullptr);
      for (UINT y = 0; y < d.Height; ++y)
        WriteFile(f, static_cast<const uint8_t*>(lr.pBits) + (size_t)y * lr.Pitch, row, &w, nullptr);
      CloseHandle(f);
      LOGI("debug: frame %u saved (%ux%u) to %ls", frame, d.Width, d.Height, path.c_str());
    }
    sys->UnlockRect();
  } else {
    LOGW("debug: frame %u not saved (back buffer format %d)", frame, (int)d.Format);
  }
  if (sys) sys->Release();
  bb->Release();
}

// [log] level=debug: once a second, fps and where a frame's time goes (the game's
// own work, tmnt2net's limiter, the driver's Present).
struct FrameStats {
  LARGE_INTEGER freq{}, second{}, last_end{};
  double game = 0, wait = 0, present = 0;
  unsigned frames = 0;
} g_fs;

double ms_between(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
  return double(b.QuadPart - a.QuadPart) * 1000.0 / double(g_fs.freq.QuadPart);
}

HRESULT WINAPI hkPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
  bool stats = tlog::enabled(tlog::Debug);
  LARGE_INTEGER t0{}, t1{}, t2{};
  if (stats) {
    if (!g_fs.freq.QuadPart) QueryPerformanceFrequency(&g_fs.freq);
    QueryPerformanceCounter(&t0);
    if (!g_fs.second.QuadPart) g_fs.second = g_fs.last_end = t0;
  }
  struct OwnDraw {
    OwnDraw() { g_own_draw = true; }
    ~OwnDraw() { g_own_draw = false; }
  } own_draw;  // (tmnt2net's own drawing is never left out)
  // Joining a running network game: replay it as fast as the game goes, and show none of it: ten times
  // a second a dark frame with the panel that says how far the replay is, so Present and vsync do not
  // hold it back and nothing flickers. (The start-up run at once: every 30th frame, as it is.)
  bool loading = netplay::catching_up() && !g_cfg.replay_draws;
  if (loading) {
    static DWORD shown = 0;
    DWORD t = GetTickCount();
    if (t - shown < 100) {
      input::on_frame();
      return D3D_OK;
    }
    shown = t;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
      dev->ColorFill(bb, nullptr, D3DCOLOR_XRGB(6, 12, 8));
      bb->Release();
    }
  } else if (netplay::catching_up() || sim::starting_up()) {
    static unsigned skipped = 0;
    if (++skipped % 30) {
      input::on_frame();
      return D3D_OK;
    }
  } else if (g_cfg.fps_limit > 0 && !netplay::hurrying() && !sim::starting_up()) {
    limit_fps();
  }
  if (stats) QueryPerformanceCounter(&t1);
  ++g_frames;
  if (g_frames == 1 || g_frames == 60 || g_frames % 3600 == 0) LOGI("video: %u frames presented", g_frames);
  // The game's frame crisper, its colours fuller; tmnt2net's words go over it as they are. Not a frame
  // shown again while the game waits (a network game waiting for a player): it has been through this
  // already, and every pass more sharpened and coloured it further until it broke up.
  if (!g_repeat_present && !loading) postfx::apply(dev);
  {
    int fw = 0, fh = 0;
    if (frame_size(fw, fh)) overlay::draw(dev, fw, fh);  // before the [debug] shots: they show it too
  }
  if (!g_shot_frames.empty() && std::find(g_shot_frames.begin(), g_shot_frames.end(), g_frames) != g_shot_frames.end())
    save_shot(dev, g_frames);
  input::on_frame();
  int tag = g_shot_tag.exchange(-1);
  if (tag >= 0) save_shot(dev, (unsigned)tag, L"\\tmnt2net_t%05u.bmp");
  if (windowed() && g_copy && !dst && g_cfg.shape != Config::Aspect::Stretch) {  // (stretch: over the whole window)
    HWND hwnd = wnd ? wnd : g_hwnd.load();
    if (hwnd && !IsIconic(hwnd))
      if (const RECT* d = fit(hwnd)) dst = d;
  }
  HRESULT hr = oPresent(dev, src, dst, wnd, dirty);
  if (stats) {
    QueryPerformanceCounter(&t2);
    g_fs.game += ms_between(g_fs.last_end, t0);
    g_fs.wait += ms_between(t0, t1);
    g_fs.present += ms_between(t1, t2);
    g_fs.last_end = t2;
    ++g_fs.frames;
    double span = ms_between(g_fs.second, t2);
    if (span >= 1000.0) {
      LOGD("video: %.1f fps; per frame: game %.1f ms, limiter %.1f ms, Present %.1f ms", g_fs.frames * 1000.0 / span,
           g_fs.game / g_fs.frames, g_fs.wait / g_fs.frames, g_fs.present / g_fs.frames);
      g_fs.game = g_fs.wait = g_fs.present = 0;
      g_fs.frames = 0;
      g_fs.second = t2;
    }
  }
  return hr;
}

// A movie's frames wait for real time in the CRI server; a player replaying a running network
// game (netplay::catching_up) does not: the movie's end comes from the session's input anyway.
void __cdecl movie_wait() {
  if (!netplay::catching_up() && !sim::starting_up()) reinterpret_cast<void(__cdecl*)()>(game::kMovieWait)();
}

IDirect3D9* WINAPI hkDirect3DCreate9(UINT sdk) {
  IDirect3D9* d3d = oDirect3DCreate9(sdk);
  if (d3d && !oCreateDevice) {
    void** vt = *reinterpret_cast<void***>(d3d);
    patch::hook("IDirect3D9::CreateDevice", reinterpret_cast<uintptr_t>(vt[16]), (void*)&hkCreateDevice,
                (void**)&oCreateDevice);
  }
  return d3d;
}

}  // namespace

bool frame_size(int& w, int& h) {
  if (windowed() && g_render_w > 0) {
    w = g_render_w;
    h = g_render_h;
  } else {
    w = read_int(game::kScreenWidth);
    h = read_int(game::kScreenHeight);
  }
  return w > 0 && h > 0;
}

void request_shot(double tag_seconds) { g_shot_tag = (int)(tag_seconds * 10 + 0.5); }

IDirect3DDevice9* device() { return g_dev; }

void set_background_run(bool run) { g_cfg.background_run = run; }

std::vector<Size> resolutions() {
  std::vector<SIZE> all;
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
    if (dm.dmBitsPerPel != 32 || dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
    if (!has_mode(all, (int)dm.dmPelsWidth, (int)dm.dmPelsHeight)) all.push_back(SIZE{(LONG)dm.dmPelsWidth, (LONG)dm.dmPelsHeight});
  }
  std::sort(all.begin(), all.end(), [](const SIZE& a, const SIZE& b) {
    return a.cx * a.cy < b.cx * b.cy || (a.cx * a.cy == b.cx * b.cy && a.cx < b.cx);
  });
  std::vector<Size> out;
  for (const SIZE& s : all) out.push_back(Size{(int)s.cx, (int)s.cy});
  return out;
}

bool set_resolution(int w, int h) {
  if (w <= 0 || h <= 0) {  // the automatic size, as choose_sizes at the start
    HWND hwnd = g_hwnd.load();
    MONITORINFO mi = hwnd ? monitor_of(hwnd) : primary_monitor();
    int mon_w = mi.rcMonitor.right - mi.rcMonitor.left, mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    bool wide_frame = g_cfg.shape == Config::Aspect::Wide;
    if (g_mode == Mode::Windowed) {
      w = g_cfg.window_w;
      h = g_cfg.window_h;
      if (!w) {
        h = ((mi.rcWork.bottom - mi.rcWork.top) * 3 / 4) & ~1;
        w = wide_frame ? (h * mon_w / mon_h) & ~1 : (h * 4 / 3) & ~1;
      }
    } else if (wide_frame || g_mode == Mode::Fullscreen) {
      w = mon_w;
      h = mon_h;
    } else {
      h = mon_h;
      w = (mon_h * 4 / 3) & ~1;
    }
  }
  int fw = 0, fh = 0;
  if (frame_size(fw, fh) && fw == w && fh == h) return true;
  LOGI("video: the settings page asks for a frame of %dx%d (now %dx%d)", w, h, fw, fh);
  bool ok = reinterpret_cast<bool(__cdecl*)(int, int, int)>(game::kChangeVideoMode)(w, h, 32);
  widescreen::frame_changed();  // (with the game's own 2D reset, as after its own change of mode)
  if (!ok) LOGW("video: the game could not switch to %dx%d", w, h);
  return ok;
}

bool present_now(bool black) {
  if (!g_dev) return false;
  struct Repeat {
    Repeat() { g_repeat_present = true; }
    ~Repeat() { g_repeat_present = false; }
  } repeat;
  if (black) {
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
      g_dev->ColorFill(bb, nullptr, D3DCOLOR_XRGB(0, 0, 0));
      bb->Release();
    }
  }
  return SUCCEEDED(g_dev->Present(nullptr, nullptr, nullptr, nullptr));  // through hkPresent: overlay, bars, pacing
}

void install(const Config& cfg) {
  g_cfg = cfg;
  g_mode = cfg.mode == "windowed" ? Mode::Windowed : cfg.mode == "fullscreen" ? Mode::Fullscreen : Mode::Borderless;
  g_start_windowed = g_mode != Mode::Fullscreen;
  LOGI("video: mode %s%s%s, fps limit %d", mode_name(g_mode), cfg.background_run ? ", keeps running in the background" : "",
       cfg.vsync < 0 ? "" : cfg.vsync ? ", vsync on" : ", vsync off", cfg.fps_limit);
  timeBeginPeriod(1);  // Sleep() precision for the game's threads and the limiter
  {
    uint8_t now[5] = {0xE8};
    int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&movie_wait) - (game::kMovieWaitCall + 5));
    std::memcpy(now + 1, &rel, 4);
    patch::replace("movies: no real-time wait while catching up a network game", game::kMovieWaitCall,
                   game::kMovieWaitCallOld, now, sizeof(now));
  }
  if (cfg.fps_limit > 0)  // one exact limiter at Present instead of the game's 16 ms wait + Present
    patch::replace("frame pacing (exact limit at Present)", game::kFrameTimerWait, game::kFrameTimerWaitOld,
                   game::kFrameTimerWaitNew, sizeof(game::kFrameTimerWaitNew));

  if (cfg.all_modes) {
    patch::replace("video mode filter (all resolutions)", game::kModeFilter, game::kModeFilterOld,
                   game::kModeFilterNew, sizeof(game::kModeFilterNew));
    fill_resolution_rows();
  }

  if (!patch::bytes_are(game::kSettingInit, game::kSettingInitHead, sizeof(game::kSettingInitHead)) ||
      !patch::hook("PCSetting init", game::kSettingInit, (void*)&hkSettingInit, (void**)&oSettingInit)) {
    LOGE("video: cannot hook the game's settings; resolution and window modes stay as in TMNT2.ini");
    return;
  }

  if (!patch::bytes_are(game::kSettingSave, game::kSettingSaveHead, sizeof(game::kSettingSaveHead)) ||
      !patch::hook("PCSetting save (TMNT2.ini)", game::kSettingSave, (void*)&hkSettingSave, (void**)&oSettingSave))
    LOGW("video: TMNT2.ini save not hooked; without tmnt2net the game may not start with the size saved there");

  bool ok = patch::slot("PlaceWindow", game::kPlaceWindowSlot, game::kPlaceWindow, (void*)&hkPlace);
  if (g_start_windowed) {
    ok = ok && patch::slot("FindMode", game::kFindModeSlot, game::kFindMode, (void*)&hkFindMode) &&
         patch::slot("ScreenWidth", game::kScreenWidthSlot, game::kScreenWidthFn, (void*)&hkScreenWidth) &&
         patch::slot("ScreenHeight", game::kScreenHeightSlot, game::kScreenHeightFn, (void*)&hkScreenHeight) &&
         patch::import("user32.dll", "GetClientRect", (void*)&hkGetClientRect, (void**)&oGetClientRect);
    if (!ok) {
      LOGE("video: the game's window code was not recognised; staying in fullscreen");
      g_mode = Mode::Fullscreen;
      g_start_windowed = false;
    }
  }
  patch::import("d3d9.dll", "Direct3DCreate9", (void*)&hkDirect3DCreate9, (void**)&oDirect3DCreate9);

  // --- [debug] test-run helpers ---
  if (cfg.no_activate && g_start_windowed)
    patch::import("user32.dll", "SetForegroundWindow", (void*)&hkSetForegroundWindow, (void**)&oSetForegroundWindow);
  for (const char* p = cfg.shots.c_str(); *p;) {
    char* end = nullptr;
    unsigned long f = strtoul(p, &end, 10);
    if (end == p) {
      ++p;
      continue;
    }
    if (f) g_shot_frames.push_back((unsigned)f);
    p = end;
  }
  if (cfg.autoquit > 0) {
    LOGI("debug: the game quits after %d s", cfg.autoquit);
    HANDLE t = CreateThread(
        nullptr, 0,
        [](void* secs) -> DWORD {
          Sleep((DWORD)(uintptr_t)secs * 1000);
          HWND hwnd = patch::read<HWND>(game::kHwnd);
          LOGI("debug: autoquit (%u frames presented)", g_frames);
          if (hwnd) PostMessageA(hwnd, WM_CLOSE, 0, 0);  // same path as the close button
          Sleep(15000);
          LOGW("debug: the game did not quit within 15 s, terminating");
          TerminateProcess(GetCurrentProcess(), 3);
          return 0;
        },
        (void*)(uintptr_t)cfg.autoquit, 0, nullptr);
    if (t) CloseHandle(t);
  }
}

}  // namespace video
