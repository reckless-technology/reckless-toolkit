#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "Json.h"

namespace reckless::core
{
/** The bytes of `file` in a `Bytes` - a std::string or a std::vector<std::uint8_t> - or nothing when it cannot be
    opened: read straight into the container at the size the file system gives, then on to the end for whatever
    lies past it (a file that grew, or one whose size reads as none), so a large file is held once and read in one
    call.
    @tparam Bytes  the container: a std::string or a std::vector<std::uint8_t>
    @param file  the file to read
    @return      the file's bytes, or nothing when it cannot be opened */
template <typename Bytes> std::optional<Bytes> ReadWhole(const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    Bytes           bytes;
    std::error_code error;
    const auto      size = std::filesystem::file_size(file, error);
    if (!error && size > 0)
    {
        bytes.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
        bytes.resize(static_cast<std::size_t>(in.gcount()));
    }
    char chunk[4096];
    while (in.read(chunk, sizeof chunk) || in.gcount() > 0)
    {
        bytes.insert(bytes.end(), chunk, chunk + in.gcount());
    }
    return bytes;
}

/** The bytes of `file` as text, or nothing when it cannot be opened: a document's.
    @param file  the file to read
    @return      the file's bytes as text, or nothing */
inline std::optional<std::string> ReadBytes(const std::filesystem::path &file)
{
    return ReadWhole<std::string>(file);
}

/** The bytes of `file` as bytes, or nothing when it cannot be opened: an audio file's, for its reader to keep.
    @param file  the file to read
    @return      the file's bytes, or nothing */
inline std::optional<std::vector<std::uint8_t>> ReadFileBytes(const std::filesystem::path &file)
{
    return ReadWhole<std::vector<std::uint8_t>>(file);
}

/** The step that stopped WriteWhole: the temporary beside the file could not be made, the bytes did not all reach it,
    or it could not be renamed over the file. */
enum class WriteFailure
{
    kNone,    ///< nothing failed
    kOpen,    ///< the temporary beside the file could not be made
    kWrite,   ///< the bytes did not all reach the temporary
    kReplace, ///< the temporary could not be renamed over the file
};

/** Writes `bytes` - a document's text, or a binary file's bytes - to `file` whole or not at all: to a hidden temporary
    beside it, then renamed over it - tried five times, a tenth of a second apart, for a file another program holds a
    moment (Windows refuses the rename while it does). The temporary's name ends in ".tmp", so one a crash leaves behind
    is never read as a document, nor as a response or anything else a loader takes by its extension. Deleting the file
    first and writing over it would leave a file another program has open appended to rather than replaced, and report
    success. True when the file is written; otherwise `failure`, where given, says which step stopped it, and no
    temporary is left behind.
    @param file     the file to write
    @param bytes    the bytes to write
    @param failure  where to put the step that stopped the write, or null; set to kNone on success
    @return         true when the file is written */
inline bool WriteWhole(const std::filesystem::path &file, std::string_view bytes, WriteFailure *failure = nullptr)
{
    const auto fail = [failure](WriteFailure step) {
        if (failure != nullptr)
        {
            *failure = step;
        }
        return false;
    };
    std::random_device random;
    const auto         draw = (static_cast<std::uint64_t>(random()) << 32) ^ static_cast<std::uint64_t>(random());
    char               hex[17];
    for (int i = 0; i < 16; ++i)
    {
        hex[i] = "0123456789abcdef"[(draw >> (60 - 4 * i)) & 0xfu];
    }
    hex[16] = '\0';
    std::filesystem::path name(".");
    name += file.filename();
    name += std::string(".") + hex + ".tmp";
    const std::filesystem::path temporary = file.parent_path() / name;
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return fail(WriteFailure::kOpen);
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return fail(WriteFailure::kWrite);
        }
    }
    constexpr int kAttempts = 5;
    for (int attempt = 1;; ++attempt)
    {
        std::error_code error;
        std::filesystem::rename(temporary, file, error);
        if (!error)
        {
            if (failure != nullptr)
            {
                *failure = WriteFailure::kNone;
            }
            return true;
        }
        if (attempt == kAttempts)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return fail(WriteFailure::kReplace);
}

/** `document` written to `file` as JSON text and a final line feed, whole (WriteWhole), its folder made first. False
    when it could not be.
    @param file      the file to write; an empty path is refused
    @param document  the document to write
    @return          true when the file is written */
inline bool WriteJsonFile(const std::filesystem::path &file, const json::Value &document)
{
    if (file.empty())
    {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    return WriteWhole(file, json::Write(document) + "\n");
}

/** The JSON document `file` holds, or nothing when it is missing or not JSON - `error`, where given, saying which.
    @param file   the file to read; an empty path reads as missing
    @param error  where to put the reason when the result is empty, or null
    @return       the document, or nothing */
inline std::optional<json::Value> ReadJsonFile(const std::filesystem::path &file, std::string *error = nullptr)
{
    const std::optional<std::string> bytes = file.empty() ? std::nullopt : ReadBytes(file);
    if (!bytes)
    {
        if (error != nullptr)
        {
            *error = "it cannot be read";
        }
        return std::nullopt;
    }
    json::ParseResult parsed = json::Parse(*bytes);
    if (!parsed.IsOk())
    {
        if (error != nullptr)
        {
            *error = "it is not JSON (" + parsed.error + ")";
        }
        return std::nullopt;
    }
    return std::move(parsed.value);
}
} // namespace reckless::core
