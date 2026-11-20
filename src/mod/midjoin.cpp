#include "mod/midjoin.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "common/log.h"
#include "mod/game.h"
#include "mod/patch.h"
#include "mod/sim.h"

namespace midjoin {
namespace {

Config g_cfg;
uint8_t g_known = 0;    // players of the session seen so far (a player back after a drop is not new)
uint8_t g_pending = 0;  // joined, not in the team yet
bool g_started = false;

template <typename T>
T& at(void* p, int off) {
  return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(p) + off);
}

template <typename R, typename... A>
R vcall(void* obj, int off, A... a) {
  void** vt = *reinterpret_cast<void***>(obj);
  return reinterpret_cast<R(__fastcall*)(void*, void*, A...)>(vt[off / 4])(obj, nullptr, a...);
}

struct V3 {
  float x, y, z;
};
struct CharaInfo {  // CGamePlayParam::CHARAINFO
  int id, costume, player;
};
struct Context {  // CGamePlayParam::PLAYERCONTEXT
  int no, hp, hp_max, damage, shuriken, shuriken_max, chara, chara_num;
};
struct Entry {  // CPlayer's character list
  uint8_t* chr;
  void* manip;
  int index;  // the slot's own number (the next character is (index + 1) % count)
};

uint8_t* play_param() { return reinterpret_cast<uint8_t*>(game::kPlayParam); }
CharaInfo* chara_infos(uint8_t* pp) { return reinterpret_cast<CharaInfo*>(pp + game::kPpCharaInfo); }
Context* contexts(uint8_t* pp) { return reinterpret_cast<Context*>(pp + game::kPpContext); }

int physical_port(int controller) {
  return (int)(int16_t)reinterpret_cast<int(__cdecl*)(int)>(game::kGetPhysicalPort)(controller);
}
int controller_of(int port) { return (int)(int16_t)reinterpret_cast<int(__cdecl*)(int)>(game::kGetController)(port); }

int team_players() {  // players in the stage now
  auto* prop = patch::read<uint8_t*>(game::kGameProperty);
  if (!prop || !patch::read<void*>(reinterpret_cast<uintptr_t>(prop) + game::kPropPlayers)) return 0;
  return reinterpret_cast<int(__cdecl*)()>(game::kGetPlayerNum)();
}

// Pad slot p already drives a player of the team (controllers 0..n-1 are the team's, in order).
bool has_place(int p, int n) {
  for (int c = 0; c < n && c < 4; ++c)
    if (physical_port(c) == p) return true;
  return false;
}

// Pad slot p's controller locked as the team's controller n (players use the controller of their number).
bool lock_as(int p, int n) {
  int c = controller_of(p);
  if (c < n || c < 0) {
    LOGW("join: pad %d has no free controller (controller %d, team %d)", p + 1, c, n);
    return false;
  }
  if (!reinterpret_cast<bool(__cdecl*)(int)>(game::kControllerLock)(c)) {
    LOGW("join: the game would not lock pad %d's controller", p + 1);
    return false;
  }
  if (physical_port(n) != p) {
    LOGW("join: pad %d's controller went to %d, not %d", p + 1, controller_of(p), n);
    reinterpret_cast<bool(__cdecl*)(int)>(game::kControllerUnlock)(controller_of(p));
    return false;
  }
  return true;
}

// The team for the next stages: chara info `k` (a turtle of player `donor`) goes to player n.
void move_in_team(uint8_t* pp, int k, int donor, int n) {
  CharaInfo* ci = chara_infos(pp);
  Context* cx = contexts(pp);
  int id = ci[k].id;
  ci[k].player = n;
  if (--cx[donor].chara_num < 1) cx[donor].chara_num = 1;
  if (cx[donor].chara == id)  // its first turtle: one it keeps
    for (int i = 0; i < at<int>(pp, game::kPpCharaInfoNum); ++i)
      if (ci[i].player == donor) {
        cx[donor].chara = ci[i].id;
        break;
      }
  Context& c = cx[n];
  c = cx[donor];  // health and shuriken as the team has them now
  c.no = n;
  c.damage = 0;
  c.hp = c.hp_max;
  c.chara = id;
  c.chara_num = 1;
  ++at<int>(pp, game::kPpPlayerNum);
  // A stage makes its players in the order of the chara infos, and each new turtle looks for its
  // owner among the players made so far: the infos must go by player number.
  int infos = at<int>(pp, game::kPpCharaInfoNum);
  if (infos > 4) infos = 4;
  std::stable_sort(ci, ci + infos, [](const CharaInfo& x, const CharaInfo& y) { return x.player < y.player; });
}

// The team member with the most turtles (at least two) and one of theirs to give: from the
// team's chara infos (between stages) — the last of that player's.
bool pick_in_team(uint8_t* pp, int n, int& donor, int& k) {
  int count[4] = {};
  CharaInfo* ci = chara_infos(pp);
  int infos = at<int>(pp, game::kPpCharaInfoNum);
  for (int i = 0; i < infos && i < 4; ++i)
    if (ci[i].player >= 0 && ci[i].player < 4) ++count[ci[i].player];
  donor = -1;
  for (int m = 0; m < n && m < 4; ++m)
    if (count[m] >= 2 && (donor < 0 || count[m] >= count[donor])) donor = m;
  if (donor < 0) return false;
  k = -1;
  for (int i = 0; i < infos && i < 4; ++i)
    if (ci[i].player == donor) k = i;
  return k >= 0;
}

// Between stages: the joiner gets a teammate's turtle in the team; the stage makes the player.
void join_team(int p) {
  uint8_t* pp = play_param();
  int n = at<int>(pp, game::kPpPlayerNum);
  if (has_place(p, n)) {
    g_pending &= (uint8_t)~(1u << p);
    return;
  }
  int donor, k;
  if (n >= 4 || !pick_in_team(pp, n, donor, k)) return;  // nothing to give: still waiting
  if (!lock_as(p, n)) return;
  int id = chara_infos(pp)[k].id;
  move_in_team(pp, k, donor, n);
  g_pending &= (uint8_t)~(1u << p);
  LOGI("join: pad %d is player %d from the next stage, with player %d's turtle %d (frame %u)", p + 1, n + 1,
       donor + 1, id, sim::frame());
}

// In a stage: a sleeping turtle of the player with the most moves to a new player with the
// joiner's pad, next to its old owner.
void join_stage(int p) {
  int n = team_players();
  if (has_place(p, n)) {
    g_pending &= (uint8_t)~(1u << p);
    return;
  }
  if (n >= 4) return;
  int donor = -1;
  uint8_t* donor_player = nullptr;
  for (int m = 0; m < n; ++m) {
    void* gp = reinterpret_cast<void*(__cdecl*)(int)>(game::kGamePlayer)(m);
    uint8_t* pl = gp ? at<uint8_t*>(gp, game::kGamePlayerPlayer) : nullptr;
    if (!pl) continue;
    int count = at<int>(pl, game::kPlayerCharaNum);
    if (count >= 2 && (!donor_player || count >= at<int>(donor_player, game::kPlayerCharaNum))) {
      donor = m;
      donor_player = pl;
    }
  }
  if (!donor_player) return;  // every player has one turtle: nothing to give
  auto* list = reinterpret_cast<Entry*>(donor_player + game::kPlayerCharaList);
  int count = at<int>(donor_player, game::kPlayerCharaNum);
  Entry* cur = at<Entry*>(donor_player, game::kPlayerCharaCurrent);
  int k = -1;
  for (int i = count - 1; i >= 0 && k < 0; --i)
    if (&list[i] != cur && list[i].chr) k = i;
  if (k < 0) return;
  uint8_t* chr = list[k].chr;
  int id = at<int>(chr, game::kChrPlayerId);

  uint8_t* pp = play_param();
  int team = at<int>(pp, game::kPpPlayerNum), ck = -1;
  for (int i = 0; i < at<int>(pp, game::kPpCharaInfoNum) && i < 4; ++i)
    if (chara_infos(pp)[i].player == donor && chara_infos(pp)[i].id == id) ck = i;
  if (team != n || ck < 0) {
    LOGW("join: the team (%d players, turtle %d of player %d: info %d) does not match the stage (%d)", team, id,
         donor + 1, ck, n);
    return;
  }
  if (!lock_as(p, n)) return;

  // Out of the old owner's list (the slots keep their numbers), its pad's manipulator gone.
  void* manip = list[k].manip;
  for (int i = k; i + 1 < count; ++i) {
    list[i].chr = list[i + 1].chr;
    list[i].manip = list[i + 1].manip;
  }
  list[count - 1].chr = nullptr;
  list[count - 1].manip = nullptr;
  if (cur > &list[k]) at<Entry*>(donor_player, game::kPlayerCharaCurrent) = cur - 1;
  at<int>(donor_player, game::kPlayerCharaNum) = count - 1;
  if (manip) vcall<void*>(manip, 0, 1);  // scalar deleting destructor, as ~CPlayer does

  // A player of the joiner's number (controller n) with that turtle.
  uint8_t* container = at<uint8_t*>(patch::read<uint8_t*>(game::kGameProperty), game::kPropPlayers);
  uint8_t* gp = at<uint8_t*>(container, game::kContainerPlayers) + n * game::kGamePlayerSize;
  reinterpret_cast<void(__fastcall*)(void*, void*, int)>(game::kGamePlayerCreate)(gp, nullptr, n);
  ++at<int>(container, game::kContainerCount);
  uint8_t* pl = at<uint8_t*>(gp, game::kGamePlayerPlayer);
  reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(game::kPlayerAddCharacter)(pl, nullptr, chr);

  move_in_team(pp, ck, donor, n);
  vcall<void>(gp, game::kGpLoadContext, (void*)&contexts(pp)[n]);
  void* dgp = reinterpret_cast<void*(__cdecl*)(int)>(game::kGamePlayer)(donor);
  const V3* at_pos = vcall<const V3*>(dgp, game::kGpGetPosition);
  float dir = vcall<float>(dgp, game::kGpGetRotY);
  V3 pos = at_pos ? *at_pos : V3{};
  pos.x += 0.8f;
  vcall<void>(gp, game::kGpRelocation, (const V3*)&pos, dir, true);
  g_pending &= (uint8_t)~(1u << p);
  LOGI("join: pad %d is player %d now, with player %d's turtle %d (frame %u)", p + 1, n + 1, donor + 1, id,
       sim::frame());
}

bool in_stage() {
  if (!reinterpret_cast<void*(__cdecl*)()>(game::kGetCurrentStage)() || team_players() < 1) return false;
  uint8_t* pp = play_param();
  return reinterpret_cast<int(__fastcall*)(void*, void*)>(game::kPpGetStageMode)(pp, nullptr) !=
         game::kStageModeRide;
}

// The stage's camera updaters run first in its frame (not while paused), before its objects.
using Updater_t = void(__fastcall*)(void* self, void* edx, void* map_camera);
Updater_t oDefaultUpdate = nullptr, oBossUpdate = nullptr;

void stage_tick() {
  if (!g_pending || !in_stage()) return;
  for (int p = 0; p < 4; ++p)
    if ((g_pending >> p) & 1) join_stage(p);
}

void __fastcall hkDefaultUpdate(void* self, void* edx, void* cam) {
  stage_tick();
  oDefaultUpdate(self, edx, cam);
}
void __fastcall hkBossUpdate(void* self, void* edx, void* cam) {
  stage_tick();
  oBossUpdate(self, edx, cam);
}

// CGameStage::AddPlayers: the team as it should be before the stage makes its players.
using AddPlayers_t = void(__fastcall*)(void* stage, void* edx, int blink);
AddPlayers_t oAddPlayers = nullptr;

void __fastcall hkAddPlayers(void* stage, void* edx, int blink) {
  for (int p = 0; p < 4; ++p)
    if ((g_pending >> p) & 1) join_team(p);
  oAddPlayers(stage, edx, blink);
}

}  // namespace

