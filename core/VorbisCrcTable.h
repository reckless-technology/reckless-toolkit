#pragma once

/*
    The once-only fill of stb_vorbis's CRC table (core/AudioDecoders.c explains the reroute), and a count of how many
    times it ran for the suite to check. Plain C linkage: its callers are the C file the decoders compile in and the
    suite.
*/

#ifdef __cplusplus
extern "C"
{
#endif

    /** Fills stb_vorbis's CRC table. Defined in core/AudioDecoders.c; call it only through the once-only gate. */
    void reckless_vorbis_crc_table_fill(void);

    /**
     * The gate every stb_vorbis open goes through: under one lock, fills the table the first time and returns at
     * once afterwards, so no thread reads the table while another writes it.
     */
    void reckless_vorbis_crc_table_ready(void);

    /** @return How many times the table has been filled in this process: one once a stream has been opened. */
    unsigned reckless_vorbis_crc_fill_count(void);

#ifdef __cplusplus
}
#endif
