// The ASIO test driver's entry point: the C runtime calls DllMain, and the SDK's COM scaffolding (common/dllentry.cpp)
// names its entry DllEntryPoint, as the sample's own project set the linker's entry to it. This hands one to the other,
// so the scaffolding sees the module attach and detach as it expects.
#include <windows.h>

BOOL WINAPI DllEntryPoint(HINSTANCE instance, ULONG reason, LPVOID reserved);

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    return DllEntryPoint(instance, reason, reserved);
}
