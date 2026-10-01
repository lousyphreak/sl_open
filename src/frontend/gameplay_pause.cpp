#include "frontend/gameplay_pause.hpp"

#include "frontend/gui_render.hpp"
#include "hud/layout.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace sl_open::frontend
{
namespace
{
struct Item
{
	float x_fraction;
	float y_fraction;
	float x_offset;
	float y_offset;
	std::uint16_t string_id;
	std::uint8_t normal_shape;
	std::uint8_t hover_shape;
	bool small_font;
};

// Frontend_screen_seven_choice (0x0048e8d0) constructs these six visible
// retail records. The apparent seventh switch result is the developer dump
// route and has no ordinary visible record.
constexpr Item kItems[] = {
	{0.5f, 1.0f, 16.0f, -32.0f, 0x32b, 0, 1, false},
	{0.5f, 1.0f, -16.0f, -32.0f, 0x181, 0, 1, false},
	{0.5f, 1.0f, -16.0f, -52.0f, 0x180, 0, 1, false},
	{0.25f, 0.5f, 0.0f, 0.0f, 0x109, 3, 6, true},
	{0.5f, 0.5f, 0.0f, 0.0f, 0x10a, 2, 5, true},
	{0.75f, 0.5f, 0.0f, 0.0f, 0x10b, 4, 7, true},
};

// Frontend_screen_online_termination (FUN_0048e370) constructs exactly the
// DAT_00502788 record: centered, forty pixels below center, with the ordinary
// pause button pair and language string 0x316 ("OK").
constexpr Item kNetworkTerminationItem{
	0.5f, 0.5f, 0.0f, 40.0f, 0x316, 0, 1, false};

bool item_visible(const GameplayPause& pause, std::size_t index)
{
	// Frontend_screen_online_pause (FUN_0048e510) creates exactly the
	// ordinary Leave Mission and Continue records. Its apparent third
	// return value is the hidden developer status dump, not a visible menu
	// item.
	return pause.mode == GameplayPauseMode::offline
		|| (pause.mode == GameplayPauseMode::online
			&& (index == 0 || index == 2));
}

struct PlacedItem
{
	float center_x;
	float center_y;
	float left;
	float top;
	float width;
	float height;
};

PlacedItem place_item(
	const Item& item,
	const render::FrontendTexture& texture,
	float scale,
	float width,
	float height)
{
	const float item_width = texture.width * scale;
	const float item_height = texture.height * scale;
	const float center_x =
		width * item.x_fraction + item.x_offset * scale;
	const float center_y =
		height * item.y_fraction + item.y_offset * scale;
	return {
		center_x,
		center_y,
		center_x - item_width * 0.5f,
		center_y - item_height * 0.5f,
		item_width,
		item_height,
	};
}

const render::FrontendTexture& item_texture(
	const render::FrontendRenderer& renderer,
	const Item& item,
	bool hovered)
{
	const std::uint8_t shape = hovered
		? item.hover_shape
		: item.normal_shape;
	if (shape == 0) return renderer.shell.pause_button;
	if (shape == 1) return renderer.shell.pause_button_hover;
	return renderer.shell.pause_panels[shape - 2];
}

bool hit(const PlacedItem& item, float x, float y)
{
	return x > item.left && x < item.left + item.width
		&& y > item.top && y < item.top + item.height;
}

enum class ScoreboardTextAlignment : std::uint8_t
{
	left,
	center,
	right,
};

struct OnlineScoreboardLayout
{
	std::uint8_t players[kGameplayPausePlayerCapacity]{};
	std::uint8_t teams[kGameplayPauseTeamCapacity]{};
	std::uint8_t player_count{};
	std::uint8_t team_count{};
	float x{};
	float team_top_y{};
	float player_top_y{};
	float scale{1.0f};
};

struct OnlineTextAnchor
{
	float x{};
	float y{};
};

OnlineTextAnchor online_text_anchor(
	const hud::Layout& layout,
	float base_x,
	float base_y,
	float x_fraction,
	float y_fraction)
{
	// Scalable_Anchor (LANCER.EXE 0x00482e90) retains a sixteen-pixel
	// border inside the authored 640x480 HUD. Its FUN_004c3330 conversion
	// uses x87 FISTP's default nearest-even rounding before applying the
	// authored offset.
	const float border = 16.0f * layout.element_scale;
	return {
		border + base_x * layout.element_scale
			+ std::nearbyint(
				(layout.width - 33.0f * layout.element_scale)
					* x_fraction),
		border + base_y * layout.element_scale
			+ std::nearbyint(
				(layout.height - 33.0f * layout.element_scale)
					* y_fraction),
	};
}

const char* safe_text(const char* text)
{
	return text == nullptr ? "" : text;
}

float gameplay_text_width(
	const render::FrontendRenderer& renderer,
	const char* text,
	float scale)
{
	return gui::text_width(
		renderer.shell.gameplay_hud_glyphs,
		renderer.shell.gameplay_hud_glyph_count,
		safe_text(text),
		scale);
}

float scoreboard_text_width(
	const render::FrontendRenderer& renderer,
	const char* text,
	float scale)
{
	return gui::text_width(
		renderer.shell.gameplay_scoreboard_glyphs,
		renderer.shell.gameplay_scoreboard_glyph_count,
		safe_text(text),
		scale);
}

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

void draw_scoreboard_header_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	float scale,
	ScoreboardTextAlignment alignment)
{
	text = safe_text(text);
	render::frontend_gameplay_scoreboard_text(
		commands,
		renderer,
		text,
		aligned_text_x(
			x,
			scoreboard_text_width(renderer, text, scale),
			alignment),
		y,
		0xffffffffu,
		scale);
}

void draw_scoreboard_value_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	float scale,
	ScoreboardTextAlignment alignment)
{
	text = safe_text(text);
	render::frontend_gameplay_hud_text(
		commands,
		renderer,
		text,
		aligned_text_x(
			x,
			gameplay_text_width(renderer, text, scale),
			alignment),
		y,
		0xffffffffu,
		scale);
}

