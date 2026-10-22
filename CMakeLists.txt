cmake_minimum_required(VERSION 3.24)
project(Patina VERSION 1.2.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(PATINA_BUILD_GUI   "Build the SDL3/ImGui desktop application" ON)
option(PATINA_BUILD_TESTS "Build tests" ON)
option(PATINA_FETCH_SDL3  "Download SDL3 if no system SDL3 is found" ON)
option(PATINA_STATIC_RUNTIME "Link libstdc++/libgcc statically (portable binaries)" OFF)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------- third party
add_library(patina_thirdparty STATIC
  ${TP}/glad/src/gl.c
  ${TP}/mikktspace/mikktspace.c
  ${TP}/tinyexr/miniz.c
  ${TP}/ufbx/ufbx.c
  src/thirdparty_impl.cpp)
if(NOT WIN32)
  target_sources(patina_thirdparty PRIVATE ${TP}/glad/src/egl.c)  # headless contexts on Linux
endif()
target_include_directories(patina_thirdparty SYSTEM PUBLIC
  ${TP}/glad/include ${TP}/glm ${TP}/json ${TP}/stb ${TP}/tinyobjloader
  ${TP}/cgltf ${TP}/mikktspace ${TP}/pfd ${TP}/tinyexr ${TP}/httplib ${TP}/ufbx)
target_compile_definitions(patina_thirdparty PUBLIC GLM_ENABLE_EXPERIMENTAL GLM_FORCE_XYZW_ONLY)
set_target_properties(patina_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(patina_thirdparty PUBLIC ${CMAKE_DL_LIBS})

# QuadriFlow (retopology's quads), with Eigen (MPL2 parts only) and LEMON; see third_party/quadriflow/README.patina
set(QF ${TP}/quadriflow)
file(GLOB PATINA_QF_SRC ${QF}/src/*.cpp)
set(LEMON ${TP}/lemon)
add_library(patina_quadriflow STATIC ${PATINA_QF_SRC}
  ${LEMON}/lemon/arg_parser.cc ${LEMON}/lemon/base.cc ${LEMON}/lemon/color.cc
  ${LEMON}/lemon/lp_base.cc ${LEMON}/lemon/lp_skeleton.cc ${LEMON}/lemon/random.cc
  ${LEMON}/lemon/bits/windows.cc)
target_include_directories(patina_quadriflow SYSTEM PUBLIC ${QF}/src ${TP}/eigen ${LEMON}
  PRIVATE ${QF}/3rd/pcg32 ${QF}/3rd/pss)
target_compile_definitions(patina_quadriflow PUBLIC EIGEN_MPL2_ONLY)
if(WIN32)
  # (LEMON tests WIN32, which MinGW's compilers don't define)
  target_compile_definitions(patina_quadriflow PUBLIC _USE_MATH_DEFINES PRIVATE WIN32)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(patina_quadriflow PRIVATE -w)
endif()
set_target_properties(patina_quadriflow PROPERTIES POSITION_INDEPENDENT_CODE ON)

# libSatsuma (retopology's quad layouts: the counts of all patches at once), with libTimekeeper; on LEMON
# above. See third_party/libsatsuma/README.patina
file(GLOB_RECURSE PATINA_SATSUMA_SRC ${TP}/libsatsuma/src/libsatsuma/*.cc)
add_library(patina_satsuma STATIC ${PATINA_SATSUMA_SRC})
target_include_directories(patina_satsuma SYSTEM PUBLIC ${TP}/libsatsuma/src ${TP}/libtimekeeper/src ${LEMON} ${TP}/json)
target_link_libraries(patina_satsuma PUBLIC patina_quadriflow)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(patina_satsuma PRIVATE -w)
endif()
set_target_properties(patina_satsuma PROPERTIES POSITION_INDEPENDENT_CODE ON)

# meshoptimizer (decimation and LODs keeping UV seams and borders, 0.29); see third_party/meshoptimizer/VERSION.txt
file(GLOB PATINA_MESHOPT_SRC ${TP}/meshoptimizer/*.cpp)
add_library(patina_meshopt STATIC ${PATINA_MESHOPT_SRC})
target_include_directories(patina_meshopt SYSTEM PUBLIC ${TP}/meshoptimizer)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(patina_meshopt PRIVATE -w)
endif()
set_target_properties(patina_meshopt PROPERTIES POSITION_INDEPENDENT_CODE ON)
# xatlas (lightmap UVs: charts, parameterization and packing, 0.35); see third_party/xatlas/VERSION.txt
add_library(patina_xatlas STATIC ${TP}/xatlas/xatlas.cpp)
target_include_directories(patina_xatlas SYSTEM PUBLIC ${TP}/xatlas)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(patina_xatlas PRIVATE -w)
endif()
set_target_properties(patina_xatlas PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------- shaders
file(GLOB PATINA_SHADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.glsl)
set(PATINA_SHADER_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders_embedded.cpp)
add_custom_command(
  OUTPUT ${PATINA_SHADER_CPP}
  COMMAND ${CMAKE_COMMAND} -DOUT=${PATINA_SHADER_CPP}
          -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/src/shaders
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
  DEPENDS ${PATINA_SHADERS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
  COMMENT "Embedding GLSL shaders")

# ---------------------------------------------------------------- toolbar icons (icons/*.svg)
file(GLOB PATINA_ICONS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/icons/*.svg)
set(PATINA_ICON_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/icons_embedded.cpp)
add_custom_command(
  OUTPUT ${PATINA_ICON_CPP}
  COMMAND ${CMAKE_COMMAND} -DOUT=${PATINA_ICON_CPP}
          -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/icons
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedIcons.cmake
  DEPENDS ${PATINA_ICONS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedIcons.cmake
  COMMENT "Embedding toolbar icons")

# ---------------------------------------------------------------- Patina Assistant (llama.cpp)
# The Assistant's model runs with the vendored llama.cpp (third_party/llama.cpp, see PATINA.md).
# CPU backend by default (portable: no -march=native); CUDA or Vulkan for local GPU builds.
option(PATINA_ASSISTANT "Build the Patina Assistant (llama.cpp)" ON)
option(PATINA_ASSISTANT_CUDA "Assistant: llama.cpp CUDA backend (local builds)" OFF)
option(PATINA_ASSISTANT_VULKAN "Assistant: llama.cpp Vulkan backend" OFF)
if(PATINA_ASSISTANT)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
  set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
  set(LLAMA_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
  set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
  set(GGML_CUDA ${PATINA_ASSISTANT_CUDA} CACHE BOOL "" FORCE)
  set(GGML_VULKAN ${PATINA_ASSISTANT_VULKAN} CACHE BOOL "" FORCE)
  if(PATINA_ASSISTANT_CUDA AND NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES "86;89;120")  # Ampere, Ada, Blackwell (RTX 5090 = 12.0)
  endif()
  add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------- core library
file(GLOB_RECURSE PATINA_CORE_SRC CONFIGURE_DEPENDS
  src/core/*.cpp src/gfx/*.cpp src/engine/*.cpp src/platform/*.cpp src/mcp/*.cpp)
if(WIN32)
  list(REMOVE_ITEM PATINA_CORE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/HeadlessContext.cpp)
else()
  list(REMOVE_ITEM PATINA_CORE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/HeadlessContextWin.cpp)
endif()
add_library(patina_core STATIC ${PATINA_CORE_SRC} ${PATINA_SHADER_CPP} ${PATINA_ICON_CPP})
target_include_directories(patina_core PUBLIC src)
target_link_libraries(patina_core PUBLIC patina_thirdparty patina_quadriflow patina_satsuma patina_meshopt patina_xatlas)
find_package(Threads REQUIRED)
target_link_libraries(patina_core PUBLIC Threads::Threads)
if(PATINA_ASSISTANT)
  target_link_libraries(patina_core PUBLIC llama)
  target_compile_definitions(patina_core PUBLIC PATINA_HAS_ASSISTANT=1)
endif()
target_compile_definitions(patina_core PUBLIC PATINA_VERSION="${PROJECT_VERSION}")
if(NOT WIN32)
  target_compile_definitions(patina_core PUBLIC PATINA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
else()
  # UTF-8 everywhere, no min/max macros, Windows 10 APIs
  target_compile_definitions(patina_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
  target_link_libraries(patina_core PUBLIC ws2_32 opengl32 gdi32 user32 shell32 ole32 comdlg32 uuid)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(patina_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# ---------------------------------------------------------------- GUI application
if(PATINA_BUILD_GUI)
  find_package(SDL3 CONFIG QUIET)
  if(NOT SDL3_FOUND)
    if(PATINA_FETCH_SDL3)
      message(STATUS "System SDL3 not found - fetching SDL 3.2 (install SDL3-devel to skip this)")
      include(FetchContent)
      set(SDL_SHARED OFF CACHE BOOL "" FORCE)
      set(SDL_STATIC ON CACHE BOOL "" FORCE)
      set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
      set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
      FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.24
        GIT_SHALLOW TRUE)
      FetchContent_MakeAvailable(SDL3)
    else()
      message(FATAL_ERROR "SDL3 not found. sudo dnf install SDL3-devel")
    endif()
  endif()

  add_library(patina_imgui STATIC
    ${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_draw.cpp ${TP}/imgui/imgui_tables.cpp
    ${TP}/imgui/imgui_widgets.cpp ${TP}/imgui/imgui_demo.cpp ${TP}/imgui/imgui_stdlib.cpp
    ${TP}/imgui/backends/imgui_impl_sdl3.cpp ${TP}/imgui/backends/imgui_impl_opengl3.cpp)
  target_include_directories(patina_imgui SYSTEM PUBLIC ${TP}/imgui ${TP}/imgui/backends)
  target_link_libraries(patina_imgui PUBLIC SDL3::SDL3)
  target_compile_definitions(patina_imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
  # (the test engine hooks, for the widget ID check in src/ui/UiIdCheck.cpp; idle unless PATINA_UI_ID_CHECK is set)
  target_compile_definitions(patina_imgui PRIVATE IMGUI_ENABLE_TEST_ENGINE)

  file(GLOB_RECURSE PATINA_UI_SRC CONFIGURE_DEPENDS src/ui/*.cpp)
  # the window icon, embedded (packaging/icons/patina_128.png)
  set(_icon ${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/patina_128.png)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_icon})
  file(READ ${_icon} _icon_hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _icon_bytes "${_icon_hex}")
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/window_icon.cpp
       "#include <cstddef>\n#include <cstdint>\nextern const uint8_t kPatinaWindowIcon[] = {${_icon_bytes}};\nextern const size_t kPatinaWindowIconSize = sizeof(kPatinaWindowIcon);\n")
  add_library(patina_app OBJECT src/main.cpp ${PATINA_UI_SRC} ${CMAKE_CURRENT_BINARY_DIR}/generated/window_icon.cpp)
  target_link_libraries(patina_app PUBLIC patina_core patina_imgui)
  target_compile_definitions(patina_app PRIVATE PATINA_HAS_GUI=1)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(patina_app PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  endif()
  set(PATINA_EXES patina)
  if(WIN32)
    # patina.exe: window app (no console). patina-cli.exe: same program as a console app for
    # --mcp / --headless (MCP clients talk to it over stdin/stdout).
    # icon, manifest and version (from project())
    set(PATINA_WIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows)
    configure_file(packaging/windows/patina.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/patina.rc @ONLY)
    set(_rc ${CMAKE_CURRENT_BINARY_DIR}/generated/patina.rc)
    add_executable(patina WIN32 $<TARGET_OBJECTS:patina_app> ${_rc})
    add_executable(patina-cli $<TARGET_OBJECTS:patina_app> ${_rc})
    list(APPEND PATINA_EXES patina-cli)
  else()
    add_executable(patina $<TARGET_OBJECTS:patina_app>)
  endif()
  foreach(t ${PATINA_EXES})
    target_link_libraries(${t} PRIVATE patina_core patina_imgui)
  endforeach()
else()
  add_executable(patina src/main.cpp)
  target_link_libraries(patina PRIVATE patina_core)
  set(PATINA_EXES patina)
  if(WIN32)
    set_target_properties(patina PROPERTIES OUTPUT_NAME patina-cli)
  endif()
endif()

if(PATINA_STATIC_RUNTIME AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT WIN32)
  foreach(t ${PATINA_EXES})
    target_link_options(${t} PRIVATE -static-libstdc++ -static-libgcc)
  endforeach()
endif()
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # self-contained exes: only system DLLs (and the Windows 10 UCRT) are needed
  foreach(t ${PATINA_EXES})
    target_link_options(${t} PRIVATE -static)
  endforeach()
endif()

# ---------------------------------------------------------------- install
include(GNUInstallDirs)
if(WIN32)
  # portable folder: exes, materials and the add-on side by side (see Paths.cpp dataDir)
  install(TARGETS ${PATINA_EXES} RUNTIME DESTINATION .)
  install(DIRECTORY materials DESTINATION .)
  install(DIRECTORY smart_materials DESTINATION .)
  install(DIRECTORY recipes DESTINATION .)
  install(DIRECTORY fonts DESTINATION .)
  install(DIRECTORY languages DESTINATION .)
  install(DIRECTORY blender/patina_bridge DESTINATION blender)
  install(FILES README.md CHANGELOG.md docs/BRIDGE.md docs/COMMANDS.md docs/WISHLIST.md DESTINATION docs)
  install(DIRECTORY docs/manual DESTINATION docs PATTERN src EXCLUDE)
  install(DIRECTORY web/dist/ DESTINATION web OPTIONAL)   # Patina Web (scripts/build_web.py)
  install(DIRECTORY samples DESTINATION .)
  install(FILES clients/python/patina_api.py DESTINATION clients/python)
  install(DIRECTORY examples/api DESTINATION examples)
else()
install(TARGETS patina RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY materials DESTINATION ${CMAKE_INSTALL_DATADIR}/patina)
install(DIRECTORY smart_materials DESTINATION ${CMAKE_INSTALL_DATADIR}/patina)
install(DIRECTORY recipes DESTINATION ${CMAKE_INSTALL_DATADIR}/patina)
install(DIRECTORY fonts DESTINATION ${CMAKE_INSTALL_DATADIR}/patina)
install(DIRECTORY languages DESTINATION ${CMAKE_INSTALL_DATADIR}/patina)
install(DIRECTORY blender/patina_bridge DESTINATION ${CMAKE_INSTALL_DATADIR}/patina/blender)
install(FILES packaging/patina.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
foreach(_sz 16 24 32 48 64 128 256 512)
  install(FILES packaging/icons/patina_${_sz}.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_sz}x${_sz}/apps
          RENAME patina.png)
endforeach()
install(FILES README.md CHANGELOG.md docs/BRIDGE.md docs/COMMANDS.md docs/WISHLIST.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/patina)
install(DIRECTORY docs/manual DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/patina PATTERN src EXCLUDE)
install(DIRECTORY web/dist/ DESTINATION ${CMAKE_INSTALL_DATADIR}/patina/web OPTIONAL)   # Patina Web (scripts/build_web.py)
install(DIRECTORY samples DESTINATION ${CMAKE_INSTALL_DATADIR}/patina)
install(FILES clients/python/patina_api.py DESTINATION ${CMAKE_INSTALL_DATADIR}/patina/clients/python)
install(DIRECTORY examples/api DESTINATION ${CMAKE_INSTALL_DATADIR}/patina/examples)
target_compile_definitions(patina_core PUBLIC
  PATINA_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/patina")
endif()

# ---------------------------------------------------------------- tests
if(PATINA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
