// 2D texture / UV editor background.
#include "common.glsl"
in vec2 vUV;
out vec4 oColor;
uniform vec4 uView;          // xy scale, zw offset: screenUV = uv * scale + offset
uniform sampler2D uTex;
uniform int uMode;           // 0 rgb, 1 r, 2 g, 3 b, 4 a, 5 normal(signed), 6 checker only, 7 r as mask
uniform float uDim;          // darken texture so wireframe reads
uniform vec2 uViewportPx;
uniform int uTiling;         // show repeated tiles outside 0..1
uniform vec3 uAtlasCell = vec3(0.0, 0.0, 1.0);  // texture tiles: the shown tile's cell in the atlas (origin, size)

void main() {
  vec2 uv = (vUV - uView.zw) / uView.xy;
  float chk = mod(floor(uv.x * 16.0) + floor(uv.y * 16.0), 2.0);
  vec3 bg = mix(vec3(0.16), vec3(0.19), chk);
  bool inside = all(greaterThanEqual(uv, vec2(0))) && all(lessThanEqual(uv, vec2(1)));
  vec3 c;
  vec4 t = texture(uTex, uAtlasCell.xy + (uAtlasCell.z < 1.0 ? fract(uv) : uv) * uAtlasCell.z);
  if (uMode == 0) c = t.rgb;
  else if (uMode == 1 || uMode == 7) c = vec3(t.r);
  else if (uMode == 2) c = vec3(t.g);
  else if (uMode == 3) c = vec3(t.b);
  else if (uMode == 4) c = vec3(t.a);
  else if (uMode == 5) c = encodeNormal(normalize(t.xyz));
  else c = bg;
  if (uMode == 6) c = vec3(0.32 + 0.1 * chk);
  c *= uDim;
  if (!inside) {
    c = uTiling == 1 ? mix(vec3(0.08), c, 0.25) : vec3(0.075, 0.078, 0.085);
  }
  // unit square border + grid
  vec2 g = abs(fract(uv * 8.0 + 0.5) - 0.5) / fwidth(uv * 8.0);
  float grid = 1.0 - clamp(min(g.x, g.y), 0.0, 1.0);
  c = mix(c, vec3(0.3, 0.32, 0.36), grid * (inside ? 0.12 : 0.25));
  vec2 b = abs(uv - 0.5);
  vec2 bw = fwidth(uv) * 1.5;
  float border = (abs(b.x - 0.5) < bw.x && b.y <= 0.5 + bw.y) || (abs(b.y - 0.5) < bw.y && b.x <= 0.5 + bw.x) ? 1.0 : 0.0;
  c = mix(c, vec3(0.9, 0.6, 0.2), border * 0.8);
  oColor = vec4(c, 1.0);
}
