// Procedural material generator. Writes tileable maps over uv in [0,1].
// Pass BAKE_SURFACE: color, rough/metal/height, emissive.
// Pass BAKE_FINISH (when asked for): grime in the low parts and worn raised edges, from the height map.
// Pass BAKE_NORMAL: tangent-space normal from the (wrapping) height map.
#include "common.glsl"
#include "noise2p.glsl"

in vec2 vUV;

#ifdef BAKE_SURFACE
layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oRMH;
layout(location = 2) out vec4 oEmissive;
layout(location = 3) out vec4 oExtra;  // opacity, sheen, clearcoat, subsurface (0.26)

uniform int uGen;
uniform vec3 uColA;    // sRGB
uniform vec3 uColB;
uniform vec3 uColC;
uniform vec2 uRough;   // A, B
uniform vec2 uMetal;   // A, B
uniform float uScale;  // base frequency (integer)
uniform float uDetail; // octaves
uniform float uSeed;
uniform float uHeightAmp;
uniform float uVariation;
uniform vec4 uExtra;
uniform vec3 uEmissive;
uniform vec2 uWarp;    // x amount (0 none), y size (whole wobbles per tile)
uniform vec4 uSurface = vec4(1, 0, 0, 0);  // the material's opacity, sheen, clearcoat, subsurface
uniform int uCutout;   // the holes of generators that have them are cut out (opacity 0)

// Set by generators (0.26): how much of this point is a hole (cut out when the material says so), and a
// multiplier for the material's sheen (velvet crush, fur clumps).
float gHole = 0.0;
float gSheen = 1.0;

struct Surf {
  vec3 col;
  float rough;
  float metal;
  float h;  // 0..1, 0.5 neutral
  vec3 emis;
};

Surf mixSurf(Surf a, Surf b, float t) {
  return Surf(mix(a.col, b.col, t), mix(a.rough, b.rough, t), mix(a.metal, b.metal, t), mix(a.h, b.h, t), mix(a.emis, b.emis, t));
}

int OCT() { return int(clamp(uDetail, 1.0, 8.0)); }
vec2 F() { return vec2(max(1.0, floor(uScale + 0.5))); }

Surf genFlat(vec2 uv) {
  float n = pfbm(uv, F(), OCT(), uSeed);
  vec3 c = uColA * (1.0 + (n - 0.5) * uVariation);
  float h = 0.5 + (n - 0.5) * uHeightAmp;
  return Surf(c, clamp(uRough.x + (n - 0.5) * uExtra.x, 0.0, 1.0), uMetal.x, h, uEmissive);
}

Surf genTwoTone(vec2 uv) {
  float n = pfbm(uv, F(), OCT(), uSeed);
  float t = smoothstep(0.5 - uExtra.y, 0.5 + uExtra.y, n + uExtra.x);
  Surf a = Surf(uColA, uRough.x, uMetal.x, 0.5, vec3(0));
  Surf b = Surf(uColB, uRough.y, uMetal.y, 0.5, vec3(0));
  Surf s = mixSurf(a, b, t);
  s.h = 0.5 + (n - 0.5) * uHeightAmp;
  return s;
}

Surf genBrushed(vec2 uv) {
  // anisotropic streaks along U: high frequency in V, low in U
  vec2 f = F();
  float streak = pfbm(uv, vec2(f.x * 0.25 + 1.0, f.y * 16.0), OCT(), uSeed);
  float fine = pnoise(uv * vec2(4.0, 256.0), vec2(4.0, 256.0), uSeed + 9.0);
  float n = mix(streak, fine, 0.5);
  float blot = pfbm(uv, f * 0.5 + 1.0, 3, uSeed + 4.0);
  vec3 c = uColA * (1.0 + (n - 0.5) * uVariation) * (1.0 - (blot - 0.5) * 0.15);
  float r = clamp(mix(uRough.x, uRough.y, n), 0.0, 1.0);
  return Surf(c, r, uMetal.x, 0.5 + (n - 0.5) * uHeightAmp, vec3(0));
}

float scratches(vec2 uv, float count, float seed) {
  // random thin line segments, periodic
  float s = 0.0;
  vec2 f = F() * 0.5 + 1.0;
  vec2 p = uv * f;
  vec2 i = floor(p);
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      vec2 g = i + vec2(x, y);
      vec2 cell = mod(g, f);
      for (int k = 0; k < 4; ++k) {
        if (float(k) >= count) break;
        vec2 h = hash22(cell * 1.3 + float(k) * 7.1 + seed);
        float ang = hash12(cell + float(k) * 3.7 + seed) * 6.2831;
        vec2 dir = vec2(cos(ang), sin(ang));
        vec2 c = g + h;
        float len = 0.3 + 0.9 * hash12(cell + float(k) + seed * 2.0);
        vec2 d = p - c;
        float t = clamp(dot(d, dir), -len, len);
        float dist = length(d - dir * t);
        float w = 0.004 + 0.01 * hash12(cell * 5.1 + float(k));
        float fade = 1.0 - abs(t) / len;
        s = max(s, (1.0 - smoothstep(0.0, w, dist)) * fade);
      }
    }
  return s;
}

Surf genScratched(vec2 uv) {
  Surf s = genBrushed(uv);
  float sc = scratches(uv, uExtra.x > 0.0 ? uExtra.x : 3.0, uSeed);
  s.col = mix(s.col, uColB, sc * 0.6);
  s.rough = mix(s.rough, uRough.y, sc);
  s.h -= sc * uHeightAmp * 2.0;
  return s;
}

Surf genRust(vec2 uv) {
  vec2 f = F();
  float base = pfbm(uv, f, OCT(), uSeed);
  float patches = pfbm(uv, f * 0.5 + 1.0, 5, uSeed + 11.0);
  float cover = smoothstep(uExtra.x - 0.08, uExtra.x + 0.08, patches + (base - 0.5) * 0.35);
  float grain = pnoise(uv * f * 8.0, f * 8.0, uSeed + 3.0);
  vec3 rustCol = mix(uColB, uColC, smoothstep(0.3, 0.8, base + (grain - 0.5) * 0.4));
  Surf metal = Surf(uColA * (0.9 + 0.2 * base), uRough.x, uMetal.x, 0.5, vec3(0));
  Surf rust = Surf(rustCol, uRough.y, uMetal.y, 0.5 + (0.4 + base * 0.6 + grain * 0.2) * uHeightAmp, vec3(0));
  Surf s = mixSurf(metal, rust, cover);
  s.h += (grain - 0.5) * uHeightAmp * 0.3 * cover;
  return s;
}

Surf genConcrete(vec2 uv) {
  vec2 f = F();
  float n = pfbm(uv, f, OCT(), uSeed);
  float big = pfbm(uv, f * 0.25 + 1.0, 3, uSeed + 5.0);
  vec4 v = pvoronoi(uv, f * 6.0, 1.0, uSeed + 2.0);
  float pores = 1.0 - smoothstep(0.0, uExtra.x > 0.0 ? uExtra.x : 0.12, v.x);
  pores *= step(0.7, v.z);
  vec3 c = mix(uColA, uColB, smoothstep(0.35, 0.65, big)) * (1.0 + (n - 0.5) * uVariation) * (1.0 - pores * 0.35);
  float h = 0.5 + (n - 0.5) * uHeightAmp - pores * uHeightAmp;
  return Surf(c, clamp(uRough.x + (n - 0.5) * 0.1 + pores * 0.1, 0.0, 1.0), uMetal.x, h, vec3(0));
}

Surf genWood(vec2 uv) {
  vec2 f = F();
  // rings run along U; distortion by noise
  float warp = pfbm(uv, vec2(2.0, f.y * 0.5 + 1.0), 4, uSeed) - 0.5;
  float rings = fract((uv.y + warp * 0.08 + pfbm(uv, vec2(1.0, 4.0), 3, uSeed + 2.0) * 0.05) * f.y);
  rings = smoothstep(0.0, 0.7, rings) * smoothstep(1.0, 0.8, rings);
  float grain = pnoise(uv * vec2(8.0, 256.0), vec2(8.0, 256.0), uSeed + 7.0);
  float t = clamp(rings * 0.8 + (grain - 0.5) * 0.4, 0.0, 1.0);
  vec3 c = mix(uColA, uColB, t) * (1.0 + (warp) * uVariation);
  float h = 0.5 + (t - 0.5) * uHeightAmp + (grain - 0.5) * uHeightAmp * 0.5;
  return Surf(c, mix(uRough.x, uRough.y, t), uMetal.x, h, vec3(0));
}

Surf genStone(vec2 uv) {
  vec2 f = F();
  vec4 v = pvoronoi(uv, f, 0.9, uSeed);
  float n = pfbm(uv, f * 4.0, OCT(), uSeed + 1.0);
  float crack = 1.0 - smoothstep(0.0, uExtra.x > 0.0 ? uExtra.x : 0.06, v.w);
  vec3 cellCol = mix(uColA, uColB, v.z) * (1.0 + (n - 0.5) * uVariation);
  vec3 c = mix(cellCol, uColC, crack);
  float dome = smoothstep(0.0, 0.35, v.w);
  float h = 0.5 + (dome * 0.6 + (n - 0.5) * 0.4 - 0.3) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, crack), uMetal.x, h, vec3(0));
}

Surf genLeather(vec2 uv) {
  vec2 f = F();
  vec4 v = pvoronoi(uv, f * 2.0, 0.85, uSeed);
  vec4 v2 = pvoronoi(uv, f * 5.0, 0.9, uSeed + 9.0);
  float cre = 1.0 - smoothstep(0.0, 0.08, v.w);
  float cre2 = 1.0 - smoothstep(0.0, 0.06, v2.w);
  float n = pfbm(uv, f, OCT(), uSeed + 3.0);
  float cr = max(cre, cre2 * 0.5);
  vec3 c = mix(uColA, uColB, n) * (1.0 - cr * 0.35);
  float h = 0.5 + (0.3 - cr * 0.8 + (n - 0.5) * 0.3) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, cr), uMetal.x, h, vec3(0));
}

Surf genFabric(vec2 uv) {
  vec2 f = F() * 8.0;
  vec2 p = uv * f;
  vec2 cell = floor(p);
  vec2 q = fract(p);
  float checker = mod(cell.x + cell.y, 2.0);
  float warp = sin(q.x * PI) * (checker > 0.5 ? 1.0 : 0.0) + sin(q.y * PI) * (checker > 0.5 ? 0.0 : 1.0);
  float thread = checker > 0.5 ? abs(sin(q.y * PI * 3.0)) : abs(sin(q.x * PI * 3.0));
  float n = pfbm(uv, F(), OCT(), uSeed);
  vec3 c = mix(uColA, uColB, checker * uExtra.x + (n - 0.5) * uVariation + thread * 0.1);
  float h = 0.5 + (warp * 0.5 + thread * 0.2 - 0.35) * uHeightAmp;
  return Surf(c, clamp(uRough.x + (1.0 - thread) * 0.05, 0.0, 1.0), uMetal.x, h, vec3(0));
}

Surf genTiles(vec2 uv) {
  vec2 f = max(vec2(1.0), floor(vec2(uScale, uScale * (uExtra.y > 0.0 ? uExtra.y : 1.0)) + 0.5));
  vec2 p = uv * f;
  float row = floor(p.y);
  if (uExtra.z > 0.5) p.x += mod(row, 2.0) * 0.5;  // brick offset
  vec2 cell = floor(p);
  vec2 q = fract(p);
  float mortar = uExtra.x > 0.0 ? uExtra.x : 0.05;
  vec2 e = min(q, 1.0 - q);
  float grout = 1.0 - smoothstep(mortar * 0.5, mortar, min(e.x, e.y));
  float id = hash12(mod(cell, f) + uSeed);
  float n = pfbm(uv, f * 2.0, OCT(), uSeed + 4.0);
  vec3 tileCol = mix(uColA, uColB, id) * (1.0 + (n - 0.5) * uVariation);
  vec3 c = mix(tileCol, uColC, grout);
  float bevel = smoothstep(mortar * 0.5, mortar * 2.0, min(e.x, e.y));
  float h = 0.5 + (bevel - 0.5) * uHeightAmp + (n - 0.5) * uHeightAmp * 0.2;
  return Surf(c, mix(uRough.x, uRough.y, grout), uMetal.x, h, vec3(0));
}

Surf genPlastic(vec2 uv) {
  float n = pfbm(uv, F() * 4.0, OCT(), uSeed);
  vec3 c = uColA * (1.0 + (n - 0.5) * uVariation);
  return Surf(c, clamp(uRough.x + (n - 0.5) * 0.08, 0.0, 1.0), uMetal.x, 0.5 + (n - 0.5) * uHeightAmp, vec3(0));
}

Surf genCarPaint(vec2 uv) {
  vec2 f = F() * 32.0;
  vec4 v = pvoronoi(uv, f, 1.0, uSeed);
  float flake = step(0.5, v.z) * (1.0 - smoothstep(0.1, 0.45, v.x));
  float n = pfbm(uv, F(), 3, uSeed + 2.0);
  vec3 c = mix(uColA, uColB, flake * 0.7) * (1.0 + (n - 0.5) * uVariation);
  return Surf(c, mix(uRough.x, uRough.y, flake), mix(uMetal.x, uMetal.y, flake), 0.5 + flake * uHeightAmp * 0.1, vec3(0));
}

Surf genDirt(vec2 uv) {
  vec2 f = F();
  float n = pfbm(uv, f, OCT(), uSeed);
  float clumps = pfbm(uv, f * 3.0, 4, uSeed + 7.0);
  vec4 v = pvoronoi(uv, f * 12.0, 1.0, uSeed + 1.0);
  float pebbles = (1.0 - smoothstep(0.15, 0.4, v.x)) * step(0.85, v.z);
  vec3 c = mix(uColA, uColB, smoothstep(0.3, 0.7, n + (clumps - 0.5) * 0.4));
  c = mix(c, uColC, pebbles);
  float h = 0.5 + ((n - 0.5) + (clumps - 0.5) * 0.5 + pebbles * 0.5) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, n), uMetal.x, h, vec3(0));
}

Surf genChippedPaint(vec2 uv) {
  vec2 f = F();
  float n = pfbm(uv, f, OCT(), uSeed);
  float chips = smoothstep(uExtra.x - 0.02, uExtra.x + 0.02, pfbm(uv, f * 2.0, 6, uSeed + 5.0) + (n - 0.5) * 0.2);
  float edgeRing = smoothstep(uExtra.x - 0.06, uExtra.x - 0.02, pfbm(uv, f * 2.0, 6, uSeed + 5.0) + (n - 0.5) * 0.2) - chips;
  Surf paint = Surf(uColA * (1.0 + (n - 0.5) * uVariation), uRough.x, uMetal.x, 0.5 + uHeightAmp * 0.5, vec3(0));
  Surf metal = Surf(uColB * (0.85 + n * 0.3), uRough.y, uMetal.y, 0.5 - uHeightAmp * 0.2, vec3(0));
  Surf s = mixSurf(paint, metal, chips);
  s.col = mix(s.col, uColC, clamp(edgeRing, 0.0, 1.0) * 0.6);
  return s;
}

Surf genCarbon(vec2 uv) {
  vec2 f = F() * 8.0;
  vec2 p = uv * f;
  vec2 cell = floor(p);
  vec2 q = fract(p);
  float diag = mod(cell.x + cell.y, 2.0);
  float weave = diag > 0.5 ? sin(q.x * PI) : sin(q.y * PI);
  float fiber = diag > 0.5 ? abs(sin(q.y * PI * 8.0)) : abs(sin(q.x * PI * 8.0));
  vec3 c = mix(uColA, uColB, weave * 0.6 + fiber * 0.15);
  float h = 0.5 + (weave - 0.5) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, fiber), uMetal.x, h, vec3(0));
}

Surf genMarble(vec2 uv) {
  vec2 f = F();
  float warp = pfbm(uv, f, OCT(), uSeed);
  float veins = pridged(uv + vec2(warp * 0.3), f * 2.0, 5, uSeed + 3.0);
  float v = smoothstep(0.55, 0.95, veins);
  vec3 c = mix(mix(uColA, uColB, warp * uVariation * 2.0), uColC, v);
  return Surf(c, mix(uRough.x, uRough.y, v), uMetal.x, 0.5 - v * uHeightAmp * 0.2, vec3(0));
}

Surf genHex(vec2 uv) {
  // hexagonal scales/plates, periodic on a 2 x sqrt3 compatible grid approximation
  vec2 f = F();
  vec4 v = pvoronoi(uv, f, 0.0, uSeed);  // jitter 0 -> regular grid (square cells offset)
  float edge = 1.0 - smoothstep(0.0, uExtra.x > 0.0 ? uExtra.x : 0.05, v.w);
  float dome = smoothstep(0.0, 0.5, v.w);
  vec3 c = mix(mix(uColA, uColB, v.z * uVariation), uColC, edge);
  return Surf(c, mix(uRough.x, uRough.y, edge), mix(uMetal.x, uMetal.y, edge), 0.5 + (dome - 0.5) * uHeightAmp, vec3(0));
}

Surf genEmissiveGrid(vec2 uv) {
  vec2 f = F();
  vec2 q = fract(uv * f);
  vec2 e = min(q, 1.0 - q);
  float line = 1.0 - smoothstep(0.0, uExtra.x > 0.0 ? uExtra.x : 0.03, min(e.x, e.y));
  float n = pfbm(uv, f, 3, uSeed);
  vec3 c = mix(uColA, uColB, line);
  return Surf(c, mix(uRough.x, uRough.y, line), uMetal.x, 0.5 + line * uHeightAmp * 0.2, uEmissive * line * (0.8 + 0.4 * n));
}

// ---- 19 diamond (tread) plate: alternating raised lozenges on brushed metal
Surf genDiamondPlate(vec2 uv) {
  vec2 f = F();
  vec2 p = uv * f * 2.0;
  vec2 cell = floor(p);
  vec2 q = fract(p) - 0.5;
  float flip = mod(cell.x + cell.y, 2.0) > 0.5 ? 1.0 : -1.0;
  vec2 r = vec2(q.x + flip * q.y, q.x - flip * q.y) * 0.7071;
  float d = length(vec2(r.x * 3.2, r.y));
  float w = uExtra.x > 0.0 ? uExtra.x : 0.42;
  float bump = 1.0 - smoothstep(w - 0.08, w, d);
  float n = pfbm(uv, f * 4.0, OCT(), uSeed);
  float streak = pnoise(uv * vec2(4.0, 128.0), vec2(4.0, 128.0), uSeed + 3.0);
  vec3 c = mix(uColA, uColB, bump) * (1.0 + (n - 0.5) * uVariation + (streak - 0.5) * 0.08);
  float h = 0.5 + (bump - 0.5) * uHeightAmp + (n - 0.5) * uHeightAmp * 0.05;
  return Surf(c, mix(uRough.x, uRough.y, bump) + (n - 0.5) * 0.06, uMetal.x, h, vec3(0));
}

// ---- 20 hammered metal: overlapping round dents
Surf genHammered(vec2 uv) {
  vec2 f = F();
  vec4 v = pvoronoi(uv, f, 0.85, uSeed);
  float dent = clamp(v.x * v.x * 2.2, 0.0, 1.0);  // 0 at dent center
  float n = pfbm(uv, f * 2.0, OCT(), uSeed + 2.0);
  vec3 c = uColA * (1.0 + (v.z - 0.5) * uVariation + (n - 0.5) * 0.1);
  c = mix(c, uColB, smoothstep(0.75, 1.0, dent) * 0.5);  // ridges catch a second tone
  float h = 0.5 + (dent - 0.5) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, dent), uMetal.x, h, vec3(0));
}

// ---- 21 corrugated sheet: sine ridges along U, grime/rust collecting in the valleys
Surf genCorrugated(vec2 uv) {
  vec2 f = F();
  float ridge = 0.5 + 0.5 * sin(uv.y * f.y * 2.0 * PI);
  float n = pfbm(uv, f * 0.5 + 1.0, OCT(), uSeed);
  float streak = pfbm(uv, vec2(f.x * 0.5 + 1.0, 32.0), 4, uSeed + 5.0);  // vertical run-off streaks
  float grime = clamp((1.0 - ridge) * uExtra.x + (streak - 0.5) * uExtra.y + (n - 0.5) * 0.5, 0.0, 1.0);
  grime = smoothstep(0.25, 0.75, grime);
  vec3 c = mix(uColA * (1.0 + (n - 0.5) * uVariation), uColB, grime);
  float h = 0.5 + (ridge - 0.5) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, grime), mix(uMetal.x, uMetal.y, grime), h, vec3(0));
}

// ---- 22 basket weave / wicker: groups of strands alternating over-under
Surf genWoven(vec2 uv) {
  vec2 f = F() * 2.0;
  vec2 p = uv * f;
  vec2 cell = floor(p);
  vec2 q = fract(p);
  bool horiz = mod(cell.x + cell.y, 2.0) < 0.5;
  float across = horiz ? q.y : q.x;   // across the strands
  float along = horiz ? q.x : q.y;
  float strands = 3.0;
  float s = fract(across * strands);
  float profile = sin(s * PI);
  float arch = 0.55 + 0.45 * sin(along * PI);  // each group dips under its neighbors at the ends
  float gap = 1.0 - smoothstep(0.0, 0.12, profile);
  float n = pfbm(uv, F() * 4.0, OCT(), uSeed);
  float fib = pnoise(uv * f * vec2(horiz ? 2.0 : 24.0, horiz ? 24.0 : 2.0), f * vec2(horiz ? 2.0 : 24.0, horiz ? 24.0 : 2.0), uSeed + 2.0);
  float id = hash12(mod(cell, f) * 1.7 + floor(across * strands) + uSeed);
  vec3 c = mix(uColA, uColB, (horiz ? 0.0 : uExtra.x) + (id - 0.5) * uVariation + (fib - 0.5) * 0.15);
  c = mix(c, uColC, gap * 0.85);
  float h = 0.5 + (profile * arch - 0.6) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, gap) + (n - 0.5) * 0.05, uMetal.x, h, vec3(0));
}

