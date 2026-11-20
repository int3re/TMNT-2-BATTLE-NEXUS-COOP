#include "mod/netplay.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/joincode.h"
#include "common/lobby_steam.h"
#include "common/log.h"
#include "common/net.h"
#include "common/protocol.h"
#include "common/sha256.h"
#include "common/steam_boot.h"
#include "common/transport_enet.h"
#include "common/transport_steam.h"
#include "common/version.h"
#include "mod/fixes.h"
#include "mod/game.h"
#include "mod/menus.h"
#include "mod/midjoin.h"
#include "mod/netpage.h"
#include "mod/overlay.h"
#include "mod/pads.h"
#include "mod/patch.h"
#include "mod/video.h"

namespace netplay {
namespace {

using transport::Channel;
using transport::PeerId;

enum class Role { None, Host, Client };
enum class Phase { Off, Menu, Connecting, Lobby, Playing, Ended };

constexpr int kMax = tmnp::kMaxPlayers;
constexpr uint32_t kRing = 1024;          // frames kept for inputs and bundles
constexpr uint32_t kResendMs = 30;        // unreliable data is repeated this often
constexpr uint32_t kRosterMs = 1000;
constexpr uint32_t kWaitShowMs = 250;     // a late frame shows "waiting" after this
constexpr char kNetSave[] = "tmntsave-net.dat";

Config g_cfg;
std::mutex g_mu;
std::condition_variable g_cv;
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<Phase> g_phase{Phase::Off};
std::atomic<bool> g_active{false};

// --- state shared by the threads (g_mu) ----------------------------------------
Role g_role = Role::None;
int g_me = 0;
uint32_t g_delay = 3;
uint8_t g_players = 1;  // bit p: player p takes part (after the start)
std::string g_note;     // UI: what is going on (UTF-8)
std::string g_error;    // UI: why it failed
uint8_t g_fp[32] = {};  // TMNT2.exe + data files fingerprint

struct Player {
  bool present = false, ready = false;
  std::string name;
  PeerId peer;
  transport::ITransport* via = nullptr;
  uint32_t last_heard = 0;
  uint32_t ping = 0;
  uint32_t out_from = 0;  // dropped: out of the game from this frame on (0 = in)
  bool late = false;      // joined a running game, still replaying it: not in yet
  uint32_t in_from = 0;   // joined a running game: in from this frame on (0 = from the start)
  bool desync = false;    // host: this player's game went another way (they restart and catch up)
  uint64_t identity = 0;  // host: Steam id (through Steam) or the PC's player_id: the same player again
  uint32_t version = 0;   // host: the player's tmnt2net, 10000 major + 100 minor + patch
  // host: the history going to this player in parts (a game that joined the running session, or one
  // that fell out and is on the line again)
  uint32_t hist_next = 0;   // the next frame to send (0: nothing going)
  uint32_t hist_from = 0;   // where it began
  bool hist_start = false;  // the session's progress and the start follow it (a game that joined anew)
  uint32_t hist_tick = 0, hist_said = 0, hist_wait = 0;
  double hist_budget = 0;   // bytes that may go now
  // host: how early this player's inputs come (RosterPacket::lead)
  uint32_t newest_in = 0;   // the newest frame an input of theirs came for
  int lead_min = 127;       // its smallest lead over the host's game since the last roster
};
Player g_pl[kMax];
uint8_t g_waiting = 0;  // bit p: the host has been waiting for player p's input (host: own; client: roster)
std::atomic<uint8_t> g_slow_mask{0};  // host: players the game has been waiting for much of the time (slow_report)

struct InSlot {
  uint32_t frame = 0;
  tmnp::InputWire in{};
};
InSlot g_in[kMax][kRing];  // host: every player's input by frame
struct BSlot {
  uint32_t frame = 0;
  tmnp::BundleWire b{};
};
BSlot g_bundle[kRing];      // bundles by frame
uint32_t g_next_bundle = 1;  // host: the next bundle to build
// Every bundle of the session by frame (the ring above keeps the newest kRing): a player who
// joins a running game gets them all and replays the game from frame 1.
std::vector<tmnp::BundleWire> g_all;
uint32_t g_newest = 0;            // newest frame in g_all
std::atomic<bool> g_fast{false};  // client: replaying frames far behind the newest
std::atomic<bool> g_hurry{false};  // client: joined a running game, not in yet: no frame limit
// Host: the session's start-up (the game's logos and opening, frame 1 to the title's "НАЖМИ
// СТАРТ"). Its frames need nobody's buttons: the host makes them alone, ends every movie at
// once, marks them kBoot, and every PC runs them as fast as it can without showing them.
std::atomic<bool> g_boot{false};
std::atomic<uint32_t> g_boot_press{0};  // host: the bundle from which the start-up presses Start (the title)
constexpr uint32_t kBootPressEvery = 45;
uint32_t g_boot_last = 0;     // host: the start-up's last bundle (0 while it lasts)
uint32_t g_reliable_sent = 0;  // host: bundles sent once on the reliable channel (the start-up's)
constexpr uint16_t kStartButton = 1u << 9;  // PadWire buttons: the game's button 9, Start
bool g_late = false;              // client: we joined a running game
uint32_t g_hist_end = 0;          // client, joined late: the newest frame of the history we got
bool g_late_in = false;           // client, joined late: our pad is in again (a frame after the history has us)
uint32_t g_sent_bundle = 0;  // host: newest bundle sent

std::deque<tmnp::InputWire> g_mine;  // client: own newest inputs (resent)
bool g_mine_new = false;

struct HSlot {
  uint32_t frame = 0, calls = 0, hash = 0;
};
HSlot g_hash[kMax][64];     // host: hashes by player (own = slot 0); client: own (slot me)
std::deque<HSlot> g_hash_out;  // client: to send
// A client's own input delay. The host makes frame N only with every player's input for it: an input
// that comes late holds the frame, and with it the game on every PC - with a delay of d frames and a
// round trip of R the game makes d / R frames a second at most (2 frames over 50 ms: 40 a second for
// everybody). The delay of the session's start is a guess from the first pings. Each client keeps its
// own by what the host tells it every second: how many frames ahead of need its inputs came.
std::atomic<uint32_t> g_game_frame{0};  // the frame the game thread asked for last (frame_input)
std::atomic<uint32_t> g_delay_now{0};   // client: its own delay now (0: the session's, g_delay)
uint32_t g_submitted = 0;               // client: the newest frame an input of ours was made for
double g_wait_acc = 0;                  // client: ms our frames waited for the host's since the last roster
int g_delay_calm = 0;                   // client: rosters in a row with the inputs well ahead
uint32_t g_sync_frames = 0;             // client: frames played in step since coming in
constexpr uint32_t kMaxDelay = 12;

void adapt_delay(int lead) {  // client, g_mu held: the host's roster came
  const uint32_t base = std::max<uint32_t>(1, g_delay), now = g_delay_now.load() ? g_delay_now.load() : base;
  uint32_t want = now;
  double waited = g_wait_acc;
  g_wait_acc = 0;
  if (lead == 127 || (g_late && !g_late_in) || g_fast || g_hurry) {
    g_delay_calm = 0;
    return;
  }
  // (A game that does not wait for the host's frames is held by its own PC, not by the network: more
  // delay would only make its controls slower.)
  if (lead <= 0) {
    if (waited >= 50) want = now + 2;
    g_delay_calm = 0;
  } else if (lead == 1) {
    if (waited >= 50) want = now + 1;
    g_delay_calm = 0;
  } else if (lead >= 4) {  // (well ahead for five seconds: one frame less, three stay in hand)
    if (++g_delay_calm >= 5) {
      want = now - 1;
      g_delay_calm = 0;
    }
  } else {
    g_delay_calm = 0;
  }
  want = std::max(base, std::min(kMaxDelay, want));
  if (want == now) return;
  g_delay_now = want;
  LOGI("net: own input delay %u -> %u frames (our inputs came %d frame(s) ahead of the host's need, our frames waited %.0f ms)",
       now, want, lead, waited);
}

// Client: a desync of our game restarts it into the same session (it replays the session and
// catches up), at most kMaxResyncs times in a row (-tmnt2net-resync=N counts them; two minutes in
// step and the count begins anew). If that does not help, the game leaves the session: it does not
// play on in a game of its own.
constexpr int kMaxResyncs = 4;
int g_resyncs = 0;
std::atomic<bool> g_restart_rejoin{false};
// The network page's lobby in a game alone (no session in this process): the host's start
// restarts every game into the session.
std::atomic<bool> g_pre{false};
std::atomic<bool> g_begin{false};
std::string g_auto_mode;                    // started as "host" / "join" / "menu": that lobby at the title
bool g_session_on = false;                  // game thread: a session began in this game (own progress kept)
ULONGLONG g_t0_ms = GetTickCount64();      // [debug] session_start_at: from the process start
// The progress a session in a running game plays with: the host's (the co-op one kept in
// tmnt2net-coop.rec, else its own), sent to every player before the start; each game keeps its own.
uint8_t g_rec_session[tmnp::kRecordSize];
bool g_rec_have = false;  // g_rec_session holds the session's (host: taken at the start; client: received)
uint8_t g_rec_own[tmnp::kRecordSize];
bool g_rec_own_kept = false;
uint8_t g_rec_fresh = 0, g_rec_own_fresh = 0;  // the "new game" flag of the session's / of our own progress
// The game's own options that are part of its logic (difficulty, autosave, help windows, hit effects):
// the host's for everybody during a session, each game's own again after it.
uint8_t g_opt_session[2] = {0, 0}, g_opt_own[2] = {0, 0};  // {RecordPacket::options, difficulty}
bool g_opt_own_kept = false;

void read_options(uint8_t out[2]) {
  out[0] = out[1] = 0;
  const uint8_t* o = patch::read<const uint8_t*>(game::kOptionDataPtr);
  if (!o) return;
  out[0] = 0x80 | (o[game::kOptAutosave] ? 1 : 0) | (o[game::kOptDisplay + game::kOptHitEffects] ? 2 : 0) |
           (o[game::kOptDisplay + game::kOptHelp] ? 4 : 0);
  int d = patch::read<int32_t>(reinterpret_cast<uintptr_t>(o) + game::kOptDifficulty);
  out[1] = (uint8_t)(d < 0 ? 0 : d > 2 ? 2 : d);
}

void write_options(const uint8_t in[2]) {
  uint8_t* o = patch::read<uint8_t*>(game::kOptionDataPtr);
  if (!o || !(in[0] & 0x80)) return;
  o[game::kOptAutosave] = in[0] & 1 ? 1 : 0;
  o[game::kOptDisplay + game::kOptHitEffects] = in[0] & 2 ? 1 : 0;
  o[game::kOptDisplay + game::kOptHelp] = in[0] & 4 ? 1 : 0;
  int32_t d = in[1] > 2 ? 1 : in[1];
  std::memcpy(o + game::kOptDifficulty, &d, 4);
}
constexpr uint32_t kSessionSeed = 0x544D4E54;  // RpRandom's seed at a session's frame 1 (every PC)
std::atomic<bool> g_keep_steam{false};    // client retrying after the page's start: Steam stays up
std::atomic<bool> g_go{false};             // client: the host started (or its game runs): restart
std::atomic<bool> g_go_request{false};     // host (page): send PT_Go, then restart
std::atomic<uint32_t> g_go_sent{0};        // host: when PT_Go went out (ms)
std::atomic<uint64_t> g_pick_lobby{0};     // client (page): the friend's game picked
std::atomic<uint64_t> g_invite{0};         // host (page): a friend to invite
std::vector<lobby::Friend> g_friends;      // host (page): friends on line (g_mu)
std::atomic<bool> g_steam_tried{false};    // Steam was asked for once (on or off is known)
std::vector<lobby::Found> g_games;         // client (page): friends' games seen (g_mu)
uint8_t g_go_players = 0;                  // host: places for the restarted game

// Save: host's own file; client: what arrived.
std::vector<uint8_t> g_save;
uint32_t g_save_total = 0, g_save_got = 0;
bool g_save_done = false;
bool g_start_request = false;  // game thread (host): Enter pressed
bool g_cancel_request = false;

// Client: whom it dials.
PeerId g_host;
transport::ITransport* g_host_via = nullptr;
uint32_t g_host_heard = 0;
uint32_t g_seq = 0;

std::unique_ptr<transport::ITransport> g_enet, g_steam_t;
std::unique_ptr<lobby::IHost> g_lobby;
std::unique_ptr<lobby::IBrowser> g_browser;
std::string g_join_code;  // host: LAN / IP code
std::string g_steam_note;

std::wstring wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

uint8_t* record() { return patch::read<uint8_t*>(game::kRecordPtr); }
std::wstring coop_record_path() { return module_dir() + L"tmnt2net-coop.rec"; }


std::string computer_name() {
  wchar_t buf[64] = {};
  DWORD n = 64;
  if (!GetComputerNameW(buf, &n)) return "player";
  char out[128] = {};
  WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
  return out;
}

// --- the game's files: everyone must run the same ----------------------------------
// TMNT2.exe (the mod refuses any other) and the data: sizes of every DAT/BIN,
// TMNTE.DAT whole and 64 slices of TMNT.DAT (text, scripts; a translation changes them).
void fingerprint(uint8_t out[32]) {
  std::wstring dir = module_dir();
  std::vector<uint8_t> acc(game::kExeSha256, game::kExeSha256 + 32);
  const wchar_t* files[] = {L"TMNT.DAT", L"TMNTE.DAT", L"TMNT2V00.DAT", L"TMNT2V01.DAT", L"TMNT2V02.DAT",
                            L"TMNT2V03.DAT", L"TMNT2V04.DAT", L"TMNT2V05.DAT", L"SeData.bin", L"TrSeData.bin"};
  for (const wchar_t* f : files) {
    HANDLE h = CreateFileW((dir + f).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    LARGE_INTEGER size{};
    if (h != INVALID_HANDLE_VALUE) GetFileSizeEx(h, &size);
    const uint8_t* sp = reinterpret_cast<const uint8_t*>(&size.QuadPart);
    acc.insert(acc.end(), sp, sp + 8);
    if (h == INVALID_HANDLE_VALUE) continue;
    bool whole = _wcsicmp(f, L"TMNTE.DAT") == 0, sliced = _wcsicmp(f, L"TMNT.DAT") == 0;
    std::vector<uint8_t> buf(65536);
    if (whole || sliced) {
      int slices = whole ? 1 : 64;
      for (int i = 0; i < slices; ++i) {
        LARGE_INTEGER at{};
        at.QuadPart = whole ? 0 : size.QuadPart / slices * i;
        SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
        DWORD want = whole ? (DWORD)std::min<LONGLONG>(size.QuadPart, 1 << 20) : (DWORD)buf.size();
        if (buf.size() < want) buf.resize(want);
        DWORD got = 0;
        ReadFile(h, buf.data(), want, &got, nullptr);
        acc.insert(acc.end(), buf.begin(), buf.begin() + got);
      }
    }
    CloseHandle(h);
  }
  sha256_bytes(acc.data(), acc.size(), out);
}

// --- sending (network thread) ------------------------------------------------------
bool g_outage = false;  // [debug] net_outage: this client's link is dead now

template <typename T>
bool send_to(transport::ITransport* t, const PeerId& peer, T& pkt, tmnp::PacketType type, Channel ch) {
  if (!t || !peer.valid() || g_outage) return false;
  tmnp::fill_header(pkt.h, type, ++g_seq);
  return t->Send(peer, &pkt, sizeof(pkt), ch);
}

template <typename T>
void send_clients(T& pkt, tmnp::PacketType type, Channel ch, uint8_t only_mask = 0xFF) {
  for (int p = 1; p < kMax; ++p)
    if (g_pl[p].present && (only_mask & (1u << p))) send_to(g_pl[p].via, g_pl[p].peer, pkt, type, ch);
}

bool send_record(int p) {
  static tmnp::RecordPacket rp;  // (2.4 KB)
  rp = tmnp::RecordPacket{};
  rp.size = tmnp::kRecordSize;
  std::memcpy(rp.data, g_rec_session, sizeof(rp.data));
  rp.fresh = g_rec_fresh;
  rp.options = g_opt_session[0];
  rp.difficulty = g_opt_session[1];
  return send_to(g_pl[p].via, g_pl[p].peer, rp, tmnp::PT_Record, Channel::Control);
}

// Host, at the start (network thread; the game sits at its title menu): the session's progress.
void take_session_record() {
  read_options(g_opt_session);  // (the host's own: plain bytes of the game's options)
  g_rec_have = false;
  FILE* f = _wfopen(coop_record_path().c_str(), L"rb");
  if (f) {
    g_rec_have = fread(g_rec_session, 1, sizeof(g_rec_session), f) == sizeof(g_rec_session);
    if (fread(&g_rec_fresh, 1, 1, f) != 1) g_rec_fresh = 0;
    fclose(f);
  }
  if (g_rec_have) {
    LOGI("net: the session plays with the co-op progress (tmnt2net-coop.rec)");
  } else if (uint8_t* r = record()) {
    std::memcpy(g_rec_session, r, sizeof(g_rec_session));
    g_rec_fresh = patch::read<uint8_t>(game::kNewGameFlag);
    g_rec_have = true;
    LOGI("net: the session plays with the host's progress (no co-op progress yet)");
  }
}


void send_roster() {
  tmnp::RosterPacket r{};
  for (int p = 0; p < kMax; ++p) {
    if (g_pl[p].present) r.present |= (uint8_t)(1u << p);
    if (g_pl[p].ready) r.ready |= (uint8_t)(1u << p);
    tmnp::put_str(r.names[p], g_pl[p].name.c_str());
    r.ping_ms[p] = (uint16_t)std::min<uint32_t>(g_pl[p].ping, 65535);
    r.lead[p] = (int8_t)std::max(-128, std::min(127, g_pl[p].lead_min));
    g_pl[p].lead_min = 127;
  }
  r.started = g_phase == Phase::Playing ? 1 : 0;
  r.waiting = g_waiting;
  send_clients(r, tmnp::PT_Roster, Channel::Control);
}

void reject(transport::ITransport* t, const PeerId& peer, uint8_t reason, const char* msg) {
  tmnp::RejectPacket r{};
  r.reason = reason;
  tmnp::put_str(r.message, msg);
  send_to(t, peer, r, tmnp::PT_Reject, Channel::Control);
  t->Disconnect(peer);
}

void send_save(int p) {
  tmnp::SavePacket s{};
  s.total = (uint32_t)g_save.size();
  if (g_save.empty()) {
    send_to(g_pl[p].via, g_pl[p].peer, s, tmnp::PT_Save, Channel::Control);
    return;
  }
  for (uint32_t off = 0; off < g_save.size(); off += sizeof(s.data)) {
    s.offset = off;
    s.size = (uint16_t)std::min<size_t>(sizeof(s.data), g_save.size() - off);
    std::memcpy(s.data, g_save.data() + off, s.size);
    send_to(g_pl[p].via, g_pl[p].peer, s, tmnp::PT_Save, Channel::Control);
  }
}

// --- the host's frame bundles ------------------------------------------------------
void keep(const tmnp::BundleWire& b) {
  if (b.frame == 0) return;
  if (g_all.size() <= b.frame) g_all.resize(b.frame + 1 + 4096);
  g_all[b.frame] = b;
  g_newest = std::max(g_newest, b.frame);
}

bool takes_part(int p, uint32_t frame) {
  if (!(g_players & (1u << p))) return false;
  if (g_pl[p].in_from && frame < g_pl[p].in_from) return false;
  return g_pl[p].out_from == 0 || frame < g_pl[p].out_from;
}

void build_bundles() {
  while (true) {
    uint32_t f = g_next_bundle;
    bool boot = g_boot.load();
    bool all = true;
    for (int p = 0; p < kMax && all; ++p)
      if (takes_part(p, f) && (p == 0 || !boot) && g_in[p][f % kRing].frame != f) all = false;
    if (!all) break;
    tmnp::BundleWire b{};
    b.frame = f;
    const tmnp::InputWire& host = g_in[0][f % kRing].in;
    b.movie = host.movie;
    bool ended = host.movie != 0;
    for (int p = 0; p < kMax; ++p) {
      if (!takes_part(p, f)) continue;
      b.players |= (uint8_t)(1u << p);
      if (boot) {  // nobody plays yet: no buttons, every movie over at once
        b.pads[p] = tmnp::PadWire{};
        b.pads[p].connected = 1;
        continue;
      }
      const tmnp::InputWire& in = g_in[p][f % kRing].in;
      b.pads[p] = in.pad;
      b.pads[p].connected = 1;  // a player's pad is there as long as the player is
      if (!(in.movie == host.movie && (in.flags & sim::kMovieEnded))) ended = false;
    }
    // A movie ends when it has ended for everyone; a voice plays as the host hears it.
    if (ended) b.flags |= sim::kMovieEnded;
    if (boot) {
      b.flags |= sim::kBoot;
      uint32_t from = g_boot_press.load();
      if (from && f >= from && (f - from) % kBootPressEvery == 0) b.pads[0].buttons |= kStartButton;
    }
    if (host.flags & sim::kVoiceBusy) b.flags |= sim::kVoiceBusy;
    g_bundle[f % kRing].frame = f;
    g_bundle[f % kRing].b = b;
    keep(b);
    ++g_next_bundle;
    g_cv.notify_all();
  }
}

void send_bundles(bool force) {
  uint32_t newest = g_next_bundle - 1;
  // The start-up's frames come far faster than the newest few repeated over the unreliable
  // channel could cover a loss: each goes once, in order, over the reliable one (as history).
  uint32_t reliable_to = g_boot_last ? std::min(newest, g_boot_last + (uint32_t)tmnp::kHistory) : newest;
  if (g_boot.load() || g_boot_last)
    while (g_reliable_sent < reliable_to) {
      tmnp::HistoryPacket hp{};
      hp.first = g_reliable_sent + 1;
      hp.count = (uint8_t)std::min<uint32_t>(tmnp::kHistoryItems, reliable_to - g_reliable_sent);
      for (int i = 0; i < hp.count; ++i) hp.items[i] = g_all[hp.first + i];
      send_clients(hp, tmnp::PT_History, Channel::Control);
      g_reliable_sent += hp.count;
    }
  if (newest == 0 || (!force && newest == g_sent_bundle)) return;
  tmnp::FramesPacket fp{};
  for (uint32_t f = newest; f >= 1 && fp.count < tmnp::kHistory; --f) fp.items[fp.count++] = g_bundle[f % kRing].b;
  send_clients(fp, tmnp::PT_Frames, Channel::Input);
  g_sent_bundle = newest;
}

// keep_link: the player fell silent in a running game. Their link is not closed: when their game is
// still there and its input comes again, they are back on the spot (return_in_place).
void drop(int p, const char* why, bool keep_link = false) {
  if (!g_pl[p].present) return;
  LOGW("net: player %d (%s) is out: %s", p + 1, g_pl[p].name.c_str(), why);
  if (g_phase == Phase::Playing && (g_players & (1u << p)) && !g_pl[p].out_from) {
    g_pl[p].out_from = std::max(g_next_bundle, 1u);
    overlay::notify(L"Р" + std::to_wstring(p + 1) + L" " + wide(g_pl[p].name) + L" ПОКИНУЛ ИГРУ", 6.0);
  }
  if (g_pl[p].via && !keep_link) g_pl[p].via->Close(g_pl[p].peer);
  g_pl[p].present = false;
  g_pl[p].ready = false;
  g_pl[p].hist_next = 0;
  if (g_phase != Phase::Playing) g_pl[p].name.clear();
  send_roster();
  if (g_lobby) g_lobby->SetInGame(g_phase == Phase::Playing);
  g_cv.notify_all();
}

// A player joining the running game: every bundle so far, then the start. After half an hour that is
// megabytes: all at once it overflowed Steam's send buffer ("limit exceeded": most of it never left, and
// the player waited for the host for ever). It goes in parts, as fast as the link takes it
// (pump_history), and the start follows the last part.
constexpr double kHistRate = 700.0 * 1024, kHistBurst = 256.0 * 1024;  // bytes a second; at once

void begin_history(int p, uint32_t from, bool then_start) {
  Player& pl = g_pl[p];
  pl.hist_next = pl.hist_from = std::max(from, 1u);
  pl.hist_start = then_start;
  pl.hist_budget = kHistBurst;
  pl.hist_tick = pl.hist_said = net::now_ms();
  pl.hist_wait = 0;
}

void send_history(int p) {
  begin_history(p, 1, true);
  uint32_t last = g_next_bundle - 1;
  LOGI("net: P%d gets frames 1..%u to catch up (%u KB, in parts)", p + 1, last,
       (unsigned)((last / tmnp::kHistoryItems + 1) * sizeof(tmnp::HistoryPacket) / 1024));
}

void pump_history(uint32_t now) {
  for (int p = 1; p < kMax; ++p) {
    Player& pl = g_pl[p];
    if (!pl.hist_next) continue;
    if (!pl.present) {
      pl.hist_next = 0;
      continue;
    }
    pl.hist_budget = std::min(kHistBurst, pl.hist_budget + kHistRate * (double)(now - pl.hist_tick) / 1000.0);
    pl.hist_tick = now;
    if (now < pl.hist_wait) continue;  // the link's queue was full a moment ago
    const uint32_t last = g_next_bundle - 1;
    auto packet = [&](uint32_t count) {
      tmnp::HistoryPacket hp{};
      hp.first = pl.hist_next;
      hp.count = (uint8_t)count;
      for (uint32_t i = 0; i < count; ++i) hp.items[i] = g_all[hp.first + i];
      if (!send_to(pl.via, pl.peer, hp, tmnp::PT_History, Channel::Control)) {
        pl.hist_wait = now + 100;
        return false;
      }
      pl.hist_next += count;
      pl.hist_budget -= (double)sizeof(hp);
      return true;
    };
    // Whole packets while there are that many frames; the last few go with the end of it, at once
    // (frame after frame, each in a packet of its own, would never catch the game up on a slow link).
    bool blocked = false;
    while (!blocked && last + 1 - pl.hist_next >= (uint32_t)tmnp::kHistoryItems &&
           pl.hist_budget >= (double)sizeof(tmnp::HistoryPacket))
      blocked = !packet(tmnp::kHistoryItems);
    const uint32_t left = last + 1 - pl.hist_next;
    const double end_size = (double)sizeof(tmnp::HistoryPacket) +
                            (pl.hist_start ? (double)(sizeof(tmnp::RecordPacket) + sizeof(tmnp::StartPacket)) : 0.0);
    if (!blocked && left < (uint32_t)tmnp::kHistoryItems && pl.hist_budget >= end_size) {
      bool ok = left == 0 || packet(left);
      if (ok && pl.hist_start) {  // from here the frames come as to everybody: the session's progress, the start
        tmnp::StartPacket st{};
        st.input_delay = (uint8_t)g_delay;
        st.players = g_players;
        ok = (!(g_cfg.session_at_title && g_rec_have) || send_record(p)) &&
             send_to(pl.via, pl.peer, st, tmnp::PT_Start, Channel::Control);
        if (ok)
          pl.hist_start = false;
        else
          pl.hist_wait = now + 100;
      }
      if (ok) {
        LOGI("net: P%d has the frames %u..%u now", p + 1, pl.hist_from, last);
        pl.hist_next = 0;
      }
    } else if (now - pl.hist_said >= 3000) {
      pl.hist_said = now;
      LOGI("net: P%d: frames up to %u of %u sent", p + 1, pl.hist_next - 1, last);
    }
  }
}

// A player who fell silent (their link was gone for longer than the host waits) and whose game is
// still running comes back without starting it again: their input is here again, from the same place
// on the line. They get the frames made since, play them at full speed and are in once their input
// reaches the newest frame, as a player who joins late. (tmnt2net 0.16.3 and newer: an older game does
// not hurry through the frames it missed and would stay behind for good.)
int return_in_place(transport::ITransport* t, const PeerId& peer, const std::vector<uint8_t>& d) {
  if (g_phase != Phase::Playing || d.size() < sizeof(tmnp::InputPacket)) return -1;
  for (int q = 1; q < kMax; ++q) {
    Player& pl = g_pl[q];
    if (pl.present || !pl.out_from || !(g_players & (1u << q)) || pl.peer != peer || pl.version < 1603) continue;
    tmnp::InputPacket ip;
    std::memcpy(&ip, d.data(), sizeof(ip));
    uint32_t newest = 0;
    for (int i = 0; i < ip.count && i < tmnp::kHistory; ++i) newest = std::max(newest, ip.items[i].frame);
    // Their game waits for a frame about as old as their newest input (less the input delay); a few
    // seconds more, to be sure (frames they have already are simply the same again).
    uint32_t have = newest > g_delay ? newest - g_delay : 1;
    uint32_t from = std::min(pl.out_from, have);
    from = from > 240 ? from - 240 : 1;
    pl.present = true;
    pl.ready = true;
    pl.via = t;
    pl.last_heard = net::now_ms();
    pl.late = true;
    begin_history(q, from, false);
    LOGI("net: P%d '%s' is on the line again (their game kept running, out since frame %u): frames from %u, in again "
         "once caught up", q + 1, pl.name.c_str(), pl.out_from, from);
    overlay::notify(L"Р" + std::to_wstring(q + 1) + L" " + wide(pl.name) + L" СНОВА НА СВЯЗИ", 4.0, 0xFF80FF80);
    send_roster();
    if (g_lobby) g_lobby->SetInGame(true);
    return q;
  }
  return -1;
}

int player_of(const PeerId& peer) {
  for (int p = 1; p < kMax; ++p)
    if (g_pl[p].present && g_pl[p].peer == peer) return p;
  return -1;
}

void compare_hash(int p, uint32_t frame) {
  const HSlot& mine = g_hash[0][(frame / tmnp::kHashEvery) % 64];
  const HSlot& theirs = g_hash[p][(frame / tmnp::kHashEvery) % 64];
  if (mine.frame != frame || theirs.frame != frame || g_pl[p].desync) return;
  if (mine.calls == theirs.calls && mine.hash == theirs.hash) return;
  g_pl[p].desync = true;
  LOGE("net: DESYNC at frame %u: host %u calls %08x, P%d (%s) %u calls %08x", frame, mine.calls, mine.hash, p + 1,
       g_pl[p].name.c_str(), theirs.calls, theirs.hash);
  sim::keep_logs(L"host");
  tmnp::DesyncPacket d{};
  d.frame = frame;
  d.player = (uint8_t)p;
  send_clients(d, tmnp::PT_Desync, Channel::Control);
  overlay::notify(L"Р" + std::to_wstring(p + 1) + L" " + wide(g_pl[p].name) + L": рассинхронизация (кадр " +
                      std::to_wstring(frame) + L"). Его игра перезапустится и догонит остальных.",
                  8.0, 0xFFFFD040);
}

void host_start() {
  uint32_t ping = 0;
  g_players = 1;
  for (int p = 1; p < kMax; ++p) {
    if (!g_pl[p].present) continue;
    if (!g_pl[p].ready) {
      reject(g_pl[p].via, g_pl[p].peer, tmnp::RR_Started, "Игра началась без вас: сохранение не успело дойти.");
      g_pl[p].present = false;
      continue;
    }
    g_players |= (uint8_t)(1u << p);
    ping = std::max(ping, g_pl[p].ping);
  }
  g_delay = g_cfg.input_delay > 0 ? (uint32_t)g_cfg.input_delay : std::min(10u, std::max(2u, ping / 16 + 2));
  // Frames 1..delay: nobody sampled them; everyone stands still.
  for (int p = 0; p < kMax; ++p) {
    if (!(g_players & (1u << p))) continue;
    for (uint32_t f = 1; f <= g_delay; ++f) {
      tmnp::InputWire in{};
      in.frame = f;
      g_in[p][f % kRing].frame = f;
      g_in[p][f % kRing].in = in;
    }
  }
  tmnp::StartPacket s{};
  s.input_delay = (uint8_t)g_delay;
  s.players = g_players;
  if (g_cfg.session_at_title) {
    take_session_record();
    for (int p = 1; p < kMax; ++p)
      if (g_rec_have && (g_players & (1u << p))) send_record(p);
  }
  send_clients(s, tmnp::PT_Start, Channel::Control, g_players);
  g_phase = Phase::Playing;
  g_active = !g_cfg.session_at_title;  // (in a running game: once it begins here, at the title menu)
  g_boot = !g_cfg.session_at_title;  // (a session in a running game begins at the title: no start-up)
  if (g_cfg.session_at_title) g_begin = true;
  g_boot_press = 0;
  g_boot_last = 0;
  g_reliable_sent = 0;
  if (g_lobby) g_lobby->SetInGame(true);
  LOGI("net: the game starts: players mask 0x%X, input delay %u frames (max ping %u ms)", g_players, g_delay, ping);
  build_bundles();
  send_bundles(true);
  g_cv.notify_all();
}

// --- receiving ---------------------------------------------------------------------
void host_data(transport::ITransport* t, const PeerId& peer, const std::vector<uint8_t>& d) {
  uint8_t type = tmnp::check_header(d.data(), d.size());
  int p = player_of(peer);
  if (p < 0 && type == tmnp::PT_Input) p = return_in_place(t, peer, d);
  if (p >= 0) g_pl[p].last_heard = net::now_ms();
  switch (type) {
    case tmnp::PT_Hello: {
      if (d.size() < sizeof(tmnp::HelloPacket)) return;
      tmnp::HelloPacket h;
      std::memcpy(&h, d.data(), sizeof(h));
      h.name[sizeof(h.name) - 1] = 0;
      if (h.h.version != tmnp::kVersion) {
        reject(t, peer, tmnp::RR_Version, "Другая версия tmnt2net: обновите мод у обоих.");
        return;
      }
      if (std::memcmp(h.exe_sha256, g_fp, 32) != 0) {
        reject(t, peer, tmnp::RR_ExeMismatch,
               "Файлы игры отличаются (версия, перевод или exe): нужна одинаковая игра у всех.");
        LOGW("net: %s refused: other game files", transport::to_string(peer).c_str());
        return;
      }
      // A running game takes a new player too: they replay it from frame 1 (send_history).
      bool late = g_phase == Phase::Playing;
      std::string name = h.name[0] ? h.name : "игрок";
      // Someone who dropped out comes back to their own place (their turtle stood there meanwhile):
      // known by their Steam id or their PC's player_id, whatever name they come with now; else by name.
      uint64_t identity = peer.kind == transport::Kind::Steam ? peer.id : h.player_id;
      uint32_t back_from = 0;
      for (int pass = 0; pass < 2 && p < 0 && late; ++pass)
        for (int q = 1; q < kMax && p < 0; ++q) {
          const Player& old = g_pl[q];
          if (old.present || !(g_players & (1u << q)) || !old.out_from) continue;
          if (pass == 0 ? (identity && old.identity == identity) : old.name == name) {
            p = q;
            back_from = old.out_from;
          }
        }
      if (p < 0) {
        for (int q = 1; q < kMax && p < 0; ++q)
          if (!g_pl[q].present && !(late && (g_players & (1u << q)))) p = q;
        if (p < 0) {
          reject(t, peer, tmnp::RR_Full, "Уже 4 игрока.");
          return;
        }
      }
      Player& pl = g_pl[p];
      pl = Player{};
      for (HSlot& s : g_hash[p]) s = HSlot{};  // (what the player in this place reported before is not theirs)
      pl.present = true;
      pl.name = name;
      pl.peer = peer;
      pl.via = t;
      pl.last_heard = net::now_ms();
      pl.late = late;
      pl.out_from = back_from;  // a returning player stays out until caught up
      pl.identity = identity;
      {
        int a = 0, b = 0, c = 0;
        h.mod_version[sizeof(h.mod_version) - 1] = 0;
        sscanf(h.mod_version, "%d.%d.%d", &a, &b, &c);
        pl.version = (uint32_t)(a * 10000 + b * 100 + c);
      }
      LOGI("net: P%d '%s' joined from %s (%s)%s%s", p + 1, pl.name.c_str(), t->Describe(peer).c_str(), t->Name(),
           late ? ", the game runs: catching up" : "", back_from ? ", back to their place" : "");
      tmnp::WelcomePacket w{};
      w.player = (uint8_t)p;
      w.late = late ? 1 : 0;
      tmnp::put_str(w.name, g_pl[0].name.c_str());
      send_to(t, peer, w, tmnp::PT_Welcome, Channel::Control);
      send_save(p);
      send_roster();
      overlay::notify(L"Р" + std::to_wstring(p + 1) + L" " + wide(pl.name) + (late ? L" ПОДКЛЮЧАЕТСЯ" : L" ВОШЕЛ В ИГРУ"),
                      4.0, 0xFF80FF80);
      break;
    }
    case tmnp::PT_Ready:
      if (p < 0) return;
      g_pl[p].ready = true;
      LOGI("net: P%d is ready", p + 1);
      if (g_phase == Phase::Playing && g_pl[p].late) send_history(p);
      send_roster();
      break;
    case tmnp::PT_Input: {
      if (p < 0 || d.size() < sizeof(tmnp::InputPacket)) return;
      tmnp::InputPacket ip;
      std::memcpy(&ip, d.data(), sizeof(ip));
      uint32_t newest = 0;
      for (int i = 0; i < ip.count && i < tmnp::kHistory; ++i) {
        const tmnp::InputWire& in = ip.items[i];
        if (in.frame < g_next_bundle || in.frame >= g_next_bundle + kRing / 2) continue;
        g_in[p][in.frame % kRing].frame = in.frame;
        g_in[p][in.frame % kRing].in = in;
        newest = std::max(newest, in.frame);
      }
      if (newest > g_pl[p].newest_in) {
        if (!g_pl[p].late && !g_pl[p].out_from && g_phase == Phase::Playing) {
          int lead = (int)newest - (int)g_game_frame.load();
          if (lead < g_pl[p].lead_min) g_pl[p].lead_min = lead;
        }
        g_pl[p].newest_in = newest;
      }
      // A player who replayed the running game is in once their input reaches the newest frame:
      // their pad is there from that frame on, the same frame on every PC.
      if (g_pl[p].late && g_phase == Phase::Playing && newest > g_next_bundle) {
        g_pl[p].late = false;
        g_pl[p].in_from = newest;
        g_pl[p].out_from = 0;
        g_players |= (uint8_t)(1u << p);
        LOGI("net: P%d '%s' caught up: in from frame %u", p + 1, g_pl[p].name.c_str(), newest);
        overlay::notify(L"Р" + std::to_wstring(p + 1) + L" " + wide(g_pl[p].name) + L" ВОШЕЛ В ИГРУ", 4.0, 0xFF80FF80);
        send_roster();
      }
      build_bundles();
      break;
    }
    case tmnp::PT_Hash: {
      if (p < 0 || d.size() < sizeof(tmnp::HashPacket)) return;
      tmnp::HashPacket hp;
      std::memcpy(&hp, d.data(), sizeof(hp));
      HSlot& s = g_hash[p][(hp.frame / tmnp::kHashEvery) % 64];
      s = HSlot{hp.frame, hp.rng_calls, hp.rng_hash};
      compare_hash(p, hp.frame);
      break;
    }
    case tmnp::PT_Bye:
      if (p >= 0) drop(p, "left");
      break;
    default:
      break;
  }
}

void client_data(transport::ITransport*, const PeerId& peer, const std::vector<uint8_t>& d) {
  if (peer != g_host) return;
  g_host_heard = net::now_ms();
  uint8_t type = tmnp::check_header(d.data(), d.size());
  switch (type) {
    case tmnp::PT_Welcome: {
      if (d.size() < sizeof(tmnp::WelcomePacket)) return;
      tmnp::WelcomePacket w;
      std::memcpy(&w, d.data(), sizeof(w));
      w.name[sizeof(w.name) - 1] = 0;
      g_me = w.player;
      g_late = w.late != 0;
      g_pl[0].name = w.name;
      g_phase = Phase::Lobby;
      g_note = "Вы P" + std::to_string(g_me + 1) +
               (g_late ? ". Игра уже идет: получаем ее с начала..." : ". Получаем сохранение хоста...");
      LOGI("net: joined '%s' as P%d%s", w.name, g_me + 1, g_late ? " (the game runs: catching up)" : "");
      if (g_pre && g_late && !g_cfg.session_at_title) g_go = true;  // (old: a restart into the running game)
      break;
    }
    case tmnp::PT_Reject: {
      if (d.size() < sizeof(tmnp::RejectPacket)) return;
      tmnp::RejectPacket r;
      std::memcpy(&r, d.data(), sizeof(r));
      r.message[sizeof(r.message) - 1] = 0;
      g_error = std::string("Хост отказал: ") + r.message;
      LOGW("net: host refused (%u): %s", r.reason, r.message);
      g_phase = Phase::Ended;
      g_cv.notify_all();
      break;
    }
    case tmnp::PT_Roster: {
      if (d.size() < sizeof(tmnp::RosterPacket)) return;
      tmnp::RosterPacket r;
      std::memcpy(&r, d.data(), sizeof(r));
      for (int p = 0; p < kMax; ++p) {
        r.names[p][31] = 0;
        g_pl[p].present = (r.present >> p) & 1;
        g_pl[p].ready = (r.ready >> p) & 1;
        g_pl[p].name = r.names[p];
        g_pl[p].ping = r.ping_ms[p];
      }
      g_waiting = r.waiting;
      if (g_phase == Phase::Playing && g_me >= 0 && g_me < kMax) adapt_delay(r.lead[g_me]);
      break;
    }
    case tmnp::PT_Record: {
      if (d.size() < sizeof(tmnp::RecordPacket)) return;
      static tmnp::RecordPacket rp;
      std::memcpy(&rp, d.data(), sizeof(rp));
      if (rp.size == tmnp::kRecordSize) {
        std::memcpy(g_rec_session, rp.data, sizeof(g_rec_session));
        g_rec_fresh = rp.fresh;
        g_opt_session[0] = rp.options;
        g_opt_session[1] = rp.difficulty;
        g_rec_have = true;
        LOGI("net: the session's progress came from the host%s", rp.options & 0x80 ? ", with its game options" : "");
      }
      break;
    }
    case tmnp::PT_Go: {
      if (g_pre) {
        LOGI("net: the host starts the network game: restarting into it");
        g_go = true;
      }
      break;
    }
    case tmnp::PT_Save: {
      if (d.size() < sizeof(tmnp::SavePacket)) return;
      tmnp::SavePacket s;
      std::memcpy(&s, d.data(), sizeof(s));
      if (g_save_done) return;
      if (s.total == 0) {
        g_save.clear();
        g_save_done = true;
      } else if (s.total < (8u << 20) && s.offset + s.size <= s.total && s.size <= sizeof(s.data)) {
        if (g_save.size() != s.total) g_save.assign(s.total, 0);
        std::memcpy(g_save.data() + s.offset, s.data, s.size);
        g_save_got += s.size;
        g_save_done = g_save_got >= s.total;  // reliable and ordered: each piece once
      }
      if (!g_save_done) return;
      std::wstring path = module_dir() + wide(kNetSave);
      if (g_save.empty()) {
        DeleteFileW(path.c_str());
      } else {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (f) {
          fwrite(g_save.data(), 1, g_save.size(), f);
          fclose(f);
        }
      }
      LOGI("net: host's save stored (%u bytes) as %s", (unsigned)g_save.size(), kNetSave);
      tmnp::ReadyPacket rp{};
      rp.save_bytes = (uint32_t)g_save.size();
      send_to(g_host_via, g_host, rp, tmnp::PT_Ready, Channel::Control);
      g_note = "Вы P" + std::to_string(g_me + 1) + ". Ждём, когда хост начнёт игру.";
      break;
    }
    case tmnp::PT_Start: {
      if (d.size() < sizeof(tmnp::StartPacket)) return;
      tmnp::StartPacket s;
      std::memcpy(&s, d.data(), sizeof(s));
      g_delay = s.input_delay;
      g_delay_now = 0;
      g_submitted = 0;
      g_players = s.players;
      g_phase = Phase::Playing;
      g_active = !g_cfg.session_at_title;  // (in a running game: once it begins here, at the title menu)
      if (g_cfg.session_at_title) g_begin = true;
      LOGI("net: the game starts: players mask 0x%X, input delay %u frames", g_players, g_delay);
      g_cv.notify_all();
      break;
    }
    case tmnp::PT_Frames: {
      if (d.size() < sizeof(tmnp::FramesPacket)) return;
      tmnp::FramesPacket fp;
      std::memcpy(&fp, d.data(), sizeof(fp));
      for (int i = 0; i < fp.count && i < tmnp::kHistory; ++i) {
        const tmnp::BundleWire& b = fp.items[i];
        g_bundle[b.frame % kRing].frame = b.frame;
        g_bundle[b.frame % kRing].b = b;
        keep(b);
      }
      g_cv.notify_all();
      break;
    }
    case tmnp::PT_History: {
      if (d.size() < sizeof(tmnp::HistoryPacket)) return;
      tmnp::HistoryPacket hp;
      std::memcpy(&hp, d.data(), sizeof(hp));
      for (int i = 0; i < hp.count && i < tmnp::kHistoryItems; ++i) {
        keep(hp.items[i]);
        g_hist_end = std::max(g_hist_end, hp.items[i].frame);
      }
      g_cv.notify_all();
      break;
    }
    case tmnp::PT_Desync: {
      if (d.size() < sizeof(tmnp::DesyncPacket)) return;
      tmnp::DesyncPacket dp;
      std::memcpy(&dp, d.data(), sizeof(dp));
      LOGE("net: DESYNC reported by the host at frame %u (P%d)", dp.frame, dp.player + 1);
      if (dp.player == g_me) sim::keep_logs(g_resyncs == 0 ? L"client" : g_resyncs == 1 ? L"client-2" : L"client-3");
      if (dp.player != g_me) {
        overlay::notify(L"Р" + std::to_wstring(dp.player + 1) + L": рассинхронизация, его игра перезапустится.", 6.0,
                        0xFFFFD040);
      } else if (g_resyncs < kMaxResyncs) {  // (the new copy goes to its title menu and comes back in from there)
        g_restart_rejoin = true;  // the game thread restarts the game into this session
        overlay::notify(L"Рассинхронизация (кадр " + std::to_wstring(dp.frame) +
                            L"): игра перезапускается и догонит остальных.",
                        8.0, 0xFFFFD040);
      } else {
        // Re-entries did not help: this PC computes another game than the others (the logs of every
        // try are kept). It leaves the session - a player does not play on in a game of their own.
        LOGE("net: %d re-entries did not help: leaving the session", g_resyncs);
        g_error = "Игра на этом ПК идёт иначе, чем у остальных, и перезаходы не помогли: вы вышли из сессии.";
        g_phase = Phase::Ended;
        g_cancel_request = true;  // (the network thread says goodbye to the host at once)
        g_cv.notify_all();
      }
      break;
    }
    case tmnp::PT_Bye:
      g_error = "Хост вышел из игры.";
      g_phase = Phase::Ended;
      g_cv.notify_all();
      break;
    default:
      break;
  }
}

void on_event(transport::ITransport* t, const transport::Event& e) {
  if (g_role == Role::Host) {
    if (e.type == transport::EventType::Data) {
      host_data(t, e.peer, e.data);
    } else if (e.type == transport::EventType::Disconnected) {
      int p = player_of(e.peer);
      if (p >= 0) drop(p, e.reason.empty() ? "disconnected" : e.reason.c_str());
    }
    return;
  }
  if (e.peer != g_host) return;
  if (e.type == transport::EventType::Connected) {
    tmnp::HelloPacket h{};
    std::memcpy(h.exe_sha256, g_fp, 32);
    tmnp::put_str(h.mod_version, TMNT2NET_VERSION);
    tmnp::put_str(h.name, g_pl[g_me].name.empty() ? g_cfg.name.c_str() : g_pl[g_me].name.c_str());
    h.player_id = g_cfg.player_id;
    send_to(t, g_host, h, tmnp::PT_Hello, Channel::Control);
    g_host_heard = net::now_ms();
    g_note = "Связь есть, ждём ответа хоста...";
    LOGI("net: connected to %s (%s)", t->Describe(g_host).c_str(), t->Name());
  } else if (e.type == transport::EventType::Disconnected) {
    if (g_phase == Phase::Connecting && t == g_enet.get()) return;  // dialled too early; tick() dials again
    if (g_phase != Phase::Ended) {
      g_error = g_phase == Phase::Playing ? "Связь с хостом потеряна." : "Не удалось подключиться к хосту.";
      LOGW("net: host link lost (%s)", e.reason.c_str());
      g_phase = Phase::Ended;
      g_cv.notify_all();
    }
  } else if (!g_outage) {
    client_data(t, e.peer, e.data);
  }
}

// --- the network thread ----------------------------------------------------------------
std::string my_name() {
  if (!g_cfg.name.empty()) return g_cfg.name;
  if (steam::ready() && !steam::my_name().empty()) return steam::my_name();
  return computer_name();
}

void setup_host() {
  std::string why;
  auto enet = std::make_unique<transport::EnetTransport>(g_cfg.port, kMax - 1, 1000);
  if (enet->Init()) {
    std::vector<net::LocalAddr> ips = net::local_ipv4();
    if (!ips.empty()) g_join_code = joincode::encode(ips[0].ip, g_cfg.port, tmnp::kDefaultPort);
    LOGI("net: ENet listening on port %u (code %s)", g_cfg.port, g_join_code.c_str());
    g_enet = std::move(enet);
  } else {
    LOGW("net: ENet cannot listen on port %u (busy?)", g_cfg.port);
  }
  g_steam_tried = true;
  if (g_cfg.steam_wanted() && steam::boot(g_cfg)) {
    // Anyone before the game starts; once it runs, the lobby's members (players joining the running game
    // or coming back after a drop: they replay it and are in, netplay's Hello decides).
    g_steam_t = transport::make_steam_transport(
        [](uint64_t id) { return g_phase.load() != Phase::Playing || (g_lobby && g_lobby->IsMember(id)); });
    if (g_steam_t && g_steam_t->Init()) {
      g_lobby = lobby::make_host(g_cfg, lobby::exe16(g_fp));
    } else {
      g_steam_t.reset();
    }
  } else if (g_cfg.steam_wanted()) {
    g_steam_note = steam::unavailable_reason();
  }
  std::lock_guard<std::mutex> lk(g_mu);
  g_pl[0] = Player{};
  g_pl[0].present = g_pl[0].ready = true;
  g_pl[0].name = my_name();
  g_me = 0;
  g_phase = Phase::Lobby;
  g_note = "Ждём игроков.";
}

bool dial(transport::ITransport* t, const PeerId& peer) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_host = peer;
  g_host_via = t;
  g_host_heard = net::now_ms();
  if (!t->Connect(peer)) {
    g_error = "Не удалось начать подключение.";
    g_phase = Phase::Ended;
    return false;
  }
  g_note = "Подключение к " + t->Describe(peer) + "...";
  return true;
}

void setup_client() {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_pl[0] = Player{};
    g_phase = Phase::Connecting;
  }
  std::string target = g_cfg.join;
  uint32_t ip = 0;
  uint16_t port = 0;
  bool via_steam = target.empty() || target == "steam" || target == "list" || steam::parse_lobby_id(target) != 0 ||
                   (target.size() >= 16 && target.find_first_not_of("0123456789") == std::string::npos);
  if (!via_steam && joincode::decode(target, tmnp::kDefaultPort, ip, port)) {
    g_enet = std::make_unique<transport::EnetTransport>(0, 1, 1000);
    if (!g_enet->Init()) {
      std::lock_guard<std::mutex> lk(g_mu);
      g_error = "Сеть не запускается (ENet).";
      g_phase = Phase::Ended;
      return;
    }
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_pl[g_me].name = my_name();
    }
    dial(g_enet.get(), PeerId::enet(ip, port));
    return;
  }
  if (!via_steam) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_error = "Не понял, куда подключаться: \"" + target + "\" (код TMNT-..., IP, steam или SteamID).";
    g_phase = Phase::Ended;
    return;
  }
  if (!steam::boot(g_cfg)) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_error = "Steam недоступен: " + steam::unavailable_reason();
    g_phase = Phase::Ended;
    return;
  }
  g_steam_t = transport::make_steam_transport([](uint64_t id) { return id == g_host.id; });
  g_browser = lobby::make_browser(lobby::exe16(g_fp));
  if (!g_steam_t || !g_steam_t->Init() || !g_browser) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_error = "Steam: не удалось открыть сеть.";
    g_phase = Phase::Ended;
    return;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  g_pl[g_me].name = my_name();
  if (uint64_t lobby_id = steam::parse_lobby_id(target)) {
    g_browser->Join(lobby_id);
    g_note = "Входим в лобби Steam...";
  } else if (target == "list") {
    g_note = "Игры друзей в Steam...";  // the network page: the player picks one (lobby_pick)
  } else if (target != "steam" && !target.empty()) {
    g_browser->JoinHost(std::strtoull(target.c_str(), nullptr, 10));
    g_note = "Ищем хоста в Steam...";
  } else {
    g_note = "Ищем игры друзей в Steam...";
  }
}

