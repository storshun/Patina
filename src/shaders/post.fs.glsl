// Small fullscreen utility passes.
#include "common.glsl"
in vec2 vUV;
out vec4 oColor;

#ifdef POST_AO_RESOLVE
uniform sampler2D uAccum;  // r = sum(vis*cos), g = sum(cos)
uniform float uStrength;
void main() {
  vec4 a = texelFetch(uAccum, ivec2(gl_FragCoord.xy), 0);
  float ao = a.g > 1e-4 ? a.r / a.g : 1.0;
  ao = pow(clamp(ao, 0.0, 1.0), uStrength);
  oColor = vec4(ao, ao, ao, 1.0);
}
#endif

#ifdef POST_MIP_DOWN
// (0.44.2) a region of one mip level from the level above it (bound as the base level): a 2x2 box filter, as
// glGenerateMipmap does, so a stroke updates the region it painted instead of every level of the whole atlas
uniform sampler2D uSrc;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) * 2;
  ivec2 m = textureSize(uSrc, 0) - 1;
  oColor = 0.25 * (texelFetch(uSrc, min(p, m), 0) + texelFetch(uSrc, min(p + ivec2(1, 0), m), 0) +
                   texelFetch(uSrc, min(p + ivec2(0, 1), m), 0) + texelFetch(uSrc, min(p + ivec2(1, 1), m), 0));
}
#endif

#ifdef POST_DILATE
// Gutter fill for any single texture using the jump-flood seed map
uniform sampler2D uSrc;
uniform sampler2D uSeed;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 s = texelFetch(uSeed, p, 0);
  oColor = texelFetch(uSrc, s.x >= 0.0 ? ivec2(s.xy) : p, 0);
}
#endif

#ifdef POST_LOOKUP
// Resample through a per-texel lookup of source UVs (topology transfer); x < 0 = none
uniform sampler2D uSrc;
uniform sampler2D uLookup;
void main() {
  vec2 l = texelFetch(uLookup, ivec2(gl_FragCoord.xy), 0).xy;
  oColor = l.x < 0.0 ? vec4(0.0) : texture(uSrc, l);
}
#endif

#ifdef POST_BAKEDOWN
// (1.1) The bake-down: a layer texture of the high-poly model resampled onto the low-poly model's texels, every
// point of a texel averaged (uLook0..7: two points each, the high's atlas UV; exactly (1, 1) = none). Where a value
// has a coverage (a paint channel's last component) the points are weighted by it, so paint next to an empty texel
// keeps its color instead of fading to black. uMode: 0 plain (masks), 1 coverage in g (one-value channels, a baked
// map's fixes), 2 coverage in a (colors), 3 a normal (coverage in a), turned from the high's surface frame (uHighN,
// uHighT) into the low's (uNrmMap, uTanMap): over the high's normal when the high's shape is baked into the low's
// normal map (uOverHigh), else over the low's own.
uniform sampler2D uSrc;
uniform sampler2D uLook0, uLook1, uLook2, uLook3, uLook4, uLook5, uLook6, uLook7;
uniform int uPairs;
uniform int uMode;
uniform sampler2D uHighN, uHighT, uNrmMap, uTanMap;
uniform int uOverHigh;
vec4 bdSum = vec4(0.0);
float bdCov = 0.0, bdW = 0.0;
void bdTap(vec2 uv) {
  if (uv.x >= 1.0 && uv.y >= 1.0) return;
  ivec2 size = textureSize(uSrc, 0);
  vec2 f = uv * vec2(size) - 0.5;
  ivec2 i = ivec2(floor(f));
  vec2 t = f - vec2(i);
  for (int k = 0; k < 4; ++k) {
    ivec2 o = ivec2(k & 1, k >> 1);
    float w = (o.x == 1 ? t.x : 1.0 - t.x) * (o.y == 1 ? t.y : 1.0 - t.y);
    vec4 c = texelFetch(uSrc, clamp(i + o, ivec2(0), size - 1), 0);
    float cov = uMode == 0 ? 1.0 : uMode == 1 ? c.g : c.a;
    bdSum += w * cov * c;
    bdCov += w * cov;
    bdW += w;
  }
}
void bdPair(sampler2D l) {
  vec4 v = texelFetch(l, ivec2(gl_FragCoord.xy), 0);
  bdTap(v.xy);
  bdTap(v.zw);
}
void main() {
  if (uPairs > 0) bdPair(uLook0);
  if (uPairs > 1) bdPair(uLook1);
  if (uPairs > 2) bdPair(uLook2);
  if (uPairs > 3) bdPair(uLook3);
  if (uPairs > 4) bdPair(uLook4);
  if (uPairs > 5) bdPair(uLook5);
  if (uPairs > 6) bdPair(uLook6);
  if (uPairs > 7) bdPair(uLook7);
  if (bdW <= 0.0) {  // no point of this texel found the high surface (or it is a gutter texel)
    oColor = uMode == 3 ? vec4(0.5, 0.5, 1.0, 0.0) : vec4(0.0);
    return;
  }
  if (uMode == 0) {
    oColor = bdSum / bdW;
    return;
  }
  float cov = bdCov / bdW;
  vec4 v = bdCov > 1e-6 ? bdSum / bdCov : vec4(0.0);
  if (uMode == 1) {
    oColor = vec4(v.r, cov, 0.0, 0.0);
    return;
  }
  if (uMode == 2 || bdCov <= 1e-6) {
    oColor = uMode == 3 && bdCov <= 1e-6 ? vec4(0.5, 0.5, 1.0, 0.0) : vec4(v.rgb, cov);
    return;
  }
  // a normal: from the high's tangent space to the low's
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec3 d = normalize(decodeNormal(v.rgb));
  vec4 hn = texelFetch(uHighN, p, 0), ht = texelFetch(uHighT, p, 0);
  vec4 ln = texelFetch(uNrmMap, p, 0), lt = texelFetch(uTanMap, p, 0);
  if (hn.w > 0.5 && dot(ln.xyz, ln.xyz) > 0.25) {
    vec3 Nh = normalize(hn.xyz);
    vec3 Th = normalize(ht.xyz - Nh * dot(Nh, ht.xyz) + vec3(1e-12, 0.0, 0.0));
    vec3 Bh = cross(Nh, Th) * (ht.w < 0.0 ? -1.0 : 1.0);
    vec3 w = Th * d.x + Bh * d.y + Nh * d.z;
    vec3 Nl = normalize(ln.xyz);
    vec3 z = uOverHigh == 1 ? Nh : Nl;
    vec3 x = lt.xyz - z * dot(z, lt.xyz);
    x = dot(x, x) > 1e-12 ? normalize(x) : normalize(cross(z, abs(z.x) < 0.9 ? vec3(1, 0, 0) : vec3(0, 1, 0)));
    vec3 y = cross(z, x) * (lt.w < 0.0 ? -1.0 : 1.0);
    d = normalize(vec3(dot(w, x), dot(w, y), dot(w, z)));
  }
  oColor = vec4(encodeNormal(d), cov);
}
#endif