// ---- 23 tree bark: vertical plates separated by deep fissures, optional lichen
Surf genBark(vec2 uv) {
  vec2 f = F();
  vec2 warp = vec2(pfbm(uv, vec2(f.x, 2.0), 4, uSeed) - 0.5, 0.0) * 0.06;
  float fis = pridged(uv + warp, vec2(f.x * 2.0, max(1.0, f.y * 0.5)), 5, uSeed + 1.0);
  float fissure = smoothstep(0.55, 0.85, fis);
  float n = pfbm(uv, f * 2.0, OCT(), uSeed + 2.0);
  float lichen = smoothstep(0.62, 0.7, pfbm(uv, f + 1.0, 5, uSeed + 8.0)) * uExtra.x;
  vec3 c = mix(uColA * (1.0 + (n - 0.5) * uVariation), uColC, fissure);
  c = mix(c, uColB, lichen * (1.0 - fissure));
  float h = 0.5 + ((1.0 - fissure) * 0.7 + (n - 0.5) * 0.4 - 0.35) * uHeightAmp + lichen * uHeightAmp * 0.15;
  return Surf(c, mix(uRough.x, uRough.y, fissure), uMetal.x, h, vec3(0));
}

// ---- 24 terrazzo: stone chips set in a polished matrix
Surf genTerrazzo(vec2 uv) {
  vec2 f = F();
  vec4 a = pvoronoi(uv, f * 3.0, 1.0, uSeed);
  vec4 b = pvoronoi(uv, f * 7.0, 1.0, uSeed + 4.0);
  float ra = uExtra.x > 0.0 ? uExtra.x : 0.3;
  float chipA = (1.0 - smoothstep(ra - 0.03, ra, a.x)) * step(0.35, a.z);
  float chipB = (1.0 - smoothstep(ra * 0.8 - 0.03, ra * 0.8, b.x)) * step(0.55, b.z) * (1.0 - chipA);
  float n = pfbm(uv, f * 2.0, OCT(), uSeed + 2.0);
  vec3 colA2 = mix(uColB, uColC, step(0.65, a.z));
  vec3 colB2 = mix(uColC, uColB, step(0.8, b.z));
  vec3 c = uColA * (1.0 + (n - 0.5) * uVariation);
  c = mix(c, colA2, chipA);
  c = mix(c, colB2, chipB);
  float chip = max(chipA, chipB);
  return Surf(c, mix(uRough.x, uRough.y, chip), uMetal.x, 0.5 + (chip - 0.5) * uHeightAmp * 0.1, vec3(0));
}

// ---- 25 sand: wind ripples plus grain
Surf genSand(vec2 uv) {
  vec2 f = F();
  float warp = pfbm(uv, f * 0.5 + 1.0, 4, uSeed) - 0.5;
  float ph = fract(uv.y * f.y + warp * 1.5 + (pfbm(uv, vec2(2.0, 1.0), 3, uSeed + 3.0) - 0.5));
  float ripple = ph < 0.7 ? ph / 0.7 : (1.0 - ph) / 0.3;  // gentle windward, steep lee side
  ripple = smoothstep(0.0, 1.0, ripple) * (uExtra.x > 0.0 ? uExtra.x : 1.0);
  float grain = pnoise(uv * f * 24.0, f * 24.0, uSeed + 7.0);
  float n = pfbm(uv, f, OCT(), uSeed + 1.0);
  vec3 c = mix(uColA, uColB, clamp(ripple * 0.35 + (grain - 0.5) * 0.5 * uVariation + (n - 0.5) * 0.4, 0.0, 1.0));
  float spark = step(0.92, hash12(floor(uv * f * 96.0) + uSeed));
  c = mix(c, uColC, spark * 0.4);
  float h = 0.5 + (ripple - 0.5) * uHeightAmp + (grain - 0.5) * uHeightAmp * 0.15;
  return Surf(c, clamp(uRough.x + (grain - 0.5) * 0.05, 0.0, 1.0), uMetal.x, h, vec3(0));
}

// ---- 26 moss: fuzzy clumps over soil, coverage in uExtra.x
Surf genMoss(vec2 uv) {
  vec2 f = F();
  float cover = pfbm(uv, f, 5, uSeed);
  float c0 = uExtra.x > 0.0 ? uExtra.x : 0.45;
  float m = smoothstep(c0 - 0.08, c0 + 0.08, cover);
  float fuzz = pfbm(uv, f * 16.0, 3, uSeed + 2.0);
  float tufts = pvoronoi(uv, f * 10.0, 1.0, uSeed + 5.0).x;
  float bump = (1.0 - tufts) * 0.6 + fuzz * 0.4;
  vec3 moss = mix(uColA, uColB, clamp(bump * 1.2 - 0.2 + (fuzz - 0.5) * uVariation, 0.0, 1.0));
  float soilN = pfbm(uv, f * 4.0, OCT(), uSeed + 9.0);
  vec3 soil = uColC * (0.8 + soilN * 0.4);
  vec3 c = mix(soil, moss, m);
  float h = 0.5 + (m * (0.3 + bump * 0.7) - 0.4) * uHeightAmp + (soilN - 0.5) * uHeightAmp * 0.1;
  return Surf(c, mix(uRough.y, uRough.x, m), uMetal.x, h, vec3(0));
}

// ---- 27 asphalt: aggregate speckles, tar patches, cracks
Surf genAsphalt(vec2 uv) {
  vec2 f = F();
  vec4 v = pvoronoi(uv, f * 24.0, 1.0, uSeed);
  float stone = (1.0 - smoothstep(0.2, 0.42, v.x)) * step(0.45, v.z);
  float n = pfbm(uv, f, OCT(), uSeed + 1.0);
  float tar = smoothstep(0.6, 0.66, pfbm(uv, f * 0.5 + 1.0, 4, uSeed + 3.0));
  float cr = pridged(uv, f * 0.5 + 1.0, 4, uSeed + 6.0);
  float crack = smoothstep(1.0 - (uExtra.x > 0.0 ? uExtra.x : 0.08), 1.0, cr);
  vec3 c = uColA * (1.0 + (n - 0.5) * uVariation);
  c = mix(c, uColB * (0.8 + v.z * 0.4), stone * (1.0 - tar));
  c = mix(c, uColC, max(tar * 0.8, crack));
  float h = 0.5 + (stone * 0.4 - crack - 0.1 + (n - 0.5) * 0.3) * uHeightAmp;
  return Surf(c, mix(mix(uRough.x, uRough.x - 0.1, stone), uRough.y, tar), uMetal.x, h, vec3(0));
}

// ---- 28 camouflage: three layered blobs over a base color (fourth color = emissive slot unused)
Surf genCamo(vec2 uv) {
  vec2 f = F();
  float sharp = uExtra.y > 0.0 ? uExtra.y : 0.02;
  float l1 = smoothstep(0.5 - sharp, 0.5 + sharp, pfbm(uv, f, 5, uSeed));
  float l2 = smoothstep(0.56 - sharp, 0.56 + sharp, pfbm(uv, f, 5, uSeed + 13.0));
  float l3 = smoothstep(0.6 - sharp, 0.6 + sharp, pfbm(uv, f * 2.0, 4, uSeed + 29.0));
  vec3 base = uColA;
  vec3 c = mix(base, uColB, l1);
  c = mix(c, uColC, l2);
  c = mix(c, uColA * 0.45, l3 * uExtra.x);
  float n = pfbm(uv, f * 8.0, 3, uSeed + 2.0);
  c *= 1.0 + (n - 0.5) * uVariation;
  return Surf(c, uRough.x + (n - 0.5) * 0.05, uMetal.x, 0.5 + (n - 0.5) * uHeightAmp, vec3(0));
}

// ---- 29 perforated sheet: grid of round holes (staggered when extra.z > 0.5)
Surf genPerforated(vec2 uv) {
  vec2 f = F();
  vec2 p = uv * f;
  if (uExtra.z > 0.5) p.x += mod(floor(p.y), 2.0) * 0.5;
  vec2 q = fract(p) - 0.5;
  float r = uExtra.x > 0.0 ? uExtra.x : 0.3;
  float d = length(q);
  float hole = 1.0 - smoothstep(r - 0.015, r, d);
  float rim = smoothstep(r, r + 0.05, d) * (1.0 - smoothstep(r + 0.05, r + 0.12, d));
  float n = pfbm(uv, F() * 0.25 + 1.0, OCT(), uSeed);
  float streak = pnoise(uv * vec2(4.0, 128.0), vec2(4.0, 128.0), uSeed + 3.0);
  vec3 c = mix(uColA * (1.0 + (n - 0.5) * uVariation + (streak - 0.5) * 0.06), uColC, hole);
  c = mix(c, uColB, rim * 0.4);
  float h = 0.5 - hole * uHeightAmp + rim * uHeightAmp * 0.1;
  gHole = hole;
  return Surf(c, mix(uRough.x, uRough.y, hole), mix(uMetal.x, uMetal.y, hole), h, vec3(0));
}

// ---- 30 sci-fi panels: recursive panel subdivision with grooves and rivets
Surf genPanels(vec2 uv) {
  vec2 f = F();
  vec2 p = uv * f;
  vec2 cell = floor(p);
  vec2 q = fract(p);
  vec2 lo = vec2(0), hi = vec2(1);
  float id = hash12(mod(cell, f) + uSeed);
  // split each cell once or twice
  float h1 = hash12(mod(cell, f) * 1.31 + uSeed + 1.0);
  if (h1 < 0.4) { if (q.x < 0.5) hi.x = 0.5; else lo.x = 0.5; id += step(0.5, q.x) * 0.37; }
  else if (h1 < 0.8) { if (q.y < 0.5) hi.y = 0.5; else lo.y = 0.5; id += step(0.5, q.y) * 0.53; }
  float h2 = hash12(mod(cell, f) * 2.17 + uSeed + 3.0);
  if (h2 < 0.35) { float m = (lo.y + hi.y) * 0.5; if (q.y < m) hi.y = m; else lo.y = m; id += step(m, q.y) * 0.71; }
  vec2 e = min(q - lo, hi - q);
  float g = uExtra.x > 0.0 ? uExtra.x : 0.012;
  float groove = 1.0 - smoothstep(g * 0.5, g, min(e.x, e.y));
  float bevel = smoothstep(g, g * 4.0, min(e.x, e.y));
  // rivets near panel corners
  vec2 cq = min(q - lo, hi - q);
  float rivet = (1.0 - smoothstep(0.012, 0.02, length(cq - vec2(0.045)))) * step(0.5, uExtra.y);
  float n = pfbm(uv, f * 4.0, OCT(), uSeed + 5.0);
  vec3 c = mix(uColA, uColB, fract(id * 7.31) * uVariation * 2.0) * (1.0 + (n - 0.5) * 0.1);
  c = mix(c, uColC, groove);
  float h = 0.5 + (bevel - 0.5) * uHeightAmp + rivet * uHeightAmp * 0.6 + (fract(id * 3.7) - 0.5) * uHeightAmp * 0.3 * uExtra.z;
  return Surf(c, mix(uRough.x, uRough.y, groove) + (n - 0.5) * 0.05, uMetal.x, h, vec3(0));
}

// ================================================================ pattern, character and stylized generators (31..)

// noise in cell-local coordinates (planks, legs of a stitch): periodic only every 4096 units, so it never shows
float lnoise(vec2 p, float seed) { return pnoise(p, vec2(4096.0), seed); }
float lfbm(vec2 p, int oct, float seed) {
  float s = 0.0, a = 0.5, n = 0.0;
  for (int i = 0; i < oct; ++i) {
    s += a * lnoise(p, seed + float(i) * 3.1);
    n += a;
    p *= 2.0;
    a *= 0.5;
  }
  return s / n;
}
float tri(float x) { return 1.0 - abs(fract(x) * 2.0 - 1.0); }  // 0 at integers, 1 halfway

// herringbone of k:1 bricks (unit = brick width): motif = a horizontal brick at o and a vertical one at
// o + (k-1, 1), repeated on the lattice (1,-1), (k,k); the pattern repeats every 2k units on both axes
void herringboneCell(vec2 p, float k, out vec2 lo, out vec2 sz) {
  lo = floor(p);
  sz = vec2(1.0);
  float bf = floor((p.x + p.y) / (2.0 * k));
  for (int db = -1; db <= 1; ++db) {
    float b = bf + float(db);
    float a0 = ceil(p.x - k - b * k), a1 = floor(p.x - b * k);
    for (float a = a0; a <= a1; a += 1.0) {
      vec2 o = vec2(a + b * k, b * k - a);
      vec2 d = p - o;
      if (d.x >= 0.0 && d.x < k && d.y >= 0.0 && d.y < 1.0) { lo = o; sz = vec2(k, 1.0); return; }
      vec2 o2 = o + vec2(k - 1.0, 1.0);
      d = p - o2;
      if (d.x >= 0.0 && d.x < 1.0 && d.y >= 0.0 && d.y < k) { lo = o2; sz = vec2(1.0, k); return; }
    }
  }
}

// basket weave: k x k squares of k parallel bricks, alternating direction like a checkerboard
void basketCell(vec2 p, float k, out vec2 lo, out vec2 sz) {
  vec2 s = floor(p / k);
  vec2 l = p - s * k;
  if (mod(s.x + s.y, 2.0) < 0.5) { lo = vec2(s.x * k, s.y * k + floor(l.y)); sz = vec2(k, 1.0); }
  else { lo = vec2(s.x * k + floor(l.x), s.y * k); sz = vec2(1.0, k); }
}

// brick bonds in units of the brick height: 0 running bond, 1 stack bond, 2 herringbone, 3 basket weave.
// A = length : height, count = brick lengths across the tile. Returns the tile size in units (T), the texel
// position (p), its brick (lo, sz) and a key that is the same for the same brick on every repeat.
void bondLayout(vec2 uv, float mode, float A, float count, out vec2 T, out vec2 p, out vec2 lo, out vec2 sz,
                out vec2 key) {
  if (mode < 1.5) {
    float N = max(1.0, floor(count + 0.5));
    float R = max(2.0, 2.0 * floor(N * A * 0.5 + 0.5));  // even, so the half-brick offset wraps
    T = vec2(N * A, R);
    p = uv * T;
    float row = floor(p.y);
    float off = mode < 0.5 ? mod(row, 2.0) * A * 0.5 : 0.0;
    float col = floor((p.x - off) / A);
    lo = vec2(col * A + off, row);
    sz = vec2(A, 1.0);
    key = vec2(mod(col, N), mod(row, R));
  } else {
    float k = clamp(floor(A + 0.5), 2.0, 8.0);
    if (mode < 2.5) {
      T = vec2(2.0 * k * max(1.0, floor(count * 0.5 + 0.5)));
      p = uv * T;
      herringboneCell(p, k, lo, sz);
    } else {
      T = vec2(k * max(2.0, 2.0 * floor(count * 0.5 + 0.5)));
      p = uv * T;
      basketCell(p, k, lo, sz);
    }
    key = mod(floor((lo + sz * 0.5) * 2.0 + 0.5), T * 2.0);
  }
}

// ---- 31 bricks: running / stack bond, herringbone, basket weave; chipped arrises, recessed mortar
// extra: x joint width (brick heights, default 0.1), y bond (0 running, 1 stack, 2 herringbone, 3 basket),
//        z wear (chipped edges and pits), w brick length : height (default 3; herringbone / basket round it)
Surf genBricks(vec2 uv) {
  float A = uExtra.w > 0.0 ? uExtra.w : 3.0;
  vec2 T, p, lo, sz, key;
  bondLayout(uv, floor(uExtra.y + 0.5), A, uScale, T, p, lo, sz, key);
  float id = hash12(key + uSeed * 7.31);
  float id2 = hash12(key * 1.73 + uSeed * 3.1 + 5.0);
  vec2 q = p - lo;
  vec2 e2 = min(q, sz - q);
  float e = min(e2.x, e2.y);  // distance to the brick's edge, in brick heights
  float mw = (uExtra.x > 0.0 ? uExtra.x : 0.1) * 0.5;
  float wear = uExtra.z;
  vec2 fq = max(floor(T + 0.5), vec2(1.0));
  float chipN = pfbm(uv, fq * 3.0, 4, uSeed + 7.0);
  float ee = e - wear * 0.25 * smoothstep(0.45, 0.75, chipN) - (chipN - 0.5) * 0.05;
  float brick = smoothstep(mw, mw + 0.03, ee);
  float bevel = smoothstep(mw, mw + 0.14, ee);
  float n = pfbm(uv, fq * 0.25 + 1.0, OCT(), uSeed + 1.0);
  float grain = pfbm(uv, fq * 8.0, 3, uSeed + 2.0);
  float pit = smoothstep(0.7, 0.8, pnoise(uv * fq * 6.0, fq * 6.0, uSeed + 3.0)) * (0.3 + wear);
  vec3 bc = mix(uColA, uColB, smoothstep(0.1, 0.9, id));
  bc *= 1.0 + (n - 0.5) * uVariation * 2.0 + (grain - 0.5) * 0.22 + (id2 - 0.5) * uVariation;
  bc *= id2 > 0.9 ? 0.7 : 1.0;  // the odd over-fired brick
  bc *= 1.0 - pit * 0.35;
  float sand = pnoise(uv * fq * 12.0, fq * 12.0, uSeed + 5.0);
  vec3 mc = uColC * (0.82 + 0.36 * sand);
  vec3 c = mix(mc, bc, brick);
  float hb = 0.2 + bevel * 0.3 + (grain - 0.5) * 0.15 - pit * 0.2 + (id - 0.5) * 0.06;
  float hm = -0.35 + (sand - 0.5) * 0.1;
  float h = 0.5 + mix(hm, hb, brick) * uHeightAmp;
  float r = mix(uRough.y, uRough.x + (grain - 0.5) * 0.1 + pit * 0.1, brick);
  return Surf(c, r, mix(uMetal.y, uMetal.x, brick), h, vec3(0));
}

// ---- 32 parquet / wood planks: floorboards, herringbone or basket parquet, grain follows each plank
// extra: x gap width (plank widths, default 0.03), y layout (0 floorboards, 1 herringbone, 2 basket),
//        z plank length : width (default 5), w grain contrast (default 1)
Surf genParquet(vec2 uv) {
  float mode = floor(uExtra.y + 0.5);
  float k = uExtra.z > 0.0 ? uExtra.z : 5.0;
  float count = max(1.0, floor(uScale + 0.5));  // plank widths across the tile
  vec2 T, p, lo, sz, key;
  if (mode < 0.5) {
    T = vec2(count);
    p = uv * T;
    float row = floor(p.y);
    float rr = mod(row, count);
    float n = max(1.0, floor(count / k * (0.7 + 0.6 * hash12(vec2(rr, 3.1) + uSeed)) + 0.5));
    float L = count / n;
    float off = hash12(vec2(rr, 7.7) + uSeed) * L;
    float col = floor((p.x - off) / L);
    lo = vec2(col * L + off, row);
    sz = vec2(L, 1.0);
    key = vec2(mod(col, n), rr);
  } else {
    float kk = clamp(floor(k + 0.5), 2.0, 8.0);
    if (mode < 1.5) {
      T = vec2(2.0 * kk * max(1.0, floor(count / (2.0 * kk) + 0.5)));
      p = uv * T;
      herringboneCell(p, kk, lo, sz);
    } else {
      T = vec2(kk * max(2.0, 2.0 * floor(count / kk * 0.5 + 0.5)));
      p = uv * T;
      basketCell(p, kk, lo, sz);
    }
    key = mod(floor((lo + sz * 0.5) * 2.0 + 0.5), T * 2.0);
  }
  float id = hash12(key + uSeed * 5.17);
  float id2 = hash12(key * 2.31 + uSeed + 11.0);
  vec2 q = p - lo;
  bool alongX = sz.x >= sz.y;
  float along = alongX ? q.x : q.y;
  float across = alongX ? q.y : q.x;
  vec2 o = vec2(id * 173.0, id2 * 91.0);  // each plank is cut from another part of the log
  float warp = lfbm(vec2(along * 0.25, across * 0.5) + o, 3, uSeed) - 0.5;
  float rings = fract(across * (2.0 + id2 * 2.5) + warp * 0.9 + along * (id - 0.5) * 0.08);
  rings = smoothstep(0.0, 0.8, rings) * smoothstep(1.0, 0.9, rings);
  float fine = lnoise(vec2(along * 1.2, across * 70.0) + o, uSeed + 4.0);
  float fleck = lnoise(vec2(along * 6.0, across * 24.0) + o, uSeed + 8.0);
  float gc = uExtra.w > 0.0 ? uExtra.w : 1.0;
  float t = clamp(0.5 + (rings * 0.6 + (fine - 0.5) * 0.7 + (fleck - 0.5) * 0.2 - 0.3) * gc, 0.0, 1.0);
  vec3 c = mix(uColA, uColB, t) * (1.0 + (id - 0.5) * uVariation * 2.0);
  vec2 e2 = min(q, sz - q);
  float e = min(e2.x, e2.y);
  float gw = (uExtra.x > 0.0 ? uExtra.x : 0.03) * 0.5;
  float gap = 1.0 - smoothstep(gw, gw + 0.015, e);
  float bevel = smoothstep(gw, gw + 0.07, e);
  c = mix(c, uColC, gap);
  float h = 0.5 + (bevel * 0.35 - 0.3 + (t - 0.5) * 0.08 + (fine - 0.5) * 0.05 + (id2 - 0.5) * 0.05) * uHeightAmp;
  float r = mix(mix(uRough.x, uRough.y, t * 0.6), uRough.y, gap);
  return Surf(c, r, uMetal.x, h, vec3(0));
}

// ---- 33 hexagonal tiles (true hexagons; scale = tiles across, rows fitted so the tile repeats)
// extra: x grout width (default 0.05), y bevel (default 0.1), z colors (0 random A/B, 1 every third tile B:
//        scale must be a multiple of 3), w > 0.5 raised walls / sunken cells (honeycomb)
Surf genHexTiles(vec2 uv) {
  const float S3 = 1.7320508;
  float N = max(1.0, floor(uScale + 0.5));
  float M = max(1.0, floor(N / S3 + 0.5));
  vec2 T = vec2(N, M * S3);
  vec2 p = uv * T;
  vec2 g = vec2(1.0, S3);
  vec2 a = (floor(p / g) + 0.5) * g;
  vec2 b = (floor((p - g * 0.5) / g) + 0.5) * g + g * 0.5;
  vec2 c = dot(p - a, p - a) < dot(p - b, p - b) ? a : b;
  vec2 d = p - c;
  float hd = max(abs(d.x), max(abs(dot(d, vec2(0.5, 0.8660254))), abs(dot(d, vec2(0.5, -0.8660254)))));
  float e = 0.5 - hd;  // distance to the tile's edge
  float x2 = floor(c.x * 2.0 + 0.5), r = floor(c.y / 0.8660254 + 0.5);
  vec2 key = mod(vec2(x2, r), vec2(2.0 * N, 2.0 * M));
  float id = hash12(key + uSeed * 3.7);
  float gw = (uExtra.x > 0.0 ? uExtra.x : 0.05) * 0.5;
  float bw = uExtra.y > 0.0 ? uExtra.y : 0.1;
  float grout = 1.0 - smoothstep(gw, gw + 0.015, e);
  float bevel = smoothstep(gw, gw + bw, e);
  float n = pfbm(uv, vec2(N) * 2.0, OCT(), uSeed + 2.0);
  float pick;
  if (uExtra.z > 0.5) pick = mod((x2 - r) * 0.5 - r, 3.0) < 0.5 ? 1.0 : 0.0;
  else pick = id;
  vec3 tc = mix(uColA, uColB, pick) * (1.0 + (n - 0.5) * uVariation + (id - 0.5) * uVariation * 0.5);
  vec3 col = mix(tc, uColC, grout);
  float h;
  if (uExtra.w > 0.5) h = 0.5 + (grout * 0.9 - 0.4 + (1.0 - bevel) * 0.3 * (1.0 - grout) + (n - 0.5) * 0.1) * uHeightAmp;
  else h = 0.5 + (bevel * 0.45 - 0.1 - grout * 0.3 + (n - 0.5) * 0.1) * uHeightAmp;
  return Surf(col, mix(uRough.x, uRough.y, grout) + (n - 0.5) * 0.05, mix(uMetal.x, uMetal.y, grout), h, vec3(0));
}