void client_browse(uint32_t now) {
  if (!g_browser) return;
  bool searching = g_phase == Phase::Connecting && !g_host.valid();
  g_browser->Tick(now, searching);
  g_games = g_browser->List();
  if (uint64_t pick = g_pick_lobby.exchange(0)) {
    if (!g_host.valid()) {
      LOGI("net: the page picked lobby %llu", (unsigned long long)pick);
      g_browser->Join(pick);
    }
  }
  if (searching && (g_cfg.join.empty() || g_cfg.join == "steam")) {
    static uint32_t asked = 0;
    for (const lobby::Found& f : g_browser->List()) {
      if (!f.compatible || now - asked < 3000) continue;  // a running game takes players too
      asked = now;
      LOGI("net: joining the Steam lobby of '%s'", f.host.c_str());
      g_note = "Входим в игру " + f.host + "...";
      g_browser->Join(f.lobby);
      break;
    }
  }
  lobby::JoinResult jr;
  if (g_browser->TakeJoinResult(jr)) {
    if (!jr.ok) {
      LOGW("net: Steam join failed: %s", jr.error.c_str());
      g_note = jr.error + " Ищем дальше...";
      return;
    }
    if (g_host.valid()) return;
    g_host = PeerId::steam(jr.host_id);
    g_host_via = g_steam_t.get();
    g_host_heard = now;
    g_steam_t->Connect(g_host);
    g_note = "Подключение к " + jr.host + " через Steam...";
  }
}

