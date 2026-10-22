// Patina Web painting engine (WebGL2 through three.js).
//
// Same idea as the desktop app, reduced: the mesh is rasterized once into UV
// space (object-space position + normal per texel). A brush dab is evaluated
// per texel *in 3D* - each texel is projected through the camera and depth
// tested against the view - so strokes ignore how the UVs are laid out. Dabs
// accumulate into a stroke mask; every frame the layer is rebuilt from its
// pre-stroke snapshot, the mask and the material (sampled tri-planar in object
// space). Layers are composited, gutters dilated, and a normal map derived
// from the height channel.
import * as THREE from 'three';
import { cachedTextures, hexToRgb } from './library.js';

const MAX_DABS = 32;

const VS_QUAD = `
in vec3 position;
out vec2 vUV;
void main() { vUV = position.xy * 0.5 + 0.5; gl_Position = vec4(position.xy, 0.0, 1.0); }`;

const HEAD = `precision highp float;
precision highp int;
precision highp sampler2D;
`;

const MAT_GLSL = `
uniform int uMatFlat;          // 1 = flat values
uniform vec3 uFlatColor;       // sRGB
uniform vec3 uFlatRMH;         // rough, metal, height
uniform sampler2D uMatColor;
uniform sampler2D uMatRMH;
uniform float uMatScale;
uniform int uMapping;          // 0 tri-planar (object space), 1 UV
uniform vec3 uTint;
uniform float uHeightAmt;
uniform float uRoughOffset;

void matTap(vec2 uv, out vec3 c, out vec3 r) {
  c = texture(uMatColor, uv).rgb;
  r = texture(uMatRMH, uv).rgb;
}
void sampleMat(vec3 P, vec3 N, vec2 meshUV, out vec3 col, out vec3 rmh) {
  if (uMatFlat == 1) {
    col = uFlatColor; rmh = uFlatRMH;
  } else if (uMapping == 1) {
    matTap(meshUV * uMatScale * 4.0, col, rmh);
  } else {
    vec3 w = pow(abs(N), vec3(4.0));
    w /= max(w.x + w.y + w.z, 1e-5);
    vec3 p = P * uMatScale;
    vec3 c0, c1, c2, r0, r1, r2;
    matTap(p.zy, c0, r0);
    matTap(p.xz + 0.31, c1, r1);
    matTap(p.xy + 0.67, c2, r2);
    // where projections meet, the one standing highest wins (crisp, no see-through double pattern)
    vec3 ht = w + vec3(r0.b, r1.b, r2.b) * 0.25;
    float ma = max(ht.x, max(ht.y, ht.z)) - 0.04;
    w = max(ht - ma, 0.0);
    w /= max(w.x + w.y + w.z, 1e-5);
    col = c0 * w.x + c1 * w.y + c2 * w.z;
    rmh = r0 * w.x + r1 * w.y + r2 * w.z;
  }
  col *= uTint;
  rmh.r = clamp(rmh.r + uRoughOffset, 0.0, 1.0);
  rmh.b = clamp(0.5 + (rmh.b - 0.5) * uHeightAmt, 0.0, 1.0);
}`;

function raw(fs, uniforms, extra = {}) {
  return new THREE.RawShaderMaterial({
    glslVersion: THREE.GLSL3,
    vertexShader: HEAD + VS_QUAD,
    fragmentShader: HEAD + fs,
    uniforms,
    depthTest: false,
    depthWrite: false,
    ...extra,
  });
}

function rt(size, opts = {}) {
  return new THREE.WebGLRenderTarget(size, size, {
    type: THREE.UnsignedByteType,
    format: THREE.RGBAFormat,
    minFilter: THREE.LinearFilter,
    magFilter: THREE.LinearFilter,
    depthBuffer: false,
    generateMipmaps: false,
    ...opts,
  });
}

// ------------------------------------------------------------------ shaders

const FS_UVRASTER_VS = `
in vec3 position;
in vec3 normal;
in vec2 uv;
in float aMir;                // mirror copy: the axes it mirrors (1..7), else 0
in float aCurv;               // convexity 0..1 (0.5 flat)
out vec3 vPos;
out vec3 vNrm;
out float vCurv;
flat out float vMir;
void main() { vPos = position; vNrm = normal; vMir = aMir; vCurv = aCurv; gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0); }`;
const FS_UVRASTER = `
in vec3 vPos;
in vec3 vNrm;
uniform int uOut;
flat in float vMir;
in float vCurv;
out vec4 o;
// Mirror copies share their texels with the half they copy: position and normal come from that
// half only (two halves writing one texel left a patchwork, ghosting the material), and a third
// pass records which axes the copy mirrors, so a brush on the copy's side still paints there.
void main() {
  if (uOut < 2 || uOut == 3) {
    if (vMir > 0.5) discard;
    o = uOut == 0 ? vec4(vPos, 1.0) : uOut == 1 ? vec4(normalize(vNrm), 1.0) : vec4(vCurv, vCurv, vCurv, 1.0);
  } else {
    if (vMir < 0.5) discard;
    o = vec4(vMir / 8.0, 0.0, 0.0, 1.0);
  }
}`;

