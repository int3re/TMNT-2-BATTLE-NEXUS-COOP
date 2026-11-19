// Gamepads for TMNT2. The game reads joysticks through DirectInput and expects
// the layout of a PS2 pad adapter (buttons 0..11 = cross, square, circle,
// triangle, L1, L2, R1, R2, select, start, L3, R3; d-pad as POV 0; sticks on
// X/Y and Rz/Slider0). Xbox and PlayStation pads report other layouts there.
// tmnt2net shows the game virtual DirectInput joysticks in that layout:
// - pad slots fed by SDL game controllers (XInput, DualShock/DualSense, Switch
//   Pro over HIDAPI), with hot-plug; the physical pads SDL serves are hidden
//   from the game's own enumeration so nothing is read twice;
// - [input] keyboard_players = 2: the keyboard split into two pads (left and
//   right half); the game's own keyboard no longer sees those keys;
// - [debug] virtual_pads: SDL virtual controllers driven by the debug script.
// Later stages feed network players through the same slots.
#pragma once
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <cstdint>

#include "mod/config.h"
#include "mod/sim.h"

namespace pads {

void configure(const Config& cfg);  // InitializeASI
bool active();                      // tmnt2net presents its own joysticks

// --- called from the DirectInput hooks (game thread) -----------------------------
// EnumDevices: present tmnt2net's pads, then the game's other devices (minus
// the ones SDL serves), at most kMaxJoysticks joysticks in all.
using EnumDevices_t = HRESULT(WINAPI*)(IDirectInput8A*, DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD);
HRESULT enum_devices(EnumDevices_t original, IDirectInput8A* di, DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref,
                     DWORD flags);
// CreateDevice / GetDeviceStatus for tmnt2net's GUIDs (false: not ours).
bool create_device(REFGUID guid, LPDIRECTINPUTDEVICE8A* out);
bool device_status(REFGUID guid, HRESULT* hr);

// The keyboard as read this frame (the game's buffer); keys owned by the
// keyboard pads are removed from what the game's keyboard sees.
void on_keyboard(uint8_t keys[256]);

// Once per presented frame: controller hot-plug.
void on_frame();

// Lockstep ([debug] determinism): the game always sees sim::kPads joysticks and
// none of its own; their state is the frame's input. This reads what is behind
// a slot on this PC (controller or keyboard half) for the next frame's input.
sim::PadInput sample(int slot);
// Netplay: this PC's player, from every gamepad here and the keyboard (the
// game's default keys: arrows, F/D/R/Space, E/S, LShift, F1/F2, Enter, Esc).
sim::PadInput local_player();

// [debug] script "PAD1.START" etc.: press / release a button of a virtual pad.
bool script_button(const char* action, bool down);

}  // namespace pads
