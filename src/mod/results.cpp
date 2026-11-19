#include "mod/results.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/game.h"
#include "mod/gamefont.h"
#include "mod/pageui.h"
#include "mod/netplay.h"
#include "mod/overlay.h"
#include "mod/patch.h"

namespace results {
namespace {

constexpr int kChars = 8;  // character ids 0..7 (retail order: Leo, Raph, Mikey, Don, Splinter, Casey, Karai, Slashuur)
const wchar_t* const kCharNames[kChars] = {L"Лео", L"Раф", L"Майки", L"Донни", L"Сплинтер", L"Кейси", L"Караи", L"Слашур"};

// By character (in co-op each player has a turtle of their own).
struct Stats {
  int hits = 0, blocks = 0, taken = 0;
};
Stats g_char[kChars];
// By player number.
int g_down[4] = {}, g_woke[4] = {};
int g_played[4] = {-1, -1, -1, -1};  // the turtle each player played in the area (the stage is gone at the dance)
int g_shown_played[4] = {-1, -1, -1, -1};  // the same for the area whose dance runs
bool g_shown = false;
DWORD g_hide_at = 0;  // GetTickCount when the table goes
// The table on screen: a row per player.
struct TableRow {
  std::wstring name;  // as the others see the player
  int chr = -1;
  Stats s;
  int down = 0, woke = 0;
};
std::vector<TableRow> g_table;
bool g_table_on = false;

// CGamePlayResult::AddTechnicalAction(character, action): the game's record of what a turtle did.
using AddAction_t = void(__fastcall*)(void* self, void* edx, int chr, int act);
AddAction_t oAddAction = nullptr;

void __fastcall hkAddAction(void* self, void* edx, int chr, int act) {
  static int logged = 0;
  if (logged < 12) {
    ++logged;
    LOGD("results: action %d by character %d", act, chr);
  }
  if (chr >= 0 && chr < kChars) {
    if (act == game::kTecActGuardImpact)
      ++g_char[chr].blocks;
    else if (act == game::kTecActDamaged)
      ++g_char[chr].taken;
    else if (act >= game::kTecActFirstAttack && act <= game::kTecActLastAttack && act != game::kTecActDash)
      ++g_char[chr].hits;
  }
  oAddAction(self, edx, chr, act);
}

void reset() {
  for (Stats& s : g_char) s = Stats{};
  for (int i = 0; i < 4; ++i) {
    g_down[i] = g_woke[i] = 0;
    g_played[i] = -1;
  }
}

std::wstring wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

}  // namespace

void note(int player, int character) {
  if (player >= 0 && player < 4 && character >= 0 && character < kChars) g_played[player] = character;
}

void down(int player) {
  if (player >= 0 && player < 4) ++g_down[player];
}

void woke(int reviver, int player) {
  (void)player;
  if (reviver >= 0 && reviver < 4) ++g_woke[reviver];
}

void show() {
  if (g_shown) return;  // the dance's extra dancers attach too
  for (int n = 0; n < 4; ++n) g_shown_played[n] = g_played[n];
  int count = 0;
  for (int n = 0; n < 4; ++n)
    if (g_played[n] >= 0) count = n + 1;
  if (count < 2) {
    reset();
    return;
  }
  g_table.clear();
  for (int n = 0; n < count; ++n) {
    int chr = g_played[n];
    std::wstring name;
    if (netplay::active()) {
      int slot = (int)(int16_t)reinterpret_cast<int(__cdecl*)(int)>(game::kGetPhysicalPort)(n);
      name = wide(netplay::player_name(slot));
    }
    if (name.empty()) name = L"P" + std::to_wstring(n + 1);
    Stats s = chr >= 0 && chr < kChars ? g_char[chr] : Stats{};
    g_table.push_back(TableRow{name, chr, s, g_down[n], g_woke[n]});
    LOGI("results: P%d (character %d): hits %d, blocks %d, taken %d, down %d, woke %d", n + 1, chr, s.hits,
         s.blocks, s.taken, g_down[n], g_woke[n]);
  }
  g_table_on = true;
  g_shown = true;
  g_hide_at = 0;
  reset();
}

int player_of(int character) {
  for (int n = 0; n < 4; ++n)
    if (g_shown_played[n] == character && character >= 0) return n;
  return -1;
}

void hide() {
  if (g_shown) g_hide_at = GetTickCount() + 6000;
  g_shown = false;
}

void tick() {
  if (g_hide_at && (int)(GetTickCount() - g_hide_at) >= 0) {
    g_hide_at = 0;
    g_table_on = false;
  }
}

// The table at the top of the frame, in the game's lettering and tmnt2net's window colours.
void draw() {
  if (!g_table_on || g_table.empty() || !gamefont::ready()) return;
  using pageui::Align;
  pageui::Ui ui;
  auto T = [](const char* ru, const char* en) { return gamefont::say(ru, en); };
  const float x0 = -286, x1 = 286, y0 = -198, band = 24, step = 19;
  const float y1 = y0 + band + 22 + step * (float)g_table.size() + 6;
  ui.rect(x0 + 4, y0 + 5, x1 + 4, y1 + 6, 0x60000000);
  ui.grad_v(x0, y0, x1, y1, 0xE00E1A12, 0xE0050907);
  ui.grad_h(x0, y0, x1, y0 + band, 0xF0081008, 0xF04A7A1E);
  ui.rect(x0, y0 - 2, x1, y0, 0xFF9EDC3C);
  ui.rect(x0, y1, x1, y1 + 2, 0xFF9EDC3C);
  ui.text(T(u8"ИТОГИ РАЙОНА", "AREA RESULTS"), 0, y0 + (band - pageui::kCap * 17) / 2, 17, pageui::kTitle, Align::Center);
  static const struct {
    const char* ru;
    const char* en;
    float x;
  } kCols[5] = {{u8"УДАРЫ", "HITS", -28}, {u8"БЛОКИ", "BLOCKS", 38}, {u8"ПОЛУЧИЛ", "TAKEN", 112}, {u8"ЛЕЖАЛ", "DOWN", 186},
                {u8"ПОДНЯЛ", "REVIVED", 252}};
  float y = y0 + band + 7;
  ui.text(T(u8"ИГРОК", "PLAYER"), x0 + 14, y, 11.5f, pageui::kDim);
  for (const auto& c : kCols) ui.text(T(c.ru, c.en), c.x, y, 11.5f, pageui::kDim, Align::Center);
  y += 16;
  int best_hits = 0;
  for (const TableRow& r : g_table) best_hits = std::max(best_hits, r.s.hits);
  for (size_t i = 0; i < g_table.size(); ++i) {
    const TableRow& r = g_table[i];
    if (i % 2 == 0) ui.rect(x0 + 6, y - 4, x1 - 6, y + 14, 0x30204818);
    std::wstring n = gamefont::name(r.name);
    if (n.empty()) n = T(u8"ИГРОК ", "PLAYER ") + std::to_wstring(i + 1);
    std::wstring who = r.chr >= 0 && r.chr < kChars ? L"  (" + gamefont::say(kCharNames[r.chr]) + L")" : L"";
    while (n.size() > 3 && pageui::width(n + who, 14.5f) > 196) n.pop_back();  // (a long name: its start)
    ui.text(n + who, x0 + 14, y, 14.5f, pageui::kText);
    const int values[5] = {r.s.hits, r.s.blocks, r.s.taken, r.down, r.woke};
    for (int c = 0; c < 5; ++c) {
      bool best = c == 0 && values[c] == best_hits && best_hits > 0;
      ui.text(std::to_wstring(values[c]), kCols[c].x, y, 14.5f, best ? pageui::kGood : values[c] ? pageui::kText : pageui::kOff,
              Align::Center);
    }
    y += step;
  }
  ui.draw();
}

void install(const Config& cfg) {
  (void)cfg;
  if (!patch::bytes_are(game::kAddTechAction, game::kAddTechActionHead, sizeof(game::kAddTechActionHead)) ||
      !patch::hook("results: the game's record of each turtle's actions", game::kAddTechAction, (void*)&hkAddAction,
                   (void**)&oAddAction))
    LOGW("results: action record not recognised, no area results");
}

}  // namespace results
