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

void itac_build_squads(
	ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
		const SquadRecord& selected =
			squad_record(shell, shell.squad_selection);
		update_scroll_limit(
			shell.squad_body_scroll,
			shell.squad_body_scroll_max,
			wrapped_line_count(
				renderer,
				language_text(itac_language, selected.description),
				179.0f),
			12);
		render::frontend_indexed_quad(
			commands,
			renderer.itac.squads[selected.shape - 1],
			renderer.itac.squad_palettes[
				squad_palette_index(selected.palette)],
			80.0f,
			263.0f);
		render::frontend_indexed_quad(
			commands,
			renderer.itac.faction_icons[shell.squad_faction],
			renderer.itac.faction_palette,
			shell.squad_faction == 0 ? 551.0f : 478.0f,
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
			79.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);
		const std::uint16_t labels[] = {60, 143, 144, 525};
		const std::uint16_t values[] = {
			selected.squad_class,
			selected.leader,
			selected.base,
			selected.nation};
		for (std::uint8_t field = 0; field < 4; ++field)
		{
			const float y = 105.0f + field * 22.0f;
			render::frontend_itac_text(
				commands,
				renderer,
				language_text(itac_language, labels[field]),
				34.0f,
				y,
				renderer.shell.font_white_palette,
				0xffffffff,
				0.62f);
			render::frontend_itac_text(
				commands,
				renderer,
				language_text(itac_language, values[field]),
				94.0f,
				y,
				renderer.shell.font_blue_palette,
				0xffffffff,
				0.62f);
		}

		render::frontend_scissor(commands, 226, 79, 185, 130);
		draw_wrapped(
			commands,
			renderer,
			language_text(itac_language, selected.description),
			228.0f,
			81.0f,
			179.0f,
			shell.squad_body_scroll,
			12,
			renderer.shell.font_blue_palette,
			0xffffffff);
		render::frontend_scissor(commands, 0, 0, 640, 480);

		shell.squad_visible_rows = 0;
		float row_y = 138.0f;
		const std::uint8_t count = squad_count(shell);
		for (std::uint8_t record = shell.squad_viewport;
			record < count
				&& shell.squad_visible_rows < 13
				&& row_y < 361.0f;
			++record)
		{
			const char* name =
				language_text(itac_language, squad_record(shell, record).name);
			const std::uint8_t lines =
				wrapped_line_count(renderer, name, 126.0f);
			const std::uint8_t row = shell.squad_visible_rows++;
			shell.squad_row_y[row] = static_cast<std::int16_t>(row_y);
			shell.squad_row_height[row] =
				static_cast<std::int16_t>(lines * 11 + 4);
			draw_wrapped(
				commands,
				renderer,
				name,
				472.0f,
				row_y,
				126.0f,
				0,
				lines,
				record == shell.squad_selection
					? renderer.shell.font_white_palette
					: renderer.shell.font_blue_palette,
				0xffffffff);
			row_y += lines * 11.0f + 6.0f;
		}
}

