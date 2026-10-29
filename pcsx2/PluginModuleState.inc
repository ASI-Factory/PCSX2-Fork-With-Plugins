// Versioned host bookkeeping stored separately from guest EE RAM.
#include "StateWrapper.h"

namespace
{
	constexpr u32 GUEST_MODULE_STATE_VERSION = 1;
	constexpr u32 GUEST_MODULE_MANIFEST_LIMIT = 128 * 1024 * 1024;
	constexpr u32 GUEST_MODULE_STATE_LIMIT = GUEST_MODULE_MANIFEST_LIMIT + 1024 * 1024;
	using GuestStateService = s32 (*)(u32, void*, u32, char*, u32);

	GuestStateService GetGuestStateService()
	{
#ifdef _WIN32
		HMODULE module = GetModuleHandleW(L"PCSX2PluginInjector.asi");
		if (!module)
			module = LoadLibraryW(L"PCSX2PluginInjector.asi");
		if (module)
		{
			if (const auto initialize = reinterpret_cast<void (*)()>(GetProcAddress(module, "InitializeASI")))
				initialize();
			return reinterpret_cast<GuestStateService>(GetProcAddress(module, "GuestPluginStateTransfer"));
		}
#endif
		return nullptr;
	}
	bool TransferGuestState(u32 operation, void* buffer, u32 size, Error* error, s32* result = nullptr)
	{
		const auto service = GetGuestStateService();
		if (!service)
		{
			Error::SetString(error, "Update PCSX2PluginInjector.asi: guest plugin save-state support is required.");
			return false;
		}
		char message[1024] = {};
		const s32 value = service(operation, buffer, size, message, sizeof(message));
		if (result)
			*result = value;
		if (value >= 0)
			return true;
		Error::SetString(error, message[0] ? message : "Guest plugin state operation failed.");
		return false;
	}
	struct GuestModuleSnapshot
	{
		bool committed = false, running = false, thread_ready = false;
		u32 index = 0, resume_pc = 0, thread_return = 0, heap_return = 0, memory_return = 0, sa = 0;
		u64 generation = 0;
		GPRregs gpr{};
		GPR_reg hi{}, lo{};
		fpuRegisters fpu{};
		std::vector<QueuedGuestModule> modules;
		std::vector<u8> manifest;

		bool DoState(StateWrapper& sw)
		{
			u32 version = GUEST_MODULE_STATE_VERSION;
			sw.Do(&version);
			if (version != GUEST_MODULE_STATE_VERSION)
				return false;
			sw.Do(&committed);
			sw.Do(&running);
			sw.Do(&thread_ready);
			sw.Do(&index);
			sw.Do(&resume_pc);
			sw.Do(&thread_return);
			sw.Do(&heap_return);
			sw.Do(&memory_return);
			sw.Do(&sa);
			sw.Do(&generation);
			u32 gpr_size = sizeof(gpr), fpu_size = sizeof(fpu);
			sw.Do(&gpr_size);
			sw.Do(&fpu_size);
			if (gpr_size != sizeof(gpr) || fpu_size != sizeof(fpu))
				return false;
			sw.DoBytes(&gpr, sizeof(gpr));
			sw.DoBytes(&hi, sizeof(hi));
			sw.DoBytes(&lo, sizeof(lo));
			sw.DoBytes(&fpu, sizeof(fpu));
			u32 count = static_cast<u32>(modules.size());
			sw.Do(&count);
			if (count > 256)
				return false;
			if (sw.IsReading())
				modules.resize(count);
			for (auto& module : modules)
			{
				sw.Do(&module.entry);
				sw.Do(&module.stack);
				sw.Do(&module.gp);
				sw.Do(&module.context);
				if (module.entry < MODULE_ARENA_BEGIN + 4096 || module.entry >= Ps2MemSize::ExposedRam || module.entry % 4 ||
					module.stack < MODULE_ARENA_BEGIN + 4096 + 16384 || module.stack > Ps2MemSize::ExposedRam || module.stack % 16 ||
					module.context < MODULE_ARENA_BEGIN + 4096 || static_cast<u64>(module.context) + 32 > Ps2MemSize::ExposedRam || module.context % 16)
					return false;
			}
			u32 length = static_cast<u32>(manifest.size());
			sw.Do(&length);
			if (length > GUEST_MODULE_MANIFEST_LIMIT)
				return false;
			if (sw.IsReading())
				manifest.resize(length);
			sw.DoBytes(manifest.data(), length);
			return sw.IsGood() && index <= count && (committed == (count != 0)) &&
			       (!running || (committed && index < count)) && (committed == !manifest.empty()) &&
			       (committed || (!running && !thread_ready && !index && !thread_return && !heap_return && !memory_return));
		}
	};
	GuestModuleSnapshot s_pending_guest_snapshot;
} // namespace

