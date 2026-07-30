# ---------------------------------------------------------------------------
# Asset code generation -- CMake translation of the asset rules in Makefile
# and Makefile.split.
#
# Every generated file is placed at ${SM64_GEN_DIR}/<same relative path as the
# Makefile's $(BUILD_DIR)/...>, because the game sources include them that way,
# e.g. levels/bob/texture.inc.c contains
#     #include "levels/bob/0.rgba16.inc.c"
# which resolves through the `-I ${SM64_GEN_DIR}` include path.
#
# Expects the following variables to be set by the caller:
#   SM64_ROOT, SM64_GEN_DIR, SM64_CMAKE_DIR, SM64_VERSION,
#   SM64_VERSION_CFLAGS, SM64_ENDIAN, SM64_BITWIDTH,
#   SM64_PYTHON, SM64_AS, SM64_OBJCOPY
#
# Produces:
#   SM64_GENERATED_C_FILES  - generated .c files that must be compiled
#   SM64_ASSET_OUTPUTS      - every generated artifact (drives the asset target)
# ---------------------------------------------------------------------------

set(SM64_GENERATED_C_FILES "")
set(SM64_ASSET_OUTPUTS "")

set(_sm64_asset_dirs "")

# Collects a directory that must exist before the build starts. Neither Ninja
# nor Make create `add_custom_command` output directories on their own.
macro(_sm64_need_dir path)
  get_filename_component(_nd "${path}" DIRECTORY)
  list(APPEND _sm64_asset_dirs "${_nd}")
endmacro()

set(_bin2c "${SM64_CMAKE_DIR}/bin2c.py")
set(_redirect "${SM64_CMAKE_DIR}/redirect.py")
set(_lf "${SM64_CMAKE_DIR}/lf.py")

# ---------------------------------------------------------------------------
# Textures
#
#   $(BUILD_DIR)/%: %.png
#           $(N64GRAPHICS) -i $@ -g $< -f <format>
#   $(BUILD_DIR)/%.inc.c: $(BUILD_DIR)/%
#           hexdump -v -e '1/1 "0x%X,"' $< > $@ ; echo >> $@
#
# The format is the last dotted component of the stem, e.g.
# `textures/cave/cave_01.rgba16.png` -> format `rgba16`.
#
# Upstream drives these pattern rules on demand via generated dependency
# files; CMake has no equivalent, so every convertible PNG under textures/,
# levels/ and actors/ is converted unconditionally. The two steps are fused
# into a single custom command to halve the number of spawned processes.
# ---------------------------------------------------------------------------
set(_sm64_tex_formats rgba16 rgba32 ia16 ia8 ia4 ia1 i8 i4 ci8 ci4)

file(GLOB_RECURSE _sm64_pngs RELATIVE "${SM64_ROOT}"
  "${SM64_ROOT}/textures/*.png"
  "${SM64_ROOT}/levels/*.png"
  "${SM64_ROOT}/actors/*.png")

set(_sm64_texture_count 0)
foreach(_png IN LISTS _sm64_pngs)
  # Skyboxes go through skyconv, not n64graphics; skybox_tiles is an output
  # directory used only by the EXTERNAL_DATA build; ipl3_raw is the N64 boot
  # font, which no PC target references.
  if(_png MATCHES "^textures/(skyboxes|skybox_tiles|ipl3_raw)/")
    continue()
  endif()

  string(REGEX REPLACE "\\.png$" "" _stem "${_png}")
  string(REGEX MATCH "[^./]+$" _fmt "${_stem}")
  if(NOT _fmt IN_LIST _sm64_tex_formats)
    # e.g. levels/ending/cake.png -- handled by skyconv below.
    continue()
  endif()

  if(_fmt STREQUAL "ci8" OR _fmt STREQUAL "ci4")
    set(_gfx_tool sm64_tool_n64graphics_ci)
  else()
    set(_gfx_tool sm64_tool_n64graphics)
  endif()

  set(_bin "${SM64_GEN_DIR}/${_stem}")
  set(_incc "${SM64_GEN_DIR}/${_stem}.inc.c")
  _sm64_need_dir("${_bin}")

  add_custom_command(
    OUTPUT "${_bin}" "${_incc}"
    COMMAND ${_gfx_tool} -i "${_bin}" -g "${SM64_ROOT}/${_png}" -f ${_fmt}
    COMMAND "${SM64_PYTHON}" "${_bin2c}" "${_bin}" "${_incc}"
    DEPENDS "${SM64_ROOT}/${_png}" "${_bin2c}"
    COMMENT "Texture ${_stem}"
    VERBATIM)

  list(APPEND SM64_ASSET_OUTPUTS "${_incc}")
  math(EXPR _sm64_texture_count "${_sm64_texture_count} + 1")
