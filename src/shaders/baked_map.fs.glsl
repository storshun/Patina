// (0.41) A baked map with its layer's fixes (texture space): the bake (uRaw) where nothing is painted, the painted
// value where the fixes cover it. BAKED_FINAL: the map everything reads, from the fixes (or the layer's adjustments,
// uStack) at the layer's strength (toward the value that changes nothing), and its opacity over the bake.
in vec2 vUV;
out vec4 oValue;

uniform sampler2D uRaw;    // r: the bake
uniform sampler2D uFixes;  // rg: the value painted, how much of it covers the bake
uniform int uHasFixes;
#ifdef BAKED_FINAL
uniform sampler2D uStack;  // r: the bake with the fixes and the adjustments
uniform int uHasStack;
uniform float uStrength;
uniform float uNeutral;
uniform float uOpacity;
#endif

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  float raw = texelFetch(uRaw, p, 0).r;
  vec2 f = uHasFixes == 1 ? texelFetch(uFixes, p, 0).rg : vec2(0.0);
  float v = mix(raw, f.r, f.g);
#ifdef BAKED_FINAL
  if (uHasStack == 1) v = texelFetch(uStack, p, 0).r;
  v = clamp(uNeutral + (v - uNeutral) * uStrength, 0.0, 1.0);
  v = mix(raw, v, uOpacity);
#endif
  oValue = vec4(v, v, v, 1.0);
}
