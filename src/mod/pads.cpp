#include "mod/pads.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/game.h"
#include "mod/input.h"
#include "mod/patch.h"
#include "mod/sim.h"

namespace pads {
namespace {

constexpr int kMaxJoysticks = 5;  // COptionData keeps 6 pad records: keyboard + 5 joysticks
constexpr int kMaxSdlSlots = 4;

Config g_cfg;
bool g_initialized = false;
bool g_sdl = false;

// --- the game's layout -----------------------------------------------------------
enum Btn { kCross, kSquare, kCircle, kTriangle, kL1, kL2, kR1, kR2, kSelect, kStart, kL3, kR3, kButtons };

struct PadState {
  LONG lx = 0, ly = 0, rx = 0, ry = 0;  // -32767..32767, y down positive (DirectInput)
  bool up = false, down = false, left = false, right = false;
  bool button[kButtons] = {};
};

DWORD pov_of(const PadState& s) {
  int x = (s.right ? 1 : 0) - (s.left ? 1 : 0), y = (s.down ? 1 : 0) - (s.up ? 1 : 0);
  if (!x && !y) return 0xFFFFFFFF;
  static const DWORD kPov[3][3] = {{31500, 0, 4500}, {27000, 0xFFFFFFFF, 9000}, {22500, 18000, 13500}};
  return kPov[y + 1][x + 1];
}

PadState from_input(const sim::PadInput& in) {
  PadState s;
  s.lx = in.lx;
  s.ly = in.ly;
  s.rx = in.rx;
  s.ry = in.ry;
  s.up = (in.dpad & 1) != 0;
  s.down = (in.dpad & 2) != 0;
  s.left = (in.dpad & 4) != 0;
  s.right = (in.dpad & 8) != 0;
  for (int b = 0; b < kButtons; ++b) s.button[b] = (in.buttons >> b) & 1;
  return s;
}

sim::PadInput to_input(const PadState& s) {
  sim::PadInput in;
  in.lx = (int16_t)s.lx;
  in.ly = (int16_t)s.ly;
  in.rx = (int16_t)s.rx;
  in.ry = (int16_t)s.ry;
  in.dpad = (uint8_t)((s.up ? 1 : 0) | (s.down ? 2 : 0) | (s.left ? 4 : 0) | (s.right ? 8 : 0));
  for (int b = 0; b < kButtons; ++b)
    if (s.button[b]) in.buttons |= (uint16_t)(1u << b);
  return in;
}

// --- slots -----------------------------------------------------------------------
enum class Source { Sdl, Keys, None };  // None: a lockstep pad with nothing local behind it
struct Slot {
  Source src;
  int index;  // SDL slot / keyboard layout
};
std::vector<Slot> g_slots;  // in the order the game sees them (its joystick ports)

SDL_GameController* g_ctrl[kMaxSdlSlots] = {};
SDL_JoystickID g_ctrl_id[kMaxSdlSlots] = {-1, -1, -1, -1};
int g_sdl_slots = 0;
std::vector<SDL_Joystick*> g_virtual;  // [debug] virtual_pads (SDL virtual controllers)
std::set<uint32_t> g_hidden;          // MAKELONG(VID, PID) of pads SDL serves

GUID slot_guid(int i) {
  GUID g = {0x746D6E74, 0x3232, 0x656E, {0x74, 0x70, 0x61, 0x64, 0x00, 0x00, 0x00, (BYTE)(0x10 + i)}};
  return g;
}

int slot_of(REFGUID guid) {
  for (int i = 0; i < (int)g_slots.size(); ++i)
    if (IsEqualGUID(guid, slot_guid(i))) return i;
  return -1;
}

// --- keyboard pads -----------------------------------------------------------------
// [input] keyboard_players = 2: two pads on one keyboard. Keys per game button.
struct KeyLayout {
  int up, down, left, right;
  int button[kButtons];
};
const KeyLayout kKeyLayouts[2] = {
    // left half: WASD, F/G/R/Space like the game's defaults, Tab = start
    {DIK_W, DIK_S, DIK_A, DIK_D,
     {DIK_F, DIK_G, DIK_SPACE, DIK_R, DIK_Q, DIK_LSHIFT, DIK_E, DIK_C, DIK_GRAVE, DIK_TAB, DIK_Z, DIK_X}},
    // right half: arrows, numpad, numpad Enter = start
    {DIK_UP, DIK_DOWN, DIK_LEFT, DIK_RIGHT,
     {DIK_NUMPAD1, DIK_NUMPAD2, DIK_NUMPAD0, DIK_NUMPAD5, DIK_NUMPAD4, DIK_NUMPAD3, DIK_NUMPAD6, DIK_DECIMAL,
      DIK_SUBTRACT, DIK_NUMPADENTER, DIK_NUMPAD7, DIK_NUMPAD9}},
};
int g_keyboard_pads = 0;

void read_keys(int layout, PadState& s) {
  uint8_t keys[256];
  if (!input::read_keyboard(keys)) return;
  const KeyLayout& k = kKeyLayouts[layout];
  auto down = [&](int dik) { return (keys[dik] & 0x80) != 0; };
  s.up = down(k.up);
  s.down = down(k.down);
  s.left = down(k.left);
  s.right = down(k.right);
  for (int b = 0; b < kButtons; ++b) s.button[b] = down(k.button[b]);
  // The stick follows the direction keys too (the game moves with the stick).
  s.lx = s.left ? -32767 : s.right ? 32767 : 0;
  s.ly = s.up ? -32767 : s.down ? 32767 : 0;
}

// --- SDL ---------------------------------------------------------------------------
void scan_xinput_devices() {
  // Pads served through XInput: their HID path contains "IG_" (Microsoft's rule).
  UINT n = 0;
  if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || !n) return;
  std::vector<RAWINPUTDEVICELIST> list(n);
  n = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
  if (n == (UINT)-1) return;
  for (UINT i = 0; i < n; ++i) {
    if (list[i].dwType != RIM_TYPEHID) continue;
    char name[512] = {};
    UINT size = sizeof(name);
    if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICENAME, name, &size) == (UINT)-1) continue;
    for (char* c = name; *c; ++c) *c = (char)toupper((unsigned char)*c);
    if (!strstr(name, "IG_")) continue;
    const char* v = strstr(name, "VID_");
    const char* p = strstr(name, "PID_");
    if (!v || !p) continue;
    unsigned vid = strtoul(v + 4, nullptr, 16), pid = strtoul(p + 4, nullptr, 16);
    g_hidden.insert(MAKELONG(vid, pid));
    LOGI("pads: XInput device %04X:%04X (read through SDL, hidden from the game's DirectInput)", vid, pid);
  }
}

