// tmnt2net post-processing of the finished frame (src/mod/postfx.cpp), Direct3D 9 ps_3_0.
// Built into src/mod/postfx_shader.h by tools/gen_postfx.py (D3DCompile of d3dcompiler_47.dll),
// so the game needs no compiler DLL.
//
// c0: x = 1 / frame width, y = 1 / frame height, z = sharpness 0..1, w = colour 0..1.
sampler2D frame : register(s0);
float4 p : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float3 c = tex2D(frame, uv).rgb;
  float3 n = tex2D(frame, uv + float2(0, -p.y)).rgb;
  float3 s = tex2D(frame, uv + float2(0, p.y)).rgb;
  float3 w = tex2D(frame, uv + float2(-p.x, 0)).rgb;
  float3 e = tex2D(frame, uv + float2(p.x, 0)).rgb;

  // Contrast-adaptive sharpening (after AMD's CAS): flat and already hard edges get little, soft
  // detail gets the most, so textures come out crisper without bright halos.
  float3 lo = min(c, min(min(n, s), min(w, e)));
  float3 hi = max(c, max(max(n, s), max(w, e)));
  float3 amp = sqrt(saturate(min(lo, 1.0 - hi) / max(hi, 0.0001)));
  float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, p.z)) * step(0.001, p.z);
  float3 o = (c + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt);

  // Colour: pale colours gain more than the ones already strong (vibrance); the brightness stays,
  // with a touch more contrast around the middle.
  float luma = dot(o, float3(0.2126, 0.7152, 0.0722));
  float most = max(o.r, max(o.g, o.b)), least = min(o.r, min(o.g, o.b));
  o = lerp(float3(luma, luma, luma), o, 1.0 + 1.2 * p.w * (1.0 - (most - least)));
  o = saturate((o - 0.45) * (1.0 + 0.12 * p.w) + 0.45);
  return float4(o, 1.0);
}
