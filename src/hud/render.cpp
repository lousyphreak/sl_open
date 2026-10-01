#include "hud/render.hpp"

#include "assets/pilot_presentation.hpp"
#include "config/config.hpp"
#include "core/math.hpp"
#include "frontend/gui_render.hpp"
#include "game/weapons.hpp"
#include "hud/layout.hpp"
#include "input/controls.hpp"
#include "localization/language.hpp"
#include "mission/deathmatch_scenarios.hpp"
#include "mission/objectives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>

namespace sl_open::hud
{
namespace
{
struct Point
{
	float x{};
	float y{};
};

struct DistortionDrawState
{
	float amount{};
	std::uint32_t random_seed{};
	bool active{};
};

struct PanelDescriptor
{
	float x_fraction;
	float y_fraction;
	float pane_x_fraction;
	float pane_y_fraction;
	std::uint8_t decoration_count;
	std::int8_t decorations[2];
};

struct Decoration
{
	std::uint16_t shape;
	float x;
	float y;
	std::uint8_t draw_mode;
};

constexpr PanelDescriptor kPanels[kPanelCount] = {
	{0.0f, 0.0f, 0.0f, 0.0f, 2, {1, 0}},
	{0.0f, 1.0f, 0.0f, 1.0f, 2, {3, 17}},
	{0.5f, 0.0f, 0.5f, 0.0f, 2, {4, 6}},
	{0.7f, 1.0f, 0.2f, 1.0f, 0, {-1, -1}},
	{1.0f, 0.0f, 1.0f, 0.0f, 2, {14, 15}},
	{0.0f, 0.0f, 0.0f, 0.0f, 2, {0, 1}},
	{0.0f, 0.0f, 0.0f, 0.0f, 2, {0, 1}},
	{0.0f, 0.5f, 0.0f, 0.5f, 1, {2, -1}},
	{1.0f, 1.0f, 1.0f, 1.0f, 2, {27, 18}},
	{1.0f, 0.5f, 1.0f, 0.5f, 1, {16, -1}},
	{1.0f, 0.5f, 1.0f, 0.5f, 1, {16, -1}},
	{0.0f, 0.0f, 0.0f, 0.0f, 2, {1, 0}},
	{0.0f, 0.0f, 0.0f, 0.0f, 2, {0, 1}},
	{1.0f, 0.5f, 1.0f, 0.5f, 1, {16, -1}},
	{0.0f, 0.0f, 0.0f, 0.0f, 2, {1, 0}},
};

constexpr Decoration kDecorations[] = {
	{120, 12.0f, 1.0f, 0},
	{121, 0.0f, 14.0f, 0},
	{122, 1.0f, -58.0f, 0},
	{123, 1.0f, -147.0f, 0},
	{124, 0.0f, 0.0f, 0},
	{125, 0.657812476f, 0.931249976f, 0},
	{126, 1.0f, 35.0f, 0},
	{127, 0.321875006f, 0.297916681f, 0},
	{353, 0.4f, 0.895833313f, 0},
	{128, 0.075f, 0.43125f, 0},
	{129, 0.065625f, 0.47083333f, 0},
	{130, 0.1203125f, 0.47083333f, 0},
	{189, 0.171875f, 0.5f, 0},
	{127, 0.571875f, 0.29791668f, 1},
	{120, -139.0f, 1.0f, 1},
	{121, -32.0f, 14.0f, 1},
	{122, -32.0f, -58.0f, 1},
	{120, 13.0f, -23.0f, 2},
	{123, -32.0f, -146.0f, 1},
	{186, 0.775f, 0.0583333f, 0},
	{187, 0.934375f, 0.0625f, 0},
	{188, 0.7828125f, 0.14375f, 0},
	{189, 0.9328125f, 0.1375f, 0},
	{190, 0.775f, 0.2208333f, 0},
	{191, 0.9390625f, 0.2166667f, 0},
	{190, 0.09375f, 0.3583333f, 0},
	{193, 0.0140625f, 0.50625f, 0},
	{125, -210.0f, -24.0f, 0},
};

// The language word at +0x2c of each exact 78-byte retail binding record.
constexpr std::uint16_t kControlLanguageIds[kControlActionCount] = {
	816, 817, 818, 819, 820, 821, 822, 823, 824, 825,
	826, 827, 828, 829, 830, 869, 870, 880, 859, 866,
	842, 843, 860, 846, 847, 848, 849, 850, 851, 852,
	853, 854, 855, 857, 856, 877, 878, 1348, 858, 838,
	833, 834, 839, 1045, 832, 867, 862, 863, 831, 835,
	836, 873, 874, 875, 876, 837, 840, 841, 844, 845,
	864, 871, 861, 865, 868, 872, 879, 881, 882, 883,
	1352, 1353, 1347, 1371,
};

// LANCER.EXE 0x00486830 builds these two exact stack tables before rendering panel
// eight. Entries are indexed by serialized SRO model_type.
constexpr std::int16_t kComponentLanguageIds[23] = {
	-1, -1, -1, 924, -1, 925, 926, 927, 928, 924, 929, 930,
	931, 932, 933, 934, 935, 936, 950, 1134, 1524, 1525, 1526,
};

constexpr std::int16_t kComponentShapes[23] = {
	-1, -1, -1, 396, -1, 394, 402, 393, 395, 396, 397, 398,
	399, 400, 401, 403, 404, 405, 406, 407, 408, 409, 410,
};

Point scalable_anchor(
	const Layout& layout,
	float base_x,
	float base_y,
	float x_fraction,
	float y_fraction)
{
	const float margin = 16.0f * layout.element_scale;
	// HUD_get_screen_anchor (LANCER.EXE 0x00482e90..0x00482ef4)
	// converts both fractional screen offsets with FISTP.
	return {
		margin + base_x * layout.element_scale
			+ std::nearbyint(
				(layout.width - 33.0f * layout.element_scale)
				* x_fraction),
		margin + base_y * layout.element_scale
			+ std::nearbyint(
				(layout.height - 33.0f * layout.element_scale)
				* y_fraction),
	};
}

Point sensor_center(const Layout& layout)
{
	return scalable_anchor(layout, 1.0f, -51.0f, 0.5f, 1.0f);
}

constexpr float kSchematicPaneAnchorOffset = 190.0f;

Point player_schematic_anchor(const Layout& layout)
{
	const Point radar = sensor_center(layout);
	return {
		radar.x - kSchematicPaneAnchorOffset * layout.element_scale,
		radar.y - 20.0f * layout.element_scale,
	};
}

Point target_panel_anchor(const Layout& layout)
{
	const Point radar = sensor_center(layout);
	return {
		radar.x + kSchematicPaneAnchorOffset * layout.element_scale,
		radar.y + 16.0f * layout.element_scale,
	};
}

void draw_shape(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& frontend,
	std::uint16_t shape,
	float x,
	float y,
	float scale,
	std::uint32_t rgba = 0xffffffff)
{
	if (shape >= render::kGameplayHudShapeCount)
	{
		return;
	}
	const render::FrontendTexture& texture =
		frontend.shell.gameplay_hud_shapes[shape];
	if (!bgfx::isValid(texture.handle))
	{
		return;
	}
	render::frontend_indexed_scaled_quad(
		commands,
		texture,
		frontend.shell.gameplay_hud_palette,
		x + texture.offset_x * scale,
		y + texture.offset_y * scale,
		texture.width * scale,
		texture.height * scale,
		rgba);
}

std::uint16_t distortion_random(DistortionDrawState& state)
{
	// LANCER.EXE imports the MSVC CRT rand implementation at 0x004cf555.
	state.random_seed =
		state.random_seed * 0x343fdu + 0x269ec3u;
	return static_cast<std::uint16_t>(
		(state.random_seed >> 16) & 0x7fffu);
}

void draw_texture_distorted(
	render::FrontendCommands& commands,
	const render::FrontendTexture& texture,
	bgfx::TextureHandle palette,
	float x,
	float y,
	float scale,
	DistortionDrawState& distortion,
	std::uint32_t rgba = 0xffffffff,
	bool mirror_horizontal = false)
{
	if (!bgfx::isValid(texture.handle))
	{
		return;
	}
	const float width = texture.width * scale;
	if (!distortion.active)
	{
		render::frontend_indexed_scaled_quad(
			commands,
			texture,
			palette,
			x + texture.offset_x * scale
				+ (mirror_horizontal ? width : 0.0f),
			y + texture.offset_y * scale,
			mirror_horizontal ? -width : width,
			texture.height * scale,
			rgba);
		return;
	}

	// HUD_draw_shape_distorted 0x0048c6e0 draws into a transparent 16-bit
	// temporary surface, then copies each native source scanline at a
	// nonnegative round(rand/RAND_MAX * 10 * amplitude) displacement.
	// Row quads retain that native operation while scaling both axes
	// uniformly for the widescreen HUD.
	constexpr float kRandomReciprocal =
		1.0f / static_cast<float>(0x7fff);
	for (std::uint16_t row = 0; row < texture.height; ++row)
	{
		const float displacement = std::nearbyint(
			static_cast<float>(distortion_random(distortion))
				* kRandomReciprocal
				* 10.0f
				* distortion.amount);
		render::frontend_indexed_scaled_region(
			commands,
			texture,
			palette,
			x + (texture.offset_x + displacement) * scale
				+ (mirror_horizontal ? width : 0.0f),
			y + (texture.offset_y + row) * scale,
			mirror_horizontal ? -width : width,
			scale,
			0,
			row,
			texture.width,
			1,
			rgba);
	}
}

void draw_shape_distorted(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& frontend,
	std::uint16_t shape,
	float x,
	float y,
	float scale,
	DistortionDrawState& distortion,
	std::uint32_t rgba = 0xffffffff,
	bool mirror_horizontal = false)
{
	if (shape >= render::kGameplayHudShapeCount)
	{
		return;
	}
	draw_texture_distorted(
		commands,
		frontend.shell.gameplay_hud_shapes[shape],
		frontend.shell.gameplay_hud_palette,
		x,
		y,
		scale,
		distortion,
		rgba,
		mirror_horizontal);
}

void draw_movie_distorted(
	render::FrontendCommands& commands,
	const render::FrontendTexture& texture,
	float x,
	float y,
	std::uint16_t width,
	std::uint16_t height,
	float scale,
	DistortionDrawState& distortion)
{
	if (!bgfx::isValid(texture.handle) || width == 0 || height == 0)
	{
		return;
	}
	if (distortion.amount <= 0.0f)
	{
		render::frontend_rgba_region(
			commands,
			texture,
			x,
			y,
			static_cast<float>(width) * scale,
			static_cast<float>(height) * scale,
			0,
			0,
			width,
			height);
		return;
	}
	// Panel case zero at 0x00486b92 and 0x00486c4a does not use the
	// ordinary shape-distortion enable flag. Every decoded FM8 row instead
	// consumes CRT rand whenever camera disturbance 0x00588724 is positive,
	// with the same rounded ten-pixel amplitude used by the HUD copy path.
	constexpr float kRandomReciprocal =
		1.0f / static_cast<float>(0x7fff);
	for (std::uint16_t row = 0; row < height; ++row)
	{
		const float displacement = std::nearbyint(
			static_cast<float>(distortion_random(distortion))
				* kRandomReciprocal
				* 10.0f
				* distortion.amount);
		render::frontend_rgba_region(
			commands,
			texture,
			x + displacement * scale,
			y + static_cast<float>(row) * scale,
			static_cast<float>(width) * scale,
			scale,
			0,
			row,
			width,
			1);
	}
}

void draw_shape_mirrored(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& frontend,
	std::uint16_t shape,
	float x,
	float y,
	float scale,
	std::uint8_t mirror_mode = 1,
	std::uint32_t rgba = 0xffffffff)
{
	if (shape >= render::kGameplayHudShapeCount)
	{
		return;
	}
	const render::FrontendTexture& texture =
		frontend.shell.gameplay_hud_shapes[shape];
	if (!bgfx::isValid(texture.handle))
	{
		return;
	}
	const float width = texture.width * scale;
	const float height = texture.height * scale;
	const bool horizontal = (mirror_mode & 1u) != 0;
	const bool vertical = (mirror_mode & 2u) != 0;
	// winvfx8.dll VFX_shape_draw_mirrored 0x10001ebe keeps the ordinary
	// shape-origin-adjusted destination rectangle and reverses source
	// traversal inside it. Negative quad dimensions reproduce that behavior
	// without reflecting the authored origin around the caller's anchor.
	render::frontend_indexed_scaled_quad(
		commands,
		texture,
		frontend.shell.gameplay_hud_palette,
		x + texture.offset_x * scale + (horizontal ? width : 0.0f),
		y + texture.offset_y * scale + (vertical ? height : 0.0f),
		horizontal ? -width : width,
		vertical ? -height : height,
		rgba);
}

void draw_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& frontend,
	const char* text,
	float x,
	float y,
	float scale,
	std::int16_t allegiance_class = -1,
	std::uint32_t rgba = 0xffffffff)
{
	if (allegiance_class >= 0)
	{
		render::frontend_gameplay_hud_target_text(
			commands,
			frontend,
			text,
			x,
			y,
			static_cast<std::uint8_t>(
				std::min<std::int16_t>(allegiance_class, 2)),
			rgba,
			scale);
		return;
	}
	render::frontend_gameplay_hud_text(
		commands,
		frontend,
		text,
		x,
		y,
		rgba,
		scale);
}

void draw_generic_list(
	const mission::PlayerCommsState& comms,
	const LanguageTable& language,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	float x,
	float y,
	float scale)
{
	// CommsMenu_render, LANCER.EXE 0x00453a70, emits DAT_00529fb4. The
	// dispatcher replaces that title for the online player list, text
	// entry, and incoming-command prompt; the numbered rows use the
	// gameplay-message font at +24 with a twelve-pixel stride.
	draw_text(
		commands,
		frontend,
		language_text(language, comms.title_language_id),
		x,
		y,
		scale);
	for (std::int16_t index = 0;
		index < comms.option_count;
		++index)
	{
		const mission::CommsMenuOption& option =
			comms.options[index];
		char number[12];
		std::snprintf(
			number,
			sizeof(number),
			"%d.",
			static_cast<int>(index + 1));
		char dynamic_label[100];
		const char* label = option.label;
		if (option.language_id != UINT16_MAX)
		{
			if (option.secondary_language_id != UINT16_MAX)
			{
				std::snprintf(
					dynamic_label,
					sizeof(dynamic_label),
					"%s (%s)",
					language_text(language, option.language_id),
					language_text(
						language,
						option.secondary_language_id));
				label = dynamic_label;
			}
			else
			{
				label = language_text(
					language,
					option.language_id);
			}
		}
		const float row_y =
			y + (24.0f + 12.0f * index) * scale;
		render::frontend_gameplay_message_text(
			commands,
			frontend,
			number,
			x,
			row_y,
			0xffffffff,
			scale);
		render::frontend_gameplay_message_text(
			commands,
			frontend,
			label,
			x + 13.0f * scale,
			row_y,
			0xffffffff,
			scale);
	}
}

float text_width(
	const render::FrontendRenderer& frontend,
	const char* text,
	float scale)
{
	return frontend::gui::text_width(
		frontend.shell.gameplay_hud_glyphs,
		frontend.shell.gameplay_hud_glyph_count,
		text,
		scale);
}

float gameplay_message_text_width(
	const render::FrontendRenderer& frontend,
	const char* text,
	float scale)
{
	return frontend::gui::text_width(
		frontend.shell.gameplay_message_glyphs,
		frontend.shell.gameplay_message_glyph_count,
		text,
		scale);
}

float scoreboard_text_width(
	const render::FrontendRenderer& frontend,
	const char* text,
	float scale)
{
	return frontend::gui::text_width(
		frontend.shell.gameplay_scoreboard_glyphs,
		frontend.shell.gameplay_scoreboard_glyph_count,
		text,
		scale);
}

enum class ScoreboardTextAlignment : std::uint8_t
{
	left,
	center,
	right,
};

float aligned_text_x(
	float x,
	float width,
	ScoreboardTextAlignment alignment)
{
	switch (alignment)
	{
	case ScoreboardTextAlignment::center:
		return x - std::trunc(width * 0.5f);
	case ScoreboardTextAlignment::right:
		return x - width;
	default:
		return x;
	}
}

void draw_scoreboard_shape(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& frontend,
	std::uint8_t shape,
	float x,
	float y,
	float scale)
{
	if (shape >= render::kGameplayScoreboardShapeCount)
	{
		return;
	}
	const render::FrontendTexture& texture =
		frontend.shell.gameplay_scoreboard_shapes[shape];
	if (!bgfx::isValid(texture.handle))
	{
		return;
	}
	render::frontend_indexed_scaled_quad(
		commands,
		texture,
		frontend.shell.gameplay_scoreboard_palette,
		x + texture.offset_x * scale,
		y + texture.offset_y * scale,
		texture.width * scale,
		texture.height * scale,
		0xffffffffu);
}

void draw_scoreboard_value_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& frontend,
	const char* text,
	float x,
	float y,
	float scale,
	ScoreboardTextAlignment alignment);

void draw_deathmatch_scenario_status(
	const mission::Runtime& mission,
	const game::World& world,
	const LanguageTable& language,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	const mission::DeathmatchScenarioHudStatus status =
		mission::deathmatch_scenarios_hud_status(mission, world);
	if (!status.visible
		|| status.shape >= render::kGameplayScoreboardShapeCount)
	{
		return;
	}

	const Point anchor =
		scalable_anchor(layout, -25.0f, 0.0f, 0.5f, 0.0f);
	const float scale = layout.element_scale;
	draw_scoreboard_shape(
		commands,
		frontend,
		static_cast<std::uint8_t>(status.shape),
		anchor.x,
		anchor.y,
		scale);
	if (!status.draw_value)
	{
		return;
	}

	char text[64];
	if (status.suffix_language_id != UINT16_MAX)
	{
		std::snprintf(
			text,
			sizeof(text),
			"%d %s",
			status.value,
			language_text(language, status.suffix_language_id));
	}
	else
	{
		std::snprintf(text, sizeof(text), "%d", status.value);
	}
	draw_scoreboard_value_text(
		commands,
		frontend,
		text,
		anchor.x + 21.0f * scale,
		anchor.y + 40.0f * scale,
		scale,
		ScoreboardTextAlignment::left);
}

