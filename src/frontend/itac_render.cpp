#include "frontend/itac_shell.hpp"

#include "frontend/gui_render.hpp"
#include "frontend/itac_pages.hpp"
#include "frontend/itac_render_internal.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

namespace sl_open::frontend
{

void itac_shell_build(
	ItacShell& shell,
	const LanguageTable& language,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	render::frontend_rgba_quad(
		commands,
		renderer.itac.backgrounds[shell.active_tab],
		0.0f,
		0.0f,
		640.0f,
		480.0f);
	switch (shell.active_tab)
	{
	case 0:
		itac_build_archive(
			shell, language, itac_language, renderer, commands);
		break;
	case 1:
		itac_build_news(shell, itac_language, renderer, commands);
		break;
	case 2:
		itac_build_media(shell, itac_language, renderer, commands);
		break;
	case 3:
		itac_build_fighters(shell, itac_language, renderer, commands);
		break;
	case 4:
		itac_build_capitals(shell, itac_language, renderer, commands);
		break;
	case 5:
		itac_build_squads(shell, itac_language, renderer, commands);
		break;
	case 6:
		itac_build_personnel(shell, itac_language, renderer, commands);
		break;
	case 7:
		itac_build_kills(shell, itac_language, renderer, commands);
		break;
	default:
		break;
	}
	if (shell.hovered_tab >= 0
		&& now - shell.hover_started_at >= 500)
	{
		const char* label = language_text(
			itac_language,
			0x712 + static_cast<std::uint8_t>(shell.hovered_tab));
		constexpr float scale = 0.62f;
		const float width =
			itac_render::text_width(renderer, label, scale);
		float x = shell.pointer_x + 14.0f;
		float y = shell.pointer_y - 18.0f;
		if (x + width + 8.0f > 636.0f)
		{
			x = shell.pointer_x - width - 14.0f;
		}
		if (y < 4.0f)
		{
			y = shell.pointer_y + 18.0f;
		}
		render::frontend_rgba_quad(
			commands,
			renderer.white,
			x - 4.0f,
			y - 2.0f,
			width + 8.0f,
			12.0f,
			0x101824e0);
		render::frontend_text(
			commands,
			renderer,
			label,
			x,
			y,
			renderer.shell.font_white_palette,
			0xffffffff,
			scale);
	}
	gui::animated_cursor(
		commands,
		renderer.shell.cursor,
		renderer.shell.cursor_palette,
		shell.pointer_x,
		shell.pointer_y,
		now,
		60);
}
}