// ---- 34 overlapping scales: fish (round) to dragon (pointed), each row lying over the one below
// extra: x pointiness (0 round .. 1 pointed), y rim highlight, z central keel ridge, w overlap (default 0.62)
Surf genScales(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  vec2 p = uv * N;  // columns 1 apart, rows 0.5 apart (2N rows)
  float R = uExtra.w > 0.0 ? uExtra.w : 0.62;
  float o = clamp(uExtra.x, 0.0, 1.0) * 0.6;
  float j0 = floor(p.y / 0.5);
  float found = 0.0, mVis = 2.0, mUp = 9.0;
  vec2 dVis = vec2(0), kVis = vec2(0);
  for (int dj = 3; dj >= -2; --dj) {  // rows from the top down: higher rows lie on top
    float j = j0 + float(dj);
    float off = mod(j, 2.0) * 0.5;
    float i0 = floor(p.x - off + 0.5);
    float mRow = 9.0;
    vec2 dRow = vec2(0), kRow = vec2(0);
    for (int di = -1; di <= 1; ++di) {
      float i = i0 + float(di);
      vec2 d = (p - vec2(i + off, j * 0.5)) / R;
      float m = max(length(d - vec2(o, 0.0)), length(d + vec2(o, 0.0))) / (1.0 + o);
      if (m < mRow) { mRow = m; dRow = d; kRow = vec2(mod(i, N), mod(j, 2.0 * N)); }
    }
    if (mRow < 1.0 && dRow.y < 0.55) { mVis = mRow; dVis = dRow; kVis = kRow; found = 1.0; break; }
    mUp = min(mUp, mRow);
  }
  float id = hash12(kVis + uSeed * 4.1);
  float n = pfbm(uv, vec2(N) * 6.0, 3, uSeed + 2.0);
  float shadow = (1.0 - smoothstep(1.0, 1.3, mUp)) * found;
  float rim = smoothstep(0.78, 0.97, mVis) * smoothstep(0.2, -0.3, dVis.y);
  float keel = (1.0 - smoothstep(0.0, 0.1, abs(dVis.x))) * smoothstep(0.5, -0.2, dVis.y) * uExtra.z;
  float dome = sqrt(max(0.0, 1.0 - mVis * mVis));
  vec3 c = mix(uColA, uColB, smoothstep(0.35, 1.0, mVis) * 0.7) * (1.0 + (id - 0.5) * uVariation + (n - 0.5) * 0.1);
  c = mix(c, uColB * 1.15, clamp(rim * uExtra.y + keel * 0.5, 0.0, 1.0));
  c = mix(c, uColC, shadow * 0.8);
  float h = 0.5 + (dome * 0.35 + (-dVis.y) * 0.2 + keel * 0.25 + (n - 0.5) * 0.06 - shadow * 0.15) * uHeightAmp;
  float r = mix(uRough.x, uRough.y, max(shadow, smoothstep(0.5, 1.0, mVis) * 0.5));
  return Surf(c, r, mix(uMetal.x, uMetal.y, shadow), h, vec3(0));
}

// ---- 35 metal grille: expanded metal, chain-link fence or woven wire mesh; holes take color C
// extra: x strand / wire width (fraction of a cell, default 0.16), y type (0 expanded metal, 1 chain-link,
//        2 woven square mesh), z strand tilt / twist shading (expanded metal, default 1)
Surf genGrille(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float mode = floor(uExtra.y + 0.5);
  float w = uExtra.x > 0.0 ? uExtra.x : 0.16;
  float n = pfbm(uv, vec2(N) * 2.0, OCT(), uSeed);
  float wire = 0.0, hw = 0.0, shade = 1.0;
  if (mode < 0.5) {
    vec2 s = uv * vec2(N, 2.0 * N);
    float a = s.x + s.y, b = s.x - s.y;
    float ea = abs(fract(a + 0.5) - 0.5), eb = abs(fract(b + 0.5) - 0.5);
    float tilt = uExtra.z > 0.0 ? uExtra.z : 1.0;
    bool fa = ea < eb;
    float e = min(ea, eb);
    float sgn = fa ? (fract(a + 0.5) - 0.5) : (fract(b + 0.5) - 0.5);
    float across = clamp(sgn / w, -1.0, 1.0);
    wire = 1.0 - smoothstep(w - 0.02, w, e);
    float node = 1.0 - smoothstep(0.0, w * 1.5, max(ea, eb));
    hw = 0.6 + across * 0.3 * tilt * (fa ? 1.0 : -1.0) * (1.0 - node) + node * 0.15;
    shade = 1.0 + across * 0.25 * tilt * (fa ? 1.0 : -1.0) * (1.0 - node);
  } else if (mode < 1.5) {
    float N2 = max(2.0, 2.0 * floor(N * 0.5 + 0.5));
    vec2 s = uv * N2;
    float a = s.x + s.y, b = s.x - s.y;
    float ea = abs(fract(a + 0.5) - 0.5), eb = abs(fract(b + 0.5) - 0.5);
    float pa = sqrt(max(0.0, 1.0 - (ea / w) * (ea / w)));
    float pb = sqrt(max(0.0, 1.0 - (eb / w) * (eb / w)));
    float ha = pa * (0.55 + 0.35 * cos(PI * b)), hb = pb * (0.55 - 0.35 * cos(PI * a));
    hw = max(ha, hb);
    wire = max(1.0 - smoothstep(w - 0.02, w, ea), 1.0 - smoothstep(w - 0.02, w, eb));
    shade = 0.75 + 0.35 * (ha > hb ? pa : pb);
  } else {
    float N2 = max(2.0, 2.0 * floor(N * 0.5 + 0.5));
    vec2 s = uv * N2;
    float ex = abs(fract(s.y + 0.5) - 0.5), ey = abs(fract(s.x + 0.5) - 0.5);  // to horizontal / vertical wires
    float jx = floor(s.y + 0.5), iy = floor(s.x + 0.5);
    float px = sqrt(max(0.0, 1.0 - (ex / w) * (ex / w)));
    float py = sqrt(max(0.0, 1.0 - (ey / w) * (ey / w)));
    float hx = px * (0.55 + 0.35 * cos(PI * (s.x + jx)));
    float hy = py * (0.55 - 0.35 * cos(PI * (s.y + iy)));
    hw = max(hx, hy);
    wire = max(1.0 - smoothstep(w - 0.02, w, ex), 1.0 - smoothstep(w - 0.02, w, ey));
    shade = 0.75 + 0.35 * (hx > hy ? px : py);
  }
  vec3 wc = uColA * shade * (1.0 + (n - 0.5) * uVariation);
  wc = mix(wc, uColB, smoothstep(0.55, 0.8, n) * 0.6);  // grime / oxide patches
  vec3 c = mix(uColC, wc, wire);
  float h = 0.5 + (mix(-0.5, hw - 0.3, wire)) * uHeightAmp;
  float r = mix(uRough.y, uRough.x + (n - 0.5) * 0.1, wire);
  gHole = 1.0 - wire;
  return Surf(c, r, mix(uMetal.y, uMetal.x, wire), h, vec3(0));
}

// ---- 36 quilted / tufted upholstery: puffed cushions between stitched seams
// extra: x stitches (0 none .. 1), y buttons at the seam crossings (> 0.5), z layout (0 diamond, 1 square,
//        2 horizontal channels: puffer jacket), w leather grain (0 smooth fabric .. 1 leather)
Surf genQuilted(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float mode = floor(uExtra.z + 0.5);
  vec2 r = uv * N;
  vec2 s = mode < 0.5 ? vec2(r.x + r.y, r.x - r.y) : r;
  vec2 l = fract(s) - 0.5;
  float e, t;
  if (mode > 1.5) { e = 0.5 - abs(l.y); t = s.x * 0.5; }
  else {
    e = 0.5 - max(abs(l.x), abs(l.y));
    t = abs(l.x) > abs(l.y) ? l.y : l.x;
  }
  // pillow: product of two rounded 1D profiles (no ridges along the diagonals)
  vec2 pl = clamp(1.0 - pow(abs(l) * 2.0, vec2(2.5)), 0.0, 1.0);
  float puff = mode > 1.5 ? sqrt(pl.y) : sqrt(pl.x * pl.y);
  float cd = length(abs(l) - 0.5);  // distance to the nearest seam crossing
  float button = 0.0;
  if (uExtra.y > 0.5 && mode < 1.5) {
    puff *= smoothstep(0.02, 0.3, cd);
    button = 1.0 - smoothstep(0.045, 0.06, cd);
  }
  float seam = 1.0 - smoothstep(0.0, 0.035, e);
  float stitch = (1.0 - smoothstep(0.008, 0.016, e)) * step(fract(t * 10.0 + 0.25), 0.55) * clamp(uExtra.x, 0.0, 1.0);
  stitch *= 1.0 - button;
  vec4 v = pvoronoi(uv, vec2(N) * 14.0, 0.9, uSeed + 3.0);
  float crease = (1.0 - smoothstep(0.0, 0.07, v.w)) * uExtra.w;
  float n = pfbm(uv, vec2(N) * 2.0, OCT(), uSeed);
  float fine = pnoise(uv * N * 48.0, vec2(N * 48.0), uSeed + 5.0);
  vec3 c = uColA * (0.8 + 0.2 * puff) * (1.0 + (n - 0.5) * uVariation + (fine - 0.5) * 0.04) * (1.0 - crease * 0.2);
  c = mix(c, uColC, seam * 0.7);
  c = mix(c, uColB, stitch);
  c = mix(c, uColA * 0.6 * (0.8 + 0.4 * smoothstep(0.06, 0.0, cd)), button);
  float h = 0.5 + (puff * 0.8 - 0.4 - crease * 0.06 + stitch * 0.08 + button * 0.35 + (fine - 0.5) * 0.02) * uHeightAmp;
  float rr = mix(uRough.x, uRough.y, max(seam, crease * 0.5)) + (n - 0.5) * 0.06;
  rr = mix(rr, 0.5, stitch);
  return Surf(c, rr, mix(uMetal.x, 0.0, max(stitch, seam)), h, vec3(0));
}

// ---- 37 knitted wool: columns of V stitches (stockinette), plied yarn, fuzz
// extra: x ribbing (purl columns every n, 0 none), y stripes per tile (color B, 0 none),
//        z fuzz (default 1), w > 0.5 seed-stitch texture (alternate rows show purl bumps)
Surf genKnit(vec2 uv) {
  float N = max(2.0, floor(uScale + 0.5));
  float rib = floor(uExtra.x + 0.5);
  if (rib > 0.5) N = max(2.0 * rib, floor(N / (2.0 * rib) + 0.5) * 2.0 * rib);
  float R = max(2.0, floor(N * 1.3 + 0.5));
  vec2 p = uv * vec2(N, R);
  vec2 cell = floor(p);
  float best = 0.0, bAlong = 0.0, bAcross = 0.0, bRow = 0.0, bCol = 0.0;
  for (int dy = -1; dy <= 1; ++dy) {
    vec2 cc = cell + vec2(0.0, float(dy));
    vec2 l = p - cc - vec2(0.5, 0.0);
    for (int side = 0; side < 2; ++side) {
      float sg = side == 0 ? -1.0 : 1.0;
      vec2 a = vec2(0.0, 0.0), b = vec2(sg * 0.36, 1.2);
      vec2 ab = b - a;
      float t = clamp(dot(l - a, ab) / dot(ab, ab), 0.0, 1.0);
      vec2 dv = l - a - ab * t;
      float rad = 0.2 * (0.8 + 0.25 * sin(t * PI));
      float d = length(dv);
      float prof = sqrt(max(0.0, 1.0 - (d / rad) * (d / rad)));
      float hgt = prof * (0.6 + 0.4 * sin(clamp(t * 1.15, 0.0, 1.0) * PI));  // dips where it passes under the next row
      if (hgt > best) {
        best = hgt;
        bAlong = t;
        bAcross = (dv.x * ab.y - dv.y * ab.x) / length(ab) / rad;
        bRow = cc.y;
        bCol = cc.x;
      }
    }
  }
  float ply = 0.5 + 0.5 * sin((bAlong * 7.0 + bAcross * 1.2) * 2.0 * PI);
  float fz = pfbm(uv, vec2(N, R) * 6.0, 3, uSeed + 1.0);
  float fuzz = uExtra.z > 0.0 ? uExtra.z : 1.0;
  float purl = 0.0;
  if (rib > 0.5) purl = step(rib, mod(bCol, 2.0 * rib));
  if (uExtra.w > 0.5) purl = mod(bCol + bRow, 2.0);
  float st = 0.0;
  float S = 2.0 * floor(uExtra.y * 0.5 + 0.5);
  if (S > 0.5) st = mod(floor(mod(bRow, R) * S / R), 2.0);
  float n = pfbm(uv, vec2(N) * 0.5 + 1.0, OCT(), uSeed + 4.0);
  vec3 yarn = mix(uColA, uColB, st) * (1.0 + (n - 0.5) * uVariation);
  float lit = best * (0.8 + 0.2 * ply) * (1.0 - purl * 0.25);
  vec3 c = mix(uColC, yarn * (0.75 + 0.3 * ply), smoothstep(0.0, 0.35, best));
  c *= 1.0 + (fz - 0.5) * 0.25 * fuzz;
  float h = 0.5 + (lit * 0.8 - 0.4 - purl * 0.2 + (fz - 0.5) * 0.15 * fuzz) * uHeightAmp;
  float r = mix(uRough.y, uRough.x, smoothstep(0.0, 0.4, best));
  return Surf(c, r, uMetal.x, h, vec3(0));
}

// ---- 38 knurling and grips: diamond / straight / square knurl or rubber dots, worn crests, dirty grooves
// extra: x type (0 diamond knurl, 1 straight, 2 rubber dots, 3 square), y wear (bright crests, color B),
//        z flat tops (0 sharp pyramids .. 0.9 checkering), w groove dirt (color C, default 0.5)
Surf genKnurling(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float mode = floor(uExtra.x + 0.5);
  float hgt;
  vec2 s = uv * N;
  if (mode < 0.5) hgt = min(tri(s.x + s.y), tri(s.x - s.y));
  else if (mode < 1.5) hgt = tri(s.x);
  else if (mode < 2.5) {
    vec2 a = fract(s) - 0.5, b = fract(s + 0.5) - 0.5;
    float d = min(length(a), length(b));
    hgt = sqrt(max(0.0, 1.0 - (d / 0.3) * (d / 0.3)));
  } else hgt = min(tri(s.x), tri(s.y));
  float fl = clamp(uExtra.z, 0.0, 0.95);
  if (mode != 2.0) hgt = min(hgt, 1.0 - fl) / (1.0 - fl);
  float n = pfbm(uv, vec2(N) * 0.25 + 1.0, OCT(), uSeed);
  float crest = smoothstep(0.75, 1.0, hgt) * smoothstep(0.35, 0.65, n + uExtra.y * 0.4);
  float groove = 1.0 - smoothstep(0.0, 0.35, hgt);
  float dirt = (uExtra.w > 0.0 ? uExtra.w : 0.5);
  vec3 c = uColA * (0.85 + 0.25 * hgt) * (1.0 + (n - 0.5) * uVariation);
  c = mix(c, uColB, crest * clamp(uExtra.y, 0.0, 1.0));
  c = mix(c, uColC, groove * dirt);
  float h = 0.5 + (hgt - 0.5) * uHeightAmp;
  float r = mix(uRough.x, uRough.y, crest * clamp(uExtra.y, 0.0, 1.0));
  r = mix(r, min(1.0, r + 0.25), groove * dirt);
  return Surf(c, r, mix(uMetal.x, uMetal.y, groove * dirt), h, vec3(0));
}

// ---- 39 stucco / plaster: knockdown splats flattened by the trowel over a sandy base coat
// extra: x splat coverage (0..1, default 0.5), y trowel flattening (0 round blobs .. 1 flat tops),
//        z sand grain, w orange-peel bumps
Surf genStucco(vec2 uv) {
  vec2 f = F();
  float cover = uExtra.x > 0.0 ? uExtra.x : 0.5;
  float n = pfbm(uv, f, min(OCT(), 4), uSeed);
  float n2 = pfbm(uv, f * 3.0, 3, uSeed + 3.0);
  float blob = n * 0.7 + n2 * 0.3;
  float thr = 0.5 + (0.5 - cover) * 0.25;
  float splat = smoothstep(thr - 0.008, thr + 0.016, blob);
  float dome = clamp((blob - thr) * 12.0, 0.0, 1.0);
  float top = mix(dome, 0.55 + (n2 - 0.5) * 0.3, clamp(uExtra.y, 0.0, 1.0)) * splat;
  float grain = pnoise(uv * f * 40.0, f * 40.0, uSeed + 7.0);
  float peel = pfbm(uv, f * 10.0, 3, uSeed + 9.0);
  float base = (grain - 0.5) * 0.2 * uExtra.z + (peel - 0.5) * 0.5 * uExtra.w;
  vec3 c = mix(uColA, uColB, splat * 0.5) * (1.0 + (n2 - 0.5) * uVariation);
  c = mix(c, uColC, (1.0 - splat) * smoothstep(0.65, 0.2, blob / thr) * 0.5);  // dirt settles low
  c *= 1.0 + (grain - 0.5) * 0.08 * uExtra.z;
  float h = 0.5 + (top * 0.7 - 0.25 + base * (1.0 - splat * 0.7)) * uHeightAmp;
  float r = mix(uRough.x, uRough.y, splat * clamp(uExtra.y, 0.0, 1.0)) + (grain - 0.5) * 0.05;
  return Surf(c, r, uMetal.x, h, vec3(0));
}

// tartan sett: bands of A, B and C across one repeat
vec3 settColor(float x) {
  if (x < 0.28) return uColA;
  if (x < 0.32) return uColC;
  if (x < 0.40) return uColA;
  if (x < 0.62) return uColB;
  if (x < 0.70) return uColA;
  if (x < 0.73) return uColC;
  if (x < 0.76) return uColA;
  if (x < 0.79) return uColC;
  return uColA;
}

// ---- 40 twill and satin cloth: denim, gabardine, satin, tartan; optional knife pleats
// extra: x weave (0 2/1 twill, 1 3/1 twill, 2 satin), y tartan repeats per tile (0 = plain: warp A, weft B),
//        z pleats per tile (0 none), w sheen (bright, smooth thread crests)
Surf genTwill(vec2 uv) {
  float weave = floor(uExtra.x + 0.5);
  float rep = weave < 0.5 ? 3.0 : weave < 1.5 ? 4.0 : 5.0;
  float T = max(rep * 2.0, floor(max(1.0, uScale) * 8.0 / rep + 0.5) * rep);
  vec2 p = uv * T;
  vec2 cell = floor(p);
  vec2 q = fract(p);
  float ph = weave < 1.5 ? mod(cell.x + cell.y, rep) : mod(cell.x * 2.0 + cell.y, 5.0) - 1.0;
  bool warpUp = weave < 1.5 ? ph < rep - 1.5 : ph > -0.5;
  float along, across;
  if (warpUp) { along = (ph + q.y) / (rep - 1.0); across = q.x; }
  else { along = q.x; across = q.y; }
  float prof = sin(PI * across);
  float arch = sin(PI * along);
  float th = warpUp ? cell.x : cell.y;
  float tid = hash12(vec2(mod(th, T), warpUp ? 1.0 : 2.0) + uSeed);
  float slub = lnoise(vec2((warpUp ? p.y : p.x) * 0.15, th * 3.7), uSeed + 6.0);
  vec3 tc;
  if (uExtra.y > 0.0) {
    float reps = max(1.0, floor(uExtra.y + 0.5));
    tc = settColor(fract((mod(th, T) + 0.5) / T * reps));
  } else tc = warpUp ? uColA : uColB;
  tc *= 1.0 + (tid - 0.5) * uVariation + (slub - 0.5) * uVariation * 0.8;
  float n = pfbm(uv, F() * 0.5 + 1.0, OCT(), uSeed + 2.0);
  tc *= 1.0 + (n - 0.5) * uVariation;
  float sheen = clamp(uExtra.w, 0.0, 1.0);
  vec3 c = tc * (0.72 + 0.28 * prof * (0.6 + 0.4 * arch)) * (1.0 + sheen * 0.25 * prof * arch);
  float hp = 0.0;
  float P = floor(uExtra.z + 0.5);
  if (P > 0.5) {  // knife pleats: slow rise, sharp fold; shading painted into the color
    float x = fract(uv.x * P);
    hp = smoothstep(0.0, 0.85, x) * (1.0 - smoothstep(0.9, 1.0, x));
    c *= 0.72 + 0.38 * hp - 0.15 * (1.0 - smoothstep(0.0, 0.08, x));
  }
  float h = 0.5 + (prof * (0.5 + 0.5 * arch) * 0.25 - 0.12) * uHeightAmp + (hp - 0.5) * uHeightAmp * 1.5 * step(0.5, P);
  float r = mix(uRough.y, uRough.x, prof * arch * (0.5 + 0.5 * sheen));
  return Surf(c, r, uMetal.x, h, vec3(0));
}

