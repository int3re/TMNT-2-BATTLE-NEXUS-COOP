// The game's settings the launcher's pages edit: see gameset.h.
#include "launcher/gameset.h"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <utility>
#include <vector>

#include "launcher/launcher.h"

namespace lz {
namespace gset {
namespace {

const wchar_t kIni[] = L"tmnt2net.ini";
const wchar_t kLevelsSince[] = L"0.17.2";  // the first mod that reads [audio]

struct Choice {
  const wchar_t* name;
  const wchar_t* value;  // as tmnt2net.ini has it
};
struct Info {
  const wchar_t* label;
  const char* key;
  Kind kind;
  const wchar_t* section;
  const wchar_t* ini;
  const wchar_t* fallback;  // the value when the ini has none: the mod's default
  std::vector<Choice> choices;
};
// The values are the ones the game's own pages offer (src/mod/menus.cpp).
const Info kInfo[kRows] = {
    {L"Окно игры", "mode", Kind::Choice, L"video", L"mode", L"borderless",
     {{L"На весь монитор", L"borderless"}, {L"В окне", L"windowed"}}},
    {L"Разрешение", "resolution", Kind::Choice, L"video", L"resolution", L"auto", {}},
    {L"Формат кадра", "aspect", Kind::Choice, L"video", L"aspect", L"auto",
     {{L"Под монитор", L"auto"}, {L"4:3 с полосами", L"4:3"}, {L"Растянуть", L"stretch"}}},
    {L"Меню и надписи", "hud", Kind::Choice, L"video", L"hud", L"center",
     {{L"По центру", L"center"}, {L"На всю ширину", L"stretch"}}},
    {L"Карта мира", "map", Kind::Choice, L"video", L"map", L"wide", {{L"На весь экран", L"wide"}, {L"4:3 с полосами", L"center"}}},
    {L"Шкалы в уровне", "gauges", Kind::Choice, L"video", L"gauges", L"corners",
     {{L"По углам", L"corners"}, {L"Ближе к центру", L"center"}}},
    {L"Размер шкал", "gauges_size", Kind::Choice, L"video", L"gauges_size", L"100",
     {{L"100 %", L"100"}, {L"85 %", L"85"}, {L"70 %", L"70"}}},
    {L"Сглаживание", "antialiasing", Kind::Choice, L"video", L"antialiasing", L"4",
     {{L"Выкл", L"0"}, {L"2x", L"2"}, {L"4x", L"4"}, {L"8x", L"8"}}},
    {L"Фильтр текстур", "anisotropy", Kind::Choice, L"video", L"anisotropy", L"16",
     {{L"Выкл", L"0"}, {L"4x", L"4"}, {L"8x", L"8"}, {L"16x", L"16"}}},
    {L"Резкость", "sharpen", Kind::Choice, L"video", L"sharpen", L"50",
     {{L"Выкл", L"0"}, {L"Слабая", L"25"}, {L"Средняя", L"50"}, {L"Сильная", L"75"}, {L"Максимум", L"100"}}},
    {L"Цвета", "colors", Kind::Choice, L"video", L"colors", L"25",
     {{L"Как в игре", L"0"}, {L"Чуть ярче", L"25"}, {L"Яркие", L"50"}, {L"Очень яркие", L"80"}}},
    {L"Угол камеры", "fov", Kind::Choice, L"video", L"fov", L"1.0",
     {{L"80 %", L"0.8"}, {L"90 %", L"0.9"}, {L"Как в игре", L"1.0"}, {L"110 %", L"1.1"}, {L"120 %", L"1.2"},
      {L"130 %", L"1.3"}, {L"140 %", L"1.4"}, {L"150 %", L"1.5"}}},
    {L"Камера в сети", "own_camera", Kind::Choice, L"video", L"own_camera", L"1",
     {{L"На своего героя", L"1"}, {L"Одна на всех", L"0"}}},
    {L"Синхронизация", "vsync", Kind::Choice, L"video", L"vsync", L"-1",
     {{L"Как в игре", L"-1"}, {L"Выключена", L"0"}, {L"Включена", L"1"}}},
    {L"Музыка", "music", Kind::Level, L"audio", L"music", L"10", {}},
    {L"Эффекты", "effects", Kind::Level, L"audio", L"effects", L"10", {}},
    {L"Голоса", "voices", Kind::Level, L"audio", L"voices", L"10", {}},
    {L"Заставки", "intro", Kind::Pair, L"video", L"intro", L"show", {{L"Показывать", L"show"}, {L"Пропускать", L"skip"}}},
    {L"Игра в фоне", "background", Kind::Pair, L"video", L"background", L"pause",
     {{L"Стоит", L"pause"}, {L"Продолжается", L"run"}}},
};

// The key table's actions (the page's order) and their places in the game's table: attack, strong
// attack, shuriken, jump, dash, guard, hero change, camera, information, up, down, left, right.
constexpr int kSlots = kKeyActions;
const struct {
  const wchar_t* label;
  const char* key;
  int slot;
} kActions[kKeyActions] = {
    {L"Вверх", "up", 9},           {L"Вниз", "down", 10},         {L"Влево", "left", 11},      {L"Вправо", "right", 12},
    {L"Прыжок", "jump", 3},        {L"Атака", "attack", 0},       {L"Сильная атака", "strong", 1},
    {L"Сюрикен", "shuriken", 2},   {L"Подкат", "dash", 4},        {L"Блок", "guard", 5},
    {L"Смена героя", "change", 6}, {L"Камера", "camera", 7},      {L"Информация", "info", 8},
};
// The ready layouts (src/mod/input.cpp): the game's own, and WASD with the mouse.
constexpr int kLayouts[2][kSlots] = {
    {0x21, 0x20, 0x13, 0x39, 0x12, 0x1F, 0x2A, 0x3B, 0x3C, 0x48, 0x50, 0x4B, 0x4D},
    {kMouseLeft, kMouseRight, 0x12, 0x39, 0x2A, 0x10, 0x21, 0x3B, 0x3C, 0x11, 0x1F, 0x1E, 0x20}};
// The codes the game's key page takes (its enabled[256], 0x6A1148) but for the arrows, which are the
// game's fixed keys beside the table, as Enter and Esc are.
constexpr unsigned char kAllowed[] = {
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,
    0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3B, 0x3C,
    0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E,
    0x4F, 0x50, 0x51, 0x52, 0x53, 0x57, 0x58, 0x9D, 0xB5, 0xB8, 0xC7, 0xC9, 0xCF, 0xD1, 0xD2, 0xD3};
// Two players at one keyboard (src/mod/pads.cpp): the left half's key and the right one's.
const struct {
  const wchar_t* label;
  const wchar_t* keys;
} kHalves[kHalfRows] = {
    {L"Ходить", L"WASD · стрелки"},  {L"Атака", L"F · Num 1"},            {L"Сильная атака", L"G · Num 2"},
    {L"Прыжок", L"Пробел · Num 0"},  {L"Сюрикен", L"R · Num 5"},          {L"Подкат", L"Q · Num 4"},
    {L"Блок", L"E · Num 6"},         {L"Смена героя", L"Л. Shift · Num 3"}, {L"Камера", L"X · Num 9"},
    {L"Информация", L"Z · Num 7"},   {L"Start", L"Tab · Num Enter"},      {L"Select", L"` · Num -"},
};

struct Res {
  std::wstring name, value;
};

struct State {
  std::wstring dir, ini;  // the folder; tmnt2net.ini where the game reads it
  bool mod = false, canned = false, levels = true;
  int cur[kRows] = {};
  std::vector<Res> res;
  int key[kSlots];
  bool own = false;  // own_key: the player's own keys, kept while a ready layout is on
  int own_key[kSlots];
  int players = 1;
  std::wstring error;
  bool kept = false;  // a copy of the ini was put aside before this run's first change
};
State g;

std::wstring env(const wchar_t* name) {
  wchar_t b[MAX_PATH * 2];
  const DWORD n = GetEnvironmentVariableW(name, b, MAX_PATH * 2);
  return n && n < MAX_PATH * 2 ? std::wstring(b, n) : std::wstring();
}

// --- the game's own view of its folder ---------------------------------------------------------------
// TMNT2.exe has no manifest, so in a folder its user may not write to (Program Files) Windows gives it
// copies of the files it writes, under <LocalAppData>\VirtualStore\<the path without its drive>, and
// shows it those copies from then on. The launcher reads and writes the file the game sees.

std::wstring store_root() {
  const std::wstring lab = env(L"TMNT2NET_LAUNCHER_VSTORE");
  if (!lab.empty()) return lab;
  PWSTR local = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = local;
  CoTaskMemFree(local);
  return dir.empty() ? dir : core::join(dir, L"VirtualStore");
}

std::wstring stored(const std::wstring& dir) {
  const std::wstring root = store_root();
  if (root.empty() || dir.size() < 3 || dir[1] != L':') return L"";
  return core::join(core::join(root, dir.substr(3)), kIni);
}

bool is_protected(const std::wstring& dir) {
  if (!env(L"TMNT2NET_LAUNCHER_VSTORE").empty()) return true;  // (lab: any folder)
  for (const wchar_t* name : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432", L"ProgramData", L"SystemRoot"}) {
    const std::wstring p = env(name);
    if (p.empty() || dir.size() <= p.size() || _wcsnicmp(dir.c_str(), p.c_str(), p.size()) != 0) continue;
    if (dir[p.size()] == L'\\' || dir[p.size()] == L'/') return true;
  }
  return false;
}

bool writable(const std::wstring& file) {
  HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  CloseHandle(f);
  return true;
}

std::wstring seen(const std::wstring& dir) {
  const std::wstring copy = stored(dir);
  return !copy.empty() && core::exists(copy) ? copy : core::join(dir, kIni);
}

// Before a change: where it goes, and a copy of the file as it was (once a run).
bool ready() {
  const std::wstring own = core::join(g.dir, kIni);
  if (g.ini == own && is_protected(g.dir) && (!env(L"TMNT2NET_LAUNCHER_VSTORE").empty() || !writable(own))) {
    // The folder is closed to this user: the change goes where Windows would put the game's own.
    const std::wstring copy = stored(g.dir);
    const size_t slash = copy.find_last_of(L'\\');
    const wchar_t* closed = L"Папка игры закрыта для записи: запустите лаунчер от имени администратора.";
    if (copy.empty() || slash == std::wstring::npos) {
      g.error = closed;
      return false;
    }
    SHCreateDirectoryExW(nullptr, copy.substr(0, slash).c_str(), nullptr);
    if (!CopyFileW(own.c_str(), copy.c_str(), TRUE)) {
      g.error = closed;
      return false;
    }
    g.ini = copy;
  }
  if (!g.kept) {
    g.kept = true;
    CopyFileW(g.ini.c_str(), core::join(core::downloads_dir(), std::wstring(kIni) + L".before-launcher").c_str(), FALSE);
  }
  return true;
}

// --- the ini ---------------------------------------------------------------------------------------------

std::wstring get(const wchar_t* section, const wchar_t* key) {
  wchar_t buf[512] = {};
  GetPrivateProfileStringW(section, key, L"", buf, 512, g.ini.c_str());
  std::wstring v(buf);
  const size_t semi = v.find(L';');  // (an inline comment)
  if (semi != std::wstring::npos) v.resize(semi);
  while (!v.empty() && iswspace(v.back())) v.pop_back();
  size_t b = 0;
  while (b < v.size() && iswspace(v[b])) ++b;
  v = v.substr(b);
  for (auto& c : v) c = towlower(c);
  return v;
}

bool put(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
  if (g.canned) return true;
  if (!ready()) return false;
  if (WritePrivateProfileStringW(section, key, value.c_str(), g.ini.c_str())) return true;
  g.error = L"tmnt2net.ini не записался (ошибка " + std::to_wstring(GetLastError()) + L").";
  return false;
}

bool is_number(const std::wstring& v) { return !v.empty() && (iswdigit(v[0]) || v[0] == L'-' || v[0] == L'.'); }

// The choice a value of the ini means: the one that has it, the nearest for a number between the
// choices (an ini written by hand), else the mod's default.
int index_of(const Info& info, std::wstring v) {
  const std::wstring key = info.ini;
  if (key == L"mode" && v == L"fullscreen") v = L"borderless";  // (none since 0.17.0)
  if (key == L"aspect" && (v == L"wide" || v == L"hor+")) v = L"auto";
  if (key == L"intro") v = v == L"1" ? L"skip" : v == L"0" ? L"show" : v;
  if (v.empty()) v = info.fallback;
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < info.choices.size(); ++i)
      if (v == info.choices[i].value) return static_cast<int>(i);
    if (is_number(v) && is_number(info.choices[0].value)) {
      const double want = _wtof(v.c_str());
      int at = 0;
      for (size_t i = 1; i < info.choices.size(); ++i)
        if (std::fabs(_wtof(info.choices[i].value) - want) < std::fabs(_wtof(info.choices[at].value) - want)) at = static_cast<int>(i);
      return at;
    }
    v = info.fallback;  // (a word the mod does not know: it plays with its default)
  }
  return 0;
}

// РАЗРЕШЕНИЕ: the monitor's modes after "as the monitor", and what the file has if it is none of them.
void make_resolutions(const std::wstring& now) {
  std::vector<std::pair<int, int>> sizes;
  if (g.canned) {
    sizes = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
  } else {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i)
      if (dm.dmBitsPerPel == 32 && dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480)
        sizes.push_back({static_cast<int>(dm.dmPelsWidth), static_cast<int>(dm.dmPelsHeight)});
  }
  int w = 0, h = 0;
  const bool own = swscanf_s(now.c_str(), L"%dx%d", &w, &h) == 2 && w >= 320 && h >= 240 && w <= 16384 && h <= 16384;
  if (own) sizes.push_back({w, h});
  std::sort(sizes.begin(), sizes.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
    return a.first * a.second < b.first * b.second || (a.first * a.second == b.first * b.second && a.first < b.first);
  });
  sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
  g.res.clear();
  g.res.push_back({L"Как монитор", L"auto"});
  if (now == L"game") g.res.push_back({L"Из TMNT2.ini", L"game"});
  g.cur[kResolution] = now == L"game" ? 1 : 0;
  for (const auto& s : sizes) {
    if (own && s.first == w && s.second == h) g.cur[kResolution] = static_cast<int>(g.res.size());
    g.res.push_back({std::to_wstring(s.first) + L" × " + std::to_wstring(s.second),
                     std::to_wstring(s.first) + L"x" + std::to_wstring(s.second)});
  }
}