void draw_deathmatch_powerup_status(
	const mission::Runtime& mission,
	const game::World& world,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout,
	std::uint32_t simulation_tick)
{
	const mission::DeathmatchPowerupHudStatus status =
		mission::deathmatch_powerup_hud_status(
			mission, world, simulation_tick);
	if (!status.visible
		|| status.shape >= render::kGameplayScoreboardShapeCount)
	{
		return;
	}
	// DMPowerup_draw asks Scalable_Anchor for (-22,-95) at the 0.5/0.5
	// screen anchor, then draws the DMICONS record without an additional
	// scenario displacement.
	const Point anchor =
		scalable_anchor(layout, -22.0f, -95.0f, 0.5f, 0.5f);
	const float scale = layout.element_scale;
	draw_scoreboard_shape(
		commands,
		frontend,
		status.shape,
		anchor.x,
		anchor.y,
		scale);
	if (!status.draw_bar)
	{
		return;
	}
	const float y = anchor.y
		+ (37.0f + static_cast<float>(status.bar_offset)) * scale;
	const float start = anchor.x + 6.0f * scale;
	render::frontend_rgba_line(
		commands,
		frontend.white,
		start,
		y,
		start + static_cast<float>(status.bar_length) * scale,
		y,
		std::max(1.0f, scale),
		// DMPowerup_draw 0x0048635f..0x004863a6 passes palette index
		// three as VFX_line_draw's color.  The old implementation
		// mistakenly interpreted that final argument as line thickness
		// and rendered a three-pixel line in palette index zero.
		frontend.shell.gameplay_scoreboard_rgba[3]);
}

void draw_scoreboard_header_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& frontend,
	const char* text,
	float x,
	float y,
	float scale,
	ScoreboardTextAlignment alignment)
{
	render::frontend_gameplay_scoreboard_text(
		commands,
		frontend,
		text,
		aligned_text_x(
			x,
			scoreboard_text_width(frontend, text, scale),
			alignment),
		y,
		0xffffffffu,
		scale);
}

void draw_scoreboard_value_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& frontend,
	const char* text,
	float x,
	float y,
	float scale,
	ScoreboardTextAlignment alignment)
{
	render::frontend_gameplay_hud_text(
		commands,
		frontend,
		text,
		aligned_text_x(
			x,
			text_width(frontend, text, scale),
			alignment),
		y,
		0xffffffffu,
		scale);
}

void draw_scoreboard(
	const mission::Runtime& mission,
	const game::World& world,
	const LanguageTable& language,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	const mission::NetworkRuntime& network = mission.network;
	const float scale = layout.element_scale;
	const float x = layout.width * 0.5f - 233.0f * scale;
	const float center_y = layout.height * 0.5f;

	std::uint8_t players[mission::kNetworkPlayerCapacity]{};
	std::uint8_t player_count = 0;
	for (std::uint8_t player = 0;
		player < mission::kNetworkPlayerCapacity;
		++player)
	{
		if (network.connected[player])
		{
			players[player_count++] = player;
		}
	}
	const auto player_precedes =
		[&](std::uint8_t lhs, std::uint8_t rhs)
		{
			if (network.player_kills[lhs]
				!= network.player_kills[rhs])
			{
				return network.player_kills[lhs]
					> network.player_kills[rhs];
			}
			return network.player_deaths[lhs]
				< network.player_deaths[rhs];
		};
	for (std::uint8_t sorted = 1; sorted < player_count; ++sorted)
	{
		const std::uint8_t player = players[sorted];
		std::uint8_t insertion = sorted;
		while (insertion != 0
			&& player_precedes(player, players[insertion - 1]))
		{
			players[insertion] = players[insertion - 1];
			--insertion;
		}
		players[insertion] = player;
	}

	std::uint8_t teams[4]{};
	std::uint8_t team_count = 0;
	if (network.team_mode)
	{
		bool represented[4]{};
		for (std::uint8_t player = 0;
			player < mission::kNetworkPlayerCapacity;
			++player)
		{
			const std::int32_t team =
				network.object_team[player];
			if (network.connected[player]
				&& team >= 0
				&& team < static_cast<std::int32_t>(
					std::size(represented)))
			{
				represented[team] = true;
			}
		}
		for (std::uint8_t team = 0;
			team < std::size(represented);
			++team)
		{
			if (represented[team])
			{
				teams[team_count++] = team;
			}
		}
		const auto team_precedes =
			[&](std::uint8_t lhs, std::uint8_t rhs)
			{
				if (network.team_score[lhs]
					!= network.team_score[rhs])
				{
					return network.team_score[lhs]
						> network.team_score[rhs];
				}
				return network.team_deaths[lhs]
					< network.team_deaths[rhs];
			};
		for (std::uint8_t sorted = 1; sorted < team_count; ++sorted)
		{
			const std::uint8_t team = teams[sorted];
			std::uint8_t insertion = sorted;
			while (insertion != 0
				&& team_precedes(team, teams[insertion - 1]))
			{
				teams[insertion] = teams[insertion - 1];
				--insertion;
			}
			teams[insertion] = team;
		}
		// Deathmatch_draw_scoreboard expands only when the configured
		// lobby team count exceeds the represented count, and that
		// expansion replaces the sorted list with sequential team IDs.
		if (network.configured_team_count > team_count)
		{
			team_count = static_cast<std::uint8_t>(
				network.configured_team_count);
			for (std::uint8_t team = 0; team < team_count; ++team)
			{
				teams[team] = team;
			}
		}
	}

	if (network.team_mode)
	{
		const float top_y = center_y - 205.0f * scale;
		draw_scoreboard_shape(
			commands, frontend, 20, x, top_y, scale);
		draw_scoreboard_header_text(
			commands,
			frontend,
			language_text(language, 0x2c2),
			x + 20.0f * scale,
			top_y,
			scale,
			ScoreboardTextAlignment::left);
		draw_scoreboard_header_text(
			commands,
			frontend,
			language_text(language, 0x308),
			x + 223.0f * scale,
			top_y,
			scale,
			ScoreboardTextAlignment::center);
		if (network.deathmatch_mode)
		{
			draw_scoreboard_header_text(
				commands,
				frontend,
				language_text(language, 0x307),
				x + 321.0f * scale,
				top_y,
				scale,
				ScoreboardTextAlignment::center);
		}

		constexpr std::uint8_t team_icon[4] = {23, 25, 24, 26};
		for (std::uint8_t row = 0; row < team_count; ++row)
		{
			const std::uint8_t team = teams[row];
			const float row_y =
				top_y + (22.0f + 17.0f * row) * scale;
			const float text_y =
				top_y + (21.0f + 17.0f * row) * scale;
			draw_scoreboard_shape(
				commands, frontend, 22, x, row_y, scale);
			draw_scoreboard_shape(
				commands,
				frontend,
				team_icon[team],
				x + 450.0f * scale,
				top_y + (24.0f + 17.0f * row) * scale,
				scale);

			char rank[12];
			char name[48];
			char kills[24];
			char deaths[24];
			std::snprintf(
				rank,
				sizeof(rank),
				"%u",
				static_cast<unsigned>(row + 1));
			std::snprintf(
				name,
				sizeof(name),
				"%s %u",
				language_text(language, 0x309),
				static_cast<unsigned>(team + 1));
			std::snprintf(
				kills,
				sizeof(kills),
				"%d",
				network.team_score[team]);
			std::snprintf(
				deaths,
				sizeof(deaths),
				"%d",
				network.team_deaths[team]);
			draw_scoreboard_header_text(
				commands,
				frontend,
				rank,
				x + 6.0f * scale,
				text_y,
				scale,
				ScoreboardTextAlignment::center);
			draw_scoreboard_value_text(
				commands,
				frontend,
				name,
				x + 20.0f * scale,
				text_y,
				scale,
				ScoreboardTextAlignment::left);
			draw_scoreboard_value_text(
				commands,
				frontend,
				kills,
				x + 223.0f * scale,
				text_y,
				scale,
				ScoreboardTextAlignment::center);
			if (network.deathmatch_mode)
			{
				draw_scoreboard_value_text(
					commands,
					frontend,
					deaths,
					x + 321.0f * scale,
					text_y,
					scale,
					ScoreboardTextAlignment::center);
			}
		}
		draw_scoreboard_shape(
			commands,
			frontend,
			21,
			x,
			top_y + (22.0f + 17.0f * team_count) * scale,
			scale);
	}

	const float player_top_y = network.team_mode
		? center_y
			+ (-205.0f + 30.0f + 17.0f * team_count) * scale
		: center_y - 110.0f * scale;
	draw_scoreboard_shape(
		commands, frontend, 20, x, player_top_y, scale);
	draw_scoreboard_header_text(
		commands,
		frontend,
		language_text(language, 0xbf),
		x + 20.0f * scale,
		player_top_y,
		scale,
		ScoreboardTextAlignment::left);
	draw_scoreboard_header_text(
		commands,
		frontend,
		language_text(language, 0x308),
		x + 223.0f * scale,
		player_top_y,
		scale,
		ScoreboardTextAlignment::center);
	if (network.deathmatch_mode)
	{
		draw_scoreboard_header_text(
			commands,
			frontend,
			language_text(language, 0x307),
			x + 321.0f * scale,
			player_top_y,
			scale,
			ScoreboardTextAlignment::center);
	}
	draw_scoreboard_header_text(
		commands,
		frontend,
		language_text(language, 0x559),
		x + 418.0f * scale,
		player_top_y,
		scale,
		ScoreboardTextAlignment::center);

	constexpr std::uint8_t team_icon[4] = {23, 25, 24, 26};
	for (std::uint8_t row = 0; row < player_count; ++row)
	{
		const std::uint8_t player = players[row];
		const float row_y =
			player_top_y + (21.0f + 17.0f * row) * scale;
		const float text_y =
			player_top_y + (20.0f + 17.0f * row) * scale;
		draw_scoreboard_shape(
			commands, frontend, 22, x, row_y, scale);
		const std::int32_t team = network.object_team[player];
		if (network.team_mode && team >= 0 && team < 4)
		{
			draw_scoreboard_shape(
				commands,
				frontend,
				team_icon[team],
				x + 450.0f * scale,
				player_top_y
					+ (23.0f + 17.0f * row) * scale,
				scale);
		}
		const std::uint16_t scenario_shape =
			mission::deathmatch_scenarios_scoreboard_shape(
				mission, world, player);
		if (scenario_shape
			< render::kGameplayScoreboardShapeCount)
		{
			draw_scoreboard_shape(
				commands,
				frontend,
				static_cast<std::uint8_t>(scenario_shape),
				x + 352.0f * scale,
				row_y + 2.0f * scale,
				scale);
			const std::int32_t scenario_value =
				mission::deathmatch_scenarios_scoreboard_value(
					mission, world, player);
			if (scenario_value >= 0)
			{
				char value[24];
				std::snprintf(
					value, sizeof(value), "%d", scenario_value);
				draw_scoreboard_value_text(
					commands,
					frontend,
					value,
					x + 370.0f * scale,
					text_y,
					scale,
					ScoreboardTextAlignment::left);
			}
		}

		char rank[12];
		char kills[24];
		char deaths[24];
		char latency[24];
		std::snprintf(
			rank,
			sizeof(rank),
			"%u",
			static_cast<unsigned>(row + 1));
		std::snprintf(
			kills,
			sizeof(kills),
			"%d",
			network.player_kills[player]);
		std::snprintf(
			deaths,
			sizeof(deaths),
			"%d",
			network.player_deaths[player]);
		std::snprintf(
			latency,
			sizeof(latency),
			"%u",
			network.player_latency[player]);
		draw_scoreboard_header_text(
			commands,
			frontend,
			rank,
			x + 6.0f * scale,
			text_y,
			scale,
			ScoreboardTextAlignment::center);
		draw_scoreboard_value_text(
			commands,
			frontend,
			// Deathmatch_draw_scoreboard 0x004afde5..0x004afe11 passes
			// the player's stored name buffer directly, including empty
			// names; it does not invent a localized fallback.
			network.player_name[player],
			x + 20.0f * scale,
			text_y,
			scale,
			ScoreboardTextAlignment::left);
		draw_scoreboard_value_text(
			commands,
			frontend,
			kills,
			x + 223.0f * scale,
			text_y,
			scale,
			ScoreboardTextAlignment::center);
		if (network.deathmatch_mode)
		{
			draw_scoreboard_value_text(
				commands,
				frontend,
				deaths,
				x + 321.0f * scale,
				text_y,
				scale,
				ScoreboardTextAlignment::center);
		}
		draw_scoreboard_value_text(
			commands,
			frontend,
			latency,
			x + 418.0f * scale,
			text_y,
			scale,
			ScoreboardTextAlignment::center);
	}
	draw_scoreboard_shape(
		commands,
		frontend,
		21,
		x,
		player_top_y + (21.0f + 17.0f * player_count) * scale,
		scale);
}

void draw_control_prompt(
	const mission::Runtime& mission,
	const Config& config,
	const LanguageTable& language,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	const std::uint32_t action = mission.waiting_control;
	if (action >= kControlActionCount)
	{
		return;
	}

	const ControlBinding& binding = config.bindings[action];
	const char* key = control_binding_key_text(binding, action);
	const char* modifier = "";
	if (binding.modifier == 1) modifier = "SHIFT";
	else if (binding.modifier == 2) modifier = "CONTROL";
	else if (binding.modifier == 3) modifier = "ALT";

	char action_text[128];
	std::snprintf(
		action_text,
		sizeof(action_text),
		// Retail's literal at 0x00502510 is "%s = " including the
		// trailing space.  Its width participates in centering the entire
		// prompt and in positioning the following key cap.
		"%s = ",
		language_text(language, kControlLanguageIds[action]));
	const float scale = layout.element_scale;
	const float label_width = text_width(frontend, action_text, scale);
	const float plus_width = text_width(frontend, " + ", scale);
	const bool has_modifier = *modifier != '\0';
	const bool long_key = std::strlen(key) > 2;
	float total_width = label_width;
	if (has_modifier)
	{
		total_width += 92.0f * scale + plus_width;
	}
	// HUD_render_frame_callback performs this 28/92 choice from the binding
	// text even for a joystick-only record, before branching on scan code.
	total_width += (long_key ? 92.0f : 28.0f) * scale;

	const float center_x = layout.width * 0.5f;
	const float center_y = layout.height * 0.5f;
	float x = center_x - total_width * 0.5f;
	draw_text(
		commands,
		frontend,
		action_text,
		x,
		center_y - 123.0f * scale,
		scale);
	const char* title = language_text(language, 1454);
	draw_text(
		commands,
		frontend,
		title,
		center_x - text_width(frontend, title, scale) * 0.5f,
		center_y - 143.0f * scale,
		scale);
	x += label_width;

	if (*key != '\0')
	{
		if (has_modifier)
		{
			draw_shape(
				commands,
				frontend,
				373,
				x,
				center_y - 128.0f * scale,
				scale);
			draw_text(
				commands,
				frontend,
				modifier,
				x + 46.0f * scale
					- text_width(frontend, modifier, scale) * 0.5f,
				center_y - 125.0f * scale,
				scale);
			x += 92.0f * scale;
			draw_text(
				commands,
				frontend,
				" + ",
				x,
				center_y - 123.0f * scale,
				scale);
			x += plus_width;
		}

		draw_shape(
			commands,
			frontend,
			long_key ? 373 : 372,
			x,
			center_y - 128.0f * scale,
			scale);
		const float key_center = long_key ? 46.0f : 10.0f;
		draw_text(
			commands,
			frontend,
			key,
			x + key_center * scale
				- text_width(frontend, key, scale) * 0.5f,
			center_y - 125.0f * scale,
			scale);
	}
	else if (binding.joystick_control >= 0 || binding.mouse_control >= 0)
	{
		char control[96];
		control_binding_text(binding, control, sizeof(control));
		draw_text(
			commands,
			frontend,
			control,
			x + 4.0f * scale,
			center_y - 123.0f * scale,
			scale);
	}
}

bool project_unclipped(
	const render::MissionRenderFrame& camera,
	std::uint32_t width,
	std::uint32_t height,
	const glm::vec3& world,
	Point& result,
	float& depth)
{
	const glm::vec3 local =
		glm::transpose(camera.camera_orientation)
		* (world - camera.camera_position);
	depth = local.z;
	if (width == 0 || height == 0)
	{
		return false;
	}
	const float half_height = camera.vertical_tangent;
	const float half_width = camera.horizontal_tangent;
	const glm::vec2 framebuffer = math::camera_plane_to_framebuffer(
		{local.x / local.z, local.y / local.z},
		{half_width, half_height},
		{width, height});
	result.x = framebuffer.x;
	result.y = framebuffer.y;
	return true;
}

bool project(
	const render::MissionRenderFrame& camera,
	std::uint32_t width,
	std::uint32_t height,
	const glm::vec3& world,
	Point& result,
	float& depth)
{
	return project_unclipped(
			camera, width, height, world, result, depth)
		&& depth > 0.0f;
}

const game::WorldObject* target_object(
	const game::World& world,
	const mission::Runtime& mission)
{
	const game::WorldObject* target =
		game::world_resolve(world, world.selected_target);
	if (target == nullptr)
	{
		return nullptr;
	}
	// HUD_render_frame_callback (LANCER.EXE 0x0048448f..0x004844e1)
	// validates the retained player TargetRef before publishing the pointer
	// consumed by both target panels and the targeting overlay. Ordinary
	// missions admit 0x100 for allegiance class zero; scenario missions do
	// not. Invalid references stay selected internally but publish null.
	const std::uint32_t allowed_runtime_flags =
		target->allegiance_class == 0
			&& mission.network.deathmatch_scenario < 0
		? 0x00000100u
		: 0u;
	if ((target->runtime_flags & 0x00000200u) == 0
		|| (target->runtime_flags
			& ~allowed_runtime_flags
			& 0x10000d40u) != 0)
	{
		return nullptr;
	}
	const std::int16_t component = world.target_component;
	if (component >= 0
		&& (component >= target->component_count
			|| (target->components[component].runtime_flags
				& 0x0030u) != 0))
	{
		return nullptr;
	}
	return target;
}

const game::WorldObject* object_by_mission_index(
	const game::World& world,
	std::uint16_t mission_index)
{
	for (const game::WorldObject& object : world.objects)
	{
		if (object.active && object.mission_index == mission_index)
		{
			return &object;
		}
	}
	return nullptr;
}