void draw_scoreboard_shape(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	std::uint16_t shape,
	float x,
	float y,
	float scale)
{
	if (shape >= render::kGameplayScoreboardShapeCount)
	{
		return;
	}
	const render::FrontendTexture& texture =
		renderer.shell.gameplay_scoreboard_shapes[shape];
	if (!bgfx::isValid(texture.handle))
	{
		return;
	}
	render::frontend_indexed_scaled_quad(
		commands,
		texture,
		renderer.shell.gameplay_scoreboard_palette,
		x + texture.offset_x * scale,
		y + texture.offset_y * scale,
		texture.width * scale,
		texture.height * scale,
		0xffffffffu);
}

OnlineScoreboardLayout online_scoreboard_layout(
	const GameplayPauseView& view,
	const hud::Layout& layout)
{
	OnlineScoreboardLayout result;
	result.scale = layout.element_scale;
	const float center_x = std::floor(layout.width * 0.5f);
	const float center_y = std::floor(layout.height * 0.5f);
	result.x = center_x - 233.0f * result.scale;

	for (std::uint8_t player = 0;
		player < kGameplayPausePlayerCapacity;
		++player)
	{
		if (view.players[player].connected)
		{
			result.players[result.player_count++] = player;
		}
	}
	const auto player_precedes =
		[&](std::uint8_t lhs, std::uint8_t rhs)
		{
			if (view.players[lhs].kills != view.players[rhs].kills)
			{
				return view.players[lhs].kills
					> view.players[rhs].kills;
			}
			return view.players[lhs].deaths
				< view.players[rhs].deaths;
		};
	for (std::uint8_t sorted = 1;
		sorted < result.player_count;
		++sorted)
	{
		const std::uint8_t player = result.players[sorted];
		std::uint8_t insertion = sorted;
		while (insertion != 0
			&& player_precedes(
				player, result.players[insertion - 1]))
		{
			result.players[insertion] =
				result.players[insertion - 1];
			--insertion;
		}
		result.players[insertion] = player;
	}

	if (view.team_mode)
	{
		bool represented[kGameplayPauseTeamCapacity]{};
		for (std::uint8_t player = 0;
			player < kGameplayPausePlayerCapacity;
			++player)
		{
			const std::int32_t team = view.players[player].team;
			if (view.players[player].connected
				&& team >= 0
				&& team < static_cast<std::int32_t>(
					kGameplayPauseTeamCapacity))
			{
				represented[team] = true;
			}
		}
		for (std::uint8_t team = 0;
			team < kGameplayPauseTeamCapacity;
			++team)
		{
			if (represented[team])
			{
				result.teams[result.team_count++] = team;
			}
		}
		const auto team_precedes =
			[&](std::uint8_t lhs, std::uint8_t rhs)
			{
				if (view.team_score[lhs] != view.team_score[rhs])
				{
					return view.team_score[lhs]
						> view.team_score[rhs];
				}
				return view.team_deaths[lhs]
					< view.team_deaths[rhs];
			};
		for (std::uint8_t sorted = 1;
			sorted < result.team_count;
			++sorted)
		{
			const std::uint8_t team = result.teams[sorted];
			std::uint8_t insertion = sorted;
			while (insertion != 0
				&& team_precedes(
					team, result.teams[insertion - 1]))
			{
				result.teams[insertion] =
					result.teams[insertion - 1];
				--insertion;
			}
			result.teams[insertion] = team;
		}
		if (view.configured_team_count > result.team_count)
		{
			result.team_count = static_cast<std::uint8_t>(
				std::min<std::int8_t>(
					view.configured_team_count,
					static_cast<std::int8_t>(
						kGameplayPauseTeamCapacity)));
			for (std::uint8_t team = 0;
				team < result.team_count;
				++team)
			{
				result.teams[team] = team;
			}
		}
	}

	// Deathmatch_draw_scoreboard observes DAT_0057e04c while the online
	// pause owner is active. It lowers the team table by 50 retail pixels.
	// In team modes it then leaves a second 50-pixel pause gap before the
	// player table; without teams the player table is lowered once.
	result.team_top_y =
		center_y - 155.0f * result.scale;
	result.player_top_y = view.team_mode
		? result.team_top_y
			+ (80.0f + 17.0f * result.team_count) * result.scale
		: center_y - 60.0f * result.scale;
	return result;
}