void tick(uint32_t now) {
  static uint32_t last_resend = 0, last_roster = 0;
  bool resend = now - last_resend >= kResendMs;
  if (resend) last_resend = now;
  if (g_role == Role::Host) {
    if (g_lobby) g_lobby->Tick(now);
    for (int p = 1; p < kMax; ++p) {
      if (!g_pl[p].present) continue;
      g_pl[p].ping = g_pl[p].via ? g_pl[p].via->RttMs(g_pl[p].peer) : 0;
      if (now - g_pl[p].last_heard > (uint32_t)g_cfg.timeout_ms) drop(p, "silent too long", g_phase == Phase::Playing);
    }
    uint64_t left = 0;
    while (g_lobby && g_lobby->TakeLeft(left))
      for (int p = 1; p < kMax; ++p)
        if (g_pl[p].present && g_pl[p].peer.kind == transport::Kind::Steam && g_pl[p].peer.id == left)
          drop(p, "left the Steam lobby");
    if (g_start_request && g_phase == Phase::Lobby) {
      g_start_request = false;
      host_start();
    }
    if (g_pre && g_lobby) {
      static uint32_t friends_at = 0;
      if (now - friends_at >= 3000 || g_friends.empty()) {
        friends_at = now;
        g_friends = g_lobby->OnlineFriends();
      }
      if (uint64_t who = g_invite.exchange(0)) {
        g_lobby->Invite(who);
        LOGI("net: Steam invite sent to %llu", (unsigned long long)who);
      }
    }
    if (g_go_request.exchange(false)) {
      tmnp::GoPacket go{};
      g_go_players = 0;
      for (int p = 0; p < kMax; ++p) g_go_players += g_pl[p].present ? 1 : 0;
      go.players = g_go_players;
      send_clients(go, tmnp::PT_Go, Channel::Control);
      g_go_sent = now ? now : 1;
      LOGI("net: the page's start: %u player(s) restart into the network game", g_go_players);
    }
    if (g_phase == Phase::Playing) {
      build_bundles();
      send_bundles(resend);
      pump_history(now);
      // Whom the game waits for (everyone shows it, and the bars of the connection under the boxes).
      static uint32_t wait_frame = 0, wait_since = 0;
      if (wait_frame != g_next_bundle) {
        wait_frame = g_next_bundle;
        wait_since = now;
      }
      uint8_t waiting = 0;
      if (now - wait_since >= kWaitShowMs)
        for (int p = 1; p < kMax; ++p)
          if (takes_part(p, g_next_bundle) && g_in[p][g_next_bundle % kRing].frame != g_next_bundle)
            waiting |= (uint8_t)(1u << p);
      waiting |= g_slow_mask.load();  // (many short waits: the game is slow because of them)
      bool changed = waiting != g_waiting;
      g_waiting = waiting;
      if (changed || now - last_roster >= kRosterMs) {
        last_roster = now;
        send_roster();
      }
    } else if (now - last_roster >= kRosterMs) {
      last_roster = now;
      send_roster();
    }
    return;
  }
  // client
  if (g_cfg.net_outage_for > 0 && g_phase == Phase::Playing) {  // [debug] the link is dead for a while
    static uint32_t began = 0;
    static bool over = false;
    if (!began && !over && g_newest >= (uint32_t)(g_cfg.net_outage_at * 60.0f)) {
      began = now ? now : 1;
      g_outage = true;
      LOGW("[debug] net_outage: the link is dead for %.1f s (newest frame %u)", g_cfg.net_outage_for, g_newest);
    }
    if (began && now - began >= (uint32_t)(g_cfg.net_outage_for * 1000.0f)) {
      began = 0;
      over = true;
      g_outage = false;
      g_host_heard = now;
      LOGW("[debug] net_outage: the link is back");
    }
  }
  client_browse(now);
  if (g_host.valid() && g_phase == Phase::Connecting && g_host_via == g_enet.get() &&
      now - g_host_heard > 3000) {
    // Nobody answers yet: the host may still be starting its game. Keep dialling.
    g_host_heard = now;
    g_enet->Close(g_host);
    g_enet->Connect(g_host);
    g_note = "Хост пока не отвечает (" + g_enet->Describe(g_host) + "), пробуем ещё...";
    return;
  }
  // In a running game the host is waited for longer: a link gone for a while comes back, and the host
  // takes this game back on the spot (return_in_place) - a game that gave up has to join anew and
  // replay the whole session.
  const uint32_t host_wait = g_phase == Phase::Playing ? std::max<uint32_t>((uint32_t)g_cfg.timeout_ms, 30000) : (uint32_t)g_cfg.timeout_ms;
  if (g_host.valid() && g_phase != Phase::Ended && !g_outage && now - g_host_heard > host_wait) {
    g_error = g_phase == Phase::Playing ? "Хост не отвечает." : "Хост не отвечает: проверьте код или адрес.";
    LOGW("net: host silent for %u ms", now - g_host_heard);
    g_phase = Phase::Ended;
    g_cv.notify_all();
  }
  static uint32_t last_alive = 0;
  if (g_phase == Phase::Lobby && g_host.valid() && now - last_alive >= 1000) {
    last_alive = now;
    tmnp::ByePacket a{};
    send_to(g_host_via, g_host, a, tmnp::PT_Alive, Channel::Input);
  }
  if (g_phase == Phase::Playing && !g_mine.empty() && (g_mine_new || resend)) {
    g_mine_new = false;
    tmnp::InputPacket ip{};
    ip.player = (uint8_t)g_me;
    for (auto it = g_mine.rbegin(); it != g_mine.rend() && ip.count < tmnp::kHistory; ++it)
      ip.items[ip.count++] = *it;
    send_to(g_host_via, g_host, ip, tmnp::PT_Input, Channel::Input);
  }
  while (!g_hash_out.empty()) {
    tmnp::HashPacket hp{};
    hp.frame = g_hash_out.front().frame;
    hp.rng_calls = g_hash_out.front().calls;
    hp.rng_hash = g_hash_out.front().hash;
    hp.player = (uint8_t)g_me;
    g_hash_out.pop_front();
    send_to(g_host_via, g_host, hp, tmnp::PT_Hash, Channel::Input);
  }
}

