// PCSX2F guest OSD, included by ImGuiOverlays.cpp.
extern "C" bool GetIsThrottlerTempDisabled();

#ifdef _WIN32
#include "common/RedtapeWindows.h"

static void DrawPluginsOverlay()
{
#ifdef _WIN32
	auto GetPCSX2PluginInjector = []() -> HMODULE {
		constexpr auto dll = L"PCSX2PluginInjector.asi";
		auto hm = GetModuleHandleW(dll);
		return (hm ? hm : LoadLibraryW(dll));
	};
	auto GetOSDVectorSize = (size_t (*)())GetProcAddress(GetPCSX2PluginInjector(), "GetOSDVectorSize");
	auto GetOSDVectorData = (const char* (*)(size_t index))GetProcAddress(GetPCSX2PluginInjector(), "GetOSDVectorData");
	if (GetOSDVectorSize != NULL && GetOSDVectorData != NULL)
	{
		if (GetOSDVectorSize())
		{
			const float scale = ImGuiManager::GetGlobalScale();
			const float shadow_offset = std::ceil(1.0f * scale);
			const float margin = std::ceil(10.0f * scale);
			const float spacing = std::ceil(5.0f * scale);
			float position_y = margin;

			ImDrawList* dl = ImGui::GetBackgroundDrawList();
			std::string text;
			ImVec2 text_size;
			text.reserve(255);

#define DRAW_LINE(font, size, text, color) \
	do \
	{ \
		text_size = font->CalcTextSizeA(size, std::numeric_limits<float>::max(), -1.0f, (text), nullptr, nullptr); \
		dl->AddText(font, size, \
			ImVec2(margin + shadow_offset, position_y + shadow_offset), \
			IM_COL32(0, 0, 0, 100), (text)); \
		dl->AddText(font, size, ImVec2(margin, position_y), color, (text)); \
		position_y += text_size.y + spacing; \
	} while (0)

			const bool paused = (VMManager::GetState() == VMState::Paused);

			if (!paused)
			{
				ImFont* const fixed_font = ImGuiManager::GetFixedFont();
				const float font_size = ImGuiManager::GetFontSizeStandard();
				for (size_t i = 0; i < GetOSDVectorSize(); i++)
				{
					std::string_view s(GetOSDVectorData(i), 255);
					if (!s.empty() && s[0] != 0)
					{
						text.clear();
						fmt::format_to(std::back_inserter(text), "{}", s);
						DRAW_LINE(fixed_font, font_size, text.c_str(), IM_COL32(255, 255, 255, 255));
					}
				}
			}
#undef DRAW_LINE
		}
	}
#endif
}
#else
static void DrawPluginsOverlay() {}
#endif
