// PCSX2F integration points. Implemented in the VMManager translation unit.
#pragma once

#include <cstdint>

class Error;

namespace VMManager::Internal
{
	void ResetGuestModules();
	bool GuestModulesReserveArena();
	bool TryStartGuestModules();
	bool TryReturnFromGuestModule();
	bool HandleGuestModuleSyscall();
	std::uint32_t GuestModuleMemorySize();
	bool AllowGuestModuleSaveState(Error* error);
	bool HandlePluginSyscall();
}
