// Blends the brush material into the target layer channels through the stroke
// mask. Reads the pre-stroke snapshot, writes the live layer texture, so the
// whole stroke is re-evaluated every frame and opacity never builds up past
// the brush opacity within one stroke.
#include "common.glsl"
#include "material.glsl"

in vec2 vUV;

// outputs (only bound ones are written; see Painter::applyStroke)
#ifndef EXTRA_PASS
layout(location = 0) out vec4 oColor;     // rgb + alpha
layout(location = 1) out vec4 oRough;     // r value, g alpha
layout(location = 2) out vec4 oMetal;
layout(location = 3) out vec4 oHeight;
layout(location = 4) out vec4 oNormal;    // encoded xyz + alpha
layout(location = 5) out vec4 oEmissive;  // rgb + alpha
#else
// EXTRA_PASS (0.26): opacity, sheen, clearcoat and subsurface in a second pass (GPUs write at most 8 targets);
// each is a value in r and its coverage in g, like roughness
layout(location = 0) out vec4 oX0;
layout(location = 1) out vec4 oX1;
layout(location = 2) out vec4 oX2;
layout(location = 3) out vec4 oX3;
uniform sampler2D uSnapX0;
uniform sampler2D uSnapX1;
uniform sampler2D uSnapX2;
uniform sampler2D uSnapX3;
uniform vec4 uChanOpacity2;
#endif

uniform sampler2D uSnapColor;
uniform sampler2D uSnapRough;
uniform sampler2D uSnapMetal;
uniform sampler2D uSnapHeight;
uniform sampler2D uSnapNormal;
uniform sampler2D uSnapEmissive;
uniform sampler2D uStrokeMask;
uniform sampler2D uPosMap;
uniform sampler2D uNrmMap;
uniform sampler2D uTanMap;

uniform sampler2D uSeamMap;   // gutter texel -> UV across the seam (x < 0 none)
uniform int uUseSeams;

uniform int uTool;            // 0 paint, 1 erase, 2 mask paint (value), 3 mask erase, 4 blur, 5 mask blur, 6 clone,
                              // 7 mask clone, 8 smudge, 9 mask smudge
uniform int uSmudgeUV;        // 1: uSmudgeDelta.xy is the motion in texels (UV strokes)
uniform vec3 uSmudgeDelta;    // motion since the previous dab: object space (3D strokes) or texels
uniform float uSmudgeMax;     // texels

// stencil: a picture fixed on the screen of the painting camera
uniform int uStencilOn;
uniform sampler2D uStencil;
uniform mat3 uStencilXform;     // viewport pixels (top left origin) -> stencil coordinates
uniform mat4 uStencilViewProj;
uniform vec2 uStencilViewport;
uniform int uStencilMode;       // 0 mask (shape), 1 decal (colors)
uniform int uStencilInvert;
uniform int uStencilTile;
uniform float uStencilLod;
uniform vec2 uCloneOffset;    // texels from destination to source
uniform float uBlurRadius;    // texels
uniform float uOpacity;
uniform vec4 uChanOpacity0;   // per channel opacity: color, rough, metal, height
uniform vec2 uChanOpacity1;   // normal, emissive
uniform float uMaskValue;     // value painted into a mask
// (0.41) a baked map layer: its fixes are the target (r the value painted, g how much of it covers the bake)
uniform int uBaked;
uniform sampler2D uBakedRaw;   // r: the bake
uniform sampler2D uBakedComp;  // r: the bake with the fixes at the stroke's start (blur and clone read it)
uniform int uHeightMode;      // 0 replace, 1 add (relative), 2 max

vec4 over(vec4 snap, vec3 value, float s) {
  float a = snap.a + s * (1.0 - snap.a);
  vec3 v = a > 1e-5 ? (snap.rgb * snap.a * (1.0 - s) + value * s) / a : value;
  return vec4(v, a);
}
vec2 over1(vec2 snap, float value, float s) {
  float a = snap.y + s * (1.0 - snap.y);
  float v = a > 1e-5 ? (snap.x * snap.y * (1.0 - s) + value * s) / a : value;
  return vec2(v, a);
}