bool parse_keys(const std::wstring& text, int out[kSlots]) {
  int n = 0;
  const wchar_t* p = text.c_str();
  while (*p && n < kSlots) {
    wchar_t* end = nullptr;
    const long v = wcstol(p, &end, 10);
    if (end == p) return false;
    if (v <= 0 || v > 255) return false;
    out[n++] = static_cast<int>(v);
    p = end;
    while (*p == L',' || *p == L' ') ++p;
  }
  return n == kSlots;
}

std::wstring key_list(const int keys[kSlots]) {
  std::wstring out;
  for (int i = 0; i < kSlots; ++i) out += (i ? L"," : L"") + std::to_wstring(keys[i]);
  return out;
}

bool is_layout(int which) { return std::memcmp(g.key, kLayouts[which], sizeof(g.key)) == 0; }

void read_file() {
  g.mod = g.canned || (!g.dir.empty() && core::exists(g.ini));
  for (int r = 0; r < kRows; ++r) {
    const Info& info = kInfo[r];
    const std::wstring v = g.mod && !g.canned ? get(info.section, info.ini) : std::wstring();
    if (r == kResolution) {
      make_resolutions(v);
    } else if (info.kind == Kind::Level) {
      g.cur[r] = std::clamp(is_number(v) ? _wtoi(v.c_str()) : _wtoi(info.fallback), 0, kLevelMax);
    } else {
      g.cur[r] = index_of(info, v);
    }
  }
  std::memcpy(g.key, kLayouts[0], sizeof(g.key));
  g.players = 1;
  if (g.mod && !g.canned) {
    int k[kSlots];
    if (parse_keys(get(L"input", L"keys"), k)) std::memcpy(g.key, k, sizeof(g.key));
    if (parse_keys(get(L"input", L"keys_own"), k)) {
      std::memcpy(g.own_key, k, sizeof(g.own_key));
      g.own = true;
    }
    g.players = get(L"input", L"keyboard_players") == L"2" ? 2 : 1;
  }
}

