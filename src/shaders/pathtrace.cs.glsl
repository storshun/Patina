// (0.39) The path tracer (engine/PathTracer.cpp). One path per pixel (one per block of pixels for the quick picture
// while things change), added to a floating-point image. The model: the GPU mesh's own vertex buffer, its triangles
// and the BVH; its look: the final textures; its light: the environment, sampled by importance (a luminance map's
// CDFs) and by the BSDF, the two weighed with the power heuristic (MIS). A ground under the model catches its
// shadow: B holds how much environment light reaches the ground there.
// ENV_LUM: the luminance map of the environment instead (the CDFs are made from it on the CPU).
#include "common.glsl"
layout(local_size_x = 8, local_size_y = 8) in;


// the environment as an equirectangular map: u round the Y axis, v from the top (+Y) down
vec3 equirectDir(vec2 uv) {
  float th = uv.y * PI, ph = uv.x * 2.0 * PI;
  return vec3(sin(th) * sin(ph), cos(th), sin(th) * cos(ph));
}
vec2 equirectUV(vec3 d) {
  float u = atan(d.x, d.z) / (2.0 * PI);
  return vec2(u < 0.0 ? u + 1.0 : u, acos(clamp(d.y, -1.0, 1.0)) / PI);
}

uniform samplerCube uEnvSpec;

#ifdef ENV_LUM
layout(r32f, binding = 0) uniform writeonly image2D uLum;
uniform ivec2 uEnvSize;
uniform float uLod;
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (p.x >= uEnvSize.x || p.y >= uEnvSize.y) return;
  vec3 d = equirectDir((vec2(p) + 0.5) / vec2(uEnvSize));
  imageStore(uLum, p, vec4(luminance(textureLod(uEnvSpec, d, uLod).rgb)));
}
#else

layout(rgba32f, binding = 0) uniform image2D uAccumA;  // rgb: the model's light, a: samples
layout(rgba32f, binding = 1) uniform image2D uAccumB;  // ground: light reaching it, its weight, its samples; a: model samples
layout(std430, binding = 0) readonly buffer Verts { float vtx[]; };   // per loop: pos 3, normal 3, uv 2, tangent 4, mesh uv 2
layout(std430, binding = 1) readonly buffer Tris { uint tri[]; };     // per triangle: 3 loops
layout(std430, binding = 2) readonly buffer Nodes { uint nodes[]; };  // per node: lo 3, hi 3 (floats), left, count
layout(std430, binding = 3) readonly buffer TriIdx { uint triIdx[]; };

uniform sampler2D uColorTex;
uniform sampler2D uRMHTex;
uniform sampler2D uNormalTex;
uniform sampler2D uEmissiveTex;
uniform sampler2D uExtraTex;
#ifdef SCENE
// (1.2) several objects: their triangles together, each with the object whose textures it reads (a layer of these)
layout(std430, binding = 4) readonly buffer TriObj { uint triObj[]; };
uniform sampler2DArray uColorArr;
uniform sampler2DArray uRMHArr;
uniform sampler2DArray uNormalArr;
uniform sampler2DArray uEmissiveArr;
uniform sampler2DArray uExtraArr;
#define TEX(name, uv, t) textureLod(name##Arr, vec3(uv, float(triObj[t])), 0.0)
#else
#define TEX(name, uv, t) textureLod(name##Tex, uv, 0.0)
#endif
uniform sampler2D uEnvCond;  // per row: the CDF over u, W + 1 values from 0 to 1
uniform sampler2D uEnvMarg;  // the CDF over the rows, H + 1 values
uniform ivec2 uEnvSize;
uniform float uEnvTotal;     // 0: no environment light to sample
uniform mat3 uEnvRot;        // world -> environment
uniform float uEnvIntensity;
uniform mat4 uInvViewProj;
uniform ivec2 uSize;
uniform int uY0;
uniform int uBlock;
uniform int uOverwrite;
uniform uint uFrame;
uniform int uBounces;
uniform float uClamp;
uniform int uGround;
uniform float uGroundY;
uniform int uOpacity;
uniform float uEps;          // ray offset off a surface (object units)

// ---- random numbers (PCG)
uint rng;
uint pcg(uint v) {
  uint s = v * 747796405u + 2891336453u;
  uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}
float rnd() {
  rng = pcg(rng);
  return float(rng) * (1.0 / 4294967296.0);
}

