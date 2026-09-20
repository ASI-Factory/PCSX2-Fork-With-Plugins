// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// ---------------------------------------------------------------------------
// Drawing into the frame of a game, under its UI
//
// The frame of a PS2 game is its world and its UI in one buffer of the GS, and the
// emulator only gets its hands on that buffer when the frame is presented, which is
// after the UI was drawn into it: a plugin that draws at the present call draws over
// the UI of its game.
//
// A guest plugin of the plugin injector knows the point of its game where the world
// is done and the UI is not drawn yet, and reports it with a syscall (see the API of
// the injector, source/API/pcsx2f_api.h). The report lands in GuestRenderPhase below,
// which is where the plugins are asked to draw into the frame.
//
// The report alone is not enough, though: a game builds the commands of its 2D into a
// buffer of its own renderer and sends them to the GS in one go, ahead of the code that
// "draws" them, so the commands of the UI are already in the queue of the GS by the time
// the report arrives, and a plugin drawing at the report draws over the UI again. The
// report is used for what it does say, which is which buffer the frame is drawn into and
// how its UI is shaped, and where the UI of the frame starts in the stream of the GS is
// measured by the emulator itself, see GuestRenderDraw.
//
// What the measurement uses:
//
//   - the frame is drawn into one block of the GS memory and its post processing into
//     others, and the UI of the frame follows the last pass that was drawn into another
//     block;
//   - a draw of the world tests depth and writes it, a draw of a UI does neither, so
//     the pass that is looked for is a flat draw into the block of the frame that
//     follows a draw of another block;
//   - how many of those passes a frame has is a property of its scene and not of what
//     is in it (four or five in the games this was made for), so the count of the frame
//     that ended is what the frame that is drawn draws the plugins at, which is the last
//     of them, right before the UI.
//
// So the frame that follows a report is cut in two: everything the guest sends up to the
// point of the frame the plugins draw at is submitted, the plugins draw into the target
// of the frame, and the UI of the game is sent after them, which puts it on top of what
// they drew.
//
// Every draw of the GS has to be seen for that, so GSRendererHW::Draw() calls
// GuestRenderDraw() for each one, GSRendererHW::VSync() calls GuestRenderFrameEnded() at
// the end of the frame, and the syscall calls GuestRenderPhase().
//
// This file is header only so that the list of sources of the build does not have to know
// about it: exactly one source file defines PCSX2F_GUEST_RENDER_IMPLEMENTATION before
// including it, which is GS/GS.cpp, and every other file only sees the declarations below.
// ---------------------------------------------------------------------------

namespace PCSX2F
{
	// Called from the SYSCALL opcode for Syscall::PluginRenderPhase, see R5900OpcodeImpl.cpp.
	// a1 is the phase of the frame the guest plugin reported and a0 the magic of the API, which
	// is what tells a call of the plugin injector apart from a game that happens to use the same
	// number of syscall. Only the phase of PCSX2FRenderPhase_BeforeGuestUI draws into the frame;
	// a plugin that reports other points of its frame to look for that one is answered with
	// nothing at all.
	//
	// The call is made on the thread of the EE, so the guest stands still here, but the frame is
	// handed over on the thread of the GS, which is the one that owns the state of the graphics
	// API the plugins draw with and the one that presents it.
	void GuestRenderPhase(u32 phase, u32 magic);

	// Called for every draw of the GS, from the top of GSRendererHW::Draw(), before the draws
	// that are handled by a path of their own leave it: those are part of the frame as much as
	// the others are. zte, ztst and zmsk are the depth test and the depth write of the draw,
	// which are what tells a draw of a UI from a draw of a world, and target_block is the block
	// of the GS memory it is drawn into, which is what tells the frame from the passes of its
	// post processing. See the head of this file for what is measured with them.
	void GuestRenderDraw(u32 zte, u32 ztst, u32 zmsk, u32 target_block);

	// Called at the end of every frame of the GS, from GSRendererHW::VSync(), which is where the
	// frame that was measured ends and the next one starts.
	void GuestRenderFrameEnded();
} // namespace PCSX2F

