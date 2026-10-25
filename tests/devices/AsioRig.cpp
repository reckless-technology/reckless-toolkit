// ASIO's test device: the SDK's sample driver (driver/asiosample, built beside this suite as
// reckless_asio_test_driver, with a clock of the suite's own - asio_driver/TestDriverClock.cpp) - sixteen inputs and
// sixteen outputs of 16-bit samples at 256 frames a block, 44.1 or 48 kHz, its first input carrying a sine and its
// second a sawtooth, the same 256 samples every block - registered by this binary for itself, with no administrator and
// nothing left behind on the machine.
//
// The registration is the hard part. The ASIO back end lists drivers through the SDK's host code
// (host/pc/asiolist.cpp), which reads HKLM\SOFTWARE\ASIO for each driver's class and HKCR\CLSID\<class>\InprocServer32
// for its DLL, once, as the back end's driver list (audio/win/AsioDevice.cpp) is built while the binary loads - before
// main, so before any test - and creates a driver later with CoCreateInstance. A CI runner's account cannot write HKLM,
// and a process at the System integrity level - a service's, as the runners' are - is not given COM's per-user classes.
// So, while that list is built:
//   - the registry's two roots are seen through views of the binary's own: RegOverridePredefKey points the local
//     machine's and the classes' roots, for this process alone, at keys in an application hive (RegLoadAppKey: a hive
//     file in the temporary folder, private to the process, loaded with no privilege) holding the test driver's
//     entries - and, copied beside them, the machine's own drivers' entries, so a person's interface still lists;
//   - the driver's class is found through an activation context - registration-free COM: a manifest naming the DLL and
//     its class, activated on the main thread, which CoCreateInstance consults before the registry, at any integrity
//     level - for every test after. The manifest is written beside the DLL: COM loads a class's DLL from the folder
//     of the manifest that names it, and a manifest elsewhere sends it looking for the DLL there (0x8007007E, module
//     not found), whatever directory the context is told its assembly is in.
// This object does the first part from the library's initialisation segment (init_seg(lib)), which the C runtime runs
// before every ordinary static initialiser, the ASIO back end's driver list among them; main puts the two roots back
// (FinishAsioRegistration) before anything else reads the registry. COM is started on the main thread before the
// roots are overridden, so its own start never reads the views, and the activation context stays active for the
// process's life.
#include "DeviceRig.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <objbase.h>

// The file's one static object, the registration, made in the library's segment of the C runtime's initialisers -
// ahead of every ordinary one, the ASIO back end's driver list among them - so that list is built through the views.
#pragma warning(disable : 4073) // "initializers put in library initialization area": the point
#pragma init_seg(lib)

namespace reckless::audio::test
{
namespace
{
constexpr const char    *kDriverName = "Reckless Test ASIO";  ///< the name the ASIO back end lists the test driver by
constexpr const wchar_t *kDriverKey  = L"Reckless Test ASIO"; ///< the driver's key under the ASIO list in the registry
constexpr const wchar_t *kClass      = L"{188135E1-D565-11d2-854F-00A0C99F5D19}"; ///< the SDK sample's own class

/** A narrow string in the ANSI code page as UTF-16, character for character: the build's path to the test driver is a
    narrow literal in the character set MSVC compiles to, that code page, so a folder named outside ASCII ('u' with
    two dots) reads as itself - widened a byte at a time, each byte sign-extended on its own, it named no file, and
    every ASIO test skipped, or failed where ASIO is required.
    @param text  the string, in the ANSI code page
    @return      the same characters in UTF-16; empty for an empty string */
std::wstring Wide(const std::string &text)
{
    if (text.empty())
    {
        return {};
    }
    const int    size = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0)
    {
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    }
    return wide;
}

/** A UTF-16 string as ASCII, for a message or a manifest.
    @param text  the string
    @return      the characters, each outside ASCII as a question mark */
std::string Narrow(const std::wstring &text)
{
    std::string out;
    for (const wchar_t c : text)
    {
        out += c < 128 ? static_cast<char>(c) : '?';
    }
    return out;
}

/** A failed Win32 call as a sentence.
    @param what  what was being done
    @param code  the call's error code
    @return      `what`, a colon and the system's message for the code, without its trailing line break */
std::string Win32Error(const char *what, LONG code)
{
    char *text = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(code), 0, reinterpret_cast<LPSTR>(&text), 0, nullptr);
    std::string message = std::string(what) + ": " + (text != nullptr ? text : "error " + std::to_string(code));
    LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
    {
        message.pop_back();
    }
    return message;
}

