// The launcher's work behind the picture: settings, the game folder and the mod in it, the newest release
// (tmnt2net-latest.json of the GitHub release), its installer.
#include "launcher/launcher.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include "common/sha256.h"

namespace lz {
namespace core {

std::wstring g_repo_url, g_settings_path, g_cache_dir;

std::wstring repo() { return g_repo_url.empty() ? std::wstring(kRepoUrl) : g_repo_url; }

namespace {

const wchar_t kExe[] = L"TMNT2.exe";
const wchar_t kInstallList[] = L"tmnt2net-install.txt";
// The NA retail exe of 2004 (also in every known Russian release): the only one tmnt2net patches.
const char kExeSha256[] = "22f49908bcd3b84278bea888b7285e1e0ba73a72a89764f495aceb76a42d8b6a";

// Versions of the mod and the protocol each speaks (docs/CHANGELOG.md). A release's own description
// (Release::protocols) comes first; this is for an installed version it does not name.
const struct {
  const wchar_t *from, *to;
  int protocol;
} kProtocols[] = {
    {L"0.17.0", L"0.17.0", 15}, {L"0.16.6", L"0.16.7", 14}, {L"0.16.4", L"0.16.5", 13}, {L"0.16.3", L"0.16.3", 12},
    {L"0.16.0", L"0.16.2", 11}, {L"0.14.0", L"0.15.0", 10}, {L"0.13.0", L"0.13.0", 9},  {L"0.12.2", L"0.12.2", 8},
    {L"0.12.0", L"0.12.1", 7},  {L"0.10.0", L"0.11.0", 6},  {L"0.9.0", L"0.9.1", 5},    {L"0.8.0", L"0.8.0", 4},
};

std::wstring folder(int csidl) {
  wchar_t buf[MAX_PATH] = {};
  SHGetFolderPathW(nullptr, csidl, nullptr, 0, buf);
  return buf;
}

std::wstring settings_file() {
  if (!g_settings_path.empty()) return g_settings_path;
  std::wstring dir = join(folder(CSIDL_APPDATA), L"tmnt2net");
  CreateDirectoryW(dir.c_str(), nullptr);
  return join(dir, L"launcher.ini");
}

std::wstring cache_dir() {
  std::wstring dir = g_cache_dir;
  if (dir.empty()) {
    dir = join(folder(CSIDL_LOCAL_APPDATA), L"tmnt2net");
    CreateDirectoryW(dir.c_str(), nullptr);
    dir = join(dir, L"cache");
  }
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::wstring ini_get(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* def = L"") {
  wchar_t buf[2048] = {};
  GetPrivateProfileStringW(section, key, def, buf, 2048, file.c_str());
  return buf;
}

void ini_set(const std::wstring& file, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
  WritePrivateProfileStringW(section, key, value.c_str(), file.c_str());
}

std::string read_all(const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool version_text(const std::wstring& v) {
  if (v.empty() || v.size() > 20) return false;
  for (wchar_t ch : v)
    if (!((ch >= L'0' && ch <= L'9') || ch == L'.')) return false;
  return v[0] != L'.' && v.back() != L'.';
}

// --- a small JSON reader ---------------------------------------------------------------------

struct Json {
  enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;
  const Json* get(const char* key) const {
    for (auto& kv : o)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  std::string str(const char* key) const {
    const Json* j = get(key);
    return j && j->type == Str ? j->s : std::string();
  }
  double num(const char* key) const {
    const Json* j = get(key);
    return j && j->type == Num ? j->n : 0;
  }
};

struct JsonReader {
  const char* p;
  const char* end;
  void ws() {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
  }
  static void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(unsigned& v) {
    if (end - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      char ch = *p++;
      v <<= 4;
      if (ch >= '0' && ch <= '9') v |= ch - '0';
      else if (ch >= 'a' && ch <= 'f') v |= ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F') v |= ch - 'A' + 10;
      else return false;
    }
    return true;
  }
  bool string(std::string& out) {
    if (p >= end || *p != '"') return false;
    ++p;
    while (p < end && *p != '"') {
      if (*p != '\\') {
        out += *p++;
        continue;
      }
      if (++p >= end) return false;
      char e = *p++;
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          unsigned cp = 0, low = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            p += 2;
            if (!hex4(low)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          utf8(out, cp);
          break;
        }
        default: out += e; break;
      }
    }
    if (p >= end) return false;
    ++p;
    return true;
  }
  bool value(Json& j, int depth) {
    if (depth > 16) return false;
    ws();
    if (p >= end) return false;
    if (*p == '{') {
      ++p;
      j.type = Json::Obj;
      ws();
      if (p < end && *p == '}') return ++p, true;
      for (;;) {
        ws();
        std::string key;
        if (!string(key)) return false;
        ws();
        if (p >= end || *p++ != ':') return false;
        Json v;
        if (!value(v, depth + 1)) return false;
        j.o.emplace_back(std::move(key), std::move(v));
        ws();
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == '}') return ++p, true;
        return false;
      }
    }
    if (*p == '[') {
      ++p;
      j.type = Json::Arr;
      ws();
      if (p < end && *p == ']') return ++p, true;
      for (;;) {
        Json v;
        if (!value(v, depth + 1)) return false;
        j.a.push_back(std::move(v));
        ws();
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == ']') return ++p, true;
        return false;
      }
    }
    if (*p == '"') {
      j.type = Json::Str;
      return string(j.s);
    }
    if (end - p >= 4 && !std::strncmp(p, "true", 4)) return p += 4, j.type = Json::Bool, j.b = true, true;
    if (end - p >= 5 && !std::strncmp(p, "false", 5)) return p += 5, j.type = Json::Bool, true;
    if (end - p >= 4 && !std::strncmp(p, "null", 4)) return p += 4, true;
    char buf[40];
    int n = 0;
    while (p < end && n < 39 && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E'))
      buf[n++] = *p++;
    if (!n) return false;
    buf[n] = 0;
    j.type = Json::Num;
    j.n = atof(buf);
    return true;
  }
};

std::wstring clipped(const std::string& utf8, size_t max_chars) {
  std::wstring w = wide(utf8);
  for (wchar_t& ch : w)
    if (ch < 0x20) ch = L' ';
  if (w.size() > max_chars) w = w.substr(0, max_chars - 1) + L"…";
  return w;
}

std::wstring date_text(const std::string& iso) {  // 2026-11-20 -> 20 ноября 2026
  static const wchar_t* months[12] = {L"января", L"февраля", L"марта",    L"апреля",  L"мая",    L"июня",
                                      L"июля",   L"августа", L"сентября", L"октября", L"ноября", L"декабря"};
  int y = 0, mo = 0, d = 0;
  if (sscanf_s(iso.c_str(), "%d-%d-%d", &y, &mo, &d) != 3 || mo < 1 || mo > 12 || d < 1 || d > 31 || y < 2000 || y > 2999)
    return L"";
  return std::to_wstring(d) + L" " + months[mo - 1] + L" " + std::to_wstring(y);
}

bool writable(const std::wstring& dir) {
  std::wstring probe = join(dir, L"tmnt2net-write-test.tmp");
  HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                         nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  CloseHandle(f);
  return true;
}

// A path as one argument of a command line (a backslash before the closing quote would swallow it).
std::wstring quoted(std::wstring path) {
  size_t tail = 0;
  while (tail < path.size() && path[path.size() - 1 - tail] == L'\\') ++tail;
  return L"\"" + path + std::wstring(tail, L'\\') + L"\"";
}

// Runs the installer and waits for it.
// In a folder this user may write to: without its windows. (The installers up to 0.17.0 write their
// report to a console when they are quiet and fall over where there is none - after their work is done:
// cmd.exe, itself unseen, gives them one.)
// In a folder this user may not write to (Program Files): through the Windows prompt for administrator
// rights, and in the open - Windows names the installer in its prompt, and it shows what it did.
int run_setup(const std::wstring& setup, const wchar_t* verb, const std::wstring& dir, bool* denied) {
  *denied = false;
  DWORD code = 99;
  if (!writable(dir)) {
    std::wstring args = std::wstring(verb) + L" " + quoted(dir);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = setup.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
      *denied = true;
      return 99;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return static_cast<int>(code);
  }
  wchar_t shell[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"ComSpec", shell, MAX_PATH) || !exists(shell)) {
    GetSystemDirectoryW(shell, MAX_PATH);
    wcscat_s(shell, L"\\cmd.exe");
  }
  std::wstring cmd = quoted(shell) + L" /d /s /c \"" + quoted(setup) + L" " + verb + L" " + quoted(dir) + L" /quiet\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(shell, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    if (GetLastError() == ERROR_ELEVATION_REQUIRED) *denied = true;
    return 99;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return static_cast<int>(code);
}

std::vector<net::Route> routes(const Settings& s, const net::Route* first) {
  std::vector<net::Route> out;
  auto add = [&](const net::Route& r) {
    for (auto& o : out)
      if (o.kind == r.kind && o.proxy == r.proxy) return;
    out.push_back(r);
  };
  if (first && !first->label.empty()) add(*first);
  if (s.proxy == 1) {
    add({1, L"", L"напрямую"});
  } else if (s.proxy == 2 && !s.proxy_addr.empty()) {
    add({2, s.proxy_addr, L"через прокси " + s.proxy_addr});
  } else {
    // Automatic: as Windows is set up, then the HTTP port of a VPN client on this PC, if one listens.
    add({0, L"", L"напрямую"});
    for (int port : {10809, 7890, 7897, 2080, 2081, 8889})
      if (net::port_open(L"127.0.0.1", port, 150)) {
        std::wstring addr = L"127.0.0.1:" + std::to_wstring(port);
        add({2, addr, L"через прокси " + addr});
      }
  }
  return out;
}

std::string hex_of_file(const std::wstring& path) {
  uint8_t h[32] = {};
  return sha256_file(path, h) ? to_hex(h, 32) : std::string();
}

// The installer of a release in the cache: there already (and whole), or downloaded now.
Step fetch_setup(const Release& rel, const Settings& s, const net::Route& first, const UpdateHooks& hooks,
                 const std::atomic<bool>& cancel, std::wstring* path) {
  std::wstring setup = cached_setup(rel.version);
  *path = setup;
  if (hooks.stage) hooks.stage(0);
  bool have = exists(setup) && (rel.setup_sha256.empty() || hex_of_file(setup) == rel.setup_sha256);
  if (!have) {
    std::wstring name = rel.setup_name.empty() ? L"tmnt2net-setup.exe" : rel.setup_name;
    std::wstring url = repo() + L"/releases/download/v" + rel.version + L"/" + name;
    std::wstring part = setup + L".part";
    bool ok = false;
    for (const net::Route& r : routes(s, &first)) {
      if (cancel.load()) return Step::Cancelled;
      net::Response resp = net::get(url, r, true, part, hooks.progress, &cancel);
      if (resp.error == ERROR_CANCELLED) return Step::Cancelled;
      if (resp.status == 200 && !resp.error) {
        ok = true;
        break;
      }
    }
    if (!ok) return Step::NetFail;
    DeleteFileW(setup.c_str());
    if (!MoveFileW(part.c_str(), setup.c_str())) return Step::NetFail;
  } else if (hooks.progress) {
    hooks.progress(rel.setup_size ? rel.setup_size : 1, rel.setup_size ? rel.setup_size : 1);
  }
  if (hooks.stage) hooks.stage(1);
  if (!rel.setup_sha256.empty() && hex_of_file(setup) != rel.setup_sha256) {
    DeleteFileW(setup.c_str());
    return Step::BadSum;
  }
  Sleep(350);  // (the step is seen)
  return Step::Ok;
}

// The game holds the mod's files while it runs: wait for it to close.
Step wait_for_game(const std::wstring& dir, const UpdateHooks& hooks, const std::atomic<bool>& cancel) {
  if (!game_running(dir)) return Step::Ok;
  if (hooks.game_open) hooks.game_open(true);
  while (game_running(dir)) {
    if (cancel.load()) return Step::Cancelled;
    Sleep(300);
  }
  Sleep(400);  // (its process lets go of the folder)
  if (hooks.game_open) hooks.game_open(false);
  return Step::Ok;
}

}  // namespace

std::wstring join(const std::wstring& dir, const std::wstring& name) {
  if (dir.empty()) return name;
  wchar_t last = dir.back();
  return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

std::wstring trimmed(std::wstring dir) {
  while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
  return dir;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string narrow(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

int compare_versions(const std::wstring& a, const std::wstring& b) {
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    unsigned long x = 0, y = 0;
    while (i < a.size() && a[i] != L'.') x = x * 10 + (a[i++] - L'0');
    while (j < b.size() && b[j] != L'.') y = y * 10 + (b[j++] - L'0');
    if (x != y) return x < y ? -1 : 1;
    if (i < a.size()) ++i;
    if (j < b.size()) ++j;
  }
  return 0;
}

int known_protocol(const Release& rel, const std::wstring& version) {
  if (!version_text(version)) return 0;
  if (rel.known && rel.version == version && rel.protocol) return rel.protocol;
  for (auto& p : rel.protocols)
    if (p.first == version) return p.second;
  for (auto& k : kProtocols)
    if (compare_versions(version, k.from) >= 0 && compare_versions(version, k.to) <= 0) return k.protocol;
  return 0;
}

void load_settings(Settings& s) {
  std::wstring f = settings_file();
  s.game_dir = ini_get(f, L"launcher", L"game_dir");
  s.auto_check = ini_get(f, L"launcher", L"auto_check", L"1") != L"0";
  s.auto_install = ini_get(f, L"launcher", L"auto_install", L"0") == L"1";
  s.close_on_play = ini_get(f, L"launcher", L"close_on_play", L"1") != L"0";
  s.proxy = _wtoi(ini_get(f, L"launcher", L"proxy", L"0").c_str());
  if (s.proxy < 0 || s.proxy > 2) s.proxy = 0;
  s.proxy_addr = ini_get(f, L"launcher", L"proxy_addr");
  s.accent = _wtoi(ini_get(f, L"launcher", L"accent", L"3").c_str()) & 3;
  s.notes_version = ini_get(f, L"notes", L"version");
  s.notes_title = ini_get(f, L"notes", L"title");
  s.notes_date = ini_get(f, L"notes", L"date");
  s.notes.clear();
  int n = _wtoi(ini_get(f, L"notes", L"count", L"0").c_str());
  for (int i = 0; i < n && i < 8; ++i) {
    std::wstring line = ini_get(f, L"notes", (L"n" + std::to_wstring(i)).c_str());
    if (!line.empty()) s.notes.push_back(line);
  }
}

void save_settings(const Settings& s) {
  std::wstring f = settings_file();
  if (!exists(f)) {  // UTF-16 with its mark: the folder and the notes keep their letters
    HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      const wchar_t head[] = L"\xFEFF; tmnt2net-launcher\r\n";
      DWORD w = 0;
      WriteFile(h, head, static_cast<DWORD>(sizeof(head) - sizeof(wchar_t)), &w, nullptr);
      CloseHandle(h);
    }
  }
  ini_set(f, L"launcher", L"game_dir", s.game_dir);
  ini_set(f, L"launcher", L"auto_check", s.auto_check ? L"1" : L"0");
  ini_set(f, L"launcher", L"auto_install", s.auto_install ? L"1" : L"0");
  ini_set(f, L"launcher", L"close_on_play", s.close_on_play ? L"1" : L"0");
  ini_set(f, L"launcher", L"proxy", std::to_wstring(s.proxy));
  ini_set(f, L"launcher", L"proxy_addr", s.proxy_addr);
  ini_set(f, L"launcher", L"accent", std::to_wstring(s.accent));
  ini_set(f, L"notes", L"version", s.notes_version);
  ini_set(f, L"notes", L"title", s.notes_title);
  ini_set(f, L"notes", L"date", s.notes_date);
  ini_set(f, L"notes", L"count", std::to_wstring(s.notes.size()));
  for (size_t i = 0; i < s.notes.size() && i < 8; ++i) ini_set(f, L"notes", (L"n" + std::to_wstring(i)).c_str(), s.notes[i]);
}

bool has_game(const std::wstring& dir) { return !dir.empty() && exists(join(dir, kExe)); }

bool exe_supported(const std::wstring& dir) {
  uint8_t h[32] = {};
  return sha256_file(join(dir, kExe), h) && to_hex(h, 32) == kExeSha256;
}

std::wstring find_game_dir() {
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring here(self);
  here.resize(here.find_last_of(L'\\'));
  if (has_game(here)) return here;
  // The game's uninstall entry (the usual installers write where it is).
  const wchar_t* base = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
      HKEY k;
      if (RegOpenKeyExW(root, base, 0, KEY_READ | view, &k) != ERROR_SUCCESS) continue;
      wchar_t name[256];
      for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY sub;
        if (RegOpenKeyExW(k, name, 0, KEY_READ | view, &sub) != ERROR_SUCCESS) continue;
        for (const wchar_t* value : {L"InstallLocation", L"Inno Setup: App Path"}) {
          wchar_t buf[MAX_PATH * 2] = {};
          DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
          if (RegQueryValueExW(sub, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
              (type == REG_SZ || type == REG_EXPAND_SZ) && has_game(buf)) {
            RegCloseKey(sub);
            RegCloseKey(k);
            return buf;
          }
        }
        RegCloseKey(sub);
      }
      RegCloseKey(k);
    }
  }
  // Beside the launcher: the folder above its own, or one next to its own (the mod's kit unpacked by the
  // game's folder). Several copies of the game there: the one the mod was installed into, if it is the
  // only such - else the player says which.
  size_t up = here.find_last_of(L'\\');
  if (up != std::wstring::npos && up > 2) {
    std::wstring parent = here.substr(0, up);
    if (has_game(parent)) return parent;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(parent, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      std::vector<std::wstring> copies, with_mod;
      int seen = 0;
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::wstring dir = join(parent, fd.cFileName);
        if (!has_game(dir)) continue;
        copies.push_back(dir);
        if (exists(join(dir, kInstallList))) with_mod.push_back(dir);
      } while (++seen < 2000 && FindNextFileW(h, &fd));
      FindClose(h);
      if (copies.size() == 1) return copies[0];
      if (with_mod.size() == 1) return with_mod[0];
    }
  }
  return L"";
}

