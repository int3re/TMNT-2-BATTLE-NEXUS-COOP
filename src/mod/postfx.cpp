#include "mod/postfx.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>

#include "common/log.h"
#include "mod/postfx_shader.h"

namespace postfx {
namespace {

std::atomic<int> g_sharpen{0}, g_colors{0};  // 0..100
bool g_failed = false;                       // this device cannot do it: off for the run
bool g_split = false;                        // [debug] the right half only

IDirect3DPixelShader9* g_ps = nullptr;
IDirect3DTexture9* g_copy = nullptr;  // the frame, not multisampled
UINT g_w = 0, g_h = 0;
D3DFORMAT g_fmt = D3DFMT_UNKNOWN;

struct Vertex {
  float x, y, z, rhw;
  float u, v;
};

void give_up(const char* why) {
  g_failed = true;
  release();
  LOGW("postfx: off, %s", why);
}

bool ready(IDirect3DDevice9* dev, const D3DSURFACE_DESC& bb) {
  if (!g_ps) {
    D3DCAPS9 caps{};
    if (FAILED(dev->GetDeviceCaps(&caps)) || caps.PixelShaderVersion < D3DPS_VERSION(3, 0)) {
      give_up("the video card has no pixel shaders 3.0");
      return false;
    }
    if (FAILED(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(kShader), &g_ps))) {
      give_up("the pixel shader was refused");
      return false;
    }
  }
  if (g_copy && (g_w != bb.Width || g_h != bb.Height || g_fmt != bb.Format)) {
    g_copy->Release();
    g_copy = nullptr;
  }
  if (!g_copy) {
    if (FAILED(dev->CreateTexture(bb.Width, bb.Height, 1, D3DUSAGE_RENDERTARGET, bb.Format, D3DPOOL_DEFAULT, &g_copy,
                                  nullptr))) {
      give_up("no texture for the frame's copy");
      return false;
    }
    g_w = bb.Width;
    g_h = bb.Height;
    g_fmt = bb.Format;
    LOGI("postfx: on for %ux%u frames (sharpness %d, colour %d)", g_w, g_h, g_sharpen.load(), g_colors.load());
  }
  return true;
}

}  // namespace

void install(const Config& cfg) {
  g_split = cfg.postfx_split;
  set(cfg.sharpen, cfg.colors);
}

void set(int sharpen, int colors) {
  g_sharpen = std::max(0, std::min(100, sharpen));
  g_colors = std::max(0, std::min(100, colors));
}

void release() {
  if (g_copy) {
    g_copy->Release();
    g_copy = nullptr;
  }
  if (g_ps) {
    g_ps->Release();
    g_ps = nullptr;
  }
  g_w = g_h = 0;
}

void apply(IDirect3DDevice9* dev) {
  int sharpen = g_sharpen.load(), colors = g_colors.load();
  if (!dev || g_failed || (sharpen <= 0 && colors <= 0)) return;
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  D3DSURFACE_DESC d{};
  bb->GetDesc(&d);
  IDirect3DSurface9* copy = nullptr;
  if ((d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) || !ready(dev, d) ||
      FAILED(g_copy->GetSurfaceLevel(0, &copy))) {
    bb->Release();
    return;
  }
  // The frame as it is (a multisampled one resolved) into the texture, then back through the shader.
  IDirect3DStateBlock9* sb = nullptr;
  if (SUCCEEDED(dev->StretchRect(bb, nullptr, copy, nullptr, D3DTEXF_NONE)) &&
      SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) {
    sb->Capture();
    IDirect3DSurface9* old_rt = nullptr;
    dev->GetRenderTarget(0, &old_rt);
    dev->SetRenderTarget(0, bb);
    if (SUCCEEDED(dev->BeginScene())) {
      float w = (float)d.Width, h = (float)d.Height;
      float x0 = g_split ? (float)(d.Width / 2) : 0.0f, u0 = x0 / w;
      Vertex v[4] = {{x0 - 0.5f, -0.5f, 0, 1, u0, 0}, {w - 0.5f, -0.5f, 0, 1, 1, 0},
                     {x0 - 0.5f, h - 0.5f, 0, 1, u0, 1}, {w - 0.5f, h - 0.5f, 0, 1, 1, 1}};
      float c0[4] = {1.0f / w, 1.0f / h, sharpen / 100.0f, colors / 100.0f};
      dev->SetVertexShader(nullptr);
      dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
      dev->SetPixelShader(g_ps);
      dev->SetPixelShaderConstantF(0, c0, 1);
      dev->SetTexture(0, g_copy);
      dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
      dev->SetRenderState(D3DRS_ZENABLE, FALSE);
      dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
      dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
      dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
      dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
      dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
      dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
      D3DVIEWPORT9 vp{0, 0, d.Width, d.Height, 0.0f, 1.0f};
      dev->SetViewport(&vp);
      dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
      dev->EndScene();
    }
    if (old_rt) {
      dev->SetRenderTarget(0, old_rt);
      old_rt->Release();
    }
    sb->Apply();
    sb->Release();
  }
  copy->Release();
  bb->Release();
}

}  // namespace postfx
