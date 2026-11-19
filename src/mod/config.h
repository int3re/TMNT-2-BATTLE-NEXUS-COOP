// tmnt2net.ini next to TMNT2.exe. Missing keys keep the defaults below.
#pragma once
#include <cstdint>
#include <string>

#include "common/log.h"

struct Config {
  // [video]
  std::string mode = "borderless";  // borderless | windowed | fullscreen
  int res_w = 0, res_h = 0;         // 0 = auto
  bool res_game = false;            // resolution = game: TMNT2.ini / the game's menu
  // aspect = auto: widescreen (Hor+, 2D kept 4:3); 4:3: a 4:3 frame with bars;
  // stretch: the 4:3 picture stretched over the frame.
  enum class Aspect { Wide, Classic, Stretch };
  Aspect aspect = Aspect::Wide;
  bool hud_center = true;           // hud = center | stretch (aspect = auto)
  bool own_camera = true;           // own_camera = 1: in a network game the stage camera follows this PC's player
  float camera_zoom = 1.0f;         // the mouse wheel's zoom of the stage camera (0.5 closer .. 2.5 farther)
  float gauges_scale = 1.0f;        // gauges_size = 100 | 85 | 70 (%): the stage's ring and players' boxes
  bool gauges_corners = true;       // gauges = corners | center (hud = center): the stage's health ring
                                    // and the players' boxes at the frame's corners or the 4:3 ones
  float fov = 1.0f;                 // view size multiplier for the main cameras
  float view_fov = 1.0f;            // netplay: the player's fov, for the draw only (the game's cameras keep 1.0)
  bool net_wide = true;             // netplay: frames of the monitor's shape (drawn wider), not 16:9 with bars
  int window_w = 0, window_h = 0;   // windowed client size, 0 = auto
  bool window_resize = true;
  int vsync = -1;                   // -1 as the game sets it, 0 off, 1 on
  int fps_limit = 60;               // frames per second (the game's logic runs one step per frame); 0 = off
  bool background_run = false;      // keep running while the window is not in front
  bool hide_cursor = true;
  int antialiasing = 4;             // MSAA samples (0 off, 2, 4, 8) when the frame fills the window
  int anisotropy = 16;              // anisotropic texture filtering (0/1 off .. 16)
  int sharpen = 50;                 // 0..100: the finished frame made crisper (postfx.cpp), 0 = off
  int colors = 25;                  // 0..100: fuller colours and a little more contrast, 0 = off

  // [input]
  int pads = 4;               // gamepad slots read through SDL (0 = the game's own DirectInput only)
  int keyboard_players = 1;   // 2: the keyboard becomes two pads (left / right half)

  // [net] (stage 4): lockstep for up to 4 players; every PC runs its own game
  std::string net_mode = "off";      // off | menu | host | join
  std::string join;                  // join: code / IP[:port] / steam / SteamID64 / lobby id
  std::string transport = "auto";    // auto (Steam if it runs, else LAN/IP) | steam | enet | mock
  std::string name;                  // shown to the others; default: Steam name / computer name
  uint64_t player_id = 0;            // this PC's id for the others (made once, kept in the ini)
  uint16_t port = 7790;
  int input_delay = 0;               // frames, 0 = from the ping
  std::string lobby_type = "friends";  // host's Steam lobby: friends | public
  int auto_start = 0;                // host: start once this many players are in (0 = Enter)
  int join_retry_s = 0;              // join: keep trying this long (the old restart flow: -tmnt2net-retry)
  bool quick_start = false;          // [video] intro = skip: the logos and the opening run at once to the menu
                                     // (show, the default: they play, and any key or click skips them)
  bool session_at_title = true;      // sessions begin at the title menu of a running game (no restart; [debug] 0 = old)
  float session_start_at = 0;        // [debug] host: begin the session this many seconds after its start
  int timeout_ms = 8000;             // a player silent this long is dropped
  bool steam_invites = true;         // playing alone: an accepted Steam invite restarts into the lobby
  float net_aspect = 0;              // set while a session is on: the camera aspect of every PC (the logic's)
  // Lab only, transport=mock: Steam stand-in for several players on one PC
  int mock_slot = 1;
  std::string mock_dir;
  int mock_loss_pct = 0;
  int mock_latency_ms = 0;
  int mock_send_rate = 0;  // [net] the mock's reliable sends: bytes a second (512 KB at once), then "limit exceeded"
  float net_outage_at = 0, net_outage_for = 0;  // [debug] net_outage = at,for (s): a client's link is dead that long
  bool mock_lobby = true;
  int mock_member_delay_ms = 0;
  bool net_on() const { return net_mode == "menu" || net_mode == "host" || net_mode == "join"; }
  bool steam_wanted() const { return transport != "enet"; }