endforeach()

message(STATUS "SM64: ${_sm64_texture_count} textures to convert")

# ---------------------------------------------------------------------------
# Skyboxes
#
#   $(BUILD_DIR)/bin/%_skybox.c: textures/skyboxes/%.png
#           $(SKYCONV) --type sky --split $< $(BUILD_DIR)/bin
# ---------------------------------------------------------------------------
file(GLOB _sm64_skybox_pngs "${SM64_ROOT}/textures/skyboxes/*.png")
foreach(_png IN LISTS _sm64_skybox_pngs)
  get_filename_component(_name "${_png}" NAME_WE)
  set(_out "${SM64_GEN_DIR}/bin/${_name}_skybox.c")
  _sm64_need_dir("${_out}")

  add_custom_command(
    OUTPUT "${_out}"
    COMMAND sm64_tool_skyconv --type sky --split "${_png}" "${SM64_GEN_DIR}/bin"
    DEPENDS "${_png}"
    COMMENT "Skybox ${_name}"
    VERBATIM)

  list(APPEND SM64_GENERATED_C_FILES "${_out}")
  list(APPEND SM64_ASSET_OUTPUTS "${_out}")
endforeach()

# ---------------------------------------------------------------------------
# Ending cake image
#
#   $(BUILD_DIR)/levels/ending/cake.inc.c: levels/ending/cake.png
#           $(SKYCONV) --type cake --split $< $(BUILD_DIR)/levels/ending
# ---------------------------------------------------------------------------
foreach(_cake cake cake_eu)
  set(_cake_png "${SM64_ROOT}/levels/ending/${_cake}.png")
  if(EXISTS "${_cake_png}")
    set(_out "${SM64_GEN_DIR}/levels/ending/${_cake}.inc.c")
    _sm64_need_dir("${_out}")
    add_custom_command(
      OUTPUT "${_out}"
      COMMAND sm64_tool_skyconv --type cake --split "${_cake_png}"
              "${SM64_GEN_DIR}/levels/ending"
      DEPENDS "${_cake_png}"
      COMMENT "Cake ${_cake}"
      VERBATIM)
    list(APPEND SM64_ASSET_OUTPUTS "${_out}")
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Text / dialog
#
#   $(BUILD_DIR)/include/text_strings.h: include/text_strings.h.in
#           $(TEXTCONV) charmap.txt $< $@
#   $(BUILD_DIR)/text/%/define_text.inc.c: text/define_text.inc.c ...
#           $(CPP) $(VERSION_CFLAGS) $< -o - -I text/%/ | $(TEXTCONV) charmap.txt - $@
#
# The pipe is replaced by an explicit intermediate .i file.
# ---------------------------------------------------------------------------
foreach(_ts text_strings:charmap.txt
            text_menu_strings:charmap_menu.txt
            text_options_strings:charmap.txt)
  string(REPLACE ":" ";" _ts_parts "${_ts}")
  list(GET _ts_parts 0 _ts_name)
  list(GET _ts_parts 1 _ts_charmap)

  set(_in "${SM64_ROOT}/include/${_ts_name}.h.in")
  if(NOT EXISTS "${_in}")
    continue()
  endif()
  set(_out "${SM64_GEN_DIR}/include/${_ts_name}.h")
  _sm64_need_dir("${_out}")

  add_custom_command(
    OUTPUT "${_out}"
    # No newline normalisation here: textconv copies non-encoded text through
    # verbatim, so the output inherits whatever line endings the .h.in source
    # was checked out with -- exactly like the Makefile.
    COMMAND sm64_tool_textconv "${SM64_ROOT}/${_ts_charmap}" "${_in}" "${_out}"
    DEPENDS "${_in}" "${SM64_ROOT}/${_ts_charmap}"
    COMMENT "textconv ${_ts_name}.h"
    VERBATIM)

  list(APPEND SM64_ASSET_OUTPUTS "${_out}")
endforeach()

