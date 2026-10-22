// Jump flood: finds, for every texel, the nearest texel covered by the UV layout.
// Output RG = coordinates (texel units) of the nearest covered texel, (-1,-1) if none yet.
in vec2 vUV;
out vec4 oSeed;
uniform sampler2D uCoverage;   // used when uInit == 1 (alpha = coverage)
uniform sampler2D uPrev;
uniform int uInit;
uniform int uStep;

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 size = textureSize(uPrev, 0);
  if (uInit == 1) {
    float c = texelFetch(uCoverage, p, 0).a;
    oSeed = c > 0.5 ? vec4(vec2(p), 0.0, 0.0) : vec4(-1.0, -1.0, 0.0, 0.0);
    return;
  }
  vec4 best = texelFetch(uPrev, p, 0);
  float bestD = best.x >= 0.0 ? dot(best.xy - vec2(p), best.xy - vec2(p)) : 1e30;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      ivec2 q = p + ivec2(x, y) * uStep;
      if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, size))) continue;
      vec4 s = texelFetch(uPrev, q, 0);
      if (s.x < 0.0) continue;
      float d = dot(s.xy - vec2(p), s.xy - vec2(p));
      if (d < bestD) {
        bestD = d;
        best = s;
      }
    }
  oSeed = best;
}