#ifdef PCSX2F_GUEST_RENDER_IMPLEMENTATION

#include "GS/GS.h"
#include "GS/GSRegs.h"
#include "GS/GSState.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "MTGS.h"

#ifdef _WIN32
#include "GS/Renderers/DX11/GSDevice11.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#endif

#ifdef ENABLE_OPENGL
#include "GS/Renderers/OpenGL/GSDeviceOGL.h"
#endif

#ifdef ENABLE_VULKAN
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#endif

#include "common/Console.h"

#include <atomic>

// ---------------------------------------------------------------------------
// The API of the plugin injector, mirrored from its source/API/pcsx2f_api.h: it is
// what a guest plugin reports a phase with and what a plugin implements to draw.
// ---------------------------------------------------------------------------
enum : u32
{
	PCSX2F_GuestSyscallMagic = 0x50434652, // 'PCFR'
};

// The phases a guest plugin reports, see the enum of the same name in the API.
enum : u32
{
	PCSX2FRenderPhase_BeforeGuestUI = 1,
};

// The phases the plugins are called with. They are the emulator's own and not the ones a
// guest plugin reports: a report only says where the frame of its game is, and the plugins
// prepare their frame there, while the drawing happens at the point of the frame this file
// measures, see the head of it.
enum : u32
{
	PCSX2FPluginPhase_PrepareFrame = 1,
	PCSX2FPluginPhase_DrawFrame = 2,
};

enum PCSX2FRenderer : u32
{
	PCSX2FRenderer_Unknown = 0,
	PCSX2FRenderer_D3D11,
	PCSX2FRenderer_D3D12,
	PCSX2FRenderer_OpenGL,
	PCSX2FRenderer_Vulkan,
};

// The frame of the game, as the plugins of the injector need it: the resource of the
// graphics API it is drawn into, how the API has to treat that resource and the renderer
// the handle belongs to.
struct PCSX2FRenderTargetInfo
{
	u32 renderer; // PCSX2FRenderer
	void* resource; // ID3D11Texture2D, ID3D12Resource, VkImage or the texture of OpenGL
	u32 format;
	u32 width;
	u32 height;
	u32 state;
};

// Exports of the plugin injector, see its dllmain.cpp.
using PCSX2FGetGuestRenderPhaseCallbackCount = size_t (*)();
using PCSX2FInvokeGuestRenderPhase = void (*)(u32 phase, const PCSX2FRenderTargetInfo* target);

namespace
{
#ifdef _WIN32
	constexpr const char* INJECTOR_MODULE_NAME = "PCSX2PluginInjector.asi";

	// Both are looked up in the injector, which is loaded before a game runs. The count
	// is asked for before anything is drawn: without a plugin that draws into the frame
	// there is nothing to hand over.
	PCSX2FGetGuestRenderPhaseCallbackCount GetCallbackCountFunction()
	{
		static PCSX2FGetGuestRenderPhaseCallbackCount count = nullptr;
		if (!count)
		{
			if (HMODULE module = GetModuleHandleA(INJECTOR_MODULE_NAME))
				count = reinterpret_cast<PCSX2FGetGuestRenderPhaseCallbackCount>(GetProcAddress(module, "GetGuestRenderPhaseCallbackCount"));
		}

		return count;
	}

	PCSX2FInvokeGuestRenderPhase GetInvokeFunction()
	{
		static PCSX2FInvokeGuestRenderPhase invoke = nullptr;
		if (!invoke)
		{
			if (HMODULE module = GetModuleHandleA(INJECTOR_MODULE_NAME))
				invoke = reinterpret_cast<PCSX2FInvokeGuestRenderPhase>(GetProcAddress(module, "InvokeGuestRenderPhase"));
		}

		return invoke;
	}
#else
	// the plugin injector is a module of Windows, there is nothing to look up
	PCSX2FGetGuestRenderPhaseCallbackCount GetCallbackCountFunction() { return nullptr; }
	PCSX2FInvokeGuestRenderPhase GetInvokeFunction() { return nullptr; }
#endif

