#include "mod/health.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "common/log.h"
#include "mod/game.h"
#include "mod/input.h"
#include "mod/netplay.h"
#include "mod/patch.h"
#include "mod/results.h"
#include "mod/sim.h"
#include "mod/widescreen.h"

namespace health {
namespace {

constexpr float kDownSeconds = 10.0f;
enum Down { kUp = 0, kFalling, kAsleep };  // kFalling: down, not asleep yet (in the air, hit...)

int g_hp[4] = {-1, -1, -1, -1};  // own health; -1 = not split from the team's yet
uint32_t g_stage_frame = 0;      // [debug] tests: when the current stage's players were set up
int g_down[4] = {};
// A teammate standing by an asleep player and guarding (block held) wakes them: frames so far.
constexpr int kReviveFrames = 90;          // 1.5 s
constexpr float kReviveDistance = 2.2f;    // world units, on the ground plane
int g_revive[4] = {};

template <typename T>
T& at(void* p, int off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off);
}
template <typename R, typename... A>
R vcall(void* obj, int off, A... a) {
  void** vt = *reinterpret_cast<void***>(obj);
  return reinterpret_cast<R(__fastcall*)(void*, void*, A...)>(vt[off / 4])(obj, nullptr, a...);
}

int& shared_hp() { return *reinterpret_cast<int*>(game::kSharedHP); }
int shared_max() { return patch::read<int>(game::kSharedHPMax); }
int revive_hp() { return shared_max() / 3 > 0 ? shared_max() / 3 : 1; }

int player_num() {
  auto* prop = patch::read<uint8_t*>(game::kGameProperty);
  if (!prop || !*reinterpret_cast<void**>(prop + game::kPropPlayers)) return 0;
  return reinterpret_cast<int(__cdecl*)()>(game::kGetPlayerNum)();
}
bool split() { return player_num() > 1; }
void* game_player(int n) { return reinterpret_cast<void*(__cdecl*)(int)>(game::kGamePlayer)(n); }
int number(void* gp) { return at<int>(gp, game::kGamePlayerNo); }

// The first time after a stage start: everybody gets what the team had.
void split_now() {
  int hp = shared_hp();
  for (int& h : g_hp)
    if (h < 0) h = hp;
}

// --- CGamePlayer: health per player ---------------------------------------------------------
using GetHP_t = int(__fastcall*)(void* gp, void* edx);
using AddHP_t = int(__fastcall*)(void* gp, void* edx, int delta);
using Context_t = void(__fastcall*)(void* gp, void* edx, uint8_t* context);

int __fastcall hkGetHP(void* gp, void* edx) {
  int n = number(gp);
  if (!split() || n < 0 || n > 3) return reinterpret_cast<GetHP_t>(game::kGpGetHPFn)(gp, edx);
  if (!vcall<bool>(gp, game::kGpIsAlive)) return 0;
  split_now();
  return g_hp[n];
}

// The game's AddHP runs on this player's health put in the team's place: its invincibility,
// damage count, clamping and comeback item stay the game's. At zero it calls player_down().
int __fastcall hkAddHP(void* gp, void* edx, int delta) {
  auto orig = reinterpret_cast<AddHP_t>(game::kGpAddHPFn);
  int n = number(gp);
  if (!split() || n < 0 || n > 3) return orig(gp, edx, delta);
  split_now();
  if (g_down[n]) return g_hp[n];  // asleep: no hits, no pizza
  int& shared = shared_hp();
  int team = shared;
  shared = g_hp[n];
  int result = orig(gp, edx, delta);
  g_hp[n] = shared;
  shared = team;
  return result;
}

void __fastcall hkSaveContext(void* gp, void* edx, uint8_t* context) {
  reinterpret_cast<Context_t>(game::kGpSaveContextFn)(gp, edx, context);
  int n = number(gp);
  if (!split() || n < 0 || n > 3) return;
  split_now();
  at<int>(context, game::kContextHP) = g_down[n] ? revive_hp() : g_hp[n];  // the next stage: up
}

void __fastcall hkLoadContext(void* gp, void* edx, uint8_t* context) {
  reinterpret_cast<Context_t>(game::kGpLoadContextFn)(gp, edx, context);
  int n = number(gp);
  if (n < 0 || n > 3) return;
  g_hp[n] = at<int>(context, game::kContextHP);
  g_down[n] = kUp;
  g_revive[n] = 0;
  LOGI("health: P%d starts the stage with %d", n + 1, g_hp[n]);
}

using Setup_t = void(__fastcall*)(void* gp, void* edx, int n, void* player);
Setup_t oSetup = nullptr;

void __fastcall hkSetup(void* gp, void* edx, int n, void* player) {
  oSetup(gp, edx, n, player);
  if (n == 0) {  // a stage's players are being made: the game zeroed the team's health
    LOGI("stage: players set up (frame %u)", sim::frame());
    g_stage_frame = sim::frame();
    for (int i = 0; i < 4; ++i) {
      g_hp[i] = -1;
      g_down[i] = kUp;
      g_revive[i] = 0;
    }
  }
}

// --- down and up again ------------------------------------------------------------------------
void game_over(int n) {
  void* stage = reinterpret_cast<void*(__cdecl*)()>(game::kGetCurrentStage)();
  if (stage) vcall<void>(stage, game::kStageNotifyPlayerDead, n);
}

// AddHP brought player n to zero (in place of the game's NotifyPlayerDead call).
void __stdcall player_down(int n) {
  int count = player_num();
  if (count < 2 || n < 0 || n > 3) return game_over(n);
  g_down[n] = kFalling;
  int down = 0;
  for (int i = 0; i < count && i < 4; ++i) down += g_down[i] ? 1 : 0;
  if (down >= count) {
    LOGI("health: player %d is down and so is everybody: game over", n + 1);
    return game_over(n);
  }
  results::down(n);
  LOGI("health: player %d is down", n + 1);
}

// A teammate who is up stands by player n and guards (block held: GUARD_READY..GUARD_IMPACT).
struct V3 {
  float x, y, z;
};
int reviver_near(int n, void* gp) {
  const V3* me = vcall<const V3*>(gp, game::kGpGetPosition);
  if (!me) return -1;
  int count = player_num();
  for (int m = 0; m < count && m < 4; ++m) {
    if (m == n || g_down[m]) continue;
    void* other = game_player(m);
    if (!other || !vcall<bool>(other, game::kGpIsAlive)) continue;
    auto* chr = vcall<uint8_t*>(other, game::kGamePlayerCurrentChr);
    const V3* pos = vcall<const V3*>(other, game::kGpGetPosition);
    if (!chr || !pos) continue;
    int st = reinterpret_cast<int(__fastcall*)(void*, void*)>(game::kChrGetStatus)(chr, nullptr);
    if (st < game::kStatusGuardReady || st > game::kStatusGuardImpact) continue;
    float dx = pos->x - me->x, dz = pos->z - me->z;
    if (dx * dx + dz * dz <= kReviveDistance * kReviveDistance) return m;
  }
  return -1;
}

// Every frame for a player who is down: invincible, asleep, then up with a third of the maximum
// once the sleep ends (its 10 s, a teammate's hit, or a teammate guarding by them for 1.5 s).
void down_tick(int n, void* gp) {
  auto* chr = vcall<uint8_t*>(gp, game::kGamePlayerCurrentChr);
  if (!chr) return;
  at<uint32_t>(chr, game::kChrAttributes) |= game::kAttrInvincible;
  int status = reinterpret_cast<int(__fastcall*)(void*, void*)>(game::kChrGetStatus)(chr, nullptr);
  if (g_down[n] == kFalling) {
    if (status != game::kStatusSleep) {
      at<float>(chr, game::kChrTroubleTime) = kDownSeconds;  // how long the sleep lasts
      status = reinterpret_cast<int(__fastcall*)(void*, void*, int)>(game::kChrChangeStatus)(chr, nullptr,
                                                                                            game::kStatusSleep);
    }
    if (status == game::kStatusSleep) g_down[n] = kAsleep;  // else: not now (in the air...), next frame
    return;
  }
  if (status == game::kStatusSleep) {
    int reviver = reviver_near(n, gp);
    g_revive[n] = reviver >= 0 ? g_revive[n] + 1 : 0;
    if (g_revive[n] == kReviveFrames) {  // the sleep's end time to now: up as after its time
      uint8_t* sm = at<uint8_t*>(chr, game::kChrStateMachine);
      uint8_t* cur = sm ? at<uint8_t*>(sm, game::kSmCurrentStatus) : nullptr;
      if (cur && at<int>(cur, game::kStatusObjId) == game::kStatusSleep) {
        at<float>(cur, game::kStatusDamageEndTime) = 0.0f;
        results::woke(reviver, n);
        LOGI("health: player %d woken by player %d", n + 1, reviver + 1);
      }
    }
    return;
  }
  g_down[n] = kUp;
  g_revive[n] = 0;
  g_hp[n] = revive_hp();
  at<uint32_t>(chr, game::kChrAttributes) &= ~game::kAttrInvincible;
  LOGI("health: player %d is up again", n + 1);
}

using Run_t = void(__fastcall*)(void* player, void* edx);

// [debug] area_test: every area the game starts is this one.
using StartArea_t = void(__fastcall*)(void* param, void* edx, int area, int index);
StartArea_t oStartArea = nullptr;
int g_area_test = 0;

void __fastcall hkStartArea(void* param, void* edx, int area, int index) {
  LOGI("area test: area %d -> %d", area, g_area_test);
  oStartArea(param, edx, g_area_test, index);
}
float g_test = 0;  // [debug] health_test

// [debug] health_test: at that second of the script clock the second player loses all their
// health and the first a quarter.
void health_test() {  // once in every stage
  static uint32_t done = 0;
  if (done == g_stage_frame || input::script_time() < g_test || player_num() < 2 || sim::frame() < g_stage_frame + 120)
    return;
  done = g_stage_frame;
  int max = shared_max();
  LOGI("health test: P2 %d, P1 %d", vcall<int>(game_player(1), game::kGpAddHP, -max),
       vcall<int>(game_player(0), game::kGpAddHP, -max / 4));
}

float g_all_test = 0;    // [debug] health_all_test
float g_clear_test = 0;  // [debug] clear_test
int g_clear_stages = 0, g_cleared = 0;  // [debug] clear_test_stages, stages cleared so far

// [debug] clear_test: at that second the stage counts as cleared, as at its goal.
void clear_test() {  // once in every stage
  static uint32_t done = 0;
  if (done == g_stage_frame || input::script_time() < g_clear_test || player_num() < 1 ||
      sim::frame() < g_stage_frame + 240 || (g_clear_stages > 0 && g_cleared >= g_clear_stages))
    return;
  done = g_stage_frame;
  ++g_cleared;
  void* stage = reinterpret_cast<void*(__cdecl*)()>(game::kGetCurrentStage)();
  LOGI("clear test: stage %p cleared", stage);
  if (stage) vcall<void>(stage, game::kStageNotifyGameClear, 0);
}

// [debug] health_all_test: at that second every player loses all their health, the last first.
void health_all_test() {
  static bool done = false;
  if (done || input::script_time() < g_all_test || player_num() < 2) return;
  done = true;
  int max = shared_max();
  for (int n = player_num() - 1; n >= 0; --n)
    LOGI("health test: P%d %d", n + 1, vcall<int>(game_player(n), game::kGpAddHP, -max));
}

void __fastcall hkPlayerRun(void* player, void* edx) {
  for (int n = 0, count = player_num(); n < count && n < 4; ++n)
    if (void* gp = game_player(n)) results::note(n, vcall<int>(gp, game::kGpGetCurrentCharacterId));
  if (g_test > 0) health_test();
  if (g_all_test > 0) health_all_test();
  if (g_clear_test > 0) clear_test();
  int count = player_num();
  for (int n = 0; count > 1 && n < count && n < 4; ++n) {
    if (!g_down[n]) continue;
    void* gp = game_player(n);
    if (gp && vcall<void*>(gp, game::kGpGetPlayer) == player) down_tick(n, gp);
  }
  reinterpret_cast<Run_t>(game::kPlayerRun)(player, edx);
}

// --- HUD ----------------------------------------------------------------------------------------
// The health ring: this PC's player in a network game, else the first player.
int display_player() {
  if (!netplay::active()) return 0;
  int mine = netplay::my_player(), count = player_num();
  for (int n = 0; n < count && n < 4; ++n)
    if ((int)(int16_t)reinterpret_cast<int(__cdecl*)(int)>(game::kGetPhysicalPort)(n) == mine) return n;
  return 0;
}

void* __cdecl meter_player(int) { return game_player(display_player()); }

// A bar with the player's health under their box at the top right.
struct Im2DVertex {
  float x, y, z, rhw;
  uint32_t argb;
  float u, v;
};
static_assert(sizeof(Im2DVertex) == game::kIm2DVertexSize, "RwIm2DVertex");
using Im2DRender_t = int(__cdecl*)(int prim, void* vertices, int count);
using RenderState_t = int(__cdecl*)(int state, void* value);
constexpr float kBarTop = 56, kBarHeight = 4, kBarInset = 6;  // 2D space, from the box's top / sides

void quad(Im2DRender_t render, float x0, float y0, float x1, float y1, uint32_t argb) {
  Im2DVertex v[4] = {{x0, y0, 0, 1, argb, 0, 0},
                     {x0, y1, 0, 1, argb, 0, 0},
                     {x1, y0, 0, 1, argb, 0, 0},
                     {x1, y1, 0, 1, argb, 0, 0}};
  render(4, v, 4);  // rwPRIMTYPETRISTRIP
}

void draw_bar(int n, float left, float right, float top) {
  auto* engine = patch::read<uint8_t*>(game::kEngineInstance);
  int w = patch::read<int>(game::kScreenStaticWidth), h = patch::read<int>(game::kScreenStaticHeight);
  if (!engine || w <= 0 || h <= 0) return;
  split_now();
  int max = shared_max();
  float f = max > 0 ? (float)g_hp[n] / (float)max : 0.0f;
  f = f < 0 ? 0 : f > 1 ? 1 : f;
  uint32_t fill = g_down[n] ? 0xFF808080u : f > 0.5f ? 0xFF40D040u : f > 0.25f ? 0xFFE0C020u : 0xFFE03020u;
  if (g_down[n] && g_revive[n] > 0) {  // being woken: the bar fills light blue
    fill = 0xFF60C0FFu;
    f = (float)g_revive[n] / (float)kReviveFrames;
  }

  const auto* vs = reinterpret_cast<const float*>(game::kVirtualScreen);  // sprites' 2D space
  auto px = [&](float x) { return (x - vs[0]) / vs[2] * (float)w; };
  auto py = [&](float y) { return (y - vs[1]) / vs[3] * (float)h; };
  float x0 = px(left + kBarInset), x1 = px(right - kBarInset);
  float y0 = py(top + kBarTop), y1 = py(top + kBarTop + kBarHeight);

  auto get = patch::read<RenderState_t>(reinterpret_cast<uintptr_t>(engine) + 0x24);
  auto set = patch::read<RenderState_t>(reinterpret_cast<uintptr_t>(engine) + 0x20);
  static const int kStates[] = {1, 6, 8, 10, 11, 12};  // raster, z test, z write, blend, vertex alpha
  static const uintptr_t kValues[] = {0, 0, 0, 5, 6, 1};
  uintptr_t saved[6] = {};
  for (int i = 0; i < 6; ++i) {
    get(kStates[i], &saved[i]);
    set(kStates[i], reinterpret_cast<void*>(kValues[i]));
  }
  auto render = patch::read<Im2DRender_t>(reinterpret_cast<uintptr_t>(engine) + game::kIm2DRenderPrimitive);
  quad(render, x0 - 1, y0 - 1, x1 + 1, y1 + 1, 0xC0000000u);
  if (f > 0) quad(render, x0, y0, x0 + (x1 - x0) * f, y1, fill);
  // A network player's connection under the bar, right: three bars like a phone's (ping to the
  // host: < 80 ms three green, < 160 two yellow, else one red); red and blinking while the game
  // waits for them.
  int slot = netplay::active() ? (int)(int16_t)reinterpret_cast<int(__cdecl*)(int)>(game::kGetPhysicalPort)(n) : -1;
  int ping = slot > 0 ? netplay::player_ping(slot) : 0;
  bool waited = slot > 0 && netplay::player_waited(slot);
  if (ping > 0 || waited) {
    int lit = waited ? 3 : ping < 80 ? 3 : ping < 160 ? 2 : 1;
    uint32_t on = waited ? ((GetTickCount() / 250) % 2 ? 0xFFFF3020u : 0x60FF3020u)
                         : lit == 3 ? 0xFF40D040u : lit == 2 ? 0xFFE0C020u : 0xFFE03020u;
    float base = py(top + kBarTop + kBarHeight + 9), unit = py(top + 1) - py(top);
    for (int i = 0; i < 3; ++i) {
      float bx1 = px(right - kBarInset - (2 - i) * 4.0f), bx0 = bx1 - (px(right) - px(right - 2.5f));
      float bar_h = (2.0f + 2.0f * i) * unit;
      quad(render, bx0, base - bar_h, bx1, base, i < lit ? on : 0x80404040u);
    }
  }
  for (int i = 0; i < 6; ++i) set(kStates[i], reinterpret_cast<void*>(saved[i]));
}

using Box_t = float(__fastcall*)(void* self, void* edx, int n, float x, float y);
Box_t oBox = nullptr;

float __fastcall hkBox(uint8_t* self, void* edx, int n, float x, float y) {
  widescreen::right_gauge_begin();  // gauges_size: smaller towards the top right
  float left = oBox(self, edx, n, x, y);
  int p = n & 0xFF;  // a byte, like the tags
  if (p < 4 && split() && !self[game::kStatusBoxHidden]) draw_bar(p, left, x, y);
  widescreen::right_gauge_end();
  return left;
}

bool calls(uintptr_t at, uintptr_t target) {
  return patch::read<uint8_t>(at) == 0xE8 && at + 5 + patch::read<int32_t>(at + 1) == target;
}

void put_call(const char* name, uintptr_t at, const void* fn) {
  uint8_t old[5], now[5] = {0xE8};
  std::memcpy(old, reinterpret_cast<const void*>(at), 5);
  int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(fn) - (at + 5));
  std::memcpy(now + 1, &rel, 4);
  patch::replace(name, at, old, now, 5);
}

}  // namespace

