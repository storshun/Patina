# Patina — build plan

Patina is a native C++20 / OpenGL 4.5 texture painter for Linux (Fedora first),
modeled on the Substance Painter workflow: paint *materials* (not just colors)
directly onto a 3D model, independent of how its UVs are laid out, with a layer
stack, a material library, a brush library, a UV editor, a live Blender bridge and
an MCP server so Claude can drive it headlessly or watch the live window.

## Guiding decisions

| Area | Decision | Why |
|---|---|---|
| Language / build | C++20, CMake ≥ 3.24, Ninja | Requested C++; CMake is native on Fedora |
| Windowing / input | SDL3 (system `SDL3-devel`, FetchContent fallback) | Native Wayland + X11, and **tablet pen pressure/tilt** (GLFW has none) |
| GPU | OpenGL 4.5 core via glad (vendored) | Runs on Mesa (AMD/Intel), NVIDIA, and llvmpipe for CI/headless |
| Headless | EGL surfaceless / device platform via glad's EGL loader (dlopen) | Same renderer with no window, for MCP automation and tests |
| UI | Dear ImGui (docking branch, vendored) | Fast to build complex dockable tool UIs |
| Math / IO | glm, nlohmann::json, stb_image(_write), tinyobjloader, cgltf, MikkTSpace | Small, vendored, permissive licenses |
| Dialogs | portable-file-dialogs (zenity / kdialog / xdg-portal) | No GTK/Qt dependency |

## How UV-independent painting works

Every brush dab is evaluated **per texel, in 3D**, not in UV space:

1. The mesh is rasterized *into the texture* (vertex position = UV), carrying the
   world position and normal of every texel.
2. Each texel projects its world position through the current camera, compares
   its depth with the viewport depth buffer (visibility) and with the brush
   footprint in screen space (or a 3D sphere for the "surface" brush).
3. Dabs accumulate into a per-stroke mask with hardware "over" blending
   (flow), and the stroke mask × opacity blends the material into the layer.
4. Because both sides of a UV seam correspond to the same 3D point, strokes cross
   seams, overlapping/mirrored islands, and arbitrary island scale without
   distortion. A dilation (gutter) pass removes seam bleeding.
5. Materials are sampled tri-planar in object space (or from the view, or in
   UV), so a painted material looks continuous regardless of UV layout.

When UVs are edited after painting, a **reprojection pass** rasterizes the mesh
at its new UVs while sampling the old texture at the old UVs, so paint survives
unwraps, island moves and repacks.

## Architecture

```
src/
  core/      Mesh (polygon/loop model), MeshIO (OBJ, glTF, .ptmesh bridge format),
             BVH picking, UV ops (islands, LSCM unwrap, projections, packing),
             Image IO (8/16-bit PNG), logging
  gfx/       GL wrappers (Shader, Texture, Framebuffer, GpuMesh), embedded GLSL
  engine/    Project + layer stack, channels, material library (procedural +
             image-based), brush library (procedural alphas), painter (stroke
             engine), compositor (layer blend, dilation, height→normal),
             mesh-map baker (AO, curvature, position, normal), undo, renderer
             (PBR viewport + UV view), command registry (automation API)
  platform/  SDL3 window context, EGL headless context, TCP JSON-lines server
  ui/        ImGui application: 3D viewport, UV/texture editor, layers,
             materials, brushes, properties, bake, export, console
  mcp/       MCP stdio server (proxy to a running GUI, or in-process headless)
blender/     Blender add-on "Patina Bridge" (Python — Blender's only API)
materials/   Built-in procedural material definitions (JSON)
tests/       Unit tests + headless EGL integration tests (ctest)
```

One **command registry** (name → JSON handler) is the single automation API.
It is reached by the TCP server (Blender add-on, MCP proxy), by the MCP server
in-process (headless) and by the UI console. Commands run on the GL thread via
a queue drained each frame.

## Blender bridge

* Add-on (N-panel "Patina"): *Send to Patina*, *Pull from Patina*, live-link
  toggle, path to the Patina binary (auto-launches it).
* Mesh goes over as `.ptmesh` (binary: positions, polygons, per-loop UVs/normals,
  edge seams) so loop order is preserved exactly.
* Patina returns textures (PNG, 16-bit for normal/height) and, if UVs were
  edited, a `.ptuv` file with per-loop UVs + seams, written back by loop index.
* Transport: JSON lines over TCP on localhost (Patina :7860, Blender :7861) plus a
  shared exchange folder `~/.cache/patina/bridge/`.
* The add-on builds/updates a Principled BSDF material (normal map, optional
  displacement) and reloads images in place on every push.

## MCP

`patina --mcp` speaks MCP over stdio. If a Patina window is running it proxies
to it (so you can watch Claude work and get screenshots of the real window);
otherwise it boots an in-process headless engine. Tools cover: open mesh /
project, materials, brushes, layers, camera, paint strokes (screen or UV
space), fills, bakes, UV unwrap/pack/transform, renders/screenshots, export,
undo, send to Blender.

## Phases

1. **Foundation** — CMake, vendored deps, SDL3 + EGL contexts, GL wrappers,
   shader embedding, logging. *(done when an empty docked UI and a headless
   offscreen render both work)*
2. **Mesh** — loop-based mesh, OBJ/glTF/.ptmesh import, triangulation, normals,
   MikkTSpace tangents, BVH picking, UV islands.
3. **Render** — orbit camera, PBR viewport with procedural studio environment
   (prefiltered specular + SH irradiance), channel/debug views, wireframe.
4. **Texture set + layers** — channels (base color, roughness, metallic,
   normal, height, emissive), paint/fill layers, masks, blend modes,
   compositor with dilation and height→normal.
5. **Painting** — stroke engine (spacing, jitter, scatter, angle, follow path,
   lazy mouse, line tool), screen-projection and 3D-surface brushes, UV-space
   painting in the UV editor, erase, mask painting, symmetry, pen pressure,
   polygon/island/mesh fill, color picker, undo/redo.
6. **Libraries** — procedural material library baked to tiling maps with
   sphere thumbnails, image-material import (ambientCG/Poly Haven naming),
   procedural brush alphas and presets, user libraries on disk.
7. **UV editor** — 2D view with texture underlay, vertex/edge/face/island
   selection, move/rotate/scale, seams marked in 3D or 2D, LSCM unwrap, smart
   and box projection, packing, texel-density normalization, paint reprojection.
8. **Mesh-map baking** — AO, curvature, world normal/position, used by smart
   masks (edge wear, cavity dirt) on fill layers.
9. **Export / project** — export presets (Blender/glTF, Unreal, Unity), project
   save/load.
10. **Automation** — command registry, TCP server, MCP server, Blender add-on.
11. **Polish** — docs, Fedora build script, desktop entry, tests.

## Known gaps / later

* Smudge and clone tools, layer folders, anchor points, UDIM tiles, displacement
  tessellation in the viewport, GPU path tracing.