	// The renderers a frame can be handed over for: the null renderer and the software
	// renderer have no frame of a graphics API to draw into. GSIsHardwareRenderer() is
	// not enough here, it counts the null renderer as one.
	bool RendererHasFrameToHandOver()
	{
		switch (GSGetCurrentRenderer())
		{
#ifdef _WIN32
			case GSRendererType::DX11:
			case GSRendererType::DX12:
				return true;
#endif

#ifdef ENABLE_VULKAN
			case GSRendererType::VK:
				return true;
#endif

#ifdef ENABLE_OPENGL
			case GSRendererType::OGL:
				return true;
#endif

			default:
				return false;
		}
	}

	// The world of the frame: the guest has sent all of it by now, but an API that records a
	// command buffer holds it in one that it would submit at the end of the frame, which is
	// after the draw a plugin makes now, and the drops would end up under the world. Submitting
	// it here keeps the order the API executes in: the world, then the plugins, then the UI the
	// guest sends next. An API that is immediate has done that already.
	void FlushRecordedFrame()
	{
		switch (GSGetCurrentRenderer())
		{
#ifdef ENABLE_VULKAN
			case GSRendererType::VK:
				static_cast<GSDeviceVK*>(g_gs_device.get())->ExecuteCommandBuffer(false);
				break;
#endif

#ifdef _WIN32
			case GSRendererType::DX12:
				static_cast<GSDevice12*>(g_gs_device.get())->ExecuteCommandList(false);
				break;
#endif

			default:
				break;
		}
	}

	void FillTargetInfo(const GSTexture* target, PCSX2FRenderTargetInfo& info)
	{
		info.width = static_cast<u32>(target->GetWidth());
		info.height = static_cast<u32>(target->GetHeight());

		switch (GSGetCurrentRenderer())
		{
#ifdef _WIN32
			case GSRendererType::DX11:
			{
				GSTexture11* texture = static_cast<GSTexture11*>(const_cast<GSTexture*>(target));
				ID3D11Texture2D* resource = static_cast<ID3D11Texture2D*>(*texture);

				D3D11_TEXTURE2D_DESC desc = {};
				if (resource)
					resource->GetDesc(&desc);

				info.renderer = PCSX2FRenderer_D3D11;
				info.resource = resource;
				info.format = static_cast<u32>(desc.Format);
			}
			break;

			case GSRendererType::DX12:
			{
				GSTexture12* texture = static_cast<GSTexture12*>(const_cast<GSTexture*>(target));

				info.renderer = PCSX2FRenderer_D3D12;
				info.resource = texture->GetResource();
				info.format = static_cast<u32>(texture->GetDXGIFormat());
				info.state = static_cast<u32>(texture->GetResourceState());
			}
			break;
#endif

#ifdef ENABLE_VULKAN
			case GSRendererType::VK:
			{
				GSTextureVK* texture = static_cast<GSTextureVK*>(const_cast<GSTexture*>(target));

				info.renderer = PCSX2FRenderer_Vulkan;
				// A non-dispatchable handle is a pointer where the API can use one and an
				// integer otherwise, one reinterpret_cast covers both.
				info.resource = reinterpret_cast<void*>(texture->GetImage());
				info.format = static_cast<u32>(texture->GetVkFormat());
				info.state = static_cast<u32>(texture->GetLayout());
			}
			break;
#endif

#ifdef ENABLE_OPENGL
			case GSRendererType::OGL:
				info.renderer = PCSX2FRenderer_OpenGL;
				info.resource = target->GetNativeHandle();
				info.format = static_cast<u32>(static_cast<GSTextureOGL*>(const_cast<GSTexture*>(target))->GetIntFormat());
				break;
#endif

			default:
				break;
		}
	}

	// -----------------------------------------------------------------------
	// The measurement, see the head of this file.
	// -----------------------------------------------------------------------

	// The block of the GS memory the frame is drawn into, taken from the last report, and
	// the number of passes into another block the frame that was reported had. Zero until
	// a plugin reports the phase, which is when the plugins are first drawn into a frame.
	u32 s_frame_block = 0;
	u32 s_passes_expected = 0;