void draw_schematic(
	const game::WorldObject& object,
	bool (&hits)[4],
	bool compact,
	float x,
	float y,
	float scale,
	const assets::ShipStatsTable& stats,
	const render::MissionRenderer& mission_renderer,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	DistortionDrawState& distortion)
{
	render::MissionModel model;
	if (!render::mission_model_for_type(object.type, model))
	{
		return;
	}
	const render::MissionHudSchematic& schematic =
		mission_renderer.hud_schematics[
			static_cast<std::size_t>(model)];
	if (schematic.ready)
	{
		const render::FrontendTexture& texture = schematic.shapes[0];
		draw_texture_distorted(
			commands,
			texture,
			frontend.shell.gameplay_hud_palette,
			x,
			y,
			scale,
			distortion,
			0xffffffff,
			compact);
		for (std::uint32_t bank = 0; bank < 4; ++bank)
		{
			if (!hits[bank])
			{
				continue;
			}
			const render::FrontendTexture& overlay =
				schematic.shapes[bank + 1];
			if (bgfx::isValid(overlay.handle))
			{
				draw_texture_distorted(
					commands,
					overlay,
					frontend.shell.gameplay_hud_palette,
					x,
					y,
					scale,
					distortion,
					0xffffffff,
					compact);
			}
			// HUD_draw_ship_schematic clears each retained bank-hit word
			// only after the corresponding schematic is actually drawn.
			hits[bank] = false;
		}
	}
	if (object.type == 215 || object.type == 142
		|| object.type >= assets::kShipStatsCount)
	{
		return;
	}
	const assets::ObjectTypeStats& type =
		stats.records[object.type].object;
	constexpr std::uint8_t bank_order[4] = {2, 3, 0, 1};
	const std::uint16_t primary_compact[4] = {158, 168, 163, 173};
	const std::uint16_t primary_flight[4] = {158, 168, 173, 163};
	const std::uint16_t secondary_compact[4] = {138, 148, 143, 153};
	const std::uint16_t secondary_flight[4] = {138, 148, 153, 143};
	// HUD_draw_ship_schematic, LANCER.EXE 0x00489766..0x00489ba3,
	// positions each overlay around the separately drawn ship image. The
	// compact top and bottom rows include the HUD's one-pixel alignment
	// adjustment relative to the executable coordinates.
	constexpr Point primary_compact_offsets[4] = {
		{10.0f, -4.0f},
		{2.0f, 52.0f},
		{63.0f, 6.0f},
		{-10.0f, 6.0f},
	};
	constexpr Point primary_flight_offsets[4] = {
		{11.0f, -4.0f},
		{0.0f, 52.0f},
		{-11.0f, 7.0f},
		{65.0f, 7.0f},
	};
	constexpr Point secondary_compact_offsets[4] = {
		{13.0f, -1.0f},
		{6.0f, 49.0f},
		{59.0f, 8.0f},
		{-4.0f, 8.0f},
	};
	constexpr Point secondary_flight_offsets[4] = {
		{13.0f, -1.0f},
		{5.0f, 49.0f},
		{-5.0f, 9.0f},
		{61.0f, 9.0f},
	};
	// Every division below calls the CRT helper at 0x004cf28c. It sets the
	// x87 rounding control to truncate before FISTP, so a C++ integer cast is
	// required here; nearest-even rounding selects the wrong HUD frames.
	for (std::uint32_t side = 0; side < 4; ++side)
	{
		const std::uint8_t bank = bank_order[side];
		const Point primary_offset =
			compact
				? primary_compact_offsets[side]
				: primary_flight_offsets[side];
		const Point secondary_offset =
			compact
				? secondary_compact_offsets[side]
				: secondary_flight_offsets[side];
		// Mirroring corrects the compact top and bottom arcs. Its side
		// shapes already encode the target-facing orientation.
		const bool mirror = compact && side < 2;
		const std::int32_t primary_frame =
			type.primary_bank_max == 0
				? 0
				: static_cast<std::int32_t>(
					object.primary_shields[bank]
					/ static_cast<float>(type.primary_bank_max)) - 1;
		if (primary_frame > 0)
		{
			const std::uint16_t base =
				compact ? primary_compact[side] : primary_flight[side];
			draw_shape_distorted(
				commands,
				frontend,
				static_cast<std::uint16_t>(base - primary_frame),
				x + primary_offset.x * scale,
				y + primary_offset.y * scale,
				scale,
				distortion,
				0xffffffff,
				mirror);
		}
		std::int32_t secondary_frame =
			type.structural_bank_max == 0
				? 0
				: static_cast<std::int32_t>(
					object.secondary_shields[bank]
					/ static_cast<float>(type.structural_bank_max)) - 1;
		// HUD_draw_ship_schematic, LANCER.EXE
		// 0x004899c2..0x00489a30, compresses the structural overlay frame
		// for protection states one and two with signed (2*n + 6) / 3.
		if (object.protection_state == 1
			|| object.protection_state == 2)
		{
			secondary_frame = (secondary_frame * 2 + 6) / 3;
		}
		if (secondary_frame > 0)
		{
			const std::uint16_t base =
				compact ? secondary_compact[side] : secondary_flight[side];
			draw_shape_distorted(
				commands,
				frontend,
				static_cast<std::uint16_t>(base - secondary_frame),
				x + secondary_offset.x * scale,
				y + secondary_offset.y * scale,
				scale,
				distortion,
				0xffffffff,
				mirror);
		}
	}
	if (!compact && type.primary_bank_max != 0)
	{
		constexpr std::uint16_t auxiliary_bases[2] = {178, 183};
		constexpr Point auxiliary_offsets[2] = {
			{8.0f, -9.0f},
			{-4.0f, 56.0f},
		};
		for (std::uint32_t side = 0; side < 2; ++side)
		{
			if (object.auxiliary_shields[side] <= 0.0f)
			{
				continue;
			}
			const std::int32_t frame =
				static_cast<std::int32_t>(
					object.auxiliary_shields[side]
					/ static_cast<float>(type.primary_bank_max)) - 1;
			if (frame > 0)
			{
				draw_shape(
					commands,
					frontend,
					static_cast<std::uint16_t>(
						auxiliary_bases[side] - frame),
					x + auxiliary_offsets[side].x * scale,
					y + auxiliary_offsets[side].y * scale,
					scale);
			}
		}
	}
}

void draw_schematic_base(
	const game::WorldObject& object,
	float x,
	float y,
	float scale,
	const render::MissionRenderer& mission_renderer,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	DistortionDrawState& distortion)
{
	render::MissionModel model;
	if (!render::mission_model_for_type(object.type, model))
	{
		return;
	}
	const render::MissionHudSchematic& schematic =
		mission_renderer.hud_schematics[
			static_cast<std::size_t>(model)];
	if (!schematic.ready)
	{
		return;
	}
	const render::FrontendTexture& texture = schematic.shapes[0];
	draw_texture_distorted(
		commands,
		texture,
		frontend.shell.gameplay_hud_palette,
		x,
		y,
		scale,
		distortion);
}

void draw_main_instruments(
	Runtime& runtime,
	const game::WorldObject& player,
	const assets::ShipStatsTable& stats,
	std::uint8_t camera_mode,
	const render::MissionRenderer& mission_renderer,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout,
	DistortionDrawState& distortion)
{
	const float scale = layout.element_scale;
	const auto draw_top_counter =
		[&](float base_x,
			float shape_x_offset,
			float shape_y_offset,
			std::uint16_t shape,
			float text_x_offset,
			std::int32_t counter)
		{
			const Point anchor =
				scalable_anchor(layout, base_x, 0.0f, 0.5f, 0.0f);
			draw_shape(
				commands,
				frontend,
				shape,
				anchor.x + shape_x_offset * scale,
				anchor.y + shape_y_offset * scale,
				scale);
			char value[24];
			std::snprintf(value, sizeof(value), "%d", counter);
			draw_text(
				commands,
				frontend,
				value,
				anchor.x + text_x_offset * scale
					- text_width(frontend, value, scale) * 0.5f,
				anchor.y + 30.0f * scale,
				scale);
		};
	// HUD_render_frame_callback 0x00485531..0x004857ca: the three
	// fixed top counters are afterburner fuel/100, persistent score, and
	// the signed chaff inventory at GameObject+0x5ec.
	draw_top_counter(
		57.0f, 0.0f, 0.0f, 205, 16.0f,
		player.afterburner_fuel / 100);
	draw_top_counter(
		95.0f, 0.0f, -4.0f, 208, 11.0f,
		player.score);
	if (runtime.hud_icon_draw[3])
	{
		// HUD_render_frame_callback 0x00485727..0x00485773 applies
		// the -26 adjustment to the icon's x coordinate, not its y.
		draw_top_counter(
			152.0f, -26.0f, 0.0f, 207, -9.0f,
			player.chaff_count);
	}

	const Point schematic = player_schematic_anchor(layout);
	draw_schematic(
		player,
		runtime.player_schematic_hits,
		false,
		schematic.x,
		schematic.y,
		scale,
		stats,
		mission_renderer,
		frontend,
		commands,
		distortion);

	const Point instrument_center =
		scalable_anchor(layout, 0.0f, 0.0f, 0.5f, 0.5f);
	const float left_x =
		instrument_center.x - 100.0f * scale;
	const float right_x =
		instrument_center.x + (100.0f - 67.0f) * scale;
	const float instrument_y =
		instrument_center.y - 74.0f * scale;
	draw_shape(commands, frontend, 127, left_x, instrument_y, scale);
	draw_shape_mirrored(
		commands, frontend, 127, right_x, instrument_y, scale);

	const float throttle = std::clamp(
		std::abs(player.throttle), 0.0f, 1.0f);
	const float maximum =
		game::world_effective_max_speed(
			player, stats, camera_mode);
	const float normalized_speed = maximum == 0.0f
		? 0.0f
		: std::clamp(player.speed / maximum, 0.0f, 1.0f);
	const auto clipped_arc =
		[&](std::uint16_t upper_shape,
			std::uint16_t lower_shape,
			float x,
			float fraction)
		{
			const render::FrontendTexture& upper =
				frontend.shell.gameplay_hud_shapes[upper_shape];
			const render::FrontendTexture& lower =
				frontend.shell.gameplay_hud_shapes[lower_shape];
			if (!bgfx::isValid(upper.handle) || !bgfx::isValid(lower.handle))
			{
				return;
			}
			const std::uint16_t split = static_cast<std::uint16_t>(
				std::clamp(
					static_cast<int>((1.0f - fraction) * upper.height),
					0,
					static_cast<int>(upper.height)));
			if (split != 0)
			{
				render::frontend_indexed_scaled_region(
					commands,
					upper,
					frontend.shell.gameplay_hud_palette,
					x + upper.offset_x * scale,
					instrument_y + upper.offset_y * scale,
					upper.width * scale,
					split * scale,
					0, 0, upper.width, split);
			}
			if (split < lower.height)
			{
				const std::uint16_t rows =
					static_cast<std::uint16_t>(lower.height - split);
				render::frontend_indexed_scaled_region(
					commands,
					lower,
					frontend.shell.gameplay_hud_palette,
					x + lower.offset_x * scale,
					instrument_y
						+ (lower.offset_y + split) * scale,
					lower.width * scale,
					rows * scale,
					0, split, lower.width, rows);
			}
		};
	clipped_arc(185, 184, left_x, normalized_speed);
	const float maximum_gun_energy =
		player.type < assets::kShipStatsCount
			? stats.records[player.type].object.gun_energy_max
			: 0.0f;
	const float gun_energy = maximum_gun_energy <= 0.0f
		? 0.0f
		: std::clamp(
			player.gun_energy / maximum_gun_energy,
			0.0f,
			1.0f);
	clipped_arc(248, 249, right_x, gun_energy);

	// HUD_render_frame_callback 0x004859d8..0x00485d46 places both
	// speed markers on the same 310-to-210 degree ellipse around the
	// left instrument's (100, 80) center. Each value is right-aligned
	// beside its marker at the executable's (-10, -8) offset.
	const auto speed_marker_position =
		[&](float fraction)
		{
			const float angle = glm::radians(
				310.0f - fraction * 100.0f);
			return Point{
				left_x
					+ (100.0f
						+ std::nearbyint(std::sin(angle) * 124.0f))
						* scale,
				instrument_y
					+ (80.0f
						+ std::nearbyint(std::cos(angle) * 94.0f))
						* scale,
			};
		};
	const auto draw_speed_marker =
		[&](float fraction, std::int32_t speed, std::uint32_t rgba)
		{
			const Point marker = speed_marker_position(fraction);
			draw_shape(
				commands,
				frontend,
				234,
				marker.x,
				marker.y,
				scale,
				rgba);
			char value[24];
			std::snprintf(value, sizeof(value), "%d", speed);
			draw_text(
				commands,
				frontend,
				value,
				marker.x - 10.0f * scale
					- text_width(frontend, value, scale),
				marker.y - 8.0f * scale,
				scale,
				-1,
				rgba);
		};

	// The selected-speed marker and number use the retail native-palette
	// intensity factor. They disappear within 0.1 after the threefold
	// separation scale, leaving the current-speed marker unobscured.
	const float speed_separation = std::min(
		std::abs(throttle - normalized_speed) * 3.0f,
		1.0f);
	if (speed_separation > 0.1f)
	{
		const std::uint32_t intensity = static_cast<std::uint32_t>(
			std::clamp(
				static_cast<int>(std::nearbyint(
					speed_separation * 255.0f)),
				0,
				255));
		const std::uint32_t faded_rgba =
			(intensity << 24)
			| (intensity << 16)
			| (intensity << 8)
			| 0xffu;
		draw_speed_marker(
			throttle,
			static_cast<std::int32_t>(std::nearbyint(
				maximum * player.throttle)),
			faded_rgba);
	}
	draw_speed_marker(
		normalized_speed,
		static_cast<std::int32_t>(std::nearbyint(player.speed)),
		0xffffffff);

	const std::uint16_t reticle =
		runtime.reticle_captured ? 216 : 215;
	draw_shape(
		commands,
		frontend,
		reticle,
		runtime.reticle_x,
		runtime.reticle_y,
		scale);
}

struct SensorContact
{
	float x;
	float y;
	float depth;
	std::uint16_t shape;
	std::uint32_t color;
	bool target;
	bool selected;
	bool nav_marker;
};

void draw_sensor_contact(
	const SensorContact& contact,
	float scale,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands)
{
	if (contact.nav_marker)
	{
		// HUD_render_sensor_scope 0x00488f8c..0x00488fd7 and
		// 0x0048913e..0x00489189 write exactly four isolated pixels.
		// The centre and the two-pixel diagonals remain transparent.
		constexpr glm::vec2 offsets[4] = {
			{0.0f, -1.0f},
			{0.0f, 1.0f},
			{-1.0f, 0.0f},
			{1.0f, 0.0f},
		};
		for (const glm::vec2& offset : offsets)
		{
			render::frontend_rgba_quad(
				commands,
				frontend.white,
				contact.x + offset.x * scale,
				contact.y + offset.y * scale,
				scale,
				scale,
				0xffffffff);
		}
		return;
	}
	const float depth = std::abs(contact.depth);
	if (depth > 0.0f)
	{
		// The stored y already contains the signed depth displacement.
		// Retail writes abs(depth) pixels back toward the equator: upward
		// for the pre-scope pass and downward for the post-scope pass.
		const float top = contact.depth > 0.0f
			? contact.y - (depth - 1.0f) * scale
			: contact.y;
		render::frontend_rgba_quad(
			commands,
			frontend.white,
			contact.x + scale,
			top,
			scale,
			depth * scale,
			contact.target
				? frontend.shell.gameplay_hud_rgba[0xff]
				: contact.selected
					? frontend.shell.gameplay_hud_rgba[0xfd]
					: contact.color);
	}
	draw_shape(
		commands,
		frontend,
		contact.target
			? 304 : contact.selected ? 230 : contact.shape,
		contact.x + 2.0f * scale,
		contact.y,
		scale);
}

void draw_sensor(
	const Runtime& runtime,
	const game::World& world,
	const game::WorldObject& player,
	const mission::Runtime& mission,
	const render::MissionRenderFrame& camera,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout,
	DistortionDrawState& distortion)
{
	constexpr float limits[3] = {90000.0f, 150000.0f, 230000.0f};
	constexpr float reciprocals[3] = {
		6.66666665e-6f,
		4.34782623e-6f,
		3.03030311e-6f,
	};
	const float scale = layout.element_scale;
	const Point center = sensor_center(layout);
	// HUD_render_sensor_scope 0x00489056..0x004890ad draws the scope at
	// (-66, -32) from the anchor used to project its contacts.
	const Point base = {
		center.x - 66.0f * scale,
		center.y - 32.0f * scale,
	};
	// LANCER.EXE HUD_render_sensor_scope uses the fixed work area
	// 0x005667b8..0x00566f88: exactly 100 records of 20 bytes.
	constexpr std::uint32_t kSensorContactCapacity = 100;
	SensorContact contacts[kSensorContactCapacity];
	std::uint32_t count = 0;
	const bool speaker_panel_visible =
		runtime.panels[0].animation == PanelAnimation::open
		|| runtime.panels[0].animation == PanelAnimation::opening;
	const std::uint16_t speaker =
		mission.presentation.speaker_mission_index;
	for (std::uint16_t object_index = 0;
		object_index < std::size(world.objects);
		++object_index)
	{
		const game::WorldObject& object =
			world.objects[object_index];
		if (!object.active)
		{
			continue;
		}
		const bool flyback = object_index == player.nav_point;
		if (!flyback
			&& (&object == &player
				|| (object.runtime_flags & 0x00000200u) == 0
				|| (object.runtime_flags & 0x00000c40u) != 0
				|| ((object.runtime_flags & 0x00000100u) != 0
					&& object.allegiance_class == 1)))
		{
			// HUD_render_sensor_scope 0x00488c90..0x00488d93 admits the
			// current flyback object first. Every other object excludes the
			// local player, requires 0x200, rejects 0x40/0x400/0x800, and
			// rejects 0x100 only for hostile allegiance one.
			continue;
		}
		const bool target =
			world.selected_target.index == object_index
			&& world.selected_target.generation == object.generation;
		// HUD_render_sensor_scope 0x00488d93..0x00488ebd subtracts
		// the viewed player's position, then applies the currently
		// published camera/view matrix. Side and rear views therefore
		// rotate the scope even though its origin remains the player.
		const glm::vec3 local =
			glm::transpose(camera.camera_orientation)
			* (object.scene_position - player.scene_position);
		const float distance = glm::length(local);
		const bool selected =
			speaker_panel_visible
			&& speaker != UINT16_MAX
			&& object.mission_index == speaker
			&& (object.runtime_flags & 0x00000100u) == 0;
		if (distance >= limits[runtime.sensor_mode])
		{
			continue;
		}
		if (count == kSensorContactCapacity)
		{
			break;
		}
		const float reciprocal = reciprocals[runtime.sensor_mode];
		// HUD_render_sensor_scope 0x00488d58..0x00488dac sends all
		// three projected contact terms through FUN_004c3330/FISTP.
		const float depth =
			std::nearbyint(local.y * reciprocal * 30.0f);
		contacts[count++] = {
			center.x
				+ std::nearbyint(
					local.x * reciprocal * 66.0f) * scale,
			std::clamp(
				center.y
					+ (std::nearbyint(
							-local.z * reciprocal * 43.0f)
						+ depth) * scale,
				0.0f,
				layout.height - 2.0f * scale),
			depth,
			static_cast<std::uint16_t>(
				object.allegiance_class == 1 ? 229 : 228),
			frontend.shell.gameplay_hud_rgba[
				object.allegiance_class == 1 ? 0x26 : 0x62],
			target,
			selected,
			flyback,
		};
	}
	// HUD_render_sensor_scope 0x00488f16..0x004890d8 puts the exact
	// zero-depth equator in the pre-scope pass; only negative contacts
	// are composited over the scope frame.
	for (std::uint32_t index = 0; index < count; ++index)
	{
		if (contacts[index].depth >= 0.0f)
		{
			draw_sensor_contact(
				contacts[index], scale, frontend, commands);
		}
	}
	draw_shape_distorted(
		commands,
		frontend,
		static_cast<std::uint16_t>(runtime.sensor_shape),
		base.x,
		base.y,
		scale,
		distortion);
	for (std::uint32_t index = 0; index < count; ++index)
	{
		if (contacts[index].depth < 0.0f)
		{
			draw_sensor_contact(
				contacts[index], scale, frontend, commands);
		}
	}
}

