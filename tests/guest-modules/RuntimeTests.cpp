// Exercise the production startup state machine without BIOS/Qt dependencies.
// EE dispatch/instruction execution remains an emulator acceptance test.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "../../pcsx2/PCSX2FGuestModule.h"
using u32 = uint32_t;
using u64 = uint64_t;
enum Syscall : uint8_t { GetMemorySize = 127 };
class Error
{
public:
    std::string message;
    static void SetString(Error* error, std::string message) { if (error) error->message = std::move(message); }
};
union alignas(16) GPR_reg { u32 UL[4]; u64 UD[2]; };
union GPRregs
{
    GPR_reg r[32];
    struct { GPR_reg r0, at, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7,
        s0, s1, s2, s3, s4, s5, s6, s7, t8, t9, k0, k1, gp, sp, fp, ra; } n;
};
struct fpuRegisters { u32 words[66]; };
struct { GPRregs GPR; GPR_reg HI, LO; u32 sa, pc, branch, IsDelaySlot; u64 cycle; } cpuRegs{};
fpuRegisters fpuRegs{};
namespace Ps2MemSize { constexpr u32 ExposedRam = 0x08000000; }
struct EEMem { uint8_t Main[Ps2MemSize::ExposedRam]; };
static std::unique_ptr<EEMem> eeMem = std::make_unique<EEMem>();
namespace Host { void AddKeyedOSDMessage(std::string, std::string, float) {} }
namespace VMManager::Internal
{
    static u32 game_entry = 0x100000;
    u32 GetCurrentELFEntryPoint() { return game_entry; }
    void ResetGuestModules();
    bool GuestModulesReserveArena();
    bool TryStartGuestModules();
    bool TryReturnFromGuestModule();
}
#include "../../pcsx2/PluginModuleRuntime.inc"
static unsigned checks = 0;
static void Check(bool condition, const char* description)
{
    ++checks;
    if (!condition) throw std::runtime_error(description);
}
int main()
{
    using namespace VMManager::Internal;
    try
    {
        ResetGuestModules();
        const auto* api = GetGuestPluginHostApi(1, sizeof(PCSX2FGuestHostV1));
        Check(api && api->size == 64, "host ABI layout");
        Check(!GetGuestPluginHostApi(2, sizeof(PCSX2FGuestHostV1)), "unknown ABI accepted");
        Check(!GetGuestPluginHostApi(1, 1), "short ABI accepted");
        uint32_t word = 0x12345678;
        Check(!api->write(0x02001000, &word, 4), "write outside load callback");
        Check(!api->queue(0x02001000, 0x02010000, 0, 0x02002000), "queue outside load callback");
        s_guest_module_load_window = true;
        Check(api->write(0x02001000, &word, 4), "valid write failed");
        Check(!api->write(0x00100000, &word, 4), "game memory write accepted");
        Check(!api->write(0x02000000, &word, 4), "return stub write accepted");
        Check(!api->write(0x07fffffe, &word, 4), "out-of-arena write accepted");
        Check(!api->write(UINT32_MAX, &word, 4), "overflow write accepted");
        bool other_thread_write = true;
        std::thread other([&] { other_thread_write = api->write(0x02001000, &word, 4); });
        other.join();
        Check(!other_thread_write, "write from another thread accepted");
        Check(!api->queue(0x02001001, 0x02010000, 0, 0x02002000), "unaligned entry accepted");
        Check(!api->queue(0x02001000, 0x02010001, 0, 0x02002000), "unaligned stack accepted");
        Check(api->queue(0x02001000, 0x02010000, 0, 0x02002000), "first queue failed");
        Check(api->queue(0x02020000, 0x02030000, 0, 0x02022000), "second queue failed");
        Check(!GuestModulesReserveArena(), "arena reserved before commit");
        Check(api->commit() && GuestModulesReserveArena(), "commit failed");
        Check(GuestModuleMemorySize() == 0x02000000, "module arena exposed to the game allocator");
        for (const uint32_t call : {127u, 0u - 127u})
        {
            cpuRegs.GPR.n.v1.UL[0] = call;
            Check(HandleGuestModuleSyscall(), "memory size query not handled");
            Check(cpuRegs.GPR.n.v0.UL[0] == 0x02000000, "incorrect reserved memory size");
        }
        cpuRegs.GPR.n.v1.UL[0] = 60;
        Check(!HandleGuestModuleSyscall(), "allocator syscall swallowed instead of continuing BIOS dispatch");
        Error error;
        Check(!AllowGuestModuleSaveState(&error) && !error.message.empty(), "active module save-state restriction missing");
        Check(!AllowGuestModuleSaveState(nullptr), "null error destination bypassed restriction");
        Check(!api->commit(), "double commit accepted");
        Check(!api->queue(0x02001000, 0x02010000, 0, 0x02002000), "queue after commit accepted");
        s_guest_module_load_window = false;
        for (unsigned i = 0; i < 32; ++i)
            for (unsigned j = 0; j < 4; ++j) cpuRegs.GPR.r[i].UL[j] = 0x100000 * i + j;
        cpuRegs.HI.UD[0] = 0x1122334455667788; cpuRegs.HI.UD[1] = 123;
        cpuRegs.LO.UD[0] = 0x8877665544332211; cpuRegs.LO.UD[1] = 456;
        for (unsigned i = 0; i < 66; ++i) fpuRegs.words[i] = i * 3;
        cpuRegs.sa = 77; cpuRegs.cycle = 100;
        const auto gpr = cpuRegs.GPR; const auto hi = cpuRegs.HI; const auto lo = cpuRegs.LO; const auto fpu = fpuRegs;
        cpuRegs.pc = 0x90000;
        Check(!TryStartGuestModules(), "started in BIOS");
        cpuRegs.pc = game_entry + 16; cpuRegs.IsDelaySlot = 1;
        Check(!TryStartGuestModules(), "started in delay slot");
        cpuRegs.IsDelaySlot = 0;
        Check(TryStartGuestModules(), "startup failed");
        Check(cpuRegs.pc == 0x02001000 && cpuRegs.GPR.n.sp.UL[0] == 0x02010000 &&
            cpuRegs.GPR.n.ra.UL[0] == 0x02000000 && cpuRegs.GPR.n.a0.UL[0] == 0x02002000, "first startup context");
        uint32_t game_gp = 0;
        std::memcpy(&game_gp, eeMem->Main + 0x02002008, 4);
        Check(game_gp == gpr.n.gp.UL[0], "game gp was lost");
        cpuRegs.GPR.n.v1.UL[0] = 0xf1;
        cpuRegs.pc = 0x01000008;
        Check(!TryReturnFromGuestModule(), "foreign syscall accepted");
        std::memset(&cpuRegs.GPR, 0xcd, sizeof(cpuRegs.GPR));
        std::memset(&fpuRegs, 0xef, sizeof(fpuRegs));
        cpuRegs.GPR.n.v1.UL[0] = 0xf1; cpuRegs.pc = 0x02000008; cpuRegs.cycle = 200;
        Check(HandleGuestModuleSyscall(), "first return failed");
        Check(cpuRegs.pc == 0x02020000 && cpuRegs.GPR.n.sp.UL[0] == 0x02030000, "second startup context");
        Check(cpuRegs.GPR.n.s0.UD[1] == gpr.n.s0.UD[1], "128-bit register restoration");
        std::memset(&cpuRegs.GPR, 0xab, sizeof(cpuRegs.GPR));
        std::memset(&fpuRegs, 0xff, sizeof(fpuRegs));
        cpuRegs.GPR.n.v1.UL[0] = 0xf1; cpuRegs.pc = 0x02000008; cpuRegs.cycle = 300;
        Check(TryReturnFromGuestModule(), "second return failed");
        Check(cpuRegs.pc == game_entry + 16, "incorrect game resume instruction");
        Check(!std::memcmp(&cpuRegs.GPR, &gpr, sizeof(gpr)), "GPR restoration");
        Check(!std::memcmp(&cpuRegs.HI, &hi, sizeof(hi)) && !std::memcmp(&cpuRegs.LO, &lo, sizeof(lo)), "HI/LO restoration");
        Check(!std::memcmp(&fpuRegs, &fpu, sizeof(fpu)) && cpuRegs.sa == 77, "FPU/SA restoration");
        Check(cpuRegs.cycle == 300, "guest cycles rewound");
        Check(!TryStartGuestModules(), "startup repeated");
        const auto generation = api->generation;
        ResetGuestModules();
        Check(!GuestModulesReserveArena() && !TryReturnFromGuestModule(), "reset left modules active");
        Check(GuestModuleMemorySize() == Ps2MemSize::ExposedRam, "reset left memory size restricted");
        Check(AllowGuestModuleSaveState(nullptr), "reset left save states restricted");
        cpuRegs.GPR.n.v1.UL[0] = 127;
        Check(!HandleGuestModuleSyscall(), "memory size query intercepted without modules");
        Check(GetGuestPluginHostApi(1, sizeof(*api))->generation > generation, "generation not advanced");
        s_guest_module_load_window = true;
        Check(api->queue(0x02001000, 0x02010000, 0, 0x02002000), "queue before abort");
        api->abort();
        Check(!api->commit(), "aborted queue committed");
        for (unsigned i = 0; i < 256; ++i) Check(api->queue(0x02001000, 0x02010000, 0, 0x02002000), "queue capacity");
        Check(!api->queue(0x02001000, 0x02010000, 0, 0x02002000), "queue overflow accepted");
        api->abort();
        std::cout << "PASS: " << checks << " runtime checks\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
