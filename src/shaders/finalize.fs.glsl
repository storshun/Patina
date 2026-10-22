// Pass 1 (FINALIZE_DILATE): copies the composite into the final textures and
// fills the gutter around UV islands with the nearest island texel.
// Pass 2 (FINALIZE_NORMAL): derives a normal from the height channel and
// combines it with the painted normal channel.
#include "common.glsl"

in vec2 vUV;

#ifdef FINALIZE_DILATE
layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oRMH;
layout(location = 2) out vec4 oNormal;
layout(location = 3) out vec4 oEmissive;
layout(location = 4) out vec4 oExtra;  // opacity, sheen, clearcoat, subsurface
uniform sampler2D uAccColor;
uniform sampler2D uAccRMH;
uniform sampler2D uAccNormal;
uniform sampler2D uAccEmissive;
uniform sampler2D uAccExtra;
uniform sampler2D uSeed;   // nearest covered texel (jump flood result)
uniform sampler2D uAOMap;
uniform int uHasAO;
uniform int uDilate;

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 seed = texelFetch(uSeed, p, 0);
  ivec2 q = (uDilate == 1 && seed.x >= 0.0) ? ivec2(seed.xy) : p;
  oColor = vec4(texelFetch(uAccColor, q, 0).rgb, 1.0);
  vec4 rmh = texelFetch(uAccRMH, q, 0);
  rmh.a = uHasAO == 1 ? texelFetch(uAOMap, q, 0).r : 1.0;
  oRMH = rmh;
  oNormal = vec4(normalize(texelFetch(uAccNormal, q, 0).xyz), 1.0);
  oEmissive = vec4(texelFetch(uAccEmissive, q, 0).rgb, 1.0);
  oExtra = texelFetch(uAccExtra, q, 0);
}
#endif

#ifdef FINALIZE_NORMAL
out vec4 oNormal;
uniform sampler2D uRMH;       // b = height (dilated)
uniform sampler2D uNormalIn;  // painted normal channel (signed xyz)
uniform sampler2D uCoverage;  // position map, w = covered by the UV layout
uniform float uHeightStrength;  // normal intensity from height (texels per unit height)

float heightAt(ivec2 q) { return texelFetch(uRMH, q, 0).b; }
bool covered(ivec2 q) { return texelFetch(uCoverage, q, 0).w > 0.5; }

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 size = textureSize(uRMH, 0);
  ivec2 l = clamp(p - ivec2(1, 0), ivec2(0), size - 1), r = clamp(p + ivec2(1, 0), ivec2(0), size - 1);
  ivec2 d = clamp(p - ivec2(0, 1), ivec2(0), size - 1), u = clamp(p + ivec2(0, 1), ivec2(0), size - 1);
  float hc = heightAt(p);
  // one-sided differences where a neighbor lies outside the UV island, so
  // island borders do not produce creases
  bool cl = covered(l), cr = covered(r), cd = covered(d), cu = covered(u);
  float dhdx = (cl && cr) ? (heightAt(r) - heightAt(l)) * 0.5 : cr ? heightAt(r) - hc : cl ? hc - heightAt(l) : 0.0;
  float dhdy = (cd && cu) ? (heightAt(u) - heightAt(d)) * 0.5 : cu ? heightAt(u) - hc : cd ? hc - heightAt(d) : 0.0;
  vec3 hn = normalize(vec3(-dhdx * uHeightStrength, -dhdy * uHeightStrength, 1.0));
  vec3 pn = normalize(texelFetch(uNormalIn, p, 0).xyz);
  oNormal = vec4(combineNormals(pn, hn), 1.0);
}
#endif
