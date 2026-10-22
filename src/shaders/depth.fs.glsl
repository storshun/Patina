// Depth-only pass (color writes disabled by the caller)
flat in uint vFlags;
out vec4 oColor;
void main() {
  if ((vFlags & 32u) != 0u) discard;  // (1.2) a hidden face set's faces hide nothing
  oColor = vec4(1.0);
}