// ---- the model
const uint VS = 14u;
vec3 vPos(uint l) { uint b = l * VS; return vec3(vtx[b], vtx[b + 1u], vtx[b + 2u]); }
vec3 vNrm(uint l) { uint b = l * VS + 3u; return vec3(vtx[b], vtx[b + 1u], vtx[b + 2u]); }
vec2 vUV(uint l) { uint b = l * VS + 6u; return vec2(vtx[b], vtx[b + 1u]); }
vec4 vTan(uint l) { uint b = l * VS + 8u; return vec4(vtx[b], vtx[b + 1u], vtx[b + 2u], vtx[b + 3u]); }
vec3 nLo(uint n) { uint b = n * 8u; return uintBitsToFloat(uvec3(nodes[b], nodes[b + 1u], nodes[b + 2u])); }
vec3 nHi(uint n) { uint b = n * 8u + 3u; return uintBitsToFloat(uvec3(nodes[b], nodes[b + 1u], nodes[b + 2u])); }

bool hitBox(uint n, vec3 o, vec3 inv, float tmax) {
  vec3 t0 = (nLo(n) - o) * inv, t1 = (nHi(n) - o) * inv;
  vec3 a = min(t0, t1), b = max(t0, t1);
  return max(max(a.x, a.y), max(a.z, 0.0)) <= min(min(b.x, b.y), min(b.z, tmax));
}
float boxNear(uint n, vec3 o, vec3 inv) {
  vec3 t0 = (nLo(n) - o) * inv, t1 = (nHi(n) - o) * inv;
  vec3 a = min(t0, t1);
  return max(max(a.x, a.y), max(a.z, 0.0));
}

bool hitTri(uint t, vec3 o, vec3 d, float tmax, out float th, out vec2 bc) {
  vec3 p0 = vPos(tri[3u * t]), p1 = vPos(tri[3u * t + 1u]), p2 = vPos(tri[3u * t + 2u]);
  vec3 e1 = p1 - p0, e2 = p2 - p0, pv = cross(d, e2);
  float det = dot(e1, pv);
  if (abs(det) < 1e-14) return false;
  float id = 1.0 / det;
  vec3 tv = o - p0;
  float u = dot(tv, pv) * id;
  if (u < 0.0 || u > 1.0) return false;
  vec3 qv = cross(tv, e1);
  float v = dot(d, qv) * id;
  if (v < 0.0 || u + v > 1.0) return false;
  th = dot(e2, qv) * id;
  bc = vec2(u, v);
  return th > 0.0 && th < tmax;
}

// see-through parts (the opacity map): a hit there counts only as often as it is opaque
bool opaqueAt(uint t, vec2 bc) {
  if (uOpacity == 0) return true;
  vec2 uv = vUV(tri[3u * t]) * (1.0 - bc.x - bc.y) + vUV(tri[3u * t + 1u]) * bc.x + vUV(tri[3u * t + 2u]) * bc.y;
  return rnd() < TEX(uExtra, uv, t).r;
}

vec3 invDir(vec3 d) { return 1.0 / mix(d, vec3(1e-20) * sign(d + 1e-30), lessThan(abs(d), vec3(1e-20))); }

// (1.2.3) the triangle a ray leaves from, left out of its tests: the ray starts on the smooth surface its normals
// describe (Mat.off), which can lie a little above that flat triangle
const uint NO_TRI = 0xFFFFFFFFu;
uint gSkip = NO_TRI;

// the nearest hit (anyHit: any hit, for shadow rays)
bool trace(vec3 o, vec3 d, float tmax, bool anyHit, out uint ht, out vec2 hb, out float tt) {
  vec3 inv = invDir(d);
  uint stack[64];
  int sp = 0;
  uint n = 0u;
  bool found = false;
  ht = 0u, hb = vec2(0.0), tt = tmax;
  if (!hitBox(0u, o, inv, tmax)) return false;
  for (int guard = 0; guard < 100000; ++guard) {
    uint cnt = nodes[n * 8u + 7u];
    if (cnt > 0u) {
      uint first = nodes[n * 8u + 6u];
      for (uint i = 0u; i < cnt; ++i) {
        uint t = triIdx[first + i];
        if (t == gSkip) continue;
        float th;
        vec2 bc;
        if (hitTri(t, o, d, tt, th, bc) && opaqueAt(t, bc)) {
          tt = th, ht = t, hb = bc, found = true;
          if (anyHit) return true;
        }
      }
    } else {
      uint a = n + 1u, b = nodes[n * 8u + 6u];
      bool ha = hitBox(a, o, inv, tt), hb2 = hitBox(b, o, inv, tt);
      if (ha && hb2) {
        if (boxNear(a, o, inv) > boxNear(b, o, inv)) { uint s = a; a = b; b = s; }
        if (sp < 64) stack[sp++] = b;
        n = a;
        continue;
      }
      if (ha) { n = a; continue; }
      if (hb2) { n = b; continue; }
    }
    // the next one still waiting that the ray can reach before what it hit
    bool next = false;
    while (sp > 0) {
      n = stack[--sp];
      if (hitBox(n, o, inv, tt)) { next = true; break; }
    }
    if (!next) break;
  }
  return found;
}

