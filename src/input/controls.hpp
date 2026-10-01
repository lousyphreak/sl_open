#pragma once

#include "config/config.hpp"

#include <cstddef>
#include <cstdint>

namespace sl_open
{
struct ControlAction
{
	const char* name{};
	const char* default_key{};
	std::uint8_t default_modifier{};
	std::int16_t default_joystick_control{-1};
	std::int16_t default_mouse_control{-1};
};

extern const ControlAction kControlActions[kControlActionCount];

void controls_defaults(Config& config);
void control_binding_text(
	const ControlBinding& binding,
	char* output,
	std::size_t capacity);
const char* control_binding_key_text(
	const ControlBinding& binding,
	std::uint32_t action);
void control_bind_key(
	Config& config,
	std::uint32_t action,
	std::uint16_t scancode,
	std::uint8_t modifier);
void control_bind_joystick(
	Config& config,
	std::uint32_t action,
	std::int16_t control);
void control_bind_mouse(
	Config& config,
	std::uint32_t action,
	std::int16_t control);
std::int16_t control_joystick_hat(
	std::uint8_t hat,
	std::uint8_t direction);
}
