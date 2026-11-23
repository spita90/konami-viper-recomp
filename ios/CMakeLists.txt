# Konami Viper static recompilation - the iOS app of a game (iPhone and iPad).
#
# Generated and configured by ios/prepare.py, which stages the game data first (see ios/README.md):
#   python3 ios/prepare.py thrild2 --team ABCDE12345
# then open build/ios/<id>/xcode/viper_<id>.xcodeproj in Xcode and run it on the device.
#
# The same sources as the desktop build (tools/buildspec.py), for iOS: OpenGL ES 3.0 for the GPU
# renderer (VIPER_GLES), link play with the phones nearby through Network.framework (VIPER_P2P:
# runtime/net_p2p.m instead of the sockets, portmap.c and their libraries), the touch controls and
# the app's services (runtime/touch.c, runtime/platform_ios.m). SDL2 is built from its release
# sources, fetched once.
cmake_minimum_required(VERSION 3.21...3.30)   # 3.30: FetchContent without a sub-build (CMP0168)
if(NOT DEFINED CMAKE_SYSTEM_NAME)
  set(CMAKE_SYSTEM_NAME iOS)
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0 CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "")
set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH YES)
project(viper_ios C CXX OBJC)

set(GAME "thrild2" CACHE STRING "the game's profile (games/<id>)")
set(APP_BINARY "td2" CACHE STRING "the executable's name (the profile's binary)")
set(APP_TITLE "Thrill Drive 2" CACHE STRING "the name on the home screen")
set(APP_VERSION "0.0.0" CACHE STRING "the port's version")
set(VIPER_DATA "" CACHE PATH "the game data for the app, staged by ios/prepare.py")
set(BUNDLE_ID "local.viper.${APP_BINARY}" CACHE STRING "the app's bundle identifier")
set(DEVELOPMENT_TEAM "" CACHE STRING "the Apple development team that signs the app")
set(APP_ASSETS "" CACHE PATH "the asset catalog with the app's icon (ios/prepare.py)")

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(GEN "${ROOT}/generated/${GAME}")
if(NOT EXISTS "${GEN}/sources.mk")
  message(FATAL_ERROR "${GEN} is missing: run 'make extract GAME=${GAME}' and 'make recomp GAME=${GAME}' first")
endif()
if(NOT IS_DIRECTORY "${VIPER_DATA}")
  message(FATAL_ERROR "VIPER_DATA (the staged game data) is missing: run ios/prepare.py")
endif()

# ------------------------------------------------------------------ SDL2
# -DFETCHCONTENT_SOURCE_DIR_SDL2=<path> uses SDL2 sources already on disk instead
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2
  URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
  URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165)
FetchContent_MakeAvailable(SDL2)

# ------------------------------------------------------------------ the app
set(RUNTIME_C cpu sched hw main frontend_sdl enhanced modes interp touch net)
set(VOODOO_CPP voodoo voodoo_2 voodoo_banshee voodoo_render voodoo_bridge voodoo_gpu)
list(TRANSFORM RUNTIME_C PREPEND "${ROOT}/runtime/")
list(TRANSFORM RUNTIME_C APPEND ".c")
list(TRANSFORM VOODOO_CPP PREPEND "${ROOT}/runtime/voodoo/")
list(TRANSFORM VOODOO_CPP APPEND ".cpp")
file(READ "${GEN}/sources.mk" GEN_MK)
string(REGEX REPLACE "^GEN_SRCS = " "" GEN_MK "${GEN_MK}")
string(STRIP "${GEN_MK}" GEN_MK)
string(REPLACE " " ";" GEN_C "${GEN_MK}")
list(TRANSFORM GEN_C PREPEND "${ROOT}/")

set(LAUNCH_SCREEN "${CMAKE_CURRENT_SOURCE_DIR}/LaunchScreen.storyboard")
set(OBJC_SRCS "${ROOT}/runtime/platform_ios.m" "${ROOT}/runtime/net_p2p.m")
add_executable(app MACOSX_BUNDLE ${RUNTIME_C} ${VOODOO_CPP} "${ROOT}/runtime/gpu_gl.cpp" ${OBJC_SRCS} ${GEN_C}
               ${LAUNCH_SCREEN})
set_source_files_properties(${LAUNCH_SCREEN} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
if(APP_ASSETS AND IS_DIRECTORY "${APP_ASSETS}")
  target_sources(app PRIVATE "${APP_ASSETS}")
  set_source_files_properties("${APP_ASSETS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(app PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon)
endif()

target_include_directories(app PRIVATE "${ROOT}/runtime" "${ROOT}/runtime/voodoo" "${GEN}")
target_compile_definitions(app PRIVATE VIPER_IOS VIPER_GLES VIPER_P2P)
set_target_properties(app PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF CXX_STANDARD 20 CXX_EXTENSIONS OFF)
# as tools/buildspec.py: -ffp-contract=off keeps the floating point as the original's, the guest's
# rounding mode is set at run time (-frounding-math); optimized also in Debug (the game is too
# slow at -O0 to play)
set(C_FLAGS -O2 -ffp-contract=off -fno-strict-aliasing -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
    -Wno-unused-label -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable
    -Wno-parentheses-equality -Wno-constant-logical-operand)
set_source_files_properties(${RUNTIME_C} PROPERTIES COMPILE_OPTIONS "${C_FLAGS}")
set_source_files_properties(${GEN_C} PROPERTIES COMPILE_OPTIONS "${C_FLAGS};-frounding-math")
set_source_files_properties(${VOODOO_CPP} "${ROOT}/runtime/gpu_gl.cpp" PROPERTIES
                            COMPILE_OPTIONS "-O2;-Wno-unused-private-field;-Wno-deprecated-declarations;-Wno-duplicate-decl-specifier")
set_source_files_properties(${OBJC_SRCS} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
# SDL2main provides main() and calls the program's, renamed
set_source_files_properties("${ROOT}/runtime/main.c" PROPERTIES COMPILE_DEFINITIONS "main=SDL_main")

target_link_libraries(app PRIVATE SDL2::SDL2main SDL2::SDL2-static
  "-framework UIKit" "-framework QuartzCore" "-framework CoreMotion" "-framework OpenGLES" "-framework Network"
  "-framework GameController" "-framework CoreHaptics" "-framework AVFoundation" "-ObjC")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
set_target_properties(app PROPERTIES
  OUTPUT_NAME "${APP_BINARY}"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
  XCODE_GENERATE_SCHEME ON
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${BUNDLE_ID}"
  XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${DEVELOPMENT_TEAM}"
  XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
  XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
  XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
  XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphoneos iphonesimulator"
  XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
  XCODE_ATTRIBUTE_ENABLE_BITCODE NO
  XCODE_ATTRIBUTE_INSTALL_PATH "$(LOCAL_APPS_DIR)"
  XCODE_ATTRIBUTE_SKIP_INSTALL NO)

# the game data at the bundle's root, where the executable is: its default paths (work/<id>/,
# roms/...) are relative to it; the bundle is signed after this
add_custom_command(TARGET app POST_BUILD
  COMMAND rsync -a "${VIPER_DATA}/" "$<TARGET_BUNDLE_DIR:app>/"
  COMMENT "Copying the game data into the app")