/** A string value written to an open registry key.
    @param key    the open key
    @param name   the value's name; nullptr for the key's default value
    @param value  the string to store, as REG_SZ
    @return       the registry call's status, ERROR_SUCCESS when written */
LONG SetString(HKEY key, const wchar_t *name, const std::wstring &value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

/** A string value read from an open registry key.
    @param key   the open key
    @param name  the value's name
    @return      the value; empty when it is missing or not a REG_SZ */
std::wstring GetString(HKEY key, const wchar_t *name)
{
    wchar_t buffer[1024] = {};
    DWORD   size         = sizeof(buffer) - sizeof(wchar_t);
    DWORD   type         = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(buffer), &size) != ERROR_SUCCESS ||
        type != REG_SZ)
    {
        return {};
    }
    return buffer;
}

/** The registration, made as the binary loads (the file's comment says why and how). */
class Registration
{
  public:
    /** Find the sample driver the build made, start COM on this thread, and register the driver in registry views of
        the process's own (MakeViews, Override, Activate). It leaves `registered_` true on success; on any failure it
        returns early with `registered_` false, and `error_` says why where the driver was not found. */
    Registration()
    {
#if defined(RECKLESS_ASIO_TEST_DRIVER)
        dll_ = Wide(RECKLESS_ASIO_TEST_DRIVER);
#endif
        if (dll_.empty() || GetFileAttributesW(dll_.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            error_ = "the ASIO test driver is not where the build put it (" + Narrow(dll_) + ")";
            return;
        }
        for (auto &c : dll_)
        {
            c = c == L'/' ? L'\\' : c;
        }
        // COM started on this, the main, thread before the roots are overridden: the back end's driver list starts it
        // as it is built, and COM is not to meet the views. The list's own start is then a nested one.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (!MakeViews() || !Override() || !Activate())
        {
            return;
        }
        registered_ = true;
    }

    /** The two roots put back: the back end's list is built, and nothing else is to see the views. */
    void Finish()
    {
        if (overridden_)
        {
            RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
            RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
            overridden_ = false;
        }
        for (HKEY *key : {&hklm_, &hkcr_, &hive_})
        {
            if (*key != nullptr)
            {
                RegCloseKey(*key);
                *key = nullptr;
            }
        }
        for (const wchar_t *suffix : {L"", L".LOG1", L".LOG2"})
        {
            DeleteFileW((hive_file_ + suffix).c_str());
        }
        DeleteFileW(manifest_.c_str());
        if (registered_)
        {
            // The class created and released as the back end's driver list will create it - the class as its own
            // interface - through the activation context, the roots being the machine's again.
            CLSID     clsid  = {};
            IUnknown *driver = nullptr;
            CLSIDFromString(kClass, &clsid);
            created_ =
                CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void **>(&driver));
            if (SUCCEEDED(created_) && driver != nullptr)
            {
                driver->Release();
            }
            else
            {
                registered_ = false;
                error_      = "its class did not create through the activation context (" + Hex(created_) + ")";
            }
        }
    }