bool covered(ivec2 q) {
  ivec2 size = textureSize(uPosMap, 0);
  return all(greaterThanEqual(q, ivec2(0))) && all(lessThan(q, size)) && texelFetch(uPosMap, q, 0).w > 0.5;
}
// Where to read texel q (a tap around the covered texel p): q itself when it is on the
// surface; in the gutter next to a UV seam, the matching texel across the seam; past the
// texture border, the other border when the island wraps around there (cylinders, spheres).
// False when there is nothing to read.
bool tapAt(ivec2 p, ivec2 q, out ivec2 src) {
  src = q;
  if (covered(q)) return true;
  if (uUseSeams == 0) return false;
  ivec2 size = textureSize(uPosMap, 0);
  if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, size))) {
    ivec2 w = q - size * ivec2(floor(vec2(q) / vec2(size)));  // (% is undefined for negative values)
    if (!covered(w)) return false;
    vec3 Pp = texelFetch(uPosMap, p, 0).xyz;
    vec3 a = covered(p + ivec2(1, 0)) ? texelFetch(uPosMap, p + ivec2(1, 0), 0).xyz - Pp
           : covered(p - ivec2(1, 0)) ? Pp - texelFetch(uPosMap, p - ivec2(1, 0), 0).xyz : vec3(0);
    vec3 b = covered(p + ivec2(0, 1)) ? texelFetch(uPosMap, p + ivec2(0, 1), 0).xyz - Pp
           : covered(p - ivec2(0, 1)) ? Pp - texelFetch(uPosMap, p - ivec2(0, 1), 0).xyz : vec3(0);
    float stepLen = max(length(a), length(b));
    // the same surface carries on there: the wrapped texel is about as far away in 3D as the tap is in texels
    if (stepLen <= 0.0 || length(texelFetch(uPosMap, w, 0).xyz - Pp) > (length(vec2(q - p)) + 1.5) * stepLen * 1.5)
      return false;
    src = w;
    return true;
  }
  vec2 s = texelFetch(uSeamMap, q, 0).xy;
  if (s.x < 0.0) return false;
  src = clamp(ivec2(s * vec2(size)), ivec2(0), size - 1);
  return covered(src);
}

// Coverage-weighted blur of a snapshot around p, staying inside UV islands (and crossing seams).
vec4 blurTap(sampler2D t, ivec2 p, bool alphaInG) {
  ivec2 size = textureSize(t, 0);
  vec4 acc = vec4(0);
  float wsum = 0.0, asum = 0.0, n = 0.0;
  const int R = 2;
  for (int y = -R; y <= R; ++y)
    for (int x = -R; x <= R; ++x) {
      ivec2 q;
      if (!tapAt(p, p + ivec2(vec2(x, y) * uBlurRadius * 0.5), q)) continue;
      vec4 v = texelFetch(t, q, 0);
      float a = alphaInG ? v.g : v.a;
      float w = exp(-float(x * x + y * y) * 0.35);
      acc += v * a * w;
      wsum += a * w;
      asum += a * w;
      n += w;
    }
  if (n <= 0.0) return texelFetch(t, p, 0);
  vec4 r = wsum > 1e-5 ? acc / wsum : texelFetch(t, p, 0);
  if (alphaInG) r.g = asum / n;
  else r.a = asum / n;
  return r;
}
vec4 blurPlain(sampler2D t, ivec2 p) {
  ivec2 size = textureSize(t, 0);
  vec4 acc = vec4(0);
  float n = 0.0;
  for (int y = -2; y <= 2; ++y)
    for (int x = -2; x <= 2; ++x) {
      ivec2 q;
      if (!tapAt(p, p + ivec2(vec2(x, y) * uBlurRadius * 0.5), q)) continue;
      float w = exp(-float(x * x + y * y) * 0.35);
      acc += texelFetch(t, q, 0) * w;
      n += w;
    }
  return n > 0.0 ? acc / n : texelFetch(t, p, 0);
}

