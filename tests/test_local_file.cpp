// Opening an installed file in the system's handler (window/LocalFileOpening.h, window::OpenLocalFile): which paths are
// refused before anything is spawned, on every platform, and on Linux the spawn itself against a stub `xdg-open` put
// first on the PATH - the path arrives as one argument, spaces and non-ASCII bytes intact, and nothing is spawned for a
// path that is not an existing regular file. Nothing here opens a window, a browser or a real handler.
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include "LocalFileOpening.h"
#include "ScratchPath.h"

#include <gtest/gtest.h>

#if defined(__linux__)
#include <chrono>
#include <thread>

#include <sys/stat.h>
#endif

namespace
{
namespace fs = std::filesystem;
using reckless::test::ScratchPath;
using reckless::window::ExistingRegularFile;

/** A file made with `text` in it.
    @param path  where
    @param text  its content */
void Write(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
} // namespace

TEST(LocalFile, AnExistingRegularFileIsAcceptedAsAnAbsolutePath)
{
    const fs::path file = ScratchPath("accepted/User Guide.html");
    Write(file, "<html></html>");
    const auto found = ExistingRegularFile(file);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, file);
    EXPECT_TRUE(found->is_absolute());
}

TEST(LocalFile, ARelativePathIsMadeAbsoluteAgainstTheWorkingDirectoryAndNeverReadsAsAnOption)
{
    // The path is made absolute, not canonicalised: it is spelled from the working directory as the system reports it
    // (which may reach a folder through a different name than the one the test made it by, as macOS's /var is
    // /private/var), so the files are compared by identity, not by spelling.
    const fs::path folder = ScratchPath("relative");
    Write(folder / "-guide.html", "x");
    const fs::path before = fs::current_path();
    fs::current_path(folder);
    const auto dash = ExistingRegularFile("-guide.html");
#if !defined(_WIN32)
    Write(folder / "mailto:guide.html", "x"); // a colon is a stream separator in a Windows file name
    const auto scheme = ExistingRegularFile("mailto:guide.html");
#endif
    fs::current_path(before);
    ASSERT_TRUE(dash.has_value());
    EXPECT_TRUE(dash->is_absolute());
    EXPECT_NE(dash->native().front(), '-') << "what an opener is given never begins as an option";
    EXPECT_TRUE(fs::equivalent(*dash, folder / "-guide.html"));
#if !defined(_WIN32)
    ASSERT_TRUE(scheme.has_value());
    EXPECT_EQ(scheme->native().front(), '/') << "begins at the root, so no scheme can be read from its start";
    EXPECT_TRUE(fs::equivalent(*scheme, folder / "mailto:guide.html"));
#endif
}

TEST(LocalFile, AnAbsolutePathIsPassedWholeAsItWasSpelled)
{
    // On Windows that is a drive letter first: C:\Program Files\...\user-guide.html is accepted and kept whole.
    const fs::path file = ScratchPath("whole/Program Files/user-guide.html");
    Write(file, "x");
    const auto found = ExistingRegularFile(file);
    ASSERT_TRUE(found.has_value());
    // Kept as spelled but for separators, which the platform may make its own (Windows: '/' to '\\'), so the
    // expectation is spelled the same way; and it is one path, spaces and all, not split.
    fs::path expected = file;
    EXPECT_EQ(found->native(), expected.make_preferred().native()) << "not rewritten, canonicalised or split";
    EXPECT_NE(found->native().find(fs::path("Program Files").native()), fs::path::string_type::npos);
    EXPECT_TRUE(fs::equivalent(*found, file));
}

TEST(LocalFile, ARefusedPathIsNothing)
{
    const fs::path folder = ScratchPath("refused");
    fs::create_directories(folder / "a folder");
    EXPECT_FALSE(ExistingRegularFile(folder / "missing.html").has_value()) << "no such file";
    EXPECT_FALSE(ExistingRegularFile(folder / "a folder").has_value()) << "a folder is not a file";
    EXPECT_FALSE(ExistingRegularFile(fs::path()).has_value()) << "empty";
    Write(folder / "real.html", "x");
    const std::string with_nul = (folder / "real.html").string() + std::string(1, '\0') + "ignored";
    EXPECT_FALSE(ExistingRegularFile(fs::path(with_nul)).has_value()) << "a C string would end at the NUL";
    EXPECT_FALSE(ExistingRegularFile("http://example.com/guide.html").has_value()) << "a URL is no file";
}

