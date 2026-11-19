#include "mod/fixes.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstring>

#include "common/log.h"
#include "mod/game.h"
#include "mod/netplay.h"
#include "mod/patch.h"

namespace fixes {
namespace {

Config g_cfg;

// --- COptionData::Snapshot -------------------------------------------------------
// The original copies RAWDATA::m_aGamepad[6] from m_pGamepad[0..5] although only
// m_nGamepadNum entries exist. With fewer than 6 the copy reads past the heap
// block; on Windows 10 the block can end at a page boundary and the read faults
// (0x483A3D rep movsd), which WinMain's catch(...) shows as "Unkown Error".
// The detour lends the function a 6-entry array: the real entries plus the
// defaults CGamepadOptionData::Initialize (0x483A90) would have written.

using Snapshot_t = void(__fastcall*)(uint8_t* self, void* edx, void* raw);
Snapshot_t oSnapshot = nullptr;

void fill_default_entry(uint8_t* e, int index) {
  static const uint32_t kButtons[7] = {0x20, 0x40, 0x10, 0x80, 0x400, 0x2000, 0x800};
  std::memset(e, 0, game::kGamepadEntrySize);
  std::memcpy(e, kButtons, sizeof(kButtons));
  e[0x1C] = 1;
  std::memcpy(e + 0x20, &index, 4);
}

void __fastcall hkSnapshot(uint8_t* self, void* edx, void* raw) {
  int n = 0;
  uint8_t* arr = nullptr;
  std::memcpy(&n, self + game::kOptionGamepadNum, 4);
  std::memcpy(&arr, self + game::kOptionGamepadArray, 4);
  if (n >= game::kSnapshotGamepads || n < 0 || !arr) {
    oSnapshot(self, edx, raw);
    return;
  }
  alignas(8) uint8_t tmp[game::kSnapshotGamepads * game::kGamepadEntrySize];
  std::memcpy(tmp, arr, (size_t)n * game::kGamepadEntrySize);
  for (int i = n; i < game::kSnapshotGamepads; ++i) fill_default_entry(tmp + i * game::kGamepadEntrySize, i);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true))
    LOGI("gamepad snapshot: n=%d, padded to %d (the original read past its array here)", n,
         game::kSnapshotGamepads);
  uint8_t* lent = tmp;
  std::memcpy(self + game::kOptionGamepadArray, &lent, 4);
  oSnapshot(self, edx, raw);
  std::memcpy(self + game::kOptionGamepadArray, &arr, 4);
}

// --- second instance ---------------------------------------------------------------
using FindWindowA_t = HWND(WINAPI*)(LPCSTR, LPCSTR);
FindWindowA_t oFindWindowA = nullptr;

HWND WINAPI hkFindWindowA(LPCSTR cls, LPCSTR title) {
  if (cls && !IsBadStringPtrA(cls, 64) && strcmp(cls, game::kWindowClass) == 0) {
    LOGI("second instance allowed: ignoring the game's FindWindow(\"%s\")", cls);
    return nullptr;
  }
  return oFindWindowA(cls, title);
}

// --- diagnostics -------------------------------------------------------------------
using MessageBoxA_t = int(WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
MessageBoxA_t oMessageBoxA = nullptr;

std::atomic<bool> g_quitting{false};

int WINAPI hkMessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type) {
  LOGE("the game shows a message box: \"%s\" (caption \"%s\")", text ? text : "", caption ? caption : "");
  if (g_quitting) {  // the player already closed the game: no error dialog on the way out
    LOGW("the game failed while quitting; ending the process");
    TerminateProcess(GetCurrentProcess(), 0);
  }
  if (g_cfg.no_activate) {  // test runs: never put a dialog in front of the owner
    LOGI("debug: no_activate, the message box is not shown");
    return IDOK;
  }
  return oMessageBoxA(hwnd, text, caption, type);
}

// The game's ASSERT (0x4019A0): callers pass (expression, source file, line,
// "Unknown error."); the release build throws with the last string only, which
// WinMain shows as "Unknown error. Program is ended compulsorily." Log the rest.
using Assert_t = void(__cdecl*)(const char* expr, const char* file, int line, const char* msg);
Assert_t oAssert = nullptr;

const char* safe_str(const char* s) { return s && !IsBadStringPtrA(s, 512) ? s : "?"; }

void __cdecl hkAssert(const char* expr, const char* file, int line, const char* msg) {
  LOGE("game ASSERT failed: %s (%s:%d), called from 0x%08X", safe_str(expr), safe_str(file), line,
       (unsigned)(uintptr_t)_ReturnAddress());
  oAssert(expr, file, line, msg);
}

HMODULE g_self = nullptr;
std::atomic<int> g_exceptions{0};

