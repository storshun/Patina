// Writes one channel of a model's own material (a texture and/or a value) into a paint layer channel, in texture
// space. vAux carries the model's UV (xy) and the face's material slot (z); only the faces of uSlot are written.
#include "common.glsl"
in vec4 vAux;
flat in uint vFlags;

uniform uint uSkipFlag = 0u;  // mirror copies: their texels belong to the other half
uniform int uSlot;
uniform int uMode;            // 0 color, 1 one value, 2 normal, 3 emissive, 4 opacity, 5 height (bump),
                              // 6 vertex colors (vAux: linear rgb, w = 1 where they apply; blended to multiply)
uniform int uHasMap;
uniform sampler2D uMap;
uniform mat3 uUVXf;
uniform vec4 uFactor;         // color / emissive: linear rgb multiplier; value: x
uniform int uChannel;         // one value: the channel holding it
uniform int uInvert;          // one value: 1 - value
uniform int uFlipGreen;       // normal: DirectX maps
uniform float uNormalScale;
uniform int uFromAlpha;       // opacity: the map's alpha (else its red)
uniform int uAlphaMode;       // opacity: 1 cut out at uCutoff
uniform float uCutoff;

layout(location = 0) out vec4 oValue;

void main() {
  if ((vFlags & uSkipFlag) != 0u) discard;
  if (uMode == 6) {
    if (vAux.w < 0.5) discard;
    oValue = vec4(linearToSrgb(clamp(vAux.rgb, 0.0, 1.0)), 1.0);
    return;
  }
  if (abs(vAux.z - float(uSlot)) > 0.5) discard;
  vec2 uv = (uUVXf * vec3(vAux.xy, 1.0)).xy;
  vec4 t = uHasMap == 1 ? texture(uMap, uv) : vec4(1.0);
  if (uMode == 0 || uMode == 3) {
    vec3 c = linearToSrgb(clamp(srgbToLinear(t.rgb) * uFactor.rgb, 0.0, 1.0));
    oValue = vec4(c, 1.0);
  } else if (uMode == 1) {
    float v = t[uChannel];
    if (uInvert == 1) v = 1.0 - v;
    oValue = vec4(clamp(v * uFactor.x, 0.0, 1.0), 1.0, 0.0, 1.0);
  } else if (uMode == 2) {
    vec3 n = t.rgb * 2.0 - 1.0;
    if (uFlipGreen == 1) n.y = -n.y;
    n.xy *= uNormalScale;
    n = length(n) > 1e-5 ? normalize(n) : vec3(0, 0, 1);
    oValue = vec4(encodeNormal(n), 1.0);
  } else if (uMode == 4) {
    float a = (uHasMap == 1 ? (uFromAlpha == 1 ? t.a : t.r) : 1.0) * uFactor.x;
    if (uAlphaMode == 1) a = a >= uCutoff ? 1.0 : 0.0;
    oValue = vec4(clamp(a, 0.0, 1.0), 1.0, 0.0, 1.0);
  } else {
    oValue = vec4(t.r, 1.0, 0.0, 1.0);
  }
}
