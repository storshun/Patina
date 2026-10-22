// Smart-mask generators from the baked mesh maps (shared by the compositor and the
// mask effect stack). Needs common.glsl.
// generator mask (fill layers and paint layers alike)
uniform int uGenType;           // 0 none, 1 edges (curvature), 2 cavity, 3 ambient occlusion, 4 gradient Y, 5 noise,
                                // 6 facing (normal toward uGenDir), 7 material slot uGenSlot, 8 thin parts (thickness),
                                // 9 gradient along uGenDir, 10 drips (streaks running down the sides)
uniform int uGenNoiseType;      // noise: 0 clouds, 1 cells, 2 cracks, 3 ridges, 4 streaks
uniform vec3 uGenCenter;        // object bounds: center and half size (gradient)
uniform vec3 uGenHalf;
uniform vec3 uGenDir;
uniform int uGenSlot;
uniform sampler2D uMatIdMap;    // r = material slot / 255
uniform vec4 uGenParams;        // x level, y contrast, z noise amount, w noise scale
uniform int uGenInvert;
uniform vec4 uGenBounds;        // object bounds: x minY, y maxY, z radius
uniform sampler2D uPosMap;
uniform sampler2D uNrmMap;
uniform sampler2D uTanMap;
uniform sampler2D uAOMap;       // r = ambient occlusion
uniform sampler2D uCurvMap;     // r = curvature (0.5 = flat, >0.5 convex)
uniform sampler2D uThickMap;    // r = thickness (1 thick, 0 thin)

float levels(float v, float level, float contrast) {
  float w = mix(0.5, 0.002, clamp(contrast, 0.0, 1.0));
  return smoothstep(level - w, level + w, v);
}

// 3D cellular noise: distances to the nearest and second nearest feature points (cell units)
vec2 cells3(vec3 p) {
  vec3 i = floor(p), f = fract(p);
  float f1 = 8.0, f2 = 8.0;
  for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x) {
        vec3 g = vec3(x, y, z);
        vec3 r = g + hash33(i + g) - f;
        float d = dot(r, r);
        if (d < f1) { f2 = f1; f1 = d; }
        else if (d < f2) f2 = d;
      }
  return sqrt(vec2(f1, f2));
}

// ridged noise: thin bright veins
float ridged3(vec3 p) {
  float s = 0.0, a = 0.5, n = 0.0;
  for (int i = 0; i < 5; ++i) {
    float r = 1.0 - abs(vnoise3(p) * 2.0 - 1.0);
    s += a * r * r * r;
    n += a;
    p = p * 2.03 + vec3(17.1, 3.7, 9.2);
    a *= 0.5;
  }
  return clamp((s / n - 0.35) * 1.8, 0.0, 1.0);
}

// object-space noise of a kind: 0 clouds, 1 cells (bright centers), 2 cracks (along the cell borders),
// 3 ridges, 4 streaks running down
float typedNoise(vec3 q, int kind) {
  if (kind == 1) return 1.0 - clamp(cells3(q * 1.5).x, 0.0, 1.0);
  if (kind == 2) {
    vec2 c = cells3(q * 1.5);
    return 1.0 - smoothstep(0.0, 0.3, c.y - c.x);
  }
  if (kind == 3) return ridged3(q);
  if (kind == 4) return fbm3(vec3(q.x * 2.5, q.y * 0.12, q.z * 2.5), 5);
  return fbm3(q, 5);
}

// streaks running down a surface seen from one side: columns along u, each from a random start downwards
float dripsPlane(vec2 uy, float seed) {
  float v = 0.0;
  for (int k = 0; k < 2; ++k) {  // two rows of columns, offset, so they don't line up
    float x = uy.x * 6.0 + float(k) * 0.5;
    float cell = floor(x);
    vec3 h = hash33(vec3(cell, seed, float(k) + 1.0));
    float wig = (vnoise3(vec3(cell, uy.y * 4.0, seed + float(k))) - 0.5) * 0.12;
    float w = 0.05 + 0.1 * h.z;
    float line = 1.0 - smoothstep(w * 0.5, w, abs(fract(x) - (0.3 + 0.4 * h.y) + wig));
    float y0 = (h.x - 0.5) * 8.0;  // where it starts (a model spans about -4..4 here)
    float len = 0.8 + 3.0 * hash13(vec3(cell, seed + 3.0, float(k)));
    float t = (y0 - uy.y) / len;  // 0 at the start, 1 at the end
    float run = smoothstep(0.0, 0.03, t) * (1.0 - smoothstep(0.4, 1.0, t)) * step(0.3, hash13(vec3(cell, seed + 7.0, float(k))));
    v = max(v, line * run * (0.55 + 0.45 * (1.0 - t)));
  }
  return v;
}

// streaks running down the sides from random starts, fading as they go (rain marks, rust runs, leaks):
// seen from the sides along x and z, blended by which way the surface faces
float drips(vec3 P, vec3 N, float scale) {
  vec3 q = P * scale;
  vec2 w = pow(abs(N.xz) + 1e-4, vec2(4.0));
  w /= w.x + w.y;
  float v = dripsPlane(vec2(q.z, q.y), 1.0) * w.x + dripsPlane(vec2(q.x, q.y), 2.0) * w.y;
  float side = 1.0 - smoothstep(0.6, 0.92, abs(N.y));  // on the sides, not the tops
  return clamp(v * side * 1.3, 0.0, 1.0);
}

float generatorMask(ivec2 p) {
  if (uGenType == 0) return 1.0;
  vec4 P = texelFetch(uPosMap, p, 0);
  float v = 0.0;
  if (uGenType == 1) v = texelFetch(uCurvMap, p, 0).r;
  else if (uGenType == 2) v = 1.0 - texelFetch(uCurvMap, p, 0).r * texelFetch(uAOMap, p, 0).r * 2.0 + 0.5;
  else if (uGenType == 3) v = 1.0 - texelFetch(uAOMap, p, 0).r;
  else if (uGenType == 4) v = (P.y - uGenBounds.x) / max(uGenBounds.y - uGenBounds.x, 1e-5);
  else if (uGenType == 5) v = typedNoise(P.xyz * uGenParams.w / max(uGenBounds.z, 1e-4) * 4.0, uGenNoiseType);
  else if (uGenType == 6) v = dot(normalize(texelFetch(uNrmMap, p, 0).xyz), uGenDir) * 0.5 + 0.5;
  else if (uGenType == 7) v = abs(texelFetch(uMatIdMap, p, 0).r * 255.0 - float(uGenSlot)) < 0.5 ? 1.0 : 0.0;
  else if (uGenType == 8) v = 1.0 - texelFetch(uThickMap, p, 0).r;  // thin parts
  else if (uGenType == 9) v = dot(P.xyz - uGenCenter, uGenDir) / max(2.0 * dot(abs(uGenDir), uGenHalf), 1e-5) + 0.5;
  else if (uGenType == 10)
    v = drips(P.xyz, normalize(texelFetch(uNrmMap, p, 0).xyz), uGenParams.w / max(uGenBounds.z, 1e-4) * 4.0);
  if (uGenParams.z > 0.0 && uGenType != 5) {
    float n = fbm3(P.xyz * uGenParams.w / max(uGenBounds.z, 1e-4) * 4.0, 5);
    v += (n - 0.5) * uGenParams.z;
  }
  float m = levels(v, uGenParams.x, uGenParams.y);
  return uGenInvert == 1 ? 1.0 - m : m;
}