// ---- the environment
vec3 envLight(vec3 wd) { return textureLod(uEnvSpec, uEnvRot * wd, 0.0).rgb * uEnvIntensity; }
float margAt(int i) { return texelFetch(uEnvMarg, ivec2(i, 0), 0).r; }
float condAt(int x, int y) { return texelFetch(uEnvCond, ivec2(x, y), 0).r; }

float envPdf(vec3 wd) {
  if (uEnvTotal <= 0.0) return 0.0;
  vec3 e = uEnvRot * wd;
  vec2 uv = equirectUV(e);
  int W = uEnvSize.x, H = uEnvSize.y;
  int x = clamp(int(uv.x * float(W)), 0, W - 1), y = clamp(int(uv.y * float(H)), 0, H - 1);
  float p = (condAt(x + 1, y) - condAt(x, y)) * float(W) * (margAt(y + 1) - margAt(y)) * float(H);
  float s = sin(uv.y * PI);
  return s > 1e-6 ? p / (2.0 * PI * PI * s) : 0.0;
}

vec3 sampleEnv(out float pdf) {
  int W = uEnvSize.x, H = uEnvSize.y;
  float r1 = rnd(), r2 = rnd();
  int lo = 0, hi = H;  // the row: margAt(lo) <= r1 < margAt(lo + 1)
  while (hi - lo > 1) {
    int m = (lo + hi) / 2;
    if (margAt(m) <= r1) lo = m; else hi = m;
  }
  int y = lo;
  float c0 = margAt(y), c1 = margAt(y + 1);
  float v = (float(y) + clamp((r1 - c0) / max(c1 - c0, 1e-12), 0.0, 1.0)) / float(H);
  lo = 0, hi = W;
  while (hi - lo > 1) {
    int m = (lo + hi) / 2;
    if (condAt(m, y) <= r2) lo = m; else hi = m;
  }
  int x = lo;
  float d0 = condAt(x, y), d1 = condAt(x + 1, y);
  float u = (float(x) + clamp((r2 - d0) / max(d1 - d0, 1e-12), 0.0, 1.0)) / float(W);
  float p = (d1 - d0) * float(W) * (c1 - c0) * float(H);
  float s = sin(v * PI);
  pdf = s > 1e-6 ? p / (2.0 * PI * PI * s) : 0.0;
  return transpose(uEnvRot) * equirectDir(vec2(u, v));
}

// ---- the material
struct Mat {
  vec3 base;
  float rough, metal;
  vec3 emis;
  float sheen, coat, sss;
  vec3 N, Ng;  // shading normal (normal map), geometric normal; both on the side the ray came from
  vec3 Ns;     // (1.2.3) the smooth normal (the vertex normals, before the normal map)
  vec3 off;    // (1.2.3) from the flat triangle out to the smooth surface: where rays leave from
};

// (1.2.3) The shadow terminator. A smooth-shaded model is still made of flat triangles: light cut off at each
// triangle's own horizon, and shadow rays leaving from the flat triangle into its neighbors, shade a curved part (a
// nose, a cheek) in triangle-shaped steps where light grazes it. Here light fades at the smooth normal's horizon
// (Chiang et al. 2019, also for a normal map bending N away from Ns), and rays leave from the smooth surface: each
// vertex's tangent plane, mixed by the barycentrics, as Cycles does (Hanika 2021, "Hacking the shadow terminator").
float shadowTerm(vec3 N, vec3 Ns, vec3 L) {
  float ngl = dot(Ns, L);
  if (ngl <= 0.0) return 0.0;
  float g = min(1.0, ngl / max(dot(N, L) * dot(N, Ns), 1e-6));
  return -g * g * g + g * g + g;
}

