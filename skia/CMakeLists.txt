# Skia (BSD-3-Clause), the 2D renderer the series' panels draw through - prebuilt, from the skia-pack releases, pinned
# by the release's tag (RECKLESS_SKIA_TAG, in cmake/ToolkitPins.cmake, which a product's notices name) and each
# archive's hash like every other fetch: macOS arm64 and x86_64 (lipo-ed into one set of universal libraries when the
# build is universal), Windows x64 (built for the static runtime, /MT, as the plugins are) and Linux x64. reckless_skia
# carries the headers, the libraries in link order and the platform's own libraries they need. The pack bundles
# HarfBuzz, ICU, libpng, zlib, expat, libjpeg-turbo, libwebp, wuffs, the DNG SDK and piex, and FreeType on Linux; of
# those a module links libpng and zlib, and FreeType on Linux - the canvas declines every image decoder but PNG's, so
# the other decoders' members are never loaded (gfx/skia/SkiaCanvas.cpp), and none of the pack's modules is on
# the link line, nor the expat, HarfBuzz and ICU under them.

set(skia_url "https://github.com/JetBrains/skia-pack/releases/download/${RECKLESS_SKIA_TAG}/Skia-${RECKLESS_SKIA_TAG}")

# The two licences of what a module links that the pack does not carry, for a product's third-party notices (its script
# reads them from here): Skia's own, from the source tree of the commit the pack was built from, which its colour
# management, skcms, shares; and the Chromium source tree's, at the revision the zlib commit Skia's DEPS names was
# copied from, which governs the SIMD routines in that zlib. Each a single file at its pinned commit, with its hash, not
# extracted.
FetchContent_Declare(skia_licence
    URL      https://raw.githubusercontent.com/JetBrains/skia/${RECKLESS_SKIA_COMMIT}/LICENSE
    URL_HASH SHA256=5f787c1dee3c56547f09ccc2906ab5f5293c4d8dd6c8654e573216c38e908dbd
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR reckless-builds-this-itself)
FetchContent_Declare(chromium_licence
    URL      https://raw.githubusercontent.com/chromium/chromium/${RECKLESS_CHROMIUM_COMMIT}/LICENSE
    URL_HASH SHA256=368cca1106be99d39ecd32a38d8305585d802a475effb66380b91ffc9bcf709b
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR reckless-builds-this-itself)
FetchContent_MakeAvailable(skia_licence chromium_licence)

