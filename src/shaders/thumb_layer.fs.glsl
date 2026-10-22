// Layer list thumbnails: box-filtered downsample of a layer texture.
in vec2 vUV;
out vec4 oColor;
uniform sampler2D uSrc;
uniform int uMode;      // 0 color with coverage over a checker, 1 mask (r) as gray
uniform vec2 uTexel;    // source texel size in UV

void main() {
  vec4 acc = vec4(0.0);
  // 6x6 taps across this thumbnail texel's footprint
  vec2 fp = vec2(dFdx(vUV.x), dFdy(vUV.y));
  for (int j = 0; j < 6; ++j)
    for (int i = 0; i < 6; ++i) {
      vec4 t = textureLod(uSrc, vUV + (vec2(i, j) - 2.5) / 6.0 * fp, 0.0);
      acc += uMode == 1 ? t : vec4(t.rgb * t.a, t.a);  // paint layers store straight color + coverage
    }
  acc /= 36.0;
  if (uMode == 1) {
    oColor = vec4(vec3(acc.r), 1.0);
    return;
  }
  vec2 c = floor(vUV * 8.0);
  vec3 checker = mod(c.x + c.y, 2.0) < 1.0 ? vec3(0.32) : vec3(0.22);
  vec3 col = acc.a > 1e-4 ? acc.rgb / acc.a : vec3(0.0);
  oColor = vec4(mix(checker, clamp(col, 0.0, 1.0), clamp(acc.a, 0.0, 1.0)), 1.0);
}
