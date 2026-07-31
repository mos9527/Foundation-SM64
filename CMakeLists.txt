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
option(SM64_BUILD_EXE "Also build the classic standalone executable for comparison (needs SDL2 + GLEW)" ON)
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
  AAPI_NULL=1)
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
if(SM64_BUILD_EXE)
  find_package(SDL2 QUIET)
  find_package(OpenGL QUIET)
  if(WIN32)
    find_library(GLEW_LIB NAMES glew32 glew)
  else()
    find_package(GLEW QUIET)
  endif()
  if(SDL2_FOUND AND OPENGL_FOUND AND (WIN32 AND GLEW_LIB OR (NOT WIN32 AND GLEW_FOUND)))        
    set(_sm64_exe_sources ${SM64_SOURCES})
    add_executable(sm64pc ${_sm64_exe_sources})
    add_dependencies(sm64pc sm64_assets)
    set_target_properties(sm64pc PROPERTIES
      OUTPUT_NAME "sm64.${SM64_VERSION}.${SM64_GRUCODE}")
    target_include_directories(sm64pc PRIVATE
      "${SM64_ROOT}/include"
      "${SM64_GEN_DIR}"
      "${SM64_GEN_DIR}/include"
      "${SM64_ROOT}/src"
      "${SM64_ROOT}")
    target_compile_definitions(sm64pc PRIVATE
      ${SM64_VERSION_DEFS}
      ${SM64_GRUCODE_DEF}
      RAPI_GL=1 WAPI_SDL2=1 AAPI_SDL2=1 CAPI_SDL2=1 HAVE_SDL2=1)
    if(SM64_GIT_HASH)
      target_compile_definitions(sm64pc PRIVATE NIGHTLY GIT_HASH="${SM64_GIT_HASH}")
    endif()
    target_compile_options(sm64pc PRIVATE
      ${_sm64_opt_flags}
      -fno-strict-aliasing -fwrapv -fsigned-char
      -Wno-format-security)
    set_target_properties(sm64pc PROPERTIES
      C_STANDARD 11
      C_EXTENSIONS ON)
    target_link_libraries(sm64pc PRIVATE SDL2::SDL2 OpenGL::GL)
    if(WIN32)
      target_link_libraries(sm64pc PRIVATE "${GLEW_LIB}" glu32 opengl32 ws2_32 winmm)
    else()
      target_link_libraries(sm64pc PRIVATE GLEW::GLEW m)
      find_package(Threads REQUIRED)
      target_link_libraries(sm64pc PRIVATE Threads::Threads)
    endif()
  else()
    message(WARNING
      "SM64_BUILD_EXE=ON but SDL2/OpenGL/GLEW not found -- "
      "skipping the standalone executable target 'sm64pc'.")
  endif()
endif()

message(STATUS "SM64: version=${SM64_VERSION} grucode=${SM64_GRUCODE} "
               "endian=${SM64_ENDIAN} bits=${SM64_BITWIDTH} "
               "build_exe=${SM64_BUILD_EXE}")
option(SM64_FOUNDATION_PORT "Build the Foundation-renderer host" OFF)
if(SM64_FOUNDATION_PORT)
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
  add_executable(sm64_foundation src/pc/foundation/sm64_foundation_main.cpp)
  
  foreach(_dep Foundation_Shaders Editor_Data Shader_PostprocessBasic)
    if(TARGET ${_dep})
      add_dependencies(sm64_foundation ${_dep})
    endif()
  endforeach()
  target_include_directories(sm64_foundation PRIVATE
    "${SM64_ROOT}/src/pc/gfx"
    "${SM64_ROOT}/src/pc"
    "${FOUNDATION_ROOT_DIR}")
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
  message(STATUS "SM64: foundation port ENABLED (source=${FOUNDATION_SOURCE_DIR})")
endif()