// ---- 41 skin: pores, fine creases, redness and freckles (smooth and stylized with them turned off)
// extra: x pores (0..1), y freckles (0..1, color C), z fine creases (0..1), w redness blotches (0..1, color B)
Surf genSkin(vec2 uv) {
  vec2 f = F();
  float n = pfbm(uv, f, OCT(), uSeed);
  float red = pfbm(uv, f, 4, uSeed + 3.0);
  vec4 pv = pvoronoi(uv, f * 48.0, 0.9, uSeed + 5.0);
  float pore = (1.0 - smoothstep(0.0, 0.2, pv.x)) * (0.5 + 0.5 * pv.z) * uExtra.x;
  pore *= 0.6 + 0.8 * smoothstep(0.3, 0.7, pfbm(uv, f * 3.0, 3, uSeed + 15.0));  // denser in places
  vec2 r = vec2(uv.x + uv.y, uv.x - uv.y);
  float c1 = pnoise(r * vec2(f.x * 3.0, f.y * 40.0), vec2(f.x * 3.0, f.y * 40.0), uSeed + 7.0);
  float c2 = pnoise(r * vec2(f.x * 40.0, f.y * 3.0), vec2(f.x * 40.0, f.y * 3.0), uSeed + 8.0);
  float cr = max(smoothstep(0.86, 0.97, 1.0 - abs(c1 * 2.0 - 1.0)), smoothstep(0.88, 0.98, 1.0 - abs(c2 * 2.0 - 1.0)));
  cr *= uExtra.z * smoothstep(0.35, 0.6, pfbm(uv, f * 4.0, 3, uSeed + 9.0));
  vec4 fv = pvoronoi(uv, f * 10.0, 1.0, uSeed + 11.0);
  float clus = smoothstep(0.4, 0.7, pfbm(uv, f * 2.0, 3, uSeed + 13.0));
  float freck = (1.0 - smoothstep(0.08, 0.3, fv.x)) * step(1.0 - clamp(uExtra.y, 0.0, 1.0) * 0.8, fv.z) * clus;
  vec3 c = uColA * (1.0 + (n - 0.5) * uVariation);
  c = mix(c, uColB, smoothstep(0.3, 0.8, red) * clamp(uExtra.w, 0.0, 1.0) * 0.6);
  c = mix(c, uColC, freck * (0.45 + 0.3 * fv.z));
  c *= 1.0 - pore * 0.07 - cr * 0.05;
  float h = 0.5 + ((n - 0.5) * 0.15 + (red - 0.5) * 0.05 - pore * 0.45 - cr * 0.35) * uHeightAmp;
  float rough = uRough.x + (red - 0.5) * 0.1 + (n - 0.5) * 0.08;
  rough = mix(rough, uRough.y, max(pore, cr));
  return Surf(c, rough, uMetal.x, h, vec3(0));
}

// ---- 42 hair: strands and locks running along V, optional waves and a painted highlight band
// extra: x lock separation (dark gaps, color C), y highlight band strength (color B), z cel (0 soft .. 1
//        hard-edged anime highlight and banding), w waves per tile
Surf genHair(vec2 uv) {
  vec2 f = F();
  float waves = floor(uExtra.w + 0.5);
  float wv = waves > 0.5 ? sin(uv.y * 2.0 * PI * waves) * 0.1 / f.x : 0.0;
  vec2 p = vec2(uv.x + wv + (pnoise(uv * vec2(f.x, 1.0), vec2(f.x, 1.0), uSeed + 9.0) - 0.5) * 0.4 / f.x, uv.y);
  float cel = clamp(uExtra.z, 0.0, 1.0);
  // every layer is long along V (one or two periods per tile) so strands stay straight, not lens-shaped like wood
  float lock = pnoise(p * vec2(f.x, 1.0), vec2(f.x, 1.0), uSeed) * 0.7 + pnoise(p * vec2(f.x * 3.0, 1.0), vec2(f.x * 3.0, 1.0), uSeed + 1.0) * 0.3;
  float fine = pnoise(p * vec2(f.x * 12.0, 1.0), vec2(f.x * 12.0, 1.0), uSeed + 2.0) * 0.6 +
               pnoise(p * vec2(f.x * 32.0, 2.0), vec2(f.x * 32.0, 2.0), uSeed + 3.0) * 0.4;
  float hair = pnoise(p * vec2(f.x * 40.0, 2.0), vec2(f.x * 40.0, 2.0), uSeed + 4.0) * 0.55 +
               pnoise(p * vec2(f.x * 100.0, 3.0), vec2(f.x * 100.0, 3.0), uSeed + 6.0) * 0.45;
  // clumps (locks): rounded bundles, bright in the middle and dark where they part
  float cw = f.x * 3.0;
  float cx = p.x * cw + (pnoise(p * vec2(cw, 1.0), vec2(cw, 1.0), uSeed + 7.0) - 0.5) * 0.6;
  float clump = pow(max(sin(PI * fract(cx)), 0.0), 0.6);
  float gap = (1.0 - smoothstep(0.0, 0.35, clump)) * clamp(uExtra.x, 0.0, 1.0);
  float t = clamp(0.5 + (clump - 0.65) * 0.5 + (lock - 0.5) * mix(0.4, 0.7, cel) + (fine - 0.5) * (0.6 + cel * 0.8) +
                      (hair - 0.5) * 1.2 * (1.0 - cel), 0.0, 1.0);
  t = mix(t, smoothstep(0.42, 0.58, t), cel);  // anime: two clean tones
  vec3 c = mix(mix(uColC, uColA, 0.35), uColA, smoothstep(0.2, 0.6, t));
  c = mix(c, mix(uColA, uColB, 0.35), smoothstep(0.65, 0.95, t) * (1.0 - cel * 0.6));
  // highlight band: soft and frayed, or a crisp zig-zag ring (cel)
  float teeth = tri(p.x * f.x * 12.0);
  float dy = abs(fract(uv.y - 0.32 + (fine - 0.5) * 0.16 * (1.0 - cel * 0.7) + (teeth - 0.5) * 0.035 * cel) - 0.5);
  float band = mix(1.0 - smoothstep(0.015, 0.07, dy), 1.0 - smoothstep(0.024, 0.03, dy), cel);
  band *= clamp(uExtra.y, 0.0, 1.0) * mix(smoothstep(0.3, 0.6, t), 1.0, cel);
  c = mix(c, uColB, band);
  c = mix(c, uColC, gap * 0.8);
  c *= 1.0 + (hair - 0.5) * uVariation * (1.0 - cel);
  float h = 0.5 + ((fine - 0.5) * 0.3 + (hair - 0.5) * 0.7 * (1.0 - cel) + (clump - 0.6) * 0.5 - gap * 0.2) * uHeightAmp;
  float r = mix(uRough.x, uRough.y, max(gap, (1.0 - t) * 0.6));
  return Surf(c, r, uMetal.x, h, vec3(0));
}

// ---- 43 brush strokes: painterly color in three tones (A base, B shade, C light) for toon / hand-painted looks
// extra: x softness (0 hard cel bands .. 1 soft gradients), y stroke direction (0 horizontal, 1 diagonal,
//        2 vertical, 3 other diagonal), z stroke length (default 3), w bristle streaks
Surf genBrushStrokes(vec2 uv) {
  vec2 f = F();
  float dir = mod(floor(uExtra.y + 0.5), 4.0);
  vec2 r = dir < 0.5 ? uv : dir < 1.5 ? vec2(uv.x + uv.y, uv.y - uv.x) : dir < 2.5 ? uv.yx : vec2(uv.x - uv.y, uv.x + uv.y);
  float len = uExtra.z > 0.0 ? uExtra.z : 3.0;
  float s = pfbm(r, vec2(f.x, f.y * len), OCT(), uSeed);
  float big = pfbm(uv, f * 0.5 + 1.0, 3, uSeed + 3.0);
  float v = s * 0.65 + big * 0.35;
  float w = mix(0.008, 0.12, clamp(uExtra.x, 0.0, 1.0));
  float sh = 1.0 - smoothstep(0.45 - w, 0.45 + w, v);
  float hl = smoothstep(0.58 - w, 0.58 + w, v);
  vec3 c = mix(mix(uColA, uColB, sh), uColC, hl);
  float br = pfbm(r, vec2(f.x * 2.0, f.y * 64.0), 2, uSeed + 5.0);
  c *= 1.0 + (br - 0.5) * 0.2 * uExtra.w;
  float n = pfbm(uv, f * 4.0, 3, uSeed + 7.0);
  c *= 1.0 + (n - 0.5) * uVariation;
  float h = 0.5 + ((s - 0.5) * 0.5 + (br - 0.5) * 0.5 * uExtra.w) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, sh) + (br - 0.5) * 0.05 * uExtra.w, uMetal.x, h, vec3(0));
}

// periodic Voronoi that also returns the vector from the cell's point to p (cell units)
vec2 pvorRel(vec2 uv, vec2 freq, float jitter, float seed, out float id, out float edge) {
  vec2 p = uv * freq;
  vec2 i = floor(p), f = fract(p);
  float f1 = 8.0;
  vec2 mr = vec2(0), mg = vec2(0);
  id = 0.0;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      vec2 g = vec2(x, y);
      vec2 c = mod(i + g, freq);
      vec2 o = hash22(c + seed * 7.7) * jitter + (1.0 - jitter) * 0.5;
      vec2 r = g + o - f;
      float d = dot(r, r);
      if (d < f1) { f1 = d; id = hash12(c + seed * 3.3); mr = r; mg = g; }
    }
  float md = 8.0;
  for (int y = -2; y <= 2; ++y)
    for (int x = -2; x <= 2; ++x) {
      vec2 g = mg + vec2(x, y);
      vec2 c = mod(i + g, freq);
      vec2 o = hash22(c + seed * 7.7) * jitter + (1.0 - jitter) * 0.5;
      vec2 r = g + o - f;
      if (dot(mr - r, mr - r) > 1e-5) md = min(md, dot(0.5 * (mr + r), normalize(r - mr)));
    }
  edge = md;
  return -mr;
}

// hand-painted lighting: light from the top left, three flat-ish value steps
const vec2 kPaintLight = vec2(-0.6, 0.8);

// ---- 44 hand-painted stone: cobbles or wall blocks with painted highlights, shadows and dark outlines
// extra: x outline width (default 0.05), y layout (0 cobbles, 1 wall blocks), z painted chips / cracks,
//        w highlight strength (default 1)
Surf genPaintedStone(vec2 uv) {
  vec2 f = F();
  float id, edge;
  vec2 rel;
  float hw = uExtra.w > 0.0 ? uExtra.w : 1.0;
  float wob = pfbm(uv, f * 6.0, 3, uSeed + 4.0) - 0.5;
  float e, lt;
  if (uExtra.y > 0.5) {
    float R = max(1.0, floor(uScale + 0.5));
    vec2 p = uv * vec2(R);
    float row = floor(p.y);
    float rr = mod(row, R);
    float n = max(1.0, floor(R / 1.6 * (0.75 + 0.5 * hash12(vec2(rr, 1.3) + uSeed)) + 0.5));
    float L = R / n;
    float off = hash12(vec2(rr, 5.1) + uSeed) * L;
    float col = floor((p.x - off) / L);
    vec2 lo = vec2(col * L + off, row);
    vec2 sz = vec2(L, 1.0);
    vec2 q = p - lo - sz * 0.5;
    vec2 qq = abs(q) - sz * 0.5 + 0.18;
    e = 0.18 - (length(max(qq, 0.0)) + min(max(qq.x, qq.y), 0.0));  // rounded rectangle
    rel = q / sz;
    id = hash12(vec2(mod(col, n), rr) + uSeed * 2.3);
    e *= 2.0;
  } else {
    rel = pvorRel(uv, f, 0.8, uSeed, id, edge);
    e = edge;
  }
  e += wob * 0.08;
  float ow = (uExtra.x > 0.0 ? uExtra.x : 0.05);
  float stone = smoothstep(ow, ow + 0.02, e);
  float rim = 1.0 - smoothstep(ow, ow + 0.22, e);  // the stone's sloping border
  lt = dot(normalize(rel + 1e-4), normalize(kPaintLight)) * rim;
  float hl = smoothstep(0.12, 0.2, lt) * hw;
  float sh = smoothstep(0.1, 0.2, -lt);
  float blot = pfbm(uv, f * 3.0, 3, uSeed + 6.0);
  vec3 base = mix(uColA, uColB, smoothstep(0.2, 0.8, id)) * (1.0 + (step(0.56, blot) - 0.5) * 0.12 + (id - 0.5) * uVariation);
  vec3 c = base;
  c = mix(c, base * 1.35 + 0.04, hl);
  c = mix(c, mix(base, uColC, 0.45) * 0.85, sh);
  float chip = smoothstep(0.62, 0.66, pfbm(uv, f * 5.0, 4, uSeed + 8.0)) * uExtra.z * smoothstep(ow + 0.1, ow + 0.2, e);
  c = mix(c, mix(base, uColC, 0.5), chip);
  c = mix(uColC, c, stone);
  float h = 0.5 + (stone * (0.25 + 0.35 * (1.0 - rim)) - 0.3 + (blot - 0.5) * 0.1 - chip * 0.1) * uHeightAmp;
  return Surf(c, mix(uRough.y, uRough.x, stone), uMetal.x, h, vec3(0));
}

// ---- 45 hand-painted wood planks: painted grain strokes, knots, light top edges and dark outlines
// extra: x grain lines per plank (default 3), y knots (0..1), z edge highlight width (default 0.12),
//        w > 0.5 nails at the plank ends
Surf genPaintedWood(vec2 uv) {
  float R = max(1.0, floor(uScale + 0.5));
  vec2 p = uv * vec2(R);
  float row = floor(p.y);
  float rr = mod(row, R);
  float n = max(1.0, floor(R / 4.0 * (0.7 + 0.6 * hash12(vec2(rr, 2.9) + uSeed)) + 0.5));
  float L = R / n;
  float off = hash12(vec2(rr, 6.3) + uSeed) * L;
  float col = floor((p.x - off) / L);
  vec2 lo = vec2(col * L + off, row);
  vec2 q = p - lo;
  float id = hash12(vec2(mod(col, n), rr) + uSeed * 1.9);
  float id2 = hash12(vec2(mod(col, n), rr) * 1.7 + uSeed + 4.0);
  vec2 o = vec2(id * 151.0, id2 * 77.0);
  // knot: a lens of rings somewhere along the plank
  vec2 kc = vec2(L * (0.2 + 0.6 * id2), 0.3 + 0.4 * id);
  vec2 kd = (q - kc) * vec2(0.45, 1.0);
  float kr = length(kd);
  float hasKnot = step(1.0 - clamp(uExtra.y, 0.0, 1.0) * 0.7, hash12(o));
  float bend = hasKnot * 0.35 * exp(-kr * kr * 6.0) * sign(q.y - kc.y);
  float warp = lfbm(vec2(q.x * 0.35, 0.0) + o, 3, uSeed) - 0.5;
  float lines = uExtra.x > 0.0 ? uExtra.x : 3.0;
  float g = q.y * lines + warp * 1.5 + bend * lines;
  float gl = abs(fract(g) - 0.5);
  float lineOn = step(0.35, hash12(vec2(floor(g), id * 17.0)));
  float stroke = (1.0 - smoothstep(0.04, 0.09, gl + (lnoise(vec2(q.x * 3.0, g) + o, uSeed + 3.0) - 0.5) * 0.12)) * lineOn;
  stroke *= smoothstep(0.0, 0.5, fract(q.x / L * 2.0 + id) * 2.0) * 0.7 + 0.3;  // strokes taper in and out
  float knot = hasKnot * (1.0 - smoothstep(0.1, 0.16, kr));
  float knotRing = hasKnot * (1.0 - smoothstep(0.03, 0.06, abs(kr - 0.2))) * 0.7;
  float ew = uExtra.z > 0.0 ? uExtra.z : 0.12;
  float top = smoothstep(1.0 - ew, 1.0 - ew * 0.6, q.y);
  float bot = 1.0 - smoothstep(ew * 0.4, ew, q.y);
  float ends = 1.0 - smoothstep(0.03, 0.06, min(q.x, L - q.x));
  float gap = 1.0 - smoothstep(0.02, 0.045, min(min(q.y, 1.0 - q.y), min(q.x, L - q.x)));
  vec3 base = uColA * (1.0 + (id - 0.5) * uVariation * 2.0);
  vec3 c = base;
  c = mix(c, mix(base, uColC, 0.6), max(stroke * 0.8, max(knot, knotRing)));
  c = mix(c, mix(base, uColB, 0.85), top);
  c = mix(c, mix(base, uColC, 0.5), max(bot, ends * 0.6));
  float nail = 0.0;
  if (uExtra.w > 0.5) {
    float dn = min(length(vec2(q.x - 0.25, q.y - 0.5)), length(vec2(L - q.x - 0.25, q.y - 0.5)));
    nail = 1.0 - smoothstep(0.06, 0.08, dn);
    vec3 nc = mix(uColC, uColB, smoothstep(0.03, -0.03, (q.y - 0.5) - (q.x < L * 0.5 ? q.x - 0.25 : L - q.x - 0.25)));
    c = mix(c, nc, nail);
  }
  c = mix(c, uColC * 0.8, gap);
  float h = 0.5 + ((1.0 - gap) * 0.3 - 0.15 - stroke * 0.08 - knot * 0.05 + top * 0.05 - bot * 0.05 + nail * 0.1) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, gap), uMetal.x, h, vec3(0));
}

// ---- 46 hand-painted metal plates: painted bevels, gradient, scratches and rivets
// extra: x bevel width (default 0.08), y > 0.5 rivets, z painted scratches (0..1), w top-to-bottom gradient
Surf genPaintedMetal(vec2 uv) {
  vec2 f = F();
  vec2 p = uv * f;
  vec2 cell = floor(p);
  vec2 q = fract(p);
  float id = hash12(mod(cell, f) + uSeed * 2.7);
  float bw = uExtra.x > 0.0 ? uExtra.x : 0.08;
  float dl = q.x, dr = 1.0 - q.x, db = q.y, dt = 1.0 - q.y;
  float e = min(min(dl, dr), min(db, dt));
  float bevel = 1.0 - smoothstep(bw * 0.85, bw, e);
  float side = e == dt ? 1.0 : e == dl ? 0.55 : e == dr ? -0.55 : -1.0;  // painted light from the top left
  float outline = 1.0 - smoothstep(0.012, 0.02, e);
  float n = pfbm(uv, f * 3.0, OCT(), uSeed + 1.0);
  vec3 base = uColA * (1.0 + (q.y - 0.5) * uExtra.w * 0.4 + (id - 0.5) * uVariation + (step(0.55, n) - 0.5) * 0.06);
  vec3 c = base;
  c = mix(c, mix(base, uColB, 0.85), bevel * smoothstep(0.0, 0.3, side));
  c = mix(c, mix(base, uColC, 0.55), bevel * smoothstep(0.0, -0.3, side));
  float edgeHi = (1.0 - smoothstep(0.0, bw * 0.3, abs(e - bw))) * step(0.0, side);  // bright line on the lit crest
  c = mix(c, uColB, edgeHi * 0.8);
  float sc = scratches(uv, 3.0, uSeed + 2.0) * clamp(uExtra.z, 0.0, 1.0) * (1.0 - bevel);
  c = mix(c, uColB, sc * 0.85);
  float rivet = 0.0;
  if (uExtra.y > 0.5) {
    vec2 rc = min(q, 1.0 - q);
    vec2 rd = rc - vec2(bw * 1.9);
    float dr2 = length(rd);
    rivet = 1.0 - smoothstep(0.03, 0.036, dr2);
    float rimR = 1.0 - smoothstep(0.03, 0.045, dr2);
    vec2 sgn = vec2(q.x < 0.5 ? -1.0 : 1.0, q.y < 0.5 ? -1.0 : 1.0);
    float spot = 1.0 - smoothstep(0.0, 0.016, length(rd * sgn - vec2(-0.012, 0.012)));
    c = mix(c, uColC, rimR * (1.0 - rivet));
    c = mix(c, mix(base * 1.1, uColB, spot), rivet);
  }
  c = mix(c, uColC, outline);
  float h = 0.5 + ((1.0 - bevel) * 0.25 + bevel * (e / bw) * 0.25 - outline * 0.3 + rivet * 0.25 - sc * 0.05) * uHeightAmp;
  float r = mix(uRough.x, uRough.y, max(outline, sc)) + (n - 0.5) * 0.05;
  return Surf(c, r, mix(uMetal.x, uMetal.y, outline), h, vec3(0));
}

// ================================================================ 0.24 generators (47..)

// a smooth square wave: 1 over the first w of each unit of t, 0 elsewhere, edges softened over aa
float band(float t, float w, float aa) {
  float fr = fract(t);
  float sd = fr < w ? min(fr, w - fr) : -min(fr - w, 1.0 - fr);
  return smoothstep(-aa, aa, sd);
}
// a thin line at every whole t, width w (units of t)
float lines1(float t, float w, float aa) {
  float d = abs(fract(t + 0.5) - 0.5);
  return 1.0 - smoothstep(w * 0.5 - aa, w * 0.5 + aa, d);
}
float sdSeg(vec2 p, vec2 a, vec2 b) {
  vec2 pa = p - a, ba = b - a;
  float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);
  return length(pa - ba * h);
}

// a plain weave under a print (stripes, checks, dots): x brightness, y height
vec2 printWeave(vec2 uv, float amt) {
  if (amt <= 0.0) return vec2(1.0, 0.5);
  float T = clamp(floor(uScale * 12.0 + 0.5), 48.0, 192.0);
  vec2 p = uv * T;
  vec2 c = floor(p), q = fract(p);
  bool over = mod(c.x + c.y, 2.0) < 0.5;
  float across = over ? q.y : q.x, along = over ? q.x : q.y;
  float prof = sin(across * PI) * (0.75 + 0.25 * sin(along * PI));
  float slub = hash12(mod(over ? vec2(c.y, 0.0) : vec2(c.x, 1.0), vec2(T)) + uSeed * 1.3);
  return vec2(mix(1.0, 0.8 + 0.2 * prof + (slub - 0.5) * 0.12, amt), mix(0.5, 0.25 + 0.6 * prof, amt));
}

