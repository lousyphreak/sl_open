#pragma once

#include "frontend/gui_render.hpp"
#include "render/frontend_renderer.hpp"

namespace sl_open::frontend::itac_render
{
inline float itac_text_width(
	const render::FrontendRenderer& renderer,
	const char* value)
{
	constexpr float scale = 0.62f;
	return gui::text_width(
		renderer.itac.glyphs,
		renderer.itac.glyph_count,
		value,
		scale);
}

inline std::uint8_t wrapped_line_count(
	const render::FrontendRenderer& renderer,
	const char* text,
	float width)
{
	if (text == nullptr || *text == '\0')
	{
		return 1;
	}
	std::uint8_t lines = 0;
	gui::TextWrapCursor cursor{text};
	char line[256];
	while (gui::next_wrapped_line(
		cursor,
		renderer.itac.glyphs,
		renderer.itac.glyph_count,
		0.62f,
		width,
		line))
	{
		++lines;
	}
	return lines == 0 ? 1 : lines;
}

inline std::uint16_t draw_wrapped(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	float width,
	std::int16_t skip_lines,
	std::uint16_t max_lines,
	bgfx::TextureHandle palette,
	std::uint32_t rgba)
{
	constexpr float scale = 0.62f;
	constexpr float line_height = 11.0f;
	char line[256];
	gui::TextWrapCursor cursor{text};
	std::int16_t logical_line = 0;
	std::uint16_t drawn = 0;
	while (drawn < max_lines
		&& gui::next_wrapped_line(
			cursor,
			renderer.itac.glyphs,
			renderer.itac.glyph_count,
			scale,
			width,
			line))
	{
		if (logical_line >= skip_lines && drawn < max_lines)
		{
			render::frontend_itac_text(
				commands,
				renderer,
				line,
				x,
				y + drawn * line_height,
				palette,
				rgba,
				scale);
			++drawn;
		}
		++logical_line;
	}
	return drawn;
}

inline float text_width(
	const render::FrontendRenderer& renderer,
	const char* value,
	float scale)
{
	return gui::text_width(
		renderer.shell.glyphs,
		renderer.shell.glyph_count,
		value,
		scale);
}

inline void update_scroll_limit(
	std::int16_t& scroll,
	std::int16_t& limit,
	std::uint16_t total_lines,
	std::uint16_t visible_lines)
{
	limit = total_lines > visible_lines
		? static_cast<std::int16_t>(total_lines - visible_lines)
		: 0;
	if (scroll > limit)
	{
		scroll = limit;
	}
}
}
