#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define Error SnapshotTestError
#define main StartupRuntimeTestsMain
#include "RuntimeTests.cpp"
#undef main

namespace PCSX2F { void GuestRenderResetState() {} }
namespace Host { bool IsFullscreen() { return false; } }
struct TestWindowInfo { void* window_handle = nullptr; u32 surface_width = 640, surface_height = 480; };
struct TestGSDevice { TestWindowInfo info; const TestWindowInfo& GetWindowInfo() const { return info; } };
static TestGSDevice test_gs_device;
static TestGSDevice* g_gs_device = &test_gs_device;
static struct { u32 AspectRatio = 0; } GSConfig;
static std::vector<u32> state_operations;
static u32 rejected_operation = 0xffffffff;
static bool simulated_installed_set_differs = false;
static int32_t SnapshotTransfer(u32 operation, void* buffer, u32 size, char*, u32)
{
    state_operations.push_back(operation);
    if (operation == rejected_operation) return -1;
    if (operation == 0)
    {
        Check(size >= 4, "snapshot manifest buffer too short");
        std::memcpy(buffer, "test", 4);
        return 4;
    }
    if (operation == 3)
    {
        const auto* context = static_cast<const uintptr_t*>(buffer);
        Check(size == sizeof(uintptr_t) * 7 && context[0] == reinterpret_cast<uintptr_t>(eeMem->Main) &&
            context[1] == Ps2MemSize::ExposedRam, "restore service context invalid");
        return simulated_installed_set_differs ? 1 : 0;
    }
    return 0;
}
static HMODULE SnapshotModule(const wchar_t*) { return reinterpret_cast<HMODULE>(1); }
static FARPROC SnapshotProc(HMODULE, const char* name)
{
    return std::strcmp(name, "GuestPluginStateTransfer") == 0 ? reinterpret_cast<FARPROC>(SnapshotTransfer) : nullptr;
}
#define GetModuleHandleW SnapshotModule
#define LoadLibraryW SnapshotModule
#define GetProcAddress SnapshotProc
#include "../../pcsx2/PluginModuleState.inc"
#undef GetModuleHandleW
#undef LoadLibraryW
#undef GetProcAddress

