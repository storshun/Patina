// Composites one layer onto the accumulation textures (in place; the caller
// issues glTextureBarrier between layers). Variants: LAYER_PAINT / LAYER_FILL.
#include "common.glsl"
#ifdef LAYER_FILL
#include "material.glsl"
#endif

in vec2 vUV;

layout(location = 0) out vec4 oColor;    // sRGB rgb, a = coverage
layout(location = 1) out vec4 oRMH;      // rough, metal, height, unused
layout(location = 2) out vec4 oNormal;   // tangent normal xyz (signed), unused
layout(location = 3) out vec4 oEmissive; // sRGB rgb
layout(location = 4) out vec4 oExtra;    // opacity, sheen, clearcoat, subsurface (0.26)

uniform sampler2D uAccColor;
uniform sampler2D uAccRMH;
uniform sampler2D uAccNormal;
uniform sampler2D uAccEmissive;
uniform sampler2D uAccExtra;

uniform sampler2D uLayerMask;   // r; white texture when the layer has no mask
uniform float uLayerOpacity;
uniform int uBlendMode;         // see Layer.h BlendMode
uniform int uChannelMask;       // bit per channel: 1 color 2 rough 4 metal 8 height 16 normal 32 emissive,
                                // 64 opacity 128 sheen 256 clearcoat 512 subsurface
uniform vec4 uChanOpacity0;     // color, rough, metal, height
uniform vec2 uChanOpacity1;     // normal, emissive
uniform vec4 uChanOpacity2 = vec4(1.0);  // opacity, sheen, clearcoat, subsurface
uniform vec4 uTileUV;           // mesh UV of this texture: offset xy, size zw (tiled exports; else 0,0,1,1)

#include "maskgen.glsl"

#ifdef LAYER_ADJUST
uniform vec4 uAdj0;  // hue, saturation, brightness, contrast
uniform vec3 uAdj1;  // roughness, metallic, height offsets
uniform vec4 uAdj2;  // (0.33) exposure (stops), temperature, tint, vibrance
uniform vec4 uAdj3;  // (0.33) highlights, shadows, de-light
vec3 rgb2hsv(vec3 c) {
  vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
  vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
  vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
  float d = q.x - min(q.w, q.y);
  return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)), d / (q.x + 1e-10), q.x);
}
vec3 hsv2rgb(vec3 c) {
  vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
  return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}
#endif

#ifdef LAYER_FOLDER
uniform sampler2D uBeforeColor;   // accumulator before the folder's children
uniform sampler2D uBeforeRMH;
uniform sampler2D uBeforeNormal;
uniform sampler2D uBeforeEmissive;
uniform sampler2D uBeforeExtra;
#endif

#ifdef LAYER_PAINT
uniform sampler2D uLColor;
uniform sampler2D uLRough;
uniform sampler2D uLMetal;
uniform sampler2D uLHeight;
uniform sampler2D uLNormal;
uniform sampler2D uLEmissive;
uniform sampler2D uLOpacity;
uniform sampler2D uLSheen;
uniform sampler2D uLCoat;
uniform sampler2D uLSubsurface;
#endif

float blend1(int mode, float b, float l) {
  if (mode == 1) return b * l;                                   // multiply
  if (mode == 2) return 1.0 - (1.0 - b) * (1.0 - l);             // screen
  if (mode == 3) return b < 0.5 ? 2.0 * b * l : 1.0 - 2.0 * (1.0 - b) * (1.0 - l);  // overlay
  if (mode == 4) return b + l;                                   // add
  if (mode == 5) return b - l;                                   // subtract
  if (mode == 6) return min(b, l);                               // darken
  if (mode == 7) return max(b, l);                               // lighten
  if (mode == 8) return b + (l - 0.5);                           // add signed (linear light-ish)
  if (mode == 9) return l < 0.5 ? b * (2.0 * l) : 1.0 - (1.0 - b) * (2.0 - 2.0 * l);  // soft-ish
  return l;                                                      // normal / replace
}
vec3 blend3(int mode, vec3 b, vec3 l) {
  return vec3(blend1(mode, b.r, l.r), blend1(mode, b.g, l.g), blend1(mode, b.b, l.b));
}

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 aC = texelFetch(uAccColor, p, 0);
  vec4 aR = texelFetch(uAccRMH, p, 0);
  vec4 aN = texelFetch(uAccNormal, p, 0);
  vec4 aE = texelFetch(uAccEmissive, p, 0);
  vec4 aX = texelFetch(uAccExtra, p, 0);
  oColor = aC; oRMH = aR; oNormal = aN; oEmissive = aE; oExtra = aX;
  bvec4 xOn = bvec4((uChannelMask & 64) != 0, (uChannelMask & 128) != 0, (uChannelMask & 256) != 0, (uChannelMask & 512) != 0);