void open_new_controllers() {
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (!SDL_IsGameController(i)) continue;
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
    bool open = false;
    for (int s = 0; s < g_sdl_slots; ++s) open = open || g_ctrl_id[s] == id;
    if (open) continue;
    int free = -1;
    for (int s = 0; s < g_sdl_slots && free < 0; ++s)
      if (!g_ctrl[s]) free = s;
    if (free < 0) return;
    SDL_GameController* c = SDL_GameControllerOpen(i);
    if (!c) continue;
    g_ctrl[free] = c;
    g_ctrl_id[free] = id;
    unsigned vid = SDL_GameControllerGetVendor(c), pid = SDL_GameControllerGetProduct(c);
    if (vid || pid) g_hidden.insert(MAKELONG(vid, pid));
    const char* name = SDL_GameControllerName(c);
    LOGI("pads: \"%s\" (%04X:%04X) -> pad %d", name ? name : "?", vid, pid, free + 1);
  }
}

void close_detached() {
  for (int s = 0; s < g_sdl_slots; ++s) {
    if (g_ctrl[s] && !SDL_GameControllerGetAttached(g_ctrl[s])) {
      LOGI("pads: pad %d disconnected", s + 1);
      SDL_GameControllerClose(g_ctrl[s]);
      g_ctrl[s] = nullptr;
      g_ctrl_id[s] = -1;
    }
  }
}

