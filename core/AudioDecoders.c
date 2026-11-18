/*
    The vendored decoders AudioFiles.cpp calls, compiled once, as C, with their own warnings left to them: dr_flac and
    dr_mp3 (public domain, or MIT-0) and stb_vorbis (public domain, or MIT), each a single file fetched at a pinned
    commit by the root CMakeLists.txt. Their configuration - no stdio, no FLAC in Ogg (a `.flac` is native FLAC), MP3
    decoded to float, Vorbis pulled from memory - comes from the reckless_audio_decoders target, which
    hands the same definitions to AudioFiles.cpp, so the interfaces it sees are the ones compiled here.
*/
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

/*
    stb_vorbis builds a CRC table in a static array, and does it in `start_decoder`, on every open, "to avoid
    multithread race conditions" - which it does not: two streams opened on two threads write the same 256 words at
    once, and a third thread reading the table (`stb_vorbis_stream_length_in_samples`, every seek) reads it mid-write.
    The values are the same every time and it is still a data race, which Helgrind, DRD and ThreadSanitizer all report
    and the language calls undefined. The file is vendored at a pinned commit and not edited, so the call is rerouted
    from here: `crc32_init(void)`, the one definition, becomes `reckless_stb_vorbis_fill_crc_table`, and `crc32_init()`,
    the one call, becomes `reckless_vorbis_crc_table_ready`, which VorbisCrcTable.cpp runs under a lock, filling the
    table the first time and returning at once after. Every open therefore ends up behind the same lock and no thread
    can read a table a thread is still writing. The two are told apart by their argument (`void` in the definition,
    nothing in the call); if a newer stb_vorbis changes either, the suite's `reckless_vorbis_crc_fill_count` check fails
    and the Valgrind and ThreadSanitizer runs say so.
*/
#define RECKLESS_STB_CRC_PASTE(first, second) first##second
#define RECKLESS_STB_CRC_void                 reckless_stb_vorbis_fill_crc_table(void)
#define RECKLESS_STB_CRC_                     reckless_vorbis_crc_table_ready()
#define crc32_init(...)                       RECKLESS_STB_CRC_PASTE(RECKLESS_STB_CRC_, __VA_ARGS__)

#include "VorbisCrcTable.h"

#include "stb_vorbis.c"

#undef crc32_init

/** Fills stb_vorbis's CRC table; VorbisCrcTable.cpp is the only caller, and calls it once. */
void reckless_vorbis_crc_table_fill(void)
{
    reckless_stb_vorbis_fill_crc_table();
}
