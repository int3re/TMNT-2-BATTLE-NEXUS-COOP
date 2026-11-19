// tmnt2net.asi entry points.
// - InitializeASI(): called by Ultimate ASI Loader right after LoadLibrary,
//   outside the loader lock and before TMNT2.exe's WinMain. All patches go here.
// - DllMain only starts a fallback thread for loaders that never call
//   InitializeASI: no hooks under the loader lock.
#include <windows.h>

#include <cstring>
#include <string>

#include "common/log.h"
#include "common/sha256.h"
#include "common/version.h"
#include "mod/config.h"
#include "mod/camera.h"
#include "mod/coop.h"
#include "mod/health.h"
#include "mod/fixes.h"
#include "mod/game.h"
#include "mod/input.h"
#include "mod/menus.h"
#include "mod/midjoin.h"
#include "mod/netplay.h"
#include "mod/overlay.h"
#include "mod/postfx.h"
#include "mod/results.h"
#include "mod/sim.h"
#include "mod/video.h"
#include "mod/widescreen.h"

namespace {

volatile LONG g_started = 0;
HANDLE g_init_called = nullptr;

bool running_in_tmnt2() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  const wchar_t* name = wcsrchr(path, L'\\');
  return _wcsicmp(name ? name + 1 : path, L"TMNT2.exe") == 0;
}

void init(const char* how) {
  if (InterlockedExchange(&g_started, 1) != 0) return;

  std::wstring dir = module_dir();
  Config cfg;
  bool have_ini = load_config(dir + L"tmnt2net.ini", cfg);
  netplay::prepare(cfg);
  tlog::open(dir + L"tmnt2net.log", cfg.log_level, false);
  LOGI("tmnt2net " TMNT2NET_VERSION " in %ls, started by %s", dir.c_str(), how);
  if (!have_ini) LOGW("tmnt2net.ini not found next to TMNT2.exe, using defaults");

  fixes::install_diagnostics(cfg);

  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  uint8_t hash[32] = {};
  if (!sha256_file(exe, hash)) {
    LOGE("cannot read %ls; game patches disabled", exe);
    return;
  }
  bool known = memcmp(hash, game::kExeSha256, 32) == 0;
  LOGI("TMNT2.exe sha256 %s (%s)", to_hex(hash, 32).c_str(),
       known ? "NA retail 2004, supported" : "unknown build");
  if (!known) {
    LOGE("this TMNT2.exe is not the one tmnt2net was made for; game patches disabled (the game runs unmodified)");
    return;
  }
  fixes::install(cfg);
  video::install(cfg);
  postfx::install(cfg);
  widescreen::install(cfg);
  sim::install(cfg);
  input::install(cfg);
  netplay::install(cfg);
  menus::install(cfg);
  coop::install(cfg);
  health::install(cfg);
  camera::install(cfg);
  results::install(cfg);
  midjoin::install(cfg);
  if (cfg.panel_test)
    overlay::set_panel({{L"Ожидание: P2 Вася", 0xFFFFD040, true}, {L"(3 с)", 0xFFB0B0B0, false}}, true);
  LOGI("tmnt2net ready");
}

DWORD WINAPI fallback_thread(void*) {
  if (WaitForSingleObject(g_init_called, 3000) == WAIT_TIMEOUT)
    init("DllMain fallback (the loader did not call InitializeASI; start-up patches may be late)");
  return 0;
}

}  // namespace

extern "C" __declspec(dllexport) void InitializeASI() {
  if (!running_in_tmnt2()) return;
  if (g_init_called) SetEvent(g_init_called);
  init("InitializeASI");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    if (running_in_tmnt2()) {
      g_init_called = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      HANDLE t = CreateThread(nullptr, 0, fallback_thread, nullptr, 0, nullptr);
      if (t) CloseHandle(t);
    }
  }
  return TRUE;
}
