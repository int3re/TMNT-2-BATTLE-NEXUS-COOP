#include "mod/input.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/netplay.h"
#include "mod/pads.h"
#include "mod/patch.h"
#include "mod/sim.h"
#include "mod/video.h"
#include "mod/game.h"
#include "mod/gamefont.h"

namespace input {
namespace {

Config g_cfg;

// --- the mouse's buttons as keys, and the keyboard's keys kept by tmnt2net ---------------------------
// The game knows the keyboard only. The mouse's buttons are given to it as four keys no keyboard has
// (game::kKeyMouse...): its own key page (ОПЦИИ → УПРАВЛЕНИЕ) takes a click as the new key and shows
// ЛКМ / ПКМ, and the keyboard controller maps them like any key. Only while the game's window is in
// front and the pointer is over it.
void add_mouse(uint8_t* keys) {
  HWND hwnd = patch::read<HWND>(game::kHwnd);
  if (!hwnd || GetForegroundWindow() != hwnd) return;
  POINT p;
  RECT rc;
  if (!GetCursorPos(&p) || !ScreenToClient(hwnd, &p) || !GetClientRect(hwnd, &rc) || !PtInRect(&rc, p)) return;
  const bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
  auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
  if (down(swap ? VK_RBUTTON : VK_LBUTTON)) keys[game::kKeyMouseLeft] = 0x80;
  if (down(swap ? VK_LBUTTON : VK_RBUTTON)) keys[game::kKeyMouseRight] = 0x80;
  if (down(VK_XBUTTON1)) keys[game::kKeyMouse4] = 0x80;
  if (down(VK_XBUTTON2)) keys[game::kKeyMouse5] = 0x80;
}

// The game keeps its keys in the save: loading any save (ПРОДОЛЖИТЬ, a network session's own save) puts
// that save's keys back. tmnt2net keeps the player's keys in tmnt2net.ini ([input] keys) instead and
// holds the game to them: set at the start, put back after every load, taken over when the player
// changes one on the game's page.
const int32_t kLayoutGame[game::kKeyFuncs] = {DIK_F, DIK_D, DIK_R, DIK_SPACE, DIK_E, DIK_S, DIK_LSHIFT, DIK_F1, DIK_F2,
                                             DIK_NUMPAD8, DIK_NUMPAD2, DIK_NUMPAD4, DIK_NUMPAD6};
// attack A, attack B, shuriken, jump, dash, guard, character change, camera, gauge, up, down, left, right
const int32_t kLayoutMouse[game::kKeyFuncs] = {game::kKeyMouseLeft, game::kKeyMouseRight, DIK_E, DIK_SPACE, DIK_LSHIFT,
                                              DIK_Q, DIK_F, DIK_F1, DIK_F2, DIK_W, DIK_S, DIK_A, DIK_D};
int32_t g_keys[game::kKeyFuncs];
bool g_keys_seen = false;  // g_keys holds what the game has
bool g_keys_ours = false;  // ... and tmnt2net.ini has them: they stand against a save's
bool g_keys_named = false;
std::wstring g_mouse_names[4];

int32_t* game_keys() {
  uint8_t* o = patch::read<uint8_t*>(game::kOptionDataPtr);
  return o ? reinterpret_cast<int32_t*>(o + game::kOptKeys) : nullptr;
}

bool usable(const int32_t* k) {
  for (int i = 0; i < game::kKeyFuncs; ++i)
    if (k[i] <= 0 || k[i] > 255) return false;
  return true;
}

std::wstring ini_path() { return module_dir() + L"tmnt2net.ini"; }

void save_keys() {
  std::wstring v;
  for (int i = 0; i < game::kKeyFuncs; ++i) v += (i ? L"," : L"") + std::to_wstring(g_keys[i]);
  WritePrivateProfileStringW(L"input", L"keys", v.c_str(), ini_path().c_str());
  g_keys_ours = true;
}

bool load_keys(int32_t* out) {
  wchar_t buf[160] = {};
  GetPrivateProfileStringW(L"input", L"keys", L"", buf, 160, ini_path().c_str());
  int n = 0;
  for (wchar_t* p = buf; *p && n < game::kKeyFuncs;) {
    out[n++] = wcstol(p, &p, 10);
    while (*p == L',' || *p == L' ') ++p;
  }
  return n == game::kKeyFuncs && usable(out);
}

void put_keys(const int32_t* k) {  // into the game, and into its keyboard's map
  int32_t* g = game_keys();
  if (!g) return;
  std::memcpy(g, k, sizeof(g_keys));
  if (patch::bytes_are(game::kKeyboardApply, game::kKeyboardApplyHead, sizeof(game::kKeyboardApplyHead)))
    reinterpret_cast<void(__fastcall*)(void*, void*)>(game::kKeyboardApply)(g, nullptr);
}

// The mouse's keys on the game's page: allowed, and named in the game's own lettering.
void name_mouse_keys() {
  if (g_keys_named || !gamefont::ready()) return;
  g_keys_named = true;
  static const struct {
    int key;
    const char* ru;
    const char* en;
  } kNames[4] = {{game::kKeyMouseLeft, u8"ЛКМ", "mouse 1"},
                 {game::kKeyMouseRight, u8"ПКМ", "mouse 2"},
                 {game::kKeyMouse4, u8"МЫШЬ 4", "mouse 4"},
                 {game::kKeyMouse5, u8"МЫШЬ 5", "mouse 5"}};
  for (int i = 0; i < 4; ++i) {
    g_mouse_names[i] = gamefont::say(kNames[i].ru, kNames[i].en);
    const wchar_t* name = g_mouse_names[i].c_str();
    const uint8_t on = 1;
    patch::write(game::kKeyNames + 4 * kNames[i].key, &name, 4);
    patch::write(game::kKeyEnabled + kNames[i].key, &on, 1);
  }
  LOGI("input: the mouse's buttons are keys %02X %02X %02X %02X on the game's key page", game::kKeyMouseLeft,
       game::kKeyMouseRight, game::kKeyMouse4, game::kKeyMouse5);
}

void keys_frame() {
  name_mouse_keys();
  int32_t* g = game_keys();
  if (!g) return;
  if (!g_keys_seen) {
    g_keys_seen = true;
    int32_t mine[game::kKeyFuncs];
    if (load_keys(mine)) {
      std::memcpy(g_keys, mine, sizeof(g_keys));
      g_keys_ours = true;
      put_keys(g_keys);
      LOGI("input: the keyboard's keys from tmnt2net.ini");
    } else if (usable(g)) {
      std::memcpy(g_keys, g, sizeof(g_keys));
    } else {
      g_keys_seen = false;  // (the game has not set its keys up yet)
    }
    return;
  }
  if (std::memcmp(g, g_keys, sizeof(g_keys)) != 0 && usable(g)) {  // changed on the game's page (or by a first save)
    std::memcpy(g_keys, g, sizeof(g_keys));
    save_keys();
  }
}

// --- the sound settings, kept by tmnt2net as the keys are ----------------------------------------------
// The game keeps its sound mode and levels in the save as well: every start is in stereo at full volume
// until a save is loaded, and a game that was never saved forgets them. tmnt2net keeps them in
// tmnt2net.ini ([audio] mode = mono | stereo; music / effects / voices = 0..10) and holds the
// game to them: set at the start, put back after every load, taken over when the player changes one on
// the game's ЗВУК page. The launcher's ЗВУК page writes the same four keys.
constexpr int kSound = 4;  // the head of CSoundOptionData: the mode, then the levels of effects, music, voices
const wchar_t* const kSoundKeys[kSound] = {L"mode", L"effects", L"music", L"voices"};
const wchar_t* const kSoundModes[game::kSoundModes] = {L"mono", L"stereo"};
int32_t g_sound[kSound];
bool g_sound_seen = false;  // g_sound holds what the game has
bool g_sound_ours = false;  // ... and tmnt2net.ini has it: it stands against a save's

int32_t* game_sound() {
  uint8_t* o = patch::read<uint8_t*>(game::kOptionDataPtr);
  return o ? reinterpret_cast<int32_t*>(o + game::kOptSound) : nullptr;
}

bool sound_usable(const int32_t* v) {
  if (v[0] < 0 || v[0] >= game::kSoundModes) return false;
  for (int i = 1; i < kSound; ++i)
    if (v[i] < 0 || v[i] > game::kSoundLevelMax) return false;
  return true;
}

void save_sound() {
  for (int i = 0; i < kSound; ++i)
    WritePrivateProfileStringW(L"audio", kSoundKeys[i], i == 0 ? kSoundModes[g_sound[0]] : std::to_wstring(g_sound[i]).c_str(),
                               ini_path().c_str());
  g_sound_ours = true;
}

// The ini's sound settings over `out` (the game's own where a key is missing). False: the ini has none.
bool load_sound(int32_t* out) {
  bool any = false;
  for (int i = 0; i < kSound; ++i) {
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(L"audio", kSoundKeys[i], L"", buf, 32, ini_path().c_str());
    if (wchar_t* semi = wcschr(buf, L';')) *semi = 0;  // (a comment after the value)
    size_t n = wcslen(buf);
    while (n && iswspace(buf[n - 1])) buf[--n] = 0;
    if (i == 0) {
      for (int m = 0; m < game::kSoundModes; ++m)
        if (_wcsicmp(buf, kSoundModes[m]) == 0) {
          out[0] = m;
          any = true;
        }
      continue;
    }
    wchar_t* end = buf;
    long v = wcstol(buf, &end, 10);
    if (end == buf) continue;  // (no key, or not a number)
    out[i] = (int32_t)std::max(0L, std::min((long)game::kSoundLevelMax, v));
    any = true;
  }
  return any;
}

void put_sound(const int32_t* v) {  // into the game's options, and to its sound
  uint8_t* o = patch::read<uint8_t*>(game::kOptionDataPtr);
  if (!o) return;
  std::memcpy(o + game::kOptSound, v, sizeof(g_sound));
  if (patch::bytes_are(game::kSoundOptionApply, game::kSoundOptionApplyHead, sizeof(game::kSoundOptionApplyHead)))
    reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(game::kSoundOptionApply)(o + game::kOptSound, nullptr);
}

void sound_frame() {
  int32_t* g = game_sound();
  if (!g || !sound_usable(g)) return;
  if (!g_sound_seen) {
    g_sound_seen = true;
    std::memcpy(g_sound, g, sizeof(g_sound));
    if (load_sound(g_sound)) {
      g_sound_ours = true;
      put_sound(g_sound);
      LOGI("input: the sound settings from tmnt2net.ini: %ls, music %d, effects %d, voices %d", kSoundModes[g_sound[0]],
           g_sound[2], g_sound[1], g_sound[3]);
    }
    return;
  }
  if (std::memcmp(g, g_sound, sizeof(g_sound)) != 0) {  // changed on the game's page (or by a first save)
    std::memcpy(g_sound, g, sizeof(g_sound));
    save_sound();
    LOGI("input: the sound settings are kept in tmnt2net.ini: %ls, music %d, effects %d, voices %d", kSoundModes[g_sound[0]],
         g_sound[2], g_sound[1], g_sound[3]);
  }
}

using OptionRestore_t = void(__fastcall*)(uint8_t* self, void* edx, const void* raw);
OptionRestore_t oOptionRestore = nullptr;

// A save's options come back (CGameData::Load): the player's keys and sound settings stay, and in a network
// session the options that are part of the game's logic stay the session's (every PC loads its own save
// there).
void __fastcall hkOptionRestore(uint8_t* self, void* edx, const void* raw) {
  netplay::options_before_restore();
  oOptionRestore(self, edx, raw);
  netplay::options_after_restore();
  if (self != patch::read<uint8_t*>(game::kOptionDataPtr)) return;
  // (the game applies the options next)
  if (g_keys_seen && g_keys_ours) std::memcpy(self + game::kOptKeys, g_keys, sizeof(g_keys));
  if (g_sound_seen && g_sound_ours) std::memcpy(self + game::kOptSound, g_sound, sizeof(g_sound));
}
IDirectInputDevice8A* g_keyboard = nullptr;

using DI8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using CreateDevice_t = HRESULT(WINAPI*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
using GetDeviceStatus_t = HRESULT(WINAPI*)(IDirectInput8A*, REFGUID);
pads::EnumDevices_t oEnumDevices = nullptr;
GetDeviceStatus_t oGetDeviceStatus = nullptr;
using Acquire_t = HRESULT(WINAPI*)(IDirectInputDevice8A*);
using GetDeviceState_t = HRESULT(WINAPI*)(IDirectInputDevice8A*, DWORD, LPVOID);
DI8Create_t oDI8Create = nullptr;
CreateDevice_t oCreateDevice = nullptr;
Acquire_t oAcquire = nullptr;
GetDeviceState_t oGetDeviceState = nullptr;

// --- [debug] script -----------------------------------------------------------------
// "8:RETURN, 12.5:DOWN:0.3, 14:SHOT" — seconds from the first presented frame,
// a DIK key name (or 0x1C) held for the given seconds (default 0.12), or SHOT.
struct Step {
  double at = 0, hold = 0.12;
  int key = -1;  // -1: SHOT or a pad button (text "PAD1.START")
  std::string text;
};
struct PadPress {
  std::string action;
  double until;
};
std::vector<PadPress> g_pad_presses;
std::vector<Step> g_script;
size_t g_next = 0;
double g_t0 = 0;
double g_release[256] = {};

struct KeyName {
  const char* name;
  int dik;
};
const KeyName kKeys[] = {
    {"RETURN", DIK_RETURN}, {"ENTER", DIK_RETURN}, {"ESCAPE", DIK_ESCAPE}, {"ESC", DIK_ESCAPE},
    {"UP", DIK_UP},         {"DOWN", DIK_DOWN},    {"LEFT", DIK_LEFT},     {"RIGHT", DIK_RIGHT},
    {"SPACE", DIK_SPACE},   {"Z", DIK_Z},          {"X", DIK_X},           {"C", DIK_C},
    {"A", DIK_A},           {"S", DIK_S},          {"D", DIK_D},           {"Q", DIK_Q},
    {"W", DIK_W},           {"E", DIK_E},          {"TAB", DIK_TAB},       {"LSHIFT", DIK_LSHIFT},
    {"F", DIK_F},           {"G", DIK_G},          {"R", DIK_R},           {"T", DIK_T},
    {"F1", DIK_F1},         {"F2", DIK_F2},        {"GRAVE", DIK_GRAVE},   {"NUMPADENTER", DIK_NUMPADENTER},
    {"NUMPAD0", DIK_NUMPAD0}, {"NUMPAD1", DIK_NUMPAD1}, {"NUMPAD2", DIK_NUMPAD2}, {"NUMPAD3", DIK_NUMPAD3},
    {"NUMPAD4", DIK_NUMPAD4}, {"NUMPAD5", DIK_NUMPAD5}, {"NUMPAD6", DIK_NUMPAD6}, {"NUMPAD7", DIK_NUMPAD7},
    {"NUMPAD8", DIK_NUMPAD8}, {"NUMPAD9", DIK_NUMPAD9},
};

double now_s() {
  static LARGE_INTEGER f{};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return double(c.QuadPart) / double(f.QuadPart);
}

void parse_script(const std::string& s) {
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? s.size() : end + 1;
    std::vector<std::string> f;
    size_t p = 0;
    while (true) {
      size_t c = item.find(':', p);
      std::string part = item.substr(p, c == std::string::npos ? std::string::npos : c - p);
      while (!part.empty() && part.front() == ' ') part.erase(part.begin());
      while (!part.empty() && part.back() == ' ') part.pop_back();
      f.push_back(part);
      if (c == std::string::npos) break;
      p = c + 1;
    }
    if (f.size() < 2 || f[0].empty()) continue;
    Step st;
    st.at = atof(f[0].c_str());
    st.text = f[1];
    for (auto& ch : st.text) ch = (char)toupper((unsigned char)ch);
    if (st.text != "SHOT" && st.text.rfind("PAD", 0) != 0) {
      if (st.text.rfind("0X", 0) == 0) {
        st.key = (int)strtol(st.text.c_str(), nullptr, 16);
      } else {
        for (const KeyName& k : kKeys)
          if (st.text == k.name) st.key = k.dik;
      }
      if (st.key < 0 || st.key > 255) {
        LOGW("debug script: unknown key \"%s\"", f[1].c_str());
        continue;
      }
    }
    if (f.size() >= 3) st.hold = atof(f[2].c_str());
    g_script.push_back(st);
  }
  std::stable_sort(g_script.begin(), g_script.end(), [](const Step& a, const Step& b) { return a.at < b.at; });
  LOGI("debug script: %d steps", (int)g_script.size());
}

bool keep_connected() { return g_cfg.background_run || !g_script.empty() || sim::active() || netplay::active(); }

void apply_script_keys(uint8_t* keys) {
  sim::apply_frame_keys(keys);
  if (g_script.empty()) return;
  double t = now_s();
  for (int k = 0; k < 256; ++k)
    if (g_release[k] > t) keys[k] = 0x80;
}

// --- hooks --------------------------------------------------------------------------
HRESULT WINAPI hkAcquire(IDirectInputDevice8A* dev) {
  HRESULT hr = oAcquire(dev);
  if (dev == g_keyboard && FAILED(hr) && hr != DIERR_INPUTLOST && keep_connected()) return DI_OK;
  return hr;
}

HRESULT WINAPI hkGetDeviceState(IDirectInputDevice8A* dev, DWORD cb, LPVOID data) {
  if (dev == g_keyboard && cb == 256 && data && sim::active()) {
    sim::keyboard(static_cast<uint8_t*>(data));  // the frame's input, the same on every copy
    return DI_OK;
  }
  HRESULT hr = oGetDeviceState(dev, cb, data);
  if (dev != g_keyboard || cb != 256 || !data) return hr;
  if (FAILED(hr)) {
    if (!keep_connected()) return hr;
    std::memset(data, 0, 256);  // not in front: connected, nothing pressed
    hr = DI_OK;
  }
  apply_script_keys(static_cast<uint8_t*>(data));
  add_mouse(static_cast<uint8_t*>(data));
  pads::on_keyboard(static_cast<uint8_t*>(data));  // keys of the keyboard pads are theirs
  return hr;
}

HRESULT WINAPI hkEnumDevices(IDirectInput8A* di, DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags) {
  return pads::enum_devices(oEnumDevices, di, type, cb, ref, flags);
}

HRESULT WINAPI hkGetDeviceStatus(IDirectInput8A* di, REFGUID guid) {
  HRESULT hr = DI_OK;
  if (pads::device_status(guid, &hr)) return hr;
  return oGetDeviceStatus(di, guid);
}

HRESULT WINAPI hkCreateDevice(IDirectInput8A* di, REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) {
  if (out && pads::create_device(guid, out)) return DI_OK;
  HRESULT hr = oCreateDevice(di, guid, out, outer);
  if (SUCCEEDED(hr) && out && *out && IsEqualGUID(guid, GUID_SysKeyboard)) {
    g_keyboard = *out;
    LOGI("input: keyboard device %p", (void*)g_keyboard);
    if (!oGetDeviceState) {
      void** vt = *reinterpret_cast<void***>(g_keyboard);
      patch::hook("IDirectInputDevice8::Acquire", reinterpret_cast<uintptr_t>(vt[7]), (void*)&hkAcquire,
                  (void**)&oAcquire);
      patch::hook("IDirectInputDevice8::GetDeviceState", reinterpret_cast<uintptr_t>(vt[9]),
                  (void*)&hkGetDeviceState, (void**)&oGetDeviceState);
    }
  }
  return hr;
}

HRESULT WINAPI hkDI8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID* out, LPUNKNOWN outer) {
  HRESULT hr = oDI8Create(inst, ver, iid, out, outer);
  if (SUCCEEDED(hr) && out && *out && !oCreateDevice) {
    void** vt = *reinterpret_cast<void***>(*out);
    patch::hook("IDirectInput8::CreateDevice", reinterpret_cast<uintptr_t>(vt[3]), (void*)&hkCreateDevice,
                (void**)&oCreateDevice);
    if (pads::active()) {
      patch::hook("IDirectInput8::EnumDevices", reinterpret_cast<uintptr_t>(vt[4]), (void*)&hkEnumDevices,
                  (void**)&oEnumDevices);
      patch::hook("IDirectInput8::GetDeviceStatus", reinterpret_cast<uintptr_t>(vt[5]), (void*)&hkGetDeviceStatus,
                  (void**)&oGetDeviceStatus);
    }
  }
  return hr;
}

}  // namespace

