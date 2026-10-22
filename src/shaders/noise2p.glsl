// Periodic (tileable) 2D noise. All functions tile over uv in [0,1] when the
// period/frequency arguments are integers.

vec2 grad2(vec2 cell, float seed) {
  float a = hash12(cell + seed * 17.13) * 6.2831853;
  return vec2(cos(a), sin(a));
}

// Gradient noise with integer period `per`, input p in [0, per)
float pnoise(vec2 p, vec2 per, float seed) {
  vec2 i = floor(p), f = fract(p);
  vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  vec2 i00 = mod(i, per), i10 = mod(i + vec2(1, 0), per), i01 = mod(i + vec2(0, 1), per), i11 = mod(i + vec2(1, 1), per);
  float a = dot(grad2(i00, seed), f);
  float b = dot(grad2(i10, seed), f - vec2(1, 0));
  float c = dot(grad2(i01, seed), f - vec2(0, 1));
  float d = dot(grad2(i11, seed), f - vec2(1, 1));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y) * 0.7071 + 0.5;  // ~[0,1]
}

// fBm over uv in [0,1]; base frequency `freq` (integer per axis)
float pfbm(vec2 uv, vec2 freq, int oct, float seed) {
  freq = max(floor(freq + 0.5), vec2(1.0));  // whole periods per tile, or the tile edge shows a seam
  float s = 0.0, a = 0.5, n = 0.0;
  for (int i = 0; i < oct; ++i) {
    s += a * pnoise(uv * freq, freq, seed + float(i) * 3.1);
    n += a;
    freq *= 2.0;
    a *= 0.5;
  }
  return s / n;
}

// Ridged fbm (cracks, veins)
float pridged(vec2 uv, vec2 freq, int oct, float seed) {
  float s = 0.0, a = 0.5, n = 0.0;
  for (int i = 0; i < oct; ++i) {
    float v = 1.0 - abs(pnoise(uv * freq, freq, seed + float(i) * 5.7) * 2.0 - 1.0);
    s += a * v * v;
    n += a;
    freq *= 2.0;
    a *= 0.5;
  }
  return s / n;
}

// Periodic Voronoi: returns x = F1 distance, y = F2 distance, z = cell id hash, w = edge distance approx
vec4 pvoronoi(vec2 uv, vec2 freq, float jitter, float seed) {
  vec2 p = uv * freq;
  vec2 i = floor(p), f = fract(p);
  float f1 = 8.0, f2 = 8.0, id = 0.0;
  vec2 mr = vec2(0), mg = vec2(0);
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      vec2 g = vec2(x, y);
      vec2 c = mod(i + g, freq);
      vec2 o = hash22(c + seed * 7.7) * jitter + (1.0 - jitter) * 0.5;
      vec2 r = g + o - f;
      float d = dot(r, r);
      if (d < f1) {
        f2 = f1;
        f1 = d;
        id = hash12(c + seed * 3.3);
        mr = r;
        mg = g;
      } else if (d < f2) {
        f2 = d;
      }
    }
  // distance to the cell border (Inigo Quilez)
  float md = 8.0;
  for (int y = -2; y <= 2; ++y)
    for (int x = -2; x <= 2; ++x) {
      vec2 g = mg + vec2(x, y);
      vec2 c = mod(i + g, freq);
      vec2 o = hash22(c + seed * 7.7) * jitter + (1.0 - jitter) * 0.5;
      vec2 r = g + o - f;
      if (dot(mr - r, mr - r) > 1e-5) md = min(md, dot(0.5 * (mr + r), normalize(r - mr)));
    }
  return vec4(sqrt(f1), sqrt(f2), id, md);
}