#if defined(__linux__)
namespace
{
/** Puts a stub `xdg-open` first on the PATH for the test's life, writing what it was given - the count of its
    arguments, and each one's bytes followed by a NUL - to a file, and puts the PATH back. */
class StubOpener
{
  public:
    /** Makes the stub in its own folder and puts that folder first on the PATH.
        @param name  the folder's name under the process's scratch */
    explicit StubOpener(const std::string &name)
        : folder_(ScratchPath(name)), record_(folder_ / "arguments"), marker_(folder_ / "started")
    {
        fs::create_directories(folder_);
        const char *old       = std::getenv("PATH");
        old_path_             = old != nullptr ? old : "";
        had_path_             = old != nullptr;
        const fs::path script = folder_ / "xdg-open";
        Write(script, "#!/bin/sh\nprintf '%s' \"$#\" > '" + record_.string() +
                          "'\nfor argument in \"$@\"; do printf '\\0%s' \"$argument\" >> '" + record_.string() +
                          "'; done\n: > '" + marker_.string() + "'\n");
        chmod(script.c_str(), 0755);
        setenv("PATH", (folder_.string() + ":" + old_path_).c_str(), 1);
    }

    ~StubOpener()
    {
        if (had_path_)
        {
            setenv("PATH", old_path_.c_str(), 1);
        }
        else
        {
            unsetenv("PATH");
        }
    }

    StubOpener(const StubOpener &)            = delete;
    StubOpener &operator=(const StubOpener &) = delete;

    /** Whether the stub ran, waiting up to five seconds for it: it runs detached after the call returns.
        @return  true when it ran */
    bool DidRun() const
    {
        for (int i = 0; i < 500 && !fs::exists(marker_); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return fs::exists(marker_);
    }

    /** What the stub recorded: its argument count, then each argument, NUL-separated.
        @return  the bytes */
    std::string Recorded() const
    {
        std::ifstream in(record_, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

  private:
    fs::path    folder_;
    fs::path    record_;
    fs::path    marker_;
    std::string old_path_;
    bool        had_path_{false};
};
} // namespace

TEST(LocalFile, TheStubOpenerIsGivenTheFileAsOneArgumentWithSpacesAndNonAsciiIntact)
{
    const fs::path file = ScratchPath("spawn/User Guide \xc3\xa9\xe6\x97\xa5\xe6\x9c\xac & 'q' $HOME;.html");
    Write(file, "<html></html>");
    StubOpener stub("spawn-stub");
    EXPECT_TRUE(reckless::window::OpenLocalFileWithXdgOpen(file));
    ASSERT_TRUE(stub.DidRun());
    EXPECT_EQ(stub.Recorded(), std::string("1") + std::string(1, '\0') + file.string())
        << "one argument, whose bytes are the path's";
}

TEST(LocalFile, AMissingFileIsRefusedBeforeAnythingIsSpawned)
{
    StubOpener stub("refuse-stub");
    EXPECT_FALSE(reckless::window::OpenLocalFileWithXdgOpen(ScratchPath("refuse/missing.html")));
    fs::create_directories(ScratchPath("refuse/folder"));
    EXPECT_FALSE(reckless::window::OpenLocalFileWithXdgOpen(ScratchPath("refuse/folder")));
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // time enough for a stub wrongly started to run
    EXPECT_FALSE(fs::exists(ScratchPath("refuse-stub/started"))) << "the opener must not have been started";
}

TEST(LocalFile, AnOpenerThatCannotBeRunIsReportedAsAFailure)
{
    const fs::path file = ScratchPath("noopener/guide.html");
    Write(file, "x");
    const char       *old   = std::getenv("PATH");
    const std::string saved = old != nullptr ? old : "";
    setenv("PATH", ScratchPath("noopener/empty-path").c_str(), 1); // a PATH with no xdg-open on it
    const bool is_started = reckless::window::OpenLocalFileWithXdgOpen(file);
    setenv("PATH", saved.c_str(), 1);
    EXPECT_FALSE(is_started);
}
#endif
