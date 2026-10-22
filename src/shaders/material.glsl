// Material sampling shared by painting and fill layers.
// A material is either flat values or a set of tiling textures produced by the
// material library (procedural bake or imported images), projected onto the
// surface tri-planar in object space, by UV, or from a camera (screen planar).

uniform int uMatHasTex;          // 0 = flat values only
uniform int uMatMapping;         // 0 triplanar, 1 uv, 2 screen planar
uniform float uMatScale;         // tiles per object-space unit (triplanar/screen) or per UV unit
uniform float uMatRotation;      // radians
uniform vec2 uMatOffset;
uniform float uMatTriSharpness;  // triplanar blend sharpness
uniform int uMatTriBlend;        // 0 smooth cross-fade, 1 by height (crisp: no see-through double pattern)
uniform vec3 uMatTint;           // multiplies base color (linear factor applied in sRGB space)
uniform vec4 uMatFlatColor;      // sRGB base color for flat materials
uniform vec4 uMatFlat;           // x rough, y metal, z height, w unused
uniform vec3 uMatFlatEmissive;   // sRGB
uniform vec4 uMatFlatExtra = vec4(1, 0, 0, 0);  // opacity, sheen, clearcoat, subsurface (0.26)
uniform float uMatEmissiveStrength;
uniform vec2 uMatRoughRemap;     // value*x + y
uniform float uMatMetalOverride; // < 0 = keep
uniform float uMatHeightScale;   // scales (h - 0.5)
uniform float uMatHeightOffset;
uniform float uMatNormalStrength;
uniform mat4 uMatScreenVP;       // object -> clip of the projection camera (screen mapping)
uniform float uMatScreenAspect;
uniform float uMatImageAspect;   // width / height of the source image (screen mapping)

uniform sampler2D uMatColorTex;
uniform sampler2D uMatRMHTex;     // r rough, g metal, b height
uniform sampler2D uMatNormalTex;  // tangent-space, encoded
uniform sampler2D uMatEmissiveTex;
uniform sampler2D uMatExtraTex;   // r opacity, g sheen, b clearcoat, a subsurface

struct MatSample {
  vec3 color;     // sRGB
  float rough;
  float metal;
  float height;   // 0.5 neutral
  vec3 normal;    // tangent space of the mesh (z up)
  vec3 emissive;  // sRGB
  vec4 extra;     // opacity, sheen, clearcoat, subsurface
};

struct MatTap {
  vec3 color;
  vec3 rmh;
  vec3 n;  // tangent space of the projection plane
  vec3 emissive;
  vec4 extra;
};

MatTap matTap(vec2 uv) {
  uv = rot2(uMatRotation) * uv + uMatOffset;
  MatTap t;
  t.color = texture(uMatColorTex, uv).rgb;
  t.rmh = texture(uMatRMHTex, uv).rgb;
  vec3 n = decodeNormal(texture(uMatNormalTex, uv).rgb);
  n.xy = transpose(rot2(uMatRotation)) * n.xy;  // back into the unrotated plane frame
  n.xy *= uMatNormalStrength;
  t.n = normalize(n);
  t.emissive = texture(uMatEmissiveTex, uv).rgb;
  t.extra = texture(uMatExtraTex, uv);
  return t;
}

// Texture tiles: the atlas grid and each cell's UV origin, to turn an atlas position back into
// the UV as authored (UV-mapped materials follow the model's UVs, not the atlas layout).
uniform int uAtlasGrid = 1;
uniform vec2 uAtlasOrigin[64];
vec2 atlasToMeshUV(vec2 a) {
  if (uAtlasGrid <= 1) return a;
  float g = float(uAtlasGrid);
  vec2 c = clamp(floor(a * g), vec2(0.0), vec2(g - 1.0));
  return a * g - c + uAtlasOrigin[int(c.y) * uAtlasGrid + int(c.x)];
}

