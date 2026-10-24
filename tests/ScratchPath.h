#pragma once

// Test-only: the scratch places a suite writes under the system's temporary directory, each its own process's. A name
// fixed in a suite is shared by every process that runs the suite - two build trees' ctest runs at once, a suite run
// by hand beside CI's, CTest's -j running two binaries that hold one helper - and each then wipes, overwrites or reads
// the other's files: the Cab's carried response, the Space's panel's files, the presets' scratch root and the board's
// factory folder have all failed so. So every name goes under a folder of the process's own, named for its id and 64
// bits from the system's generator, made at the first call and removed with everything in it as the process exits.
// Within a process a name is one path every time, so a suite's helpers that build it twice meet one file; only the
// process's own folder makes it unique.

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace reckless::test
{
/** This process's id, as the process-scratch folder's name and its owner's check read it.
    @return  the id */
inline long ProcessId()
{
#if defined(_WIN32)
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

/** The folder under the system's temporary directory that is this process's alone, made on construction and removed
    whole at the process's exit - by the process that made it alone, so a child that exits after a fork leaves its
    parent's. Reached through ProcessScratch(). */
class ProcessScratchFolder
{
  public:
    /** The folder named for the process's id and 64 bits from the system's generator, and made. */
    ProcessScratchFolder() : owner_(ProcessId())
    {
        std::random_device       random;
        const unsigned long long tag =
            (static_cast<unsigned long long>(random()) << 32) ^ static_cast<unsigned long long>(random());
        char hex[17] = {};
        for (int i = 0; i < 16; ++i)
        {
            hex[i] = "0123456789abcdef"[(tag >> (60 - 4 * i)) & 0xF];
        }
        folder_ = std::filesystem::temp_directory_path() /
                  ("reckless-tests-" + std::to_string(owner_) + "-" + std::string(hex));
        std::error_code ignored;
        std::filesystem::create_directories(folder_, ignored);
    }

    /** The folder and everything in it removed, if this is the process that made it; a file still held open stays. */
    ~ProcessScratchFolder()
    {
        if (ProcessId() == owner_)
        {
            std::error_code ignored;
            std::filesystem::remove_all(folder_, ignored);
        }
    }

    ProcessScratchFolder(const ProcessScratchFolder &)            = delete; ///< one folder a process
    ProcessScratchFolder &operator=(const ProcessScratchFolder &) = delete; ///< one folder a process

    /** The folder.
        @return  its path */
    const std::filesystem::path &path() const
    {
        return folder_;
    }

  private:
    long                  owner_;  ///< the process that made the folder, the one that removes it
    std::filesystem::path folder_; ///< the folder
};

/** The process's scratch folder, made at the first call.
    @return  its path */
inline const std::filesystem::path &ProcessScratch()
{
    static const ProcessScratchFolder folder;
    return folder.path();
}

/** `name` - a file or a folder - in this process's scratch folder under the system's temporary directory: the same
    path for the same name throughout the process, and no other process's. Nothing is made at it.
    @param name  the file's or the folder's name, which may hold folders of its own
    @return      the path */
inline std::filesystem::path ScratchPath(const std::string &name)
{
    return ProcessScratch() / name;
}
} // namespace reckless::test