std::int16_t blindfire_selected_bullet_type(
	const game::WorldObject& player)
{
	const bool all_guns =
		(player.active_weapon_selection_bits & 0x0010u) != 0;
	// HUD_render_frame_callback, LANCER.EXE 0x0048606b..0x004860b3.
	// All-guns capture is only admitted by a type with one authored gun
	// group. The projectile exclusion is tested against the first mount of
	// the selected pair even in that one-group all-guns case.
	if ((all_guns && player.gun_pair_count != 1)
		|| player.gun_pair_count == 0)
	{
		return -1;
	}
	const std::uint8_t pair_index = static_cast<std::uint8_t>(
		(player.active_weapon_selection_bits & 0x0007u)
			% player.gun_pair_count);
	const std::int8_t first = player.gun_pairs[pair_index].first;
	if (first < 0
		|| static_cast<std::uint8_t>(first) >= player.gun_mount_count)
	{
		return -1;
	}
	return player.gun_mounts[
		static_cast<std::uint8_t>(first)].bullet_type;
}

void draw_lead_connector(
	const Point& target,
	const Point& lead,
	std::int32_t opening_percent,
	float scale,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands)
{
	const float dx = lead.x - target.x;
	const float dy = lead.y - target.y;
	const float band = 5.0f * scale;
	Point start = lead;
	if (std::abs(dx) >= std::abs(dy))
	{
		if (std::abs(dx) <= band)
		{
			return;
		}
		start.x = lead.x - std::copysign(band, dx);
		start.y = target.y + (start.x - target.x) * dy / dx;
	}
	else
	{
		if (std::abs(dy) <= band)
		{
			return;
		}
		start.y = lead.y - std::copysign(band, dy);
		start.x = target.x + (start.y - target.y) * dx / dy;
	}

	// HUD_render_targeting_overlay 0x0048b012..0x0048b0c8 first moves
	// five pixels inward from shape 303, then removes
	// (100 - acquisition_percentage) * 0.28 pixels from the target end.
	const glm::vec2 toward_target{
		target.x - start.x,
		target.y - start.y,
	};
	const float length = glm::length(toward_target);
	const float target_gap =
		(100.0f - static_cast<float>(opening_percent))
		* 0.280000001f * scale;
	if (length <= target_gap)
	{
		return;
	}
	const float retained = (length - target_gap) / length;
	const Point endpoint{
		start.x + toward_target.x * retained,
		start.y + toward_target.y * retained,
	};
	render::frontend_rgba_line(
		commands,
		frontend.white,
		// HUD_render_targeting_overlay 0x0048b093..0x0048b0c8 keeps
		// the clipping calculations in floating point, then FISTP-rounds
		// all four connector endpoints immediately before drawing.
		std::nearbyint(start.x),
		std::nearbyint(start.y),
		std::nearbyint(endpoint.x),
		std::nearbyint(endpoint.y),
		std::max(1.0f, scale),
		frontend.shell.gameplay_hud_rgba[0x26]);
}

void draw_secondary_contact_brackets(
	const Runtime& runtime,
	const game::World& world,
	const mission::Runtime& mission,
	const render::MissionRenderFrame& camera,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	// HUD_draw_secondary_contact_brackets 0x0048b0f0 accepts only an active
	// ordinary speaker while comms panel zero is open or opening. Compiled
	// static pilots use encoded IDs >= 0xffff and are deliberately excluded.
	const PanelAnimation panel = runtime.panels[0].animation;
	const std::uint16_t speaker =
		mission.presentation.speaker_mission_index;
	if (!mission.presentation.comms_active
		|| (panel != PanelAnimation::open
			&& panel != PanelAnimation::opening)
		|| speaker == UINT16_MAX)
	{
		return;
	}
	const game::WorldObject* object =
		object_by_mission_index(world, speaker);
	if (object == nullptr || !object->visible)
	{
		return;
	}
	Point center{};
	float center_depth;
	if (!project(
			camera,
			static_cast<std::uint32_t>(layout.width),
			static_cast<std::uint32_t>(layout.height),
			object->scene_position,
			center,
			center_depth))
	{
		return;
	}
	// HUD_draw_secondary_contact_brackets 0x0048b1b7..0x0048b20f
	// classifies the projected centre only after both axes pass through
	// FUN_004c3330/FISTP.
	const Point rounded_center{
		std::nearbyint(center.x),
		std::nearbyint(center.y),
	};
	if (rounded_center.x < 0.0f
		|| rounded_center.x >= layout.width
		|| rounded_center.y < 0.0f
		|| rounded_center.y >= layout.height)
	{
		return;
	}

	float left = layout.width;
	float top = layout.height;
	float right = 0.0f;
	float bottom = 0.0f;
	bool any = false;
	for (std::uint32_t corner = 0; corner < 8; ++corner)
	{
		const glm::vec3 local{
			(corner & 1) != 0
				? object->bounds_max.x : object->bounds_min.x,
			(corner & 2) != 0
				? object->bounds_max.y : object->bounds_min.y,
			(corner & 4) != 0
				? object->bounds_max.z : object->bounds_min.z,
		};
		Point projected;
		float depth;
		if (!project_unclipped(
				camera,
				static_cast<std::uint32_t>(layout.width),
				static_cast<std::uint32_t>(layout.height),
				object->scene_position
					+ object->scene_orientation * local,
				projected,
				depth))
		{
			continue;
		}
		// HUD_draw_secondary_contact_brackets (LANCER.EXE 0x0048b0f0)
		// perspective-divides all eight corners after testing only the
		// object's centre depth. Corners behind the camera still contribute.
		left = std::min(left, projected.x);
		top = std::min(top, projected.y);
		right = std::max(right, projected.x);
		bottom = std::max(bottom, projected.y);
		any = true;
	}
	if (!any)
	{
		return;
	}
	const float minimum = 15.0f * layout.element_scale;
	// Preserve the projected midpoint while applying the minimum extent. The
	// retail right/bottom-only growth produces a visible distant-target offset.
	if (right - left < minimum)
	{
		const float padding = (minimum - (right - left)) * 0.5f;
		left -= padding;
		right += padding;
	}
	if (bottom - top < minimum)
	{
		const float padding = (minimum - (bottom - top)) * 0.5f;
		top -= padding;
		bottom += padding;
	}
	// HUD_draw_secondary_contact_brackets 0x0048b3a8..0x0048b4de
	// sends all four projected anchors through FUN_004c3330/FISTP.
	const float bracket_left = std::nearbyint(left);
	const float bracket_top = std::nearbyint(top);
	const float bracket_right = std::nearbyint(right);
	const float bracket_bottom = std::nearbyint(bottom);
	draw_shape(
		commands, frontend, 298,
		bracket_left, bracket_top, layout.element_scale);
	draw_shape(
		commands, frontend, 299,
		bracket_right, bracket_top, layout.element_scale);
	draw_shape(
		commands, frontend, 300,
		bracket_left, bracket_bottom, layout.element_scale);
	draw_shape(
		commands, frontend, 301,
		bracket_right, bracket_bottom, layout.element_scale);
}

struct DirectionChevronClipStart
{
	std::int32_t x{};
	std::int32_t y{};
	bool valid{};
};

struct IntegerLinePoint
{
	std::int32_t x{};
	std::int32_t y{};
};

void clip_retail_hud_line(
	IntegerLinePoint& first,
	IntegerLinePoint& second,
	std::int32_t width,
	std::int32_t height)
{
	if (width <= 0 || height <= 0)
	{
		return;
	}
	// VFX_buffer_clip_line (LANCER.EXE 0x004aafc0..0x004ab286) is an
	// integer Cohen-Sutherland clip.  The VFX buffer stores its extents at
	// width/height and the accepted endpoints are then clamped to the
	// framebuffer's final width-1/height-1 pixels.
	const auto outcode =
		[width, height](const IntegerLinePoint& point)
		{
			std::uint8_t result = 0;
			if (point.y > height) result |= 0x01;
			if (point.y < 0) result |= 0x02;
			if (point.x > width) result |= 0x04;
			if (point.x < 0) result |= 0x08;
			return result;
		};

	bool accepted = false;
	for (;;)
	{
		const std::uint8_t first_code = outcode(first);
		const std::uint8_t second_code = outcode(second);
		if ((first_code | second_code) == 0)
		{
			accepted = true;
			break;
		}
		if ((first_code & second_code) != 0)
		{
			break;
		}

		const std::uint8_t code =
			first_code != 0 ? first_code : second_code;
		IntegerLinePoint clipped{};
		if ((code & 0x01) != 0)
		{
			clipped.y = height;
			clipped.x = first.x
				+ (height - first.y) * (second.x - first.x)
					/ (second.y - first.y);
		}
		else if ((code & 0x02) != 0)
		{
			clipped.y = 0;
			clipped.x = first.x
				+ (0 - first.y) * (second.x - first.x)
					/ (second.y - first.y);
		}
		else if ((code & 0x04) != 0)
		{
			clipped.x = width;
			clipped.y = first.y
				+ (width - first.x) * (second.y - first.y)
					/ (second.x - first.x);
		}
		else
		{
			clipped.x = 0;
			clipped.y = first.y
				+ (0 - first.x) * (second.y - first.y)
					/ (second.x - first.x);
		}
		if (code == first_code)
		{
			first = clipped;
		}
		else
		{
			second = clipped;
		}
	}
	if (accepted)
	{
		first.x = std::clamp(first.x, 0, width - 1);
		first.y = std::clamp(first.y, 0, height - 1);
		second.x = std::clamp(second.x, 0, width - 1);
		second.y = std::clamp(second.y, 0, height - 1);
	}
}

void draw_direction_chevron(
	glm::vec2 direction,
	std::uint32_t color,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout,
	DirectionChevronClipStart* retail_clip_start = nullptr)
{
	if (retail_clip_start != nullptr)
	{
		*retail_clip_start = {};
	}
	const float length = glm::length(direction);
	if (length <= 0.0f)
	{
		return;
	}
	direction /= length;
	const glm::vec2 perpendicular{-direction.y, direction.x};
	const float scale = layout.element_scale;
	const glm::vec2 center{
		std::nearbyint(layout.width * 0.5f),
		std::nearbyint(layout.height * 0.5f),
	};
	// HUD_render_targeting_overlay 0x00489e32..0x0048a045 quantizes the
	// 32-pixel tip and the retained -10-pixel step independently through
	// FUN_004c3330/FISTP. Collapsing them into one 22-pixel conversion
	// changes the base at fractional headings.
	const glm::vec2 tip_offset{
		std::nearbyint(direction.x * 32.0f * scale),
		std::nearbyint(direction.y * 32.0f * scale),
	};
	const glm::vec2 tip = center + tip_offset;
	const glm::vec2 base = tip + glm::vec2{
		std::nearbyint(direction.x * -10.0f * scale),
		std::nearbyint(direction.y * -10.0f * scale),
	};
	const glm::vec2 side = {
		std::nearbyint(perpendicular.x * 4.0f * scale),
		std::nearbyint(perpendicular.y * 4.0f * scale),
	};
	const glm::vec2 first = base + side;
	const glm::vec2 second = base - side;
	if (retail_clip_start != nullptr)
	{
		// HUD_render_targeting_overlay 0x0048a650..0x0048a65e passes the
		// chevron tip as the first endpoint of VFX_buffer_clip_line.
		retail_clip_start->x =
			static_cast<std::int32_t>(tip.x);
		retail_clip_start->y =
			static_cast<std::int32_t>(tip.y);
		retail_clip_start->valid = true;
	}
	const float thickness = std::max(1.0f, scale);
	render::frontend_rgba_line(
		commands, frontend.white,
		tip.x, tip.y, first.x, first.y, thickness, color);
	render::frontend_rgba_line(
		commands, frontend.white,
		tip.x, tip.y, second.x, second.y, thickness, color);
	render::frontend_rgba_line(
		commands, frontend.white,
		second.x, second.y, first.x, first.y, thickness, color);
}

void draw_navigation_marker(
	const game::World& world,
	const render::MissionRenderFrame& camera,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (world.camera_mode != 0
		|| player == nullptr
		|| player->nav_point == UINT16_MAX)
	{
		return;
	}
	if (player->nav_point >= std::size(world.objects))
	{
		return;
	}
	const game::WorldObject* marker =
		&world.objects[player->nav_point];
	if (!marker->active)
	{
		return;
	}
	const glm::vec3 local =
		glm::transpose(camera.camera_orientation)
		* (marker->position - camera.camera_position);
	draw_direction_chevron(
		{local.x, local.y},
		frontend.shell.gameplay_hud_rgba[0x1f],
		frontend,
		commands,
		layout);
}

void draw_projected_flyback_marker(
	const game::World& world,
	const render::MissionRenderFrame& camera,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (world.camera_mode != 0
		|| player == nullptr
		|| player->nav_point == UINT16_MAX
		|| player->nav_point >= std::size(world.objects))
	{
		return;
	}
	const game::WorldObject& marker =
		world.objects[player->nav_point];
	if (!marker.active)
	{
		return;
	}
	Point screen;
	float depth;
	if (!project(
			camera,
			static_cast<std::uint32_t>(layout.width),
			static_cast<std::uint32_t>(layout.height),
			marker.position,
			screen,
			depth))
	{
		return;
	}
	// HUD_render_frame_callback 0x00485453..0x0048552b projects the active
	// flyback object independently of its centre direction chevron and draws
	// retail HUD shape 0x15f at the rounded screen coordinate.
	draw_shape(
		commands,
		frontend,
		0x15f,
		std::nearbyint(screen.x),
		std::nearbyint(screen.y),
		layout.element_scale);
}

struct TargetScenarioDecoration
{
	std::uint8_t shape{UINT8_MAX};
	std::int32_t value{};
	bool draw_value{};
	bool visible{};
};

TargetScenarioDecoration target_scenario_decoration(
	const mission::Runtime& mission,
	const game::WorldObject& target)
{
	TargetScenarioDecoration result;
	const std::uint16_t player = target.mission_index;
	if (player >= mission::kDeathmatchScenarioPlayerCapacity)
	{
		return result;
	}
	switch (static_cast<mission::DeathmatchScenario>(
		mission.network.deathmatch_scenario))
	{
	case mission::DeathmatchScenario::nuclear_threat:
		result.shape = 16;
		result.value = target.deathmatch_scenario_counter == -1
			? 0
			: target.deathmatch_scenario_counter;
		result.draw_value = true;
		result.visible = true;
		break;
	case mission::DeathmatchScenario::dark_reign:
		result.shape = 17;
		result.visible =
			target.deathmatch_scenario_counter != -1;
		break;
	case mission::DeathmatchScenario::tag_bomb:
		result.shape = 15;
		result.visible =
			mission.deathmatch.tag_holder
			== static_cast<std::int8_t>(player);
		break;
	case mission::DeathmatchScenario::vampires:
		result.shape = 18;
		result.visible = mission.deathmatch.vampire[player];
		break;
	default:
		break;
	}
	return result;
}

