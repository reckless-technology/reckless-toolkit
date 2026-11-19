#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#if defined(__linux__)
#include <cerrno>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace reckless::window
{
/**
    What opening a file of the person's machine in the system's own handler asks of every platform (OpenLocalFile,
    Window.h), and the Linux spawn both that and OpenInBrowser stand on. Header only and free of the window, so a suite
    holds it with no display (tests/test_local_file.cpp).

    **Security.** The system's handler runs whatever its association says a file is: a `.html` file opens in the
    browser, but an executable, a script or a shortcut is started or edited by the shell, with the person's rights.
    So a product must only ever hand OpenLocalFile a path it built itself - a file it installed, in a folder it
    chose - and never one taken from a document, a network message, a command line or a dialog's free text. A refusal
    here (the path is not an existing regular file) is a guard against a mistake, not against a hostile path.
*/

/** The absolute path of the existing regular file `path` names, or nothing: the check that comes before anything is
    spawned or handed to a shell. A path that is empty, holds a NUL (a C string would end at it and name another file),
    does not exist, or is a folder, a device, a socket or a pipe is refused; a symbolic link is followed and the file it
    reaches must be a regular one. A relative path is made absolute against the working directory as the system reports
   it, and an absolute one is passed as it was spelled, but for its separators, which are the platform's own
    (Windows makes '/' a backslash): nothing is canonicalised, so a symbolic link in the way (macOS's
    /var) stays as the product wrote it. What reaches an opener therefore begins with a root - '/' on Linux and macOS,
    where no option or scheme can be read from it, and a drive or share on Windows, where the shell is given the whole
    wide path and no parameters.
    @param path  the file's path, as the platform spells it
    @return      the absolute path, or nothing where it is not an existing regular file */
inline std::optional<std::filesystem::path> ExistingRegularFile(const std::filesystem::path &path)
{
    if (path.empty() || path.native().find('\0') != std::filesystem::path::string_type::npos)
    {
        return std::nullopt;
    }
    std::error_code       error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error || !absolute.has_root_path() || !std::filesystem::is_regular_file(absolute, error) || error)
    {
        return std::nullopt;
    }
    return absolute;
}

#if defined(__linux__)
/** `target` handed to `opener` - found on the PATH, as `execvp` finds it - as its one argument, in a process of its own
    started from a child that leaves at once, so the caller waits for nothing and keeps no child behind. The target is
    one argv entry whatever it holds - spaces, quotes, non-ASCII bytes - because no shell and no command line is
    involved. This is the spawn OpenInBrowser makes with `xdg-open`.
    @param opener  the program's name
    @param target  a folder's or a file's path, or a URL
    @return        true when the opener started; false when no child could be made or the program would not execute
                   (not found, not executable) - the opener's own outcome, a handler that fails after it started, is
                   not seen */
inline bool SpawnDesktopOpener(const char *opener, std::string target)
{
    std::string program = opener;
    int         report[2];
    if (pipe2(report, O_CLOEXEC) != 0)
    {
        return false;
    }
    const pid_t child = fork();
    if (child == 0)
    {
        setsid();
        if (fork() == 0)
        {
            char *argv[] = {program.data(), target.data(), nullptr};
            execvp(argv[0], argv);
            const int  failure = errno; // exec's failure, told to the caller through the pipe that closes on success
            const auto ignored = write(report[1], &failure, sizeof(failure));
            static_cast<void>(ignored);
            _exit(127);
        }
        _exit(0);
    }
    close(report[1]);
    bool is_started = child > 0;
    if (child > 0)
    {
        waitpid(child, nullptr, 0);
        int     failure = 0;
        ssize_t got;
        do
        {
            got = read(report[0], &failure, sizeof(failure));
        } while (got < 0 && errno == EINTR);
        is_started = got == 0; // end of file with nothing written: the exec closed the pipe
    }
    close(report[0]);
    return is_started;
}

/** A file opened by `xdg-open`, the Linux half of OpenLocalFile, after ExistingRegularFile has said it is one.
    @param path  the file's path
    @return      true when xdg-open started on the file's absolute path as its one argument */
inline bool OpenLocalFileWithXdgOpen(const std::filesystem::path &path)
{
    const std::optional<std::filesystem::path> file = ExistingRegularFile(path);
    return file.has_value() && SpawnDesktopOpener("xdg-open", file->native());
}
#endif
} // namespace reckless::window
