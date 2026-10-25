cmake_minimum_required(VERSION 3.22)
# A library a target reaches twice - the preset files, which several libraries link - was repeated on the link line, and
# Apple's linker warns of every repeat. From CMake 3.29 the link line keeps a repeat only where the linker needs it: GNU
# ld, which reads its archives once, keeps it; ld64 and link.exe, which resolve across every archive, do not.
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()
# URL archives take the extraction time rather than their contents' historical timestamps, so a changed dependency
# is newer than targets made from the one before it. CMake 3.24 made this CMP0135's NEW behaviour.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# The Reckless Technology toolkit: what the company's products share, written against no one product - JSON, text
# and the user's files (core/), the drawing layer (gfx/) and the series' look on it (look/), the
# views a panel is made of (view/) and the series' panel on them (panel/), the child window a panel is shown in on Skia
# (window/, skia/), an app's platform layer (app/), the audio device layer (audio/), the network (net/), the update
# check (update/) and the licence (licence/), with the artwork they draw (assets/), the suites that hold them (tests/)
# and the CMake helpers a product builds its binaries with (cmake/). Every directory is an include directory of its own
# and a header is included by its bare name ("View.h").
#
# Added by a product with add_subdirectory(<this directory>), or configured on its own, when it builds its own suites:
#
#   cmake -S external/reckless-toolkit -B build/toolkit -G Ninja -DCMAKE_BUILD_TYPE=Release
#
# Nothing here reaches outside this directory: a product depends on the toolkit, never the reverse.

# Configured on its own, the toolkit is the top-level project and sets what the product's root sets otherwise: the
# oldest macOS the Mac binaries run on - 13.3, the first whose libc++ has the floating-point std::to_chars core/ writes
# numbers with (ParameterText.h, Json.h) - set before project() because the compilers' flags are made from it there, an
# empty value counting as unset.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "The oldest macOS the Mac binaries run on" FORCE)
endif()

project(RecklessToolkit LANGUAGES C CXX)

