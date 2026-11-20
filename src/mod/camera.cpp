#include "mod/camera.h"

#include <windows.h>
#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/log.h"
#include "mod/fixes.h"
#include "mod/game.h"
#include "mod/health.h"
#include "mod/netplay.h"
#include "mod/patch.h"
#include "mod/sim.h"
#include "mod/video.h"
#include "mod/widescreen.h"

namespace camera {
namespace {

Config g_cfg;

struct V3 {
  float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }

struct RwMatrix {  // RenderWare: right, up, at (forward), pos, each with a pad word
  V3 right;
  uint32_t flags;
  V3 up;
  uint32_t pad1;
  V3 at;
  uint32_t pad2;
  V3 pos;
  uint32_t pad3;
};
static_assert(sizeof(RwMatrix) == 64, "RwMatrix");

using CameraUpdate_t = void*(__cdecl*)(void* camera);
using FrameTransform_t = void*(__cdecl*)(void* frame, const RwMatrix* m, int combine);
using CamScene_t = bool(__fastcall*)(uint8_t* ccam, void* edx);  // CCamera::BeginScene / EndScene
using PlayerFn_t = void*(__cdecl*)(int n);

// The wheel's zoom: what the player asked for, and what is shown (eased towards it).
float g_zoom_want = 1.0f, g_zoom = 1.0f;
DWORD g_zoom_changed = 0;  // GetTickCount of the last wheel step not yet written to the ini

// This PC's view, kept from frame to frame (eased like the game's own camera).
struct View {
  void* map_camera = nullptr;  // the stage it belongs to
  V3 own{}, team{};            // smoothed: this PC's player, the players' middle
  float dist = 0;              // smoothed distance from the eye to the look-at point
  bool fresh = true;
} g_view;

// While a camera draws with this PC's view: what to put back before its EndScene.
struct V2 {
  float x, y;
};
struct Swap {
  uint8_t* ccam = nullptr;  // the game's CCamera
  void* camera = nullptr;   // its RwCamera
  void* frame = nullptr;    // the stage camera's frame, moved to this PC's player (else null)
  RwMatrix game{};
  bool window = false;      // the view window widened to the frame's shape / the player's fov
  V2 game_window{};
};
Swap g_swaps[4];
int g_nswaps = 0;

uint8_t* map_camera() {
  auto* prop = patch::read<uint8_t*>(game::kGameProperty);
  return prop ? patch::read<uint8_t*>(reinterpret_cast<uintptr_t>(prop) + game::kPropMapCamera) : nullptr;
}

void* rw_camera(uint8_t* map) {
  auto* cam = patch::read<uint8_t*>(reinterpret_cast<uintptr_t>(map) + game::kMapCamCamera);
  return cam ? patch::read<void*>(reinterpret_cast<uintptr_t>(cam) + game::kCameraRw) : nullptr;
}

int player_num() {
  auto* prop = patch::read<uint8_t*>(game::kGameProperty);
  if (!prop || !patch::read<void*>(reinterpret_cast<uintptr_t>(prop) + game::kPropPlayers)) return 0;
  return reinterpret_cast<int(__cdecl*)()>(game::kGetPlayerNum)();
}

bool player_pos(int n, V3& out) {
  void* gp = reinterpret_cast<PlayerFn_t>(game::kGamePlayer)(n);
  if (!gp) return false;
  void** vt = *reinterpret_cast<void***>(gp);
  auto* p = reinterpret_cast<const V3*>(
      reinterpret_cast<const void*(__fastcall*)(void*, void*)>(vt[game::kGpGetPosition / 4])(gp, nullptr));
  if (!p || !std::isfinite(p->x) || !std::isfinite(p->y) || !std::isfinite(p->z)) return false;
  out = *p;
  return true;
}

void save_zoom() {
  wchar_t v[16];
  swprintf(v, 16, L"%.2f", g_zoom_want);
  WritePrivateProfileStringW(L"video", L"camera_zoom", v, (module_dir() + L"tmnt2net.ini").c_str());
  LOGI("camera: zoom %.2f written to tmnt2net.ini", g_zoom_want);
}

// This PC's view for this frame from the game's camera matrix; false = draw the game's view.
bool own_view(uint8_t* map, const RwMatrix& gm, RwMatrix& out) {
  if (patch::read<int>(reinterpret_cast<uintptr_t>(map) + game::kMapCamMode) != game::kMapCamModeAuto ||
      patch::read<void*>(reinterpret_cast<uintptr_t>(map) + game::kMapCamIntroduction))
    return false;  // scripted
  char kind = patch::read<char>(reinterpret_cast<uintptr_t>(map) + game::kMapCamAreaName);
  bool path = kind == 'p' || kind == 'P' || kind == 'e' || kind == 'E';
  bool still = kind == 't' || kind == 'T' || kind == 'f' || kind == 'F';
  if (!path && !still) return false;

  V3 at = patch::read<V3>(reinterpret_cast<uintptr_t>(map) + game::kMapCamAt);
  float d_game = dot(at - gm.pos, gm.at);  // along the view
  if (!(d_game > 0.01f) || !std::isfinite(d_game)) return false;
  float zoom_game = patch::read<float>(reinterpret_cast<uintptr_t>(map) + game::kMapCamZoom);
  if (!(zoom_game >= 1.0f) || zoom_game > 10.0f) zoom_game = 1.0f;

  // Whose view: this PC's player in a network game, the only player alone; players sharing a
  // screen keep the team's view.
  int count = player_num();
  int mine = netplay::active() ? health::own_player() : (count == 1 ? 0 : -1);
  V3 own{}, team{};
  bool follow = path && g_cfg.own_camera && mine >= 0 && player_pos(mine, own);
  if (follow) {
    int n = 0;
    for (int i = 0; i < count && i < 4; ++i) {
      V3 p;
      if (!player_pos(i, p)) continue;
      team = team + p;
      ++n;
    }
    follow = n > 0;
    if (follow) team = team * (1.0f / (float)n);
  }

  if (!follow && g_zoom_want == 1.0f && std::fabs(g_zoom - 1.0f) < 0.001f) {  // the game's view as it is
    g_zoom = 1.0f;
    g_view.fresh = true;
    return false;
  }
  if (g_view.map_camera != map) {
    g_view = View{};
    g_view.map_camera = map;
  }
  g_zoom += (g_zoom_want - g_zoom) * 0.25f;
  if (std::fabs(g_zoom_want - g_zoom) < 0.001f) g_zoom = g_zoom_want;

  // The distance the game would use for this player alone (its co-op zoom backs off to keep
  // everyone in view), times the wheel's zoom; on fixed views the game's distance times the zoom.
  float d_want = (follow ? d_game / zoom_game : d_game) * g_zoom;
  if (follow && (g_view.fresh || length(own - g_view.own) > 4.0f)) {  // a stage start, a jump cut
    g_view.own = own;
    g_view.team = team;
    g_view.dist = d_want;
  }
  if (g_view.fresh) g_view.dist = d_want;
  g_view.fresh = false;
  if (follow) {
    g_view.own = lerp(g_view.own, own, 0.2f);
    g_view.team = lerp(g_view.team, team, 0.2f);
  }
  g_view.dist += (d_want - g_view.dist) * 0.15f;

  out = gm;
  V3 shift = follow ? g_view.own - g_view.team : V3{};
  V3 target = gm.pos + gm.at * d_game + shift;  // this view's look-at point
  V3 eye = target - gm.at * g_view.dist;
  // Not through walls: the line from the game's eye (always in the open) to this view's eye,
  // against the map, as the game checks its own co-op zoom; on a hit the eye stays on this side.
  // The map's result is a static the game's logic reads: kept as it was.
  V3 from = gm.pos;
  if (length(eye - from) > 0.01f) {
    uint8_t keep[game::kWorldCollisionResultSize];
    std::memcpy(keep, reinterpret_cast<const void*>(game::kWorldCollisionResult), sizeof(keep));
    if (reinterpret_cast<bool(__cdecl*)(const V3*, const V3*)>(game::kWorldCheckLine)(&from, &eye)) {
      V3 hit = patch::read<V3>(game::kWorldCollisionResult);
      V3 back = from - hit;
      float l = length(back);
      eye = l > 0.3f ? hit + back * (0.3f / l) : from;
    }
    std::memcpy(reinterpret_cast<void*>(game::kWorldCollisionResult), keep, sizeof(keep));
  }
  out.pos = eye;
  return true;
}

CamScene_t oBegin = nullptr, oEnd = nullptr;

// Netplay: the game's 3D cameras keep the session's 16:9 (and fov 1) for the logic; for the draw
// a camera of that shape gets the frame's shape (Hor+ from its height) times the player's fov.
bool draw_window(void* cam, V2& logic, V2& draw) {
  if (g_cfg.net_aspect <= 0) return false;
  V2 vw = patch::read<V2>(reinterpret_cast<uintptr_t>(cam) + game::kRwCameraViewWindow);
  if (!(vw.x > 0) || !(vw.y > 0) || std::fabs(vw.x / vw.y - g_cfg.net_aspect) > 0.01f) return false;
  int fw = 0, fh = 0;
  if (!video::frame_size(fw, fh) || fw <= 0 || fh <= 0) return false;
  float k = g_cfg.view_fov > 0 ? g_cfg.view_fov : 1.0f;
  V2 r{vw.y * (float)fw / (float)fh * k, vw.y * k};
  if (std::fabs(r.x - vw.x) < 1e-5f && std::fabs(r.y - vw.y) < 1e-5f) return false;
  logic = vw;
  draw = r;
  return true;
}

bool is_2d_camera(const uint8_t* ccam) {
  return ccam && !g_cfg.no_2d_fix && ccam == patch::read<const uint8_t*>(game::k2DCamera);
}

// [debug] MessageWindow::SetRect(x, y, w, h): where the game's message windows go.
using MsgRect_t = void(__fastcall*)(void* self, void* edx, float x, float y, float w, float h);
MsgRect_t oMsgRect = nullptr;
void __fastcall hkMsgRect(void* self, void* edx, float x, float y, float w, float h) {
  static int logged = 0;
  if (logged++ < 30)
    LOGI("[debug] message window %p from %p: x %.1f y %.1f w %.1f h %.1f", self, _ReturnAddress(), x, y, w, h);
  oMsgRect(self, edx, x, y, w, h);
}
using MsgLayout_t = void(__fastcall*)(uint8_t* self, void* edx, float a, float b, float c, float d);
MsgLayout_t oMsgLayout = nullptr;
void __fastcall hkMsgLayout(uint8_t* self, void* edx, float a, float b, float c, float d) {
  static int logged = 0;
  oMsgLayout(self, edx, a, b, c, d);
  if (logged++ < 40) {
    auto f = [](uintptr_t x) { return patch::read<float>(x); };
    uintptr_t s = reinterpret_cast<uintptr_t>(self);
    LOGI("[debug] window %p layout from %p (%.1f %.1f %.1f %.1f): sprite x %.1f w %.1f; vs %.1f %.1f; screen %d",
         self, _ReturnAddress(), a, b, c, d, f(s + 0x44 + 4), f(s + 0x44 + 0x10), f(game::kVirtualScreen),
         f(game::kVirtualScreen + 8), patch::read<int>(0x6DFFA4));
  }
}
// [debug] the character select's "are you sure" bar: the 2D state as it draws.
using SureDraw_t = void(__fastcall*)(uint8_t* self, void* edx);
SureDraw_t oSureDraw = nullptr;
void __fastcall hkSureDraw(uint8_t* self, void* edx) {
  static int logged = 0;
  if (logged++ < 2) {
    auto f = [](uintptr_t a) { return patch::read<float>(a); };
    uint8_t* c2d = patch::read<uint8_t*>(game::k2DCamera);
    uint8_t* rw = c2d ? patch::read<uint8_t*>(reinterpret_cast<uintptr_t>(c2d) + game::kCameraRw) : nullptr;
    uint8_t* rt = patch::read<uint8_t*>(0x7E2C4C);
    LOGI("[debug] sure bar draws: rt2d metric %.1f %.1f %.1f %.1f, view %.3f %.3f, raster %.0f %.0f, ctm level %d, "
         "cached %d; rt2d camera %p (2D camera's %p) view window %.3f x %.3f; vs %.1f %.1f; screen %dx%d",
         f(0x7E2C38), f(0x7E2C3C), f(0x7E2C40), f(0x7E2C44), f(0x7E2B48), f(0x7E2B4C), f(0x7E2B50), f(0x7E2B54),
         patch::read<int>(0x7E2B44), patch::read<int>(0x7E2C18), (void*)rt, (void*)rw,
         rt ? f(reinterpret_cast<uintptr_t>(rt) + 0x68) : 0.0f, rt ? f(reinterpret_cast<uintptr_t>(rt) + 0x6C) : 0.0f,
         f(game::kVirtualScreen), f(game::kVirtualScreen + 8), patch::read<int>(0x6DFFA4), patch::read<int>(0x6DFFA8));
    uint8_t* m = patch::read<uint8_t*>(0x7E2AC4 + 4 * patch::read<int>(0x7E2B44));
    if (m)
      LOGI("[debug] sure bar: ctm %.4f %.4f %.4f | %.4f %.4f %.4f | pos %.3f %.3f %.3f", f((uintptr_t)m), f((uintptr_t)m + 4),
           f((uintptr_t)m + 8), f((uintptr_t)m + 0x10), f((uintptr_t)m + 0x14), f((uintptr_t)m + 0x18), f((uintptr_t)m + 0x30),
           f((uintptr_t)m + 0x34), f((uintptr_t)m + 0x38));
  }
  oSureDraw(self, edx);
}
// [debug] Rt2dFontShow: which camera's scene the text is drawn in, and what Rt2d keeps.
void* g_scene_cam = nullptr;
using FontShow_t = void*(__cdecl*)(void* font, const void* text, float height, void* anchor, void* brush);
FontShow_t oFontShow = nullptr;
void* __cdecl hkFontShow(void* font, const void* text, float height, void* anchor, void* brush) {
  static void* seen[12];
  static float seen_w[12];
  static int nseen = 0;
  auto f = [](uintptr_t a) { return patch::read<float>(a); };
  uint8_t* eng = patch::read<uint8_t*>(0x6EE738);
  void* sc = eng ? patch::read<void*>(reinterpret_cast<uintptr_t>(eng)) : g_scene_cam;  // RwEngineInstance->curCamera
  float w = sc ? f(reinterpret_cast<uintptr_t>(sc) + 0x68) : 0.0f;
  bool known = false;
  for (int i = 0; i < nseen; ++i) known = known || (seen[i] == sc && seen_w[i] == w);
  if (!known && nseen < 12) {
    seen[nseen] = sc;
    seen_w[nseen++] = w;
    uint8_t* rt = patch::read<uint8_t*>(0x7E2C4C);
    uint8_t* m = patch::read<uint8_t*>(0x7E2AC4 + 4 * patch::read<int>(0x7E2B44));
    LOGI("[debug] text from %p h %.1f: scene camera %p view window %.3f x %.3f; rt2d camera %p view window %.3f x %.3f; "
         "rt2d view %.3f %.3f metric %.1f %.1f %.1f %.1f; ctm %.3f %.3f / %.3f %.3f",
         _ReturnAddress(), height, sc, w, sc ? f(reinterpret_cast<uintptr_t>(sc) + 0x6C) : 0.0f, (void*)rt,
         rt ? f(reinterpret_cast<uintptr_t>(rt) + 0x68) : 0.0f, rt ? f(reinterpret_cast<uintptr_t>(rt) + 0x6C) : 0.0f,
         f(0x7E2B48), f(0x7E2B4C), f(0x7E2C38), f(0x7E2C3C), f(0x7E2C40), f(0x7E2C44), m ? f((uintptr_t)m) : 0.0f,
         m ? f((uintptr_t)m + 4) : 0.0f, m ? f((uintptr_t)m + 0x10) : 0.0f, m ? f((uintptr_t)m + 0x14) : 0.0f);
  }
  return oFontShow(font, text, height, anchor, brush);
}
using MsgDraw_t = void(__fastcall*)(uint8_t* self, void* edx);
MsgDraw_t oMsgDraw = nullptr;
void __fastcall hkMsgDraw(uint8_t* self, void* edx) {
  static uint8_t* seen[16];
  static int nseen = 0;
  bool known = false;
  for (int i = 0; i < nseen; ++i) known = known || seen[i] == self;
  if (self && patch::read<int>(reinterpret_cast<uintptr_t>(self) + 0x134) >= 1 && !known && nseen < 16) {
    seen[nseen++] = self;
    auto f = [](uintptr_t a) { return patch::read<float>(a); };
    uintptr_t b = reinterpret_cast<uintptr_t>(self);
    LOGI("[debug] message window %p draws (state %d): sprites x %.1f w %.1f | x %.1f w %.1f | x %.1f w %.1f; vs %.1f %.1f; "
         "rt2d metric %.1f %.1f %.1f %.1f, view %.3f %.3f raster %.0f %.0f; screen %d",
         self, patch::read<int>(b + 0x134), f(b + 4 + 4), f(b + 4 + 0x10), f(b + 0x44 + 4), f(b + 0x44 + 0x10), f(b + 0x84 + 4), f(b + 0x84 + 0x10),
         f(game::kVirtualScreen), f(game::kVirtualScreen + 8), f(0x7E2C38), f(0x7E2C3C), f(0x7E2C40), f(0x7E2C44),
         f(0x7E2B48), f(0x7E2B4C), f(0x7E2B50), f(0x7E2B54), patch::read<int>(0x6DFFA4));
  }
  oMsgDraw(self, edx);
}

// The 2D camera (menus, the area map, the computer, texts, gauges) takes the frame's shape for good,
// as off line: no logic looks through it, and the game lays part of its 2D on its view window
// outside a scene (Rt2d keeps it at Rt2dDeviceSetCamera; texts are measured with it), so the
// session's 16:9 swapped only for the draw squeezed menus on other frames. Set as the 2D starts
// and after a video mode change (both give Rt2d the camera then), before Rt2d keeps it.
using Rt2dSetCamera_t = int(__cdecl*)(void* cam);
Rt2dSetCamera_t oRt2dSetCamera = nullptr;

int __cdecl hkRt2dSetCamera(void* cam) {
  const uint8_t* c2d = patch::read<const uint8_t*>(game::k2DCamera);
  int fw = 0, fh = 0;
  if (cam && g_cfg.net_aspect > 0 && !g_cfg.no_2d_fix && c2d && patch::read<void*>(reinterpret_cast<uintptr_t>(c2d) + game::kCameraRw) == cam &&
      video::frame_size(fw, fh) && fw > 0 && fh > 0) {
    V2 vw = patch::read<V2>(reinterpret_cast<uintptr_t>(cam) + game::kRwCameraViewWindow);
    V2 r{vw.y * (float)fw / (float)fh, vw.y};
    if (vw.y > 0 && std::fabs(r.x - vw.x) > 1e-5f) {
      widescreen::set_view_window_raw(cam, &r.x);
      LOGI("camera: the 2D camera takes the frame's shape: view window %.3f x %.3f (%dx%d)", r.x, r.y, fw, fh);
    }
  }
  return oRt2dSetCamera(cam);
}

// The frame's own camera (CGraphicsDevice's: every other camera is its clone, and the game draws under it
// whatever is outside a CCamera scene - in a stage the texts of its windows) keeps the session's 16:9 as
// the 3D cameras do. Rt2d lays texts out on the 2D camera's window (the frame's shape), so on a 21:9 or
// 32:9 frame those texts were stretched sideways. For the draw it takes the 2D camera's window; the
// game's own back at the end of the frame (the logic runs outside the draw).
using RenderBegin_t = bool(__fastcall*)(uint8_t* dev, void* edx);
using RenderEnd_t = void(__fastcall*)(uint8_t* dev, void* edx);
RenderBegin_t oRenderBegin = nullptr;
RenderEnd_t oRenderEnd = nullptr;
struct FrameSwap {
  void* camera = nullptr;
  V2 game_window{};
  bool on = false;
} g_frame_swap;

bool __fastcall hkRenderBegin(uint8_t* dev, void* edx) {
  sim::arithmetic();  // (drawn effects compute too)
  void* cam = dev ? patch::read<void*>(reinterpret_cast<uintptr_t>(dev) + game::kDeviceCamera) : nullptr;
  const uint8_t* c2d = patch::read<const uint8_t*>(game::k2DCamera);
  void* cam2d = c2d ? patch::read<void*>(reinterpret_cast<uintptr_t>(c2d) + game::kCameraRw) : nullptr;
  g_frame_swap.on = false;
  if (cam && cam2d && cam != cam2d && g_cfg.net_aspect > 0 && !g_cfg.no_2d_fix && !fixes::quitting()) {
    V2 mine = patch::read<V2>(reinterpret_cast<uintptr_t>(cam) + game::kRwCameraViewWindow);
    V2 want = patch::read<V2>(reinterpret_cast<uintptr_t>(cam2d) + game::kRwCameraViewWindow);
    if (want.x > 0 && want.y > 0 && mine.x > 0 && mine.y > 0 &&
        (std::fabs(want.x - mine.x) > 1e-5f || std::fabs(want.y - mine.y) > 1e-5f)) {
      g_frame_swap.camera = cam;
      g_frame_swap.game_window = mine;
      g_frame_swap.on = true;
      widescreen::set_view_window_raw(cam, &want.x);
      static bool logged = false;
      if (!logged) {
        logged = true;
        LOGI("camera: the frame's camera draws with the 2D camera's window %.3f x %.3f (the game's %.3f x %.3f)", want.x,
             want.y, mine.x, mine.y);
      }
    }
  }
  bool ok = oRenderBegin(dev, edx);
  if (!ok && g_frame_swap.on) {
    widescreen::set_view_window_raw(g_frame_swap.camera, &g_frame_swap.game_window.x);
    g_frame_swap.on = false;
  }
  return ok;
}

void __fastcall hkRenderEnd(uint8_t* dev, void* edx) {
  oRenderEnd(dev, edx);
  if (g_frame_swap.on) {
    widescreen::set_view_window_raw(g_frame_swap.camera, &g_frame_swap.game_window.x);
    g_frame_swap.on = false;
  }
}

// The camera's view matrix and frustum again from its frame and view window: its own end and
// begin of an update, as the 2D layer does inside a scene (nothing is drawn in between).
void resync(void* cam) {
  auto end = patch::read<CameraUpdate_t>(reinterpret_cast<uintptr_t>(cam) + game::kRwCameraEndUpdate);
  auto begin = patch::read<CameraUpdate_t>(reinterpret_cast<uintptr_t>(cam) + game::kRwCameraBeginUpdate);
  end(cam);
  begin(cam);
}

void put_back(const Swap& s) {
  if (s.window) widescreen::set_view_window_raw(s.camera, &s.game_window.x);
  if (s.frame) reinterpret_cast<FrameTransform_t>(game::kRwFrameTransform)(s.frame, &s.game, 0);
}

// CCamera::BeginScene: the stage camera's frame on this PC's player; in a network game every 3D
// camera with the frame's shape and the player's fov.
bool __fastcall hkBegin(uint8_t* ccam, void* edx) {
  uint8_t* map = map_camera();
  if (g_cfg.log_cameras) {
    static void* seen[48][2];
    static int nseen = 0;
    void* from = _ReturnAddress();
    bool known = false;
    for (int i = 0; i < nseen && !known; ++i) known = seen[i][0] == ccam && seen[i][1] == from;
    if (!known && nseen < 48) {
      seen[nseen][0] = ccam;
      seen[nseen++][1] = from;
      void* c = ccam ? patch::read<void*>(reinterpret_cast<uintptr_t>(ccam) + game::kCameraRw) : nullptr;
      V2 vw = c ? patch::read<V2>(reinterpret_cast<uintptr_t>(c) + game::kRwCameraViewWindow) : V2{};
      LOGI("[debug] camera scene: CCamera %p (RwCamera %p) from %p, view window %.3f x %.3f, map camera %s, 2D camera %s",
           (void*)ccam, c, from, vw.x, vw.y,
           map && ccam == patch::read<uint8_t*>(reinterpret_cast<uintptr_t>(map) + game::kMapCamCamera) ? "yes" : "no",
           ccam == patch::read<uint8_t*>(game::k2DCamera) ? "yes" : "no");
    }
  }
  if (!map) g_view.fresh = true;
  Swap s;
  void* cam = ccam ? patch::read<void*>(reinterpret_cast<uintptr_t>(ccam) + game::kCameraRw) : nullptr;
  if (cam && !fixes::quitting() && g_nswaps < 4) {
    s.ccam = ccam;
    s.camera = cam;
    if (map && ccam == patch::read<uint8_t*>(reinterpret_cast<uintptr_t>(map) + game::kMapCamCamera)) {
      void* frame = patch::read<void*>(reinterpret_cast<uintptr_t>(cam) + game::kRwObjectParent);
      if (frame) {
        RwMatrix gm = patch::read<RwMatrix>(reinterpret_cast<uintptr_t>(frame) + game::kRwFrameModelling);
        RwMatrix mine;
        if (own_view(map, gm, mine)) {
          s.frame = frame;
          s.game = gm;
          reinterpret_cast<FrameTransform_t>(game::kRwFrameTransform)(frame, &mine, 0);  // rwCOMBINEREPLACE
        }
      }
    }
    V2 draw;
    if (!is_2d_camera(ccam) && draw_window(cam, s.game_window, draw)) {
      s.window = true;
      widescreen::set_view_window_raw(cam, &draw.x);
    }
  }
  bool swapped = s.frame || s.window;
  if (swapped) g_swaps[g_nswaps++] = s;
  bool ok = oBegin(ccam, edx);
  if (ok) g_scene_cam = cam;
  if (!ok && swapped) {  // nothing drawn: just the game's view back
    put_back(s);
    --g_nswaps;
  }
  return ok;
}

// CCamera::EndScene: the game's view again (frame, view window, then the camera's matrices from
// them), then the game's EndScene.
bool __fastcall hkEnd(uint8_t* ccam, void* edx) {
  if (g_nswaps > 0 && g_swaps[g_nswaps - 1].ccam == ccam) {
    const Swap s = g_swaps[--g_nswaps];
    put_back(s);
    resync(s.camera);
  }
  bool ok = oEnd(ccam, edx);
  g_scene_cam = nullptr;
  if (g_zoom_changed && GetTickCount() - g_zoom_changed > 2000) {
    g_zoom_changed = 0;
    save_zoom();
  }
  return ok;
}

}  // namespace

void wheel(int delta) {
  if (delta == 0) return;
  float k = std::pow(1.1f, (float)-delta / (float)WHEEL_DELTA);  // wheel up = closer, down = farther
  float z = g_zoom_want * k;
  z = z < game::kOwnZoomMin ? game::kOwnZoomMin : z > game::kOwnZoomMax ? game::kOwnZoomMax : z;
  if (z == g_zoom_want) return;
  g_zoom_want = z;
  g_zoom_changed = GetTickCount() | 1;
}

void set_own(bool own) { g_cfg.own_camera = own; }

void reset_zoom() {
  if (g_zoom_want == 1.0f) return;
  g_zoom_want = 1.0f;
  g_zoom_changed = GetTickCount() | 1;
}

void install(const Config& cfg) {
  g_cfg = cfg;
  g_zoom_want = g_zoom = cfg.camera_zoom;
  bool ok = patch::bytes_are(game::kCamBeginScene, game::kCamBeginSceneHead, sizeof(game::kCamBeginSceneHead)) &&
            patch::bytes_are(game::kCamEndScene, game::kCamEndSceneHead, sizeof(game::kCamEndSceneHead)) &&
            patch::bytes_are(game::kMapCamLookAtFrame, game::kMapCamLookAtFrameBytes,
                             sizeof(game::kMapCamLookAtFrameBytes)) &&
            patch::bytes_are(game::kWorldCheckLine, game::kWorldCheckLineHead, sizeof(game::kWorldCheckLineHead)) &&
            patch::read<uint32_t>(0x507631) == game::kWorldCollisionResult;
  if (!ok) {
    LOGW("camera: camera code not recognised, the game's view only");
    return;
  }
  if (!patch::hook("CCamera::BeginScene (own view for the stage's draw)", game::kCamBeginScene, (void*)&hkBegin,
                   (void**)&oBegin) ||
      !patch::hook("CCamera::EndScene (the game's view back)", game::kCamEndScene, (void*)&hkEnd, (void**)&oEnd))
    return;
  if (cfg.net_aspect > 0 &&
      (!patch::bytes_are(game::kRt2dDeviceSetCamera, game::kRt2dDeviceSetCameraHead,
                         sizeof(game::kRt2dDeviceSetCameraHead)) ||
       !patch::hook("Rt2dDeviceSetCamera (the 2D camera in the frame's shape)", game::kRt2dDeviceSetCamera,
                    (void*)&hkRt2dSetCamera, (void**)&oRt2dSetCamera)))
    LOGW("camera: 2D camera not recognised, menus keep 16:9 on other frames");
  if (cfg.net_aspect > 0 && oRt2dSetCamera) {
    if (!patch::bytes_are(game::kRenderBegin, game::kRenderBeginHead, sizeof(game::kRenderBeginHead)) ||
        !patch::bytes_are(game::kRenderEnd, game::kRenderEndHead, sizeof(game::kRenderEndHead)) ||
        !patch::hook("CGraphicsDevice::RenderBegin (the frame's camera in the frame's shape)", game::kRenderBegin,
                     (void*)&hkRenderBegin, (void**)&oRenderBegin) ||
        !patch::hook("CGraphicsDevice::RenderEnd (the game's window back)", game::kRenderEnd, (void*)&hkRenderEnd,
                     (void**)&oRenderEnd))
      LOGW("camera: frame code not recognised, stage texts keep 16:9 on other frames");
  }
  if (cfg.log_cameras) {
    patch::hook("[debug] MessageWindow::SetRect", 0x4BECE0, (void*)&hkMsgRect, (void**)&oMsgRect);
    patch::hook("[debug] MessageWindow::Draw", 0x4BF300, (void*)&hkMsgDraw, (void**)&oMsgDraw);
    patch::hook("[debug] MessageWindow::Layout", 0x4BED60, (void*)&hkMsgLayout, (void**)&oMsgLayout);
    patch::hook("[debug] character select: sure bar", 0x4E26D0, (void*)&hkSureDraw, (void**)&oSureDraw);
    patch::hook("[debug] Rt2dFontShow", 0x5B48C0, (void*)&hkFontShow, (void**)&oFontShow);
  }
  LOGI("camera: %s, zoom %.2f (mouse wheel; middle button: 1.0)",
       cfg.own_camera ? "own view (this PC's player)" : "the team's view", g_zoom_want);
}

}  // namespace camera