PlacedItem reject_item(
	const OnlineScoreboardLayout& layout,
	const render::FrontendRenderer& renderer,
	std::uint8_t row,
	bool hovered)
{
	const render::FrontendTexture& texture = hovered
		? renderer.shell.pause_button_hover
		: renderer.shell.pause_button;
	const float authored_x = layout.x + 515.0f * layout.scale;
	const float authored_y = layout.player_top_y
		+ (27.0f + 17.0f * row) * layout.scale;
	const float width = texture.width * layout.scale;
	const float height = texture.height * layout.scale;
	const float left = authored_x + texture.offset_x * layout.scale
		- std::floor(texture.width * 0.5f) * layout.scale;
	const float top = authored_y + texture.offset_y * layout.scale
		- std::floor(texture.height * 0.5f) * layout.scale;
	return {
		left + width * 0.5f,
		top + height * 0.5f,
		left,
		top,
		width,
		height,
	};
}

std::uint8_t reject_player_at(
	const GameplayPauseView& view,
	const render::FrontendRenderer& renderer,
	const hud::Layout& screen,
	float x,
	float y)
{
	const OnlineScoreboardLayout layout =
		online_scoreboard_layout(view, screen);
	for (std::uint8_t row = 0; row < layout.player_count; ++row)
	{
		const std::uint8_t player = layout.players[row];
		if (player == view.local_player
			|| !view.players[player].rejectable)
		{
			continue;
		}
		if (hit(reject_item(layout, renderer, row, false), x, y))
		{
			return player;
		}
	}
	return UINT8_MAX;
}