// Esc goes back in the game's menus too (the game's own key for that is Space: the pages say
// "ПРОБЕЛ→НАЗАД"). A menu asks GetDigitalTrigger(port, cancel mask): with Esc newly down this
// frame the answer is yes. Off line only: in a network game the keyboard is a pad of the
// session, its Esc that pad's Select, which the menus take as cancel already; a key of one PC
// alone would part the games.
using DigitalTrigger_t = int(__cdecl*)(int port, uint32_t mask);
DigitalTrigger_t oDigitalTrigger = nullptr;
bool g_esc_down = false, g_esc_new = false;

int __cdecl hkDigitalTrigger(int port, uint32_t mask) {
  int r = oDigitalTrigger(port, mask);
  // (With the frame's input fixed by sim - always, since 0.14 - the keyboard is the player's pad and
  // its Esc that pad's Select, which the menus take as cancel already: one press must not count twice.)
  if (r || !g_esc_new || sim::active() || netplay::active()) return r;
  uint32_t cancel = patch::read<uint32_t>(game::kDigitalCancel);
  return cancel && (mask & cancel) == cancel ? 1 : 0;
}

void esc_frame() {
  uint8_t keys[256];
  bool down = oDigitalTrigger && read_keyboard(keys) && (keys[DIK_ESCAPE] & 0x80);
  g_esc_new = down && !g_esc_down;
  g_esc_down = down;
}

