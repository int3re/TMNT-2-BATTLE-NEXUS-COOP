#include "mod/widescreen.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <string>

#include "common/log.h"
#include "mod/game.h"
#include "mod/patch.h"
#include "mod/video.h"

namespace widescreen {
namespace {

constexpr float kAspect43 = 4.0f / 3.0f;

Config g_cfg;

struct RwV2d {
  float x, y;
};

bool raster_size(const uint8_t* camera, int& w, int& h) {
  if (!camera) return false;
  const uint8_t* raster = patch::read<const uint8_t*>(reinterpret_cast<uintptr_t>(camera) + game::kCameraFrameBuffer);
  if (!raster) return false;
  w = patch::read<int>(reinterpret_cast<uintptr_t>(raster) + game::kRasterWidth);
  h = patch::read<int>(reinterpret_cast<uintptr_t>(raster) + game::kRasterHeight);
  return w > 0 && h > 0;
}

// --- Hor+ ------------------------------------------------------------------------
// The game sets view windows of (x, 0.75x). On a frame wider than 4:3 the
// vertical size is kept and the horizontal one grows (Hor+); on a narrower
// frame (5:4) the horizontal size is kept and the vertical one grows.
using SetViewWindow_t = void*(__cdecl*)(void* camera, const RwV2d* vw);
SetViewWindow_t oSetViewWindow = nullptr;

void* __cdecl hkSetViewWindow(void* camera, const RwV2d* vw) {
  int w = 0, h = 0;
  if (!camera || !vw || vw->x <= 0 || vw->y <= 0 || std::fabs(vw->x / vw->y - kAspect43) > 0.01f ||
      !raster_size(static_cast<uint8_t*>(camera), w, h))
    return oSetViewWindow(camera, vw);
  float aspect = float(w) / float(h);
  // Netplay: the view window is part of the game's logic (enemy activity, the
  // co-op zoom), so it is the session's exact aspect, not the frame's rounding.
  // Not the 2D camera's (menus, texts): no logic looks through it (camera.cpp).
  const uint8_t* c2d = patch::read<const uint8_t*>(game::k2DCamera);
  bool is_2d = c2d && patch::read<void*>(reinterpret_cast<uintptr_t>(c2d) + game::kCameraRw) == camera;
  if (g_cfg.net_aspect > 0 && (!is_2d || g_cfg.no_2d_fix)) aspect = g_cfg.net_aspect;
  RwV2d out = *vw;
  if (aspect > kAspect43 + 0.005f)
    out.x = vw->y * aspect;
  else if (aspect < kAspect43 - 0.005f)
    out.y = vw->x / aspect;
  int fw = 0, fh = 0;
  if (g_cfg.fov != 1.0f && video::frame_size(fw, fh) && fw == w && fh == h) {
    out.x *= g_cfg.fov;
    out.y *= g_cfg.fov;
  }
  if (out.x != vw->x || out.y != vw->y) {
    static int logged_w = 0, logged_h = 0;
    if (logged_w != w || logged_h != h) {
      logged_w = w;
      logged_h = h;
      LOGI("widescreen: camera on %dx%d, view window %.3f x %.3f -> %.3f x %.3f", w, h, vw->x, vw->y, out.x, out.y);
    }
    return oSetViewWindow(camera, &out);
  }
  return oSetViewWindow(camera, vw);
}

// --- 2D in the 4:3 middle -------------------------------------------------------
// CSprite::SetDefaultVirtualScreen (0x52E4A0) sets (-320, -240, 640, 480); the
// same values go to Rt2dDeviceSetMetric. Widening them keeps 640x480 virtual
// units in the 4:3 middle of a wider frame (taller for 5:4).
using SetDefaultVirtualScreen_t = void(__cdecl*)();
SetDefaultVirtualScreen_t oSetDefaultVirtualScreen = nullptr;

void __cdecl hkSetDefaultVirtualScreen() {
  oSetDefaultVirtualScreen();
  int w = 0, h = 0;
  if (!g_cfg.hud_center || !video::frame_size(w, h)) return;
  float aspect = float(w) / float(h);
  auto* vs = reinterpret_cast<float*>(game::kVirtualScreen);  // x, y, w, h
  if (aspect > kAspect43 + 0.005f) {
    float k = aspect / kAspect43;
    vs[0] *= k;
    vs[2] *= k;
  } else if (aspect < kAspect43 - 0.005f) {
    float k = kAspect43 / aspect;
    vs[1] *= k;
    vs[3] *= k;
  } else {
    return;
  }
  LOGI("widescreen: 2D virtual screen %.1f %.1f %.1f x %.1f on a %dx%d frame (2D kept 4:3)", vs[0], vs[1], vs[2],
       vs[3], w, h);
}

// Rt2d (text, message windows, Maestro scenes): a 4:3 metric is widened the same
// way, so 640 metric units cover the 4:3 middle. Metrics already wider than 4:3
// (from the widened virtual screen) pass unchanged.
using Rt2dDeviceSetMetric_t = int(__cdecl*)(float x, float y, float w, float h);
Rt2dDeviceSetMetric_t oRt2dDeviceSetMetric = nullptr;

int __cdecl hkRt2dDeviceSetMetric(float x, float y, float w, float h) {
  int fw = 0, fh = 0;
  if (g_cfg.hud_center && w > 0 && h > 0 && std::fabs(w / h - kAspect43) < 0.01f && video::frame_size(fw, fh)) {
    float aspect = float(fw) / float(fh);
    if (aspect > kAspect43 + 0.005f) {
      float k = aspect / kAspect43;
      float cx = x + w * 0.5f;
      w *= k;
      x = cx - w * 0.5f;
    } else if (aspect < kAspect43 - 0.005f) {
      float k = kAspect43 / aspect;
      float cy = y + h * 0.5f;
      h *= k;
      y = cy - h * 0.5f;
    }
    static int logged = 0;
    if (logged++ < 2) LOGI("widescreen: Rt2d metric %.1f %.1f %.1f x %.1f (text kept 4:3)", x, y, w, h);
  }
  return oRt2dDeviceSetMetric(x, y, w, h);
}

// --- the stage's gauges in the frame's corners ------------------------------------
// With the 2D kept 4:3 the health ring (top left) and the players' boxes (top
// right) sit in the 4:3 corners; they move out by the extra half-width (virtual
// units), keeping their margins from the frame's edges. The ring: the 2D origin is
// moved while it runs and draws (its sprites take pixel places then). The boxes
// and the icons beside the ring: their start x in CGaugeStatus (its draw also
// puts the markers over the players, which must stay where they are).
float corner_shift() {
  if (!g_cfg.hud_center || !g_cfg.gauges_corners) return 0.0f;
  float d = -reinterpret_cast<const float*>(game::kVirtualScreen)[0] - 320.0f;
  return d > 0.5f ? d : 0.0f;
}

// gauges_size: the gauges drawn smaller towards their corner of the HUD's area (the frame's
// corners with gauges = corners, else the 4:3 middle's): the 2D virtual screen widened by 1/s
// around that corner while they draw (sprites take pixel places and sizes from it then).
float gauge_scale() {
  float s = g_cfg.gauges_scale;
  return s >= 0.5f && s <= 1.0f ? s : 1.0f;
}

struct SavedScreen {
  float vs[4];
};
SavedScreen g_right_saved;
int g_right_depth = 0;

using GaugeFn_t = void(__fastcall*)(uint8_t* self, void* edx);
GaugeFn_t oMeterRun = nullptr, oMeterDraw = nullptr, oStatusRun = nullptr, oStatusDraw = nullptr;

void left_shifted(GaugeFn_t fn, uint8_t* self, void* edx) {
  auto* vs = reinterpret_cast<volatile float*>(game::kVirtualScreen);
  float keep[4] = {vs[0], vs[1], vs[2], vs[3]};
  float d = corner_shift(), s = gauge_scale();
  // Moved left by d, then scaled by s around the HUD area's top left (-320 - d, -240).
  vs[0] = -320.0f + (keep[0] + 320.0f + d) / s;
  vs[1] = -240.0f + (keep[1] + 240.0f) / s;
  vs[2] = keep[2] / s;
  vs[3] = keep[3] / s;
  fn(self, edx);
  for (int i = 0; i < 4; ++i) vs[i] = keep[i];
}

void __fastcall hkMeterRun(uint8_t* self, void* edx) { left_shifted(oMeterRun, self, edx); }
void __fastcall hkMeterDraw(uint8_t* self, void* edx) { left_shifted(oMeterDraw, self, edx); }

void place_status(uint8_t* self) {
  if (!self || !self[0x16]) return;  // not set up yet
  float d = corner_shift(), s = gauge_scale();
  *reinterpret_cast<float*>(self + game::kGaugeBoxesX) = game::kGaugeBoxesX0 + d;
  // The icons beside the ring follow it (moved and scaled around the same corner).
  *reinterpret_cast<float*>(self + game::kGaugeIconsX) = -320.0f - d + s * (game::kGaugeIconsX0 + 320.0f);
  *reinterpret_cast<float*>(self + game::kGaugeIconsY) = -240.0f + s * (game::kGaugeIconsY0 + 240.0f);
  static float logged = -1;
  if (d != logged) {
    logged = d;
    LOGI("widescreen: stage gauges moved %.1f units out to the corners", d);
  }
}

void __fastcall hkStatusRun(uint8_t* self, void* edx) {
  oStatusRun(self, edx);
  place_status(self);
}
void __fastcall hkStatusDraw(uint8_t* self, void* edx) {
  place_status(self);
  oStatusDraw(self, edx);
}

bool wider_than_43(int& w, int& h, int& w43) {
  w = patch::read<int>(game::kScreenStaticWidth);
  h = patch::read<int>(game::kScreenStaticHeight);
  if (w <= 0 || h <= 0 || w * 3 <= h * 4 + 3) return false;
  w43 = (h * 4 + 1) / 3;
  return true;
}

// --- Rt2d Maestro scenes (menus made in the 2D animation tool) ---------------------
// CAnimation2D::Start fits the scene's bbox onto the whole frame. With the screen
// width set to 4:3 for the call the scene lands in the left 4:3 part; it is
// then moved right by half the extra width (in scene units: pixels * bbox.w / w43).
struct Rt2dBBox {
  float x, y, w, h;
};
using Anim2DStart_t = void(__fastcall*)(uint8_t* self, void* edx);
Anim2DStart_t oAnim2DStart = nullptr;
using MaestroGetBBox_t = Rt2dBBox*(__cdecl*)(void* maestro);
using MaestroGetScene_t = void*(__cdecl*)(void* maestro);
using ObjectMTMTranslate_t = void*(__cdecl*)(void* object, float x, float y);

void __fastcall hkAnim2DStart(uint8_t* self, void* edx) {
  int w, h, w43;
  void* maestro = self ? patch::read<void*>(reinterpret_cast<uintptr_t>(self) + game::kAnim2DMaestro) : nullptr;
  if (!g_cfg.hud_center || !maestro || !wider_than_43(w, h, w43)) {
    oAnim2DStart(self, edx);
    return;
  }
  auto* width = reinterpret_cast<volatile int*>(game::kScreenStaticWidth);
  *width = w43;
  oAnim2DStart(self, edx);
  *width = w;
  auto bbox = reinterpret_cast<MaestroGetBBox_t>(game::kRt2dMaestroGetBBox)(maestro);
  void* scene = reinterpret_cast<MaestroGetScene_t>(game::kRt2dMaestroGetScene)(maestro);
  if (!bbox || !scene || bbox->w <= 0) return;
  float dx = (w - w43) * 0.5f * bbox->w / float(w43);
  reinterpret_cast<ObjectMTMTranslate_t>(game::kRt2dObjectMTMTranslate)(scene, dx, 0.0f);
  static int logged = 0;
  if (logged++ < 3) LOGI("widescreen: 2D scene %.0fx%.0f fitted into the 4:3 middle (%dx%d frame)", bbox->w, bbox->h, w, h);
}

// --- movies (CRI Sofdec) -------------------------------------------------------------
// The movie sprite covers the whole frame in real pixels; for the draw it is
// narrowed to 4:3 in the middle (black bars, the frame is cleared to black).
using MovieDraw_t = void(__fastcall*)(uint8_t* self, void* edx);
MovieDraw_t oMovieDraw = nullptr;

void __fastcall hkMovieDraw(uint8_t* self, void* edx) {
  int w, h, w43;
  uint8_t* sprite = self ? patch::read<uint8_t*>(reinterpret_cast<uintptr_t>(self) + game::kMovieSprite) : nullptr;
  if (!sprite || !wider_than_43(w, h, w43)) {
    oMovieDraw(self, edx);
    return;
  }
  auto* x = reinterpret_cast<float*>(sprite + game::kSpriteX);
  auto* sw = reinterpret_cast<float*>(sprite + game::kSpriteW);
  float sh = *reinterpret_cast<float*>(sprite + game::kSpriteH);
  float ax = *reinterpret_cast<float*>(sprite + game::kSpriteAnchorX);
  float x0 = *x - ax * *sw;
  if (x0 > 1.0f || x0 + *sw < w - 1.0f || sh <= 0) {  // not a full-width movie
    oMovieDraw(self, edx);
    return;
  }
  float old_x = *x, old_w = *sw;
  float nw = sh * 4.0f / 3.0f;
  *sw = nw;
  *x = (w - nw) * 0.5f + ax * nw;
  oMovieDraw(self, edx);
  *x = old_x;
  *sw = old_w;
}

}  // namespace

void set_gauges(bool corners, float scale) {
  g_cfg.gauges_corners = corners;
  g_cfg.gauges_scale = scale;
}

void right_gauge_begin() {
  if (g_right_depth++ > 0) return;
  auto* vs = reinterpret_cast<float*>(game::kVirtualScreen);
  for (int i = 0; i < 4; ++i) g_right_saved.vs[i] = vs[i];
  float s = gauge_scale();
  if (s >= 1.0f) return;
  float r = 320.0f + corner_shift();  // the HUD area's right edge
  vs[0] = r + (g_right_saved.vs[0] - r) / s;
  vs[1] = -240.0f + (g_right_saved.vs[1] + 240.0f) / s;
  vs[2] = g_right_saved.vs[2] / s;
  vs[3] = g_right_saved.vs[3] / s;
}

void right_gauge_end() {
  if (--g_right_depth > 0) return;
  auto* vs = reinterpret_cast<float*>(game::kVirtualScreen);
  for (int i = 0; i < 4; ++i) vs[i] = g_right_saved.vs[i];
}

// The frame has a new size (a new video mode while the game runs): the 2D's virtual screen and Rt2d's
// metric again for its shape. The game itself keeps both for good (640 x 480), so nothing of its own
// sets them again on a frame of another shape.
// The order is Rt2d's: the camera first (the game's own 2D reset gives it the 2D camera, which takes
// the new frame's shape there), then the metric, whose scale is worked out from that camera.
void frame_changed() {
  if (oSetDefaultVirtualScreen) hkSetDefaultVirtualScreen();
  reinterpret_cast<bool(__cdecl*)()>(game::kSystem2DReset)();
  const float* vs = reinterpret_cast<const float*>(game::kVirtualScreen);
  if (oSetDefaultVirtualScreen && oRt2dDeviceSetMetric) oRt2dDeviceSetMetric(vs[0], vs[1], vs[2], vs[3]);
}

void set_view_window_raw(void* camera, const float* xy) {
  RwV2d vw{xy[0], xy[1]};
  if (oSetViewWindow)
    oSetViewWindow(camera, &vw);
  else
    reinterpret_cast<SetViewWindow_t>(game::kSetViewWindow)(camera, &vw);
}

// Two bars of the game take the frame's width in PIXELS as their width in virtual units ("fild ScreenW"
// before CSprite::Resize): right on a 640-pixel frame only. On 1920x1080 the bar is three frames wide, its
// gradient and edges stretched and cut. They get the virtual screen's own width: the 4:3 middle, where the
// rest of the page is (the whole frame when the 2D is stretched).
const float kBarWidth = 640.0f;

void fix_bars() {
  static const struct {
    const char* name;
    uintptr_t at;
  } kSites[] = {{"character select: the ARE YOU SURE bar as wide as the page", 0x4E2720},
                {"results: the prize row's bar as wide as the page", 0x4EDF08}};
  const uint8_t expect[6] = {0xDB, 0x05, 0xA4, 0xFF, 0x6D, 0x00};  // fild dword ptr [ScreenW]
  uint8_t code[6] = {0xD9, 0x05};                                  // fld dword ptr [kBarWidth]
  const float* w = &kBarWidth;
  std::memcpy(code + 2, &w, 4);
  for (const auto& s : kSites) patch::replace(s.name, s.at, expect, code, sizeof(code));
}

void install(const Config& cfg) {
  g_cfg = cfg;
  fix_bars();
  if (cfg.aspect != Config::Aspect::Wide) {
    LOGI("widescreen: off (aspect %s)", cfg.aspect == Config::Aspect::Classic ? "4:3" : "stretch");
    return;
  }
  if (patch::bytes_are(game::kSetViewWindow, game::kSetViewWindowHead, sizeof(game::kSetViewWindowHead)))
    patch::hook("RwCameraSetViewWindow (Hor+)", game::kSetViewWindow, (void*)&hkSetViewWindow,
                (void**)&oSetViewWindow);
  else
    LOGW("widescreen: RwCameraSetViewWindow not recognised, no Hor+");
  if (patch::bytes_are(game::kSetDefaultVirtualScreen, game::kSetDefaultVirtualScreenHead,
                       sizeof(game::kSetDefaultVirtualScreenHead)))
    patch::hook("CSprite::SetDefaultVirtualScreen (2D in 4:3)", game::kSetDefaultVirtualScreen,
                (void*)&hkSetDefaultVirtualScreen, (void**)&oSetDefaultVirtualScreen);
  else
    LOGW("widescreen: virtual screen code not recognised, 2D stays stretched");
  if (patch::bytes_are(game::kRt2dDeviceSetMetric, game::kRt2dDeviceSetMetricHead,
                       sizeof(game::kRt2dDeviceSetMetricHead)))
    patch::hook("Rt2dDeviceSetMetric (text in 4:3)", game::kRt2dDeviceSetMetric, (void*)&hkRt2dDeviceSetMetric,
                (void**)&oRt2dDeviceSetMetric);
  else
    LOGW("widescreen: Rt2d metric not recognised, text stays stretched");
  if (patch::bytes_are(game::kAnim2DStart, game::kAnim2DStartHead, sizeof(game::kAnim2DStartHead)))
    patch::hook("CAnimation2D::Start (2D scenes in 4:3)", game::kAnim2DStart, (void*)&hkAnim2DStart,
                (void**)&oAnim2DStart);
  else
    LOGW("widescreen: 2D scene code not recognised, menus made of scenes stay stretched");
  if (cfg.hud_center && (cfg.gauges_corners || cfg.gauges_scale < 1.0f)) {
    bool ok = patch::bytes_are(game::kMeterRun, game::kMeterHead, sizeof(game::kMeterHead)) &&
              patch::bytes_are(game::kMeterDraw, game::kMeterHead, sizeof(game::kMeterHead)) &&
              patch::bytes_are(game::kGaugeStatusRun, game::kGaugeStatusHead, sizeof(game::kGaugeStatusHead)) &&
              patch::bytes_are(game::kGaugeStatusDraw, game::kGaugeStatusHead, sizeof(game::kGaugeStatusHead)) &&
              patch::read<uint32_t>(0x494E63) == 0x43870000 && patch::read<uint32_t>(0x494EFB) == 0xC2BA0000;
    if (ok && patch::hook("CGaugeMeter::Run (ring to the corner)", game::kMeterRun, (void*)&hkMeterRun,
                          (void**)&oMeterRun) &&
        patch::hook("CGaugeMeter::Draw (ring to the corner)", game::kMeterDraw, (void*)&hkMeterDraw,
                    (void**)&oMeterDraw) &&
        patch::hook("CGaugeStatus::Run (boxes to the corner)", game::kGaugeStatusRun, (void*)&hkStatusRun,
                    (void**)&oStatusRun))
      patch::hook("CGaugeStatus::Draw (boxes to the corner)", game::kGaugeStatusDraw, (void*)&hkStatusDraw,
                  (void**)&oStatusDraw);
    else if (!ok)
      LOGW("widescreen: gauge code not recognised, the stage's gauges stay in the 4:3 middle");
  }
  if (patch::bytes_are(game::kMovieDraw, game::kMovieDrawHead, sizeof(game::kMovieDrawHead)))
    patch::hook("CPCMovie::Draw (movies in 4:3)", game::kMovieDraw, (void*)&hkMovieDraw, (void**)&oMovieDraw);
  else
    LOGW("widescreen: movie code not recognised, movies stay stretched");
  LOGI("widescreen: Hor+%s%s", cfg.hud_center ? (cfg.gauges_corners ? ", 2D in the 4:3 middle, gauges in the corners"
                                                                     : ", 2D in the 4:3 middle")
                                                : ", 2D stretched",
       cfg.fov != 1.0f ? (", fov x" + std::to_string(cfg.fov)).c_str() : "");
}

}  // namespace widescreen
