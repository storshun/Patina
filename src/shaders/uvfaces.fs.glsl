// Fills faces in the UV editor (selection highlight / island colors / distortion).
in vec3 vPos;
in vec3 vNormal;
in vec4 vTangent;
in vec2 vUV;
in vec4 vAux;
flat in uint vFlags;
flat in uint vFace;
out vec4 oColor;
uniform int uMode;      // 0 selected faces only, 1 aux color (distortion heat), 2 uniform fill, 3 aux rgb where aux a
uniform vec4 uColor;
uniform uint uSelFlag;
void main() {
  if (uMode == 0) {
    if ((vFlags & uSelFlag) == 0u) discard;
    oColor = uColor;
  } else if (uMode == 1) {
    float d = clamp(vAux.x, -1.0, 1.0);
    vec3 c = d < 0.0 ? mix(vec3(0.2, 0.9, 0.3), vec3(0.2, 0.4, 1.0), -d) : mix(vec3(0.2, 0.9, 0.3), vec3(1.0, 0.25, 0.15), d);
    oColor = vec4(c, uColor.a);
  } else if (uMode == 3) {
    if (vAux.a < 0.5) discard;  // (1.2) a face set's color; none: left as it is
    oColor = vec4(vAux.rgb, uColor.a);
  } else {
    oColor = uColor;
  }
}