set(_define_text_i "${SM64_GEN_DIR}/text/${SM64_VERSION}/define_text.i")
set(_define_text_c "${SM64_GEN_DIR}/text/${SM64_VERSION}/define_text.inc.c")
_sm64_need_dir("${_define_text_c}")
add_custom_command(
  OUTPUT "${_define_text_c}"
  COMMAND "${CMAKE_C_COMPILER}" -E -P ${SM64_VERSION_CFLAGS}
          -I "${SM64_ROOT}/text/${SM64_VERSION}/"
          "${SM64_ROOT}/text/define_text.inc.c" -o "${_define_text_i}"
  # gcc writes its -E output through the CRT's text mode on Windows; normalise
  # the intermediate so the preprocessor's line endings do not leak into the
  # generated source.
  COMMAND "${SM64_PYTHON}" "${_lf}" "${_define_text_i}"
  COMMAND sm64_tool_textconv "${SM64_ROOT}/charmap.txt"
          "${_define_text_i}" "${_define_text_c}"
  DEPENDS "${_lf}"
          "${SM64_ROOT}/text/define_text.inc.c"
          "${SM64_ROOT}/text/${SM64_VERSION}/courses.h"
          "${SM64_ROOT}/text/${SM64_VERSION}/dialogs.h"
          "${SM64_ROOT}/charmap.txt"
  COMMENT "textconv define_text.inc.c"
  VERBATIM)
list(APPEND SM64_ASSET_OUTPUTS "${_define_text_c}")

# ---------------------------------------------------------------------------
# Level headers
#
#   $(BUILD_DIR)/include/level_headers.h: levels/level_headers.h.in
#           $(CPP) -I . $< | $(PYTHON) tools/output_level_headers.py > $@
#
# `-x c` is required because gcc cannot infer the language of a `.h.in` file,
# whereas the standalone `cpp` driver assumes C. `-P` matches the Makefile's
# `CPP := cpp -P` override, which is what every Windows build uses.
# ---------------------------------------------------------------------------
set(_lh_i "${SM64_GEN_DIR}/include/level_headers.i")
set(_lh_h "${SM64_GEN_DIR}/include/level_headers.h")
_sm64_need_dir("${_lh_h}")
add_custom_command(
  OUTPUT "${_lh_h}"
  COMMAND "${CMAKE_C_COMPILER}" -E -P -x c -I "${SM64_ROOT}"
          "${SM64_ROOT}/levels/level_headers.h.in" -o "${_lh_i}"
  COMMAND "${SM64_PYTHON}" "${_redirect}" "${_lh_h}" --stdin "${_lh_i}" --
          "${SM64_PYTHON}" "${SM64_ROOT}/tools/output_level_headers.py"
  DEPENDS "${SM64_ROOT}/levels/level_headers.h.in"
          "${SM64_ROOT}/tools/output_level_headers.py" "${_redirect}"
  COMMENT "Generating level_headers.h"
  VERBATIM)
list(APPEND SM64_ASSET_OUTPUTS "${_lh_h}")

# ---------------------------------------------------------------------------
# Mario animations and demo data
#
#   $(BUILD_DIR)/assets/mario_anim_data.c: $(wildcard assets/anims/*.inc.c)
#           $(PYTHON) tools/mario_anims_converter.py > $@
#   $(BUILD_DIR)/assets/demo_data.c: assets/demo_data.json $(wildcard assets/demos/*.bin)
#           $(PYTHON) tools/demo_data_converter.py assets/demo_data.json $(VERSION_CFLAGS) > $@
#
# Both scripts resolve their inputs relative to the repository root.
# ---------------------------------------------------------------------------
file(GLOB _sm64_anim_files "${SM64_ROOT}/assets/anims/*.inc.c")
set(_anim_c "${SM64_GEN_DIR}/assets/mario_anim_data.c")
_sm64_need_dir("${_anim_c}")
add_custom_command(
  OUTPUT "${_anim_c}"
  COMMAND "${SM64_PYTHON}" "${_redirect}" "${_anim_c}" --
          "${SM64_PYTHON}" "${SM64_ROOT}/tools/mario_anims_converter.py"
  WORKING_DIRECTORY "${SM64_ROOT}"
  DEPENDS ${_sm64_anim_files} "${SM64_ROOT}/tools/mario_anims_converter.py" "${_redirect}"
  COMMENT "Generating mario_anim_data.c"
  VERBATIM)
list(APPEND SM64_GENERATED_C_FILES "${_anim_c}")
list(APPEND SM64_ASSET_OUTPUTS "${_anim_c}")

