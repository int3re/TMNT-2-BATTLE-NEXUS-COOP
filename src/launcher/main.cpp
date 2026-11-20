// tmnt2net-launcher.exe: the window, the pointer and the keys, what each press does, the work in the
// background (asking GitHub, downloading, installing), and the modes without a window:
//   --shot <screen> <file.png> [--scale N] [--accent 0..3]   a screen of the design as a picture
//   --check [--out file]                                     ask for the newest release, write what came
//   --where [--out file]                                     where the game is found from here
// Tests: --settings <ini>, --cache <dir>, --repo <url> (the repository served from elsewhere), --pick <dir>
// (the answer of the folder dialog), --drive "<script>" (the window stays hidden; wait:ms, click:name,
// until:screen:ms, type:text, shot:file, quit), --out <file> (a log of what happened).
#include "launcher/launcher.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <cstdio>
#include <memory>
#include <thread>

#include "launcher/res.h"

namespace lz {
namespace {

const wchar_t kClass[] = L"tmnt2net-launcher";
const UINT WM_APP_CHECK = WM_APP + 1, WM_APP_STAGE = WM_APP + 2, WM_APP_PROGRESS = WM_APP + 3, WM_APP_GAME = WM_APP + 4,
           WM_APP_DONE = WM_APP + 5;

Model g;
HWND g_hwnd = nullptr;
ID2D1HwndRenderTarget* g_rt = nullptr;
std::unique_ptr<gfx::Canvas> g_canvas;
std::vector<Hit> g_hits;
std::atomic<bool> g_cancel{false};
std::atomic<int> g_gen{0};       // checks: only the newest one's answer counts
bool g_job = false;              // a download / install / removal runs
bool g_startup_check = false;    // the check of the start (it may install by itself)
net::Route g_route;              // the way to GitHub that worked last
std::wstring g_setup_path;       // the installer of the last try
Pending g_last_job = Pending::None;
ULONGLONG g_start = 0, g_confirm_until = 0, g_launch_until = 0, g_quit_at = 0;
bool g_tracking = false, g_dirty = true;

// tests
bool g_drive = false;
std::wstring g_out, g_pick;
std::vector<std::wstring> g_script;
size_t g_script_at = 0;
ULONGLONG g_script_wait = 0, g_script_deadline = 0;

const wchar_t* screen_name(Screen s) {
  switch (s) {
    case Screen::Checking: return L"checking";
    case Screen::Update: return L"update";
    case Screen::Downloading: return L"downloading";
    case Screen::Ready: return L"ready";
    case Screen::Install: return L"install";
    case Screen::NoGame: return L"nogame";
    case Screen::Offline: return L"offline";
    case Screen::Running: return L"running";
    case Screen::Problem: return L"problem";
    default: return L"settings";
  }
}

void logf(const std::wstring& line) {
  if (g_out.empty()) return;
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_out.c_str(), L"ab") || !f) return;
  std::string s = core::narrow(L"[" + std::to_wstring(GetTickCount64() - g_start) + L"] " + line + L"\n");
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}

