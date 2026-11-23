# Konami Viper static recompilation - the native part of a game's Android app (libmain.so, with
# SDL2's libSDL2.so). Built by Gradle in the project android/prepare.py makes (see android/README.md).
#
# The same sources as the desktop build (tools/buildspec.py), for Android: OpenGL ES 3.0 for the
# GPU renderer (VIPER_GLES), the touch controls and the app's services (runtime/touch.c,
# runtime/platform_android.c; VIPER_MOBILE, VIPER_ANDROID). Link play is the desktop's, over the
# Internet with a session code (sockets, UPnP / NAT-PMP).
cmake_minimum_required(VERSION 3.22)
project(viper_android C CXX)

set(GAME "thrild2" CACHE STRING "the game's profile (games/<id>)")
set(SDL_SRC "" CACHE PATH "SDL2's release sources (android/prepare.py fetches them)")
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(GEN "${ROOT}/generated/${GAME}")
if(NOT EXISTS "${GEN}/sources.mk")
  message(FATAL_ERROR "${GEN} is missing: run android/prepare.py")
endif()

# ------------------------------------------------------------------ SDL2 (libSDL2.so, loaded by SDLActivity)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${SDL_SRC}" sdl)

# ------------------------------------------------------------------ the sources, from tools/buildspec.py
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ROOT}/tools/buildspec.py")
function(buildspec kind out)         # one kind of sources ("runtime", "voodoo", "gl", "third_party")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -I -c
      "import sys; sys.path.insert(0, sys.argv[1]); import buildspec as b; print(';'.join(b.sources(sys.argv[2], 'Linux')[sys.argv[3]]))"
      "${ROOT}/tools" "${ROOT}" "${kind}"
    OUTPUT_VARIABLE list OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  set(${out} "${list}" PARENT_SCOPE)
endfunction()
buildspec(runtime RUNTIME_C)
buildspec(voodoo VOODOO_CPP)
buildspec(gl GL_CPP)
buildspec(third_party TP_C)
list(APPEND RUNTIME_C "${ROOT}/runtime/touch.c" "${ROOT}/runtime/platform_android.c")
file(READ "${GEN}/sources.mk" GEN_MK)
string(REGEX REPLACE "^GEN_SRCS = " "" GEN_MK "${GEN_MK}")
string(STRIP "${GEN_MK}" GEN_MK)
string(REPLACE " " ";" GEN_C "${GEN_MK}")
list(TRANSFORM GEN_C PREPEND "${ROOT}/")

add_library(main SHARED ${RUNTIME_C} ${VOODOO_CPP} ${GL_CPP} ${TP_C} ${GEN_C})
target_include_directories(main PRIVATE "${ROOT}/runtime" "${ROOT}/runtime/voodoo" "${GEN}")
# UPnP and NAT-PMP's headers for the C sources only, as tools/buildspec.py: libnatpmp's folder has a
# file named "version", which C++'s <version> would find first
set_source_files_properties(${RUNTIME_C} ${TP_C} PROPERTIES INCLUDE_DIRECTORIES
  "${ROOT}/third_party/miniupnpc/include;${ROOT}/third_party/miniupnpc/src;${ROOT}/third_party/libnatpmp")
target_compile_definitions(main PRIVATE VIPER_ANDROID VIPER_MOBILE VIPER_GLES MINIUPNP_STATICLIB NATPMP_STATICLIB)
set_target_properties(main PROPERTIES C_STANDARD 11 C_EXTENSIONS ON CXX_STANDARD 20 CXX_EXTENSIONS OFF)
# as tools/buildspec.py: -ffp-contract=off keeps the floating point as the original's, the guest's
# rounding mode is set at run time (-frounding-math); optimized in every build type (the game is
# too slow to play unoptimized)
set(C_FLAGS -O2 -ffp-contract=off -fno-strict-aliasing -D_DEFAULT_SOURCE -w)
set_source_files_properties(${RUNTIME_C} PROPERTIES COMPILE_OPTIONS "${C_FLAGS}")
set_source_files_properties(${GEN_C} PROPERTIES COMPILE_OPTIONS "${C_FLAGS};-frounding-math")
set_source_files_properties(${VOODOO_CPP} ${GL_CPP} PROPERTIES COMPILE_OPTIONS "-O2;-w")
set_source_files_properties(${TP_C} PROPERTIES COMPILE_OPTIONS
  "-O2;-w;-DMINIUPNPC_SET_SOCKET_TIMEOUT;-DMINIUPNPC_GET_SRC_ADDR;-D_BSD_SOURCE")
# SDLActivity runs SDL_main of libmain.so
set_source_files_properties("${ROOT}/runtime/main.c" PROPERTIES COMPILE_DEFINITIONS "main=SDL_main")
target_link_libraries(main PRIVATE SDL2 GLESv3 EGL log android m)
