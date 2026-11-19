#include "mod/overlay.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <mutex>

#include "common/log.h"

namespace overlay {
namespace {

std::mutex g_mu;
std::vector<Line> g_panel;
bool g_center = true;
std::wstring g_notice;
uint32_t g_notice_color = 0xFFFFD040;
ULONGLONG g_notice_until = 0;
unsigned g_version = 1;  // bumped on every change
bool g_last_by_game = false;

// The texture of what was drawn last.
IDirect3DTexture9* g_tex = nullptr;
int g_tex_w = 0, g_tex_h = 0;
unsigned g_tex_version = 0;
int g_tex_frame_h = 0;

struct Vertex {
  float x, y, z, rhw;
  float u, v;
};

// Renders the lines into an ARGB texture: a dark panel, antialiased text.
bool build(IDirect3DDevice9* dev, std::vector<Line> lines, int frame_h) {
  if (g_tex) {
    g_tex->Release();
    g_tex = nullptr;
  }
  if (lines.empty()) return false;
  int small_px = std::max(14, frame_h / 30), big_px = std::max(18, frame_h / 17);
  HDC dc = CreateCompatibleDC(nullptr);
  HFONT fonts[2] = {
      // Bold wide capitals, close to the game's own lettering (Bahnschrift is on Windows 10 and 11;
      // without it Windows picks its usual sans).
      CreateFontW(-small_px, 0, 0, 0, FW_BOLD, FALSE, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Bahnschrift"),
      CreateFontW(-big_px, 0, 0, 0, FW_BOLD, FALSE, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Bahnschrift")};
  int pad = small_px, gap = small_px / 3;
  for (Line& l : lines)  // capitals, as everything the game writes
    if (!l.text.empty()) CharUpperBuffW(&l.text[0], (DWORD)l.text.size());
  std::vector<SIZE> sizes(lines.size());
  int w = 0, h = pad;
  for (size_t i = 0; i < lines.size(); ++i) {
    SelectObject(dc, fonts[lines[i].big ? 1 : 0]);
    SIZE s{0, lines[i].big ? big_px : small_px};
    if (!lines[i].text.empty()) GetTextExtentPoint32W(dc, lines[i].text.c_str(), (int)lines[i].text.size(), &s);
    sizes[i] = s;
    w = std::max(w, (int)s.cx);
    h += s.cy + gap;
  }
  w += pad * 2;
  h += pad - gap;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool ok = false;
  if (bmp && bits) {
    HGDIOBJ old = SelectObject(dc, bmp);
    std::fill_n(static_cast<uint32_t*>(bits), (size_t)w * h, 0u);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    std::vector<int> line_of_row(h, -1);
    int y = pad;
    for (size_t i = 0; i < lines.size(); ++i) {
      SelectObject(dc, fonts[lines[i].big ? 1 : 0]);
      int x = (w - sizes[i].cx) / 2;
      TextOutW(dc, x, y, lines[i].text.c_str(), (int)lines[i].text.size());
      for (int r = y; r < y + sizes[i].cy && r < h; ++r) line_of_row[r] = (int)i;
      y += sizes[i].cy + gap;
    }
    GdiFlush();
    if (SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) {
      D3DLOCKED_RECT lr{};
      if (SUCCEEDED(g_tex->LockRect(0, &lr, nullptr, 0))) {
        for (int r = 0; r < h; ++r) {
          const uint32_t* src = static_cast<const uint32_t*>(bits) + (size_t)r * w;
          uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + (size_t)r * lr.Pitch);
          uint32_t c = line_of_row[r] >= 0 ? lines[line_of_row[r]].color : 0xFFFFFFFF;
          for (int x = 0; x < w; ++x) {
            uint32_t p = src[x];
            uint32_t a = std::max({p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF});  // text coverage
            // over a dark green panel (the colours of tmnt2net's pages), lime lines along its top and bottom
            const uint32_t br = 10 + 30 * x / w, bg = 22 + 44 * x / w, bb = 12 + 10 * x / w;
            uint32_t cr = (((c >> 16) & 0xFF) * a + br * (255 - a)) / 255, cg = (((c >> 8) & 0xFF) * a + bg * (255 - a)) / 255,
                     cb = ((c & 0xFF) * a + bb * (255 - a)) / 255;
            uint32_t alpha = 232 + (23 * a) / 255;
            if (r < 2 || r >= h - 2) {
              cr = 0x9E;
              cg = 0xDC;
              cb = 0x3C;
              alpha = 255;
            }
            dst[x] = alpha << 24 | cr << 16 | cg << 8 | cb;
          }
        }
        g_tex->UnlockRect(0);
        g_tex_w = w;
        g_tex_h = h;
        ok = true;
      }
    }
    SelectObject(dc, old);
  }
  if (bmp) DeleteObject(bmp);
  DeleteObject(fonts[0]);
  DeleteObject(fonts[1]);
  DeleteDC(dc);
  if (!ok && g_tex) {
    g_tex->Release();
    g_tex = nullptr;
  }
  return ok;
}

}  // namespace

void set_panel(const std::vector<Line>& lines, bool center) {
  std::lock_guard<std::mutex> lk(g_mu);
  bool same = center == g_center && lines.size() == g_panel.size();
  for (size_t i = 0; same && i < lines.size(); ++i)
    same = lines[i].text == g_panel[i].text && lines[i].color == g_panel[i].color && lines[i].big == g_panel[i].big;
  if (same) return;
  g_panel = lines;
  g_center = center;
  ++g_version;
}

void clear_panel() { set_panel({}, true); }

void notify(const std::wstring& text, double seconds, uint32_t color) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_notice = text;
  g_notice_color = color;
  g_notice_until = GetTickCount64() + (ULONGLONG)(seconds * 1000);
  ++g_version;
}

bool g_notice_by_game = false;  // the game's font showed the notice in this frame

bool peek_notice(std::wstring* text, uint32_t* color) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_notice.empty() || GetTickCount64() >= g_notice_until) return false;
  *text = g_notice;
  *color = g_notice_color;
  return true;
}

