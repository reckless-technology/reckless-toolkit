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

#include "stb_vorbis.c"
