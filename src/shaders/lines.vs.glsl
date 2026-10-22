// Edge lines in 3D (uSpace 0) or UV space (uSpace 1)
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
layout(location = 5) in uint aFlags;
layout(location = 7) in vec2 aMeshUV;
uniform mat4 uViewProj;
uniform int uSpace;
uniform float uDepthOffset;
uniform vec3 uCamPos;
uniform float uPull = 0.0;  // (3D) moved this far toward the camera (in model units), not by a depth offset
// (0.40.1) or this many pixels toward the camera, measured where each point is: a share of the depth range grows with
// the distance (to a third of the model and more), so lines on the far side showed through the near one
uniform float uPullPx = 0.0;
uniform float uPixel = 0.0;  // the size of a pixel: per unit of distance (perspective), or in model units (orthographic)
uniform int uOrtho = 0;
uniform vec3 uToEye = vec3(0.0, 0.0, 1.0);  // orthographic: toward the camera
uniform int uTile = -1;  // UV space: only the edges of this texture tile
flat out uint vFlags;
void main() {
  vFlags = aFlags;
  if (uSpace == 1) {
    gl_Position = uViewProj * vec4(aMeshUV, 0.0, 1.0);
    if (uTile >= 0 && int((aFlags >> 16) & 255u) != uTile) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  } else {
    vec3 p = aPos;
    if (uPull > 0.0) p += normalize(uCamPos - p) * uPull;
    if (uPullPx > 0.0) {
      if (uOrtho == 1) p += uToEye * (uPullPx * uPixel);
      else p += (uCamPos - p) * (uPullPx * uPixel);  // (|eye - p| * pixel per unit, along the unit direction)
    }
    gl_Position = uViewProj * vec4(p, 1.0);
    gl_Position.z -= uDepthOffset * gl_Position.w;
    if ((aFlags & 32u) != 0u) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // (1.2) a hidden face set's edges
  }
}
