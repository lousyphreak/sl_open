#include "frontend/options_video.hpp"

#include "frontend/gui_render.hpp"
#include "frontend/options_render_internal.hpp"
#include "render/frontend_renderer.hpp"

#include <cstdio>

namespace sl_open::frontend
{
namespace
{
constexpr gui::Rect kRegions[] = {
	{300, 111, 19, 26}, {322, 111, 19, 26},
	{300, 146, 19, 26}, {322, 146, 19, 26},
	{300, 181, 19, 26}, {322, 181, 19, 26},
	{300, 216, 19, 26}, {322, 216, 19, 26},
	{300, 251, 19, 26}, {322, 251, 19, 26},
	{340, 281, 190, 32},
	{293, 318, 20, 20},
	{538, 318, 20, 20},
	{293, 350, 20, 20},
	{538, 350, 20, 20},
	{293, 382, 20, 20},
	{390, 414, 115, 27},
	{340, 414, 45, 27},
	{340, 434, 85, 27},
	{425, 434, 135, 27},
};

using options_render::draw_label;

std::uint32_t resolution_index(
	const DisplayModes& modes,
	std::uint32_t width,
	std::uint32_t height)
{
	for (std::uint32_t index = 0; index < modes.count; ++index)
	{
		if (modes.items[index].width == width
			&& modes.items[index].height == height)
		{
			return index;
		}
	}
	return 0;
}

void cycle_resolution(
	Config& config,
	const DisplayModes& modes,
	int direction)
{
	if (modes.count == 0)
	{
		return;
	}
	const std::uint32_t current = resolution_index(
		modes, config.display_width, config.display_height);
	const std::uint32_t next = static_cast<std::uint32_t>(
		(static_cast<int>(current) + direction
			+ static_cast<int>(modes.count))
		% static_cast<int>(modes.count));
	config.display_width = modes.items[next].width;
	config.display_height = modes.items[next].height;
}

void draw_cycle_row(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* label,
	const char* value,
	float y,
	bool previous_selected,
	bool next_selected)
{
	draw_label(commands, renderer, label, 85.0f, y, false, 0.68f);
	draw_label(commands, renderer, "<", 302.0f, y, previous_selected);
	draw_label(commands, renderer, ">", 324.0f, y, next_selected);
	draw_label(commands, renderer, value, 365.0f, y, false, 0.62f);
}

}

void options_video_enter(OptionsMenu& menu, const Config& config)
{
	menu.video_width_snapshot = config.display_width;
	menu.video_height_snapshot = config.display_height;
	menu.video_snapshot[0] = config.texture_detail;
	menu.video_snapshot[1] = config.graphics_detail;
	menu.video_snapshot[2] = config.default_view;
	menu.brightness_snapshot = config.brightness;
	menu.display_mode_snapshot = config.display_mode;
	menu.video_flags_snapshot[0] = config.light_maps;
	menu.video_flags_snapshot[1] = config.transitions;
	menu.video_flags_snapshot[2] = config.vsync;
	menu.video_flags_snapshot[3] = config.expand_widescreen_movies;
	menu.video_flags_snapshot[4] = config.pause_in_background;
}

void options_video_set_pointer(OptionsMenu& menu, float x, float y)
{
	for (std::uint32_t index = 0;
		index < sizeof(kRegions) / sizeof(kRegions[0]);
		++index)
	{
		if (gui::hit_open(kRegions[index], x, y))
		{
			menu.hovered = static_cast<std::int32_t>(index);
			return;
		}
	}
}

OptionsSelection options_video_select(
	const OptionsMenu& menu,
	Config& config,
	const DisplayModes& modes)
{
	if (menu.hovered == 0 || menu.hovered == 1)
	{
		cycle_resolution(config, modes, menu.hovered == 0 ? -1 : 1);
		return OptionsSelection::changed;
	}
	if (menu.hovered >= 2 && menu.hovered <= 9)
	{
		const int direction = (menu.hovered & 1) == 0 ? -1 : 1;
		if (menu.hovered <= 3)
		{
			const int value =
				(static_cast<int>(config.display_mode) + direction + 3) % 3;
			config.display_mode = static_cast<DisplayMode>(value);
		}
		else if (menu.hovered <= 5)
		{
			config.texture_detail = static_cast<std::uint8_t>(
				(config.texture_detail + direction + 2) % 2);
		}
		else if (menu.hovered <= 7)
		{
			config.graphics_detail = static_cast<std::uint8_t>(
				(config.graphics_detail + direction + 3) % 3);
		}
		else
		{
			config.default_view = static_cast<std::uint8_t>(
				(config.default_view + direction + 3) % 3);
		}
		return OptionsSelection::changed;
	}
	if (menu.hovered == 10)
	{
		options_video_drag(menu, config);
		return OptionsSelection::changed;
	}
	if (menu.hovered >= 11 && menu.hovered <= 15)
	{
		bool* flags[] = {
			&config.light_maps,
			&config.transitions,
			&config.vsync,
			&config.expand_widescreen_movies,
			&config.pause_in_background,
		};
		*flags[menu.hovered - 11] = !*flags[menu.hovered - 11];
		return OptionsSelection::changed;
	}
	if (menu.hovered == 16)
	{
		config.texture_detail = 1;
		config.graphics_detail = 2;
		config.default_view = 0;
		config.brightness = 100;
		config.display_mode = DisplayMode::windowed;
		config.light_maps = true;
		config.transitions = true;
		config.vsync = true;
		config.expand_widescreen_movies = true;
		config.pause_in_background = true;
		return OptionsSelection::changed;
	}
	if (menu.hovered == 17) return OptionsSelection::apply_options;
	if (menu.hovered == 18) return OptionsSelection::apply_main_menu;
	if (menu.hovered == 19)
	{
		config.display_width = menu.video_width_snapshot;
		config.display_height = menu.video_height_snapshot;
		config.texture_detail = menu.video_snapshot[0];
		config.graphics_detail = menu.video_snapshot[1];
		config.default_view = menu.video_snapshot[2];
		config.brightness = menu.brightness_snapshot;
		config.display_mode = menu.display_mode_snapshot;
		config.light_maps = menu.video_flags_snapshot[0];
		config.transitions = menu.video_flags_snapshot[1];
		config.vsync = menu.video_flags_snapshot[2];
		config.expand_widescreen_movies = menu.video_flags_snapshot[3];
		config.pause_in_background = menu.video_flags_snapshot[4];
		return OptionsSelection::changed;
	}
	return OptionsSelection::none;
}

bool options_video_drag(const OptionsMenu& menu, Config& config)
{
	if (menu.hovered != 10)
	{
		return false;
	}
	const float x = menu.pointer_x < 347.0f
		? 347.0f
		: (menu.pointer_x > 522.0f ? 522.0f : menu.pointer_x);
	config.brightness = static_cast<std::uint8_t>(
		50.0f + (x - 347.0f) * (150.0f / 175.0f) + 0.5f);
	return true;
}

void options_video_build(
	const OptionsMenu& menu,
	const render::FrontendRenderer& renderer,
	const Config& config,
	render::FrontendCommands& commands)
{
	char resolution[32];
	std::snprintf(
		resolution,
		sizeof(resolution),
		"%u x %u",
		config.display_width,
		config.display_height);
	const char* display_modes[] = {"WINDOWED", "BORDERLESS", "FULLSCREEN"};
	const char* texture[] = {"LOW", "HIGH"};
	const char* graphics[] = {"LOW", "MEDIUM", "HIGH"};
	const char* views[] = {"COCKPIT VIEW", "CHASE VIEW", "NO COCKPIT VIEW"};

	draw_label(
		commands, renderer, "GRAPHICS CONFIGURATION", 165.0f, 67.0f, false);
	draw_cycle_row(
		commands, renderer, "RESOLUTION", resolution, 112.0f,
		menu.hovered == 0, menu.hovered == 1);
	draw_cycle_row(
		commands, renderer, "DISPLAY MODE",
		display_modes[static_cast<unsigned>(config.display_mode)], 147.0f,
		menu.hovered == 2, menu.hovered == 3);
	draw_cycle_row(
		commands, renderer, "TEXTURE DETAIL",
		texture[config.texture_detail], 182.0f,
		menu.hovered == 4, menu.hovered == 5);
	draw_cycle_row(
		commands, renderer, "GRAPHIC DETAIL",
		graphics[config.graphics_detail], 217.0f,
		menu.hovered == 6, menu.hovered == 7);
	draw_cycle_row(
		commands, renderer, "DEFAULT VIEW",
		views[config.default_view], 252.0f,
		menu.hovered == 8, menu.hovered == 9);

	draw_label(commands, renderer, "BRIGHTNESS", 85.0f, 287.0f, false, 0.68f);
	gui::slider(
		commands,
		renderer.white,
		347.0f,
		292.0f,
		175.0f,
		(config.brightness - 50) / 150.0f,
		menu.hovered == 10);

	const char* toggles[] = {
		"LIGHT MAPS",
		"VR TRANSITIONS",
		"VSYNC",
		"WIDESCREEN MOVIES",
		"PAUSE IN BACKGROUND",
	};
	const bool values[] = {
		config.light_maps,
		config.transitions,
		config.vsync,
		config.expand_widescreen_movies,
		config.pause_in_background,
	};
	const float toggle_x[] = {85.0f, 330.0f, 85.0f, 330.0f, 85.0f};
	const float checkbox_x[] = {295.0f, 540.0f, 295.0f, 540.0f, 295.0f};
	const float toggle_y[] = {320.0f, 320.0f, 352.0f, 352.0f, 384.0f};
	for (std::uint32_t index = 0; index < 5; ++index)
	{
		draw_label(
			commands,
			renderer,
			toggles[index],
			toggle_x[index],
			toggle_y[index],
			menu.hovered == static_cast<std::int32_t>(11 + index), 0.68f);
		gui::checkbox(
			commands,
			renderer.white,
			checkbox_x[index],
			toggle_y[index],
			14.0f,
			values[index]);
	}

	draw_label(
		commands, renderer, "OK", 350.0f, 420.0f,
		menu.hovered == 17, 0.58f);
	draw_label(
		commands, renderer, "RESET DEFAULTS", 395.0f, 420.0f,
		menu.hovered == 16, 0.58f);
	draw_label(
		commands, renderer, "MAIN MENU", 350.0f, 440.0f,
		menu.hovered == 18, 0.58f);
	draw_label(
		commands, renderer, "CANCEL CHANGES", 430.0f, 440.0f,
		menu.hovered == 19, 0.58f);
}
}