void draw_targeting(
	Runtime& runtime,
	game::World& world,
	const mission::Runtime& mission,
	const assets::ShipStatsTable& ship_stats,
	const assets::GunStatsTable& gun_stats,
	const render::MissionRenderFrame& camera,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	draw_secondary_contact_brackets(
		runtime,
		world,
		mission,
		camera,
		frontend,
		commands,
		layout);
	draw_navigation_marker(
		world,
		camera,
		frontend,
		commands,
		layout);
	const game::WorldObject* target = target_object(world, mission);
	if (target == nullptr)
	{
		return;
	}
	render::MissionModel model;
	if (!render::mission_model_for_type(target->type, model))
	{
		return;
	}
	Point center{};
	float center_depth;
	const bool center_in_front = project(
		camera,
		static_cast<std::uint32_t>(layout.width),
		static_cast<std::uint32_t>(layout.height),
		target->scene_position,
		center,
		center_depth);
	const Point rounded_center{
		std::nearbyint(center.x),
		std::nearbyint(center.y),
	};
	// HUD_render_targeting_overlay 0x0048a0f8..0x0048a14e performs the
	// on-screen bounds test on FISTP-rounded centre coordinates.
	const bool center_on_screen = center_in_front
		&& rounded_center.x >= 0.0f
		&& rounded_center.x < layout.width
		&& rounded_center.y >= 0.0f
		&& rounded_center.y < layout.height;
	const float scale = layout.element_scale;
	char distance[32];
	game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (player == nullptr)
	{
		return;
	}
	// Both HUD_render_targeting_overlay and HUD_panel_render_contents read
	// the two live root SR nodes (+0x30 -> +0x3c), never the camera or the
	// newly integrated 25 Hz GameObject positions.
	const float range = glm::length(
		target->scene_position - player->scene_position);
	// HUD_render_targeting_overlay 0x0048a060..0x0048a078 rounds the range
	// through FUN_004c3330's x87 FISTP before the integer /1000.
	std::snprintf(
		distance, sizeof(distance), "%dk",
		static_cast<int>(std::nearbyint(range)) / 1000);

	if (!center_on_screen)
	{
		// HUD_build_direction_indicator (0x00489bc0) derives the edge
		// direction from the target and viewed player's live root SR nodes.
		// It transforms by the player's orientation and normalizes X/Y; the
		// target's camera-space depth is not used to reverse the direction.
		const glm::vec3 local =
			glm::transpose(player->scene_orientation)
			* (target->scene_position - player->scene_position);
		float dx = local.x;
		float dy = local.y;
		const std::uint32_t direction_color =
			frontend.shell.gameplay_hud_rgba[
				target->allegiance_class == 1 ? 0x26 : 0x62];
		DirectionChevronClipStart clip_start;
		draw_direction_chevron(
			{dx, dy},
			direction_color,
			frontend,
			commands,
			layout,
			&clip_start);
		const float length = std::sqrt(dx * dx + dy * dy);
		if (length > 0.0f)
		{
			dx /= length;
			dy /= length;
		}
		const std::int32_t screen_width =
			static_cast<std::int32_t>(layout.width);
		const std::int32_t screen_height =
			static_cast<std::int32_t>(layout.height);
		IntegerLinePoint clipped_start{
			clip_start.x,
			clip_start.y,
		};
		IntegerLinePoint clipped_endpoint{
			static_cast<std::int32_t>(
				std::nearbyint(layout.width * 0.5f))
				+ static_cast<std::int32_t>(
					std::nearbyint(dx * layout.width)),
			static_cast<std::int32_t>(
				std::nearbyint(layout.height * 0.5f))
				+ static_cast<std::int32_t>(
					std::nearbyint(dy * layout.height)),
		};
		if (clip_start.valid)
		{
			// Retail 0x0048a5c5..0x0048a65e first FISTP-quantizes a
			// full-buffer endpoint, then sends that integer segment
			// through VFX_buffer_clip_line.  Floating-point ray/edge
			// intersection produces different marker edges and pixels.
			clip_retail_hud_line(
				clipped_start,
				clipped_endpoint,
				screen_width,
				screen_height);
		}
		const float maximum_y = layout.height - 1.0f;
		const float x =
			static_cast<float>(clipped_endpoint.x);
		const float y =
			static_cast<float>(clipped_endpoint.y);
		const std::uint16_t base =
			target->allegiance_class == 1 ? 364 : 368;
		float text_x;
		float text_y;
		std::uint8_t alignment;
		// LANCER.EXE 0x0048a680..0x0048a7b8 tests the clipped
		// endpoint in this order and applies these exact retail offsets.
		if (y == 0.0f)
		{
			draw_shape(
				commands, frontend, base + 3,
				x, 12.0f * scale, scale);
			text_x = x - 2.0f * scale;
			text_y = y + 11.0f * scale;
			alignment = 1;
		}
		else if (y >= maximum_y)
		{
			draw_shape(
				commands, frontend, base,
				x, y - 4.0f * scale, scale);
			text_x = x - 2.0f * scale;
			text_y = y - 16.0f * scale;
			alignment = 1;
		}
		else if (x == 0.0f)
		{
			draw_shape(
				commands, frontend, base + 1,
				8.0f * scale, y, scale);
			text_x = x + 9.0f * scale;
			text_y = y - 6.0f * scale;
			alignment = 0;
		}
		else
		{
			draw_shape(
				commands, frontend, base + 2,
				x - 6.0f * scale, y, scale);
			text_x = x - 8.0f * scale;
			text_y = y - 6.0f * scale;
			alignment = 2;
		}
		const float width = text_width(frontend, distance, scale);
		if (alignment == 1)
		{
			text_x -= width * 0.5f;
		}
		else if (alignment == 2)
		{
			text_x -= width;
		}
		draw_text(
			commands,
			frontend,
			distance,
			text_x,
			text_y,
			scale,
			target->allegiance_class);
		return;
	}

	float left = layout.width;
	float top = layout.height;
	float right = 0.0f;
	float bottom = 0.0f;
	bool any = false;
	const game::ObjectModelReference* selected_model = nullptr;
	glm::mat4 selected_model_transform{1.0f};
	if (world.target_component >= 0
		&& world.target_component < target->component_count)
	{
		const game::ObjectComponent& component =
			target->components[world.target_component];
		if (component.model_reference >= 0
			&& component.model_reference
				< static_cast<std::int16_t>(
					target->model_references.size()))
		{
			selected_model =
				&target->model_references[
					component.model_reference];
			// TargetRef_resolve_model (0x004018f0) supplies the live
			// component scene node at 0x0048a0ce..0x0048a0e2; the
			// 0x0048a963..0x0048aa58 branch projects that node's own
			// bounds through its articulated transform.
			selected_model_transform = selected_model->scene_transform;
		}
	}
	for (std::uint32_t corner = 0; corner < 8; ++corner)
	{
		const glm::vec3 local{
			(corner & 1) != 0
				? (selected_model == nullptr
					? target->bounds_max.x
					: selected_model->bounds_max.x)
				: (selected_model == nullptr
					? target->bounds_min.x
					: selected_model->bounds_min.x),
			(corner & 2) != 0
				? (selected_model == nullptr
					? target->bounds_max.y
					: selected_model->bounds_max.y)
				: (selected_model == nullptr
					? target->bounds_min.y
					: selected_model->bounds_min.y),
			(corner & 4) != 0
				? (selected_model == nullptr
					? target->bounds_max.z
					: selected_model->bounds_max.z)
				: (selected_model == nullptr
					? target->bounds_min.z
					: selected_model->bounds_min.z),
		};
		const glm::vec3 model_point =
			selected_model == nullptr
				? local
				: glm::vec3(
					selected_model_transform
					* glm::vec4(local, 1.0f));
		Point projected;
		float depth;
		if (!project_unclipped(
				camera,
				static_cast<std::uint32_t>(layout.width),
				static_cast<std::uint32_t>(layout.height),
				target->scene_position
					+ target->scene_orientation * model_point,
				projected,
				depth))
		{
			continue;
		}
		// HUD_render_targeting_overlay 0x0048aa5a..0x0048ab8e likewise
		// transforms and divides every bounds corner without a per-corner
		// near-plane rejection.
		left = std::min(left, projected.x);
		top = std::min(top, projected.y);
		right = std::max(right, projected.x);
		bottom = std::max(bottom, projected.y);
		any = true;
	}
	if (!any)
	{
		return;
	}
	const float minimum = 15.0f * scale;
	// Preserve the projected midpoint while applying the minimum extent. The
	// retail right/bottom-only growth produces a visible distant-target offset.
	if (right - left < minimum)
	{
		const float padding = (minimum - (right - left)) * 0.5f;
		left -= padding;
		right += padding;
	}
	if (bottom - top < minimum)
	{
		const float padding = (minimum - (bottom - top)) * 0.5f;
		top -= padding;
		bottom += padding;
	}
	constexpr std::uint16_t bases[3] = {290, 294, 290};
	const std::uint16_t base =
		bases[std::clamp<std::int16_t>(
			target->allegiance_class, 0, 2)];
	// HUD_render_targeting_overlay 0x0048abfa..0x0048ace9 likewise
	// quantizes every primary bracket anchor through x87 FISTP.
	const float bracket_left = std::nearbyint(left);
	const float bracket_top = std::nearbyint(top);
	const float bracket_right = std::nearbyint(right);
	const float bracket_bottom = std::nearbyint(bottom);
	// HUD_render_targeting_overlay 0x0048abc4..0x0048ad0c scales the
	// primary-bracket palette by the lock ring's opening percentage. It
	// stops drawing these four shapes at ten percent, while the network
	// decorations, distance, weapon-lead marker, and connector below remain.
	const float bracket_brightness = std::min(
		static_cast<float>(runtime.missile_lock.opening_percent) * 0.01f,
		1.0f);
	if (bracket_brightness > 0.1f)
	{
		const std::uint32_t channel = static_cast<std::uint32_t>(
			std::nearbyint(bracket_brightness * 255.0f));
		const std::uint32_t bracket_tint =
			channel * 0x01010100u | 0xffu;
		draw_shape(
			commands, frontend, base,
			bracket_left, bracket_top, scale, bracket_tint);
		draw_shape(
			commands, frontend, base + 1,
			bracket_right, bracket_top, scale, bracket_tint);
		draw_shape(
			commands, frontend, base + 2,
			bracket_left, bracket_bottom, scale, bracket_tint);
		draw_shape(
			commands, frontend, base + 3,
			bracket_right, bracket_bottom, scale, bracket_tint);
	}
	if (mission.network.role != mission::NetworkRole::offline
		&& world.selected_target.index < mission.player_prefix_count
		&& world.selected_target.index
			< mission::kNetworkPlayerCapacity)
	{
		// HUD_render_targeting_overlay 0x0048ad0c..0x0048ad73 places
		// the selected network player's stored name above the brackets
		// with the gameplay-message font. The x coordinate is rounded
		// before the retail twelve-pixel inset.
		render::frontend_gameplay_message_text(
			commands,
			frontend,
			mission.network.player_name[
				world.selected_target.index],
			bracket_left - 12.0f * scale,
			std::nearbyint(top - 24.0f * scale),
			0xffffffff,
			scale);
		const std::int32_t team =
			mission.network.object_team[
				world.selected_target.index];
		if (mission.network.team_mode
			&& team >= 0
			&& team < 4)
		{
			// Deathmatch_draw_target_status (LANCER.EXE
			// 0x004b1650..0x004b16f2) draws the selected network
			// player's team shape at (-26,-19) from the rounded
			// target-bracket corner before dispatching scenario HUD.
			constexpr std::uint8_t team_shape[4] = {
				23, 25, 24, 26,
			};
			draw_scoreboard_shape(
				commands,
				frontend,
				team_shape[team],
				bracket_left - 26.0f * scale,
				bracket_top - 19.0f * scale,
				scale);
		}
		const TargetScenarioDecoration decoration =
			target_scenario_decoration(mission, *target);
		if (decoration.visible)
		{
			// FUN_004b1650's scenario dispatch at 0x004b1711..0x004b1721
			// reaches the target callbacks at 0x004b3840, 0x004b4300,
			// 0x004b47e0, and 0x004b5060. Every callback anchors its
			// scoreboard shape at (-33,+2) from the rounded bracket corner.
			draw_scoreboard_shape(
				commands,
				frontend,
				decoration.shape,
				bracket_left - 33.0f * scale,
				bracket_top + 2.0f * scale,
				scale);
			if (decoration.draw_value)
			{
				char value[24];
				std::snprintf(
					value,
					sizeof(value),
					"%d",
					decoration.value);
				const float value_x =
					bracket_left - 24.0f * scale
					- std::trunc(
						gameplay_message_text_width(
							frontend,
							value,
							scale)
						* 0.5f);
				render::frontend_gameplay_message_text(
					commands,
					frontend,
					value,
					value_x,
					bracket_top + 18.0f * scale,
					0xffffffff,
					scale);
			}
		}
	}
	draw_text(
		commands,
		frontend,
		distance,
		// HUD_render_targeting_overlay 0x0048ad9c..0x0048addc
		// right-aligns the range at FISTP(right)+10; it is not centred
		// beneath the bracket pair.
		bracket_right + 10.0f * scale
			- text_width(frontend, distance, scale),
		std::nearbyint(bottom + 9.0f * scale),
		scale,
		target->allegiance_class);

	glm::vec3 lead_point;
	Point lead;
	float lead_depth;
	const std::int16_t target_name_id =
		target->type < assets::kShipStatsCount
			? ship_stats.records[target->type].object.name_language_id
			: 0;
	if ((target->runtime_flags & game::kObjectFlagCompound) == 0
		&& target_name_id != 0
		&& game::weapons_compute_lead_point(
			*player,
			*target,
			world.target_component,
			gun_stats,
			1.0f,
			lead_point)
		&& project(
			camera,
			static_cast<std::uint32_t>(layout.width),
			static_cast<std::uint32_t>(layout.height),
			lead_point,
			lead,
			lead_depth))
	{
		runtime.weapon_lead_world = lead_point;
		runtime.weapon_lead_x = std::nearbyint(lead.x);
		runtime.weapon_lead_y = std::nearbyint(lead.y);
		runtime.weapon_lead_valid = true;
		// The local projectile path reads the world-space global written by
		// this current overlay, while +0x674 was captured from the previous
		// overlay's projected coordinates above.
		if (player->blindfire_active)
		{
			player->blindfire_aim_point = lead_point;
		}
		// HUD_render_targeting_overlay 0x0048ae60..0x0048aecd
		// FISTP-rounds the projected lead point for shape 303.
		draw_shape(
			commands,
			frontend,
			303,
			std::nearbyint(lead.x),
			std::nearbyint(lead.y),
			scale);
		draw_lead_connector(
			center,
			lead,
			runtime.missile_lock.opening_percent,
			scale,
			frontend,
			commands);
	}
}

void draw_meter(
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	float x,
	float y,
	float value,
	float scale)
{
	draw_shape(commands, frontend, 224, x, y, scale);
	// HUD_draw_system_health_meter, LANCER.EXE 0x00488b30, quantizes the
	// 77-pixel fill boundary through FUN_004c3330's x87 FISTP.
	const std::int32_t pixels =
		static_cast<std::int32_t>(
			std::nearbyint(value * 77.0f));
	const std::int32_t first = std::max(pixels, 0);
	const std::int32_t width = 78 - first;
	if (width <= 0)
	{
		return;
	}
	const render::FrontendTexture& fill =
		frontend.shell.gameplay_hud_shapes[223];
	render::frontend_indexed_scaled_region(
		commands,
		fill,
		frontend.shell.gameplay_hud_palette,
		x + (first - 1) * scale,
		y - scale,
		width * scale,
		5.0f * scale,
		static_cast<std::uint16_t>(first),
		0,
		static_cast<std::uint16_t>(width),
		5);
}

void draw_vertical_health_bar(
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	float x,
	float y,
	float fraction,
	float scale)
{
	// LANCER.EXE panel case 13 at 0x0048704a..0x00487057 clips the paired
	// damage/health shapes at FISTP(fraction * 38). Both shapes are 4x38 with
	// the same (-1,-1) anchor, so retaining the source row preserves
	// their pixel alignment on either side of the split.
	const std::int32_t healthy_rows = std::clamp(
		static_cast<std::int32_t>(
			std::nearbyint(fraction * 38.0f)),
		0,
		38);
	const std::int32_t damaged_rows = 38 - healthy_rows;
	const auto region =
		[&](std::uint16_t shape,
			std::int32_t source_y,
			std::int32_t rows)
		{
			if (rows <= 0)
			{
				return;
			}
			const render::FrontendTexture& texture =
				frontend.shell.gameplay_hud_shapes[shape];
			render::frontend_indexed_scaled_region(
				commands,
				texture,
				frontend.shell.gameplay_hud_palette,
				x + texture.offset_x * scale,
				y + (texture.offset_y + source_y) * scale,
				texture.width * scale,
				rows * scale,
				0,
				static_cast<std::uint16_t>(source_y),
				texture.width,
				static_cast<std::uint16_t>(rows));
		};
	region(242, 0, damaged_rows);
	region(243, damaged_rows, healthy_rows);
}

void draw_shape_rows(
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	std::uint16_t shape,
	float x,
	float y,
	std::int32_t source_y,
	std::int32_t rows,
	float scale)
{
	if (rows <= 0 || shape >= render::kGameplayHudShapeCount)
	{
		return;
	}
	const render::FrontendTexture& texture =
		frontend.shell.gameplay_hud_shapes[shape];
	const std::int32_t first = std::clamp(
		source_y, 0, static_cast<std::int32_t>(texture.height));
	const std::int32_t count = std::clamp(
		rows, 0, static_cast<std::int32_t>(texture.height) - first);
	if (count == 0)
	{
		return;
	}
	render::frontend_indexed_scaled_region(
		commands,
		texture,
		frontend.shell.gameplay_hud_palette,
		x + texture.offset_x * scale,
		y + (texture.offset_y + first) * scale,
		texture.width * scale,
		count * scale,
		0,
		static_cast<std::uint16_t>(first),
		texture.width,
		static_cast<std::uint16_t>(count));
}

void draw_component_health_bar(
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	float x,
	float y,
	float fraction,
	float scale)
{
	draw_shape(commands, frontend, 222, x, y, scale);
	const std::int32_t healthy = std::clamp(
		static_cast<std::int32_t>(
			std::nearbyint(fraction * 38.0f)),
		0,
		38);
	draw_shape_rows(
		frontend,
		commands,
		219,
		x,
		y,
		0,
		38 - healthy,
		scale);
}

void draw_hull_health_bar(
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	float x,
	float y,
	float fraction,
	bool structural_shield_hull,
	float scale)
{
	const std::int32_t healthy = std::clamp(
		static_cast<std::int32_t>(
			std::nearbyint(fraction * 98.0f)),
		0,
		98);
	const std::int32_t damaged = 98 - healthy;
	draw_shape_rows(
		frontend,
		commands,
		220,
		x,
		y,
		damaged,
		healthy,
		scale);
	// Retail deliberately starts the red overlay seven rows below the bar
	// origin for collision-class five and ten rows below it otherwise.
	draw_shape_rows(
		frontend,
		commands,
		219,
		x,
		y + (structural_shield_hull ? 7.0f : 10.0f) * scale,
		0,
		damaged,
		scale);
}

void draw_panel_decorations(
	std::uint8_t panel,
	float x,
	float y,
	float scale,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands)
{
	const PanelDescriptor& descriptor = kPanels[panel];
	for (std::uint8_t index = 0;
		index < descriptor.decoration_count;
		++index)
	{
		const Decoration& decoration =
			kDecorations[descriptor.decorations[index]];
		if (decoration.draw_mode != 0)
		{
			draw_shape_mirrored(
				commands,
				frontend,
				decoration.shape,
				x + decoration.x * scale,
				y + decoration.y * scale,
				scale,
				decoration.draw_mode);
		}
		else
		{
			draw_shape(
				commands,
				frontend,
				decoration.shape,
				x + decoration.x * scale,
				y + decoration.y * scale,
				scale);
		}
	}
}