file(GLOB _sm64_demo_files "${SM64_ROOT}/assets/demos/*.bin")
set(_demo_c "${SM64_GEN_DIR}/assets/demo_data.c")
add_custom_command(
  OUTPUT "${_demo_c}"
  COMMAND "${SM64_PYTHON}" "${_redirect}" "${_demo_c}" --
          "${SM64_PYTHON}" "${SM64_ROOT}/tools/demo_data_converter.py"
          "${SM64_ROOT}/assets/demo_data.json" ${SM64_VERSION_CFLAGS}
  WORKING_DIRECTORY "${SM64_ROOT}"
  DEPENDS "${SM64_ROOT}/assets/demo_data.json" ${_sm64_demo_files}
          "${SM64_ROOT}/tools/demo_data_converter.py" "${_redirect}"
  COMMENT "Generating demo_data.c"
  VERBATIM)
list(APPEND SM64_GENERATED_C_FILES "${_demo_c}")
list(APPEND SM64_ASSET_OUTPUTS "${_demo_c}")

# ---------------------------------------------------------------------------
# Sound
#
#   $(BUILD_DIR)/sound/samples/%.table: sound/samples/%.aiff
#           $(AIFF_EXTRACT_CODEBOOK) $< > $@
#   $(BUILD_DIR)/sound/samples/%.aifc: $(BUILD_DIR)/sound/samples/%.table sound/samples/%.aiff
#           $(VADPCM_ENC) -c $^ $@
# ---------------------------------------------------------------------------
set(_sm64_aifcs "")
file(GLOB _sm64_sample_dirs RELATIVE "${SM64_ROOT}/sound/samples"
     "${SM64_ROOT}/sound/samples/*")
foreach(_dir IN LISTS _sm64_sample_dirs)
  if(NOT IS_DIRECTORY "${SM64_ROOT}/sound/samples/${_dir}")
    continue()
  endif()
  file(GLOB _aiffs RELATIVE "${SM64_ROOT}/sound/samples/${_dir}"
       "${SM64_ROOT}/sound/samples/${_dir}/*.aiff")
  foreach(_aiff IN LISTS _aiffs)
    get_filename_component(_name "${_aiff}" NAME_WE)
    set(_aiff_path "${SM64_ROOT}/sound/samples/${_dir}/${_aiff}")
    set(_table "${SM64_GEN_DIR}/sound/samples/${_dir}/${_name}.table")
    set(_aifc "${SM64_GEN_DIR}/sound/samples/${_dir}/${_name}.aifc")
    _sm64_need_dir("${_aifc}")

    # A target name is only substituted when it appears as the *command*; here
    # the tool is an argument to redirect.py, so it needs an explicit
    # generator expression plus a manual dependency on the tool target.
    add_custom_command(
      OUTPUT "${_aifc}" "${_table}"
      COMMAND "${SM64_PYTHON}" "${_redirect}" "${_table}" --
              "$<TARGET_FILE:sm64_tool_aiff_extract_codebook>" "${_aiff_path}"
      COMMAND sm64_tool_vadpcm_enc -c "${_table}" "${_aiff_path}" "${_aifc}"
      DEPENDS "${_aiff_path}" "${_redirect}" sm64_tool_aiff_extract_codebook
      COMMENT "Sound sample ${_dir}/${_name}"
      VERBATIM)

    list(APPEND _sm64_aifcs "${_aifc}")
  endforeach()
endforeach()

list(LENGTH _sm64_aifcs _sm64_aifc_count)
message(STATUS "SM64: ${_sm64_aifc_count} sound samples to encode")

