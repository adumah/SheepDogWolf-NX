# SheepD3D: Sheep, Dog 'n' Wolf on SDL3, 64-bit.
#
# Windows: Visual Studio's MSVC, x64 (in CLion: Toolchains > Visual Studio > Architecture: amd64).
# macOS:   Xcode's Apple Clang, arm64.
# Linux:   Clang (GCC is not tested), x86_64 or arm64, and SDL3's build dependencies (BUILDING.md).
# All need Python 3 and your copy of the game in "Sheep, Dog 'n' Wolf (PAL Version)/" at the repository root.

cmake_minimum_required(VERSION 3.20)

project(SheepD3D C CXX)

# Nintendo Switch: devkitPro's toolchain (aarch64-none-elf-cmake) builds the game as a homebrew .nro, on its own
# (switch/switch.cmake)
if(NINTENDO_SWITCH)
    include(switch/switch.cmake)
    return()
endif()

# macOS: the launcher's update check uses NSURLSession, from Objective-C++ (src/launcher/http_get_mac.mm)
if(APPLE)
    enable_language(OBJCXX)
endif()

# explicit, so every compiler builds the same language (older Apple Clang defaults to C++98, which Dear ImGui rejects)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS ON)

# ---------------------------------------------------------------------------
# Platform validation
# ---------------------------------------------------------------------------

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SheepD3D builds for 64-bit targets only.")
endif()

if(WIN32)

    if(NOT MSVC AND NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR
                "Windows builds of SheepD3D require Microsoft Visual C++ (MSVC).")
    endif()

    message(STATUS "SheepD3D: Windows/MSVC")

elseif(APPLE)

    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(FATAL_ERROR "SheepD3D: unsupported Apple platform")
    endif()

    message(STATUS "SheepD3D: macOS/Apple Clang")

    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING
            "macOS architecture" FORCE)

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(WARNING "SheepD3D is tested with Clang; ${CMAKE_CXX_COMPILER_ID} may reject some of the game code. "
                "Configure with -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++.")
    endif()

    message(STATUS "SheepD3D: Linux/${CMAKE_CXX_COMPILER_ID}")

else()

    message(FATAL_ERROR
            "SheepD3D supports Windows/MSVC, macOS and Linux only.")

endif()

# ---------------------------------------------------------------------------
# Platform-specific compiler configuration
# ---------------------------------------------------------------------------

