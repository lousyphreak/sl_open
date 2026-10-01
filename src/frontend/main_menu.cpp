#include "frontend/main_menu.hpp"

#include "frontend/gui_render.hpp"
#include "frontend/quit_dialog.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

#include <algorithm>

namespace sl_open::frontend
{
namespace
{
// These are the retail hit rectangles. Their edges are deliberately excluded.
constexpr gui::Rect kRegions[] = {
	{27, 123, 184, 290},
	{203, 125, 184, 290},
	{421, 165, 184, 290},
	{332, 441, 20, 15},
	{300, 441, 20, 15},
};

void draw_large_label(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const LanguageTable& language,
	std::uint32_t first,
	std::uint32_t second,
	float x,
	float y,
	bgfx::TextureHandle palette,
	std::uint32_t rgba)
{
	render::frontend_text(
		commands,
		renderer,
		language_text(language, first),
		x,
		y,
		palette,
		rgba);
	render::frontend_text(
		commands,
		renderer,
		language_text(language, second),
		x + 8.0f,
		y + 23.0f,
		palette,
		rgba);
}
}

void main_menu_set_pointer(MainMenu& menu, float x, float y, bool inside)
{
	menu.pointer_x = x;
	menu.pointer_y = y;
	menu.hovered = -1;
	if (!inside)
	{
		return;
	}
	if (menu.quit_confirmation)
	{
		for (std::uint32_t index = 0; index < 2; ++index)
		{
			if (gui::hit_open(kQuitDialogRegions[index], x, y))
			{
				menu.hovered = static_cast<std::int32_t>(index);
				return;
			}
		}
		return;
	}
	for (std::uint32_t index = 0; index < 5; ++index)
	{
		if (gui::hit_open(kRegions[index], x, y))
		{
			menu.hovered = static_cast<std::int32_t>(index);
			return;
		}
	}
}

void main_menu_build(
	const MainMenu& menu,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	gui::begin_screen(commands);

	const std::uint64_t elapsed =
		now > menu.entered_at ? now - menu.entered_at : 0;
	render::frontend_rgba_quad(
		commands,
		renderer.shell.splash,
		0.0f,
		0.0f,
		render::kFrontendWidth,
		render::kFrontendHeight,
		0xffffffff);

	const std::uint32_t gold = 0xffffffff;
	draw_large_label(
		commands, renderer, language, 0xbd, 0xbf, 83.0f, 338.0f,
		renderer.shell.font_gold_palette, gold);
	draw_large_label(
		commands, renderer, language, 0x5b4, 0x5b5, 290.0f, 338.0f,
		renderer.shell.font_gold_palette, gold);
	draw_large_label(
		commands, renderer, language, 0xc0, 0xc1, 500.0f, 338.0f,
		renderer.shell.font_gold_palette, gold);
	render::frontend_text(
		commands,
		renderer,
		language_text(language, 0x288),
		195.0f,
		440.0f,
		renderer.shell.font_gold_palette,
		gold,
		0.62f);
	render::frontend_text(
		commands,
		renderer,
		language_text(language, 0xbc),
		356.0f,
		440.0f,
		renderer.shell.font_gold_palette,
		gold,
		0.62f);
	render::frontend_indexed_quad(
		commands,
		renderer.shell.bottom_icon,
		renderer.shell.icon_palette,
		300.0f,
		441.0f);
	render::frontend_indexed_quad(
		commands,
		renderer.shell.bottom_icon,
		renderer.shell.icon_palette,
		332.0f,
		441.0f);

	if (!menu.quit_confirmation && menu.hovered >= 0 && menu.hovered < 3)
	{
		const std::uint32_t selected = static_cast<std::uint32_t>(menu.hovered);
		const float highlight_x[] = {27.0f, 203.0f, 421.0f};
		const float highlight_y[] = {123.0f, 125.0f, 165.0f};
		render::frontend_indexed_quad(
			commands,
			renderer.shell.highlights[selected],
			renderer.shell.highlight_palette,
			highlight_x[selected],
			highlight_y[selected]);

		if (selected == 0)
		{
			draw_large_label(
				commands, renderer, language, 0xbd, 0xbf, 83.0f, 338.0f,
				renderer.shell.font_blue_palette, gold);
		}
		else if (selected == 1)
		{
			draw_large_label(
				commands, renderer, language, 0x5b4, 0x5b5, 290.0f, 338.0f,
				renderer.shell.font_blue_palette, gold);
		}
		else
		{
			draw_large_label(
				commands, renderer, language, 0xc0, 0xc1, 500.0f, 338.0f,
				renderer.shell.font_blue_palette, gold);
		}
	}
	else if (!menu.quit_confirmation
		&& (menu.hovered == 3 || menu.hovered == 4))
	{
		const float x = menu.hovered == 3 ? 332.0f : 300.0f;
		render::frontend_indexed_quad(
			commands,
			renderer.shell.bottom_icon_hover,
			renderer.shell.icon_palette,
			x,
			441.0f);
	}

	if (menu.quit_confirmation)
	{
		quit_dialog_build(language, renderer, commands, menu.hovered);
	}

	gui::animated_cursor(
		commands,
		renderer.shell.cursor,
		renderer.shell.cursor_palette,
		menu.pointer_x,
		menu.pointer_y,
		elapsed,
		40);
}
}
