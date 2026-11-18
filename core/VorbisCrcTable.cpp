// The once-only fill of stb_vorbis's CRC table, behind a lock. core/AudioDecoders.c says why, and VorbisCrcTable.h
// declares what it calls.

#include "VorbisCrcTable.h"

#include <atomic>
#include <mutex>

namespace
{

std::mutex            table_lock;             ///< Held while the table is filled and while it is checked.
bool                  is_table_filled{false}; ///< Whether the table has been filled; read under the lock.
std::atomic<unsigned> fill_count{0};          ///< How many times it has been, for the suite.

} // namespace

extern "C" void reckless_vorbis_crc_table_ready(void)
{
    // A mutex rather than std::call_once or an atomic flag, so that every race detector, Helgrind and DRD among them,
    // sees the happens-before edge from the thread that filled the table to each thread that goes on to read it.
    const std::lock_guard<std::mutex> hold(table_lock);
    if (!is_table_filled)
    {
        reckless_vorbis_crc_table_fill();
        is_table_filled = true;
        fill_count.fetch_add(1, std::memory_order_relaxed);
    }
}

extern "C" unsigned reckless_vorbis_crc_fill_count(void)
{
    return fill_count.load(std::memory_order_relaxed);
}