Installed read_installed(const std::wstring& dir) {
  Installed in;
  if (!has_game(dir)) return in;
  // The installer's list begins "# tmnt2net 0.17.0. ..." (later ones: "(protocol 15)").
  std::string list = read_all(join(dir, kInstallList));
  const char* tag = "# tmnt2net ";
  if (list.compare(0, std::strlen(tag), tag) == 0 && exists(join(dir, L"tmnt2net.asi"))) {
    size_t p = std::strlen(tag), e = p;
    while (e < list.size() && ((list[e] >= '0' && list[e] <= '9') || list[e] == '.')) ++e;
    std::string v = list.substr(p, e - p);
    while (!v.empty() && v.back() == '.') v.pop_back();
    in.present = true;
    in.version = wide(v);
    if (!version_text(in.version)) in.version.clear();
    size_t line_end = list.find('\n');
    size_t pr = list.find("protocol ");
    if (pr != std::string::npos && pr < line_end) in.protocol = atoi(list.c_str() + pr + 9);
  } else if (exists(join(dir, L"tmnt2net.asi"))) {
    in.present = true;  // (put there by hand: the version is not written anywhere)
  }
  return in;
}

// A running TMNT2.exe from this folder keeps tmnt2net.asi open.
bool game_running(const std::wstring& dir) {
  std::wstring asi = join(dir, L"tmnt2net.asi");
  if (!exists(asi)) return false;
  HANDLE f = CreateFileW(asi.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
  CloseHandle(f);
  return false;
}

namespace {
struct CloseAsk {
  std::wstring exe;
  int asked = 0;
};
BOOL CALLBACK close_window(HWND hwnd, LPARAM lp) {
  auto* ask = reinterpret_cast<CloseAsk*>(lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid || pid == GetCurrentProcessId() || GetWindow(hwnd, GW_OWNER)) return TRUE;
  HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc) return TRUE;
  wchar_t path[MAX_PATH * 2] = {};
  DWORD size = MAX_PATH * 2;
  if (QueryFullProcessImageNameW(proc, 0, path, &size) && _wcsicmp(path, ask->exe.c_str()) == 0) {
    PostMessageW(hwnd, WM_CLOSE, 0, 0);  // (as the window's own cross: the mod ends the game cleanly)
    ++ask->asked;
  }
  CloseHandle(proc);
  return TRUE;
}
}  // namespace