void net_main() {
  net::startup();
  fingerprint(g_fp);
  LOGI("net: game files fingerprint %s", to_hex(g_fp, 8).c_str());
  if (g_role == Role::Host) {
    // The co-op save: the last network game's progress (everybody saved the same there). The
    // first time, the host's own save is where the co-op one starts from.
    const wchar_t* from = nullptr;
    for (const wchar_t* name : {L"tmntsave-net.dat", L"tmntsave.dat"}) {
      FILE* f = _wfopen((module_dir() + name).c_str(), L"rb");
      if (!f) continue;
      fseek(f, 0, SEEK_END);
      long n = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (n > 0 && n < (8 << 20)) {
        g_save.resize((size_t)n);
        g_save.resize(fread(g_save.data(), 1, (size_t)n, f));
      }
      fclose(f);
      if (!g_save.empty()) {
        from = name;
        break;
      }
    }
    if (from && wcscmp(from, L"tmntsave-net.dat") != 0) {
      FILE* f = _wfopen((module_dir() + wide(kNetSave)).c_str(), L"wb");  // the host plays on it too
      if (f) {
        fwrite(g_save.data(), 1, g_save.size(), f);
        fclose(f);
      }
    }
    LOGI("net: co-op save: %ls", from ? from : L"none (everyone starts without one)");
    setup_host();
  } else {
    setup_client();
  }
  transport::Event ev[32];
  uint32_t last_pump = 0;
  while (!g_stop) {
    if (steam::ready() && net::now_ms() - last_pump >= 10) {
      last_pump = net::now_ms();
      steam::pump();
    }
    transport::ITransport* ts[2] = {g_enet.get(), g_steam_t.get()};
    for (transport::ITransport* t : ts) {
      if (!t) continue;
      t->Poll(0);
      int n = t->Receive(ev, 32);
      std::lock_guard<std::mutex> lk(g_mu);
      for (int i = 0; i < n; ++i) on_event(t, ev[i]);
    }
    {
      std::lock_guard<std::mutex> lk(g_mu);
      tick(net::now_ms());
      if (g_cancel_request) break;
    }
    Sleep(1);
  }
  // Goodbye: the others learn at once instead of after a timeout.
  {
    std::lock_guard<std::mutex> lk(g_mu);
    tmnp::ByePacket b{};
    if (g_role == Role::Host)
      send_clients(b, tmnp::PT_Bye, Channel::Control);
    else if (g_host.valid())
      send_to(g_host_via, g_host, b, tmnp::PT_Bye, Channel::Control);
  }
  for (int i = 0; i < 20; ++i) {
    if (g_enet) g_enet->Poll(5);
    if (g_steam_t) g_steam_t->Poll(5);
  }
  g_lobby.reset();
  g_browser.reset();
  if (g_steam_t) g_steam_t->Shutdown();
  if (g_enet) g_enet->Shutdown();
  g_steam_t.reset();
  g_enet.reset();
  if (!g_pre && !g_keep_steam) steam::shutdown();  // (the page's lobby: Steam stays for invites)
  LOGI("net: stopped");
}