// pos/normal: object space; T/sign: mesh tangent frame; meshUV: texture coordinate
MatSample sampleMaterial(vec3 pos, vec3 N, vec4 T, vec2 meshUV) {
  MatSample s;
  s.normal = vec3(0, 0, 1);
  if (uMatHasTex == 0) {
    s.color = uMatFlatColor.rgb;
    s.rough = uMatFlat.x;
    s.metal = uMatFlat.y;
    s.height = uMatFlat.z;
    s.emissive = uMatFlatEmissive;
    s.extra = uMatFlatExtra;
  } else if (uMatMapping == 1 || uMatMapping == 2) {
    vec2 uv;
    if (uMatMapping == 1) {
      uv = atlasToMeshUV(meshUV) * uMatScale;
    } else {
      vec4 c = uMatScreenVP * vec4(pos, 1.0);
      vec2 ndc = c.xy / max(c.w, 1e-6);
      // centered on the view, image aspect preserved: scale 1 = image fills the view height
      uv = vec2(ndc.x * uMatScreenAspect / max(uMatImageAspect, 1e-3), ndc.y) * 0.5 * uMatScale + 0.5;
    }
    MatTap t = matTap(uv);
    s.color = t.color;
    s.rough = t.rmh.r;
    s.metal = t.rmh.g;
    s.height = t.rmh.b;
    s.emissive = t.emissive;
    s.extra = t.extra;
    if (uMatMapping == 1) {
      s.normal = t.n;
    } else {
      // screen projection: plane normal = toward camera; approximate with mesh normal frame
      s.normal = t.n;
    }
  } else {
    vec3 w = pow(abs(N), vec3(uMatTriSharpness));
    w /= max(w.x + w.y + w.z, 1e-5);
    vec3 p = pos * uMatScale;
    // Skip planes with negligible weight (saves texture taps)
    MatTap zero = MatTap(vec3(0), vec3(0), vec3(0, 0, 1), vec3(0), vec4(0));
    MatTap tx = w.x > 0.001 ? matTap(p.zy) : zero;
    MatTap ty = w.y > 0.001 ? matTap(p.xz) : zero;
    MatTap tz = w.z > 0.001 ? matTap(p.xy) : zero;
    if (uMatTriBlend == 1) {
      // Where two projections meet, a cross-fade shows both patterns at part strength (ghosting,
      // worst with sharp repeating patterns such as tread plate or tiles). Here each point takes
      // the projection that stands highest there, weighted by how squarely it faces that
      // projection, with only a thin blend: transitions follow the pattern and stay opaque.
      vec3 h = vec3(w.x > 0.001 ? tx.rmh.b : 0.0, w.y > 0.001 ? ty.rmh.b : 0.0, w.z > 0.001 ? tz.rmh.b : 0.0);
      vec3 ht = w + h * 0.25 * step(vec3(0.001), w);
      float ma = max(ht.x, max(ht.y, ht.z)) - 0.04;
      w = max(ht - ma, 0.0);
      w /= max(w.x + w.y + w.z, 1e-5);
    }
    s.color = tx.color * w.x + ty.color * w.y + tz.color * w.z;
    vec3 rmh = tx.rmh * w.x + ty.rmh * w.y + tz.rmh * w.z;
    s.rough = rmh.r;
    s.metal = rmh.g;
    s.height = rmh.b;
    s.emissive = tx.emissive * w.x + ty.emissive * w.y + tz.emissive * w.z;
    s.extra = tx.extra * w.x + ty.extra * w.y + tz.extra * w.z;
    // Whiteout tri-planar normal blend (Ben Golus) -> object space
    vec3 nx = tx.n, ny = ty.n, nz = tz.n;
    vec3 ax = vec3(nx.xy + N.zy, abs(nx.z) * N.x);
    vec3 ay = vec3(ny.xy + N.xz, abs(ny.z) * N.y);
    vec3 az = vec3(nz.xy + N.xy, abs(nz.z) * N.z);
    vec3 objN = normalize(ax.zyx * w.x + ay.xzy * w.y + az.xyz * w.z);
    // object space -> mesh tangent space
    vec3 Tn = normalize(T.xyz - N * dot(N, T.xyz));
    vec3 B = cross(N, Tn) * (T.w < 0.0 ? -1.0 : 1.0);
    s.normal = normalize(vec3(dot(objN, Tn), dot(objN, B), dot(objN, N)));
  }
  s.color *= uMatTint;
  s.rough = clamp(s.rough * uMatRoughRemap.x + uMatRoughRemap.y, 0.0, 1.0);
  if (uMatMetalOverride >= 0.0) s.metal = uMatMetalOverride;
  s.height = clamp(0.5 + (s.height - 0.5) * uMatHeightScale + uMatHeightOffset, 0.0, 1.0);
  s.emissive *= uMatEmissiveStrength;
  return s;
}
