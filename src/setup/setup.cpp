// tmnt2net-setup.exe: installs, updates and removes tmnt2net in the game folder.
//   Ultimate ASI Loader (winmm.dll) + tmnt2net.asi + tmnt2net.ini + steam_api.dll
//   + README go next to TMNT2.exe; TMNT2.exe and the game's data are never touched.
// The folder is found through the game's uninstall entry in the registry (the
// usual Inno Setup releases), next to the setup, or picked by hand. TMNT2.exe is
// checked: tmnt2net works with one exe only (SHA-256 below).
// Every installed file and every backup is listed in tmnt2net-install.txt in the
// game folder; uninstall removes exactly those and puts the backups back.
//
// Command line (no UI): /install <dir> | /uninstall <dir> [/quiet]
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/sha256.h"
#include "common/version.h"
#include "setup/payload_ids.h"

namespace {

const wchar_t* kTitle = L"tmnt2net " TEXT(TMNT2NET_VERSION) L" — TMNT 2: Battle Nexus";
const wchar_t* kManifest = L"tmnt2net-install.txt";
const wchar_t* kBackupDir = L"tmnt2net-backup";
const wchar_t* kExe = L"TMNT2.exe";
// The NA retail exe of 2004 (also in every known Russian release): the only one tmnt2net patches.
const char* kExeSha256 = "22f49908bcd3b84278bea888b7285e1e0ba73a72a89764f495aceb76a42d8b6a";
bool g_quiet = false;

// --- small helpers ------------------------------------------------------------------

std::wstring join(const std::wstring& dir, const std::wstring& name) {
  if (dir.empty()) return name;
  wchar_t last = dir.back();
  return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

void message(const std::wstring& text, bool error = false) {
  if (g_quiet) {
    fwprintf(error ? stderr : stdout, L"%ls\n", text.c_str());
    return;
  }
  MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

std::wstring last_error_text(DWORD err) {
  wchar_t* buf = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
  std::wstring s = buf ? buf : L"";
  if (buf) LocalFree(buf);
  while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
  return s + L" (" + std::to_wstring(err) + L")";
}

bool read_resource(int id, const void*& data, DWORD& size) {
  HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
  HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
  data = g ? LockResource(g) : nullptr;
  size = r ? SizeofResource(nullptr, r) : 0;
  return data && size;
}

bool write_file(const std::wstring& path, const void* data, DWORD size, DWORD& err) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    err = GetLastError();
    return false;
  }
  DWORD written = 0;
  bool ok = WriteFile(f, data, size, &written, nullptr) && written == size;
  err = ok ? 0 : GetLastError();
  CloseHandle(f);
  return ok;
}

std::string read_all(const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool file_contains(const std::wstring& path, const char* needle) {
  return read_all(path).find(needle) != std::string::npos;
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

// --- manifest --------------------------------------------------------------------------

struct Manifest {
  std::vector<std::wstring> files;                             // installed by us
  std::vector<std::pair<std::wstring, std::wstring>> backups;  // original -> backup

  bool load(const std::wstring& dir) {
    std::wifstream in(join(dir, kManifest));
    if (!in) return false;
    std::wstring line;
    while (std::getline(in, line)) {
      if (line.rfind(L"file\t", 0) == 0) files.push_back(line.substr(5));
      if (line.rfind(L"backup\t", 0) == 0) {
        size_t tab = line.find(L'\t', 7);
        if (tab != std::wstring::npos) backups.emplace_back(line.substr(7, tab - 7), line.substr(tab + 1));
      }
    }
    return true;
  }
  bool save(const std::wstring& dir) const {
    std::wofstream out(join(dir, kManifest));
    out << L"# tmnt2net " << TEXT(TMNT2NET_VERSION) << L". tmnt2net-setup.exe /uninstall uses this list.\n";
    for (auto& f : files) out << L"file\t" << f << L"\n";
    for (auto& b : backups) out << L"backup\t" << b.first << L"\t" << b.second << L"\n";
    return static_cast<bool>(out);
  }
  bool has(const std::wstring& name) const {
    for (auto& f : files)
      if (_wcsicmp(f.c_str(), name.c_str()) == 0) return true;
    return false;
  }
};

// --- install / uninstall -------------------------------------------------------------------

enum class Result { Ok, Failed, AccessDenied };

// The same bytes as the payload: an earlier copy of this file (a manual install).
bool same_as_payload(const std::wstring& path, int id) {
  const void* data = nullptr;
  DWORD size = 0;
  if (!id || !read_resource(id, data, size)) return false;
  std::string have = read_all(path);
  return have.size() == size && memcmp(have.data(), data, size) == 0;
}

// Puts one payload file into dir. An existing file that is not ours is moved to
// tmnt2net-backup\ first (and restored on uninstall).
Result put(const std::wstring& dir, int id, const std::wstring& name, Manifest& m, std::wstring& log,
           const std::string* data_override = nullptr) {
  std::wstring dst = join(dir, name);
  // Ours: listed, the mod's own file of an earlier version, or byte-identical to ours.
  bool ours = m.has(name) || _wcsicmp(name.c_str(), L"tmnt2net.asi") == 0 ||
              _wcsicmp(name.c_str(), L"tmnt2net-README.md") == 0 || same_as_payload(dst, id);
  if (exists(dst) && !ours) {
    std::wstring bdir = join(dir, kBackupDir);
    CreateDirectoryW(bdir.c_str(), nullptr);
    std::wstring bak = join(bdir, name);
    if (!MoveFileExW(dst.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      DWORD e = GetLastError();
      log += L"не удалось сохранить старый " + name + L": " + last_error_text(e) + L"\n";
      return e == ERROR_ACCESS_DENIED ? Result::AccessDenied : Result::Failed;
    }
    m.backups.emplace_back(name, std::wstring(kBackupDir) + L"\\" + name);
    log += L"прежний " + name + L" сохранён в " + kBackupDir + L"\\\n";
  }
  const void* data = nullptr;
  DWORD size = 0, err = 0;
  if (data_override) {
    data = data_override->data();
    size = (DWORD)data_override->size();
  } else if (!read_resource(id, data, size)) {
    log += L"в установщике нет файла " + name + L"\n";
    return Result::Failed;
  }
  if (!write_file(dst, data, size, err)) {
    log += L"не удалось записать " + name + L": " + last_error_text(err) + L"\n";
    return err == ERROR_ACCESS_DENIED ? Result::AccessDenied : Result::Failed;
  }
  if (!m.has(name)) m.files.push_back(name);
  log += name + L"\n";
  return Result::Ok;
}

std::wstring exe_check(const std::wstring& dir, bool* supported) {
  uint8_t h[32] = {};
  *supported = false;
  if (!sha256_file(join(dir, kExe), h)) return L"TMNT2.exe не читается";
  if (to_hex(h, 32) == kExeSha256) {
    *supported = true;
    return L"TMNT2.exe: та версия, с которой работает мод";
  }
  std::string hex = to_hex(h, 8);
  return L"TMNT2.exe другой версии (SHA-256 " + std::wstring(hex.begin(), hex.end()) +
         L"…): мод его не изменит и в игре ничего не включит";
}

Result install(const std::wstring& dir, std::wstring& log) {
  if (!exists(join(dir, kExe))) {
    log += L"в папке нет TMNT2.exe\n";
    return Result::Failed;
  }
  if (game_running(dir)) {
    log += L"игра сейчас запущена: закройте её и запустите установщик снова\n";
    return Result::Failed;
  }
  bool supported = false;
  log += exe_check(dir, &supported) + L"\n";
  Manifest m;
  m.load(dir);
  Result r = Result::Ok;
  // An Ultimate ASI Loader that is already there (for another mod) is reused.
  std::wstring loader = join(dir, L"winmm.dll");
  if (exists(loader) && !m.has(L"winmm.dll") && !same_as_payload(loader, RES_UAL) &&
      file_contains(loader, "IsUltimateASILoader"))
    log += L"winmm.dll: уже стоит Ultimate ASI Loader, используется он\n";
  else
    r = put(dir, RES_UAL, L"winmm.dll", m, log);
  if (r == Result::Ok) r = put(dir, RES_ASI, L"tmnt2net.asi", m, log);
  const void* steam = nullptr;
  DWORD steam_size = 0;
  if (r == Result::Ok && read_resource(RES_STEAM, steam, steam_size)) {
    r = put(dir, RES_STEAM, L"steam_api.dll", m, log);
    std::string app_id = "480";  // Steam knows tmnt2net as Valve's test app Spacewar
    if (r == Result::Ok && !exists(join(dir, L"steam_appid.txt"))) r = put(dir, 0, L"steam_appid.txt", m, log, &app_id);
  }
  if (r == Result::Ok) {
    // Settings already there (an earlier version, set up by hand) stay as they are.
    if (exists(join(dir, L"tmnt2net.ini"))) {
      log += L"tmnt2net.ini: оставлены ваши настройки\n";
      if (!m.has(L"tmnt2net.ini")) m.files.push_back(L"tmnt2net.ini");
    } else {
      r = put(dir, RES_INI, L"tmnt2net.ini", m, log);
    }
  }
  if (r == Result::Ok) r = put(dir, RES_README, L"tmnt2net-README.md", m, log);
  m.save(dir);
  return r;
}

Result uninstall(const std::wstring& dir, std::wstring& log) {
  Manifest m;
  if (!m.load(dir)) {
    log += L"в папке нет " + std::wstring(kManifest) + L": tmnt2net здесь не ставился этим установщиком\n";
    return Result::Failed;
  }
  if (game_running(dir)) {
    log += L"игра сейчас запущена: закройте её и запустите установщик снова\n";
    return Result::Failed;
  }
  Result r = Result::Ok;
  for (auto& f : m.files) {
    std::wstring p = join(dir, f);
    if (exists(p) && !DeleteFileW(p.c_str())) {
      DWORD e = GetLastError();
      log += L"не удалось удалить " + f + L": " + last_error_text(e) + L"\n";
      r = e == ERROR_ACCESS_DENIED ? Result::AccessDenied : Result::Failed;
    } else {
      log += L"удалён " + f + L"\n";
    }
  }
  for (auto& b : m.backups)
    if (MoveFileExW(join(dir, b.second).c_str(), join(dir, b.first).c_str(), MOVEFILE_REPLACE_EXISTING))
      log += L"возвращён прежний " + b.first + L"\n";
  // What the mod made itself while the game ran (the player's own save stays).
  for (const wchar_t* extra : {L"tmnt2net.log", L"tmnt2net-sim.log", L"tmntsave-net.dat"})
    if (DeleteFileW(join(dir, extra).c_str())) log += L"удалён " + std::wstring(extra) + L"\n";
  RemoveDirectoryW(join(dir, kBackupDir).c_str());  // only if empty
  if (r == Result::Ok) DeleteFileW(join(dir, kManifest).c_str());
  return r;
}

// --- finding the game ----------------------------------------------------------------------

std::wstring reg_string(HKEY root, const std::wstring& key, const wchar_t* value, REGSAM view) {
  HKEY k;
  if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) return L"";
  wchar_t buf[MAX_PATH * 2];
  DWORD size = sizeof(buf), type = 0;
  std::wstring out;
  if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
      (type == REG_SZ || type == REG_EXPAND_SZ))
    out.assign(buf, wcsnlen(buf, size / sizeof(wchar_t)));
  RegCloseKey(k);
  return out;
}

std::wstring find_game_dir() {
  // Running from the game folder counts first.
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring here(self);
  here.resize(here.find_last_of(L'\\'));
  if (exists(join(here, kExe))) return here;
  const wchar_t* base = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
      HKEY k;
      if (RegOpenKeyExW(root, base, 0, KEY_READ | view, &k) != ERROR_SUCCESS) continue;
      wchar_t name[256];
      for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring sub = std::wstring(base) + L"\\" + name;
        for (const wchar_t* v : {L"InstallLocation", L"Inno Setup: App Path"}) {
          std::wstring dir = reg_string(root, sub, v, view);
          if (!dir.empty() && exists(join(dir, kExe))) {  // any entry that holds TMNT2.exe
            RegCloseKey(k);
            return dir;
          }
        }
      }
      RegCloseKey(k);
    }
  }
  return L"";
}

