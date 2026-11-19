#include "mod/sim.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <dinput.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/config.h"
#include "mod/game.h"
#include "mod/input.h"
#include "mod/netplay.h"
#include "mod/pads.h"
#include "mod/patch.h"
#include "mod/video.h"

namespace sim {
namespace {

Config g_cfg;
bool g_active = false;
uint32_t g_frame = 0;      // frames fixed (CPCInputsDevice::Sync calls)
uint32_t g_epoch = 0;      // RpRandomSeed calls
uint32_t g_rng_calls = 0;  // RpRandom calls since start
uint16_t g_movies = 0;     // CPCMovie objects created since start
FrameInput g_cur;          // the input of the running frame
FILE* g_log = nullptr;

// --- [debug] frame_script: "300:RETURN:2, 420:F:30, 900:SHOT" -----------------------
struct FrameStep {
  uint32_t frame = 0, hold = 2;
  int key = -1;  // -1: SHOT
};
std::vector<FrameStep> g_steps;

int key_code(const std::string& name) {
  struct K {
    const char* n;
    int k;
  };
  static const K kKeys[] = {{"RETURN", DIK_RETURN}, {"ESCAPE", DIK_ESCAPE}, {"UP", DIK_UP},       {"DOWN", DIK_DOWN},
                            {"LEFT", DIK_LEFT},     {"RIGHT", DIK_RIGHT},   {"SPACE", DIK_SPACE}, {"F", DIK_F},
                            {"G", DIK_G},           {"D", DIK_D},           {"R", DIK_R},         {"E", DIK_E},
                            {"S", DIK_S},           {"W", DIK_W},           {"A", DIK_A},         {"LSHIFT", DIK_LSHIFT}};
  for (const K& k : kKeys)
    if (name == k.n) return k.k;
  if (name.rfind("0X", 0) == 0) return (int)strtol(name.c_str(), nullptr, 16);
  return -2;
}

void parse_frame_script(const std::string& s) {
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? s.size() : end + 1;
    char name[32] = {};
    unsigned f = 0, hold = 2;
    int n = sscanf(item.c_str(), " %u:%31[^:]:%u", &f, name, &hold);
    if (n < 2) continue;
    std::string key(name);
    while (!key.empty() && key.back() == ' ') key.pop_back();
    for (auto& c : key) c = (char)toupper((unsigned char)c);
    FrameStep st;
    st.frame = f;
    st.hold = hold ? hold : 1;
    st.key = key == "SHOT" ? -1 : key_code(key);
    if (st.key == -2) {
      LOGW("frame script: unknown key \"%s\"", name);
      continue;
    }
    g_steps.push_back(st);
  }
  std::sort(g_steps.begin(), g_steps.end(), [](const FrameStep& a, const FrameStep& b) { return a.frame < b.frame; });
  LOGI("sim: frame script, %d steps", (int)g_steps.size());
}

uint32_t fnv(const void* p, size_t n, uint32_t h = 2166136261u) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) {
    h ^= b[i];
    h *= 16777619u;
  }
  return h;
}

std::wstring full_path(const std::string& p) {
  std::wstring w(p.begin(), p.end());
  bool absolute = w.size() > 1 && (w[1] == L':' || (w[0] == L'\\' && w[1] == L'\\'));
  return absolute ? w : module_dir() + w;
}

// --- the generator ------------------------------------------------------------------
uint32_t* rng_globals() {
  uint8_t* engine = patch::read<uint8_t*>(game::kEngineInstance);
  if (!engine) return nullptr;
  uint32_t off = patch::read<uint32_t>(game::kRpRandomPluginOffset);
  return reinterpret_cast<uint32_t*>(engine + off);
}

uint32_t rng_hash() {
  uint32_t* g = rng_globals();
  if (!g || !g[0]) return 0;
  const uint32_t* table = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(g[0]));
  const uint32_t* end = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(g[3]));
  size_t words = end > table ? std::min<size_t>(end - table, 64) : 0;
  uint32_t h = fnv(table, words * 4);
  uint32_t p[2] = {g[1] - g[0], g[2] - g[0]};
  return fnv(p, sizeof(p), h);
}

using RpRandom_t = uint32_t(__cdecl*)();
using RpRandomSeed_t = void(__cdecl*)(uint32_t);
RpRandom_t oRpRandom = nullptr;
RpRandomSeed_t oRpRandomSeed = nullptr;