bool VMManager::Internal::SaveGuestModuleState(std::vector<u8>& data, Error* error)
{
	if (!AllowGuestModuleSaveState(error))
		return false;
	GuestModuleSnapshot snapshot;
	if (s_guest_modules_committed)
	{
		snapshot.committed = true;
		snapshot.running = s_guest_module_running;
		snapshot.thread_ready = s_guest_thread_ready;
		snapshot.index = static_cast<u32>(s_guest_module_index);
		snapshot.resume_pc = s_guest_resume_pc;
		snapshot.thread_return = s_guest_thread_return_pc;
		snapshot.heap_return = s_guest_heap_return_pc;
		snapshot.memory_return = s_guest_memory_return_pc;
		snapshot.sa = s_guest_saved_sa;
		snapshot.generation = s_guest_module_generation;
		snapshot.gpr = s_guest_saved_gpr;
		snapshot.hi = s_guest_saved_hi;
		snapshot.lo = s_guest_saved_lo;
		snapshot.fpu = s_guest_saved_fpu;
		snapshot.modules = s_guest_modules;
		snapshot.manifest.resize(GUEST_MODULE_MANIFEST_LIMIT);
		s32 size = 0;
		if (!TransferGuestState(0, snapshot.manifest.data(), static_cast<u32>(snapshot.manifest.size()), error, &size))
			return false;
		snapshot.manifest.resize(size);
	}
	StateWrapper::VectorMemoryStream stream;
	StateWrapper sw(&stream, StateWrapper::Mode::Write, 0);
	if (!snapshot.DoState(sw))
	{
		Error::SetString(error, "Could not serialize guest plugin runtime.");
		return false;
	}
	data = stream.GetBuffer();
	return true;
}

bool VMManager::Internal::PrepareGuestModuleState(const std::vector<u8>& data, Error* error)
{
	s_pending_guest_snapshot = {};
	if (data.empty())
		return true; // Older states predate guest modules.
	if (data.size() > GUEST_MODULE_STATE_LIMIT)
	{
		Error::SetString(error, "Guest plugin state section is too large.");
		return false;
	}
	GuestModuleSnapshot snapshot;
	StateWrapper::ReadOnlyMemoryStream stream(data.data(), static_cast<u32>(data.size()));
	StateWrapper sw(&stream, StateWrapper::Mode::Read, 0);
	if (!snapshot.DoState(sw) || stream.GetPosition() != data.size())
	{
		Error::SetString(error, "Unsupported or corrupt guest plugin state section.");
		return false;
	}
	if (snapshot.committed)
	{
		if (!TransferGuestState(1, snapshot.manifest.data(), static_cast<u32>(snapshot.manifest.size()), error))
			return false;
		static_assert(sizeof(QueuedGuestModule) == 16);
		if (!TransferGuestState(5, snapshot.modules.data(), static_cast<u32>(snapshot.modules.size() * sizeof(QueuedGuestModule)), error))
		{
			TransferGuestState(4, nullptr, 0, nullptr);
			return false;
		}
	}
	s_pending_guest_snapshot = std::move(snapshot);
	return true;
}

bool VMManager::Internal::BeginGuestModuleStateLoad(Error* error)
{
	if (s_guest_modules_committed || s_pending_guest_snapshot.committed)
	{
		if (!TransferGuestState(2, nullptr, 0, error))
			return false;
	}
	ResetGuestModules();
	PCSX2F::GuestRenderResetState();
	if (!s_pending_guest_snapshot.committed && Ps2MemSize::ExposedRam > MODULE_ARENA_BEGIN)
		std::memset(eeMem->Main + MODULE_ARENA_BEGIN, 0, Ps2MemSize::ExposedRam - MODULE_ARENA_BEGIN);
	return true;
}

bool VMManager::Internal::FinishGuestModuleStateLoad(Error* error)
{
	const auto& snapshot = s_pending_guest_snapshot;
	s_guest_modules = snapshot.modules;
	s_guest_modules_committed = snapshot.committed;
	s_guest_module_running = snapshot.running;
	s_guest_thread_ready = snapshot.thread_ready;
	s_guest_module_index = snapshot.index;
	s_guest_resume_pc = snapshot.resume_pc;
	s_guest_thread_return_pc = snapshot.thread_return;
	s_guest_heap_return_pc = snapshot.heap_return;
	s_guest_memory_return_pc = snapshot.memory_return;
	s_guest_saved_sa = snapshot.sa;
	s_guest_saved_gpr = snapshot.gpr;
	s_guest_saved_hi = snapshot.hi;
	s_guest_saved_lo = snapshot.lo;
	s_guest_saved_fpu = snapshot.fpu;
	// ResetGuestModules gave this restored layout a fresh host address-cache epoch.
	GetGuestPluginHostApi(PCSX2F_HOST_VERSION, sizeof(PCSX2FGuestHostV1));
	Host::RemoveKeyedOSDMessage("guest-plugin-state-versions");
	if (snapshot.committed)
	{
		uintptr_t context[] = {reinterpret_cast<uintptr_t>(eeMem->Main), Ps2MemSize::ExposedRam,
			reinterpret_cast<uintptr_t>(&g_gs_device->GetWindowInfo().window_handle),
			g_gs_device->GetWindowInfo().surface_width, g_gs_device->GetWindowInfo().surface_height,
			Host::IsFullscreen(), static_cast<uintptr_t>(GSConfig.AspectRatio)};
		s32 installed_set_differs = 0;
		if (!TransferGuestState(3, context, sizeof(context), error, &installed_set_differs))
			return false;
		if (installed_set_differs)
			Host::AddKeyedOSDMessage("guest-plugin-state-versions",
				"Plugins from the save state are active. Restart the game to use the installed versions.", 8.0f);
	}
	s_pending_guest_snapshot = {};
	return true;
}

void VMManager::Internal::CancelGuestModuleStateLoad()
{
	if (s_pending_guest_snapshot.committed)
		TransferGuestState(4, nullptr, 0, nullptr);
	s_pending_guest_snapshot = {};
}

bool VMManager::Internal::PendingGuestModuleStateHasModules()
{
	return s_pending_guest_snapshot.committed;
}