void itac_build_personnel(
	ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
		const PilotRecord& selected =
			pilot_record(shell, shell.pilot_selection);
		render::frontend_indexed_quad(
			commands,
			renderer.itac.persons[selected.portrait_shape - 1],
			renderer.itac.person_palettes[
				pilot_palette_index(selected.portrait_shape)],
			296.0f,
			76.0f);
		render::frontend_indexed_quad(
			commands,
			renderer.itac.faction_icons[shell.pilot_faction],
			renderer.itac.faction_palette,
			shell.pilot_faction == 0 ? 551.0f : 478.0f,
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
			79.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);

		const std::uint16_t labels[] = {987, 988, 989, 990};
		const std::uint16_t values[] = {
			selected.age,
			selected.nationality,
			selected.ship,
			selected.callsign};
		float field_y = 105.0f;
		for (std::uint8_t field = 0; field < 4; ++field)
		{
			if (values[field] == 0) continue;
			render::frontend_itac_text(
				commands,
				renderer,
				language_text(itac_language, labels[field]),
				34.0f,
				field_y,
				renderer.shell.font_white_palette,
				0xffffffff,
				0.62f);
			render::frontend_itac_text(
				commands,
				renderer,
				language_text(itac_language, values[field]),
				258.0f - itac_text_width(
					renderer,
					language_text(itac_language, values[field])),
				field_y,
				renderer.shell.font_blue_palette,
				0xffffffff,
				0.62f);
			field_y += 26.0f;
		}

		render::frontend_itac_text(
			commands,
			renderer,
			language_text(itac_language, 135),
			35.0f,
			244.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);
		render::frontend_scissor(commands, 35, 266, 405, 60);
		const std::uint16_t section_labels[] = {992, 993, 994};
		const std::uint16_t section_values[] = {
			selected.military_history,
			selected.training,
			selected.background};
		std::uint16_t total_body_lines = 0;
		for (std::uint8_t section = 0; section < 3; ++section)
		{
			total_body_lines = static_cast<std::uint16_t>(
				total_body_lines
				+ (section == 0 ? 0 : 1)
				+ wrapped_line_count(
					renderer,
					language_text(
						itac_language, section_labels[section]),
					399.0f)
				+ wrapped_line_count(
					renderer,
					language_text(
						itac_language, section_values[section]),
					399.0f));
		}
		update_scroll_limit(
			shell.pilot_body_scroll,
			shell.pilot_body_scroll_max,
			total_body_lines,
			6);
		std::int16_t skipped = shell.pilot_body_scroll;
		std::uint16_t body_lines = 0;
		for (std::uint8_t section = 0;
			section < 3 && body_lines < 6;
			++section)
		{
			const char* label =
				language_text(itac_language, section_labels[section]);
			const char* value =
				language_text(itac_language, section_values[section]);
			const std::uint8_t label_lines =
				wrapped_line_count(renderer, label, 399.0f);
			const std::uint8_t value_lines =
				wrapped_line_count(renderer, value, 399.0f);
			const std::int16_t leading_blank = section == 0 ? 0 : 1;
			if (skipped >= leading_blank + label_lines + value_lines)
			{
				skipped -= leading_blank + label_lines + value_lines;
				continue;
			}
			if (leading_blank != 0)
			{
				if (skipped > 0) --skipped;
				else ++body_lines;
			}
			body_lines += draw_wrapped(
				commands,
				renderer,
				label,
				35.0f,
				266.0f + body_lines * 11.0f,
				399.0f,
				skipped,
				static_cast<std::uint16_t>(6 - body_lines),
				renderer.shell.font_blue_palette,
				0xffffffff);
			if (skipped > label_lines) skipped -= label_lines;
			else skipped = 0;
			body_lines += draw_wrapped(
				commands,
				renderer,
				value,
				35.0f,
				266.0f + body_lines * 11.0f,
				399.0f,
				skipped,
				static_cast<std::uint16_t>(6 - body_lines),
				renderer.shell.font_blue_palette,
				0xffffffff);
			skipped = 0;
		}
		render::frontend_scissor(commands, 0, 0, 640, 480);

		shell.pilot_visible_rows = 0;
		float row_y = 140.0f;
		const std::uint8_t count = pilot_count(shell);
		for (std::uint8_t record = shell.pilot_viewport;
			record < count
				&& shell.pilot_visible_rows < 13
				&& row_y < 358.0f;
			++record)
		{
			const char* name =
				language_text(itac_language, pilot_record(shell, record).name);
			const std::uint8_t lines =
				wrapped_line_count(renderer, name, 126.0f);
			const std::uint8_t row = shell.pilot_visible_rows++;
			shell.pilot_row_y[row] = static_cast<std::int16_t>(row_y);
			shell.pilot_row_height[row] =
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
				record == shell.pilot_selection
					? renderer.shell.font_white_palette
					: renderer.shell.font_blue_palette,
				0xffffffff);
			row_y += lines * 11.0f + 6.0f;
		}
}

void itac_build_kills(
	const ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
		for (std::uint8_t row = 0; row < 5; ++row)
		{
			const std::uint8_t sorted = shell.kills_viewport + row;
			if (sorted >= shell.kills_count) break;
			const std::uint8_t record_index = shell.kills_order[sorted];
			const bool player = record_index == 0xff;
			const float y = 96.0f + row * 55.0f;
			const std::uint8_t shape = player
				? 1
				: kKills[record_index].shape;
			render::frontend_indexed_quad(
				commands,
				renderer.itac.kills[shape - 1],
				renderer.itac.kills_palettes[shape <= 5 ? 0 : 1],
				42.0f,
				y + 1.0f);

			char rank[8];
			std::snprintf(
				rank, sizeof(rank), "%u",
				static_cast<unsigned>(sorted + 1));
			render::frontend_itac_text(
				commands,
				renderer,
				rank,
				37.0f - itac_text_width(renderer, rank),
				y + 13.0f,
				renderer.shell.font_white_palette,
				0xffffffff,
				0.62f);

			const char* name = player
				? shell.kills_callsign
				: language_text(itac_language, kKills[record_index].name);
			render::frontend_itac_text(
				commands,
				renderer,
				name,
				143.0f,
				y,
				renderer.shell.font_white_palette,
				0xffffffff,
				0.62f);
			if (!player)
			{
				render::frontend_itac_text(
					commands,
					renderer,
					language_text(
						itac_language, kKills[record_index].squadron),
					143.0f,
					y + 15.0f,
					renderer.shell.font_blue_palette,
					0xffffffff,
					0.62f);
				render::frontend_itac_text(
					commands,
					renderer,
					language_text(itac_language, kKills[record_index].ship),
					390.0f,
					y + 15.0f,
					renderer.shell.font_blue_palette,
					0xffffffff,
					0.62f);
			}
			char kills[16];
			std::snprintf(
				kills, sizeof(kills), "%u",
				static_cast<unsigned>(shell.kills_values[sorted]));
			render::frontend_itac_text(
				commands,
				renderer,
				kills,
				591.0f - itac_text_width(renderer, kills),
				y + 16.0f,
				renderer.shell.font_blue_palette,
				0xffffffff,
				0.62f);
		}
}
}
