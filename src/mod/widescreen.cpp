#include "mod/widescreen.h"

#include <windows.h>

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

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

// --- the area map on to the frame's edges ---------------------------------------------
// The map's picture is such a scene: 4:3, black bars beside it on a wider frame - and its clouds, which
// the scene does not cut at its own edges, drifting over the bars. The picture goes on into each bar as
// its own reflection at that 4:3 edge: the scene drawn again there, turned over about a line a little
// inside the edge (the pictures' outermost texels are lighter than the rest: a line at the very edge
// would show as a seam), each draw cut to its own part of the frame. What goes out of the picture by
// itself - the clouds - is drawn in the bars as it is, between the layers it lies between, so a cloud
// crosses the edge and floats on. The marks, the names, the turtle and the map's menu are drawn after
// the scene, by other code: once, in the middle.
using Method_t = void(__fastcall*)(uint8_t* self, void* edx);
Method_t oAreaSeqDraw = nullptr, oAnim2DDraw = nullptr;
using MaestroRender_t = void*(__cdecl*)(void* maestro);
using ObjectMTMScale_t = void*(__cdecl*)(void* object, float x, float y);
using SceneForAll_t = void*(__cdecl*)(void* scene, void*(__cdecl* cb)(void*, void*, void*), void* data);
bool g_map_draw = false;

void __fastcall hkAreaSeqDraw(uint8_t* self, void* edx) {
  g_map_draw = true;
  oAreaSeqDraw(self, edx);
  g_map_draw = false;
}

// RwRenderStateSet / Get: the engine's own (RwEngineInstance +0x20 / +0x24, both __cdecl(state, value)).
constexpr int kRwCullMode = 20, kRwCullNone = 1;
bool render_state(int state, uint32_t* old, uint32_t value) {
  uint8_t* eng = patch::read<uint8_t*>(game::kEngineInstance);
  if (!eng) return false;
  auto set = patch::read<int(__cdecl*)(int, void*)>(reinterpret_cast<uintptr_t>(eng) + 0x20);
  auto get = patch::read<int(__cdecl*)(int, void*)>(reinterpret_cast<uintptr_t>(eng) + 0x24);
  if (!set || !get) return false;
  if (old) get(state, old);
  set(state, reinterpret_cast<void*>(static_cast<uintptr_t>(value)));
  return true;
}

uint32_t& object_flags(uint8_t* object) { return *reinterpret_cast<uint32_t*>(object + game::kRt2dObjectFlags); }

void* __cdecl collect_child(void* object, void*, void* data) {
  static_cast<std::vector<uint8_t*>*>(data)->push_back(static_cast<uint8_t*>(object));
  return object;
}

void children(uint8_t* scene, std::vector<uint8_t*>& out) {
  reinterpret_cast<SceneForAll_t>(game::kRt2dSceneForAllChildren)(scene, collect_child, &out);
}

// The six maps, each known by its layers (type/depth, the lowest first), and what its layers are. The
// ground of the picture is reflected in the bars. Things of their own over it are not - a reflection
// would give a second moon beside the first, and clouds meeting themselves at the edge:
// - `own`: drawn in the bars as they are: clouds that drift out of the picture and float on, a moon;
// - `none`: not drawn in the bars: a planet that ends where the picture ends (the two space maps hide
//   what is beyond the edge behind a black block of the same layer: drawn as it is, that block shows);
// - `whole`: a planet the picture's edge cuts a piece off is made whole in that bar by its own
//   reflection about its middle line, which follows the planet (it sways).
// The fog and the rain of the two cities run across the picture and end at its edges: reflected with
// the ground. (Each layer was looked at hidden in turn; the notes are in docs/TZ.md.)
struct MapWhole {
  int depth;   // the layer (0: there is none)
  int side;    // 0 the left bar, 1 the right one
  int part;    // which of the layer's parts the planet is
  float line;  // the middle line, in the picture's units right of that part's own place
  float reach; // how far from the line the reflection is drawn (0: to the frame's edge)
  float tint[3];  // the reflection's colour, times this: the side the picture cuts is the planet's
                  // shaded one, the side reflected its lit one
  float top;   // the picture's row (0 the top .. 480) the reflection starts at: above it the layer's
               // other parts would show a second time
};
struct MapScene {
  const char* name;
  const char* layers;
  int own[4];   // depths, 0 ends
  int none[4];
  MapWhole whole;
};
const MapScene kMapScenes[] = {
    // 2 clouds; 24, 26 fog
    {"New York", "2/1 1/2 2/7 1/8 1/19 1/24 1/26 2/28 4/29", {2}, {}, {}},
    // 2 a small moon, 4 the red planet and its ring, 6 the grey moon
    {"D'Hoonnib", "2/1 1/2 1/4 1/6 2/8 4/9", {}, {2, 4, 6}, {4, 1, 0, 365.9f, 0.0f, {0.58f, 0.19f, 0.07f}, 0.0f}},
    // 2 the red planet and two moons, 6 the ship
    {"the Triceraton ship", "2/1 1/2 1/6 2/10 4/11", {6}, {2}, {2, 0, 0, -37.5f, 98.0f, {1.0f, 1.0f, 1.0f}, 255.0f}},
    // 2 the sun's glare, 3 fires and water, 14 trees
    {"Japan", "2/1 2/2 1/3 2/14 2/15 4/16", {}, {}, {}},
    // 2 clouds; 17, 19 fog; 21 rain
    {"the unknown city", "2/1 1/2 2/15 2/16 1/17 1/19 1/21 2/34 4/35", {2}, {}, {}},
    // 2 the moon, 3 clouds
    {"the freighter", "2/1 2/2 1/3 2/8 1/9 1/13 1/15 2/26 4/27", {2, 3}, {}, {}},
};

