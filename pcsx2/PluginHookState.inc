// Included by VMManager.cpp through PluginHostRuntime.inc; no build target edits.
#include "PluginHookState.h"
#include "VUmicro.h"

static bool TransferGuestHookState(uint8_t* image, uint32_t flags, bool restore)
{
    static_assert(offsetof(fpuRegisters, ACCflag) == offsetof(fpuRegisters, ACC) + 4);
    constexpr PluginHookState::VULayout layout{
        sizeof(VURegs), offsetof(VURegs, Mem), offsetof(VURegs, VIBackupCycles),
        offsetof(VURegs, cycle), offsetof(VURegs, VI), offsetof(VURegs, idx),
        offsetof(VURegs, fmacreadpos), offsetof(VURegs, fmacwritepos), offsetof(VURegs, fmaccount),
        offsetof(VURegs, ialureadpos), offsetof(VURegs, ialuwritepos), offsetof(VURegs, ialucount)};
    static_assert(16 + layout.prefix + layout.size - layout.tail <= PluginHookState::VUBytes);
    return PluginHookState::Transfer(image, flags, restore,
        reinterpret_cast<uint8_t*>(&fpuRegs.ACC), reinterpret_cast<uint8_t*>(&VU0), layout, _vu0FinishMicro);
}
