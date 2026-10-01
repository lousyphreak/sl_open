#include "input/gameplay_input.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <iterator>

namespace sl_open::input
{
namespace
{
bool shift_active(const GameplayDeviceState& devices)
{
	return devices.keyboard[SDL_SCANCODE_LSHIFT]
		|| devices.keyboard[SDL_SCANCODE_RSHIFT];
}

bool control_active(const GameplayDeviceState& devices)
{
	return devices.keyboard[SDL_SCANCODE_LCTRL]
		|| devices.keyboard[SDL_SCANCODE_RCTRL];
}

bool alt_active(const GameplayDeviceState& devices)
{
	return devices.keyboard[SDL_SCANCODE_LALT]
		|| devices.keyboard[SDL_SCANCODE_RALT];
}

bool generic_modifier_active(
	std::uint8_t required,
	const GameplayDeviceState& devices,
	bool pressed)
{
	const bool shift = shift_active(devices);
	const bool control = control_active(devices);
	const bool alt = alt_active(devices);
	switch (required)
	{
	case 0:
		// FUN_00412630's held branch tests Shift and Control but, unlike
		// its raw pressed helper, deliberately ignores Alt.
		return !shift && !control && (!pressed || !alt);
	case 1:
		return shift;
	case 2:
		return control;
	case 3:
		return alt;
	default:
		return false;
	}
}

bool wait_for_key_keyboard_active(
	const ControlBinding& binding,
	const GameplayDeviceState& devices)
{
	const std::uint32_t scancode = binding.scancode;
	if (scancode == SDL_SCANCODE_UNKNOWN
		|| scancode >= SDL_SCANCODE_COUNT
		|| !devices.keyboard[scancode])
	{
		return false;
	}
	switch (binding.modifier)
	{
	case 0:
		return true;
	case 1:
		return shift_active(devices);
	case 2:
		return control_active(devices);
	default:
		// WaitForKey_command has no accepting Alt/default keyboard branch.
		return false;
	}
}

bool valid_scancode(SDL_Scancode scancode)
{
	return scancode > SDL_SCANCODE_UNKNOWN
		&& scancode < SDL_SCANCODE_COUNT;
}

bool communications_number_key(
	const GameplayInputPoller& poller,
	std::uint32_t scancode)
{
	return poller.communications_state == 3
		&& scancode >= SDL_SCANCODE_1
		&& scancode <= SDL_SCANCODE_8;
}

constexpr std::uint8_t kHatDirections[] = {
	SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT};

bool joystick_control_held(
	const GameplayDeviceState& devices,
	std::int16_t control)
{
	if (control >= 0 && control < kControlJoystickButtonCount)
	{
		return devices.joystick_buttons[control];
	}
	if (control >= kControlJoystickHatBase
		&& control < kControlJoystickLeftTrigger)
	{
		const std::int16_t offset = control - kControlJoystickHatBase;
		return (devices.joystick_hats[offset / 4]
			& kHatDirections[offset % 4]) != 0;
	}
	if (control == kControlJoystickLeftTrigger
		|| control == kControlJoystickRightTrigger)
	{
		return devices.gamepad_triggers[
			control - kControlJoystickLeftTrigger];
	}
	return false;
}

bool joystick_control_pressed(
	GameplayDeviceState& devices,
	std::int16_t control)
{
	if (control >= 0 && control < kControlJoystickButtonCount)
	{
		if (devices.joystick_buttons[control]
			&& devices.joystick_pressed[control])
		{
			devices.joystick_pressed[control] = false;
			return true;
		}
		return false;
	}
	if (control >= kControlJoystickHatBase
		&& control < kControlJoystickLeftTrigger)
	{
		const std::int16_t offset = control - kControlJoystickHatBase;
		const std::uint8_t direction = kHatDirections[offset % 4];
		if ((devices.joystick_hats[offset / 4] & direction) != 0
			&& (devices.joystick_hat_pressed[offset / 4] & direction) != 0)
		{
			devices.joystick_hat_pressed[offset / 4] =
				static_cast<std::uint8_t>(
					devices.joystick_hat_pressed[offset / 4] & ~direction);
			return true;
		}
		return false;
	}
	if (control == kControlJoystickLeftTrigger
		|| control == kControlJoystickRightTrigger)
	{
		const std::size_t trigger =
			control - kControlJoystickLeftTrigger;
		if (devices.gamepad_triggers[trigger]
			&& devices.gamepad_trigger_pressed[trigger])
		{
			devices.gamepad_trigger_pressed[trigger] = false;
			return true;
		}
	}
	return false;
}

bool mouse_control_active(
	const GameplayDeviceState& devices,
	std::int16_t control)
{
	if (control > 0 && control <= kControlMouseButtonCount)
	{
		return devices.mouse_buttons[control];
	}
	if (control >= kControlMouseWheelUp
		&& control <= kControlMouseWheelRight)
	{
		return devices.mouse_wheel[control - kControlMouseWheelUp] != 0;
	}
	return false;
}

bool mouse_control_held(
	GameplayDeviceState& devices,
	std::int16_t control)
{
	if (control > 0 && control <= kControlMouseButtonCount)
	{
		return devices.mouse_buttons[control];
	}
	if (control >= kControlMouseWheelUp
		&& control <= kControlMouseWheelRight)
	{
		std::uint16_t& ticks =
			devices.mouse_wheel[control - kControlMouseWheelUp];
		if (ticks != 0)
		{
			--ticks;
			return true;
		}
	}
	return false;
}

bool mouse_control_pressed(
	GameplayDeviceState& devices,
	std::int16_t control)
{
	if (control > 0 && control <= kControlMouseButtonCount)
	{
		if (devices.mouse_buttons[control]
			&& devices.mouse_pressed[control])
		{
			devices.mouse_pressed[control] = false;
			return true;
		}
		return false;
	}
	return mouse_control_held(devices, control);
}
}

void gameplay_input_begin_frame(
	const Config& config,
	GameplayDeviceState& devices,
	std::uint8_t communications_state,
	GameplayInputPoller& poller)
{
	for (std::size_t key = 0; key < std::size(devices.keyboard); ++key)
	{
		if (!devices.keyboard[key])
		{
			devices.keyboard_pressed[key] = false;
		}
	}
	for (std::size_t button = 0;
		button < std::size(devices.joystick_buttons);
		++button)
	{
		if (!devices.joystick_buttons[button])
		{
			devices.joystick_pressed[button] = false;
		}
	}
	for (std::size_t hat = 0; hat < std::size(devices.joystick_hats); ++hat)
	{
		devices.joystick_hat_pressed[hat] = static_cast<std::uint8_t>(
			devices.joystick_hat_pressed[hat] & devices.joystick_hats[hat]);
	}
	for (std::size_t trigger = 0;
		trigger < std::size(devices.gamepad_triggers);
		++trigger)
	{
		if (!devices.gamepad_triggers[trigger])
		{
			devices.gamepad_trigger_pressed[trigger] = false;
		}
	}
	for (std::size_t button = 1;
		button < std::size(devices.mouse_buttons);
		++button)
	{
		if (!devices.mouse_buttons[button])
		{
			devices.mouse_pressed[button] = false;
		}
	}
	if (!shift_active(devices))
	{
		devices.modifier_latched[0] = false;
	}
	if (!control_active(devices))
	{
		devices.modifier_latched[1] = false;
	}
	if (!alt_active(devices))
	{
		devices.modifier_latched[2] = false;
	}
	if (devices.fire_release_key != SDL_SCANCODE_UNKNOWN
		&& (!valid_scancode(devices.fire_release_key)
			|| !devices.keyboard[devices.fire_release_key]))
	{
		devices.fire_release_key = SDL_SCANCODE_UNKNOWN;
	}
	if (devices.fire_release_mouse_active
		&& (devices.fire_release_mouse
				>= std::size(devices.mouse_buttons)
			|| !devices.mouse_buttons[devices.fire_release_mouse]))
	{
		devices.fire_release_mouse_active = false;
	}
	poller.config = &config;
	poller.devices = &devices;
	poller.communications_state = communications_state;
}

void gameplay_input_gate_fire_until_key_released(
	GameplayDeviceState& devices,
	SDL_Scancode scancode)
{
	if (valid_scancode(scancode) && devices.keyboard[scancode])
	{
		devices.fire_release_key = scancode;
	}
}

void gameplay_input_gate_fire_until_mouse_released(
	GameplayDeviceState& devices,
	std::uint8_t button)
{
	if (button < std::size(devices.mouse_buttons)
		&& devices.mouse_buttons[button])
	{
		devices.fire_release_mouse = button;
		devices.fire_release_mouse_active = true;
	}
}

bool gameplay_input_fire_release_gated(
	const GameplayInputPoller& poller)
{
	return poller.devices != nullptr
		&& (poller.devices->fire_release_key != SDL_SCANCODE_UNKNOWN
			|| poller.devices->fire_release_mouse_active);
}

void gameplay_input_set_communications_state(
	GameplayInputPoller& poller,
	std::uint8_t communications_state)
{
	poller.communications_state = communications_state;
}

void gameplay_input_capture_analog(
	const GameplayInputPoller& poller,
	GameplayInput& input)
{
	if (poller.config == nullptr || poller.devices == nullptr)
	{
		return;
	}
	const GameplayDeviceState& devices = *poller.devices;
	std::copy(
		std::begin(devices.joystick_axes),
		std::end(devices.joystick_axes),
		std::begin(input.joystick_axes));
	std::copy(
		std::begin(devices.joystick_axis_available),
		std::end(devices.joystick_axis_available),
		std::begin(input.joystick_axis_available));
	input.mouse_relative_x = devices.mouse_relative_x;
	input.mouse_relative_y = devices.mouse_relative_y;
	if (poller.config->hat_control && devices.joystick_hat_available[0])
	{
		// The executable accepts only exact cardinal DirectInput POV values.
		// Centered and diagonal positions do not select a camera.
		switch (devices.joystick_hats[0])
		{
		case SDL_HAT_UP:
			input.camera_hat_mode = 0;
			break;
		case SDL_HAT_RIGHT:
			input.camera_hat_mode = 2;
			break;
		case SDL_HAT_DOWN:
			input.camera_hat_mode = 3;
			break;
		case SDL_HAT_LEFT:
			input.camera_hat_mode = 1;
			break;
		default:
			break;
		}
	}
}

void gameplay_input_capture_wait_for_key(
	const GameplayInputPoller& poller,
	GameplayInput& input)
{
	if (poller.config == nullptr || poller.devices == nullptr)
	{
		return;
	}
	const Config& config = *poller.config;
	const GameplayDeviceState& devices = *poller.devices;
	for (std::uint32_t action = 0;
		action < kControlActionCount;
		++action)
	{
		const ControlBinding& binding = config.bindings[action];
		const bool joystick = joystick_control_held(
			devices, binding.joystick_control);
		const bool mouse = mouse_control_active(
			devices, binding.mouse_control);
		input.wait_for_key_accepted[action] =
			wait_for_key_keyboard_active(binding, devices)
			|| joystick || mouse;
	}
}

bool gameplay_input_action_held(
	GameplayInputPoller& poller,
	std::uint8_t action)
{
	if (poller.config == nullptr || poller.devices == nullptr
		|| action >= kControlActionCount)
	{
		return false;
	}
	const ControlBinding& binding = poller.config->bindings[action];
	GameplayDeviceState& devices = *poller.devices;
	if (joystick_control_held(devices, binding.joystick_control))
	{
		return true;
	}
	if (mouse_control_held(devices, binding.mouse_control))
	{
		return true;
	}
	const std::uint32_t scancode = binding.scancode;
	if (scancode == SDL_SCANCODE_UNKNOWN
		|| scancode >= SDL_SCANCODE_COUNT
		|| communications_number_key(poller, scancode))
	{
		return false;
	}
	return devices.keyboard[scancode]
		&& generic_modifier_active(binding.modifier, devices, false);
}

bool gameplay_input_action_pressed(
	GameplayInputPoller& poller,
	std::uint8_t action)
{
	if (poller.config == nullptr || poller.devices == nullptr
		|| action >= kControlActionCount)
	{
		return false;
	}
	const ControlBinding& binding = poller.config->bindings[action];
	GameplayDeviceState& devices = *poller.devices;
	if (joystick_control_pressed(devices, binding.joystick_control))
	{
		return true;
	}
	if (mouse_control_pressed(devices, binding.mouse_control))
	{
		return true;
	}
	const std::uint32_t scancode = binding.scancode;
	if (scancode == SDL_SCANCODE_UNKNOWN
		|| scancode >= SDL_SCANCODE_COUNT
		|| communications_number_key(poller, scancode))
	{
		return false;
	}
	return gameplay_input_raw_keyboard_pressed(
		poller,
		static_cast<SDL_Scancode>(scancode),
		binding.modifier);
}

bool gameplay_input_raw_keyboard_held(
	GameplayInputPoller& poller,
	SDL_Scancode scancode,
	std::uint8_t modifier)
{
	if (poller.devices == nullptr || !valid_scancode(scancode))
	{
		return false;
	}
	GameplayDeviceState& devices = *poller.devices;
	if (!devices.keyboard[scancode])
	{
		return false;
	}
	bool accepted = false;
	switch (modifier)
	{
	case 0:
		accepted = !devices.modifier_latched[0]
			&& !devices.modifier_latched[1]
			&& !devices.modifier_latched[2];
		break;
	case 1:
		accepted = shift_active(devices);
		break;
	case 2:
		accepted = control_active(devices);
		break;
	case 3:
		accepted = alt_active(devices);
		break;
	default:
		break;
	}
	if (!accepted)
	{
		return false;
	}
	// FUN_004bd570's held branch clears the physical key latch and the
	// matching modifier latch. In this inverse representation that rearms
	// the key for a later pressed owner while it remains down.
	devices.keyboard_pressed[scancode] = true;
	if (modifier != 0)
	{
		devices.modifier_latched[modifier - 1] = false;
	}
	return true;
}

bool gameplay_input_raw_keyboard_pressed(
	GameplayInputPoller& poller,
	SDL_Scancode scancode,
	std::uint8_t modifier)
{
	if (poller.devices == nullptr || !valid_scancode(scancode))
	{
		return false;
	}
	GameplayDeviceState& devices = *poller.devices;
	if (!devices.keyboard[scancode]
		|| !devices.keyboard_pressed[scancode]
		|| !generic_modifier_active(modifier, devices, true))
	{
		return false;
	}
	devices.keyboard_pressed[scancode] = false;
	if (modifier != 0)
	{
		devices.modifier_latched[modifier - 1] = true;
	}
	return true;
}

void gameplay_input_poll(
	const Config& config,
	GameplayDeviceState& devices,
	const bool* previous,
	GameplayInput& input)
{
	(void)previous;
	input = {};
	GameplayInputPoller poller;
	gameplay_input_begin_frame(config, devices, 0, poller);
	gameplay_input_capture_analog(poller, input);
	gameplay_input_capture_wait_for_key(poller, input);

	// Compatibility path for non-session callers. The live mission owner
	// uses the explicit query API above so that gates and shared physical
	// latches follow the executable's actual per-owner order.
	for (std::uint8_t action = 0;
		action < kControlActionCount;
		++action)
	{
		input.held[action] =
			gameplay_input_action_held(poller, action);
		input.pressed[action] =
			gameplay_input_action_pressed(poller, action);
	}
	for (std::uint8_t option = 0; option < 8; ++option)
	{
		input.comms_option_pressed[option] =
			gameplay_input_raw_keyboard_pressed(
				poller,
				static_cast<SDL_Scancode>(SDL_SCANCODE_1 + option),
				0);
	}
}
}