void install(const Config& cfg) {
  g_cfg = cfg;
  if (!cfg.script.empty()) parse_script(cfg.script);
  pads::configure(cfg);
  if (cfg.esc_back && patch::bytes_are(game::kGetDigitalTrigger, game::kGetDigitalTriggerHead,
                                       sizeof(game::kGetDigitalTriggerHead)))
    patch::hook("GetDigitalTrigger (Esc goes back in menus)", game::kGetDigitalTrigger, (void*)&hkDigitalTrigger,
                (void**)&oDigitalTrigger);
  if (patch::bytes_are(game::kOptionRestore, game::kOptionRestoreHead, sizeof(game::kOptionRestoreHead)))
    patch::hook("COptionData::Restore (the player's keys, sound settings and a session's options stay)", game::kOptionRestore,
                (void*)&hkOptionRestore, (void**)&oOptionRestore);
  // (always: the mouse's buttons reach the game through the keyboard's state)
  patch::import("DINPUT8.dll", "DirectInput8Create", (void*)&hkDI8Create, (void**)&oDI8Create);
}

void set_background_run(bool run) { g_cfg.background_run = run; }

int layout() {
  const int32_t* g = game_keys();
  if (!g) return 0;
  if (std::memcmp(g, kLayoutGame, sizeof(kLayoutGame)) == 0) return 0;
  if (std::memcmp(g, kLayoutMouse, sizeof(kLayoutMouse)) == 0) return 1;
  return 2;
}