void onb(vec3 n, out vec3 t, out vec3 b) {
  float s = n.z >= 0.0 ? 1.0 : -1.0;
  float a = -1.0 / (s + n.z), c = n.x * n.y * a;
  t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
  b = vec3(c, s + n.y * n.y * a, -n.y);
}
vec3 cosineDir(vec3 n) {
  float r = sqrt(rnd()), ph = 2.0 * PI * rnd();
  vec3 t, b;
  onb(n, t, b);
  return normalize(t * (r * cos(ph)) + b * (r * sin(ph)) + n * sqrt(max(0.0, 1.0 - r * r)));
}
float dGGX(float nh, float a2) {
  float d = nh * nh * (a2 - 1.0) + 1.0;
  return a2 / (PI * d * d);
}
float g1(float nx, float a2) { return 2.0 * nx / (nx + sqrt(a2 + (1.0 - a2) * nx * nx)); }
// a microfacet normal as the view sees them (Heitz 2018), in the frame of n
vec3 sampleVndf(vec3 V, vec3 n, float a) {
  vec3 t, b;
  onb(n, t, b);
  vec3 Ve = vec3(dot(V, t), dot(V, b), dot(V, n));
  vec3 Vh = normalize(vec3(a * Ve.x, a * Ve.y, Ve.z));
  float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
  vec3 T1 = lensq > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
  vec3 T2 = cross(Vh, T1);
  float r = sqrt(rnd()), ph = 2.0 * PI * rnd();
  float t1 = r * cos(ph), t2 = r * sin(ph), s = 0.5 * (1.0 + Vh.z);
  t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;
  vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
  vec3 h = normalize(vec3(a * Nh.x, a * Nh.y, max(0.0, Nh.z)));
  return normalize(t * h.x + b * h.y + n * h.z);
}

const float COAT_A = 0.05;

// how likely each lobe is chosen: diffuse, specular, clearcoat, through (subsurface)
vec4 lobes(Mat m, float nv) {
  vec3 F0 = mix(vec3(0.04), m.base, m.metal);
  float fv = luminance(F0 + (1.0 - F0) * pow(1.0 - nv, 5.0));
  float coatF = m.coat * (0.04 + 0.96 * pow(1.0 - nv, 5.0));
  float diff = luminance(m.base) * (1.0 - m.metal) * (1.0 - fv);
  vec4 w = vec4(diff * (1.0 - 0.5 * m.sss), max(fv, 0.02), coatF, diff * 0.5 * m.sss);
  return w / max(w.x + w.y + w.z + w.w, 1e-6);
}

// the BSDF times the cosine for light from L seen from V, and the pdf of choosing L
vec3 evalBsdf(Mat m, vec3 V, vec3 L, out float pdf) {
  vec3 N = m.N;
  float nv = max(dot(N, V), 1e-4), nl = dot(N, L);
  vec4 p = lobes(m, nv);
  pdf = 0.0;
  if (nl > 0.0 && dot(m.Ns, L) > 0.0) {
    vec3 H = normalize(V + L);
    float nh = max(dot(N, H), 0.0), vh = max(dot(V, H), 0.0);
    float a = max(m.rough * m.rough, 1e-3), a2 = a * a;  // (the same as sampleBsdf's)
    vec3 F0 = mix(vec3(0.04), m.base, m.metal);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - vh, 5.0);
    float D = dGGX(nh, a2), Gv = g1(nv, a2), Gl = g1(nl, a2);
    vec3 spec = D * Gv * Gl * F / (4.0 * nv * nl);
    vec3 diff = m.base * (1.0 - m.metal) * (1.0 - luminance(F0 + (1.0 - F0) * pow(1.0 - nv, 5.0))) * (1.0 - 0.5 * m.sss) / PI;
    // sheen (velvet, satin): a soft bright rim at grazing angles, as the raster view's
    vec3 sheen = vec3(m.sheen * (0.02 + 0.4 * pow(1.0 - nv, 4.0)) / PI);
    vec3 f = diff + spec + sheen;
    pdf += p.x * nl / PI + p.y * Gv * D / (4.0 * nv);
    if (m.coat > 0.001) {
      float ca2 = COAT_A * COAT_A;
      float Fc = 0.04 + 0.96 * pow(1.0 - vh, 5.0);
      float coat = dGGX(nh, ca2) * g1(nv, ca2) * g1(nl, ca2) * Fc / (4.0 * nv * nl);
      f = f * (1.0 - m.coat * (0.04 + 0.96 * pow(1.0 - nv, 5.0))) + vec3(m.coat * coat);
      pdf += p.z * g1(nv, ca2) * dGGX(nh, ca2) / (4.0 * nv);
    }
    return f * nl * shadowTerm(N, m.Ns, L);
  }
  if (nl < 0.0 && m.sss > 0.001) {
    // subsurface: light through thin parts (ears, leaves), warmer
    pdf += p.w * -nl / PI;
    return m.base * (1.0 - m.metal) * vec3(1.0, 0.55, 0.4) * 0.5 * m.sss / PI * -nl;
  }
  return vec3(0.0);
}