void draw_online_scoreboard(
	const GameplayPause& pause,
	const GameplayPauseView& view,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	const hud::Layout& screen)
{
	const OnlineScoreboardLayout layout =
		online_scoreboard_layout(view, screen);
	const float scale = layout.scale;

	if (view.team_mode)
	{
		draw_scoreboard_shape(
			commands, renderer, 20, layout.x, layout.team_top_y, scale);
		draw_scoreboard_header_text(
			commands,
			renderer,
			language_text(language, 0x2c2),
			layout.x + 20.0f * scale,
			layout.team_top_y,
			scale,
			ScoreboardTextAlignment::left);
		draw_scoreboard_header_text(
			commands,
			renderer,
			language_text(language, 0x308),
			layout.x + 223.0f * scale,
			layout.team_top_y,
			scale,
			ScoreboardTextAlignment::center);
		if (view.deathmatch_mode)
		{
			draw_scoreboard_header_text(
				commands,
				renderer,
				language_text(language, 0x307),
				layout.x + 321.0f * scale,
				layout.team_top_y,
				scale,
				ScoreboardTextAlignment::center);
		}

		constexpr std::uint8_t team_icon[] = {23, 25, 24, 26};
		for (std::uint8_t row = 0; row < layout.team_count; ++row)
		{
			const std::uint8_t team = layout.teams[row];
			const float row_y = layout.team_top_y
				+ (22.0f + 17.0f * row) * scale;
			const float text_y = layout.team_top_y
				+ (21.0f + 17.0f * row) * scale;
			draw_scoreboard_shape(
				commands, renderer, 22, layout.x, row_y, scale);
			draw_scoreboard_shape(
				commands,
				renderer,
				team_icon[team],
				layout.x + 450.0f * scale,
				layout.team_top_y
					+ (24.0f + 17.0f * row) * scale,
				scale);

			char rank[12];
			char name[48];
			char kills[24];
			char deaths[24];
			std::snprintf(
				rank, sizeof(rank), "%u",
				static_cast<unsigned>(row + 1));
			std::snprintf(
				name,
				sizeof(name),
				"%s %u",
				language_text(language, 0x309),
				static_cast<unsigned>(team + 1));
			std::snprintf(
				kills, sizeof(kills), "%d", view.team_score[team]);
			std::snprintf(
				deaths,
				sizeof(deaths),
				"%d",
				view.team_deaths[team]);
			draw_scoreboard_header_text(
				commands,
				renderer,
				rank,
				layout.x + 6.0f * scale,
				text_y,
				scale,
				ScoreboardTextAlignment::center);
			draw_scoreboard_value_text(
				commands,
				renderer,
				name,
				layout.x + 20.0f * scale,
				text_y,
				scale,
				ScoreboardTextAlignment::left);
			draw_scoreboard_value_text(
				commands,
				renderer,
				kills,
				layout.x + 223.0f * scale,
				text_y,
				scale,
				ScoreboardTextAlignment::center);
			if (view.deathmatch_mode)
			{
				draw_scoreboard_value_text(
					commands,
					renderer,
					deaths,
					layout.x + 321.0f * scale,
					text_y,
					scale,
					ScoreboardTextAlignment::center);
			}
		}
		draw_scoreboard_shape(
			commands,
			renderer,
			21,
			layout.x,
			layout.team_top_y
				+ (22.0f + 17.0f * layout.team_count) * scale,
			scale);
	}

	draw_scoreboard_shape(
		commands, renderer, 20, layout.x, layout.player_top_y, scale);
	draw_scoreboard_header_text(
		commands,
		renderer,
		language_text(language, 0xbf),
		layout.x + 20.0f * scale,
		layout.player_top_y,
		scale,
		ScoreboardTextAlignment::left);
	draw_scoreboard_header_text(
		commands,
		renderer,
		language_text(language, 0x308),
		layout.x + 223.0f * scale,
		layout.player_top_y,
		scale,
		ScoreboardTextAlignment::center);
	if (view.deathmatch_mode)
	{
		draw_scoreboard_header_text(
			commands,
			renderer,
			language_text(language, 0x307),
			layout.x + 321.0f * scale,
			layout.player_top_y,
			scale,
			ScoreboardTextAlignment::center);
	}
	draw_scoreboard_header_text(
		commands,
		renderer,
		language_text(language, 0x559),
		layout.x + 418.0f * scale,
		layout.player_top_y,
		scale,
		ScoreboardTextAlignment::center);
	draw_scoreboard_value_text(
		commands,
		renderer,
		language_text(language, 0x5f2),
		layout.x + 515.0f * scale,
		layout.player_top_y,
		scale,
		ScoreboardTextAlignment::center);

	constexpr std::uint8_t team_icon[] = {23, 25, 24, 26};
	for (std::uint8_t row = 0; row < layout.player_count; ++row)
	{
		const std::uint8_t player = layout.players[row];
		const GameplayPausePlayer& value = view.players[player];
		const float row_y = layout.player_top_y
			+ (21.0f + 17.0f * row) * scale;
		const float text_y = layout.player_top_y
			+ (20.0f + 17.0f * row) * scale;
		draw_scoreboard_shape(
			commands, renderer, 22, layout.x, row_y, scale);
		if (view.team_mode && value.team >= 0
			&& value.team < static_cast<std::int32_t>(
				kGameplayPauseTeamCapacity))
		{
			draw_scoreboard_shape(
				commands,
				renderer,
				team_icon[value.team],
				layout.x + 450.0f * scale,
				layout.player_top_y
					+ (23.0f + 17.0f * row) * scale,
				scale);
		}
		if (value.scenario_shape
			< render::kGameplayScoreboardShapeCount)
		{
			draw_scoreboard_shape(
				commands,
				renderer,
				value.scenario_shape,
				layout.x + 352.0f * scale,
				row_y + 2.0f * scale,
				scale);
			if (value.scenario_value >= 0)
			{
				char scenario[24];
				std::snprintf(
					scenario,
					sizeof(scenario),
					"%d",
					value.scenario_value);
				draw_scoreboard_value_text(
					commands,
					renderer,
					scenario,
					layout.x + 370.0f * scale,
					text_y,
					scale,
					ScoreboardTextAlignment::left);
			}
		}

		char rank[12];
		char fallback[48];
		char kills[24];
		char deaths[24];
		char latency[24];
		std::snprintf(
			rank, sizeof(rank), "%u",
			static_cast<unsigned>(row + 1));
		std::snprintf(kills, sizeof(kills), "%d", value.kills);
		std::snprintf(deaths, sizeof(deaths), "%d", value.deaths);
		std::snprintf(
			latency,
			sizeof(latency),
			"%u",
			static_cast<unsigned>(value.latency));
		const char* name = safe_text(value.name);
		if (name[0] == '\0')
		{
			std::snprintf(
				fallback,
				sizeof(fallback),
				"%s %u",
				language_text(language, 0xbf),
				static_cast<unsigned>(player + 1));
			name = fallback;
		}
		draw_scoreboard_header_text(
			commands,
			renderer,
			rank,
			layout.x + 6.0f * scale,
			text_y,
			scale,
			ScoreboardTextAlignment::center);
		draw_scoreboard_value_text(
			commands,
			renderer,
			name,
			layout.x + 20.0f * scale,
			text_y,
			scale,
			ScoreboardTextAlignment::left);
		draw_scoreboard_value_text(
			commands,
			renderer,
			kills,
			layout.x + 223.0f * scale,
			text_y,
			scale,
			ScoreboardTextAlignment::center);
		if (view.deathmatch_mode)
		{
			draw_scoreboard_value_text(
				commands,
				renderer,
				deaths,
				layout.x + 321.0f * scale,
				text_y,
				scale,
				ScoreboardTextAlignment::center);
		}
		draw_scoreboard_value_text(
			commands,
			renderer,
			latency,
			layout.x + 418.0f * scale,
			text_y,
			scale,
			ScoreboardTextAlignment::center);

		if (player != view.local_player && value.rejectable)
		{
			const bool hovered =
				pause.hovered_reject_player == player;
			const render::FrontendTexture& texture = hovered
				? renderer.shell.pause_button_hover
				: renderer.shell.pause_button;
			const PlacedItem item =
				reject_item(layout, renderer, row, hovered);
			render::frontend_indexed_scaled_quad(
				commands,
				texture,
				renderer.shell.pause_palette,
				item.left,
				item.top,
				item.width,
				item.height);
		}
	}
	draw_scoreboard_shape(
		commands,
		renderer,
		21,
		layout.x,
		layout.player_top_y
			+ (21.0f + 17.0f * layout.player_count) * scale,
		scale);
}