int close_game(const std::wstring& dir) {
  CloseAsk ask{join(dir, kExe)};
  EnumWindows(close_window, reinterpret_cast<LPARAM>(&ask));
  return ask.asked;
}

bool launch_game(const std::wstring& dir) {
  std::wstring exe = join(dir, kExe);
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.lpFile = exe.c_str();
  sei.lpDirectory = dir.c_str();
  sei.nShow = SW_SHOWNORMAL;
  return ShellExecuteExW(&sei) != FALSE;
}

bool pick_folder(HWND owner, const std::wstring& initial, std::wstring& out) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(L"Папка игры: где лежит TMNT2.exe");
  IShellItem* init = nullptr;
  if (!initial.empty() && SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&init)))) {
    dlg->SetFolder(init);
    init->Release();
  }
  bool ok = false;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out = path;
        CoTaskMemFree(path);
        ok = true;
      }
      item->Release();
    }
  }
  dlg->Release();
  return ok;
}

bool parse_manifest(const std::string& json, Release& rel) {
  Json j;
  JsonReader r{json.data(), json.data() + json.size()};
  if (json.size() >= 3 && !std::memcmp(json.data(), "\xEF\xBB\xBF", 3)) r.p += 3;
  if (!r.value(j, 0) || j.type != Json::Obj) return false;
  Release out;
  out.version = wide(j.str("version"));
  if (!version_text(out.version)) return false;
  out.known = true;
  out.protocol = static_cast<int>(j.num("protocol"));
  out.date = date_text(j.str("date"));
  out.title = clipped(j.str("title"), 90);
  if (const Json* notes = j.get("notes"))
    for (const Json& n : notes->a)
      if (n.type == Json::Str && out.notes.size() < 6) out.notes.push_back(clipped(n.s, 220));
  if (const Json* setup = j.get("setup")) {
    std::wstring name = wide(setup->str("name"));
    bool fine = !name.empty() && name.size() <= 64 && name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0;
    for (wchar_t ch : name)
      if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'.' || ch == L'-' ||
            ch == L'_'))
        fine = false;
    std::string sha = setup->str("sha256");
    for (char& ch : sha)
      if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch - 'A' + 'a');
    bool hex = sha.size() == 64;
    for (char ch : sha)
      if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) hex = false;
    double size = setup->num("size");
    if (fine && hex && size > 0 && size < 512.0 * 1024 * 1024) {
      out.setup_name = name;
      out.setup_sha256 = sha;
      out.setup_size = static_cast<uint64_t>(size);
      out.full = true;
    }
  }
  if (const Json* l = j.get("launcher")) {
    std::wstring v = wide(l->str("version"));
    if (version_text(v)) out.launcher_version = v;
  }
  if (const Json* p = j.get("protocols"))
    for (auto& kv : p->o) {
      std::wstring v = wide(kv.first);
      if (version_text(v) && kv.second.type == Json::Num && out.protocols.size() < 200)
        out.protocols.emplace_back(v, static_cast<int>(kv.second.n));
    }
  if (!out.full) return false;
  rel = out;
  return true;
}