void dirty() {
  g_dirty = true;
  if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void set_screen(Screen s) {
  if (g.screen != s) logf(std::wstring(L"screen ") + screen_name(g.screen) + L" -> " + screen_name(s));
  g.screen = s;
  g.net.clear();
  dirty();
}

void flash(const std::wstring& text, int ms = 4500) {
  g.flash = text;
  g.flash_until = GetTickCount64() + ms;
  logf(L"note: " + text);
  dirty();
}

void refresh_installed() {
  g.has_game = core::has_game(g.set.game_dir);
  g.exe_ok = !g.has_game || core::exe_supported(g.set.game_dir);
  g.inst = g.has_game ? core::read_installed(g.set.game_dir) : Installed{};
  if (g.inst.present && !g.inst.protocol) g.inst.protocol = core::known_protocol(g.rel, g.inst.version);
}

Screen decide() {
  if (!g.has_game) return Screen::NoGame;
  if (!g.rel.known) return g.unchecked && g.inst.present ? Screen::Ready : Screen::Offline;
  if (g.assets_blocked) return Screen::Offline;
  if (!g.inst.present) return Screen::Install;
  if (g.inst.version.empty() || core::compare_versions(g.rel.version, g.inst.version) > 0) return Screen::Update;
  return Screen::Ready;
}

void go_decided() {
  Screen s = decide();
  if (g.screen == Screen::Settings) g.back = s;
  else set_screen(s);
  dirty();
}

// --- the work in the background -----------------------------------------------------------------

void start_check(bool quiet) {
  int gen = ++g_gen;
  g.unchecked = false;
  if (quiet) {
    g.rechecking = true;
  } else if (g.has_game && g.screen != Screen::Settings) {
    set_screen(Screen::Checking);
  }
  logf(L"check: asking");
  dirty();
  Settings s = g.set;
  HWND hwnd = g_hwnd;
  std::thread([s, hwnd, gen]() {
    static std::atomic<bool> never{false};
    auto* r = new core::CheckResult(core::check(s, never));
    if (!PostMessageW(hwnd, WM_APP_CHECK, static_cast<WPARAM>(gen), reinterpret_cast<LPARAM>(r))) delete r;
  }).detach();
}

struct Done {
  core::Step step;
  int code;
  std::wstring setup;
  Pending kind;
};

void start_job(Pending kind) {
  if (g_job) return;
  if (kind == Pending::Install && (!g.rel.known || g.assets_blocked)) {
    flash(L"Сначала нужен ответ GitHub: проверяю связь");
    start_check(false);
    return;
  }
  g_job = true;
  g_last_job = kind;
  g_cancel = false;
  g.pending = kind;
  g.stage = kind == Pending::Uninstall ? 2 : 0;
  g.got = 0;
  g.total = g.rel.setup_size;
  g.confirm_uninstall = false;
  set_screen(Screen::Downloading);
  logf(kind == Pending::Uninstall ? L"job: uninstall" : L"job: install " + g.rel.version);
  Release rel = g.rel;
  Settings s = g.set;
  net::Route route = g_route;
  HWND hwnd = g_hwnd;
  std::thread([rel, s, route, hwnd, kind]() {
    core::UpdateHooks hooks;
    hooks.stage = [hwnd](int stage) { PostMessageW(hwnd, WM_APP_STAGE, static_cast<WPARAM>(stage), 0); };
    hooks.progress = [hwnd](uint64_t got, uint64_t total) {
      static ULONGLONG last = 0;
      ULONGLONG now = GetTickCount64();
      if (got == total || now - last >= 40) {
        last = now;
        PostMessageW(hwnd, WM_APP_PROGRESS, static_cast<WPARAM>(got), static_cast<LPARAM>(total));
      }
    };
    hooks.game_open = [hwnd](bool waiting) { PostMessageW(hwnd, WM_APP_GAME, waiting ? 1 : 0, 0); };
    auto* d = new Done{core::Step::Ok, 0, L"", kind};
    if (kind == Pending::Uninstall) d->step = core::uninstall(rel, s, route, hooks, g_cancel, &d->code);
    else d->step = core::install(rel, s, route, hooks, g_cancel, &d->code, &d->setup);
    if (!PostMessageW(hwnd, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(d))) delete d;
  }).detach();
}

void problem(const wchar_t* title, const std::wstring& text, const wchar_t* primary, Fix fix) {
  g.problem_title = title;
  g.problem_text = text;
  g.problem_primary = primary;
  g.problem_secondary = L"НАЗАД";
  g.fix = fix;
  g.fix2 = Fix::Back;
  set_screen(Screen::Problem);
  logf(std::wstring(L"problem: ") + title);
}

void on_done(Done* d) {
  g_job = false;
  g.pending = Pending::None;
  g_setup_path = d->setup;
  logf(L"job done: step " + std::to_wstring(static_cast<int>(d->step)) + L", exit code " + std::to_wstring(d->code));
  switch (d->step) {
    case core::Step::Ok:
      refresh_installed();
      if (d->kind == Pending::Install) {
        if (g.rel.full) {  // the notes of what is installed now, for the days GitHub does not answer
          g.set.notes_version = g.rel.version;
          g.set.notes_title = g.rel.title;
          g.set.notes_date = g.rel.date;
          g.set.notes = g.rel.notes;
          core::save_settings(g.set);
        }
        go_decided();
        flash(L"Мод установлен: версия " + g.inst.version);
      } else {
        go_decided();
        flash(L"Мод удалён из папки игры");
      }
      break;
    case core::Step::Cancelled:
      go_decided();
      break;
    case core::Step::NetFail:
      problem(L"Загрузка не удалась",
              L"Сервер загрузок GitHub не ответил, или связь оборвалась. Проверьте интернет и прокси в настройках.",
              L"ПОВТОРИТЬ", Fix::Retry);
      break;
    case core::Step::BadSum:
      problem(L"Файл повреждён",
              L"Контрольная сумма скачанного файла не совпала с той, что указана в релизе. Файл удалён — скачайте его ещё раз.",
              L"СКАЧАТЬ ЗАНОВО", Fix::Retry);
      break;
    case core::Step::Denied:
      problem(L"Нужно разрешение Windows",
              L"Игра лежит в папке, куда обычной программе писать нельзя. Windows спросит разрешение — без него мод не поставить.",
              L"ПОВТОРИТЬ", Fix::Retry);
      break;
    case core::Step::NoSetup:
      problem(L"Нет установщика",
              L"Мод убирает его же установщик, а его нет на этом ПК, и GitHub не отвечает. Проверьте связь и повторите.",
              L"ПРОВЕРИТЬ СВЯЗЬ", Fix::Recheck);
      break;
    default:
      problem(d->kind == Pending::Uninstall ? L"Удаление не удалось" : L"Установка не удалась",
              L"Установщик завершился с ошибкой (код " + std::to_wstring(d->code) +
                  L"). Откройте его сами: он покажет, что помешало.",
              L"ОТКРЫТЬ УСТАНОВЩИК", Fix::OpenSetup);
      break;
  }
  delete d;
}

void on_check(int gen, core::CheckResult* r) {
  if (gen != g_gen.load()) {
    delete r;
    return;
  }
  logf(L"check: " + (r->rel.known ? L"version " + r->rel.version + (r->rel.full ? L" (described)" : L" (the version only)")
                                  : std::wstring(L"no answer")) +
       (r->assets_blocked ? L", files blocked" : L"") + (r->route.label.empty() ? L"" : L", " + r->route.label));
  for (size_t p = 0; p < r->log.size();) {
    size_t e = r->log.find(L'\n', p);
    logf(L"  " + r->log.substr(p, e - p));
    p = e == std::wstring::npos ? r->log.size() : e + 1;
  }
  g.rel = r->rel;
  g.assets_blocked = r->assets_blocked;
  if (r->rel.known) g_route = r->route;
  g.rechecking = false;
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t at[16];
  swprintf(at, 16, L"%02d:%02d", t.wHour, t.wMinute);
  g.checked_at = at;
  g.via = r->rel.known && r->route.kind == 2 ? r->route.label : L"";
  refresh_installed();
  bool startup = g_startup_check;
  g_startup_check = false;
  delete r;
  if (g.screen == Screen::Downloading || g.screen == Screen::Running || g.screen == Screen::Problem) return;
  go_decided();
  if (startup && g.set.auto_install && g.screen == Screen::Update) start_job(Pending::Install);
}

// --- what a press does -------------------------------------------------------------------------

void open_url(const std::wstring& url) {
  logf(L"open " + url);
  if (!g_drive) ShellExecuteW(g_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void copy_text(const std::wstring& text) {
  if (!OpenClipboard(g_hwnd)) return;
  EmptyClipboard();
  size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    memcpy(GlobalLock(h), text.c_str(), bytes);
    GlobalUnlock(h);
    if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
  }
  CloseClipboard();
}

void play() {
  if (!g.has_game || g.launching) return;
  logf(L"play");
  if (!g_drive && !core::launch_game(g.set.game_dir)) {
    flash(L"Не удалось запустить TMNT2.exe");
    return;
  }
  g.launching = true;
  g_launch_until = GetTickCount64() + 5000;
  if (g.set.close_on_play && !g_drive) g_quit_at = GetTickCount64() + 2500;
  dirty();
}

void choose_folder() {
  std::wstring dir;
  if (g_drive) dir = g_pick;
  else if (!core::pick_folder(g_hwnd, g.set.game_dir, dir)) return;
  if (dir.empty()) return;
  if (!core::has_game(dir)) {
    flash(L"В этой папке нет TMNT2.exe");
    return;
  }
  logf(L"folder chosen");
  g.set.game_dir = core::trimmed(dir);
  core::save_settings(g.set);
  refresh_installed();
  if (g.rel.known) go_decided();
  else start_check(false);
}

std::wstring newest_desync_log(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(core::join(dir, L"tmnt2net-desync-*.log").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return L"";
  std::wstring best;
  FILETIME when{};
  do {
    if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &when) > 0) {
      best = fd.cFileName;
      when = fd.ftLastWriteTime;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return core::join(dir, best);
}

void fix(Fix f) {
  switch (f) {
    case Fix::Retry:
      start_job(g_last_job == Pending::None ? Pending::Install : g_last_job);
      break;
    case Fix::OpenSetup:
      if (!g_setup_path.empty() && core::exists(g_setup_path)) {
        logf(L"the installer opened by hand");
        if (!g_drive) {
          std::wstring args = L"\"" + g.set.game_dir + L"\"";
          ShellExecuteW(g_hwnd, L"open", g_setup_path.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
        }
      } else {
        open_url(std::wstring(kRepoUrl) + L"/releases/latest");
      }
      break;
    case Fix::Recheck:
      start_check(false);
      break;
    default:
      go_decided();
      break;
  }
}

void act(int id) {
  logf(L"press " + std::to_wstring(id));
  if (id != H_ProxyField && g.proxy_focus) {
    g.proxy_focus = false;
    core::save_settings(g.set);
  }
  if (id != H_Uninstall) g.confirm_uninstall = false;
  switch (id) {
    case H_Close:
      g_cancel = true;
      DestroyWindow(g_hwnd);
      return;
    case H_Min:
      ShowWindow(g_hwnd, SW_MINIMIZE);
      return;
    case H_Settings:
      if (g.screen == Screen::Settings) {
        set_screen(g.back);
      } else {
        g.back = g.screen;
        set_screen(Screen::Settings);
      }
      break;
    case H_Back:
      if (g.screen == Screen::Settings) set_screen(g.back);
      break;
    case H_Primary:
      if (!view::primary_enabled(g)) break;
      switch (g.screen) {
        case Screen::Checking:
        case Screen::Ready: play(); break;
        case Screen::Update:
        case Screen::Install: start_job(Pending::Install); break;
        case Screen::Offline:
          if (g.inst.present) play();
          else start_check(false);
          break;
        case Screen::NoGame: choose_folder(); break;
        case Screen::Running:
          if (core::close_game(g.set.game_dir)) flash(L"Игру попросили закрыться");
          else flash(L"Окно игры не найдено: закройте её сами");
          break;
        case Screen::Problem: fix(g.fix); break;
        default: break;
      }
      break;
    case H_Secondary:
      switch (g.screen) {
        case Screen::Update: play(); break;
        case Screen::Ready:
          if (!g.rechecking) start_check(true);
          break;
        case Screen::Offline: start_check(false); break;
        case Screen::Running: g_cancel = true; break;
        case Screen::Problem: fix(g.fix2); break;
        default: break;
      }
      break;
    case H_Cancel:
      g_cancel = true;
      break;
    case H_Folder:
    case H_FolderSettings:
      if (!g_job) choose_folder();
      break;
    case H_PaperLink:
      open_url(view::link_url(g));
      break;
    case H_Copy:
      copy_text(std::wstring(kRepoUrl) + L"/releases/latest");
      g.copied_until = GetTickCount64() + 3000;
      break;
    case H_Repo:
      open_url(kRepoUrl);
      break;
    case H_NewLauncher:
      open_url(std::wstring(kRepoUrl) + L"/releases/latest");
      break;
    case H_Toggle1:
      g.set.auto_check = !g.set.auto_check;
      core::save_settings(g.set);
      break;
    case H_Toggle2:
      g.set.auto_install = !g.set.auto_install;
      core::save_settings(g.set);
      break;
    case H_Toggle3:
      g.set.close_on_play = !g.set.close_on_play;
      core::save_settings(g.set);
      break;
    case H_ProxyAuto:
    case H_ProxyNone:
    case H_ProxyOwn:
      g.set.proxy = id - H_ProxyAuto;
      g.proxy_focus = id == H_ProxyOwn && g.set.proxy_addr.empty();
      core::save_settings(g.set);
      break;
    case H_ProxyField:
      g.set.proxy = 2;
      g.proxy_focus = true;
      break;
    case H_OpenLog: {
      std::wstring log = core::join(g.set.game_dir, L"tmnt2net.log");
      if (g.has_game && core::exists(log)) open_url(log);
      else flash(L"Журнала пока нет: игра с модом ещё не запускалась");
      break;
    }
    case H_DesyncLogs: {
      std::wstring log = g.has_game ? newest_desync_log(g.set.game_dir) : L"";
      if (log.empty()) {
        flash(L"Журналов рассинхронизации нет");
      } else {
        logf(L"desync log shown");
        std::wstring args = L"/select,\"" + log + L"\"";
        if (!g_drive) ShellExecuteW(g_hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
      }
      break;
    }
    case H_Reinstall:
      if (!g.has_game) flash(L"Сначала укажите папку игры");
      else if (!g_job) start_job(Pending::Install);
      break;
    case H_Uninstall:
      if (!g.inst.present) {
        flash(L"Мод не установлен");
      } else if (!g.confirm_uninstall) {
        g.confirm_uninstall = true;
        g_confirm_until = GetTickCount64() + 5000;
      } else if (!g_job) {
        start_job(Pending::Uninstall);
      }
      break;
    case H_Accent0:
    case H_Accent1:
    case H_Accent2:
    case H_Accent3:
      g.set.accent = id - H_Accent0;
      core::save_settings(g.set);
      break;
    default:
      break;
  }
  dirty();
}

// --- the window -----------------------------------------------------------------------------

float scale_of(HWND hwnd) { return GetDpiForWindow(hwnd) / 96.0f; }

void discard_target() {
  g_canvas.reset();
  if (g_rt) g_rt->Release();
  g_rt = nullptr;
}

void paint(HWND hwnd) {
  if (!g_rt) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    float dpi = static_cast<float>(GetDpiForWindow(hwnd));
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    if (FAILED(gfx::d2d()->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt)))
      return;
    g_canvas = std::make_unique<gfx::Canvas>(g_rt);
  }
  g.time = (GetTickCount64() - g_start) / 1000.0;
  g_rt->BeginDraw();
  g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
  view::draw(*g_canvas, g, &g_hits);
  HRESULT hr = g_rt->EndDraw();
  if (g_drive) {
    static bool said = false;
    if (!said || FAILED(hr)) logf(FAILED(hr) ? L"window: drawing FAILED" : L"window: drawn");
    said = true;
  }
  if (hr == D2DERR_RECREATE_TARGET) discard_target();
  g_dirty = false;
}

int hit_at(float x, float y) {
  for (size_t i = g_hits.size(); i-- > 0;) {
    const Hit& h = g_hits[i];
    D2D1_POINT_2F p = h.inv.TransformPoint(D2D1::Point2F(x, y));
    if (p.x >= h.r.left && p.x < h.r.right && p.y >= h.r.top && p.y < h.r.bottom) return h.id;
  }
  return H_None;
}

bool moving() {
  return g.screen == Screen::Checking || g.screen == Screen::Downloading || g.rechecking || g.proxy_focus;
}

int hit_by_name(const std::wstring& n) {
  static const struct {
    const wchar_t* name;
    int id;
  } names[] = {{L"settings", H_Settings}, {L"min", H_Min}, {L"close", H_Close}, {L"primary", H_Primary},
               {L"secondary", H_Secondary}, {L"folder", H_Folder}, {L"link", H_PaperLink}, {L"copy", H_Copy},
               {L"cancel", H_Cancel}, {L"repo", H_Repo}, {L"back", H_Back}, {L"t1", H_Toggle1}, {L"t2", H_Toggle2},
               {L"t3", H_Toggle3}, {L"proxy_auto", H_ProxyAuto}, {L"proxy_none", H_ProxyNone}, {L"proxy_own", H_ProxyOwn},
               {L"proxy_field", H_ProxyField}, {L"folder2", H_FolderSettings}, {L"log", H_OpenLog}, {L"desync", H_DesyncLogs},
               {L"reinstall", H_Reinstall}, {L"uninstall", H_Uninstall}, {L"accent0", H_Accent0}, {L"accent1", H_Accent1},
               {L"accent2", H_Accent2}, {L"accent3", H_Accent3}};
  for (auto& x : names)
    if (n == x.name) return x.id;
  return H_None;
}

bool save_shot(const std::wstring& file, float scale) {
  g.time = (GetTickCount64() - g_start) / 1000.0;
  return gfx::save_png(file.c_str(), kW, kH, scale, [](gfx::Canvas& c) { view::draw(c, g, nullptr); });
}

// The test script: one step at a time, from the timer.
void drive() {
  ULONGLONG now = GetTickCount64();
  while (g_script_at < g_script.size()) {
    if (now < g_script_wait) return;
    const std::wstring& cmd = g_script[g_script_at];
    size_t colon = cmd.find(L':');
    std::wstring op = cmd.substr(0, colon), arg = colon == std::wstring::npos ? L"" : cmd.substr(colon + 1);
    if (op == L"wait") {
      g_script_wait = now + _wtoi(arg.c_str());
      ++g_script_at;
      return;
    }
    if (op == L"until") {  // until:<screen>:<ms>
      size_t c2 = arg.find(L':');
      std::wstring want = arg.substr(0, c2);
      if (!g_script_deadline) g_script_deadline = now + (c2 == std::wstring::npos ? 20000 : _wtoi(arg.c_str() + c2 + 1));
      if (want == screen_name(g.screen)) {
        logf(L"until " + want + L": reached");
        g_script_deadline = 0;
        ++g_script_at;
        continue;
      }
      if (now > g_script_deadline) {
        logf(L"until " + want + L": TIMEOUT on " + screen_name(g.screen));
        g_script_deadline = 0;
        ++g_script_at;
        continue;
      }
      return;
    }
    ++g_script_at;
    if (op == L"click") {
      int id = hit_by_name(arg);
      bool there = false;
      std::vector<Hit> hits;  // (what is on the screen now: drawn aside)
      gfx::save_png(nullptr, kW, kH, 1, [&](gfx::Canvas& c) { view::draw(c, g, &hits); });
      for (auto& h : hits) there = there || h.id == id;
      logf(L"click " + arg + (there ? L"" : L": NOT ON THIS SCREEN"));
      if (there) act(id);
      if (!g_hwnd) return;
    } else if (op == L"mouse") {  // mouse:x,y - a real press at that place of the window
      paint(g_hwnd);
      float s = scale_of(g_hwnd);
      int mx = static_cast<int>(_wtof(arg.c_str()) * s), my = static_cast<int>(_wtof(arg.c_str() + arg.find(L',') + 1) * s);
      LPARAM at = MAKELPARAM(mx, my);
      SendMessageW(g_hwnd, WM_MOUSEMOVE, 0, at);
      logf(L"mouse " + arg + L": over " + std::to_wstring(g.hover) + L" of " + std::to_wstring(g_hits.size()));
      SendMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at);
      SendMessageW(g_hwnd, WM_LBUTTONUP, 0, at);
      if (!g_hwnd) return;
    } else if (op == L"type") {
      for (wchar_t ch : arg) SendMessageW(g_hwnd, WM_CHAR, ch, 0);
    } else if (op == L"shot") {
      logf(L"shot " + arg + (save_shot(arg, 1) ? L"" : L": FAILED"));
    } else if (op == L"quit") {
      logf(L"quit");
      DestroyWindow(g_hwnd);
      return;
    }
  }
}

void tick(HWND hwnd) {
  ULONGLONG now = GetTickCount64();
  if (!g.flash.empty() && now > g.flash_until) g.flash.clear(), dirty();
  if (g.copied_until && now > g.copied_until) g.copied_until = 0, dirty();
  if (g.confirm_uninstall && now > g_confirm_until) g.confirm_uninstall = false, dirty();
  if (g.launching && now > g_launch_until) g.launching = false, dirty();
  if (g_quit_at && now > g_quit_at) {
    DestroyWindow(hwnd);
    return;
  }
  if (g_drive) drive();
  if (g_hwnd && (moving() || g_dirty) && !IsIconic(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      paint(hwnd);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
      return 0;
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      discard_target();
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_TIMER:
      tick(hwnd);
      return 0;
    case WM_NCHITTEST: {
      POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &p);
      float s = scale_of(hwnd);
      if (p.y / s < 48 && p.x / s < 824) return HTCAPTION;  // the header drags the window
      return HTCLIENT;
    }
    case WM_NCLBUTTONDBLCLK:
      return 0;  // (no maximizing)
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_MAXIMIZE) return 0;
      break;
    case WM_MOUSEMOVE: {
      float s = scale_of(hwnd);
      int id = hit_at(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
      if (id != g.hover) {
        g.hover = id;
        dirty();
      }
      if (!g_tracking) {
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
        g_tracking = TrackMouseEvent(&t) != FALSE;
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      g_tracking = false;
      if (g.hover) g.hover = 0, dirty();
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, g.hover == H_ProxyField ? IDC_IBEAM : g.hover ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_LBUTTONDOWN: {
      float s = scale_of(hwnd);
      g.pressed = hit_at(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
      if (g.proxy_focus && g.pressed != H_ProxyField) {
        g.proxy_focus = false;
        core::save_settings(g.set);
      }
      SetCapture(hwnd);
      dirty();
      return 0;
    }
    case WM_LBUTTONUP: {
      float s = scale_of(hwnd);
      int id = hit_at(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s), was = g.pressed;
      g.pressed = 0;
      ReleaseCapture();
      dirty();
      if (was && id == was) act(id);
      return 0;
    }
    case WM_KEYDOWN:
      if (g.proxy_focus) {
        if (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB) {
          g.proxy_focus = false;
          core::save_settings(g.set);
          dirty();
        }
        return 0;
      }
      if (wp == VK_ESCAPE && g.screen == Screen::Settings) act(H_Back);
      else if (wp == VK_RETURN && g.screen != Screen::Settings && g.screen != Screen::Downloading) act(H_Primary);
      return 0;
    case WM_CHAR:
      if (g.proxy_focus) {
        if (wp == 8) {
          if (!g.set.proxy_addr.empty()) g.set.proxy_addr.pop_back();
        } else if (wp == 22) {  // Ctrl+V
          if (OpenClipboard(hwnd)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
              if (const wchar_t* t = static_cast<const wchar_t*>(GlobalLock(h))) {
                for (; *t && g.set.proxy_addr.size() < 64; ++t)
                  if (*t > 0x20 && *t < 0x7F) g.set.proxy_addr += *t;
                GlobalUnlock(h);
              }
            }
            CloseClipboard();
          }
        } else if (wp > 0x20 && wp < 0x7F && g.set.proxy_addr.size() < 64) {
          g.set.proxy_addr += static_cast<wchar_t>(wp);
        }
        dirty();
      }
      return 0;
    case WM_APP + 1:
      on_check(static_cast<int>(wp), reinterpret_cast<core::CheckResult*>(lp));
      return 0;
    case WM_APP + 2:
      g.stage = static_cast<int>(wp);
      if (g.screen == Screen::Running && g.stage == 2) set_screen(Screen::Downloading);
      logf(L"stage " + std::to_wstring(g.stage));
      dirty();
      return 0;
    case WM_APP + 3: {
      uint64_t got = static_cast<uint64_t>(wp), total = static_cast<uint64_t>(lp);
      if (g_drive && total && got * 4 / total != g.got * 4 / total) logf(L"downloaded " + std::to_wstring(got * 100 / total) + L"%");
      g.got = got;
      g.total = total;
    }
      dirty();
      return 0;
    case WM_APP + 4:
      if (g_job) set_screen(wp ? Screen::Running : Screen::Downloading);
      return 0;
    case WM_APP + 5:
      on_done(reinterpret_cast<Done*>(lp));
      return 0;
    case WM_DESTROY:
      g_cancel = true;
      discard_target();
      g_hwnd = nullptr;
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

std::vector<std::wstring> split(const std::wstring& s, wchar_t sep) {
  std::vector<std::wstring> out;
  size_t p = 0;
  while (p <= s.size()) {
    size_t e = s.find(sep, p);
    if (e == std::wstring::npos) e = s.size();
    if (e > p) out.push_back(s.substr(p, e - p));
    p = e + 1;
  }
  return out;
}

int run(int argc, wchar_t** argv) {
  std::wstring shot_screen, shot_file;
  float shot_scale = 1;
  int accent = -1;
  bool check_only = false, where_only = false;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
    if (a == L"--shot") {
      shot_screen = next();
      shot_file = next();
    } else if (a == L"--scale") {
      shot_scale = static_cast<float>(_wtof(next().c_str()));
    } else if (a == L"--accent") {
      accent = _wtoi(next().c_str()) & 3;
    } else if (a == L"--check") {
      check_only = true;
    } else if (a == L"--where") {
      where_only = true;
    } else if (a == L"--out") {
      g_out = next();
    } else if (a == L"--settings") {
      core::g_settings_path = next();
    } else if (a == L"--cache") {
      core::g_cache_dir = next();
    } else if (a == L"--repo") {
      core::g_repo_url = next();
    } else if (a == L"--pick") {
      g_pick = next();
    } else if (a == L"--drive") {
      g_drive = true;
      g_script = split(next(), L';');
    }
  }
  g_start = GetTickCount64();
  if (!gfx::init()) {
    MessageBoxW(nullptr, L"Не удалось включить Direct2D: лаунчеру нужна Windows 10 или новее.", L"tmnt2net", MB_ICONERROR);
    return 3;
  }

  if (!shot_file.empty()) {  // a screen of the design as a picture
    if (!view::demo(shot_screen, g)) return 2;
    if (accent >= 0) g.set.accent = accent;
    bool ok = gfx::save_png(shot_file.c_str(), kW, kH, shot_scale <= 0 ? 1 : shot_scale,
                            [](gfx::Canvas& c) { view::draw(c, g, nullptr); });
    if (!ok) {
      wchar_t why[80];
      swprintf(why, 80, L"shot failed at step %d, hr %08lX", gfx::g_last_step, static_cast<unsigned long>(gfx::g_last_hr));
      logf(why);
    }
    gfx::shutdown();
    return ok ? 0 : 1;
  }

  if (where_only) {  // where the game would be found from here, and what is in it
    std::wstring dir = core::find_game_dir();
    Installed in = core::read_installed(dir);
    logf(L"game: " + (dir.empty() ? std::wstring(L"(not found)") : dir));
    logf(std::wstring(L"mod: ") + (in.present ? (in.version.empty() ? L"of an unknown version" : in.version.c_str()) : L"not installed") +
         (dir.empty() ? L"" : core::exe_supported(dir) ? L", the exe is the supported one" : L", ANOTHER exe"));
    gfx::shutdown();
    return dir.empty() ? 1 : 0;
  }
  core::load_settings(g.set);
  if (check_only) {
    std::atomic<bool> never{false};
    core::CheckResult r = core::check(g.set, never);
    logf(std::wstring(L"known ") + (r.rel.known ? L"1" : L"0") + L" full " + (r.rel.full ? L"1" : L"0") + L" blocked " +
         (r.assets_blocked ? L"1" : L"0") + L" version " + r.rel.version + L" protocol " + std::to_wstring(r.rel.protocol) +
         L" route " + r.route.label);
    logf(L"date " + r.rel.date + L" title " + r.rel.title + L" notes " + std::to_wstring(r.rel.notes.size()) + L" setup " +
         r.rel.setup_name + L" " + std::to_wstring(r.rel.setup_size) + L" " + core::wide(r.rel.setup_sha256) + L" launcher " +
         r.rel.launcher_version);
    logf(r.log);
    gfx::shutdown();
    return r.rel.known ? 0 : 1;
  }

  // One launcher at a time: a second start shows the first one.
  HANDLE once = g_drive ? nullptr : CreateMutexW(nullptr, TRUE, L"tmnt2net-launcher-running");
  if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(kClass, nullptr)) {
      ShowWindow(other, SW_RESTORE);
      SetForegroundWindow(other);
    }
    return 0;
  }

  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
  wc.hIconSm = wc.hIcon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);

  // In the middle of the screen the pointer is on, at that screen's scale.
  POINT cursor{};
  GetCursorPos(&cursor);
  HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  UINT dpi = 96;
  if (HDC dc = GetDC(nullptr)) {
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
  }
  int w = MulDiv(kW, dpi, 96), h = MulDiv(kH, dpi, 96);
  int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2, y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
  g_hwnd = CreateWindowExW(0, kClass, L"tmnt2net — лаунчер", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, x, y, w, h, nullptr, nullptr,
                           inst, nullptr);
  if (!g_hwnd) return 3;
  UINT real = GetDpiForWindow(g_hwnd);  // (the screen it landed on may have another scale)
  if (real != dpi) SetWindowPos(g_hwnd, nullptr, 0, 0, MulDiv(kW, real, 96), MulDiv(kH, real, 96), SWP_NOMOVE | SWP_NOZORDER);
  int round = 2;  // DWMWCP_ROUND: round corners where Windows has them (11)
  DwmSetWindowAttribute(g_hwnd, 33, &round, sizeof(round));

  // Where things stand at the start.
  if (!core::has_game(g.set.game_dir) && !g_drive) {  // (a test never goes looking for the real game)
    std::wstring found = core::trimmed(core::find_game_dir());
    if (!found.empty()) {
      g.set.game_dir = found;
      core::save_settings(g.set);
    }
  }
  refresh_installed();
  logf(std::wstring(L"start: game ") + (g.has_game ? L"found" : L"not found") + L", mod " +
       (g.inst.present ? (g.inst.version.empty() ? L"of an unknown version" : g.inst.version) : L"not installed"));
  if (!g.has_game) {
    g.screen = Screen::NoGame;
    start_check(true);  // (the newest version is shown meanwhile)
    g.rechecking = false;
  } else if (g.set.auto_check || !g.inst.present) {
    g.screen = Screen::Checking;
    g_startup_check = true;
    start_check(false);
  } else {
    g.unchecked = true;
    g.screen = Screen::Ready;
  }

  SetTimer(g_hwnd, 1, 33, nullptr);
  if (!g_drive) {
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
  }
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  logf(L"exit");
  gfx::shutdown();
  if (once) CloseHandle(once);
  return 0;
}

}  // namespace
}  // namespace lz

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  int rc = lz::run(argc, argv);
  LocalFree(argv);
  CoUninitialize();
  return rc;
}