bool interesting(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case 0xE06D7363:  // C++ throw (the game's asserts)
      return true;
    default:
      return false;
  }
}

void module_of(void* addr, char* out, size_t n, uintptr_t& offset) {
  HMODULE m = nullptr;
  offset = reinterpret_cast<uintptr_t>(addr);
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(addr), &m) && m) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    strncpy_s(out, n, name ? name + 1 : path, _TRUNCATE);
    offset -= reinterpret_cast<uintptr_t>(m);
  } else {
    strncpy_s(out, n, "?", _TRUNCATE);
  }
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  if (!interesting(er->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
  // Access violations anywhere (the game's catch(...) turns them into "Unkown
  // Error" even in a driver); other codes only from the game's code and ours.
  HMODULE m = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     static_cast<LPCSTR>(er->ExceptionAddress), &m);
  bool game_code = m == GetModuleHandleW(nullptr) || m == g_self;
  if (!game_code && er->ExceptionCode != 0xE06D7363 && er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  int n = ++g_exceptions;
  if (n > 32) return EXCEPTION_CONTINUE_SEARCH;
  char mod[64];
  uintptr_t off = 0;
  module_of(er->ExceptionAddress, mod, sizeof(mod), off);
  const CONTEXT* c = ep->ContextRecord;
  if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
    LOGE("exception C0000005 at %p (%s+0x%X): %s 0x%08X", er->ExceptionAddress, mod, (unsigned)off,
         er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute",
         (unsigned)er->ExceptionInformation[1]);
  } else {
    LOGE("exception %08X at %p (%s+0x%X)", (unsigned)er->ExceptionCode, er->ExceptionAddress, mod, (unsigned)off);
  }
  const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
  DWORD st[16] = {};
  for (int i = 0; i < 16; ++i)
    if (!IsBadReadPtr(sp + i, 4)) st[i] = sp[i];
  LOGE("  eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X", c->Eax, c->Ebx, c->Ecx, c->Edx,
       c->Esi, c->Edi, c->Ebp, c->Esp);
  LOGE("  stack %08X %08X %08X %08X %08X %08X %08X %08X", st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]);
  LOGE("        %08X %08X %08X %08X %08X %08X %08X %08X", st[8], st[9], st[10], st[11], st[12], st[13], st[14], st[15]);
  if (n == 32) LOGW("exception log limit reached, further exceptions are not logged");
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool quitting() { return g_quitting; }

void request_quit() {
  if (g_quitting.exchange(true)) return;
  LOGI("quit requested (window closed)");
  netplay::shutdown();  // the others hear "bye" now, not after a timeout
  PostQuitMessage(0);
  HANDLE t = CreateThread(
      nullptr, 0,
      [](void*) -> DWORD {
        Sleep(5000);
        LOGW("the game did not finish quitting within 5 s; ending the process");
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
      },
      nullptr, 0, nullptr);
  if (t) CloseHandle(t);
}

void install_diagnostics(const Config& cfg) {
  g_cfg = cfg;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&install_diagnostics), &g_self);
  if (cfg.log_exceptions) {
    AddVectoredExceptionHandler(1, on_exception);
    LOGI("exceptions in the game's code are logged");
  }
  patch::import("user32.dll", "MessageBoxA", (void*)&hkMessageBoxA, (void**)&oMessageBoxA);
}

void install(const Config& cfg) {
  g_cfg = cfg;
  if (cfg.fix_gamepads) {
    if (patch::bytes_are(game::kOptionSnapshot, game::kOptionSnapshotHead, sizeof(game::kOptionSnapshotHead)))
      patch::hook("COptionData::Snapshot (Unkown Error fix)", game::kOptionSnapshot, (void*)&hkSnapshot,
                  (void**)&oSnapshot);
    else
      LOGW("COptionData::Snapshot: unexpected code, the Unkown Error fix is not installed");
  } else {
    LOGW("[fixes] gamepad_snapshot=0: the game may stop with \"Unkown Error\" on PCs with few gamepads");
  }
  if (cfg.fix_keyboard_focus)
    patch::replace("keyboard start without focus (Alt+Tab while loading)", game::kKeyboardAcquireCheck,
                   game::kKeyboardAcquireOld, game::kKeyboardAcquireNew, sizeof(game::kKeyboardAcquireNew));
  if (cfg.multi_instance)
    patch::import("user32.dll", "FindWindowA", (void*)&hkFindWindowA, (void**)&oFindWindowA);
  if (cfg.log_exceptions && patch::bytes_are(game::kAssert, game::kAssertHead, sizeof(game::kAssertHead)))
    patch::hook("game ASSERT", game::kAssert, (void*)&hkAssert, (void**)&oAssert);
}

}  // namespace fixes