CheckResult check(const Settings& s, const std::atomic<bool>& cancel) {
  CheckResult out;
  std::wstring base = repo() + L"/releases/latest/download";
  std::vector<net::Route> rs = routes(s, nullptr);
  bool no_description = false;  // the release has no tmnt2net-latest.json
  for (const net::Route& r : rs) {
    if (cancel.load()) return out;
    net::Response resp = net::get(base + L"/tmnt2net-latest.json", r, true);
    out.log += L"описание релиза, " + r.label + L": " +
               (resp.sent ? L"HTTP " + std::to_wstring(resp.status) : L"нет ответа (" + std::to_wstring(resp.error) + L")") + L"\n";
    if (resp.status == 200 && !resp.error && parse_manifest(resp.body, out.rel)) {
      out.answered = true;
      out.route = r;
      return out;
    }
    if (resp.sent) out.answered = true;
    if (resp.sent && resp.status == 404) {  // (answered, and clearly: no other way would say otherwise)
      no_description = true;
      break;
    }
  }
  // No description came. Where "latest" points tells the version at least (github.com alone answers
  // that; the files of a release are on another host).
  for (const net::Route& r : rs) {
    if (cancel.load()) return out;
    net::Response resp = net::get(repo() + L"/releases/latest", r, false);
    out.log += L"последний релиз, " + r.label + L": " +
               (resp.sent ? L"HTTP " + std::to_wstring(resp.status) : L"нет ответа (" + std::to_wstring(resp.error) + L")") + L"\n";
    size_t tag = resp.location.find(L"/releases/tag/v");
    if (resp.sent && (resp.status == 301 || resp.status == 302) && tag != std::wstring::npos) {
      std::wstring v = resp.location.substr(tag + 15);
      if (version_text(v)) {
        out.answered = true;
        out.rel = Release{};
        out.rel.known = true;
        out.rel.version = v;
        out.rel.protocol = known_protocol(out.rel, v);
        out.rel.setup_name = L"tmnt2net-setup.exe";
        out.assets_blocked = !no_description;
        out.route = r;
        return out;
      }
    }
  }
  return out;
}

