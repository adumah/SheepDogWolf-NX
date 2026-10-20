# The Nintendo Switch build: SheepD3D as a homebrew .nro, with devkitPro's devkitA64, libnx, SDL2 and Mesa (OpenGL ES 3
# through EGL). Included by CMakeLists.txt when the toolchain is devkitPro's Switch one:
#
#   aarch64-none-elf-cmake -S . -B build-switch -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-switch -j$(nproc)
#
# (or switch/build.sh). Needs the devkitPro packages switch-dev, switch-sdl2, switch-mesa, switch-libdrm_nouveau,
# switch-libpng and switch-zlib. The output is build-switch/SheepD3D.nro; see switch/README.md for the SD card.
#
# What differs from the desktop build: no launcher (the options come from environment.txt and the in-game pause menu),
# the OpenGL ES renderer (src/render/gl/) instead of SDL3's GPU API, and the platform layer in src/platform/switch/.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

message(STATUS "SheepD3D: Nintendo Switch/${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# ---------------------------------------------------------------------------
# The level objects' property layouts (as CMakeLists.txt: generated from the game's data when missing)
# ---------------------------------------------------------------------------

set(GAME_DIR "${CMAKE_SOURCE_DIR}/Sheep, Dog 'n' Wolf (PAL Version)")

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/include/scenaric_props.h")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(NOT EXISTS "${GAME_DIR}/Levels/Lvl-03/Scenaric_Classes.h")
        message(FATAL_ERROR
                "src/include/scenaric_props.h is missing: copy the game's PAL disc to \"${GAME_DIR}/\" "
                "to generate it (BUILDING.md section 1)")
    endif()
    execute_process(
            COMMAND ${Python3_EXECUTABLE} tools/scenaric_to_c.py
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE scn_result
    )
    if(scn_result)
        message(FATAL_ERROR "tools/scenaric_to_c.py failed")
    endif()
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

file(GLOB GAME_SRC CONFIGURE_DEPENDS
        src/app/*.cpp
        src/engine/*.cpp
        src/fx/*.cpp
        src/game/*.cpp
        src/objects/*.cpp
        )

file(GLOB JPEG_SRC CONFIGURE_DEPENDS
        src/jpeg/*.c
        )

add_library(ijg STATIC ${JPEG_SRC})
target_compile_options(ijg PRIVATE -w -O2)

add_executable(SheepD3D
        ${GAME_SRC}
        src/compat/pure_slots.cpp
        src/render/render_select.cpp
        src/render/perf_overlay.cpp
        src/render/gl/gl_render_device.cpp
        src/platform/save_store.c
        src/platform/options.c
        src/platform/crt_posix_compat.cpp
        src/platform/dsound_sdl.cpp           # DirectSound over Platform_Voice* (no SDL calls of its own)
        src/platform/switch/platform_switch.cpp
        src/platform/switch/platform_audio_switch.cpp
        src/platform/switch/wav_switch.cpp
        src/platform/switch/win32_switch_compat.cpp
        )

target_include_directories(SheepD3D PRIVATE
        src/include
        "${DEVKITPRO}/portlibs/switch/include"
        )

set(SDW_VERSION "dev" CACHE STRING "the version the game shows")
target_compile_definitions(SheepD3D PRIVATE
        SDW_RENDER_GL
        SDW_VERSION="${SDW_VERSION}"
        )

# ---------------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------------

# The desktop build keeps optimised game code out of the inliner (-fno-inline): a few helpers are inline in the files
# that define them and only declared in others. That costs a lot on the Switch's Cortex-A57, so this build inlines and
# has GCC keep an out-of-line copy of every inline function as well (-fkeep-inline-functions), which the other files
# link against. SDW_SWITCH_NO_INLINE=ON goes back to the desktop's way.
option(SDW_SWITCH_NO_INLINE "build the game code with -fno-inline, as the desktop build" OFF)
if(SDW_SWITCH_NO_INLINE)
    set(SDW_INLINE_FLAG -fno-inline)
else()
    set(SDW_INLINE_FLAG $<$<COMPILE_LANGUAGE:CXX>:-fkeep-inline-functions>)
endif()

# -fsigned-char: the game code expects a signed char, which AArch64 does not default to
# -fno-strict-aliasing: decompiled code reads the same memory through several types
# -fno-omit-frame-pointer: the watchdog's call chains in the log (platform_switch.cpp)
target_compile_options(SheepD3D PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
        $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
        -fsigned-char
        -fno-strict-aliasing
        -fno-omit-frame-pointer
        -ffunction-sections
        -fdata-sections
        -w
        $<$<CONFIG:Release>:-O2>
        $<$<NOT:$<CONFIG:Debug>>:${SDW_INLINE_FLAG}>
        )

target_link_options(SheepD3D PRIVATE -Wl,--gc-sections)

# ---------------------------------------------------------------------------
# Libraries: SDL2 (audio, threads, the log), Mesa's OpenGL ES and EGL over nouveau, libpng, libnx
# ---------------------------------------------------------------------------

target_link_libraries(SheepD3D PRIVATE
        ijg
        SDL2
        png16
        z
        GLESv2
        EGL
        glapi
        drm_nouveau
        nx
        m
        stdc++
        pthread
        )

# ---------------------------------------------------------------------------
# The .nro: the Homebrew Menu's name, author, version and icon (res/switch/icon.jpg, 256 x 256)
# ---------------------------------------------------------------------------

nx_generate_nacp(SheepD3D.nacp
        NAME "Sheep, Dog 'n' Wolf: Rewooled"
        AUTHOR "SDW Community"
        VERSION "${SDW_VERSION}"
        )

nx_create_nro(SheepD3D
        ICON "${CMAKE_SOURCE_DIR}/res/switch/icon.jpg"
        NACP SheepD3D.nacp
        )
