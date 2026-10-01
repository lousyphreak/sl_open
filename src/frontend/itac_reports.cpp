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

namespace
{
std::uint8_t automatic_medal(std::uint8_t mission)
{
	switch (mission)
	{
	case 6: return 1;
	case 11: return 2;
	case 16: return 3;
	case 21: return 4;
	case 23: return 5;
	case 27: return 6;
	default: return 0;
	}
}

std::uint8_t automatic_bar(std::uint8_t mission)
{
	switch (mission)
	{
	case 7: return 1;
	case 11: return 2;
	case 19: return 3;
	case 21: return 4;
	case 25: return 5;
	default: return 0;
	}
}

std::uint8_t progression_tier(std::uint8_t mission)
{
	switch (mission)
	{
	case 11: return 1;
	case 19: return 2;
	case 21: return 3;
	default: return 0;
	}
}
}

void itac_build_archive(
	ItacShell& shell,
	const LanguageTable& language,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	if (shell.archive_count == 0)
	{
		shell.archive_visible_rows = 0;
		return;
	}
		const std::int8_t grade = shell.archive_grades[
			shell.archive_selection];
		const std::uint8_t matrix = grade < 0
			? 0
			: static_cast<std::uint8_t>(4 - (grade > 4 ? 4 : grade));
		const std::uint16_t* debrief =
			kArchiveDebriefs[matrix][shell.archive_selection];
		std::uint16_t body[12]{};
		std::uint8_t body_count = 0;
		const std::uint16_t retry =
			shell.archive_retry_history[shell.archive_selection];
		if (retry != 0 && retry <= 3)
		{
			// The temporary mission boundary supplies the retail default
			// (objectives incomplete) variant of the ejection result.
			body[body_count++] = 0x721;
			constexpr std::uint16_t ejection_warning[] = {
				0, 227, 228, 229};
			body[body_count++] = ejection_warning[retry];
		}
		else
		{
			for (std::uint8_t paragraph = 1;
				paragraph < 9 && debrief[paragraph] != 9;
				++paragraph)
			{
				body[body_count++] = debrief[paragraph];
			}
			const std::uint8_t mission =
				shell.archive_mission_ids[shell.archive_selection];
			const std::uint8_t medal = automatic_medal(mission);
			if (medal != 0 && grade == 4)
			{
				body[body_count++] =
					static_cast<std::uint16_t>(204 + medal);
			}
			const std::uint8_t best_rank =
				shell.archive_best_ranks[shell.archive_selection];
			if (best_rank != 0 && best_rank <= 8)
			{
				constexpr std::uint16_t promotion_text[] = {
					230, 230, 231, 232, 233, 234, 235, 236, 237};
				body[body_count++] = promotion_text[best_rank];
			}
			const std::uint8_t bar = automatic_bar(mission);
			if (bar != 0)
			{
				body[body_count++] =
					static_cast<std::uint16_t>(210 + bar);
			}
			const std::uint8_t progression = progression_tier(mission);
			if (progression != 0)
			{
				body[body_count++] =
					static_cast<std::uint16_t>(237 + progression);
			}
		}

		std::uint16_t total_body_lines = 0;
		for (std::uint8_t paragraph = 0;
			paragraph < body_count;
			++paragraph)
		{
			total_body_lines = static_cast<std::uint16_t>(
				total_body_lines
				+ wrapped_line_count(
					renderer,
					language_text(itac_language, body[paragraph]),
					393.0f)
				+ (paragraph != 0 ? 1 : 0));
		}
		update_scroll_limit(
			shell.archive_body_scroll,
			shell.archive_body_scroll_max,
			total_body_lines,
			20);
		render::frontend_itac_text(
			commands,
			renderer,
			language_text(itac_language, debrief[0]),
			35.0f,
			82.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);
		render::frontend_scissor(commands, 34, 103, 399, 229);
		std::uint16_t line = 0;
		std::int16_t skipped = shell.archive_body_scroll;
		for (std::uint8_t paragraph = 0;
			paragraph < body_count && line < 20;
			++paragraph)
		{
			const char* text =
				language_text(itac_language, body[paragraph]);
			const std::uint8_t paragraph_lines =
				wrapped_line_count(renderer, text, 393.0f);
			if (skipped >= paragraph_lines + (paragraph != 0 ? 1 : 0))
			{
				skipped -= paragraph_lines + (paragraph != 0 ? 1 : 0);
				continue;
			}
			if (paragraph != 0 && line < 20)
			{
				if (skipped > 0) --skipped;
				else ++line;
			}
			line += draw_wrapped(
				commands,
				renderer,
				text,
				35.0f,
				103.0f + line * 11.0f,
				393.0f,
				skipped,
				static_cast<std::uint16_t>(20 - line),
				renderer.shell.font_blue_palette,
				0xffffffff);
			skipped = 0;
		}
		render::frontend_scissor(commands, 0, 0, 640, 480);

		const std::uint16_t labels[] = {138, 139};
		const std::uint16_t general_labels[] = {232, 233};
		const std::uint16_t values[] = {
			shell.archive_mission_kills[shell.archive_selection],
			shell.archive_overall_kills,
			shell.archive_rank,
			shell.archive_level};
		for (std::uint8_t field = 0; field < 4; ++field)
		{
			const float y = 346.0f + field * 16.0f;
			render::frontend_itac_text(
				commands,
				renderer,
				field < 2
					? language_text(itac_language, labels[field])
					: language_text(language, general_labels[field - 2]),
				36.0f,
				y,
				renderer.shell.font_white_palette,
				0xffffffff,
				0.62f);
			char value[16];
			std::snprintf(
				value, sizeof(value), "%u",
				static_cast<unsigned>(values[field]));
			render::frontend_itac_text(
				commands,
				renderer,
				value,
				214.0f - itac_text_width(renderer, value),
				y,
				renderer.shell.font_blue_palette,
				0xffffffff,
				0.62f);
		}

		shell.archive_visible_rows = 0;
		float row_y = 104.0f;
		for (std::uint8_t record = shell.archive_viewport;
			record < shell.archive_count
				&& shell.archive_visible_rows < 13
				&& row_y < 363.0f;
			++record)
		{
			char mission[64];
			std::snprintf(
				mission,
				sizeof(mission),
				"%s %u",
				language_text(itac_language, 142),
				static_cast<unsigned>(record + 1));
			const std::uint8_t row = shell.archive_visible_rows++;
			shell.archive_row_y[row] = static_cast<std::int16_t>(row_y);
			shell.archive_row_height[row] = 17;
			render::frontend_itac_text(
				commands,
				renderer,
				mission,
				474.0f,
				row_y,
				record == shell.archive_selection
					? renderer.shell.font_white_palette
					: renderer.shell.font_blue_palette,
				0xffffffff,
				0.62f);
			row_y += 17.0f;
		}
}