bool failed() {  // (the page shows what the file has)
  if (g.canned) return false;
  const std::wstring why = g.error;
  read_file();
  g.error = why;
  return false;
}

bool put_keys() { return put(L"input", L"keys", key_list(g.key)) || failed(); }

}  // namespace

void load(const std::wstring& game_dir, const std::wstring& mod_version) {
  if (g.dir != game_dir) g.kept = g.own = false;
  g.canned = false;
  g.dir = game_dir;
  g.ini = game_dir.empty() ? std::wstring() : seen(game_dir);
  g.levels = mod_version.empty() || core::compare_versions(mod_version, kLevelsSince) >= 0;
  g.error.clear();
  if (!g.ini.empty()) WritePrivateProfileStringW(nullptr, nullptr, nullptr, g.ini.c_str());  // (Windows' copy of it: let go)
  read_file();
}

void load_canned(bool two_players) {
  g = State();
  g.canned = true;
  read_file();
  g.cur[kMusic] = 7;
  g.cur[kVoices] = 9;
  g.players = two_players ? 2 : 1;
}

bool has_mod() { return g.mod; }
bool keeps_levels() { return g.levels; }
const std::wstring& error() { return g.error; }
std::wstring ini_file() { return g.ini; }

Kind kind(Row r) { return kInfo[r].kind; }
const wchar_t* label(Row r) { return kInfo[r].label; }
const char* key(Row r) { return kInfo[r].key; }
bool usable(Row r) { return g.mod && (kInfo[r].kind != Kind::Level || g.levels); }