#ifdef POST_BLUR
// Separable box/gauss blur used for curvature smoothing
uniform sampler2D uSrc;
uniform vec2 uDirPx;
void main() {
  vec2 ts = 1.0 / vec2(textureSize(uSrc, 0));
  vec4 s = vec4(0);
  float w[5] = float[](0.227, 0.194, 0.121, 0.054, 0.016);
  s += texture(uSrc, vUV) * w[0];
  for (int i = 1; i < 5; ++i) {
    s += texture(uSrc, vUV + uDirPx * ts * float(i)) * w[i];
    s += texture(uSrc, vUV - uDirPx * ts * float(i)) * w[i];
  }
  oColor = s;
}
#endif

#ifdef POST_EXPORT
// Converts final maps to export layouts. uOut selects which packed map.
uniform sampler2D uColor;     // sRGB
uniform sampler2D uRMH;       // rough metal height ao
uniform sampler2D uNormal;    // signed
uniform sampler2D uEmissive;
uniform int uOut;  // 0 color,1 rough,2 metal,3 height,4 normalGL,5 normalDX,6 emissive,7 ao,8 ORM,9 glTF MR (g rough b metal),10 unity mask (r metal g ao b 0 a smooth),11 smoothness,
                   // 12 thickness, 13 bent normal (tangent, GL), 14 bent normal (tangent, DX),
                   // 15 opacity, 16 sheen, 17 clearcoat, 18 subsurface, 19 color with opacity in alpha