uint32_t __cdecl hkRpRandom() {
  ++g_rng_calls;
  return oRpRandom();
}

void __cdecl hkRpRandomSeed(uint32_t seed) {
  ++g_epoch;
  LOGI("sim: frame %u, RpRandomSeed(%u), epoch %u", g_frame, seed, g_epoch);
  if (g_log) fprintf(g_log, "# frame %u seed %u epoch %u\n", g_frame, seed, g_epoch);
  oRpRandomSeed(seed);
}

// --- loading within the frame ------------------------------------------------------
using FileSync_t = void(__fastcall*)(uint8_t* self, void* edx);
FileSync_t oFileSync = nullptr;

void __fastcall hkFileSync(uint8_t* self, void* edx) {
  oFileSync(self, edx);
  DWORD start = GetTickCount();
  int rounds = 0;
  while (patch::read<int>(reinterpret_cast<uintptr_t>(self) + 4) == 1) {  // busy
    if (GetTickCount() - start > 30000) {
      LOGW("sim: a file load took over 30 s; letting the game continue");
      break;
    }
    Sleep(1);
    oFileSync(self, edx);
    ++rounds;
  }
  if (rounds > 0) LOGD("sim: frame %u, file load finished in the frame (%d waits)", g_frame, rounds);
}

void hook_file_sync() {
  static bool done = false;
  if (done) return;
  uint8_t* fm = patch::read<uint8_t*>(game::kFileManager);
  if (!fm) return;
  done = true;
  uintptr_t vt = patch::read<uintptr_t>(reinterpret_cast<uintptr_t>(fm));
  uintptr_t sync = patch::read<uintptr_t>(vt + 3 * 4);
  if (!patch::in_image(sync, 1)) {
    LOGW("sim: file manager Sync not found (vtable 0x%08X)", (unsigned)vt);
    return;
  }
  patch::hook("CFileManager::Sync (loads within the frame)", sync, (void*)&hkFileSync, (void**)&oFileSync);
}

// Sound data loads on the sound thread; the logic polls CGameSound::IsLoadEnd.
using SoundGetLoadInfo_t = int(__cdecl*)();
const SoundGetLoadInfo_t SoundGetLoadInfo = reinterpret_cast<SoundGetLoadInfo_t>(game::kSoundGetLoadInfo);
using Bool_t = bool(__cdecl*)();
Bool_t oSoundIsLoadEnd = nullptr;

bool __cdecl hkSoundIsLoadEnd() {
  DWORD start = GetTickCount();
  int rounds = 0;
  while (SoundGetLoadInfo() != 0) {
    if (GetTickCount() - start > 30000) {
      LOGW("sim: sound data took over 30 s to load; letting the game continue");
      return oSoundIsLoadEnd();
    }
    Sleep(1);
    ++rounds;
  }
  if (rounds > 0) LOGD("sim: frame %u, sound data loaded in the frame (%d waits)", g_frame, rounds);
  return true;
}

// --- answers from the frame's input ------------------------------------------------
using MovieIsEnded_t = int(__fastcall*)(uint8_t* self, void* edx);
MovieIsEnded_t oMovieIsEnded = nullptr;
using MovieCtor_t = void*(__fastcall*)(void* self, void* edx, int w, int h, int bps, int pal);
MovieCtor_t oPCMovieCtor = nullptr;
Bool_t oVoicePlaying = nullptr;

int __fastcall hkMovieIsEnded(uint8_t* self, void* edx) {
  if (!patch::read<uint8_t>(reinterpret_cast<uintptr_t>(self) + game::kMovieHandleOk)) return 1;  // as the game
  if (self != patch::read<uint8_t*>(game::kMovieInstance)) return oMovieIsEnded(self, edx);
  return g_cur.movie == g_movies && (g_cur.flags & kMovieEnded) ? 1 : 0;
}

void* __fastcall hkPCMovieCtor(void* self, void* edx, int w, int h, int bps, int pal) {
  void* r = oPCMovieCtor(self, edx, w, h, bps, pal);
  ++g_movies;
  LOGD("sim: frame %u, movie %u created", g_frame, (unsigned)g_movies);
  return r;
}

bool __cdecl hkVoicePlaying() { return (g_cur.flags & kVoiceBusy) != 0; }