function(reckless_fetch_skia name platform hash)
    FetchContent_Declare(${name}
        URL      "${skia_url}-${platform}.zip"
        URL_HASH SHA256=${hash}
        SOURCE_SUBDIR reckless-builds-this-itself)
    FetchContent_MakeAvailable(${name})
    set(${name}_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

# The libraries, in the order a single-pass linker needs them: Skia, then what it bundles that a module links. None of
# the pack's modules: the meters' artwork is read by the series' own reader of its SVG (gfx/SvgArt.h) and a
# panel's text is drawn by Skia's core alone, so the SVG module, text shaping and paragraphs - and the expat, HarfBuzz
# and ICU under them - stay off the link line; and no image codec's but PNG's: the canvas declines the others
# (gfx/skia/SkiaCanvas.cpp), so nothing a module links needs libjpeg-turbo's, libwebp's or wuffs' archives.
set(skia_names skia png zlib)

if(APPLE)
    set(archs ${CMAKE_OSX_ARCHITECTURES})
    if(NOT archs)
        set(archs ${CMAKE_HOST_SYSTEM_PROCESSOR})
    endif()
    set(lib_dirs "")
    set(slice_hashes "")
    foreach(arch IN LISTS archs)
        if(arch STREQUAL "arm64")
            set(hash 5ec2f81f28bf79ce4b9fd744dd183b3f718cb72a742a8465ae44f395a7bfb411)
            reckless_fetch_skia(skia_macos_arm64 macos-Release-arm64 ${hash})
            set(include_root "${skia_macos_arm64_DIR}")
            list(APPEND lib_dirs "${skia_macos_arm64_DIR}/out/Release-macos-arm64")
            list(APPEND slice_hashes ${hash})
        elseif(arch STREQUAL "x86_64")
            set(hash 077513011356f9d96c596def3c5b292e43c705dc6902812daa82bc147303a9f4)
            reckless_fetch_skia(skia_macos_x64 macos-Release-x64 ${hash})
            set(include_root "${skia_macos_x64_DIR}")
            list(APPEND lib_dirs "${skia_macos_x64_DIR}/out/Release-macos-x64")
            list(APPEND slice_hashes ${hash})
        else()
            message(FATAL_ERROR "Skia: no prebuilt package for macOS ${arch}")
        endif()
    endforeach()
    list(LENGTH lib_dirs dir_count)
    set(skia_libraries "")
    if(dir_count EQUAL 1)
        foreach(n IN LISTS skia_names)
            list(APPEND skia_libraries "${lib_dirs}/lib${n}.a")
        endforeach()
    else()
        # A universal build: each library's slices joined once, at configure time, into a folder named for the slices'
        # archives - the pin's hashes - so a moved pin joins its own libraries in a kept tree rather than linking the
        # last pin's against the new pin's headers; a folder another pin joined is removed.
        string(SHA256 slices_key "${slice_hashes}")
        string(SUBSTRING "${slices_key}" 0 16 slices_key)
        set(universal "${CMAKE_BINARY_DIR}/skia-universal-${slices_key}")
        file(GLOB stale LIST_DIRECTORIES true "${CMAKE_BINARY_DIR}/skia-universal*")
        list(REMOVE_ITEM stale "${universal}")
        if(stale)
            file(REMOVE_RECURSE ${stale})
        endif()
        file(MAKE_DIRECTORY "${universal}")
        foreach(n IN LISTS skia_names)
            set(slices "")
            foreach(d IN LISTS lib_dirs)
                list(APPEND slices "${d}/lib${n}.a")
            endforeach()
            if(NOT EXISTS "${universal}/lib${n}.a")
                execute_process(COMMAND lipo -create ${slices} -output "${universal}/lib${n}.a"
                                RESULT_VARIABLE lipo_result)
                if(NOT lipo_result EQUAL 0)
                    message(FATAL_ERROR "Skia: lipo failed for lib${n}.a")
                endif()
            endif()
            list(APPEND skia_libraries "${universal}/lib${n}.a")
        endforeach()
    endif()
    set(skia_system "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText"
                    "-framework CoreServices" "-framework ApplicationServices" "-framework Foundation")
elseif(WIN32)
    reckless_fetch_skia(skia_windows_x64 windows-Release-x64
                        50869b098a4f72f5843687093a1a8fdd94e5d61d98397024bb4e521227fedcba)
    set(include_root "${skia_windows_x64_DIR}")
    set(skia_libraries "")
    foreach(n IN LISTS skia_names)
        # The package names libpng's library as its own project does (libpng.lib) and the rest bare.
        if(n STREQUAL "png")
            set(n "lib${n}")
        endif()
        list(APPEND skia_libraries "${skia_windows_x64_DIR}/out/Release-windows-x64/${n}.lib")
    endforeach()
    set(skia_system opengl32 user32 gdi32 usp10 ole32 dwrite)
else()
    reckless_fetch_skia(skia_linux_x64 linux-Release-x64
                        895628ecaf8f60dee5b4c3b04574a290335408049110a008d79ad4668382a79b)
    set(include_root "${skia_linux_x64_DIR}")
    set(skia_libraries "")
    foreach(n IN LISTS skia_names)
        list(APPEND skia_libraries "${skia_linux_x64_DIR}/out/Release-linux-x64/lib${n}.a")
    endforeach()
    find_package(Freetype REQUIRED)
    find_package(Fontconfig REQUIRED)
    set(skia_system Freetype::Freetype Fontconfig::Fontconfig dl pthread)
endif()

add_library(reckless_skia INTERFACE)
target_include_directories(reckless_skia SYSTEM INTERFACE "${include_root}")
target_link_libraries(reckless_skia INTERFACE ${skia_libraries} ${skia_system})
target_compile_definitions(reckless_skia INTERFACE SK_RELEASE)

# The Canvas on Skia (gfx/skia), with a meter's artwork drawn on it - the document through the series' own reader of
# the meters' SVG (gfx/SvgArt.h), the shadow blurred by hand (SkiaArtwork) - and its suite.
add_library(reckless_gfx_skia STATIC "${RECKLESS_TOOLKIT_DIR}/gfx/skia/SkiaCanvas.cpp"
                                     "${RECKLESS_TOOLKIT_DIR}/gfx/skia/SkiaArtwork.cpp")
target_include_directories(reckless_gfx_skia PUBLIC "${RECKLESS_TOOLKIT_DIR}/gfx/skia")
target_link_libraries(reckless_gfx_skia PUBLIC reckless_common_gfx reckless_skia PRIVATE reckless_warnings)
set_target_properties(reckless_gfx_skia PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
                                                   VISIBILITY_INLINES_HIDDEN ON)

if(RECKLESS_TOOLKIT_BUILD_TESTS)
    add_executable(reckless_skia_test tests/test_skia_canvas.cpp tests/test_skia_svg_and_fonts.cpp)
    target_link_libraries(reckless_skia_test PRIVATE reckless_gfx_skia reckless_common_look reckless_warnings
                                                     GTest::gtest_main)
    target_compile_definitions(reckless_skia_test PRIVATE RECKLESS_ASSETS_DIR="${RECKLESS_TOOLKIT_DIR}/assets"
                                                          RECKLESS_SVG_FIXTURES="${RECKLESS_TOOLKIT_DIR}/tests/svg")
    add_test(NAME skia COMMAND reckless_skia_test)

    # The views that show many lines (view/ListView.h) drawn on Skia in the series' look (panel/ListStyles.h), with
    # their review images when RECKLESS_SCREENSHOT_DIR is set.
    add_executable(reckless_list_text_views_test tests/test_list_text_views.cpp)
    target_link_libraries(reckless_list_text_views_test PRIVATE reckless_gfx_skia reckless_common_panel
                                                                reckless_warnings GTest::gtest_main)
    target_compile_definitions(reckless_list_text_views_test PRIVATE
        RECKLESS_ASSETS_DIR="${RECKLESS_TOOLKIT_DIR}/assets")
    add_test(NAME list_text_views COMMAND reckless_list_text_views_test)
endif()

# The child window that shows a panel in a host's window on Skia (window/: an NSView on macOS, an HWND on Windows, an
# X11 window on the host's run loop on Linux). reckless_window_api is its header alone, for an adapter - the window's
# headers (reckless_window_headers: the shell, which offers the machine's licence and the update check) with the
# canvas on Skia; reckless_window the window compiled once, for the tools; a plug-in module takes the window through
# reckless_module_window().
add_library(reckless_window_api INTERFACE)
target_link_libraries(reckless_window_api INTERFACE reckless_gfx_skia reckless_window_headers)

if(APPLE)
    set(RECKLESS_WINDOW_SOURCE "${RECKLESS_TOOLKIT_DIR}/window/mac/WindowMac.mm" CACHE INTERNAL "")
    set(reckless_window_system "-framework AppKit" "-framework QuartzCore"
                               "-framework UniformTypeIdentifiers")
elseif(WIN32)
    set(RECKLESS_WINDOW_SOURCE "${RECKLESS_TOOLKIT_DIR}/window/win/WindowWin.cpp" CACHE INTERNAL "")
    set(reckless_window_system user32 gdi32)
else()
    find_package(X11 REQUIRED)
    set(RECKLESS_WINDOW_SOURCE "${RECKLESS_TOOLKIT_DIR}/window/linux/WindowLinux.cpp" CACHE INTERNAL "")
    set(reckless_window_system X11::X11)
endif()
# The platform's libraries ride on the window's interface, linked here, where X11's imported target was found: a
# module takes the window from its own directory (reckless_module_window), where that target is not known.
target_link_libraries(reckless_window_api INTERFACE ${reckless_window_system})

add_library(reckless_window STATIC "${RECKLESS_WINDOW_SOURCE}")
target_link_libraries(reckless_window PUBLIC reckless_window_api PRIVATE reckless_warnings)
if(APPLE)
    target_compile_options(reckless_window PRIVATE -fobjc-arc)
endif()
set_target_properties(reckless_window PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
                                                 VISIBILITY_INLINES_HIDDEN ON)

# A window of the platform's own for a suite to attach an editor to (tests/TestHostWindow.h): AppKit's
# on macOS, X11's on Linux - on the display a suite runs under, Xvfb's on the Linux runner through xvfb-run - and none
# on Windows, whose runners are services on a window station that shows nothing: a window made there is never visible.
if(RECKLESS_TOOLKIT_BUILD_TESTS)
    if(APPLE)
        add_library(reckless_test_host_window STATIC "${RECKLESS_TOOLKIT_DIR}/tests/TestHostWindowMac.mm")
        target_compile_options(reckless_test_host_window PRIVATE -fobjc-arc)
        target_link_libraries(reckless_test_host_window PUBLIC "-framework AppKit" PRIVATE reckless_warnings)
        target_include_directories(reckless_test_host_window PUBLIC "${RECKLESS_TOOLKIT_DIR}/tests")
    elseif(WIN32)
        add_library(reckless_test_host_window INTERFACE)
        target_include_directories(reckless_test_host_window INTERFACE "${RECKLESS_TOOLKIT_DIR}/tests")
    else()
        add_library(reckless_test_host_window STATIC "${RECKLESS_TOOLKIT_DIR}/tests/TestHostWindowLinux.cpp")
        target_link_libraries(reckless_test_host_window PUBLIC X11::X11 PRIVATE reckless_warnings)
        target_include_directories(reckless_test_host_window PUBLIC "${RECKLESS_TOOLKIT_DIR}/tests")
    endif()

    # The child window's own suite (tests/test_child_window.cpp): on Linux, on a display, the files a desktop drags over
    # an app's top-level window taken by the child window that fills it, through XDND's proxy - run with the window
    # suites under xvfb-run; on macOS a scroll saying whether the system inverted the device's direction; on every
    # platform the paths of a drag source's URI list (window/UriList.h). Named for its CTest, apart from a product's
    # window suites.
    add_executable(reckless_child_window_test tests/test_child_window.cpp)
    target_link_libraries(reckless_child_window_test PRIVATE reckless_window reckless_test_host_window reckless_warnings
                                                             GTest::gtest_main)
    add_test(NAME child_window COMMAND reckless_child_window_test)
    set_tests_properties(child_window PROPERTIES LABELS window RESOURCE_LOCK display)
endif()

# reckless_module_window(<target>): the child window inside one plug-in module, compiled into it. On macOS the window's
# NSView subclass is registered as the module first makes a view, under the name given here for the module
# (RecklessPanelView_<target>) and the address of the loaded image: the Objective-C runtime finds one class per name in
# a process, so of two images sharing a name - a plug-in's VST3 and its AU in one host, or two versions of one module -
# a lookup by the name, as key-value observing makes, would take one image's class for the other's views, and lose it
# when that image is unloaded. On Windows the window class is already the module's own (named from its handle), and X
# has no class registry, but the window is compiled in the same way on every platform.
function(reckless_module_window target)
    target_sources(${target} PRIVATE "${RECKLESS_WINDOW_SOURCE}")
    string(MAKE_C_IDENTIFIER "${target}" tag)
    target_compile_definitions(${target} PRIVATE RECKLESS_PANEL_VIEW=RecklessPanelView_${tag})
    target_link_libraries(${target} PRIVATE reckless_window_api)
    if(APPLE)
        set_property(SOURCE "${RECKLESS_WINDOW_SOURCE}" TARGET_DIRECTORY ${target} APPEND PROPERTY COMPILE_OPTIONS
                     -fobjc-arc)
    endif()
endfunction()
