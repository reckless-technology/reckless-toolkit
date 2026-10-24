#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace reckless::core
{
/** An environment variable's value, read the way each platform's runtime reads it safely: MSVC's runtime deprecates
    std::getenv, whose pointer another thread's change to the environment may free, for _dupenv_s, which hands back a
    copy; every other runtime's std::getenv is read and copied at once.
    @param name  the variable's name
    @return      its value, empty when it is set to nothing, or none when it is not set */
inline std::optional<std::string> EnvironmentVariable(const char *name)
{
#if defined(_MSC_VER)
    char       *value  = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
    {
        return std::nullopt;
    }
    std::string text(value);
    std::free(value);
    return text;
#else
    const char *value = std::getenv(name);
    if (value == nullptr)
    {
        return std::nullopt;
    }
    return std::string(value);
#endif
}
} // namespace reckless::core