  // [fixes]
  bool fix_gamepads = true;
  bool fix_keyboard_focus = true;
  bool all_modes = true;
  bool esc_back = true;  // Esc goes back in the game's menus (as Space does) when playing alone

  // [log]
  tlog::Level log_level = tlog::Info;
  bool log_exceptions = true;

  // [debug]
  bool multi_instance = false;
  bool no_activate = false;      // never take focus (test runs while the owner works)
  bool start_minimized = false;  // show the window minimized (borderless / windowed)
  bool offscreen = false;        // place the window beyond the desktop's right edge (test runs)
  int autoquit = 0;              // quit after N seconds (test runs)
  bool determinism = false;      // stage 3: frame log, no frame skipping, loads within the frame
  std::string frame_script;      // "300:RETURN:2, 900:SHOT": keys on exact frames
  int rng_mutant_frame = 0;      // one extra RpRandom at this frame (checks the checker)
  std::string record;            // determinism: write every frame's input to this file
  std::string replay;            // determinism: play a recording back (it may still be growing)
  bool font_sheet = false;      // the title menu shows every glyph of the game's font with its code
  std::string tag_names;         // "Лео,Вася": names over the players outside a network game (test runs)
  float camera_zoom_max = 0;     // > 0: the stage camera backs off at most this much (1..3; test runs)
  float transition_test = 0;     // > 0: at this second of the script clock a stage transition starts (test runs)
  float health_test = 0;         // > 0: at this second P2 loses all health, P1 a quarter (test runs)
  float health_all_test = 0;     // > 0: at this second every player loses all health (test runs)
  float clear_test = 0;          // > 0: at this second the stage counts as cleared (test runs)
  int clear_test_stages = 0;     // clear_test: only this many stages (0 = every one; e.g. 1 = not the boss after)
  bool all_costumes = false;     // character select: every costume can be picked (test runs)
  bool replay_draws = false;     // [debug] a replayed session is drawn and shown, as before 0.16.5
  bool keyboard_lab = false;     // the keyboard at DirectInput's background level (a test copy is never in front)
  bool all_characters = false;   // Casey, Splinter, Karai and Slashuur count as unlocked (test runs)
  int map_area = 0;              // > 0: the area map opens on this area (2 = 0-2, 3 = Home; test runs)
  bool panel_test = false;       // tmnt2net's own panel with sample lines from the start (test runs: its look)
  bool postfx_split = false;     // post-processing on the right half of the frame only (test runs: compare)
  bool log_cameras = false;      // log each camera scene's camera, caller and view window once (test runs)
  bool no_2d_fix = false;        // the 2D camera keeps the session's 16:9 as before 0.12.3 (test runs)
  int area_test = 0;             // > 0: whatever area the game starts, it is this one (test runs)
  int jump_test = 0;             // every N frames everyone on the ground jumps (test runs)
  int enbu_test = 0;             // 1..7: the title menu goes straight to the team's dance at this rank
  int virtual_pads = 0;          // SDL virtual pads driven by the script ("PAD1.START")
  std::string script;            // "8:RETURN, 12:SHOT": timed keys / frame captures
  std::string shots;             // "300,900": save these frames as BMP (screenshots folder)
};

// Reads the ini; returns false when the file does not exist (defaults kept).
bool load_config(const std::wstring& path, Config& cfg);
std::wstring module_dir();  // folder of TMNT2.exe, with a trailing backslash
