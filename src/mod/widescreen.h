// Widescreen for TMNT2 (aspect = auto):
// - Hor+ 3D: RwCameraSetViewWindow widens the 4:3 view window of every camera
//   whose frame is wider than 4:3 (vertical view kept);
// - 2D in the 4:3 middle: the game lays out sprites, the HUD, menus, movies and
//   Rt2d text on a virtual 640x480 screen spread over CScreen::Width(). The
//   virtual screen (CSprite::m_fVirtualScreen*) is widened by the same factor,
//   so that 640 virtual units cover the 4:3 middle. 2D placed in real pixels
//   (fades, the players' markers projected from the world) keeps the full width.
// - the area map (a 4:3 picture: a 2D scene) goes on to the frame's edges: its ground reflected in the
//   side bars, its clouds, moons and planets as they are (map = wide).
// See re/notes.md "Widescreen".
#pragma once
#include "mod/config.h"

namespace widescreen {

void install(const Config& cfg);  // InitializeASI, after video::install

// The players' boxes (health.cpp, around StatusWakuDispSub and its bars): drawn smaller towards
// the top right corner (gauges_size). Pairs; game thread.
// The settings page, at once: the stage's gauges in the corners, their size (0.5..1).
void set_gauges(bool corners, float scale);
void right_gauge_begin();
void right_gauge_end();

// RwCameraSetViewWindow without tmnt2net's Hor+ (camera.cpp: a camera's view for one draw).
void set_view_window_raw(void* camera, const float* xy);
// After a new video mode while the game runs: the 2D layout for the new frame's shape.
void frame_changed();

}  // namespace widescreen
