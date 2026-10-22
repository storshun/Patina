// Bakes object-space position, normal and tangent into texture space.
in vec3 vPos;
in vec3 vNormal;
in vec4 vTangent;
in vec2 vUV;
in vec4 vAux;
flat in uint vFlags;
flat in uint vFace;

uniform uint uSkipFlag = 0u;  // faces with this flag are left out (mirror copies: their texels belong to the other half)

layout(location = 0) out vec4 oPos;      // xyz, w = coverage
layout(location = 1) out vec4 oNormal;   // xyz, w = face id (as float)
layout(location = 2) out vec4 oTangent;  // xyz, w = bitangent sign
layout(location = 3) out vec4 oAux;      // per-loop auxiliary value (curvature)

void main() {
  if ((vFlags & uSkipFlag) != 0u) discard;
  oPos = vec4(vPos, 1.0);
  oNormal = vec4(normalize(vNormal), float(vFace));
  vec3 t = vTangent.xyz;
  oTangent = vec4(length(t) > 1e-8 ? normalize(t) : vec3(1, 0, 0), vTangent.w < 0.0 ? -1.0 : 1.0);
  oAux = vAux;
}
