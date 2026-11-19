// The look of tmnt2net's own pages and notices: a window in the colours of the game's message
// windows (dark body, lime lines, a black-to-olive title band), a highlight bar for the chosen row,
// small arrows beside a value, a band of key hints at the bottom. Shapes are plain Direct3D inside
// the game's scene, words are the game's own font (gamefont). Game thread, while the game draws.
//
// Coordinates are the game's 2D space (x -320..320, y -240 top .. 240, the 4:3 middle of the frame).
// A Ui collects shapes and words in any order; draw() puts the shapes down first, then the words.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "mod/overlay.h"

namespace pageui {

enum class Align { Left, Center, Right };

// Colours (RGB for words, ARGB for shapes).
constexpr uint32_t kTitle = 0xFFA000, kItem = 0xFFFF00, kText = 0xFFFFFF, kDim = 0xA8B4A8, kOff = 0x707868,
                   kGood = 0x8CFF8C, kWarn = 0xFFA050, kBad = 0xFF7058, kLime = 0x9EDC3C;

class Ui {
 public:
  void rect(float x0, float y0, float x1, float y1, uint32_t argb);
  void grad_h(float x0, float y0, float x1, float y1, uint32_t left, uint32_t right);
  void grad_v(float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom);
  void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb);
  // Words already in the game's encoding (gamefont::say / name); y is the top of the capital
  // letters, which are kCap * h tall.
  void text(const std::wstring& s, float x, float y, float h, uint32_t rgb, Align a = Align::Left, uint8_t alpha = 255);
  // Several lines no wider than `width`, `step` apart; returns the y after the last one.
  float wrapped(const std::wstring& s, float x, float y, float h, float width, float step, uint32_t rgb,
                Align a = Align::Left, int max_lines = 4);
  void draw();

 private:
  struct Word {
    std::wstring s;
    float x, y, h;
    uint32_t rgb;
    uint8_t alpha;
  };
  std::vector<overlay::ColorVertex> shapes_;
  std::vector<Word> words_;
};

// The game's font puts a line's capitals from 0.31 h above the y it is given to 0.41 h below it.
constexpr float kCap = 0.72f, kRise = 0.31f;
float width(const std::wstring& s, float h);
// The colour of the chosen row's words now (the title menu's cursor pulse, yellow to green).
uint32_t pulse();
// 0..1, slow: for marks that breathe.
float breath();

// A window with a title band and a band of key hints; the rest of the frame is dimmed.
struct Window {
  float x0, y0, x1, y1;  // the whole window
  float top, bottom;     // the room between the bands
};
Window window(Ui& ui, const std::wstring& title, const std::wstring& hints, float half_w = 292, float top = -200,
              float bottom = 224);
// The bar behind the chosen row: from the top of its first letters to the bottom of its last.
void chosen(Ui& ui, const Window& w, float top, float bottom);
inline float bottom_of(float y, float h) { return y + kCap * h; }
// A thin line across the window's room.
void rule(Ui& ui, const Window& w, float y);
// Small arrows left and right of a value centred at x (the value `half` wide to each side).
void arrows(Ui& ui, float x, float y, float h, float half, uint32_t rgb);
// A short notice at the top of the frame.
void notice(Ui& ui, const std::wstring& s, uint32_t rgb);

}  // namespace pageui
