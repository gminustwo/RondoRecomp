# Assembles the SDK that Setup compiles rondo.exe from (no game material):
#   sdk/sources.txt          runtime + frontend sources to compile (relative to sdk/), and
#                            -I (relative to sdk/) and -D options for them
#   sdk/src/rcheevos/...     RetroAchievements' client library (third_party/rcheevos)
#   sdk/src/runtime/...      runtime sources and headers
#   sdk/src/frontend/...     windowed frontend
#   sdk/include/SDL3/        SDL3 headers
#   sdk/lib/libSDL3.dll.a    SDL3 import library (Windows)
#   sdk/lib/libSDL3.so.0     SDL3 itself (Linux): linked against and copied next to rondo
#   sdk/bin/*.dll            DLLs copied next to rondo.exe (Windows)
#
# cmake -DSRC=<repo> -DOUT=<sdk dir> -DSDL_INC=<dir containing SDL3/>
#       -DSDL_LIB=<libSDL3.dll.a | libSDL3.so.0> -DSDL_BIN=<dir with SDL3.dll> -P tools/make_sdk.cmake

set(RUNTIME_SOURCES
  runtime/cpu/huc6280.c
  runtime/cpu/huc6280_info.c
  runtime/pce/pce.c
  runtime/pce/video.c
  runtime/pce/disc.c
  runtime/pce/discid.c
  runtime/pce/bios_hle.c
  runtime/pce/kanji_font.c
  runtime/pce/bram.c
  runtime/pce/sound.c
  runtime/pce/state.c
  runtime/recomp/dispatch.c)
set(FRONTEND_SOURCES
  tools/frontend/main.c
  tools/frontend/locate.c
  tools/frontend/game.c
  tools/frontend/prompt.c
  tools/frontend/input.c
  tools/frontend/binds.c
  tools/frontend/menu.c
  tools/frontend/settings.c
  tools/frontend/video.c
  tools/frontend/theme.c
  tools/frontend/ui_fonts.c
  tools/frontend/http.c
  tools/frontend/achievements.c
  tools/frontend/update.c
  tools/frontend/child.c
  tools/frontend/showcase.c)
include("${SRC}/tools/rcheevos_sources.cmake")

file(REMOVE_RECURSE "${OUT}")
foreach(dir cpu pce recomp)
  file(COPY "${SRC}/runtime/${dir}" DESTINATION "${OUT}/src/runtime"
       FILES_MATCHING PATTERN "*.c" PATTERN "*.h")
endforeach()
file(COPY "${SRC}/tools/frontend/" DESTINATION "${OUT}/src/frontend"
     FILES_MATCHING PATTERN "*.c" PATTERN "*.h")
file(COPY "${SRC}/third_party/rcheevos/include" "${SRC}/third_party/rcheevos/src" "${SRC}/third_party/rcheevos/LICENSE"
     DESTINATION "${OUT}/src/rcheevos")
# the capture input script, used by rondo.exe --autotest-features
file(COPY "${SRC}/tools/recomp/capture_script.h" DESTINATION "${OUT}/src/frontend")
file(COPY "${SDL_INC}/SDL3" DESTINATION "${OUT}/include")
# configure_file copies what a symlink (libSDL3.so.0 -> libSDL3.so.0.x.y) points to
get_filename_component(sdl_lib_name "${SDL_LIB}" NAME)
configure_file("${SDL_LIB}" "${OUT}/lib/${sdl_lib_name}" COPYONLY)
foreach(dll SDL3.dll libiconv-2.dll)
  if(EXISTS "${SDL_BIN}/${dll}")
    file(COPY "${SDL_BIN}/${dll}" DESTINATION "${OUT}/bin")
  endif()
endforeach()

set(list "# compiled by Setup together with build/gen/*.c (-I paths are relative to sdk/)\n")
string(APPEND list "-Isrc/rcheevos/include\n")
foreach(d ${RCHEEVOS_DEFINES})
  string(APPEND list "-D${d}\n")
endforeach()
foreach(s ${RUNTIME_SOURCES})
  string(APPEND list "src/${s}\n")
endforeach()
foreach(s ${FRONTEND_SOURCES})
  string(REPLACE "tools/frontend/" "frontend/" s "${s}")
  string(APPEND list "src/${s}\n")
endforeach()
foreach(s ${RCHEEVOS_SOURCES})
  string(APPEND list "src/rcheevos/${s}\n")
endforeach()
file(WRITE "${OUT}/sources.txt" "${list}")