# Sequences stored as assembly need to be assembled and stripped down to their
# .rodata section:
#   $(BUILD_DIR)/%.o: %.s      -> $(AS) $(ASFLAGS) -o $@ $<
#   $(SOUND_BIN_DIR)/%.m64: %.o -> $(OBJCOPY) -j .rodata $< -O binary $@
set(_sm64_sequence_files "")
foreach(_seqdir "sound/sequences" "sound/sequences/${SM64_VERSION}")
  file(GLOB _m64s "${SM64_ROOT}/${_seqdir}/*.m64")
  list(APPEND _sm64_sequence_files ${_m64s})

  file(GLOB _asms "${SM64_ROOT}/${_seqdir}/*.s")
  foreach(_asm IN LISTS _asms)
    get_filename_component(_name "${_asm}" NAME_WE)
    set(_obj "${SM64_GEN_DIR}/${_seqdir}/${_name}.o")
    set(_m64 "${SM64_GEN_DIR}/${_seqdir}/${_name}.m64")
    _sm64_need_dir("${_m64}")

    add_custom_command(
      OUTPUT "${_m64}"
      COMMAND "${SM64_AS}" -I "${SM64_ROOT}/include" -I "${SM64_GEN_DIR}"
              --defsym AVOID_UB=1 -o "${_obj}" "${_asm}"
      COMMAND "${SM64_OBJCOPY}" -j .rodata "${_obj}" -O binary "${_m64}"
      DEPENDS "${_asm}"
      COMMENT "Sequence ${_name}.m64"
      VERBATIM)

    list(APPEND _sm64_sequence_files "${_m64}")
  endforeach()
endforeach()

file(GLOB _sm64_sound_banks "${SM64_ROOT}/sound/sound_banks/*.json")

# `$$(cat $(ENDIAN_BITWIDTH))` -- upstream compiles a probe program and scrapes
# the resulting error message. CMake already knows both properties.
set(_endian_bitwidth --endian ${SM64_ENDIAN} --bitwidth ${SM64_BITWIDTH})

set(_ctl "${SM64_GEN_DIR}/sound/sound_data.ctl")
set(_tbl "${SM64_GEN_DIR}/sound/sound_data.tbl")
_sm64_need_dir("${_ctl}")
add_custom_command(
  OUTPUT "${_ctl}" "${_tbl}"
  COMMAND "${SM64_PYTHON}" "${SM64_ROOT}/tools/assemble_sound.py"
          "${SM64_GEN_DIR}/sound/samples/" "${SM64_ROOT}/sound/sound_banks/"
          "${_ctl}" "${_tbl}" ${SM64_VERSION_CFLAGS} ${_endian_bitwidth}
  WORKING_DIRECTORY "${SM64_ROOT}"
  DEPENDS ${_sm64_aifcs} ${_sm64_sound_banks}
          "${SM64_ROOT}/tools/assemble_sound.py"
  COMMENT "Assembling sound_data.ctl/tbl"
  VERBATIM)

set(_seqbin "${SM64_GEN_DIR}/sound/sequences.bin")
set(_banksets "${SM64_GEN_DIR}/sound/bank_sets")
add_custom_command(
  OUTPUT "${_seqbin}" "${_banksets}"
  COMMAND "${SM64_PYTHON}" "${SM64_ROOT}/tools/assemble_sound.py" --sequences
          "${_seqbin}" "${_banksets}" "${SM64_ROOT}/sound/sound_banks/"
          "${SM64_ROOT}/sound/sequences.json" ${_sm64_sequence_files}
          ${SM64_VERSION_CFLAGS} ${_endian_bitwidth}
  WORKING_DIRECTORY "${SM64_ROOT}"
  DEPENDS ${_sm64_sequence_files} ${_sm64_sound_banks}
          "${SM64_ROOT}/sound/sequences.json"
          "${SM64_ROOT}/tools/assemble_sound.py"
  COMMENT "Assembling sequences.bin/bank_sets"
  VERBATIM)

# sound/sound_data.c wraps these four binaries with the usual hexdump rule.
foreach(_sb sound_data.ctl sound_data.tbl sequences.bin bank_sets)
  set(_bin "${SM64_GEN_DIR}/sound/${_sb}")
  set(_incc "${SM64_GEN_DIR}/sound/${_sb}.inc.c")
  add_custom_command(
    OUTPUT "${_incc}"
    COMMAND "${SM64_PYTHON}" "${_bin2c}" "${_bin}" "${_incc}"
    DEPENDS "${_bin}" "${_bin2c}"
    COMMENT "Embedding sound/${_sb}"
    VERBATIM)
  list(APPEND SM64_ASSET_OUTPUTS "${_incc}")
endforeach()

# ---------------------------------------------------------------------------
# Create every output directory up front.
# ---------------------------------------------------------------------------
list(REMOVE_DUPLICATES _sm64_asset_dirs)
foreach(_d IN LISTS _sm64_asset_dirs)
  file(MAKE_DIRECTORY "${_d}")
endforeach()

list(LENGTH SM64_ASSET_OUTPUTS _sm64_output_count)
message(STATUS "SM64: ${_sm64_output_count} generated asset files")
