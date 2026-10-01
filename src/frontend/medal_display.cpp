#include "frontend/medal_display.hpp"

#include "campaign/campaign.hpp"
#include "frontend/gui_render.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

#include <algorithm>

namespace sl_open::frontend
{
namespace
{
struct Region
{
	std::int16_t x;
	std::int16_t y;
	std::int16_t width;
	std::int16_t height;
};

constexpr Region kEarlyRegions[] = {
	{226, 111, 79, 43},
	{303, 132, 86, 52},
	{399, 152, 96, 68},
	{116, 214, 46, 29},
	{161, 232, 49, 31},
	{213, 252, 50, 35},
};

constexpr Region kLateRegions[] = {
	{206, 154, 68, 60},
	{291, 157, 72, 62},
	{384, 164, 80, 68},
	{172, 223, 74, 67},
	{263, 229, 82, 84},
	{366, 239, 90, 97},
	{145, 299, 29, 24},
	{190, 308, 40, 23},
	{239, 317, 44, 26},
	{290, 326, 42, 26},
	{345, 336, 49, 28},
};

constexpr std::uint16_t kEarlyLabels[] = {
	0x56a, 0x56b, 0x56c, 0x56d, 0x56e, 0x56f};
constexpr std::uint16_t kLateLabels[] = {
	0x56a, 0x56b, 0x56c, 0x570, 0x571, 0x572,
	0x56d, 0x56e, 0x56f, 0x573, 0x574};

constexpr std::uint8_t kEarlyMedalShapes[3][22] = {
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
		11, 12, 13},
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8,
		9, 10, 11},
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8,
		9, 10, 11},
};
constexpr std::uint8_t kEarlyBarShapes[22] = {
	0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
	13, 14, 15};
constexpr std::uint8_t kLateMedalShapes[6][15] = {
	{0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 6, 6, 6},
	{0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 6, 6, 6},
	{0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 6, 6, 6},
	{0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 8, 8},
	{0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 7, 7, 7, 7},
	{0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 8, 8},
};
constexpr std::uint8_t kLateBarShapes[15] = {
	0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 8, 8, 8};

constexpr std::int16_t kEarlyMedalX[] = {202, 287, 382};
constexpr std::int16_t kEarlyMedalY[] = {100, 121, 146};
constexpr std::int16_t kEarlyBarX[] = {116, 158, 210};
constexpr std::int16_t kEarlyBarY[] = {212, 228, 250};
constexpr std::int16_t kLateMedalX[] = {191, 272, 368, 153, 242, 349};
constexpr std::int16_t kLateMedalY[] = {148, 153, 158, 213, 222, 233};
constexpr std::int16_t kLateBarX[] = {142, 183, 235, 282, 340};
constexpr std::int16_t kLateBarY[] = {296, 303, 313, 320, 331};

std::uint32_t animation_index(
	const MedalDisplay& display,
	std::uint32_t movie_frame)
{
	const std::uint32_t last = display.late_campaign ? 14 : 21;
	if (display.phase == MedalPhase::waiting)
	{
		return last;
	}
	if (display.phase == MedalPhase::closing)
	{
		return movie_frame >= last ? 0 : last - movie_frame;
	}
	const std::uint32_t opening_frame =
		movie_frame == 0 ? 0 : movie_frame - 1;
	return std::min(opening_frame, last);
}

}

void medal_display_reset(MedalDisplay& display, bool late_campaign)
{
	display = {};
	display.phase = late_campaign ? MedalPhase::zoom : MedalPhase::opening;
	display.hovered = -1;
	display.pointer_x = 320;
	display.pointer_y = 240;
	display.late_campaign = late_campaign;
}

const char* medal_display_movie(const MedalDisplay& display)
{
	switch (display.phase)
	{
	case MedalPhase::zoom: return "vr/lockzomi.bik";
	case MedalPhase::opening:
		return display.late_campaign
			? "vr/locklidup.bik"
			: "vr/rel_locklup.bik";
	case MedalPhase::closing:
		return display.late_campaign
			? "vr/lokliddo.bik"
			: "vr/rel_lockldo.bik";
	default: return nullptr;
	}
}

