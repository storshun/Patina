// Viewport shading: image-based PBR (GGX) plus debug/channel views.
#include "common.glsl"

in vec3 vWorldPos;
in vec3 vObjPos;
in vec3 vNormal;
in vec4 vTangent;
in vec2 vUV;
flat in uint vFlags;
flat in uvec2 vIds;

out vec4 oColor;

uniform sampler2D uColorTex;     // sRGB
uniform sampler2D uRMHTex;       // rough, metal, height, ao
uniform sampler2D uNormalTex;    // signed tangent normal (final)
uniform sampler2D uEmissiveTex;  // sRGB
uniform sampler2D uExtraTex;     // opacity, sheen, clearcoat, subsurface (0.26)
uniform int uAlphaPass;          // 0 all opaque, 1 only the opaque parts, 2 only the see-through parts (blended)
uniform sampler2D uAOMap;
uniform sampler2D uCurvMap;
uniform sampler2D uMaskTex;      // selected layer mask
uniform samplerCube uEnvSpec;
uniform sampler2D uBrdfLut;
uniform vec4 uSH[9];
uniform float uEnvMips;
uniform float uEnvIntensity;
uniform mat3 uEnvRot;
uniform float uExposure;
uniform vec3 uCamPos;
uniform sampler2D uThickMap;
uniform sampler2D uBentMap;  // object space
uniform sampler2D uBakeCheck;  // (1.1) the bake-down's check: r share by the nearest point, g guessed, b covered
uniform int uHasBakeCheck;
uniform int uView;
uniform int uUseAO;
uniform float uSelTint;
uniform int uShowPins;
uniform uint uHoverFace;
uniform int uShowUVIslands;
uniform int uIsolateTile;  // >= 0: faces of other texture sets are dim ghosts
uniform sampler2D uMatIdTex;      // r = material slot / 255
uniform sampler2D uFaceData;      // per-face value (density / stretch views)
uniform int uFaceDataW;
uniform sampler2D uRetopoData;    // retopology: per face, the patch (1) or log2 of the density (2)
uniform int uRetopoView;          // 0 off, 1 patches, 2 density, 3 the quads themselves (clay)
uniform int uRetopoSel;           // the selected zone (patches view): brighter
float faceValue() {
  uint f = vIds.x;
  return texelFetch(uFaceData, ivec2(int(f % uint(uFaceDataW)), int(f / uint(uFaceDataW))), 0).r;
}

vec3 irradianceSH(vec3 n) {
  float x = n.x, y = n.y, z = n.z;
  vec3 r = uSH[0].rgb * 0.886227 + uSH[1].rgb * 1.023328 * y + uSH[2].rgb * 1.023328 * z +
           uSH[3].rgb * 1.023328 * x + uSH[4].rgb * 0.858086 * x * y + uSH[5].rgb * 0.858086 * y * z +
           uSH[6].rgb * 0.247708 * (3.0 * z * z - 1.0) + uSH[7].rgb * 0.858086 * x * z +
           uSH[8].rgb * 0.429043 * (x * x - y * y);
  return max(r, vec3(0.0)) / PI;
}

