#include "mod/config.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

std::string get(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
  std::vector<wchar_t> buf(65536);  // [debug] scripts run long
  GetPrivateProfileStringW(section, key, L"", buf.data(), (DWORD)buf.size(), path.c_str());
  // Drop an inline "; comment" and surrounding blanks.
  std::wstring w(buf.data());
  size_t semi = w.find(L';');
  if (semi != std::wstring::npos) w.resize(semi);
  while (!w.empty() && iswspace(w.back())) w.pop_back();
  size_t b = 0;
  while (b < w.size() && iswspace(w[b])) ++b;
  std::string s;
  for (size_t i = b; i < w.size(); ++i) s += (char)(w[i] < 128 ? towlower(w[i]) : '?');
  return s;
}

// A value as typed (case kept, UTF-8): names.
std::string get_text(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
  wchar_t buf[256] = {};
  GetPrivateProfileStringW(section, key, L"", buf, 256, path.c_str());
  std::wstring w(buf);
  size_t semi = w.find(L';');
  if (semi != std::wstring::npos) w.resize(semi);
  while (!w.empty() && iswspace(w.back())) w.pop_back();
  size_t b = 0;
  while (b < w.size() && iswspace(w[b])) ++b;
  w = w.substr(b);
  // Windows reads an ini without a BOM in the ANSI code page, but Notepad saves
  // UTF-8: back to the file's bytes, and if they are UTF-8 with letters past
  // ASCII, they are taken as UTF-8.
  char raw[512] = {};
  int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), raw, sizeof(raw) - 1, nullptr, nullptr);
  bool high = false;
  for (int i = 0; i < n; ++i) high = high || (unsigned char)raw[i] >= 0x80;
  if (high && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw, n, nullptr, 0) > 0) return std::string(raw, n);
  char out[512] = {};
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, sizeof(out), nullptr, nullptr);
  return out;
}

void get_bool(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool& out) {
  std::string v = get(path, section, key);
  if (v == "1" || v == "true" || v == "yes" || v == "on") out = true;
  if (v == "0" || v == "false" || v == "no" || v == "off") out = false;
}

bool get_size(const std::string& v, int& w, int& h) {
  int a = 0, b = 0;
  if (sscanf(v.c_str(), "%dx%d", &a, &b) == 2 && a >= 320 && b >= 240 && a <= 16384 && b <= 16384) {
    w = a;
    h = b;
    return true;
  }
  return false;
}

}  // namespace

std::wstring module_dir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring s(path);
  size_t slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L".\\" : s.substr(0, slash + 1);
}