void sdl_update() {
  if (!g_sdl) return;
  static DWORD last = 0;
  DWORD now = GetTickCount();
  if (now - last < 4) return;
  last = now;
  SDL_PumpEvents();  // joystick update + device detection (video is not initialised)
  SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
  close_detached();
  open_new_controllers();
}

void read_sdl(int slot, PadState& s) {
  SDL_GameController* c = g_ctrl[slot];
  if (!c) return;
  auto btn = [c](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(c, b) != 0; };
  auto axis = [c](SDL_GameControllerAxis a) {
    int v = SDL_GameControllerGetAxis(c, a);
    return (LONG)std::max(-32767, std::min(32767, v));
  };
  // Positions, not labels: south = cross, east = circle, west = square, north = triangle.
  s.button[kCross] = btn(SDL_CONTROLLER_BUTTON_A);
  s.button[kCircle] = btn(SDL_CONTROLLER_BUTTON_B);
  s.button[kSquare] = btn(SDL_CONTROLLER_BUTTON_X);
  s.button[kTriangle] = btn(SDL_CONTROLLER_BUTTON_Y);
  s.button[kL1] = btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
  s.button[kR1] = btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
  s.button[kL2] = axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
  s.button[kR2] = axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
  s.button[kSelect] = btn(SDL_CONTROLLER_BUTTON_BACK);
  s.button[kStart] = btn(SDL_CONTROLLER_BUTTON_START);
  s.button[kL3] = btn(SDL_CONTROLLER_BUTTON_LEFTSTICK);
  s.button[kR3] = btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
  s.up = btn(SDL_CONTROLLER_BUTTON_DPAD_UP);
  s.down = btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
  s.left = btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
  s.right = btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
  s.lx = axis(SDL_CONTROLLER_AXIS_LEFTX);
  s.ly = axis(SDL_CONTROLLER_AXIS_LEFTY);
  s.rx = axis(SDL_CONTROLLER_AXIS_RIGHTX);
  s.ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
}

void sdl_init() {
  SDL_SetMainReady();
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");  // other DirectInput pads stay with the game
  SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
  SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
  if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
    LOGE("pads: SDL_Init failed: %s", SDL_GetError());
    return;
  }
  g_sdl = true;
  SDL_version v;
  SDL_GetVersion(&v);
  LOGI("pads: SDL %d.%d.%d, %d joystick(s) found", v.major, v.minor, v.patch, SDL_NumJoysticks());
  for (int i = 0; i < g_cfg.virtual_pads && i < kMaxSdlSlots; ++i) {
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.name = "tmnt2net virtual pad";
    int index = SDL_JoystickAttachVirtualEx(&desc);
    SDL_Joystick* j = index >= 0 ? SDL_JoystickOpen(index) : nullptr;
    if (j) g_virtual.push_back(j);
    LOGI("pads: [debug] virtual pad %d %s", i + 1, j ? "attached" : SDL_GetError());
  }
}

// --- the virtual DirectInput joystick -----------------------------------------------
class VirtualPad final : public IDirectInputDevice8A {
 public:
  explicit VirtualPad(int slot) : slot_(slot) {}