vec3 acesFilm(vec3 x) {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 islandColor(uint id) {
  float h = fract(float(id) * 0.61803398875);
  vec3 k = clamp(abs(fract(h + vec3(0, 2, 1) / 3.0) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
  return mix(vec3(0.6), k, 0.65);
}

void main() {
  if ((vFlags & 32u) != 0u) discard;  // (1.2) the faces of a hidden face set
  vec3 Ng = normalize(vNormal);
  if (!gl_FrontFacing) Ng = -Ng;
  vec3 T = vTangent.xyz;
  T = normalize(T - Ng * dot(Ng, T) + vec3(1e-6, 0, 0));
  vec3 B = cross(Ng, T) * (vTangent.w < 0.0 ? -1.0 : 1.0);
  vec3 tn = normalize(texture(uNormalTex, vUV).xyz);
  vec3 N = normalize(T * tn.x + B * tn.y + Ng * tn.z);
  vec3 V = normalize(uCamPos - vWorldPos);

  vec3 baseSrgb = texture(uColorTex, vUV).rgb;
  vec4 rmh = texture(uRMHTex, vUV);
  vec3 emis = srgbToLinear(texture(uEmissiveTex, vUV).rgb);
  vec4 xt = texture(uExtraTex, vUV);  // opacity, sheen, clearcoat, subsurface
  float alpha = 1.0;
  vec3 outc;

  // (0.38) solid: the shape alone, lit the same way: light gray, half rough, the mesh's own normals, no textures
  if (uView == 25) {
    baseSrgb = vec3(0.8);
    rmh = vec4(0.5, 0.0, 0.0, uUseAO == 1 ? texture(uAOMap, vUV).r : 1.0);
    emis = vec3(0.0);
    xt = vec4(1.0, 0.0, 0.0, 0.0);
    N = Ng;
  }
  if (uView == 0 || uView == 25) {
    vec3 albedo = srgbToLinear(baseSrgb);
    float rough = clamp(rmh.r, 0.03, 1.0);
    float metal = rmh.g;
    float ao = uUseAO == 1 ? mix(1.0, rmh.a, 0.85) : 1.0;
    float ndv = max(dot(N, V), 1e-4);
    vec3 F0 = mix(vec3(0.04), albedo, metal);
    vec2 brdf = texture(uBrdfLut, vec2(ndv, rough)).rg;
    vec3 F = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - ndv, 5.0);
    vec3 kd = (1.0 - F) * (1.0 - metal);
    vec3 R = reflect(-V, N);
    vec3 spec = textureLod(uEnvSpec, uEnvRot * R, rough * uEnvMips).rgb * (F0 * brdf.x + brdf.y);
    vec3 diff = irradianceSH(uEnvRot * N) * albedo * kd;
    // horizon occlusion for normal-mapped reflections
    float horizon = clamp(1.0 + dot(R, Ng), 0.0, 1.0);
    spec *= horizon * horizon;
    // 0.26: subsurface (light that goes in and comes out softened and warmer: skin, wax, leaves)
    if (xt.a > 0.001) {
      vec3 soft = irradianceSH(uEnvRot * normalize(mix(N, Ng, 0.75))) * albedo * kd;
      vec3 wrap = irradianceSH(uEnvRot * -Ng) * albedo * vec3(1.0, 0.45, 0.3) * 0.35 * (1.0 - texture(uThickMap, vUV).r * 0.6);
      diff = mix(diff, soft * mix(vec3(1.0), vec3(1.08, 0.95, 0.9), 0.5) + wrap, xt.a);
    }
    // sheen (velvet, satin): a soft bright rim at grazing angles
    if (xt.g > 0.001) {
      // (irradianceSH gives the mean radiance around N; the factor is close to a Charlie lobe's, roughness 0.5)
      float sheenDfg = 0.02 + 0.4 * pow(1.0 - ndv, 4.0);
      vec3 sheenL = irradianceSH(uEnvRot * N) * 0.7 + textureLod(uEnvSpec, uEnvRot * R, uEnvMips * 0.7).rgb * 0.3;
      diff *= 1.0 - xt.g * 0.08;
      spec += xt.g * sheenL * sheenDfg;
    }
    // clearcoat: a smooth varnish over the surface (it follows the shape, not the bumps)
    if (xt.b > 0.001) {
      float cr = 0.04;
      float ndvc = max(dot(Ng, V), 1e-4);
      vec3 Rc = reflect(-V, Ng);
      vec2 bc = texture(uBrdfLut, vec2(ndvc, cr)).rg;
      float Fc = 0.04 + 0.96 * pow(1.0 - ndvc, 5.0);
      vec3 coat = textureLod(uEnvSpec, uEnvRot * Rc, cr * uEnvMips).rgb * (0.04 * bc.x + bc.y);
      diff *= 1.0 - Fc * xt.b;
      spec = spec * (1.0 - Fc * xt.b) + coat * xt.b;
    }
    vec3 c = (diff + spec) * ao * uEnvIntensity + emis * 4.0;
    outc = linearToSrgb(acesFilm(c * uExposure));
    alpha = xt.r;
  } else if (uView == 1) outc = baseSrgb;
  else if (uView == 2) outc = vec3(rmh.r);
  else if (uView == 3) outc = vec3(rmh.g);
  else if (uView == 4) outc = encodeNormal(tn);
  else if (uView == 5) outc = vec3(rmh.b);
  else if (uView == 6) outc = texture(uEmissiveTex, vUV).rgb;
  else if (uView == 7) outc = vec3(texture(uAOMap, vUV).r);
  else if (uView == 8) outc = vec3(texture(uCurvMap, vUV).r);
  else if (uView == 9) {
    vec2 c = floor(vUV * 16.0);
    float k = mod(c.x + c.y, 2.0);
    vec3 a = mix(vec3(0.18), vec3(0.85), k);
    vec2 g = abs(fract(vUV * 4.0) - 0.5);
    float line = 1.0 - smoothstep(0.47, 0.49, max(g.x, g.y));
    outc = mix(vec3(0.9, 0.35, 0.1), a * mix(vec3(1.0), vec3(0.75, 0.85, 1.0), step(0.5, fract(vUV.x * 2.0))), line);
    outc *= 0.6 + 0.4 * max(dot(Ng, V), 0.0);
  } else if (uView == 10) outc = vec3(texture(uMaskTex, vUV).r);
  else if (uView == 11) outc = encodeNormal(N);
  else if (uView == 13) {
    // texel density against the target: blue too low, green on target, red too high (log2 scale)
    float r = faceValue();
    if (r < -50.0) outc = vec3(0.35, 0.05, 0.35);
    else {
      float t = clamp(r, -2.0, 2.0);
      vec3 ok = vec3(0.35, 0.8, 0.35);
      outc = t < 0.0 ? mix(ok, vec3(0.15, 0.35, 0.95), smoothstep(0.1, 2.0, -t))
                     : mix(ok, vec3(0.95, 0.2, 0.15), smoothstep(0.1, 2.0, t));
      vec2 c = floor(vUV * 64.0);
      outc *= 0.9 + 0.1 * mod(c.x + c.y, 2.0);
    }
    outc *= 0.55 + 0.45 * max(dot(Ng, V), 0.0);
  } else if (uView == 14) {
    // stretch: 1 = none (blue-gray), 1.5 yellow, 2+ red
    float s = faceValue();
    if (s < -50.0) outc = vec3(0.35, 0.05, 0.35);
    else {
      float t = clamp((s - 1.0) / 1.0, 0.0, 1.0);
      outc = t < 0.5 ? mix(vec3(0.55, 0.62, 0.72), vec3(0.95, 0.85, 0.2), t * 2.0)
                     : mix(vec3(0.95, 0.85, 0.2), vec3(0.95, 0.2, 0.15), t * 2.0 - 1.0);
    }
    outc *= 0.55 + 0.45 * max(dot(Ng, V), 0.0);
  } else if (uView == 15) {
    float id = floor(texture(uMatIdTex, vUV).r * 255.0 + 0.5);
    outc = islandColor(uint(id) * 7u + 3u) * (0.55 + 0.45 * max(dot(Ng, V), 0.0));
  } else if (uView == 16) {
    outc = vec3(texture(uThickMap, vUV).r);
  } else if (uView == 17) {
    vec3 b = texture(uBentMap, vUV).xyz;
    outc = dot(b, b) > 0.25 ? encodeNormal(normalize(b)) : vec3(0.5);
  } else if (uView == 28) {
    // (1.2) face sets: each in its color over the shape, a face in none gray
    float c = faceValue();
    vec3 base = vec3(0.6, 0.61, 0.64);
    if (c >= 0.0) {
      uint u = uint(c + 0.5);
      base = vec3(float((u >> 16u) & 255u), float((u >> 8u) & 255u), float(u & 255u)) / 255.0;
    }
    outc = base * (0.45 + 0.55 * max(dot(Ng, V), 0.0));
  } else if (uView == 23) {
    // the Mesh Check: gray fine, blue a note, orange a warning, red a problem
    float sv = faceValue();
    vec3 base = vec3(0.62, 0.63, 0.66);
    outc = sv > 2.5 ? vec3(0.95, 0.22, 0.18) : sv > 1.5 ? vec3(0.98, 0.62, 0.15) : sv > 0.5 ? vec3(0.35, 0.55, 0.95) : base;
    outc *= 0.5 + 0.5 * max(dot(Ng, V), 0.0);
    if (!gl_FrontFacing) outc = mix(outc, vec3(0.75, 0.2, 0.75), 0.6);  // the back of a face: magenta
  } else if (uView == 24) {
    // PBR range (0.33): base colors a real material would not have. Non-metals: red below sRGB 30 (darker than
    // charcoal), blue above 240 (whiter than fresh snow). Metals: orange below sRGB 180 (metals reflect at least
    // about half the light). Purple: metallic half-way (fine at a border, odd over an area). Else gray.
    float lum = luminance(baseSrgb), metal = rmh.g;
    vec3 base = vec3(0.35 + 0.5 * lum);
    vec3 warn = vec3(-1.0);
    if (metal > 0.1 && metal < 0.9) warn = vec3(0.65, 0.3, 0.9);
    else if (metal >= 0.9 && lum < 180.0 / 255.0) warn = vec3(0.98, 0.55, 0.12);
    else if (metal <= 0.1 && lum < 30.0 / 255.0) warn = vec3(0.92, 0.18, 0.15);
    else if (metal <= 0.1 && lum > 240.0 / 255.0) warn = vec3(0.2, 0.45, 0.95);
    outc = warn.r >= 0.0 ? warn : base;
    outc *= 0.55 + 0.45 * max(dot(Ng, V), 0.0);
  } else if (uView == 27) {
    // (1.1) the bake-down check: the texture, faded, where the rays found the high-poly surface; amber where only
    // the nearest point did (a guess close by); red where it was guessed from farther away
    vec3 base = mix(vec3(0.62, 0.63, 0.66), baseSrgb, 0.3);
    vec4 ck = uHasBakeCheck == 1 ? texture(uBakeCheck, vUV) : vec4(0.0);
    outc = mix(base, vec3(0.98, 0.7, 0.15), clamp(ck.r * 1.5, 0.0, 1.0));
    outc = mix(outc, vec3(0.95, 0.18, 0.15), clamp(ck.g * 1.5, 0.0, 1.0));
    outc = mix(outc, vec3(0.8, 0.2, 0.8), ck.b > 0.5 ? 0.85 : 0.0);  // the low faces away from the high: magenta
    outc *= 0.55 + 0.45 * max(dot(Ng, V), 0.0);
  } else if (uView >= 19 && uView <= 22) {
    outc = vec3(xt[uView - 19]);  // opacity, sheen, clearcoat, subsurface
  } else if (uView == 18) {
    // symmetry: distance to the mirrored surface in tolerances (0.1% of the size):
    // gray-blue = symmetric, yellow = 1 tolerance, red = 10 or more; magenta = no other side
    float s = faceValue();
    if (s < -50.0) outc = vec3(0.75, 0.15, 0.75);
    else {
      float t = clamp(log2(max(s, 1e-3)) / log2(10.0), -1.0, 1.0);  // 0.1 .. 10 tolerances
      outc = t < 0.0 ? mix(vec3(0.55, 0.62, 0.72), vec3(0.95, 0.85, 0.2), smoothstep(-1.0, 0.0, t))
                     : mix(vec3(0.95, 0.85, 0.2), vec3(0.95, 0.15, 0.1), smoothstep(0.0, 1.0, t));
    }
    outc *= 0.55 + 0.45 * max(dot(Ng, V), 0.0);
  } else outc = islandColor(vIds.y) * (0.55 + 0.45 * max(dot(Ng, V), 0.0));

  if (uShowUVIslands == 1 && uView != 12) outc = mix(outc, islandColor(vIds.y), 0.35);
  if (uRetopoView == 1 || uRetopoView == 2) {
    uint f = vIds.x;
    float r = texelFetch(uRetopoData, ivec2(int(f % uint(uFaceDataW)), int(f / uint(uFaceDataW))), 0).r;
    float lit = 0.55 + 0.45 * max(dot(Ng, V), 0.0);
    if (uRetopoView == 1 && r > 0.5) {
      bool sel = uRetopoSel > 0 && int(r + 0.5) == uRetopoSel;
      outc = mix(outc, islandColor(uint(r + 0.5) * 7u + 1u) * lit, sel ? 0.85 : 0.55);
      if (sel) outc = mix(outc, vec3(1.0), 0.25);
    }
    if (uRetopoView == 2 && abs(r) > 0.01)
      outc = mix(outc, (r > 0.0 ? vec3(1.0, 0.35, 0.15) : vec3(0.2, 0.45, 1.0)) * lit, clamp(abs(r) * 0.35, 0.15, 0.7));
  }
  if (uRetopoView == 3) outc = vec3(0.72, 0.7, 0.66) * (0.35 + 0.65 * max(dot(Ng, V), 0.0));
  if ((vFlags & 1u) != 0u) outc = mix(outc, vec3(1.0, 0.55, 0.1), uSelTint);
  // (0.37.1) pinned faces (Document::kPinnedFlag): blue stripes across the screen, the UV editor's pin color
  if (uShowPins == 1 && (vFlags & 8u) != 0u && fract((gl_FragCoord.x + gl_FragCoord.y) / 14.0) < 0.36)
    outc = mix(outc, vec3(0.22, 0.52, 1.0), 0.7);
  if (vIds.x == uHoverFace) outc = mix(outc, vec3(1.0, 1.0, 0.4), 0.18);
  if (uIsolateTile >= 0 && int((vFlags >> 16u) & 255u) != uIsolateTile)
    outc = vec3(0.16, 0.17, 0.19) + 0.08 * max(dot(Ng, V), 0.0);
  if (uAlphaPass == 1 && alpha < 0.995) discard;
  if (uAlphaPass == 2 && (alpha >= 0.995 || alpha < 0.004)) discard;
  oColor = vec4(outc, uAlphaPass == 2 ? alpha : 1.0);
}
