layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent;
layout(location = 4) in uvec2 aIds;
layout(location = 5) in uint aFlags;

uniform mat4 uModel;
uniform mat4 uViewProj;

out vec3 vWorldPos;
out vec3 vObjPos;
out vec3 vNormal;
out vec4 vTangent;
out vec2 vUV;
flat out uint vFlags;
flat out uvec2 vIds;

void main() {
  vec4 wp = uModel * vec4(aPos, 1.0);
  vWorldPos = wp.xyz;
  vObjPos = aPos;
  mat3 nm = transpose(inverse(mat3(uModel)));
  vNormal = nm * aNormal;
  vTangent = vec4(mat3(uModel) * aTangent.xyz, aTangent.w);
  vUV = aUV;
  vFlags = aFlags;
  vIds = aIds;
  gl_Position = uViewProj * wp;
}
