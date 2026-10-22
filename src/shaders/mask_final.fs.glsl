// A layer's effective mask (painted mask x smart mask), for exporting it.
#include "common.glsl"
#include "maskgen.glsl"
in vec2 vUV;
out vec4 oValue;
uniform sampler2D uLayerMask;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  float v = texture(uLayerMask, vUV).r * generatorMask(p);
  oValue = vec4(v, v, v, 1.0);
}
