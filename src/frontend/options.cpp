#include "frontend/options.hpp"

#include "frontend/gui_render.hpp"
#include "frontend/options_audio.hpp"
#include "frontend/options_controls.hpp"
#include "frontend/options_render_internal.hpp"
#include "frontend/options_video.hpp"
#include "frontend/quit_dialog.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

namespace sl_open::frontend
{
namespace
{
using namespace options_render;

struct Region
{
	std::int16_t x;
	std::int16_t y;
	std::int16_t width;
	std::int16_t height;
};

constexpr Region kHubRegions[] = {
	{30, 165, 152, 127},
	{219, 165, 152, 127},
	{408, 165, 152, 127},
	{220, 434, 100, 27},
	{320, 434, 90, 27},
	{165, 414, 155, 27},
};

constexpr Region kInGameRegions[] = {
	{140, 121, 131, 112},
	{338, 121, 130, 109},
	{65, 276, 130, 109},
	{250, 272, 130, 111},
	{436, 272, 131, 110},
	{199, 422, 120, 15},
	{199, 443, 120, 15},
	{329, 443, 100, 15},
	{329, 422, 100, 15},
};

constexpr Region kAboutRegion = {316, 334, 25, 16};

void build_modal(
	const OptionsMenu& menu,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands)
{
	if (menu.modal == OptionsModal::about)
	{
		render::frontend_indexed_quad(
			commands,
			renderer.shell.about_panel,
			renderer.shell.about_panel_palette,
			114.0f,
			177.0f);
		render::frontend_indexed_quad(
			commands,
			menu.hovered == 0
				? renderer.shell.about_button_hover
				: renderer.shell.about_button,
			renderer.shell.about_button_palette,
			316.0f,
			334.0f);
		draw_aligned_label(
			commands,
			renderer,
			language_text(language, 0x10c),
			320.0f,
			180.0f,
			1,
			renderer.shell.font_gold_palette,
			0.62f);
		draw_aligned_label(
			commands,
			renderer,
			"PID - ",
			320.0f,
			194.0f,
			1,
			renderer.shell.font_gold_palette,
			0.48f);
		draw_wrapped_label(
			commands,
			renderer,
			language_text(language, 0x315),
			320.0f,
			208.0f,
			400.0f,
			0.42f);
		draw_aligned_label(
			commands,
			renderer,
			language_text(language, 0x316),
			308.0f,
			329.0f,
			2,
			menu.hovered == 0
				? renderer.shell.font_white_palette
				: renderer.shell.font_gold_palette,
			0.58f);
	}
	else if (menu.modal == OptionsModal::quit_confirmation)
	{
		quit_dialog_build(language, renderer, commands, menu.hovered);
	}
}



}

void options_enter_page(
	OptionsMenu& menu,
	OptionsPage page,
	const Config& config,
	std::uint64_t now)
{
	menu.page = page;
	menu.modal = OptionsModal::none;
	menu.hovered = -1;
	menu.entered_at = now;
	if (page == OptionsPage::audio)
	{
		options_audio_enter(menu, config);
	}
	else if (page == OptionsPage::video)
	{
		options_video_enter(menu, config);
	}
	else if (page == OptionsPage::controls)
	{
		options_controls_enter(menu, config);
	}
}

void options_set_pointer(OptionsMenu& menu, float x, float y, bool inside)
{
	menu.pointer_x = x;
	menu.pointer_y = y;
	menu.hovered = -1;
	if (!inside)
	{
		return;
	}
	if (menu.modal == OptionsModal::about)
	{
		if (gui::hit_open(kAboutRegion, x, y))
		{
			menu.hovered = 0;
		}
		return;
	}
	if (menu.modal == OptionsModal::quit_confirmation)
	{
		for (std::uint32_t index = 0; index < 2; ++index)
		{
			const gui::Rect& region = kQuitDialogRegions[index];
			if (gui::hit_open(region, x, y))
			{
				menu.hovered = static_cast<std::int32_t>(index);
				return;
			}
		}
		return;
	}
	const Region* regions = kHubRegions;
	std::uint32_t count = 6;
	if (menu.in_game && menu.page == OptionsPage::hub)
	{
		regions = kInGameRegions;
		count = 9;
	}
	if (menu.page == OptionsPage::audio)
	{
		options_audio_set_pointer(menu, x, y);
		return;
	}
	else if (menu.page == OptionsPage::video)
	{
		options_video_set_pointer(menu, x, y);
		return;
	}
	else if (menu.page == OptionsPage::controls)
	{
		options_controls_set_pointer(menu, x, y);
		return;
	}
	for (std::uint32_t index = 0; index < count; ++index)
	{
		if (gui::hit_open(regions[index], x, y))
		{
			menu.hovered = static_cast<std::int32_t>(index);
			return;
		}
	}
}

OptionsSelection options_select(
	OptionsMenu& menu,
	Config& config,
	const DisplayModes& modes,
	bool hrtf_supported,
	bool joystick_available)
{
	if (menu.modal == OptionsModal::about)
	{
		if (menu.hovered == 0)
		{
			menu.modal = OptionsModal::none;
			menu.hovered = -1;
			return OptionsSelection::changed;
		}
		return OptionsSelection::none;
	}
	if (menu.modal == OptionsModal::quit_confirmation)
	{
		if (menu.hovered == 0)
		{
			return OptionsSelection::quit;
		}
		if (menu.hovered == 1)
		{
			menu.modal = OptionsModal::none;
			menu.hovered = -1;
			return OptionsSelection::changed;
		}
		return OptionsSelection::none;
	}
	if (menu.page == OptionsPage::audio)
	{
		return options_audio_select(menu, config, hrtf_supported);
	}
	if (menu.page == OptionsPage::video)
	{
		return options_video_select(menu, config, modes);
	}
	if (menu.page == OptionsPage::controls)
	{
		return options_controls_select(menu, config, joystick_available);
	}
	if (menu.in_game)
	{
		switch (menu.hovered)
		{
		case 0: return OptionsSelection::load;
		case 1: return OptionsSelection::save;
		case 2: return OptionsSelection::audio;
		case 3: return OptionsSelection::controls;
		case 4: return OptionsSelection::video;
		case 5: return OptionsSelection::back;
		case 6: return OptionsSelection::main_menu;
		case 7:
			menu.modal = OptionsModal::quit_confirmation;
			menu.hovered = -1;
			return OptionsSelection::changed;
		case 8:
			menu.modal = OptionsModal::about;
			menu.hovered = -1;
			return OptionsSelection::changed;
		default: return OptionsSelection::none;
		}
	}
	switch (menu.hovered)
	{
	case 0: return OptionsSelection::audio;
	case 1: return OptionsSelection::controls;
	case 2: return OptionsSelection::video;
	case 3: return OptionsSelection::main_menu;
	case 4:
		menu.modal = OptionsModal::quit_confirmation;
		menu.hovered = -1;
		return OptionsSelection::changed;
	case 5:
		menu.modal = OptionsModal::about;
		menu.hovered = -1;
		return OptionsSelection::changed;
	default: return OptionsSelection::none;
	}
}

bool options_drag(OptionsMenu& menu, Config& config)
{
	if (menu.page == OptionsPage::audio)
	{
		return options_audio_drag(menu, config);
	}
	if (menu.page == OptionsPage::video)
	{
		return options_video_drag(menu, config);
	}
	return false;
}

void options_build(
	const OptionsMenu& menu,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	const Config& config,
	bool hrtf_supported,
	bool joystick_available,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	gui::begin_screen(commands);
	const bool in_game_hub =
		menu.in_game && menu.page == OptionsPage::hub;
	render::frontend_rgba_quad(
		commands,
		in_game_hub
			? renderer.shell.in_game_options_background
			: (menu.page == OptionsPage::hub
				? renderer.shell.options_background
				: (menu.in_game
					? renderer.shell.in_game_options_detail_background
					: renderer.shell.options_detail_background)),
		0.0f,
		0.0f,
		render::kFrontendWidth,
		render::kFrontendHeight);

	if (in_game_hub)
	{
		static constexpr float kHighlightX[] =
			{107.0f, 308.0f, 35.0f, 230.0f, 406.0f};
		static constexpr float kHighlightY[] =
			{115.0f, 114.0f, 250.0f, 255.0f, 252.0f};
		static constexpr std::uint16_t kLabels[] = {
			0x149, 0x3b5, 0x109, 0x10a, 0x10b,
			0xf7, 0xbb, 0xbc, 0x10c,
		};
		static constexpr float kLabelX[] = {
			208.0f, 408.0f, 130.0f, 322.0f, 506.0f,
			299.0f, 299.0f, 354.0f, 354.0f,
		};
		static constexpr float kLabelY[] = {
			233.0f, 233.0f, 385.0f, 385.0f, 385.0f,
			421.0f, 442.0f, 442.0f, 421.0f,
		};
		for (std::uint32_t index = 0; index < 5; ++index)
		{
			if (menu.modal == OptionsModal::none
				&& menu.hovered == static_cast<std::int32_t>(index))
			{
				render::frontend_indexed_quad(
					commands,
					renderer.shell.in_game_options_highlights[index],
					renderer.shell.in_game_options_highlight_palette,
					kHighlightX[index],
					kHighlightY[index]);
			}
			draw_aligned_label(
				commands,
				renderer,
				language_text(language, kLabels[index]),
				kLabelX[index],
				kLabelY[index],
				1,
				menu.modal == OptionsModal::none
					&& menu.hovered == static_cast<std::int32_t>(index)
					? renderer.shell.font_white_palette
					: renderer.shell.font_gold_palette,
				0.7f);
		}
		for (std::uint32_t index = 5; index < 9; ++index)
		{
			const float icon_x = index < 7 ? 299.0f : 329.0f;
			const float icon_y = (index == 5 || index == 8)
				? 422.0f
				: 443.0f;
			const bool selected =
				menu.modal == OptionsModal::none
				&& menu.hovered == static_cast<std::int32_t>(index);
			render::frontend_indexed_quad(
				commands,
				selected
					? renderer.shell.in_game_options_bottom_icon_hover
					: renderer.shell.in_game_options_bottom_icon,
				renderer.shell.in_game_options_icon_palette,
				icon_x,
				icon_y);
			draw_aligned_label(
				commands,
				renderer,
				language_text(language, kLabels[index]),
				kLabelX[index],
				kLabelY[index],
				index < 7 ? 2 : 0,
				selected
					? renderer.shell.font_white_palette
					: renderer.shell.font_gold_palette,
				0.58f);
		}
	}
	else if (menu.page == OptionsPage::hub)
	{
		draw_aligned_label(
			commands,
			renderer,
			"SELECT AN OPTION",
			320.0f,
			95.0f,
			1,
			renderer.shell.font_gold_palette,
			1.0f);
		render::frontend_indexed_quad(
			commands,
			renderer.shell.options_bottom_icon,
			renderer.shell.options_icon_palette,
			292.0f,
			421.0f);
		render::frontend_indexed_quad(
			commands,
			renderer.shell.options_bottom_icon,
			renderer.shell.options_icon_palette,
			292.0f,
			441.0f);
		render::frontend_indexed_quad(
			commands,
			renderer.shell.options_bottom_icon,
			renderer.shell.options_icon_palette,
			324.0f,
			441.0f);

		const char* panels[] = {"AUDIO", "CONTROL DEVICES", "VIDEO"};
		const float highlight_x[] = {35.0f, 202.0f, 392.0f};
		const float highlight_y[] = {155.0f, 160.0f, 161.0f};
		const float label_x[] = {133.0f, 320.0f, 511.0f};
		for (std::uint32_t index = 0; index < 3; ++index)
		{
			const bool selected =
				menu.modal == OptionsModal::none
				&& menu.hovered == static_cast<std::int32_t>(index);
			if (selected)
			{
				render::frontend_indexed_quad(
					commands,
					renderer.shell.options_highlights[index],
					renderer.shell.options_highlight_palette,
					highlight_x[index],
					highlight_y[index]);
			}
			draw_aligned_label(
				commands,
				renderer,
				panels[index],
				label_x[index],
				319.0f,
				1,
				selected
					? renderer.shell.font_white_palette
					: renderer.shell.font_gold_palette,
				1.0f);
		}

		const bool main_hovered =
			menu.modal == OptionsModal::none && menu.hovered == 3;
		const bool quit_hovered =
			menu.modal == OptionsModal::none && menu.hovered == 4;
		const bool about_hovered =
			menu.modal == OptionsModal::none && menu.hovered == 5;
		if (main_hovered || quit_hovered || about_hovered)
		{
			const float x = quit_hovered ? 324.0f : 292.0f;
			const float y = about_hovered ? 421.0f : 441.0f;
			render::frontend_indexed_quad(
				commands,
				renderer.shell.options_bottom_icon_hover,
				renderer.shell.options_icon_palette,
				x,
				y);
		}
		draw_aligned_label(
			commands,
			renderer,
			"ABOUT STARLANCER",
			288.0f,
			420.0f,
			2,
			about_hovered
				? renderer.shell.font_white_palette
				: renderer.shell.font_gold_palette,
			0.58f);
		draw_aligned_label(
			commands,
			renderer,
			"MAIN MENU",
			288.0f,
			440.0f,
			2,
			main_hovered
				? renderer.shell.font_white_palette
				: renderer.shell.font_gold_palette,
			0.58f);
		draw_aligned_label(
			commands,
			renderer,
			"QUIT",
			353.0f,
			440.0f,
			0,
			quit_hovered
				? renderer.shell.font_white_palette
				: renderer.shell.font_gold_palette,
			0.58f);
	}
	else if (menu.page == OptionsPage::audio)
	{
		options_audio_build(
			menu, renderer, config, hrtf_supported, commands);
	}
	else if (menu.page == OptionsPage::video)
	{
		options_video_build(menu, renderer, config, commands);
	}
	else if (menu.page == OptionsPage::controls)
	{
		options_controls_build(
			menu, renderer, config, joystick_available, commands);
	}

	build_modal(menu, language, renderer, commands);

	const std::uint64_t elapsed =
		now > menu.entered_at ? now - menu.entered_at : 0;
	if (menu.page == OptionsPage::controls)
	{
		gui::animated_cursor(
			commands,
			renderer.shell.control_options_cursor,
			renderer.shell.control_options_cursor_palette,
			menu.pointer_x,
			menu.pointer_y,
			elapsed,
			40);
	}
	else if (menu.page == OptionsPage::audio
		|| menu.page == OptionsPage::video)
	{
		gui::animated_cursor(
			commands,
			renderer.shell.options_detail_cursor,
			renderer.shell.options_detail_cursor_palette,
			menu.pointer_x,
			menu.pointer_y,
			elapsed,
			40);
	}
	else if (menu.in_game)
	{
		gui::animated_cursor(
			commands,
			renderer.shell.in_game_options_cursor,
			renderer.shell.in_game_options_cursor_palette,
			menu.pointer_x,
			menu.pointer_y,
			elapsed,
			40);
	}
	else
	{
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
}
