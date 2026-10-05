// PCSX2F module startup, executed exclusively on the EE/CPU thread.
// This is included by VMManager.cpp to share its lifecycle and build targets.
#include "PCSX2FGuestModule.h"
#include "PluginModuleRuntime.h"

namespace
{
	constexpr u32 MODULE_ARENA_BEGIN = 0x02000000;
	constexpr u32 MODULE_ARENA_END = 0x08000000;
	constexpr u32 MODULE_RETURN = MODULE_ARENA_BEGIN;
	struct QueuedGuestModule
	{
		u32 entry, stack, gp, context;
	};
	std::vector<QueuedGuestModule> s_guest_modules;
	thread_local bool s_guest_module_load_window = false;
	bool s_guest_modules_committed = false;
	bool s_guest_module_running = false;
	size_t s_guest_module_index = 0;
	u32 s_guest_resume_pc = 0;
	GPRregs s_guest_saved_gpr;
	GPR_reg s_guest_saved_hi, s_guest_saved_lo;
	fpuRegisters s_guest_saved_fpu;
	u32 s_guest_saved_sa = 0;
	u64 s_guest_module_generation = 0;

	bool WriteGuestModule(u32 address, const void* bytes, u32 size)
	{
		if (!s_guest_module_load_window || !bytes || !size || address < MODULE_ARENA_BEGIN + 4096 ||
			static_cast<u64>(address) + size > std::min<u32>(Ps2MemSize::ExposedRam, MODULE_ARENA_END))
			return false;
		std::memcpy(eeMem->Main + address, bytes, size);
		return true; // EntryPointCompiling clears EE caches after the complete transaction.
	}
	bool QueueGuestModule(u32 entry, u32 stack, u32 gp, u32 context)
	{
		if (!s_guest_module_load_window || s_guest_modules_committed || s_guest_modules.size() >= 256 ||
			entry < MODULE_ARENA_BEGIN + 4096 || entry >= Ps2MemSize::ExposedRam || entry % 4 ||
			stack < MODULE_ARENA_BEGIN + 4096 + 16384 || stack > Ps2MemSize::ExposedRam || stack % 16 ||
			context < MODULE_ARENA_BEGIN + 4096 || static_cast<u64>(context) + 32 > Ps2MemSize::ExposedRam || context % 16)
			return false;
		s_guest_modules.push_back({entry, stack, gp, context});
		return true;
	}
	void AbortGuestModules()
	{
		if (!s_guest_module_load_window)
			return;
		s_guest_modules.clear();
		s_guest_modules_committed = false;
	}
	void WarnGuestModule(const char* message)
	{
		if (s_guest_module_load_window && message)
			Host::AddKeyedOSDMessage("guest-plugin-update", message, 20.0f);
	}
	bool CommitGuestModules()
	{
		if (!s_guest_module_load_window || s_guest_modules.empty() || s_guest_modules_committed)
			return false;
		// No guest code patches and no ra/delay-slot tricks. Return goes through a
		// private syscall stub, where the complete game GPR/FPU state is restored.
		const u32 stub[] = {0x240300f1, 0x0000000c, 0x00000000};
		std::memcpy(eeMem->Main + MODULE_RETURN, stub, sizeof(stub));
		s_guest_modules_committed = true;
		return true;
	}
	void EnterGuestModule()
	{
		const auto& module = s_guest_modules[s_guest_module_index];
		cpuRegs.GPR = s_guest_saved_gpr;
		cpuRegs.HI = s_guest_saved_hi;
		cpuRegs.LO = s_guest_saved_lo;
		fpuRegs = s_guest_saved_fpu;
		cpuRegs.sa = s_guest_saved_sa;
		cpuRegs.GPR.n.sp.UD[0] = module.stack;
		if (module.gp)
			cpuRegs.GPR.n.gp.UD[0] = module.gp;
		cpuRegs.GPR.n.ra.UD[0] = MODULE_RETURN;
		cpuRegs.GPR.n.a0.UD[0] = module.context;
		cpuRegs.GPR.n.t9.UD[0] = module.entry;
		const u32 game_gp = s_guest_saved_gpr.n.gp.UL[0];
		std::memcpy(eeMem->Main + module.context + 8, &game_gp, sizeof(game_gp));
		cpuRegs.pc = module.entry;
		cpuRegs.branch = 0;
		cpuRegs.IsDelaySlot = 0;
	}
} // namespace