void itac_build_news(
	ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
		const NewsRecord& selected = kNews[shell.news_selection];
		std::uint16_t total_body_lines = 0;
		for (std::uint8_t paragraph = 0;
			paragraph < 5 && selected.body[paragraph] >= 0;
			++paragraph)
		{
			total_body_lines = static_cast<std::uint16_t>(
				total_body_lines
				+ wrapped_line_count(
					renderer,
					language_text(
						itac_language,
						static_cast<std::uint16_t>(
							selected.body[paragraph])),
					252.0f)
				+ (paragraph != 0 ? 1 : 0));
		}
		update_scroll_limit(
			shell.news_body_scroll,
			shell.news_body_scroll_max,
			total_body_lines,
			19);
		render::frontend_indexed_quad(
			commands,
			renderer.itac.news[selected.shape - 1],
			renderer.itac.news_palettes[
				selected.shape < 13 ? 0 : (selected.shape < 26 ? 1 : 2)],
			302.0f,
			100.0f);
		char headline[128];
		const char* source = language_text(
			itac_language, selected.headline);
		std::size_t index = 0;
		for (; source[index] != '\0' && index + 1 < sizeof(headline); ++index)
		{
			headline[index] = static_cast<char>(
				std::toupper(static_cast<unsigned char>(source[index])));
		}
		headline[index] = '\0';
		render::frontend_itac_text(
			commands,
			renderer,
			headline,
			36.0f,
			79.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);

		render::frontend_scissor(commands, 34, 101, 258, 210);
		std::int16_t skipped = shell.news_body_scroll;
		std::uint16_t body_lines = 0;
		for (std::uint8_t paragraph = 0;
			paragraph < 5 && selected.body[paragraph] >= 0
				&& body_lines < 19;
			++paragraph)
		{
			const char* text = language_text(
				itac_language,
				static_cast<std::uint16_t>(selected.body[paragraph]));
			const std::uint8_t paragraph_lines =
				wrapped_line_count(renderer, text, 252.0f);
			if (skipped >= paragraph_lines + (paragraph != 0 ? 1 : 0))
			{
				skipped -= paragraph_lines + (paragraph != 0 ? 1 : 0);
				continue;
			}
			if (paragraph != 0 && body_lines < 19)
			{
				if (skipped > 0) --skipped;
				else ++body_lines;
			}
			body_lines += draw_wrapped(
				commands,
				renderer,
				text,
				36.0f,
				103.0f + body_lines * 11.0f,
				252.0f,
				skipped,
				static_cast<std::uint16_t>(19 - body_lines),
				renderer.shell.font_blue_palette,
				0xffffffff);
			skipped = 0;
		}
		render::frontend_scissor(commands, 0, 0, 640, 480);

		shell.news_visible_rows = 0;
		float row_y = 104.0f;
		for (std::uint8_t record = shell.news_viewport;
			record < shell.news_count
				&& shell.news_visible_rows < 12
				&& row_y < 357.0f;
			++record)
		{
			const char* title = language_text(
				itac_language, kNews[record].headline);
			const std::uint8_t lines =
				wrapped_line_count(renderer, title, 134.0f);
			const std::uint8_t row = shell.news_visible_rows++;
			shell.news_row_y[row] = static_cast<std::int16_t>(row_y);
			shell.news_row_height[row] =
				static_cast<std::int16_t>(lines * 11);
			draw_wrapped(
				commands,
				renderer,
				title,
				474.0f,
				row_y,
				134.0f,
				0,
				lines,
				record == shell.news_selection
					? renderer.shell.font_white_palette
					: renderer.shell.font_blue_palette,
				0xffffffff);
			row_y += lines * 11.0f;
		}
}