int own_player() { return display_player(); }

void install(const Config& cfg) {
  g_test = cfg.health_test;
  g_all_test = cfg.health_all_test;
  g_clear_test = cfg.clear_test;
  g_clear_stages = cfg.clear_test_stages;
  g_area_test = cfg.area_test;
  if (g_area_test > 0 && patch::bytes_are(game::kSetStartArea, game::kSetStartAreaHead, sizeof(game::kSetStartAreaHead)))
    patch::hook("[debug] area_test", game::kSetStartArea, (void*)&hkStartArea, (void**)&oStartArea);
  const uintptr_t vt = game::kGamePlayerVtable;
  bool ok = patch::read<uintptr_t>(vt + game::kGpGetHP) == game::kGpGetHPFn &&
            patch::read<uintptr_t>(vt + game::kGpAddHP) == game::kGpAddHPFn &&
            patch::read<uintptr_t>(vt + game::kGpSaveContext) == game::kGpSaveContextFn &&
            patch::read<uintptr_t>(vt + game::kGpLoadContext) == game::kGpLoadContextFn &&
            patch::read<uintptr_t>(game::kPlayerRunSlot) == game::kPlayerRun &&
            patch::bytes_are(game::kPlayerDeadCall, game::kPlayerDeadCallOld, sizeof(game::kPlayerDeadCallOld)) &&
            patch::bytes_are(game::kGamePlayerSetup, game::kGamePlayerSetupHead, sizeof(game::kGamePlayerSetupHead)) &&
            patch::bytes_are(game::kStatusBox, game::kStatusBoxHead, sizeof(game::kStatusBoxHead));
  for (uintptr_t c : game::kMeterPlayerCalls) ok = ok && calls(c, game::kGamePlayer);
  if (!ok) {
    LOGW("health: unexpected code, the team keeps one health");
    return;
  }
  if (!patch::hook("health: stage start", game::kGamePlayerSetup, (void*)&hkSetup, (void**)&oSetup) ||
      !patch::hook("health: bar in each player's box", game::kStatusBox, (void*)&hkBox, (void**)&oBox))
    return;
  // At zero: player_down(n) instead of CGameStage::NotifyPlayerDead(n).
  uint8_t down[16];
  std::memset(down, 0x90, sizeof(down));
  down[0] = 0xFF, down[1] = 0x77, down[2] = 0x08;  // push dword ptr [edi+8]
  down[3] = 0xE8;                                  // call player_down (__stdcall)
  int32_t rel = (int32_t)(reinterpret_cast<uintptr_t>(&player_down) - (game::kPlayerDeadCall + 8));
  std::memcpy(down + 4, &rel, 4);
  patch::replace("health: a player at zero falls asleep", game::kPlayerDeadCall, game::kPlayerDeadCallOld, down,
                 sizeof(down));
  for (uintptr_t c : game::kMeterPlayerCalls) put_call("health: the ring shows this PC's player", c, &meter_player);
  patch::slot("health: own health (get)", vt + game::kGpGetHP, game::kGpGetHPFn, (void*)&hkGetHP);
  patch::slot("health: own health (add)", vt + game::kGpAddHP, game::kGpAddHPFn, (void*)&hkAddHP);
  patch::slot("health: own health (save)", vt + game::kGpSaveContext, game::kGpSaveContextFn, (void*)&hkSaveContext);
  patch::slot("health: own health (load)", vt + game::kGpLoadContext, game::kGpLoadContextFn, (void*)&hkLoadContext);
  patch::slot("health: asleep and up again", game::kPlayerRunSlot, game::kPlayerRun, (void*)&hkPlayerRun);
}

}  // namespace health
