in vec4 vColor;
out vec4 oColor;
uniform float uAlpha;
void main() { oColor = vec4(vColor.rgb, vColor.a * uAlpha); }