void start(Role role) {
  g_cancel_request = false;
  g_role = role;
  g_phase = role == Role::Host ? Phase::Lobby : Phase::Connecting;
  g_stop = false;
  g_thread = std::thread(net_main);
}

void stop() {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_cancel_request = true;
  }
  g_stop = true;
  if (g_thread.joinable()) g_thread.join();
  g_active = false;
}

// --- the lobby on screen (game thread) ------------------------------------------------
struct Keys {
  bool enter = false, esc = false, back = false;
  std::string typed;
  int digit = -1;
};

// Pumps the window's messages; false when the game is to quit (WM_QUIT re-posted).
bool pump(Keys* keys) {
  MSG m;
  while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
    if (m.message == WM_QUIT) {
      PostQuitMessage((int)m.wParam);
      return false;
    }
    if (keys && m.message == WM_KEYDOWN) {
      if (m.wParam == VK_RETURN) keys->enter = true;
      if (m.wParam == VK_ESCAPE) keys->esc = true;
      if (m.wParam == VK_BACK) keys->back = true;
      if (m.wParam >= '1' && m.wParam <= '9') keys->digit = (int)(m.wParam - '0');
    }
    if (keys && m.message == WM_CHAR && m.wParam >= 32 && m.wParam < 127) keys->typed += (char)m.wParam;
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
  return true;
}

bool pad_start_pressed() {
  static bool was = false;
  bool now = false;
  for (int i = 0; i < sim::kPads; ++i) now = now || (pads::sample(i).buttons & (1u << 9)) != 0;  // START
  bool edge = now && !was;
  was = now;
  return edge;
}

