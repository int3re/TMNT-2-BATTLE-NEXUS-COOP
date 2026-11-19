#include "mod/pageui.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "mod/game.h"
#include "mod/gamefont.h"
#include "mod/patch.h"
#include "mod/video.h"

namespace pageui {
namespace {

// The game's 2D space on the frame: x, y, w, h (wider than 640 on a wide frame).
struct Space {
  float x, y, w, h;
  int fw, fh;
  bool ok;
};

Space space() {
  Space s{};
  const auto* vs = reinterpret_cast<const float*>(game::kVirtualScreen);
  s.x = vs[0];
  s.y = vs[1];
  s.w = vs[2];
  s.h = vs[3];
  s.ok = video::frame_size(s.fw, s.fh) && s.w > 0 && s.h > 0;
  return s;
}

uint32_t with_alpha(uint32_t rgb, uint8_t a) { return (rgb & 0xFFFFFF) | ((uint32_t)a << 24); }

}  // namespace

float width(const std::wstring& s, float h) { return gamefont::width(s, h); }

uint32_t pulse() {
  float t = (float)(GetTickCount() % 100000) / 1000.0f;
  uint32_t rb = (uint32_t)(127.0f - std::cos(t * 4.0f) * 127.0f);  // the title menu's cursor colour
  return rb << 16 | 0xFF00 | rb;
}

float breath() {
  float t = (float)(GetTickCount() % 100000) / 1000.0f;
  return 0.5f - 0.5f * std::cos(t * 4.0f);
}

void Ui::tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb) {
  shapes_.push_back({x0, y0, 0, 1, argb});
  shapes_.push_back({x1, y1, 0, 1, argb});
  shapes_.push_back({x2, y2, 0, 1, argb});
}

void Ui::grad_h(float x0, float y0, float x1, float y1, uint32_t left, uint32_t right) {
  shapes_.push_back({x0, y0, 0, 1, left});
  shapes_.push_back({x1, y0, 0, 1, right});
  shapes_.push_back({x0, y1, 0, 1, left});
  shapes_.push_back({x1, y0, 0, 1, right});
  shapes_.push_back({x1, y1, 0, 1, right});
  shapes_.push_back({x0, y1, 0, 1, left});
}

void Ui::grad_v(float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom) {
  shapes_.push_back({x0, y0, 0, 1, top});
  shapes_.push_back({x1, y0, 0, 1, top});
  shapes_.push_back({x0, y1, 0, 1, bottom});
  shapes_.push_back({x1, y0, 0, 1, top});
  shapes_.push_back({x1, y1, 0, 1, bottom});
  shapes_.push_back({x0, y1, 0, 1, bottom});
}

void Ui::rect(float x0, float y0, float x1, float y1, uint32_t argb) { grad_h(x0, y0, x1, y1, argb, argb); }

void Ui::text(const std::wstring& s, float x, float y, float h, uint32_t rgb, Align a, uint8_t alpha) {
  if (s.empty()) return;
  if (a != Align::Left) {
    float w = gamefont::width(s, h);
    x -= a == Align::Center ? w / 2 : w;
  }
  words_.push_back({s, x, y + kRise * h, h, rgb, alpha});
}

float Ui::wrapped(const std::wstring& s, float x, float y, float h, float width, float step, uint32_t rgb, Align a,
                  int max_lines) {
  std::wstring line;
  size_t at = 0;
  int lines = 0;
  while (at < s.size() && lines < max_lines) {
    size_t sp = s.find(L' ', at);
    std::wstring word = s.substr(at, sp == std::wstring::npos ? std::wstring::npos : sp - at);
    std::wstring longer = line.empty() ? word : line + L" " + word;
    if (!line.empty() && gamefont::width(longer, h) > width) {
      text(line, x, y, h, rgb, a);
      y += step;
      ++lines;
      line = word;
    } else {
      line = longer;
    }
    if (sp == std::wstring::npos) break;
    at = sp + 1;
  }
  if (!line.empty() && lines < max_lines) {
    text(line, x, y, h, rgb, a);
    y += step;
  }
  return y;
}

void Ui::draw() {
  Space sp = space();
  if (!sp.ok) return;
  if (!shapes_.empty()) {
    // The 2D space onto frame pixels; straight edges on whole pixels (thin lines stay crisp).
    for (overlay::ColorVertex& v : shapes_) {
      v.x = std::floor((v.x - sp.x) / sp.w * sp.fw + 0.5f) - 0.5f;
      v.y = std::floor((v.y - sp.y) / sp.h * sp.fh + 0.5f) - 0.5f;
    }
    overlay::tris_in_scene(video::device(), shapes_.data(), (int)shapes_.size());
  }
  if (!words_.empty()) {
    gamefont::begin();
    for (const Word& w : words_) {
      gamefont::color((uint8_t)(w.rgb >> 16), (uint8_t)(w.rgb >> 8), (uint8_t)w.rgb, w.alpha);
      gamefont::show(w.s, w.x, w.y, w.h);
    }
    gamefont::end();
  }
  shapes_.clear();
  words_.clear();
}