void draw_target_panel(
	bool expanded,
	std::uint16_t target_index,
	std::int16_t selected_component,
	bool (&schematic_hits)[4],
	const game::WorldObject& player,
	const game::WorldObject& target,
	const mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	const LanguageTable& language,
	const render::MissionRenderer& mission_renderer,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	float x,
	float y,
	float scale,
	DistortionDrawState& distortion)
{
	const std::int16_t name_id =
		target.type < assets::kShipStatsCount
			? stats.records[target.type].object.name_language_id
			: 0;
	char distance[32];
	// HUD_panel_render_contents 0x00487bcd..0x00487c27 uses the same x87
	// nearest-even helper for the complete distance and the target speed.
	std::snprintf(
		distance,
		sizeof(distance),
		"%dk",
		static_cast<int>(std::nearbyint(
			glm::length(
				target.scene_position - player.scene_position))) / 1000);
	char speed[32];
	std::snprintf(
		speed,
		sizeof(speed),
		"%d kps",
		static_cast<int>(std::nearbyint(target.speed)));
	if (!expanded)
	{
		char scenario_label[100]{};
		const bool have_scenario_label =
			sl_open::mission::deathmatch_scenarios_target_label(
				mission,
				target,
				language,
				scenario_label,
				sizeof(scenario_label));
		// The player schematic extends inward from the shared fixed radar
		// offset. Mirror that placement for the compact target schematic while
		// retaining the player's bottom baseline on wide displays.
		draw_schematic(
			target, schematic_hits, true,
			x - 68.0f * scale, y - 44.0f * scale,
			scale, stats, mission_renderer, frontend, commands,
			distortion);
		if (name_id != 0)
		{
			draw_text(
				commands,
				frontend,
				language_text(language, name_id),
				x + 55.0f * scale,
				y - 67.0f * scale,
				scale);
		}
		const assets::PilotPresentationDefinition* pilot =
			assets::pilot_presentation(target.pilot);
		const char* pilot_label = nullptr;
		if (pilot != nullptr)
		{
			// HUD_panel_render_contents 0x00487ca8..0x00487d1f replaces
			// the authored pilot label with the network player's name when
			// the selected live object is in the multiplayer player prefix.
			// The three special online object types skip this row entirely.
			if (mission.network.role != mission::NetworkRole::offline
				&& target_index < mission.player_prefix_count
				&& target_index < mission::kNetworkPlayerCapacity)
			{
				pilot_label =
					mission.network.player_name[target_index];
			}
			else if (mission.network.role
					== mission::NetworkRole::offline
				|| (target.type != 0xd7
					&& target.type != 0x8d
					&& target.type != 0x8e))
			{
				pilot_label = language_text(
					language,
					static_cast<std::uint16_t>(
						pilot->portrait_or_movie_id));
			}
		}
		if (pilot_label != nullptr)
		{
			draw_text(
				commands,
				frontend,
				pilot_label,
				x + 55.0f * scale,
				y - 55.0f * scale,
				scale);
		}
		draw_text(
			commands,
			frontend,
			distance,
			x + 55.0f * scale,
			y - 43.0f * scale,
			scale);
		draw_text(
			commands,
			frontend,
			speed,
			x + 55.0f * scale,
			y - 31.0f * scale,
			scale);
		if (have_scenario_label)
		{
			draw_text(
				commands,
				frontend,
				scenario_label,
				x + 55.0f * scale,
				y - 19.0f * scale,
				scale);
		}
		return;
	}
	draw_schematic_base(
		target,
		x - 208.0f * scale,
		y - 128.0f * scale,
		scale,
		mission_renderer,
		frontend,
		commands,
		distortion);
	if (name_id != 0)
	{
		const char* name = language_text(language, name_id);
		draw_text(
			commands,
			frontend,
			name,
			x - 2.0f * scale - text_width(frontend, name, scale),
			y - 157.0f * scale,
			scale);
	}
	if (target.type != 111 && target.type != 112
		&& selected_component >= 0
		&& selected_component < target.component_count)
	{
		const game::ObjectComponent& component =
			target.components[selected_component];
		if (component.model_type < std::size(kComponentShapes))
		{
			const std::int16_t language_id =
				kComponentLanguageIds[component.model_type];
			const std::int16_t shape =
				kComponentShapes[component.model_type];
			if (language_id >= 0 && shape >= 0)
			{
				draw_text(
					commands,
					frontend,
					language_text(
						language,
						static_cast<std::uint16_t>(language_id)),
					x - 120.0f * scale,
					y - 51.0f * scale,
					scale);
				draw_shape_distorted(
					commands,
					frontend,
					static_cast<std::uint16_t>(shape),
					x - 186.0f * scale,
					y - 52.0f * scale,
					scale,
					distortion);
				if (component.maximum_health > 0.0f)
				{
					draw_component_health_bar(
						frontend,
						commands,
						x - 198.0f * scale,
						y - 50.0f * scale,
						component.health
							/ component.maximum_health,
						scale);
				}
			}
		}
	}

	float hull_health = 0.0f;
	bool have_hull_health = false;
	const bool structural_shield_hull =
		target.collision_class == 5;
	if (structural_shield_hull
		&& target.type < assets::kShipStatsCount)
	{
		const std::int32_t maximum =
			stats.records[target.type].object.structural_bank_max * 6;
		if (maximum > 0)
		{
			hull_health = *std::min_element(
					std::begin(target.secondary_shields),
					std::end(target.secondary_shields))
				/ static_cast<float>(maximum);
			have_hull_health = true;
		}
	}
	else
	{
		// HUD_panel_render_contents (0x004878b2..0x0048791d) walks the
		// live GameObject+0x128 SR model-wrapper array, not the separately
		// registered component records. The first live authored type-one
		// model with hit points owns the expanded target hull bar.
		for (const game::ObjectModelReference& candidate
			: target.model_references)
		{
			if (candidate.model_type == 1
				&& candidate.maximum_health > 0.0f
				&& !candidate.removed)
			{
				hull_health =
					candidate.health / candidate.maximum_health;
				have_hull_health = true;
				break;
			}
		}
	}
	if (have_hull_health)
	{
		draw_hull_health_bar(
			frontend,
			commands,
			x - 7.0f * scale,
			y - 127.0f * scale,
			hull_health,
			structural_shield_hull,
			scale);
	}
	draw_text(
		commands,
		frontend,
		distance,
		x - 3.0f * scale - text_width(frontend, distance, scale),
		y - 29.0f * scale,
		scale);
	draw_text(
		commands,
		frontend,
		speed,
		x - 3.0f * scale - text_width(frontend, speed, scale),
		y - 17.0f * scale,
		scale);
}

void draw_objective_panel(
	const mission::Runtime& mission,
	const LanguageTable& language,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	float x,
	float y,
	float scale)
{
	draw_text(
		commands,
		frontend,
		language_text(language, 296),
		x - 3.0f * scale
			- text_width(
				frontend,
				language_text(language, 296),
				scale),
		y - 78.0f * scale,
		scale);
	const std::uint16_t objective = mission.current_objective;
	if (mission.no_current_objective || objective >= 10)
	{
		// Retail's no-current-objective flag takes this early branch after
		// the title and right-aligns language string 0x54a at y - 66.
		const char* no_current = language_text(language, 1354);
		draw_text(
			commands,
			frontend,
			no_current,
			x - 3.0f * scale
				- text_width(frontend, no_current, scale),
			y - 66.0f * scale,
			scale);
		return;
	}
	const char* status = language_text(
		language,
		mission.objectives[objective] == 2 ? 298 : 299);
	draw_text(
		commands,
		frontend,
		status,
		x - 3.0f * scale - text_width(frontend, status, scale),
		y - 66.0f * scale,
		scale);
	char buffer[256];
	const std::uint16_t objective_language_id =
		mission::objective_language_id(
			mission.mission_number,
			mission.mission_25_alternate,
			objective);
	if (objective_language_id == UINT16_MAX)
	{
		std::snprintf(
			buffer,
			sizeof(buffer),
			"%s",
			"ERROR: No mission Objectives defined");
	}
	else
	{
		std::snprintf(
			buffer,
			sizeof(buffer),
			"%s",
			language_text(language, objective_language_id));
	}
	const char* cursor = buffer;
	float line_y = y - 48.0f * scale;
	// HUD_draw_wrapped_text, LANCER.EXE 0x00480fd0..0x00481290, consumes
	// glyph advances until the next byte no longer fits. It honours authored
	// newlines, prefers a preceding space, then a preceding hyphen, and
	// hard-splits an unbroken word by retaining its last fitting byte for the
	// next line while drawing a hyphen in its place.
	for (std::uint32_t line_index = 0;
		line_index < 6 && *cursor != '\0';
		++line_index)
	{
		const char* fit = cursor;
		std::int32_t remaining = 140;
		while (*fit != '\0' && *fit != '\n' && remaining > 0)
		{
			const std::uint8_t character =
				static_cast<std::uint8_t>(*fit);
			const std::int32_t advance =
				character
						< frontend.shell.gameplay_hud_glyph_count
					? frontend.shell.gameplay_hud_glyphs[
							character].width
					: 0;
			remaining -= advance;
			if (remaining < 0)
			{
				remaining = 0;
			}
			else
			{
				++fit;
			}
		}
		char line[1024];
		auto draw_line =
			[&](const char* begin, std::size_t length, bool hyphen)
			{
				if (length == 0 && !hyphen)
				{
					return;
				}
				std::memcpy(line, begin, length);
				if (hyphen)
				{
					line[length++] = '-';
				}
				line[length] = '\0';
				draw_text(
					commands,
					frontend,
					line,
					x - 140.0f * scale,
					line_y,
					scale);
			};
		if (*fit == '\0')
		{
			draw_line(
				cursor,
				static_cast<std::size_t>(fit - cursor),
				false);
			return;
		}
		if (*fit == '\n')
		{
			draw_line(
				cursor,
				static_cast<std::size_t>(fit - cursor),
				false);
			cursor = fit + 1;
			line_y += 14.0f * scale;
			continue;
		}
		const char* split = fit;
		while (split > cursor && *split != ' ')
		{
			--split;
		}
		if (split > cursor)
		{
			draw_line(
				cursor,
				static_cast<std::size_t>(split - cursor),
				false);
			cursor = split + 1;
			line_y += 14.0f * scale;
			continue;
		}
		split = fit;
		while (split > cursor && *split != '-')
		{
			--split;
		}
		if (split > cursor)
		{
			draw_line(
				cursor,
				static_cast<std::size_t>(split - cursor + 1),
				false);
			cursor = split + 1;
			line_y += 14.0f * scale;
			continue;
		}
		const std::size_t retained =
			static_cast<std::size_t>(fit - cursor - 1);
		draw_line(cursor, retained, true);
		cursor = fit - 1;
		line_y += 14.0f * scale;
	}
}

float power_ray_distance(
	float cursor_x,
	float cursor_y,
	float direction_x,
	float direction_y)
{
	const float dot =
		cursor_x * direction_x + cursor_y * direction_y;
	const float discriminant =
		dot * dot
		- (cursor_x * cursor_x + cursor_y * cursor_y - 4096.0f);
	return discriminant < 0.0f
		? 0.0f
		: std::max(0.0f, dot + std::sqrt(discriminant));
}

void draw_power_panel(
	const game::WorldObject& player,
	const LanguageTable& language,
	render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	float x,
	float y,
	float scale)
{
	render::frontend_gameplay_powerball_update(
		frontend,
		player.power_cursor_x,
		player.power_cursor_y);
	render::frontend_rgba_quad(
		commands,
		frontend.shell.gameplay_powerball,
		x + 37.0f * scale,
		y - 32.0f * scale,
		62.0f * scale,
		62.0f * scale);

	draw_text(
		commands,
		frontend,
		language_text(language, 167),
		x + 2.0f * scale,
		y - 77.0f * scale,
		scale);

	constexpr glm::vec2 directions[3] = {
		{0.0f, 1.0f},
		{0.8660254f, -0.5f},
		{-0.8660254f, -0.5f},
	};
	float weights[3];
	float total = 0.0f;
	for (std::uint32_t index = 0; index < 3; ++index)
	{
		weights[index] = power_ray_distance(
			player.power_cursor_x,
			player.power_cursor_y,
			directions[index].x,
			directions[index].y);
		total += weights[index];
	}
	const float normalization = total == 0.0f ? 0.0f : 1.0f / total;
	std::int32_t percentages[3];
	for (std::uint32_t index = 0; index < 3; ++index)
	{
		weights[index] *= normalization;
		percentages[index] = static_cast<std::int32_t>(
			std::nearbyint(weights[index] * 100.0f));
	}
	if (percentages[0] + percentages[1] + percentages[2] == 101)
	{
		for (std::int32_t& percentage : percentages)
		{
			if (percentage == 34)
			{
				percentage = 33;
				break;
			}
		}
	}
	const Point value_positions[3] = {
		{x + 50.0f * scale, y - 43.0f * scale},
		{x - 18.0f * scale, y + 30.0f * scale},
		{x + 50.0f * scale, y + 30.0f * scale},
	};
	for (std::uint32_t index = 0; index < 3; ++index)
	{
		char value[16];
		std::snprintf(
			value,
			sizeof(value),
			"%d%%",
			percentages[index]);
		draw_text(
			commands,
			frontend,
			value,
			value_positions[index].x,
			value_positions[index].y,
			scale);
	}

	draw_shape(
		commands, frontend, 132,
		x + 33.0f * scale, y - 13.0f * scale, scale);
	draw_shape(
		commands, frontend, 133,
		x + 69.0f * scale, y - 13.0f * scale, scale);
	draw_shape(
		commands, frontend, 131,
		x + 40.0f * scale, y - 32.0f * scale, scale);

	const auto clipped_vertical =
		[&](std::uint16_t shape,
			float destination_x,
			float destination_y,
			float fraction,
			bool from_top)
		{
			const render::FrontendTexture& texture =
				frontend.shell.gameplay_hud_shapes[shape];
			const std::uint16_t rows = static_cast<std::uint16_t>(
				std::clamp(
					static_cast<std::int32_t>(
						std::nearbyint(fraction * 48.0f)),
					0,
					static_cast<std::int32_t>(texture.height)));
			if (rows == 0)
			{
				return;
			}
			const std::uint16_t source_y = from_top
				? 0
				: static_cast<std::uint16_t>(texture.height - rows);
			render::frontend_indexed_scaled_region(
				commands,
				texture,
				frontend.shell.gameplay_hud_palette,
				destination_x + texture.offset_x * scale,
				destination_y
					+ (texture.offset_y + source_y) * scale,
				texture.width * scale,
				rows * scale,
				0,
				source_y,
				texture.width,
				rows);
		};
	clipped_vertical(
		129,
		x + 33.0f * scale,
		y - 13.0f * scale,
		weights[0],
		false);
	clipped_vertical(
		130,
		x + 69.0f * scale,
		y - 13.0f * scale,
		weights[1],
		true);

	const render::FrontendTexture& horizontal =
		frontend.shell.gameplay_hud_shapes[128];
	const std::uint16_t columns = static_cast<std::uint16_t>(
		std::clamp(
			static_cast<std::int32_t>(
				std::nearbyint(weights[2] * 54.0f)),
			0,
			static_cast<std::int32_t>(horizontal.width)));
	if (columns != 0)
	{
		const std::uint16_t source_x = static_cast<std::uint16_t>(
			horizontal.width - columns);
		render::frontend_indexed_scaled_region(
			commands,
			horizontal,
			frontend.shell.gameplay_hud_palette,
			x + (39.0f + source_x) * scale,
			y - 33.0f * scale,
			columns * scale,
			horizontal.height * scale,
			source_x,
			0,
			columns,
			horizontal.height);
	}
	draw_shape(
		commands, frontend, 190,
		x + 52.0f * scale, y - 67.0f * scale, scale);
	draw_shape(
		commands, frontend, 189,
		x + 102.0f * scale, y + 1.0f * scale, scale);
	draw_shape(
		commands, frontend, 193,
		x + 2.0f * scale, y + 4.0f * scale, scale);
}