bool pick_folder(const wchar_t* title, const std::wstring& initial, std::wstring& out) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(title);
  IShellItem* init = nullptr;
  if (!initial.empty() && SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&init)))) {
    dlg->SetFolder(init);
    init->Release();
  }
  bool ok = false;
  if (SUCCEEDED(dlg->Show(nullptr))) {
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

// --- UI --------------------------------------------------------------------------------------

enum Action { kNone, kInstall, kUninstall };

int task_dialog(const wchar_t* main, const wchar_t* content, const std::vector<std::pair<int, std::wstring>>& buttons,
                bool links) {
  std::vector<TASKDIALOG_BUTTON> b;
  for (auto& x : buttons) b.push_back({x.first, x.second.c_str()});
  TASKDIALOGCONFIG c{sizeof(c)};
  c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | (links ? TDF_USE_COMMAND_LINKS : 0);
  c.pszWindowTitle = kTitle;
  c.pszMainInstruction = main;
  c.pszContent = content;
  c.cButtons = (UINT)b.size();
  c.pButtons = b.data();
  c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  int pressed = IDCANCEL;
  TaskDialogIndirect(&c, &pressed, nullptr, nullptr);
  return pressed;
}

Action ask_action() {
  int r = task_dialog(
      L"Что сделать?",
      L"tmnt2net для TMNT 2: Battle Nexus: запуск на Windows 10/11, широкий экран, геймпады, сетевая игра "
      L"до 4 игроков (у каждого своя копия игры). TMNT2.exe и файлы игры не меняются.",
      {{100, L"Установить или обновить мод\nВ папку игры (где лежит TMNT2.exe)"},
       {101, L"Удалить мод\nВернуть всё как было до установки"}},
      true);
  return r == 100 ? kInstall : r == 101 ? kUninstall : kNone;
}

bool confirm_dir(const wchar_t* what, std::wstring& dir) {
  for (;;) {
    std::wstring text = std::wstring(what) + L"\n\n" + (dir.empty() ? L"(игра не найдена, выберите папку)" : dir);
    if (!dir.empty() && exists(join(dir, kExe))) {
      bool supported = false;
      text += L"\n\n" + exe_check(dir, &supported);
    }
    std::vector<std::pair<int, std::wstring>> b = {{201, L"Выбрать другую папку"}};
    if (!dir.empty()) b.insert(b.begin(), {200, L"Продолжить"});
    int r = task_dialog(L"Папка игры", text.c_str(), b, false);
    if (r == 200) return true;
    if (r != 201) return false;
    std::wstring picked;
    if (pick_folder(L"Папка с TMNT2.exe", dir, picked)) dir = picked;
  }
}

// Restarts elevated with an explicit command line (a game in Program Files).
bool relaunch_elevated(const std::wstring& args) {
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  SHELLEXECUTEINFOW sei{sizeof(sei)};
  sei.lpVerb = L"runas";
  sei.lpFile = self;
  sei.lpParameters = args.c_str();
  sei.nShow = SW_SHOWNORMAL;
  return ShellExecuteExW(&sei) != FALSE;
}

int run(Action action, std::wstring dir, bool interactive) {
  std::wstring log;
  Result r = Result::Failed;
  if (interactive && dir.empty()) dir = find_game_dir();
  if (action == kInstall) {
    if (interactive && !confirm_dir(L"Куда установить мод:", dir)) return 1;
    r = install(dir, log);
  } else {
    if (interactive && !confirm_dir(L"Откуда удалить мод:", dir)) return 1;
    r = uninstall(dir, log);
  }
  if (r == Result::AccessDenied && interactive) {
    if (relaunch_elevated(std::wstring(action == kInstall ? L"/install" : L"/uninstall") + L" \"" + dir + L"\""))
      return 0;
  }
  if (r == Result::Ok) {
    std::wstring done = action == kUninstall ? L"Мод удалён из:\n" : L"Готово. Мод установлен в:\n";
    done += dir + L"\n\n" + log;
    if (action == kInstall)
      done += L"\nЗапускайте игру как обычно. Сетевая игра — пункт «СЕТЕВАЯ ИГРА» в главном меню, "
              L"настройки мода — «ДОПОЛНИТЕЛЬНО». Подробно: tmnt2net-README.md.";
    message(done);
    return 0;
  }
  message(L"Не получилось.\n\n" + log, true);
  return 2;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  Action action = kNone;
  std::wstring dir;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"/quiet")
      g_quiet = true;
    else if (a == L"/install")
      action = kInstall;
    else if (a == L"/uninstall")
      action = kUninstall;
    else
      dir = a;
  }
  LocalFree(argv);
  if (g_quiet) {  // print to the parent console
    AttachConsole(ATTACH_PARENT_PROCESS);
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
  }
  int rc;
  if (action != kNone && !dir.empty()) {
    rc = run(action, dir, !g_quiet);
  } else {
    Action a = action != kNone ? action : ask_action();
    rc = a == kNone ? 1 : run(a, dir, true);
  }
  CoUninitialize();
  return rc;
}
