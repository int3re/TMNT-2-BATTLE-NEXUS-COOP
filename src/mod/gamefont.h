// The game's own lettering for tmnt2net's words: CGameFont (fonts/mainfont.met,
// the Russian letters of your copy of the game) through the game's functions,
// inside the game's 2D scene. Game thread only, while the game draws a frame.
//
// Coordinates are the game's 2D space: x -320..320, y -240 (top) .. 240, the
// 4:3 middle of the frame (widescreen keeps that space centred).
#pragma once
#include <cstdint>
#include <string>

namespace gamefont {

// The font and the texts are loaded (from the logo on).
bool ready();

// UTF-8 text (Russian or English) in the game's text encoding. The Russian
// version stores Cyrillic either as Unicode or as Windows-1251 codes; tmnt2net
// looks at the game's own word for ОПЦИИ / ПАРАМЕТРЫ to tell. `en` is used when
// the game is not Russian.
std::wstring text(const char* ru_utf8, const char* en);
// The same for Unicode text from elsewhere (names, notices).
std::wstring text(const std::wstring& s);
// Any text for the game's font, written the way the game's own Russian texts are: the letters
// its font lacks become the look-alikes the translation itself uses (Б 6, З 3, Ч 4, Щ Ш, Ъ Ь,
// Э 3), an arrow (U+2192) its arrow sign, other signs it lacks are left out. Never empty-handed
// for normal sentences.
std::wstring say(const char* ru_utf8, const char* en);
std::wstring say(const std::wstring& s);
// False when the game's font cannot show every letter of s (then tmnt2net
// writes it itself).
bool can_write(const std::wstring& s);
// A name for the game's font: letters it lacks become look-alikes (Б 6, З 3, Ч 4,
// Щ Ш, Ъ Ь, Э Е; _ -, [ ] ( )), anything else it cannot show is left out; Latin
// capitals become small ones (the Russian font has Russian letters in their place).
// "" if nothing of it can be shown (the caller shows P1..P4 instead).
std::wstring name(const std::wstring& s);

void begin();  // CSystem2D::BeginScene
void end();    // CSystem2D::EndScene

void color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
float width(const std::wstring& s, float height);
void show(const std::wstring& s, float x, float y, float height);
void show_centered(const std::wstring& s, float y, float height);

// A dark panel behind words, in the game's 2D space (raw Direct3D, inside the scene).
void panel(float x0, float y0, float x1, float y1, uint8_t alpha);

// A menu sound of the game: 0 cursor, 1 decide, 2 cancel.
void sound(int i);
// A button of any controller went down this frame (the game's own reading).
bool pressed(uint32_t mask);
uint32_t ok_mask();
uint32_t cancel_mask();

}  // namespace gamefont