// ---- 47 stripes: plain, pinstripe, chevron, wavy or three-color stripes, printed or woven
// extra: x width of the B stripes (part of each repeat; 0 = 0.5, pinstripes 0.06), y style (0 plain,
//        1 pinstripe, 2 chevron, 3 wavy, 4 three colors A B C B), z direction (0 across, 1 along, 2 diagonal,
//        3 other diagonal), w woven fabric underneath (0 a smooth print .. 1)
Surf genStripes(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float style = floor(uExtra.y + 0.5), dir = floor(uExtra.z + 0.5);
  vec2 p = uv * N;
  float t, s;  // across the stripes (one repeat per unit) and along them
  if (dir < 0.5) t = p.y, s = p.x;
  else if (dir < 1.5) t = p.x, s = p.y;
  else if (dir < 2.5) t = p.x + p.y, s = p.x - p.y;
  else t = p.x - p.y, s = p.x + p.y;
  if (style > 1.5 && style < 2.5) t += abs(fract(s) - 0.5);
  if (style > 2.5 && style < 3.5) t += 0.18 * sin(s * 2.0 * PI);
  float aa = max(fwidth(t), 1e-4) * 0.75;
  float w = uExtra.x > 0.0 ? uExtra.x : (style > 0.5 && style < 1.5 ? 0.06 : 0.5);
  float n = pfbm(uv, vec2(4.0), OCT(), uSeed);
  vec3 c;
  float tb;
  if (style > 3.5) {
    // A, then B, C, B sharing the width w
    float a = 1.0 - w;
    float inB1 = band(t - a, w * 0.25, aa), inC = band(t - a - w * 0.25, w * 0.5, aa), inB2 = band(t - a - w * 0.75, w * 0.25, aa);
    c = mix(mix(uColA, uColB, clamp(inB1 + inB2, 0.0, 1.0)), uColC, inC);
    tb = clamp(inB1 + inB2 + inC, 0.0, 1.0);
  } else {
    tb = band(t, w, aa);
    c = mix(uColA, style > 0.5 && style < 1.5 ? uColC : uColB, tb);
  }
  vec2 wv = printWeave(uv, uExtra.w);
  c *= wv.x * (1.0 + (n - 0.5) * uVariation);
  float h = 0.5 + (wv.y - 0.5) * uHeightAmp + (tb - 0.5) * uHeightAmp * 0.2;
  return Surf(c, mix(uRough.x, uRough.y, tb), mix(uMetal.x, uMetal.y, tb), h, vec3(0));
}

// ---- 48 checks: checkerboard, gingham, houndstooth, argyle, windowpane
// extra: x style (0 checkerboard, 1 gingham, 2 houndstooth, 3 argyle, 4 windowpane), y line width (argyle and
//        windowpane, default 0.05), z woven fabric underneath (0 a smooth print .. 1), w argyle diamond
//        height : width (default 1.5)
Surf genChecks(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float style = floor(uExtra.x + 0.5);
  float lw = uExtra.y > 0.0 ? uExtra.y : 0.05;
  vec2 p = uv * N;
  vec2 fw = fwidth(p);
  float aa = max(max(fw.x, fw.y), 1e-4) * 0.75;
  float tb = 0.0, tc = 0.0;  // how much of colors B and C
  if (style < 0.5) {
    float N2 = 2.0 * max(1.0, floor(N * 0.5 + 0.5));  // even, so the board repeats
    vec2 q = uv * N2 * 0.5;
    float sx = band(q.x, 0.5, aa * 0.5), sy = band(q.y, 0.5, aa * 0.5);
    tb = sx * sy + (1.0 - sx) * (1.0 - sy);
  } else if (style < 1.5) {
    tb = (band(p.x, 0.5, aa) + band(p.y, 0.5, aa)) * 0.5;
  } else if (style < 2.5) {
    // houndstooth: per check, a dark quarter, a light quarter and two quarters of diagonal stripes
    float qx = band(p.x, 0.5, aa), qy = band(p.y, 0.5, aa);  // 1 in the left / bottom half
    float st = band((p.x + p.y) * 2.0, 0.5, aa * 2.0);
    tb = qx * qy + st * (qx * (1.0 - qy) + qy * (1.0 - qx));
  } else if (style < 3.5) {
    float A = uExtra.w > 0.0 ? uExtra.w : 1.5;
    float rows = max(1.0, floor(N / A + 0.5));
    vec2 d = vec2(uv.x * N, uv.y * rows);
    float a = d.x + d.y, b = d.x - d.y;
    float fa = max(fwidth(a), 1e-4) * 0.75;
    float sa = band(a * 0.5, 0.5, fa * 0.5), sb = band(b * 0.5, 0.5, fa * 0.5);
    tb = sa * sb + (1.0 - sa) * (1.0 - sb);
    tc = max(lines1(a + 0.5, lw * 2.0, fa), lines1(b + 0.5, lw * 2.0, fa));
  } else {
    tb = max(lines1(p.x, lw, aa), lines1(p.y, lw, aa));
  }
  float n = pfbm(uv, vec2(4.0), OCT(), uSeed);
  vec3 c = mix(mix(uColA, uColB, tb), uColC, tc);
  vec2 wv = printWeave(uv, uExtra.z);
  c *= wv.x * (1.0 + (n - 0.5) * uVariation);
  float h = 0.5 + (wv.y - 0.5) * uHeightAmp + (n - 0.5) * uHeightAmp * 0.1;
  return Surf(c, mix(uRough.x, uRough.y, tb), mix(uMetal.x, uMetal.y, tb), h, vec3(0));
}

// ---- 49 dots: polka dots, staggered, scattered spots or halftone; printed or raised
// extra: x dot size (radius, part of the spacing, default 0.3), y layout (0 grid, 1 staggered, 2 scattered,
//        3 halftone), z raised (0 printed .. 1 domed studs), w woven fabric underneath (0 .. 1)
Surf genDots(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float lay = floor(uExtra.y + 0.5);
  float r = uExtra.x > 0.0 ? uExtra.x : 0.3;
  vec2 p = uv * N;
  float aa = max(max(fwidth(p.x), fwidth(p.y)), 1e-4);
  float d, rr = r, pick = 0.0;
  if (lay < 0.5) {
    d = length(fract(p) - 0.5);
  } else if (lay < 1.5 || lay > 2.5) {
    vec2 q1 = fract(p) - 0.5, q2 = fract(p + 0.5) - 0.5;
    d = min(length(q1), length(q2));
    rr = r * 0.72;
    if (lay > 2.5) {
      float tone = smoothstep(0.2, 0.8, pfbm(uv, vec2(2.0), 4, uSeed + 3.0));
      rr = 0.36 * clamp((0.15 + 0.85 * tone) * (r / 0.3), 0.0, 1.05);
    }
  } else {
    float id, edge;
    vec2 rel = pvorRel(uv, vec2(N), 0.8, uSeed, id, edge);
    d = length(rel);
    rr = r * (0.45 + 0.75 * fract(id * 13.7));
    pick = step(0.6, fract(id * 7.3));
  }
  float dot_ = 1.0 - smoothstep(rr - aa, rr + aa, d);
  float dome = sqrt(max(0.0, 1.0 - (d / max(rr, 1e-3)) * (d / max(rr, 1e-3))));
  float n = pfbm(uv, vec2(4.0), OCT(), uSeed + 1.0);
  vec3 c = mix(uColA, mix(uColB, uColC, pick), dot_);
  vec2 wv = printWeave(uv, uExtra.w);
  c *= mix(wv.x, 1.0, dot_ * uExtra.z) * (1.0 + (n - 0.5) * uVariation);
  float h = 0.5 + (wv.y - 0.5) * uHeightAmp * (1.0 - dot_ * uExtra.z) + (dome * dot_ * uExtra.z * 0.8 + dot_ * 0.05) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, dot_), mix(uMetal.x, uMetal.y, dot_), h, vec3(0));
}

// ---- 50 animal prints: leopard, cheetah, zebra, tiger, giraffe, cow
// extra: x pattern (0 leopard, 1 cheetah, 2 zebra, 3 tiger, 4 giraffe, 5 cow), y fur (0 a smooth print .. 1
//        short fur), z mark size (default 1). Colors: A the coat, B the marks, C rosette centers / patches.
Surf genAnimal(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float kind = floor(uExtra.x + 0.5);
  float sz = uExtra.z > 0.0 ? uExtra.z : 1.0;
  float fur = pnoise(uv * vec2(192.0, 48.0), vec2(192.0, 48.0), uSeed + 21.0);
  float fur2 = pnoise(uv * vec2(96.0, 24.0), vec2(96.0, 24.0), uSeed + 23.0);
  float fz = (fur - 0.5) * 0.25 * uExtra.y;  // fur feathers the marks' edges
  float wob = pfbm(uv, vec2(N * 4.0), 3, uSeed + 2.0) - 0.5;
  float mk = 0.0, cen = 0.0;
  if (kind < 0.5) {
    float id, edge;
    vec2 rel = pvorRel(uv, vec2(N), 0.75, uSeed, id, edge);
    float d = length(rel) + wob * 0.18 + fz;
    float r1 = 0.2 * sz * (0.8 + 0.4 * id), r2 = r1 + 0.12 * sz;
    float a = atan(rel.y, rel.x) / (2.0 * PI);
    float k = 3.0 + floor(id * 3.0);
    float seg = fract(a * k + id * 5.3 + wob * 0.6);
    float on = smoothstep(0.0, 0.06, seg) * (1.0 - smoothstep(0.74, 0.82, seg));
    mk = smoothstep(r1 - 0.02, r1 + 0.01, d) * (1.0 - smoothstep(r2 - 0.01, r2 + 0.02, d)) * on;
    cen = 1.0 - smoothstep(r1 - 0.03, r1 + 0.01, d);
    vec4 v2 = pvoronoi(uv, vec2(N * 2.0), 1.0, uSeed + 9.0);
    float sp = (1.0 - smoothstep(0.1 * sz, 0.14 * sz, v2.x + wob * 0.1 + fz)) * step(0.55, v2.z) * step(r2 + 0.08, d);
    mk = max(mk, sp);
  } else if (kind < 1.5) {
    vec4 v = pvoronoi(uv, vec2(N * 2.0), 0.85, uSeed);
    mk = 1.0 - smoothstep(0.22 * sz, 0.26 * sz, v.x + wob * 0.15 + fz);
  } else if (kind < 3.5) {
    float bend = pfbm(uv, vec2(2.0, 3.0), 5, uSeed) - 0.5;
    float t = uv.x * N + bend * N * 0.45 + (pfbm(uv, vec2(6.0), 3, uSeed + 4.0) - 0.5) * 0.6;
    float z = sin(t * 2.0 * PI) + (pfbm(uv, vec2(8.0), 4, uSeed + 5.0) - 0.5) * 1.2 + fz * 4.0;
    float thr = kind > 2.5 ? 1.0 - 0.45 * sz : 0.1 + (1.0 - sz) * 0.5;
    mk = smoothstep(-0.05, 0.05, z - thr);
    if (kind > 2.5) mk *= smoothstep(0.32, 0.5, pfbm(uv, vec2(3.0, 5.0), 4, uSeed + 7.0));
  } else if (kind < 4.5) {
    float id, edge;
    pvorRel(uv, vec2(N), 0.9, uSeed, id, edge);
    float e = edge + wob * 0.08 + fz * 0.5;
    float bw = 0.06 * sz;
    mk = smoothstep(bw, bw + 0.02, e);  // the patches
    cen = fract(id * 7.1) * smoothstep(bw + 0.05, bw + 0.3, e);
  } else {
    float b = pfbm(uv, vec2(max(1.0, floor(N / 3.0 + 0.5))), 5, uSeed) + fz * 0.25;
    mk = smoothstep(0.58 / sz - 0.01, 0.58 / sz + 0.01, b + wob * 0.05);
  }
  vec3 coat = uColA;
  vec3 c;
  if (kind > 3.5 && kind < 4.5) c = mix(uColA, mix(uColB, uColC, cen * 0.6), mk);
  else c = mix(mix(coat, uColC, cen), uColB, mk);
  float n = pfbm(uv, vec2(4.0), OCT(), uSeed + 1.0);
  c *= (1.0 + (n - 0.5) * uVariation) * mix(1.0, 0.8 + 0.35 * fur * fur2 * 2.0, uExtra.y);
  float h = 0.5 + ((fur - 0.5) * 0.6 + (fur2 - 0.5) * 0.4) * uExtra.y * uHeightAmp + (n - 0.5) * 0.1 * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, mk), uMetal.x, h, vec3(0));
}

// ---- 51 cracked earth: dry mud plates with finer cracks and curled edges; glowing cracks make lava
// extra: x crack width (default 0.05), y finer cracks (0 none .. 1), z curled plate edges (0 flat .. 1),
//        w glow in the cracks (the Emissive color: 0 none .. 1 lava)
Surf genCrackedEarth(vec2 uv) {
  vec2 f = F();
  float wob = pfbm(uv, f * 4.0, 4, uSeed + 3.0) - 0.5;
  vec4 v = pvoronoi(uv, f, 0.9, uSeed);
  float cw = (uExtra.x > 0.0 ? uExtra.x : 0.05) * 0.5;
  float e = v.w + wob * 0.06;
  float crack = 1.0 - smoothstep(cw, cw + 0.02, e);
  vec4 v2 = pvoronoi(uv, f * 3.0, 0.9, uSeed + 7.0);
  float fine = (1.0 - smoothstep(cw * 0.5, cw * 0.5 + 0.03, v2.w + wob * 0.05)) * uExtra.y;
  fine *= smoothstep(0.38, 0.55, pfbm(uv, f * 2.0, 4, uSeed + 11.0)) * (1.0 - crack);
  float n = pfbm(uv, f * 2.0, OCT(), uSeed + 1.0);
  float dust = pfbm(uv, f * 8.0, 3, uSeed + 5.0);
  vec3 plate = mix(uColA, uColB, clamp(v.z * 0.6 + (n - 0.5) * 0.8 + (dust - 0.5) * 0.4, 0.0, 1.0));
  plate *= 1.0 + (n - 0.5) * uVariation;
  float curl = uExtra.z * (1.0 - smoothstep(0.0, 0.22, e - cw));
  float cr = max(crack, fine * 0.85);
  vec3 c = mix(plate, plate * 1.12 + 0.02, curl * 0.6);
  c = mix(c, uColC, cr);
  float h = 0.5 + (0.25 + curl * 0.35 + (n - 0.5) * 0.25 + (dust - 0.5) * 0.1 - crack * 0.75 - fine * 0.3) * uHeightAmp;
  float glow = uExtra.w * (crack + fine * 0.6 + 0.35 * (1.0 - smoothstep(cw, cw + 0.12, e))) * (0.7 + 0.6 * n);
  return Surf(c, mix(uRough.x, uRough.y, cr), uMetal.x, h, uEmissive * glow);
}

// ---- 52 flagstones: irregular stone slabs (crazy paving) with mortar or grassy joints
// extra: x joint width (default 0.08), y uneven slabs (0 flat .. 1 tilted), z riven, layered surface (0 .. 1),
//        w tufts in the joints (0 mortar .. 1). Colors: A and B the stones, C the joints.
Surf genFlagstone(vec2 uv) {
  vec2 f = F();
  float id, edge;
  vec2 rel = pvorRel(uv, f, 0.85, uSeed, id, edge);
  float wob = pfbm(uv, f * 5.0, 3, uSeed + 2.0) - 0.5;
  float e = edge + wob * 0.05;
  float jw = (uExtra.x > 0.0 ? uExtra.x : 0.08) * 0.5;
  float stone = smoothstep(jw, jw + 0.025, e);
  float bevel = smoothstep(jw, jw + 0.12, e);
  float n = pfbm(uv, f * 3.0, OCT(), uSeed + 4.0);
  float riv = pfbm(uv, f * 6.0, 4, uSeed + 9.0 + id * 5.0);
  float layers = mix(riv, floor(riv * 6.0 + 0.5) / 6.0, 0.75);
  vec2 tilt = (hash22(vec2(id * 91.7, id * 37.3)) - 0.5) * uExtra.y;
  vec3 sc = mix(uColA, uColB, smoothstep(0.15, 0.85, id));
  sc *= 1.0 + (n - 0.5) * uVariation * 1.5 + (layers - 0.5) * 0.25 * uExtra.z + (fract(id * 7.7) - 0.5) * uVariation;
  float sand = pnoise(uv * f * 20.0, f * 20.0, uSeed + 6.0);
  float tuftN = pfbm(uv, f * 16.0, 3, uSeed + 13.0);
  float tuft = uExtra.w * smoothstep(0.35, 0.65, tuftN);
  vec3 jc = uColC * (0.8 + 0.4 * sand) * (1.0 + tuft * 0.35);
  vec3 c = mix(jc, sc, stone);
  float hs = 0.1 + bevel * 0.25 + dot(rel, tilt) * 0.6 + (n - 0.5) * 0.25 + (layers - 0.5) * 0.45 * uExtra.z;
  float hj = -0.4 + (sand - 0.5) * 0.1 + tuft * 0.45;
  float h = 0.5 + mix(hj, hs, stone) * uHeightAmp;
  return Surf(c, mix(uRough.y, uRough.x + (n - 0.5) * 0.08, stone), uMetal.x, h, vec3(0));
}

// ---- 53 pebbles: rounded stones packed together, sand or mud between
// extra: x gap (default 0.12), y size variation (0 .. 1), z flatness (0 round .. 1 flat river stones),
//        w small gravel between (0 .. 1). Colors: A and B the stones, C what lies between.
Surf genPebbles(vec2 uv) {
  vec2 f = F();
  float id, edge;
  vec2 rel = pvorRel(uv, f, 0.6, uSeed, id, edge);  // points kept near their cell's middle: whole stones
  float gap = (uExtra.x > 0.0 ? uExtra.x : 0.12) * 0.5;
  float shrink = gap * 0.6 + uExtra.y * 0.12 * fract(id * 13.1);
  // the cell, its corners rounded off: a smooth minimum with a disc round the stone's center
  float disc = 0.56 * (0.9 + 0.2 * fract(id * 5.3)) - length(rel * vec2(1.0, 1.0 + 0.3 * fract(id * 9.1)));
  float k = 0.1;
  float hh = clamp(0.5 + 0.5 * (disc - edge) / k, 0.0, 1.0);
  float er = mix(disc, edge, hh) - k * hh * (1.0 - hh);
  float e = er - shrink + (pfbm(uv, f * 4.0, 3, uSeed + 3.0) - 0.5) * 0.04;
  float inside = smoothstep(0.0, 0.015, e);
  float rim = 0.12 + 0.2 * (1.0 - uExtra.z);
  float x = clamp(e / rim, 0.0, 1.0);
  float dome = sqrt(1.0 - (1.0 - x) * (1.0 - x));
  float speck = pnoise(uv * f * 24.0, f * 24.0, uSeed + 7.0);
  float n = pfbm(uv, f * 2.0, OCT(), uSeed + 1.0);
  vec3 sc = mix(uColA, uColB, smoothstep(0.1, 0.9, fract(id * 3.71)));
  sc *= 1.0 + (fract(id * 17.3) - 0.5) * uVariation * 2.0 + (speck - 0.5) * 0.25 + (n - 0.5) * 0.15;
  float ang = id * 6.2831;
  float vein = step(0.85, fract(id * 5.9)) * (1.0 - smoothstep(0.02, 0.05, abs(dot(rel, vec2(cos(ang), sin(ang))) - (fract(id * 3.3) - 0.5) * 0.3)));
  sc = mix(sc, vec3(0.9, 0.88, 0.84), vein * 0.55);
  vec4 v2 = pvoronoi(uv, f * 4.0, 1.0, uSeed + 5.0);
  float grav = (1.0 - smoothstep(0.28, 0.36, v2.x)) * uExtra.w * (1.0 - inside);
  float sand = pnoise(uv * f * 16.0, f * 16.0, uSeed + 9.0);
  vec3 ground = uColC * (0.8 + 0.4 * sand);
  ground = mix(ground, mix(uColA, uColB, v2.z) * 0.85, grav);
  vec3 c = mix(ground, sc, inside);
  float h = 0.5 + mix(-0.35 + (sand - 0.5) * 0.1 + grav * 0.25 * (1.0 - smoothstep(0.0, 0.33, v2.x)), -0.3 + dome * 0.8 + (speck - 0.5) * 0.04, inside) * uHeightAmp;
  return Surf(c, mix(uRough.y, uRough.x, inside), uMetal.x, h, vec3(0));
}

// ---- 54 strata: layered rock (sandstone, cliffs, canyon walls), harder bands standing out
// extra: x folding of the layers (default 0.3), y erosion relief (0 .. 1), z vertical cracks (0 .. 1),
//        w sand grain (0 .. 1). Colors: A, B and C the layers.
Surf genStrata(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float fold = uExtra.x > 0.0 ? uExtra.x : 0.3;
  float warp = pfbm(uv, vec2(2.0, 1.0), 4, uSeed) - 0.5;
  float warp2 = pfbm(uv, vec2(6.0, 3.0), 3, uSeed + 2.0) - 0.5;
  float t = uv.y * N + (warp * 2.0 + warp2 * 0.4) * fold * N * 0.5;
  float K = 4.0;  // layers per repeat
  float lt = t * K;
  float L = floor(lt);
  float key = mod(L, N * K);
  float hard = hash12(vec2(key, 1.3) + uSeed);
  float tone = hash12(vec2(key, 7.9) + uSeed);
  float fr = fract(lt);
  float aa = max(fwidth(lt), 1e-4);
  float edgeL = smoothstep(0.0, 0.08 + aa, fr) * smoothstep(1.0, 0.92 - aa, fr);
  vec3 c = tone < 0.4 ? mix(uColA, uColB, tone / 0.4) : mix(uColB, uColC, (tone - 0.4) / 0.6);
  float n = pfbm(uv, vec2(N * 2.0), OCT(), uSeed + 4.0);
  float grain = pnoise(uv * N * 48.0, vec2(N * 48.0), uSeed + 6.0);
  c *= 1.0 + (n - 0.5) * uVariation + (grain - 0.5) * 0.3 * uExtra.w;
  c *= mix(0.8, 1.0, edgeL);
  float prof = hard > 0.5 ? mix(1.0, 1.0 - pow(abs(fr * 2.0 - 1.0), 6.0), 0.8) : 1.0 - pow(abs(fr * 2.0 - 1.0), 2.0) * 0.6;
  float relief = (hard - 0.5) * 0.8 + prof * 0.4;
  float cr = pridged(uv, vec2(N * 2.0, max(1.0, N * 0.5)), 4, uSeed + 8.0);
  float crack = smoothstep(0.86, 0.97, cr) * uExtra.z * step(0.35, hash12(vec2(key, 3.3) + uSeed));
  c = mix(c, uColC * 0.35, crack);
  float h = 0.5 + (relief * uExtra.y + (n - 0.5) * 0.3 + (grain - 0.5) * 0.05 * uExtra.w - crack * 0.5) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, tone), uMetal.x, h, vec3(0));
}