int count(Row r) {
  if (r == kResolution) return static_cast<int>(g.res.size());
  if (kInfo[r].kind == Kind::Level) return kLevelMax + 1;
  return static_cast<int>(kInfo[r].choices.size());
}

int value(Row r) { return g.cur[r]; }

std::wstring name(Row r, int v) {
  if (v < 0 || v >= count(r)) return L"";
  if (r == kResolution) return g.res[v].name;
  if (kInfo[r].kind == Kind::Level) return v == 0 ? std::wstring(L"без звука") : std::to_wstring(v) + L" из 10";
  return kInfo[r].choices[v].name;
}

bool set(Row r, int v) {
  const int n = count(r);
  if (n <= 0 || !usable(r)) return false;
  const Info& info = kInfo[r];
  v = info.kind == Kind::Level ? std::clamp(v, 0, kLevelMax) : ((v % n) + n) % n;
  g.error.clear();
  if (v == g.cur[r]) return true;
  g.cur[r] = v;
  const std::wstring text = r == kResolution ? g.res[v].value : info.kind == Kind::Level ? std::to_wstring(v) : info.choices[v].value;
  return put(info.section, info.ini, text) || failed();
}

bool reset(int page) {
  const int from = page == 2 ? kFirstSound : 0, to = page == 2 ? kRows : kFirstSound;
  for (int i = from; i < to; ++i) {
    const Row r = static_cast<Row>(i);
    if (!usable(r)) continue;
    const Info& info = kInfo[i];
    const int v = r == kResolution ? 0 : info.kind == Kind::Level ? _wtoi(info.fallback) : index_of(info, info.fallback);
    if (!set(r, v)) return false;
  }
  return true;
}