const FS_DAB = `
in vec2 vUV;
uniform sampler2D uPos;
uniform sampler2D uNrm;
uniform sampler2D uMir;       // r * 8: the axes a mirror copy sharing this texel mirrors (0: none)
uniform sampler2D uDepth;
uniform mat4 uVP;
uniform vec3 uCamPos;
uniform vec3 uCamDir;
uniform int uOrtho;
uniform vec2 uView;           // viewport in CSS pixels
uniform float uNear, uFar;
uniform float uDepthTol;
uniform int uMode;            // 0 screen-projected, 1 UV space (2D view)
uniform vec4 uDabs[${MAX_DABS}];      // xy center (px, y up / texels), z radius, w strength
uniform int uCount;
uniform float uHard;
uniform int uTip;             // 0 round, 1 square, 2 spray, 3 grunge
uniform float uSeed;
uniform int uThrough;         // paint through to hidden/back faces
uniform int uSym;             // mirror painting: bit 0 X, 1 Y, 2 Z (every combination is painted)
uniform float uTexSize;
out vec4 o;

float hash13(vec3 p) { p = fract(p * 0.1031); p += dot(p, p.zyx + 31.32); return fract((p.x + p.y) * p.z); }
float vnoise(vec3 p) {
  vec3 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
  float a = mix(mix(hash13(i), hash13(i + vec3(1,0,0)), f.x), mix(hash13(i + vec3(0,1,0)), hash13(i + vec3(1,1,0)), f.x), f.y);
  float b = mix(mix(hash13(i + vec3(0,0,1)), hash13(i + vec3(1,0,1)), f.x), mix(hash13(i + vec3(0,1,1)), hash13(i + vec3(1,1,1)), f.x), f.y);
  return mix(a, b, f.z);
}
float linDepth(float d) {
  if (uOrtho == 1) return uNear + d * (uFar - uNear);
  float z = d * 2.0 - 1.0;
  return 2.0 * uNear * uFar / (uFar + uNear - z * (uFar - uNear));
}
// brush coverage of the surface point P (normal N)
float coverage(vec4 P, vec3 N) {
  vec2 sp;
  float fade = 1.0;
  if (uMode == 0) {
    vec4 c = uVP * vec4(P.xyz, 1.0);
    if (c.w <= 1e-6) return 0.0;
    vec3 ndc = c.xyz / c.w;
    if (abs(ndc.x) > 1.02 || abs(ndc.y) > 1.02 || ndc.z > 1.0) return 0.0;
    sp = (ndc.xy * 0.5 + 0.5) * uView;
    vec3 V = uOrtho == 1 ? -uCamDir : normalize(uCamPos - P.xyz);
    float facing = dot(N, V);
    if (uThrough == 0) {
      if (facing <= 0.0) return 0.0;
      float scene = linDepth(texture(uDepth, ndc.xy * 0.5 + 0.5).r);
      float mine = linDepth(ndc.z * 0.5 + 0.5);
      if (mine - scene > uDepthTol) return 0.0;
      fade = smoothstep(0.0, 0.25, facing);
    } else {
      fade = smoothstep(0.0, 0.25, abs(facing));
    }
  } else {
    sp = gl_FragCoord.xy;
  }
  float keep = 1.0;
  for (int i = 0; i < ${MAX_DABS}; ++i) {
    if (i >= uCount) break;
    vec4 d = uDabs[i];
    vec2 q = (sp - d.xy) / max(d.z, 0.5);
    float r = uTip == 1 ? max(abs(q.x), abs(q.y)) : length(q);
    if (r >= 1.0) continue;
    float h = min(uHard, 0.98);
    float f = 1.0 - smoothstep(h, 1.0, r);
    if (uTip == 2) f *= step(0.72, hash13(vec3(floor(sp), uSeed + float(i) * 7.13)));
    if (uTip == 3) f *= smoothstep(0.45, 0.62, vnoise(P.xyz * 24.0 + uSeed) * 0.6 + vnoise(P.xyz * 61.0) * 0.4);
    keep *= 1.0 - clamp(f * d.w, 0.0, 1.0);
  }
  return (1.0 - keep) * fade;
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 P = texelFetch(uPos, p, 0);
  o = vec4(0.0);
  if (P.w < 0.5) return;
  vec3 N = normalize(texelFetch(uNrm, p, 0).xyz);
  float m = coverage(P, N);
  int mir = uMode == 0 ? int(texelFetch(uMir, p, 0).r * 8.0 + 0.5) : 0;
  // every mirror image of this point the brush may be over: each combination of the symmetry axes
  // (s), and each of those once more across the axes of a stacked copy sharing this texel (mir)
  int sym = uMode == 0 ? uSym : 0;
  for (int s = 0; s < 8; ++s) {
    if ((s & sym) != s) continue;
    for (int k = 0; k < 2; ++k) {
      int t = k == 0 ? s : (s ^ mir);
      if (t == 0 || (k == 1 && mir == 0)) continue;
      vec3 f = vec3((t & 1) != 0 ? -1.0 : 1.0, (t & 2) != 0 ? -1.0 : 1.0, (t & 4) != 0 ? -1.0 : 1.0);
      m = max(m, coverage(vec4(P.xyz * f, 1.0), N * f));
    }
  }
  o = vec4(m, m, m, m);
}`;

const FS_APPLY = `
in vec2 vUV;
uniform sampler2D uSnap;       // pre-stroke target (color, rmh or mask)
uniform sampler2D uStroke;     // stroke mask
uniform sampler2D uPos;
uniform sampler2D uNrm;
uniform int uOut;              // 0 color, 1 rmh, 2 layer mask
uniform int uTool;             // 0 paint, 1 erase, 2 blur
uniform float uOpacity;
uniform float uBlurR;          // blur radius in texels
uniform int uChColor, uChRM, uChHeight;
${MAT_GLSL}
out vec4 o;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 s = texelFetch(uSnap, p, 0);
  float m = texelFetch(uStroke, p, 0).r * uOpacity;
  o = s;
  if (m <= 0.0) return;
  if (uTool == 2) {
    // blur: a gaussian of the pre-stroke content over covered texels (premultiplied by alpha for
    // paint, so empty texels don't darken it)
    ivec2 size = textureSize(uSnap, 0);
    vec4 acc = vec4(0.0);
    float wsum = 0.0, step = max(uBlurR / 3.0, 1.0);
    for (int y = -3; y <= 3; ++y)
      for (int x = -3; x <= 3; ++x) {
        ivec2 q = clamp(p + ivec2(vec2(x, y) * step), ivec2(0), size - 1);
        if (texelFetch(uPos, q, 0).w < 0.5) continue;
        float w = exp(-float(x * x + y * y) / 4.5);
        vec4 c = texelFetch(uSnap, q, 0);
        if (uOut != 2) c.rgb *= (uOut == 1 ? 1.0 : c.a);
        acc += c * w;
        wsum += w;
      }
    if (wsum <= 0.0) return;
    vec4 b = acc / wsum;
    if (uOut == 0) b.rgb = b.a > 1e-4 ? b.rgb / b.a : s.rgb;
    o = mix(s, b, m);
    if (uOut == 2) o.a = 1.0;
    return;
  }
  if (uOut == 2) { o = vec4(vec3(mix(s.r, uTool == 1 ? 0.0 : 1.0, m)), 1.0); return; }
  if (uTool == 1) {
    if (uOut == 0 && uChColor == 1) o.a = s.a * (1.0 - m);
    if (uOut == 1) {
      if (uChRM == 1) o.a = s.a * (1.0 - m);
      if (uChHeight == 1) o.b = mix(s.b, 0.5, m);
    }
    return;
  }
  vec4 P = texelFetch(uPos, p, 0);
  vec3 N = normalize(texelFetch(uNrm, p, 0).xyz);
  vec3 col, rmh;
  sampleMat(P.xyz, N, (vec2(p) + 0.5) / vec2(textureSize(uSnap, 0)), col, rmh);
  if (uOut == 0) {
    if (uChColor == 0) return;
    float a = s.a + m * (1.0 - s.a);
    o.rgb = a > 1e-4 ? (s.rgb * s.a * (1.0 - m) + col * m) / a : col;
    o.a = a;
  } else {
    if (uChRM == 1) {
      float a = s.a + m * (1.0 - s.a);
      o.rg = a > 1e-4 ? (s.rg * s.a * (1.0 - m) + rmh.rg * m) / a : rmh.rg;
      o.a = a;
    }
    if (uChHeight == 1) o.b = mix(s.b, rmh.b, m);
  }
}`;

