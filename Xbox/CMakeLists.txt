# SNES: Snes9x (Super Nintendo) in a standalone UWP app for Xbox Dev Mode (and Windows), made the same way as the
# Genesis app.
#
# Configure with the Visual Studio generator for WindowsStore, after building SDL3-uwp into ../out/uwp-deps:
#   cmake -S Xbox -B out/xbox -G "Visual Studio 17 2022" -A x64 -DCMAKE_SYSTEM_NAME=WindowsStore
#         -DCMAKE_SYSTEM_VERSION=10.0 -DCMAKE_PREFIX_PATH=out/uwp-deps
cmake_minimum_required(VERSION 3.24)
project(SNESXbox VERSION 0.1.0 LANGUAGES C CXX)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    message(FATAL_ERROR "The Xbox build is a UWP app: configure with -DCMAKE_SYSTEM_NAME=WindowsStore")
endif ()

set(APP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(APP_PACKAGE_VERSION "" CACHE STRING "Four-part package version (e.g. 0.1.0.12); CI sets it from the run number")

# Visual Studio's UWP projects turn on SDL checks (/sdl), which make CRT deprecation warnings errors
add_compile_options(/sdl-)
# Xbox Series X|S (Zen 2)
add_compile_options(/arch:AVX2)

include(FetchContent)

## Dear ImGui (menus)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

find_package(SDL3 REQUIRED CONFIG)

## Snes9x (the repository this folder is in), built as a libretro core and linked straight into the app (the file
## list of its libretro/Makefile.common)
add_library(snes9x_core STATIC
    "${APP_ROOT}/apu/apu.cpp" "${APP_ROOT}/apu/bapu/dsp/sdsp.cpp" "${APP_ROOT}/apu/bapu/smp/smp.cpp"
    "${APP_ROOT}/apu/bapu/smp/smp_state.cpp" "${APP_ROOT}/bsx.cpp" "${APP_ROOT}/c4.cpp" "${APP_ROOT}/c4emu.cpp"
    "${APP_ROOT}/cheats.cpp" "${APP_ROOT}/cheats2.cpp" "${APP_ROOT}/clip.cpp" "${APP_ROOT}/conffile.cpp"
    "${APP_ROOT}/controls.cpp" "${APP_ROOT}/cpu.cpp" "${APP_ROOT}/cpuexec.cpp" "${APP_ROOT}/cpuops.cpp"
    "${APP_ROOT}/crosshairs.cpp" "${APP_ROOT}/dma.cpp" "${APP_ROOT}/dsp.cpp" "${APP_ROOT}/dsp1.cpp"
    "${APP_ROOT}/dsp2.cpp" "${APP_ROOT}/dsp3.cpp" "${APP_ROOT}/dsp4.cpp" "${APP_ROOT}/fxinst.cpp"
    "${APP_ROOT}/fxemu.cpp" "${APP_ROOT}/gfx.cpp" "${APP_ROOT}/globals.cpp" "${APP_ROOT}/memmap.cpp"
    "${APP_ROOT}/obc1.cpp" "${APP_ROOT}/msu1.cpp" "${APP_ROOT}/ppu.cpp" "${APP_ROOT}/stream.cpp" "${APP_ROOT}/sa1.cpp"
    "${APP_ROOT}/sa1cpu.cpp" "${APP_ROOT}/screenshot.cpp" "${APP_ROOT}/sdd1.cpp" "${APP_ROOT}/sdd1emu.cpp"
    "${APP_ROOT}/seta.cpp" "${APP_ROOT}/seta010.cpp" "${APP_ROOT}/seta011.cpp" "${APP_ROOT}/seta018.cpp"
    "${APP_ROOT}/snapshot.cpp" "${APP_ROOT}/snes9x.cpp" "${APP_ROOT}/spc7110.cpp" "${APP_ROOT}/srtc.cpp"
    "${APP_ROOT}/tile.cpp" "${APP_ROOT}/tileimpl-n1x1.cpp" "${APP_ROOT}/tileimpl-n2x1.cpp"
    "${APP_ROOT}/tileimpl-h2x1.cpp" "${APP_ROOT}/sha256.cpp" "${APP_ROOT}/bml.cpp" "${APP_ROOT}/movie.cpp"
    "${APP_ROOT}/fscompat.cpp" "${APP_ROOT}/libretro/libretro.cpp" "${APP_ROOT}/filter/snes_ntsc.c")
target_include_directories(snes9x_core PUBLIC "${APP_ROOT}/libretro" "${APP_ROOT}/libretro/libretro-common/include"
    "${APP_ROOT}" "${APP_ROOT}/apu" "${APP_ROOT}/apu/bapu")
target_compile_definitions(snes9x_core PRIVATE __WIN32__ RIGHTSHIFT_IS_SAR __LIBRETRO__ ALLOW_CPU_OVERCLOCK HAVE_STDINT_H
    _CRT_SECURE_NO_DEPRECATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
target_compile_options(snes9x_core PRIVATE /W0 /wd4996 /MP /EHsc)
set_target_properties(snes9x_core PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON VS_WINRT_COMPONENT FALSE)

## The app
add_executable(SNES WIN32
    src/main.cpp
    src/App.cpp
    src/App.h
    src/Core.cpp
    src/Core.h
    src/XboxAudio.cpp
    src/XboxAudio.h
    src/XboxInput.cpp
    src/XboxInput.h
    src/EdgeScaler.cpp
    src/EdgeScaler.h
    src/Covers.cpp
    src/Covers.h
    src/KnownCheats.cpp
    src/KnownCheats.h
    src/Net.cpp
    src/Net.h
    src/Library.cpp
    src/Library.h
    src/Settings.cpp
    src/Settings.h
    src/UwpPlatform.cpp
    src/UwpPlatform.h
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
)
target_include_directories(SNES PRIVATE src "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(SNES PRIVATE snes9x_core SDL3::SDL3 xaudio2)
set_target_properties(SNES PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(SNES PRIVATE /utf-8 /MP /W3 /wd4996 /EHsc)
target_compile_definitions(SNES PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_options(SNES PRIVATE /STACK:8388608 "/MAP:${CMAKE_BINARY_DIR}/SNES.map")

## UWP package
if (APP_PACKAGE_VERSION)
    set(APP_VERSION4 "${APP_PACKAGE_VERSION}")
else ()
    set(APP_VERSION4 "${PROJECT_VERSION}.0")
endif ()
message(STATUS "SNES: package version ${APP_VERSION4}")
target_compile_definitions(SNES PRIVATE "APP_VERSION=\"${APP_VERSION4}\"")

configure_file(Package.appxmanifest.in "${CMAKE_CURRENT_BINARY_DIR}/Package.appxmanifest" @ONLY)
target_sources(SNES PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Package.appxmanifest")

file(GLOB APP_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/Assets/*.png")
target_sources(SNES PRIVATE ${APP_ASSETS})
set_source_files_properties(${APP_ASSETS} PROPERTIES VS_DEPLOYMENT_CONTENT 1 VS_DEPLOYMENT_LOCATION "Assets")

# Font, and Snes9x's license (it must travel with the app)
set(APP_RES
    "${CMAKE_CURRENT_SOURCE_DIR}/res/SplineSans-Bold.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/res/SplineSans-OFL.txt"
    "${APP_ROOT}/LICENSE")
target_sources(SNES PRIVATE ${APP_RES})
set_source_files_properties(${APP_RES} PROPERTIES VS_DEPLOYMENT_CONTENT 1 VS_DEPLOYMENT_LOCATION "res")

set(APP_CERT "${CMAKE_CURRENT_BINARY_DIR}/PS1_TemporaryKey.pfx")
configure_file("${CMAKE_ROOT}/Templates/Windows/Windows_TemporaryKey.pfx" "${APP_CERT}" COPYONLY)
target_sources(SNES PRIVATE "${APP_CERT}")

get_target_property(APP_SDL_DLL SDL3::SDL3-shared IMPORTED_LOCATION_RELEASE)
if (NOT APP_SDL_DLL)
    get_target_property(APP_SDL_DLL SDL3::SDL3-shared IMPORTED_LOCATION)
endif ()
if (NOT APP_SDL_DLL OR NOT EXISTS "${APP_SDL_DLL}")
    message(FATAL_ERROR "SDL3.dll not found; build SDL3-uwp first (Xbox/build.ps1)")
endif ()
target_sources(SNES PRIVATE "${APP_SDL_DLL}")
set_source_files_properties("${APP_SDL_DLL}" PROPERTIES VS_DEPLOYMENT_CONTENT 1)

set_target_properties(SNES PROPERTIES VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION "10.0.17763.0")
