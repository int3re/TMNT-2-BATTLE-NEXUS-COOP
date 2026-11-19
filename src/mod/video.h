// Screen for TMNT2: every monitor mode in the game's list, and three ways to
// show the game. Fullscreen is the game's own exclusive mode at any
// resolution. Borderless and windowed switch on the window mode the retail
// build carries but never enables (m_bWindowMode, "-wnd"): RenderWare renders
// a frame of our size into the window, the mod places the window, and Present
// fits the frame in (black bars when the aspect differs). See re/notes.md.
#pragma once
#include <vector>

#include "mod/config.h"

struct IDirect3DDevice9;

namespace video {

void install(const Config& cfg);  // InitializeASI, after the exe hash matched

// The frame the game renders (window modes: tmnt2net's size; fullscreen: the mode).
bool frame_size(int& w, int& h);

// [debug]: save the next presented frame as screenshots/tmnt2net_t<tenths of a second>.bmp.
void request_shot(double tag_seconds);

// Game thread, while the game's own frame is held (netplay lobby, waiting for a
// player): presents the last frame again (black: a black one) with the overlay.
bool present_now(bool black);

// The game's Direct3D device (null before it exists).
IDirect3DDevice9* device();
// The settings page, at once: the game keeps running while its window is not in front.
void set_background_run(bool run);

// The monitor's 32-bit modes from 640x480, smallest first: the choices of РАЗРЕШЕНИЕ.
struct Size {
  int w, h;
};
std::vector<Size> resolutions();
// The frame's size at once, by the game's own change of video mode (game thread, outside the draw).
// 0 x 0: the automatic size (the monitor's; in a window the window's own).
bool set_resolution(int w, int h);

}  // namespace video