#ifdef _WIN32
extern "C" __declspec(dllexport)
#else
extern "C" __attribute__((visibility("default"), used))
#endif
const PCSX2FGuestHostV1* GetGuestPluginHostApi(u32 version, u32 size)
{
	static PCSX2FGuestHostV1 api{sizeof(PCSX2FGuestHostV1), PCSX2F_HOST_VERSION,
		MODULE_ARENA_BEGIN + 4096, MODULE_ARENA_END, 0, WriteGuestModule, QueueGuestModule, CommitGuestModules, AbortGuestModules, WarnGuestModule};
	if (version != api.version || size != sizeof(api))
		return nullptr;
	api.generation = s_guest_module_generation;
	return &api;
}

void VMManager::Internal::ResetGuestModules()
{
	s_guest_modules.clear();
	s_guest_modules_committed = false;
	s_guest_module_running = false;
	s_guest_module_index = 0;
	++s_guest_module_generation;
}
bool VMManager::Internal::GuestModulesReserveArena()
{
	return s_guest_modules_committed;
}
u32 VMManager::Internal::GuestModuleMemorySize()
{
	return s_guest_modules_committed ? MODULE_ARENA_BEGIN : Ps2MemSize::ExposedRam;
}
bool VMManager::Internal::AllowGuestModuleSaveState(Error* error)
{
	if (!s_guest_modules_committed)
		return true;
	Error::SetString(error, "Save/load states are unavailable while relocatable plugins are active. Save to the memory card instead.");
	return false;
}
bool VMManager::Internal::HandleGuestModuleSyscall()
{
	if (TryReturnFromGuestModule())
		return true;
	const u32 code = cpuRegs.GPR.n.v1.UL[0];
	const auto call = static_cast<Syscall>((code & 0x80000000u) ? 0u - code : code);
	if (s_guest_modules_committed && call == Syscall::GetMemorySize)
	{
		cpuRegs.GPR.n.v0.UL[0] = MODULE_ARENA_BEGIN;
		return true;
	}
	return false;
}
bool VMManager::Internal::TryStartGuestModules()
{
	if (!s_guest_modules_committed || s_guest_module_running || s_guest_module_index ||
		s_guest_modules.empty() || cpuRegs.branch || cpuRegs.IsDelaySlot)
		return false;
	// Keep the existing readiness boundary: the executable's startup EI, after
	// its kernel/heap setup. Needs proper testing with both EE execution modes.
	const u32 entry = GetCurrentELFEntryPoint();
	const u32 pc = cpuRegs.pc & 0x1fffffff;
	if (pc <= entry || static_cast<u64>(pc) > static_cast<u64>(entry) + 2000)
		return false;
	s_guest_resume_pc = cpuRegs.pc;
	s_guest_saved_gpr = cpuRegs.GPR;
	s_guest_saved_hi = cpuRegs.HI;
	s_guest_saved_lo = cpuRegs.LO;
	s_guest_saved_fpu = fpuRegs;
	s_guest_saved_sa = cpuRegs.sa;
	s_guest_module_running = true;
	EnterGuestModule();
	return true;
}
bool VMManager::Internal::TryReturnFromGuestModule()
{
	if (!s_guest_module_running || cpuRegs.GPR.n.v1.UL[0] != 0xf1 ||
		cpuRegs.pc != MODULE_RETURN + 8 || cpuRegs.branch || cpuRegs.IsDelaySlot)
		return false;
	if (++s_guest_module_index < s_guest_modules.size())
		EnterGuestModule();
	else
	{
		cpuRegs.GPR = s_guest_saved_gpr;
		cpuRegs.HI = s_guest_saved_hi;
		cpuRegs.LO = s_guest_saved_lo;
		fpuRegs = s_guest_saved_fpu;
		cpuRegs.sa = s_guest_saved_sa;
		cpuRegs.pc = s_guest_resume_pc;
		s_guest_module_running = false;
	}
	return true;
}