// rgb = the stencil's color at texel p, a = how much paint gets through
vec4 stencilAt(ivec2 p) {
  vec4 P = texelFetch(uPosMap, p, 0);
  vec4 clip = uStencilViewProj * vec4(P.xyz, 1.0);
  float shape = 0.0;
  vec3 col = vec3(1.0);
  if (clip.w > 0.0) {
    vec2 ndc = clip.xy / clip.w;
    vec2 sp = vec2((ndc.x * 0.5 + 0.5) * uStencilViewport.x, (0.5 - ndc.y * 0.5) * uStencilViewport.y);
    vec2 st = (uStencilXform * vec3(sp, 1.0)).xy;
    bool inside = uStencilTile == 1 || (st.x >= 0.0 && st.y >= 0.0 && st.x <= 1.0 && st.y <= 1.0);
    if (inside) {
      vec4 c = textureLod(uStencil, vec2(st.x, 1.0 - st.y), uStencilLod);
      col = c.rgb;
      shape = uStencilMode == 1 ? c.a : c.a * dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
    }
  }
  if (uStencilInvert == 1) shape = 1.0 - shape;
  return vec4(col, clamp(shape, 0.0, 1.0));
}

// --- smudge
bool surfStep(ivec2 p, ivec2 d, out vec3 v) {
  bool a = covered(p + d), b = covered(p - d);
  vec3 P = texelFetch(uPosMap, p, 0).xyz;
  if (a && b) v = (texelFetch(uPosMap, p + d, 0).xyz - texelFetch(uPosMap, p - d, 0).xyz) * 0.5;
  else if (a) v = texelFetch(uPosMap, p + d, 0).xyz - P;
  else if (b) v = P - texelFetch(uPosMap, p - d, 0).xyz;
  else { v = vec3(0); return false; }
  return true;
}
// The motion in texels at p: least squares fit of the 3D motion to the surface's texel steps.
vec2 smudgeOffset(ivec2 p) {
  vec2 o;
  if (uSmudgeUV == 1) {
    o = uSmudgeDelta.xy;
  } else {
    vec3 a, b;
    if (!surfStep(p, ivec2(1, 0), a) || !surfStep(p, ivec2(0, 1), b)) return vec2(0);
    float aa = dot(a, a), ab = dot(a, b), bb = dot(b, b);
    float det = aa * bb - ab * ab;
    if (det <= 1e-6 * aa * bb || det <= 0.0) return vec2(0);
    float ad = dot(a, uSmudgeDelta), bd = dot(b, uSmudgeDelta);
    o = vec2(bb * ad - ab * bd, aa * bd - ab * ad) / det;
  }
  float l = length(o);
  return l > uSmudgeMax ? o * (uSmudgeMax / l) : o;
}
// Bilinear read at a fractional texel position over surface texels only (premultiplied,
// so empty texels don't tint). Alpha is in .g for the single-value channels.
vec4 smudgeFetch(ivec2 p, sampler2D t, vec2 sp, bool alphaInG, out bool ok) {
  vec2 f = sp - 0.5;
  ivec2 b = ivec2(floor(f));
  vec2 w = f - vec2(b);
  vec4 acc = vec4(0);
  float wsum = 0.0, asum = 0.0;
  for (int k = 0; k < 4; ++k) {
    ivec2 o = ivec2(k & 1, k >> 1);
    float wk = (o.x == 1 ? w.x : 1.0 - w.x) * (o.y == 1 ? w.y : 1.0 - w.y);
    ivec2 q;
    if (wk <= 0.0 || !tapAt(p, b + o, q)) continue;
    vec4 v = texelFetch(t, q, 0);
    float a = alphaInG ? v.g : v.a;
    acc += v * a * wk;
    asum += a * wk;
    wsum += wk;
  }
  ok = wsum > 1e-4;
  if (!ok) return vec4(0);
  vec4 r = asum > 1e-5 ? acc / asum : vec4(0);
  if (alphaInG) r.g = asum / wsum;
  else r.a = asum / wsum;
  return r;
}
vec4 mixPremul(vec4 a, vec4 b, float k, bool alphaInG) {
  if (alphaInG) {
    float al = mix(a.g, b.g, k);
    float v = al > 1e-5 ? mix(a.r * a.g, b.r * b.g, k) / al : b.r;
    return vec4(v, al, 0, 1);
  }
  float al = mix(a.a, b.a, k);
  vec3 v = al > 1e-5 ? mix(a.rgb * a.a, b.rgb * b.a, k) / al : b.rgb;
  return vec4(v, al);
}