    /** An HRESULT as text.
        @param hr  the result code
        @return    the code as `0x` and eight hexadecimal digits */
    static std::string Hex(HRESULT hr)
    {
        char text[16];
        std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(hr));
        return text;
    }

    /** Whether the test driver is registered.
        @return  true when the views and the activation context are in place and its class creates */
    bool registered() const
    {
        return registered_;
    }

    /** Why the registration failed.
        @return  the reason; empty when nothing failed */
    const std::string &error() const
    {
        return error_;
    }

    /** What the registration did, for the header the suite prints.
        @return  the report, or why nothing was registered */
    std::string Report() const
    {
        if (!registered_)
        {
            return "not registered: " + error_;
        }
        return "the SDK's sample driver (" + Narrow(dll_) + ") registered in-process as '" + kDriverName +
               "': registry views over HKLM and HKCR from an application hive while the ASIO back end built its driver "
               "list (" +
               views_ +
               "), its class from an activation context, a manifest beside the DLL (created: " + Hex(created_) + "); " +
               std::to_string(machine_drivers_) + " of the machine's own drivers listed beside it";
    }

    /** The test driver's DLL.
        @return  its full path, with backslashes; empty when the build named none */
    const std::wstring &dll() const
    {
        return dll_;
    }

  private:
    /** The application hive with the two views: `SOFTWARE\ASIO` as HKLM shows it and `CLSID` as HKCR does, the test
        driver's entries and the machine's own drivers' copied from the real roots.
        @return  true when the hive and the views are made; `error_` says why when not */
    bool MakeViews()
    {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring stem = std::wstring(temp) + L"reckless-asio-test-" + std::to_wstring(GetCurrentProcessId());
        hive_file_              = stem + L".hive";
        for (const wchar_t *suffix : {L"", L".LOG1", L".LOG2"})
        {
            DeleteFileW((hive_file_ + suffix).c_str()); // a run that died, under this process id, left these
        }
        LONG result = RegLoadAppKeyW(hive_file_.c_str(), &hive_, KEY_ALL_ACCESS, 0, 0);
        if (result != ERROR_SUCCESS)
        {
            error_ = Win32Error("RegLoadAppKey", result);
            return false;
        }
        HKEY asio = nullptr, classes = nullptr, entry = nullptr, server = nullptr;
        result = RegCreateKeyExW(hive_, L"HKLM", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &hklm_, nullptr);
        result = result == ERROR_SUCCESS
                     ? RegCreateKeyExW(hive_, L"HKCR", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &hkcr_, nullptr)
                     : result;
        result = result == ERROR_SUCCESS
                     ? RegCreateKeyExW(hklm_, L"SOFTWARE\\ASIO", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &asio, nullptr)
                     : result;
        result = result == ERROR_SUCCESS
                     ? RegCreateKeyExW(hkcr_, L"CLSID", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &classes, nullptr)
                     : result;
        if (result != ERROR_SUCCESS)
        {
            error_ = Win32Error("creating the registry views", result);
            return false;
        }

        // The machine's own drivers first, so the test driver's entries are what its class's key holds.
        HKEY real_asio = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &real_asio) == ERROR_SUCCESS)
        {
            wchar_t name[256];
            for (DWORD i = 0;; ++i)
            {
                DWORD length = 256;
                if (RegEnumKeyExW(real_asio, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                {
                    break;
                }
                HKEY real_entry = nullptr, copy = nullptr;
                if (RegOpenKeyExW(real_asio, name, 0, KEY_READ, &real_entry) != ERROR_SUCCESS)
                {
                    continue;
                }
                const std::wstring clsid = GetString(real_entry, L"CLSID");
                if (RegCreateKeyExW(asio, name, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &copy, nullptr) ==
                    ERROR_SUCCESS)
                {
                    RegCopyTreeW(real_entry, nullptr, copy);
                    RegCloseKey(copy);
                }
                RegCloseKey(real_entry);
                HKEY               real_server = nullptr, server_copy = nullptr;
                const std::wstring path = L"CLSID\\" + clsid + L"\\InprocServer32";
                if (!clsid.empty() &&
                    RegOpenKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &real_server) == ERROR_SUCCESS)
                {
                    if (RegCreateKeyExW(hkcr_, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &server_copy,
                                        nullptr) == ERROR_SUCCESS)
                    {
                        RegCopyTreeW(real_server, nullptr, server_copy);
                        RegCloseKey(server_copy);
                        ++machine_drivers_;
                    }
                    RegCloseKey(real_server);
                }
            }
            RegCloseKey(real_asio);
        }

        // The test driver: its entry under SOFTWARE\ASIO - its class and the name the back end lists it by - and its
        // DLL under its class.
        result = RegCreateKeyExW(asio, kDriverKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &entry, nullptr);
        if (result == ERROR_SUCCESS)
        {
            SetString(entry, L"CLSID", kClass);
            SetString(entry, L"Description", Wide(kDriverName));
            RegCloseKey(entry);
            const std::wstring path = std::wstring(kClass) + L"\\InprocServer32";
            result = RegCreateKeyExW(classes, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &server, nullptr);
        }
        if (result == ERROR_SUCCESS)
        {
            SetString(server, nullptr, dll_);
            SetString(server, L"ThreadingModel", L"Apartment");
            RegCloseKey(server);
        }
        RegCloseKey(asio);
        RegCloseKey(classes);
        if (result != ERROR_SUCCESS)
        {
            error_ = Win32Error("writing the test driver's entries", result);
            return false;
        }
        return true;
    }

    /** The two roots overridden, for this process, with the views.
        @return  true when both are overridden; `error_` says why when not */
    bool Override()
    {
        LONG result = RegOverridePredefKey(HKEY_LOCAL_MACHINE, hklm_);
        if (result == ERROR_SUCCESS)
        {
            result = RegOverridePredefKey(HKEY_CLASSES_ROOT, hkcr_);
            if (result != ERROR_SUCCESS)
            {
                RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
            }
        }
        if (result != ERROR_SUCCESS)
        {
            error_ = Win32Error("RegOverridePredefKey", result);
            return false;
        }
        overridden_ = true;
        views_      = CheckViews();
        return true;
    }

    /** What a reader of the two roots sees through the views, read as the SDK's host code reads them - the ANSI calls
        on the predefined roots: the drivers under `SOFTWARE\ASIO` and the test driver's DLL under its class.
        @return  what the views answer, or what did not answer */
    static std::string CheckViews()
    {
        HKEY asio   = nullptr;
        LONG result = RegOpenKeyA(HKEY_LOCAL_MACHINE, "software\\asio", &asio);
        if (result != ERROR_SUCCESS)
        {
            return Win32Error("the HKLM view does not answer for software\\asio", result);
        }
        DWORD drivers = 0;
        RegQueryInfoKeyA(asio, nullptr, nullptr, nullptr, &drivers, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr);
        RegCloseKey(asio);
        HKEY              server = nullptr;
        const std::string path   = "clsid\\" + Narrow(kClass) + "\\InprocServer32";
        result                   = RegOpenKeyA(HKEY_CLASSES_ROOT, path.c_str(), &server);
        if (result != ERROR_SUCCESS)
        {
            return Win32Error("the HKCR view does not answer for the test driver's class", result);
        }
        char       dll[MAX_PATH] = {};
        LONG       size          = sizeof(dll) - 1;
        const LONG read          = RegQueryValueA(server, nullptr, dll, &size);
        RegCloseKey(server);
        return "the views answer " + std::to_string(drivers) + " driver(s) under HKLM\\SOFTWARE\\ASIO and " +
               (read == ERROR_SUCCESS ? "'" + std::string(dll) + "'" : std::string("no DLL")) +
               " under the test driver's class";
    }

    /** Registration-free COM: the driver's class from a manifest beside nothing, its DLL found in the DLL's folder.
        @return  true when the manifest is written and its context active; `error_` says why when not */
    bool Activate()
    {
        const std::size_t  slash  = dll_.find_last_of(L'\\');
        const std::wstring folder = dll_.substr(0, slash);
        const std::wstring file   = dll_.substr(slash + 1);
        manifest_ = folder + L"\\reckless-asio-test-" + std::to_wstring(GetCurrentProcessId()) + L".manifest";
        {
            std::ofstream out(manifest_.c_str(), std::ios::binary);
            out << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                << "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\r\n"
                << "  <assemblyIdentity type=\"win32\" name=\"RecklessTechnology.AsioTestDriver\" "
                   "version=\"1.0.0.0\"/>\r\n"
                << "  <file name=\"" << Narrow(file) << "\">\r\n"
                << "    <comClass clsid=\"" << Narrow(kClass) << "\" threadingModel=\"Apartment\"/>\r\n"
                << "  </file>\r\n"
                << "</assembly>\r\n";
            if (!out)
            {
                error_ = "could not write the activation context's manifest at " + Narrow(manifest_);
                return false;
            }
        }
        ACTCTXW context  = {};
        context.cbSize   = sizeof(context);
        context.lpSource = manifest_.c_str();
        HANDLE handle    = CreateActCtxW(&context);
        if (handle == INVALID_HANDLE_VALUE)
        {
            error_ = Win32Error("CreateActCtx", static_cast<LONG>(GetLastError()));
            return false;
        }
        ULONG_PTR cookie = 0;
        if (!ActivateActCtx(handle, &cookie))
        {
            error_ = Win32Error("ActivateActCtx", static_cast<LONG>(GetLastError()));
            ReleaseActCtx(handle);
            return false;
        }
        return true;
    }

    std::wstring dll_;                ///< the test driver's DLL, from the build's path
    std::wstring hive_file_;          ///< the application hive's file in the temporary folder
    std::wstring manifest_;           ///< the activation context's manifest, beside the DLL
    HKEY         hive_{nullptr};      ///< the application hive's root
    HKEY         hklm_{nullptr};      ///< the key that stands for HKEY_LOCAL_MACHINE
    HKEY         hkcr_{nullptr};      ///< the key that stands for HKEY_CLASSES_ROOT
    bool         overridden_{false};  ///< whether the two roots are overridden
    bool         registered_{false};  ///< whether the driver is registered
    int          machine_drivers_{0}; ///< the machine's own drivers copied into the views
    std::string  views_;              ///< what the views answered when checked
    HRESULT      created_{E_PENDING}; ///< the result of creating the driver's class once the roots are back
    std::string  error_;              ///< why the registration failed; empty when nothing did
};

