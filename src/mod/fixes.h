// Fixes for bugs of the original game and diagnostics:
// - "Unkown Error" at start (COptionData::Snapshot reads 6 gamepad records
//   from an array sized for CController::Max() = 3 + joysticks);
// - exceptions in the game's code and its error message boxes go to the log;
// - optional second instance (the game exits when a "TMNT2" window exists).
#pragma once
#include "mod/config.h"

namespace fixes {

void install(const Config& cfg);  // InitializeASI, game patches allowed (exe hash matched)
void install_diagnostics(const Config& cfg);  // any exe

// The window was closed (close button, Alt+F4). The retail game has no such
// exit (it never posts WM_QUIT): ask its message loop to stop, and if the
// shutdown crashes (seen in ntdll while a movie was playing) or hangs, end the
// process quietly instead of showing "Unkown Error".
void request_quit();
bool quitting();  // the player closed the game (request_quit was called)

}  // namespace fixes