vec3 sampleBsdf(Mat m, vec3 V, out vec3 L, out float pdf) {
  vec3 N = m.N;
  vec4 p = lobes(m, max(dot(N, V), 1e-4));
  float r = rnd();
  if (r < p.x) L = cosineDir(N);
  else if (r < p.x + p.y) L = reflect(-V, sampleVndf(V, N, max(m.rough * m.rough, 1e-3)));
  else if (r < p.x + p.y + p.z) L = reflect(-V, sampleVndf(V, N, COAT_A));
  else L = cosineDir(-N);
  return evalBsdf(m, V, L, pdf);
}

Mat material(uint t, vec2 bc, vec3 d) {
  uint l0 = tri[3u * t], l1 = tri[3u * t + 1u], l2 = tri[3u * t + 2u];
  vec3 w = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
  vec3 p0 = vPos(l0), p1 = vPos(l1), p2 = vPos(l2);
  vec3 Ng = normalize(cross(p1 - p0, p2 - p0));
  vec3 n0 = vNrm(l0), n1 = vNrm(l1), n2 = vNrm(l2);
  vec3 Ns = normalize(n0 * w.x + n1 * w.y + n2 * w.z);
  vec4 t0 = vTan(l0);
  vec3 T = t0.xyz * w.x + vTan(l1).xyz * w.y + vTan(l2).xyz * w.z;
  vec2 uv = vUV(l0) * w.x + vUV(l1) * w.y + vUV(l2) * w.z;
  // two-sided: the side the ray came from
  if (dot(Ng, d) > 0.0) Ng = -Ng;
  float side = dot(Ns, Ng) < 0.0 ? -1.0 : 1.0;
  Ns *= side;
  // (1.2.3) the smooth surface over this point: only ever outward, so a hollow gets none
  vec3 P = p0 * w.x + p1 * w.y + p2 * w.z;
  n0 *= side, n1 *= side, n2 *= side;
  vec3 off = -(n0 * (w.x * min(0.0, dot(P - p0, n0))) + n1 * (w.y * min(0.0, dot(P - p1, n1))) +
               n2 * (w.z * min(0.0, dot(P - p2, n2))));
  T = normalize(T - Ns * dot(Ns, T) + vec3(1e-6, 0.0, 0.0));
  vec3 B = cross(Ns, T) * (t0.w < 0.0 ? -1.0 : 1.0);
  vec3 tn = normalize(TEX(uNormal, uv, t).xyz);
  vec3 N = normalize(T * tn.x + B * tn.y + Ns * tn.z);
  // (a normal map that points away from the viewer: bent back just in front)
  vec3 V = -d;
  float nv = dot(N, V);
  if (nv < 0.02) N = normalize(N + V * (0.02 - nv));
  Mat m;
  m.base = srgbToLinear(TEX(uColor, uv, t).rgb);
  vec4 rmh = TEX(uRMH, uv, t);
  m.rough = clamp(rmh.r, 0.03, 1.0);
  m.metal = clamp(rmh.g, 0.0, 1.0);
  m.emis = srgbToLinear(TEX(uEmissive, uv, t).rgb) * 4.0;
  vec4 xt = TEX(uExtra, uv, t);
  m.sheen = xt.g, m.coat = xt.b, m.sss = xt.a;
  m.N = N, m.Ng = Ng, m.Ns = Ns, m.off = dot(off, Ng) > 0.0 ? off : vec3(0.0);
  return m;
}

vec3 clampLight(vec3 c) {
  float l = luminance(c);
  return uClamp > 0.0 && l > uClamp ? c * (uClamp / l) : c;
}