const FS_COMPOSITE = `
in vec2 vUV;
uniform sampler2D uAcc;
uniform sampler2D uLayer;     // paint layer target for this output
uniform sampler2D uMask;      // fill layer mask
uniform sampler2D uPos;
uniform sampler2D uNrm;
uniform int uOut;             // 0 color, 1 rmh
uniform int uKind;            // -1 base, 0 paint, 1 fill, 2 adjustment
uniform float uOpacity;
uniform vec3 uBaseColor;
uniform vec3 uBaseRMH;
uniform int uChColor, uChRM, uChHeight;
uniform int uBlend;           // 0 normal, 1 multiply, 2 screen, 3 overlay, 4 add, 5 soft light, 6 darken, 7 lighten
uniform sampler2D uCurv;      // convexity 0..1
uniform int uGen;             // smart mask: 0 none, 1 edges, 2 cavities, 3 facing up, 4 height gradient, 5 noise
uniform float uGenLevel, uGenContrast, uGenNoise;
uniform int uGenInvert;
uniform vec4 uAdj;            // hue shift, saturation, brightness, contrast
uniform vec3 uAdjRMH;         // roughness, metallic, height offsets
${MAT_GLSL}
out vec4 o;
float h31(vec3 p) { p = fract(p * 0.1031); p += dot(p, p.zyx + 31.32); return fract((p.x + p.y) * p.z); }
float vn(vec3 p) {
  vec3 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(h31(i), h31(i + vec3(1,0,0)), f.x), mix(h31(i + vec3(0,1,0)), h31(i + vec3(1,1,0)), f.x), f.y),
             mix(mix(h31(i + vec3(0,0,1)), h31(i + vec3(1,0,1)), f.x), mix(h31(i + vec3(0,1,1)), h31(i + vec3(1,1,1)), f.x), f.y), f.z);
}
float fbm(vec3 p) { float a = 0.5, s = 0.0; for (int i = 0; i < 4; ++i) { s += a * vn(p); p *= 2.03; a *= 0.5; } return s / 0.9375; }
vec3 blendRGB(vec3 a, vec3 b) {
  if (uBlend == 1) return a * b;
  if (uBlend == 2) return 1.0 - (1.0 - a) * (1.0 - b);
  if (uBlend == 3) return mix(2.0 * a * b, 1.0 - 2.0 * (1.0 - a) * (1.0 - b), step(0.5, a));
  if (uBlend == 4) return min(a + b, 1.0);
  if (uBlend == 5) return mix(a - (1.0 - 2.0 * b) * a * (1.0 - a), a + (2.0 * b - 1.0) * (sqrt(a) - a), step(0.5, b));
  if (uBlend == 6) return min(a, b);
  if (uBlend == 7) return max(a, b);
  return b;
}
vec3 rgb2hsv(vec3 c) {
  vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
  vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
  vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
  float d = q.x - min(q.w, q.y), e = 1.0e-10;
  return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
  vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
  return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}
// the smart mask's value at this texel (1 = full)
float generator(vec3 P, vec3 N, ivec2 p) {
  if (uGen == 0) return 1.0;
  float v;
  if (uGen == 1) v = texelFetch(uCurv, p, 0).r;
  else if (uGen == 2) v = 1.0 - texelFetch(uCurv, p, 0).r;
  else if (uGen == 3) v = N.y * 0.5 + 0.5;
  else if (uGen == 4) v = P.y * 0.5 + 0.5;
  else v = fbm(P * 4.0);
  // breakup noise moves the threshold, so edges wear unevenly
  float n = (fbm(P * 9.0 + 7.3) - 0.5) * uGenNoise;
  float lvl = 1.0 - uGenLevel;  // level 1 = everywhere
  float w = max(0.02, uGenContrast);
  float g = smoothstep(lvl - w, lvl + w, v + n);
  return uGenInvert == 1 ? 1.0 - g : g;
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 P = texelFetch(uPos, p, 0);
  if (uKind == -1) {
    o = uOut == 0 ? vec4(uBaseColor, P.w) : vec4(uBaseRMH, P.w);
    return;
  }
  vec4 acc = texelFetch(uAcc, p, 0);
  o = acc;
  if (P.w < 0.5) return;
  if (uKind == 0) {
    vec4 l = texelFetch(uLayer, p, 0);
    if (uOut == 0) o.rgb = mix(acc.rgb, blendRGB(acc.rgb, l.rgb), l.a * uOpacity);
    else {
      o.rg = mix(acc.rg, l.rg, l.a * uOpacity);
      o.b = clamp(acc.b + (l.b - 0.5) * uOpacity, 0.0, 1.0);
    }
    return;
  }
  float k = texelFetch(uMask, p, 0).r * uOpacity;
  if (k <= 0.0) return;
  vec3 N = normalize(texelFetch(uNrm, p, 0).xyz);
  k *= generator(P.xyz, N, p);
  if (k <= 0.0) return;
  if (uKind == 2) {
    // adjustment: hue / saturation / brightness / contrast of what is below, and offsets
    if (uOut == 0) {
      vec3 h = rgb2hsv(acc.rgb);
      h.x = fract(h.x + uAdj.x);
      h.y = clamp(h.y * uAdj.y, 0.0, 1.0);
      vec3 c = hsv2rgb(h) * uAdj.z;
      c = clamp((c - 0.5) * uAdj.w + 0.5, 0.0, 1.0);
      o.rgb = mix(acc.rgb, c, k);
    } else {
      o.r = clamp(acc.r + uAdjRMH.x * k, 0.0, 1.0);
      o.g = clamp(acc.g + uAdjRMH.y * k, 0.0, 1.0);
      o.b = clamp(acc.b + uAdjRMH.z * k, 0.0, 1.0);
    }
    return;
  }
  vec3 col, rmh;
  sampleMat(P.xyz, N, (vec2(p) + 0.5) / vec2(textureSize(uAcc, 0)), col, rmh);
  if (uOut == 0) { if (uChColor == 1) o.rgb = mix(acc.rgb, blendRGB(acc.rgb, col), k); }
  else {
    if (uChRM == 1) o.rg = mix(acc.rg, rmh.rg, k);
    if (uChHeight == 1) o.b = clamp(acc.b + (rmh.b - 0.5) * k, 0.0, 1.0);
  }
}`;

// fills empty gutter texels from covered neighbors (run with growing steps)
const FS_DILATE = `
in vec2 vUV;
uniform sampler2D uSrc;
uniform int uStep;
out vec4 o;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 size = textureSize(uSrc, 0);
  vec4 c = texelFetch(uSrc, p, 0);
  o = c;
  if (c.a > 0.5) return;
  vec4 sum = vec4(0.0);
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      if (x == 0 && y == 0) continue;
      ivec2 q = clamp(p + ivec2(x, y) * uStep, ivec2(0), size - 1);
      vec4 n = texelFetch(uSrc, q, 0);
      if (n.a > 0.5) sum += vec4(n.rgb, 1.0);
    }
  if (sum.a > 0.0) o = vec4(sum.rgb / sum.a, 1.0);
}`;

