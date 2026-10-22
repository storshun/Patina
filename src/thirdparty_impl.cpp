// Single translation unit holding the implementations of header-only libraries.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define TINYOBJLOADER_IMPLEMENTATION
#define CGLTF_IMPLEMENTATION

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_image_resize2.h>
#include <tiny_obj_loader.h>
#include <cgltf.h>
#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 1
#define TINYEXR_USE_THREAD 0
#define TINYEXR_USE_OPENMP 0
#include <tinyexr.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
