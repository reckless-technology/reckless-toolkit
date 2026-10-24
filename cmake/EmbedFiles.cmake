# Run as a script (cmake -P) by RecklessEmbed.cmake, beside it, in one of two modes:
#
#   MODE=bytes: writes OUT_CPP, one file's bytes (IN_FILE) as the array SYMBOL - every byte, then a zero the table's
#   size does not count, so an empty file is still an array and a text file's bytes end as a C string's do.
#   MODE=table: writes OUT_CPP, the table reckless::embedded::Find walks, from LIST_FILE's lines, one name=symbol=path a
#   line: each name with its file's array, declared here and defined in that file's own translation unit, and its size.
if(MODE STREQUAL "bytes")
    file(READ "${IN_FILE}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    file(WRITE "${OUT_CPP}"
         "// Written by the toolkit's EmbedFiles.cmake from ${IN_FILE}. Do not edit.\n"
         "namespace reckless::embedded\n{\n"
         "extern const unsigned char ${SYMBOL}[] = {${bytes}0};\n"
         "} // namespace reckless::embedded\n")
elseif(MODE STREQUAL "table")
    file(STRINGS "${LIST_FILE}" FILES)
    set(declarations "")
    set(entries "")
    set(count 0)
    foreach(item IN LISTS FILES)
        string(FIND "${item}" "=" eq)
        string(SUBSTRING "${item}" 0 ${eq} name)
        math(EXPR start "${eq} + 1")
        string(SUBSTRING "${item}" ${start} -1 rest)
        string(FIND "${rest}" "=" eq)
        string(SUBSTRING "${rest}" 0 ${eq} symbol)
        math(EXPR start "${eq} + 1")
        string(SUBSTRING "${rest}" ${start} -1 path)
        file(SIZE "${path}" size)
        string(APPEND declarations "extern const unsigned char ${symbol}[];\n")
        string(APPEND entries "    {\"${name}\", ${symbol}, ${size}},\n")
        math(EXPR count "${count} + 1")
    endforeach()
    file(WRITE "${OUT_CPP}"
         "// Written by the toolkit's EmbedFiles.cmake from the files it names. Do not edit.\n#include <cstddef>\n\n"
         "namespace reckless::embedded\n{\nstruct Entry\n{\n    const char *name;\n"
         "    const unsigned char *data;\n    std::size_t size;\n};\n\n${declarations}\n"
         "extern const Entry kEntries[] = {\n${entries}};\n"
         "extern const std::size_t kEntryCount = ${count};\n} // namespace reckless::embedded\n")
else()
    message(FATAL_ERROR "EmbedFiles.cmake: MODE is bytes or table, not '${MODE}'")
endif()