uniform sampler2D uExtra;  // opacity, sheen, clearcoat, subsurface
uniform sampler2D uThick;
uniform sampler2D uBent;     // object space
uniform sampler2D uNrmMap;
uniform sampler2D uTanMap;
vec3 objectToTangent(ivec2 p, vec3 v) {
  vec3 N = texelFetch(uNrmMap, p, 0).xyz;
  vec4 tt = texelFetch(uTanMap, p, 0);
  if (dot(N, N) < 0.25 || dot(v, v) < 0.25) return vec3(0.0, 0.0, 1.0);
  N = normalize(N);
  vec3 T = tt.xyz - N * dot(N, tt.xyz);
  T = dot(T, T) > 1e-10 ? normalize(T) : vec3(1.0, 0.0, 0.0);
  vec3 B = cross(N, T) * (tt.w < 0.0 ? -1.0 : 1.0);
  v = normalize(v);
  return normalize(vec3(dot(v, T), dot(v, B), dot(v, N)));
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec3 c = texelFetch(uColor, p, 0).rgb;
  vec4 rmh = texelFetch(uRMH, p, 0);
  vec3 n = normalize(texelFetch(uNormal, p, 0).xyz);
  vec3 e = texelFetch(uEmissive, p, 0).rgb;
  vec4 o = vec4(0, 0, 0, 1);
  if (uOut == 0) o.rgb = c;
  else if (uOut == 1) o.rgb = vec3(rmh.r);
  else if (uOut == 2) o.rgb = vec3(rmh.g);
  else if (uOut == 3) o.rgb = vec3(rmh.b);
  else if (uOut == 4) o.rgb = encodeNormal(n);
  else if (uOut == 5) o.rgb = encodeNormal(vec3(n.x, -n.y, n.z));
  else if (uOut == 6) o.rgb = e;
  else if (uOut == 7) o.rgb = vec3(rmh.a);
  else if (uOut == 8) o.rgb = vec3(rmh.a, rmh.r, rmh.g);
  else if (uOut == 9) o.rgb = vec3(1.0, rmh.r, rmh.g);
  else if (uOut == 10) o = vec4(rmh.g, rmh.a, 0.0, 1.0 - rmh.r);
  else if (uOut == 11) o.rgb = vec3(1.0 - rmh.r);
  else if (uOut == 12) o.rgb = vec3(texelFetch(uThick, p, 0).r);
  else if (uOut >= 15 && uOut <= 18) o.rgb = vec3(texelFetch(uExtra, p, 0)[uOut - 15]);
  else if (uOut == 19) o = vec4(c, texelFetch(uExtra, p, 0).r);
  else if (uOut == 13 || uOut == 14) {
    vec3 b = objectToTangent(p, texelFetch(uBent, p, 0).xyz);
    if (uOut == 14) b.y = -b.y;
    o.rgb = encodeNormal(b);
  }
  oColor = o;
}
#endif

#ifdef POST_BASE_NORMAL
// Converts the object-space normal baked from a high-poly mesh into the tangent space
// of the low-poly mesh (same frame as pbr.fs and the exported normal maps).
uniform sampler2D uHighNormal;
uniform sampler2D uNrmMap;
uniform sampler2D uTanMap;
uniform sampler2D uAccNormal;  // the layer stack's tangent normal (read and written per texel)
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 acc = texelFetch(uAccNormal, p, 0);
  vec3 h = texelFetch(uHighNormal, p, 0).xyz;
  vec4 nn = texelFetch(uNrmMap, p, 0);
  vec4 tt = texelFetch(uTanMap, p, 0);
  if (dot(h, h) < 0.25 || dot(nn.xyz, nn.xyz) < 0.25) {
    oColor = acc;
    return;
  }
  vec3 N = normalize(nn.xyz);
  h = normalize(h);
  // (1.1) a face of the low turned inside out (it faces away from the high): read from its outside, the side the
  // two-sided view (and a two-sided material) shows, so it does not shade black
  if (dot(h, N) < 0.0) N = -N;
  vec3 T = tt.xyz - N * dot(N, tt.xyz);
  T = dot(T, T) > 1e-10 ? normalize(T) : vec3(1.0, 0.0, 0.0);
  vec3 B = cross(N, T) * (tt.w < 0.0 ? -1.0 : 1.0);
  vec3 base = normalize(vec3(dot(h, T), dot(h, B), dot(h, N)));
  vec3 layers = dot(acc.xyz, acc.xyz) > 1e-8 ? normalize(acc.xyz) : vec3(0.0, 0.0, 1.0);
  oColor = vec4(combineNormals(base, layers), acc.a);
}
#endif

#ifdef POST_REGION
// Resamples a part of a texture (uRegion: uv offset xy, size zw) onto the whole target;
// bilinear, or nearest for id maps. Outside 0..1 clamps to the edge.
uniform sampler2D uSrc;
uniform vec4 uRegion;
uniform int uNearest;
void main() {
  ivec2 size = textureSize(uSrc, 0);
  vec2 uv = uRegion.xy + vUV * uRegion.zw;
  vec2 f = uv * vec2(size) - 0.5;
  if (uNearest == 1) {
    oColor = texelFetch(uSrc, clamp(ivec2(floor(f + 0.5)), ivec2(0), size - 1), 0);
    return;
  }
  ivec2 b = ivec2(floor(f));
  vec2 w = f - vec2(b);
  vec4 c00 = texelFetch(uSrc, clamp(b, ivec2(0), size - 1), 0);
  vec4 c10 = texelFetch(uSrc, clamp(b + ivec2(1, 0), ivec2(0), size - 1), 0);
  vec4 c01 = texelFetch(uSrc, clamp(b + ivec2(0, 1), ivec2(0), size - 1), 0);
  vec4 c11 = texelFetch(uSrc, clamp(b + ivec2(1, 1), ivec2(0), size - 1), 0);
  oColor = mix(mix(c00, c10, w.x), mix(c01, c11, w.x), w.y);
}
#endif