if(MSVC)

    set(CMAKE_MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

    foreach(lang C CXX)
        string(REPLACE "/RTC1" "/RTCs"
                CMAKE_${lang}_FLAGS_DEBUG
                "${CMAKE_${lang}_FLAGS_DEBUG}")
    endforeach()

else()

    add_compile_options(
            $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
            $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    )

endif()

# ---------------------------------------------------------------------------
# Game
# ---------------------------------------------------------------------------

set(GAME_DIR "${CMAKE_SOURCE_DIR}/Sheep, Dog 'n' Wolf (PAL Version)")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The level objects' property layouts: src/include/scenaric_props.h, generated from the disc's Scenaric_Classes.h when
# it is not in the tree (the build needs nothing else from the game: its data is read at run time)
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/include/scenaric_props.h")
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
# Windows resources (the game's icons, dialogs and strings, from your SheepD3D.exe)
# ---------------------------------------------------------------------------

# Without the game's exe (a CI runner) the build goes on without them: the window then has SDL's default icon.
if(WIN32 AND EXISTS "${GAME_DIR}/SheepD3D.exe")

    set(GAME_RES "${CMAKE_BINARY_DIR}/SheepD3D.res")

    add_custom_command(
            OUTPUT ${GAME_RES}
            COMMAND ${Python3_EXECUTABLE} tools/exe_res.py ${GAME_RES}
            DEPENDS
            "${GAME_DIR}/SheepD3D.exe"
            tools/exe_res.py
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Extracting resources from SheepD3D.exe"
    )

    set_source_files_properties(
            ${GAME_RES}
            PROPERTIES
            EXTERNAL_OBJECT TRUE
            GENERATED TRUE
    )

elseif(WIN32)

    message(STATUS "SheepD3D: no ${GAME_DIR}/SheepD3D.exe: building without its icon and resources")

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

# ---------------------------------------------------------------------------
# IJG libjpeg 6
# ---------------------------------------------------------------------------

add_library(ijg STATIC ${JPEG_SRC})

target_compile_definitions(ijg PRIVATE
        _CRT_SECURE_NO_WARNINGS
        )

if(MSVC)
    target_compile_options(ijg PRIVATE /W0)
else()
    target_compile_options(ijg PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# CRT compatibility
# ---------------------------------------------------------------------------

if(WIN32)
    add_library(crt_compat STATIC
            src/compat/crt_compat.c
            )
endif()

# ---------------------------------------------------------------------------
# Renderer
# ---------------------------------------------------------------------------

set(RENDER_SRC
        src/render/render_select.cpp
        src/render/perf_overlay.cpp
        )

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

if(WIN32)

    add_executable(SheepD3D WIN32
            ${GAME_SRC}
            src/compat/pure_slots.cpp
            ${RENDER_SRC}
            ${GAME_RES}
            src/platform/save_store.c
            src/platform/options.c
            )

else()

    add_executable(SheepD3D
            ${GAME_SRC}
            src/compat/pure_slots.cpp
            ${RENDER_SRC}
            src/platform/save_store.c
            src/platform/options.c
            )

endif()

target_include_directories(SheepD3D PRIVATE
        src/include
        )

# The app icon, res/icon.png (a square PNG, 1024 x 1024 at best; never required): embedded in the game and the launcher,
# which set it as their window icon (src/platform/app_icon.h). A new icon.png is picked up at the next build.
set(SDW_ICON_PNG "${CMAKE_SOURCE_DIR}/res/icon.png")
set(SDW_ICON_HEADER_DIR "${CMAKE_BINARY_DIR}/generated")
if(EXISTS "${SDW_ICON_PNG}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SDW_ICON_PNG}")
    file(READ "${SDW_ICON_PNG}" SDW_ICON_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," SDW_ICON_BYTES "${SDW_ICON_HEX}")
    file(WRITE "${SDW_ICON_HEADER_DIR}/sdw_icon_png.h.tmp"
            "/* generated by CMakeLists.txt from res/icon.png */\nstatic const unsigned char g_sdwIconPng[] = {${SDW_ICON_BYTES}};\n")
    # rewritten only when the icon changed, so a reconfigure does not rebuild what includes it
    configure_file("${SDW_ICON_HEADER_DIR}/sdw_icon_png.h.tmp" "${SDW_ICON_HEADER_DIR}/sdw_icon_png.h" COPYONLY)
    set(SDW_HAVE_APP_ICON ON)
else()
    message(STATUS "SheepD3D: no res/icon.png: the windows have the default icon")
    set(SDW_HAVE_APP_ICON OFF)
endif()
if(SDW_HAVE_APP_ICON)
    target_include_directories(SheepD3D PRIVATE "${SDW_ICON_HEADER_DIR}")
    target_compile_definitions(SheepD3D PRIVATE SDW_HAVE_APP_ICON)
endif()

# ---------------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------------

# Optimised builds keep the game code out of the inliner: a few helpers are inline in the files that define them and
# only declared in others, so once the optimiser inlines every use no out-of-line copy is left to link against.
if(MSVC)
    target_compile_options(SheepD3D PRIVATE $<$<NOT:$<CONFIG:Debug>>:/Ob0>)
else()
    target_compile_options(SheepD3D PRIVATE $<$<NOT:$<CONFIG:Debug>>:-fno-inline>)
endif()

if(MSVC)

    target_compile_options(SheepD3D PRIVATE
            /Zc:wchar_t-
            /permissive
            /EHs-c- /GR-
            /W0 /MP
            /we4311 /we4312 /we4302
            )

else()

    # -fsigned-char: the game code expects a signed char, which ARM Linux does not default to (x86 and Apple do)
    target_compile_options(SheepD3D PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
            $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
            -fsigned-char
            -ffunction-sections
            -fdata-sections
            -w
            $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
            )

    if(APPLE)
        target_link_options(SheepD3D PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(SheepD3D PRIVATE -Wl,--gc-sections)
    endif()

endif()

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------

if(WIN32)

    target_link_libraries(SheepD3D PRIVATE
            ijg
            crt_compat
            kernel32
            user32
            gdi32
            advapi32
            shell32
            ole32
            uuid
            dxguid
            strmiids
            )

else()

    target_link_libraries(SheepD3D PRIVATE
            ijg
            $<$<PLATFORM_ID:Linux>:m>
            )

endif()

# ---------------------------------------------------------------------------
# SDL3
# ---------------------------------------------------------------------------

if(WIN32)

    # Some Windows SDKs ship a winnt.h whose PVOID64 line does not compile; SDL3 is then built with a corrected copy.
    string(REPLACE "\\" "/" sdw_include_dirs "$ENV{INCLUDE}")

    foreach(dir IN LISTS sdw_include_dirs)

        if(EXISTS "${dir}/winnt.h")

            file(READ "${dir}/winnt.h" sdw_winnt)

            string(FIND
                    "${sdw_winnt}"
                    "typedef void * POINTER_64, * PVOID64;"
                    sdw_at
                    )

            if(NOT sdw_at EQUAL -1)

                file(MAKE_DIRECTORY
                        "${CMAKE_BINARY_DIR}/winnt_fix")

                string(REPLACE
                        "typedef void * POINTER_64, * PVOID64;"
                        "typedef void * POINTER_64 PVOID64;"
                        sdw_winnt
                        "${sdw_winnt}"
                        )

                file(WRITE
                        "${CMAKE_BINARY_DIR}/winnt_fix/winnt.h"
                        "${sdw_winnt}"
                        )

                include_directories(BEFORE
                        "${CMAKE_BINARY_DIR}/winnt_fix"
                        )

                list(APPEND CMAKE_REQUIRED_INCLUDES
                        "${CMAKE_BINARY_DIR}/winnt_fix"
                        )

                message(WARNING
                        "${dir}/winnt.h has an edited PVOID64 line that does "
                        "not compile; SDL3 is built with a corrected copy. "
                        "Repairing the Windows SDK (Visual Studio Installer) "
                        "fixes it."
                        )

            endif()

            break()

        endif()

    endforeach()

endif()

include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
        URL
        https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
        URL_HASH
        SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
        )

FetchContent_MakeAvailable(SDL3)

# ---------------------------------------------------------------------------
# SDL3 platform/renderer sources
# ---------------------------------------------------------------------------

target_sources(SheepD3D PRIVATE
        src/platform/platform_sdl.cpp
        src/platform/platform_audio_sdl.cpp
        src/platform/dsound_sdl.cpp
        src/platform/wav_sdl.cpp
        src/render/sdl3/sdl3_render_device.cpp
        )

# the Windows and C runtime names the game code calls, where Windows does not provide them (the .wav files are read by
# SDL3 on every platform: src/platform/wav_sdl.cpp)
if(NOT WIN32)
    target_sources(SheepD3D PRIVATE
            src/platform/win32_sdl_compat.cpp
            src/platform/crt_posix_compat.cpp
            )
endif()

target_compile_definitions(SheepD3D PRIVATE
        SDW_HAVE_SDL3
        )

# the version the game shows (the performance overlay's first line): the build workflow passes the release's tag, a
# pull request's or a commit's name (.github/workflows/build.yml); a local build is "dev"
set(SDW_VERSION "dev" CACHE STRING "the version the game shows")
target_compile_definitions(SheepD3D PRIVATE SDW_VERSION="${SDW_VERSION}")

target_link_libraries(SheepD3D PRIVATE
        SDL3::SDL3-static
        )

# ---------------------------------------------------------------------------
# The launcher: picks the renderer, resolution and controller, then starts the game with them as flags
# ---------------------------------------------------------------------------

FetchContent_Declare(imgui
        URL
        https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.tar.gz
        URL_HASH
        SHA256=3872a5f90df78fced023c1945f4466b654fd74573370b77b17742149763a7a7c
        )

FetchContent_MakeAvailable(imgui)

if(WIN32)
    set(SDW_LAUNCHER_KIND WIN32)
endif()

add_executable(SheepLauncher ${SDW_LAUNCHER_KIND}
        src/launcher/launcher.cpp
        src/launcher/update_check.cpp
        src/launcher/screen_setup.cpp
        src/launcher/screen_main.cpp
        src/launcher/popup_common.cpp
        src/launcher/extract_job.cpp
        src/launcher/xa_extract.cpp
        src/launcher/popup_install.cpp
        src/launcher/popup_video.cpp
        src/launcher/popup_credits.cpp
        src/launcher/popup_advanced.cpp
        src/launcher/popup_langexport.cpp
        src/launcher/langexport.cpp         # the Language Export page's work
        src/launcher/langexport_psxdav.cpp
        src/launcher/langexport_tables.cpp
        src/platform/options.c
        src/platform/save_store.c
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
        )

# the launcher's title shows the version too (SDW_VERSION, set above); the update check asks this GitHub repository
# (owner/name: the build workflow passes its own) for a newer release, and does not check when it is empty
set(SDW_REPOSITORY "" CACHE STRING "the GitHub repository (owner/name) whose releases the launcher checks")
target_compile_definitions(SheepLauncher PRIVATE SDW_VERSION="${SDW_VERSION}" SDW_REPOSITORY="${SDW_REPOSITORY}")

target_include_directories(SheepLauncher PRIVATE
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        )

target_link_libraries(SheepLauncher PRIVATE
        SDL3::SDL3-static
        )

# the update check's one HTTPS request, with the system's own networking (src/launcher/http_get.h)
if(WIN32)
    target_sources(SheepLauncher PRIVATE src/launcher/http_get_win.cpp)
    target_link_libraries(SheepLauncher PRIVATE winhttp)
elseif(APPLE)
    target_sources(SheepLauncher PRIVATE src/launcher/http_get_mac.mm) # Objective-C++: enabled after project() above
    set_source_files_properties(src/launcher/http_get_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(SheepLauncher PRIVATE "-framework Foundation")
else()
    target_sources(SheepLauncher PRIVATE src/launcher/http_get_posix.cpp) # the system's libcurl, if any (dlopen)
    target_link_libraries(SheepLauncher PRIVATE ${CMAKE_DL_LIBS})
endif()

# the disc extraction reports its errors as C++ exceptions (and std::filesystem throws), unlike the rest of the build
if(MSVC)
    set_source_files_properties(src/launcher/xa_extract.cpp PROPERTIES COMPILE_OPTIONS "/EHsc")
else()
    set_source_files_properties(src/launcher/xa_extract.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
endif()

if(SDW_HAVE_APP_ICON)
    target_include_directories(SheepLauncher PRIVATE "${SDW_ICON_HEADER_DIR}")
    target_compile_definitions(SheepLauncher PRIVATE SDW_HAVE_APP_ICON)
endif()

# The launcher's banner (res/banner.png), font (Dear ImGui's Roboto Medium) and credits (res/credits.txt): embedded
# like the icon, as byte arrays in generated headers, so the launcher needs no files beside it
function(sdw_embed file name header)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
    file(READ "${file}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    file(WRITE "${SDW_ICON_HEADER_DIR}/${header}.tmp"
            "/* generated by CMakeLists.txt from ${file} */\nstatic const unsigned char ${name}[] = {${bytes}};\n")
    # rewritten only when the file changed, so a reconfigure does not rebuild what includes it
    configure_file("${SDW_ICON_HEADER_DIR}/${header}.tmp" "${SDW_ICON_HEADER_DIR}/${header}" COPYONLY)
endfunction()
sdw_embed("${CMAKE_SOURCE_DIR}/res/banner.png" g_sdwBannerPng sdw_banner_png.h)
sdw_embed("${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf" g_sdwLauncherFont sdw_launcher_font.h)
sdw_embed("${CMAKE_SOURCE_DIR}/res/credits.txt" g_sdwCreditsTxt sdw_credits_txt.h)
target_include_directories(SheepLauncher PRIVATE "${SDW_ICON_HEADER_DIR}")

# the launcher starts the game from its own folder
add_dependencies(SheepLauncher SheepD3D)

# macOS: the launcher is built as the app bundle Rewooled.app, so that the Dock and the menu bar show the app's name and
# icon instead of the executable's file name. The game is copied into the bundle next to the launcher
# (Contents/MacOS/SheepD3D); the game's data goes next to the .app (Platform_GetBasePath treats the bundle's folder
# as the base). The icon is res/icon.png (a square PNG, 1024 x 1024 at best) when it is there.
if(APPLE)
    set(SDW_ICON_FILE "")
    if(EXISTS "${SDW_ICON_PNG}")
        set(SDW_ICONSET "${CMAKE_BINARY_DIR}/Rewooled.iconset")
        set(SDW_ICNS "${CMAKE_BINARY_DIR}/Rewooled.icns")
        set(SDW_ICON_COMMANDS)
        foreach(size 16 32 128 256 512)
            math(EXPR size2 "${size} * 2")
            list(APPEND SDW_ICON_COMMANDS
                    COMMAND sips -z ${size} ${size} "${SDW_ICON_PNG}" --out "${SDW_ICONSET}/icon_${size}x${size}.png"
                    COMMAND sips -z ${size2} ${size2} "${SDW_ICON_PNG}" --out "${SDW_ICONSET}/icon_${size}x${size}@2x.png")
        endforeach()
        add_custom_command(
                OUTPUT "${SDW_ICNS}"
                COMMAND ${CMAKE_COMMAND} -E rm -rf "${SDW_ICONSET}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SDW_ICONSET}"
                ${SDW_ICON_COMMANDS}
                COMMAND iconutil -c icns "${SDW_ICONSET}" -o "${SDW_ICNS}"
                DEPENDS "${SDW_ICON_PNG}"
                COMMENT "Making the app icon from res/icon.png"
                VERBATIM
        )
        target_sources(SheepLauncher PRIVATE "${SDW_ICNS}")
        set_source_files_properties("${SDW_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set(SDW_ICON_FILE "Rewooled.icns")
    else()
        message(STATUS "SheepLauncher: no res/icon.png: the app bundle has the default icon")
    endif()

    # the name the Dock, Finder and the menu bar show. The bundle's file stays Rewooled.app (a file name cannot hold
    # the ':'); Finder and the Dock show a different name only as a localized one, hence the en.lproj strings file.
    set(SDW_APP_NAME "Sheep, Dog 'n' Wolf: Rewooled")
    set(SDW_APP_STRINGS "${CMAKE_BINARY_DIR}/InfoPlist.strings")
    file(WRITE "${SDW_APP_STRINGS}"
            "CFBundleDisplayName = \"${SDW_APP_NAME}\";\nCFBundleName = \"${SDW_APP_NAME}\";\n")
    target_sources(SheepLauncher PRIVATE "${SDW_APP_STRINGS}")
    set_source_files_properties("${SDW_APP_STRINGS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources/en.lproj)

    set_target_properties(SheepLauncher PROPERTIES
            OSX_ARCHITECTURES arm64
            MACOSX_BUNDLE ON
            OUTPUT_NAME Rewooled
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/res/macos/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${SDW_APP_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER io.github.lace22l.rewooled
            MACOSX_BUNDLE_ICON_FILE "${SDW_ICON_FILE}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING 1.0
            MACOSX_BUNDLE_BUNDLE_VERSION 1.0
            LINK_DEPENDS "$<TARGET_FILE:SheepD3D>"  # relinked, so copied again, whenever the game changes
            )
    add_custom_command(TARGET SheepLauncher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:SheepD3D>" "$<TARGET_BUNDLE_CONTENT_DIR:SheepLauncher>/MacOS/SheepD3D"
            # a newer date on the bundle makes Finder and the Dock read its icon and name again
            COMMAND ${CMAKE_COMMAND} -E touch_nocreate "$<TARGET_BUNDLE_DIR:SheepLauncher>"
            COMMENT "Copying the game into Rewooled.app"
            VERBATIM
    )
endif()

# ---------------------------------------------------------------------------
# psxdav_convert: the game's PlayStation .DAV conversion (src/engine/psx_dav.cpp) as a command, for tools/retexture.py
# ---------------------------------------------------------------------------

add_executable(psxdav_convert
        tools/psxdav_convert.cpp
        src/engine/psx_dav.cpp
        )

target_include_directories(psxdav_convert PRIVATE
        src/include
        )

# ---------------------------------------------------------------------------
# macOS
# ---------------------------------------------------------------------------

if(APPLE)

    set_target_properties(SheepD3D PROPERTIES
            OSX_ARCHITECTURES arm64
            MACOSX_BUNDLE OFF
            )

endif()
