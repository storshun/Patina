flat in uint vFlags;
out vec4 oColor;
uniform vec4 uColor;
uniform vec4 uSelColor;
uniform uint uSelFlag;
void main() {
  oColor = (vFlags & uSelFlag) != 0u ? uSelColor : uColor;
}