// The scene once more with only the layers [from, to) of `layers` shown; turned over about the vertical
// line x = about (in the scene's own units) when `turned`.
void draw_layers(void* maestro, uint8_t* scene, const std::vector<uint8_t*>& layers, size_t from, size_t to,
                 bool turned, float about, const float* tint = nullptr) {
  std::vector<uint32_t> flags(layers.size());
  for (size_t i = 0; i < layers.size(); ++i) {
    flags[i] = object_flags(layers[i]);
    if (i < from || i >= to) object_flags(layers[i]) &= ~game::kRt2dObjectVisible;
  }
  uint8_t keep[64];
  std::memcpy(keep, scene + game::kRt2dObjectMTM, sizeof(keep));
  uint32_t cull = 0;
  bool culled = false;
  if (turned) {
    // (both go before the scene's own matrix, the last one first: x -> -x -> 2 * about - x)
    reinterpret_cast<ObjectMTMTranslate_t>(game::kRt2dObjectMTMTranslate)(scene, 2.0f * about, 0.0f);
    reinterpret_cast<ObjectMTMScale_t>(game::kRt2dObjectMTMScale)(scene, -1.0f, 1.0f);
    // (turned over, its triangles go the other way round: none of them may be culled)
    culled = render_state(kRwCullMode, &cull, kRwCullNone);
  }
  float colour[4];
  auto* mult = reinterpret_cast<float*>(scene + game::kRt2dObjectColorMult);
  std::memcpy(colour, mult, sizeof(colour));
  for (int i = 0; tint && i < 3; ++i) mult[i] *= tint[i];
  object_flags(scene) |= game::kRt2dObjectDirtyLTM;
  reinterpret_cast<MaestroRender_t>(game::kRt2dMaestroRender)(maestro);
  std::memcpy(mult, colour, sizeof(colour));
  if (culled) render_state(kRwCullMode, nullptr, cull);
  std::memcpy(scene + game::kRt2dObjectMTM, keep, sizeof(keep));
  object_flags(scene) |= game::kRt2dObjectDirtyLTM;
  // (what a draw itself changes in a layer's flags stays; only "shown" is put back)
  for (size_t i = 0; i < layers.size(); ++i)
    if (flags[i] & game::kRt2dObjectVisible) object_flags(layers[i]) |= game::kRt2dObjectVisible;
}

