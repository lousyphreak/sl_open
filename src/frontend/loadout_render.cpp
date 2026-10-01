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
constexpr std::uint64_t kPageTransitionMilliseconds = 1000;
constexpr std::uint64_t kHardpointZoomMilliseconds = 400;

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

void draw_ship_stats(
	const Loadout& loadout,
	const LoadoutCatalog& catalog,
	const LanguageTable& language,
	render::FrontendCommands& commands)
{
	const std::uint8_t ship = std::min<std::uint8_t>(loadout.selected_ship, 11);
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
	char detail[48];
	std::snprintf(
		detail,
		sizeof(detail),
		"%s       %u SECS",
		language_text(language, 0x211),
		static_cast<unsigned>(stats.afterburner_seconds));
	render::frontend_loadout_text(commands, 2, 2.0f, 147.0f, detail);
	std::snprintf(detail, sizeof(detail), "%s                         %u",
		language_text(language, 0x217),
		static_cast<unsigned>(definition.crew_count));
	render::frontend_loadout_text(commands, 2, 2.0f, 162.0f, detail);
	char capabilities[96]{};
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
	render::frontend_loadout_text(commands, 2, 2.0f, 177.0f, capabilities);
}

void draw_missile_stats(
	std::uint8_t missile,
	const LoadoutCatalog& catalog,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	missile = std::min<std::uint8_t>(missile, 9);
	char text[96];
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
	char line[96]{};
	std::size_t line_length = 0;
	float line_width = 0.0f;
	float line_y = 32.0f;
	for (const char* word = text; *word != '\0';)
	{
		while (*word == ' ')
		{
			++word;
		}
		const char* end = word;
		float word_width = 0.0f;
		while (*end != '\0' && *end != ' ')
		{
			const auto character = static_cast<std::uint8_t>(*end);
			if (character < renderer.loadout.info_glyph_count)
			{
				word_width +=
					renderer.loadout.info_glyphs[character].width;
			}
			++end;
		}
		const float space_width =
			line_length == 0
				? 0.0f
				: renderer.loadout.info_glyphs[
					static_cast<std::uint8_t>(' ')].width;
		if (line_length != 0
			&& line_width + space_width + word_width > 252.0f)
		{
			render::frontend_loadout_text(
				commands, 2, 2.0f, line_y, line);
			std::memset(line, 0, sizeof(line));
			line_length = 0;
			line_width = 0.0f;
			line_y += 15.0f;
		}
		if (line_length != 0 && line_length + 1 < sizeof(line))
		{
			line[line_length++] = ' ';
			line_width += space_width;
		}
		while (word < end && line_length + 1 < sizeof(line))
		{
			line[line_length++] = *word++;
		}
		line[line_length] = '\0';
		line_width += word_width;
		word = end;
	}
	if (line_length != 0)
	{
		render::frontend_loadout_text(
			commands, 2, 2.0f, line_y, line);
	}

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
	render::frontend_loadout_text(
		commands, 2, 128.0f, 195.0f,
		language_text(language, 0x220));
	render::frontend_loadout_text(
		commands, 2, 128.0f, 210.0f,
		language_text(language, 0x221));
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

	char ship_name[64];
	std::snprintf(
		ship_name,
		sizeof(ship_name),
		"%s",
		language_text(language, 0x1f7 + loadout.selected_ship));
	for (char* letter = ship_name; *letter != '\0'; ++letter)
	{
		*letter = static_cast<char>(
			std::toupper(static_cast<unsigned char>(*letter)));
	}
	render::frontend_loadout_scene(
		commands,
		loadout.selected_ship,
		loadout.available_ships,
		loadout.available_ship_mask,
		static_cast<std::uint8_t>(loadout.page),
		loadout.difficulty,
		loadout.use_default_loadout,
		loadout.mounted_loadout,
		loadout.missile_animation_active,
		loadout.missile_animation_removing,
		loadout.missile_animation_item,
		loadout.missile_animation_at,
		visible_missile_mask(loadout),
		loadout.missile_layout_tier,
		loadout.selected_missile,
		loadout.hovered_hardpoint,
		static_cast<std::uint8_t>(loadout.previous_page),
		loadout.page_transition_active
			? std::min(
				1.0f,
				static_cast<float>(now - loadout.page_transition_at)
					/ kPageTransitionMilliseconds)
			: 1.0f,
		loadout.page == LoadoutPage::missiles
			? std::clamp(
				static_cast<float>(
					now - loadout.page_transition_at
						- std::min<std::uint64_t>(
							now - loadout.page_transition_at,
							kPageTransitionMilliseconds))
					/ kHardpointZoomMilliseconds,
				0.001f,
				1.0f)
			: 1.0f,
		loadout.previous_ship,
		loadout.ship_selection_active
			? std::min(
				1.0f,
				static_cast<float>(now - loadout.ship_selection_at)
					/ kShipSelectionMilliseconds)
			: 1.0f,
		loadout.phase == LoadoutPhase::entering
			? std::min(
				1.0f,
				static_cast<float>(now - loadout.animation_at)
					/ kActivationMilliseconds)
			: (loadout.phase == LoadoutPhase::exiting
				? std::max(
					0.0f,
					1.0f
						- static_cast<float>(now - loadout.animation_at)
							/ kActivationMilliseconds)
				: (loadout.phase == LoadoutPhase::complete ? 0.0f : 1.0f)),
		loadout.phase == LoadoutPhase::exiting,
		loadout.previous_spin,
		now >= loadout.spin_at ? now - loadout.spin_at : 0);
	render::frontend_loadout_text(
		commands, 0, 17.0f, 4.0f, language_text(language, 0x222));
	render::frontend_loadout_text(
		commands, 1, 17.0f, 28.0f, ship_name);
	if (loadout.page == LoadoutPage::ships)
	{
		draw_ship_stats(loadout, catalog, language, commands);
	}
	else
	{
		if (loadout.page == LoadoutPage::missiles
			&& loadout.hovered_missile >= 0)
		{
			draw_missile_stats(
				static_cast<std::uint8_t>(loadout.hovered_missile),
				catalog,
				language,
				renderer,
				commands);
		}
	}
	if (loadout.phase == LoadoutPhase::active)
	{
		const char* tooltip = loadout.page == LoadoutPage::ships
			? language_text(language, 0x22f)
			: (loadout.page == LoadoutPage::missiles
				? (loadout.hovered_missile >= 0
					? language_text(
						language,
						static_cast<std::uint16_t>(
							0x203 + loadout.hovered_missile))
					: language_text(language, 0x22d))
				: language_text(language, 0x230));
		render::frontend_text(
			commands, renderer, tooltip, 235.0f, 456.0f,
			renderer.shell.font_gold_palette, 0x70ff90ff, 0.55f);
		render::frontend_loadout_cursor(
			commands, loadout.pointer_x, loadout.pointer_y);
	}
}
}