	// The frame that is being drawn: the passes of it that went into another block so far,
	// whether the last draw went into one of those, the target the plugins draw into, which
	// is remembered from the last draw that went into the block of the frame, and whether
	// they were drawn into it already.
	u32 s_passes = 0;
	bool s_draw_was_elsewhere = false;
	GSTexture* s_frame_texture = nullptr;
	bool s_armed = false;
	bool s_inserted = false;

	// Hands the frame that is being drawn over to the plugins, for them to draw into it. Called
	// on the thread of the GS, from the middle of the draws of a frame, right after everything
	// the guest sent so far was submitted: what the plugins draw then is under everything the
	// guest sends next, which is the UI of the frame at the point this is called at.
	void PluginDrawFrameNow(GSTexture* target)
	{
		const PCSX2FInvokeGuestRenderPhase invoke = GetInvokeFunction();
		const PCSX2FGetGuestRenderPhaseCallbackCount count = GetCallbackCountFunction();

		if (!invoke || !count || count() == 0 || !target)
			return;

		// everything the guest sent so far is submitted first, so that the API draws the plugins
		// into the frame after what is already in it, see FlushRecordedFrame
		FlushRecordedFrame();

		PCSX2FRenderTargetInfo info = {};
		FillTargetInfo(target, info);

		invoke(PCSX2FPluginPhase_PrepareFrame, &info);
		invoke(PCSX2FPluginPhase_DrawFrame, &info);
	}
} // namespace

// Whether this emulator answers the syscall a guest plugin reports a phase with. What the
// plugin injector looks for to know that: without it the call would go to the BIOS of the
// console, so the plugins are told not to make it at all, see PCSX2Data_GuestRenderPhase in
// its pcsx2f_api.h.
#ifdef _MSC_VER
#define PCSX2F_GUEST_RENDER_EXPORT __declspec(dllexport)
#else
#define PCSX2F_GUEST_RENDER_EXPORT __attribute__((visibility("default"), used))
#endif

extern "C" PCSX2F_GUEST_RENDER_EXPORT void PCSX2F_GuestRenderPhaseSupported() {}

#undef PCSX2F_GUEST_RENDER_EXPORT

void PCSX2F::GuestRenderDraw(u32 zte, u32 ztst, u32 zmsk, u32 target_block)
{
	// A draw of a UI is a flat one: it neither tests depth nor writes it.
	const bool flat = (zte == 0 || ztst == ZTST_ALWAYS || zmsk != 0);
	const bool in_frame_block = (s_frame_block != 0 && target_block == s_frame_block);

	if (s_frame_block != 0)
	{
		if (!in_frame_block)
		{
			// a pass of the frame, its post processing for one, which the UI of the frame
			// follows: the flat draw into the block of the frame that comes next is where
			// the UI of the frame starts
			s_draw_was_elsewhere = true;
		}
		else
		{
			if (flat && s_draw_was_elsewhere)
			{
				s_passes++;

				// The plugins are drawn at the pass the frame before the one being drawn had,
				// which is the last of them and is right before its UI. The count is taken
				// from the frame that was reported, see GuestRenderPhase.
				if (s_passes == s_passes_expected)
					s_armed = true;
			}

			s_draw_was_elsewhere = false;
		}

		// The target the plugins draw into is the one of the frame, which is what the last
		// draw into it was drawn into. The target of the draw before this one is the one to
		// ask for: the draw that is being made here is not drawn yet, and a pass of the frame
		// that went into another block is not part of it. A frame is presented with the target
		// forgotten, so it is always one of the frame that is being drawn.
		GSRendererHW* renderer = GSRendererHW::GetInstance();
		if (in_frame_block && renderer && renderer->GetLastDrawnRenderTargetBlock() == s_frame_block)
			s_frame_texture = renderer->GetLastDrawnRenderTargetTexture();

		// The plugins are drawn into the frame at the draw the last pass of it before its UI
		// starts at, which is the draw this was armed at: it happens right before the draw
		// that starts the UI of the frame, so the UI is sent to the GS after them.
		if (s_armed && !s_inserted && in_frame_block && s_frame_texture)
		{
			s_armed = false;
			s_inserted = true;
			PluginDrawFrameNow(s_frame_texture);
		}
	}
}

