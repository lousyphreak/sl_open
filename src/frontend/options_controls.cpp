#include "frontend/options_controls.hpp"

#include "frontend/gui_render.hpp"
#include "frontend/options_render_internal.hpp"
#include "input/controls.hpp"
#include "render/frontend_renderer.hpp"

#include <cstdio>
#include <cstring>

namespace sl_open::frontend
{
using options_render::draw_label;

namespace
{
constexpr gui::Rect kControlRegions[] = {
	{70, 100, 140, 22},
	{215, 100, 80, 22},
	{300, 100, 125, 22},
	{430, 100, 130, 22},
	{70, 132, 130, 22},
	{205, 132, 130, 22},
	{340, 132, 100, 22},
	{445, 132, 105, 22},
	{70, 170, 490, 16},
	{70, 187, 490, 16},
	{70, 204, 490, 16},
	{70, 221, 490, 16},
	{70, 238, 490, 16},
	{70, 255, 490, 16},
	{70, 272, 490, 16},
	{70, 289, 490, 16},
	{70, 306, 490, 16},
	{70, 323, 490, 16},
	{70, 340, 490, 16},
	{70, 357, 490, 16},
	{570, 170, 24, 24},
	{570, 349, 24, 24},
	{390, 414, 115, 27},
	{340, 414, 45, 27},
	{340, 434, 85, 27},
	{425, 434, 135, 27},
};

constexpr std::int16_t kControlRows[80] = {
	0, 1, 2, 3, 4, 5, 6, 7, -1,
	8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, -1,
	20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
	36, 37, -1,
	38, 39, 40, 41, 42, 43, 44, 45, 46, 47, -1,
	48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1,
	62, 63, 64, 65, 66, -1,
	67, 68, 69, 70, -1,
	71, 72,
};
static_assert(kControlRows[79] == 72);
void draw_control_toggle(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* label,
	float x,
	float y,
	bool value,
	bool selected)
{
	gui::checkbox(
		commands, renderer.white, x, y + 2.0f, 12.0f, value);
	draw_label(
		commands, renderer, label, x + 18.0f, y, selected, 0.52f);
}

}

void options_controls_enter(OptionsMenu& menu, const Config& config)
{
	menu.controller_snapshot = config.controller;
	menu.control_flags_snapshot[0] = config.force_feedback;
	menu.control_flags_snapshot[1] = config.invert_pitch;
	menu.control_flags_snapshot[2] = config.hat_control;
	menu.control_flags_snapshot[3] = config.twist_control;
	std::memcpy(
		menu.control_snapshot,
		config.bindings,
		sizeof(menu.control_snapshot));
	menu.first_control_row = 0;
	menu.capture_action = -1;
	menu.control_wheel_suppressed_until = 0;
}

void options_controls_set_pointer(OptionsMenu& menu, float x, float y)
{
	for (std::uint32_t index = 0;
		index < sizeof(kControlRegions) / sizeof(kControlRegions[0]);
		++index)
	{
		if (gui::hit_open(kControlRegions[index], x, y))
		{
			menu.hovered = static_cast<std::int32_t>(index);
			return;
		}
	}
}

void options_scroll_controls(OptionsMenu& menu, int direction)
{
	if (menu.page != OptionsPage::controls)
	{
		return;
	}
	const int next =
		static_cast<int>(menu.first_control_row) + direction;
	menu.first_control_row = static_cast<std::uint32_t>(
		next < 0 ? 0 : (next > 68 ? 68 : next));
	menu.capture_action = -1;
}

OptionsSelection options_controls_select(
	OptionsMenu& menu,
	Config& config,
	bool joystick_available)
{
	if (menu.hovered >= 0 && menu.hovered <= 3)
	{
		constexpr std::uint8_t controllers[] = {
			kControllerJoystick,
			kControllerMouse,
			kControllerModernMouse,
			kControllerKeyboard,
		};
		const std::uint8_t controller = controllers[menu.hovered];
		if (controller != kControllerJoystick || joystick_available)
		{
			config.controller = controller;
			return OptionsSelection::changed;
		}
		return OptionsSelection::none;
	}
	if (menu.hovered >= 4 && menu.hovered <= 7)
	{
		switch (menu.hovered)
		{
		case 4: config.force_feedback = !config.force_feedback; break;
		case 5: config.invert_pitch = !config.invert_pitch; break;
		case 6: config.hat_control = !config.hat_control; break;
		case 7: config.twist_control = !config.twist_control; break;
		default: break;
		}
		return OptionsSelection::changed;
	}
	if (menu.hovered >= 8 && menu.hovered <= 19)
	{
		const std::uint32_t row =
			menu.first_control_row
			+ static_cast<std::uint32_t>(menu.hovered - 8);
		if (row < 80 && kControlRows[row] >= 0)
		{
			menu.capture_action = kControlRows[row];
		}
		return OptionsSelection::none;
	}
	if (menu.hovered == 20)
	{
		options_scroll_controls(menu, -1);
		return OptionsSelection::none;
	}
	if (menu.hovered == 21)
	{
		options_scroll_controls(menu, 1);
		return OptionsSelection::none;
	}
	if (menu.hovered == 22)
	{
		controls_defaults(config);
		if (!joystick_available && config.controller == 0)
		{
			config.controller = 2;
		}
		menu.capture_action = -1;
		return OptionsSelection::changed;
	}
	if (menu.hovered == 23) return OptionsSelection::options;
	if (menu.hovered == 24) return OptionsSelection::main_menu;
	if (menu.hovered == 25)
	{
		config.controller = menu.controller_snapshot;
		config.force_feedback = menu.control_flags_snapshot[0];
		config.invert_pitch = menu.control_flags_snapshot[1];
		config.hat_control = menu.control_flags_snapshot[2];
		config.twist_control = menu.control_flags_snapshot[3];
		std::memcpy(
			config.bindings,
			menu.control_snapshot,
			sizeof(menu.control_snapshot));
		menu.capture_action = -1;
		return OptionsSelection::changed;
	}
	return OptionsSelection::none;
}

bool options_capture_key(
	OptionsMenu& menu,
	Config& config,
	std::uint16_t scancode,
	std::uint8_t modifier)
{
	if (menu.page != OptionsPage::controls || menu.capture_action < 0)
	{
		return false;
	}
	control_bind_key(
		config,
		static_cast<std::uint32_t>(menu.capture_action),
		scancode,
		modifier);
	menu.capture_action = -1;
	return true;
}

bool options_capture_joystick(
	OptionsMenu& menu,
	Config& config,
	std::int16_t control)
{
	if (menu.page != OptionsPage::controls || menu.capture_action < 0)
	{
		return false;
	}
	control_bind_joystick(
		config,
		static_cast<std::uint32_t>(menu.capture_action),
		control);
	menu.capture_action = -1;
	return true;
}

bool options_capture_mouse(
	OptionsMenu& menu,
	Config& config,
	std::int16_t control)
{
	if (menu.page != OptionsPage::controls || menu.capture_action < 0)
	{
		return false;
	}
	control_bind_mouse(
		config,
		static_cast<std::uint32_t>(menu.capture_action),
		control);
	menu.capture_action = -1;
	return true;
}

void options_controls_build(
	const OptionsMenu& menu,
	const render::FrontendRenderer& renderer,
	const Config& config,
	bool joystick_available,
	render::FrontendCommands& commands)
{
	draw_label(
		commands, renderer, "CONTROL CONFIGURATION", 175.0f, 62.0f, false);
	constexpr const char* controllers[] = {
		"JOYSTICK / GAMEPAD",
		"MOUSE",
		"MODERN MOUSE",
		"KEYBOARD ONLY",
	};
	constexpr std::uint8_t controller_values[] = {
		kControllerJoystick,
		kControllerMouse,
		kControllerModernMouse,
		kControllerKeyboard,
	};
	constexpr float controller_x[] = {75.0f, 220.0f, 305.0f, 435.0f};
	for (std::uint32_t index = 0; index < 4; ++index)
	{
		const std::uint8_t controller = controller_values[index];
		const bool available =
			controller != kControllerJoystick || joystick_available;
		const bool selected = config.controller == controller;
		render::frontend_rgba_quad(
			commands,
			renderer.white,
			kControlRegions[index].x,
			kControlRegions[index].y,
			kControlRegions[index].width,
			kControlRegions[index].height,
			selected ? 0x2d8fa080 : 0x06151d40);
		draw_label(
			commands,
			renderer,
			controllers[index],
			controller_x[index],
			102.0f,
			menu.hovered == static_cast<std::int32_t>(index),
			available ? 0.42f : 0.38f);
	}
	draw_control_toggle(
		commands, renderer, "FORCE FEEDBACK", 75.0f, 134.0f,
		config.force_feedback, menu.hovered == 4);
	draw_control_toggle(
		commands, renderer, "INVERT PITCH", 210.0f, 134.0f,
		config.invert_pitch, menu.hovered == 5);
	draw_control_toggle(
		commands, renderer, "HAT", 345.0f, 134.0f,
		config.hat_control, menu.hovered == 6);
	draw_control_toggle(
		commands, renderer, "TWIST", 450.0f, 134.0f,
		config.twist_control, menu.hovered == 7);

	draw_label(commands, renderer, "FUNCTION", 75.0f, 155.0f, false, 0.5f);
	draw_label(commands, renderer, "CONTROL", 360.0f, 155.0f, false, 0.5f);
	for (std::uint32_t row = 0; row < 12; ++row)
	{
		const std::uint32_t display_row = menu.first_control_row + row;
		if (display_row >= 80)
		{
			break;
		}
		const std::int16_t action = kControlRows[display_row];
		const float y = 171.0f + row * 17.0f;
		const bool hovered =
			menu.hovered == static_cast<std::int32_t>(8 + row);
		if (action < 0)
		{
			render::frontend_rgba_quad(
				commands, renderer.white, 75.0f, y + 7.0f, 475.0f, 1.0f,
				0x8dc5d080);
			continue;
		}
		const bool capturing =
			menu.capture_action == action;
		if (hovered || capturing)
		{
			render::frontend_rgba_quad(
				commands,
				renderer.white,
				70.0f,
				y - 1.0f,
				490.0f,
				16.0f,
				capturing ? 0x9b642080 : 0x2d8fa060);
		}
		draw_label(
			commands,
			renderer,
			kControlActions[static_cast<std::uint32_t>(action)].name,
			75.0f,
			y,
			hovered,
			0.45f);
		char binding[96];
		if (capturing)
		{
			std::snprintf(
				binding, sizeof(binding), "PRESS A KEY, BUTTON OR WHEEL");
		}
		else
		{
			control_binding_text(
				config.bindings[static_cast<std::uint32_t>(action)],
				binding,
				sizeof(binding));
		}
		draw_label(
			commands, renderer, binding, 355.0f, y, capturing, 0.42f);
	}
	draw_label(commands, renderer, "^", 575.0f, 171.0f, menu.hovered == 20);
	draw_label(commands, renderer, "v", 575.0f, 351.0f, menu.hovered == 21);

	draw_label(
		commands, renderer, "OK", 350.0f, 420.0f,
		menu.hovered == 23, 0.58f);
	draw_label(
		commands, renderer, "RESET DEFAULTS", 395.0f, 420.0f,
		menu.hovered == 22, 0.58f);
	draw_label(
		commands, renderer, "MAIN MENU", 350.0f, 440.0f,
		menu.hovered == 24, 0.58f);
	draw_label(
		commands, renderer, "CANCEL CHANGES", 430.0f, 440.0f,
		menu.hovered == 25, 0.58f);
}
}
