// Network play (stage 4): lockstep for up to 4 players. Every player runs the
// game on their own PC and sees it on their own screen; only input travels.
// The host collects every player's input for frame N (each PC samples it
// input_delay frames ahead) and sends everyone the frame's bundle; sim.cpp feeds
// the bundle to the game at CPCInputsDevice::Sync. Same input, same game
// (stage 3, re/notes.md §10).
//
// A session starts on the game's first frame, so every PC starts from the same
// state: the game holds there for the lobby, the clients get the host's save
// (their own is left alone), then all start together.
//
// Threads: a network thread owns the transports (ENet for LAN / IP, Steam with
// its lobby, or the lab mock) and Steam; the game thread only calls the
// functions below.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "mod/config.h"
#include "mod/sim.h"

namespace netplay {

// Right after the ini is read: a session needs the same game on every PC
// (determinism on, background run, the session's camera aspect, no exclusive
// fullscreen while the lobby is shown).
void prepare(Config& cfg);
void install(const Config& cfg);  // after sim::install
bool configured();                // [net] mode = menu | host | join
bool active();                    // a session runs: frames come from the host

// Game thread, at the first frame: the lobby. Returns when the game starts, or
// when the player leaves netplay (the game then goes on alone).
void lobby();

// Game thread, every frame of a session (sim's frame hook).
uint32_t input_delay();
int my_player();  // the pad slot this PC drives
// Joined a running game and replaying it far behind the newest frame: frames are not shown.
bool catching_up();
// Joined a running game and not in yet: frames run without the frame limit.
bool hurrying();
// --- the network page (title menu, a game alone): players meet in the running game; the
// host's start restarts every game into the session (game thread) ----------------------------
struct LobbyPlayer {
  bool present = false, ready = false, me = false;
  std::string name;  // UTF-8
  uint32_t ping = 0;
};
struct LobbyGame {
  uint64_t lobby = 0;
  std::string host;  // UTF-8
  bool running = false, compatible = false;
  std::string why_not;  // UTF-8, Russian: why it cannot be joined
};
struct LobbyFriend {
  uint64_t id = 0;
  std::string name;      // UTF-8
  bool in_game = false;  // runs tmnt2net (or another Spacewar game) now
};
struct LobbyView {
  int role = 0;          // 0 none, 1 host, 2 client
  bool joined = false;   // client: the host took us
  bool failed = false;   // it ended (error below)
  bool steam = false;    // Steam is on (friends can join)
  std::string error;     // UTF-8, Russian
  std::string code;      // host: LAN / IP code
  std::string host_name;
  LobbyPlayer players[4];
  std::vector<LobbyGame> games;  // client looking for a friend's game
  bool browsing = false;         // client: no game picked yet (the list above)
  std::string note;              // UTF-8, Russian: what the network is doing now
  bool can_invite = false;       // host: a Steam lobby is open (friends below can be called)
  std::vector<LobbyFriend> friends;
};
// Steam before any lobby is open: 0 not known yet, 1 on (name), 2 off (reason, Russian).
struct SteamInfo {
  int state = 0;
  std::string name, reason;
};
SteamInfo steam_info();
void lobby_invite(uint64_t friend_id);  // host: a Steam invite into the open game
bool lobby_open_host();
bool lobby_open_client(const std::string& target);  // "list": friends' games to pick from
void lobby_pick(uint64_t lobby_id);                 // client: join this friend's game
void lobby_start();                                 // host: everybody restarts into the session
void lobby_close();
bool lobby_running();
LobbyView lobby_view();
// Game thread, every frame: the restarts the page's lobby asks for.
void lobby_tick();
// The session's title menu, its last item: the session ends here (game thread; every PC does it in the
// same frame, each stays in its own game with its own progress, the co-op progress is kept).
void leave_session();
// Around COptionData::Restore (a save is loaded): in a session the options that are part of the game's
// logic stay as they are (every PC loads its own save, the values must stay one for all).
void options_before_restore();
void options_after_restore();

// A session begun in a running game (no restart): true once, at the frame boundary where its frame 1
// starts (sim resets its frame count and the game's random generator there). Game thread.
bool take_begin(uint32_t* seed);

// The title sequence's state each update (game thread): a network game's start-up runs up to its
// menu (step >= 1 in phase 2), pressing Start for player 1 in the title (its opening, then
// "НАЖМИ СТАРТ").
void title_state(int phase, int step);
// Own input for `frame` (sampled input_delay frames earlier).
void submit(uint32_t frame, const sim::PadInput& pad, uint16_t movie, uint8_t flags);
// The bundle of `frame`; waits for it, showing who is late. false: the session
// ended (host gone / quit): the game goes on alone.
bool frame_input(uint32_t frame, sim::FrameInput* out);
void report_hash(uint32_t frame, uint32_t rng_calls, uint32_t rng_hash);

void shutdown();  // the game is quitting: say goodbye, stop the thread

// Name of network player p (pad p), "" if nobody (any thread).
std::string player_name(int p);
// Player p's round trip to the host in ms (0: the host itself, unknown, no session).
int player_ping(int p);
// The game has been waiting for player p's input for a while (any thread).
bool player_waited(int p);

// The game's СЕТЕВАЯ ИГРА item: a session must start at the game's first frame,
// so a new copy of the game is started straight into the network menu (it waits
// for this one to close); the caller then closes this one.
bool restart_for_netplay();

// Game thread, every frame while no session is set up: a Steam invite accepted (or "Join game"
// on a friend) restarts the game joined to that lobby ([net] steam_invites).
void idle_tick();

}  // namespace netplay