// ---- 55 crackle: crazed glaze, alligatored old paint, craquelure on old varnish
// extra: x crack width (default 0.03), y style (0 crazed glaze, 1 alligator paint, 2 craquelure), z finer
//        cracks inside each piece (0 .. 1), w crack depth (default 1). Colors: A the glaze or paint, B what
//        shows under the paint, C the cracks.
Surf genCrackle(vec2 uv) {
  vec2 f = F();
  float style = floor(uExtra.y + 0.5);
  float cw = (uExtra.x > 0.0 ? uExtra.x : style < 0.5 ? 0.015 : 0.03) * (style > 0.5 && style < 1.5 ? 3.0 : 1.0) * 0.5;
  float depth = uExtra.w > 0.0 ? uExtra.w : 1.0;
  vec2 fr = style > 1.5 ? vec2(f.x * 2.0, f.y) : f;
  float wob = pfbm(uv, f * 6.0, 3, uSeed + 2.0) - 0.5;
  vec4 v = pvoronoi(uv, fr, 0.95, uSeed);
  float e = v.w + wob * 0.04;
  float big = 1.0 - smoothstep(cw, cw + 0.015, e);
  vec4 v2 = pvoronoi(uv, fr * 3.0, 0.95, uSeed + 5.0);
  float sub = (1.0 - smoothstep(cw * 0.45, cw * 0.45 + 0.02, v2.w + wob * 0.03)) * uExtra.z;
  sub *= smoothstep(0.4, 0.6, pfbm(uv, f * 2.0, 4, uSeed + 7.0)) * (1.0 - big);
  float n = pfbm(uv, f * 2.0, OCT(), uSeed + 1.0);
  vec3 top = uColA * (1.0 + (n - 0.5) * uVariation + (v.z - 0.5) * uVariation * 0.3);
  float crack = max(big, sub * 0.8);
  vec3 c;
  float h;
  if (style > 0.5 && style < 1.5) {
    float curl = 1.0 - smoothstep(cw, cw + 0.18, e);  // the paint lifts at its edges
    vec3 under = uColB * (0.85 + 0.3 * pfbm(uv, f * 8.0, 3, uSeed + 9.0));
    c = mix(mix(top, top * 0.85, curl * (1.0 - big)), under, big);
    c = mix(c, uColC, (1.0 - smoothstep(cw * 0.25, cw * 0.6, e)) * 0.8 + sub * 0.6);
    h = 0.5 + ((1.0 - big) * (0.3 + curl * 0.25) - 0.25 - sub * 0.2) * depth * uHeightAmp + (n - 0.5) * 0.1 * uHeightAmp;
  } else {
    c = mix(top, uColC, crack);
    h = 0.5 + (-crack * 0.5 * depth + (n - 0.5) * 0.2 + (v.z - 0.5) * 0.04) * uHeightAmp;
  }
  return Surf(c, mix(uRough.x, uRough.y, crack), uMetal.x, h, vec3(0));
}

// ---- 56 chainmail: interlocking rings, each passing over its neighbor on one side and under on the other
// extra: x wire thickness (part of the spacing, default 0.12), y ring size (default 0.42), z lean (default
//        0.6), w per-ring tarnish (0 .. 1, color B). Colors: A the rings, B tarnish, C between the rings.
Surf genChainmail(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  vec2 p = uv * N;  // columns 1 apart, rows 0.5 apart (2N rows)
  float R = uExtra.y > 0.0 ? uExtra.y : 0.42;
  float w = (uExtra.x > 0.0 ? uExtra.x : 0.12) * 0.5;
  float lean = uExtra.z > 0.0 ? uExtra.z : 0.6;
  float best = -9.0, second = -9.0, prof = 0.0;
  vec2 key = vec2(0);
  float j0 = floor(p.y / 0.5);
  for (int dj = -2; dj <= 2; ++dj) {
    float j = j0 + float(dj);
    float off = mod(j, 2.0) * 0.5;
    float i0 = floor(p.x - off + 0.5);
    for (int di = -1; di <= 1; ++di) {
      float i = i0 + float(di);
      vec2 q = p - vec2(i + off, j * 0.5);
      float d = abs(length(q) - R);
      if (d < w) {
        float hp = sqrt(1.0 - (d / w) * (d / w));
        float H = hp * w + lean * q.x * 0.5;
        if (H > best) { second = best; best = H; prof = hp; key = vec2(mod(i, N), mod(j, 2.0 * N)); }
        else if (H > second) second = H;
      }
    }
  }
  float id = hash12(key + uSeed * 3.1);
  float n = pfbm(uv, vec2(N), OCT(), uSeed + 2.0);
  bool on = best > -8.0;
  float under = second > -8.0 ? 1.0 : 0.0;  // another ring passes below: a little shadow
  vec3 ring = mix(uColA, uColB, clamp(uExtra.w * (id * 1.2 - 0.2 + (n - 0.5)), 0.0, 1.0));
  ring *= (0.7 + 0.3 * prof) * (1.0 + (id - 0.5) * uVariation) * (1.0 - under * 0.08);
  vec3 c = on ? ring : uColC * (0.8 + 0.4 * n);
  float hmax = w + lean * (R + w) * 0.5;
  float h = on ? 0.5 + (best / max(hmax, 1e-3) * 0.5) * uHeightAmp : 0.5 - 0.6 * uHeightAmp;
  gHole = on ? 0.0 : 1.0;
  return Surf(c, on ? uRough.x + (1.0 - prof) * 0.1 : uRough.y, on ? uMetal.x : uMetal.y, h, vec3(0));
}

// ---- 57 damascus steel: folded layers of bright and dark steel, etched
// extra: x folding (default 0.5), y pattern (0 random, 1 ladder, 2 raindrop), z etch depth (0 .. 1),
//        w layer contrast (default 1). Colors: A the bright steel, B the dark steel.
Surf genDamascus(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float pat = floor(uExtra.y + 0.5);
  float fold = uExtra.x > 0.0 ? uExtra.x : 0.5;
  float con = uExtra.w > 0.0 ? uExtra.w : 1.0;
  float w1 = pfbm(uv, vec2(2.0, 3.0), 5, uSeed) - 0.5;
  float w2 = pfbm(uv, vec2(6.0), 4, uSeed + 3.0) - 0.5;
  float t = uv.y * N * 2.0 + (w1 * 2.5 + w2 * 0.6) * fold * N;
  if (pat > 0.5 && pat < 1.5) t += sin(uv.x * 2.0 * PI * max(2.0, floor(N * 0.5 + 0.5))) * N * 0.35;
  if (pat > 1.5) {
    vec4 v = pvoronoi(uv, vec2(max(2.0, floor(N * 0.5 + 0.5))), 0.8, uSeed + 5.0);
    t += (1.0 - smoothstep(0.0, 0.6, v.x)) * 3.0;
  }
  float l1 = 0.5 + 0.5 * sin(t * 2.0 * PI);
  float l2 = 0.5 + 0.5 * sin(t * 6.0 * PI + w2 * 6.0);
  float s = clamp((l1 * 0.75 + l2 * 0.25 - 0.5) * (1.5 + con * 2.5) + 0.5, 0.0, 1.0);
  float streak = pnoise(uv * vec2(4.0, 256.0), vec2(4.0, 256.0), uSeed + 7.0);
  float n = pfbm(uv, vec2(N), OCT(), uSeed + 1.0);
  vec3 c = mix(uColB, uColA, s) * (1.0 + (n - 0.5) * uVariation + (streak - 0.5) * 0.06);
  float h = 0.5 + ((s - 0.5) * uExtra.z + (streak - 0.5) * 0.04) * uHeightAmp;
  return Surf(c, mix(uRough.y, uRough.x, s), mix(uMetal.y, uMetal.x, s), h, vec3(0));
}

// ---- 58 circuit board: copper traces, pads, vias and chips on a solder mask; glowing traces for sci-fi
// extra: x trace density (default 0.5), y chips (0 none .. 1), z glow along the traces (the Emissive color),
//        w trace width (default 0.18). Colors: A the board, B traces and pads, C the chips.
float cbPort(vec2 cell, vec2 G, float side, float dens) {
  return step(hash12(mod(cell, G) + vec2(side * 0.37 + 0.11, side * 0.73) + uSeed * 1.7), dens);
}
Surf genCircuit(vec2 uv) {
  float S = max(1.0, floor(uScale + 0.5));
  float G = S * 6.0;
  vec2 p = uv * G;
  vec2 c = floor(p), q = fract(p) - 0.5;
  float dens = uExtra.x > 0.0 ? uExtra.x : 0.5;
  float tw = (uExtra.w > 0.0 ? uExtra.w : 0.18) * 0.5;
  vec2 g2 = vec2(G);
  float pe = cbPort(c, g2, 0.0, dens), pw = cbPort(c - vec2(1, 0), g2, 0.0, dens);
  float pn = cbPort(c, g2, 1.0, dens), ps = cbPort(c - vec2(0, 1), g2, 1.0, dens);
  float k = pe + pw + pn + ps;
  float d = 9.0;
  // two ports at a right angle: one 45-degree run between them; otherwise each port runs to the center
  if (k == 2.0 && pe + pw == 1.0) {
    vec2 a = vec2(pe > 0.5 ? 0.5 : -0.5, 0.0), b = vec2(0.0, pn > 0.5 ? 0.5 : -0.5);
    vec2 m = (a + b) * 0.5;
    d = min(min(sdSeg(q, a, a * 0.6 + m * 0.4), sdSeg(q, a * 0.6 + m * 0.4, b * 0.6 + m * 0.4)), sdSeg(q, b * 0.6 + m * 0.4, b));
  } else {
    if (pe > 0.5) d = min(d, sdSeg(q, vec2(0), vec2(0.5, 0)));
    if (pw > 0.5) d = min(d, sdSeg(q, vec2(0), vec2(-0.5, 0)));
    if (pn > 0.5) d = min(d, sdSeg(q, vec2(0), vec2(0, 0.5)));
    if (ps > 0.5) d = min(d, sdSeg(q, vec2(0), vec2(0, -0.5)));
  }
  float aa = max(fwidth(p.x), 1e-4);
  float trace = 1.0 - smoothstep(tw - aa, tw + aa, d);
  float pad = 0.0, hole = 0.0;
  if (k == 1.0 || k >= 3.0) {
    float pr = k == 1.0 ? 0.3 : tw * 1.6;
    pad = 1.0 - smoothstep(pr - aa, pr + aa, length(q));
    if (k == 1.0) hole = 1.0 - smoothstep(0.1 - aa, 0.1 + aa, length(q));
  }
  float cu = max(trace, pad) * (1.0 - hole);
  // chips: on a coarse grid, a body with pins along its long sides
  vec2 cc = floor(uv * S), cq = fract(uv * S) * 6.0;
  float hc = hash12(mod(cc, vec2(S)) + uSeed * 2.9 + 4.0);
  float chip = 0.0, pin = 0.0;
  if (hc < uExtra.y) {
    vec2 h2 = hash22(mod(cc, vec2(S)) + uSeed + 9.0);
    vec2 sz = vec2(2.0 + floor(h2.x * 2.5), 1.0 + floor(h2.y * 2.0));
    vec2 lo = floor((6.0 - sz) * 0.5), hi = lo + sz;
    vec2 e = min(cq - lo, hi - cq);
    chip = smoothstep(-aa, aa, min(e.x, e.y) - 0.1);
    float py = min(abs(cq.y - lo.y), abs(cq.y - hi.y));
    float px = fract(cq.x * 2.0);
    pin = step(py, 0.25) * step(lo.x + 0.15, cq.x) * step(cq.x, hi.x - 0.15) * smoothstep(0.2, 0.3, px) * (1.0 - smoothstep(0.7, 0.8, px)) * (1.0 - chip);
  }
  float n = pfbm(uv, vec2(S * 2.0), OCT(), uSeed + 1.0);
  vec2 wv = printWeave(uv, 0.25);
  vec3 col = uColA * (1.0 + (n - 0.5) * uVariation) * wv.x;
  col = mix(col, uColB * (0.9 + 0.2 * n), max(cu, pin));
  col = mix(col, uColA * 0.3, hole);
  col = mix(col, uColC * (1.0 + (n - 0.5) * 0.1), chip);
  float metal = max(cu, pin) * (1.0 - chip);
  float h = 0.5 + ((wv.y - 0.5) * 0.1 + metal * 0.15 + pad * 0.05 - hole * 0.35 + chip * 0.7) * uHeightAmp;
  vec3 em = uEmissive * uExtra.z * trace * (1.0 - chip) * (0.6 + 0.8 * pfbm(uv, vec2(S * 3.0), 3, uSeed + 11.0));
  return Surf(col, mix(mix(uRough.x, uRough.y, metal), uRough.x * 0.8, chip), mix(uMetal.x, uMetal.y, metal), h, em);
}

// ---- 59 stained glass: colored pieces held by dark lead lines; glows when lit from behind
// extra: x lead width (default 0.06), y layout (0 random pieces, 1 square panes, 2 diamond panes), z glow
//        (0 none .. 1: the pieces light up in their own colors), w rippled glass (0 smooth .. 1).
//        Colors: A, B and C the glass.
Surf genStainedGlass(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float lay = floor(uExtra.y + 0.5);
  float lw = (uExtra.x > 0.0 ? uExtra.x : 0.06) * 0.5;
  float id, e;
  float wob = (pfbm(uv, vec2(N * 3.0), 3, uSeed + 2.0) - 0.5) * 0.03;
  if (lay < 0.5) {
    pvorRel(uv, vec2(N), 0.9, uSeed, id, e);
  } else if (lay < 1.5) {
    vec2 p = uv * N, q = fract(p);
    vec2 ee = min(q, 1.0 - q);
    e = min(ee.x, ee.y);
    id = hash12(mod(floor(p), vec2(N)) + uSeed);
  } else {
    vec2 d = uv * N;
    vec2 ab = vec2(d.x + d.y, d.x - d.y);
    vec2 q = fract(ab);
    vec2 ee = min(q, 1.0 - q);
    e = min(ee.x, ee.y) * 0.7071;
    vec2 fab = floor(ab);  // a tile further on is (N, N) or (N, -N) further in ab: keyed so both wrap
    id = hash12(mod(vec2(fab.x + fab.y, fab.x - fab.y), vec2(2.0 * N)) + uSeed);
    wob *= 0.5;
  }
  e += wob;
  float lead = 1.0 - smoothstep(lw, lw + 0.012, e);
  float lp = clamp(1.0 - e / max(lw, 1e-3), 0.0, 1.0);  // rounded top of the came
  float pick = fract(id * 3.17);
  vec3 g = pick < 0.34 ? uColA : pick < 0.67 ? uColB : uColC;
  g *= 1.0 + (fract(id * 11.3) - 0.5) * uVariation * 2.0;
  float mott = pfbm(uv, vec2(N * 4.0), OCT(), uSeed + 4.0);
  float rip = pfbm(uv, vec2(N * 10.0), 3, uSeed + 6.0);
  g *= 1.0 + (mott - 0.5) * 0.25 + (rip - 0.5) * 0.3 * uExtra.w;
  vec3 leadCol = vec3(0.12, 0.12, 0.13) * (0.8 + 0.4 * mott);
  vec3 c = mix(g, leadCol, lead);
  float h = 0.5 + (lead * (0.3 + 0.3 * sqrt(1.0 - (1.0 - lp) * (1.0 - lp))) - 0.1 + (rip - 0.5) * 0.25 * uExtra.w) * uHeightAmp;
  vec3 em = g * uExtra.z * (1.0 - lead) * (0.85 + 0.3 * mott);
  return Surf(c, mix(uRough.x, uRough.y, lead), mix(uMetal.x, uMetal.y, lead), h, em);
}

// ---- 60 mosaic: small tiles in grout: squares, penny rounds or irregular tesserae
// extra: x grout width (default 0.08), y shape (0 squares, 1 penny rounds, 2 tesserae), z color (0 random
//        A or B per tile .. 1 blended in large patches), w tile bevel (default 0.15).
//        Colors: A and B the tiles, C the grout.
Surf genMosaic(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  vec2 p = uv * N;
  float shape = floor(uExtra.y + 0.5);
  float gw = (uExtra.x > 0.0 ? uExtra.x : 0.08) * 0.5;
  float bw = uExtra.w > 0.0 ? uExtra.w : 0.15;
  float e, id;
  vec2 loc;
  if (shape < 0.5) {
    vec2 q = fract(p);
    vec2 ee = min(q, 1.0 - q);
    e = min(ee.x, ee.y);
    loc = q - 0.5;
    id = hash12(mod(floor(p), vec2(N)) + uSeed * 1.37);
  } else if (shape < 1.5) {
    vec2 q1 = fract(p) - 0.5, q2 = fract(p + 0.5) - 0.5;
    bool first = dot(q1, q1) < dot(q2, q2);
    loc = first ? q1 : q2;
    vec2 cell = first ? floor(p) : floor(p + 0.5) + vec2(0.5);
    e = 0.36 - length(loc);
    id = hash12(mod(cell, vec2(N)) + uSeed * 1.37);
  } else {
    loc = pvorRel(uv, vec2(N), 0.4, uSeed, id, e);
  }
  float grout = 1.0 - smoothstep(gw, gw + 0.03, e);
  float bevel = smoothstep(gw, gw + bw, e);
  float patch_ = pfbm(uv, vec2(2.0), 4, uSeed + 5.0);
  float pick = mix(step(0.5, id), smoothstep(0.4, 0.6, patch_ + (id - 0.5) * 0.3), uExtra.z);
  vec3 tc = mix(uColA, uColB, pick) * (1.0 + (fract(id * 19.7) - 0.5) * uVariation * 2.0);
  float sand = pnoise(uv * N * 8.0, vec2(N * 8.0), uSeed + 3.0);
  vec3 c = mix(tc, uColC * (0.85 + 0.3 * sand), grout);
  vec2 tilt = (vec2(fract(id * 7.3), fract(id * 3.9)) - 0.5) * 0.3;
  float h = 0.5 + (bevel * 0.35 - 0.1 + dot(loc, tilt) * (1.0 - grout) - grout * 0.25 + (sand - 0.5) * 0.04 * grout) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, grout), mix(uMetal.x, uMetal.y, grout), h, vec3(0));
}

// ---- 61 sequins: overlapping shiny discs in rows, each turned its own way; or fine glitter
// extra: x sequin size (default 0.65), y tilt (default 0.5), z 1 = glitter (fine flakes instead of discs),
//        w center holes (0 none .. 1). Colors: A the sequins, B a second color (mixed in by Variation),
//        C the fabric.
Surf genSequins(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float tiltA = uExtra.y > 0.0 ? uExtra.y : 0.5;
  if (uExtra.z > 0.5) {
    float G = N * 8.0;
    float id, edge;
    vec2 rel = pvorRel(uv, vec2(G), 1.0, uSeed, id, edge);
    vec2 tl = (vec2(fract(id * 7.3), fract(id * 13.1)) - 0.5) * 2.0 * tiltA;
    float spark = pow(fract(id * 29.7), 6.0);
    vec3 c = mix(uColA, uColB, step(1.0 - uVariation * 0.5, fract(id * 5.1))) * (0.75 + 0.25 * fract(id * 3.3) + spark * 0.6);
    float h = 0.5 + dot(rel, tl) * 0.25 * uHeightAmp;
    float seam = 1.0 - smoothstep(0.0, 0.04, edge);
    return Surf(c * (1.0 - seam * 0.3), uRough.x, uMetal.x, h, vec3(0));
  }
  vec2 p = uv * N;
  float R = uExtra.x > 0.0 ? uExtra.x : 0.65;
  float j0 = floor(p.y / 0.5);
  vec2 dV = vec2(0), key = vec2(0);
  float found = 0.0;
  for (int dj = 3; dj >= -2; --dj) {  // higher rows lie on top
    float j = j0 + float(dj);
    float off = mod(j, 2.0) * 0.5;
    float i0 = floor(p.x - off + 0.5);
    for (int di = -1; di <= 1; ++di) {
      float i = i0 + float(di);
      vec2 d = p - vec2(i + off, j * 0.5);
      if (found < 0.5 && length(d) < R * 0.5) { found = 1.0; dV = d / (R * 0.5); key = vec2(mod(i, N), mod(j, 2.0 * N)); }
    }
    if (found > 0.5) break;
  }
  float id = hash12(key + uSeed * 2.7);
  vec2 tl = (hash22(key + uSeed + 3.0) - 0.5) * 2.0 * tiltA;
  float r = length(dV);
  float hole = (1.0 - smoothstep(0.14 * uExtra.w, 0.14 * uExtra.w + 0.04, r)) * step(0.01, uExtra.w);
  float rim = smoothstep(0.85, 1.0, r);
  vec3 s = mix(uColA, uColB, step(1.0 - uVariation * 0.5, fract(id * 5.1))) * (0.85 + 0.3 * fract(id * 3.3)) * (1.0 - rim * 0.25);
  float n = pfbm(uv, vec2(N * 4.0), 3, uSeed + 1.0);
  vec3 c = found > 0.5 ? mix(s, uColC, hole) : uColC * (0.8 + 0.4 * n);
  float h = found > 0.5 ? 0.5 + (0.25 + dot(dV, tl) * 0.25 + r * r * 0.1 - hole * 0.3) * uHeightAmp : 0.5 - 0.3 * uHeightAmp;
  float on = found * (1.0 - hole);
  return Surf(c, mix(uRough.y, uRough.x, on), mix(uMetal.y, uMetal.x, on), h, vec3(0));
}