bool medal_display_movie_finished(MedalDisplay& display)
{
	if (display.phase == MedalPhase::zoom)
	{
		display.phase = MedalPhase::opening;
		return true;
	}
	if (display.phase == MedalPhase::opening)
	{
		display.phase = MedalPhase::waiting;
		return false;
	}
	if (display.phase == MedalPhase::closing)
	{
		display.phase = MedalPhase::complete;
	}
	return false;
}

bool medal_display_begin_close(MedalDisplay& display)
{
	if (display.phase != MedalPhase::waiting)
	{
		return false;
	}
	display.phase = MedalPhase::closing;
	display.hovered = -1;
	return true;
}

void medal_display_set_pointer(
	MedalDisplay& display,
	float x,
	float y,
	bool inside)
{
	display.pointer_x = x;
	display.pointer_y = y;
	display.hovered = -1;
	if (!inside || display.phase != MedalPhase::waiting)
	{
		return;
	}
	const Region* regions =
		display.late_campaign ? kLateRegions : kEarlyRegions;
	const std::uint8_t count = display.late_campaign ? 11 : 6;
	for (std::uint8_t index = 0; index < count; ++index)
	{
		if (gui::hit_open(regions[index], x, y))
		{
			display.hovered = static_cast<std::int8_t>(index);
			return;
		}
	}
}

void medal_display_build(
	const MedalDisplay& display,
	const campaign::CampaignState& campaign,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint32_t movie_frame,
	std::uint64_t now)
{
	const std::uint32_t phase = animation_index(display, movie_frame);
	if (display.late_campaign)
	{
		for (std::uint8_t index = 0; index < 6; ++index)
		{
			const std::uint8_t shape = kLateMedalShapes[index][phase];
			if (campaign.medals[index] && shape != 0)
			{
				render::frontend_indexed_quad(
					commands,
					renderer.medals.late_medals[index][shape - 1],
					renderer.medals.late_medal_palettes[index],
					kLateMedalX[index],
					kLateMedalY[index]);
			}
		}
		for (std::uint8_t index = 0; index < 5; ++index)
		{
			const std::uint8_t shape = kLateBarShapes[phase];
			if (campaign.bars[index] && shape != 0)
			{
				render::frontend_indexed_quad(
					commands,
					renderer.medals.late_bars[index][shape - 1],
					renderer.medals.late_bar_palettes[index],
					kLateBarX[index],
					kLateBarY[index]);
			}
		}
	}
	else
	{
		for (std::uint8_t index = 0; index < 3; ++index)
		{
			const std::uint8_t medal_shape =
				kEarlyMedalShapes[index][phase];
			if (campaign.medals[index] && medal_shape != 0)
			{
				render::frontend_indexed_quad(
					commands,
					renderer.medals.early_medals[index][medal_shape - 1],
					renderer.medals.early_medal_palettes[index],
					kEarlyMedalX[index],
					kEarlyMedalY[index]);
			}
			const std::uint8_t bar_shape = kEarlyBarShapes[phase];
			if (campaign.bars[index] && bar_shape != 0)
			{
				render::frontend_indexed_quad(
					commands,
					renderer.medals.early_bars[index][bar_shape - 1],
					renderer.medals.early_bar_palettes[index],
					kEarlyBarX[index],
					kEarlyBarY[index]);
			}
		}
	}

	if (display.hovered >= 0)
	{
		const std::uint8_t hovered =
			static_cast<std::uint8_t>(display.hovered);
		const bool earned = display.late_campaign
			? (hovered < 6
				? campaign.medals[hovered]
				: campaign.bars[hovered - 6])
			: (hovered < 3
				? campaign.medals[hovered]
				: campaign.bars[hovered - 3]);
		const std::uint16_t* labels =
			display.late_campaign ? kLateLabels : kEarlyLabels;
		const char* label = language_text(
			language, earned ? labels[hovered] : 0xd5);
		constexpr float scale = 0.62f;
		render::frontend_text(
			commands,
			renderer,
			label,
			gui::aligned_x(
				440.0f,
				gui::text_width(
					renderer.shell.glyphs,
					renderer.shell.glyph_count,
					label,
					scale),
				gui::TextAlign::center),
			320.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			scale);
	}
	if (display.phase == MedalPhase::waiting)
	{
		gui::animated_cursor(
			commands,
			renderer.shell.cursor,
			renderer.shell.cursor_palette,
			display.pointer_x,
			display.pointer_y,
			now,
			60);
	}
}
}