#ifdef EXTRA_PASS
vec4 extraOne(int i, sampler2D snap, ivec2 p, float m, float k, float value) {
  vec4 sx = texelFetch(snap, p, 0);
  if (uTool == 8) {
    bool ok;
    vec2 sp = vec2(p) + 0.5 - smudgeOffset(p);
    vec4 q = smudgeFetch(p, snap, sp, true, ok);
    return ok ? mixPremul(sx, q, m * k, true) : sx;
  }
  if (m <= 0.0) return sx;
  if (uTool == 6) {
    ivec2 q = clamp(p + ivec2(round(uCloneOffset)), ivec2(0), textureSize(snap, 0) - 1);
    return vec4(mix(sx.rg, texelFetch(snap, q, 0).rg, m * k), 0, 1);
  }
  if (uTool == 4) return vec4(mix(sx.rg, blurTap(snap, p, true).rg, m * k), 0, 1);
  if (uTool == 1) return vec4(sx.r, sx.g * (1.0 - m * k), 0, 1);
  return vec4(over1(sx.rg, value, m * k), 0, 1);
}

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  float m = texelFetch(uStrokeMask, p, 0).r * uOpacity;
  if (uStencilOn == 1 && uTool <= 3 && m > 0.0) m *= stencilAt(p).a;
  if ((uTool == 8) && (m <= 0.0 || !covered(p))) discard;
  vec4 v = vec4(1, 0, 0, 0);
  if (uTool == 0 && m > 0.0) {
    MatSample ms = sampleMaterial(texelFetch(uPosMap, p, 0).xyz, normalize(texelFetch(uNrmMap, p, 0).xyz),
                                  texelFetch(uTanMap, p, 0), vUV);
    v = ms.extra;
  }
  oX0 = extraOne(0, uSnapX0, p, m, uChanOpacity2.x, v.x);
  oX1 = extraOne(1, uSnapX1, p, m, uChanOpacity2.y, v.y);
  oX2 = extraOne(2, uSnapX2, p, m, uChanOpacity2.z, v.z);
  oX3 = extraOne(3, uSnapX3, p, m, uChanOpacity2.w, v.w);
}
#else
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  float m = texelFetch(uStrokeMask, p, 0).r * uOpacity;
  vec4 stencil = vec4(1.0);
  if (uStencilOn == 1 && uTool <= 3 && m > 0.0) {
    stencil = stencilAt(p);
    m *= stencil.a;
  }
  vec4 sc = texelFetch(uSnapColor, p, 0), sr = texelFetch(uSnapRough, p, 0), sm = texelFetch(uSnapMetal, p, 0);
  vec4 sh = texelFetch(uSnapHeight, p, 0), sn = texelFetch(uSnapNormal, p, 0), se = texelFetch(uSnapEmissive, p, 0);

  if (uBaked == 1) {
    // (0.41) every tool lays a value over the bake with the fixes so far; the eraser takes fixes away (the bake
    // shows again)
    if (uTool == 9) {
      // smudge: the snapshot slot holds the live fixes around this dab
      if (m <= 0.0 || !covered(p)) discard;
      vec2 sp = vec2(p) + 0.5 - smudgeOffset(p);
      vec2 f = sp - 0.5;
      ivec2 b = ivec2(floor(f));
      vec2 w = f - vec2(b);
      float acc = 0.0, ws = 0.0;
      for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        float wk = (o.x == 1 ? w.x : 1.0 - w.x) * (o.y == 1 ? w.y : 1.0 - w.y);
        ivec2 q;
        if (wk <= 0.0 || !tapAt(p, b + o, q)) continue;
        vec2 fx = texelFetch(uSnapColor, q, 0).rg;
        acc += mix(texelFetch(uBakedRaw, q, 0).r, fx.r, fx.g) * wk;
        ws += wk;
      }
      if (ws <= 1e-4) discard;
      oColor = vec4(over1(sc.rg, acc / ws, m), 0, 1);
      return;
    }
    if (m <= 0.0) {
      oColor = vec4(sc.rg, 0, 1);
      return;
    }
    if (uTool == 3) {
      oColor = vec4(sc.r, sc.g * (1.0 - m), 0, 1);
      return;
    }
    float value = uMaskValue;
    if (uTool == 5) value = blurPlain(uBakedComp, p).r;
    if (uTool == 7) value = texelFetch(uBakedComp, clamp(p + ivec2(round(uCloneOffset)), ivec2(0), textureSize(uBakedComp, 0) - 1), 0).r;
    oColor = vec4(over1(sc.rg, value, m), 0, 1);
    return;
  }

  if (uTool == 8 || uTool == 9) {
    // the snapshot slots hold copies of the live channels around this dab
    if (m <= 0.0 || !covered(p)) discard;
    vec2 sp = vec2(p) + 0.5 - smudgeOffset(p);
    bool ok;
    if (uTool == 9) {
      // a mask is one value with no coverage of its own: plain bilinear over surface texels
      vec4 mv = vec4(0);
      float ws = 0.0;
      vec2 f = sp - 0.5;
      ivec2 b = ivec2(floor(f));
      vec2 w = f - vec2(b);
      for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        float wk = (o.x == 1 ? w.x : 1.0 - w.x) * (o.y == 1 ? w.y : 1.0 - w.y);
        ivec2 q;
        if (wk <= 0.0 || !tapAt(p, b + o, q)) continue;
        mv += texelFetch(uSnapColor, q, 0) * wk;
        ws += wk;
      }
      if (ws <= 1e-4) discard;
      oColor = vec4(mix(sc.r, mv.r / ws, m));
      return;
    }
    vec4 qc = smudgeFetch(p, uSnapColor, sp, false, ok);
    if (!ok) discard;
    vec4 qr = smudgeFetch(p, uSnapRough, sp, true, ok), qm = smudgeFetch(p, uSnapMetal, sp, true, ok);
    vec4 qh = smudgeFetch(p, uSnapHeight, sp, true, ok), qe = smudgeFetch(p, uSnapEmissive, sp, false, ok);
    vec4 qn = smudgeFetch(p, uSnapNormal, sp, false, ok);
    oColor = mixPremul(sc, qc, m * uChanOpacity0.x, false);
    oRough = mixPremul(sr, qr, m * uChanOpacity0.y, true);
    oMetal = mixPremul(sm, qm, m * uChanOpacity0.z, true);
    oHeight = mixPremul(sh, qh, m * uChanOpacity0.w, true);
    vec4 nn = mixPremul(vec4(decodeNormal(sn.rgb), sn.a), vec4(decodeNormal(qn.rgb), qn.a), m * uChanOpacity1.x, false);
    oNormal = vec4(encodeNormal(normalize(nn.rgb + vec3(0, 0, 1e-4))), nn.a);
    oEmissive = mixPremul(se, qe, m * uChanOpacity1.y, false);
    return;
  }
  if (uTool == 2 || uTool == 3) {
    // mask target: single channel in .r of uSnapColor slot
    float v = uTool == 2 ? uMaskValue : 0.0;
    oColor = vec4(mix(sc.r, v, m));
    return;
  }
  if (uTool == 5) {
    oColor = vec4(m > 0.0 ? mix(sc.r, blurPlain(uSnapColor, p).r, m) : sc.r);
    return;
  }
  if (uTool == 7) {
    ivec2 q = clamp(p + ivec2(round(uCloneOffset)), ivec2(0), textureSize(uSnapColor, 0) - 1);
    oColor = vec4(mix(sc.r, texelFetch(uSnapColor, q, 0).r, m));
    return;
  }
  if (uTool == 6) {
    if (m <= 0.0) {
      oColor = sc; oRough = sr; oMetal = sm; oHeight = sh; oNormal = sn; oEmissive = se;
      return;
    }
    ivec2 q = clamp(p + ivec2(round(uCloneOffset)), ivec2(0), textureSize(uSnapColor, 0) - 1);
    vec4 qc = texelFetch(uSnapColor, q, 0), qr = texelFetch(uSnapRough, q, 0), qm = texelFetch(uSnapMetal, q, 0);
    vec4 qh = texelFetch(uSnapHeight, q, 0), qn = texelFetch(uSnapNormal, q, 0), qe = texelFetch(uSnapEmissive, q, 0);
    // copy value and coverage: mix toward the source texel
    oColor = mix(sc, qc, m * uChanOpacity0.x);
    oRough = vec4(mix(sr.rg, qr.rg, m * uChanOpacity0.y), 0, 1);
    oMetal = vec4(mix(sm.rg, qm.rg, m * uChanOpacity0.z), 0, 1);
    oHeight = vec4(mix(sh.rg, qh.rg, m * uChanOpacity0.w), 0, 1);
    vec4 nn = mix(sn, qn, m * uChanOpacity1.x);
    oNormal = vec4(encodeNormal(normalize(decodeNormal(nn.rgb) + vec3(0, 0, 1e-4))), nn.a);
    oEmissive = mix(se, qe, m * uChanOpacity1.y);
    return;
  }
  if (uTool == 4) {
    if (m <= 0.0) {
      oColor = sc; oRough = sr; oMetal = sm; oHeight = sh; oNormal = sn; oEmissive = se;
      return;
    }
    oColor = mix(sc, blurTap(uSnapColor, p, false), m * uChanOpacity0.x);
    oRough = vec4(mix(sr.rg, blurTap(uSnapRough, p, true).rg, m * uChanOpacity0.y), 0, 1);
    oMetal = vec4(mix(sm.rg, blurTap(uSnapMetal, p, true).rg, m * uChanOpacity0.z), 0, 1);
    oHeight = vec4(mix(sh.rg, blurTap(uSnapHeight, p, true).rg, m * uChanOpacity0.w), 0, 1);
    vec4 bn = blurTap(uSnapNormal, p, false);
    vec4 nn = mix(sn, bn, m * uChanOpacity1.x);
    oNormal = vec4(encodeNormal(normalize(decodeNormal(nn.rgb))), nn.a);
    oEmissive = mix(se, blurTap(uSnapEmissive, p, false), m * uChanOpacity1.y);
    return;
  }
  if (m <= 0.0) {
    oColor = sc; oRough = sr; oMetal = sm; oHeight = sh; oNormal = sn; oEmissive = se;
    return;
  }
  if (uTool == 1) {
    oColor = vec4(sc.rgb, sc.a * (1.0 - m * uChanOpacity0.x));
    oRough = vec4(sr.r, sr.g * (1.0 - m * uChanOpacity0.y), 0, 1);
    oMetal = vec4(sm.r, sm.g * (1.0 - m * uChanOpacity0.z), 0, 1);
    oHeight = vec4(sh.r, sh.g * (1.0 - m * uChanOpacity0.w), 0, 1);
    oNormal = vec4(sn.rgb, sn.a * (1.0 - m * uChanOpacity1.x));
    oEmissive = vec4(se.rgb, se.a * (1.0 - m * uChanOpacity1.y));
    return;
  }
  vec4 P = texelFetch(uPosMap, p, 0);
  vec4 N = texelFetch(uNrmMap, p, 0);
  vec4 T = texelFetch(uTanMap, p, 0);
  MatSample ms = sampleMaterial(P.xyz, normalize(N.xyz), T, vUV);
  if (uStencilOn == 1 && uStencilMode == 1) ms.color = stencil.rgb;  // decal: the picture's colors
  oColor = over(sc, ms.color, m * uChanOpacity0.x);
  oRough = vec4(over1(sr.rg, ms.rough, m * uChanOpacity0.y), 0, 1);
  oMetal = vec4(over1(sm.rg, ms.metal, m * uChanOpacity0.z), 0, 1);
  float h = ms.height;
  if (uHeightMode == 1) h = clamp((sh.g > 0.0 ? sh.r : 0.5) + (ms.height - 0.5), 0.0, 1.0);
  if (uHeightMode == 2) h = max(sh.g > 0.0 ? sh.r : 0.5, ms.height);
  oHeight = vec4(over1(sh.rg, h, m * uChanOpacity0.w), 0, 1);
  vec4 nn = over(vec4(decodeNormal(sn.rgb), sn.a), ms.normal, m * uChanOpacity1.x);
  oNormal = vec4(encodeNormal(normalize(nn.rgb)), nn.a);
  oEmissive = over(se, ms.emissive, m * uChanOpacity1.y);
}
#endif