bool draw_wide_map(uint8_t* self, void* edx, void* maestro, int w, int h, int w43) {
  IDirect3DDevice9* dev = video::device();
  auto bbox = reinterpret_cast<MaestroGetBBox_t>(game::kRt2dMaestroGetBBox)(maestro);
  auto* scene = static_cast<uint8_t*>(reinterpret_cast<MaestroGetScene_t>(game::kRt2dMaestroGetScene)(maestro));
  if (!dev || !bbox || !scene || bbox->w <= 0) return false;
  std::vector<uint8_t*> layers;
  children(scene, layers);
  if (layers.empty()) return false;
  std::stable_sort(layers.begin(), layers.end(), [](const uint8_t* a, const uint8_t* b) {
    return *reinterpret_cast<const int*>(a + game::kRt2dObjectDepth) < *reinterpret_cast<const int*>(b + game::kRt2dObjectDepth);
  });
  // Which map this is.
  std::string made;
  for (const uint8_t* l : layers)
    made += (made.empty() ? "" : " ") + std::to_string(*reinterpret_cast<const int*>(l)) + "/" +
            std::to_string(*reinterpret_cast<const int*>(l + game::kRt2dObjectDepth));
  const MapScene* map = nullptr;
  for (const MapScene& m : kMapScenes)
    if (made == m.layers) map = &m;
  // 0 the layer's reflection in the bars, 1 the layer itself, 2 nothing (a text would read backwards).
  std::vector<int> kind(layers.size());
  for (size_t i = 0; i < layers.size(); ++i) {
    int depth = *reinterpret_cast<const int*>(layers[i] + game::kRt2dObjectDepth);
    kind[i] = *reinterpret_cast<const int*>(layers[i]) == game::kRt2dObjectString ? 2 : 0;
    for (int k = 0; map && k < 4; ++k) {
      if (map->own[k] && map->own[k] == depth) kind[i] = 1;
      if (map->none[k] && map->none[k] == depth) kind[i] = 2;
    }
  }
  // The planet to be made whole: its middle line now (the places are those of the last draw).
  size_t whole = layers.size();
  float whole_line = 0.0f;
  const float* scene_ltm = reinterpret_cast<const float*>(scene + game::kRt2dObjectLTM);
  if (map && map->whole.depth && std::fabs(scene_ltm[0]) > 0.001f)
    for (size_t i = 0; i < layers.size(); ++i) {
      if (*reinterpret_cast<const int*>(layers[i] + game::kRt2dObjectDepth) != map->whole.depth) continue;
      std::vector<uint8_t*> parts;
      if (*reinterpret_cast<const int*>(layers[i]) == game::kRt2dObjectScene) children(layers[i], parts);
      if (map->whole.part >= (int)parts.size()) break;
      float x = (reinterpret_cast<const float*>(parts[map->whole.part] + game::kRt2dObjectLTM)[12] - scene_ltm[12]) / scene_ltm[0];
      if (!(x > -bbox->w && x < 2.0f * bbox->w)) break;  // (not placed yet: the scene's first draw)
      whole = i;
      whole_line = x + map->whole.line;
    }

  int x0 = (w - w43) / 2;
  int inset = static_cast<int>(std::ceil(3.0f * w43 / bbox->w));  // 3 units of the picture, in whole pixels
  float about = inset * bbox->w / float(w43);
  RECT old_rect{};
  DWORD old_on = FALSE;
  dev->GetScissorRect(&old_rect);
  dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &old_on);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
  for (int side = 0; side < 2; ++side) {
    RECT bar = side == 0 ? RECT{0, 0, x0 + inset, h} : RECT{x0 + w43 - inset, 0, w, h};
    dev->SetScissorRect(&bar);
    for (size_t i = 0; i < layers.size();) {
      if (i == whole && side == map->whole.side) {
        RECT part = bar;
        part.top = static_cast<LONG>(map->whole.top * h / bbox->h);
        if (map->whole.reach > 0) {
          float end = whole_line + (side == 0 ? -map->whole.reach : map->whole.reach);
          LONG end_px = x0 + static_cast<LONG>(end * w43 / bbox->w);
          if (side == 0) part.left = std::max<LONG>(part.left, end_px);
          else part.right = std::min<LONG>(part.right, end_px);
          if (part.left >= part.right) part.right = part.left;  // (nothing of it in this bar)
        }
        dev->SetScissorRect(&part);
        draw_layers(maestro, scene, layers, i, i + 1, true, whole_line, map->whole.tint);
        dev->SetScissorRect(&bar);
      }
      size_t j = i;
      while (j < layers.size() && kind[j] == kind[i] && (j == i || j != whole)) ++j;
      if (kind[i] != 2) draw_layers(maestro, scene, layers, i, j, kind[i] == 0, side == 0 ? about : bbox->w - about);
      i = j;
    }
  }
  RECT middle{x0 + inset, 0, x0 + w43 - inset, h};
  dev->SetScissorRect(&middle);
  oAnim2DDraw(self, edx);  // (the last draw: every layer's place is left as the game set it)
  dev->SetScissorRect(&old_rect);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, old_on);
  static std::string logged;
  if (logged != made) {
    logged = made;
    if (map)
      LOGI("widescreen: the area map of %s goes on to the edges of the %dx%d frame", map->name, w, h);
    else
      LOGW("widescreen: an area map tmnt2net does not know (layers %s): all of it is reflected in the bars", made.c_str());
  }
  return true;
}

void __fastcall hkAnim2DDraw(uint8_t* self, void* edx) {
  int w, h, w43;
  void* maestro = self ? patch::read<void*>(reinterpret_cast<uintptr_t>(self) + game::kAnim2DMaestro) : nullptr;
  if (g_map_draw && g_cfg.hud_center && g_cfg.map_wide && oAnim2DStart && maestro && wider_than_43(w, h, w43) &&
      draw_wide_map(self, edx, maestro, w, h, w43))
    return;
  oAnim2DDraw(self, edx);
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
  if (cfg.hud_center && cfg.map_wide && oAnim2DStart) {
    if (patch::bytes_are(game::kAreaSeqDraw, game::kAreaSeqDrawHead, sizeof(game::kAreaSeqDrawHead)) &&
        patch::bytes_are(game::kAnim2DDraw, game::kAnim2DDrawHead, sizeof(game::kAnim2DDrawHead)) &&
        patch::hook("CAreaSequence::OnDraw (the area map)", game::kAreaSeqDraw, (void*)&hkAreaSeqDraw,
                    (void**)&oAreaSeqDraw))
      patch::hook("CAnimation2D::Draw (the area map on to the frame's edges)", game::kAnim2DDraw, (void*)&hkAnim2DDraw,
                  (void**)&oAnim2DDraw);
    else
      LOGW("widescreen: area map code not recognised, the map stays 4:3 between bars");
  }
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
