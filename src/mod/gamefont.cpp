#include "mod/gamefont.h"

#include <windows.h>

#include "common/log.h"
#include "mod/game.h"
#include "mod/overlay.h"
#include "mod/patch.h"
#include "mod/video.h"

namespace gamefont {
namespace {

using GetText_t = const wchar_t*(__cdecl*)(int);
using SetRGBA_t = void(__fastcall*)(void* font, void* edx, int r, int g, int b, int a);
using Show_t = void(__fastcall*)(void* font, void* edx, const wchar_t* s, float h, float x, float y);
using Width_t = float(__cdecl*)(void* rt2dfont, const wchar_t* s, float h);
using Scene_t = void(__fastcall*)(void* sys2d, void* edx);
using Sound_t = void(__cdecl*)(int);
using Trigger_t = int(__cdecl*)(int port, uint32_t mask);

// Unicode: real Cyrillic; Cp1251: Cyrillic at the Windows-1251 codes; Remapped:
// the common Russian release draws Russian capitals in place of the Latin ones
// (fonts/mainfont.met: A А, B В, C С, D Д, E Е, F Г, G Ж, H Н, J Ь, K К, L Л,
// M М, N И, O О, P Р, Q Ц, R Я, S Ы, T Т, U Ю, V Ф, W Ш, X Х, Y У, Z П; no Б, З,
// Ч, Щ, Ъ, Э; small Latin letters stay Latin). re/notes.md §12.
enum class Enc { Unknown, Unicode, Cp1251, Remapped, English };
Enc g_enc = Enc::Unknown;

void* font() { return patch::read<void*>(game::kGameFont); }

void detect() {
  if (g_enc != Enc::Unknown) return;
  const wchar_t* t = reinterpret_cast<GetText_t>(game::kGetText)(0xD);  // ОПЦИИ / ПАРАМЕТРЫ / OPTIONS
  const wchar_t* new_game = reinterpret_cast<GetText_t>(game::kGetText)(0x232);
  if (new_game && wcscmp(new_game, L"HOBAR NFPA") == 0) {  // НОВАЯ ИГРА in the remapped font
    g_enc = Enc::Remapped;
    LOGI("gamefont: the game's texts are Russian (capitals drawn in place of Latin letters)");
    return;
  }
  g_enc = Enc::English;
  for (const wchar_t* c = t; c && *c; ++c) {
    if (*c >= 0x400 && *c <= 0x4FF) {
      g_enc = Enc::Unicode;
      break;
    }
    if (*c >= 0xC0 && *c <= 0xFF) g_enc = Enc::Cp1251;
  }
  LOGI("gamefont: the game's texts are %s", g_enc == Enc::Unicode  ? "Russian (Unicode)"
                                            : g_enc == Enc::Cp1251 ? "Russian (Windows-1251 codes)"
                                                                   : "not Russian (English words used)");
}

// Russian capitals of the remapped font: index = letter - U+0410 (А..Я), 0 = none.
const char kRemap[32] = {'A', 0,   'B', 'F', 'D', 'E', 'G', 0,   'N', 'N', 'K', 'L', 'M', 'H', 'O', 'Z',
                         'P', 'C', 'T', 'Y', 'V', 'X', 'Q', 0,   'W', 0,   0,   'S', 'J', 0,   'U', 'R'};

// The text for the remapped font; false if a letter is missing there.
bool remap(const std::wstring& in, std::wstring* out) {
  out->clear();
  for (wchar_t c : in) {
    if (c == 0x401 || c == 0x451) c = 0x415;  // Ё -> Е
    if (c >= 0x430 && c <= 0x44F) c = (wchar_t)(c - 0x20);  // small -> capital
    if (c >= 0x410 && c <= 0x42F) {
      char m = kRemap[c - 0x410];
      if (!m) return false;
      *out += (wchar_t)m;
    } else if (c >= L'A' && c <= L'Z') {
      *out += (wchar_t)(c - L'A' + L'a');  // Latin capitals would show as Russian ones: small Latin
    } else if (c == L' ' || (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || wcschr(L"!\"%&'()*+,-./:=>?", c)) {
      *out += c;
    } else {
      return false;
    }
  }
  return true;
}

std::wstring utf16(const char* s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
  return w;
}

}  // namespace

bool ready() { return font() != nullptr && patch::read<void*>(game::kSystem2D) != nullptr; }

std::wstring text(const char* ru, const char* en) {
  detect();
  if (g_enc == Enc::English) return utf16(en);
  std::wstring w = utf16(ru);
  if (g_enc == Enc::Unicode) return w;
  if (g_enc == Enc::Remapped) {
    std::wstring r;
    if (!remap(w, &r)) LOGD("gamefont: \"%s\" has letters the game's font lacks", ru);
    return r;
  }
  std::string a(w.size() * 2 + 1, '\0');
  int n = WideCharToMultiByte(1251, 0, w.c_str(), (int)w.size(), &a[0], (int)a.size(), nullptr, nullptr);
  std::wstring out;
  for (int i = 0; i < n; ++i) out += (wchar_t)(uint8_t)a[i];  // the font keeps the letters at the 1251 codes
  return out;
}

bool can_write(const std::wstring& s) {
  detect();
  if (g_enc != Enc::Remapped) return true;
  std::wstring r;
  return remap(s, &r);
}

std::wstring text(const std::wstring& s) {
  detect();
  if (g_enc == Enc::Remapped) {
    std::wstring r;
    remap(s, &r);
    return r;
  }
  if (g_enc != Enc::Cp1251) return s;
  std::string a(s.size() * 2 + 1, '\0');
  int n = WideCharToMultiByte(1251, 0, s.c_str(), (int)s.size(), &a[0], (int)a.size(), nullptr, nullptr);
  std::wstring out;
  for (int i = 0; i < n; ++i) out += (wchar_t)(uint8_t)a[i];
  return out;
}

std::wstring say(const std::wstring& s) {
  detect();
  std::wstring in;
  for (wchar_t c : s) {  // signs no font of the game has, as plain ones
    switch (c) {
      case 0x2014: case 0x2013: in += L'-'; break;          // dashes
      case 0x00AB: case 0x00BB: case 0x201C: case 0x201D: case 0x201E: in += L'"'; break;
      case 0x2026: in += L"..."; break;
      case 0x2192: in += g_enc == Enc::Remapped ? L"\x01" : L" - "; break;  // (the arrow: below)
      default: in += c; break;
    }
  }
  if (g_enc != Enc::Remapped) return text(in);
  std::wstring out;
  size_t at = 0;
  while (at <= in.size()) {  // name() piece by piece: it trims, the pieces' spaces are kept
    size_t arrow = in.find(L'\x01', at);
    std::wstring piece = in.substr(at, arrow == std::wstring::npos ? std::wstring::npos : arrow - at);
    size_t lead = 0, trail = 0;
    while (lead < piece.size() && piece[lead] == L' ') ++lead;
    while (trail < piece.size() - lead && piece[piece.size() - 1 - trail] == L' ') ++trail;
    out += std::wstring(lead, L' ') + name(piece) + std::wstring(lead < piece.size() ? trail : 0, L' ');
    if (arrow == std::wstring::npos) break;
    out += L'@';  // the font's arrow (as in the game's "ВВОД@ВЫБОР")
    at = arrow + 1;
  }
  return out;
}

std::wstring say(const char* ru, const char* en) {
  detect();
  return g_enc == Enc::English ? utf16(en) : say(utf16(ru));
}

std::wstring name(const std::wstring& s) {
  detect();
  if (g_enc != Enc::Remapped) return text(s);
  std::wstring out;
  for (wchar_t c : s) {
    wchar_t u = (c >= 0x430 && c <= 0x44F) ? (wchar_t)(c - 0x20) : c;
    switch (u) {
      case 0x411: u = L'6'; break;    // Б
      case 0x417: u = L'3'; break;    // З
      case 0x427: u = L'4'; break;    // Ч
      case 0x429: u = 0x428; break;   // Щ -> Ш
      case 0x42A: u = 0x42C; break;   // Ъ -> Ь
      case 0x42D: u = L'3'; break;    // Э (the game's own texts: 3КРАН)
      case L'_': case L'~': u = L'-'; break;  // signs the font lacks: look-alikes
      case L'[': case L'{': case L'<': u = L'('; break;
      case L']': case L'}': u = L')'; break;
      case L'\\': u = L'/'; break;
      case L'|': u = L'!'; break;
      case L';': u = L':'; break;
      case L'`': u = L'\''; break;
      default: break;
    }
    std::wstring one(1, u), r;
    if (remap(one, &r)) out += r;
  }
  while (!out.empty() && out.back() == L' ') out.pop_back();
  while (!out.empty() && out.front() == L' ') out.erase(out.begin());
  return out;
}

// RenderWare's render states (RwEngineInstance +0x20 set, +0x24 get, as the game
// calls them): words over a 3D scene must not be depth-tested, the way the game's
// own HUD draws (CSystem2D::PushRenderState).
using RsSet_t = int(__cdecl*)(int state, void* value);
using RsGet_t = int(__cdecl*)(int state, void* value);
constexpr int kZTest = 6, kZWrite = 8;
uintptr_t g_saved_ztest = 0, g_saved_zwrite = 0;

void begin() {
  void* s = patch::read<void*>(game::kSystem2D);
  if (s) reinterpret_cast<Scene_t>(game::kSystem2DBegin)(s, nullptr);
  uint8_t* engine = patch::read<uint8_t*>(game::kEngineInstance);
  if (!engine) return;
  auto get = patch::read<RsGet_t>(reinterpret_cast<uintptr_t>(engine) + 0x24);
  auto set = patch::read<RsSet_t>(reinterpret_cast<uintptr_t>(engine) + 0x20);
  get(kZTest, &g_saved_ztest);
  get(kZWrite, &g_saved_zwrite);
  set(kZTest, nullptr);
  set(kZWrite, nullptr);
}

void end() {
  uint8_t* engine = patch::read<uint8_t*>(game::kEngineInstance);
  if (engine) {
    auto set = patch::read<RsSet_t>(reinterpret_cast<uintptr_t>(engine) + 0x20);
    set(kZTest, reinterpret_cast<void*>(g_saved_ztest));
    set(kZWrite, reinterpret_cast<void*>(g_saved_zwrite));
  }
  void* s = patch::read<void*>(game::kSystem2D);
  if (s) reinterpret_cast<Scene_t>(game::kSystem2DEnd)(s, nullptr);
}

void color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (void* f = font()) reinterpret_cast<SetRGBA_t>(game::kFontSetRGBA)(f, nullptr, r, g, b, a);
}

float width(const std::wstring& s, float height) {
  void* f = font();
  if (!f || s.empty()) return 0;
  void* rt = patch::read<void*>(reinterpret_cast<uintptr_t>(f) + 8);
  return rt ? reinterpret_cast<Width_t>(game::kRt2dFontGetStringWidth)(rt, s.c_str(), height) : 0;
}

void show(const std::wstring& s, float x, float y, float height) {
  void* f = font();
  if (!f || s.empty()) return;
  reinterpret_cast<Show_t>(game::kFontShow)(f, nullptr, s.c_str(), height, x, y);
}

void show_centered(const std::wstring& s, float y, float height) { show(s, -width(s, height) / 2, y, height); }

void panel(float x0, float y0, float x1, float y1, uint8_t alpha) {
  int fw = 0, fh = 0;
  if (!video::frame_size(fw, fh)) return;
  auto* vs = reinterpret_cast<const float*>(game::kVirtualScreen);  // x, y, w, h of the 2D space in the frame
  float vx = vs[0], vy = vs[1], vw = vs[2], vh = vs[3];
  if (vw <= 0 || vh <= 0) return;
  auto px = [&](float x) { return (x - vx) / vw * fw; };
  auto py = [&](float y) { return (y - vy) / vh * fh; };
  overlay::fill_in_scene(video::device(), px(x0), py(y0), px(x1), py(y1), (uint32_t)alpha << 24);
}

void sound(int i) { reinterpret_cast<Sound_t>(game::kMenuSound)(i); }

bool pressed(uint32_t mask) { return reinterpret_cast<Trigger_t>(game::kGetDigitalTrigger)(-1, mask) != 0; }
uint32_t ok_mask() { return patch::read<uint32_t>(game::kDigitalOk); }
uint32_t cancel_mask() { return patch::read<uint32_t>(game::kDigitalCancel); }

}  // namespace gamefont
