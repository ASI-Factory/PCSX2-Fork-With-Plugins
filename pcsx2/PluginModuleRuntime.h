// PCSX2F integration points. Implemented in the VMManager translation unit.
#pragma once

#include <cstdint>
#include <vector>

class Error;

namespace VMManager::Internal
{
	void ResetGuestModules();
	bool GuestModulesReserveArena();
	bool TryStartGuestModules();
    bool TryStartGuestModulesAfterKernel();
	bool TryReturnFromGuestModule();
	bool HandleGuestModuleSyscall();
	std::uint32_t GuestModuleMemorySize();
	bool AllowGuestModuleSaveState(Error* error);
	bool HandlePluginSyscall();
	bool SaveGuestModuleState(std::vector<std::uint8_t>& data, Error* error);
	bool PrepareGuestModuleState(const std::vector<std::uint8_t>& data, Error* error);
	bool BeginGuestModuleStateLoad(Error* error);
	bool FinishGuestModuleStateLoad(Error* error);
	void CancelGuestModuleStateLoad();
	bool PendingGuestModuleStateHasModules();
}