// --- УПРАВЛЕНИЕ -----------------------------------------------------------------------------------------

const wchar_t* action_label(int action) { return kActions[action].label; }
const char* action_key(int action) { return kActions[action].key; }
int key_code(int action) { return g.key[kActions[action].slot]; }

std::wstring key_name(int code) {
  static const struct {
    int code;
    const wchar_t* name;
  } kNames[] = {
      {0x01, L"Esc"},         {0x0C, L"-"},            {0x0D, L"="},          {0x0E, L"Backspace"},
      {0x0F, L"Tab"},         {0x1A, L"["},            {0x1B, L"]"},          {0x1C, L"Enter"},
      {0x1D, L"Левый Ctrl"},  {0x27, L";"},            {0x28, L"'"},          {0x29, L"`"},
      {0x2A, L"Левый Shift"}, {0x2B, L"\\"},           {0x33, L","},          {0x34, L"."},
      {0x35, L"/"},           {0x36, L"Правый Shift"}, {0x37, L"Num *"},      {0x38, L"Левый Alt"},
      {0x39, L"Пробел"},      {0x3A, L"Caps Lock"},    {0x45, L"Num Lock"},   {0x46, L"Scroll Lock"},
      {0x47, L"Num 7"},       {0x48, L"Num 8"},        {0x49, L"Num 9"},      {0x4A, L"Num -"},
      {0x4B, L"Num 4"},       {0x4C, L"Num 5"},        {0x4D, L"Num 6"},      {0x4E, L"Num +"},
      {0x4F, L"Num 1"},       {0x50, L"Num 2"},        {0x51, L"Num 3"},      {0x52, L"Num 0"},
      {0x53, L"Num ."},       {0x57, L"F11"},          {0x58, L"F12"},        {0x9C, L"Num Enter"},
      {0x9D, L"Правый Ctrl"}, {0xB5, L"Num /"},        {0xB8, L"Правый Alt"}, {0xC5, L"Pause"},
      {0xC7, L"Home"},        {0xC8, L"Стрелка вверх"}, {0xC9, L"Page Up"},   {0xCB, L"Стрелка влево"},
      {0xCD, L"Стрелка вправо"}, {0xCF, L"End"},       {0xD0, L"Стрелка вниз"}, {0xD1, L"Page Down"},
      {0xD2, L"Insert"},      {0xD3, L"Delete"},       {0xDB, L"Левый Win"},  {0xDC, L"Правый Win"},
      {kMouseLeft, L"ЛКМ"},   {kMouseRight, L"ПКМ"},   {kMouse4, L"Мышь 4"},  {kMouse5, L"Мышь 5"}};
  for (const auto& k : kNames)
    if (k.code == code) return k.name;
  static const wchar_t kLetters[] = L"QWERTYUIOP\0\0\0\0ASDFGHJKL";  // 0x10..0x26
  if (code >= 0x02 && code <= 0x0B) return std::wstring(1, L"1234567890"[code - 0x02]);
  if (code >= 0x10 && code <= 0x26 && kLetters[code - 0x10]) return std::wstring(1, kLetters[code - 0x10]);
  if (code >= 0x2C && code <= 0x32) return std::wstring(1, L"ZXCVBNM"[code - 0x2C]);
  if (code >= 0x3B && code <= 0x44) return L"F" + std::to_wstring(code - 0x3B + 1);
  if (code <= 0) return L"—";
  wchar_t buf[16];
  swprintf(buf, 16, L"код 0x%02X", code);
  return buf;
}

