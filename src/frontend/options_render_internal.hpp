#pragma once

#include "frontend/gui_render.hpp"
#include "render/frontend_renderer.hpp"

#include <cstring>

namespace sl_open::frontend::options_render
{
inline void draw_label(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	bool selected,
	float scale = 1.0f)
{
	render::frontend_text(
		commands,
		renderer,
		text,
		x,
		y,
		selected ? renderer.shell.font_blue_palette : renderer.shell.font_gold_palette,
		0xffffffff,
		scale);
}

inline void draw_aligned_label(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	int alignment,
	bgfx::TextureHandle palette,
	float scale)
{
	const float width = gui::text_width(
		renderer.shell.glyphs,
		renderer.shell.glyph_count,
		text,
		scale);
	x = gui::aligned_x(
		x, width, static_cast<gui::TextAlign>(alignment));
	render::frontend_text(
		commands, renderer, text, x, y, palette, 0xffffffff, scale);
}

inline void draw_wrapped_label(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	float maximum_width,
	float scale)
{
	gui::TextWrapCursor cursor{text};
	char line[128];
	while (gui::next_wrapped_line(
		cursor,
		renderer.shell.glyphs,
		renderer.shell.glyph_count,
		scale,
		maximum_width,
		line))
	{
		draw_aligned_label(
			commands,
			renderer,
			line,
			x,
			y,
			1,
			renderer.shell.font_gold_palette,
			scale);
		y += 14.0f;
	}
}
}