const FS_FINAL = `
in vec2 vUV;
uniform sampler2D uColor;
uniform sampler2D uRMH;
uniform sampler2D uPos;      // coverage in .w (one-sided differences at island borders)
uniform int uOut;            // 0 color, 1 ORM (three.js: G rough, B metal), 2 normal, 3 height, 4 roughness, 5 metallic
uniform float uNormalStrength;
out vec4 o;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 size = textureSize(uRMH, 0);
  if (uOut == 0) { o = vec4(texelFetch(uColor, p, 0).rgb, 1.0); return; }
  vec4 r = texelFetch(uRMH, p, 0);
  if (uOut == 1) { o = vec4(1.0, r.r, r.g, 1.0); return; }
  if (uOut == 3) { o = vec4(vec3(r.b), 1.0); return; }
  if (uOut == 4) { o = vec4(vec3(r.r), 1.0); return; }
  if (uOut == 5) { o = vec4(vec3(r.g), 1.0); return; }
  float hc = r.b;
  ivec2 pl = clamp(p + ivec2(-1, 0), ivec2(0), size - 1), pr = clamp(p + ivec2(1, 0), ivec2(0), size - 1);
  ivec2 pd = clamp(p + ivec2(0, -1), ivec2(0), size - 1), pu = clamp(p + ivec2(0, 1), ivec2(0), size - 1);
  bool cl = texelFetch(uPos, pl, 0).w > 0.5, cr = texelFetch(uPos, pr, 0).w > 0.5;
  bool cd = texelFetch(uPos, pd, 0).w > 0.5, cu = texelFetch(uPos, pu, 0).w > 0.5;
  float hl = cl ? texelFetch(uRMH, pl, 0).b : hc, hr = cr ? texelFetch(uRMH, pr, 0).b : hc;
  float hd = cd ? texelFetch(uRMH, pd, 0).b : hc, hu = cu ? texelFetch(uRMH, pu, 0).b : hc;
  float dx = (cl && cr) ? (hl - hr) : 2.0 * (cl ? hl - hc : hc - hr);
  float dy = (cd && cu) ? (hd - hu) : 2.0 * (cd ? hd - hc : hc - hu);
  float k = uNormalStrength * float(size.x) / 512.0;
  vec3 n = normalize(vec3(dx * k, dy * k, 1.0));
  o = vec4(n * 0.5 + 0.5, texelFetch(uPos, p, 0).w);  // alpha = coverage, dilated afterward
}`;

const FS_COPY = `
in vec2 vUV;
uniform sampler2D uSrc;
out vec4 o;
void main() { o = texelFetch(uSrc, ivec2(gl_FragCoord.xy), 0); }`;

const FS_CLEAR = `
in vec2 vUV;
uniform vec4 uValue;
out vec4 o;
void main() { o = uValue; }`;

// ------------------------------------------------------------------ engine

let nextLayerId = 1;
export const BLENDS = ['normal', 'multiply', 'screen', 'overlay', 'add', 'softlight', 'darken', 'lighten'];
export const GENERATORS = ['none', 'edges', 'cavity', 'up', 'gradient', 'noise'];