bool key_allowed(int code) {
  if (code == kMouseLeft || code == kMouseRight || code == kMouse4 || code == kMouse5) return true;
  return std::find(std::begin(kAllowed), std::end(kAllowed), code) != std::end(kAllowed);
}

bool set_key(int action, int code) {
  if (!g.mod || action < 0 || action >= kKeyActions || !key_allowed(code)) return false;
  g.error.clear();
  const int slot = kActions[action].slot, old = g.key[slot];
  if (old == code) return true;
  for (int& k : g.key)
    if (k == code) k = old;
  g.key[slot] = code;
  return put_keys();
}

int layout() { return is_layout(1) ? 1 : is_layout(0) ? 0 : 2; }
bool has_own_layout() { return g.own; }

bool set_layout(int v) {
  const int now = layout();
  if (!g.mod || v < 0 || v > 2) return false;
  if (v == now) return true;
  if (v == 2 && !g.own) return false;
  g.error.clear();
  if (now == 2) {  // the player's own keys are kept: they come back with СВОЯ
    g.own = true;
    std::memcpy(g.own_key, g.key, sizeof(g.key));
    put(L"input", L"keys_own", key_list(g.own_key));
    g.error.clear();  // (not kept in the file: kept until the launcher is closed)
  }
  std::memcpy(g.key, v == 2 ? g.own_key : kLayouts[v], sizeof(g.key));
  return put_keys();
}

bool reset_keys() {
  if (!g.mod) return false;
  return set_layout(0) && set_keyboard_players(1);
}

int keyboard_players() { return g.players; }

bool set_keyboard_players(int n) {
  if (!g.mod || (n != 1 && n != 2)) return false;
  g.error.clear();
  if (n == g.players) return true;
  g.players = n;
  return put(L"input", L"keyboard_players", std::to_wstring(n)) || failed();
}

const wchar_t* half_label(int row) { return kHalves[row].label; }
const wchar_t* half_keys(int row) { return kHalves[row].keys; }

}  // namespace gset
}  // namespace lz
