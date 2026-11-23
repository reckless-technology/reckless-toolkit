#pragma once

#include <cerrno>
#include <chrono>
#include <climits>
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

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
// MoveFileExW alone is wanted from <windows.h>, which defines ReplaceFile, MoveFile, CopyFile, DeleteFile and more as
// macros and min and max besides; so it is declared here, as the SDK does, rather than the header pulled into every
// file that reads a JSON document.
extern "C" __declspec(dllimport) int __stdcall MoveFileExW(const wchar_t *existing, const wchar_t *replacement,
                                                           unsigned long flags);
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace reckless::core
{
/** What WriteWhole does to reach the device, one small function to a step, the platforms' differences kept in here. */
namespace durable
{
#if defined(_WIN32)
/** The temporary made, truncated and open for writing in binary, shared with other programs as std::ofstream shares.
    @param file  the file to make
    @return      its descriptor, or -1 */
inline int OpenForWrite(const std::filesystem::path &file)
{
    // _wsopen_s, not _wopen, which MSVC deprecates (C4996) in every file that includes this header. _SH_DENYNO is the
    // sharing std::ofstream gives.
    int descriptor = -1;
    if (::_wsopen_s(&descriptor, file.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _SH_DENYNO,
                    _S_IREAD | _S_IWRITE) != 0)
    {
        return -1;
    }
    return descriptor;
}

/** One write of up to `count` bytes (the CRT's takes an unsigned int).
    @param descriptor  the open file
    @param data        the bytes
    @param count       how many
    @return            how many were written, or -1 */
inline long long WriteSome(int descriptor, const char *data, std::size_t count)
{
    return ::_write(descriptor, data, static_cast<unsigned int>(count < (1u << 30) ? count : (1u << 30)));
}

/** The file's data sent to the device: FlushFileBuffers, which _commit calls.
    @param descriptor  the open file
    @return            true when it is flushed */
inline bool FlushFile(int descriptor)
{
    return ::_commit(descriptor) == 0;
}

/** @param descriptor the file to close */
inline void CloseFile(int descriptor)
{
    ::_close(descriptor);
}

/** The temporary moved over the file and the move written through to the device before the call returns.
    @param temporary  the file to move
    @param file       where to
    @return           true when it is moved */
inline bool Replace(const std::filesystem::path &temporary, const std::filesystem::path &file)
{
    constexpr unsigned long kReplaceExisting = 0x1; // MOVEFILE_REPLACE_EXISTING
    constexpr unsigned long kWriteThrough    = 0x8; // MOVEFILE_WRITE_THROUGH
    return ::MoveFileExW(temporary.c_str(), file.c_str(), kReplaceExisting | kWriteThrough) != 0;
}

/** Windows has no folder to flush: the replace was written through.
    @param folder the folder the file is in
    @return       true */
inline bool FlushFolder(const std::filesystem::path &)
{
    return true;
}
#else
/** The temporary made, truncated and open for writing, with the permissions std::ofstream gives (0666 less the umask).
    @param file  the file to make
    @return      its descriptor, or -1 */
inline int OpenForWrite(const std::filesystem::path &file)
{
    return ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
}

/** One write of up to `count` bytes, tried again when a signal interrupts it.
    @param descriptor  the open file
    @param data        the bytes
    @param count       how many
    @return            how many were written, or -1 */
inline long long WriteSome(int descriptor, const char *data, std::size_t count)
{
    for (;;)
    {
        const auto written = ::write(descriptor, data, count);
        if (written >= 0 || errno != EINTR)
        {
            return written;
        }
    }
}

/** A descriptor's data sent to the device. On macOS fsync reaches only the drive's cache, which the drive may keep
    through a power cut; F_FULLFSYNC asks the drive to empty it, and is used here, falling back to fsync for a volume
   that refuses it. It costs milliseconds to tens of milliseconds more a call there, and a save is a rare event.
    @param descriptor  the open file or folder
    @return            true when it is flushed, or when the file system cannot flush a folder (EINVAL, ENOTSUP) */
inline bool FlushDescriptor(int descriptor)
{
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (::fcntl(descriptor, F_FULLFSYNC) == 0)
    {
        return true;
    }
#endif
    for (;;)
    {
        if (::fsync(descriptor) == 0)
        {
            return true;
        }
        if (errno != EINTR)
        {
            return errno == EINVAL || errno == ENOTSUP;
        }
    }
}

/** @param descriptor the open file to flush
    @return           true when it is flushed */
inline bool FlushFile(int descriptor)
{
    return FlushDescriptor(descriptor);
}

/** @param descriptor the file to close */
inline void CloseFile(int descriptor)
{
    ::close(descriptor);
}

/** The temporary renamed over the file, atomically.
    @param temporary  the file to move
    @param file       where to
    @return           true when it is moved */
inline bool Replace(const std::filesystem::path &temporary, const std::filesystem::path &file)
{
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    return !error;
}

/** The folder's entry for the new name sent to the device, so the rename survives a power cut. A folder that cannot be
    opened for reading is taken as flushed as it can be, as is a file system that refuses to flush one.
    @param folder  the folder the file is in; empty for the current one
    @return        true unless the device reported an error
    */
inline bool FlushFolder(const std::filesystem::path &folder)
{
#if defined(O_DIRECTORY)
    const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#else
    const int flags = O_RDONLY | O_CLOEXEC;
#endif
    const int descriptor = ::open(folder.empty() ? "." : folder.c_str(), flags);
    if (descriptor < 0)
    {
        return true;
    }
    const bool isFlushed = FlushDescriptor(descriptor);
    ::close(descriptor);
    return isFlushed;
}
#endif

/** `bytes` written whole to the open file and flushed to the device.
    @param descriptor  the open file
    @param bytes       the bytes
    @return            true when every byte is written and flushed */
inline bool WriteAndFlush(int descriptor, std::string_view bytes)
{
    while (!bytes.empty())
    {
        const auto written = WriteSome(descriptor, bytes.data(), bytes.size());
        if (written <= 0)
        {
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return FlushFile(descriptor);
}
} // namespace durable

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
    success. Durable: the temporary's data is flushed to the device before the rename (fsync; F_FULLFSYNC on macOS;
    FlushFileBuffers on Windows) and the folder after it (fsync of the folder on POSIX, a file system that refuses
    one taken as done; MOVEFILE_WRITE_THROUGH on Windows), so a power cut after a true return leaves the new file, and
    one before it leaves the old or the new, never an empty or truncated one. A flush the device reports failing
    after the rename is kReplace, the file then holding the new bytes. True when the file is written; otherwise
    `failure`, where given, says which step stopped it, and no temporary is left behind except in that last case
    (none: it was already renamed).
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
        const int descriptor = durable::OpenForWrite(temporary);
        if (descriptor < 0)
        {
            return fail(WriteFailure::kOpen);
        }
        const bool isWritten = durable::WriteAndFlush(descriptor, bytes);
        durable::CloseFile(descriptor);
        if (!isWritten)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return fail(WriteFailure::kWrite);
        }
    }
    constexpr int kAttempts = 5;
    for (int attempt = 1;; ++attempt)
    {
        if (durable::Replace(temporary, file))
        {
            if (!durable::FlushFolder(file.parent_path()))
            {
                return fail(WriteFailure::kReplace);
            }
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