export class PaintEngine {
  constructor(renderer) {
    this.renderer = renderer;
    this.size = 0;
    this.layers = [];
    this.selected = -1;
    this.base = { color: '#9a9a9a', roughness: 0.6, metallic: 0.0 };
    this.normalStrength = 1.0;
    this.undoStack = [];
    this.redoStack = [];
    this.undoLimit = 16;
    this.dirty = true;
    this.quad = new THREE.Mesh(new THREE.PlaneGeometry(2, 2));
    this.quad.frustumCulled = false;
    this.quadScene = new THREE.Scene();
    this.quadScene.add(this.quad);
    this.quadCam = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);
    const tex = null;
    this.mats = {
      dab: raw(FS_DAB, {
        uPos: { value: tex }, uNrm: { value: tex }, uMir: { value: tex }, uDepth: { value: tex }, uVP: { value: new THREE.Matrix4() },
        uCamPos: { value: new THREE.Vector3() }, uCamDir: { value: new THREE.Vector3() }, uOrtho: { value: 0 },
        uView: { value: new THREE.Vector2(1, 1) }, uNear: { value: 0.1 }, uFar: { value: 100 }, uDepthTol: { value: 0.02 },
        uMode: { value: 0 }, uDabs: { value: Array.from({ length: MAX_DABS }, () => new THREE.Vector4()) },
        uCount: { value: 0 }, uHard: { value: 0.7 }, uTip: { value: 0 }, uSeed: { value: 0 }, uThrough: { value: 0 }, uSym: { value: 0 },
        uTexSize: { value: 1024 },
      }, {
        blending: THREE.CustomBlending, blendEquation: THREE.AddEquation,
        blendSrc: THREE.OneFactor, blendDst: THREE.OneMinusSrcColorFactor,
        blendSrcAlpha: THREE.OneFactor, blendDstAlpha: THREE.OneMinusSrcAlphaFactor,
      }),
      apply: raw(FS_APPLY, this._matUniforms({
        uSnap: { value: tex }, uStroke: { value: tex }, uPos: { value: tex }, uNrm: { value: tex }, uOut: { value: 0 },
        uTool: { value: 0 }, uOpacity: { value: 1 }, uChColor: { value: 1 }, uChRM: { value: 1 }, uChHeight: { value: 1 },
        uBlurR: { value: 6 },
      })),
      composite: raw(FS_COMPOSITE, this._matUniforms({
        uAcc: { value: tex }, uLayer: { value: tex }, uMask: { value: tex }, uPos: { value: tex }, uNrm: { value: tex },
        uOut: { value: 0 }, uKind: { value: 0 }, uOpacity: { value: 1 }, uBaseColor: { value: new THREE.Vector3() },
        uBaseRMH: { value: new THREE.Vector3() }, uChColor: { value: 1 }, uChRM: { value: 1 }, uChHeight: { value: 1 },
        uBlend: { value: 0 }, uCurv: { value: tex }, uGen: { value: 0 }, uGenLevel: { value: 0.5 }, uGenContrast: { value: 0.1 },
        uGenNoise: { value: 0 }, uGenInvert: { value: 0 }, uAdj: { value: new THREE.Vector4(0, 1, 1, 1) }, uAdjRMH: { value: new THREE.Vector3() },
      })),
      dilate: raw(FS_DILATE, { uSrc: { value: tex }, uStep: { value: 1 } }),
      final: raw(FS_FINAL, { uColor: { value: tex }, uRMH: { value: tex }, uPos: { value: tex }, uOut: { value: 0 }, uNormalStrength: { value: 1 } }),
      copy: raw(FS_COPY, { uSrc: { value: tex } }),
      clear: raw(FS_CLEAR, { uValue: { value: new THREE.Vector4() } }),
    };
    this.uvMat = new THREE.RawShaderMaterial({
      glslVersion: THREE.GLSL3, vertexShader: HEAD + FS_UVRASTER_VS, fragmentShader: HEAD + FS_UVRASTER,
      uniforms: { uOut: { value: 0 } }, side: THREE.DoubleSide, depthTest: false, depthWrite: false,
    });
    this.white = new THREE.DataTexture(new Uint8Array([255, 255, 255, 255]), 1, 1);
    this.white.needsUpdate = true;
    this.flatNormal = new THREE.DataTexture(new Uint8Array([128, 128, 255, 255]), 1, 1);
    this.flatNormal.needsUpdate = true;
    this.depthRT = null;
    this.stroke = null;
    this.depthScene = new THREE.Scene();
    this.depthMesh = new THREE.Mesh(undefined, new THREE.MeshBasicMaterial({ colorWrite: false }));
    this.depthScene.add(this.depthMesh);
  }

  _matUniforms(u) {
    return Object.assign(u, {
      uMatFlat: { value: 1 }, uFlatColor: { value: new THREE.Vector3(0.5, 0.5, 0.5) }, uFlatRMH: { value: new THREE.Vector3(0.5, 0, 0.5) },
      uMatColor: { value: null }, uMatRMH: { value: null }, uMatScale: { value: 1 }, uMapping: { value: 0 },
      uTint: { value: new THREE.Vector3(1, 1, 1) }, uHeightAmt: { value: 1 }, uRoughOffset: { value: 0 },
    });
  }

  // material: { id, flat:{color, roughness, metallic, height}, scale, mapping, tint, heightAmt, roughOffset, isFlat }
  _bindMaterial(mat, m) {
    const u = mat.uniforms;
    const tex = m.isFlat ? null : cachedTextures(m.id);
    u.uMatFlat.value = tex ? 0 : 1;
    const fc = hexToRgb(m.flat.color);
    u.uFlatColor.value.set(fc[0], fc[1], fc[2]);
    u.uFlatRMH.value.set(m.flat.roughness, m.flat.metallic, m.flat.height ?? 0.5);
    u.uMatColor.value = tex ? tex.color : this.white;
    u.uMatRMH.value = tex ? tex.rmh : this.white;
    u.uMatScale.value = (m.scale || 1) * (m.defaultScale || 1);
    u.uMapping.value = m.mapping || 0;
    const t = hexToRgb(m.tint || '#ffffff');
    u.uTint.value.set(t[0], t[1], t[2]);
    u.uHeightAmt.value = m.heightAmt ?? 1;
    u.uRoughOffset.value = m.roughOffset ?? 0;
  }

  _pass(material, target) {
    this.quad.material = material;
    const r = this.renderer;
    const prev = r.getRenderTarget();
    const ac = r.autoClear;
    r.autoClear = false;
    r.setRenderTarget(target);
    r.render(this.quadScene, this.quadCam);
    r.setRenderTarget(prev);
    r.autoClear = ac;
  }

  _clear(target, v) {
    this.mats.clear.uniforms.uValue.value.set(v[0], v[1], v[2], v[3]);
    this._pass(this.mats.clear, target);
  }

  _copy(src, dst) {
    this.mats.copy.uniforms.uSrc.value = src.texture;
    this._pass(this.mats.copy, dst);
  }

  // ---------------------------------------------------------------- set-up
  setMesh(geometry, size) {
    this.geometry = geometry;
    this.depthMesh.geometry = geometry;
    this.allocate(size, false);
  }

  allocate(size, keepLayers) {
    const old = this.size;
    this.size = size;
    const fl = { type: this._floatType(), minFilter: THREE.NearestFilter, magFilter: THREE.NearestFilter };
    for (const k of ['posRT', 'nrmRT', 'mirRT', 'curvRT', 'strokeRT', 'accA', 'accB', 'rmhA', 'rmhB', 'nA', 'nB']) if (this[k]) this[k].dispose();
    this.posRT = rt(size, fl);
    this.nrmRT = rt(size, fl);
    this.mirRT = rt(size, { minFilter: THREE.NearestFilter, magFilter: THREE.NearestFilter });
    this.curvRT = rt(size, { minFilter: THREE.NearestFilter, magFilter: THREE.NearestFilter });
    this.strokeRT = rt(size);
    this.accA = rt(size); this.accB = rt(size); this.rmhA = rt(size); this.rmhB = rt(size);
    this.nA = rt(size); this.nB = rt(size);
    for (const k of ['finalColor', 'finalORM', 'finalNormal']) {
      if (this[k]) this[k].dispose();
      this[k] = rt(size, { generateMipmaps: true, minFilter: THREE.LinearMipmapLinearFilter });
    }
    this.rasterUV();
    if (keepLayers && old) {
      for (const l of this.layers) this._resizeLayer(l, size);
    } else {
      for (const l of this.layers) this._disposeLayer(l);
      this.layers = [];
      this.selected = -1;
      this.clearUndo();
      this.addPaintLayer('Layer 1');
    }
    this.dirty = true;
  }

  _floatType() {
    const gl = this.renderer.getContext();
    return gl.getExtension('EXT_color_buffer_float') ? THREE.FloatType : THREE.HalfFloatType;
  }

  // triangle edges as line segments: drawn under the triangles they give the texels
  // straddling island borders a position on the surface (instead of none), so bilinear
  // filtering across UV seams blends values from the right place
  _edgeGeometry() {
    const g = this.geometry;
    if (this._edges && this._edges.src === g && this._edges.uvVersion === g.attributes.uv.version &&
        this._edges.curvVersion === (g.attributes.aCurv ? g.attributes.aCurv.version : -1)) return this._edges.geo;
    const P = g.attributes.position.array, N = g.attributes.normal.array, U = g.attributes.uv.array;
    const C = g.attributes.aCurv ? g.attributes.aCurv.array : null;
    const n = P.length / 9;
    const p = new Float32Array(n * 18), nn = new Float32Array(n * 18), uu = new Float32Array(n * 12), cc = new Float32Array(n * 6).fill(0.5);
    let k = 0;
    for (let t = 0; t < n; t++)
      for (const [a, b] of [[0, 1], [1, 2], [2, 0]])
        for (const v of [t * 3 + a, t * 3 + b]) {
          p.set(P.subarray(v * 3, v * 3 + 3), k * 3);
          nn.set(N.subarray(v * 3, v * 3 + 3), k * 3);
          uu.set(U.subarray(v * 2, v * 2 + 2), k * 2);
          if (C) cc[k] = C[v];
          k++;
        }
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(p, 3));
    geo.setAttribute('normal', new THREE.BufferAttribute(nn, 3));
    geo.setAttribute('uv', new THREE.BufferAttribute(uu, 2));
    geo.setAttribute('aCurv', new THREE.BufferAttribute(cc, 1));
    if (this._edges) this._edges.geo.dispose();
    this._edges = { src: g, uvVersion: g.attributes.uv.version, curvVersion: C ? g.attributes.aCurv.version : -1, geo };
    return geo;
  }

  // per-vertex mirror axes of copy triangles (getMirrorFlags: per-triangle flags, bit 0 copy, bits 1-3 axes)
  _setMirror(geo, perTri) {
    const flags = this.getMirrorFlags ? this.getMirrorFlags() : null;
    const n = geo.attributes.position.count;
    let a = geo.attributes.aMir;
    if (!a || a.count !== n) { a = new THREE.BufferAttribute(new Float32Array(n), 1); geo.setAttribute('aMir', a); }
    const arr = a.array;
    arr.fill(0);
    if (flags && flags.length * perTri === n)
      for (let t = 0; t < flags.length; t++) if (flags[t] & 1) arr.fill((flags[t] >> 1) & 7, t * perTri, (t + 1) * perTri);
    a.needsUpdate = true;
  }

  rasterUV() {
    if (!this.geometry.attributes.aCurv) {  // flat until the app computes it
      const n = this.geometry.attributes.position.count;
      this.geometry.setAttribute('aCurv', new THREE.BufferAttribute(new Float32Array(n).fill(0.5), 1));
    }
    this._setMirror(this.geometry, 3);
    this._setMirror(this._edgeGeometry(), 6);
    const scene = new THREE.Scene();
    const lines = new THREE.LineSegments(this._edgeGeometry(), this.uvMat);
    lines.frustumCulled = false;
    lines.renderOrder = 0;
    scene.add(lines);
    const mesh = new THREE.Mesh(this.geometry, this.uvMat);
    mesh.frustumCulled = false;
    mesh.renderOrder = 1;
    scene.add(mesh);
    const r = this.renderer;
    const prev = r.getRenderTarget();
    const cc = r.getClearColor(new THREE.Color()), ca = r.getClearAlpha();
    r.setClearColor(0x000000, 0);
    for (const [out, target] of [[0, this.posRT], [1, this.nrmRT], [2, this.mirRT], [3, this.curvRT]]) {
      this.uvMat.uniforms.uOut.value = out;
      r.setRenderTarget(target);
      r.clear(true, false, false);
      r.render(scene, this.quadCam);
    }
    r.setRenderTarget(prev);
    r.setClearColor(cc, ca);
  }

  // ---------------------------------------------------------------- layers
  _newTargets(kind) {
    const t = {};
    if (kind === 'paint') {
      t.color = rt(this.size); this._clear(t.color, [0, 0, 0, 0]);
      t.rmh = rt(this.size); this._clear(t.rmh, [0.5, 0, 0.5, 0]);
    } else {
      t.mask = rt(this.size); this._clear(t.mask, [1, 1, 1, 1]);
    }
    return t;
  }

  addPaintLayer(name, index) {
    const l = { id: nextLayerId++, kind: 'paint', name: name || 'Layer ' + nextLayerId, visible: true, opacity: 1, ...this._newTargets('paint') };
    return this._insert(l, index);
  }

  addFillLayer(material, name, index) {
    const l = { id: nextLayerId++, kind: 'fill', name: name || 'Fill', visible: true, opacity: 1, material: { ...material },
      channels: { color: true, rm: true, height: true }, ...this._newTargets('fill') };
    return this._insert(l, index);
  }

  addAdjustLayer(name, index) {
    const l = { id: nextLayerId++, kind: 'adjust', name: name || 'Adjustment', visible: true, opacity: 1,
      adjust: { hue: 0, saturation: 1, brightness: 1, contrast: 1, roughness: 0, metallic: 0, height: 0 }, ...this._newTargets('adjust') };
    return this._insert(l, index);
  }

  _insert(l, index) {
    let i = index ?? (this.selected >= 0 ? this.selected + 1 : this.layers.length);
    i = Math.min(Math.max(i, 0), this.layers.length);
    this.layers.splice(i, 0, l);
    this.selected = i;
    this.pushUndo({ kind: 'add', layer: l, index: i });
    this.dirty = true;
    return l;
  }

  removeLayer(i) {
    if (i < 0 || i >= this.layers.length) return;
    const [l] = this.layers.splice(i, 1);
    this.selected = Math.min(i, this.layers.length - 1);
    this.pushUndo({ kind: 'remove', layer: l, index: i });
    this.dirty = true;
  }

  moveLayer(i, j) {
    if (j < 0 || j >= this.layers.length) return;
    const [l] = this.layers.splice(i, 1);
    this.layers.splice(j, 0, l);
    this.selected = j;
    this.pushUndo({ kind: 'move', from: i, to: j });
    this.dirty = true;
  }

  clearLayer(i) {
    const l = this.layers[i];
    if (!l) return;
    this.pushUndo({ kind: 'pixels', layer: l, saved: this._saveTargets(l) });
    if (l.kind === 'paint') { this._clear(l.color, [0, 0, 0, 0]); this._clear(l.rmh, [0.5, 0, 0.5, 0]); }
    else this._clear(l.mask, [0, 0, 0, 1]);
    this.dirty = true;
  }

  setFillMask(i, value) {
    const l = this.layers[i];
    if (!l || l.kind === 'paint') return;
    this.pushUndo({ kind: 'pixels', layer: l, saved: this._saveTargets(l) });
    this._clear(l.mask, [value, value, value, 1]);
    this.dirty = true;
  }

  _saveTargets(l) {
    const keys = l.kind === 'paint' ? ['color', 'rmh'] : ['mask'];
    const s = {};
    for (const k of keys) { s[k] = rt(this.size); this._copy(l[k], s[k]); }
    return s;
  }

  _resizeLayer(l, size) {
    for (const k of ['color', 'rmh', 'mask']) {
      if (!l[k]) continue;
      const n = rt(size);
      this.mats.copy.uniforms.uSrc.value = l[k].texture;
      // resample with bilinear filtering
      const fs = raw(`in vec2 vUV; uniform sampler2D uSrc; out vec4 o; void main(){ o = texture(uSrc, vUV); }`, { uSrc: { value: l[k].texture } });
      this._pass(fs, n);
      fs.dispose();
      l[k].dispose();
      l[k] = n;
    }
  }

  _disposeLayer(l) {
    for (const k of ['color', 'rmh', 'mask']) if (l[k]) l[k].dispose();
  }

  // ---------------------------------------------------------------- undo
  pushUndo(e) {
    this.undoStack.push(e);
    for (const r of this.redoStack) this._dropEntry(r);
    this.redoStack = [];
    while (this.undoStack.length > this.undoLimit) this._dropEntry(this.undoStack.shift());
  }

  _dropEntry(e) {
    if (e.kind === 'pixels') for (const k in e.saved) e.saved[k].dispose();
    if (e.kind === 'uv') for (const s of e.targets) s.rt.dispose();
    if (e.kind === 'remove' && !this.layers.includes(e.layer)) this._disposeLayer(e.layer);
  }

  clearUndo() {
    for (const e of [...this.undoStack, ...this.redoStack]) this._dropEntry(e);
    this.undoStack = [];
    this.redoStack = [];
  }

  _invert(e) {
    if (e.kind === 'uv') {
      // a UV edit: the layout (and per-triangle flags) swap via the app, the layer textures here
      for (const s of e.targets) { const cur = s.layer[s.key]; s.layer[s.key] = s.rt; s.rt = cur; }
      if (this.onUVSwap) this.onUVSwap(e);
      this.dirty = true;
      return;
    }
    if (e.kind === 'pixels') {
      for (const k in e.saved) { const cur = e.layer[k]; e.layer[k] = e.saved[k]; e.saved[k] = cur; }
    } else if (e.kind === 'add') {
      this.layers.splice(this.layers.indexOf(e.layer), 1);
      e.kind = 'remove';
    } else if (e.kind === 'remove') {
      this.layers.splice(e.index, 0, e.layer);
      e.kind = 'add';
    } else if (e.kind === 'move') {
      const [l] = this.layers.splice(e.to, 1);
      this.layers.splice(e.from, 0, l);
      [e.from, e.to] = [e.to, e.from];
    }
    this.selected = Math.min(Math.max(this.selected, 0), this.layers.length - 1);
    this.dirty = true;
  }

  undo() {
    const e = this.undoStack.pop();
    if (!e) return false;
    this._invert(e);
    this.redoStack.push(e);
    return true;
  }

  redo() {
    const e = this.redoStack.pop();
    if (!e) return false;
    this._invert(e);
    this.undoStack.push(e);
    return true;
  }

  // ---------------------------------------------------------------- strokes
  // view: { camera, cssWidth, cssHeight }  opts: brush/tool/material settings
  beginStroke(view, opts) {
    const l = this.layers[this.selected];
    if (!l || !l.visible) return false;
    const target = l.kind === 'paint' ? ['color', 'rmh'] : ['mask'];
    const saved = {};
    for (const k of target) { saved[k] = rt(this.size); this._copy(l[k], saved[k]); }
    this.stroke = { layer: l, saved, opts, view };
    this._clear(this.strokeRT, [0, 0, 0, 0]);
    if (!opts.uvSpace && view.camera) this._renderDepth(view);
    this.mats.dab.uniforms.uSeed.value = Math.random() * 100;
    return true;
  }

  _renderDepth(view) {
    const r = this.renderer;
    const pr = Math.min(r.getPixelRatio(), 2);
    const w = Math.max(16, Math.round(view.cssWidth * pr)), h = Math.max(16, Math.round(view.cssHeight * pr));
    if (!this.depthRT || this.depthRT.width !== w || this.depthRT.height !== h) {
      if (this.depthRT) this.depthRT.dispose();
      this.depthRT = new THREE.WebGLRenderTarget(w, h, { depthBuffer: true });
      this.depthRT.depthTexture = new THREE.DepthTexture(w, h);
      this.depthRT.depthTexture.type = THREE.UnsignedIntType;
    }
    const prev = r.getRenderTarget();
    r.setRenderTarget(this.depthRT);
    r.clear(true, true, false);
    this.depthMesh.material.side = THREE.FrontSide;
    r.render(this.depthScene, view.camera);
    r.setRenderTarget(prev);
  }

  // dabs: [{x, y, radius, strength}] in CSS px (y down) for 3D, or texels for UV space
  addDabs(dabs) {
    if (!this.stroke || !dabs.length) return;
    const s = this.stroke, o = s.opts, u = this.mats.dab.uniforms;
    u.uPos.value = this.posRT.texture;
    u.uNrm.value = this.nrmRT.texture;
    u.uMir.value = this.mirRT.texture;
    u.uMode.value = o.uvSpace ? 1 : 0;
    u.uHard.value = o.hardness;
    u.uTip.value = o.tip;
    u.uThrough.value = o.through ? 1 : 0;
    u.uSym.value = o.symmetry || 0;
    u.uTexSize.value = this.size;
    if (!o.uvSpace) {
      const cam = s.view.camera;
      cam.updateMatrixWorld();
      u.uVP.value.multiplyMatrices(cam.projectionMatrix, cam.matrixWorldInverse);
      u.uCamPos.value.setFromMatrixPosition(cam.matrixWorld);
      cam.getWorldDirection(u.uCamDir.value);
      u.uOrtho.value = cam.isOrthographicCamera ? 1 : 0;
      u.uNear.value = cam.near;
      u.uFar.value = cam.far;
      u.uView.value.set(s.view.cssWidth, s.view.cssHeight);
      u.uDepth.value = this.depthRT.depthTexture;
      u.uDepthTol.value = 0.015 * (this.geometry.boundingSphere ? this.geometry.boundingSphere.radius : 1);
    }
    for (let i = 0; i < dabs.length; i += MAX_DABS) {
      const chunk = dabs.slice(i, i + MAX_DABS);
      chunk.forEach((d, k) => {
        const y = o.uvSpace ? d.y : s.view.cssHeight - d.y;
        u.uDabs.value[k].set(d.x, y, d.radius, d.strength);
      });
      u.uCount.value = chunk.length;
      this._pass(this.mats.dab, this.strokeRT);
    }
    this._applyStroke();
  }

  _applyStroke() {
    const s = this.stroke, o = s.opts, l = s.layer, a = this.mats.apply, u = a.uniforms;
    u.uStroke.value = this.strokeRT.texture;
    u.uPos.value = this.posRT.texture;
    u.uNrm.value = this.nrmRT.texture;
    u.uTool.value = o.tool === 'erase' ? 1 : o.tool === 'blur' ? 2 : 0;
    u.uBlurR.value = (o.blurRadius || 6) * this.size / 1024;
    u.uOpacity.value = o.opacity;
    u.uChColor.value = o.channels.color ? 1 : 0;
    u.uChRM.value = o.channels.rm ? 1 : 0;
    u.uChHeight.value = o.channels.height ? 1 : 0;
    this._bindMaterial(a, o.material);
    const outs = l.kind === 'paint' ? [['color', 0], ['rmh', 1]] : [['mask', 2]];
    for (const [k, out] of outs) {
      u.uSnap.value = s.saved[k].texture;
      u.uOut.value = out;
      this._pass(a, l[k]);
    }
    this.dirty = true;
  }

  // Polygon fill: the triangles `tris` (and their edges, for the texels on their borders) at full
  // strength, as one stroke with the current material / tool
  fillTriangles(opts, tris) {
    if (!tris.length) return false;
    if (!this.beginStroke({ camera: null, cssWidth: 1, cssHeight: 1 }, { ...opts, uvSpace: true })) return false;
    const P = this.geometry.attributes.position.array, U = this.geometry.attributes.uv.array;
    const pos = new Float32Array(tris.length * 9), uv = new Float32Array(tris.length * 6);
    const lp = new Float32Array(tris.length * 18), lu = new Float32Array(tris.length * 12);
    tris.forEach((t, i) => {
      pos.set(P.subarray(9 * t, 9 * t + 9), 9 * i);
      uv.set(U.subarray(6 * t, 6 * t + 6), 6 * i);
      let k = 0;
      for (const [a, b] of [[0, 1], [1, 2], [2, 0]])
        for (const v of [a, b]) {
          lp.set(P.subarray(9 * t + 3 * v, 9 * t + 3 * v + 3), 18 * i + 3 * k);
          lu.set(U.subarray(6 * t + 2 * v, 6 * t + 2 * v + 2), 12 * i + 2 * k);
          k++;
        }
    });
    const g = new THREE.BufferGeometry(), gl = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    g.setAttribute('uv', new THREE.BufferAttribute(uv, 2));
    gl.setAttribute('position', new THREE.BufferAttribute(lp, 3));
    gl.setAttribute('uv', new THREE.BufferAttribute(lu, 2));
    const mat = new THREE.RawShaderMaterial({
      glslVersion: THREE.GLSL3,
      vertexShader: `precision highp float; in vec3 position; in vec2 uv; void main(){ gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0); }`,
      fragmentShader: `precision highp float; out vec4 o; void main(){ o = vec4(1.0); }`,
      side: THREE.DoubleSide, depthTest: false, depthWrite: false,
    });
    const sc = new THREE.Scene();
    const lines = new THREE.LineSegments(gl, mat), m = new THREE.Mesh(g, mat);
    lines.frustumCulled = m.frustumCulled = false;
    sc.add(lines, m);
    const r = this.renderer, prev = r.getRenderTarget(), ac = r.autoClear;
    r.autoClear = false;
    r.setRenderTarget(this.strokeRT);
    r.render(sc, this.quadCam);
    r.setRenderTarget(prev);
    r.autoClear = ac;
    mat.dispose(); g.dispose(); gl.dispose();
    this._applyStroke();
    this.endStroke();
    return true;
  }

  // the composited values at a texture coordinate: {color: [r,g,b] sRGB 0..1, roughness, metallic, height}
  sampleAt(u, v) {
    this.composite();
    const x = Math.min(this.size - 1, Math.max(0, Math.floor(u * this.size)));
    const y = Math.min(this.size - 1, Math.max(0, Math.floor(v * this.size)));
    const c = new Uint8Array(4), r = new Uint8Array(4);
    this.renderer.readRenderTargetPixels(this.colorResult, x, y, 1, 1, c);
    this.renderer.readRenderTargetPixels(this.rmhResult, x, y, 1, 1, r);
    return { color: [c[0] / 255, c[1] / 255, c[2] / 255], roughness: r[0] / 255, metallic: r[1] / 255, height: r[2] / 255 };
  }

  endStroke() {
    if (!this.stroke) return;
    this.pushUndo({ kind: 'pixels', layer: this.stroke.layer, saved: this.stroke.saved });
    this.stroke = null;
  }

  cancelStroke() {
    if (!this.stroke) return;
    const s = this.stroke;
    for (const k in s.saved) { this._copy(s.saved[k], s.layer[k]); s.saved[k].dispose(); }
    this.stroke = null;
    this.dirty = true;
  }

  // ---------------------------------------------------------------- compositing
  composite() {
    if (!this.dirty || !this.size) return false;
    this.dirty = false;
    const c = this.mats.composite, u = c.uniforms;
    u.uPos.value = this.posRT.texture;
    u.uNrm.value = this.nrmRT.texture;
    u.uCurv.value = this.curvRT.texture;
    const bc = hexToRgb(this.base.color);
    u.uBaseColor.value.set(bc[0], bc[1], bc[2]);
    u.uBaseRMH.value.set(this.base.roughness, this.base.metallic, 0.5);
    for (const [out, A0, B0] of [[0, this.accA, this.accB], [1, this.rmhA, this.rmhB]]) {
      let A = A0, B = B0;
      u.uOut.value = out;
      u.uKind.value = -1;
      this._pass(c, A);
      for (const l of this.layers) {
        if (!l.visible || l.opacity <= 0) continue;
        u.uAcc.value = A.texture;
        u.uOpacity.value = l.opacity;
        u.uBlend.value = BLENDS.indexOf(l.blend || 'normal');
        const gen = l.gen || {};
        u.uGen.value = GENERATORS.indexOf(gen.type || 'none');
        u.uGenLevel.value = gen.level ?? 0.5;
        u.uGenContrast.value = gen.contrast ?? 0.1;
        u.uGenNoise.value = gen.noise ?? 0;
        u.uGenInvert.value = gen.invert ? 1 : 0;
        if (l.kind === 'adjust') {
          const a = l.adjust;
          u.uKind.value = 2;
          u.uMask.value = l.mask.texture;
          u.uAdj.value.set(a.hue, a.saturation, a.brightness, a.contrast);
          u.uAdjRMH.value.set(a.roughness, a.metallic, a.height);
        } else if (l.kind === 'paint') {
          u.uGen.value = 0;
          u.uKind.value = 0;
          u.uLayer.value = (out === 0 ? l.color : l.rmh).texture;
          u.uChColor.value = u.uChRM.value = u.uChHeight.value = 1;
        } else {
          if (!l.material.isFlat && !cachedTextures(l.material.id)) continue;  // still loading
          u.uKind.value = 1;
          u.uMask.value = l.mask.texture;
          u.uChColor.value = l.channels.color ? 1 : 0;
          u.uChRM.value = l.channels.rm ? 1 : 0;
          u.uChHeight.value = l.channels.height ? 1 : 0;
          this._bindMaterial(c, l.material);
        }
        this._pass(c, B);
        [A, B] = [B, A];
      }
      if (out === 1) {
        // normals from the (undilated) height, island-border aware; dilated like the other maps
        const f = this.mats.final;
        f.uniforms.uRMH.value = A.texture;
        f.uniforms.uPos.value = this.posRT.texture;
        f.uniforms.uNormalStrength.value = this.normalStrength;
        f.uniforms.uOut.value = 2;
        this._pass(f, this.nA);
        this.normalResult = this._dilate(this.nA, this.nB);
      }
      const R = this._dilate(A, B);
      if (out === 0) this.colorResult = R; else this.rmhResult = R;
    }
    const f = this.mats.final;
    f.uniforms.uColor.value = this.colorResult.texture;
    f.uniforms.uRMH.value = this.rmhResult.texture;
    f.uniforms.uPos.value = this.posRT.texture;
    for (const [out, target] of [[0, this.finalColor], [1, this.finalORM]]) {
      f.uniforms.uOut.value = out;
      this._pass(f, target);
    }
    this._copy(this.normalResult, this.finalNormal);
    return true;
  }

  // fills empty gutter texels from covered neighbors; returns the target holding the result
  _dilate(A, B) {
    const d = this.mats.dilate;
    for (const step of [1, 1, 2, 4, 8, 16]) {
      d.uniforms.uSrc.value = A.texture;
      d.uniforms.uStep.value = step;
      this._pass(d, B);
      [A, B] = [B, A];
    }
    return A;
  }

  // ---------------------------------------------------------------- read-back for export
  // which: color | orm | normal | height | roughness | metallic -> {width, height, data(Uint8Array RGBA, bottom row first)}
  readMap(which) {
    this.composite();
    const outs = { color: 0, orm: 1, normal: 2, height: 3, roughness: 4, metallic: 5 };
    const t = rt(this.size);
    if (which === 'normal') {
      this._copy(this.normalResult, t);
    } else {
      const f = this.mats.final;
      f.uniforms.uColor.value = this.colorResult.texture;
      f.uniforms.uRMH.value = this.rmhResult.texture;
      f.uniforms.uPos.value = this.posRT.texture;
      f.uniforms.uOut.value = outs[which];
      this._pass(f, t);
    }
    const data = new Uint8Array(this.size * this.size * 4);
    this.renderer.readRenderTargetPixels(t, 0, 0, this.size, this.size, data);
    t.dispose();
    return { width: this.size, height: this.size, data };
  }

  // a layer map from an image (row 0 = the texture's row 0, as readLayer returns them)
  loadLayerImage(l, key, image) {
    const t = new THREE.Texture(image);
    t.flipY = false;
    t.colorSpace = THREE.NoColorSpace;
    t.premultiplyAlpha = false;
    t.minFilter = t.magFilter = THREE.NearestFilter;
    t.generateMipmaps = false;
    t.needsUpdate = true;
    const fs = raw(`in vec2 vUV; uniform sampler2D uSrc; out vec4 o; void main(){ o = texture(uSrc, vUV); }`, { uSrc: { value: t } });
    this._pass(fs, l[key]);
    fs.dispose();
    t.dispose();
    this.dirty = true;
  }

  readLayer(l, key) {
    const data = new Uint8Array(this.size * this.size * 4);
    this.renderer.readRenderTargetPixels(l[key], 0, 0, this.size, this.size, data);
    return data;
  }
}
