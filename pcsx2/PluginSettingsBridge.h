#pragma once

// Included by the fork's existing host implementation; no build-system entry.
// File ownership, validation and IO remain in the injector, on the CPU thread.
namespace PCSX2F
{
inline u32 InvokePluginSettings(u32 request, u32 caller)
{
#ifdef _WIN32
    using Service = u32 (*)(u32, u32);
    const HMODULE module = GetModuleHandleW(L"PCSX2PluginInjector.asi");
    const auto service = module ? reinterpret_cast<Service>(GetProcAddress(module, "InvokeGuestPluginSettings")) : nullptr;
    return service ? service(request, caller) : 1u;
#else
    return 1u;
#endif
}
}
#ifdef _WIN32
extern "C" __declspec(dllexport) void PCSX2F_PluginSettingsSupported() {}
#endif