void set_layout(int which) {
  if (which != 0 && which != 1) return;
  std::memcpy(g_keys, which ? kLayoutMouse : kLayoutGame, sizeof(g_keys));
  g_keys_seen = true;
  put_keys(g_keys);
  save_keys();
  LOGI("input: keyboard layout: %s", which ? "WASD + mouse" : "the game's own");
}

void on_frame() {
  keys_frame();
  sound_frame();
  esc_frame();
  pads::on_frame();
  double t = now_s();
  if (!g_t0) g_t0 = t;
  if (g_script.empty()) return;
  for (size_t i = 0; i < g_pad_presses.size();) {
    if (g_pad_presses[i].until <= t) {
      pads::script_button(g_pad_presses[i].action.c_str(), false);
      g_pad_presses.erase(g_pad_presses.begin() + i);
    } else {
      ++i;
    }
  }
  while (g_next < g_script.size() && t - g_t0 >= g_script[g_next].at) {
    const Step& st = g_script[g_next++];
    if (st.key < 0 && st.text.rfind("PAD", 0) == 0) {
      pads::script_button(st.text.c_str(), true);
      g_pad_presses.push_back({st.text, t + st.hold});
      LOGD("debug script: %.1f s %s", st.at, st.text.c_str());
    } else if (st.key < 0) {
      video::request_shot(st.at);
      if (g_cfg.log_cameras) sim::dump_data((int)(st.at * 10 + 0.5));  // [debug] with the shot
    } else {
      g_release[st.key] = t + st.hold;
      LOGD("debug script: %.1f s %s", st.at, st.text.c_str());
    }
  }
}

