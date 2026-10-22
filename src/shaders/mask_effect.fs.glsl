// One step of a layer's mask effect stack (texture space). Reads the stack so far
// (uPrev) and writes the new value. Blur runs in two passes: MASKFX_BLUR_H writes the
// horizontally blurred value only, MASKFX_BLUR_V finishes and mixes with uPrev (Edges: the
// band where the blurred stack crosses the middle). Grow / shrink runs the same way with a
// maximum / minimum in place of the average (MASKFX_GROW_H, MASKFX_GROW_V).
#include "common.glsl"
#include "material.glsl"
#include "maskgen.glsl"

in vec2 vUV;
out vec4 oValue;

uniform sampler2D uPrev;     // r: the stack so far
uniform int uFxType;         // MaskEffect::Type
uniform int uFxBlend;        // MaskEffect::Blend
uniform float uFxOpacity;
uniform sampler2D uPaint;    // Paint: the layer's painted mask (white when none)
uniform float uFxValue;      // Fill
uniform vec4 uFxNoise;       // Noise: scale, seed, contrast
uniform vec4 uFxLevelsIn;    // Levels: in low, in high, gamma
uniform vec2 uFxLevelsOut;   // Levels: out low, out high
uniform float uFxRadius;     // Blur, Edges, Grow: texels
uniform int uFxNoiseType;    // Noise
uniform int uFxGrow;         // Grow: 1 grow, 0 shrink
uniform int uFxEdges;        // Blur pass of an Edges effect
uniform vec4 uFxPattern;     // Pattern: x source (0 height, 1 brightness, 2 roughness, 3 metallic), y level, z contrast, w invert
uniform vec4 uTileUV;        // mesh UV of this texture: offset xy, size zw
uniform sampler2D uBlurSrc;  // Blur: what to blur (V pass: the H result)

float blendOp(int m, float a, float b) {
  if (m == 1) return a * b;
  if (m == 2) return min(a + b, 1.0);
  if (m == 3) return max(a - b, 0.0);
  if (m == 4) return max(a, b);
  if (m == 5) return min(a, b);
  if (m == 6) return 1.0 - (1.0 - a) * (1.0 - b);
  return b;
}

float blurAxis(ivec2 p, ivec2 axis) {
  // gaussian weighted by surface coverage, so islands don't pick up the background
  int r = int(ceil(uFxRadius));
  float sigma = max(uFxRadius * 0.5, 0.5);
  ivec2 size = textureSize(uBlurSrc, 0);
  float sum = 0.0, wsum = 0.0;
  for (int i = -r; i <= r; ++i) {
    ivec2 q = clamp(p + axis * i, ivec2(0), size - 1);
    float cov = texelFetch(uPosMap, q, 0).w;
    float w = exp(-float(i * i) / (2.0 * sigma * sigma)) * (cov > 0.5 ? 1.0 : 0.0);
    sum += texelFetch(uBlurSrc, q, 0).r * w;
    wsum += w;
  }
  return wsum > 0.0 ? sum / wsum : texelFetch(uBlurSrc, p, 0).r;
}

// maximum (grow) or minimum (shrink) along an axis, over the covered texels only
float growAxis(ivec2 p, ivec2 axis) {
  int r = int(ceil(uFxRadius));
  ivec2 size = textureSize(uBlurSrc, 0);
  float v = texelFetch(uBlurSrc, p, 0).r;
  for (int i = -r; i <= r; ++i) {
    ivec2 q = clamp(p + axis * i, ivec2(0), size - 1);
    if (texelFetch(uPosMap, q, 0).w < 0.5) continue;
    float s = texelFetch(uBlurSrc, q, 0).r;
    v = uFxGrow == 1 ? max(v, s) : min(v, s);
  }
  return v;
}

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  float prev = texelFetch(uPrev, p, 0).r;
#if defined(MASKFX_BLUR_H)
  oValue = vec4(blurAxis(p, ivec2(1, 0)));
  return;
#elif defined(MASKFX_BLUR_V)
  float b = blurAxis(p, ivec2(0, 1));
  if (uFxEdges == 1) b = smoothstep(0.0, 0.6, 1.0 - abs(b * 2.0 - 1.0));  // where the stack goes from black to white
  oValue = vec4(mix(prev, b, uFxOpacity));
  return;
#elif defined(MASKFX_GROW_H)
  oValue = vec4(growAxis(p, ivec2(1, 0)));
  return;
#elif defined(MASKFX_GROW_V)
  oValue = vec4(mix(prev, growAxis(p, ivec2(0, 1)), uFxOpacity));
  return;
#else
  float v;
  bool filterOp = false;
  if (uFxType == 0) {
    v = texture(uPaint, vUV).r;
  } else if (uFxType == 1) {
    v = generatorMask(p);
  } else if (uFxType == 2) {
    v = uFxValue;
  } else if (uFxType == 3) {
    vec4 P = texelFetch(uPosMap, p, 0);
    float n = typedNoise(P.xyz * uFxNoise.x / max(uGenBounds.z, 1e-4) * 4.0 + vec3(uFxNoise.y * 17.31), uFxNoiseType);
    v = levels(n, 0.5, uFxNoise.z);
  } else if (uFxType == 4) {
    filterOp = true;
    float t = clamp((prev - uFxLevelsIn.x) / max(uFxLevelsIn.y - uFxLevelsIn.x, 1e-4), 0.0, 1.0);
    t = pow(t, 1.0 / max(uFxLevelsIn.z, 0.01));
    v = mix(uFxLevelsOut.x, uFxLevelsOut.y, t);
  } else if (uFxType == 9) {
    // a library material's value, projected the way a fill layer projects it
    vec4 P = texelFetch(uPosMap, p, 0);
    MatSample m = sampleMaterial(P.xyz, normalize(texelFetch(uNrmMap, p, 0).xyz), texelFetch(uTanMap, p, 0),
                                 uTileUV.xy + vUV * uTileUV.zw);
    int src = int(uFxPattern.x + 0.5);
    v = src == 1 ? luminance(m.color) : src == 2 ? m.rough : src == 3 ? m.metal : m.height;
    if (uFxPattern.z > 0.0) v = levels(v, uFxPattern.y, uFxPattern.z);
    if (uFxPattern.w > 0.5) v = 1.0 - v;
  } else if (uFxType == 6) {
    filterOp = true;
    v = 1.0 - prev;
  } else {
    v = prev;
    filterOp = true;
  }
  float r = filterOp ? v : blendOp(uFxBlend, prev, v);
  oValue = vec4(clamp(mix(prev, r, uFxOpacity), 0.0, 1.0));
#endif
}