void notice_drawn() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_notice_by_game = true;
}

void fill_in_scene(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, uint32_t argb) {
  ColorVertex v[6] = {{x0 - 0.5f, y0 - 0.5f, 0, 1, argb}, {x1 - 0.5f, y0 - 0.5f, 0, 1, argb},
                      {x0 - 0.5f, y1 - 0.5f, 0, 1, argb}, {x1 - 0.5f, y0 - 0.5f, 0, 1, argb},
                      {x1 - 0.5f, y1 - 0.5f, 0, 1, argb}, {x0 - 0.5f, y1 - 0.5f, 0, 1, argb}};
  tris_in_scene(dev, v, 6);
}

void tris_in_scene(IDirect3DDevice9* dev, const ColorVertex* v, int vertices) {
  if (!dev || !v || vertices < 3) return;
  IDirect3DStateBlock9* sb = nullptr;
  if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return;
  sb->Capture();
  dev->SetVertexShader(nullptr);
  dev->SetPixelShader(nullptr);
  dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  dev->SetTexture(0, nullptr);
  dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
  dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, vertices / 3, v, sizeof(ColorVertex));
  sb->Apply();
  sb->Release();
}

void draw(IDirect3DDevice9* dev, int frame_w, int frame_h) {
  if (!dev || frame_w <= 0 || frame_h <= 0) return;
  std::vector<Line> lines;
  bool center = true;
  unsigned version;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_notice.empty() && GetTickCount64() >= g_notice_until) {
      g_notice.clear();
      ++g_version;
    }
    bool by_game = g_notice_by_game;
    g_notice_by_game = false;  // the next frame decides again
    if (!g_notice.empty() && !by_game) lines.push_back({g_notice, g_notice_color, false});
    if (by_game != g_last_by_game) {
      g_last_by_game = by_game;
      ++g_version;
    }
    lines.insert(lines.end(), g_panel.begin(), g_panel.end());
    center = g_center && !g_panel.empty();
    version = g_version;
  }
  if (lines.empty()) {
    if (g_tex) release();
    return;
  }
  if (version != g_tex_version || frame_h != g_tex_frame_h || !g_tex) {
    g_tex_version = version;
    g_tex_frame_h = frame_h;
    if (!build(dev, lines, frame_h)) return;
  }
  float w = (float)std::min(g_tex_w, frame_w), h = (float)g_tex_h;
  float x = std::floor((frame_w - w) / 2) - 0.5f;
  float y = (center ? std::floor((frame_h - h) / 2) : std::floor(frame_h / 20.0f)) - 0.5f;
  float u1 = w / g_tex_w;
  Vertex v[4] = {{x, y, 0, 1, 0, 0}, {x + w, y, 0, 1, u1, 0}, {x, y + h, 0, 1, 0, 1}, {x + w, y + h, 0, 1, u1, 1}};

  IDirect3DStateBlock9* sb = nullptr;
  if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return;
  sb->Capture();
  // The back buffer, whatever target the game left set (not part of a state block).
  IDirect3DSurface9 *old_rt = nullptr, *bb = nullptr;
  dev->GetRenderTarget(0, &old_rt);
  if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) dev->SetRenderTarget(0, bb);
  if (SUCCEEDED(dev->BeginScene())) {
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetTexture(0, g_tex);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    D3DVIEWPORT9 vp{0, 0, (DWORD)frame_w, (DWORD)frame_h, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
    dev->EndScene();
  }
  if (old_rt) {
    dev->SetRenderTarget(0, old_rt);
    old_rt->Release();
  }
  if (bb) bb->Release();
  sb->Apply();
  sb->Release();
}

void release() {
  if (g_tex) {
    g_tex->Release();
    g_tex = nullptr;
  }
  g_tex_version = 0;
}

}  // namespace overlay