void PCSX2F::GuestRenderFrameEnded()
{
	s_passes = 0;
	s_draw_was_elsewhere = false;
	s_frame_texture = nullptr;
	s_armed = false;
	s_inserted = false;
}

void PCSX2F::GuestRenderPhase(u32 phase, u32 magic)
{
	// a game that happens to use the same number of syscall does not call the API of the
	// injector, and a plugin that reports another point of its frame than the one that draws
	// is answered with nothing at all
	if (magic != PCSX2F_GuestSyscallMagic || phase != PCSX2FRenderPhase_BeforeGuestUI)
		return;

	const PCSX2FInvokeGuestRenderPhase invoke = GetInvokeFunction();
	const PCSX2FGetGuestRenderPhaseCallbackCount get_callback_count = GetCallbackCountFunction();
	if (!invoke || !get_callback_count || get_callback_count() == 0)
		return;

	if (!MTGS::IsOpen() || !g_gs_device || !RendererHasFrameToHandOver())
		return;

	PCSX2FRenderTargetInfo info = {};
	std::atomic<bool> drawn = false;

	// The draw is put into the queue of the GS instead of happening here, on the thread of the
	// EE, for two reasons:
	//
	//   what is drawn between the commands the guest has already sent and the ones it sends
	//   when it resumes is drawn in the order the guest asked for, without draining anything
	//   on the way;
	//
	//   the thread of the GS is the one that owns the state of the graphics API the plugins
	//   draw with, and the one that presents it, which the API of OpenGL requires and the
	//   others expect.
	//
	// The target is read on the GS thread as well, so it is the one the frame that is being
	// drawn really is into.
	MTGS::RunOnGSThread([invoke, &info, &drawn]() {
		GSRendererHW* renderer = GSRendererHW::GetInstance();

		// The UI of the frame the report is about was sent to the GS before the report arrived,
		// see the head of this file, so this is where that frame ends: what is drawn into the
		// block it was drawn into from now on is the frame that is being drawn next, and the
		// passes of the frame that ended are the ones it is drawn with.
		if (renderer)
		{
			s_frame_block = renderer->GetLastDrawnRenderTargetBlock();
			s_passes_expected = s_passes;
		}

		// everything the guest sent up to the report is processed now, but the API may still hold
		// it in a command buffer it would submit at the end of the frame, which is after the draw
		// the plugins make next, and they would end up under the part of the frame that is in it.
		// Submitting it here puts the frame up to the report under what they draw.
		FlushRecordedFrame();

		// The plugins drew into the frame in the middle of it already, which is the point the UI
		// of it starts at: drawing them again now would put them over that UI. That is the case
		// once a report has been measured, which is every frame but the first one after it.
		if (s_inserted)
			return;

		// A report without a measurement to draw at is the first one of a game, or one where the
		// frame had no pass of its own: the plugins are drawn into the frame here, which is at the
		// end of it, over the UI of this one and under the UI of the next.
		GSTexture* target = renderer ? renderer->GetLastDrawnRenderTargetTexture() : nullptr;
		if (!target)
			return;

		FillTargetInfo(target, info);
		drawn = true;

		invoke(PCSX2FPluginPhase_PrepareFrame, &info);
		invoke(PCSX2FPluginPhase_DrawFrame, &info);
	});

	// the guest stands still until the plugins have drawn, so the UI it sends next is drawn
	// on top of what they drew
	MTGS::WaitGS();

	// once, so a run says whether the plugins draw into the frames of the game at all
	static bool logged = false;
	if (drawn.load() && !logged)
	{
		logged = true;
		Console.WriteLn("PCSX2F: the plugins of the injector draw into the frame of the game (renderer %u).", info.renderer);
	}
}

#endif // PCSX2F_GUEST_RENDER_IMPLEMENTATION
