#include "mod/coop.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "common/log.h"
#include "mod/game.h"
#include "mod/gamefont.h"
#include "mod/pageui.h"
#include "mod/input.h"
#include "mod/netplay.h"
#include "mod/patch.h"
#include "mod/results.h"
#include "mod/sim.h"
#include "mod/video.h"

namespace coop {
namespace {

int g_players = 1;

using Method_t = void(__fastcall*)(uint8_t* self, void* edx);

// --- character select (CPlayerSelectWorkPool, re/notes.md §13) -----------------------------
Method_t oSelectMove = nullptr;
bool g_select_seen = false;  // the character select ran since the last frame was drawn

uint8_t* player(uint8_t* pool, int p) { return pool + game::kSelPlayers + p * game::kSelPlayerSize; }
bool enabled(uint8_t* pool, int slot) { return pool[game::kSelChars + slot * 8] != 0; }
bool locked(uint8_t* pool, int slot) { return pool[game::kSelChars + slot * 8 + 1] != 0; }
int& field(uint8_t* pl, int off) { return *reinterpret_cast<int*>(pl + off); }

// Costumes. The game takes a costume only while a shoulder button is held as the hero is picked (R1
// samurai, L1 nexus) and only once it is unlocked. tmnt2net: every costume from the start, chosen with
// up / down - ordinary, samurai, nexus, then the place's secret partner where the game offers one (the
// game's own up / down) - and the choice stays until changed; its name is under the player's panel.
// The choice lives in the pool's own bytes (the secret flag and the two costume flags, which the game
// reads when a hero is picked and when it draws the icons), written again after every Move: the same
// on every PC of a network game, and after a join in mid-game, from the same buttons.
bool g_costumes = false;
// The controller of each panel while its player is in (-1: nobody yet). Panel i is the game's controller
// i: the i-th player to press Start - in a network game whichever PC that was, not network player i.
int g_select_ctrl[4] = {-1, -1, -1, -1};
struct SelectLook {
  int rows = 1;         // rows of picks in the player's panel (the picks made and the one being made)
  bool shown = false;   // the player may still pick: the name of the look is drawn
  int look = 0;         // 0 ordinary, 1 samurai, 2 nexus, 3 the secret partner
  bool partner = false; // the place offers its partner
} g_look[4];

int look_of(uint8_t* pool, int p) {
  if (player(pool, p)[game::kSelSecret]) return 3;
  return pool[game::kSelSamurai + p] ? 1 : pool[game::kSelNexus + p] ? 2 : 0;
}

void __fastcall hkSelectMove(uint8_t* self, void* edx) {
  int before[4] = {0, 0, 0, 0}, step[4] = {0, 0, 0, 0}, place[4] = {0, 0, 0, 0};
  bool was_secret[4] = {false, false, false, false};
  if (g_costumes) {
    auto trigger = reinterpret_cast<int(__cdecl*)(int, uint32_t)>(game::kGetDigitalTrigger);
    const uint32_t ok = patch::read<uint32_t>(game::kDigitalOk), cancel = patch::read<uint32_t>(game::kDigitalCancel);
    for (int p = 0; p < 4; ++p) {
      uint8_t* pl = player(self, p);
      before[p] = look_of(self, p);
      place[p] = field(pl, game::kSelCursor);
      was_secret[p] = pl[game::kSelSecret] != 0;
      if (field(pl, game::kSelState) != 1) continue;
      int pad = *reinterpret_cast<uint16_t*>(pl);
      // (the game's own order: OK, cancel, left, right, and only then up / down)
      if ((trigger(pad, ok) & 0xFF) || (trigger(pad, cancel) & 0xFF) || (trigger(pad, 4) & 0xFF) || (trigger(pad, 8) & 0xFF))
        continue;
      step[p] = (trigger(pad, 2) & 0xFF) ? 1 : (trigger(pad, 1) & 0xFF) ? -1 : 0;  // down: the next look, up: the one before
    }
  }
  oSelectMove(self, edx);
  if (g_costumes) {
    for (int p = 0; p < 4; ++p) {
      uint8_t* pl = player(self, p);
      int cursor = field(pl, game::kSelCursor);
      bool partner = cursor >= 0 && cursor <= 3 && enabled(self, cursor + 4);
      int look = before[p];
      bool secret_now = pl[game::kSelSecret] != 0;
      if (step[p]) {
        int n = partner ? 4 : 3;
        look = ((look >= n ? 0 : look) + step[p] + n) % n;
        if (secret_now == was_secret[p]) reinterpret_cast<void(__cdecl*)(int)>(game::kPlaySE)(0x1004);  // (else the game's own)
      } else if (secret_now != was_secret[p]) {
        look = secret_now ? 3 : 0;  // (the game's own switch without our step: follow it)
      }
      if (look == 3 && !partner) look = 0;
      pl[game::kSelSecret] = look == 3 ? 1 : 0;
      self[game::kSelSamurai + p] = look == 1 ? 1 : 0;
      self[game::kSelNexus + p] = look == 2 ? 1 : 0;
      static int picked_before[4] = {0, 0, 0, 0};
      int picked = field(pl, game::kSelPicked);
      if (picked > picked_before[p] && picked >= 1 && picked <= 4)
        LOGI("character select: player %d picks place %d, look %d (0 ordinary, 1 samurai, 2 nexus, 3 partner); costume kept %d",
             p + 1, place[p], before[p], field(pl, 0x28 + (picked - 1) * 4));
      picked_before[p] = picked;
      g_look[p].look = look;
      g_look[p].partner = partner;
      g_look[p].rows = std::max(1, std::min(4, picked + (picked < field(pl, game::kSelMax) ? 1 : 0)));
      g_look[p].shown = field(pl, game::kSelState) == 1 && field(pl, game::kSelPicked) < field(pl, game::kSelMax);
    }
  }
  for (int p = 0; p < 4; ++p) {
    uint8_t* pl = player(self, p);
    int ctrl = field(pl, game::kSelState) == 1 ? (int)*reinterpret_cast<int16_t*>(pl) : -1;
    if (ctrl != g_select_ctrl[p] && ctrl >= 0 && netplay::active()) {
      int port = (int)(int16_t)reinterpret_cast<int(__cdecl*)(int)>(game::kGetPhysicalPort)(ctrl);
      LOGI("character select: panel %d is network player %d (controller %d)", p + 1, port + 1, ctrl);
    }
    g_select_ctrl[p] = ctrl;
  }
  if (!g_select_seen) {
    static uint32_t last = 0;
    if (sim::frame() > last + 60) LOGI("character select (frame %u)", sim::frame());
    last = sim::frame();
  }
  g_select_seen = true;
  int total = 0, active = 0;
  for (int p = 0; p < 4; ++p) {
    total += field(player(self, p), game::kSelPicked);
    active += field(player(self, p), game::kSelState) == 1 ? 1 : 0;
  }
  if (active > 0) g_players = active;
  if (total >= 4) return;  // the team is complete: "are you sure?"
  for (int p = 0; p < 4; ++p) {
    uint8_t* pl = player(self, p);
    if (field(pl, game::kSelState) != 1) continue;
    int& cursor = field(pl, game::kSelCursor);
    // A place holds a turtle and its secret partner (Up/Down: Leo/Slashuur, Raph/Casey, Mikey/Karai,
    // Don/Splinter); a pick takes the place, so the partner of a turtle someone has is out too.
    if (cursor < 0 || cursor > 3 || !locked(self, cursor)) continue;
    if (field(pl, game::kSelPicked) >= field(pl, game::kSelMax)) continue;  // done picking: may rest there
    // On a turtle somebody has: on to the next free one, the way the cursor last went.
    int dir = field(pl, game::kSelAnimDir) == 2 ? 3 : 1;
    int secret = pl[game::kSelSecret] ? 4 : 0;
    for (int k = 1; k < 4; ++k) {
      int c = (cursor + dir * k) % 4;
      if (enabled(self, c + secret) && !locked(self, c)) {
        cursor = c;
        break;
      }
    }
  }
}

// --- the team's leash ---------------------------------------------------------------------------
// The game keeps every player inside a circle around the stage camera's look-at point, the middle of
// the team: one screen has to hold everybody. In a network game every player has a screen and a camera
// of their own, and the circle only drags the players to each other "like a magnet": whoever walks away
// pulls the others along and is pulled back. In a session there is no circle (a part of the game's
// logic: the same on every PC); in a game on one screen it stays.
using MoveBoundary_t = bool(__cdecl*)(float* out, const float* in);
MoveBoundary_t oMoveBoundary = nullptr;

bool __cdecl hkMoveBoundary(float* out, const float* in) {
  if (!netplay::active()) return oMoveBoundary(out, in);
  if (out != in) std::memcpy(out, in, 3 * sizeof(float));
  return false;
}

// --- area map: a turtle icon per player (CAreaWorkPool::KameIconDisp) ------------------------
Method_t oKameIcon = nullptr;

void __fastcall hkKameIcon(uint8_t* self, void* edx) {
  if (g_players > 1 && patch::read<int>(reinterpret_cast<uintptr_t>(self) + game::kAreaCleared) == 0) {
    float* pos = reinterpret_cast<float*>(self + game::kAreaKamePos);
    uint8_t& anim = self[game::kAreaAnimation];
    float x = pos[0], y = pos[1];
    uint8_t a = anim;
    // The others first, half behind the first player's icon; no arrows for them.
    static const float kOffset[3][2] = {{-26, 6}, {26, 6}, {0, 16}};
    for (int k = g_players - 2; k >= 0; --k) {
      pos[0] = x + kOffset[k][0];
      pos[1] = y + kOffset[k][1];
      anim = game::kAreaAnimKameMove;
      oKameIcon(self, edx);
    }
    pos[0] = x;
    pos[1] = y;
    anim = a;
  }
  oKameIcon(self, edx);
}

// --- names in place of the player tags ------------------------------------------------------
// PlayerMarkerDisp draws the "P1".."P4" tag as one RwIm2D quad (off screen, then an arrow), inside
// the stage camera's scene, before the pause menu and the message windows. For a player with a
// name, while it runs the engine's Im2DRenderPrimitive points at tag_quad(), which draws the name
// in the tag's place and colour instead of the first quad. The arrow is drawn as before.
using MarkerDisp_t = void(__fastcall*)(void* self, void* edx, int player);
MarkerDisp_t oMarkerDisp = nullptr;
using Im2DRender_t = int(__cdecl*)(int prim, void* vertices, int count);
using RenderState_t = int(__cdecl*)(int state, void* value);

int g_tag_player = -1;          // whose tag PlayerMarkerDisp is drawing
bool g_tag_done = false;        // its quad is replaced
std::string g_tag_name;
Im2DRender_t g_im2d = nullptr;  // the engine's own
std::vector<std::string> g_test_names;  // [debug] tag_names

std::wstring wide_name(const std::string& s) {
  std::wstring w;
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  w.resize(len > 0 ? len - 1 : 0);
  if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
  return w;
}

// The game font switches to the 2D camera and back; the gauge drawn after the tags keeps the
// render states it set (raster, address, filter, blend, fog...).
void draw_tag_name(float cx, float bottom, uint32_t argb) {
  int fw = 0, fh = 0;
  auto* engine = patch::read<uint8_t*>(game::kEngineInstance);
  if (!engine || !video::frame_size(fw, fh)) return;
  static const int kStates[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 20, 30, 31};
  uintptr_t saved[sizeof(kStates) / sizeof(kStates[0])] = {};
  auto get = patch::read<RenderState_t>(reinterpret_cast<uintptr_t>(engine) + 0x24);
  auto set = patch::read<RenderState_t>(reinterpret_cast<uintptr_t>(engine) + 0x20);
  for (size_t i = 0; i < sizeof(kStates) / sizeof(kStates[0]); ++i) get(kStates[i], &saved[i]);

  std::wstring w = gamefont::name(wide_name(g_tag_name));
  if (w.size() > 20) w.resize(20);
  auto* vs = reinterpret_cast<const float*>(game::kVirtualScreen);  // the 2D space over the frame
  const float h = 13.0f;
  float tw = gamefont::width(w, h);
  // Centred where the tag was, its bottom on the tag's bottom; kept inside the frame (a tag at the
  // edge stands for a player off screen).
  float x = vs[0] + cx / fw * vs[2] - tw / 2;
  float y = vs[1] + bottom / fh * vs[3] - h;
  float lo = vs[0] + 2, hi = vs[0] + vs[2] - 2 - tw;
  x = x < lo ? lo : x > hi ? hi : x;
  y = y < vs[1] ? vs[1] : y;
  gamefont::begin();
  gamefont::color(0, 0, 0, 200);  // a shadow, then the name in the turtle's colour
  gamefont::show(w, x + 1, y + 1, h);
  gamefont::color((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
  gamefont::show(w, x, y, h);
  gamefont::end();

  for (size_t i = 0; i < sizeof(kStates) / sizeof(kStates[0]); ++i)
    set(kStates[i], reinterpret_cast<void*>(saved[i]));
}

int __cdecl tag_quad(int prim, void* vertices, int count) {
  if (g_tag_done || g_tag_player < 0 || count != 4) return g_im2d(prim, vertices, count);  // the arrow
  g_tag_done = true;
  const auto* v = static_cast<const uint8_t*>(vertices);
  float x0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (int i = 0; i < 4; ++i) {
    const auto* xy = reinterpret_cast<const float*>(v + i * game::kIm2DVertexSize);
    x0 = xy[0] < x0 ? xy[0] : x0;
    x1 = xy[0] > x1 ? xy[0] : x1;
    y1 = xy[1] > y1 ? xy[1] : y1;
  }
  draw_tag_name((x0 + x1) / 2, y1, *reinterpret_cast<const uint32_t*>(v + game::kIm2DVertexColor));
  return 1;
}

std::string tag_name(int n);

// Character select: each player's name in the lower, empty part of their panel. Panel i is the game's
// controller i, the i-th player who pressed Start: in a network game the name of the player whose pad
// that controller is (as over the heroes in a stage), and no name under a panel nobody has taken yet.
void draw_select_names() {
  if (!g_select_seen) return;
  g_select_seen = false;
  if (!gamefont::ready()) return;
  static const float kPanelX[4] = {-211, -71, 67, 209};  // panel centres, 2D space (measured)
  const float y = 78.0f, kMaxW = 128.0f;  // a panel's inner width
  bool begun = false;
  for (int i = 0; i < 4; ++i) {
    std::string name = netplay::active() ? (g_select_ctrl[i] >= 0 ? tag_name(g_select_ctrl[i]) : std::string())
                                         : i < (int)g_test_names.size() ? g_test_names[i] : std::string();
    if (name.empty()) continue;
    std::wstring w = gamefont::name(wide_name(name));
    if (w.empty()) w = gamefont::name(L"Р" + std::to_wstring(i + 1));  // nothing of it in the game's font
    if (w.size() > 20) w.resize(20);
    // A long name gets smaller letters (down to 10) to fit its panel; only then is it cut.
    float h = 14.0f, tw = gamefont::width(w, h);
    if (tw > kMaxW) {
      h = std::max(10.0f, h * kMaxW / tw);
      while (w.size() > 1 && gamefont::width(w, h) > kMaxW) w.pop_back();
    }
    if (!begun) gamefont::begin(), begun = true;
    float x = kPanelX[i] - gamefont::width(w, h) / 2;
    // Under the panel's rows of picks (one row in a game of four; two each in a game of two).
    float ny = y + 29.0f * (float)((g_costumes ? g_look[i].rows : 1) - 1);
    gamefont::color(0, 0, 0, 220);
    gamefont::show(w, x + 1, ny + 1 + (14.0f - h), h);
    gamefont::color(255, 255, 255);
    gamefont::show(w, x, ny + (14.0f - h), h);
  }
  if (begun) gamefont::end();
  if (!g_costumes) return;
  // The look chosen with up / down, under the player's name: marks up and down beside its name.
  pageui::Ui ui;
  for (int i = 0; i < 4; ++i) {
    const SelectLook& l = g_look[i];
    if (!l.shown) continue;
    static const struct {
      const char* ru;
      const char* en;
      uint32_t rgb;
    } kLooks[4] = {{u8"ОБЫЧНЫЙ", "ORDINARY", 0xFFFFFF},
                   {u8"САМУРАЙ", "SAMURAI", 0xFFC040},
                   {u8"НЕКСУС", "NEXUS", 0x70D8FF},
                   {u8"ДРУГОЙ ГЕРОЙ", "THE PARTNER", 0xC8FF70}};
    // Between the game's costume icons (on the portrait's foot) and the hero's name: free in every mode
    // (lower down are the picks, four rows of them when one player takes the whole team).
    const float cx = kPanelX[i], cy = -3.0f, h = 11.5f;
    std::wstring name = gamefont::say(kLooks[l.look].ru, kLooks[l.look].en);
    float half = pageui::width(name, h) / 2;
    ui.rect(cx - 63, cy - 11, cx + 63, cy + 11, 0xB4000000);
    ui.text(gamefont::say(u8"КОСТЮМ", "COSTUME"), cx, cy - 9, 8.5f, pageui::kDim, pageui::Align::Center);
    ui.text(name, cx, cy, h, kLooks[l.look].rgb, pageui::Align::Center);
    const float ax = cx - half - 9, bx = cx + half + 9, my = cy + pageui::kCap * h / 2;
    ui.tri(ax - 4, my + 3, ax + 4, my + 3, ax, my - 4, 0xFF9EDC3C);  // up
    ui.tri(bx - 4, my - 3, bx + 4, my - 3, bx, my + 4, 0xFF9EDC3C);  // down
  }
  ui.draw();
}

std::string tag_name(int n) {
  if (netplay::active()) {
    int port = (int)(int16_t)reinterpret_cast<int(__cdecl*)(int)>(game::kGetPhysicalPort)(n);
    return netplay::player_name(port);
  }
  return n < (int)g_test_names.size() ? g_test_names[n] : std::string();
}

void __fastcall hkMarkerDisp(void* self, void* edx, int player) {
  int n = player & 0xFF;  // the game passes the number in a byte (movzx bl); the rest is whatever was there
  auto* engine = patch::read<uint8_t*>(game::kEngineInstance);
  std::string name = n < 4 && engine && gamefont::ready() ? tag_name(n) : std::string();
  if (name.empty() || gamefont::name(wide_name(name)).empty()) return oMarkerDisp(self, edx, player);  // P1..P4
  auto& slot = *reinterpret_cast<Im2DRender_t*>(engine + game::kIm2DRenderPrimitive);
  g_im2d = slot;
  slot = &tag_quad;
  g_tag_player = n;
  g_tag_done = false;
  g_tag_name = name;
  oMarkerDisp(self, edx, player);
  slot = g_im2d;
  g_tag_player = -1;
}

// --- the dance after an area: the whole team (CEnbuProc, re/notes.md §13) ---------------------
// The game builds one dancer, the MVP, in a static container. The rest of the team get
// containers of their own (the game's Initialize with the global swapped), their own
// Enbu_<chara>.lpac, and the game's Settings / Period / Draw3D / Terminate after the MVP's.
int g_mvp_player = -1;  // the dance runs: the MVP's player (their name above the game's "MVP")

std::vector<uint8_t*> g_dancers;
bool g_merge_sets = false;  // every Enbu_<chara>.lpac makes a motion set "enbu": keep one
bool g_hush = false;        // the dancers' Period: the MVP alone shouts

uint8_t*& enbu() { return *reinterpret_cast<uint8_t**>(game::kEnbuContainer); }

// Behind the MVP, who stays where the camera looks and circles: left, right, and one
// between the MVP and the right one, far enough back to show past the MVP's shoulder.
const float kDancerPos[3][3] = {{-1.2f, 0.5f, -0.6f}, {1.2f, 0.5f, -0.6f}, {0.6f, 0.5f, -1.5f}};

// Enbu_<chara>.lpac's file id, picked the way CEnbuSequence::OnAttach does (0x4F15A8).
int enbu_file(int id, int costume) {
  static const int kOthers[4] = {0xE4, 0xE2, 0xE3, 0xE1};  // Slashuur, Casey, Karai, Splinter
  if (id >= 4) return kOthers[id - 4];
  if (costume == 1) return 0xE5 + id;  // samurai
  if (costume == 2) return 0xE9 + id;  // nexus
  return 0xDD + id;
}

using Attach_t = bool(__fastcall*)(void* self, void* edx, const void* param);
using Static_t = void(__cdecl*)();
using Thiscall1_t = int(__fastcall*)(void* self, void* edx, int arg);

int g_test_rank = 0;  // [debug] enbu_test

void set_rank(uint8_t* d, int rank) {
  field(d, game::kEnbuRank) = rank;
  field(d, game::kEnbuRankValue) = rank == 1 ? 2 : rank < 5 ? 1 : 0;
}

bool __fastcall hkEnbuAttach(void* self, void* edx, const void* param) {
  bool ok = reinterpret_cast<Attach_t>(game::kEnbuOnAttach)(self, edx, param);
  results::show();
  if (uint8_t* m = enbu()) g_mvp_player = results::player_of(field(m, game::kEnbuPlayer));
  g_dancers.clear();
  uint8_t* mvp = enbu();
  if (!mvp) return ok;
  if (g_test_rank) {
    set_rank(mvp, g_test_rank);
    field(mvp, 8) = g_test_rank;  // total rank
  }
  auto* result = reinterpret_cast<uint8_t*>(game::kPlayResult);
  for (int i = 0; i < 4 && g_dancers.size() < 3; ++i) {
    int id = field(result, game::kResultTeam + i * 4);
    if (id < 0 || id > 7 || id == field(mvp, game::kEnbuPlayer)) continue;
    enbu() = nullptr;
    reinterpret_cast<Static_t>(game::kEnbuInitialize)();  // a container for the MVP again
    uint8_t* d = enbu();
    if (!d) continue;
    int rank = g_test_rank ? g_test_rank : reinterpret_cast<Thiscall1_t>(game::kGetPersonalRank)(result, nullptr, i);
    int costume = reinterpret_cast<Thiscall1_t>(game::kGetCostume)(reinterpret_cast<void*>(game::kPlayParam), nullptr, i);
    field(d, game::kEnbuPlayer) = id;  // ... made this team member's
    field(d, game::kEnbuCostume) = costume;
    set_rank(d, rank);
    g_dancers.push_back(d);
    reinterpret_cast<void(__cdecl*)(int)>(game::kDataLoaderRegist)(enbu_file(id, costume));
  }
  enbu() = mvp;
  g_merge_sets = !g_dancers.empty();
  if (!g_dancers.empty()) LOGI("dance: MVP %d and %d more", field(mvp, game::kEnbuPlayer), (int)g_dancers.size());
  return ok;
}

Method_t oEnbuSettings = nullptr;

void __fastcall hkEnbuSettings(uint8_t* self, void* edx) {
  oEnbuSettings(self, edx);
  if (self != enbu()) return;
  for (size_t k = 0; k < g_dancers.size(); ++k) {
    oEnbuSettings(g_dancers[k], edx);
    auto* model = *reinterpret_cast<uint8_t**>(g_dancers[k] + game::kEnbuModel);
    if (!model) continue;
    std::memcpy(model + game::kModelPosition, kDancerPos[k], sizeof(kDancerPos[k]));
    reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(game::kModelUpdateFrame)(
        model, nullptr, *reinterpret_cast<void**>(model + game::kModelClump));
  }
}

Method_t oEnbuPeriod = nullptr;

void __fastcall hkEnbuPeriod(uint8_t* self, void* edx) {
  oEnbuPeriod(self, edx);
  if (self != enbu()) return;
  g_hush = true;
  for (uint8_t* d : g_dancers) oEnbuPeriod(d, edx);
  g_hush = false;
}

void __cdecl enbu_shout(int code) {  // Period's PlaySE call
  if (!g_hush) reinterpret_cast<void(__cdecl*)(int)>(game::kPlaySE)(code);
}

Static_t oEnbuDraw3D = nullptr;

void __cdecl hkEnbuDraw3D() {
  oEnbuDraw3D();
  if (g_dancers.empty()) return;
  uint8_t* mvp = enbu();
  uint32_t cull = patch::read<uint32_t>(game::kEnbuCullSave);  // what the MVP's draw saved
  for (uint8_t* d : g_dancers) {
    enbu() = d;
    oEnbuDraw3D();
  }
  enbu() = mvp;
  *reinterpret_cast<uint32_t*>(game::kEnbuCullSave) = cull;
}

Static_t oEnbuTerminate = nullptr;

void __cdecl hkEnbuTerminate() {
  results::hide();
  g_mvp_player = -1;
  uint8_t* mvp = enbu();
  for (uint8_t* d : g_dancers) {
    enbu() = d;
    oEnbuTerminate();
  }
  if (!g_dancers.empty()) LOGI("dance: over, %d dancers gone", (int)g_dancers.size());
  g_dancers.clear();
  g_merge_sets = false;
  enbu() = mvp;
  oEnbuTerminate();
}

using CreateSet_t = void(__cdecl*)(const char* name, const char* parent);
CreateSet_t oCreateMotionSet = nullptr;

void __cdecl hkCreateMotionSet(const char* name, const char* parent) {
  void* box = patch::read<void*>(game::kMotionContainer);
  if (g_merge_sets && box && name &&
      reinterpret_cast<void*(__fastcall*)(void*, void*, const char*)>(game::kFindMotionSet)(box, nullptr, name)) {
    reinterpret_cast<void(__cdecl*)(const char*)>(game::kSetCurrentMotionSet)(name);  // the motions go there
    return;
  }
  oCreateMotionSet(name, parent);
}

// --- in-stage transitions: the whole team jumps (CCameraChangeGimmick, re/notes.md §13) -------
// The game fades out, then puts everybody where the zone leads. Only the one who went into the
// zone jumped (if he did); now everyone standing, walking or running jumps as the fade begins.
int g_jump_test = 0;  // [debug] jump_test

int player_num() {
  auto* prop = patch::read<uint8_t*>(game::kGameProperty);
  if (!prop || !*reinterpret_cast<void**>(prop + game::kPropPlayers)) return 0;
  return reinterpret_cast<int(__cdecl*)()>(game::kGetPlayerNum)();
}

// Straight up, as if each had pressed jump on the spot: a run's speed could carry someone off an
// edge while the screen goes dark. Returns how many jumped.
int team_jump() {
  int n = player_num(), jumped = 0;
  for (int i = 0; i < n && i < 4; ++i) {
    void* gp = reinterpret_cast<void*(__cdecl*)(int)>(game::kGamePlayer)(i);
    if (!gp) continue;
    void** vt = *reinterpret_cast<void***>(gp);
    auto* chr = reinterpret_cast<uint8_t*(__fastcall*)(void*, void*)>(vt[game::kGamePlayerCurrentChr / 4])(gp, nullptr);
    if (!chr) continue;
    int st = reinterpret_cast<int(__fastcall*)(void*, void*)>(game::kChrGetStatus)(chr, nullptr);
    if (st < game::kStatusIdle || st > game::kStatusRun) continue;  // busy, hurt or in the air already
    if (reinterpret_cast<Thiscall1_t>(game::kChrChangeStatus)(chr, nullptr, game::kStatusJump) != game::kStatusJumpReady)
      continue;
    field(*reinterpret_cast<uint8_t**>(chr + game::kChrStateMachine), game::kSmJumpGround) = game::kStatusIdle;
    ++jumped;
  }
  return jumped;
}

Method_t oCameraChangeMove = nullptr;
float g_transition_test = 0;  // [debug] transition_test

void __fastcall hkCameraChangeMove(uint8_t* self, void* edx) {
  static bool test_done = false;
  if (g_transition_test > 0 && !test_done && input::script_time() >= g_transition_test &&
      field(self, game::kCameraChangeStep) == 0) {
    test_done = true;  // the first transition object of the stage starts, as if someone entered its zone
    self[game::kCameraChangeSender] = 0;
    field(self, game::kCameraChangeStep) = game::kCameraChangeFadeOut;
    LOGI("transition test: %.1f s", input::script_time());
  }
  if (field(self, game::kCameraChangeStep) == game::kCameraChangeFadeOut) {
    int n = player_num();
    if (n > 1) LOGI("transition: %d of %d players jump", team_jump(), n);
  }
  oCameraChangeMove(self, edx);
}

// [debug] jump_test: every N frames everyone on the ground jumps (called once a frame).
void jump_test_tick() {
  static uint32_t last = 0;
  uint32_t f = sim::frame();
  if (f == last || f < 600 || f % g_jump_test) return;
  last = f;
  int n = player_num();
  if (n > 0) LOGI("jump test: frame %u, %d of %d players jump", f, team_jump(), n);
}

// [debug] camera_zoom_max: the stage camera backs off less, so players can walk off screen.
using NiceZoom_t = float(__fastcall*)(void* self, void* edx, void* positions, int count);
NiceZoom_t oCalcNiceZoom = nullptr;
float g_zoom_max = 0;

float __fastcall hkCalcNiceZoom(void* self, void* edx, void* positions, int count) {
  float z = oCalcNiceZoom(self, edx, positions, count);
  return z > g_zoom_max ? g_zoom_max : z;
}

// Every costume can be picked at character select from the start.
using CostumeTaken_t = bool(__fastcall*)(void* self, void* edx, int cursor, int costume);
CostumeTaken_t oIsCostumeTaken = nullptr;

bool __fastcall hkIsCostumeTaken(void*, void*, int, int costume) { return costume == 1 || costume == 2; }

// [debug] map_area: the area map opens with its cursor on this area (test runs reach 0-2 or Home).
using SelectedArea_t = int(__fastcall*)(void* self, void* edx);
SelectedArea_t oSelectedArea = nullptr;
int g_map_area = 0;

int __fastcall hkSelectedArea(void* self, void* edx) {
  int v = oSelectedArea(self, edx);
  if (g_map_area > 0 && _ReturnAddress() == reinterpret_cast<void*>(game::kMapAttachReadsArea)) {
    LOGI("[debug] map_area: the map opens on area %d (was %d)", g_map_area, v);
    return g_map_area;
  }
  return v;
}

// [debug] all_characters: the secret characters count as unlocked.
using Unlocked_t = bool(__fastcall*)(void* self, void* edx, int id);
Unlocked_t oIsUnlocked = nullptr;

bool __fastcall hkIsUnlocked(void* self, void* edx, int id) {
  if (id >= game::kSecretFirstCharacter && id <= game::kSecretLastCharacter) return true;
  return oIsUnlocked(self, edx, id);
}

// [debug] enbu_test: whatever the title menu goes to, it calls the dance of Leo (MVP), Raph,
// Mike and Don instead.
Method_t oTitleBranch = nullptr;

void __fastcall hkTitleBranch(uint8_t* self, void* edx) {
  auto* team = reinterpret_cast<int*>(game::kPlayResult + game::kResultTeam);
  for (int i = 0; i < 4; ++i) team[i] = i;
  LOGI("dance test: title item %d -> the dance, rank %d", field(self, game::kTitleNext), g_test_rank);
  reinterpret_cast<void(__fastcall*)(void*, void*, int, const void*)>(game::kSequenceCall)(self, edx, game::kProcEnbu,
                                                                                           nullptr);
}

void install_dance() {
  const uint8_t shout_old[] = {0xE8, 0xA4, 0x9C, 0x02, 0x00};  // call PlaySE
  bool ok = patch::bytes_are(game::kEnbuSettings, game::kEnbuSettingsHead, sizeof(game::kEnbuSettingsHead)) &&
            patch::bytes_are(game::kEnbuPeriod, game::kEnbuPeriodHead, sizeof(game::kEnbuPeriodHead)) &&
            patch::bytes_are(game::kEnbuDraw3D, game::kEnbuDraw3DHead, sizeof(game::kEnbuDraw3DHead)) &&
            patch::bytes_are(game::kEnbuTerminate, game::kEnbuTerminateHead, sizeof(game::kEnbuTerminateHead)) &&
            patch::bytes_are(game::kCreateMotionSet, game::kCreateMotionSetHead, sizeof(game::kCreateMotionSetHead)) &&
            patch::bytes_are(game::kEnbuShout, shout_old, sizeof(shout_old)) &&
            patch::read<uint32_t>(game::kEnbuOnAttachSlot) == game::kEnbuOnAttach;
  if (!ok) {
    LOGW("dance: unexpected code, the MVP dances alone");
    return;
  }
  uint8_t shout_new[5] = {0xE8};
  int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&enbu_shout) - (game::kEnbuShout + 5));
  std::memcpy(shout_new + 1, &rel, 4);
  ok = patch::hook("dance: settings", game::kEnbuSettings, (void*)&hkEnbuSettings, (void**)&oEnbuSettings) &&
       patch::hook("dance: period", game::kEnbuPeriod, (void*)&hkEnbuPeriod, (void**)&oEnbuPeriod) &&
       patch::hook("dance: draw", game::kEnbuDraw3D, (void*)&hkEnbuDraw3D, (void**)&oEnbuDraw3D) &&
       patch::hook("dance: terminate", game::kEnbuTerminate, (void*)&hkEnbuTerminate, (void**)&oEnbuTerminate) &&
       patch::hook("dance: one motion set", game::kCreateMotionSet, (void*)&hkCreateMotionSet,
                   (void**)&oCreateMotionSet) &&
       patch::replace("dance: only the MVP shouts", game::kEnbuShout, shout_old, shout_new, sizeof(shout_new));
  // Last: without this no dancer is ever made, so a failed hook above leaves the game's dance.
  if (ok) patch::slot("dance: the whole team", game::kEnbuOnAttachSlot, game::kEnbuOnAttach, (void*)&hkEnbuAttach);
}

}  // namespace

// The dance after an area: the MVP's player name above the game's "MVP" (y 40) and the turtle's
// name (y 65), in the game's font, white with a shadow.
void draw_mvp_name() {
  if (g_mvp_player < 0 || !gamefont::ready()) return;
  std::wstring w = gamefont::name(wide_name(tag_name(g_mvp_player)));
  if (w.empty()) return;
  if (w.size() > 20) w.resize(20);
  float h = 16.0f, tw = gamefont::width(w, h);
  if (tw > 300.0f) h = std::max(10.0f, h * 300.0f / tw);
  float x = -gamefont::width(w, h) / 2, y = 38.0f - h - 4.0f;
  // It comes and goes with the picture: the screen fade (drawn by the game before this) dims it.
  int fade = 0;
  if (auto* fc = patch::read<uint8_t*>(game::kScreenFade)) fade = fc[game::kScreenFadeColor + 3];
  int alpha = 255 - fade;
  if (alpha <= 0) return;
  gamefont::begin();
  gamefont::color(0, 0, 0, (uint8_t)(alpha * 220 / 255));
  gamefont::show(w, x + 1, y + 1, h);
  gamefont::color(255, 255, 255, (uint8_t)alpha);
  gamefont::show(w, x, y, h);
  gamefont::end();
}

void after_frame() {
  results::tick();
  results::draw();
  draw_mvp_name();
  netplay::idle_tick();
  netplay::lobby_tick();
  if (g_jump_test) jump_test_tick();
  draw_select_names();
}

void install(const Config& cfg) {
  if (patch::bytes_are(game::kPlayerMarkerDisp, game::kPlayerMarkerDispHead, sizeof(game::kPlayerMarkerDispHead)))
    patch::hook("player tags: names instead of P1..P4", game::kPlayerMarkerDisp, (void*)&hkMarkerDisp,
                (void**)&oMarkerDisp);
  if (patch::bytes_are(game::kSelectMove, game::kSelectMoveHead, sizeof(game::kSelectMoveHead)))
    patch::hook("character select: cursor passes taken turtles", game::kSelectMove, (void*)&hkSelectMove,
                (void**)&oSelectMove);
  if (patch::bytes_are(game::kKameIconDisp, game::kKameIconDispHead, sizeof(game::kKameIconDispHead)))
    patch::hook("area map: a turtle per player", game::kKameIconDisp, (void*)&hkKameIcon, (void**)&oKameIcon);
  install_dance();
  if (patch::bytes_are(game::kMoveBoundary, game::kMoveBoundaryHead, sizeof(game::kMoveBoundaryHead)))
    patch::hook("network game: the players are not held together (no move boundary)", game::kMoveBoundary,
                (void*)&hkMoveBoundary, (void**)&oMoveBoundary);
  else
    LOGW("coop: move boundary not recognised: the players are held together as in the game");
  if (patch::bytes_are(game::kCameraChangeMove, game::kCameraChangeMoveHead, sizeof(game::kCameraChangeMoveHead)))
    patch::hook("in-stage transitions: the whole team jumps", game::kCameraChangeMove, (void*)&hkCameraChangeMove,
                (void**)&oCameraChangeMove);
  g_jump_test = cfg.jump_test;
  g_transition_test = cfg.transition_test;
  if (oSelectMove &&
      patch::bytes_are(game::kIsCostumeTaken, game::kIsCostumeTakenHead, sizeof(game::kIsCostumeTakenHead)) &&
      patch::bytes_are(game::kSelectNexusSecret, game::kSelectNexusSecretOld, sizeof(game::kSelectNexusSecretOld)) &&
      patch::bytes_are(game::kSelectNexusIconSecret, game::kSelectNexusIconSecretOld,
                       sizeof(game::kSelectNexusIconSecretOld))) {
    const uint8_t yes[5] = {0x59, 0xB0, 0x01, 0x90, 0x90};  // pop ecx (the secret id pushed for it); mov al, 1
    patch::replace("costumes: nexus without its secret (pick)", game::kSelectNexusSecret, game::kSelectNexusSecretOld, yes, 5);
    patch::replace("costumes: nexus without its secret (icon)", game::kSelectNexusIconSecret,
                   game::kSelectNexusIconSecretOld, yes, 5);
    g_costumes = patch::hook("costumes: all of them from the start, chosen with up / down", game::kIsCostumeTaken,
                             (void*)&hkIsCostumeTaken, (void**)&oIsCostumeTaken);
  } else {
    LOGW("coop: costume code not recognised, costumes as in the game (held shoulder buttons, once unlocked)");
  }
  g_map_area = cfg.map_area;
  if (cfg.map_area > 0 &&
      patch::bytes_are(game::kGetSelectedArea, game::kGetSelectedAreaHead, sizeof(game::kGetSelectedAreaHead)))
    patch::hook("[debug] map_area", game::kGetSelectedArea, (void*)&hkSelectedArea, (void**)&oSelectedArea);
  // Casey, Splinter, Karai and Slashuur can be picked from the start too (the game unlocks them one by
  // one): at character select they are the fourth look of their place (up / down). The same on every PC
  // of a network game, as the costumes.
  if (patch::bytes_are(game::kIsUnlockedSecret, game::kIsUnlockedSecretHead, sizeof(game::kIsUnlockedSecretHead)))
    patch::hook("characters: Casey, Splinter, Karai and Slashuur from the start", game::kIsUnlockedSecret,
                (void*)&hkIsUnlocked, (void**)&oIsUnlocked);
  if (cfg.camera_zoom_max > 0 &&
      patch::bytes_are(game::kCalcNiceZoom, game::kCalcNiceZoomHead, sizeof(game::kCalcNiceZoomHead))) {
    g_zoom_max = cfg.camera_zoom_max;
    patch::hook("[debug] camera_zoom_max", game::kCalcNiceZoom, (void*)&hkCalcNiceZoom, (void**)&oCalcNiceZoom);
  }
  for (size_t a = 0, b; a <= cfg.tag_names.size(); a = b + 1) {
    b = cfg.tag_names.find(',', a);
    if (b == std::string::npos) b = cfg.tag_names.size();
    if (b > a) g_test_names.push_back(cfg.tag_names.substr(a, b - a));
  }
  if (cfg.enbu_test && patch::bytes_are(game::kTitleBranch, game::kTitleBranchHead, sizeof(game::kTitleBranchHead))) {
    g_test_rank = cfg.enbu_test;
    patch::hook("[debug] enbu_test: the title menu = the dance", game::kTitleBranch, (void*)&hkTitleBranch,
                (void**)&oTitleBranch);
  }
}

int players() { return g_players; }

}  // namespace coop
