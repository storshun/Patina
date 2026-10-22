# Third-party components

Patina includes or links these libraries. All of them allow free use in
commercial and non-commercial software; see each project's license text (in the
source tree under `third_party/` and on the project pages) for the exact terms.

| Component | Use | License |
|---|---|---|
| [SDL 3](https://libsdl.org) | windows, input, pen pressure | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) (docking; one change: `label###id` hashes as `id` alone, so translated labels keep their IDs) | user interface | MIT |
| [glad](https://github.com/Dav1dde/glad) | OpenGL / EGL loader (generated code) | MIT / public domain (generated) |
| [glm](https://github.com/g-truc/glm) | math | MIT (Happy Bunny) |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON | MIT |
| [stb](https://github.com/nothings/stb) image / image_write / image_resize2 | image files | MIT or public domain |
| [cgltf](https://github.com/jkuhlmann/cgltf) | glTF import | MIT |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) | OBJ import | MIT |
| [MikkTSpace](https://github.com/mmikk/MikkTSpace) | tangents | zlib-style |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) 0.18 | HTTP API server | MIT |
| [tinyexr](https://github.com/syoyo/tinyexr) 1.0.9 | EXR texture maps | BSD 3-clause |
| [miniz](https://github.com/richgel999/miniz) (with tinyexr) | EXR compression | MIT |
| [ufbx](https://github.com/ufbx/ufbx) 0.23.0 | FBX import (test files in `tests/data/fbx` are from its repository) | MIT or public domain (Unlicense) |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) b11540 (with ggml) | runs the Patina Assistant's model on the computer | MIT |
| [portable-file-dialogs](https://github.com/samhocevar/portable-file-dialogs) | native file dialogs | WTFPL |
| [QuadriFlow](https://github.com/hjwdzh/QuadriFlow) (810b7a0, with the changes in Blender's copy) | retopology: the quads along the model's shape | BSD 3-clause (`quadriflow/LICENSE.txt`); Blender's max-flow code MIT |
| [Eigen](https://eigen.tuxfamily.org) 3.4.0 (unchanged, MPL2 parts only) | linear algebra for QuadriFlow | MPL 2.0 (`eigen/COPYING.MPL2`) |
| [LEMON](https://lemon.cs.elte.hu) (1.3.1 and later fixes, [cgg-bern/lemon](https://github.com/cgg-bern/lemon) 3c6aa54) | network flow for QuadriFlow and libSatsuma | Boost Software License 1.0 (`lemon/LICENSE`) |
| [libSatsuma](https://github.com/cgg-bern/libSatsuma) (4e96979) | retopology: the counts of a quad layout's patches, all at once | MIT (`libsatsuma/LICENSE`) |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) (v0.25) | decimation and LODs that keep UV seams, borders and material edges | MIT (`meshoptimizer/LICENSE.md`) |
| [xatlas](https://github.com/jpcy/xatlas) (2022-07-26) | lightmap UVs: charts, unwrapping and packing | MIT (`xatlas/LICENSE`) |
| [libTimekeeper](https://github.com/cgg-bern/libTimekeeper) (f4f4866) | timing, for libSatsuma | MIT (`libtimekeeper/LICENSE`) |
| pcg32 (Wenzel Jakob's version of PCG) | random numbers for QuadriFlow | Apache 2.0 |
| parallel_stable_sort (Intel) | sorting for QuadriFlow | BSD 3-clause |
| [three.js](https://threejs.org) r170 (Patina Web only) | WebGL rendering in the browser | MIT |
| [DejaVu fonts](https://dejavu-fonts.github.io/) 2.37 (`fonts/`, for the text tool) | Sans, Sans Bold, Serif, Sans Mono | Bitstream Vera license, DejaVu changes public domain (`fonts/LICENSE-DejaVu.txt`) |
| Suzanne sample (`samples/Suzanne.glb`) | sample model, from Khronos glTF-Sample-Assets, © 2017 UX3D | CC0 1.0 |

The Windows build is compiled with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
(mingw-w64 runtime: ZPL / public domain; LLVM libc++ and libunwind: Apache 2.0 with
LLVM exceptions), statically linked.

All materials in the Patina default library (`materials/patina_*.json`) are
procedural definitions created for Patina; they contain no third-party images.

The Patina Assistant's model (a separate download, not in the packages) is a fine-tune of
[Qwen3 1.7B](https://huggingface.co/Qwen/Qwen3-1.7B) by the Qwen team, licensed under the
Apache License 2.0; the fine-tuned weights are distributed under the same license.
