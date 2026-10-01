#include "frontend/quit_dialog.hpp"

#include "frontend/gui.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

namespace sl_open::frontend
{
void quit_dialog_build(
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::int32_t hovered)
{
	render::frontend_indexed_quad(
		commands,
		renderer.shell.quit_background,
		renderer.shell.quit_background_palette,
		114.0f,
		177.0f);
	for (std::uint32_t index = 0; index < 2; ++index)
	{
		render::frontend_indexed_quad(
			commands,
			renderer.shell.quit_button,
			renderer.shell.quit_button_palette,
			kQuitDialogRegions[index].x,
			kQuitDialogRegions[index].y);
	}
	if (hovered >= 0 && hovered < 2)
	{
		render::frontend_indexed_quad(
			commands,
			renderer.shell.quit_button_hover,
			renderer.shell.quit_button_palette,
			kQuitDialogRegions[hovered].x,
			kQuitDialogRegions[hovered].y);
	}

	constexpr float scale = 0.62f;
	const char* message = language_text(language, 0x374);
	const char* yes = language_text(language, 0x28f);
	const char* no = language_text(language, 0x290);
	render::frontend_text(
		commands,
		renderer,
		message,
		gui::aligned_x(
			320.0f,
			gui::text_width(
				renderer.shell.glyphs,
				renderer.shell.glyph_count,
				message,
				scale),
			gui::TextAlign::center),
		208.0f,
		renderer.shell.font_gold_palette,
		0xffffffff,
		scale);
	render::frontend_text(
		commands,
		renderer,
		yes,
		gui::aligned_x(
			282.0f,
			gui::text_width(
				renderer.shell.glyphs,
				renderer.shell.glyph_count,
				yes,
				scale),
			gui::TextAlign::right),
		263.0f,
		renderer.shell.font_gold_palette,
		0xffffffff,
		scale);
	render::frontend_text(
		commands,
		renderer,
		no,
		354.0f,
		263.0f,
		renderer.shell.font_gold_palette,
		0xffffffff,
		scale);
}
}