// --- the input of a frame ------------------------------------------------------------
// What the logic asks of real time, as this PC sees it now.
void sample_flags(FrameInput& in) {
  uint8_t* movie = patch::read<uint8_t*>(game::kMovieInstance);
  if (movie && oMovieIsEnded) {
    in.movie = g_movies;
    if (oMovieIsEnded(movie, nullptr)) in.flags |= kMovieEnded;
  }
  if (oVoicePlaying && oVoicePlaying()) in.flags |= kVoiceBusy;
}

int g_solo_slot = -1;  // a network session ended: this PC's player goes on alone in its slot
bool g_starting = false;  // the start-up runs at once to the title menu (quick_start, no network session)
bool g_start_over = false;  // the title menu has been up once: the start-up is behind
bool g_skip_held = false;   // a key that skips the start-up is down (one press, one skip)
constexpr uint32_t kStartPressEvery = 45;

FrameInput sample_local() {
  FrameInput in;
  if (g_solo_slot >= 0) {
    in.pads[g_solo_slot] = pads::local_player();
    sample_flags(in);
    return in;
  }
  uint8_t keys[256];
  if (input::read_keyboard(keys)) {
    pads::on_keyboard(keys);  // keys of the keyboard pads are theirs
    for (int k = 0; k < 256; ++k)
      if (keys[k] & 0x80) in.keys[k >> 3] |= (uint8_t)(1u << (k & 7));
  }
  for (int i = 0; i < kPads; ++i) in.pads[i] = pads::sample(i);
  sample_flags(in);
  if (!g_start_over && !g_starting) {
    // The logos and the opening movie play ([video] intro = show): Enter, Esc, Space, a mouse button or
    // a gamepad's button runs the rest of the start-up at once, to the main menu.
    auto key = [&in](int k) { return (in.keys[k >> 3] >> (k & 7)) & 1; };
    bool press = key(DIK_RETURN) || key(DIK_NUMPADENTER) || key(DIK_ESCAPE) || key(DIK_SPACE) ||
                 key(game::kKeyMouseLeft) || key(game::kKeyMouseRight);
    for (int i = 0; i < kPads; ++i) press = press || in.pads[i].buttons != 0;
    if (press && !g_skip_held && g_frame > 30) {
      g_starting = true;
      LOGI("sim: the player skips the start-up (frame %u): at once to the title menu", g_frame);
    }
    g_skip_held = press;
  }
  if (g_starting) {  // the start-up at once: Enter now and then, every movie over
    if (g_frame % kStartPressEvery < 2) in.keys[DIK_RETURN >> 3] |= (uint8_t)(1u << (DIK_RETURN & 7));
    if (in.movie) in.flags |= kMovieEnded;
  }
  return in;
}

// Network session: this PC's input goes to the host for a later frame, the
// frame's bundle comes back. false: no session (any more).
bool g_hold_buttons = false;  // a session just began here: our buttons count once all are up (the start's press)

bool net_input(FrameInput& in) {
  if (!netplay::active()) return false;
  FrameInput mine;
  sample_flags(mine);
  PadInput pad = pads::local_player();
  if (g_hold_buttons) {
    if (pad.buttons || pad.dpad) pad = PadInput{};
    else g_hold_buttons = false;
  }
  netplay::submit(g_frame + netplay::input_delay(), pad, mine.movie, mine.flags);
  if (netplay::frame_input(g_frame, &in)) return true;
  g_solo_slot = netplay::my_player();
  return false;
}

struct Record {
  uint32_t frame;
  FrameInput in;
};
constexpr char kRecMagic[8] = {'T', '2', 'N', 'R', 'E', 'C', '1', 0};
FILE* g_rec = nullptr;
HANDLE g_play = INVALID_HANDLE_VALUE;

// Reads exactly n bytes, waiting while the recording is still being written.
bool play_read(void* buf, DWORD n, DWORD wait_ms) {
  DWORD start = GetTickCount();
  DWORD got = 0;
  while (got < n) {
    DWORD r = 0;
    if (!ReadFile(g_play, static_cast<uint8_t*>(buf) + got, n - got, &r, nullptr)) return false;
    got += r;
    if (got < n) {
      if (GetTickCount() - start > wait_ms) return false;
      Sleep(1);
    }
  }
  return true;
}

void stop_replay(const char* why) {
  LOGW("sim: replay stops at frame %u (%s); the game's own input from now on", g_frame, why);
  CloseHandle(g_play);
  g_play = INVALID_HANDLE_VALUE;
}

