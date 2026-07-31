cmake_minimum_required(VERSION 3.20)
project(sm64ex LANGUAGES C CXX)
set(SM64_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(SM64_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(SM64_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(SM64_TOOL_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/tools")
set(SM64_VERSION "us" CACHE STRING "ROM version: us / jp / eu / sh")
set_property(CACHE SM64_VERSION PROPERTY STRINGS us jp eu sh)
set(SM64_GRUCODE "f3dex2e" CACHE STRING "Microcode: f3dex2e / f3dex2 / f3dex / f3d_new / f3dzex")
set_property(CACHE SM64_GRUCODE PROPERTY STRINGS f3dex2e f3dex2 f3dex f3d_new f3dzex)
option(SM64_BUILD_ROM_TOOLS "Also build host tools only needed for ROM builds" OFF)
option(SM64_NIGHTLY "Embed the git hash in the title screen (upstream nightly behaviour)" ON)
set(SM64_OPT_FLAGS "-O2" CACHE STRING "Optimisation flags (upstream uses -O2)")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(NOT (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang"))
  message(FATAL_ERROR
    "sm64ex requires GCC or Clang (MinGW-w64 on Windows); "
    "'${CMAKE_C_COMPILER_ID}' cannot compile the upstream sources.")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(SM64_PYTHON "${Python3_EXECUTABLE}")
get_filename_component(_sm64_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
find_program(SM64_AS      NAMES ${CMAKE_C_COMPILER_TARGET}-as      as      HINTS "${_sm64_compiler_dir}")
find_program(SM64_OBJCOPY NAMES ${CMAKE_C_COMPILER_TARGET}-objcopy objcopy HINTS "${_sm64_compiler_dir}")
if(NOT SM64_AS OR NOT SM64_OBJCOPY)
  message(FATAL_ERROR "Could not find 'as' and 'objcopy' (needed for .s music sequences)")
endif()
if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
  set(SM64_ENDIAN big)
else()
  set(SM64_ENDIAN little)
endif()
math(EXPR SM64_BITWIDTH 64)
if(SM64_VERSION STREQUAL "jp")
  set(SM64_VERSION_DEF VERSION_JP)
elseif(SM64_VERSION STREQUAL "us")
  set(SM64_VERSION_DEF VERSION_US)
elseif(SM64_VERSION STREQUAL "eu")
  set(SM64_VERSION_DEF VERSION_EU)
elseif(SM64_VERSION STREQUAL "sh")
  set(SM64_VERSION_DEF VERSION_SH)
else()
  message(FATAL_ERROR "Unknown SM64_VERSION '${SM64_VERSION}'")
endif()
if(SM64_GRUCODE STREQUAL "f3dex")
  set(SM64_GRUCODE_DEF F3DEX_GBI)
elseif(SM64_GRUCODE STREQUAL "f3dex2" OR SM64_GRUCODE STREQUAL "f3dzex")
  set(SM64_GRUCODE_DEF F3DEX_GBI_2)
elseif(SM64_GRUCODE STREQUAL "f3dex2e")
  set(SM64_GRUCODE_DEF F3DEX_GBI_2E)
elseif(SM64_GRUCODE STREQUAL "f3d_new")
  set(SM64_GRUCODE_DEF F3D_NEW)
else()
  message(FATAL_ERROR "Unknown SM64_GRUCODE '${SM64_GRUCODE}'")
endif()
set(SM64_VERSION_DEFS ${SM64_VERSION_DEF} _LANGUAGE_C NON_MATCHING AVOID_UB)
set(SM64_VERSION_CFLAGS "")
foreach(_d IN LISTS SM64_VERSION_DEFS)
  list(APPEND SM64_VERSION_CFLAGS "-D${_d}")
endforeach()
set(SM64_GIT_HASH "")
if(SM64_NIGHTLY)
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
      WORKING_DIRECTORY "${SM64_ROOT}"
      OUTPUT_VARIABLE SM64_GIT_HASH
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  endif()
endif()
add_subdirectory(tools)
file(MAKE_DIRECTORY "${SM64_GEN_DIR}" "${SM64_GEN_DIR}/include")
include("${SM64_CMAKE_DIR}/SM64Assets.cmake")
add_custom_target(sm64_assets ALL
  DEPENDS ${SM64_ASSET_OUTPUTS} ${SM64_GENERATED_C_FILES})
set(SM64_SRC_DIRS
  src src/engine src/game src/audio src/menu src/buffers
  actors levels bin data assets
  src/pc src/pc/gfx src/pc/audio src/pc/controller src/pc/fs src/pc/fs/packtypes)
set(SM64_SOURCES "")
foreach(_dir IN LISTS SM64_SRC_DIRS)
  file(GLOB _c "${SM64_ROOT}/${_dir}/*.c")
  list(APPEND SM64_SOURCES ${_c})
endforeach()
file(GLOB _level_c
  "${SM64_ROOT}/levels/*/leveldata.c"
  "${SM64_ROOT}/levels/*/script.c"
  "${SM64_ROOT}/levels/*/geo.c")
list(APPEND SM64_SOURCES ${_level_c})
foreach(_dir IN ITEMS src/goddard src/goddard/dynlists)
  file(GLOB _c "${SM64_ROOT}/${_dir}/*.c")
  list(APPEND SM64_SOURCES ${_c})
endforeach()
foreach(_u IN ITEMS alBnkfNew guLookAtRef guMtxF2L guNormalize guOrthoF
                   guPerspectiveF guRotateF guScaleF guTranslateF ldiv)
  list(APPEND SM64_SOURCES "${SM64_ROOT}/lib/src/${_u}.c")
endforeach()
list(APPEND SM64_SOURCES "${SM64_ROOT}/sound/sound_data.c")
list(REMOVE_ITEM SM64_SOURCES "${SM64_ROOT}/src/game/main.c")
list(APPEND SM64_SOURCES ${SM64_GENERATED_C_FILES})
separate_arguments(_sm64_opt_flags NATIVE_COMMAND "${SM64_OPT_FLAGS}")
set(_sm64_all_backend_sources
  src/pc/gfx/gfx_sdl1.c
  src/pc/gfx/gfx_sdl2.c
  src/pc/gfx/gfx_opengl.c
  src/pc/gfx/gfx_opengl_legacy.c
  src/pc/audio/audio_sdl1.c
  src/pc/audio/audio_sdl2.c
  src/pc/controller/controller_sdl1.c
  src/pc/controller/controller_sdl2.c
  src/pc/controller/controller_emscripten_keyboard.c)
set(_sm64_host_sources ${SM64_SOURCES})
foreach(_bs IN LISTS _sm64_all_backend_sources)
  list(REMOVE_ITEM _sm64_host_sources "${SM64_ROOT}/${_bs}")
endforeach()
list(REMOVE_ITEM _sm64_host_sources "${SM64_ROOT}/src/pc/pc_main.c")
list(APPEND _sm64_host_sources "${SM64_ROOT}/src/pc/host/sm64ex_host.c")
# SDL3 gamepad backend (mirrors controller_sdl2.c); guarded by CAPI_SDL3 so it
# stays inert in the regular SDL2/PC builds that compile this same source list.
list(APPEND _sm64_host_sources "${SM64_ROOT}/src/pc/controller/controller_sdl3.c")
add_library(sm64ex STATIC ${_sm64_host_sources})
add_dependencies(sm64ex sm64_assets)
target_include_directories(sm64ex PRIVATE
  "${SM64_ROOT}/include"
  "${SM64_GEN_DIR}"
  "${SM64_GEN_DIR}/include"
  "${SM64_ROOT}/src"
  "${SM64_ROOT}")
target_include_directories(sm64ex PUBLIC "${SM64_ROOT}/src/pc/host")
target_compile_definitions(sm64ex PRIVATE
  ${SM64_VERSION_DEFS}
  ${SM64_GRUCODE_DEF}
  AAPI_NULL=1
  # Match Makefile defaults / common PC-port toggles:
  #   EXT_OPTIONS_MENU  — Options / Controls / Display / Sound (pause + R)
  #   NODRAWINGDISTANCE — keep distant objects/surfaces loaded
  #   TEXTSAVES         — human-readable save files alongside binary
  EXT_OPTIONS_MENU=1
  NODRAWINGDISTANCE=1
  TEXTSAVES=1)
if(SM64_GIT_HASH)
  target_compile_definitions(sm64ex PRIVATE NIGHTLY GIT_HASH="${SM64_GIT_HASH}")
endif()
target_compile_options(sm64ex PRIVATE
  ${_sm64_opt_flags}
  -fno-strict-aliasing -fwrapv -fsigned-char
  -Wno-format-security)
set_target_properties(sm64ex PROPERTIES
  C_STANDARD 11
  C_EXTENSIONS ON)
if(WIN32)
  target_link_libraries(sm64ex PRIVATE ws2_32 winmm)
else()
  target_link_libraries(sm64ex PRIVATE m)
  find_package(Threads REQUIRED)
  target_link_libraries(sm64ex PRIVATE Threads::Threads)
endif()
# SM64_BUILD_EXE / sm64pc (classic SDL2+GLEW standalone) are removed; the
# Foundation-renderer host is the only executable target.
if(DEFINED SM64_BUILD_EXE)
  message(DEPRECATION
    "SM64_BUILD_EXE / sm64pc is deprecated and ignored; "
    "only the Foundation host (sm64_foundation) is built.")
  unset(SM64_BUILD_EXE CACHE)
endif()
if(DEFINED SM64_FOUNDATION_PORT)
  message(DEPRECATION
    "SM64_FOUNDATION_PORT is always enabled and no longer an option; "
    "unset -DSM64_FOUNDATION_PORT=... from your configure command.")
  unset(SM64_FOUNDATION_PORT CACHE)
endif()
message(STATUS "SM64: version=${SM64_VERSION} grucode=${SM64_GRUCODE} "
               "endian=${SM64_ENDIAN} bits=${SM64_BITWIDTH}")

set(FOUNDATION_WITH_EXAMPLES ON CACHE BOOL "" FORCE)
set(FOUNDATION_WITH_JOLT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FOUNDATION_OS_ALLOCATOR ON CACHE BOOL "" FORCE)
include(FetchContent)
if(FOUNDATION_SOURCE_DIR)
  FetchContent_Declare(Foundation SOURCE_DIR "${FOUNDATION_SOURCE_DIR}")
  message(STATUS "SM64: using local Foundation at ${FOUNDATION_SOURCE_DIR}")
else()
  FetchContent_Declare(Foundation
    GIT_REPOSITORY https://github.com/mos9527/Foundation
    GIT_TAG dev
    GIT_SHALLOW TRUE)
endif()
FetchContent_MakeAvailable(Foundation)
# Foundation defines FOUNDATION_ROOT_DIR only inside its own subdirectory
# scope (CMakeLists.txt: set(FOUNDATION_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})),
# so it is NOT visible in this top-level scope. Derive it from the source dir
# that is actually in scope here (the user-provided FOUNDATION_SOURCE_DIR, or
# the FetchContent-populated Foundation_SOURCE_DIR in the git-tag branch) and
# re-expose it as a cache var for the rest of this file.
if(FOUNDATION_SOURCE_DIR)
  set(_foundation_src "${FOUNDATION_SOURCE_DIR}")
else()
  set(_foundation_src "${Foundation_SOURCE_DIR}")
endif()
set(FOUNDATION_ROOT_DIR "${_foundation_src}" CACHE PATH "Foundation source root" FORCE)
# Slang shader toolchain (same as Foundation/Examples): compiles .slang -> .spv
# into the runtime Data/Shaders dir, where the host resolves shaders at load.
# The module lives under Source/ in current Foundation trees but used to sit at
# the repo root, so accept either layout instead of hard-failing at configure.
if(EXISTS "${FOUNDATION_ROOT_DIR}/Source/slangc_compile.cmake")
  set(FOUNDATION_INCLUDE_DIR "${FOUNDATION_ROOT_DIR}/Source")
else()
  set(FOUNDATION_INCLUDE_DIR "${FOUNDATION_ROOT_DIR}")
endif()
include("${FOUNDATION_INCLUDE_DIR}/slangc_compile.cmake")
set(SHADER_BUILD_DIR "${CMAKE_BINARY_DIR}/bin/Data/Shaders")
file(MAKE_DIRECTORY "${SHADER_BUILD_DIR}")
# Screen-space overlay shader used by the skybox and UI passes.
slangc_compile(Shader_SM64Overlay
  SOURCE "${SM64_ROOT}/src/pc/foundation/shaders/SM64Overlay.slang"
  OUTPUT_DIR "${SHADER_BUILD_DIR}"
  OUTPUT_NAME "SM64Overlay"
  INCLUDE_DIRS "${FOUNDATION_INCLUDE_DIR}/Renderer/Shaders")
# sm64ex compiles the SDL3 gamepad backend; give it the SDL3 headers/libs and
# the CAPI_SDL3 switch so controller_sdl3.c (and its registration in
# controller_entry_point.c) are active on the Foundation host.
target_compile_definitions(sm64ex PRIVATE CAPI_SDL3=1 AAPI_SDL3=1)
target_link_libraries(sm64ex PRIVATE SDL3::SDL3)
add_executable(sm64_foundation src/pc/foundation/sm64_foundation_main.cpp)
foreach(_dep Foundation_Shaders Editor_Data Shader_PostprocessBasic Shader_SM64Overlay)
  if(TARGET ${_dep})
    add_dependencies(sm64_foundation ${_dep})
  endif()
endforeach()
target_include_directories(sm64_foundation PRIVATE
  "${SM64_ROOT}/src/pc/gfx"
  "${SM64_ROOT}/src/pc"
  "${FOUNDATION_INCLUDE_DIR}")
target_link_libraries(sm64_foundation PRIVATE
  sm64ex
  Example_Common
  Example_Renderer
  SDL3::SDL3)
# Run outside MSYS2 (RenderDoc, plain Explorer), where an unrelated
# libwinpthread-1.dll on PATH otherwise wins and fails to resolve nanosleep64.
# Windows prefers the exe's own directory, so copying the runtime beside it is
# enough -- and unlike -static it avoids MinGW's static winpthread, whose
# nanosleep disagrees with the headers about struct timespec.
if(MINGW)
  get_filename_component(_sm64_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  foreach(_dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll libatomic-1.dll)
    if(EXISTS "${_sm64_cxx_dir}/${_dll}")
      add_custom_command(TARGET sm64_foundation POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_sm64_cxx_dir}/${_dll}" "$<TARGET_FILE_DIR:sm64_foundation>"
        COMMENT "Staging ${_dll} next to sm64_foundation")
    endif()
  endforeach()
endif()
set_target_properties(sm64_foundation PROPERTIES
  CXX_STANDARD 20
  CXX_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_definitions(sm64_foundation PRIVATE SM64_FOUNDATION_HOST=1)
message(STATUS "SM64: foundation port (source=${_foundation_src})")