set(RECKLESS_TOOLKIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "The toolkit's source directory")

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(MSVC)
        # The static runtime, as every binary built on the toolkit links it, so a plug-in loads in any host.
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            add_compile_options(/MP)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------------------------------------------
# Options. A product that adds the toolkit sets the toolkit's own as normal variables before it does (CMP0077 lets a
# normal variable stand in for an option), so they follow the product's.
# ---------------------------------------------------------------------------------------------------------------
# The toolkit's suites (and GoogleTest, which a product's suites use too).
option(RECKLESS_TOOLKIT_BUILD_TESTS "Build the toolkit's suites" ON)
# The windowed parts: Skia (fetched, prebuilt), the canvas on it, the child window a panel is shown in and an app's
# platform layer. Off, nothing of Skia is fetched: the drawing layer, the look, the views and the panel are header-only
# or plain C++ and build, with their suites, without it.
option(RECKLESS_TOOLKIT_GUI "Build the windowed parts: Skia, its canvas, the child window and the app platform" ON)
# The audio device layer (audio/), a back end of the toolkit's own for every API, with its suites.
option(RECKLESS_BUILD_AUDIO "Build the audio device layer and its suites" ON)
# Windows only: the audio device layer offers ASIO devices, through the toolkit's ASIO back end on Steinberg's ASIO SDK. The
# SDK is dual-licensed, Steinberg's proprietary ASIO licence or GPLv3, and a closed product takes the proprietary one,
# whose terms the build keeps: the SDK is fetched from Steinberg's own server at a pinned version with a hash, below,
# and never committed or shipped (the licence forbids distributing it in part or whole), and Steinberg's usage
# guidelines are kept where ASIO shows - the ASIO-compatible logo and the trademark line beside an ASIO device's choice
# (app/SettingsWidgets.h's AsioNoticeView, look/LookTrademarks.h). RECKLESS_ASIO off builds without it;
# RECKLESS_ASIO_SDK_DIR names an unzipped SDK (the folder holding common/iasiodrv.h) to build on instead of the fetch.
option(RECKLESS_ASIO "Windows: the audio device layer offers ASIO devices, on Steinberg's ASIO SDK (fetched)" ON)
set(RECKLESS_ASIO_SDK_DIR "" CACHE PATH "Windows: an unzipped Steinberg ASIO SDK to build on instead of fetching one")
# Fetch the ASIO SDK wherever ASIO is on, even where nothing compiles it: a product's third-party notices quote the
# licence of the SDK's host files, which its notices script reads from the fetched tree on any platform.
option(RECKLESS_TOOLKIT_FETCH_ASIO_SDK "Fetch the ASIO SDK on every platform, for a product's notices" OFF)
# Demo mode: a product gates an unlicensed binary (the switch, licence/DemoMode.h, which the licence's targets carry as
# a compile definition so every translation unit that reads it agrees). Off until a product is sold.
option(RECKLESS_DEMO_MODE "Gate an unlicensed product: the build's demo switch (licence/DemoMode.h)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ToolkitPins)
include(RecklessEmbed)  # reckless_embed, reckless_embedded_bytes, reckless_module_assets: files inside a binary
include(RecklessBundle) # reckless_bundle_icon and the rest: what a bundle carries beside its module

# The strict warning set on every compiler, for every target the toolkit (and a product) owns. Deliberately not errors:
# a new compiler release must not break the build. The code is kept warning-free on gcc, clang and MSVC. The flags
# follow the compiler's command line, not its make: MSVC is true for cl and for clang-cl, which takes cl's spelling and
# reads -Wall as clang's -Weverything, and false for clang++ on Windows, which takes the GNU spelling. A product that
# defines the target first keeps its own.
if(NOT TARGET reckless_warnings)
    add_library(reckless_warnings INTERFACE)
    if(MSVC)
        target_compile_options(reckless_warnings INTERFACE /W4 /permissive-)
    else()
        target_compile_options(reckless_warnings INTERFACE -Wall -Wextra -Wpedantic)
    endif()
endif()

include(FetchContent)

if(RECKLESS_TOOLKIT_BUILD_TESTS)
    if(PROJECT_IS_TOP_LEVEL)
        enable_testing()
    endif()
    # GoogleTest for the gtest suites: gtest alone, nothing here mocks, and gmock is four more libraries to compile.
    if(NOT TARGET GTest::gtest)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        ${RECKLESS_GOOGLETEST_TAG}
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(googletest)
        # googletest compiles its own two libraries under its own strict set (-Wall -Wshadow -Wconversion on clang),
        # where clang 21's -Wcharacter-conversion meets its char8_t printer in gtest-printers.h. That is googletest's
        # to fix, so those two libraries alone let it pass; the suites see its headers as system headers already.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            include(CheckCXXCompilerFlag)
            check_cxx_compiler_flag(-Wcharacter-conversion RECKLESS_CLANG_KNOWS_character_conversion)
            if(RECKLESS_CLANG_KNOWS_character_conversion)
                target_compile_options(gtest PRIVATE -Wno-character-conversion)
                target_compile_options(gtest_main PRIVATE -Wno-character-conversion)
            endif()
        endif()
    endif()
endif()

# The ASIO SDK (the comment by RECKLESS_ASIO above says why it is fetched and what its licence asks): Steinberg's
# download server keeps every release at a versioned name, so the zip is pinned by that name and its hash, like every
# other fetch, and kept under FETCHCONTENT_BASE_DIR. Its single top-level folder is stripped, so the SDK's root, with
# common/iasiodrv.h, is the source dir. On Windows the audio layer compiles its ASIO back end on the SDK's headers and
# three host files (below).
if(RECKLESS_ASIO AND NOT RECKLESS_ASIO_SDK_DIR
   AND (RECKLESS_TOOLKIT_FETCH_ASIO_SDK OR (WIN32 AND RECKLESS_BUILD_AUDIO)))
    FetchContent_Declare(asiosdk
        URL      https://download.steinberg.net/sdk_downloads/ASIO-SDK_${RECKLESS_ASIO_SDK_VERSION}_${RECKLESS_ASIO_SDK_RELEASED}.zip
        URL_HASH SHA256=d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca
        SOURCE_SUBDIR reckless-builds-this-itself)
    FetchContent_MakeAvailable(asiosdk)
    # A normal variable over the empty cache entry, on Windows alone: the audio layer compiles ASIO where it is set,
    # and an app shows Steinberg's notice.
    if(WIN32)
        set(RECKLESS_ASIO_SDK_DIR "${asiosdk_SOURCE_DIR}")
    endif()
endif()
if(NOT RECKLESS_ASIO)
    set(RECKLESS_ASIO_SDK_DIR "")
endif()

# The audio files (core/AudioFiles.cpp, held by the core's suite to the formats' definitions and to the files
# tests/audio carries): the uncompressed formats it reads itself, and the three codecs on single-file decoders fetched
# here - dr_flac and dr_mp3 from one commit of dr_libs (public domain or MIT-0, whichever the user prefers), stb_vorbis
# from one commit of stb (public domain or MIT, likewise). Each a tarball with a hash, as every fetch is; neither
# repository has a build the toolkit wants, so each SOURCE_SUBDIR names a directory that does not exist and
# MakeAvailable fetches without adding anything (the three files compile, once, into reckless_audio_decoders, below).
# The dr_libs commit carries fixes after the last tags of both headers (dr_flac 0.13.3, dr_mp3 0.7.3): a decode error
# dr_flac went on past, and an out-of-bounds read in dr_mp3's Xing parse. stb_vorbis is 1.22, the commit that last
# touched it. A product finds the stb tree through FetchContent_GetProperties(stb) for stb_image.
FetchContent_Declare(dr_libs
    URL      https://github.com/mackron/dr_libs/archive/${RECKLESS_DR_LIBS_COMMIT}.tar.gz
    URL_HASH SHA256=4654acb029f4f2a43ac2edb60c4cb09f40615b4b5bee9709954f910cb979e5fd
    SOURCE_SUBDIR reckless-builds-this-itself)
FetchContent_Declare(stb
    URL      https://github.com/nothings/stb/archive/${RECKLESS_STB_COMMIT}.tar.gz
    URL_HASH SHA256=be9842d207353c89c0b970d618030fe98a944ab3fe1374534cba1b8e934bfdd7
    SOURCE_SUBDIR reckless-builds-this-itself)
FetchContent_MakeAvailable(dr_libs stb)

# A licence is checked by its Ed25519 signature (licence/): Monocypher, a small C library of modern cryptography under
# the licence the user prefers of BSD 2-clause and CC0, fetched as its release tag with a hash; only its RFC 8032
# Ed25519 (src/optional/monocypher-ed25519.c, SHA-512 inside) and the curve arithmetic under it in src/monocypher.c are
# compiled (reckless_monocypher, below). 4.0.3 is the release that fixed a timing leak in EdDSA signing.
FetchContent_Declare(monocypher
    URL      https://github.com/LoupVaillant/Monocypher/archive/refs/tags/${RECKLESS_MONOCYPHER_VERSION}.tar.gz
    URL_HASH SHA256=a7cbae546fbdc489bca632c3747e1ceb8ca3d4bd39e2706a0916f28ccd280e50
    SOURCE_SUBDIR reckless-builds-this-itself)
FetchContent_MakeAvailable(monocypher)

# The audio device layer (audio/) reaches the machine's audio interfaces and MIDI ports through the toolkit's own back
# ends alone, each on the platform's own API (docs/audio/native-backends.md):
#   macOS    CoreAudio on the HAL and CoreMIDI (audio/mac/);
#   Windows  Windows Audio (WASAPI), ASIO on Steinberg's SDK - fetched by name and hash, its host files compiled here as
#            reckless_asio_host - and WinMM's MIDI (audio/win/);
#   Linux    ALSA's PCMs and sequencer and a JACK client (audio/linux/), each where its development headers are
#            installed.
# JACK is linked at no point: libjack is loaded by name at run time (audio/linux/JackLoader.cpp), so a machine without it
# runs on ALSA rather than failing to start - every JACK function the toolkit calls is renamed to the loader's by
# audio/linux/JackNames.h. The ASIO SDK's host files compile without the toolkit's warning set, a fetched tree being no
# part of this one's to keep clean, and their headers are SYSTEM to the code that includes them. RtAudio and RtMidi, which
# the layer was first written on, are neither fetched nor built (docs/audio/native-backends.md).
if(RECKLESS_BUILD_AUDIO)
    find_package(Threads REQUIRED)
    # The APIs compiled in, for the configure's log and the suite that lists them.
    set(RECKLESS_AUDIO_APIS "")
    if(APPLE)
        # CoreAudio and CoreMIDI, the toolkit's own (audio/mac/).
        list(APPEND RECKLESS_AUDIO_APIS "CoreAudio (the toolkit's own)" "CoreMIDI (the toolkit's own)")
    elseif(WIN32)
        # Every API is the toolkit's own here: Windows Audio, ASIO and WinMM's MIDI (audio/win/).
        list(APPEND RECKLESS_AUDIO_APIS "Windows Audio (the toolkit's own: shared, exclusive)")
        if(RECKLESS_ASIO_SDK_DIR AND CMAKE_SIZEOF_VOID_P EQUAL 8)
            if(NOT EXISTS "${RECKLESS_ASIO_SDK_DIR}/host/pc/asiolist.cpp")
                message(FATAL_ERROR "RECKLESS_ASIO_SDK_DIR is set but '${RECKLESS_ASIO_SDK_DIR}/host/pc/asiolist.cpp' "
                                    "was not found. Point it at the unzipped Steinberg ASIO SDK root.")
            endif()
            # The SDK's host files, which every ASIO host compiles - the API on its one driver (asio.cpp), the loader
            # (asiodrivers.cpp) and the registry's list (asiolist.cpp) - into a library of their own, the SDK's code
            # being no part of this tree to keep clean, its headers SYSTEM to the back end that includes them.
            add_library(reckless_asio_host STATIC
                "${RECKLESS_ASIO_SDK_DIR}/common/asio.cpp"
                "${RECKLESS_ASIO_SDK_DIR}/host/asiodrivers.cpp"
                "${RECKLESS_ASIO_SDK_DIR}/host/pc/asiolist.cpp")
            target_include_directories(reckless_asio_host SYSTEM PUBLIC
                "${RECKLESS_ASIO_SDK_DIR}/common" "${RECKLESS_ASIO_SDK_DIR}/host" "${RECKLESS_ASIO_SDK_DIR}/host/pc")
            target_compile_definitions(reckless_asio_host PRIVATE _CRT_SECURE_NO_WARNINGS)
            target_link_libraries(reckless_asio_host PUBLIC ole32 advapi32 user32)
            set_target_properties(reckless_asio_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
            list(APPEND RECKLESS_AUDIO_APIS "ASIO (the toolkit's own, on Steinberg's SDK)")
        endif()
        list(APPEND RECKLESS_AUDIO_APIS "WinMM (the toolkit's own)")
    else()
        find_package(ALSA)
        if(ALSA_FOUND)
            # ALSA's audio and its sequencer, the toolkit's own (audio/linux/AlsaDevice.cpp and AlsaMidi.cpp, added to
            # reckless_audio_devices below).
            list(APPEND RECKLESS_AUDIO_APIS "ALSA (the toolkit's own)" "ALSA sequencer (the toolkit's own)")
        endif()
        find_path(RECKLESS_JACK_INCLUDE_DIR jack/jack.h)
        if(RECKLESS_JACK_INCLUDE_DIR)
            # Every JACK function the toolkit calls must be one the loader stands in for: a call JackNames.h lacks
            # would bind to libjack at link time - or, for one jack/jack.h declares weak, to nothing at all, a null
            # called the first time a stream opens. So the calls of each file that meets JACK are held to the list
            # here, every configure.
            set(jack_names_header "${CMAKE_CURRENT_SOURCE_DIR}/audio/linux/JackNames.h")
            file(READ "${jack_names_header}" jack_names)
            foreach(jack_source audio/linux/JackDevice.cpp tests/devices/JackRig.cpp)
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                             "${CMAKE_CURRENT_SOURCE_DIR}/${jack_source}")
                file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${jack_source}" jack_text)
                string(REGEX REPLACE "//[^\n]*" "" jack_text "${jack_text}")
                string(REPLACE "reckless_jack_" "" jack_text "${jack_text}") # the loader's own names, called as such
                string(REGEX MATCHALL "jack_[a-z_]+[ ]*\\(" jack_calls "${jack_text}")
                foreach(call IN LISTS jack_calls)
                    string(REGEX REPLACE "[ (]" "" call "${call}")
                    string(FIND "${jack_names}" "#define ${call} " named)
                    if(named EQUAL -1)
                        message(FATAL_ERROR "${jack_source} calls ${call}, which the JACK loader does not stand in "
                                            "for: add it to ${jack_names_header} and to audio/linux/JackLoader.cpp")
                    endif()
                endforeach()
            endforeach()
            # The loader is the toolkit's own code, so it carries the toolkit's warnings, in a library of its own that
            # the JACK back end (audio/linux/JackDevice.cpp, in reckless_audio_devices below) and the JACK rig link.
            add_library(reckless_jack_loader STATIC "${CMAKE_CURRENT_SOURCE_DIR}/audio/linux/JackLoader.cpp")
            target_include_directories(reckless_jack_loader PRIVATE "${RECKLESS_JACK_INCLUDE_DIR}")
            target_link_libraries(reckless_jack_loader PRIVATE reckless_warnings ${CMAKE_DL_LIBS})
            set_target_properties(reckless_jack_loader PROPERTIES POSITION_INDEPENDENT_CODE ON)
            list(APPEND RECKLESS_AUDIO_APIS "JACK (the toolkit's own, libjack loaded at run time)")
        endif()
    endif()
    set(reckless_audio_libraries "")
    if(RECKLESS_AUDIO_APIS)
        list(JOIN RECKLESS_AUDIO_APIS ", " apis)
        message(STATUS "Audio device layer: ${reckless_audio_libraries}the toolkit's own back ends, on ${apis}")
    else()
        message(STATUS "Audio device layer: ${reckless_audio_libraries}the toolkit's own back ends, no audio API's "
                       "headers found - no devices (libasound2-dev, and libjack-jackd2-dev)")
    endif()
endif()

# ---------------------------------------------------------------------------------------------------------------
# What the suites share: the deterministic noise source every suite drives its engine with (tests/TestNoise.h), the
# benches' support (tests/BenchSupport.h), a folder of the process's own for a
# suite's files (tests/ScratchPath.h), the tooltip rule (tests/TooltipRule.h), a counting allocator
# (tests/CountAllocations.h), a sanitizer's skip (tests/Sanitizers.h), audio files built a field at a time
# (tests/AudioTestFiles.h), HTTP on the loopback interface (tests/TestHttpServer.h), the device suites' harness
# (tests/devices/) and the audio fixtures (tests/audio/). Header only; a product's own test support links it.
# ---------------------------------------------------------------------------------------------------------------
add_library(reckless_toolkit_test_support INTERFACE)
target_include_directories(reckless_toolkit_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/tests")

# ---------------------------------------------------------------------------------------------------------------
# Core: namespace reckless::core, included by bare name - JSON (Json.h, JsonFile.h), UTF-8 (Utf8.h), numbers to and
# from text in any locale (DecimalText.h, ParameterText.h), the user's preset files and folders (PresetFiles.h), the
# interface's preferences (InterfaceSettings.h), audio files (AudioFiles.h), the denormal flush (FlushDenormals.h) and a
# meter held between threads (HeldMaximum.h). Header only but for the two libraries below.
# ---------------------------------------------------------------------------------------------------------------
add_library(reckless_common_core INTERFACE)
target_include_directories(reckless_common_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/core")
target_compile_features(reckless_common_core INTERFACE cxx_std_20)

# The preset files' platform part (core/PresetFiles.cpp, behind core/PresetFiles.h): where the user's documents and
# music are and where a per-user settings file lives (core/InterfaceSettings.h's folder), each found by the platform's
# own call - on macOS through sysdir, which is libSystem's; on Windows through the shell's known folders, which is why
# it links shell32; on Linux from the XDG user directories - and on Windows the system's words for an error, so the
# headers stay free of the platform's headers. A static library of plain C++ beside the header-only core, linked by
# what reads and writes those files.
add_library(reckless_core_preset_files STATIC core/PresetFiles.cpp)
target_link_libraries(reckless_core_preset_files PUBLIC reckless_common_core PRIVATE reckless_warnings)
# Compiled as a plug-in module's own code is, since a module links it: position-independent, its symbols hidden.
set_target_properties(reckless_core_preset_files PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
                                                            VISIBILITY_INLINES_HIDDEN ON)
if(WIN32)
    target_link_libraries(reckless_core_preset_files PUBLIC shell32)
endif()

# Audio files (core/AudioFiles.h): six formats decoded from a file's bytes, and a WAV writer. Compiled, not header only:
# the codecs are the vendored decoders fetched above, whose implementations compile once, as C, with their own warnings
# off, in reckless_audio_decoders (core/AudioDecoders.c); their configuration is defined on that target and handed to
# AudioFiles.cpp with their headers, as SYSTEM includes so the toolkit's warnings stay on the toolkit's code. Both are
# PIC with their symbols hidden, as the plug-in binaries they may go into are compiled.
add_library(reckless_audio_decoders STATIC core/AudioDecoders.c)
target_include_directories(reckless_audio_decoders SYSTEM PUBLIC "${dr_libs_SOURCE_DIR}" "${stb_SOURCE_DIR}")
target_compile_definitions(reckless_audio_decoders PUBLIC
    DR_FLAC_NO_STDIO
    DR_FLAC_NO_OGG
    DR_MP3_NO_STDIO
    DR_MP3_FLOAT_OUTPUT
    STB_VORBIS_NO_STDIO
    STB_VORBIS_NO_PUSHDATA_API)
target_compile_options(reckless_audio_decoders
    PRIVATE   $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>
    INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/external:W0>)
set_target_properties(reckless_audio_decoders PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

add_library(reckless_common_audio_files STATIC core/AudioFiles.cpp)
target_link_libraries(reckless_common_audio_files
    PUBLIC  reckless_common_core
    PRIVATE reckless_audio_decoders reckless_warnings)
set_target_properties(reckless_common_audio_files PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
                                                             VISIBILITY_INLINES_HIDDEN ON)

# ---------------------------------------------------------------------------------------------------------------
# Drawing: header only, namespace reckless::gfx, included by bare name - a panel's geometry (points, rectangles,
# transforms), colour, paths, fonts and text placement, images and the Canvas every panel draws on, written against no
# toolkit, so the panels' drawing outlives the renderer underneath it (gfx/skia's SkiaCanvas draws a Canvas through
# Skia), and the meters' artwork read from its SVG and drawn on any Canvas (SvgArt.h).
# ---------------------------------------------------------------------------------------------------------------
add_library(reckless_common_gfx INTERFACE)
target_include_directories(reckless_common_gfx INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/gfx")
target_compile_features(reckless_common_gfx INTERFACE cxx_std_20)
# The meters' artwork reads its numbers as the rest of the toolkit reads text, through core's DecimalText.h.
target_link_libraries(reckless_common_gfx INTERFACE reckless_common_core)

# ---------------------------------------------------------------------------------------------------------------
# The series' look: namespace reckless::look, included by bare name - the metrics, the palette, the chicken-head knob,
# the legend plates and buttons, the header with its mark and lamp, the indicator cells and the tubes in them, the
# frames round the knobs, the meters' scales, the oscilloscope and the tone stack's schematic, every one drawn on a
# gfx::Canvas, and the images the look caches made through an interface a renderer implements (look/LookImages.h;
# window/'s on Skia). A static library: built once, position-independent and with its symbols hidden, as the plug-in
# binaries it goes into are compiled.
# ---------------------------------------------------------------------------------------------------------------
add_library(reckless_common_look STATIC
    look/LookDrawing.cpp
    look/LookTubes.cpp
    look/LookMeters.cpp)
target_include_directories(reckless_common_look PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/look")
target_link_libraries(reckless_common_look PUBLIC reckless_common_gfx PRIVATE reckless_warnings)
set_target_properties(reckless_common_look PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# The panels' views on the drawing layer: header only, namespace reckless::view - the view tree, its painting and its
# mouse, a parameter as a control drives it, the label, the text field, the knob, the plate, the button, menus,
# dialogs, the presets a panel reads, a picture a screen keeps between paints, and the panel - which a window hosts
# (window/'s child window on Skia).
add_library(reckless_common_view INTERFACE)
target_include_directories(reckless_common_view INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/view")
target_link_libraries(reckless_common_view INTERFACE reckless_common_gfx)

# The series' panel on the views, drawn by the series' look: header only, namespace reckless::panel - the chrome every
# panel has (the face plate, the header, the PRESETS plate and its menu, the knob bank's knobs, plates and frames), the
# look's styles for the views, and the series' screens and meters as views (the
# oscilloscope, the indicator bay, the log frequency frame, the backlit meters, the level meter and bargraphs, the lamp,
# the seed button). A product's panel derives from it.
add_library(reckless_common_panel INTERFACE)
target_include_directories(reckless_common_panel INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/panel")
target_link_libraries(reckless_common_panel INTERFACE reckless_common_view reckless_common_look reckless_common_core)

# ---------------------------------------------------------------------------------------------------------------
# The network: namespace reckless::net, included by bare name - an HTTP exchange on the platform's own stack (Http.h:
# NSURLSession on macOS, WinHTTP on Windows, libcurl loaded by name on Linux, each with the system's TLS, certificates
# and proxies, so the toolkit ships no TLS of its own; no redirect followed by the platform, Fetch following them by the
# toolkit's rule), URLs and forms (Url.h), SHA-256, base64url and the system's random bytes (Sha256.h) and a one-shot
# listener on the loopback interface for a sign-in's redirect (LoopbackListener.h). A static library, compiled as a
# plug-in module's code is. On macOS the session's delegate class is registered at run time under a name of the image's
# own, as the window's view class is, so two modules carrying it in one process never meet over one name.
# ---------------------------------------------------------------------------------------------------------------
if(APPLE)
    set(reckless_net_platform net/mac/TransportMac.mm)
elseif(WIN32)
    set(reckless_net_platform net/win/TransportWin.cpp)
else()
    set(reckless_net_platform net/linux/TransportLinux.cpp)
endif()
add_library(reckless_common_net STATIC net/LoopbackListener.cpp ${reckless_net_platform})
target_include_directories(reckless_common_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/net")
target_link_libraries(reckless_common_net PUBLIC reckless_common_core PRIVATE reckless_warnings)
set_target_properties(reckless_common_net PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
                                                     VISIBILITY_INLINES_HIDDEN ON)
if(APPLE)
    set_source_files_properties(net/mac/TransportMac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(reckless_common_net PUBLIC "-framework Foundation")
elseif(WIN32)
    # WinHTTP for the exchange, Winsock for the loopback listener, the system's generator for random bytes: every one a
    # part of every Windows the toolkit runs on.
    target_link_libraries(reckless_common_net PUBLIC winhttp ws2_32 bcrypt)
else()
    # dlopen for libcurl, a part of glibc from 2.34, libdl's before.
    target_link_libraries(reckless_common_net PRIVATE ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------------------------------------------
# The update check: namespace reckless::update, included by bare name - whether a newer release is out, read from a
# small static manifest on the company's website once a day per machine and only while a window shows, its answer and
# the person's "Check automatically" kept in a file under the user's settings folder, whose platform part is
# reckless_core_preset_files (UpdateCheck.h); a product's description and the one factory that makes its checker
# (UpdateProduct.h); and the Updates menu every product offers, as the views' menu rows (UpdatesMenu.h). Header only,
# on the network and the views' headers.
# ---------------------------------------------------------------------------------------------------------------
add_library(reckless_update INTERFACE)
target_include_directories(reckless_update INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/update")
target_link_libraries(reckless_update INTERFACE reckless_common_net reckless_common_core reckless_core_preset_files
                                                reckless_common_view)

# ---------------------------------------------------------------------------------------------------------------
# The licence: namespace reckless::licence, included by bare name - a licence's text and its Ed25519 signature checked
# against the public keys a build holds, offline (Licence.h, PublicKeys.h), and the licence a machine keeps for every
# product on it (LicenceStore.h, its files under the user's and the machine's settings folders, whose platform part is
# reckless_core_preset_files). The one compiled part is the signature's check (Ed25519.cpp), on Monocypher's Ed25519,
# compiled once as C with its warnings left to it in reckless_monocypher - the curve arithmetic and RFC 8032's Ed25519,
# nothing else of the library. Both are compiled as a plug-in module's code is, position-independent with their symbols
# hidden, so two modules in one process each keep their own. The build's demo switch (DemoMode.h) is reckless_demo_mode,
# which every translation unit that reads it links, so all agree on RECKLESS_DEMO_MODE.
# ---------------------------------------------------------------------------------------------------------------
add_library(reckless_demo_mode INTERFACE)
target_include_directories(reckless_demo_mode INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/licence")
target_compile_definitions(reckless_demo_mode INTERFACE RECKLESS_DEMO_MODE=$<BOOL:${RECKLESS_DEMO_MODE}>)

add_library(reckless_monocypher STATIC
    "${monocypher_SOURCE_DIR}/src/monocypher.c"
    "${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c")
target_include_directories(reckless_monocypher SYSTEM PUBLIC
    "${monocypher_SOURCE_DIR}/src" "${monocypher_SOURCE_DIR}/src/optional")
target_compile_options(reckless_monocypher
    PRIVATE   $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>
    INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/external:W0>)
set_target_properties(reckless_monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

add_library(reckless_common_licence STATIC licence/Ed25519.cpp)
target_include_directories(reckless_common_licence PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/licence")
target_link_libraries(reckless_common_licence
    PUBLIC  reckless_common_core reckless_core_preset_files reckless_demo_mode
    PRIVATE reckless_monocypher reckless_warnings)
set_target_properties(reckless_common_licence PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
                                                         VISIBILITY_INLINES_HIDDEN ON)

# ---------------------------------------------------------------------------------------------------------------
# The window's headers without the window: the shell every window's view tree is rooted in (window/ShellView.h, on the
# views), the licence's card (LicenceDialog.h), the module's embedded files (Embedded.h) and what a module's panel is
# made with (ModulePanel.h), for code that names them with no Skia - the view suites. reckless_window_api (skia/) is
# these with the renderer.
# ---------------------------------------------------------------------------------------------------------------
add_library(reckless_window_headers INTERFACE)
target_include_directories(reckless_window_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/window")
target_link_libraries(reckless_window_headers INTERFACE reckless_common_panel reckless_common_licence reckless_update)

# The app platform's headers without a platform: the seam (app/AppPlatform.h) and what is written on it alone, for code
# that names them with no Skia and no window compiled - a suite's platform of its own (tests/FakeAppPlatform.h).
# reckless_app_platform (below, with the GUI) is these with this build's platform.
add_library(reckless_app_headers INTERFACE)
target_include_directories(reckless_app_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/app")
target_link_libraries(reckless_app_headers INTERFACE reckless_window_headers reckless_common_core)

# An app's shell on the platform's seam, written against no platform and no renderer, so a suite drives it on its own
# platform (tests/FakeAppPlatform.h): the window opened round the app's views and its loop run, with the command line's
# --smoke (app/AppRun.h), the root that zooms a layout down to a small screen and shows the series' tooltip
# (app/AppRoot.h), the window's sizes from a View menu held on that root (app/AppWindowSizes.h), the app's colours and
# fonts (app/AppTheme.h), the widgets an app's window is made of (app/AppWidgets.h) and the update check an app runs
# (app/AppUpdates.h). reckless_app_resources (below, with the GUI) adds what the views draw with on Skia.
add_library(reckless_app_shell STATIC app/AppRun.cpp app/AppRoot.cpp app/AppWindowSizes.cpp)
target_link_libraries(reckless_app_shell
    PUBLIC  reckless_app_headers reckless_common_look reckless_update reckless_core_preset_files
    PRIVATE reckless_warnings)

# ---------------------------------------------------------------------------------------------------------------
# The audio device layer: header only but for the devices, namespace reckless::audio, included by bare name - the
# device and MIDI interfaces (AudioDevice.h), the settings an app keeps (AudioSettings.h: reckless-technology/<name>/
# audio.json) and an offline device for the suites (OfflineDevice.h). The devices themselves are reckless_audio_devices,
# with the toolkit's own back ends on each platform's API (audio/mac/, audio/win/, audio/linux/).
# ---------------------------------------------------------------------------------------------------------------
add_library(reckless_common_audio INTERFACE)
target_include_directories(reckless_common_audio INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/audio")
target_link_libraries(reckless_common_audio INTERFACE reckless_common_core)

if(RECKLESS_BUILD_AUDIO)
    # DeviceBackends.cpp is the only place that knows which back ends this build carries, and the one a product calls
    # through: it assembles them by hand, since a self-registering back end in a translation unit nothing references
    # is stripped from this static library by the linker (audio/DeviceBackends.h says so at length).
    add_library(reckless_audio_devices STATIC audio/DeviceBackends.cpp)
    target_link_libraries(reckless_audio_devices PUBLIC reckless_common_audio PRIVATE Threads::Threads reckless_warnings)
    if(APPLE)
        # CoreAudio, the toolkit's own back end on the HAL (audio/mac/CoreAudioDevice.cpp), and MIDI on CoreMIDI
        # (audio/mac/CoreMidi.cpp).
        target_sources(reckless_audio_devices PRIVATE audio/mac/CoreAudioDevice.cpp audio/mac/CoreMidi.cpp)
        target_link_libraries(reckless_audio_devices PRIVATE
            "-framework CoreAudio" "-framework CoreMIDI" "-framework CoreFoundation")
        target_compile_definitions(reckless_audio_devices PRIVATE RECKLESS_AUDIO_COREAUDIO RECKLESS_AUDIO_COREMIDI)
    endif()
    if(WIN32)
        # Windows Audio, the toolkit's own WASAPI back end in its two modes: COM, and the multimedia scheduler (avrt)
        # its stream thread runs under; and MIDI on WinMM (audio/win/WinMidi.cpp).
        target_sources(reckless_audio_devices PRIVATE audio/win/WasapiDevice.cpp audio/win/WinMidi.cpp)
        target_link_libraries(reckless_audio_devices PRIVATE ole32 avrt winmm)
        target_compile_definitions(reckless_audio_devices PRIVATE RECKLESS_AUDIO_WINMM)
    endif()
    if(NOT APPLE AND NOT WIN32 AND ALSA_FOUND)
        # ALSA, the toolkit's own back ends on alsa-lib: its PCMs (audio/linux/AlsaDevice.cpp) and its sequencer's MIDI
        # (audio/linux/AlsaMidi.cpp).
        target_sources(reckless_audio_devices PRIVATE audio/linux/AlsaDevice.cpp audio/linux/AlsaMidi.cpp)
        target_link_libraries(reckless_audio_devices PRIVATE ALSA::ALSA)
        target_compile_definitions(reckless_audio_devices PRIVATE RECKLESS_AUDIO_ALSA)
    endif()
    if(TARGET reckless_jack_loader)
        # JACK, the toolkit's own client (audio/linux/JackDevice.cpp), its calls through the run-time loader.
        target_sources(reckless_audio_devices PRIVATE audio/linux/JackDevice.cpp)
        target_include_directories(reckless_audio_devices SYSTEM PRIVATE "${RECKLESS_JACK_INCLUDE_DIR}")
        target_link_libraries(reckless_audio_devices PRIVATE reckless_jack_loader)
        target_compile_definitions(reckless_audio_devices PRIVATE RECKLESS_AUDIO_JACK)
    endif()
    if(TARGET reckless_asio_host)
        # ASIO, the toolkit's own back end on Steinberg's SDK (audio/win/AsioDevice.cpp), the SDK's host files linked.
        target_sources(reckless_audio_devices PRIVATE audio/win/AsioDevice.cpp)
        target_link_libraries(reckless_audio_devices PRIVATE reckless_asio_host)
        target_compile_definitions(reckless_audio_devices PRIVATE RECKLESS_AUDIO_ASIO)
        # cl's spelling for cl and clang-cl, the SDK's headers' warnings off; clang++'s GNU driver reads SYSTEM
        # includes as -isystem already.
        if(MSVC)
            target_compile_options(reckless_audio_devices PRIVATE /external:W0)
        endif()
    endif()
endif()

# The windowed parts: Skia, the canvas on it, the child window a panel is shown in, and an app's platform layer.
if(RECKLESS_TOOLKIT_GUI)
    add_subdirectory(skia)

    # An app's platform layer (app/AppPlatform.h, behind which the platforms sit), compiled once: an NSApplication
    # (app/mac/AppMac.mm), a Win32 message loop (app/win/AppWin.cpp) or an X11 connection on a poll loop that runs the
    # child windows' descriptors and timers too (app/linux/AppLinux.cpp); with where a window goes (app/WindowPlace.h)
    # and the audio settings' widgets on the views (app/SettingsWidgets.h).
    if(APPLE)
        set(reckless_app_platform_source "${CMAKE_CURRENT_SOURCE_DIR}/app/mac/AppMac.mm")
    elseif(WIN32)
        set(reckless_app_platform_source "${CMAKE_CURRENT_SOURCE_DIR}/app/win/AppWin.cpp")
    else()
        set(reckless_app_platform_source "${CMAKE_CURRENT_SOURCE_DIR}/app/linux/AppLinux.cpp")
    endif()
    add_library(reckless_app_platform STATIC "${reckless_app_platform_source}")
    target_include_directories(reckless_app_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/app")
    target_link_libraries(reckless_app_platform PUBLIC reckless_window PRIVATE reckless_warnings)
    if(APPLE)
        target_compile_options(reckless_app_platform PRIVATE -fobjc-arc)
        target_link_libraries(reckless_app_platform PUBLIC "-framework AppKit")
    endif()

    # What an app's views draw with on Skia (app/AppResources.h): the series' context, its images, the company mark.
    add_library(reckless_app_resources STATIC app/AppResources.cpp)
    target_link_libraries(reckless_app_resources PUBLIC reckless_app_shell reckless_window PRIVATE reckless_warnings)
endif()

# ---------------------------------------------------------------------------------------------------------------
# The suites.
# ---------------------------------------------------------------------------------------------------------------
if(RECKLESS_TOOLKIT_BUILD_TESTS)
    # Core: JSON, decimal text, the preset files and folders, audio files and the interface's preferences.
    add_executable(reckless_core_test tests/test_json.cpp tests/test_decimal_text.cpp tests/test_preset_files.cpp
                                      tests/test_audio_files.cpp tests/test_interface_settings.cpp)
    target_link_libraries(reckless_core_test PRIVATE reckless_common_core reckless_core_preset_files
                                                     reckless_common_audio_files reckless_warnings
                                                     reckless_toolkit_test_support GTest::gtest_main)
    target_compile_definitions(reckless_core_test PRIVATE
        RECKLESS_AUDIO_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/audio")
    add_test(NAME core COMMAND reckless_core_test)

    add_executable(reckless_gfx_test tests/test_gfx.cpp tests/test_svg_art.cpp tests/test_svg_image.cpp)
    target_link_libraries(reckless_gfx_test PRIVATE reckless_common_gfx reckless_warnings GTest::gtest_main)
    target_compile_definitions(reckless_gfx_test PRIVATE
        RECKLESS_METER_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets/meters"
        RECKLESS_SVG_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/svg")
    add_test(NAME gfx COMMAND reckless_gfx_test)

    add_executable(reckless_look_test tests/test_look.cpp)
    target_link_libraries(reckless_look_test PRIVATE reckless_common_look reckless_warnings GTest::gtest_main)
    add_test(NAME look COMMAND reckless_look_test)

    # The views, the series' panel, a picture kept between paints and the window's shell (window/ShellView.h, header
    # only, on the views alone) with the licence's card on a store of the suite's own, its licences the shared vectors'.
    add_executable(reckless_view_test tests/test_view.cpp tests/test_panel.cpp tests/test_cached_picture.cpp
                                      tests/test_shell_view.cpp tests/test_list_view.cpp
                                      tests/test_text_view.cpp)
    target_compile_definitions(reckless_view_test PRIVATE
        RECKLESS_LICENCE_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/licence/tests/vectors.json")
    target_link_libraries(reckless_view_test PRIVATE reckless_window_headers reckless_core_preset_files
                                                     reckless_warnings reckless_toolkit_test_support
                                                     GTest::gtest_main)
    add_test(NAME view COMMAND reckless_view_test)

    # The files a binary carries inside itself (cmake/RecklessEmbed.cmake): two tables naming the serif in common,
    # whose bytes every module shares, each held to its files by tests/test_embedded.cpp (CTests embedded_a and
    # embedded_b) - its names and files written beside it in EmbeddedFiles.h; the second takes the monospaced
    # family's faces too, as a product asks for them (reckless_monospace_font_files).
    set(assets_dir "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    set(embedded_a "NotoSerif-Regular.ttf=${assets_dir}/fonts/NotoSerif-Regular.ttf"
                   "vu_meter_face.svg=${assets_dir}/meters/vu_meter_face.svg"
                   "logos/RecklessRtLogo.png=${assets_dir}/logos/RecklessRtLogo.png")
    reckless_monospace_font_files(monospace_files)
    set(embedded_b "level_meter_face.svg=${assets_dir}/meters/level_meter_face.svg"
                   "serif.ttf=${assets_dir}/fonts/NotoSerif-Regular.ttf" ${monospace_files})
    foreach(table IN ITEMS a b)
        reckless_embed(reckless_embedded_files_${table} FILES ${embedded_${table}})
        set(listing "")
        foreach(item IN LISTS embedded_${table})
            string(FIND "${item}" "=" eq)
            string(SUBSTRING "${item}" 0 ${eq} name)
            math(EXPR start "${eq} + 1")
            string(SUBSTRING "${item}" ${start} -1 path)
            string(APPEND listing "    {\"${name}\", \"${path}\"},\n")
        endforeach()
        set(listing_dir "${CMAKE_CURRENT_BINARY_DIR}/embedded_test_${table}")
        file(CONFIGURE OUTPUT "${listing_dir}/EmbeddedFiles.h" CONTENT
"#pragma once
// Written by the toolkit's CMakeLists.txt: the files reckless_embedded_files_${table} carries, by name, in its order.
namespace reckless::test
{
struct EmbeddedFile
{
    const char *name;
    const char *path;
};

inline constexpr EmbeddedFile kEmbeddedFiles[] = {
@listing@};
} // namespace reckless::test
" @ONLY)
        add_executable(reckless_embedded_test_${table} tests/test_embedded.cpp)
        target_include_directories(reckless_embedded_test_${table} PRIVATE "${listing_dir}"
                                                                          "${CMAKE_CURRENT_SOURCE_DIR}/window")
        target_link_libraries(reckless_embedded_test_${table} PRIVATE reckless_embedded_files_${table}
                                                                      reckless_warnings GTest::gtest_main)
        add_test(NAME embedded_${table} COMMAND reckless_embedded_test_${table})
    endforeach()

    # The audio device layer on the offline device and its arithmetic: the settings an app keeps, and Windows Audio's
    # formats, periods and capture ring, which need no Windows.
    add_executable(reckless_audio_test tests/test_audio_settings.cpp tests/test_wasapi_formats.cpp
                                       tests/test_asio_formats.cpp tests/test_win_midi_formats.cpp
                                       tests/test_core_midi_formats.cpp)
    target_link_libraries(reckless_audio_test PRIVATE reckless_common_audio reckless_toolkit_test_support
                                                      reckless_warnings GTest::gtest_main)
    add_test(NAME audio COMMAND reckless_audio_test)

    # The network's parts on their own, and each platform's transport against a server of the suite's own on the
    # loopback interface - nothing reaches the internet unless RECKLESS_NET_LIVE is set.
    add_executable(reckless_net_test tests/test_net.cpp tests/test_net_transport.cpp)
    target_link_libraries(reckless_net_test PRIVATE reckless_common_net reckless_warnings reckless_toolkit_test_support
                                                    GTest::gtest_main)
    add_test(NAME net COMMAND reckless_net_test)

    # The update check on a scripted transport and a clock of its own: nothing reaches the internet.
    add_executable(reckless_update_test update/tests/test_update_check.cpp update/tests/test_updates_menu.cpp)
    target_link_libraries(reckless_update_test PRIVATE reckless_update reckless_core_preset_files
                                                       reckless_toolkit_test_support reckless_warnings
                                                       GTest::gtest_main)
    add_test(NAME update COMMAND reckless_update_test)

    # The licence's checker held to the vectors tools/licence.py writes (licence/tests/vectors.json), with Monocypher's
    # own signing beside it: the suite signs the vectors' payloads and holds the bytes.
    add_executable(reckless_licence_test tests/test_licence.cpp)
    target_link_libraries(reckless_licence_test PRIVATE reckless_common_licence reckless_monocypher
                                                        reckless_toolkit_test_support reckless_warnings
                                                        GTest::gtest_main)
    target_compile_definitions(reckless_licence_test PRIVATE
        RECKLESS_LICENCE_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/licence/tests/vectors.json")
    add_test(NAME licence COMMAND reckless_licence_test)

    # The app's platform seam with nothing on a screen: the queue other threads post calls to the main thread through
    # (core/PostQueue.h), the menu bar written once (app/MenuBar.h) and drawn (app/MenuBarView.h), the app's shell, and
    # the suites' own platform (tests/FakeAppPlatform.h), which a product's app suite drives - a key to the view with
    # the keyboard first. Named for the shell, since a product that builds the toolkit's suites beside its own may have
    # an app suite of its own: vst-plugins' reckless_app_test, its Standalone's.
    add_executable(reckless_app_shell_test tests/test_post_queue.cpp tests/test_fake_app_platform.cpp
                                           tests/test_app_shell.cpp tests/test_menu_bar.cpp tests/test_window_sizes.cpp)
    target_link_libraries(reckless_app_shell_test PRIVATE reckless_app_shell reckless_toolkit_test_support
                                                          reckless_warnings GTest::gtest_main)
    add_test(NAME app COMMAND reckless_app_shell_test)

    # This build's platform itself (app/AppPlatform.h's Create): posts from other threads through its loop. On Linux
    # it needs a display, so it runs with the window suites (under xvfb-run in CI); on macOS Run takes the focus.
    if(RECKLESS_TOOLKIT_GUI)
        add_executable(reckless_app_platform_test tests/test_app_platform.cpp)
        target_link_libraries(reckless_app_platform_test PRIVATE reckless_app_platform reckless_toolkit_test_support
                                                                 reckless_warnings GTest::gtest_main)
        add_test(NAME app_platform COMMAND reckless_app_platform_test)
        set_tests_properties(app_platform PROPERTIES LABELS window RESOURCE_LOCK display)

        # The clipboard written and read back: the system's only where it is no person's (CI, or
        # RECKLESS_TEST_CLIPBOARD=1), a pasteboard of the suite's own on macOS, and on Linux the CLIPBOARD selection's
        # owner answering pastes on a display - so it runs with the window suites.
        set(clipboard_sources tests/test_clipboard.cpp)
        if(APPLE)
            list(APPEND clipboard_sources tests/test_clipboard_mac.mm)
        endif()
        add_executable(reckless_clipboard_test ${clipboard_sources})
        target_link_libraries(reckless_clipboard_test PRIVATE reckless_app_platform reckless_test_host_window
                                                              reckless_warnings GTest::gtest_main)
        if(APPLE)
            target_compile_options(reckless_clipboard_test PRIVATE -fobjc-arc)
        endif()
        add_test(NAME clipboard COMMAND reckless_clipboard_test)
        set_tests_properties(clipboard PROPERTIES LABELS window RESOURCE_LOCK display)

        # The menu bar on this build's platform: the bar a Windows window carries and its keys, a child window handing
        # on what its views do not take; the drawn bar and a key press on Linux, on a display; the screen's bar and the
        # window's keyDown on macOS.
        set(app_menu_sources tests/test_app_menu.cpp)
        if(APPLE)
            list(APPEND app_menu_sources tests/test_app_menu_mac.mm)
        endif()
        add_executable(reckless_app_menu_test ${app_menu_sources})
        target_link_libraries(reckless_app_menu_test PRIVATE reckless_app_platform reckless_toolkit_test_support
                                                             reckless_warnings GTest::gtest_main)
        if(APPLE)
            target_compile_options(reckless_app_menu_test PRIVATE -fobjc-arc)
        endif()
        add_test(NAME app_menu COMMAND reckless_app_menu_test)
        set_tests_properties(app_menu PROPERTIES LABELS window RESOURCE_LOCK display)
    endif()
endif()

# The audio layer on the real APIs: reckless_audio_device_test (CTest `audio_devices`) opens every API the build carries
# on a deterministic test device - CoreAudio's default output, Windows Audio's default render endpoint with WASAPI's
# loopback capture, ASIO's test driver (the SDK's sample driver, built below and registered by the binary itself),
# JACK's dummy-driver server (started by the suite), ALSA's loopback card (loaded by the CI job) - and holds the
# devices, a stream's callbacks and rate, the samples a loop brings back, an xrun, and a device that is not there
# (tests/test_audio_devices.cpp, on the harness tests/devices/DeviceSuite.h). A missing test device skips its tests with
# the reason, unless RECKLESS_AUDIO_REQUIRE names the API: then they fail. The APIs the configure promised - the
# platform's own, ASIO wherever the SDK is, JACK and ALSA wherever their headers are - are handed to the suite, which
# fails a build that carries any other set. A product's suite of an engine playing through the same devices is
# made by the same function, reckless_audio_device_suite.
if(RECKLESS_TOOLKIT_BUILD_TESTS AND TARGET reckless_audio_devices)
    set(reckless_promised_apis "")
    if(APPLE)
        list(APPEND reckless_promised_apis "CoreAudio")
    elseif(WIN32)
        list(APPEND reckless_promised_apis "Windows Audio" "Windows Audio (Exclusive Mode)")
        if(RECKLESS_ASIO_SDK_DIR AND CMAKE_SIZEOF_VOID_P EQUAL 8)
            list(APPEND reckless_promised_apis "ASIO")
        endif()
    else()
        if(RECKLESS_JACK_INCLUDE_DIR)
            list(APPEND reckless_promised_apis "JACK")
        endif()
        if(ALSA_FOUND)
            list(APPEND reckless_promised_apis "ALSA")
        endif()
    endif()
    list(JOIN reckless_promised_apis "," reckless_promised_text)
    set(RECKLESS_AUDIO_PROMISED_TEXT "${reckless_promised_text}" CACHE INTERNAL "")
    if(WIN32 AND "ASIO" IN_LIST reckless_promised_apis)
        # The ASIO test driver: the SDK's sample driver from the fetched SDK - its COM scaffolding and its device - with
        # a clock and an entry point of the suite's own (tests/devices/asio_driver), a DLL beside the suite that only
        # the suite loads, registered by the suite itself (tests/devices/AsioRig.cpp). Never installed, never shipped:
        # the SDK is compiled against. The SDK's code is of its time - string literals as char *, the deprecated version
        # calls - so the target compiles as C++14, permissively, with its warnings off.
        add_library(reckless_asio_test_driver SHARED
            "${RECKLESS_ASIO_SDK_DIR}/driver/asiosample/asiosmpl.cpp"
            "${RECKLESS_ASIO_SDK_DIR}/common/combase.cpp"
            "${RECKLESS_ASIO_SDK_DIR}/common/dllentry.cpp"
            "${RECKLESS_ASIO_SDK_DIR}/common/register.cpp"
            tests/devices/asio_driver/TestDriverClock.cpp
            tests/devices/asio_driver/TestDriverEntry.cpp
            tests/devices/asio_driver/reckless_asio_test_driver.def)
        target_include_directories(reckless_asio_test_driver PRIVATE
            "${RECKLESS_ASIO_SDK_DIR}/common"
            "${RECKLESS_ASIO_SDK_DIR}/driver/asiosample")
        target_compile_definitions(reckless_asio_test_driver PRIVATE _CRT_SECURE_NO_WARNINGS)
        # cl's spelling for cl and clang-cl; clang++'s GNU driver takes the literals with a warning, and -w.
        if(MSVC)
            target_compile_options(reckless_asio_test_driver PRIVATE /w /permissive)
        else()
            target_compile_options(reckless_asio_test_driver PRIVATE -w)
        endif()
        set_target_properties(reckless_asio_test_driver PROPERTIES CXX_STANDARD 14)
        target_link_libraries(reckless_asio_test_driver PRIVATE winmm)
    endif()

    # reckless_audio_device_suite(<target> <test name> SOURCES <source>... [LIBRARIES <library>...])
    # A suite on the device layer's test devices (tests/devices/DeviceSuite.h, its harness, included by exactly one of
    # the sources): the rigs of the platform's test devices compiled in, the APIs the configure promised handed to it,
    # the ASIO test driver beside it where the build has one, and the CTest <test name>, run alone (RUN_SERIAL), since a
    # stream's timing is what it judges.
    function(reckless_audio_device_suite target test)
        cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES" ${ARGN})
        set(tests_dir "${RECKLESS_TOOLKIT_DIR}/tests")
        add_executable(${target} ${ARG_SOURCES})
        target_link_libraries(${target} PRIVATE reckless_audio_devices reckless_common_gfx
                                                reckless_toolkit_test_support ${ARG_LIBRARIES} reckless_warnings
                                                GTest::gtest)
        if(APPLE)
            # A device of the suite's own on CoreAudio, a private aggregate, named outside ASCII; and a MIDI source of
            # its own on CoreMIDI.
            target_sources(${target} PRIVATE "${tests_dir}/devices/AggregateMac.cpp" "${tests_dir}/devices/MidiSourceMac.cpp")
            target_link_libraries(${target} PRIVATE
                "-framework CoreAudio" "-framework CoreMIDI" "-framework CoreFoundation")
        endif()
        target_compile_definitions(${target} PRIVATE RECKLESS_AUDIO_PROMISED_APIS="${RECKLESS_AUDIO_PROMISED_TEXT}")
        if(TARGET reckless_jack_loader)
            # JACK's rig starts a server of its own and wires the loop through the same libjack the back end loads by name.
            target_sources(${target} PRIVATE "${tests_dir}/devices/JackRig.cpp")
            target_include_directories(${target} PRIVATE "${RECKLESS_TOOLKIT_DIR}/audio/linux")
            target_include_directories(${target} SYSTEM PRIVATE "${RECKLESS_JACK_INCLUDE_DIR}")
            target_link_libraries(${target} PRIVATE reckless_jack_loader)
            target_compile_definitions(${target} PRIVATE RECKLESS_AUDIO_TEST_JACK)
        endif()
        if(NOT APPLE AND NOT WIN32 AND TARGET ALSA::ALSA)
            # The MIDI test's own sequencer port, which sends what the ALSA sequencer back end must carry.
            target_link_libraries(${target} PRIVATE ALSA::ALSA)
            target_compile_definitions(${target} PRIVATE RECKLESS_AUDIO_TEST_ALSA_SEQ)
        endif()
        if(WIN32)
            target_sources(${target} PRIVATE "${tests_dir}/devices/WasapiRig.cpp")
            target_link_libraries(${target} PRIVATE ole32)
        endif()
        if(TARGET reckless_asio_test_driver)
            target_sources(${target} PRIVATE "${tests_dir}/devices/AsioRig.cpp")
            add_dependencies(${target} reckless_asio_test_driver)
            target_compile_definitions(${target} PRIVATE
                RECKLESS_AUDIO_TEST_ASIO
                RECKLESS_ASIO_TEST_DRIVER="$<TARGET_FILE:reckless_asio_test_driver>")
        endif()
        add_test(NAME ${test} COMMAND ${target})
        set_tests_properties(${test} PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
    endfunction()

    reckless_audio_device_suite(reckless_audio_device_test audio_devices SOURCES tests/test_audio_devices.cpp)
endif()

# What the directory that added the toolkit reads back: every pin (a product's third-party notices name them) and what
# the configure decided of the audio layer - the ASIO SDK it builds on, and the APIs it carries.
if(NOT PROJECT_IS_TOP_LEVEL)
    foreach(name IN LISTS RECKLESS_TOOLKIT_PINS ITEMS RECKLESS_ASIO_SDK_DIR RECKLESS_AUDIO_APIS)
        set(${name} "${${name}}" PARENT_SCOPE)
    endforeach()
endif()
