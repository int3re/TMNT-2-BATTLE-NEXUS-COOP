// Text over the game's frame (netplay lobby, "waiting for a player", desync):
// GDI renders the lines into a texture once per change, Direct3D draws it on
// the back buffer right before Present. Works in every screen mode.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct IDirect3DDevice9;

namespace overlay {

struct Line {
  std::wstring text;
  uint32_t color = 0xFFFFFFFF;  // ARGB
  bool big = false;
};

// Any thread. The panel sits in the middle (center = true) or at the top.
void set_panel(const std::vector<Line>& lines, bool center);
void clear_panel();
// A short notice at the top for `seconds` (any thread).
void notify(const std::wstring& text, double seconds, uint32_t color = 0xFFFFD040);

// Game thread, while the game draws its frame (menus.cpp, with the game's font):
// the current notice, if any; after notice_drawn() the overlay leaves it out of
// this frame.
bool peek_notice(std::wstring* text, uint32_t* color);
void notice_drawn();

// Game thread, inside the game's own scene (Direct3D BeginScene active): a dark
// rectangle in frame pixels, states saved and restored.
void fill_in_scene(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, uint32_t argb);
// The same for any number of shaded triangles (a list, three vertices each), in one go.
struct ColorVertex {
  float x, y, z, rhw;
  uint32_t argb;
};
void tris_in_scene(IDirect3DDevice9* dev, const ColorVertex* v, int vertices);

// Game thread, inside Present: draws what is set onto the back buffer.
void draw(IDirect3DDevice9* dev, int frame_w, int frame_h);
// Device reset: textures are managed, nothing to do; device gone: release.
void release();

}  // namespace overlay
