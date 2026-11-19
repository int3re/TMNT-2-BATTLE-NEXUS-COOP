// The finished frame made crisper and its colours fuller, right before Present: the back buffer
// (resolved, if antialiased) is copied into a texture and drawn back through one pixel shader
// (tools/postfx.hlsl: contrast-adaptive sharpening, vibrance). The game's logic sees nothing of it.
// [video] sharpen = 0..100, colors = 0..100; both can change while the game runs (the settings page).
#pragma once

#include "mod/config.h"

struct IDirect3DDevice9;

namespace postfx {

void install(const Config& cfg);
// Game thread, inside Present, before tmnt2net's own words go over the frame.
void apply(IDirect3DDevice9* dev);
// Before a device Reset and when the device goes: the frame's copy is a default-pool resource.
void release();
// The settings page: 0..100 each, at once.
void set(int sharpen, int colors);

}  // namespace postfx
