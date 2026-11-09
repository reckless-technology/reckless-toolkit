# reckless_download(<url> <file>): a URL's bytes into a file, a transient failure tried again. CMake's own download,
# which FetchContent uses, tries a failed connection again but an HTTP error only once; this tries again on an HTTP 429
# or 5xx, or a connection that failed, timed out, was reset or ended part way (curl's errors 7, 18, 28, 35, 52, 55, 56
# and 92), six tries in all after the waits in RECKLESS_DOWNLOAD_WAITS (4, 8, 16, 32 and 60 seconds), and says so on
# each. Any other failure, or the last, stops the configure with nothing left behind. It checks no hash.
#
# reckless_fetch_content(<name> <url> <sha256> [<FetchContent_Declare options>...]): FetchContent_Declare for a URL with
# its hash, the archive fetched first by reckless_download into <FETCHCONTENT_BASE_DIR>/_downloads/<name>/ and
# FetchContent taking it from there. A hash that is not the pin's stops the configure at once and is never tried again;
# an archive already there at its hash is not fetched again. Call FetchContent_MakeAvailable (or Populate) as for any
# declared content; the options after the hash (SOURCE_SUBDIR, DOWNLOAD_NO_EXTRACT) go to FetchContent_Declare as given.

include_guard(GLOBAL)
include(FetchContent)

if(NOT DEFINED RECKLESS_DOWNLOAD_WAITS)
    set(RECKLESS_DOWNLOAD_WAITS 4 8 16 32 60)
endif()

function(reckless_download url file)
    set(transient_curl_errors 7 18 28 35 52 55 56 92)
    foreach(wait IN LISTS RECKLESS_DOWNLOAD_WAITS ITEMS 0)
        file(DOWNLOAD "${url}" "${file}" STATUS status LOG log TIMEOUT 300 INACTIVITY_TIMEOUT 60)
        list(GET status 0 code)
        if(code EQUAL 0)
            return()
        endif()
        set(http "")
        if(log MATCHES "returned error: ([0-9]+)")
            set(http "${CMAKE_MATCH_1}")
        endif()
        if(wait EQUAL 0 OR NOT (http MATCHES "^(5..|429)$" OR code IN_LIST transient_curl_errors))
            break()
        endif()
        message(STATUS "${url}: ${status} ${http}; trying again in ${wait} s")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${wait})
    endforeach()
    file(REMOVE "${file}")
    message(FATAL_ERROR "${url} could not be downloaded: ${status} ${http}")
endfunction()

function(reckless_fetch_content name url sha256)
    get_filename_component(archive_name "${url}" NAME)
    set(archive "${FETCHCONTENT_BASE_DIR}/_downloads/${name}/${archive_name}")
    string(TOUPPER "${name}" upper_name)
    set(had "")
    if(EXISTS "${archive}")
        file(SHA256 "${archive}" had)
    endif()
    if(NOT FETCHCONTENT_SOURCE_DIR_${upper_name} AND NOT FETCHCONTENT_FULLY_DISCONNECTED
       AND NOT had STREQUAL sha256)
        reckless_download("${url}" "${archive}")
        file(SHA256 "${archive}" got)
        if(NOT got STREQUAL sha256)
            file(REMOVE "${archive}")
            message(FATAL_ERROR "${url} is not the pinned file: its SHA256 is ${got}, the pin is ${sha256}")
        endif()
    endif()
    FetchContent_Declare(${name} URL "${archive}" URL_HASH SHA256=${sha256} ${ARGN})
endfunction()