std::wstring cached_setup(const std::wstring& version) { return join(cache_dir(), L"tmnt2net-setup-" + version + L".exe"); }

Step install(const Release& rel, const Settings& s, const net::Route& first, const UpdateHooks& hooks,
             const std::atomic<bool>& cancel, int* exit_code, std::wstring* setup_path) {
  *exit_code = 0;
  std::wstring setup;
  Step st = fetch_setup(rel, s, first, hooks, cancel, &setup);
  if (setup_path) *setup_path = setup;
  if (st != Step::Ok) return st;
  if (hooks.stage) hooks.stage(2);
  st = wait_for_game(s.game_dir, hooks, cancel);
  if (st != Step::Ok) return st;
  if (cancel.load()) return Step::Cancelled;
  bool denied = false;
  *exit_code = run_setup(setup, L"/install", s.game_dir, &denied);
  if (denied) return Step::Denied;
  // What is in the folder now says more than the installer's code.
  Installed now = read_installed(s.game_dir);
  return *exit_code == 0 || (now.present && now.version == rel.version) ? Step::Ok : Step::SetupFail;
}

Step uninstall(const Release& rel, const Settings& s, const net::Route& first, const UpdateHooks& hooks,
               const std::atomic<bool>& cancel, int* exit_code) {
  *exit_code = 0;
  // Any installer of the mod removes it: the one of the installed version if it is still in the cache,
  // else the newest release's.
  Installed in = read_installed(s.game_dir);
  std::wstring setup = in.version.empty() ? L"" : cached_setup(in.version);
  if (setup.empty() || !exists(setup)) {
    if (!rel.known) return Step::NoSetup;
    Step st = fetch_setup(rel, s, first, hooks, cancel, &setup);
    if (st != Step::Ok) return st;
  } else {
    if (hooks.stage) hooks.stage(1);
  }
  if (hooks.stage) hooks.stage(2);
  Step st = wait_for_game(s.game_dir, hooks, cancel);
  if (st != Step::Ok) return st;
  if (cancel.load()) return Step::Cancelled;
  bool denied = false;
  *exit_code = run_setup(setup, L"/uninstall", s.game_dir, &denied);
  if (denied) return Step::Denied;
  return *exit_code == 0 || !read_installed(s.game_dir).present ? Step::Ok : Step::SetupFail;
}

}  // namespace core
}  // namespace lz