void itac_build_media(
	ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
		const MovieRecord& selected = kMovies[shell.movie_selection];
		const std::uint16_t total_body_lines =
			static_cast<std::uint16_t>(
				wrapped_line_count(
					renderer,
					language_text(itac_language, selected.body[0]),
					252.0f)
				+ 1
				+ wrapped_line_count(
					renderer,
					language_text(itac_language, selected.body[1]),
					252.0f));
		update_scroll_limit(
			shell.movie_body_scroll,
			shell.movie_body_scroll_max,
			total_body_lines,
			14);
		render::frontend_indexed_quad(
			commands,
			renderer.itac.video[6],
			renderer.itac.video_thumbnail_palette,
			297.0f,
			97.0f);
		render::frontend_indexed_region(
			commands,
			renderer.itac.video[selected.shape - 29],
			renderer.itac.video_thumbnail_palette,
			317.0f,
			96.0f,
			0,
			64,
			116,
			26);
		render::frontend_indexed_region(
			commands,
			renderer.itac.video[selected.shape - 29],
			renderer.itac.video_thumbnail_palette,
			317.0f,
			130.0f,
			0,
			0,
			116,
			91);
		render::frontend_indexed_region(
			commands,
			renderer.itac.video[selected.shape - 29],
			renderer.itac.video_thumbnail_palette,
			317.0f,
			228.0f,
			0,
			0,
			116,
			26);
		char heading[128];
		const char* source =
			language_text(itac_language, selected.title);
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
			36.0f,
			79.0f,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);

		render::frontend_scissor(commands, 34, 101, 258, 156);
		std::int16_t skipped = shell.movie_body_scroll;
		std::uint16_t body_lines = 0;
		for (std::uint8_t paragraph = 0;
			paragraph < 2 && body_lines < 14;
			++paragraph)
		{
			const char* text =
				language_text(itac_language, selected.body[paragraph]);
			const std::uint8_t paragraph_lines =
				wrapped_line_count(renderer, text, 252.0f);
			if (skipped >= paragraph_lines + (paragraph != 0 ? 1 : 0))
			{
				skipped -= paragraph_lines + (paragraph != 0 ? 1 : 0);
				continue;
			}
			if (paragraph != 0)
			{
				if (skipped > 0) --skipped;
				else ++body_lines;
			}
			body_lines += draw_wrapped(
				commands,
				renderer,
				text,
				36.0f,
				103.0f + body_lines * 11.0f,
				252.0f,
				skipped,
				static_cast<std::uint16_t>(14 - body_lines),
				renderer.shell.font_blue_palette,
				0xffffffff);
			skipped = 0;
		}
		render::frontend_scissor(commands, 0, 0, 640, 480);

		shell.movie_visible_rows = 0;
		float row_y = 105.0f;
		for (std::uint8_t record = 0;
			record < shell.movie_count && row_y < 357.0f;
			++record)
		{
			const char* title =
				language_text(itac_language, kMovies[record].title);
			const std::uint8_t lines =
				wrapped_line_count(renderer, title, 134.0f);
			shell.movie_row_y[record] = static_cast<std::int16_t>(row_y);
			shell.movie_row_height[record] =
				static_cast<std::int16_t>(lines * 11);
			++shell.movie_visible_rows;
			draw_wrapped(
				commands,
				renderer,
				title,
				474.0f,
				row_y,
				134.0f,
				0,
				lines,
				record == shell.movie_selection
					? renderer.shell.font_white_palette
					: renderer.shell.font_blue_palette,
				0xffffffff);
			row_y += lines * 11.0f;
		}
}
}