Registration g_registration; ///< the file's one registration, made as the binary loads

/** The test driver's request function: 1 reports an xrun, 2 the device gone, at the driver's next block. */
using Request = void (*)(int);

/** The running test driver's request function, from the DLL COM loaded (or loads now, the same module).
    @return  the function; nullptr when the DLL does not export it */
Request DriverRequest()
{
    HMODULE module = GetModuleHandleW(g_registration.dll().c_str());
    if (module == nullptr)
    {
        module = LoadLibraryW(g_registration.dll().c_str());
    }
    return module != nullptr ? reinterpret_cast<Request>(reinterpret_cast<void *>(
                                   GetProcAddress(module, "reckless_asio_test_driver_request")))
                             : nullptr;
}

/** ASIO's rig: the SDK's sample driver, registered in-process. */
class AsioRig final : public DeviceRig
{
  public:
    TestDevice Find(const std::vector<DeviceInfo> &devices) override
    {
        TestDevice device;
        if (!g_registration.registered())
        {
            device.missing = "the ASIO test driver could not be registered in-process: " + g_registration.error();
            return device;
        }
        for (const DeviceInfo &info : devices)
        {
            if (info.name != kDriverName)
            {
                continue;
            }
            device.found          = true;
            device.output         = info.name;
            device.input          = info.name;
            device.engine_input   = info.name;
            device.channels       = 2;
            device.sample_rate    = 48000;
            device.block_size     = 256;
            device.loop           = Loop::kKnownInput;
            device.xrun_by        = XrunBy::kDevice;
            device.rate_tolerance = 0.03;
            device.about = "the SDK's sample driver, registered in-process: 16 in, 16 out, 16-bit, 256 frames, a sine "
                           "and a sawtooth on its first two inputs";
            return device;
        }
        device.missing =
            std::string("the ASIO test driver was registered in-process but the ASIO back end does not list '") +
            kDriverName + "' (" + g_registration.Report() + ")";
        return device;
    }