#ifdef LAYER_FOLDER
  {
    // children were composited onto the accumulator; blend that back over "before"
    vec4 bC = texelFetch(uBeforeColor, p, 0), bR = texelFetch(uBeforeRMH, p, 0);
    vec4 bN = texelFetch(uBeforeNormal, p, 0), bE = texelFetch(uBeforeEmissive, p, 0);
    float kf = clamp(uLayerOpacity * texture(uLayerMask, vUV).r * generatorMask(p), 0.0, 1.0);
    vec4 k0 = kf * uChanOpacity0;
    vec2 k1 = kf * uChanOpacity1;
    oColor = vec4(mix(bC.rgb, aC.rgb, (uChannelMask & 1) != 0 ? k0.x : 0.0), aC.a);
    oRMH = vec4(mix(bR.r, aR.r, (uChannelMask & 2) != 0 ? k0.y : 0.0),
                mix(bR.g, aR.g, (uChannelMask & 4) != 0 ? k0.z : 0.0),
                mix(bR.b, aR.b, (uChannelMask & 8) != 0 ? k0.w : 0.0), aR.a);
    vec3 nb = normalize(bN.xyz), na = normalize(aN.xyz);
    oNormal = vec4(normalize(mix(nb, na, (uChannelMask & 16) != 0 ? k1.x : 0.0)), aN.a);
    oEmissive = vec4(mix(bE.rgb, aE.rgb, (uChannelMask & 32) != 0 ? k1.y : 0.0), aE.a);
    vec4 bX = texelFetch(uBeforeExtra, p, 0);
    oExtra = mix(bX, aX, vec4(xOn) * kf * uChanOpacity2);
    return;
  }
#endif

  float k = uLayerOpacity * texture(uLayerMask, vUV).r * generatorMask(p);
  if (k <= 0.0) return;

  vec3 lc; float lr, lm, lh; vec3 ln; vec3 le;
  vec4 lx;              // opacity, sheen, clearcoat, subsurface
  vec4 alpha0, alpha1;  // per-channel coverage
  vec4 alpha2;          // coverage of opacity, sheen, clearcoat, subsurface
#ifdef LAYER_PAINT
  vec4 c = texelFetch(uLColor, p, 0);
  vec2 r = texelFetch(uLRough, p, 0).rg;
  vec2 m = texelFetch(uLMetal, p, 0).rg;
  vec2 h = texelFetch(uLHeight, p, 0).rg;
  vec4 n = texelFetch(uLNormal, p, 0);
  vec4 e = texelFetch(uLEmissive, p, 0);
  lc = c.rgb; lr = r.x; lm = m.x; lh = h.x; ln = normalize(decodeNormal(n.rgb)); le = e.rgb;
  alpha0 = vec4(c.a, r.y, m.y, h.y);
  alpha1 = vec4(n.a, e.a, 0, 0);
  vec2 xo = texelFetch(uLOpacity, p, 0).rg, xs = texelFetch(uLSheen, p, 0).rg;
  vec2 xc = texelFetch(uLCoat, p, 0).rg, xu = texelFetch(uLSubsurface, p, 0).rg;
  lx = vec4(xo.x, xs.x, xc.x, xu.x);
  alpha2 = vec4(xo.y, xs.y, xc.y, xu.y);