void open_replay() {
  std::wstring path = full_path(g_cfg.replay);
  DWORD start = GetTickCount();
  while (true) {
    g_play = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_play != INVALID_HANDLE_VALUE || GetTickCount() - start > 30000) break;
    Sleep(10);
  }
  if (g_play == INVALID_HANDLE_VALUE) {
    LOGE("sim: replay %s cannot be opened", g_cfg.replay.c_str());
    return;
  }
  char magic[8];
  if (!play_read(magic, sizeof(magic), 30000) || memcmp(magic, kRecMagic, sizeof(magic)) != 0) {
    stop_replay("not a tmnt2net recording");
    return;
  }
  LOGI("sim: replaying %s", g_cfg.replay.c_str());
}

void next_input() {
  FrameInput in;
  bool played = false;
  if (g_play != INVALID_HANDLE_VALUE) {
    Record r;
    if (!play_read(&r, sizeof(r), 15000)) {
      stop_replay("end of the recording");
    } else if (r.frame != g_frame) {
      stop_replay("frame numbers differ");
    } else {
      in = r.in;
      played = true;
    }
  }
  if (!played && !net_input(in)) in = sample_local();
  if (g_rec) {
    Record r{g_frame, in};
    fwrite(&r, sizeof(r), 1, g_rec);
    fflush(g_rec);
  }
  g_cur = in;
}

// --- the frame ------------------------------------------------------------------------
using InputsSync_t = void(__fastcall*)(void* self, void* edx);
const InputsSync_t oInputsSync = reinterpret_cast<InputsSync_t>(game::kInputsSync);

void __fastcall hkInputsSync(void* self, void* edx) {
  // The frame that just ran (logic + drawing) ends here. One line a frame of a network session (or of
  // a test run's recording / replay or the old start from frame 1), none while playing alone.
  bool logging = g_log && (netplay::active() || !g_cfg.session_at_title || g_rec || g_play != INVALID_HANDLE_VALUE);
  if (logging && g_frame > 0)
    fprintf(g_log, "%u %u %u %08x %08x\n", g_frame, g_epoch, g_rng_calls, rng_hash(), fnv(&g_cur, sizeof(g_cur)));
  if (g_cfg.rng_mutant_frame && g_frame == (uint32_t)g_cfg.rng_mutant_frame) {
    LOGW("sim: [debug] rng_mutant_frame: one extra RpRandom at frame %u", g_frame);
    hkRpRandom();
  }
  if (g_frame > 0 && netplay::active()) netplay::report_hash(g_frame, g_rng_calls, rng_hash());
  uint32_t seed = 0;
  if (netplay::take_begin(&seed)) {  // a session in the running game: its frame 1 is the next
    if (g_log) fprintf(g_log, "# session begins after frame %u\n", g_frame);
    g_frame = 0;
    g_movies = 0;
    g_rng_calls = 0;
    g_hold_buttons = true;
    if (oRpRandomSeed) hkRpRandomSeed(seed);
  }
  ++g_frame;
  static bool first_done = false;  // (a session in a running game counts its frames from 1 again)
  if (g_frame == 1 && !first_done) {
    first_done = true;
    hook_file_sync();
    if (!g_cfg.replay.empty()) open_replay();
    netplay::lobby();  // a network session starts here, the same state on every PC
  }
  next_input();
  for (const FrameStep& st : g_steps)
    if (st.key < 0 && st.frame == g_frame && (!g_cfg.session_at_title || netplay::active()))
      video::request_shot(g_frame / 10.0);
  if (logging) fflush(g_log);  // the process may be terminated at any frame
  oInputsSync(self, edx);  // the game reads its devices: they report g_cur
}

template <size_t N>
void hook_checked(const char* name, uintptr_t addr, const uint8_t (&head)[N], void* fn, void** orig) {
  if (patch::bytes_are(addr, head, N))
    patch::hook(name, addr, fn, orig);
  else
    LOGE("sim: %s at 0x%08X has unexpected bytes; lockstep would desync", name, (unsigned)addr);
}

}  // namespace

// [debug] dump_data=1: the game's data objects to tmnt2net-data-<n>.bin (record 0x968, data 0x88) at script shots.
void dump_data(int n) {
  struct Obj { uintptr_t ptr; size_t size; const char* name; } objs[] = {{0x6D4428, 0x968, "record"}, {0x6D4424, 0x88, "data"}};
  for (const Obj& o : objs) {
    uint8_t* p = patch::read<uint8_t*>(o.ptr);
    if (!p) continue;
    wchar_t name[64];
    swprintf(name, 64, L"tmnt2net-%hs-%d.bin", o.name, n);
    FILE* f = _wfopen((module_dir() + name).c_str(), L"wb");
    if (f) { fwrite(p, 1, o.size, f); fclose(f); }
  }
}

