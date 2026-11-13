cmake_minimum_required(VERSION 3.20)
project(RondoRecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

add_library(hucpu STATIC runtime/cpu/huc6280.c runtime/cpu/huc6280_info.c)
target_include_directories(hucpu PUBLIC runtime)

add_library(pce STATIC
  runtime/pce/pce.c
  runtime/pce/video.c
  runtime/pce/disc.c
  runtime/pce/discid.c
  runtime/pce/bios_hle.c
  runtime/pce/kanji_font.c
  runtime/pce/bram.c
  runtime/pce/sound.c
  runtime/pce/state.c
  runtime/recomp/dispatch.c
  runtime/util/png.c)
target_include_directories(pce PUBLIC runtime)
target_link_libraries(pce PUBLIC hucpu)
if(NOT WIN32)
  target_link_libraries(pce PUBLIC m)   # pow, fmod (Windows has them in its C library)
endif()

add_executable(rondo_headless tools/headless/main.c runtime/recomp/recomp_none.c tools/frontend/game.c tools/frontend/prompt.c)
target_link_libraries(rondo_headless PRIVATE pce)

# Code generator (developer front end; Setup links the same generator)
add_library(recompgen STATIC tools/recomp/gen.c)
target_link_libraries(recompgen PUBLIC pce)
add_executable(rondo_gen tools/recomp/gen_main.c runtime/recomp/recomp_none.c)
target_link_libraries(rondo_gen PRIVATE recompgen)

# Developer builds with generated code: -DRONDO_GEN_DIR=<rondo_gen --out directory>
set(RONDO_GEN_DIR "" CACHE PATH "directory with generated regions (rondo_gen --out)")
if(RONDO_GEN_DIR AND EXISTS "${RONDO_GEN_DIR}/table.c")
  file(GLOB RONDO_GEN_SOURCES "${RONDO_GEN_DIR}/*.c")
  add_library(rondogen STATIC ${RONDO_GEN_SOURCES})
  target_include_directories(rondogen PUBLIC runtime "${RONDO_GEN_DIR}")
  target_compile_options(rondogen PRIVATE -O1 -w)
  add_executable(rondo_headless_rc tools/headless/main.c tools/frontend/game.c tools/frontend/prompt.c)
  target_link_libraries(rondo_headless_rc PRIVATE pce rondogen)
endif()

# Windowed frontend (optional, needs SDL3)
# also look next to the compiler (e.g. an MSYS2 prefix)
get_filename_component(_cc_prefix "${CMAKE_C_COMPILER}" DIRECTORY)
get_filename_component(_cc_prefix "${_cc_prefix}" DIRECTORY)
find_path(SDL3_INCLUDE_DIR SDL3/SDL.h HINTS "${_cc_prefix}/include")
find_library(SDL3_LIBRARY NAMES SDL3 HINTS "${_cc_prefix}/lib")
if(SDL3_INCLUDE_DIR AND SDL3_LIBRARY)
  # RetroAchievements' client library (tools/frontend/achievements.c)
  include(tools/rcheevos_sources.cmake)
  list(TRANSFORM RCHEEVOS_SOURCES PREPEND third_party/rcheevos/)
  add_library(rcheevos STATIC ${RCHEEVOS_SOURCES})
  target_include_directories(rcheevos PUBLIC third_party/rcheevos/include)
  target_compile_definitions(rcheevos PUBLIC ${RCHEEVOS_DEFINES})
  target_compile_options(rcheevos PRIVATE -w)

  add_executable(rondo WIN32 tools/frontend/main.c tools/frontend/locate.c tools/frontend/game.c tools/frontend/prompt.c
    tools/frontend/input.c tools/frontend/binds.c tools/frontend/menu.c tools/frontend/settings.c tools/frontend/video.c tools/frontend/theme.c tools/frontend/ui_fonts.c
    tools/frontend/http.c tools/frontend/achievements.c tools/frontend/update.c tools/frontend/child.c
    tools/frontend/showcase.c runtime/recomp/recomp_none.c)
  target_include_directories(rondo PRIVATE ${SDL3_INCLUDE_DIR} tools/recomp)
  target_link_libraries(rondo PRIVATE pce rcheevos ${SDL3_LIBRARY})
  if(WIN32)
    target_link_libraries(rondo PRIVATE winhttp)
  endif()
  get_filename_component(SDL3_LIBDIR "${SDL3_LIBRARY}" DIRECTORY)
  if(WIN32)
    # copy SDL3.dll and its non-system dependency so rondo.exe runs outside the toolchain shell
    foreach(_dll SDL3.dll libiconv-2.dll)
      find_file(_dll_path_${_dll} ${_dll} HINTS "${SDL3_LIBDIR}/../bin" NO_DEFAULT_PATH)
      if(_dll_path_${_dll})
        add_custom_command(TARGET rondo POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll_path_${_dll}}" "$<TARGET_FILE_DIR:rondo>")
      endif()
    endforeach()
    set(SDL3_SDK_LIB "${SDL3_LIBDIR}/libSDL3.dll.a")
  else()
    # Linux: the programs find libSDL3.so.0 next to themselves ($ORIGIN), as in the release
    # folder, so it is copied there (the file itself, not the symlink)
    set(SDL3_SDK_LIB "${SDL3_LIBDIR}/libSDL3.so.0")
    add_custom_command(TARGET rondo POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDL3_SDK_LIB}" "$<TARGET_FILE_DIR:rondo>")
    set_target_properties(rondo PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
  endif()

  # Setup: imports the player's disc image and builds rondo.exe with zig cc
  find_library(ZLIB_STATIC NAMES libz.a z HINTS "${_cc_prefix}/lib")
  if(ZLIB_STATIC)
    # Virtual Console releases (tools/setup/vc.c), .7z archives and .chd images (tools/setup/chd.c):
    # the LZMA SDK's decoder and stb_vorbis, both public domain
    file(GLOB LZMA_SOURCES third_party/lzma/*.c)
    add_library(lzmadec STATIC ${LZMA_SOURCES})
    target_compile_definitions(lzmadec PUBLIC Z7_PPMD_SUPPORT Z7_EXTRACT_ONLY)
    target_include_directories(lzmadec PUBLIC third_party/lzma)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(lzmadec PRIVATE -w)
      set_source_files_properties(tools/setup/vorbis.c PROPERTIES COMPILE_OPTIONS "-w")
    endif()
    # updates (tools/setup/install.c): GitHub's releases, downloaded as the game's own requests are
    set(SETUP_SOURCES tools/setup/main.c tools/setup/zip.c tools/setup/install.c tools/setup/chd.c
      tools/setup/vc.c tools/setup/vc_crypto.c tools/setup/sevenzip.c tools/setup/vorbis.c
      tools/frontend/http.c tools/frontend/update.c tools/frontend/child.c runtime/recomp/recomp_none.c)
    add_executable(rondo_setup ${SETUP_SOURCES})
    target_include_directories(rondo_setup PRIVATE runtime ${SDL3_INCLUDE_DIR} third_party/stb_vorbis)
    target_link_libraries(rondo_setup PRIVATE recompgen lzmadec ${SDL3_LIBRARY} ${ZLIB_STATIC})
    # the same Setup with a window (what players double-click)
    add_executable(rondo_setup_gui WIN32 tools/setup/gui.c ${SETUP_SOURCES})
    target_compile_definitions(rondo_setup_gui PRIVATE SETUP_GUI=1)
    target_include_directories(rondo_setup_gui PRIVATE runtime ${SDL3_INCLUDE_DIR} third_party/stb_vorbis)
    target_link_libraries(rondo_setup_gui PRIVATE recompgen lzmadec ${SDL3_LIBRARY} ${ZLIB_STATIC})
    if(NOT WIN32)
      target_link_libraries(rondo_setup PRIVATE m)
      target_link_libraries(rondo_setup_gui PRIVATE m)
    endif()
    if(WIN32)
      target_link_libraries(rondo_setup PRIVATE winhttp)
      target_link_libraries(rondo_setup_gui PRIVATE winhttp)
    endif()
    set_target_properties(rondo_setup_gui PROPERTIES OUTPUT_NAME "RondoRecomp Setup")
    if(NOT WIN32)
      set_target_properties(rondo_setup rondo_setup_gui PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
    endif()
  else()
    message(STATUS "zlib not found: rondo_setup is not built")
  endif()

  # SDK that Setup compiles from: build/sdk
  add_custom_target(sdk ALL
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/sdk
            -DSDL_INC=${SDL3_INCLUDE_DIR} -DSDL_LIB=${SDL3_SDK_LIB}
            -DSDL_BIN=${SDL3_LIBDIR}/../bin -P ${CMAKE_SOURCE_DIR}/tools/make_sdk.cmake
    COMMENT "Assembling the Setup SDK")
else()
  message(STATUS "SDL3 not found: the windowed frontend (rondo) and Setup are not built")
endif()

# CPU conformance tests (SingleStepTests/huc6280, MIT). Download the v1 *.json.bin
# files into work/sst/v1 (git-ignored) or point SST_DIR elsewhere.
add_executable(sst_huc6280 tests/sst_huc6280.c)
target_link_libraries(sst_huc6280 PRIVATE hucpu)

set(SST_DIR "${CMAKE_SOURCE_DIR}/work/sst/v1" CACHE PATH "SingleStepTests huc6280 v1 directory")
enable_testing()
# 73/C3/F3: self-overwriting wraparound block transfers, see docs/runtime.md
add_test(NAME huc6280_sst COMMAND sst_huc6280 ${SST_DIR} -v 0 --known 73,c3,f3)

add_executable(sound_test tests/sound_test.c runtime/recomp/recomp_none.c)
target_link_libraries(sound_test PRIVATE pce)
add_test(NAME sound COMMAND sound_test)

# needs the disc: build/state_test <game.cue>
add_executable(state_test tests/state_test.c runtime/recomp/recomp_none.c)
target_include_directories(state_test PRIVATE tools)
target_link_libraries(state_test PRIVATE pce)