void main() {
  ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
  ivec2 px = ivec2(cell.x * uBlock, uY0 + cell.y * uBlock);
  if (px.x >= uSize.x || px.y >= uSize.y) return;
  rng = pcg(uint(px.x) * 1973u + uint(px.y) * 9277u + uFrame * 26699u) | 1u;
  // the ray through a random point of the pixel (or the block)
  vec2 ndc = (vec2(px) + vec2(rnd(), rnd()) * float(uBlock)) / vec2(uSize) * 2.0 - 1.0;
  vec4 a = uInvViewProj * vec4(ndc, -1.0, 1.0), b = uInvViewProj * vec4(ndc, 1.0, 1.0);
  vec3 o = a.xyz / a.w, d = normalize(b.xyz / b.w - a.xyz / a.w);

  vec3 Lsum = vec3(0.0);
  vec4 g = vec4(0.0);  // ground: light reaching it, its weight, its samples, model samples
  uint t;
  vec2 bc;
  float th;
  bool hit = trace(o, d, 1e30, false, t, bc, th);
  float tg = uGround == 1 && d.y < -1e-6 ? (uGroundY - o.y) / d.y : -1.0;
  if ((!hit || (tg > 0.0 && tg < th)) && tg > 0.0) {
    // the ground: how much of the environment's light the model leaves it (the environment sampled by its light)
    vec3 P = o + d * tg + vec3(0.0, uEps, 0.0);
    for (int k = 0; k < 4 && uEnvTotal > 0.0; ++k) {
      float pe;
      vec3 wl = sampleEnv(pe);
      if (wl.y <= 0.0 || pe <= 0.0) continue;
      uint t2;
      vec2 b2;
      float t3;
      g.x += trace(P, wl, 1e30, true, t2, b2, t3) ? 0.0 : wl.y;
      g.y += wl.y;
    }
    g.z = 1.0;
  } else if (hit) {
    g.w = 1.0;
    vec3 thr = vec3(1.0);
    float lastPdf = 0.0;
    for (int bounce = 0; bounce <= uBounces; ++bounce) {
      Mat m = material(t, bc, d);
      vec3 P = o + d * th;
      vec3 V = -d;
      Lsum += thr * m.emis;
      // (1.2.3) rays out leave from the smooth surface (and pass their own triangle), rays in from just under it
      vec3 Pout = P + m.off + m.Ng * uEps, Pin = P - m.Ng * uEps;
      uint here = t;
      // the environment's light straight from where it is brightest
      if (uEnvTotal > 0.0) {
        float pe;
        vec3 wl = sampleEnv(pe);
        float pb;
        vec3 fc = evalBsdf(m, V, wl, pb);
        if (pe > 0.0 && (fc.x + fc.y + fc.z) > 0.0) {
          bool leaves = dot(wl, m.Ns) > 0.0;
          uint t2;
          vec2 b2;
          float t3;
          gSkip = leaves ? here : NO_TRI;
          bool shadowed = trace(leaves ? Pout : Pin, wl, 1e30, true, t2, b2, t3);
          gSkip = NO_TRI;
          if (!shadowed) {
            vec3 c = thr * fc * envLight(wl) * (pe / (pe * pe + pb * pb));
            Lsum += bounce > 0 ? clampLight(c) : c;
          }
        }
      }
      // on along the BSDF
      vec3 L;
      float pdf;
      vec3 f = sampleBsdf(m, V, L, pdf);
      if (pdf < 1e-7 || (f.x + f.y + f.z) <= 0.0) break;
      thr *= f / pdf;
      lastPdf = pdf;
      if (bounce >= 2) {
        float q = clamp(max(thr.x, max(thr.y, thr.z)), 0.05, 0.95);
        if (rnd() > q) break;
        thr /= q;
      }
      bool leaves = dot(L, m.Ns) > 0.0;
      o = leaves ? Pout : Pin;
      d = L;
      gSkip = leaves ? here : NO_TRI;
      bool more = trace(o, d, 1e30, false, t, bc, th);
      gSkip = NO_TRI;
      if (!more) {
        float pe = envPdf(d);
        vec3 c = thr * envLight(d) * (lastPdf * lastPdf / (lastPdf * lastPdf + pe * pe));
        Lsum += clampLight(c);
        break;
      }
    }
  }
  if (any(isnan(Lsum)) || any(isinf(Lsum))) Lsum = vec3(0.0);
  vec4 sa = vec4(Lsum, 1.0), sb = g;
  for (int j = 0; j < uBlock; ++j)
    for (int i = 0; i < uBlock; ++i) {
      ivec2 q = px + ivec2(i, j);
      if (q.x >= uSize.x || q.y >= uSize.y) continue;
      if (uOverwrite == 1) {
        imageStore(uAccumA, q, sa);
        imageStore(uAccumB, q, sb);
      } else {
        imageStore(uAccumA, q, imageLoad(uAccumA, q) + sa);
        imageStore(uAccumB, q, imageLoad(uAccumB, q) + sb);
      }
    }
}
#endif