double script_time() {
  if (sim::active()) return sim::frame() / 60.0;  // the same frame on every PC of a session
  return g_t0 ? now_s() - g_t0 : 0;
}

// The game takes its keyboard again (Acquire) only when its own read of it fails (0x537D76). With the
// frame's input fixed by sim - always, since 0.14 - the game's read is answered by sim and never fails,
// so nobody took the keyboard again: once the window had not been in front (at the start, or after
// Alt+Tab) no key reached the game any more. tmnt2net's own read takes it again.
bool read_keyboard(uint8_t keys[256]) {
  if (!g_keyboard || !oGetDeviceState) return false;
  HRESULT hr = oGetDeviceState(g_keyboard, 256, keys);
  if (FAILED(hr)) {
    if (g_cfg.keyboard_lab) {  // [debug] a test copy is never in front: its keyboard without that condition
      static bool once = false;
      if (!once) {
        once = true;
        g_keyboard->Unacquire();
        HRESULT lr = g_keyboard->SetCooperativeLevel(patch::read<HWND>(game::kHwnd), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
        LOGI("[debug] keyboard_lab: background level (0x%08lX)", (unsigned long)lr);
      }
    }
    HRESULT ar = oAcquire ? oAcquire(g_keyboard) : g_keyboard->Acquire();
    if (SUCCEEDED(ar)) hr = oGetDeviceState(g_keyboard, 256, keys);
  }
  static int state = -1;  // 1 read, 0 not: said when it changes
  int now = SUCCEEDED(hr) ? 1 : 0;
  if (now != state) {
    static int said = 0;
    if (said++ < 40) {
      if (now)
        LOGI("input: the keyboard is read%s", state == 0 ? " again (taken back after the window was not in front)" : "");
      else
        LOGI("input: the keyboard cannot be read now (0x%08lX: the window is not in front)", (unsigned long)hr);
    }
    state = now;
  }
  if (FAILED(hr)) std::memset(keys, 0, 256);
  apply_script_keys(keys);
  add_mouse(keys);
  return true;
}

}  // namespace input