#elif defined(LAYER_ADJUST)
  // develop (0.33), in linear light: de-light, white balance, exposure, then highlights and shadows
  vec3 dc = aC.rgb;
  if (uAdj2 != vec4(0.0) || uAdj3 != vec4(0.0)) {
    vec3 lin = srgbToLinear(dc);
    if (uAdj3.z > 0.0) lin /= mix(1.0, max(texelFetch(uAOMap, p, 0).r, 0.25), uAdj3.z);
    vec3 wb = vec3(1.0 + 0.3 * uAdj2.y, 1.0 - 0.25 * uAdj2.z, 1.0 - 0.3 * uAdj2.y);
    lin *= wb / dot(wb, vec3(0.2126, 0.7152, 0.0722));
    lin *= exp2(uAdj2.x);
    float lp = luminance(linearToSrgb(clamp(lin, 0.0, 1.0)));
    float ws = 1.0 - smoothstep(0.0, 0.6, lp), wh = smoothstep(0.4, 1.0, lp);
    lin *= exp2(uAdj3.y * ws + uAdj3.x * wh);
    dc = linearToSrgb(clamp(lin, 0.0, 1.0));
    if (uAdj2.w != 0.0) {
      vec3 v = rgb2hsv(dc);
      v.y = clamp(v.y * (1.0 + uAdj2.w * (1.0 - v.y)), 0.0, 1.0);
      dc = hsv2rgb(v);
    }
  }
  vec3 hsv = rgb2hsv(dc);
  hsv.x = fract(hsv.x + uAdj0.x);
  hsv.y = clamp(hsv.y * uAdj0.y, 0.0, 1.0);
  vec3 adj = hsv2rgb(hsv) * uAdj0.z;
  adj = (adj - 0.5) * uAdj0.w + 0.5;
  lc = clamp(adj, 0.0, 1.0);
  lr = clamp(aR.r + uAdj1.x, 0.0, 1.0);
  lm = clamp(aR.g + uAdj1.y, 0.0, 1.0);
  lh = clamp(aR.b + uAdj1.z, 0.0, 1.0);
  ln = normalize(aN.xyz);
  le = aE.rgb;
  lx = aX;
  alpha0 = vec4(1.0);
  alpha1 = vec4(1.0);
  alpha2 = vec4(0.0);
#elif defined(LAYER_FILL)
  vec4 P = texelFetch(uPosMap, p, 0);
  if (P.w < 0.5) return;
  MatSample s = sampleMaterial(P.xyz, normalize(texelFetch(uNrmMap, p, 0).xyz), texelFetch(uTanMap, p, 0),
                               uTileUV.xy + vUV * uTileUV.zw);
  lc = s.color; lr = s.rough; lm = s.metal; lh = s.height; ln = s.normal; le = s.emissive; lx = s.extra;
  alpha0 = vec4(1.0);
  alpha1 = vec4(1.0);
  alpha2 = vec4(1.0);
#else
  lc = aC.rgb; lr = aR.r; lm = aR.g; lh = aR.b; ln = vec3(0, 0, 1); le = aE.rgb; lx = aX;
  alpha0 = vec4(0.0);
  alpha1 = vec4(0.0);
  alpha2 = vec4(0.0);
#endif
  alpha0 *= k * uChanOpacity0;
  alpha1.xy *= k * uChanOpacity1;
  alpha2 *= k * uChanOpacity2;
  for (int i = 0; i < 4; ++i)
    if (xOn[i] && alpha2[i] > 0.0) oExtra[i] = mix(aX[i], clamp(blend1(uBlendMode, aX[i], lx[i]), 0.0, 1.0), alpha2[i]);

  if ((uChannelMask & 1) != 0 && alpha0.x > 0.0) {
    vec3 v = clamp(blend3(uBlendMode, aC.rgb, lc), 0.0, 1.0);
    oColor.rgb = mix(aC.rgb, v, alpha0.x);
  }
  if ((uChannelMask & 2) != 0 && alpha0.y > 0.0)
    oRMH.r = mix(aR.r, clamp(blend1(uBlendMode, aR.r, lr), 0.0, 1.0), alpha0.y);
  if ((uChannelMask & 4) != 0 && alpha0.z > 0.0)
    oRMH.g = mix(aR.g, clamp(blend1(uBlendMode, aR.g, lm), 0.0, 1.0), alpha0.z);
  if ((uChannelMask & 8) != 0 && alpha0.w > 0.0) {
    // height: normal mode replaces; other modes treat 0.5 as neutral offset
    float hv = uBlendMode == 0 ? lh : clamp(aR.b + (lh - 0.5), 0.0, 1.0);
    if (uBlendMode == 7) hv = max(aR.b, lh);
    if (uBlendMode == 6) hv = min(aR.b, lh);
    oRMH.b = mix(aR.b, hv, alpha0.w);
  }
  if ((uChannelMask & 16) != 0 && alpha1.x > 0.0) {
    vec3 base = normalize(aN.xyz);
    vec3 v = uBlendMode == 0 ? ln : combineNormals(base, ln);
    oNormal.xyz = normalize(mix(base, v, alpha1.x));
  }
  if ((uChannelMask & 32) != 0 && alpha1.y > 0.0)
    oEmissive.rgb = mix(aE.rgb, clamp(blend3(uBlendMode == 0 ? 0 : uBlendMode, aE.rgb, le), 0.0, 1.0), alpha1.y);
}