bool active() { return g_active; }
bool starting_up() { return g_starting && !netplay::active(); }
void run_to_title_menu() {
  if (!g_starting) LOGI("sim: to the title menu at once (a network game waits there)");
  g_starting = true;
}

void title_menu_reached() {
  g_start_over = true;
  if (!g_starting) return;
  g_starting = false;
  LOGI("sim: the start-up ran at once to the title menu (frame %u)", g_frame);
}
uint32_t frame() { return g_frame; }

void apply_frame_keys(uint8_t keys[256]) {
  if (!g_active) return;
  if (g_cfg.session_at_title && !netplay::active()) return;  // [debug] the script is the session's
  for (const FrameStep& st : g_steps) {
    if (st.frame > g_frame) break;
    if (st.key >= 0 && g_frame < st.frame + st.hold) keys[st.key] = 0x80;
  }
}

void keyboard(uint8_t keys[256]) {
  for (int k = 0; k < 256; ++k) keys[k] = (g_cur.keys[k >> 3] >> (k & 7)) & 1 ? 0x80 : 0;
}

const PadInput& pad(int slot) {
  static const PadInput kNone;
  return slot >= 0 && slot < kPads ? g_cur.pads[slot] : kNone;
}

void install(const Config& cfg) {
  g_cfg = cfg;
  if (!cfg.determinism) return;
  g_active = true;
  // A session from frame 1 has its own start-up (netplay's g_boot); else this game's runs at once.
  g_starting = cfg.quick_start && !(cfg.net_on() && !cfg.session_at_title);
  LOGI("sim: determinism mode (no frame skipping, loads within the frame, input fixed per frame)");
  patch::replace("frame skipping off", game::kFrameSkipCheck, game::kFrameSkipOld, game::kFrameSkipNew,
                 sizeof(game::kFrameSkipNew));
  hook_checked("RpRandom (counted)", game::kRpRandom, game::kRpRandomHead, (void*)&hkRpRandom, (void**)&oRpRandom);
  hook_checked("RpRandomSeed (epochs)", game::kRpRandomSeed, game::kRpRandomSeedHead, (void*)&hkRpRandomSeed,
               (void**)&oRpRandomSeed);
  hook_checked("CMovie::IsEnded (from the frame's input)", game::kMovieIsEnded, game::kMovieIsEndedHead,
               (void*)&hkMovieIsEnded, (void**)&oMovieIsEnded);
  hook_checked("CPCMovie::CPCMovie (movies counted)", game::kPCMovieCtor, game::kPCMovieCtorHead,
               (void*)&hkPCMovieCtor, (void**)&oPCMovieCtor);
  hook_checked("CGameSound::IsVoicePlaying (from the frame's input)", game::kVoicePlaying, game::kVoicePlayingHead,
               (void*)&hkVoicePlaying, (void**)&oVoicePlaying);
  hook_checked("CGameSound::IsLoadEnd (sound loads within the frame)", game::kSoundIsLoadEnd,
               game::kSoundIsLoadEndHead, (void*)&hkSoundIsLoadEnd, (void**)&oSoundIsLoadEnd);
  patch::slot("CPCInputsDevice::Sync (frame)", game::kInputsSyncSlot, game::kInputsSync, (void*)&hkInputsSync);
  if (!cfg.frame_script.empty()) parse_frame_script(cfg.frame_script);
  std::wstring path = module_dir() + L"tmnt2net-sim.log";
  g_log = _wfopen(path.c_str(), L"w");
  if (g_log) fprintf(g_log, "# frame epoch rng_calls rng_hash input_hash\n");
  if (!cfg.record.empty()) {
    g_rec = _wfopen(full_path(cfg.record).c_str(), L"wb");
    if (g_rec) {
      fwrite(kRecMagic, sizeof(kRecMagic), 1, g_rec);
      fflush(g_rec);
      LOGI("sim: recording the input to %s", cfg.record.c_str());
    } else {
      LOGE("sim: cannot write %s", cfg.record.c_str());
    }
  }
}

}  // namespace sim
