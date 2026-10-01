#include "frontend/multiplayer_lobby.hpp"

#include "frontend/gui.hpp"
#include "frontend/gui_render.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace sl_open::frontend
{
namespace
{
enum Hover : std::int8_t
{
	kHoverNone = -1,
	kHoverCallsign,
	kHoverSession,
	kHoverStart,
	kHoverNew,
	kHoverLoad,
	kHoverRemove,
	kHoverBack,
	kHoverMain,
	kHoverQuit,
	kHoverDescriptionUp,
	kHoverDescriptionDown,
	kHoverTeamPicker,
	kHoverShipPicker,
	kHoverAiTurrets,
	kHoverTargetPlayers,
	kHoverTeamplay,
	kHoverPlayerFirst = 24,
	kHoverScenarioFirst = 40,
	kHoverTeamFirst = 48,
	kHoverShipFirst = 56,
};

constexpr gui::Rect kBackRegion{323.0f, 420.0f, 28.0f, 20.0f};
constexpr gui::Rect kMainRegion{291.0f, 441.0f, 28.0f, 18.0f};
constexpr gui::Rect kQuitRegion{323.0f, 441.0f, 28.0f, 18.0f};

constexpr gui::Rect kCoopClientCallsign{
	45.0f, 140.0f, 149.0f, 21.0f};
constexpr gui::Rect kCoopHostCallsign{
	45.0f, 179.0f, 149.0f, 21.0f};
constexpr gui::Rect kCoopSession{45.0f, 217.0f, 149.0f, 21.0f};
constexpr gui::Rect kCoopNew{465.0f, 300.0f, 28.0f, 14.0f};
constexpr gui::Rect kCoopLoad{465.0f, 343.0f, 28.0f, 14.0f};
constexpr gui::Rect kCoopStart{464.0f, 386.0f, 32.0f, 23.0f};
constexpr gui::Rect kCoopRemove{567.0f, 242.0f, 28.0f, 18.0f};

constexpr gui::Rect kDmCallsign{45.0f, 126.0f, 149.0f, 21.0f};
constexpr gui::Rect kDmStart{464.0f, 388.0f, 32.0f, 21.0f};
constexpr gui::Rect kDmDescriptionUp{428.0f, 251.0f, 28.0f, 14.0f};
constexpr gui::Rect kDmDescriptionDown{428.0f, 266.0f, 28.0f, 14.0f};
constexpr gui::Rect kDmTeamPicker{266.0f, 130.0f, 28.0f, 14.0f};
constexpr gui::Rect kDmShipPicker{399.0f, 168.0f, 28.0f, 14.0f};
constexpr gui::Rect kDmAiTurrets{578.0f, 312.0f, 25.0f, 16.0f};
constexpr gui::Rect kDmTargetPlayers{578.0f, 334.0f, 25.0f, 16.0f};
constexpr gui::Rect kDmTeamplay{578.0f, 356.0f, 25.0f, 16.0f};

struct ScenarioDefinition
{
	DeathmatchScenario scenario;
	std::uint16_t name;
	std::uint16_t description;
	std::uint16_t team_description;
	bool supports_teamplay;
};

// Exact record order and capability byte from the six 0x40-byte records at
// 0x0050c798.  FUN_0044b950 addresses these by visual row, not mission number.
constexpr ScenarioDefinition kScenarios[] = {
	{DeathmatchScenario::nuclear_threat, 0x2b0, 0x2b6, 0x2bc, true},
	{DeathmatchScenario::dark_reign, 0x2b1, 0x2b7, 0x2bd, false},
	{DeathmatchScenario::tag_bomb, 0x2b2, 0x2b8, 0x2be, true},
	{DeathmatchScenario::hunt_the_shadow, 0x2b3, 0x2b9, 0x2bf, true},
	{DeathmatchScenario::vampires, 0x2b4, 0x2ba, 0x2c0, true},
	{DeathmatchScenario::asteroid_field, 0x2af, 0x2b5, 0x2bb, false},
};

constexpr std::uint16_t kShipNames[] = {
	0x2c7, 0x2c8, 0x2c9, 0x2ca, 0x2cb, 0x2cc,
	0x2cd, 0x2ce, 0x2cf, 0x2d0, 0x2d1, 0x2d2,
};

constexpr std::uint32_t kTeamColours[] = {
	0xad0000ff,
	0x2ac900ff,
	0xfff600ff,
	0x4ac4ffff,
};

const char* safe_text(const char* text)
{
	return text == nullptr ? "" : text;
}

std::uint32_t bounded_players(
	const MultiplayerLobbyView& view,
	std::uint32_t capacity)
{
	return std::min<std::uint32_t>(view.player_count, capacity);
}

const ScenarioDefinition& scenario_definition(DeathmatchScenario scenario)
{
	for (const ScenarioDefinition& definition : kScenarios)
	{
		if (definition.scenario == scenario)
		{
			return definition;
		}
	}
	return kScenarios[0];
}

void draw_text(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	bgfx::TextureHandle palette,
	gui::TextAlign alignment = gui::TextAlign::left)
{
	x = gui::aligned_x(
		x,
		gui::text_width(
			renderer.shell.glyphs,
			renderer.shell.glyph_count,
			safe_text(text)),
		alignment);
	render::frontend_text(
		commands, renderer, safe_text(text), x, y, palette);
}

void draw_frame(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	float x,
	float y,
	float width,
	float height)
{
	// FUN_00435c60's three retail bevel colours.
	constexpr std::uint32_t bright = 0x00a7ffff;
	constexpr std::uint32_t middle = 0x008586ff;
	constexpr std::uint32_t dark = 0x0057cdff;
	render::frontend_rgba_quad(
		commands, renderer.white, x, y, width, 1.0f, bright);
	render::frontend_rgba_quad(
		commands, renderer.white, x, y, 1.0f, height, bright);
	render::frontend_rgba_quad(
		commands, renderer.white, x + 1.0f, y + 1.0f,
		width - 2.0f, 1.0f, middle);
	render::frontend_rgba_quad(
		commands, renderer.white, x + 1.0f, y + 1.0f,
		1.0f, height - 2.0f, middle);
	render::frontend_rgba_quad(
		commands, renderer.white, x, y + height - 1.0f,
		width, 1.0f, dark);
	render::frontend_rgba_quad(
		commands, renderer.white, x + width - 1.0f, y,
		1.0f, height, dark);
}

void draw_field(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const gui::Rect& rect,
	const char* value,
	bool editing,
	bgfx::TextureHandle palette)
{
	draw_frame(
		commands, renderer,
		rect.x, rect.y, rect.width, rect.height);
	render::frontend_scissor(
		commands,
		static_cast<std::uint16_t>(rect.x + 4.0f),
		static_cast<std::uint16_t>(rect.y + 2.0f),
		static_cast<std::uint16_t>(rect.width - 8.0f),
		static_cast<std::uint16_t>(rect.height - 4.0f));
	draw_text(
		commands, renderer, value,
		rect.x + 4.0f, rect.y + 2.0f, palette);
	if (editing)
	{
		const float caret_x = rect.x + 4.0f
			+ gui::text_width(
				renderer.shell.glyphs,
				renderer.shell.glyph_count,
				safe_text(value));
		draw_text(
			commands, renderer, "_",
			caret_x, rect.y + 2.0f, palette);
	}
	render::frontend_scissor(commands, 0, 0, 640, 480);
}

void draw_small_button(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	float x,
	float y,
	bool hovered)
{
	render::frontend_indexed_quad(
		commands,
		renderer.multiplayer.provider_button,
		renderer.multiplayer.screen_palette,
		x,
		y);
	if (hovered)
	{
		render::frontend_indexed_quad(
			commands,
			renderer.multiplayer.provider_button_selected,
			renderer.multiplayer.screen_palette,
			x,
			y);
	}
}

void draw_action_button(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	float x,
	float y,
	bool hovered)
{
	render::frontend_indexed_quad(
		commands,
		hovered
			? renderer.multiplayer.action_button_hover
			: renderer.multiplayer.action_button,
		renderer.multiplayer.screen_palette,
		x,
		y);
}

void draw_footer(
	const MultiplayerLobby& lobby,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	const auto gold = renderer.shell.font_gold_palette;
	const auto white = renderer.shell.font_white_palette;
	draw_small_button(
		commands, renderer, 323.0f, 421.0f,
		lobby.hovered == kHoverBack);
	draw_small_button(
		commands, renderer, 291.0f, 441.0f,
		lobby.hovered == kHoverMain);
	draw_small_button(
		commands, renderer, 323.0f, 441.0f,
		lobby.hovered == kHoverQuit);
	draw_text(
		commands, renderer, language_text(language, 0xf7),
		353.0f, 420.0f,
		lobby.hovered == kHoverBack ? white : gold);
	draw_text(
		commands, renderer, language_text(language, 0xbb),
		287.0f, 441.0f,
		lobby.hovered == kHoverMain ? white : gold,
		gui::TextAlign::right);
	draw_text(
		commands, renderer, language_text(language, 0xbc),
		353.0f, 441.0f,
		lobby.hovered == kHoverQuit ? white : gold);
}

void draw_chat(
	const MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	float y,
	float height)
{
	const auto gold = renderer.shell.font_gold_palette;
	const auto white = renderer.shell.font_white_palette;
	draw_text(
		commands, renderer, language_text(language, 0x140),
		49.0f, y - 17.0f, gold);
	draw_frame(commands, renderer, 45.0f, y, 379.0f, height);
	const std::uint32_t count = std::min<std::uint32_t>(
		view.chat_line_count, kMultiplayerLobbyChatLineCapacity);
	const std::uint32_t visible = static_cast<std::uint32_t>(
		std::max(0.0f, (height - 20.0f) / 12.0f));
	const std::uint32_t first = count > visible ? count - visible : 0;
	float line_y = y + 4.0f;
	render::frontend_scissor(
		commands,
		49,
		static_cast<std::uint16_t>(y + 3.0f),
		371,
		static_cast<std::uint16_t>(height - 6.0f));
	for (std::uint32_t index = first; index < count; ++index)
	{
		draw_text(
			commands, renderer, view.chat_lines[index],
			49.0f, line_y, gold);
		line_y += 12.0f;
	}
	if (safe_text(view.chat_entry)[0] != '\0'
		|| lobby.focus == MultiplayerLobbyFocus::chat)
	{
		const float entry_y = y + height - 15.0f;
		render::frontend_rgba_quad(
			commands, renderer.white,
			48.0f, entry_y - 1.0f, 372.0f, 13.0f, 0x000000c0);
		draw_text(
			commands, renderer, view.chat_entry,
			49.0f, entry_y, white);
		if (lobby.focus == MultiplayerLobbyFocus::chat)
		{
			const float caret_x = 49.0f
				+ gui::text_width(
					renderer.shell.glyphs,
					renderer.shell.glyph_count,
					safe_text(view.chat_entry));
			draw_text(
				commands, renderer, "_",
				caret_x, entry_y, white);
		}
	}
	render::frontend_scissor(commands, 0, 0, 640, 480);
}

void copy_campaign(
	MultiplayerLobbyAction& action,
	const MultiplayerLobbyView& view)
{
	action.campaign_identity = view.cooperative_campaign.identity;
	action.mission = view.cooperative_campaign.valid
		? view.cooperative_campaign.mission
		: view.cooperative_mission;
	action.selected_ship = view.cooperative_campaign.selected_ship;
	std::fill(
		std::begin(action.loadout),
		std::end(action.loadout),
		static_cast<std::int16_t>(-1));
	action.loadout_count = std::min<std::uint8_t>(
		view.cooperative_campaign.loadout_count,
		kMultiplayerLobbyLoadoutCapacity);
	if (view.cooperative_campaign.loadout != nullptr)
	{
		std::copy_n(
			view.cooperative_campaign.loadout,
			action.loadout_count,
			action.loadout);
	}
}

void draw_cooperative(
	const MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	const auto gold = renderer.shell.font_gold_palette;
	const auto white = renderer.shell.font_white_palette;
	const bool host = view.role == MultiplayerLobbyRole::host;
	draw_text(
		commands,
		renderer,
		language_text(language, host ? 0x147 : 0x146),
		320.0f,
		98.0f,
		gold,
		gui::TextAlign::center);

	if (!host)
	{
		draw_text(
			commands, renderer, language_text(language, 0x143),
			49.0f, 126.0f, gold);
		draw_field(
			commands, renderer, kCoopClientCallsign,
			view.callsign,
			lobby.focus == MultiplayerLobbyFocus::callsign,
			lobby.hovered == kHoverCallsign ? white : gold);
	}

	draw_text(
		commands, renderer, language_text(language, 0x13f),
		49.0f, 165.0f, gold);
	draw_field(
		commands, renderer, kCoopHostCallsign,
		host ? view.callsign : view.host_callsign,
		host && lobby.focus == MultiplayerLobbyFocus::callsign,
		host && lobby.hovered == kHoverCallsign ? white : gold);

	draw_text(
		commands, renderer, language_text(language, 0xc0),
		49.0f, 203.0f, gold);
	draw_field(
		commands, renderer, kCoopSession,
		view.session_name,
		host && lobby.focus == MultiplayerLobbyFocus::session_name,
		host && lobby.hovered == kHoverSession ? white : gold);

	draw_text(
		commands, renderer, language_text(language, 0xe5),
		203.0f, 205.0f, gold);
	draw_frame(commands, renderer, 201.0f, 217.0f, 84.0f, 21.0f);
	char mission[16];
	std::snprintf(
		mission,
		sizeof(mission),
		"%u",
		static_cast<unsigned>(
			view.cooperative_campaign.valid
				? view.cooperative_campaign.mission
				: view.cooperative_mission));
	draw_text(commands, renderer, mission, 205.0f, 219.0f, gold);

	draw_text(
		commands, renderer, language_text(language, 0x141),
		359.0f, 165.0f, gold);
	draw_text(
		commands, renderer, language_text(language, 0x142),
		591.0f, 165.0f, gold, gui::TextAlign::right);
	draw_frame(commands, renderer, 355.0f, 179.0f, 240.0f, 59.0f);
	const std::uint32_t player_count =
		bounded_players(view, kCooperativeLobbyPlayerCapacity);
	if (player_count == 0)
	{
		draw_text(
			commands, renderer, language_text(language, 0x144),
			359.0f, 181.0f, gold);
	}
	for (std::uint32_t index = 0; index < player_count; ++index)
	{
		const MultiplayerLobbyPlayer& player = view.players[index];
		if (!player.connected)
		{
			continue;
		}
		const bool selected =
			lobby.selected_player == static_cast<std::int8_t>(index);
		const auto palette = selected ? white : gold;
		const float y = 181.0f + index * 13.0f;
		draw_text(
			commands, renderer, player.callsign, 359.0f, y, palette);
		draw_text(
			commands,
			renderer,
			language_text(language, player.ready ? 0x145 : 0x5a8),
			591.0f,
			y,
			palette,
			gui::TextAlign::right);
	}

	draw_chat(
		lobby, view, language, renderer, commands, 264.0f, 117.0f);

	if (host)
	{
		draw_small_button(
			commands, renderer, 465.0f, 300.0f,
			lobby.hovered == kHoverNew);
		draw_small_button(
			commands, renderer, 465.0f, 343.0f,
			lobby.hovered == kHoverLoad);
		draw_text(
			commands, renderer, language_text(language, 0x148),
			499.0f, 300.0f,
			lobby.hovered == kHoverNew ? white : gold);
		draw_text(
			commands, renderer, language_text(language, 0x149),
			499.0f, 343.0f,
			lobby.hovered == kHoverLoad ? white : gold);
		draw_small_button(
			commands, renderer, 567.0f, 242.0f,
			lobby.hovered == kHoverRemove);
		draw_text(
			commands, renderer, language_text(language, 0x2c5),
			563.0f, 237.0f,
			lobby.hovered == kHoverRemove ? white : gold);
		draw_text(
			commands, renderer, language_text(language, 0x141),
			563.0f, 249.0f,
			lobby.hovered == kHoverRemove ? white : gold);
	}

	draw_action_button(
		commands, renderer, 464.0f, 386.0f,
		lobby.hovered == kHoverStart);
	draw_text(
		commands,
		renderer,
		language_text(
			language,
			host ? 0x14a : (view.local_ready ? 0x5a8 : 0x145)),
		host ? 498.0f : 494.0f,
		host ? 382.0f : 390.0f,
		lobby.hovered == kHoverStart ? white : gold);
}

void draw_team_swatch(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	float x,
	float y,
	std::int8_t team)
{
	if (team < 0 || team >= 4)
	{
		return;
	}
	render::frontend_rgba_quad(
		commands, renderer.white, x, y, 49.0f, 13.0f,
		kTeamColours[static_cast<std::size_t>(team)]);
}

void draw_deathmatch_description(
	MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	const ScenarioDefinition& scenario =
		scenario_definition(view.deathmatch_scenario);
	char description[512];
	std::snprintf(
		description,
		sizeof(description),
		"%s  %s %s",
		language_text(language, scenario.description),
		language_text(language, 0x2c6),
		language_text(language, scenario.team_description));

	gui::TextWrapCursor measure{description};
	char line[128];
	std::uint32_t line_count = 0;
	while (gui::next_wrapped_line(
		measure,
		renderer.shell.glyphs,
		renderer.shell.glyph_count,
		1.0f,
		215.0f,
		line))
	{
		++line_count;
	}
	const std::int32_t total_height =
		static_cast<std::int32_t>(line_count * 12);
	lobby.description_scroll_max = static_cast<std::int16_t>(
		std::max(0, total_height - 36));
	lobby.description_scroll = static_cast<std::int16_t>(std::clamp(
		static_cast<std::int32_t>(lobby.description_scroll),
		0,
		static_cast<std::int32_t>(lobby.description_scroll_max)));

	render::frontend_scissor(commands, 205, 207, 215, 36);
	gui::TextWrapCursor cursor{description};
	float y = 207.0f - lobby.description_scroll;
	while (gui::next_wrapped_line(
		cursor,
		renderer.shell.glyphs,
		renderer.shell.glyph_count,
		1.0f,
		215.0f,
		line))
	{
		draw_text(
			commands,
			renderer,
			line,
			205.0f,
			y,
			renderer.shell.font_gold_palette);
		y += 12.0f;
	}
	render::frontend_scissor(commands, 0, 0, 640, 480);

	render::frontend_indexed_quad(
		commands,
		renderer.multiplayer.lobby_scroll,
		renderer.multiplayer.screen_palette,
		428.0f,
		251.0f);
	if (lobby.hovered == kHoverDescriptionUp)
	{
		render::frontend_indexed_quad(
			commands,
			renderer.multiplayer.lobby_scroll_hover,
			renderer.multiplayer.screen_palette,
			428.0f,
			251.0f);
	}
	else if (lobby.hovered == kHoverDescriptionDown)
	{
		render::frontend_indexed_quad(
			commands,
			renderer.multiplayer.lobby_scroll_hover,
			renderer.multiplayer.screen_palette,
			428.0f,
			266.0f);
	}
}

void draw_rule(
	const MultiplayerLobby& lobby,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint16_t label,
	float y,
	bool checked,
	Hover hover)
{
	const auto palette = lobby.hovered == hover
		? renderer.shell.font_white_palette
		: renderer.shell.font_gold_palette;
	draw_text(
		commands, renderer, language_text(language, label),
		572.0f, y - 1.0f, palette, gui::TextAlign::right);
	render::frontend_indexed_quad(
		commands,
		renderer.multiplayer.lobby_checkbox,
		renderer.multiplayer.lobby_checkbox_palette,
		578.0f,
		y);
	if (checked)
	{
		render::frontend_indexed_quad(
			commands,
			renderer.multiplayer.lobby_checkmark,
			renderer.multiplayer.lobby_checkbox_palette,
			581.0f,
			y + 3.0f);
	}
}

void draw_deathmatch_pickers(
	const MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	const auto gold = renderer.shell.font_gold_palette;
	const auto white = renderer.shell.font_white_palette;
	if (lobby.team_picker_open)
	{
		for (std::int8_t team = 0; team < 4; ++team)
		{
			const float y = 150.0f + team * 22.0f;
			render::frontend_rgba_quad(
				commands, renderer.white,
				202.0f, y - 1.0f, 57.0f, 19.0f, 0x000000e0);
			draw_frame(commands, renderer, 203.0f, y, 55.0f, 17.0f);
			draw_team_swatch(commands, renderer, 206.0f, y + 2.0f, team);
			if (lobby.hovered == kHoverTeamFirst + team)
			{
				render::frontend_rgba_quad(
					commands, renderer.white,
					204.0f, y + 1.0f, 53.0f, 15.0f, 0xffffff35);
			}
		}
	}
	if (lobby.ship_picker_open)
	{
		for (std::int8_t ship = 0; ship < 12; ++ship)
		{
			const float y = 188.0f + ship * 22.0f;
			render::frontend_rgba_quad(
				commands, renderer.white,
				202.0f, y - 1.0f, 190.0f, 20.0f, 0x000000ef);
			draw_frame(commands, renderer, 203.0f, y, 188.0f, 18.0f);
			draw_text(
				commands,
				renderer,
				language_text(language, kShipNames[ship]),
				207.0f,
				y + 1.0f,
				lobby.hovered == kHoverShipFirst + ship
					? white
					: gold);
		}
	}
	(void)view;
}

void draw_deathmatch(
	MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	const auto gold = renderer.shell.font_gold_palette;
	const auto white = renderer.shell.font_white_palette;
	const bool host = view.role == MultiplayerLobbyRole::host;
	const bool teamplay_supported =
		deathmatch_scenario_supports_teamplay(view.deathmatch_scenario);
	const bool teamplay = view.teamplay && teamplay_supported;

	draw_text(
		commands,
		renderer,
		language_text(language, host ? 0x2c1 : 0x2d3),
		320.0f,
		host ? 98.0f : 95.0f,
		gold,
		gui::TextAlign::center);
	draw_text(
		commands, renderer, language_text(language, 0x143),
		49.0f, 109.0f, gold);
	draw_field(
		commands, renderer, kDmCallsign,
		view.callsign,
		lobby.focus == MultiplayerLobbyFocus::callsign,
		lobby.hovered == kHoverCallsign ? white : gold);

	draw_text(
		commands, renderer, language_text(language, 0x2c2),
		205.0f, 109.0f, gold);
	draw_frame(commands, renderer, 201.0f, 126.0f, 59.0f, 21.0f);
	const MultiplayerLobbyPlayer* local = nullptr;
	for (std::uint32_t index = 0;
		index < bounded_players(view, kMultiplayerLobbyPlayerCapacity);
		++index)
	{
		if (view.players[index].connected && view.players[index].local)
		{
			local = &view.players[index];
			break;
		}
	}
	if (local != nullptr)
	{
		draw_team_swatch(
			commands, renderer, 205.0f, 130.0f, local->team);
	}
	draw_small_button(
		commands, renderer, 266.0f, 130.0f,
		lobby.hovered == kHoverTeamPicker);

	draw_text(
		commands, renderer, language_text(language, 0x141),
		435.0f, 109.0f, gold);
	draw_text(
		commands,
		renderer,
		language_text(language, teamplay ? 0x2c2 : 0x142),
		591.0f,
		109.0f,
		gold,
		gui::TextAlign::right);
	draw_frame(commands, renderer, 431.0f, 126.0f, 164.0f, 120.0f);
	const std::uint32_t player_count =
		bounded_players(view, kMultiplayerLobbyPlayerCapacity);
	if (player_count == 0)
	{
		draw_text(
			commands, renderer, language_text(language, 0x144),
			435.0f, 130.0f, gold);
	}
	for (std::uint32_t index = 0; index < player_count; ++index)
	{
		const MultiplayerLobbyPlayer& player = view.players[index];
		if (!player.connected)
		{
			continue;
		}
		const bool selected =
			lobby.selected_player == static_cast<std::int8_t>(index);
		const auto palette = selected ? white : gold;
		const float y = 130.0f + index * 14.0f;
		draw_text(
			commands, renderer, player.callsign, 435.0f, y, palette);
		if (teamplay)
		{
			draw_team_swatch(
				commands, renderer, 574.0f, y + 1.0f, player.team);
		}
		else
		{
			draw_text(
				commands,
				renderer,
				language_text(language, player.ready ? 0x145 : 0x5a8),
				591.0f,
				y,
				palette,
				gui::TextAlign::right);
		}
	}

	if (host)
	{
		draw_text(
			commands, renderer, language_text(language, 0xc0),
			49.0f, 147.0f, gold);
		draw_frame(commands, renderer, 45.0f, 164.0f, 149.0f, 90.0f);
		for (std::size_t index = 0; index < std::size(kScenarios); ++index)
		{
			const bool selected =
				kScenarios[index].scenario == view.deathmatch_scenario;
			const bool hovered =
				lobby.hovered == kHoverScenarioFirst
					+ static_cast<std::int8_t>(index);
			draw_text(
				commands,
				renderer,
				language_text(language, kScenarios[index].name),
				49.0f,
				168.0f + index * 14.0f,
				(selected || hovered) ? white : gold);
		}
	}
	else
	{
		draw_text(
			commands, renderer, language_text(language, 0x13f),
			49.0f, 147.0f, gold);
		draw_field(
			commands, renderer,
			{45.0f, 164.0f, 149.0f, 21.0f},
			view.host_callsign, false, gold);
		draw_text(
			commands, renderer, language_text(language, 0xc0),
			49.0f, 185.0f, gold);
		draw_field(
			commands, renderer,
			{45.0f, 202.0f, 149.0f, 18.0f},
			language_text(
				language,
				scenario_definition(view.deathmatch_scenario).name),
			false,
			gold);
	}

	draw_text(
		commands, renderer, language_text(language, 0x2c3),
		205.0f, 147.0f, gold);
	draw_frame(commands, renderer, 201.0f, 164.0f, 192.0f, 21.0f);
	if (local != nullptr
		&& local->selected_ship >= 0
		&& local->selected_ship < 12)
	{
		draw_text(
			commands,
			renderer,
			language_text(
				language,
				kShipNames[
					static_cast<std::size_t>(local->selected_ship)]),
			205.0f,
			166.0f,
			gold);
	}
	draw_small_button(
		commands, renderer, 399.0f, 168.0f,
		lobby.hovered == kHoverShipPicker);

	draw_text(
		commands, renderer, language_text(language, 0x2c4),
		205.0f, 185.0f, gold);
	draw_frame(commands, renderer, 201.0f, 202.0f, 223.0f, 81.0f);
	draw_deathmatch_description(
		lobby, view, language, renderer, commands);
	draw_chat(
		lobby, view, language, renderer, commands, 300.0f, 81.0f);

	draw_rule(
		lobby, language, renderer, commands,
		0x30a, 312.0f, view.ai_turrets, kHoverAiTurrets);
	draw_rule(
		lobby, language, renderer, commands,
		0x30b, 334.0f, view.target_players, kHoverTargetPlayers);
	draw_rule(
		lobby, language, renderer, commands,
		0x30c, 356.0f, teamplay, kHoverTeamplay);

	draw_action_button(
		commands, renderer, 464.0f, 388.0f,
		lobby.hovered == kHoverStart);
	const std::uint16_t first_line = host
		? 0x5b6
		: (view.launch_allowed ? 0x329 : 0x30e);
	const std::uint16_t second_line =
		host || view.launch_allowed ? 0x5b7 : 0x30f;
	draw_text(
		commands, renderer, language_text(language, first_line),
		496.0f, 384.0f,
		lobby.hovered == kHoverStart ? white : gold);
	draw_text(
		commands, renderer, language_text(language, second_line),
		496.0f, 396.0f,
		lobby.hovered == kHoverStart ? white : gold);

	draw_deathmatch_pickers(
		lobby, view, language, renderer, commands);
}

std::int16_t tooltip_for(const MultiplayerLobby& lobby)
{
	switch (lobby.hovered)
	{
	case kHoverCallsign:
		return lobby.role == MultiplayerLobbyRole::host ? 0x59a : 0x59b;
	case kHoverSession: return 0x593;
	case kHoverNew: return 0x596;
	case kHoverLoad: return 0x597;
	case kHoverRemove: return 0x595;
	case kHoverTeamPicker: return 0x59f;
	case kHoverShipPicker: return 0x5a3;
	case kHoverAiTurrets: return 0x5a0;
	case kHoverTargetPlayers: return 0x5a1;
	case kHoverTeamplay: return 0x5a2;
	case kHoverStart:
		return lobby.role == MultiplayerLobbyRole::host ? 0x599 : 0x5a7;
	default: return -1;
	}
}

void draw_tooltip(
	MultiplayerLobby& lobby,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	if (lobby.tooltip_hover != lobby.hovered)
	{
		lobby.tooltip_hover = lobby.hovered;
		lobby.hovered_at = now;
	}
	const std::int16_t id = tooltip_for(lobby);
	if (id < 0 || now < lobby.hovered_at + 100)
	{
		return;
	}
	const char* text = language_text(
		language, static_cast<std::uint16_t>(id));
	const float text_width = gui::text_width(
		renderer.shell.glyphs,
		renderer.shell.glyph_count,
		text);
	float x = std::min(lobby.pointer_x, 634.0f - text_width);
	float y = lobby.pointer_y + 38.0f;
	if (y > 450.0f)
	{
		y = lobby.pointer_y - 19.0f;
	}
	render::frontend_rgba_quad(
		commands, renderer.white,
		x - 1.0f, y - 3.0f, text_width + 7.0f, 18.0f, 0x000000ff);
	constexpr std::uint32_t border = 0x000c21ff;
	render::frontend_rgba_quad(
		commands, renderer.white,
		x - 1.0f, y - 3.0f, text_width + 7.0f, 1.0f, border);
	render::frontend_rgba_quad(
		commands, renderer.white,
		x - 1.0f, y + 15.0f, text_width + 7.0f, 1.0f, border);
	render::frontend_rgba_quad(
		commands, renderer.white,
		x - 1.0f, y - 3.0f, 1.0f, 19.0f, border);
	render::frontend_rgba_quad(
		commands, renderer.white,
		x + text_width + 5.0f, y - 3.0f, 1.0f, 19.0f, border);
	draw_text(
		commands, renderer, text, x + 2.0f, y - 2.0f,
		renderer.shell.font_white_palette);
}

MultiplayerLobbyAction simple_action(MultiplayerLobbyActionType type)
{
	MultiplayerLobbyAction action;
	action.type = type;
	return action;
}

const char* focused_value(
	const MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view,
	std::size_t& capacity)
{
	switch (lobby.focus)
	{
	case MultiplayerLobbyFocus::callsign:
		capacity = kMultiplayerLobbyCallsignBytes;
		return safe_text(view.callsign);
	case MultiplayerLobbyFocus::session_name:
		capacity = kMultiplayerLobbySessionNameBytes;
		return safe_text(view.session_name);
	case MultiplayerLobbyFocus::chat:
		capacity = kMultiplayerLobbyChatBytes;
		return safe_text(view.chat_entry);
	default:
		capacity = 0;
		return "";
	}
}
}

void multiplayer_lobby_enter(
	MultiplayerLobby& lobby,
	MultiplayerLobbyKind kind,
	MultiplayerLobbyRole role,
	std::uint64_t now)
{
	lobby = {};
	lobby.pointer_x = 320.0f;
	lobby.pointer_y = 200.0f;
	lobby.hovered = kHoverNone;
	lobby.tooltip_hover = kHoverNone;
	lobby.selected_player = -1;
	lobby.kind = kind;
	lobby.role = role;
	lobby.entered_at = now;
	lobby.hovered_at = now;
}

void multiplayer_lobby_set_pointer(
	MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view,
	float x,
	float y,
	bool inside)
{
	lobby.pointer_x = x;
	lobby.pointer_y = y;
	lobby.kind = view.kind;
	lobby.role = view.role;
	lobby.hovered = kHoverNone;
	if (!inside)
	{
		return;
	}
	if (lobby.team_picker_open)
	{
		for (std::int8_t team = 0; team < 4; ++team)
		{
			if (gui::hit_open(
				{203.0f, 150.0f + team * 22.0f, 55.0f, 17.0f},
				x,
				y))
			{
				lobby.hovered = kHoverTeamFirst + team;
				return;
			}
		}
	}
	if (lobby.ship_picker_open)
	{
		for (std::int8_t ship = 0; ship < 12; ++ship)
		{
			if (gui::hit_open(
				{203.0f, 188.0f + ship * 22.0f, 188.0f, 18.0f},
				x,
				y))
			{
				lobby.hovered = kHoverShipFirst + ship;
				return;
			}
		}
	}
	if (gui::hit_open(kBackRegion, x, y))
	{
		lobby.hovered = kHoverBack;
		return;
	}
	if (gui::hit_open(kMainRegion, x, y))
	{
		lobby.hovered = kHoverMain;
		return;
	}
	if (gui::hit_open(kQuitRegion, x, y))
	{
		lobby.hovered = kHoverQuit;
		return;
	}

	const bool host = view.role == MultiplayerLobbyRole::host;
	if (view.kind == MultiplayerLobbyKind::cooperative)
	{
		if (gui::hit_open(
			host ? kCoopHostCallsign : kCoopClientCallsign, x, y))
		{
			lobby.hovered = kHoverCallsign;
		}
		else if (host && gui::hit_open(kCoopSession, x, y))
		{
			lobby.hovered = kHoverSession;
		}
		else if (gui::hit_open(kCoopStart, x, y))
		{
			lobby.hovered = kHoverStart;
		}
		else if (host && gui::hit_open(kCoopNew, x, y))
		{
			lobby.hovered = kHoverNew;
		}
		else if (host && gui::hit_open(kCoopLoad, x, y))
		{
			lobby.hovered = kHoverLoad;
		}
		else if (host && gui::hit_open(kCoopRemove, x, y))
		{
			lobby.hovered = kHoverRemove;
		}
		else if (host)
		{
			for (std::uint32_t index = 0;
				index < bounded_players(
					view, kCooperativeLobbyPlayerCapacity);
				++index)
			{
				if (gui::hit_open(
					{355.0f, 179.0f + index * 13.0f, 240.0f, 12.0f},
					x,
					y)
					&& view.players[index].connected
					&& !view.players[index].local)
				{
					lobby.hovered = kHoverPlayerFirst
						+ static_cast<std::int8_t>(index);
					break;
				}
			}
		}
		return;
	}

	if (gui::hit_open(kDmCallsign, x, y))
	{
		lobby.hovered = kHoverCallsign;
	}
	else if (gui::hit_open(kDmStart, x, y))
	{
		lobby.hovered = kHoverStart;
	}
	else if (gui::hit_open(kDmDescriptionUp, x, y))
	{
		lobby.hovered = kHoverDescriptionUp;
	}
	else if (gui::hit_open(kDmDescriptionDown, x, y))
	{
		lobby.hovered = kHoverDescriptionDown;
	}
	else if (gui::hit_open(kDmShipPicker, x, y))
	{
		lobby.hovered = kHoverShipPicker;
	}
	else if (view.teamplay
		&& deathmatch_scenario_supports_teamplay(
			view.deathmatch_scenario)
		&& gui::hit_open(kDmTeamPicker, x, y))
	{
		lobby.hovered = kHoverTeamPicker;
	}
	else if (host && gui::hit_open(kDmAiTurrets, x, y))
	{
		lobby.hovered = kHoverAiTurrets;
	}
	else if (host && gui::hit_open(kDmTargetPlayers, x, y))
	{
		lobby.hovered = kHoverTargetPlayers;
	}
	else if (host
		&& deathmatch_scenario_supports_teamplay(
			view.deathmatch_scenario)
		&& gui::hit_open(kDmTeamplay, x, y))
	{
		lobby.hovered = kHoverTeamplay;
	}
	else if (host)
	{
		for (std::size_t index = 0; index < std::size(kScenarios); ++index)
		{
			if (gui::hit_open(
				{48.0f, 168.0f + index * 14.0f, 147.0f, 12.0f},
				x,
				y))
			{
				lobby.hovered = kHoverScenarioFirst
					+ static_cast<std::int8_t>(index);
				return;
			}
		}
		for (std::uint32_t index = 0;
			index < bounded_players(
				view, kMultiplayerLobbyPlayerCapacity);
			++index)
		{
			if (gui::hit_open(
				{435.0f, 130.0f + index * 14.0f, 162.0f, 12.0f},
				x,
				y)
				&& view.players[index].connected
				&& !view.players[index].local)
			{
				lobby.hovered = kHoverPlayerFirst
					+ static_cast<std::int8_t>(index);
				return;
			}
		}
	}
}

MultiplayerLobbyAction multiplayer_lobby_select(
	MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view)
{
	if (lobby.hovered == kHoverBack)
	{
		return simple_action(MultiplayerLobbyActionType::back_to_browser);
	}
	if (lobby.hovered == kHoverMain)
	{
		return simple_action(MultiplayerLobbyActionType::main_menu);
	}
	if (lobby.hovered == kHoverQuit)
	{
		return simple_action(MultiplayerLobbyActionType::quit);
	}
	if (lobby.hovered == kHoverCallsign)
	{
		lobby.focus = MultiplayerLobbyFocus::callsign;
		return simple_action(MultiplayerLobbyActionType::focus_callsign);
	}
	const bool host = view.role == MultiplayerLobbyRole::host;
	if (view.kind == MultiplayerLobbyKind::cooperative)
	{
		if (host && lobby.hovered == kHoverSession)
		{
			lobby.focus = MultiplayerLobbyFocus::session_name;
			return simple_action(
				MultiplayerLobbyActionType::focus_session_name);
		}
		if (host && lobby.hovered == kHoverNew)
		{
			MultiplayerLobbyAction action = simple_action(
				MultiplayerLobbyActionType::new_cooperative_game);
			action.mission = 1;
			return action;
		}
		if (host && lobby.hovered == kHoverLoad)
		{
			// The coordinator enters FUN_00431730's load-mode equivalent
			// and restores this lobby only after a complete campaign load.
			return simple_action(
				MultiplayerLobbyActionType::load_cooperative_game);
		}
		if (host && lobby.hovered == kHoverRemove)
		{
			if (lobby.selected_player >= 0
				&& static_cast<std::uint32_t>(lobby.selected_player)
					< bounded_players(
						view, kCooperativeLobbyPlayerCapacity))
			{
				const MultiplayerLobbyPlayer& player =
					view.players[lobby.selected_player];
				if (player.connected && !player.local)
				{
					MultiplayerLobbyAction action = simple_action(
						MultiplayerLobbyActionType::remove_player);
					action.player_identity = player.identity;
					return action;
				}
			}
			return {};
		}
		if (lobby.hovered >= kHoverPlayerFirst
			&& lobby.hovered < kHoverPlayerFirst
				+ kCooperativeLobbyPlayerCapacity)
		{
			const std::int8_t index = lobby.hovered - kHoverPlayerFirst;
			if (index >= 0
				&& static_cast<std::uint32_t>(index)
					< bounded_players(
						view, kCooperativeLobbyPlayerCapacity)
				&& view.players[index].connected
				&& !view.players[index].local)
			{
				lobby.selected_player = index;
				MultiplayerLobbyAction action = simple_action(
					MultiplayerLobbyActionType::select_player);
				action.player_identity = view.players[index].identity;
				return action;
			}
		}
		if (lobby.hovered == kHoverStart)
		{
			if (host)
			{
				if (!view.launch_allowed
					|| !view.cooperative_campaign.valid)
				{
					return {};
				}
				// Retail START enters the shared prelaunch/loadout flow.
				MultiplayerLobbyAction action = simple_action(
					MultiplayerLobbyActionType::start_cooperative_game);
				copy_campaign(action, view);
				return action;
			}
			if (view.launch_allowed || view.local_ready)
			{
				MultiplayerLobbyAction action = simple_action(
					MultiplayerLobbyActionType::toggle_ready);
				action.value = !view.local_ready;
				return action;
			}
		}
		return {};
	}

	if (lobby.hovered == kHoverDescriptionUp)
	{
		lobby.description_scroll = static_cast<std::int16_t>(
			std::max(0, lobby.description_scroll - 4));
		return {};
	}
	if (lobby.hovered == kHoverDescriptionDown)
	{
		lobby.description_scroll = static_cast<std::int16_t>(
			std::min<int>(
				lobby.description_scroll_max,
				lobby.description_scroll + 4));
		return {};
	}
	if (lobby.hovered == kHoverTeamPicker)
	{
		lobby.team_picker_open = !lobby.team_picker_open;
		lobby.ship_picker_open = false;
		return {};
	}
	if (lobby.hovered == kHoverShipPicker)
	{
		lobby.ship_picker_open = !lobby.ship_picker_open;
		lobby.team_picker_open = false;
		return {};
	}
	if (lobby.hovered >= kHoverTeamFirst
		&& lobby.hovered < kHoverTeamFirst + 4)
	{
		MultiplayerLobbyAction action = simple_action(
			MultiplayerLobbyActionType::select_team);
		action.team = lobby.hovered - kHoverTeamFirst;
		lobby.team_picker_open = false;
		return action;
	}
	if (lobby.hovered >= kHoverShipFirst
		&& lobby.hovered < kHoverShipFirst + 12)
	{
		MultiplayerLobbyAction action = simple_action(
			MultiplayerLobbyActionType::select_ship);
		action.selected_ship = lobby.hovered - kHoverShipFirst;
		lobby.ship_picker_open = false;
		return action;
	}
	if (host && lobby.hovered == kHoverAiTurrets)
	{
		MultiplayerLobbyAction action = simple_action(
			MultiplayerLobbyActionType::toggle_ai_turrets);
		action.value = !view.ai_turrets;
		return action;
	}
	if (host && lobby.hovered == kHoverTargetPlayers)
	{
		MultiplayerLobbyAction action = simple_action(
			MultiplayerLobbyActionType::toggle_target_players);
		action.value = !view.target_players;
		return action;
	}
	if (host && lobby.hovered == kHoverTeamplay
		&& deathmatch_scenario_supports_teamplay(
			view.deathmatch_scenario))
	{
		MultiplayerLobbyAction action = simple_action(
			MultiplayerLobbyActionType::toggle_teamplay);
		action.value = !view.teamplay;
		return action;
	}
	if (host
		&& lobby.hovered >= kHoverScenarioFirst
		&& lobby.hovered
			< kHoverScenarioFirst
				+ static_cast<std::int8_t>(std::size(kScenarios)))
	{
		const std::size_t index = static_cast<std::size_t>(
			lobby.hovered - kHoverScenarioFirst);
		MultiplayerLobbyAction action = simple_action(
			MultiplayerLobbyActionType::select_deathmatch_scenario);
		action.scenario = kScenarios[index].scenario;
		action.mission = deathmatch_scenario_mission(action.scenario);
		lobby.description_scroll = 0;
		lobby.team_picker_open = false;
		return action;
	}
	if (host
		&& lobby.hovered >= kHoverPlayerFirst
		&& lobby.hovered
			< kHoverPlayerFirst + kMultiplayerLobbyPlayerCapacity)
	{
		const std::int8_t index = lobby.hovered - kHoverPlayerFirst;
		if (index >= 0
			&& static_cast<std::uint32_t>(index)
				< bounded_players(
					view, kMultiplayerLobbyPlayerCapacity)
			&& view.players[index].connected
			&& !view.players[index].local)
		{
			lobby.selected_player = index;
			MultiplayerLobbyAction action = simple_action(
				MultiplayerLobbyActionType::select_player);
			action.player_identity = view.players[index].identity;
			return action;
		}
	}
	if (lobby.hovered == kHoverStart)
	{
		if (host)
		{
			if (!view.launch_allowed)
			{
				return {};
			}
			MultiplayerLobbyAction action = simple_action(
				MultiplayerLobbyActionType::start_deathmatch);
			action.scenario = view.deathmatch_scenario;
			action.mission =
				deathmatch_scenario_mission(view.deathmatch_scenario);
			return action;
		}
		if (view.launch_allowed || view.local_ready)
		{
			MultiplayerLobbyAction action = simple_action(
				MultiplayerLobbyActionType::toggle_ready);
			action.value = !view.local_ready;
			return action;
		}
	}
	return {};
}

void multiplayer_lobby_begin_chat(MultiplayerLobby& lobby)
{
	lobby.focus = MultiplayerLobbyFocus::chat;
}

MultiplayerLobbyAction multiplayer_lobby_text(
	const MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view,
	const char* text)
{
	if (text == nullptr || lobby.focus == MultiplayerLobbyFocus::none)
	{
		return {};
	}
	std::size_t capacity = 0;
	const char* current = focused_value(lobby, view, capacity);
	const std::size_t current_length =
		std::min(std::strlen(current), capacity == 0 ? 0 : capacity - 1);
	if (capacity == 0 || current_length + 1 >= capacity)
	{
		return {};
	}
	MultiplayerLobbyAction action = simple_action(
		MultiplayerLobbyActionType::edit_text);
	action.text_field = lobby.focus;
	std::size_t output = 0;
	for (const unsigned char* character =
			reinterpret_cast<const unsigned char*>(text);
		*character != 0
			&& current_length + output + 1 < capacity
			&& output + 1 < sizeof(action.text);
		++character)
	{
		if (*character >= 0x20 && *character < 0x7f)
		{
			action.text[output++] = static_cast<char>(*character);
		}
	}
	action.text[output] = '\0';
	if (output == 0)
	{
		return {};
	}
	return action;
}

MultiplayerLobbyAction multiplayer_lobby_backspace(
	const MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view)
{
	std::size_t capacity = 0;
	const char* current = focused_value(lobby, view, capacity);
	if (capacity == 0 || current[0] == '\0')
	{
		return {};
	}
	MultiplayerLobbyAction action = simple_action(
		MultiplayerLobbyActionType::backspace_text);
	action.text_field = lobby.focus;
	return action;
}

MultiplayerLobbyAction multiplayer_lobby_submit_chat(
	MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view)
{
	if (safe_text(view.chat_entry)[0] == '\0')
	{
		lobby.focus = MultiplayerLobbyFocus::chat;
		return simple_action(MultiplayerLobbyActionType::focus_chat);
	}
	MultiplayerLobbyAction action = simple_action(
		MultiplayerLobbyActionType::send_chat);
	action.text_field = MultiplayerLobbyFocus::chat;
	std::snprintf(
		action.text,
		sizeof(action.text),
		"%s",
		safe_text(view.chat_entry));
	lobby.focus = MultiplayerLobbyFocus::none;
	return action;
}

void multiplayer_lobby_build(
	MultiplayerLobby& lobby,
	const MultiplayerLobbyView& view,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	lobby.kind = view.kind;
	lobby.role = view.role;
	gui::begin_screen(commands);
	render::frontend_rgba_quad(
		commands,
		renderer.multiplayer.lobby_background,
		0.0f,
		0.0f,
		render::kFrontendWidth,
		render::kFrontendHeight);
	if (view.kind == MultiplayerLobbyKind::cooperative)
	{
		draw_cooperative(lobby, view, language, renderer, commands);
	}
	else
	{
		draw_deathmatch(lobby, view, language, renderer, commands);
	}
	draw_footer(lobby, language, renderer, commands);
	draw_tooltip(lobby, language, renderer, commands, now);
	const std::uint64_t elapsed =
		now > lobby.entered_at ? now - lobby.entered_at : 0;
	gui::animated_cursor(
		commands,
		renderer.multiplayer.cursor,
		renderer.multiplayer.cursor_palette,
		lobby.pointer_x,
		lobby.pointer_y,
		elapsed,
		40);
}

std::uint16_t deathmatch_scenario_mission(DeathmatchScenario scenario)
{
	return static_cast<std::uint16_t>(scenario);
}

bool deathmatch_scenario_supports_teamplay(DeathmatchScenario scenario)
{
	return scenario_definition(scenario).supports_teamplay;
}
}
