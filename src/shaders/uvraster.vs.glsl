// Rasterizes the mesh into texture space: clip position = UV.
// Carries object-space geometry for per-texel 3D evaluation.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent;
layout(location = 4) in uvec2 aIds;
layout(location = 5) in uint aFlags;
layout(location = 6) in vec4 aAux;
layout(location = 7) in vec2 aMeshUV;

uniform vec4 uUVTransform = vec4(1, 1, 0, 0);  // scale.xy, offset.zw applied to the raster position
uniform int uUseAuxAsUV = 0;                    // reprojection: rasterize at aAux.xy instead of aUV
uniform int uUseMeshUV = 0;                     // UV editor: the UVs as authored, not the atlas
uniform int uTile = -1;                         // UV editor: only the faces of this texture tile

out vec3 vPos;
out vec3 vNormal;
out vec4 vTangent;
out vec2 vUV;
out vec4 vAux;
flat out uint vFlags;
flat out uint vFace;

void main() {
  vPos = aPos;
  vNormal = aNormal;
  vTangent = aTangent;
  vUV = aUV;
  vAux = aAux;
  vFlags = aFlags;
  vFace = aIds.x;
  vec2 rasterUV = uUseAuxAsUV == 1 ? aAux.xy : uUseMeshUV == 1 ? aMeshUV : aUV;
  vec2 p = rasterUV * uUVTransform.xy + uUVTransform.zw;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
  if (uTile >= 0 && int((aFlags >> 16) & 255u) != uTile) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // clipped away
}
