// tmnt2net-launcher.exe: what its parts share. The launcher finds the game, asks GitHub for the newest
// release of the mod, downloads its installer, checks it, runs it and starts the game.
//   model: what is on the screen (main.cpp changes it, view.cpp draws it)
//   core:  the game folder, the installed mod, settings, the release, the installer (core.cpp)
//   net:   HTTP through WinHTTP, with or without a proxy (net.cpp)
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "launcher/gfx.h"

namespace lz {

constexpr int kW = 960, kH = 600;  // the window, in units of 1/96 inch
constexpr wchar_t kLauncherVersion[] = L"1.0.0";
constexpr wchar_t kRepoUrl[] = L"https://github.com/int3re/TMNT-2-BATTLE-NEXUS-COOP";
constexpr wchar_t kRepoText[] = L"github.com/int3re/TMNT-2-BATTLE-NEXUS-COOP";

enum class Screen { Checking, Update, Downloading, Ready, Install, NoGame, Offline, Running, Problem, Settings };

// The newest release of the mod, by tmnt2net-latest.json of the release.
struct Release {
  bool known = false;   // the version is known
  bool full = false;    // the whole description came (notes, the installer's size and checksum)
  std::wstring version;
  int protocol = 0;
  std::wstring date;    // "20 ноября 2026"
  std::wstring title;
  std::vector<std::wstring> notes;
  std::wstring setup_name;
  uint64_t setup_size = 0;
  std::string setup_sha256;
  std::wstring launcher_version;  // the newest launcher
  std::vector<std::pair<std::wstring, int>> protocols;  // version -> protocol
};

struct Installed {
  bool present = false;   // the mod's files are in the game folder
  std::wstring version;   // empty: not known (put there by hand)
  int protocol = 0;       // 0: not known
};

struct Settings {
  std::wstring game_dir;
  bool auto_check = true;
  bool auto_install = false;
  bool close_on_play = true;
  int proxy = 0;  // 0 automatic, 1 none, 2 the address below
  std::wstring proxy_addr;
  int accent = 3;  // the bandana: 0 blue, 1 red, 2 purple, 3 orange
  // the notes of the installed version (kept from the release it came from)
  std::wstring notes_version, notes_title, notes_date;
  std::vector<std::wstring> notes;
};

enum class Pending { None, Install, Uninstall };
enum class Fix { None, Retry, OpenSetup, Back, Recheck };

struct Model {
  Screen screen = Screen::Checking;
  Screen back = Screen::Ready;  // under the settings
  Settings set;
  Installed inst;
  Release rel;
  bool has_game = false;
  bool exe_ok = true;           // TMNT2.exe is the one the mod works with
  bool unchecked = false;       // no answer asked of GitHub (the check at start is off)
  bool assets_blocked = false;  // github.com answers, its download host does not
  // the update
  Pending pending = Pending::None;
  int stage = 0;  // 0 the download, 1 the checksum, 2 the installer
  uint64_t got = 0, total = 0;
  // the footer's line
  std::wstring checked_at, via;  // when GitHub answered last (HH:MM), and by what way if not directly
  std::wstring net;
  int net_state = 0;  // 0 fine, 1 waiting, 2 bad
  std::wstring flash;  // a passing note in its place
  ULONGLONG flash_until = 0;
  // passing states of buttons
  bool launching = false, rechecking = false, confirm_uninstall = false;
  ULONGLONG copied_until = 0;
  // the Problem screen
  std::wstring problem_title, problem_text, problem_primary, problem_secondary;
  Fix fix = Fix::None, fix2 = Fix::None;
  // the pointer and the keyboard
  int hover = 0, pressed = 0;
  bool proxy_focus = false;
  double time = 0;  // seconds, for what moves
};

// What can be pressed. The view tells where each one is; main.cpp acts.
enum HitId {
  H_None = 0,
  H_Settings, H_Min, H_Close,
  H_Primary, H_Secondary, H_Folder, H_PaperLink, H_Copy, H_Cancel, H_Repo, H_NewLauncher,
  H_Back, H_Toggle1, H_Toggle2, H_Toggle3, H_ProxyAuto, H_ProxyNone, H_ProxyOwn, H_ProxyField,
  H_FolderSettings, H_OpenLog, H_DesyncLogs, H_Reinstall, H_Uninstall,
  H_Accent0, H_Accent1, H_Accent2, H_Accent3,
  H_Count
};
struct Hit {
  int id;
  D2D1_RECT_F r;     // in the space of `inv`
  gfx::Matrix inv;   // window -> that space
};

namespace view {
// Draws the whole window; `hits` gets what can be pressed (null: not wanted).
void draw(gfx::Canvas& c, const Model& m, std::vector<Hit>* hits);
bool primary_enabled(const Model& m);
std::wstring link_url(const Model& m);  // where the banner's link leads
// The model of a screen as the design shows it (launcher --shot <screen>).
bool demo(const std::wstring& screen, Model& m);
}  // namespace view

namespace net {
struct Route {
  int kind = 0;        // 0 the system's settings, 1 no proxy, 2 a proxy
  std::wstring proxy;  // host:port
  std::wstring label;  // for the log and the footer
};
struct Response {
  bool sent = false;   // the server answered
  int status = 0;
  std::wstring location;  // of a redirect that was not followed
  std::string body;
  DWORD error = 0;
};
// GET. follow: redirects are followed. to_file: the body goes there (and `progress` hears of it).
Response get(const std::wstring& url, const Route& route, bool follow, const std::wstring& to_file = L"",
             const std::function<void(uint64_t got, uint64_t total)>& progress = nullptr,
             const std::atomic<bool>* cancel = nullptr);
bool port_open(const wchar_t* host, int port, int timeout_ms);
}  // namespace net

namespace core {
extern std::wstring g_repo_url;      // tests: the repository's page served from elsewhere (--repo)
std::wstring repo();                 // its address: github.com/..., unless a test says otherwise
extern std::wstring g_settings_path; // tests: another settings file (--settings)
extern std::wstring g_cache_dir;     // tests: another folder for downloads (--cache)

std::wstring join(const std::wstring& dir, const std::wstring& name);
std::wstring trimmed(std::wstring dir);  // a folder without the slash at its end
bool exists(const std::wstring& path);
std::string narrow(const std::wstring& s);
std::wstring wide(const std::string& utf8);
int compare_versions(const std::wstring& a, const std::wstring& b);
int known_protocol(const Release& rel, const std::wstring& version);

void load_settings(Settings& s);
void save_settings(const Settings& s);

bool has_game(const std::wstring& dir);           // TMNT2.exe is there
bool exe_supported(const std::wstring& dir);      // ... and it is the one the mod works with
std::wstring find_game_dir();                     // the registry, the launcher's own folder
Installed read_installed(const std::wstring& dir);
bool game_running(const std::wstring& dir);
int close_game(const std::wstring& dir);          // asks its windows to close; how many were asked
bool launch_game(const std::wstring& dir);
bool pick_folder(HWND owner, const std::wstring& initial, std::wstring& out);

// The newest release. Tries every route the settings allow; `route` gets the one that worked.
struct CheckResult {
  Release rel;
  bool answered = false;        // github.com answered at all
  bool assets_blocked = false;  // ... but the release's files did not come
  net::Route route;
  std::wstring log;
};
CheckResult check(const Settings& s, const std::atomic<bool>& cancel);
bool parse_manifest(const std::string& json, Release& rel);

enum class Step { Ok, Cancelled, NetFail, BadSum, SetupFail, Denied, NoSetup };
struct UpdateHooks {
  std::function<void(int stage)> stage;
  std::function<void(uint64_t got, uint64_t total)> progress;
  std::function<void(bool waiting)> game_open;  // the game holds the mod's files: waiting for it to close
};
// Downloads the release's installer (unless it is there already), checks it and runs it.
Step install(const Release& rel, const Settings& s, const net::Route& first, const UpdateHooks& hooks,
             const std::atomic<bool>& cancel, int* exit_code, std::wstring* setup_path);
Step uninstall(const Release& rel, const Settings& s, const net::Route& first, const UpdateHooks& hooks,
               const std::atomic<bool>& cancel, int* exit_code);
std::wstring cached_setup(const std::wstring& version);  // its path in the cache (may not exist)
}  // namespace core

}  // namespace lz
