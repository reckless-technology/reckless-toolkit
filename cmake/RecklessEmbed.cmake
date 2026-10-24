# reckless_embed(<target> FILES <name>=<path>...): a static library holding the table of the files, found at run time by
# name through reckless::embedded::Find (window/Embedded.h) - a module's fonts, images and face plates, inside the
# binary, written by a script of plain CMake (EmbedFiles.cmake, beside this file). A file the build writes (a face
# plate) may be named.
#
# Each file's bytes are a translation unit of their own, made once for the whole configuration however many tables name
# the file (reckless_embedded_bytes) - the serif, the chicken head and the mark are in every module and app of a
# product, and a face plate in every binary that draws it - so a plate is turned into C++ and compiled once rather than
# once a binary, twenty megabytes of plates among forty translation units rather than in two of a hundred megabytes of
# C++ each, and a plate that changes is embedded again on its own. The
# table, the names and sizes with each file's array, is a translation unit of its own too, a few lines, written again
# when any of its files changes.
function(reckless_embed target)
    cmake_parse_arguments(ARG "" "" "FILES" ${ARGN})
    set(out_cpp "${CMAKE_CURRENT_BINARY_DIR}/${target}_embedded.cpp")
    set(paths "")
    set(byte_libraries "")
    set(lines "")
    foreach(item IN LISTS ARG_FILES)
        string(FIND "${item}" "=" eq)
        string(SUBSTRING "${item}" 0 ${eq} name)
        math(EXPR start "${eq} + 1")
        string(SUBSTRING "${item}" ${start} -1 path)
        reckless_embedded_bytes("${path}" library symbol)
        list(APPEND paths "${path}")
        list(APPEND byte_libraries ${library})
        list(APPEND lines "${name}=${symbol}=${path}")
    endforeach()
    list(REMOVE_DUPLICATES byte_libraries)
    # The list goes to the script in a file, one name=symbol=path a line: a list on a command line loses its separators
    # to the shell's quoting.
    set(list_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_embedded.txt")
    string(REPLACE ";" "\n" lines "${lines}")
    file(CONFIGURE OUTPUT "${list_file}" CONTENT "${lines}\n")
    add_custom_command(OUTPUT "${out_cpp}"
        COMMAND "${CMAKE_COMMAND}" -DMODE=table "-DOUT_CPP=${out_cpp}" "-DLIST_FILE=${list_file}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedFiles.cmake"
        DEPENDS ${paths} "${list_file}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedFiles.cmake"
        COMMENT "Embedding ${target}'s table"
        VERBATIM)
    add_library(${target} STATIC "${out_cpp}")
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(${target} PRIVATE ${byte_libraries})
endfunction()

# reckless_embedded_bytes(<path> <library-var> <symbol-var> [DEPENDS <target>...]): the static library holding the file
# at <path> as one array, made the first time the configuration asks for it and found after - with the names set in the
# caller's two variables. The library is named for the file, its folder and its name (reckless_embedded_cburnett_wK_svg
# for pieces/cburnett/wK.svg), so a build's list of targets says what each holds; a second path that would take a name
# already given takes the path's hash after it. The array is named for the hash. Excluded from all, so it is compiled
# only for a binary that embeds it, and its symbol hidden, so no module exports it. DEPENDS, the targets that write the
# file, for a file the build makes: a product's face plates, each given theirs as they are made, in the directory whose
# custom command writes them, so whichever table names a plate later - in a directory of its own - links bytes that
# wait for it.
function(reckless_embedded_bytes path library_var symbol_var)
    cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
    string(MD5 hash "${path}")
    string(SUBSTRING "${hash}" 0 16 hash)
    set(symbol "kBytes_${hash}")
    get_property(library GLOBAL PROPERTY "RECKLESS_EMBEDDED_LIBRARY_${hash}")
    if(NOT library)
        get_filename_component(file_name "${path}" NAME)
        get_filename_component(folder "${path}" DIRECTORY)
        get_filename_component(folder "${folder}" NAME)
        string(MAKE_C_IDENTIFIER "${folder}_${file_name}" readable)
        set(library "reckless_embedded_${readable}")
        if(TARGET ${library})
            set(library "${library}_${hash}")
        endif()
        set_property(GLOBAL PROPERTY "RECKLESS_EMBEDDED_LIBRARY_${hash}" "${library}")
        set(out_cpp "${CMAKE_BINARY_DIR}/embedded/${hash}.cpp")
        add_custom_command(OUTPUT "${out_cpp}"
            COMMAND "${CMAKE_COMMAND}" -DMODE=bytes "-DOUT_CPP=${out_cpp}" "-DIN_FILE=${path}" "-DSYMBOL=${symbol}"
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedFiles.cmake"
            DEPENDS "${path}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedFiles.cmake"
            COMMENT "Embedding ${path}"
            VERBATIM)
        add_library(${library} STATIC EXCLUDE_FROM_ALL "${out_cpp}")
        set_target_properties(${library} PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
    endif()
    if(ARG_DEPENDS)
        add_dependencies(${library} ${ARG_DEPENDS})
    endif()
    set(${library_var} ${library} PARENT_SCOPE)
    set(${symbol_var} ${symbol} PARENT_SCOPE)
endfunction()

# reckless_module_assets(<target> [<name>=<path>...]): the files a module's panel draws with - the series' serif, the
# chicken head and the company mark (assets/), and whatever the module names besides (its face plate) - embedded once
# per product: a plug-in's VST3 and Audio Unit name the same library and the second finds it made.
function(reckless_module_assets target)
    if(TARGET ${target})
        return()
    endif()
    get_filename_component(assets_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../assets" ABSOLUTE)
    reckless_embed(${target} FILES
        "NotoSerif-Regular.ttf=${assets_dir}/fonts/NotoSerif-Regular.ttf"
        "NotoSerif-Bold.ttf=${assets_dir}/fonts/NotoSerif-Bold.ttf"
        "RecklessChickenHead.png=${assets_dir}/knobs/RecklessChickenHead.png"
        "RecklessRtLogo.png=${assets_dir}/logos/RecklessRtLogo.png"
        ${ARGN})
endfunction()

# reckless_monospace_font_files(<var>): the monospaced family's two faces - Monaspace Xenon, regular and bold
# (assets/fonts/monaspace, under the SIL Open Font License, its OFL.txt beside them) - as the name=path items
# reckless_embed and reckless_module_assets take, set in the caller's <var>. Opt-in: no table carries them unless a
# product names them, so a binary that never draws the monospaced family is no bigger for it. At run time the product
# hands the two files to the canvas, gfx::SetMonospaceFaces(embedded::Find("MonaspaceXenon-Regular.otf"),
# embedded::Find("MonaspaceXenon-Bold.otf")), before it draws text in gfx::FontFamily::kMonospace.
function(reckless_monospace_font_files out_var)
    get_filename_component(fonts_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../assets/fonts/monaspace" ABSOLUTE)
    set(${out_var} "MonaspaceXenon-Regular.otf=${fonts_dir}/MonaspaceXenon-Regular.otf"
                   "MonaspaceXenon-Bold.otf=${fonts_dir}/MonaspaceXenon-Bold.otf" PARENT_SCOPE)
endfunction()
