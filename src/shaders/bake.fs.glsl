// Mesh-map bakers evaluated in texture space (vertex shader: uvraster).
#include "common.glsl"

in vec3 vPos;
in vec3 vNormal;
in vec4 vTangent;
in vec2 vUV;
in vec4 vAux;
flat in uint vFlags;
flat in uint vFace;
layout(location = 0) out vec4 oColor;
#ifdef BAKE_AO
layout(location = 1) out vec4 oBent;  // sum of unoccluded directions (bent normal), w = count
#endif

#ifdef BAKE_AO
// One direction per draw; additive blending accumulates visibility * cos.
uniform sampler2D uShadow;     // depth from the light direction (orthographic)
uniform mat4 uLightVP;         // object -> light clip
uniform vec3 uDir;             // direction toward the light (object space)
uniform float uBias;
uniform int uUseSurfaceTex;     // 1: position / normal per texel from a high-poly bake
uniform sampler2D uSurfPos;
uniform sampler2D uSurfNrm;
uniform uint uSkipFlag = 0u;    // mirror copies: their texels belong to the other half
void main() {
  if ((vFlags & uSkipFlag) != 0u) discard;
  vec3 N = normalize(vNormal);
  vec3 P = vPos;
  if (uUseSurfaceTex == 1) {
    ivec2 q = ivec2(gl_FragCoord.xy);
    P = texelFetch(uSurfPos, q, 0).xyz;
    vec3 n = texelFetch(uSurfNrm, q, 0).xyz;
    if (dot(n, n) > 0.25) N = normalize(n);
  }
  float ndl = dot(N, uDir);
  if (ndl <= 0.0) {
    oColor = vec4(0.0, 0.0, 1.0, 0.0);  // b counts the sample weight (hemisphere)
    oBent = vec4(0.0);
    return;
  }
  vec4 c = uLightVP * vec4(P + N * uBias, 1.0);
  vec3 ndc = c.xyz / c.w;
  vec2 suv = ndc.xy * 0.5 + 0.5;
  float d = ndc.z * 0.5 + 0.5;
  // 4-tap PCF
  vec2 ts = 1.0 / vec2(textureSize(uShadow, 0));
  float vis = 0.0;
  vis += d <= texture(uShadow, suv + vec2(-0.5, -0.5) * ts).r + uBias * 0.5 ? 1.0 : 0.0;
  vis += d <= texture(uShadow, suv + vec2(0.5, -0.5) * ts).r + uBias * 0.5 ? 1.0 : 0.0;
  vis += d <= texture(uShadow, suv + vec2(-0.5, 0.5) * ts).r + uBias * 0.5 ? 1.0 : 0.0;
  vis += d <= texture(uShadow, suv + vec2(0.5, 0.5) * ts).r + uBias * 0.5 ? 1.0 : 0.0;
  vis *= 0.25;
  oColor = vec4(vis * ndl, ndl, 1.0, 0.0);
  oBent = vec4(uDir * vis, vis);
}
#endif

#ifdef BAKE_REPROJECT
// Rasterized at the NEW uv (aux.xy holds it), samples the source texture at the OLD uv.
uniform sampler2D uSource;
uniform int uAlphaMode;  // 0 opaque, 1 coverage in .a (RGBA layers), 2 coverage in .g (RG layers)
void main() {
  vec4 c = texture(uSource, vUV);
  // empty texels are skipped, so an unpainted face sharing texels with a painted
  // one (overlapping UVs) never erases the paint; straight alpha stays intact
  if ((uAlphaMode == 1 && c.a < 0.002) || (uAlphaMode == 2 && c.g < 0.002)) discard;
  oColor = uAlphaMode == 0 ? vec4(c.rgb, 1.0) : c;
}
#endif