std::vector<overlay::Line> lobby_lines(bool menu, bool typing, const std::string& code) {
  using overlay::Line;
  std::vector<Line> l;
  // The game's menu colours: orange titles, yellow items, green for this player.
  const uint32_t kTitle = 0xFFFFA000, kItem = 0xFFFFFF00, kDim = 0xFFB0B0B0, kWarn = 0xFFFF8060;
  l.push_back({L"СЕТЕВАЯ ИГРА", kTitle, true});
  if (menu) {
    if (typing) {
      l.push_back({L"Код хоста (TMNT-....) или IP:", kItem, false});
      l.push_back({wide(code) + L"_", 0xFFFFD040, true});
      l.push_back({L"Enter — подключиться    Esc — назад", kDim, false});
    } else {
      l.push_back({L"1 — СОЗДАТЬ ИГРУ (ВЫ ХОСТ)", kItem, false});
      l.push_back({L"2 — ВОЙТИ В ИГРУ ДРУГА ЧЕРЕЗ STEAM", kItem, false});
      l.push_back({L"3 — ВОЙТИ ПО КОДУ ИЛИ IP (LAN, ИНТЕРНЕТ)", kItem, false});
      l.push_back({L"Esc — играть одному", kDim, false});
    }
    return l;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  // After the network page's start every game comes here by itself: just who is back.
  if (g_cfg.auto_start > 0 || g_cfg.join_retry_s > 0) {
    l.push_back({L"Сетевая игра запускается...", kItem, false});
    for (int p = 0; p < kMax; ++p)
      if (g_pl[p].present)
        l.push_back({L"P" + std::to_wstring(p + 1) + L"  " + wide(g_pl[p].name), p == g_me ? 0xFF80FF80 : 0xFFFFFFFF,
                     false});
    if (!g_error.empty()) l.push_back({wide(g_error), kWarn, false});
    l.push_back({L"Esc — отменить и играть одному", kDim, false});
    return l;
  }
  if (g_role == Role::Host) {
    l.push_back({L"Вы хост. Остальные входят до начала игры.", 0xFFFFFFFF, false});
    if (!g_join_code.empty()) l.push_back({L"Код для LAN / IP: " + wide(g_join_code), 0xFFFFD040, false});
    if (g_lobby)
      l.push_back({L"Steam: друзья входят через «Присоединиться к игре» в списке друзей", 0xFFFFFFFF, false});
    else if (!g_steam_note.empty())
      l.push_back({L"Steam недоступен: " + wide(g_steam_note), kWarn, false});
  } else {
    l.push_back({wide(g_note), 0xFFFFFFFF, false});
  }
  if (!g_error.empty()) l.push_back({wide(g_error), kWarn, false});
  for (int p = 0; p < kMax; ++p) {
    if (!g_pl[p].present) continue;
    std::wstring s = L"P" + std::to_wstring(p + 1) + L"  " + wide(g_pl[p].name);
    if (p == g_me) s += L"  (вы)";
    if (p != 0 && !g_pl[p].ready) s += L"  — получает сохранение";
    if (p != 0 && g_pl[p].ping) s += L"  " + std::to_wstring(g_pl[p].ping) + L" мс";
    l.push_back({s, p == g_me ? 0xFF80FF80 : 0xFFFFFFFF, false});
  }
  if (g_role == Role::Host) {
    int n = 0;
    for (int p = 0; p < kMax; ++p) n += g_pl[p].present ? 1 : 0;
    l.push_back({L"Enter / Start — начать (игроков: " + std::to_wstring(n) + L"; другие смогут войти и потом)    Esc — "
                 L"играть без сети",
                 kDim, false});
  } else {
    l.push_back({L"Esc — отменить и играть одному", kDim, false});
  }
  return l;
}

// A network game plays on the co-op save on every PC: the game's save path names
// tmntsave-net.dat (the host sent its copy at the start). The own save stays untouched.
// After a session in a running game: the game saves to its own file again.
void own_save_name() {
  uint8_t now[5];
  std::memcpy(now, reinterpret_cast<const void*>(game::kSaveNamePush), 5);
  if (std::memcmp(now, game::kSaveNamePushOld, 5) != 0)
    patch::write(game::kSaveNamePush, game::kSaveNamePushOld, sizeof(game::kSaveNamePushOld));
}

void use_net_save() {
  static char name[sizeof(kNetSave)];
  std::memcpy(name, kNetSave, sizeof(kNetSave));
  uint32_t ptr = (uint32_t)(uintptr_t)name;
  uint8_t bytes[5] = {0x68, 0, 0, 0, 0};
  std::memcpy(bytes + 1, &ptr, 4);
  patch::replace("save file name (co-op save, own one untouched)", game::kSaveNamePush, game::kSaveNamePushOld, bytes,
                 sizeof(bytes));
}

// Our game went another way than the host's: start the game again, joined to the same host. The
// host keeps our place (the same name); the new copy replays the session and is in once it has
// caught up. Game thread (it quits this copy).
// A new copy of the game joined to `target` (it waits for this one to close); this one quits.
// Game thread.
bool restart_joined(const std::string& target, const std::wstring& extra) {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" -tmnt2net-net=join -tmnt2net-join=" +
                     std::wstring(target.begin(), target.end()) + extra + L" -tmnt2net-after=" +
                     std::to_wstring(GetCurrentProcessId());
  std::wstring dir = module_dir();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> line(cmd.begin(), cmd.end());
  line.push_back(0);
  if (!CreateProcessW(exe, line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
    LOGE("net: cannot restart the game joined to %s (error %lu)", target.c_str(), GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  LOGI("net: the game restarts joined to %s (new pid %lu)", target.c_str(), pi.dwProcessId);
  fixes::request_quit();
  return true;
}

// The page's start, host: this game again as the session's host, waiting for `players`.
bool restart_as_host(int players) {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" -tmnt2net-net=host -tmnt2net-auto=" + std::to_wstring(players) +
                     L" -tmnt2net-after=" + std::to_wstring(GetCurrentProcessId());
  std::wstring dir = module_dir();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> line(cmd.begin(), cmd.end());
  line.push_back(0);
  if (!CreateProcessW(exe, line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
    LOGE("net: cannot restart the game as the host (error %lu)", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  LOGI("net: the game restarts as the network game's host for %d player(s) (new pid %lu)", players, pi.dwProcessId);
  fixes::request_quit();
  return true;
}

void make_player_id() {
  if (g_cfg.player_id) return;
  LARGE_INTEGER t{};
  QueryPerformanceCounter(&t);
  uint64_t x = (uint64_t)t.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ GetTickCount64() * 0x9E3779B97F4A7C15ull;
  x ^= x >> 31;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 29;
  g_cfg.player_id = x ? x : 1;
  wchar_t v[24];
  swprintf(v, 24, L"%016llx", (unsigned long long)g_cfg.player_id);
  WritePrivateProfileStringW(L"net", L"player_id", v, (module_dir() + L"tmnt2net.ini").c_str());
  LOGI("net: this PC's player id made and kept in tmnt2net.ini");
}

void reset_lobby_state() {
  std::lock_guard<std::mutex> lk(g_mu);
  // A game takes part in session after session without a restart (0.14): nothing of the session before may
  // be left. Its frames were: a game that had played found the old session's frame N under the new one's
  // number N and played that at once, without waiting for the host - another game than everybody else's
  // from the first frames (0.14 - 0.16.5: "desync" for whoever had already played since their game's start).
  for (auto& row : g_in)
    for (InSlot& s : row) s = InSlot{};
  for (BSlot& s : g_bundle) s = BSlot{};
  g_next_bundle = 1;
  g_all.clear();
  g_newest = 0;
  g_fast = false;
  g_hurry = false;
  g_boot = false;
  g_boot_press = 0;
  g_boot_last = 0;
  g_reliable_sent = 0;
  g_sent_bundle = 0;
  g_hist_end = 0;
  g_late_in = false;
  g_mine.clear();
  g_mine_new = false;
  for (auto& row : g_hash)
    for (HSlot& s : row) s = HSlot{};
  g_hash_out.clear();
  g_waiting = 0;
  g_slow_mask = 0;
  g_players = 1;
  g_start_request = false;
  g_rec_have = false;
  g_game_frame = 0;
  g_delay_now = 0;
  g_submitted = 0;
  g_wait_acc = 0;
  g_delay_calm = 0;
  g_sync_frames = 0;
  for (Player& p : g_pl) p = Player{};
  g_me = 0;
  g_note.clear();
  g_error.clear();
  g_host = PeerId{};
  g_host_via = nullptr;
  g_save.clear();
  g_save_total = g_save_got = 0;
  g_save_done = false;
  g_late = false;
  g_games.clear();
  g_friends.clear();
  g_invite = 0;
  g_join_code.clear();
  g_go = false;
  g_go_request = false;
  g_go_sent = 0;
  g_pick_lobby = 0;
}

void restart_rejoin() {
  std::string target;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_host.valid()) return;
    target = g_host.kind == transport::Kind::Steam ? std::to_string(g_host.id) : transport::to_string(g_host);
  }
  LOGI("net: desync: back into the session (attempt %d)", g_resyncs + 1);
  restart_joined(target, L" -tmnt2net-resync=" + std::to_wstring(g_resyncs + 1));
}

}  // namespace

void prepare(Config& cfg) {
  // A restart from the game's СЕТЕВАЯ ИГРА item: "-tmnt2net-net=menu -tmnt2net-after=<pid>".
  // The new copy waits until the old one has gone (its window would make the game exit).
  std::wstring cl = GetCommandLineW();
  size_t at = cl.find(L"-tmnt2net-after=");
  if (at != std::wstring::npos) {
    DWORD pid = (DWORD)wcstoul(cl.c_str() + at + 16, nullptr, 10);
    if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
      WaitForSingleObject(old, 15000);
      CloseHandle(old);
    }
  }
  at = cl.find(L"-tmnt2net-resync=");  // a restart after a desync (restart_rejoin)
  if (at != std::wstring::npos) {
    g_resyncs = (int)wcstol(cl.c_str() + at + 17, nullptr, 10);
    cfg.rng_mutant_frame = 0;  // [debug] the desync test makes one desync, not one per restart
  }
  at = cl.find(L"-tmnt2net-auto=");  // the page's start: the host waits for this many players
  if (at != std::wstring::npos) cfg.auto_start = (int)wcstol(cl.c_str() + at + 15, nullptr, 10);
  at = cl.find(L"-tmnt2net-retry=");  // the page's start: the host restarts too, keep trying
  if (at != std::wstring::npos) cfg.join_retry_s = (int)wcstol(cl.c_str() + at + 16, nullptr, 10);
  at = cl.find(L"-tmnt2net-join=");
  if (at != std::wstring::npos) {
    size_t end = cl.find(L' ', at);
    std::wstring v = cl.substr(at + 15, end == std::wstring::npos ? std::wstring::npos : end - at - 15);
    cfg.join.clear();
    for (wchar_t c : v) cfg.join += (char)c;  // digits, dots, a colon
  }
  at = cl.find(L"-tmnt2net-net=");
  if (at != std::wstring::npos) {
    std::wstring v = cl.substr(at + 14, cl.find(L' ', at) == std::wstring::npos ? std::wstring::npos
                                                                                : cl.find(L' ', at) - at - 14);
    if (v == L"menu") cfg.net_mode = "menu";
    if (v == L"host") cfg.net_mode = "host";
    if (v == L"join") cfg.net_mode = "join";
  }
  // Sessions begin at the title menu of a running game: every game runs ready for one.
  if (!cfg.net_on() && !cfg.session_at_title) return;
  cfg.determinism = true;  // same input, same game (a game alone too: a session may begin in it)
  if (cfg.net_on()) cfg.background_run = true;  // (during a session the window keeps the game running anyway)
  cfg.net_aspect = 16.0f / 9.0f;  // the camera is logic: one aspect for every PC
  cfg.view_fov = cfg.fov;         // the player's own wide angle: for the draw only (camera.cpp)
  cfg.fov = 1.0f;
  // The cameras of every game that may be in a session are the session's, whatever picture the player
  // chose: with [video] aspect = 4:3 or stretch the widescreen code was off, the stage camera of that PC
  // was another one than everybody else's, and its game parted from the others a moment after a stage
  // began (0.14 - 0.16.5). The choice is the shape of the frame now (cfg.shape, video.cpp).
  cfg.aspect = Config::Aspect::Wide;
  // And no exclusive fullscreen: there the game stops while its window is not in front (Alt+Tab), every
  // PC of a session waits for it, and after the device comes back its frames are drawn late - drawn
  // effects take random numbers, so that game would part from the others. The whole monitor without a
  // frame is the same picture and keeps running. (The old start into a session did this already.)
  if (cfg.mode == "fullscreen") cfg.mode = "borderless";
  if (cfg.timeout_ms < 2000) cfg.timeout_ms = 2000;
  if (!cfg.net_on()) return;
  cfg.shape = Config::Aspect::Wide;  // (the old start into a session: the monitor's shape)
  cfg.aspect = Config::Aspect::Wide;
}

void install(const Config& cfg) {
  g_cfg = cfg;
  if (!cfg.net_on()) return;
  if (!g_cfg.player_id) {  // made once per PC, kept: the host knows this player again by it
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    uint64_t x = (uint64_t)t.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ GetTickCount64() * 0x9E3779B97F4A7C15ull;
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 29;
    g_cfg.player_id = x ? x : 1;
    wchar_t v[24];
    swprintf(v, 24, L"%016llx", (unsigned long long)g_cfg.player_id);
    WritePrivateProfileStringW(L"net", L"player_id", v, (module_dir() + L"tmnt2net.ini").c_str());
    LOGI("net: this PC's player id made and kept in tmnt2net.ini");
  }
  LOGI("net: mode %s%s%s, transport %s", cfg.net_mode.c_str(), cfg.join.empty() ? "" : ", join ",
       cfg.join.c_str(), cfg.transport.c_str());
  g_phase = Phase::Menu;
  if (g_cfg.session_at_title) {  // the network page's lobby, once the game is at its title menu (lobby_tick)
    g_auto_mode = cfg.net_mode;
    return;
  }
  if (cfg.net_mode == "host") start(Role::Host);
  if (cfg.net_mode == "join") start(Role::Client);
}

bool configured() { return g_cfg.net_on(); }

void idle_tick() {
  static bool tried = false;
  static std::unique_ptr<lobby::IInvites> invites;
  static DWORD last = 0;
  if (g_pre) {  // a lobby of the page is open: the network thread has Steam; no invite kept for later
    invites.reset();
    return;
  }
  if ((configured() && !g_cfg.session_at_title) || !g_cfg.steam_invites || g_cfg.no_activate || active())
    return;  // (no_activate: test runs)
  if (!tried) {
    tried = true;
    bool on = steam::boot(g_cfg);
    g_steam_tried = true;
    if (on) invites = lobby::make_invites();
    LOGI("net: Steam invites while playing alone: %s", invites ? "on" : "off (no Steam)");
  }
  if (!invites && steam::ready()) invites = lobby::make_invites();  // (again after a lobby)
  DWORD now = GetTickCount();
  if (!invites || now - last < 50) return;
  last = now;
  steam::pump();
  uint64_t lobby_id = 0;
  if (!invites->Take(lobby_id)) return;
  // Into the friend's game here first (its network page lobby, or its running game: then a restart).
  overlay::notify(L"Приглашение Steam: входим в игру друга...", 5.0, 0xFF80FF80);
  invites.reset();  // (the network thread pumps Steam from now on)
  lobby_open_client(std::to_string(lobby_id));
}

// The co-op progress is kept two ways: tmnt2net-coop.rec, written when a session ends at the title menu,
// and the game's own co-op save tmntsave-net.dat, which the game writes whenever it saves in a session.
// A game closed in the middle of a session (or one that crashed) leaves only the save newer, and the next
// session began without the missions played. Then the save's progress goes into the .rec: the game's own
// loader reads the save's record into the live record for a moment (game thread, at the title, no session).
void import_coop_save() {
  const std::wstring save = module_dir() + wide(kNetSave), rec = coop_record_path();
  WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
  if (!GetFileAttributesExW(save.c_str(), GetFileExInfoStandard, &a)) return;
  if (GetFileAttributesExW(rec.c_str(), GetFileExInfoStandard, &b) && CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) <= 0)
    return;  // (the .rec is the newer one: a session that ended as it should)
  std::vector<uint8_t> file(game::kSaveHeader + game::kSaveData);
  FILE* f = _wfopen(save.c_str(), L"rb");
  if (!f) return;
  size_t n = fread(file.data(), 1, file.size() + 0, f);
  bool more = fgetc(f) != EOF;
  fclose(f);
  uint32_t version = 0;
  std::memcpy(&version, file.data(), 4);
  uint8_t* r = record();
  if (n != file.size() || more || version != 3 || !r || g_session_on ||
      !patch::bytes_are(game::kRecordRestore, game::kRecordRestoreHead, sizeof(game::kRecordRestoreHead)) ||
      !patch::bytes_are(game::kAreaIsNewGame, game::kAreaIsNewGameHead, sizeof(game::kAreaIsNewGameHead))) {
    LOGW("net: tmntsave-net.dat is newer than the co-op progress but cannot be read here (%u bytes, version %u)",
         (unsigned)n, version);
    return;
  }
  std::vector<uint8_t> own(r, r + tmnp::kRecordSize);
  reinterpret_cast<void(__fastcall*)(uint8_t*, void*, const uint8_t*, int)>(game::kRecordRestore)(
      r, nullptr, file.data() + game::kSaveHeader + game::kSaveRecord, 1);
  uint8_t fresh = reinterpret_cast<uint8_t(__fastcall*)(uint8_t*, void*)>(game::kAreaIsNewGame)(r + game::kRecordArea, nullptr) & 1;
  f = _wfopen(rec.c_str(), L"wb");
  if (f) {
    fwrite(r, 1, tmnp::kRecordSize, f);
    fwrite(&fresh, 1, 1, f);
    fclose(f);
  }
  std::memcpy(r, own.data(), own.size());  // our own progress again
  LOGI("net: the co-op progress comes from the game's co-op save tmntsave-net.dat (the last session did not end at the "
       "title menu): %s", fresh ? "no missions in it yet" : "the missions played are there");
}

bool lobby_open(Role role, const std::string& target) {
  if (g_pre) return false;
  if (role == Role::Host) import_coop_save();
  make_player_id();
  reset_lobby_state();
  g_cfg.join = target;
  g_pre = true;
  start(role);
  LOGI("net: the network page's lobby: %s%s", role == Role::Host ? "host" : "client, ", target.c_str());
  return true;
}

bool lobby_open_host() { return lobby_open(Role::Host, ""); }
bool lobby_open_client(const std::string& target) { return lobby_open(Role::Client, target); }
void lobby_pick(uint64_t lobby_id) { g_pick_lobby = lobby_id; }
void lobby_start() {
  if (!g_pre || g_role != Role::Host) return;
  if (g_cfg.session_at_title) {  // the session begins here, at this title menu, and in every player's game
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_phase.load() == Phase::Lobby) g_start_request = true;
    return;
  }
  g_go_request = true;
}
bool lobby_running() { return g_pre; }
void lobby_invite(uint64_t friend_id) { g_invite = friend_id; }

SteamInfo steam_info() {
  SteamInfo s;
  if (!g_cfg.steam_wanted()) {
    s.state = 2;
    s.reason = "выключен в tmnt2net.ini";
  } else if (steam::ready()) {
    s.state = 1;
    s.name = steam::my_name();
  } else if (g_steam_tried) {
    s.state = 2;
    s.reason = steam::unavailable_reason();
  }
  return s;
}

void lobby_close() {
  if (!g_pre) return;
  stop();
  g_pre = false;
  g_role = Role::None;
  g_phase = Phase::Menu;
  LOGI("net: the network page's lobby closed");
}

LobbyView lobby_view() {
  LobbyView v;
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_pre) return v;
  v.role = g_role == Role::Host ? 1 : g_role == Role::Client ? 2 : 0;
  v.joined = g_role == Role::Client && (g_phase == Phase::Lobby || g_phase == Phase::Playing);
  v.failed = g_phase == Phase::Ended;
  v.steam = steam::ready();
  v.error = g_error;
  v.code = g_join_code;
  v.host_name = g_role == Role::Client ? g_pl[0].name : "";
  v.browsing = g_role == Role::Client && g_cfg.join == "list" && !g_host.valid() && g_phase == Phase::Connecting;
  v.note = g_note;
  v.can_invite = g_role == Role::Host && g_lobby != nullptr;
  for (const lobby::Friend& f : g_friends) v.friends.push_back(LobbyFriend{f.id, f.name, f.playing_480});
  for (int p = 0; p < kMax; ++p) {
    v.players[p].present = g_pl[p].present;
    v.players[p].ready = g_pl[p].ready;
    v.players[p].me = p == g_me;
    v.players[p].name = g_pl[p].name;
    v.players[p].ping = g_pl[p].ping;
  }
  for (const lobby::Found& f : g_games) {
    LobbyGame g;
    g.lobby = f.lobby;
    g.host = f.host.empty() ? f.name : f.host;
    g.running = f.state == "ingame";
    g.compatible = f.compatible;
    g.why_not = f.why_not;
    v.games.push_back(g);
  }
  return v;
}

