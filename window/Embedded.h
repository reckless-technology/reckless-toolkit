#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace reckless::embedded
{
/** One file a module carries inside itself (cmake/RecklessEmbed.cmake): its name and its bytes. */
struct Entry
{
    const char          *name; ///< the file's name, a null-terminated string with static storage
    const unsigned char *data; ///< the file's bytes, static storage
    std::size_t          size; ///< the number of bytes in `data`
};

/** The table of files the module carries, written by cmake/EmbedFiles.cmake. */
extern const Entry kEntries[];
/** The number of entries in kEntries. */
extern const std::size_t kEntryCount;

/** The bytes of the file named `name`, or none.
    @param name  the file's name, as the module's table lists it
    @return      the file's bytes, which live as long as the module; empty when no file has that name */
inline std::span<const std::uint8_t> Find(std::string_view name)
{
    for (std::size_t i = 0; i < kEntryCount; ++i)
    {
        if (name == kEntries[i].name)
        {
            return {reinterpret_cast<const std::uint8_t *>(kEntries[i].data), kEntries[i].size};
        }
    }
    return {};
}
} // namespace reckless::embedded
