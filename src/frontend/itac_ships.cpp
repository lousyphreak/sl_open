#include "frontend/itac_internal.hpp"
#include "frontend/itac_pages.hpp"
#include "frontend/itac_render_internal.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

#include <cctype>
#include <cstdio>

namespace sl_open::frontend
{
using namespace itac;
using namespace itac_render;

void itac_build_fighters(
	ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
		const FighterRecord& selected =
			fighter_record(shell, shell.fighter_selection);
		render::frontend_indexed_quad(
			commands,
			renderer.itac.fighters[selected.shape - 1],
			renderer.itac.fighter_palettes[
				fighter_palette_index(selected.shape)],
			91.0f,
			65.0f);
		render::frontend_indexed_quad(
			commands,
			renderer.itac.faction_icons[shell.fighter_faction],
			renderer.itac.faction_palette,
			shell.fighter_faction == 0 ? 551.0f : 478.0f,
			59.0f);

		char heading[128];
		const char* source =
			language_text(itac_language, selected.name);
		std::size_t index = 0;
		for (; source[index] != '\0' && index + 1 < sizeof(heading); ++index)
		{
			heading[index] = static_cast<char>(
				std::toupper(static_cast<unsigned char>(source[index])));
		}
		heading[index] = '\0';
		render::frontend_itac_text(
			commands,
			renderer,
			heading,
			34.0f,
			80.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);

		const std::uint16_t rating_labels[] = {
			1205, 1217, 1207, 1209, 1208, 1210, 1809, 1211};
		const std::uint16_t rating_values[] = {
			selected.type,
			selected.clearance,
			selected.speed,
			selected.acceleration,
			selected.agility,
			selected.shield_strength,
			selected.shield_recharge,
			selected.armor};
		for (std::uint8_t field = 0; field < 8; ++field)
		{
			if (field == 1 && selected.clearance == 0) continue;
			const float y = 254.0f + field * 15.0f;
			render::frontend_itac_text(
				commands,
				renderer,
				language_text(itac_language, rating_labels[field]),
				35.0f,
				y,
				renderer.shell.font_white_palette,
				0xffffffff,
				0.62f);
			char number[16];
			const char* value = nullptr;
			if (field < 2)
			{
				value =
					language_text(itac_language, rating_values[field]);
			}
			else
			{
				std::snprintf(
					number,
					sizeof(number),
					"%u",
					static_cast<unsigned>(rating_values[field]));
				value = number;
			}
			render::frontend_itac_text(
				commands,
				renderer,
				value,
				232.0f - itac_text_width(renderer, value),
				y,
				renderer.shell.font_blue_palette,
				0xffffffff,
				0.62f);
		}

		render::frontend_itac_text(
			commands,
			renderer,
			language_text(itac_language, 1215),
			255.0f,
			254.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);
		char value[256];
		std::snprintf(
			value,
			sizeof(value),
			"%u %s",
			static_cast<unsigned>(selected.afterburner),
			language_text(itac_language, 1233));
		render::frontend_itac_text(
			commands,
			renderer,
			value,
			452.0f - itac_text_width(renderer, value),
			254.0f,
			renderer.shell.font_blue_palette,
			0xffffffff,
			0.62f);
		render::frontend_itac_text(
			commands,
			renderer,
			language_text(itac_language, 1212),
			255.0f,
			269.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);
		float detail_y = 269.0f;
		for (std::uint8_t slot = 0;
			slot < 4 && selected.armament[slot] != 0;
			++slot)
		{
			if (selected.armament_count[slot] != 0)
			{
				std::snprintf(
					value,
					sizeof(value),
					"%u %s",
					static_cast<unsigned>(selected.armament_count[slot]),
					language_text(itac_language, selected.armament[slot]));
			}
			else
			{
				std::snprintf(
					value,
					sizeof(value),
					"%s",
					language_text(itac_language, selected.armament[slot]));
			}
			render::frontend_itac_text(
				commands,
				renderer,
				value,
				452.0f - itac_text_width(renderer, value),
				detail_y,
				renderer.shell.font_blue_palette,
				0xffffffff,
				0.62f);
			detail_y += 15.0f;
		}
		render::frontend_itac_text(
			commands,
			renderer,
			language_text(itac_language, 1213),
			255.0f,
			detail_y,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);
		std::snprintf(
			value,
			sizeof(value),
			"%u",
			static_cast<unsigned>(selected.crew));
		render::frontend_itac_text(
			commands,
			renderer,
			value,
			452.0f - itac_text_width(renderer, value),
			detail_y,
			renderer.shell.font_blue_palette,
			0xffffffff,
			0.62f);
		detail_y += 15.0f;
		value[0] = '\0';
		std::size_t used = 0;
		for (std::uint8_t slot = 0;
			slot < 4 && selected.capability[slot] != 0;
			++slot)
		{
			const int written = std::snprintf(
				value + used,
				sizeof(value) - used,
				"%s%s",
				slot == 0 ? "" : ", ",
				language_text(itac_language, selected.capability[slot]));
			if (written < 0
				|| static_cast<std::size_t>(written)
					>= sizeof(value) - used)
			{
				used = sizeof(value) - 1;
				break;
			}
			used += static_cast<std::size_t>(written);
		}
		if (used != 0)
		{
			draw_wrapped(
				commands,
				renderer,
				value,
				255.0f,
				detail_y,
				197.0f,
				0,
				2,
				renderer.shell.font_blue_palette,
				0xffffffff);
		}

		shell.fighter_visible_rows = 0;
		float row_y = 146.0f;
		const std::uint8_t count = fighter_count(shell);
		for (std::uint8_t record = 0;
			record < count
				&& shell.fighter_visible_rows < 12
				&& row_y < 364.0f;
			++record)
		{
			const char* name = language_text(
				itac_language,
				fighter_record(shell, record).name);
			const std::uint8_t lines =
				wrapped_line_count(renderer, name, 126.0f);
			const std::uint8_t row = shell.fighter_visible_rows++;
			shell.fighter_row_y[row] = static_cast<std::int16_t>(row_y);
			shell.fighter_row_height[row] =
				static_cast<std::int16_t>(lines * 11 + 4);
			draw_wrapped(
				commands,
				renderer,
				name,
				474.0f,
				row_y,
				126.0f,
				0,
				lines,
				record == shell.fighter_selection
					? renderer.shell.font_white_palette
					: renderer.shell.font_blue_palette,
				0xffffffff);
			row_y += lines * 11.0f + 6.0f;
		}
}

void itac_build_capitals(
	ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
		const CapitalRecord& selected =
			capital_record(shell, shell.capital_selection);
		if (selected.shape >= 0)
		{
			render::frontend_indexed_quad(
				commands,
				renderer.itac.capitals[selected.shape - 1],
				renderer.itac.capital_palettes[
					static_cast<std::uint8_t>((selected.shape - 1) / 3)],
				57.0f,
				234.0f);
		}
		render::frontend_indexed_quad(
			commands,
			renderer.itac.faction_icons[shell.capital_faction],
			renderer.itac.faction_palette,
			shell.capital_faction == 0 ? 551.0f : 478.0f,
			59.0f);

		char heading[128];
		const char* source =
			language_text(itac_language, selected.name);
		std::size_t index = 0;
		for (; source[index] != '\0' && index + 1 < sizeof(heading); ++index)
		{
			heading[index] = static_cast<char>(
				std::toupper(static_cast<unsigned char>(source[index])));
		}
		heading[index] = '\0';
		render::frontend_itac_text(
			commands,
			renderer,
			heading,
			35.0f,
			80.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);

		const std::uint16_t labels[] = {
			1294, 1295, 1296, 1297, 1298, 1299, 1300};
		const std::uint16_t values[] = {
			selected.commissioned,
			selected.type,
			selected.displacement,
			selected.propulsion,
			selected.spacecraft,
			selected.armament};
		for (std::uint8_t field = 0; field < 7; ++field)
		{
			const float y = 104.0f + field * 15.0f;
			render::frontend_itac_text(
				commands,
				renderer,
				language_text(itac_language, labels[field]),
				35.0f,
				y,
				renderer.shell.font_white_palette,
				0xffffffff,
				0.62f);
			char crew[16];
			const char* value = nullptr;
			if (field == 6)
			{
				std::snprintf(
					crew,
					sizeof(crew),
					"%u",
					static_cast<unsigned>(selected.crew));
				value = crew;
			}
			else
			{
				value = language_text(itac_language, values[field]);
			}
			render::frontend_itac_text(
				commands,
				renderer,
				value,
				232.0f - itac_text_width(renderer, value),
				y,
				renderer.shell.font_blue_palette,
				0xffffffff,
				0.62f);
		}

		render::frontend_itac_text(
			commands,
			renderer,
			language_text(itac_language, 135),
			257.0f,
			81.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);
		char description[4096];
		const char* primary =
			language_text(itac_language, selected.description);
		if (selected.description == 0x578 || selected.description == 0x5a8)
		{
			const char* continuation = language_text(
				itac_language,
				selected.description == 0x578 ? 0x78d : 0x78a);
			std::snprintf(
				description,
				sizeof(description),
				"%s%s",
				primary,
				continuation);
		}
		else
		{
			std::snprintf(description, sizeof(description), "%s", primary);
		}
		update_scroll_limit(
			shell.capital_body_scroll,
			shell.capital_body_scroll_max,
			wrapped_line_count(renderer, description, 203.0f),
			8);
		render::frontend_scissor(commands, 255, 103, 207, 88);
		draw_wrapped(
			commands,
			renderer,
			description,
			257.0f,
			103.0f,
			203.0f,
			shell.capital_body_scroll,
			8,
			renderer.shell.font_blue_palette,
			0xffffffff);
		render::frontend_scissor(commands, 0, 0, 640, 480);

		shell.capital_visible_rows = 0;
		float row_y = 140.0f;
		const std::uint8_t count = capital_count(shell);
		for (std::uint8_t record = shell.capital_viewport;
			record < count
				&& shell.capital_visible_rows < 13
				&& row_y < 360.0f;
			++record)
		{
			const char* name = language_text(
				itac_language,
				capital_record(shell, record).name);
			const std::uint8_t lines =
				wrapped_line_count(renderer, name, 126.0f);
			const std::uint8_t row = shell.capital_visible_rows++;
			shell.capital_row_y[row] = static_cast<std::int16_t>(row_y);
			shell.capital_row_height[row] =
				static_cast<std::int16_t>(lines * 11 + 4);
			draw_wrapped(
				commands,
				renderer,
				name,
				474.0f,
				row_y,
				126.0f,
				0,
				lines,
				record == shell.capital_selection
					? renderer.shell.font_white_palette
					: renderer.shell.font_blue_palette,
				0xffffffff);
			row_y += lines * 11.0f + 6.0f;
		}
}
}
