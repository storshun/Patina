// Procedural brush alpha (tip) generator. Output: r = alpha over the dab square.
#include "common.glsl"
#include "noise2p.glsl"

in vec2 vUV;
out vec4 oAlpha;
uniform int uShape;
uniform float uSeed;
uniform vec4 uParams;  // shape specific

float sdStar(vec2 p, float r, int n, float m) {
  float an = PI / float(n), en = PI / m;
  vec2 acs = vec2(cos(an), sin(an)), ecs = vec2(cos(en), sin(en));
  float bn = mod(atan(p.x, p.y), 2.0 * an) - an;
  p = length(p) * vec2(cos(bn), abs(sin(bn)));
  p -= r * acs;
  p += ecs * clamp(-dot(p, ecs), 0.0, r * acs.y / ecs.y);
  return length(p) * sign(p.x);
}

void main() {
  vec2 p = vUV * 2.0 - 1.0;  // -1..1
  float r = length(p);
  float a = 0.0;
  float circle = 1.0 - smoothstep(0.96, 1.0, r);
  if (uShape == 0) {
    a = circle;  // round (falloff applied by hardness at paint time)
  } else if (uShape == 1) {
    vec2 q = abs(p);
    a = 1.0 - smoothstep(0.92, 0.98, max(q.x, q.y));  // square
  } else if (uShape == 2) {
    a = 1.0 - smoothstep(-0.02, 0.02, sdStar(p, 0.9, 5, 2.5));  // star
  } else if (uShape == 3) {
    // splatter: blob + droplets
    float n = pfbm(vUV, vec2(4.0), 5, uSeed);
    float blob = 1.0 - smoothstep(0.35, 0.45, r + (n - 0.5) * 0.5);
    float drops = 0.0;
    for (int i = 0; i < 24; ++i) {
      vec2 h = hash22(vec2(float(i), uSeed));
      float ang = h.x * 6.2831, dist = 0.45 + h.y * 0.5;
      vec2 c = vec2(cos(ang), sin(ang)) * dist;
      float rad = 0.015 + 0.06 * hash12(vec2(uSeed, float(i)));
      drops = max(drops, 1.0 - smoothstep(rad * 0.7, rad, length(p - c)));
    }
    a = max(blob, drops) * circle;
  } else if (uShape == 4) {
    // noise / chalk: fbm thresholded inside a soft circle
    float n = pfbm(vUV, vec2(uParams.x > 0.0 ? uParams.x : 8.0), 5, uSeed);
    a = smoothstep(0.42, 0.62, n + (1.0 - r) * 0.25) * circle;
  } else if (uShape == 5) {
    // scratches: a few thin random lines
    for (int i = 0; i < 9; ++i) {
      vec2 h = hash22(vec2(float(i) * 3.1, uSeed));
      float ang = 0.35 * (hash12(vec2(uSeed, float(i))) - 0.5) + 0.6;
      vec2 dir = vec2(cos(ang), sin(ang));
      vec2 c = (h - 0.5) * 1.2;
      vec2 d = p - c;
      float t = clamp(dot(d, dir), -0.8, 0.8);
      float w = 0.008 + 0.02 * hash12(vec2(float(i), uSeed * 2.0));
      a = max(a, (1.0 - smoothstep(w * 0.5, w, length(d - dir * t))) * (1.0 - abs(t) / 0.8));
    }
    a *= circle;
  } else if (uShape == 6) {
    // grunge: ridged noise blots
    float n = pridged(vUV, vec2(5.0), 6, uSeed);
    float m = pfbm(vUV, vec2(3.0), 4, uSeed + 4.0);
    a = smoothstep(0.35, 0.75, n * m * 1.6) * (1.0 - smoothstep(0.6, 1.0, r));
  } else if (uShape == 7) {
    // dots / stipple
    vec4 v = pvoronoi(vUV, vec2(uParams.x > 0.0 ? uParams.x : 10.0), 1.0, uSeed);
    a = (1.0 - smoothstep(0.18, 0.3, v.x)) * step(0.4, v.z) * circle;
  } else if (uShape == 8) {
    // hatch lines
    vec2 q = rot2(0.785) * p;
    float l = abs(fract(q.x * (uParams.x > 0.0 ? uParams.x : 6.0)) - 0.5);
    a = (1.0 - smoothstep(0.12, 0.2, l)) * circle;
  } else if (uShape == 9) {
    // cracks
    vec4 v = pvoronoi(vUV, vec2(uParams.x > 0.0 ? uParams.x : 5.0), 1.0, uSeed);
    a = (1.0 - smoothstep(0.0, 0.035, v.w)) * (1.0 - smoothstep(0.5, 1.0, r));
  } else if (uShape == 10) {
    // bristle / dry brush: streaks along x
    float streak = pnoise(vec2(vUV.x * 2.0, vUV.y * 48.0), vec2(2.0, 48.0), uSeed);
    float breakup = pfbm(vUV, vec2(6.0), 4, uSeed + 2.0);
    a = smoothstep(0.45, 0.6, streak * 0.7 + breakup * 0.5) * (1.0 - smoothstep(0.8, 1.0, abs(p.y))) *
        (1.0 - smoothstep(0.85, 1.0, abs(p.x)));
  } else if (uShape == 11) {
    // spray: random sparse speckles with radial density
    vec4 v = pvoronoi(vUV, vec2(40.0), 1.0, uSeed);
    float dens = 1.0 - r;
    a = (1.0 - smoothstep(0.15, 0.3, v.x)) * step(1.0 - dens * dens, v.z) * circle;
  } else if (uShape == 12) {
    // leaf / ellipse with vein
    vec2 q = vec2(p.x, p.y * 2.2);
    float e = 1.0 - smoothstep(0.85, 0.95, length(q) + abs(p.x) * 0.1);
    float vein = 1.0 - smoothstep(0.0, 0.03, abs(p.y));
    a = e * (1.0 - vein * 0.6);
  } else if (uShape == 13) {
    // ring
    a = (1.0 - smoothstep(0.08, 0.14, abs(r - 0.75))) ;
  } else if (uShape == 14) {
    // cloud / soft noise
    float n = pfbm(vUV, vec2(3.0), 6, uSeed);
    a = smoothstep(0.3, 0.8, n) * (1.0 - smoothstep(0.3, 1.0, r));
  } else if (uShape == 15) {
    // rust spots
    float n = pfbm(vUV, vec2(6.0), 6, uSeed);
    vec4 v = pvoronoi(vUV, vec2(7.0), 1.0, uSeed + 3.0);
    a = smoothstep(0.5, 0.65, n + (1.0 - v.x) * 0.3) * (1.0 - smoothstep(0.55, 1.0, r));
  }
  oAlpha = vec4(clamp(a, 0.0, 1.0));
}