int main()
{
    if (StartupRuntimeTestsMain()) return 1;
    using namespace VMManager::Internal;
    try
    {
        GuestModuleSnapshot source;
        source.committed = true; source.running = true; source.thread_ready = true;
        source.modules = {{0x02001000, 0x02010000, 0x12345678, 0x02002000}, {0x02020000, 0x02030000, 0, 0x02022000}};
        source.index = 1; source.resume_pc = 0x123450; source.thread_return = 0x124560;
        source.heap_return = 0x125670; source.memory_return = 0x126780;
        source.generation = 0x0123456789abcdef; source.sa = 7;
        std::memset(&source.gpr, 0x5a, sizeof(source.gpr));
        std::memset(&source.hi, 0x61, sizeof(source.hi));
        std::memset(&source.lo, 0x72, sizeof(source.lo));
        std::memset(&source.fpu, 0x83, sizeof(source.fpu));
        source.manifest = {1, 2, 3, 4};
        auto encode = [](GuestModuleSnapshot snapshot) {
            StateWrapper::VectorMemoryStream stream;
            StateWrapper sw(&stream, StateWrapper::Mode::Write, 0);
            Check(snapshot.DoState(sw), "snapshot encoding failed");
            return stream.GetBuffer();
        };
        const auto bytes = encode(source);
        GuestModuleSnapshot restored;
        StateWrapper::ReadOnlyMemoryStream stream(bytes.data(), static_cast<u32>(bytes.size()));
        StateWrapper sw(&stream, StateWrapper::Mode::Read, 0);
        Check(restored.DoState(sw), "snapshot decoding failed");
        Check(encode(restored) == bytes, "snapshot round trip lost bookkeeping or saved registers");
        for (u32 length = 0; length < bytes.size(); ++length)
        {
            GuestModuleSnapshot truncated;
            StateWrapper::ReadOnlyMemoryStream short_stream(bytes.data(), length);
            StateWrapper reader(&short_stream, StateWrapper::Mode::Read, 0);
            Check(!truncated.DoState(reader), "truncated snapshot accepted");
        }
        Error error;
        auto corrupt = bytes; corrupt[0] = 2;
        Check(!PrepareGuestModuleState(corrupt, &error), "unknown snapshot version accepted");
        corrupt = bytes; corrupt.push_back(0);
        Check(!PrepareGuestModuleState(corrupt, &error), "trailing snapshot bytes accepted");
        Check(PrepareGuestModuleState({}, &error), "legacy snapshot rejected");
        s_guest_modules_committed = false;
        s_guest_modules.clear();
        eeMem->Main[MODULE_ARENA_BEGIN + 0x1000] = 0xab;
        Check(BeginGuestModuleStateLoad(&error) && FinishGuestModuleStateLoad(&error), "plugin-free restore failed");
        Check(eeMem->Main[MODULE_ARENA_BEGIN + 0x1000] == 0 && !s_guest_modules_committed && s_guest_modules.empty(), "legacy restore left guest module memory or bookkeeping");
        s_guest_modules = source.modules;
        s_guest_modules_committed = source.committed;
        s_guest_module_running = source.running;
        s_guest_thread_ready = source.thread_ready;
        s_guest_module_index = source.index;
        s_guest_resume_pc = source.resume_pc;
        s_guest_thread_return_pc = source.thread_return;
        s_guest_heap_return_pc = source.heap_return;
        s_guest_memory_return_pc = source.memory_return;
        s_guest_saved_sa = source.sa;
        s_guest_module_generation = source.generation;
        s_guest_saved_gpr = source.gpr; s_guest_saved_hi = source.hi; s_guest_saved_lo = source.lo;
        s_guest_saved_fpu = source.fpu;
        state_operations.clear();
        std::vector<u8> active;
        Check(SaveGuestModuleState(active, &error), "active snapshot capture failed");
        ResetGuestModules();
        const auto* cached_api = GetGuestPluginHostApi(PCSX2F_HOST_VERSION, sizeof(PCSX2FGuestHostV1));
        const u64 generation_before_restore = cached_api->generation;
        Check(PrepareGuestModuleState(active, &error) && BeginGuestModuleStateLoad(&error) && FinishGuestModuleStateLoad(&error), "active snapshot restore failed");
        Check(cached_api->generation == s_guest_module_generation && cached_api->generation > generation_before_restore, "restore did not publish a fresh host address-cache generation");
        std::vector<u8> roundtrip;
        GuestModuleSnapshot expected;
        StateWrapper::ReadOnlyMemoryStream expected_stream(active.data(), static_cast<u32>(active.size()));
        StateWrapper expected_reader(&expected_stream, StateWrapper::Mode::Read, 0);
        Check(expected.DoState(expected_reader), "captured state could not be inspected");
        expected.generation = s_guest_module_generation;
        Check(SaveGuestModuleState(roundtrip, &error) && roundtrip == encode(expected), "active restore lost host fields");
        Check(state_operations == std::vector<u32>({0, 1, 5, 2, 3, 0}), "restore transaction ordering changed");
        Check(Host::osd_key != "guest-plugin-state-versions", "matching plugins displayed a note");
        simulated_installed_set_differs = true;
        Check(PrepareGuestModuleState(active, &error) && BeginGuestModuleStateLoad(&error) && FinishGuestModuleStateLoad(&error), "different installed set refused");
        Check(Host::osd_key == "guest-plugin-state-versions" && Host::osd_message ==
            "Plugins from the save state are active. Restart the game to use the installed versions.", "different plugins did not display the short OSD note");
        simulated_installed_set_differs = false;
        Check(PrepareGuestModuleState(active, &error) && BeginGuestModuleStateLoad(&error) && FinishGuestModuleStateLoad(&error), "matching restore failed after different set");
        Check(Host::osd_key.empty(), "matching restore retained the earlier difference note");
        auto large = source; large.manifest.resize(2 * 1024 * 1024);
        Check(PrepareGuestModuleState(encode(large), &error), "embedded ELF manifest above old limit rejected");
        CancelGuestModuleStateLoad();
        rejected_operation = 1;
        Check(!PrepareGuestModuleState(active, &error) && s_guest_modules_committed && s_guest_module_index == source.index,
            "failed plugin preflight changed live runtime");
        rejected_operation = 5;
        Check(!PrepareGuestModuleState(active, &error) && s_guest_modules_committed && s_guest_module_index == source.index,
            "failed queue validation changed live runtime");
        Check(state_operations.back() == 4, "failed queue validation retained staged registrations");
        rejected_operation = 0xffffffff;
        Check(PrepareGuestModuleState(active, &error), "valid preflight failed after earlier refusal");
        CancelGuestModuleStateLoad();
        Check(!PendingGuestModuleStateHasModules() && state_operations.back() == 4, "cancel retained staged runtime");
        ResetGuestModules();
        std::vector<u8> empty;
        Check(SaveGuestModuleState(empty, &error) && PrepareGuestModuleState(empty, &error), "plugin-free snapshot save/load failed");
        std::cout << "PASS: " << checks << " combined runtime and snapshot checks\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