// ---- 62 paper: fibers, crumples and folds, ruled or squared lines, age stains
// extra: x crumple (0 flat .. 1), y lines (0 none, 1 ruled, 2 squared, 3 dot grid), z fibers (0 smooth .. 1
//        coarse card or kraft), w age stains (0 .. 1). Colors: A the paper, B fibers and stains, C ink.
Surf genPaper(vec2 uv) {
  vec2 f = F();
  float n = pfbm(uv, f, OCT(), uSeed);
  float fib1 = pnoise(uv * vec2(256.0, 48.0), vec2(256.0, 48.0), uSeed + 3.0);
  float fib2 = pnoise(uv * vec2(48.0, 256.0), vec2(48.0, 256.0), uSeed + 5.0);
  float fib = max(smoothstep(0.62, 0.8, fib1), smoothstep(0.62, 0.8, fib2)) * uExtra.z;
  float fine = pnoise(uv * 384.0, vec2(384.0), uSeed + 7.0);
  float id, edge;
  vec2 rel = pvorRel(uv, f * 2.0, 1.0, uSeed + 9.0, id, edge);
  vec2 tl = vec2(fract(id * 7.1), fract(id * 3.3)) - 0.5;
  float crease = 1.0 - smoothstep(0.0, 0.03, edge);
  float fold = pridged(uv, f, 4, uSeed + 11.0);
  vec3 c = uColA * (1.0 + (n - 0.5) * uVariation * 0.5 + (fine - 0.5) * 0.06);
  c = mix(c, uColB, fib * 0.6);
  // lines in ink
  float L = floor(uExtra.y + 0.5);
  float ink = 0.0;
  if (L > 0.5) {
    float R = 32.0;
    vec2 q = uv * R;
    float aa = max(fwidth(q.y), 1e-4);
    if (L < 1.5) ink = lines1(q.y, 0.06, aa);
    else if (L < 2.5) ink = max(lines1(q.x, 0.05, aa), lines1(q.y, 0.05, aa));
    else ink = 1.0 - smoothstep(0.06 - aa, 0.06 + aa, length(fract(q) - 0.5));
    ink *= 0.85 + 0.15 * fine;
  }
  c = mix(c, uColC, ink * 0.85);
  // age: yellowing blotches and foxing spots
  float age = pfbm(uv, f * 0.5 + 1.0, 5, uSeed + 13.0);
  vec4 sp = pvoronoi(uv, f * 6.0, 1.0, uSeed + 15.0);
  float fox = (1.0 - smoothstep(0.05, 0.22, sp.x)) * step(0.8, sp.z);
  c = mix(c, uColB, clamp((age - 0.45) * 1.5 + fox * 0.7, 0.0, 1.0) * uExtra.w);
  float h = 0.5 + (dot(rel, tl) * 1.2 * uExtra.x - crease * 0.15 * uExtra.x + (fold - 0.5) * 0.5 * uExtra.x +
                   (fine - 0.5) * 0.08 + fib * 0.08 + (n - 0.5) * 0.1) * uHeightAmp;
  return Surf(c, clamp(uRough.x + (fine - 0.5) * 0.05 + ink * (uRough.y - uRough.x), 0.0, 1.0), uMetal.x, h, vec3(0));
}

// ================================================================ 0.26 generators (63..)

float sdEllipse(vec2 p, vec2 c, vec2 r, float ang) {
  vec2 d = rot2(-ang) * (p - c) / r;
  return (length(d) - 1.0) * min(r.x, r.y);
}
// A leaf: q.x across, q.y from the stalk (0) to the tip (1); > 0 inside (about the distance to the edge)
float leafShape(vec2 q, float wide) {
  float w = wide * pow(max(sin(PI * clamp(q.y, 0.0, 1.0)), 0.0), 0.7) * (1.0 - 0.25 * q.y);
  return min(w - abs(q.x), min(q.y, 1.0 - q.y) * wide);
}
// Ornaments for damask, devoré and lace: q in -0.5..0.5 (mirrored left to right); < 0 inside
float ornament(vec2 q, float style) {
  q.x = abs(q.x);
  float d = 1e3;
  float r = length(q), a = atan(q.x, q.y);
  if (style < 0.5) {  // damask medallion
    d = min(d, r - (0.13 + 0.035 * cos(a * 8.0)));
    d = max(d, -(0.035 - abs(r - 0.075)));  // a ring cut into the bloom
    d = min(d, r - 0.035);
    d = min(d, sdEllipse(q, vec2(0.0, 0.28), vec2(0.07, 0.14), 0.0));
    d = min(d, sdEllipse(q, vec2(0.0, -0.28), vec2(0.08, 0.12), 0.0));
    d = min(d, sdEllipse(q, vec2(0.17, 0.17), vec2(0.05, 0.15), -0.7));
    d = min(d, sdEllipse(q, vec2(0.17, -0.17), vec2(0.05, 0.15), 0.7));
    d = min(d, sdEllipse(q, vec2(0.31, 0.0), vec2(0.045, 0.11), 0.0));
    float arc = abs(length(q - vec2(0.22, 0.33)) - 0.1) - 0.018;
    if (q.y > 0.33 || q.x > 0.22) d = min(d, arc);
    float arc2 = abs(length(q - vec2(0.22, -0.33)) - 0.1) - 0.018;
    if (q.y < -0.33 || q.x > 0.22) d = min(d, arc2);
    d = min(d, length(q - vec2(0.0, 0.45)) - 0.03);
    d = min(d, length(q - vec2(0.42, 0.2)) - 0.025);
    d = min(d, length(q - vec2(0.42, -0.2)) - 0.025);
  } else if (style < 1.5) {  // floral trellis: onion-shaped (ogee) lines with a flower in each
    d = min(d, abs(q.x - (0.33 + 0.11 * cos(q.y * 2.0 * PI))) - 0.02);
    d = min(d, r - (0.11 + 0.04 * cos(a * 5.0)));
    d = max(d, -(r - 0.035));
    d = min(d, sdEllipse(q, vec2(0.0, 0.24), vec2(0.045, 0.1), 0.0));
    d = min(d, sdEllipse(q, vec2(0.12, -0.16), vec2(0.045, 0.1), 0.6));
    d = min(d, sdEllipse(q, vec2(0.15, 0.1), vec2(0.035, 0.08), -0.9));
  } else {  // fleur-de-lis
    d = min(d, sdEllipse(q, vec2(0.0, 0.12), vec2(0.065, 0.25), 0.0));
    d = min(d, sdEllipse(q, vec2(0.16, 0.08), vec2(0.055, 0.2), -0.6));
    d = min(d, sdEllipse(q, vec2(0.0, -0.1), vec2(0.23, 0.035), 0.0));
    d = min(d, sdEllipse(q, vec2(0.08, -0.25), vec2(0.04, 0.11), 0.45));
    d = min(d, length(q - vec2(0.28, 0.22)) - 0.03);
  }
  return d;
}
// Half-drop repeat: the cell coordinate (-0.5..0.5) of an ornament grid with N2 (even) columns per tile
vec2 halfDrop(vec2 uv, float N2, float sz) {
  vec2 p = uv * N2;
  float col = floor(p.x);
  return vec2(fract(p.x) - 0.5, fract(p.y + 0.5 * mod(col, 2.0)) - 0.5) / sz;
}

// ---- 63 fur: short dense fur lying one way, in clumps; A the coat, B the tips, C deep between the clumps
// extra: x hair length (0 = 1), y clumping (0 = 0.6), z wave, w lighter tips
Surf genFur(vec2 uv) {
  vec2 f = F();
  float L = uExtra.x > 0.0 ? uExtra.x : 1.0;
  float cl = uExtra.y > 0.0 ? clamp(uExtra.y, 0.0, 1.0) : 0.6;
  float wf = max(1.0, floor(f.x * 0.5 + 0.5));
  float sway = (pnoise(uv * vec2(wf, 2.0), vec2(wf, 2.0), uSeed + 3.0) - 0.5) * uExtra.z * 0.6 / wf;
  vec2 p = vec2(uv.x + sway, uv.y);
  vec2 cf = vec2(f.x * 3.0, max(1.0, floor(f.y * 2.0 / L + 0.5)));
  float clump = pnoise(p * cf, cf, uSeed + 5.0) * 0.7 + pnoise(p * cf * 2.0, cf * 2.0, uSeed + 6.0) * 0.3;
  vec2 sf = vec2(f.x * 48.0, max(1.0, floor(f.y * 3.0 / L + 0.5)));
  float st = pnoise(p * sf, sf, uSeed + 7.0) * 0.6 + pnoise(p * sf * 2.0, sf * 2.0, uSeed + 8.0) * 0.4;
  float cs = smoothstep(0.5 - 0.35 * cl, 0.65, clump);
  float t = clamp(cs * 0.45 * cl + (st - 0.5) * 1.1 + 0.55 - 0.25 * cl, 0.0, 1.0);
  vec3 c = mix(uColC, uColA, smoothstep(0.0, 0.45, t));
  c = mix(c, uColB, smoothstep(0.65, 1.0, t) * clamp(uExtra.w, 0.0, 1.0));
  c *= 1.0 + (st - 0.5) * uVariation;
  float h = 0.5 + ((cs - 0.5) * 0.6 + (st - 0.5) * 0.5) * uHeightAmp;
  gSheen = 0.5 + 0.5 * cs;
  return Surf(c, mix(uRough.y, uRough.x, t), uMetal.x, h, vec3(0));
}

// ---- 64 feathers: rows of overlapping feathers with a shaft and barbs; A the vane, B the shaft and the edges,
//      C the markings and the shadow under the row above
// extra: x feather length : width (0 = 2), y barbs along a feather (0 = 24), z markings (0 none, 1 bars,
//        2 dark tips, 3 eyes), w ruffle
Surf genFeathers(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float A = uExtra.x > 0.0 ? uExtra.x : 2.0;
  float R = 2.0 * max(1.0, floor(N / A + 0.5));  // rows per tile (even: the stagger repeats); a feather is two rows
  vec2 p = vec2(uv.x * N, uv.y * R);
  float nb = uExtra.y > 0.0 ? uExtra.y : 24.0;
  float mark = floor(uExtra.z + 0.5);
  float j0 = floor(p.y);
  float found = 0.0, out0 = 9.0;
  float fu = 0.0, fv = 0.0, fw = 1.0;
  vec2 key = vec2(0);
  for (int dj = 0; dj <= 1; ++dj) {  // the row a point is in lies over the row below it
    float j = j0 - float(dj);
    float off = mod(j, 2.0) * 0.5;
    float i0 = floor(p.x - off);
    for (int di = -1; di <= 1; ++di) {
      float i = i0 + float(di);
      vec2 k = vec2(mod(i, N), mod(j, R));
      float u = (p.x - (i + off + 0.5)) / 0.5;
      float v = (p.y - j) / 2.0;  // 0 tip .. 1 base
      float w = 0.92 * sqrt(clamp(v * 1.8, 0.0, 1.0)) * (1.0 - 0.12 * v) *
                (1.0 + (hash12(k + floor(v * 6.0) + uSeed) - 0.5) * 0.3 * uExtra.w);
      bool in_ = v >= 0.0 && v <= 1.0 && abs(u) < w;
      if (in_ && found < 0.5) { found = 1.0; fu = u; fv = v; fw = w; key = k; }
      if (!in_ && dj == 0) out0 = min(out0, max(abs(u) - w, 0.0) * 0.5 + max(-v, 0.0) * 2.0);
    }
    if (found > 0.5) {
      if (dj == 0) out0 = 9.0;
      break;
    }
  }
  float shadow = 1.0 - smoothstep(0.0, 0.12, out0);
  float aa = max(fwidth(p.y), 1e-4) * nb * 0.5;
  float shaft = 1.0 - smoothstep(0.035, 0.06, abs(fu) / (1.0 + fv));
  float barb = lines1(fv * nb + abs(fu) * nb * 0.55, 0.3, aa + 0.02);
  float edge = smoothstep(0.75, 1.0, abs(fu) / fw);
  float rnd = hash12(key + uSeed * 2.3);
  vec3 c = mix(uColA, uColB, edge * 0.5) * (1.0 + (rnd - 0.5) * uVariation);
  if (mark > 0.5 && mark < 1.5) c = mix(c, uColC, band(fv * 5.0 + rnd, 0.35, 0.05) * 0.75);
  else if (mark > 1.5 && mark < 2.5) c = mix(c, uColC, 1.0 - smoothstep(0.12, 0.3, fv));
  else if (mark > 2.5) {
    float e = length(vec2(fu * 0.9, (fv - 0.32) * 2.2));
    c = mix(c, uColB, 1.0 - smoothstep(0.42, 0.48, e));
    c = mix(c, uColC, 1.0 - smoothstep(0.26, 0.31, e));
    c = mix(c, uColA * 0.35, 1.0 - smoothstep(0.12, 0.16, e));
  }
  c *= 1.0 - barb * 0.18;
  c = mix(c, uColB, shaft * 0.8);
  c = mix(c, uColC * 0.6, shadow * 0.6);
  float h = 0.5 + ((1.0 - abs(fu) / fw) * 0.12 + (1.0 - fv) * 0.3 + shaft * 0.12 - barb * 0.05 - shadow * 0.2) * uHeightAmp;
  gSheen = 1.0 - shaft * 0.5;
  return Surf(c, mix(uRough.x, uRough.y, barb), uMetal.x, h, vec3(0));
}

// ---- 65 leaves: fallen leaves on the ground, one leaf (a card) or foliage with gaps; A and B the leaves (they
//      vary between the two), C the ground (or the gaps, cut out)
// extra: x style (0 fallen leaves, 1 one leaf, 2 foliage), y leaf width (0 = 0.35), z veins, w how much the
//        leaves vary toward B (autumn)
Surf genLeaves(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float style = floor(uExtra.x + 0.5);
  float wide = uExtra.y > 0.0 ? uExtra.y : 0.35;
  vec2 lq = vec2(0.0);
  float best = -1.0, lid = 0.0;
  if (style > 0.5 && style < 1.5) {
    vec2 q = vec2((uv.x - 0.5) * 1.1, uv.y * 1.1 - 0.07);
    float s = leafShape(q, wide * 1.25);
    if (s > 0.0) { lq = q; best = s; lid = 0.3; }
    if (abs(q.x) < 0.012 && q.y < 0.05 && q.y > -0.07) { lq = q; best = max(best, 0.006); }
  } else {
    float per = style < 0.5 ? 2.0 : 3.0;
    vec2 p = uv * N;
    vec2 ci = floor(p);
    float top = -1.0;
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x)
        for (int k = 0; k < 3; ++k) {
          if (float(k) >= per) continue;
          vec2 cell = mod(ci + vec2(x, y), N);
          vec3 r = hash33(vec3(cell, float(k) + uSeed * 1.7));
          vec2 o = ci + vec2(x, y) + r.xy;
          float ang = style < 0.5 ? r.z * 2.0 * PI : PI + (r.z - 0.5) * 1.4;  // foliage hangs, tips down
          float size = 0.85 + 0.5 * hash12(cell + float(k) * 3.1 + uSeed);
          vec2 d = rot2(-ang) * (p - o) / size;
          vec2 q = vec2(d.x, d.y + 0.5);
          float sh = leafShape(q, wide);
          float layer = hash12(cell * 1.3 + float(k) * 7.7 + uSeed);
          if (sh > 0.0 && layer > top) { top = layer; lq = q; best = sh; lid = layer; }
        }
  }
  float inside = smoothstep(0.0, 0.012, best);
  float mid = 1.0 - smoothstep(0.006, 0.02, abs(lq.x));
  float side = lines1((lq.y - abs(lq.x) * 0.9) * 9.0, 0.12, 0.08) * smoothstep(0.02, 0.05, abs(lq.x));
  float vein = clamp(mid + side * 0.7, 0.0, 1.0) * clamp(uExtra.z, 0.0, 1.0) * inside;
  float n = pfbm(uv, vec2(N * 4.0), 4, uSeed + 3.0);
  vec3 leaf = mix(uColA, uColB, clamp(lid * uExtra.w * 1.6 + (n - 0.5) * 0.4 * uExtra.w, 0.0, 1.0));
  leaf = mix(leaf, leaf * 1.35 + 0.04, vein);
  leaf *= 1.0 - (1.0 - smoothstep(0.0, 0.035, best)) * 0.25;
  vec3 c = mix(uColC * (0.85 + 0.3 * n), leaf, inside);
  c *= 1.0 + (n - 0.5) * uVariation;
  float dome = clamp(best * 6.0, 0.0, 1.0);
  float h = 0.5 + (inside * (0.12 + dome * 0.15 + lid * 0.2) - vein * 0.04 + (n - 0.5) * 0.08 - (1.0 - inside) * 0.1) * uHeightAmp;
  gHole = style > 0.5 ? 1.0 - inside : 0.0;
  return Surf(c, mix(uRough.x, uRough.y, 1.0 - inside), uMetal.x, h, vec3(0));
}

// ---- 66 rope: twisted strands of fiber, a braided cord or a coiled cable (lengthwise along V); A the fiber,
//      B the highlights, C the grooves between strands
// extra: x style (0 twisted, 1 braided, 2 coiled), y strands across a tile (0 = 3), z twist steepness (0 = 1),
//        w fuzz
Surf genRope(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float style = floor(uExtra.x + 0.5);
  float S = uExtra.y > 0.0 ? floor(uExtra.y + 0.5) : 3.0;
  float tw = uExtra.z > 0.0 ? floor(uExtra.z + 0.5) : 1.0;
  float t, along;
  float dip = 0.0;
  if (style < 0.5) {
    t = uv.x * S + uv.y * N * S * tw;
    along = uv.y * N * S * tw * 3.0 - uv.x * S;
  } else if (style < 1.5) {
    float a = uv.x * S + uv.y * N * S * tw, b = uv.x * S - uv.y * N * S * tw;
    bool aOver = mod(floor(a) + floor(b), 2.0) < 0.5;
    t = aOver ? a : b;
    along = aOver ? b : a;
    dip = 1.0 - sin(fract(along) * PI);  // the strand dips where it goes under
  } else {
    t = uv.y * N * S * 2.0 + uv.x;  // rings round the cable, with a little lead
    along = uv.x * 8.0;
  }
  float ft = fract(t);
  float prof = sin(ft * PI);
  float groove = 1.0 - smoothstep(0.0, 0.18, min(ft, 1.0 - ft));
  // a strand's number repeats with the tile: t grows by S across it (by 1 for the coils)
  float sid = mod(floor(t), style < 1.5 ? S : 1.0);
  float fib = hash12(vec2(floor(ft * 18.0 + 0.4 * sin(along * 2.0 * PI)), sid) + uSeed);
  float fuzz = pfbm(uv, vec2(N * 8.0), 3, uSeed + 4.0);
  vec3 c = mix(uColA, uColB, clamp(prof * 0.6 + (fib - 0.5) * 0.3 - dip * 0.3, 0.0, 1.0));
  c = mix(c, uColC, max(groove * 0.85, dip * 0.4));
  c *= 1.0 + (fib - 0.5) * uVariation + (fuzz - 0.5) * uExtra.w * 0.3;
  float h = 0.5 + (prof * 0.45 - groove * 0.2 - dip * 0.15 + (fib - 0.5) * 0.06 + (fuzz - 0.5) * 0.1 * uExtra.w) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, groove), uMetal.x, h, vec3(0));
}

// ---- 67 damask: an ornament on a half-drop repeat (wallpaper, brocade, damask cloth); A the ground, B the
//      ornament, C its outline
// extra: x style (0 damask medallion, 1 floral trellis, 2 fleur-de-lis), y ornament size (0 = 1), z outline
//        width (0 = 0.012), w woven fabric underneath
Surf genDamask(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float N2 = 2.0 * max(1.0, floor(N * 0.5 + 0.5));
  float sz = uExtra.y > 0.0 ? uExtra.y : 1.0;
  float ow = uExtra.z > 0.0 ? uExtra.z : 0.012;
  vec2 q = halfDrop(uv, N2, sz);
  float d = ornament(q, floor(uExtra.x + 0.5));
  float aa = max(fwidth(uv.x * N2), 1e-4) / sz;
  float inside = 1.0 - smoothstep(-aa, aa, d);
  float outline = 1.0 - smoothstep(ow - aa, ow + aa, abs(d));
  vec2 wv = printWeave(uv, uExtra.w);
  float n = pfbm(uv, vec2(N2 * 3.0), OCT(), uSeed);
  vec3 c = mix(uColA, uColB, inside);
  c = mix(c, uColC, outline * 0.8);
  c *= wv.x * (1.0 + (n - 0.5) * uVariation);
  float h = 0.5 + ((wv.y - 0.5) + inside * 0.15 + outline * 0.05 + (n - 0.5) * 0.05) * uHeightAmp;
  gSheen = mix(0.4, 1.0, inside);
  return Surf(c, mix(uRough.x, uRough.y, inside), mix(uMetal.x, uMetal.y, inside), h, vec3(0));
}

// ---- 68 ice: clear or frosted ice with cracks, trapped bubbles and frost; A the deep ice, B frost and bubbles,
//      C the cracks
// extra: x cracks, y bubbles, z frost, w crack depth (0 = 1)
Surf genIce(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  vec2 f = vec2(N);
  float n = pfbm(uv, f, OCT(), uSeed);
  float cloud = pfbm(uv, f * 0.5 + 1.0, 4, uSeed + 2.0);
  vec4 v1 = pvoronoi(uv, f, 0.9, uSeed + 3.0);
  vec4 v2 = pvoronoi(uv, f * 3.0, 0.9, uSeed + 5.0);
  float aa = max(fwidth(uv.x * N), 1e-4);
  float big = 1.0 - smoothstep(0.0, 0.02 + aa, v1.w);
  float fine = (1.0 - smoothstep(0.0, 0.012 + aa * 3.0, v2.w)) * smoothstep(0.45, 0.6, pfbm(uv, f * 2.0, 3, uSeed + 7.0));
  float crack = max(big, fine * 0.7) * clamp(uExtra.x, 0.0, 1.0);
  float bid, bedge;
  vec2 br = pvorRel(uv, f * 6.0, 0.9, uSeed + 9.0, bid, bedge);
  float brad = 0.05 + 0.18 * fract(bid * 17.3);
  float bub = (1.0 - smoothstep(brad - 0.02, brad, length(br))) * step(0.55, fract(bid * 5.1)) * clamp(uExtra.y, 0.0, 1.0);
  float rim = bub * smoothstep(brad * 0.5, brad, length(br));
  float frost = smoothstep(0.45, 0.8, pridged(uv, f * 2.0, 5, uSeed + 11.0) * 0.7 + cloud * 0.5) * clamp(uExtra.z, 0.0, 1.0);
  vec3 c = mix(uColA, uColA * 1.25 + 0.05, cloud * 0.5);
  c = mix(c, uColB, max(frost, rim * 0.8));
  c = mix(c, uColB * 0.9, bub * 0.35);
  c = mix(c, uColC, crack);
  float depth = uExtra.w > 0.0 ? uExtra.w : 1.0;
  float h = 0.5 + ((n - 0.5) * 0.15 - crack * 0.25 * depth + frost * 0.12 + bub * 0.03) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, max(frost, crack * 0.5)), uMetal.x, h, vec3(0));
}