void draw_online_text(
	const GameplayPauseView& view,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	const hud::Layout& layout)
{
	char reason[160]{};
	if (view.pause_reason == 0)
	{
		const char* owner = "";
		if (view.pause_owner < kGameplayPausePlayerCapacity)
		{
			owner = safe_text(view.players[view.pause_owner].name);
		}
		std::snprintf(
			reason,
			sizeof(reason),
			"%s - %s",
			owner,
			language_text(language, 0x53e));
	}
	else if (view.pause_reason == 1)
	{
		std::snprintf(
			reason,
			sizeof(reason),
			"%s",
			language_text(language, 0x55a));
	}
	if (reason[0] != '\0')
	{
		const float width = gui::text_width(
			renderer.shell.glyphs,
			renderer.shell.glyph_count,
			reason,
			layout.element_scale);
		render::frontend_text(
			commands,
			renderer,
			reason,
			std::nearbyint(layout.width * 0.5f)
				- std::floor(width * 0.5f),
			layout.height * 0.2f
				+ 32.0f * layout.element_scale,
			renderer.shell.font_gold_palette,
			0xffffffffu,
			layout.element_scale);
	}

	const OnlineTextAnchor messages = online_text_anchor(
		layout, -110.0f, -84.0f, 0.5f, 1.0f);
	const std::uint8_t message_count =
		std::min<std::uint8_t>(
			view.recent_message_count,
			static_cast<std::uint8_t>(
				kGameplayPauseMessageCapacity));
	for (std::uint8_t index = 0; index < message_count; ++index)
	{
		char line[112];
		std::snprintf(
			line,
			sizeof(line),
			"- %s",
			safe_text(view.recent_messages[index]));
		render::frontend_gameplay_message_text(
			commands,
			renderer,
			line,
			messages.x,
			messages.y + index * 11.0f * layout.element_scale,
			0xffffffffu,
			layout.element_scale);
	}

	if (view.chat_active)
	{
		const char* chat = safe_text(view.chat_text);
		const OnlineTextAnchor editor = online_text_anchor(
			layout, 0.0f, -84.0f, 0.5f, 1.0f);
		const float width = gameplay_text_width(
			renderer, chat, layout.element_scale);
		const float half_width = std::trunc(width * 0.5f);
		const float y = editor.y + 44.0f * layout.element_scale;
		render::frontend_gameplay_hud_text(
			commands,
			renderer,
			chat,
			editor.x - half_width,
			y,
			0xffffffffu,
			layout.element_scale);
		render::frontend_gameplay_hud_text(
			commands,
			renderer,
			"_",
			editor.x + half_width,
			y,
			0xffffffffu,
			layout.element_scale);
	}
}
}