void draw_panel(
	std::uint8_t panel,
	Runtime& runtime,
	const game::WorldObject* player,
	const game::WorldObject* target,
	const game::World& world,
	const mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	const LanguageTable& language,
	const render::MissionRenderer& mission_renderer,
	render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	float x,
	float y,
	float scale,
	DistortionDrawState& distortion)
{
	// HUD_panel_render_contents, LANCER.EXE 0x00486830, suppresses all
	// panel decorations outside cockpit camera zero. Every panel body has
	// the same camera gate except case fourteen's generic command list.
	if (world.camera_mode == 0)
	{
		draw_panel_decorations(
			panel, x, y, scale, frontend, commands);
	}
	else if (panel != 14)
	{
		return;
	}
	switch (panel)
	{
	case 0:
		if (runtime.panels[0].animation
			== PanelAnimation::closing)
		{
			// LANCER.EXE 0x00488035 selects the retained contact-family
			// static as soon as panel zero enters closing state, even while
			// the movie object remains ready.
			const std::uint16_t idle_shape =
				runtime.comms_contact_class != 0
					? 0x15e
					: 0x147;
			draw_shape_distorted(
				commands,
				frontend,
				idle_shape,
				x + 13.0f * scale,
				y + 19.0f * scale,
				scale,
				distortion);
		}
		else if (runtime.movie.active
			&& runtime.comms_static_active)
		{
			// hudmovie_play_resource starts a strict 92-tick lead-in.
			// The retail frame advances only every four ticks and selects
			// one of two authored static families from the speaker class.
			const std::uint16_t static_shape =
				static_cast<std::uint16_t>(
					(runtime.comms_contact_class != 0
						? 0x148
						: 0x131)
					+ (runtime.comms_static_time >> 2));
			draw_shape_distorted(
				commands,
				frontend,
				static_shape,
				x + 13.0f * scale,
				y + 19.0f * scale,
				scale,
				distortion);
		}
		else if (runtime.movie.active && runtime.movie.ready)
		{
			draw_text(
				commands,
				frontend,
				language_text(
					language,
					mission.presentation.voice),
				x + 1.0f * scale,
				y + 2.0f * scale,
				scale);
			draw_movie_distorted(
				commands,
				frontend.shell.gameplay_hud_movie,
				x + 13.0f * scale,
				y + 19.0f * scale,
				runtime.movie.width,
				runtime.movie.height,
				scale,
				distortion);
		}
		break;
	case 1:
		if (player != nullptr
			&& player->gun_frame_shape != UINT16_MAX)
		{
			draw_shape_distorted(
				commands,
				frontend,
				player->gun_frame_shape,
				x + 11.0f * scale,
				y - 134.0f * scale,
				scale,
				distortion);
			if (player->gun_synchronized)
			{
				draw_text(
					commands,
					frontend,
					language_text(language, 663),
					x + 1.0f * scale,
					y - 157.0f * scale,
					scale);
			}
			else if (player->selected_gun_group
				< player->gun_pair_count)
			{
				const game::GunPair& pair =
					player->gun_pairs[player->selected_gun_group];
				if (pair.first >= 0
					&& static_cast<std::uint8_t>(pair.first)
						< player->gun_mount_count)
				{
					const std::int32_t bullet_index =
						std::max<std::int32_t>(
							static_cast<std::int32_t>(
								player->gun_mounts[
									static_cast<std::uint8_t>(
										pair.first)]
									.bullet_type)
								- 1,
							0);
					draw_text(
						commands,
						frontend,
						language_text(
							language,
							static_cast<std::uint16_t>(
								0x3a9 + bullet_index)),
						x + 1.0f * scale,
						y - 157.0f * scale,
						scale);
				}
			}
			if (player->gun_group_count > 1)
			{
				if (player->gun_synchronized)
				{
					for (std::uint8_t group = 0;
						group < player->gun_group_count;
						++group)
					{
						const game::GunPair* pair =
							group < player->gun_pair_count
								? &player->gun_pairs[group]
								: nullptr;
						const game::GunMount* first = nullptr;
						if (pair != nullptr
							&& pair->first >= 0
							&& static_cast<std::uint8_t>(pair->first)
								< player->gun_mount_count)
						{
							first = &player->gun_mounts[
								static_cast<std::uint8_t>(pair->first)];
						}
						// The synchronized/all-guns branch at
						// 0x004872ca..0x00487360 indexes the authored
						// six-byte gun-pair table directly. An absent
						// first mount still draws its overlay; only the
						// dedicated Nova (one-based type 11) omits it.
						if (first != nullptr && first->bullet_type == 11)
						{
							continue;
						}
						draw_shape_distorted(
							commands,
							frontend,
							static_cast<std::uint16_t>(
								player->gun_frame_shape + group + 1),
							x + 11.0f * scale,
							y - 134.0f * scale,
							scale,
							distortion);
					}
				}
				else
				{
					std::uint8_t selected_bullet_type = 0;
					const game::GunPair* selected_pair =
						player->selected_gun_group
								< player->gun_pair_count
							? &player->gun_pairs[
								player->selected_gun_group]
							: nullptr;
					if (selected_pair != nullptr
						&& selected_pair->first >= 0
						&& static_cast<std::uint8_t>(
							selected_pair->first)
							< player->gun_mount_count)
					{
						selected_bullet_type =
							player->gun_mounts[
								static_cast<std::uint8_t>(
									selected_pair->first)]
								.bullet_type;
					}
					draw_shape_distorted(
						commands,
						frontend,
						static_cast<std::uint16_t>(
							player->gun_frame_shape
								+ player->selected_gun_group + 1),
						x + 11.0f * scale,
						y - 134.0f * scale,
						scale,
						distortion);
					// Retail draws the 0xf1-frame sync animation only
					// when the selected group has its second mount and
					// the first mount is not the Nova type.
					if (selected_pair != nullptr
						&& selected_pair->second >= 0
						&& selected_bullet_type != 11)
					{
						draw_shape_distorted(
							commands,
							frontend,
							static_cast<std::uint16_t>(
								0xf1 - player->gun_sync_frame),
							x + 1.0f * scale,
							y - 139.0f * scale,
							scale,
							distortion);
					}
				}
			}
			// Panel case one at 0x0048732a..0x00487448 appends the
			// dedicated ammunition frame and live signed counter for
			// object types 2, 8, and 9.
			if (player->type == 2
				|| player->type == 8
				|| player->type == 9)
			{
				draw_shape_distorted(
					commands,
					frontend,
					238,
					x + 4.0f * scale,
					y - 17.0f * scale,
					scale,
					distortion);
				char ammunition[24];
				std::snprintf(
					ammunition,
					sizeof(ammunition),
					"%d",
					player->ammunition);
				draw_text(
					commands,
					frontend,
					ammunition,
					x + 21.0f * scale,
					y - 19.0f * scale,
					scale);
			}
		}
		break;
	case 2:
		for (const OrdnanceEntry& entry : runtime.ordnance)
		{
			if (entry.count < 0)
			{
				continue;
			}
			if (entry.ring_position == 0)
			{
				char count[16];
				std::snprintf(count, sizeof(count), "%d", entry.count);
				draw_text(
					commands,
					frontend,
					count,
					x - 1.0f * scale
						- text_width(frontend, count, scale) * 0.5f,
					y + 67.0f * scale,
					scale);
				const char* label = language_text(
					language,
					static_cast<std::uint16_t>(entry.language_id));
				draw_text(
					commands,
					frontend,
					label,
					x - text_width(frontend, label, scale) * 0.5f,
					y + 1.0f * scale,
					scale);
			}
			draw_shape_distorted(
				commands,
				frontend,
				static_cast<std::uint16_t>(
					entry.shape + entry.ring_position),
				x,
				y + 71.0f * scale,
				scale,
				distortion);
		}
		break;
	case 3:
		if (player != nullptr && target != nullptr)
		{
			draw_target_panel(
				false, world.selected_target.index,
				world.target_component,
				runtime.target_schematic_hits,
				*player, *target, mission, stats, language,
				mission_renderer, frontend, commands,
				x, y, scale, distortion);
		}
		break;
	case 4:
		if (player != nullptr)
		{
			const char* title = language_text(language, 653);
			draw_text(
				commands, frontend, title,
				x - 2.0f * scale
					- text_width(frontend, title, scale),
				y + 2.0f * scale,
				scale);
			const char* labels[3] = {
				language_text(language, 645),
				language_text(language, 646),
				language_text(language, 647),
			};
			const float values[3] = {
				player->weapons_health,
				player->engines_health,
				player->shields_health,
			};
			const std::uint16_t icons[3] = {193, 189, 190};
			const float icon_x[3] = {-136.0f, -134.0f, -135.0f};
			const float icon_y[3] = {23.0f, 59.0f, 97.0f};
			const float label_y[3] = {24.0f, 62.0f, 101.0f};
			const float frame_y[3] = {42.0f, 80.0f, 119.0f};
			const float meter_y[3] = {45.0f, 83.0f, 122.0f};
			for (std::uint32_t row = 0; row < 3; ++row)
			{
				draw_shape_distorted(
					commands,
					frontend,
					icons[row],
					x + icon_x[row] * scale,
					y + icon_y[row] * scale,
					scale,
					distortion);
				draw_text(
					commands, frontend, labels[row],
					x - 102.0f * scale,
					y + label_y[row] * scale,
					scale);
				draw_shape(
					commands,
					frontend,
					352,
					x - 132.0f * scale,
					y + frame_y[row] * scale,
					scale);
				draw_meter(
					frontend, commands,
					x - 98.0f * scale,
					y + meter_y[row] * scale,
					values[row],
					scale);
			}
		}
		break;
	case 7:
		if (player != nullptr)
		{
			draw_power_panel(
				*player,
				language,
				frontend,
				commands,
				x,
				y,
				scale);
		}
		break;
	case 8:
		if (player != nullptr && target != nullptr)
		{
			draw_target_panel(
				true, world.selected_target.index,
				world.target_component,
				runtime.target_schematic_hits,
				*player, *target, mission, stats, language,
				mission_renderer, frontend, commands,
				x, y, scale, distortion);
		}
		break;
	case 10:
		draw_objective_panel(
			mission, language, frontend, commands,
			x, y, scale);
		break;
	case 11:
		draw_generic_list(
			mission.player_comms,
			language,
			frontend,
			commands,
			x + 2.0f * scale,
			y + 2.0f * scale,
			scale);
		break;
	case 13:
	{
		const char* title = language_text(language, 166);
		draw_text(
			commands,
			frontend,
			title,
			x - 3.0f * scale
				- text_width(frontend, title, scale),
			y - 78.0f * scale,
			scale);

		// MissionRuntime_build_group_class_object_lists
		// (LANCER.EXE 0x0045ac60) rebuilds a class table for every authored
		// group. A later group of the same class replaces the prior table,
		// so panel thirteen consumes the final class-zero group's members.
		constexpr std::int16_t x_offsets[6] = {
			-111, -64, -17, -111, -64, -17,
		};
		constexpr std::int16_t y_offsets[6] = {
			17, 17, 17, 63, 63, 63,
		};
		const mission::GroupRecord* wing_group = nullptr;
		for (std::uint16_t group_index = 0;
			group_index < mission.group_count;
			++group_index)
		{
			const mission::GroupRecord& group =
				mission.groups[group_index];
			if (group.object_class == 0)
			{
				wing_group = &group;
			}
		}
		if (wing_group != nullptr)
		{
			for (std::uint8_t slot = 0;
				slot < wing_group->member_count && slot < 6;
				++slot)
			{
				const std::uint16_t object_index =
					mission.group_members[
						wing_group->first_member + slot];
				const game::WorldObject* wing =
					mission::runtime_resolve_object(
						mission, object_index, world);
				if (wing == nullptr || !wing->active
					|| wing->allegiance_class != 0
					|| (wing->runtime_flags & 0x00000800u) != 0
					|| wing->type >= assets::kShipStatsCount)
				{
					continue;
				}
				const float cell_x =
					x + (x_offsets[slot] - 24.0f) * scale;
				const float cell_y =
					y + (y_offsets[slot] - 71.0f) * scale;
				const assets::ObjectTypeStats& type =
					stats.records[wing->type].object;
				float health = 0.0f;
				if (type.structural_bank_max > 0)
				{
					health = *std::min_element(
							std::begin(wing->secondary_shields),
							std::end(wing->secondary_shields))
						/ static_cast<float>(
							type.structural_bank_max * 6);
				}
				draw_vertical_health_bar(
					frontend,
					commands,
					cell_x,
					cell_y,
					health,
					scale);
				if (wing->hud_icon != 0)
				{
					draw_shape_distorted(
						commands,
						frontend,
						wing->hud_icon,
						cell_x + 6.0f * scale,
						cell_y + 1.0f * scale,
						scale,
						distortion);
				}
				char ordinal[4];
				std::snprintf(
					ordinal,
					sizeof(ordinal),
					"%u",
					slot + 1);
				draw_text(
					commands,
					frontend,
					ordinal,
					cell_x + 6.0f * scale,
					cell_y - 2.0f * scale,
					scale);
			}
		}
		break;
	}
	case 14:
		draw_generic_list(
			mission.player_comms,
			language,
			frontend,
			commands,
			x + 15.0f * scale,
			y + 21.0f * scale,
			scale);
		break;
	default:
		break;
	}
}

void retain_panel_snapshot(
	PanelRenderSnapshot& snapshot,
	const render::FrontendCommands& commands,
	std::uint32_t first,
	float draw_x,
	float draw_y,
	float draw_scale,
	float pane_x,
	float pane_y)
{
	snapshot.commands.assign(
		commands.items + first,
		commands.items + commands.count);
	// Stable panels are emitted directly at the scaled screen anchor. Convert
	// that command image back into the retail private-pane coordinate system
	// so the close path can apply the ordinary pane transform unchanged.
	for (render::FrontendCommand& item : snapshot.commands)
	{
		item.x = pane_x + (item.x - draw_x) / draw_scale;
		item.y = pane_y + (item.y - draw_y) / draw_scale;
		item.width /= draw_scale;
		item.height /= draw_scale;
	}
	snapshot.valid = true;
}

void draw_panels(
	Runtime& runtime,
	const game::World& world,
	const mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	const LanguageTable& language,
	const render::MissionRenderer& mission_renderer,
	render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout,
	DistortionDrawState& distortion)
{
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	const game::WorldObject* target = target_object(world, mission);
	for (std::uint8_t panel = 0; panel < kPanelCount; ++panel)
	{
		const PanelState& state = runtime.panels[panel];
		PanelRenderSnapshot* close_snapshot = nullptr;
		if (panel == 3)
		{
			close_snapshot = &runtime.target_panel_snapshots[0];
		}
		else if (panel == 8)
		{
			close_snapshot = &runtime.target_panel_snapshots[1];
		}
		if (state.animation == PanelAnimation::closed)
		{
			if (close_snapshot != nullptr)
			{
				close_snapshot->commands.clear();
				close_snapshot->valid = false;
			}
			continue;
		}
		const PanelDescriptor& descriptor = kPanels[panel];
		const float source_x = std::nearbyint(
			223.0f * descriptor.pane_x_fraction + 1.0f);
		const float source_y = std::nearbyint(
			168.0f * descriptor.pane_y_fraction + 1.0f);
		const Point stable = panel == 3
			? target_panel_anchor(layout)
			: scalable_anchor(
				layout,
				0.0f,
				0.0f,
				descriptor.x_fraction,
				descriptor.y_fraction);
		if (state.animation == PanelAnimation::open)
		{
			const std::uint32_t first = commands.count;
			draw_panel(
				panel, runtime, player, target, world, mission, stats, language,
				mission_renderer, frontend, commands,
				stable.x, stable.y, layout.element_scale, distortion);
			if (close_snapshot != nullptr)
			{
				// The original keeps a dedicated 225-by-170 pane for this
				// purpose. Retaining the latest emitted pane is the
				// command-stream equivalent and preserves the old subject
				// even after its world handle becomes invalid.
				retain_panel_snapshot(
					*close_snapshot,
					commands,
					first,
					stable.x,
					stable.y,
					layout.element_scale,
					source_x,
					source_y);
			}
			continue;
		}

		// LANCER.EXE 0x004864a4..0x004866e2 renders transitional
		// panels into a private 225-by-170 pane, then calls
		// VFX_buffer_transform with uniform 16.16 scale 2-progress.
		// The pane anchor table at 0x00501f88 is separate from the
		// stable panel anchor; panel three deliberately uses 0.2 here.
		const float progress =
			std::clamp(state.animation_time / 60.0f, 0.0f, 1.0f);
		const float factor = 2.0f - progress;
		const float transition_x =
			(descriptor.x_fraction - 0.5f) * factor + 0.5f;
		const float transition_y =
			(descriptor.y_fraction - 0.5f) * factor + 0.5f;
		Point transition;
		if (panel == 3)
		{
			const Point center = scalable_anchor(
				layout, 0.0f, 0.0f, 0.5f, 0.5f);
			transition = {
				center.x + (stable.x - center.x) * factor,
				center.y + (stable.y - center.y) * factor,
			};
		}
		else
		{
			transition = scalable_anchor(
				layout, 0.0f, 0.0f, transition_x, transition_y);
		}
		const float center_x = transition.x + std::nearbyint(
			112.0f * factor * layout.element_scale);
		const float center_y = transition.y + std::nearbyint(
			84.0f * factor * layout.element_scale);
		const float transform_scale = factor * layout.element_scale;

		const std::uint32_t scissor = commands.count;
		render::frontend_scissor(commands, 0, 0, 1, 1);
		const std::uint32_t first = commands.count;
		if (close_snapshot != nullptr
			&& state.animation == PanelAnimation::closing)
		{
			// HUD_panel_close, LANCER.EXE 0x0048b590, renders panels three
			// and eight into their private pane exactly once. The regular
			// transition owner then transforms that frozen pane for every
			// remaining close frame, so target changes cannot alter it.
			if (!close_snapshot->valid)
			{
				auto snapshot_commands =
					std::make_unique<render::FrontendCommands>();
				draw_panel(
					panel,
					runtime,
					player,
					target,
					world,
					mission,
					stats,
					language,
					mission_renderer,
					frontend,
					*snapshot_commands,
					source_x,
					source_y,
					1.0f,
					distortion);
				close_snapshot->commands.assign(
					snapshot_commands->items,
					snapshot_commands->items
						+ snapshot_commands->count);
				close_snapshot->valid = true;
			}
			if (close_snapshot->commands.size()
				> render::kMaxFrontendCommands - commands.count)
			{
				std::fputs(
					"sl_open: frontend render command limit "
					"(4096) exceeded\n",
					stderr);
				std::abort();
			}
			std::copy(
				close_snapshot->commands.begin(),
				close_snapshot->commands.end(),
				commands.items + commands.count);
			commands.count += static_cast<std::uint32_t>(
				close_snapshot->commands.size());
		}
		else
		{
			draw_panel(
				panel,
				runtime,
				player,
				target,
				world,
				mission,
				stats,
				language,
				mission_renderer,
				frontend,
				commands,
				source_x,
				source_y,
				1.0f,
				distortion);
			if (close_snapshot != nullptr)
			{
				retain_panel_snapshot(
					*close_snapshot,
					commands,
					first,
					source_x,
					source_y,
					1.0f,
					source_x,
					source_y);
			}
		}
		for (std::uint32_t command = first;
			command < commands.count;
			++command)
		{
			render::FrontendCommand& item = commands.items[command];
			item.x = center_x
				+ (item.x - 112.0f) * transform_scale;
			item.y = center_y
				+ (item.y - 84.0f) * transform_scale;
			item.width *= transform_scale;
			item.height *= transform_scale;
		}
		render::FrontendCommand& clip = commands.items[scissor];
		const float pane_left =
			center_x - 112.0f * transform_scale;
		const float pane_top =
			center_y - 84.0f * transform_scale;
		const float clip_left = std::clamp(
			pane_left, 0.0f, layout.width);
		const float clip_top = std::clamp(
			pane_top, 0.0f, layout.height);
		const float clip_right = std::clamp(
			pane_left + 225.0f * transform_scale,
			0.0f,
			layout.width);
		const float clip_bottom = std::clamp(
			pane_top + 170.0f * transform_scale,
			0.0f,
			layout.height);
		clip.x = clip_left;
		clip.y = clip_top;
		clip.width = std::max(0.0f, clip_right - clip_left);
		clip.height = std::max(0.0f, clip_bottom - clip_top);
		render::frontend_scissor(
			commands,
			0,
			0,
			static_cast<std::uint16_t>(layout.width),
			static_cast<std::uint16_t>(layout.height));
	}
}

void draw_timer(
	const mission::Runtime& mission,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout,
	std::uint32_t simulation_tick,
	std::uint16_t mission_number)
{
	char timer[16];
	// HUD_render_frame_callback 0x004861fd..0x0048624c selects the signed
	// session countdown at +0x84 for mission 29 (and multiplayer mode two),
	// clamps a negative value to zero, and otherwise uses the audio clock's
	// already-separated minute/second words.
	const std::uint32_t seconds = mission_number == 29
		? static_cast<std::uint32_t>(std::max<std::int32_t>(
			static_cast<std::int32_t>(mission.session_state[33]),
			0))
		: simulation_tick / 100;
	std::snprintf(
		timer, sizeof(timer), "%02u:%02u",
		seconds / 60, seconds % 60);
	const Point timer_anchor =
		scalable_anchor(layout, 0.0f, -130.0f, 0.5f, 1.0f);
	draw_text(
		commands, frontend, timer,
		timer_anchor.x
			- text_width(frontend, timer, layout.element_scale) * 0.5f,
		timer_anchor.y,
		layout.element_scale);
}

void draw_messages(
	const Runtime& runtime,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	// HUD_render_frame_callback calls only the four-entry timed-message
	// owner at 0x00485423..0x0048544e. The PrintDebugMessage storage at
	// 0x00536d58/0x0052abd0/0x005373dc has no read xrefs in the retail
	// executable and is emitted solely through its debug-output hook.
	const Point messages =
		scalable_anchor(layout, -110.0f, -140.0f, 0.5f, 0.5f);
	for (std::uint8_t index = 0;
		index < runtime.message_count;
		++index)
	{
		char line[112];
		std::snprintf(
			line, sizeof(line), "- %s", runtime.messages[index].text);
		render::frontend_gameplay_message_text(
			commands,
			frontend,
			line,
			messages.x,
			messages.y + index * 11.0f * layout.element_scale,
			0xffffffff,
			layout.element_scale);
	}
}

