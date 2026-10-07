// PCSX2F host exports and lifecycle, included once by VMManager.cpp.
#include "common/RedtapeWindows.h"
#include "GS/PCSX2FGuestRender.h"
#include "R5900OpcodeTables.h"
#include <atomic>
#include <functional>
#include <vector>

class VMEvent
{
public:
	template <typename... Args>
	class Event : public std::function<void(Args...)>
	{
	public:
		using std::function<void(Args...)>::function;

	private:
		std::vector<std::function<void(Args...)>> handlers;

	public:
		void operator+=(std::function<void(Args...)>&& handler)
		{
			handlers.push_back(handler);
		}

		void executeAll(Args... args) const
		{
			if (!handlers.empty())
			{
				for (auto& handler : handlers)
				{
					handler(args...);
				}
			}
		}
	};

public:
	static auto& onGameElfInit()
	{
		static Event<const char*, const char*, const char*, const char*, const char*,
			const uint32_t, const uint32_t, const uint32_t, uint8_t*, size_t, const void*,
			const uint32_t, const uint32_t, const bool, const uint8_t> eventEntryPointCompilingOnCPUThread;
		return eventEntryPointCompilingOnCPUThread;
	}
	static auto& onGameShutdown()
	{
		static Event<> eventShutdown;
		return eventShutdown;
	}
};

extern "C"
{
#ifdef _WIN32
#define DLLEXPORT _declspec(dllexport)
#else
#define DLLEXPORT __attribute__((visibility("default"), used))
#endif

	static std::atomic_bool s_is_throttler_temp_disabled{false};

	using InitCB = void (*)(
	const char* s_disc_serial,
	const char* s_disc_elf,
	const char* s_disc_version,
	const char* s_title,
	const char* s_elf_path,
	const uint32_t s_disc_crc,
	const uint32_t s_current_crc,
	const uint32_t s_elf_entry_point,
	uint8_t* EEMainMemoryStart,
	size_t EEMainMemorySize,
	const void* WindowHandle,
	const uint32_t WindowSizeX,
	const uint32_t WindowSizeY,
	const bool IsFullscreen,
	const uint8_t AspectRatioSetting);
	using ShutdownCB = void (*)();

	DLLEXPORT void WriteBytes(uint32_t mem, const void* src, uint32_t size);
	DLLEXPORT bool GetIsThrottlerTempDisabled();
	DLLEXPORT void SetIsThrottlerTempDisabled(bool disable);
	DLLEXPORT VMState GetVMState();
	DLLEXPORT void AddOnGameElfInitCallback(InitCB callback);
	DLLEXPORT void AddOnGameShutdownCallback(ShutdownCB callback);

	void WriteBytes(uint32_t mem, const void* src, uint32_t size)
	{
		if (vtlb_memSafeCmpBytes(mem, src, size) != 0)
			vtlb_memSafeWriteBytes(mem, src, size);
	}

	bool GetIsThrottlerTempDisabled()
	{
		return s_is_throttler_temp_disabled.load(std::memory_order_relaxed);
	}

	void SetIsThrottlerTempDisabled(bool disable)
	{
		s_is_throttler_temp_disabled.store(disable, std::memory_order_relaxed);
	}

	VMState GetVMState()
	{
		return VMManager::GetState();
	}

	void AddOnGameElfInitCallback(InitCB callback)
	{
		VMEvent::onGameElfInit() += callback;
	}

	void AddOnGameShutdownCallback(ShutdownCB callback)
	{
		VMEvent::onGameShutdown() += callback;
	}
#undef DLLEXPORT
}



#include "PluginHookState.inc"
#include "PluginModuleRuntime.inc"
#include "PluginSettingsBridge.h"

namespace VMManager::Internal
{
	void NotifyGuestPluginShutdown()
	{
		VMEvent::onGameShutdown().executeAll();
	}
	void ResetGuestPlugins()
	{
		NotifyGuestPluginShutdown();
		ResetGuestModules();
	}
	void InitializeGuestPlugins()
	{
#ifdef _WIN32
		constexpr auto dll = L"PCSX2PluginInjector.asi";
		if (!GetModuleHandleW(dll))
			LoadLibraryW(dll);
#endif
		// Restrict guest writes to the complete CPU-thread load callback, including
		// early returns/exceptions. Boot patching and JIT reset happen afterwards.
		s_guest_module_load_window = true;
		struct CloseLoadWindow
		{
			~CloseLoadWindow() { s_guest_module_load_window = false; }
		} close;
		VMEvent::onGameElfInit().executeAll(
			s_disc_serial.data(), s_disc_elf.data(), s_disc_version.data(), s_title.data(),
			s_elf_path.data(), s_disc_crc, s_current_crc, s_elf_entry_point,
			&eeMem->Main[0], Ps2MemSize::ExposedRam, &g_gs_device->GetWindowInfo().window_handle,
			g_gs_device->GetWindowInfo().surface_width, g_gs_device->GetWindowInfo().surface_height,
			Host::IsFullscreen(), static_cast<uint8_t>(GSConfig.AspectRatio));
	}
	bool HandlePluginSyscall()
	{
		constexpr u32 PluginRenderPhase = 240; // Private before-UI render syscall; unchanged guest ABI.
		if (HandleGuestModuleSyscall())
			return true;
		const u32 code = cpuRegs.GPR.n.v1.UL[0];
		const u32 call = ((code & 0x80000000u) ? 0u - code : code) & 0xff;
		if (call == 0xf5 && cpuRegs.GPR.n.a0.UL[0] == 0x50434653)
		{
			cpuRegs.GPR.n.v0.UD[0] = PCSX2F::InvokePluginSettings(cpuRegs.GPR.n.a1.UL[0], cpuRegs.pc - 4);
			return true;
		}
		if (call != PluginRenderPhase)
			return false;
		PCSX2F::GuestRenderPhase(cpuRegs.GPR.n.a1.UL[0], cpuRegs.GPR.n.a0.UL[0]);
		return true; // Do not hand a private render request to the BIOS.
	}
}