void gameplay_pause_reset(
	GameplayPause& pause,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now,
	bool online)
{
	pause = {};
	pause.pointer_x = drawable_width * 0.5f;
	pause.pointer_y = drawable_height * 0.5f;
	pause.hovered = -1;
	pause.entered_at = now;
	pause.mode = online
		? GameplayPauseMode::online
		: GameplayPauseMode::offline;
}

void gameplay_pause_reset_network_termination(
	GameplayPause& pause,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now)
{
	gameplay_pause_reset(
		pause, drawable_width, drawable_height, now, true);
	pause.mode = GameplayPauseMode::network_termination;
}

void gameplay_pause_set_pointer(
	GameplayPause& pause,
	const render::FrontendRenderer& renderer,
	float x,
	float y,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	bool inside)
{
	gameplay_pause_set_pointer(
		pause,
		GameplayPauseView{},
		renderer,
		x,
		y,
		drawable_width,
		drawable_height,
		inside);
}

void gameplay_pause_set_pointer(
	GameplayPause& pause,
	const GameplayPauseView& view,
	const render::FrontendRenderer& renderer,
	float x,
	float y,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	bool inside)
{
	pause.pointer_x = x;
	pause.pointer_y = y;
	pause.hovered = -1;
	pause.hovered_reject_player = UINT8_MAX;
	if (!inside)
	{
		return;
	}
	const hud::Layout layout =
		hud::make_layout(drawable_width, drawable_height);
	if (pause.mode == GameplayPauseMode::network_termination)
	{
		const PlacedItem item = place_item(
			kNetworkTerminationItem,
			item_texture(
				renderer, kNetworkTerminationItem, false),
			layout.element_scale,
			layout.width,
			layout.height);
		if (hit(item, x, y))
		{
			pause.hovered = 0;
		}
		return;
	}
	if (pause.mode == GameplayPauseMode::online
		&& view.online_overlay)
	{
		pause.hovered_reject_player = reject_player_at(
			view, renderer, layout, x, y);
		if (pause.hovered_reject_player != UINT8_MAX)
		{
			return;
		}
	}
	for (std::size_t index = 0; index < std::size(kItems); ++index)
	{
		if (!item_visible(pause, index))
		{
			continue;
		}
		const PlacedItem item = place_item(
			kItems[index],
			item_texture(renderer, kItems[index], false),
			layout.element_scale,
			layout.width,
			layout.height);
		if (hit(item, x, y))
		{
			pause.hovered = static_cast<std::int8_t>(index);
			return;
		}
	}
}

