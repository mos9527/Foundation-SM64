# ---------------------------------------------------------------------------
# Host tools -- CMake translation of tools/Makefile
#
# Only the tools that the PC build actually needs are built by default.
# `tabledesign` is deliberately omitted: it is the only tool that requires the
# bundled `audiofile` sub-project, and the PC asset pipeline uses
# `aiff_extract_codebook` + `vadpcm_enc` instead.
#
# These are *host* tools. The upstream Makefile builds them with the plain
# `gcc` on PATH; because this port is native (host == target) we can simply
# build them as ordinary executables in the same build tree.
# ---------------------------------------------------------------------------

# tools/Makefile: CFLAGS := -I../include -I. -Wall -Wextra -Wno-unused-parameter
#                          -pedantic -std=c99 -O2 -s
set(SM64_TOOL_WARNINGS -Wall -Wextra -Wno-unused-parameter)

# Deviation from upstream: gnu99 instead of strict c99. Several of these tools
# use POSIX functions (strdup, fileno, ...) that a strict -std=c99 MinGW
# environment hides behind feature-test macros. The generated data is
# unaffected.
function(sm64_add_host_tool name)
  cmake_parse_arguments(TOOL "" "" "SOURCES;DEFINES;OPTIONS;LIBS" ${ARGN})

  add_executable(sm64_tool_${name} ${TOOL_SOURCES})
  set_target_properties(sm64_tool_${name} PROPERTIES
    OUTPUT_NAME               ${name}
    RUNTIME_OUTPUT_DIRECTORY  "${SM64_TOOL_BIN_DIR}"
    C_STANDARD                99
    C_EXTENSIONS              ON)
  target_include_directories(sm64_tool_${name} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
  target_compile_options(sm64_tool_${name} PRIVATE
    ${SM64_TOOL_WARNINGS} ${TOOL_OPTIONS})
  if(TOOL_DEFINES)
    target_compile_definitions(sm64_tool_${name} PRIVATE ${TOOL_DEFINES})
  endif()
  target_link_libraries(sm64_tool_${name} PRIVATE m ${TOOL_LIBS})
endfunction()

sm64_add_host_tool(n64graphics
  SOURCES n64graphics.c utils.c
  DEFINES N64GRAPHICS_STANDALONE)

sm64_add_host_tool(n64graphics_ci
  SOURCES n64graphics_ci_dir/n64graphics_ci.c
          n64graphics_ci_dir/exoquant/exoquant.c
          n64graphics_ci_dir/utils.c)

sm64_add_host_tool(textconv
  SOURCES textconv.c utf8.c hashtable.c)

sm64_add_host_tool(skyconv
  SOURCES skyconv.c n64graphics.c utils.c)

sm64_add_host_tool(aiff_extract_codebook
  SOURCES aiff_extract_codebook.c)

sm64_add_host_tool(vadpcm_enc
  SOURCES sdk-tools/adpcm/vadpcm_enc.c
          sdk-tools/adpcm/vpredictor.c
          sdk-tools/adpcm/quant.c
          sdk-tools/adpcm/util.c
          sdk-tools/adpcm/vencode.c
  OPTIONS -Wno-unused-result -Wno-uninitialized -Wno-sign-compare)

# Tools that are only needed for a real N64 ROM build (or for extract_assets).
# Kept behind an option so the default PC build stays lean.
if(SM64_BUILD_ROM_TOOLS)
  sm64_add_host_tool(mio0    SOURCES libmio0.c  DEFINES MIO0_STANDALONE)
  sm64_add_host_tool(n64cksum SOURCES n64cksum.c utils.c DEFINES N64CKSUM_STANDALONE)
  sm64_add_host_tool(aifc_decode SOURCES aifc_decode.c)
  sm64_add_host_tool(extract_data_for_mio SOURCES extract_data_for_mio.c)
  sm64_add_host_tool(patch_libultra_math SOURCES patch_libultra_math.c)
endif()