void draw_chat_input(
	const Runtime& runtime,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	if (!runtime.chat_active)
	{
		return;
	}
	// HUD_render_frame_callback, LANCER.EXE 0x00486789..0x00486820,
	// draws typed multiplayer chat after every ordinary HUD panel. The
	// buffer is centered on scalable anchor (0, -106, .5, 1), while the
	// underscore begins at the centered string's right edge and never
	// blinks.
	const Point anchor =
		scalable_anchor(layout, 0.0f, -106.0f, 0.5f, 1.0f);
	const float width = text_width(
		frontend,
		runtime.chat_message,
		layout.element_scale);
	render::frontend_gameplay_hud_text(
		commands,
		frontend,
		runtime.chat_message,
		anchor.x - width * 0.5f,
		anchor.y,
		0xffffffffu,
		layout.element_scale);
	render::frontend_gameplay_hud_text(
		commands,
		frontend,
		"_",
		anchor.x + std::trunc(width * 0.5f),
		anchor.y,
		0xffffffffu,
		layout.element_scale);
}

void draw_director_subtitle(
	std::uint8_t camera_mode,
	const mission::Runtime& mission,
	const LanguageTable& language,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	// HUD_render_frame_callback (LANCER.EXE 0x004853d7..0x00485423)
	// renders the selected subtitle only while Director camera mode 13 is
	// active. Resource 0x90 is the initialization sentinel.
	if (camera_mode != 13
		|| mission.presentation.subtitle_id == 0x90)
	{
		return;
	}
	const char* text =
		language_text(language, mission.presentation.subtitle_id);
	if (text == nullptr || text[0] == '\0')
	{
		return;
	}
	const float scale = layout.element_scale;
	// The subtitle draw at 0x004853f9 loads font 0x00595490, the same
	// ordinary HUD glyph set used by camera labels.
	draw_text(
		commands,
		frontend,
		text,
		layout.width * 0.5f
			- text_width(frontend, text, scale) * 0.5f,
		layout.height - 60.0f * scale,
		scale);
}

void draw_camera_label(
	std::uint8_t camera_mode,
	const LanguageTable& language,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	// CameraDefinition records at LANCER.EXE 0x004f72a8 have a four-byte
	// stride; their first signed word is the language ID rendered by
	// HUD_render_frame_callback 0x00485396. Modes 36..38 are the three
	// retail camera states which deliberately suppress the label.
	constexpr std::uint16_t language_ids[] = {
		170, 171, 172, 173, 181, 175, 176, 177, 178, 179,
		179, 179, 180, 181, 181, 181, 181, 181, 182, 181,
		181, 181, 181, 181, 181, 181, 181, 181, 181, 181,
		181, 181, 181, 181, 181, 181, 181, 181, 181,
	};
	if (camera_mode == 0
		|| camera_mode >= std::size(language_ids)
		|| (camera_mode >= 36 && camera_mode <= 38))
	{
		return;
	}
	const char* text =
		language_text(language, language_ids[camera_mode]);
	if (text == nullptr || text[0] == '\0')
	{
		return;
	}
	const float scale = layout.element_scale;
	draw_text(
		commands,
		frontend,
		text,
		layout.width * 0.5f
			- text_width(frontend, text, scale) * 0.5f,
		10.0f * scale,
		scale);
}

void draw_launch_mission_title(
	const mission::Runtime& mission,
	const LanguageTable& language,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	if (!mission.launch_title_active
		|| mission.mission_number == 0
		|| mission.mission_number >= 29)
	{
		return;
	}
	// The compiled table at 0x005023d6 contains zero for slot zero and
	// consecutive language IDs 0x3d2..0x3ed for missions 1..28.
	const char* source = language_text(
		language,
		static_cast<std::uint16_t>(
			0x3d1u + mission.mission_number));
	if (source == nullptr)
	{
		return;
	}
	char visible[52]{};
	const std::size_t source_length = std::strlen(source);
	const std::size_t reveal = std::min<std::size_t>(
		mission.launch_title_reveal,
		std::min<std::size_t>(source_length, sizeof(visible) - 1));
	std::memcpy(visible, source, reveal);
	const float scale = layout.element_scale;
	const float x = 50.0f * scale;
	const float y = layout.height - 30.0f * scale;
	// Both title calls at 0x0048469b and 0x00484704 pass the ordinary
	// gameplay HUD font (0x00595490), not the timed-message font.
	draw_text(
		commands,
		frontend,
		visible,
		x,
		y,
		scale);
	if (reveal < source_length)
	{
		draw_text(
			commands,
			frontend,
			"_",
			x + text_width(frontend, visible, scale),
			y,
			scale);
	}
}

void draw_target_lock_animation(
	const Runtime& runtime,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	if (runtime.tracked_contact == UINT16_MAX)
	{
		return;
	}
	const Point anchor =
		scalable_anchor(layout, -16.0f, -100.0f, 0.5f, 0.5f);
	draw_shape(
		commands,
		frontend,
		static_cast<std::uint16_t>(
			209 + runtime.lock_animation_frame),
		anchor.x,
		anchor.y,
		layout.element_scale);
}

void draw_missile_lock_ring(
	const Runtime& runtime,
	const render::MissionRenderer& mission_renderer,
	const render::MissionRenderFrame& camera,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	const MissileLockState& lock = runtime.missile_lock;
	if (lock.phase == MissileLockPhase::idle
		|| !bgfx::isValid(mission_renderer.missile_lock_ring.handle))
	{
		return;
	}
	Point center;
	float depth;
	if (!project(
			camera,
			static_cast<std::uint32_t>(layout.width),
			static_cast<std::uint32_t>(layout.height),
			lock.retained_point,
			center,
			depth))
	{
		return;
	}

	// HUD_update_missile_lock_ring 0x00491ae4..0x00491b43 normalizes the
	// target displacement before scaling it by projection_scale * 2560 /
	// framebuffer_width * opening_progress. Projecting the 256-unit mesh
	// therefore leaves both the framebuffer-width term and the reciprocal of
	// the normalized forward component in its final screen extent.
	const float opening_progress = std::clamp(
		(100.0f - static_cast<float>(lock.opening_percent)) * 0.01f,
		0.0f,
		1.0f);
	if (opening_progress <= 0.0f)
	{
		return;
	}
	const glm::vec3 camera_space =
		glm::transpose(camera.camera_orientation)
		* (lock.retained_point - camera.camera_position);
	const float direction_length = glm::length(camera_space);
	if (direction_length <= 0.0f || camera_space.z <= 0.0f)
	{
		return;
	}
	const float normalized_forward = camera_space.z / direction_length;
	const float extent = layout.width * 0.1f
		/ (opening_progress * normalized_forward);
	const float half = extent * 0.5f;
	const float phase_radians =
		glm::radians(static_cast<float>(lock.acquisition_phase));
	float offsets[3];
	if (lock.phase == MissileLockPhase::locked)
	{
		offsets[0] = phase_radians;
		offsets[1] = phase_radians;
		offsets[2] = phase_radians;
	}
	else
	{
		offsets[0] = std::sin(phase_radians * 1.5f);
		offsets[1] = std::sin(phase_radians * 2.5f) * 0.6f;
		offsets[2] = phase_radians;
	}
	const float carried =
		glm::radians(static_cast<float>(
			lock.carried_rotation_degrees));
	// HUD_lock_ring_mesh_data_init/HUD_lock_ring_init
	// (0x00491080/0x004911d0) give all three mesh objects the same complete
	// -128..128 square. Their UVs select three separate 128-by-128 cells in
	// TARRING.TGA; the cells are overlapping animation layers, not spatial
	// quadrants of one 256-by-256 image.
	const glm::vec2 origin_from_center{-half, -half};
	const std::uint16_t source_x[3] = {0, 128, 0};
	const std::uint16_t source_y[3] = {0, 0, 128};
	const float bases[3] = {0.328125f, 0.109375f, 0.0f};
	const float color_mix = std::clamp(
		(static_cast<float>(lock.acquisition_phase) + 50.0f)
			* 0.02f,
		0.0f,
		1.0f);
	const float red = 0.5f + color_mix * 0.5f;
	const float green_blue = color_mix;
	const auto channel = [](float value)
	{
		return static_cast<std::uint32_t>(
			std::clamp(
				static_cast<int>(std::nearbyint(value * 255.0f)),
				0,
				255));
	};
	for (std::uint32_t layer = 0; layer < 3; ++layer)
	{
		const float angle = carried + offsets[layer];
		const float cosine = std::cos(angle);
		const float sine = std::sin(angle);
		const glm::vec2 origin{
			origin_from_center.x * cosine
				- origin_from_center.y * sine,
			origin_from_center.x * sine
				+ origin_from_center.y * cosine,
		};
		float alpha_base = bases[layer];
		if (lock.acquisition_phase > -50)
		{
			alpha_base = lock.phase == MissileLockPhase::locked
				? 0.0f
				: static_cast<float>(lock.acquisition_phase)
					* alpha_base * -0.02f;
		}
		const float alpha =
			std::clamp((alpha_base + 1.0f) * 0.7f, 0.0f, 1.0f);
		const std::uint32_t rgba =
			(channel(red) << 24)
			| (channel(green_blue) << 16)
			| (channel(green_blue) << 8)
			| channel(alpha);
		render::frontend_rgba_rotated_region(
			commands,
			mission_renderer.missile_lock_ring,
			center.x + origin.x,
			center.y + origin.y,
			extent,
			extent,
			source_x[layer],
			source_y[layer],
			128,
			128,
			// frontend transforms use bx::mtxSRT's negated Euler angles.
			// The layer origin above is rotated by +angle, so negate the
			// submitted value to keep its texture basis on that same orbit.
			-angle,
			rgba);
	}
}

void draw_warning_icons(
	const Runtime& runtime,
	const game::WorldObject& player,
	const render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	const Layout& layout)
{
	const float scale = layout.element_scale;
	if (runtime.request_blink_shape != UINT16_MAX)
	{
		// HUD_draw_jump_warp_request_blink 0x00482fa0 uses this common
		// center anchor for warp shape 201 and jump shape 206.
		const Point request =
			scalable_anchor(layout, -16.0f, -90.0f, 0.5f, 0.5f);
		draw_shape(
			commands,
			frontend,
			runtime.request_blink_shape,
			request.x,
			request.y,
			scale);
	}
	constexpr std::uint16_t shapes[9] = {
		204, 203, 197, 195, 196, 198, 199, 202, 200,
	};
	// HUD_icon_grid_position lays the retail status-shape sequence into
	// two 48-by-38 columns from this exact scalable anchor.
	const Point anchor =
		scalable_anchor(layout, -156.0f, 0.0f, 0.5f, 0.0f);
	for (std::uint8_t icon = 0; icon < std::size(shapes); ++icon)
	{
		const std::uint8_t slot = runtime.hud_icon_grid_slot[icon];
		if (slot == UINT8_MAX || !runtime.status_icon_draw[icon])
		{
			continue;
		}
		draw_shape(
			commands,
			frontend,
			shapes[icon],
			anchor.x + static_cast<float>(slot & 1u) * 48.0f * scale,
			anchor.y + static_cast<float>(slot / 2u) * 38.0f * scale,
			scale);
	}
	const auto charge_bar =
		[&](std::uint8_t icon,
			bool active,
			std::int32_t charge,
			float multiplier,
			float y_offset)
		{
			const std::uint8_t slot = runtime.hud_icon_grid_slot[icon];
			if (!active || slot == UINT8_MAX)
			{
				return;
			}
			const float endpoint = static_cast<float>(std::max(
				0,
				static_cast<int>(
					std::nearbyint(
						static_cast<float>(charge)
							* multiplier))));
			render::frontend_rgba_quad(
				commands,
				frontend.white,
				anchor.x
					+ (static_cast<float>(slot & 1u) * 48.0f + 1.0f)
						* scale,
				anchor.y
					+ (static_cast<float>(slot / 2u) * 38.0f + y_offset)
						* scale,
				(endpoint + 1.0f) * scale,
				std::max(1.0f, scale),
				0xe76800ff);
		};
	// Exact executable constants at 0x004dc904, 0x004dc900, and
	// 0x004dc8fc.  The ECM branch at 0x0048500b..0x004850e4 draws its
	// charge line after either the native or scripted gate admits the icon.
	charge_bar(5, true,
		runtime.ecm_charge,
		0.016129031777381897f, 35.0f);
	charge_bar(6, player.cloak_supported, runtime.cloak_charge,
		0.0032051282469183207f, 32.0f);
	charge_bar(7, (player.runtime_flags & 0x08000000u) != 0,
		runtime.spectral_charge,
		0.005347593687474728f, 32.0f);
	if (runtime.hud_icon_draw[5])
	{
		const Point anchor =
			scalable_anchor(layout, -16.0f, -100.0f, 0.5f, 0.5f);
		draw_shape(
			commands,
			frontend,
			194,
			anchor.x,
			anchor.y + 38.0f * scale,
			scale);
	}
}

}

void render(
	Runtime& runtime,
	game::World& world,
	const mission::Runtime& mission,
	const assets::ShipStatsTable& ship_stats,
	const assets::GunStatsTable& gun_stats,
	const Config& config,
	const LanguageTable& language,
	const render::MissionRenderer& mission_renderer,
	const render::MissionRenderFrame& mission_frame,
	render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint32_t simulation_tick,
	std::uint16_t mission_number)
{
	game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (player == nullptr || drawable_width == 0 || drawable_height == 0)
	{
		return;
	}
	const Layout layout =
		make_layout(drawable_width, drawable_height);
	const bool networked =
		mission.network.role != mission::NetworkRole::offline;
	const bool forced_scoreboard =
		mission.network.deathmatch_mode
		&& (world.camera_mode == 8
			|| world.camera_mode == 26
			|| world.camera_mode == 27);
	// Deathmatch's scenario callback is dispatched at
	// LANCER.EXE 0x00484571..0x0048457f, before the scoreboard replacement
	// gate. Its scenario icon therefore remains in the command stream even
	// when the scoreboard replaces the ordinary HUD.
	draw_deathmatch_scenario_status(
		mission, world, language, frontend, commands, layout);
	// HUD_render_frame_callback, LANCER.EXE 0x00484584..0x004845dc,
	// replaces the complete ordinary HUD with Deathmatch_draw_scoreboard
	// while F10 is held. Deathmatch death cameras 8, 26, and 27 force the
	// same replacement independently of F10.
	if (networked
		&& (forced_scoreboard || runtime.scoreboard_held))
	{
		draw_scoreboard(
			mission, world, language, frontend, commands, layout);
		return;
	}
	// HUD_render_frame_callback, LANCER.EXE 0x00485f67..0x004860cf.
	// Capture consumes the previous targeting overlay's projected marker;
	// the overlay below publishes the solution for the following frame.
	player->blindfire_active = false;
	const float center_x = layout.width * 0.5f;
	const float center_y = layout.height * 0.5f;
	const bool lead_in_capture_box = world.camera_mode == 0
		&& runtime.weapon_lead_valid
		&& std::abs(runtime.weapon_lead_x - center_x)
			< 70.0f * layout.element_scale
		&& std::abs(runtime.weapon_lead_y - center_y)
			< 50.0f * layout.element_scale;
	const bool blindfire_available = player->blindfire_supported
		&& player->blindfire_enabled;
	const std::int16_t selected_bullet =
		blindfire_selected_bullet_type(*player);
	const bool capture = lead_in_capture_box
		&& blindfire_available
		&& selected_bullet >= 0
		&& selected_bullet != 11;
	if (capture)
	{
		player->blindfire_active = true;
		player->blindfire_aim_point = runtime.weapon_lead_world;
		runtime.reticle_x = runtime.weapon_lead_x;
		runtime.reticle_y = runtime.weapon_lead_y;
		runtime.reticle_captured = true;
	}
	else
	{
		runtime.reticle_captured = false;
		// Nova's one-based projectile type 11 jumps directly past the
		// return-to-centre owner at 0x004860b3. Non-cockpit cameras skip the
		// complete reticle path after clearing +0x674.
		if (world.camera_mode == 0
			&& !(lead_in_capture_box
				&& blindfire_available
				&& selected_bullet == 11))
		{
			const float amount = runtime.reticle_return_amount;
			const auto approach = [amount](float value, float target)
			{
				return value < target
					? std::min(value + amount, target)
					: std::max(value - amount, target);
			};
			runtime.reticle_x = approach(runtime.reticle_x, center_x);
			runtime.reticle_y = approach(runtime.reticle_y, center_y);
		}
	}
	runtime.weapon_lead_valid = false;
	runtime.weapon_lead_x = -1.0f;
	runtime.weapon_lead_y = -1.0f;
	DistortionDrawState distortion{
		runtime.camera_disturbance,
		runtime.distortion_random_seed,
		runtime.hit_distortion > 0.0f,
	};
	draw_launch_mission_title(
		mission, language, frontend, commands, layout);
	draw_control_prompt(
		mission, config, language, frontend, commands, layout);
	if (world.camera_mode == 0)
	{
		draw_missile_lock_ring(
			runtime,
			mission_renderer,
			mission_frame,
			commands,
			layout);
		draw_target_lock_animation(
			runtime, frontend, commands, layout);
		draw_targeting(
			runtime,
			world,
			mission,
			ship_stats,
			gun_stats,
			mission_frame,
			frontend,
			commands,
			layout);
		draw_warning_icons(
			runtime, *player, frontend, commands, layout);
	}
	else
	{
		draw_camera_label(
			world.camera_mode,
			language,
			frontend,
			commands,
			layout);
		draw_director_subtitle(
			world.camera_mode,
			mission,
			language,
			frontend,
			commands,
			layout);
	}
	// The retained gameplay-message queue is rendered before the
	// camera-zero instruments and remains visible in every camera mode.
	draw_messages(
		runtime,
		frontend,
		commands,
		layout);
	if (world.camera_mode == 0)
	{
		draw_main_instruments(
			runtime,
			*player,
			ship_stats,
			world.camera_mode,
			mission_renderer,
			frontend,
			commands,
			layout,
			distortion);
		draw_projected_flyback_marker(
			world,
			mission_frame,
			frontend,
			commands,
			layout);
		draw_sensor(
			runtime,
			world,
			*player,
			mission,
			mission_frame,
			frontend,
			commands,
			layout,
			distortion);
		draw_timer(
			mission,
			frontend,
			commands,
			layout,
			simulation_tick,
			mission_number);
		// The inline DMPowerup renderer at
		// LANCER.EXE 0x00486286..0x004863a6 runs after the mission timer
		// and only inside the camera-zero instrument branch.
		draw_deathmatch_powerup_status(
			mission,
			world,
			frontend,
			commands,
			layout,
			simulation_tick);
	}
	draw_panels(
		runtime,
		world,
		mission,
		ship_stats,
		language,
		mission_renderer,
		frontend,
		commands,
		layout,
		distortion);
	draw_chat_input(
		runtime,
		frontend,
		commands,
		layout);
	runtime.distortion_random_seed = distortion.random_seed;
}
}