bool load_config(const std::wstring& path, Config& cfg) {
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

  std::string v = get(path, L"video", L"mode");
  if (v == "borderless" || v == "windowed" || v == "fullscreen") cfg.mode = v;
  v = get(path, L"video", L"resolution");
  if (v == "game") cfg.res_game = true;
  else get_size(v, cfg.res_w, cfg.res_h);
  v = get(path, L"video", L"aspect");
  if (v == "auto" || v == "wide" || v == "hor+") cfg.aspect = Config::Aspect::Wide;
  if (v == "4:3") cfg.aspect = Config::Aspect::Classic;
  if (v == "stretch") cfg.aspect = Config::Aspect::Stretch;
  cfg.shape = cfg.aspect;
  v = get(path, L"video", L"hud");
  if (v == "center") cfg.hud_center = true;
  if (v == "stretch") cfg.hud_center = false;
  v = get(path, L"net", L"steam_invites");
  if (!v.empty()) cfg.steam_invites = v != "0";
  v = get(path, L"net", L"wide");
  if (!v.empty()) cfg.net_wide = v != "0";
  v = get(path, L"video", L"own_camera");
  if (!v.empty()) cfg.own_camera = v != "0";
  v = get(path, L"video", L"camera_zoom");
  if (!v.empty()) {
    float z = (float)atof(v.c_str());
    if (z >= 0.5f && z <= 2.5f) cfg.camera_zoom = z;
  }
  v = get(path, L"video", L"antialiasing");
  if (!v.empty()) cfg.antialiasing = std::max(0, std::min(16, atoi(v.c_str())));
  // The picture settings tmnt2net recommends, once for an ini of an older tmnt2net: its texture
  // filter was 8x in every ini; 16x costs nothing on any card that runs this.
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && get(path, L"mod", L"settings").empty()) {
    if (get(path, L"video", L"anisotropy") == "8") WritePrivateProfileStringW(L"video", L"anisotropy", L"16", path.c_str());
    WritePrivateProfileStringW(L"mod", L"settings", L"2", path.c_str());
  }
  v = get(path, L"video", L"anisotropy");
  if (!v.empty()) cfg.anisotropy = std::max(0, std::min(16, atoi(v.c_str())));
  v = get(path, L"video", L"sharpen");
  if (!v.empty()) cfg.sharpen = std::max(0, std::min(100, atoi(v.c_str())));
  v = get(path, L"video", L"colors");
  if (!v.empty()) cfg.colors = std::max(0, std::min(100, atoi(v.c_str())));
  v = get(path, L"video", L"gauges_size");
  if (!v.empty()) {
    int pct = atoi(v.c_str());
    if (pct >= 50 && pct <= 100) cfg.gauges_scale = pct / 100.0f;
  }
  v = get(path, L"video", L"gauges");
  if (v == "corners") cfg.gauges_corners = true;
  if (v == "center") cfg.gauges_corners = false;
  v = get(path, L"video", L"fov");
  if (!v.empty()) {
    float f = (float)atof(v.c_str());
    if (f >= 0.5f && f <= 2.0f) cfg.fov = f;
  }
  get_size(get(path, L"video", L"window_size"), cfg.window_w, cfg.window_h);
  get_bool(path, L"video", L"window_resize", cfg.window_resize);
  v = get(path, L"video", L"vsync");
  if (v == "0" || v == "1" || v == "-1") cfg.vsync = std::stoi(v);
  v = get(path, L"video", L"fps_limit");
  if (!v.empty()) cfg.fps_limit = std::max(0, std::min(1000, atoi(v.c_str())));
  v = get(path, L"video", L"background");
  if (v == "run") cfg.background_run = true;
  if (v == "pause") cfg.background_run = false;
  get_bool(path, L"video", L"hide_cursor", cfg.hide_cursor);

  v = get(path, L"input", L"pads");
  if (!v.empty()) cfg.pads = std::max(0, std::min(4, atoi(v.c_str())));
  v = get(path, L"input", L"keyboard_players");
  if (v == "1" || v == "2") cfg.keyboard_players = atoi(v.c_str());

  v = get(path, L"net", L"mode");
  if (v == "off" || v == "menu" || v == "host" || v == "join") cfg.net_mode = v;
  cfg.join = get(path, L"net", L"join");
  v = get(path, L"net", L"transport");
  if (v == "auto" || v == "steam" || v == "enet" || v == "mock") cfg.transport = v;
  cfg.name = get_text(path, L"net", L"name");
  v = get(path, L"net", L"player_id");
  if (!v.empty()) cfg.player_id = std::strtoull(v.c_str(), nullptr, 16);
  v = get(path, L"net", L"port");
  if (!v.empty() && atoi(v.c_str()) > 0 && atoi(v.c_str()) < 65536) cfg.port = (uint16_t)atoi(v.c_str());
  v = get(path, L"net", L"input_delay");
  if (!v.empty() && v != "auto") cfg.input_delay = std::max(1, std::min(15, atoi(v.c_str())));
  v = get(path, L"net", L"lobby_type");
  if (v == "friends" || v == "public") cfg.lobby_type = v;
  v = get(path, L"net", L"auto_start");
  if (!v.empty()) cfg.auto_start = std::max(0, std::min(4, atoi(v.c_str())));
  v = get(path, L"net", L"timeout_ms");
  if (!v.empty()) cfg.timeout_ms = std::max(2000, atoi(v.c_str()));
  v = get(path, L"net", L"mock_slot");
  if (!v.empty()) cfg.mock_slot = std::max(1, std::min(8, atoi(v.c_str())));
  cfg.mock_dir = get(path, L"net", L"mock_dir");
  v = get(path, L"net", L"mock_loss_pct");
  if (!v.empty()) cfg.mock_loss_pct = std::max(0, std::min(90, atoi(v.c_str())));
  v = get(path, L"net", L"mock_send_rate");
  if (!v.empty()) cfg.mock_send_rate = std::max(0, atoi(v.c_str()));
  v = get(path, L"debug", L"net_outage");
  if (!v.empty()) sscanf(v.c_str(), "%f,%f", &cfg.net_outage_at, &cfg.net_outage_for);
  v = get(path, L"net", L"mock_latency_ms");
  if (!v.empty()) cfg.mock_latency_ms = std::max(0, std::min(2000, atoi(v.c_str())));
  get_bool(path, L"net", L"mock_lobby", cfg.mock_lobby);
  v = get(path, L"net", L"mock_member_delay_ms");
  if (!v.empty()) cfg.mock_member_delay_ms = std::max(0, atoi(v.c_str()));

  get_bool(path, L"fixes", L"gamepad_snapshot", cfg.fix_gamepads);
  get_bool(path, L"fixes", L"keyboard_focus", cfg.fix_keyboard_focus);
  get_bool(path, L"fixes", L"all_modes", cfg.all_modes);
  get_bool(path, L"fixes", L"esc_back", cfg.esc_back);

  cfg.log_level = tlog::parse_level(get(path, L"log", L"level"), cfg.log_level);
  get_bool(path, L"log", L"exceptions", cfg.log_exceptions);

  get_bool(path, L"debug", L"multi_instance", cfg.multi_instance);
  get_bool(path, L"debug", L"no_activate", cfg.no_activate);
  get_bool(path, L"debug", L"start_minimized", cfg.start_minimized);
  get_bool(path, L"debug", L"offscreen", cfg.offscreen);
  v = get(path, L"debug", L"autoquit");
  if (!v.empty()) cfg.autoquit = std::max(0, atoi(v.c_str()));
  cfg.shots = get(path, L"debug", L"shots");
  cfg.script = get(path, L"debug", L"script");
  get_bool(path, L"debug", L"determinism", cfg.determinism);
  cfg.frame_script = get(path, L"debug", L"frame_script");
  v = get(path, L"debug", L"rng_mutant_frame");
  if (!v.empty()) cfg.rng_mutant_frame = std::max(0, atoi(v.c_str()));
  cfg.record = get(path, L"debug", L"record");
  cfg.replay = get(path, L"debug", L"replay");
  get_bool(path, L"debug", L"font_sheet", cfg.font_sheet);
  cfg.tag_names = get_text(path, L"debug", L"tag_names");
  v = get(path, L"debug", L"camera_zoom_max");
  if (!v.empty()) cfg.camera_zoom_max = (float)atof(v.c_str());
  v = get(path, L"debug", L"transition_test");
  if (!v.empty()) cfg.transition_test = (float)atof(v.c_str());
  v = get(path, L"debug", L"health_test");
  if (!v.empty()) cfg.health_test = (float)atof(v.c_str());
  v = get(path, L"debug", L"health_all_test");
  if (!v.empty()) cfg.health_all_test = (float)atof(v.c_str());
  v = get(path, L"debug", L"clear_test_stages");
  if (!v.empty()) cfg.clear_test_stages = atoi(v.c_str());
  v = get(path, L"debug", L"clear_test");
  if (!v.empty()) cfg.clear_test = (float)atof(v.c_str());
  get_bool(path, L"debug", L"all_costumes", cfg.all_costumes);
  get_bool(path, L"debug", L"keyboard_lab", cfg.keyboard_lab);
  get_bool(path, L"debug", L"replay_draws", cfg.replay_draws);
  get_bool(path, L"debug", L"fpu_keep", cfg.fpu_keep);
  v = get(path, L"debug", L"fpu_test");
  if (!v.empty()) cfg.fpu_test = atoi(v.c_str());
  get_bool(path, L"debug", L"all_characters", cfg.all_characters);
  get_bool(path, L"debug", L"log_cameras", cfg.log_cameras);
  get_bool(path, L"debug", L"postfx_split", cfg.postfx_split);
  get_bool(path, L"debug", L"panel_test", cfg.panel_test);
  get_bool(path, L"debug", L"session_at_title", cfg.session_at_title);
  // The logos and the opening movie play again by default (0.14 - 0.16.1 ran them at once to the menu,
  // which a network game needed while it had to restart the game); a key or a click skips them. Once for
  // an ini of those versions: its "skip" was the default of that time, not the player's choice.
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && atoi(get(path, L"mod", L"settings").c_str()) < 3) {
    if (get(path, L"video", L"intro") == "skip") WritePrivateProfileStringW(L"video", L"intro", L"show", path.c_str());
    WritePrivateProfileStringW(L"mod", L"settings", L"3", path.c_str());
  }
  v = get(path, L"video", L"intro");
  if (v == "skip" || v == "1") cfg.quick_start = true;
  if (v == "show" || v == "0") cfg.quick_start = false;
  v = get(path, L"debug", L"session_start_at");
  if (!v.empty()) cfg.session_start_at = (float)atof(v.c_str());
  get_bool(path, L"debug", L"no_2d_fix", cfg.no_2d_fix);
  v = get(path, L"debug", L"map_area");
  if (!v.empty()) cfg.map_area = atoi(v.c_str());
  v = get(path, L"debug", L"area_test");
  if (!v.empty()) cfg.area_test = std::max(0, atoi(v.c_str()));
  v = get(path, L"debug", L"jump_test");
  if (!v.empty()) cfg.jump_test = std::max(0, atoi(v.c_str()));
  v = get(path, L"debug", L"enbu_test");
  if (!v.empty()) cfg.enbu_test = std::max(0, std::min(7, atoi(v.c_str())));
  v = get(path, L"debug", L"virtual_pads");
  if (!v.empty()) cfg.virtual_pads = std::max(0, std::min(4, atoi(v.c_str())));
  return true;
}