// ---- 69 crosshatch: pen or pencil hatching on paper, darker where more layers of lines cross; A the paper,
//      B the ink, C the darkest ink
// extra: x line width (part of the spacing, 0 = 0.3), y layers (1 to 4, 0 = 3), z contrast, w hand-drawn wobble
Surf genCrosshatch(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float layers = uExtra.y > 0.0 ? clamp(floor(uExtra.y + 0.5), 1.0, 4.0) : 3.0;
  float lw = uExtra.x > 0.0 ? uExtra.x : 0.3;
  float L = N * 8.0;
  float tone = pfbm(uv, vec2(2.0), 4, uSeed);
  tone = clamp((tone - 0.5) * (2.0 + uExtra.z * 3.0) + 0.5, 0.0, 1.0);
  float wob = (pfbm(uv, vec2(N * 2.0), 3, uSeed + 4.0) - 0.5) * uExtra.w * 1.5;
  float ink = 0.0;
  for (int k = 0; k < 4; ++k) {
    if (float(k) >= layers) break;
    float th = (float(k) + 1.0) / (layers + 1.0);
    float on = smoothstep(th - 0.04, th + 0.04, tone);
    float t = k == 0 ? (uv.x + uv.y) * L : k == 1 ? (uv.x - uv.y) * L : k == 2 ? uv.y * L : uv.x * L;
    float aa = max(fwidth(t), 1e-4);
    float stroke = lines1(t + wob + hash12(vec2(mod(floor(t), L * 2.0), float(k))) * 0.15 * uExtra.w, lw, aa);
    stroke *= 0.75 + 0.25 * pnoise(uv * vec2(L * 0.25), vec2(L * 0.25), uSeed + float(k) * 3.0);  // pressure
    ink = max(ink, stroke * on);
  }
  float paper = pfbm(uv, vec2(N * 16.0), 3, uSeed + 9.0);
  vec3 c = uColA * (1.0 + (paper - 0.5) * 0.08);
  c = mix(c, mix(uColB, uColC, smoothstep(0.75, 1.0, tone)), ink);
  c *= 1.0 + (paper - 0.5) * uVariation;
  float h = 0.5 + ((paper - 0.5) * 0.1 - ink * 0.05) * uHeightAmp;
  return Surf(c, mix(uRough.x, uRough.y, ink), uMetal.x, h, vec3(0));
}

// ---- 70 lace: flowers and leaves of close-woven thread on a fine net, edged with a raised cord; the holes are
//      cut out (or show color C); A the thread, B the cord
// extra: x motif size (0 = 1), y net holes across a motif (0 = 10), z style (0 roses, 1 vines, 2 geometric),
//        w cord thickness (0 = 0.02)
Surf genLace(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float N2 = 2.0 * max(1.0, floor(N * 0.5 + 0.5));
  float style = floor(uExtra.z + 0.5);
  float sz = uExtra.x > 0.0 ? uExtra.x : 1.0;
  float fine = uExtra.y > 0.0 ? floor(uExtra.y + 0.5) : 10.0;
  float cw = uExtra.w > 0.0 ? uExtra.w : 0.02;
  vec2 q = halfDrop(uv, N2, sz);
  float r = length(q), a = atan(q.y, q.x);
  vec2 aq = abs(q);
  float d;
  if (style < 0.5) {
    d = r - (0.2 + 0.07 * cos(a * 5.0));
    d = max(d, -(0.018 - abs(r - 0.11)));  // open work: a ring of net inside the flower
    d = min(d, sdEllipse(aq, vec2(0.3, 0.3), vec2(0.05, 0.12), -0.785));
    d = min(d, r - 0.035);
  } else if (style < 1.5) {
    float stem = abs(q.x - 0.12 * sin(q.y * 2.0 * PI)) - 0.016;
    d = stem;
    for (int k = 0; k < 4; ++k) {
      float y = -0.375 + 0.25 * float(k);
      float sx = 0.12 * sin(y * 2.0 * PI);
      float sgn = mod(float(k), 2.0) < 0.5 ? 1.0 : -1.0;
      d = min(d, sdEllipse(q, vec2(sx + sgn * 0.09, y + 0.03), vec2(0.035, 0.085), -sgn * 0.9));
    }
  } else {
    d = abs(aq.x + aq.y - 0.3) - 0.035;
    d = min(d, abs(r - 0.14) - 0.025);
    d = min(d, r - 0.04);
  }
  float M = fine * N2;
  float t1 = (uv.x + uv.y) * M, t2 = (uv.x - uv.y) * M;
  float aa = max(fwidth(t1), 1e-4);
  float net = max(lines1(t1, 0.16, aa), lines1(t2, 0.16, aa));
  float aa2 = max(fwidth(uv.x * N2), 1e-4) / sz;
  float solid = 1.0 - smoothstep(-aa2, aa2, d);
  float cord = 1.0 - smoothstep(cw - aa2, cw + aa2, abs(d));
  float cloth = max(lines1(t1 * 3.0, 0.75, aa * 3.0), lines1(t2 * 3.0, 0.75, aa * 3.0));
  float thread = max(max(net, solid * max(cloth, 0.8)), cord);
  vec3 c = mix(uColA, uColB, cord);
  c *= (0.85 + 0.15 * thread) * (1.0 + (hash12(floor(vec2(t1, t2))) - 0.5) * uVariation * 0.3);
  c = mix(uColC, c, thread);
  float h = 0.5 + (thread * 0.15 + cord * 0.25 + solid * 0.08) * uHeightAmp;
  gHole = 1.0 - thread;
  return Surf(c, mix(uRough.y, uRough.x, thread), uMetal.x, h, vec3(0));
}

// ---- 71 net: fishnet, honeycomb tulle, square mesh or knotted netting; the holes are cut out (or show color C);
//      A the thread, B the knots
// extra: x thread width (part of a cell, 0 = 0.12), y style (0 fishnet, 1 honeycomb, 2 square mesh, 3 knotted
//        net), z knot size, w stretch (cells taller than wide)
Surf genNet(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float style = floor(uExtra.y + 0.5);
  float w = uExtra.x > 0.0 ? uExtra.x : 0.12;
  float st = 1.0 + max(uExtra.w, 0.0);
  float thread = 0.0, knot = 0.0, prof = 1.0;
  if (style < 0.5 || style > 2.5) {
    float rows = max(1.0, floor(N * st + 0.5));
    vec2 p = vec2(uv.x * N, uv.y * rows);
    float a = p.x + p.y, b = p.x - p.y;
    float aa = max(fwidth(a), 1e-4);
    float la = lines1(a, w * 1.4, aa), lb = lines1(b, w * 1.4, aa);
    thread = max(la, lb);
    float ga = floor(a + 0.5), gb = floor(b + 0.5);
    vec2 cr = vec2((ga + gb) * 0.5, (ga - gb) * 0.5);
    float ks = (style > 2.5 ? 1.5 : 1.1) * w * (1.0 + uExtra.z);
    knot = 1.0 - smoothstep(ks * 0.8, ks, length(p - cr));
    prof = max(sin(fract(a + 0.5) * PI), sin(fract(b + 0.5) * PI));
  } else if (style < 1.5) {
    float rows = max(1.0, floor(N / 1.732 * st + 0.5));
    vec2 p = vec2(uv.x * N, uv.y * rows * 1.732);
    vec2 s2 = vec2(1.0, 1.732);
    vec2 ha = mod(p, s2) - s2 * 0.5, hb = mod(p - s2 * 0.5, s2) - s2 * 0.5;
    vec2 g = dot(ha, ha) < dot(hb, hb) ? ha : hb;
    vec2 ag = abs(g);
    float e = 0.5 - max(dot(ag, vec2(0.5, 0.866)), ag.x);  // to the hexagon's edge
    float aa = max(fwidth(p.x), 1e-4);
    thread = 1.0 - smoothstep(w * 0.5 - aa, w * 0.5 + aa, e);
    knot = thread * (1.0 - smoothstep(0.0, 0.15, length(g) - 0.5)) * uExtra.z;
  } else {
    float rows = max(1.0, floor(N * st + 0.5));
    vec2 p = vec2(uv.x * N, uv.y * rows);
    float aa = max(fwidth(p.x), 1e-4);
    float lx = lines1(p.x, w, aa), ly = lines1(p.y, w, aa);
    bool xOver = mod(floor(p.x + 0.5) + floor(p.y + 0.5), 2.0) < 0.5;
    thread = max(lx, ly);
    prof = xOver ? mix(0.7, 1.0, lx) : mix(0.7, 1.0, ly);
  }
  float n = pfbm(uv, vec2(N * 2.0), 3, uSeed);
  vec3 c = mix(uColA, uColB, knot) * (0.75 + 0.25 * prof) * (1.0 + (n - 0.5) * uVariation);
  c = mix(uColC, c, max(thread, knot));
  float h = 0.5 + (max(thread, knot) * (0.15 + 0.15 * prof) + knot * 0.2) * uHeightAmp;
  gHole = 1.0 - max(thread, knot);
  return Surf(c, mix(uRough.y, uRough.x, thread), uMetal.x, h, vec3(0));
}

// ---- 72 hair cards: strands for hair cards, roots at the top (V = 1) and tapered tips at different lengths
//      lower down, gathered into clumps; between the strands is cut out (or color C); A the hair, B the tips and
//      the highlight
// extra: x strands across a card (0 = 40), y clumping (0 = 0.5), z how much shorter the short strands are
//        (0 = 0.35 of the card), w lighter tips. Scale: cards side by side in the tile
Surf genHairCards(vec2 uv) {
  float cards = max(1.0, floor(uScale + 0.5));
  float S = uExtra.x > 0.0 ? floor(uExtra.x + 0.5) : 40.0;
  float cl = uExtra.y > 0.0 ? uExtra.y : 0.5;
  float tipL = uExtra.z > 0.0 ? uExtra.z : 0.35;
  float cx = fract(uv.x * cards), card = floor(uv.x * cards);
  float K = max(1.0, floor(S / 6.0));
  float kc = (floor(cx * K) + 0.5) / K;
  float pinch = 1.0 - cl * pow(1.0 - uv.y, 1.5) * 0.7;
  float xl = kc + (cx - kc) / max(pinch, 0.15);
  float inClump = 1.0 - smoothstep(0.5 / K - 0.004, 0.5 / K, abs(xl - kc));
  float sx = xl * S;
  float si = floor(sx);
  vec3 r = hash33(vec3(mod(si, S), card, uSeed));
  float end = r.x * tipL;  // the strand ends at this V
  float taper = smoothstep(end, end + 0.12, uv.y);
  float fx = fract(sx) - 0.5 + (r.y - 0.5) * 0.25;
  float w = 0.42 * taper;
  float aa = max(fwidth(sx), 1e-4);
  float strand = (1.0 - smoothstep(w - aa, w + aa, abs(fx))) * inClump * step(end, uv.y);
  float sheenBand = 1.0 - smoothstep(0.0, 0.08, abs(uv.y - 0.62 - (r.z - 0.5) * 0.08));
  vec3 c = mix(uColA, uColB, clamp((1.0 - taper) * uExtra.w + sheenBand * 0.35, 0.0, 1.0));
  c *= (0.8 + 0.25 * cos(fx * PI * 1.6)) * (1.0 + (r.z - 0.5) * uVariation);
  c = mix(uColC, c, strand);
  float h = 0.5 + strand * (0.2 + 0.1 * cos(fx * PI)) * uHeightAmp;
  gHole = 1.0 - strand;
  return Surf(c, mix(uRough.y, uRough.x, strand), uMetal.x, h, vec3(0));
}

// ---- 73 velvet: soft pile that catches the light (give the material sheen); plain, crushed, corduroy wales or
//      devoré (a burnt-out pattern, sheer where the pile is gone); A the pile, B the lighter crushed pile, C the
//      sheer ground
// extra: x style (0 plain, 1 crushed, 2 corduroy, 3 devoré), y crush or pattern size (0 = 1), z pile grain,
//        w wales per tile (corduroy, 0 = 24)
Surf genVelvet(vec2 uv) {
  float N = max(1.0, floor(uScale + 0.5));
  float style = floor(uExtra.x + 0.5);
  float sz = uExtra.y > 0.0 ? uExtra.y : 1.0;
  vec2 f = vec2(max(1.0, floor(N / sz + 0.5)));
  float pile = pnoise(uv * vec2(256.0), vec2(256.0), uSeed + 1.0) * 0.5 + pnoise(uv * vec2(128.0), vec2(128.0), uSeed + 2.0) * 0.5;
  float crush = pfbm(uv, f, 5, uSeed + 3.0);
  float cr = style > 0.5 && style < 1.5 ? smoothstep(0.35, 0.65, crush + (pfbm(uv, f * 4.0, 3, uSeed + 5.0) - 0.5) * 0.4)
                                        : 0.3 + 0.2 * crush;
  float wale = 0.5, valley = 0.0;
  if (style > 1.5 && style < 2.5) {
    float W = uExtra.w > 0.0 ? floor(uExtra.w + 0.5) : 24.0;
    float ft = fract(uv.x * W);
    wale = sin(ft * PI);
    valley = 1.0 - smoothstep(0.0, 0.12, min(ft, 1.0 - ft));
  }
  float burnt = 0.0;
  if (style > 2.5) {
    float N2 = 2.0 * max(1.0, floor(N * 0.5 + 0.5));
    vec2 q = halfDrop(uv, N2, sz);
    float aa = max(fwidth(uv.x * N2), 1e-4) / sz;
    burnt = smoothstep(-aa, aa, ornament(q, 0.0));
  }
  vec3 c = mix(uColA, uColB, cr);
  c *= 1.0 + (pile - 0.5) * (0.1 + 0.3 * uExtra.z) * (1.0 + uVariation);
  c *= 1.0 - valley * 0.35;
  c = mix(c, uColC, burnt);
  float h = 0.5 + ((pile - 0.5) * 0.08 * (1.0 + uExtra.z) + (wale - 0.5) * 0.5 - valley * 0.1 - burnt * 0.25) * uHeightAmp;
  gSheen = (0.6 + 0.4 * cr) * (1.0 - burnt) * (1.0 - valley * 0.5);
  gHole = burnt * 0.6;
  return Surf(c, mix(uRough.x, uRough.y, burnt), uMetal.x, h, vec3(0));
}

Surf generate(vec2 uv) {
  switch (uGen) {
    case 63: return genFur(uv);
    case 64: return genFeathers(uv);
    case 65: return genLeaves(uv);
    case 66: return genRope(uv);
    case 67: return genDamask(uv);
    case 68: return genIce(uv);
    case 69: return genCrosshatch(uv);
    case 70: return genLace(uv);
    case 71: return genNet(uv);
    case 72: return genHairCards(uv);
    case 73: return genVelvet(uv);
    case 47: return genStripes(uv);
    case 48: return genChecks(uv);
    case 49: return genDots(uv);
    case 50: return genAnimal(uv);
    case 51: return genCrackedEarth(uv);
    case 52: return genFlagstone(uv);
    case 53: return genPebbles(uv);
    case 54: return genStrata(uv);
    case 55: return genCrackle(uv);
    case 56: return genChainmail(uv);
    case 57: return genDamascus(uv);
    case 58: return genCircuit(uv);
    case 59: return genStainedGlass(uv);
    case 60: return genMosaic(uv);
    case 61: return genSequins(uv);
    case 62: return genPaper(uv);
    case 19: return genDiamondPlate(uv);
    case 20: return genHammered(uv);
    case 21: return genCorrugated(uv);
    case 22: return genWoven(uv);
    case 23: return genBark(uv);
    case 24: return genTerrazzo(uv);
    case 25: return genSand(uv);
    case 26: return genMoss(uv);
    case 27: return genAsphalt(uv);
    case 28: return genCamo(uv);
    case 29: return genPerforated(uv);
    case 30: return genPanels(uv);
    case 31: return genBricks(uv);
    case 32: return genParquet(uv);
    case 33: return genHexTiles(uv);
    case 34: return genScales(uv);
    case 35: return genGrille(uv);
    case 36: return genQuilted(uv);
    case 37: return genKnit(uv);
    case 38: return genKnurling(uv);
    case 39: return genStucco(uv);
    case 40: return genTwill(uv);
    case 41: return genSkin(uv);
    case 42: return genHair(uv);
    case 43: return genBrushStrokes(uv);
    case 44: return genPaintedStone(uv);
    case 45: return genPaintedWood(uv);
    case 46: return genPaintedMetal(uv);
    case 1: return genTwoTone(uv);
    case 2: return genBrushed(uv);
    case 3: return genScratched(uv);
    case 4: return genRust(uv);
    case 5: return genConcrete(uv);
    case 6: return genWood(uv);
    case 7: return genStone(uv);
    case 8: return genLeather(uv);
    case 9: return genFabric(uv);
    case 10: return genTiles(uv);
    case 11: return genPlastic(uv);
    case 12: return genCarPaint(uv);
    case 13: return genDirt(uv);
    case 14: return genChippedPaint(uv);
    case 15: return genCarbon(uv);
    case 16: return genMarble(uv);
    case 17: return genHex(uv);
    case 18: return genEmissiveGrid(uv);
    default: return genFlat(uv);
  }
}

void main() {
  vec2 uv = vUV;
  if (uWarp.x > 0.0) {
    // wobble the pattern: offsets that repeat with the tile, so it still tiles
    vec2 wf = vec2(max(1.0, floor(uWarp.y + 0.5)));
    vec2 w = vec2(pfbm(uv, wf, 4, uSeed + 101.0), pfbm(uv, wf, 4, uSeed + 131.0)) - 0.5;
    uv = fract(uv + w * uWarp.x * 1.2 / wf.x);
  }
  gHole = 0.0;
  gSheen = 1.0;
  Surf s = generate(uv);
  // cut-out holes take the thread's color: mipmaps and filtering then don't fringe the edges with the hole color
  if (uCutout != 0) s.col = mix(s.col, uColA, clamp(gHole, 0.0, 1.0));
  oColor = vec4(clamp(s.col, 0.0, 1.0), 1.0);
  oRMH = vec4(clamp(s.rough, 0.0, 1.0), clamp(s.metal, 0.0, 1.0), clamp(s.h, 0.0, 1.0), 1.0);
  oEmissive = vec4(clamp(s.emis, 0.0, 1.0), 1.0);
  float op = uSurface.x * (uCutout != 0 ? 1.0 - clamp(gHole, 0.0, 1.0) : 1.0);
  oExtra = clamp(vec4(op, uSurface.y * gSheen, uSurface.z, uSurface.w), 0.0, 1.0);
}
#endif

#ifdef BAKE_FINISH
layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oRMH;
uniform sampler2D uColorIn;
uniform sampler2D uRMHIn;
uniform vec4 uGrime;  // sRGB color, amount
uniform vec4 uWear;   // sRGB color, amount
uniform float uHeightAmp;
uniform float uScale;
uniform float uSeed;

// mean height round uv within radius r (uv units), from a mip level that already averages most of it
float meanHeight(vec2 uv, float r) {
  float size = float(textureSize(uRMHIn, 0).x);
  float lod = max(0.0, log2(r * size * 0.5));
  float s = textureLod(uRMHIn, uv, lod).b;
  const float GA = 2.39996323;
  for (int i = 0; i < 12; ++i) {
    float t = (float(i) + 0.5) / 12.0;
    float a = float(i) * GA;
    s += textureLod(uRMHIn, uv + vec2(cos(a), sin(a)) * r * sqrt(t), lod).b;
  }
  return s / 13.0;
}

void main() {
  vec2 uv = vUV;
  vec4 col = texture(uColorIn, uv);
  vec4 rmh = texture(uRMHIn, uv);
  float h = rmh.b;
  // grooves against the mean height round them at a few sizes; edges only at the smallest (a flat top is level
  // with its surroundings, an edge stands above them)
  float m0 = meanHeight(uv, 0.004), m1 = meanHeight(uv, 0.012), m2 = meanHeight(uv, 0.035);
  float amp = max(uHeightAmp, 0.03);
  float low = clamp(((m0 - h) * 0.4 + (m1 - h) * 0.4 + (m2 - h) * 0.3) / amp * 5.0, 0.0, 1.0);
  float high = clamp(((h - m0) * 0.7 + (h - m1) * 0.3) / amp * 9.0, 0.0, 1.0);
  vec2 f = vec2(max(1.0, floor(uScale + 0.5)));
  float bn = pfbm(uv, f * 2.0, 5, uSeed + 61.0);  // breaks the grime and the wear up
  if (uGrime.w > 0.0) {
    float g = smoothstep(0.0, 1.0, low * (0.6 + uGrime.w * 1.4) + (bn - 0.5) * 0.6) * min(1.0, uGrime.w * 1.5);
    float film = (pfbm(uv, f, 4, uSeed + 67.0) - 0.4) * uGrime.w * 0.3;  // a thin film elsewhere
    g = clamp(max(g, film), 0.0, 1.0);
    col.rgb = mix(col.rgb, uGrime.rgb, g * 0.75) * (1.0 - low * uGrime.w * 0.2);
    rmh.r = mix(rmh.r, max(rmh.r, 0.85), g);
    rmh.g = mix(rmh.g, 0.0, g);
    rmh.b = mix(h, max(h, m1), g * 0.3);  // dirt fills the grooves a little
  }
  if (uWear.w > 0.0) {
    float e = smoothstep(0.0, 1.0, high * (0.5 + uWear.w * 1.5) + (bn - 0.5) * 0.8) * min(1.0, uWear.w * 1.5);
    col.rgb = mix(col.rgb, uWear.rgb, e * 0.85);
    rmh.r = mix(rmh.r, rmh.r * 0.6, e);
  }
  oColor = vec4(clamp(col.rgb, 0.0, 1.0), 1.0);
  oRMH = vec4(clamp(rmh.rgb, 0.0, 1.0), 1.0);
}
#endif

#ifdef BAKE_NORMAL
out vec4 oNormal;
uniform sampler2D uRMH;
uniform float uNormalStrength;
void main() {
  ivec2 size = textureSize(uRMH, 0);
  ivec2 p = ivec2(gl_FragCoord.xy);
  // wrap for tileability
  float l = texelFetch(uRMH, (p + ivec2(-1, 0) + size) % size, 0).b;
  float r = texelFetch(uRMH, (p + ivec2(1, 0)) % size, 0).b;
  float d = texelFetch(uRMH, (p + ivec2(0, -1) + size) % size, 0).b;
  float u = texelFetch(uRMH, (p + ivec2(0, 1)) % size, 0).b;
  float k = uNormalStrength * float(size.x) / 256.0;
  vec3 n = normalize(vec3((l - r) * k, (d - u) * k, 1.0));
  oNormal = vec4(encodeNormal(n), 1.0);
}
#endif
