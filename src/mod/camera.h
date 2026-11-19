// Each PC's own view of the stage (0.10.0, user request: "у каждого игрока своя камера ... каждый
// смотрит на своего персонажа", zoom with the mouse wheel).
// The game has one stage camera, and it is part of the game's logic (enemy activity, line of
// sight, the co-op zoom: re/notes.md §10), so every PC of a session keeps computing the same one.
// Only while the stage is drawn (the stage camera's CCamera BeginScene..EndScene) its frame is
// moved for this PC:
// - path cameras (most of the game): centred on this PC's player, at the distance the game would
//   use for that player alone, times the wheel's zoom;
// - set and fixed cameras (rooms, bosses): the game's view, with the wheel's zoom only;
// - introductions and other scripted cameras: untouched.
// Several players on one PC share the screen: the wheel's zoom only. After the draw the game's
// frame comes back and the camera's view matrix and frustum are made from it again, the way the
// camera's own update makes them, so the logic sees exactly the game's camera.
#pragma once
#include "mod/config.h"

namespace camera {

void install(const Config& cfg);  // InitializeASI, after health::install

// The window procedure (game thread): mouse wheel steps (WHEEL_DELTA = one notch), middle button.
void wheel(int delta);
void reset_zoom();
void set_own(bool own);  // the settings page: own_camera, at once

}  // namespace camera