    /** The driver asks to be reset, as one that has lost its device does: the back end tells the stream it failed, and
       the engine closes it from its own and reports the disconnection.
        @param why  set to the reason when the driver's request function was not found
        @return     false when the request could not be made */
    bool Vanish(std::string &why) override
    {
        const Request request = DriverRequest();
        if (request == nullptr)
        {
            why = "the ASIO test driver's request function was not found";
            return false;
        }
        request(2);
        return true;
    }

    /** Nothing to bring back: the driver is still registered, and opens again.
        @param why  cleared
        @return     true, always */
    bool Return(std::string &why) override
    {
        why.clear();
        return true;
    }

    /** The driver reports a resync request, as one does that has lost a buffer.
        @param why  set to the reason when the driver's request function was not found
        @return     false when the request could not be made */
    bool Xrun(std::string &why) override
    {
        const Request request = DriverRequest();
        if (request == nullptr)
        {
            why = "the ASIO test driver's request function was not found";
            return false;
        }
        request(1);
        return true;
    }
};
} // namespace

void FinishAsioRegistration()
{
    g_registration.Finish();
}

std::string AsioRegistrationReport()
{
    return g_registration.Report();
}

/** The sample driver's two waves exactly as it makes them (driver/asiosample/asiosmpl.cpp, makeSine and makeSaw, with
    its own value of pi), each sample then read as the back end reads an int16: over 32768. */
std::vector<std::vector<float>> AsioTestDriverWaves()
{
    const double       pi     = 0.3141592654; // the sample driver's constant, as it has it
    const double       frames = 256.0;
    std::vector<float> sine, saw;
    double             f = (pi * 2.) / frames;
    for (double i = 0; i < frames; i++)
    {
        sine.push_back(static_cast<float>(static_cast<short>(static_cast<double>(0x7fff) * std::sin(f * i))) / 32768.f);
    }
    f = 2. / frames;
    for (double i = 0; i < frames; i++)
    {
        saw.push_back(static_cast<float>(static_cast<short>(static_cast<double>(0x7fff) * (-1. + f * i))) / 32768.f);
    }
    return {sine, saw};
}

std::unique_ptr<DeviceRig> MakeAsioRig()
{
    return std::make_unique<AsioRig>();
}
} // namespace reckless::audio::test