GameplayPauseAction gameplay_pause_select(const GameplayPause& pause)
{
	if (pause.mode == GameplayPauseMode::network_termination)
	{
		return pause.hovered == 0
			? GameplayPauseAction::acknowledge_termination
			: GameplayPauseAction::none;
	}
	switch (pause.hovered)
	{
	case 0: return GameplayPauseAction::leave_mission;
	case 1: return GameplayPauseAction::restart;
	case 2: return GameplayPauseAction::continue_mission;
	case 3: return GameplayPauseAction::audio;
	case 4: return GameplayPauseAction::controls;
	case 5: return GameplayPauseAction::video;
	default: return GameplayPauseAction::none;
	}
}

GameplayPauseSelection gameplay_pause_select(
	const GameplayPause& pause,
	const GameplayPauseView& view)
{
	if (pause.mode == GameplayPauseMode::online
		&& view.online_overlay
		&& pause.hovered_reject_player
			< kGameplayPausePlayerCapacity
		&& pause.hovered_reject_player != view.local_player
		&& view.players[
			pause.hovered_reject_player].connected
		&& view.players[
			pause.hovered_reject_player].rejectable)
	{
		return {
			GameplayPauseAction::reject_player,
			pause.hovered_reject_player,
		};
	}
	return {gameplay_pause_select(pause), UINT8_MAX};
}

void gameplay_pause_build(
	const GameplayPause& pause,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now)
{
	gameplay_pause_build(
		pause,
		GameplayPauseView{},
		language,
		renderer,
		commands,
		drawable_width,
		drawable_height,
		now);
}