// Sessions in a running game: the lobby a command line asked for, once at the title menu; after a
// session, back at the title menu, this game's own progress, save file and menu again.
void session_tick() {
  if (!g_auto_mode.empty() && menus::title_menu_up()) {
    std::string m = g_auto_mode;
    g_auto_mode.clear();
    if (m == "host") lobby_open_host();
    if (m == "join") lobby_open_client(g_cfg.join.empty() ? "list" : g_cfg.join);
    if (m == "menu") netpage::open();
    if (m == "host" && g_cfg.auto_start > 0) LOGI("net: the host starts once %d player(s) are in", g_cfg.auto_start);
  }
  if (g_cfg.auto_start > 0 && g_pre && g_role == Role::Host && menus::title_menu_up()) {
    std::lock_guard<std::mutex> lk(g_mu);
    int ready = 0;
    for (int p = 0; p < kMax; ++p) ready += g_pl[p].present && g_pl[p].ready ? 1 : 0;
    if (g_phase.load() == Phase::Lobby && ready >= g_cfg.auto_start) g_start_request = true;
  }
  if (g_session_on && !g_active && !g_begin.load() && menus::title_menu_up()) {
    g_session_on = false;
    if (uint8_t* r = record()) {
      FILE* f = _wfopen(coop_record_path().c_str(), L"wb");  // the co-op progress, for the next session
      if (f) {
        fwrite(r, 1, tmnp::kRecordSize, f);
        uint8_t fresh = patch::read<uint8_t>(game::kNewGameFlag);
        fwrite(&fresh, 1, 1, f);
        fclose(f);
      }
      if (g_rec_own_kept) {
        std::memcpy(r, g_rec_own, tmnp::kRecordSize);
        patch::write(game::kNewGameFlag, &g_rec_own_fresh, 1);
      }
    }
    if (g_opt_own_kept) write_options(g_opt_own);
    g_opt_own_kept = false;
    g_rec_own_kept = false;
    own_save_name();
    menus::on_session_end();
    if (g_pre) lobby_close();
    LOGI("net: the session is over here: our own progress, save and title menu again");
  }
}

void leave_session() {
  if (!g_session_on || !g_active) return;
  LOGI("net: the session is ended from its title menu");
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_error = "Сетевая игра закончена.";
    g_phase = Phase::Ended;
  }
  g_cv.notify_all();
}

uint8_t g_opt_through_load[2] = {0, 0};

void options_before_restore() {
  g_opt_through_load[0] = 0;
  if (g_session_on) read_options(g_opt_through_load);
}

void options_after_restore() {
  if (g_session_on && (g_opt_through_load[0] & 0x80)) write_options(g_opt_through_load);
}

void lobby_tick() {
  session_tick();
  if (!g_pre) return;
  if (g_go.exchange(false)) {  // client: the host started, or its game runs already
    std::string target;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      if (g_host.valid())
        target = g_host.kind == transport::Kind::Steam ? std::to_string(g_host.id) : transport::to_string(g_host);
    }
    if (!target.empty()) {
      overlay::notify(L"Игра начинается: перезапуск в сетевую игру...", 5.0, 0xFF80FF80);
      restart_joined(target, L" -tmnt2net-retry=45");
    }
    return;
  }
  uint32_t sent = g_go_sent.load();
  if (g_role == Role::Host && sent && net::now_ms() - sent >= 400) {  // PT_Go is out: this game goes too
    g_go_sent = 0;
    overlay::notify(L"Игра начинается: перезапуск в сетевую игру...", 5.0, 0xFF80FF80);
    restart_as_host(std::max<int>(1, g_go_players));
  }
}
bool active() { return g_active; }
uint32_t input_delay() { return g_delay; }
int my_player() { return g_me; }

void lobby() {
  if (!configured()) return;
  if (g_cfg.session_at_title) {  // the game goes on; the session begins at its title menu (take_begin)
    LOGI("net: [debug] session_at_title: the game runs on, the session begins at the title menu");
    return;
  }
  LOGI("net: the game waits at its first frame for the session");
  bool menu = g_role == Role::None, typing = false;
  // After the page's start the host restarts too: until it is back, try again.
  ULONGLONG retry_until = g_cfg.join_retry_s > 0 ? GetTickCount64() + (ULONGLONG)g_cfg.join_retry_s * 1000 : 0;
  ULONGLONG last_try = GetTickCount64();
  bool ever_joined = false;
  std::string code;
  ULONGLONG last_present = 0;
  while (true) {
    Keys k;
    if (!pump(&k)) {  // the window was closed
      stop();
      overlay::clear_panel();
      return;
    }
    bool start_key = k.enter || pad_start_pressed();
    if (menu) {
      if (typing) {
        for (char c : k.typed)
          if (std::isalnum((unsigned char)c) || c == '.' || c == ':' || c == '-') code += (char)toupper(c);
        if (k.back && !code.empty()) code.pop_back();
        if (k.esc) typing = false;
        if (k.enter && !code.empty()) {
          g_cfg.join = code;
          menu = false;
          start(Role::Client);
        }
      } else {
        if (k.digit == 1) {
          menu = false;
          start(Role::Host);
        } else if (k.digit == 2) {
          g_cfg.join = "steam";
          menu = false;
          start(Role::Client);
        } else if (k.digit == 3) {
          typing = true;
        } else if (k.esc) {
          break;
        }
      }
    } else {
      Phase ph = g_phase;
      if (ph == Phase::Playing) break;
      if (k.esc) break;
      if (g_role == Role::Host && ph == Phase::Lobby) {
        int ready = 0, present = 0;
        {
          std::lock_guard<std::mutex> lk(g_mu);
          for (int p = 0; p < kMax; ++p) {
            present += g_pl[p].present ? 1 : 0;
            ready += g_pl[p].present && g_pl[p].ready ? 1 : 0;
          }
          // The host may start alone too: friends join the running game, straight into the team.
          bool auto_go = g_cfg.auto_start > 0 && ready >= g_cfg.auto_start;
          if ((start_key && present >= 1 && ready == present) || auto_go) g_start_request = true;
          if (start_key && present > 1 && ready < present) g_note = "Подождите: не все получили сохранение.";
        }
      }
      if (g_role == Role::Client && ph == Phase::Lobby) ever_joined = true;
      if (g_role == Role::Client && ph == Phase::Ended && !ever_joined && retry_until &&
          GetTickCount64() < retry_until && GetTickCount64() - last_try >= 2000) {
        last_try = GetTickCount64();
        LOGI("net: the host's game is not there yet: trying again");
        g_keep_steam = true;
        stop();
        g_keep_steam = false;
        reset_lobby_state();
        start(Role::Client);
      } else if (g_role == Role::Client && ph == Phase::Ended) {
        // Shown until Esc; the game then goes on alone.
      }
    }
    ULONGLONG now = GetTickCount64();
    if (now - last_present >= 33) {
      last_present = now;
      overlay::set_panel(lobby_lines(menu, typing, code), true);
      video::present_now(true);
    } else {
      Sleep(2);
    }
  }
  overlay::clear_panel();
  if (g_phase == Phase::Playing) {
    use_net_save();  // everybody, the host too: the co-op save
    menus::on_session_start();
    LOGI("net: session on, P%d, input delay %u", g_me + 1, g_delay);
    return;
  }
  LOGI("net: no session; the game goes on alone");
  stop();
}

// The game thread's input of this frame goes to a later one: `frame` is the frame being played.
void submit(uint32_t frame, const sim::PadInput& pad, uint16_t movie, uint8_t flags) {
  tmnp::InputWire in{};
  in.pad.buttons = pad.buttons;
  in.pad.dpad = pad.dpad;
  in.pad.connected = 1;
  in.pad.lx = pad.lx;
  in.pad.ly = pad.ly;
  in.pad.rx = pad.rx;
  in.pad.ry = pad.ry;
  in.movie = movie;
  in.flags = flags;
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_role == Role::Host) {
    in.frame = frame + g_delay;
    g_in[0][in.frame % kRing].frame = in.frame;
    g_in[0][in.frame % kRing].in = in;  // the network thread builds and sends the bundle
    return;
  }
  // A client's delay is its own and changes (adapt_delay). Every frame gets exactly one input, in order:
  // when the delay grows the frames between get this input too; when it shrinks this sample is left
  // out (its frame has one already).
  uint32_t d = g_delay_now.load();
  uint32_t want = frame + (d ? d : g_delay);
  if (!g_submitted || want > g_submitted + 60 || want + 60 < g_submitted) g_submitted = want - 1;  // (a session's first)
  for (uint32_t f = g_submitted + 1; f <= want; ++f) {
    in.frame = f;
    g_mine.push_back(in);
    g_mine_new = true;
  }
  if (want > g_submitted) g_submitted = want;
  while (g_mine.size() > tmnp::kHistory) g_mine.pop_front();
}

// A replayed session is not heard: its sounds, played ten and more times as fast, are noise. The game's
// own volume setters, 0 for the replay; then the player's volumes again (the options' own Apply).
void quiet(bool on) {
  static bool is = false;
  if (on == is) return;
  is = on;
  if (on) {
    for (uintptr_t f : game::kSoundVolume)
      if (patch::bytes_are(f, game::kSoundVolumeHead, sizeof(game::kSoundVolumeHead)))
        reinterpret_cast<void(__cdecl*)(int)>(f)(0);
  } else if (uint8_t* o = patch::read<uint8_t*>(game::kOptionDataPtr)) {
    if (patch::bytes_are(game::kSoundOptionApply, game::kSoundOptionApplyHead, sizeof(game::kSoundOptionApplyHead)))
      reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(game::kSoundOptionApply)(o + game::kOptSound, nullptr);
  }
}

// A network game goes at the pace of its slowest player: every frame waits for everybody's input. Short
// waits show no "waiting" panel, the game just runs slow, and nobody can tell why. Counted here (game
// thread): how long the frames waited and for whom; said in the log and on screen when the game is slow.
struct Slow {
  LARGE_INTEGER freq{}, since{};
  uint32_t frames = 0;
  double waited = 0, by[kMax] = {};  // ms: all; host: per player whose input was missing
  DWORD told = 0;
  int run = 0;  // slow stretches in a row before this one
} g_slow;
double slow_ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
  return g_slow.freq.QuadPart ? (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)g_slow.freq.QuadPart : 0.0;
}

void slow_report(uint32_t frame) {  // (g_mu not held)
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!g_slow.since.QuadPart) g_slow.since = now;
  const uint32_t kFrames = 300;  // five seconds of game
  if (++g_slow.frames < kFrames) return;
  double span = slow_ms(g_slow.since, now);
  uint8_t mask = 0;
  int worst = -1;
  for (int p = 1; p < kMax; ++p)
    if (g_slow.by[p] > span * 0.2) {
      mask |= (uint8_t)(1u << p);
      if (worst < 0 || g_slow.by[p] > g_slow.by[worst]) worst = p;
    }
  if (g_role == Role::Host) g_slow_mask = mask;
  bool slow = span > kFrames * 1000.0 / 60.0 * 1.3 && !g_fast && !g_hurry;  // under ~46 frames a second
  if (slow) {
    std::string who;
    std::wstring line;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      uint8_t m = g_role == Role::Host ? mask : g_waiting;
      for (int p = 1; p < kMax; ++p)
        if ((m >> p) & 1) {
          char b[96];
          snprintf(b, sizeof(b), "%sP%d %s %.0f ms", who.empty() ? "" : ", ", p + 1, g_pl[p].name.c_str(), g_slow.by[p]);
          who += b;
          if (p == g_me)
            line = L"Игра идёт медленно: все ждут ваш ПК или вашу связь.";
          else if (line.empty())
            line = L"Игра идёт медленно: ждём игрока Р" + std::to_wstring(p + 1) + L" " + wide(g_pl[p].name) +
                   L" (слабая связь или медленный ПК).";
        }
    }
    bool net = g_slow.waited > span * 0.2;
    LOGW("net: slow: %u frames in %.1f s (%.0f a second); the frames waited %.1f s for the network%s%s", kFrames, span / 1000.0,
         kFrames * 1000.0 / span, g_slow.waited / 1000.0, who.empty() ? "" : ": ", who.c_str());
    if (line.empty())
      line = net ? (g_role == Role::Host ? L"" : L"Игра идёт медленно: ждём данные от хоста.")
                 : L"Игра идёт медленно: этот ПК не успевает. Уменьшите сглаживание или разрешение (ОПЦИИ → ЭКРАН).";
    DWORD t = GetTickCount();
    // (one slow stretch is a stage loading: said on screen from the second in a row)
    if (!line.empty() && g_slow.run >= 1 && t - g_slow.told > 20000) {
      g_slow.told = t;
      overlay::notify(line, 6.0, 0xFFFFB040);
    }
  }
  g_slow.run = slow ? g_slow.run + 1 : 0;
  g_slow.since = now;
  g_slow.frames = 0;
  g_slow.waited = 0;
  for (double& v : g_slow.by) v = 0;
  (void)frame;
}

