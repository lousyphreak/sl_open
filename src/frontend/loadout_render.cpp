#include "frontend/loadout.hpp"

#include "assets/player_ship.hpp"
#include "frontend/gui_render.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace sl_open::frontend
{
namespace
{
constexpr std::uint64_t kActivationMilliseconds = 2000;
constexpr std::uint64_t kShipSelectionMilliseconds = 1500;

std::uint16_t visible_missile_mask(const Loadout& loadout)
{
	std::uint16_t mask = 0;
	for (std::uint8_t missile = 0; missile < 10; ++missile)
	{
		if (loadout_missile_available(loadout, missile))
		{
			mask |= static_cast<std::uint16_t>(1u << missile);
		}
	}
	return mask;
}

float draw_wrapped_text(
	const char* text, float y,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands, std::uint8_t max_lines)
{
	// Frontend_draw_wrapped_text (0x00480fd0): preserve explicit newlines,
	// split at spaces or hyphens, and hyphenate a word wider than the panel.
	for (std::uint8_t row = 0; *text != '\0' && row < max_lines; ++row)
	{
		const char* end = text;
		int remaining = 252;
		while (*end != '\0' && *end != '\n' && remaining > 0)
		{
			remaining -= renderer.loadout.info_glyphs[static_cast<std::uint8_t>(*end)].width;
			if (remaining < 0)
			{
				break;
			}
			++end;
		}
		const char* next = end;
		bool hyphenate = false;
		if (*end == '\n')
		{
			++next;
		}
		else if (*end != '\0')
		{
			const char* split = end;
			while (split > text && *split != ' ')
			{
				--split;
			}
			if (split > text)
			{
				end = split;
				next = split + 1;
			}
			else
			{
				split = end;
				while (split > text && *split != '-')
				{
					--split;
				}
				if (split > text)
				{
					end = next = split + 1;
				}
				else
				{
					next = --end;
					hyphenate = true;
				}
			}
		}
		char line[1024];
		const std::size_t length = static_cast<std::size_t>(end - text);
		std::memcpy(line, text, length);
		if (hyphenate)
		{
			line[length] = '-';
		}
		line[length + (hyphenate ? 1 : 0)] = '\0';
		render::frontend_loadout_text(commands, 2, 2.0f, y, line);
		y += 15.0f;
		text = next;
	}
	return y;
}


void draw_ship_stats(
	std::uint8_t ship,
	const LoadoutCatalog& catalog,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	const assets::PlayerShipDefinition& definition =
		assets::kPlayerShipDefinitions[ship];
	const LoadoutShipStats& stats = catalog.ship_stats[ship];
	const char* labels[] = {
		language_text(language, 0x20e),
		language_text(language, 0x210),
		language_text(language, 0x20f),
		language_text(language, 0x212),
		language_text(language, 0x213),
		language_text(language, 0x214),
	};
	const std::uint8_t ratings[] = {
		stats.max_speed_rating,
		stats.acceleration_rating,
		stats.agility_rating,
		stats.shield_power_rating,
		stats.shield_recharge_rating,
		stats.armor_rating,
	};
	char heading[64];
	const std::uint16_t class_text = definition.ship_class == 7
		? 0x553
		: static_cast<std::uint16_t>(0x222 + definition.ship_class);
	std::snprintf(
		heading,
		sizeof(heading),
		"%s%s",
		language_text(language, 0x554),
		language_text(language, class_text));
	render::frontend_loadout_text(commands, 2, 2.0f, 12.0f, heading);
	std::snprintf(
		heading,
		sizeof(heading),
		"%s%s",
		language_text(language, 0x555),
		language_text(
			language,
			static_cast<std::uint16_t>(0x228 + definition.access_level)));
	render::frontend_loadout_text(commands, 2, 2.0f, 32.0f, heading);
	for (std::uint8_t row = 0; row < 6; ++row)
	{
		const float y = 57.0f + row * 15.0f;
		char label[64];
		std::snprintf(label, sizeof(label), "%s", labels[row]);
		for (char* letter = label; *letter != '\0'; ++letter)
		{
			*letter = static_cast<char>(
				std::toupper(static_cast<unsigned char>(*letter)));
		}
		render::frontend_loadout_text(commands, 2, 2.0f, y, label);
		for (std::uint8_t rating = 0; rating < 10; ++rating)
		{
			render::frontend_loadout_bar(
				commands,
				184.0f + rating * 7.0f,
				y,
				4.0f,
				10.0f,
				rating < ratings[row]);
		}
	}
	char detail[64];
	constexpr std::uint16_t value_labels[] = {0x211, 0x217};
	const unsigned values[] = {stats.afterburner_seconds, definition.crew_count};
	for (std::uint8_t row = 0; row < 2; ++row)
	{
		const float y = 147.0f + row * 15.0f;
		std::snprintf(detail, sizeof(detail), "%s", language_text(language, value_labels[row]));
		for (char* letter = detail; *letter != '\0'; ++letter)
		{
			*letter = static_cast<char>(std::toupper(static_cast<unsigned char>(*letter)));
		}
		render::frontend_loadout_text(commands, 2, 2.0f, y, detail);
		std::snprintf(detail, sizeof(detail), "%u %s", values[row],
			row == 0 ? language_text(language, 0x21c) : "");
		render::frontend_loadout_text(commands, 2, 180.0f, y, detail);
	}
	char capabilities[512]{};
	std::size_t capabilities_length = 0;
	for (const assets::PlayerShipCapabilityDefinition& definition :
		assets::kPlayerShipCapabilityDefinitions)
	{
		if (!assets::player_ship_has_capability(
				ship, definition.capability))
		{
			continue;
		}
		const int written = std::snprintf(
			capabilities + capabilities_length,
			sizeof(capabilities) - capabilities_length,
			capabilities_length == 0 ? "%s" : ", %s",
			language_text(language, definition.language_id));
		capabilities_length += static_cast<std::size_t>(written);
	}
	for (char* letter = capabilities; *letter != '\0'; ++letter)
	{
		*letter = static_cast<char>(
			std::toupper(static_cast<unsigned char>(*letter)));
	}
	draw_wrapped_text(capabilities, 180.0f, renderer, commands, 3);
}


void draw_armament(
	std::uint8_t ship,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	// LANCER.EXE 0x004ec29c, stride 0x22c: language ID/count pairs.
	constexpr std::int16_t weapons[12][8] = {
		{571, 2, 1358, 1, -1}, {569, 2, -1},
		{577, 2, 568, 2, 1359, 1, -1}, {572, 2, 1358, 1, -1},
		{571, 2, 1358, 1, -1}, {574, 2, 570, 2, -1},
		{573, 2, 569, 2, 1359, 1, -1}, {573, 2, 571, 2, -1},
		{576, 2, 568, 2, 573, 1, 1359, 1}, {572, 2, 578, 2, 1361, 1, -1},
		{571, 2, -1}, {572, 2, 569, 2, 575, 1, 1362, 1},
	};
	// Description fragments at 0x004ee548, five signed IDs per gun.
	constexpr std::int16_t descriptions[11][5] = {
		{588, 580, 591, 582, 586}, {588, 580, 591, 583, 586},
		{589, 580, 591, 583, 585}, {589, 580, 591, 583, 587},
		{590, 581, 594, 584, 585}, {590, 581, 594, 583, 585},
		{590, 581, 592, 582, 585}, {590, 579, 593, 583, 586},
		{-1, 581, 593, 583, 587}, {-1, 581, 593, 584, 585},
		{-1, 0, 0, 0, 0},
	};
	float y = 0.0f;
	for (std::uint8_t weapon = 0; weapon < 4; ++weapon)
	{
		const std::int16_t id = weapons[ship][weapon * 2];
		if (id == -1)
		{
			break;
		}
		char text[512];
		std::snprintf(text, sizeof(text), "%s X %d", language_text(language, id),
			weapons[ship][weapon * 2 + 1]);
		for (char* letter = text; *letter != '\0'; ++letter)
		{
			*letter = static_cast<char>(std::toupper(static_cast<unsigned char>(*letter)));
		}
		render::frontend_loadout_text(commands, 2, 2.0f, y, text);
		y += 20.0f;
		if (id < 1358)
		{
			const auto& ids = descriptions[id - 568];
			std::snprintf(text, sizeof(text), "%s%s%s%s%s",
				ids[0] == -1 ? "" : language_text(language, ids[0]),
				language_text(language, ids[1]), language_text(language, ids[2]),
				language_text(language, ids[3]), language_text(language, ids[4]));
			y = draw_wrapped_text(text, y, renderer, commands, 20) + 10.0f;
		}
	}
}

void draw_missile_stats(
	std::uint8_t missile,
	const LoadoutCatalog& catalog,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	missile = std::min<std::uint8_t>(missile, 9);
	char text[512];
	std::snprintf(
		text,
		sizeof(text),
		"%s",
		language_text(
			language,
			static_cast<std::uint16_t>(0x203 + missile)));
	for (char* letter = text; *letter != '\0'; ++letter)
	{
		*letter = static_cast<char>(
			std::toupper(static_cast<unsigned char>(*letter)));
	}
	render::frontend_loadout_text(commands, 2, 2.0f, 12.0f, text);
	std::snprintf(
		text,
		sizeof(text),
		"%s",
		language_text(
			language,
			static_cast<std::uint16_t>(
				0x1e5 + (missile == 5 ? 6
					: missile == 6 ? 5
					: missile == 7 ? 8
					: missile == 8 ? 7
					: missile))));
	for (char* letter = text; *letter != '\0'; ++letter)
	{
		*letter = static_cast<char>(
			std::toupper(static_cast<unsigned char>(*letter)));
	}
	draw_wrapped_text(text, 32.0f, renderer, commands, 6);

	constexpr std::uint16_t labels[4] = {
		0x218, 0x219, 0x21a, 0x21b};
	const LoadoutMissileStats& stats = catalog.missile_stats[missile];
	const std::int8_t values[] = {
		stats.lock_seconds,
		stats.speed_rating,
		stats.travel_rating,
		stats.damage_rating,
	};
	for (std::uint8_t row = 0; row < 4; ++row)
	{
		const float y = 127.0f + row * 15.0f;
		std::snprintf(
			text,
			sizeof(text),
			"%s",
			language_text(language, labels[row]));
		for (char* letter = text; *letter != '\0'; ++letter)
		{
			*letter = static_cast<char>(
				std::toupper(static_cast<unsigned char>(*letter)));
		}
		render::frontend_loadout_text(commands, 2, 2.0f, y, text);
		const std::int8_t value = values[row];
		if (row == 0)
		{
			if (value < 0)
			{
				render::frontend_loadout_text(
					commands, 2, 180.0f, y, "---");
			}
			else
			{
				std::snprintf(
					text,
					sizeof(text),
					"%d %s",
					value,
					language_text(language, 0x21c));
				render::frontend_loadout_text(
					commands, 2, 180.0f, y, text);
			}
			continue;
		}
		if (value < 0)
		{
			render::frontend_loadout_text(
				commands, 2, 180.0f, y, "---");
			continue;
		}
		for (std::uint8_t rating = 0; rating < 10; ++rating)
		{
			render::frontend_loadout_bar(
				commands,
				184.0f + rating * 7.0f,
				y,
				4.0f,
				10.0f,
				rating < value);
		}
	}
	for (std::uint8_t row = 0; row < 2; ++row)
	{
		const char* help = language_text(language, 0x220 + row);
		float width = 0.0f;
		for (const auto* character = reinterpret_cast<const std::uint8_t*>(help);
			*character != 0; ++character)
		{
			width += renderer.loadout.info_glyphs[*character].width;
		}
		render::frontend_loadout_text(commands, 2, 128.0f - width * 0.5f,
			195.0f + row * 15.0f, help);
	}
}
}

void loadout_build(
	const Loadout& loadout,
	const LoadoutCatalog& catalog,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	gui::begin_screen(commands);
	render::frontend_rgba_quad(
		commands,
		renderer.loadout.backgrounds[loadout.late_campaign ? 1 : 0],
		0.0f,
		0.0f,
		640.0f,
		480.0f);

	render::LoadoutRenderState state{};
	state.selected_ship = loadout.selected_ship;
	state.previous_ship = loadout.previous_ship;
	state.available_ships = loadout.available_ships;
	state.available_ship_mask = loadout.available_ship_mask;
	state.selector_lod = loadout.selector_lod;
	state.page = static_cast<std::uint8_t>(loadout.page);
	state.previous_page = static_cast<std::uint8_t>(loadout.previous_page);
	state.missile_mask = visible_missile_mask(loadout);
	state.missile_layout_tier = loadout.missile_layout_tier;
	state.pressed_button = loadout.pressed_button;
	state.page_elapsed = loadout.page_transition_active
		? static_cast<float>(now - loadout.page_transition_at) : 10000.0f;
	state.page_transition = std::min(state.page_elapsed / kShipSelectionMilliseconds, 1.0f);
	state.ship_selection = loadout.ship_selection_active
		? std::min(static_cast<float>(now - loadout.ship_selection_at) / kShipSelectionMilliseconds, 1.0f)
		: 1.0f;
	state.name_flip = loadout.ship_selection_active ? state.ship_selection : 0.0f;
	state.info_flip = loadout.ship_selection_active ? state.ship_selection
		: (loadout.page_transition_active
			&& (loadout.page == LoadoutPage::guns || loadout.previous_page != LoadoutPage::ships)
			? state.page_transition : 0.0f);
	state.activation = loadout.phase == LoadoutPhase::entering
		? std::min(static_cast<float>(now - loadout.animation_at) / kActivationMilliseconds, 1.0f)
		: (loadout.phase == LoadoutPhase::exiting
			? std::max(1.0f - static_cast<float>(now - loadout.animation_at) / kActivationMilliseconds, 0.0f)
			: (loadout.phase == LoadoutPhase::complete ? 0.0f : 1.0f));
	state.reverse = loadout.phase == LoadoutPhase::exiting;
	state.previous_spin = loadout.previous_spin;
	state.spin = now >= loadout.spin_at
		? static_cast<float>(now - loadout.spin_at) * 0.0015707963611930609f : 0.0f;
	state.blink_launch = loadout_launch_button_hidden(loadout, now);
	std::copy(std::begin(loadout.mounted_loadout), std::end(loadout.mounted_loadout),
		std::begin(state.mounted_loadout));
	for (std::uint8_t hardpoint = 0; hardpoint < 20; ++hardpoint)
	{
		state.missile_animation_active[hardpoint] = loadout.missile_animation_active[hardpoint];
		state.missile_animation_removing[hardpoint] = loadout.missile_animation_removing[hardpoint];
		state.missile_animation_item[hardpoint] = loadout.missile_animation_item[hardpoint];
		state.missile_animation_progress[hardpoint] = std::min(
			static_cast<float>(now - loadout.missile_animation_at[hardpoint]) / 1000.0f, 1.0f);
	}
	render::frontend_loadout_scene(commands, state);
	const std::uint8_t info_ship = loadout.ship_selection_active && state.ship_selection < 0.5f
		? loadout.previous_ship : loadout.selected_ship;
	char ship_name[64];
	std::snprintf(ship_name, sizeof(ship_name), "%s", language_text(language, 0x1f7 + info_ship));
	for (char* letter = ship_name; *letter != '\0'; ++letter)
	{
		*letter = static_cast<char>(std::toupper(static_cast<unsigned char>(*letter)));
	}
	render::frontend_loadout_text(
		commands, 0, 17.0f, 4.0f, language_text(language, 0x222));
	render::frontend_loadout_text(
		commands, 1, 17.0f, 28.0f, ship_name);
	const LoadoutPage info_page = state.info_flip > 0.0f && state.info_flip < 0.5f
		? loadout.previous_page : loadout.page;
	if (info_page == LoadoutPage::guns)
	{
		draw_armament(info_ship, language, renderer, commands);
	}
	else if (info_page == LoadoutPage::missiles && loadout.hovered_missile >= 0)
	{
		draw_missile_stats(static_cast<std::uint8_t>(loadout.hovered_missile),
			catalog, language, renderer, commands);
	}
	else
	{
		draw_ship_stats(info_ship, catalog, language, renderer, commands);
	}
	if (loadout.phase == LoadoutPhase::active
		&& !loadout.page_transition_active && !loadout.ship_selection_active)
	{
		const char* tooltip = "";
		const std::int16_t object = loadout.hovered_object;
		if (object >= 0 && object < 6)
		{
			constexpr std::uint16_t ids[] = {0x22e, 0x22d, 0x22f, 0x230, 0x231, 0x232};
			tooltip = language_text(language, ids[object]);
		}
		else if (object >= 16 && object < 28)
		{
			tooltip = language_text(language, 0x1f7 + object - 16);
		}
		else if (object >= 32 && object < 42)
		{
			tooltip = language_text(language, 0x203 + object - 32);
		}
		else if (object >= 48)
		{
			const std::int16_t missile = loadout.mounted_loadout[object - 48];
			tooltip = missile >= 0 ? language_text(language, 0x203 + missile) : "Hardpoint";
		}
		float tooltip_width = 0.0f;
		for (const auto* character = reinterpret_cast<const std::uint8_t*>(tooltip);
			*character != 0; ++character)
		{
			tooltip_width += renderer.loadout.title_glyphs[*character].width;
		}
		float text_x = 320.0f - tooltip_width * 0.5f;
		for (const auto* character = reinterpret_cast<const std::uint8_t*>(tooltip);
			*character != 0; ++character)
		{
			const render::FrontendGlyph& glyph = renderer.loadout.title_glyphs[*character];
			render::frontend_indexed_scaled_region(commands, renderer.loadout.title_font,
				renderer.loadout.title_font_palette, text_x, 458.0f,
				static_cast<float>(glyph.width), static_cast<float>(renderer.loadout.title_font_height),
				glyph.x, glyph.y, glyph.width, renderer.loadout.title_font_height);
			text_x += glyph.width;
		}
		render::frontend_loadout_cursor(
			commands, loadout.pointer_x, loadout.pointer_y);
	}
}
}