void gameplay_pause_build(
	const GameplayPause& pause,
	const GameplayPauseView& view,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint64_t now)
{
	render::frontend_commands_begin(commands);
	const hud::Layout layout =
		hud::make_layout(drawable_width, drawable_height);
	const float scale = layout.element_scale;

	render::frontend_rgba_quad(
		commands,
		renderer.white,
		drawable_width * 0.1f,
		drawable_height * 0.2f,
		drawable_width * 0.8f,
		drawable_height * 0.6f,
		0x020609c0);

	const char* heading = language_text(language, 0x108);
	const float heading_width = gui::text_width(
		renderer.shell.glyphs,
		renderer.shell.glyph_count,
		heading,
		scale);
	render::frontend_text(
		commands,
		renderer,
		heading,
		(drawable_width - heading_width) * 0.5f,
		drawable_height * 0.2f - 32.0f * scale,
		renderer.shell.font_gold_palette,
		0xffffffff,
		scale);

	for (std::size_t index = 0; index < std::size(kItems); ++index)
	{
		if (!item_visible(pause, index))
		{
			continue;
		}
		const bool selected =
			pause.hovered == static_cast<std::int8_t>(index);
		const render::FrontendTexture& texture =
			item_texture(renderer, kItems[index], selected);
		const PlacedItem item = place_item(
			kItems[index],
			texture,
			scale,
			layout.width,
			layout.height);
		render::frontend_indexed_scaled_quad(
			commands,
			texture,
			renderer.shell.pause_palette,
			item.left,
			item.top,
			item.width,
			item.height);

		const char* label =
			language_text(language, kItems[index].string_id);
		const bgfx::TextureHandle palette = selected
			? renderer.shell.font_white_palette
			: renderer.shell.font_gold_palette;
		if (index < 3)
		{
			float x = item.center_x + 25.0f * scale;
			if (index != 0)
			{
				x = item.center_x - 25.0f * scale
					- gui::text_width(
						renderer.shell.glyphs,
						renderer.shell.glyph_count,
						label,
						scale);
			}
			render::frontend_text(
				commands,
				renderer,
				label,
				x,
				item.center_y - 5.0f * scale,
				palette,
				0xffffffff,
				scale);
		}
		else
		{
			const float label_width =
				render::frontend_pause_small_text_width(
					renderer, label, scale);
			render::frontend_pause_small_text(
				commands,
				renderer,
				label,
				item.center_x - label_width * 0.5f,
				item.center_y + 65.0f * scale,
				palette,
				0xffffffff,
				scale);
		}
	}

	if (pause.mode == GameplayPauseMode::online
		&& view.online_overlay)
	{
		draw_online_scoreboard(
			pause, view, language, renderer, commands, layout);
		draw_online_text(
			view, language, renderer, commands, layout);
	}

	if (pause.mode == GameplayPauseMode::network_termination)
	{
		const char* message = language_text(language, 0x5f1);
		gui::TextWrapCursor cursor{message};
		char line[256];
		float message_y =
			layout.height * 0.5f - 42.0f * scale;
		while (gui::next_wrapped_line(
			cursor,
			renderer.shell.glyphs,
			renderer.shell.glyph_count,
			scale,
			400.0f * scale,
			line))
		{
			const float line_width = gui::text_width(
				renderer.shell.glyphs,
				renderer.shell.glyph_count,
				line,
				scale);
			render::frontend_text(
				commands,
				renderer,
				line,
				(layout.width - line_width) * 0.5f,
				message_y,
				renderer.shell.font_gold_palette,
				0xffffffff,
				scale);
			message_y += 14.0f * scale;
		}

		const bool selected = pause.hovered == 0;
		const render::FrontendTexture& texture = item_texture(
			renderer, kNetworkTerminationItem, selected);
		const PlacedItem item = place_item(
			kNetworkTerminationItem,
			texture,
			scale,
			layout.width,
			layout.height);
		render::frontend_indexed_scaled_quad(
			commands,
			texture,
			renderer.shell.pause_palette,
			item.left,
			item.top,
			item.width,
			item.height);
		const char* label = language_text(
			language, kNetworkTerminationItem.string_id);
		render::frontend_text(
			commands,
			renderer,
			label,
			item.center_x + 25.0f * scale,
			item.center_y - 5.0f * scale,
			selected
				? renderer.shell.font_white_palette
				: renderer.shell.font_gold_palette,
			0xffffffff,
			scale);
	}

	const std::uint64_t elapsed =
		now > pause.entered_at ? now - pause.entered_at : 0;
	const std::size_t cursor_frame = static_cast<std::size_t>(
		(elapsed / 40) % std::size(renderer.shell.cursor));
	const render::FrontendTexture& cursor =
		renderer.shell.cursor[cursor_frame];
	render::frontend_indexed_scaled_quad(
		commands,
		cursor,
		renderer.shell.cursor_palette,
		pause.pointer_x,
		pause.pointer_y,
		cursor.width * scale,
		cursor.height * scale);
}
}