  // IUnknown
  STDMETHOD(QueryInterface)(REFIID iid, LPVOID* out) override {
    if (!out) return E_POINTER;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDirectInputDevice8A)) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHOD_(ULONG, AddRef)() override { return (ULONG)InterlockedIncrement(&refs_); }
  STDMETHOD_(ULONG, Release)() override {
    LONG r = InterlockedDecrement(&refs_);
    if (!r) delete this;
    return (ULONG)r;
  }

  // IDirectInputDevice8A
  STDMETHOD(GetCapabilities)(LPDIDEVCAPS caps) override {
    if (!caps || caps->dwSize < sizeof(DIDEVCAPS_DX3)) return DIERR_INVALIDPARAM;
    DWORD size = caps->dwSize;
    DIDEVCAPS c{};
    c.dwSize = size;
    c.dwFlags = DIDC_ATTACHED;
    c.dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
    c.dwAxes = 4;
    c.dwButtons = kButtons;
    c.dwPOVs = 1;
    std::memcpy(caps, &c, std::min<DWORD>(size, sizeof(c)));
    return DI_OK;
  }
  STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) override {
    if (!cb) return DIERR_INVALIDPARAM;
    if (flags & (DIDFT_FFACTUATOR | DIDFT_FFEFFECTTRIGGER)) return DI_OK;  // no force feedback
    bool all = DIDFT_GETTYPE(flags) == 0;
    if (all || (flags & DIDFT_AXIS)) {
      const GUID* guids[4] = {&GUID_XAxis, &GUID_YAxis, &GUID_RzAxis, &GUID_Slider};
      const DWORD ofs[4] = {DIJOFS_X, DIJOFS_Y, DIJOFS_RZ, DIJOFS_SLIDER(0)};
      const char* names[4] = {"X", "Y", "Rz", "Slider"};
      for (int i = 0; i < 4; ++i) {
        DIDEVICEOBJECTINSTANCEA o{};
        o.dwSize = sizeof(o);
        o.guidType = *guids[i];
        o.dwOfs = ofs[i];
        o.dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i);
        o.dwFlags = DIDOI_ASPECTPOSITION;
        strncpy_s(o.tszName, names[i], _TRUNCATE);
        if (cb(&o, ref) == DIENUM_STOP) return DI_OK;
      }
    }
    if (all || (flags & DIDFT_BUTTON)) {
      for (int i = 0; i < kButtons; ++i) {
        DIDEVICEOBJECTINSTANCEA o{};
        o.dwSize = sizeof(o);
        o.guidType = GUID_Button;
        o.dwOfs = DIJOFS_BUTTON(i);
        o.dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i);
        sprintf_s(o.tszName, "Button %d", i);
        if (cb(&o, ref) == DIENUM_STOP) return DI_OK;
      }
    }
    if (all || (flags & DIDFT_POV)) {
      DIDEVICEOBJECTINSTANCEA o{};
      o.dwSize = sizeof(o);
      o.guidType = GUID_POV;
      o.dwOfs = DIJOFS_POV(0);
      o.dwType = DIDFT_POV | DIDFT_MAKEINSTANCE(0);
      strncpy_s(o.tszName, "Hat", _TRUNCATE);
      cb(&o, ref);
    }
    return DI_OK;
  }
  STDMETHOD(GetProperty)(REFGUID prop, LPDIPROPHEADER h) override {
    if (!h) return DIERR_INVALIDPARAM;
    if (&prop == &DIPROP_RANGE) {
      auto* r = reinterpret_cast<LPDIPROPRANGE>(h);
      r->lMin = -32767;
      r->lMax = 32767;
      return DI_OK;
    }
    return DIERR_UNSUPPORTED;
  }
  STDMETHOD(SetProperty)(REFGUID, LPCDIPROPHEADER) override { return DI_OK; }
  STDMETHOD(Acquire)() override { return connected() ? DI_OK : DIERR_UNPLUGGED; }
  STDMETHOD(Unacquire)() override { return DI_OK; }
  STDMETHOD(GetDeviceState)(DWORD cb, LPVOID data) override {
    if (!data || (cb != sizeof(DIJOYSTATE) && cb != sizeof(DIJOYSTATE2))) return DIERR_INVALIDPARAM;
    if (!connected()) return DIERR_UNPLUGGED;
    PadState s;
    if (sim::active())
      s = from_input(sim::pad(slot_));  // the frame's input, the same on every copy
    else
      read(s);
    DIJOYSTATE2 js{};
    js.lX = s.lx;
    js.lY = s.ly;
    js.lRz = s.rx;
    js.rglSlider[0] = s.ry;
    js.rgdwPOV[0] = pov_of(s);
    js.rgdwPOV[1] = js.rgdwPOV[2] = js.rgdwPOV[3] = 0xFFFFFFFF;
    for (int b = 0; b < kButtons; ++b) js.rgbButtons[b] = s.button[b] ? 0x80 : 0;
    std::memcpy(data, &js, cb);
    return DI_OK;
  }
  STDMETHOD(GetDeviceData)(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD count, DWORD) override {
    if (count) *count = 0;
    return DI_OK;
  }
  STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT) override { return DI_OK; }
  STDMETHOD(SetEventNotification)(HANDLE) override { return DI_OK; }
  STDMETHOD(SetCooperativeLevel)(HWND, DWORD) override { return DI_OK; }
  STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) override { return DIERR_UNSUPPORTED; }
  STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEA info) override {
    if (!info) return DIERR_INVALIDPARAM;
    fill_instance(slot_, info);
    return DI_OK;
  }
  STDMETHOD(RunControlPanel)(HWND, DWORD) override { return DI_OK; }
  STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
  STDMETHOD(CreateEffect)(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN) override {
    return DIERR_UNSUPPORTED;
  }
  STDMETHOD(EnumEffects)(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override { return DI_OK; }
  STDMETHOD(GetEffectInfo)(LPDIEFFECTINFOA, REFGUID) override { return DIERR_UNSUPPORTED; }
  STDMETHOD(GetForceFeedbackState)(LPDWORD) override { return DIERR_UNSUPPORTED; }
  STDMETHOD(SendForceFeedbackCommand)(DWORD) override { return DIERR_UNSUPPORTED; }
  STDMETHOD(EnumCreatedEffectObjects)(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; }
  STDMETHOD(Escape)(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
  STDMETHOD(Poll)() override { return connected() ? DI_OK : DIERR_NOTACQUIRED; }
  STDMETHOD(SendDeviceData)(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_UNSUPPORTED; }
  STDMETHOD(EnumEffectsInFile)(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override {
    return DIERR_UNSUPPORTED;
  }
  STDMETHOD(WriteEffectToFile)(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
  STDMETHOD(BuildActionMap)(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
  STDMETHOD(SetActionMap)(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
  STDMETHOD(GetImageInfo)(LPDIDEVICEIMAGEINFOHEADERA) override { return DIERR_UNSUPPORTED; }

  static void fill_instance(int slot, LPDIDEVICEINSTANCEA out) {
    DWORD size = out->dwSize ? out->dwSize : sizeof(DIDEVICEINSTANCEA);
    DIDEVICEINSTANCEA d{};
    d.dwSize = size;
    d.guidInstance = slot_guid(slot);
    d.guidProduct = slot_guid(0x40 + slot);
    d.dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
    const Slot& s = g_slots[slot];
    if (s.src == Source::Keys)
      sprintf_s(d.tszInstanceName, "tmnt2net keyboard pad %d", s.index + 1);
    else
      sprintf_s(d.tszInstanceName, "tmnt2net pad %d", s.src == Source::None ? slot + 1 : s.index + 1);
    strncpy_s(d.tszProductName, d.tszInstanceName, _TRUNCATE);
    std::memcpy(out, &d, std::min<DWORD>(size, sizeof(d)));
  }

  bool connected() const { return slot_connected(slot_); }

  static bool slot_connected(int slot) {
    if (slot < 0 || slot >= (int)g_slots.size()) return false;
    if (sim::active()) return sim::pad(slot).connected != 0;
    return local_connected(slot);
  }

  static bool local_connected(int slot) {
    if (slot < 0 || slot >= (int)g_slots.size()) return false;
    const Slot& s = g_slots[slot];
    if (s.src == Source::Keys) return true;
    if (s.src == Source::None) return false;
    sdl_update();
    return g_ctrl[s.index] != nullptr;
  }

  static void read_slot(int slot, PadState& s) {
    const Slot& sl = g_slots[slot];
    if (sl.src == Source::Keys)
      read_keys(sl.index, s);
    else if (sl.src == Source::Sdl)
      read_sdl(sl.index, s);
  }

 private:
  void read(PadState& s) const { read_slot(slot_, s); }

  int slot_;
  LONG refs_ = 1;
};

// Real devices reported by DirectInput during our pre-scan.
BOOL CALLBACK collect(LPCDIDEVICEINSTANCEA d, LPVOID ref) {
  static_cast<std::vector<DIDEVICEINSTANCEA>*>(ref)->push_back(*d);
  return DIENUM_CONTINUE;
}

bool hidden(const DIDEVICEINSTANCEA& d) { return g_hidden.count(d.guidProduct.Data1) != 0; }

bool is_joystick(const DIDEVICEINSTANCEA& d) {
  BYTE t = GET_DIDEVICE_TYPE(d.dwDevType);
  return t == DI8DEVTYPE_GAMEPAD || t == DI8DEVTYPE_JOYSTICK;
}

}  // namespace

void configure(const Config& cfg) {
  g_cfg = cfg;
  g_keyboard_pads = cfg.keyboard_players == 2 ? 2 : 0;
  if (active())
    LOGI("pads: up to %d gamepad(s) through SDL%s%s", cfg.pads,
         g_keyboard_pads ? ", two players on the keyboard" : "",
         cfg.virtual_pads ? ", [debug] virtual pads" : "");
}

bool active() { return sim::active() || g_cfg.pads > 0 || g_cfg.keyboard_players == 2 || g_cfg.virtual_pads > 0; }

HRESULT enum_devices(EnumDevices_t original, IDirectInput8A* di, DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref,
                     DWORD flags) {
  bool controllers = type == DI8DEVCLASS_GAMECTRL || type == DI8DEVCLASS_ALL || type == DI8DEVTYPE_GAMEPAD ||
                     type == DI8DEVTYPE_JOYSTICK;
  if (!active() || !controllers || !cb) return original(di, type, cb, ref, flags);

  if (!g_initialized) {
    g_initialized = true;
    if (g_cfg.pads > 0 || g_cfg.virtual_pads > 0) {
      scan_xinput_devices();
      sdl_init();
    }
  }
  std::vector<DIDEVICEINSTANCEA> real;
  HRESULT hr = original(di, type, collect, &real, flags);
  if (FAILED(hr)) return hr;
  std::vector<DIDEVICEINSTANCEA> kept;
  for (const auto& d : real) {
    if (is_joystick(d) && hidden(d)) {
      LOGI("pads: \"%s\" hidden from the game (read through SDL)", d.tszProductName);
      continue;
    }
    kept.push_back(d);
  }
  int real_joysticks = (int)std::count_if(kept.begin(), kept.end(), is_joystick);

  if (g_slots.empty()) {
    int room = kMaxJoysticks - g_keyboard_pads - (sim::active() ? 0 : real_joysticks);
    g_sdl_slots = g_sdl ? std::max(0, std::min({g_cfg.pads > 0 ? g_cfg.pads : 0, kMaxSdlSlots, room})) : 0;
    if (g_cfg.virtual_pads > 0 && g_sdl) g_sdl_slots = std::max(g_sdl_slots, std::min((int)g_virtual.size(), room));
    for (int i = 0; i < g_sdl_slots; ++i) g_slots.push_back({Source::Sdl, i});
    for (int i = 0; i < g_keyboard_pads; ++i) g_slots.push_back({Source::Keys, i});
    if (sim::active()) {
      // Lockstep: every copy shows the game the same four pads and none of its own joysticks.
      g_slots.resize(sim::kPads, Slot{Source::None, 0});
      real_joysticks = 0;
    }
    open_new_controllers();
    LOGI("pads: the game gets %d gamepad slot(s)%s and %d DirectInput joystick(s) of its own",
         g_sdl_slots, g_keyboard_pads ? " + 2 keyboard pads" : "", std::min(real_joysticks, kMaxJoysticks - (int)g_slots.size()));
  }

  int joysticks = 0;
  for (int i = 0; i < (int)g_slots.size(); ++i) {
    DIDEVICEINSTANCEA d{};
    d.dwSize = sizeof(d);
    VirtualPad::fill_instance(i, &d);
    ++joysticks;
    if (cb(&d, ref) == DIENUM_STOP) return DI_OK;
  }
  for (const auto& d : kept) {
    if (is_joystick(d)) {
      if (joysticks >= kMaxJoysticks || sim::active()) continue;
      ++joysticks;
    }
    if (cb(&d, ref) == DIENUM_STOP) return DI_OK;
  }
  return DI_OK;
}

bool create_device(REFGUID guid, LPDIRECTINPUTDEVICE8A* out) {
  int slot = slot_of(guid);
  if (slot < 0) return false;
  *out = new VirtualPad(slot);
  return true;
}

bool device_status(REFGUID guid, HRESULT* hr) {
  int slot = slot_of(guid);
  if (slot < 0) return false;
  *hr = VirtualPad::slot_connected(slot) ? DI_OK : DI_NOTATTACHED;
  return true;
}

void on_keyboard(uint8_t keys[256]) {
  if (!g_keyboard_pads) return;
  for (const KeyLayout& k : kKeyLayouts) {
    keys[k.up] = keys[k.down] = keys[k.left] = keys[k.right] = 0;
    for (int b = 0; b < kButtons; ++b) keys[k.button[b]] = 0;
  }
}

void on_frame() { sdl_update(); }

sim::PadInput local_player() {
  // Every gamepad on this PC and the keyboard with the game's own default keys.
  PadState s;
  auto merge = [&s](const PadState& t) {
    auto stronger = [](LONG a, LONG b) { return std::abs(b) > std::abs(a) ? b : a; };
    s.lx = stronger(s.lx, t.lx);
    s.ly = stronger(s.ly, t.ly);
    s.rx = stronger(s.rx, t.rx);
    s.ry = stronger(s.ry, t.ry);
    s.up |= t.up;
    s.down |= t.down;
    s.left |= t.left;
    s.right |= t.right;
    for (int b = 0; b < kButtons; ++b) s.button[b] |= t.button[b];
  };
  sdl_update();
  for (int i = 0; i < g_sdl_slots; ++i) {
    if (!g_ctrl[i]) continue;
    PadState t;
    read_sdl(i, t);
    merge(t);
  }
  uint8_t keys[256];
  if (input::read_keyboard(keys)) {
    auto down = [&keys](int dik) { return (keys[dik] & 0x80) != 0; };
    PadState t;
    t.up = down(DIK_UP);
    t.down = down(DIK_DOWN);
    t.left = down(DIK_LEFT);
    t.right = down(DIK_RIGHT);
    // The keys of the game's own key page (CKeyboardOptionData: attack A, attack B, shuriken, jump, dash,
    // guard, chara change, camera, gauge, up, down, left, right; the mouse's buttons are keys there too),
    // its defaults when they cannot be read: F D R Space E S LShift F1 F2, numpad 8 2 4 6. Enter start,
    // Esc select and the arrows are the game's fixed keys.
    static const int kDefaults[game::kKeyFuncs] = {DIK_F, DIK_D, DIK_R, DIK_SPACE, DIK_E, DIK_S, DIK_LSHIFT, DIK_F1, DIK_F2,
                                                   DIK_NUMPAD8, DIK_NUMPAD2, DIK_NUMPAD4, DIK_NUMPAD6};
    int k[game::kKeyFuncs];
    const uint8_t* opt = patch::read<const uint8_t*>(game::kOptionDataPtr);
    for (int i = 0; i < game::kKeyFuncs; ++i) {
      int v = opt ? patch::read<int32_t>(reinterpret_cast<uintptr_t>(opt) + game::kOptKeys + 4 * i) : 0;
      k[i] = v > 0 && v < 256 ? v : kDefaults[i];
    }
    t.up = t.up || down(k[9]);
    t.down = t.down || down(k[10]);
    t.left = t.left || down(k[11]);
    t.right = t.right || down(k[12]);
    t.button[kCross] = down(k[0]);
    t.button[kSquare] = down(k[1]);
    t.button[kTriangle] = down(k[2]);
    t.button[kCircle] = down(k[3]);
    t.button[kL1] = down(k[4]);
    t.button[kR1] = down(k[5]);
    t.button[kL2] = down(k[6]);
    t.button[kR3] = down(k[7]);
    t.button[kL3] = down(k[8]);
    t.button[kStart] = down(DIK_RETURN) || down(DIK_NUMPADENTER);
    t.button[kSelect] = down(DIK_ESCAPE);
    t.lx = t.left ? -32767 : t.right ? 32767 : 0;
    t.ly = t.up ? -32767 : t.down ? 32767 : 0;
    merge(t);
  }
  sim::PadInput in = to_input(s);
  in.connected = 1;
  return in;
}

sim::PadInput sample(int slot) {
  sim::PadInput in;
  if (!VirtualPad::local_connected(slot)) return in;
  PadState s;
  VirtualPad::read_slot(slot, s);
  in = to_input(s);
  in.connected = 1;
  return in;
}

bool script_button(const char* action, bool down) {
  // "PAD1.START": virtual pad 1, SDL game controller button or a trigger.
  if (_strnicmp(action, "PAD", 3) != 0 || !action[3] || action[4] != '.') return false;
  int pad = action[3] - '1';
  if (pad < 0 || pad >= (int)g_virtual.size()) return true;
  const char* name = action + 5;
  struct Name {
    const char* n;
    int b;
  };
  static const Name kNames[] = {{"A", SDL_CONTROLLER_BUTTON_A},          {"B", SDL_CONTROLLER_BUTTON_B},
                                {"X", SDL_CONTROLLER_BUTTON_X},          {"Y", SDL_CONTROLLER_BUTTON_Y},
                                {"BACK", SDL_CONTROLLER_BUTTON_BACK},    {"START", SDL_CONTROLLER_BUTTON_START},
                                {"LB", SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, {"RB", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
                                {"LS", SDL_CONTROLLER_BUTTON_LEFTSTICK}, {"RS", SDL_CONTROLLER_BUTTON_RIGHTSTICK},
                                {"UP", SDL_CONTROLLER_BUTTON_DPAD_UP},   {"DOWN", SDL_CONTROLLER_BUTTON_DPAD_DOWN},
                                {"LEFT", SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {"RIGHT", SDL_CONTROLLER_BUTTON_DPAD_RIGHT}};
  for (const Name& n : kNames) {
    if (_stricmp(name, n.n) == 0) {
      SDL_JoystickSetVirtualButton(g_virtual[pad], n.b, down ? 1 : 0);
      return true;
    }
  }
  if (_stricmp(name, "LT") == 0 || _stricmp(name, "RT") == 0) {
    SDL_JoystickSetVirtualAxis(g_virtual[pad],
                               _stricmp(name, "LT") == 0 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
                               down ? 32767 : 0);
    return true;
  }
  if (_stricmp(name, "STICKLEFT") == 0 || _stricmp(name, "STICKRIGHT") == 0) {
    SDL_JoystickSetVirtualAxis(g_virtual[pad], SDL_CONTROLLER_AXIS_LEFTX,
                               down ? (_stricmp(name, "STICKLEFT") == 0 ? -32767 : 32767) : 0);
    return true;
  }
  LOGW("debug script: unknown pad button \"%s\"", name);
  return true;
}

}  // namespace pads
