// Retopology: the quads' edge loops, each in a color of its own (and the poles), pulled a little
// toward the eye so they sit on the model
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uViewProj;
uniform vec3 uCamPos;
uniform float uPull;
out vec4 vColor;
void main() {
  vColor = aColor;
  vec3 p = aPos + normalize(uCamPos - aPos) * uPull;
  gl_Position = uViewProj * vec4(p, 1.0);
}
