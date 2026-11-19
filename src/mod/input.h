// DirectInput for TMNT2: the game reads the keyboard with GetDeviceState on a
// FOREGROUND device and treats a failed Acquire as "keyboard unplugged".
// - background=run: the keyboard stays "connected" (no keys) while the window
//   is not in front, so the game neither pauses nor shows a controller prompt;
// - [debug] script: timed key presses and frame captures for test runs, where
//   the window never has focus. Later stages inject remote players here.
#pragma once
#include <cstdint>

#include "mod/config.h"

namespace input {

void install(const Config& cfg);  // InitializeASI

// Game thread, once per presented frame: advances the debug script.
void on_frame();

// The keyboard now (DirectInput, plus scripted keys); false if it cannot be read.
// Seconds since the first frame: the clock of [debug] script; in lockstep frames / 60.
double script_time();

bool read_keyboard(uint8_t keys[256]);
void set_background_run(bool run);  // the settings page, at once

// The keyboard's keys of the game (its ОПЦИИ → УПРАВЛЕНИЕ page), which tmnt2net keeps in tmnt2net.ini:
// 0 the game's own (F D R Space ...), 1 WASD + mouse, 2 the player's own choice of keys.
int layout();
void set_layout(int which);  // 0 or 1

}  // namespace input