Window window(Ui& ui, const std::wstring& title, const std::wstring& hints, float half_w, float top, float bottom) {
  Space sp = space();
  Window w{-half_w, top, half_w, bottom, 0, 0};
  const float band = 44.0f, foot = hints.empty() ? 0.0f : 28.0f;
  w.top = w.y0 + band + 2;
  w.bottom = w.y1 - foot;
  // The frame behind goes dark; a soft shadow under the window.
  ui.rect(sp.x, sp.y, sp.x + sp.w, sp.y + sp.h, 0x96000000);
  ui.rect(w.x0 + 5, w.y0 + 6, w.x1 + 5, w.y1 + 8, 0x70000000);
  // Body, title band (black to olive, as the game's message windows), lines.
  ui.grad_v(w.x0, w.y0, w.x1, w.y1, 0xF40E1A12, 0xF4050907);
  ui.grad_h(w.x0, w.y0, w.x1, w.y0 + band, 0xFF081008, 0xFF4A7A1E);
  ui.rect(w.x0, w.y0 + band, w.x1, w.y0 + band + 2, with_alpha(kLime, 255));
  ui.rect(w.x0 - 2, w.y0 - 2, w.x1 + 2, w.y0, with_alpha(kLime, 255));
  ui.rect(w.x0 - 2, w.y1, w.x1 + 2, w.y1 + 2, with_alpha(kLime, 255));
  ui.rect(w.x0 - 2, w.y0, w.x0, w.y1, 0xFF4E7A22);
  ui.rect(w.x1, w.y0, w.x1 + 2, w.y1, 0xFF4E7A22);
  ui.text(title, 0, w.y0 + (band - kCap * 27) / 2, 27, kTitle, Align::Center);
  if (foot > 0) {
    ui.rect(w.x0, w.bottom, w.x1, w.y1, 0xB0000000);
    ui.rect(w.x0, w.bottom, w.x1, w.bottom + 1, 0xFF2E4A18);
    ui.text(hints, 0, w.bottom + (foot - kCap * 13) / 2 + 1, 13, kDim, Align::Center);
  }
  return w;
}

void chosen(Ui& ui, const Window& w, float top, float bottom) {
  uint32_t a = (uint32_t)(96 + 40 * breath());
  ui.grad_h(w.x0 + 10, top - 7, w.x1 - 10, bottom + 7, a << 24 | 0x3C9A18, 0x143C9A18);
  ui.rect(w.x0 + 10, top - 7, w.x0 + 13, bottom + 7, with_alpha(kLime, 255));
}

void rule(Ui& ui, const Window& w, float y) { ui.rect(w.x0 + 14, y, w.x1 - 14, y + 1, 0xFF2E4A18); }

void arrows(Ui& ui, float x, float y, float h, float half, uint32_t rgb) {
  float m = y + kCap * h * 0.5f, s = h * 0.27f, gap = 9.0f + 2.0f * breath();
  uint32_t c = with_alpha(rgb, 255);
  ui.tri(x - half - gap, m - s, x - half - gap, m + s, x - half - gap - s * 1.3f, m, c);
  ui.tri(x + half + gap, m - s, x + half + gap, m + s, x + half + gap + s * 1.3f, m, c);
}

void notice(Ui& ui, const std::wstring& s, uint32_t rgb) {
  if (s.empty()) return;
  float h = 18.0f, w = gamefont::width(s, h);
  const float room = 600.0f;
  if (w > room) {  // a long one: smaller letters
    h = std::max(11.0f, h * room / w);
    w = gamefont::width(s, h);
  }
  float x0 = -w / 2 - 16, x1 = w / 2 + 16, y0 = -234.0f, y1 = y0 + kCap * h + 16;
  ui.grad_h(x0, y0, x1, y1, 0xE4081008, 0xE42E4E16);
  ui.rect(x0, y0 - 2, x1, y0, with_alpha(kLime, 255));
  ui.rect(x0, y1, x1, y1 + 2, with_alpha(kLime, 255));
  ui.text(s, 0, y0 + 8, h, rgb, Align::Center);
}

}  // namespace pageui