bool frame_input(uint32_t frame, sim::FrameInput* out) {
  g_game_frame = frame;
  if (g_restart_rejoin.exchange(false)) restart_rejoin();
  ULONGLONG start = GetTickCount64(), last_present = 0;
  bool shown = false;
  if (!g_slow.freq.QuadPart) QueryPerformanceFrequency(&g_slow.freq);
  LARGE_INTEGER wait0, wait1;
  QueryPerformanceCounter(&wait0);
  uint8_t missing = 0;
  std::unique_lock<std::mutex> lk(g_mu);
  auto have = [frame] { return frame < g_all.size() && g_all[frame].frame == frame; };
  while (!have()) {
    if (g_role == Role::Host)
      for (int p = 1; p < kMax; ++p)
        if (takes_part(p, g_next_bundle) && g_in[p][g_next_bundle % kRing].frame != g_next_bundle)
          missing |= (uint8_t)(1u << p);
    if (g_phase == Phase::Ended || g_stop) {
      g_active = false;
      std::string why = g_error.empty() ? "Сессия закончилась." : g_error;
      lk.unlock();
      overlay::clear_panel();
      overlay::notify(wide(why) + L" Игра продолжается без сети.", 8.0, 0xFFFF8060);
      LOGW("net: session over at frame %u: %s", frame, why.c_str());
      return false;
    }
    g_cv.wait_for(lk, std::chrono::milliseconds(5));
    ULONGLONG now = GetTickCount64();
    if (now - start < kWaitShowMs) continue;
    // Late: keep the window alive and say whom we wait for.
    std::wstring who;
    if (g_role == Role::Host) {
      for (int p = 1; p < kMax; ++p)
        if (takes_part(p, g_next_bundle) && g_in[p][g_next_bundle % kRing].frame != g_next_bundle)
          who += (who.empty() ? L"" : L", ") + (L"P" + std::to_wstring(p + 1) + L" " + wide(g_pl[p].name));
    } else {
      for (int p = 1; p < kMax; ++p)
        if ((g_waiting >> p) & 1)
          who += (who.empty() ? L"" : L", ") + (L"P" + std::to_wstring(p + 1) + L" " + wide(g_pl[p].name));
      if (who.empty()) who = L"хоста";
    }
    lk.unlock();
    if (!pump(nullptr)) {
      lk.lock();
      g_error = "Игра закрывается.";
      g_phase = Phase::Ended;
      continue;
    }
    if (now - last_present >= 50) {
      last_present = now;
      overlay::set_panel({{L"Ожидание: " + (who.empty() ? std::wstring(L"сеть") : who), 0xFFFFD040, true},
                          {L"(" + std::to_wstring((now - start) / 1000) + L" с)", 0xFFB0B0B0, false}},
                         true);
      video::present_now(false);
      shown = true;
    }
    lk.lock();
  }
  QueryPerformanceCounter(&wait1);
  {
    double w = slow_ms(wait0, wait1);
    g_slow.waited += w;
    if (g_role == Role::Client) g_wait_acc += w;
    for (int p = 1; p < kMax; ++p)
      if ((missing >> p) & 1) g_slow.by[p] += w;
  }
  const tmnp::BundleWire b = g_all[frame];
  if (g_role == Role::Client) g_players |= b.players;  // names of players who joined later
  // We joined a running game: replay it at full speed until the host lets our pad in (which
  // needs our input to reach its newest frame); far behind, without showing each frame. A player
  // back after a drop or a desync is in the old frames too: in is a frame after the history.
  if (g_late && !g_late_in && ((b.players >> g_me) & 1) && frame > g_hist_end && g_hist_end) g_late_in = true;
  bool boot = (b.flags & sim::kBoot) != 0;
  // This frame was made without us though we are a player of the session: our link was gone for a while,
  // the others went on, and the host took us back (return_in_place). At full speed to the newest frame
  // and on with no frame limit, until our input is in time there and the frames have us again.
  bool left_out = g_role == Role::Client && !boot && !g_late && !((b.players >> g_me) & 1);
  static bool was_left_out = false;
  if (left_out != was_left_out) {
    was_left_out = left_out;
    LOGI(left_out ? "net: the game went on without us from frame %u (the link was gone): catching up to get back in"
                  : "net: back in the game at frame %u", frame);
    if (!left_out) overlay::notify(L"Связь восстановлена: вы снова в игре.", 4.0, 0xFF80FF80);
  }
  bool hurry = boot || (g_late && !g_late_in) || left_out;
  g_hurry = hurry;
  if (g_role == Role::Client) {  // two minutes in step: an earlier desync is behind us
    if (hurry) g_sync_frames = 0;
    else if (++g_sync_frames == 7200 && g_resyncs > 0) {
      LOGI("net: in step with the others for two minutes: the earlier desync is behind");
      g_resyncs = 0;
    }
  }
  bool fast = boot || (hurry && g_newest > frame + 30);
  uint32_t newest = g_newest;
  lk.unlock();
  slow_report(frame);
  midjoin::frame_players(frame, b.players);  // a player new in this frame gets into the team
  if (fast != g_fast.exchange(fast)) {
    if (fast)
      LOGI(boot ? "net: the game's start-up at full speed from frame %u" : "net: catching up from frame %u to %u", frame,
           newest);
    else
      LOGI("net: %s at frame %u", boot ? "start-up over" : "caught up", frame);
    if (!fast) overlay::clear_panel();
    quiet(fast);
  }
  if (fast && frame % 30 == 0) {
    // How far the replay is and about how long it still takes (from its pace so far).
    static DWORD began = 0;
    static uint32_t began_frame = 0;
    DWORD t = GetTickCount();
    if (!began || frame < began_frame) {
      began = t;
      began_frame = frame;
    }
    std::wstring left;
    if (t - began >= 1500 && frame > began_frame + 60 && newest > frame) {
      double pace = (double)(frame - began_frame) / (double)(t - began);  // frames a millisecond
      // (the others play on meanwhile: 60 frames a second more to catch)
      double ms = pace > 0.07 ? (double)(newest - frame) / (pace - 0.06) : 0;
      int s = (int)(ms / 1000.0 + 0.5);
      if (s >= 60)
        left = L"осталось около " + std::to_wstring((s + 30) / 60) + L" мин";
      else if (s >= 3)
        left = L"осталось около " + std::to_wstring(s) + L" с";
    }
    if (boot)
      overlay::set_panel({{L"Загрузка сетевой игры...", 0xFFFFD040, true}}, true);
    else
      overlay::set_panel({{L"Подключение к игре", 0xFFFFD040, true},
                          {std::to_wstring(frame * 100ull / (newest ? newest : 1)) + L"%" + (left.empty() ? L"" : L"   " + left),
                           0xFFFFFFFF, true},
                          {L"Игра догоняет то, что уже сыграно. У остальных она не останавливается.", 0xFFB0B0B0, false}},
                         true);
  }
  if (shown) overlay::clear_panel();
  sim::FrameInput in;
  for (int p = 0; p < sim::kPads; ++p) {
    const tmnp::PadWire& w = b.pads[p];
    sim::PadInput& pi = in.pads[p];
    pi.buttons = w.buttons;
    pi.dpad = w.dpad;
    pi.connected = w.connected;
    pi.lx = w.lx;
    pi.ly = w.ly;
    pi.rx = w.rx;
    pi.ry = w.ry;
  }
  in.movie = b.movie;
  in.flags = b.flags;
  *out = in;
  return true;
}

bool catching_up() { return g_fast; }

bool take_begin(uint32_t* seed) {
  // At this game's title menu only (a player coming in may still be starting up): there every game
  // is in the same state.
  if (!g_begin.load()) return false;
  if (!menus::title_menu_up()) {
    if (menus::title_before_menu()) sim::run_to_title_menu();  // at the opening / НАЖМИ СТАРТ: on to the menu
    static DWORD told = 0;
    if (GetTickCount() - told > 4000) {
      told = GetTickCount();
      overlay::notify(L"Сетевая игра началась: выйдите в главное меню, чтобы войти в неё.", 4.0, 0xFFFFD040);
    }
    return false;
  }
  g_begin = false;
  g_active = true;
  g_session_on = true;
  g_slow.since.QuadPart = 0;  // (the pace is measured from this session's first frame, not from an earlier one's)
  g_slow.frames = 0;
  g_slow.waited = 0;
  g_slow.run = 0;
  for (double& v : g_slow.by) v = 0;
  if (seed) *seed = kSessionSeed;
  if (uint8_t* r = record()) {  // every game plays with the session's progress; its own comes back after
    if (!g_rec_own_kept) {
      std::memcpy(g_rec_own, r, sizeof(g_rec_own));
      g_rec_own_fresh = patch::read<uint8_t>(game::kNewGameFlag);
      g_rec_own_kept = true;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_rec_have) {
      std::memcpy(r, g_rec_session, tmnp::kRecordSize);
      patch::write(game::kNewGameFlag, &g_rec_fresh, 1);
    }
    else LOGW("net: no session progress came: playing with our own (the games may part)");
    if (g_rec_have && (g_opt_session[0] & 0x80)) {  // the host's difficulty, autosave, help and hit effects
      uint8_t mine[2];
      read_options(mine);
      if (!g_opt_own_kept && (mine[0] != g_opt_session[0] || mine[1] != g_opt_session[1])) {
        g_opt_own[0] = mine[0];
        g_opt_own[1] = mine[1];
        g_opt_own_kept = true;  // (equal ones: nothing to bring back, a change made in the session stays)
        LOGI("net: the session plays with the host's game options (difficulty %d, flags %02X; ours %d, %02X come back after)",
             g_opt_session[1], g_opt_session[0], mine[1], mine[0]);
      }
      write_options(g_opt_session);
    }
  }
  use_net_save();
  menus::on_session_start();
  {
    int n = 0;
    for (int p = 0; p < kMax; ++p) n += (g_players >> p) & 1;
    overlay::notify(n >= 2 ? L"Сетевая игра началась. Игроков: " + std::to_wstring(n) + L". Меню ведёт любой игрок."
                           : L"Сетевая игра началась. Друзья могут войти по ходу игры.",
                    6.0, 0xFF8CFF8C);
  }
  LOGI("net: the session begins in the running game: P%d, input delay %u", g_me + 1, g_delay);
  return true;
}

void title_state(int phase, int step) {
  if (g_cfg.session_at_title && g_role == Role::Host && g_cfg.session_start_at > 0 && phase == 2 && step >= 1 &&
      g_phase.load() == Phase::Lobby && GetTickCount64() >= g_t0_ms + (ULONGLONG)(g_cfg.session_start_at * 1000)) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_start_request) {
      g_start_request = true;
      LOGI("net: [debug] the host begins the session at its title menu");
    }
  }
  if (g_role != Role::Host || !g_boot.load() || phase != 2) return;
  std::lock_guard<std::mutex> lk(g_mu);
  if (step == 0 && !g_boot_press.load()) {
    g_boot_press = g_next_bundle;
    LOGI("net: the title's opening: Start for player 1 every %u frames from bundle %u", kBootPressEvery, g_next_bundle);
  } else if (step >= 1 && g_boot.exchange(false)) {
    g_boot_last = g_next_bundle - 1;
    LOGI("net: the title menu is on screen: the start-up ends with bundle %u", g_next_bundle);
  }
}
bool hurrying() { return g_hurry; }

void report_hash(uint32_t frame, uint32_t rng_calls, uint32_t rng_hash) {
  if (frame % tmnp::kHashEvery) return;
  std::lock_guard<std::mutex> lk(g_mu);
  HSlot s{frame, rng_calls, rng_hash};
  if (g_role == Role::Host) {
    g_hash[0][(frame / tmnp::kHashEvery) % 64] = s;
    for (int p = 1; p < kMax; ++p)
      if (g_pl[p].present) compare_hash(p, frame);
  } else {
    g_hash_out.push_back(s);
  }
}

void shutdown() {
  if (g_thread.joinable()) stop();
}

int player_ping(int p) {
  if (p <= 0 || p >= kMax || !g_active) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  return g_pl[p].present ? (int)g_pl[p].ping : 0;
}

bool player_waited(int p) {
  if (p < 0 || p >= kMax || !g_active) return false;
  return (g_waiting >> p) & 1;
}

std::string player_name(int p) {
  if (p < 0 || p >= kMax) return "";
  std::lock_guard<std::mutex> lk(g_mu);
  if (!(g_players & (1u << p)) || (g_pl[p].out_from && !g_pl[p].present)) return "";
  return g_pl[p].name;
}

bool restart_for_netplay() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t cmd[MAX_PATH + 96];
  swprintf(cmd, MAX_PATH + 96, L"\"%ls\" -tmnt2net-net=menu -tmnt2net-after=%lu", exe, GetCurrentProcessId());
  std::wstring dir = module_dir();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
    LOGE("net: cannot restart the game for network play (error %lu)", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  LOGI("net: the game restarts into the network menu (new pid %lu)", pi.dwProcessId);
  return true;
}

}  // namespace netplay