void frame_players(uint32_t frame, uint8_t mask) {
  // A session's first frame: its first players - the character select places them. (Every session's: a
  // game plays one after another without a restart. Until 0.16.6 the players of the session before stayed
  // "known": in a game that had played with fewer players, the others of the next session counted as
  // having joined in mid-game and were given a turtle of the team before the first stage - in that game
  // only: another team there than on the other PCs, and the games parted as the stage began.)
  if (frame <= 1 || !g_started) {
    g_started = true;
    g_known = mask;
    g_pending = 0;
    return;
  }
  uint8_t fresh = (uint8_t)(mask & ~g_known);
  if (!fresh) return;
  g_known |= mask;
  g_pending |= fresh;
  LOGI("join: pad(s) 0x%X in the game from frame %u: into the team", fresh, frame);
}

void install(const Config& cfg) {
  g_cfg = cfg;
  if (!cfg.net_on() && !cfg.session_at_title) return;  // (sessions begin in any running game)
  bool ok = patch::read<uintptr_t>(game::kDefaultUpdaterSlot) == game::kDefaultUpdater &&
            patch::read<uintptr_t>(game::kBossUpdaterSlot) == game::kBossUpdater &&
            patch::bytes_are(game::kAddPlayers, game::kAddPlayersHead, sizeof(game::kAddPlayersHead)) &&
            patch::bytes_are(game::kPlayerAddCharacter, game::kPlayerAddCharacterHead,
                             sizeof(game::kPlayerAddCharacterHead)) &&
            patch::bytes_are(game::kPlayParamAddChara, game::kPlayParamAddCharaHead,
                             sizeof(game::kPlayParamAddCharaHead));
  if (!ok) {
    LOGW("join: game code not recognised; players who join later wait for the character select");
    return;
  }
  oDefaultUpdate = reinterpret_cast<Updater_t>(game::kDefaultUpdater);
  oBossUpdate = reinterpret_cast<Updater_t>(game::kBossUpdater);
  patch::slot("join: into the stage (default camera updater)", game::kDefaultUpdaterSlot, game::kDefaultUpdater,
              (void*)&hkDefaultUpdate);
  patch::slot("join: into the stage (boss camera updater)", game::kBossUpdaterSlot, game::kBossUpdater,
              (void*)&hkBossUpdate);
  patch::hook("join: into the team before a stage (CGameStage::AddPlayers)", game::kAddPlayers,
              (void*)&hkAddPlayers, (void**)&oAddPlayers);
}

}  // namespace midjoin
