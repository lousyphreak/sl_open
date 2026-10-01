#include "input/controls.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <iterator>

namespace sl_open
{
const ControlAction kControlActions[kControlActionCount] = {
	{"COCKPIT CAMERA", "1", 0, -1},
	{"LEFT VIEW CAMERA", "2", 0, -1},
	{"RIGHT VIEW CAMERA", "3", 0, -1},
	{"REAR VIEW CAMERA", "4", 0, -1},
	{"FLYBY CAMERA", "5", 0, -1},
	{"TARGET CAMERA", "6", 0, -1},
	{"EXTERNAL CAMERA", "7", 0, -1},
	{"MISSILE CAMERA", "8", 0, -1},
	{"NEXT ENEMY TARGET", "E", 0, -1},
	{"PREVIOUS ENEMY TARGET", "E", 1, -1},
	{"NEXT FRIENDLY TARGET", "Q", 0, 5},
	{"PREVIOUS FRIENDLY TARGET", "Q", 1, -1},
	{"NEXT SUBTARGET", "S", 0, -1},
	{"PREVIOUS SUBTARGET", "S", 1, -1},
	{"TARGET UNDER RETICULE", "Y", 0, -1},
	{"TARGET NEAREST ENEMY", "R", 0, 3},
	{"TARGET NEAREST FRIENDLY", "W", 0, -1},
	{"TARGET TORPEDO", "T", 0, -1},
	{"SMART TARGET", "E", 2, -1},
	{"PRIMARY TARGET", "A", 0, -1},
	{"AFTERBURNERS", "TAB", 0, 2},
	{"AFTERBURNER TOGGLE", "`", 0, -1},
	{"REVERSE THRUST", "TAB", 1, -1},
	{"JUMP DRIVE", "J", 0, -1},
	{"MATCH SPEED", "Z", 0, -1},
	{"ACCELERATE", "+", 0, -1},
	{"DECELERATE", "-", 0, -1},
	{"ZERO THROTTLE", "BACKSPACE", 0, -1},
	{"FULL THROTTLE", "\\", 0, -1},
	{"ROLL SHIP CLOCKWISE", "PAGE UP", 0, -1},
	// The PE record's stale literal says "PAGE DOWN", but startup rebuilds
	// that text from its actual DIK 0xc7 binding and displays "HOME".
	{"ROLL SHIP ANTI-CLOCKWISE", "HOME", 0, -1},
	{"NOSE UP", "CURSOR DOWN", 0, -1},
	{"NOSE DOWN", "CURSOR UP", 0, -1},
	{"ROTATE CLOCKWISE", "CURSOR LEFT", 0, -1},
	{"ROTATE ANTI-CLOCKWISE", "CURSOR RIGHT", 0, -1},
	{"STRAFE LEFT", "END", 0, 7},
	{"STRAFE RIGHT", "PAGE DOWN", 0, 4},
	{"JOYSTICK ROLL", "INSERT", 0, -1},
	{"FIRE LASERS", "SPACE", 0, 0, SDL_BUTTON_LEFT},
	{"FULL GUNS", "F", 0, -1},
	{"GUNNERY WINDOW", "G", 0, -1},
	{"GUNNERY WINDOW LOCKED", "G", 1, -1},
	{"SYNCHRONISE GUNS", "G", 2, -1},
	{"TOGGLE BLINDFIRE", "F", 1, -1},
	{"LAUNCH MISSILE", "ENTER", 0, 1, SDL_BUTTON_RIGHT},
	{"MISSILE WINDOW", "M", 0, -1},
	{"ROTATE MISSILES CLOCKWISE", ".", 0, -1},
	{"ROTATE MISSILES ANTICLOCKWISE", ",", 0, -1},
	{"COMMS WINDOW", "C", 0, -1},
	{"POWERBALL WINDOW", "P", 0, -1},
	{"POWERBALL WINDOW LOCKED", "P", 1, -1},
	{"FULL POWER TO GUNNERY", "U", 0, -1},
	{"FULL POWER TO ENGINES", "I", 0, -1},
	{"FULL POWER TO SHIELDS", "O", 0, -1},
	{"EQUALIZE POWER", "[", 0, -1},
	{"OBJECTIVES WINDOW", "B", 0, -1},
	{"WING STATUS WINDOW", "X", 0, -1},
	{"WING STATUS WINDOW LOCKED", "X", 1, -1},
	{"DAMAGE WINDOW", "D", 0, -1},
	{"DAMAGE WINDOW LOCKED", "D", 1, -1},
	{"RADAR RANGES", "V", 0, -1},
	{"SHIELD BALANCING", "N", 0, -1},
	{"COUNTERMEASURES", "H", 0, -1},
	{"EJECT", "F12", 0, -1},
	{"CLOAK SHIP", "K", 0, -1},
	{"ECM", "L", 0, -1},
	{"SPECTRAL SHIELDS", ";", 0, -1},
	{"ATTACK MY TARGET", "F5", 0, -1},
	{"BACK OFF", "F6", 0, -1},
	{"HELP ME", "F7", 0, -1},
	{"PERMISSION TO LAND", "F8", 0, -1},
	{"DISPLAY KILLS", "F10", 0, -1},
	{"SEND COMMS MESSAGE", "'", 0, -1},
	{"KEY CONFIG", "F1", 0, -1},
};

namespace
{
struct SupportedKey
{
	SDL_Scancode scancode;
	const char* name;
};

// ControlBindings_accept_key scans these same 89 retail names in this order.
// The original DirectInput scan codes are projected to their SDL scancodes.
constexpr SupportedKey kSupportedKeys[] = {
	{SDL_SCANCODE_A, "A"}, {SDL_SCANCODE_B, "B"},
	{SDL_SCANCODE_C, "C"}, {SDL_SCANCODE_D, "D"},
	{SDL_SCANCODE_E, "E"}, {SDL_SCANCODE_F, "F"},
	{SDL_SCANCODE_G, "G"}, {SDL_SCANCODE_H, "H"},
	{SDL_SCANCODE_I, "I"}, {SDL_SCANCODE_J, "J"},
	{SDL_SCANCODE_K, "K"}, {SDL_SCANCODE_L, "L"},
	{SDL_SCANCODE_M, "M"}, {SDL_SCANCODE_N, "N"},
	{SDL_SCANCODE_O, "O"}, {SDL_SCANCODE_P, "P"},
	{SDL_SCANCODE_Q, "Q"}, {SDL_SCANCODE_R, "R"},
	{SDL_SCANCODE_S, "S"}, {SDL_SCANCODE_T, "T"},
	{SDL_SCANCODE_U, "U"}, {SDL_SCANCODE_V, "V"},
	{SDL_SCANCODE_W, "W"}, {SDL_SCANCODE_X, "X"},
	{SDL_SCANCODE_Y, "Y"}, {SDL_SCANCODE_Z, "Z"},
	{SDL_SCANCODE_SPACE, "SPACE"},
	{SDL_SCANCODE_PERIOD, "PERIOD"},
	{SDL_SCANCODE_COMMA, "COMMA"},
	{SDL_SCANCODE_1, "1"}, {SDL_SCANCODE_2, "2"},
	{SDL_SCANCODE_3, "3"}, {SDL_SCANCODE_4, "4"},
	{SDL_SCANCODE_5, "5"}, {SDL_SCANCODE_6, "6"},
	{SDL_SCANCODE_7, "7"}, {SDL_SCANCODE_8, "8"},
	{SDL_SCANCODE_9, "9"}, {SDL_SCANCODE_0, "0"},
	{SDL_SCANCODE_END, "END"}, {SDL_SCANCODE_DELETE, "DELETE"},
	{SDL_SCANCODE_MINUS, "-"}, {SDL_SCANCODE_EQUALS, "="},
	{SDL_SCANCODE_RETURN, "ENTER"},
	{SDL_SCANCODE_UP, "CURSOR UP"},
	{SDL_SCANCODE_DOWN, "CURSOR DOWN"},
	{SDL_SCANCODE_LEFT, "CURSOR LEFT"},
	{SDL_SCANCODE_RIGHT, "CURSOR RIGHT"},
	{SDL_SCANCODE_F1, "F1"}, {SDL_SCANCODE_F2, "F2"},
	{SDL_SCANCODE_F3, "F3"}, {SDL_SCANCODE_F4, "F4"},
	{SDL_SCANCODE_F5, "F5"}, {SDL_SCANCODE_F6, "F6"},
	{SDL_SCANCODE_F7, "F7"}, {SDL_SCANCODE_F8, "F8"},
	{SDL_SCANCODE_F9, "F9"}, {SDL_SCANCODE_F10, "F10"},
	{SDL_SCANCODE_F11, "F11"}, {SDL_SCANCODE_F12, "F12"},
	{SDL_SCANCODE_INSERT, "INSERT"}, {SDL_SCANCODE_HOME, "HOME"},
	{SDL_SCANCODE_PAGEUP, "PAGE UP"},
	{SDL_SCANCODE_PAGEDOWN, "PAGE DOWN"},
	{SDL_SCANCODE_KP_ENTER, "PAD ENTER"},
	{SDL_SCANCODE_BACKSPACE, "BACKSPACE"},
	{SDL_SCANCODE_KP_MINUS, "PAD MINUS"},
	{SDL_SCANCODE_KP_PLUS, "PAD PLUS"},
	{SDL_SCANCODE_LEFTBRACKET, "["},
	{SDL_SCANCODE_RIGHTBRACKET, "]"},
	{SDL_SCANCODE_APOSTROPHE, "'"},
	{SDL_SCANCODE_SEMICOLON, ";"},
	{SDL_SCANCODE_KP_DIVIDE, "NUMPAD /"},
	{SDL_SCANCODE_KP_MULTIPLY, "NUMPAD *"},
	{SDL_SCANCODE_KP_0, "NUMPAD 0"},
	{SDL_SCANCODE_KP_1, "NUMPAD 1"},
	{SDL_SCANCODE_KP_2, "NUMPAD 2"},
	{SDL_SCANCODE_KP_3, "NUMPAD 3"},
	{SDL_SCANCODE_KP_4, "NUMPAD 4"},
	{SDL_SCANCODE_KP_5, "NUMPAD 5"},
	{SDL_SCANCODE_KP_6, "NUMPAD 6"},
	{SDL_SCANCODE_KP_7, "NUMPAD 7"},
	{SDL_SCANCODE_KP_8, "NUMPAD 8"},
	{SDL_SCANCODE_KP_9, "NUMPAD 9"},
	{SDL_SCANCODE_TAB, "TAB"}, {SDL_SCANCODE_BACKSLASH, "\\"},
	{SDL_SCANCODE_SLASH, "/"}, {SDL_SCANCODE_GRAVE, "`"},
	{SDL_SCANCODE_KP_PERIOD, "NUMPAD ."},
};
static_assert(std::size(kSupportedKeys) == 89);

SDL_Scancode named_scancode(const char* name)
{
	for (const SupportedKey& key : kSupportedKeys)
	{
		if (std::strcmp(name, key.name) == 0)
		{
			return key.scancode;
		}
	}
	// The compiled accelerate default is displayed as "+" while using the
	// equals-key scan code; it is the sole alias outside the 89-name table.
	if (std::strcmp(name, "+") == 0)
	{
		return SDL_SCANCODE_EQUALS;
	}
	if (std::strcmp(name, ".") == 0)
	{
		return SDL_SCANCODE_PERIOD;
	}
	if (std::strcmp(name, ",") == 0)
	{
		return SDL_SCANCODE_COMMA;
	}
	return SDL_SCANCODE_UNKNOWN;
}

const char* supported_key_name(std::uint16_t scancode)
{
	for (const SupportedKey& key : kSupportedKeys)
	{
		if (key.scancode == static_cast<SDL_Scancode>(scancode))
		{
			return key.name;
		}
	}
	return "";
}

const char* gamepad_button_name(std::int16_t control)
{
	switch (static_cast<SDL_GamepadButton>(control))
	{
	case SDL_GAMEPAD_BUTTON_SOUTH: return "PAD SOUTH";
	case SDL_GAMEPAD_BUTTON_EAST: return "PAD EAST";
	case SDL_GAMEPAD_BUTTON_WEST: return "PAD WEST";
	case SDL_GAMEPAD_BUTTON_NORTH: return "PAD NORTH";
	case SDL_GAMEPAD_BUTTON_BACK: return "PAD BACK";
	case SDL_GAMEPAD_BUTTON_GUIDE: return "PAD GUIDE";
	case SDL_GAMEPAD_BUTTON_START: return "PAD START";
	case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "LEFT STICK";
	case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "RIGHT STICK";
	case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LEFT SHOULDER";
	case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RIGHT SHOULDER";
	case SDL_GAMEPAD_BUTTON_DPAD_UP: return "DPAD UP";
	case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "DPAD DOWN";
	case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "DPAD LEFT";
	case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "DPAD RIGHT";
	case SDL_GAMEPAD_BUTTON_MISC1: return "PAD MISC 1";
	case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "RIGHT PADDLE 1";
	case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "LEFT PADDLE 1";
	case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "RIGHT PADDLE 2";
	case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "LEFT PADDLE 2";
	case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "TOUCHPAD";
	case SDL_GAMEPAD_BUTTON_MISC2: return "PAD MISC 2";
	case SDL_GAMEPAD_BUTTON_MISC3: return "PAD MISC 3";
	case SDL_GAMEPAD_BUTTON_MISC4: return "PAD MISC 4";
	case SDL_GAMEPAD_BUTTON_MISC5: return "PAD MISC 5";
	case SDL_GAMEPAD_BUTTON_MISC6: return "PAD MISC 6";
	default: return nullptr;
	}
}

void joystick_control_text(
	std::int16_t control,
	char* output,
	std::size_t capacity)
{
	if (control >= kControlJoystickHatBase
		&& control < kControlJoystickLeftTrigger)
	{
		constexpr const char* directions[] = {"UP", "RIGHT", "DOWN", "LEFT"};
		const std::int16_t offset = control - kControlJoystickHatBase;
		std::snprintf(
			output,
			capacity,
			"HAT %d %s",
			offset / 4 + 1,
			directions[offset % 4]);
		return;
	}
	if (control == kControlJoystickLeftTrigger)
	{
		std::snprintf(output, capacity, "LEFT TRIGGER");
		return;
	}
	if (control == kControlJoystickRightTrigger)
	{
		std::snprintf(output, capacity, "RIGHT TRIGGER");
		return;
	}
	const char* gamepad = gamepad_button_name(control);
	if (gamepad != nullptr)
	{
		std::snprintf(output, capacity, "%s / JOY %d", gamepad, control);
		return;
	}
	std::snprintf(output, capacity, "JOY BUTTON %d", control);
}

void mouse_control_text(
	std::int16_t control,
	char* output,
	std::size_t capacity)
{
	switch (control)
	{
	case SDL_BUTTON_LEFT:
		std::snprintf(output, capacity, "LEFT MOUSE");
		return;
	case SDL_BUTTON_MIDDLE:
		std::snprintf(output, capacity, "MIDDLE MOUSE");
		return;
	case SDL_BUTTON_RIGHT:
		std::snprintf(output, capacity, "RIGHT MOUSE");
		return;
	case SDL_BUTTON_X1:
		std::snprintf(output, capacity, "MOUSE X1");
		return;
	case SDL_BUTTON_X2:
		std::snprintf(output, capacity, "MOUSE X2");
		return;
	case kControlMouseWheelUp:
		std::snprintf(output, capacity, "WHEEL UP");
		return;
	case kControlMouseWheelDown:
		std::snprintf(output, capacity, "WHEEL DOWN");
		return;
	case kControlMouseWheelLeft:
		std::snprintf(output, capacity, "WHEEL LEFT");
		return;
	case kControlMouseWheelRight:
		std::snprintf(output, capacity, "WHEEL RIGHT");
		return;
	default:
		std::snprintf(output, capacity, "MOUSE %d", control);
		return;
	}
}

void append_control_text(
	char* output,
	std::size_t capacity,
	const char* text)
{
	const std::size_t used = std::strlen(output);
	if (used >= capacity)
	{
		return;
	}
	std::snprintf(
		output + used,
		capacity - used,
		"%s%s",
		used == 0 ? "" : " / ",
		text);
}
}

void controls_defaults(Config& config)
{
	config.force_feedback = true;
	config.invert_pitch = true;
	config.hat_control = true;
	config.twist_control = false;
	config.controller = 0;
	for (std::uint32_t index = 0; index < kControlActionCount; ++index)
	{
		config.bindings[index].scancode = static_cast<std::uint16_t>(
			named_scancode(kControlActions[index].default_key));
		config.bindings[index].modifier =
			kControlActions[index].default_modifier;
		config.bindings[index].joystick_control =
			kControlActions[index].default_joystick_control;
		config.bindings[index].mouse_control =
			kControlActions[index].default_mouse_control;
	}
}

void control_binding_text(
	const ControlBinding& binding,
	char* output,
	std::size_t capacity)
{
	if (capacity == 0)
	{
		return;
	}
	const char* key = binding.scancode == SDL_SCANCODE_UNKNOWN
		? nullptr
		: SDL_GetScancodeName(
			static_cast<SDL_Scancode>(binding.scancode));
	output[0] = '\0';
	if (key != nullptr && *key != '\0')
	{
		char keyboard[96];
		const char* modifier = "";
		if (binding.modifier == 1) modifier = "SHIFT + ";
		else if (binding.modifier == 2) modifier = "CONTROL + ";
		else if (binding.modifier == 3) modifier = "ALT + ";
		std::snprintf(keyboard, sizeof(keyboard), "%s%s", modifier, key);
		append_control_text(output, capacity, keyboard);
	}
	if (binding.joystick_control >= 0)
	{
		char joystick[64];
		joystick_control_text(
			binding.joystick_control, joystick, sizeof(joystick));
		append_control_text(output, capacity, joystick);
	}
	if (binding.mouse_control >= 0)
	{
		char mouse[32];
		mouse_control_text(binding.mouse_control, mouse, sizeof(mouse));
		append_control_text(output, capacity, mouse);
	}
	if (output[0] == '\0')
	{
		std::snprintf(output, capacity, "! NOT ASSIGNED !");
	}
}

const char* control_binding_key_text(
	const ControlBinding& binding,
	std::uint32_t action)
{
	if (binding.scancode == SDL_SCANCODE_UNKNOWN)
	{
		return "";
	}
	if (action < kControlActionCount
		&& binding.scancode == named_scancode(
			kControlActions[action].default_key)
		&& binding.modifier == kControlActions[action].default_modifier)
	{
		return kControlActions[action].default_key;
	}
	const char* retail_name = supported_key_name(binding.scancode);
	return *retail_name != '\0'
		? retail_name
		: SDL_GetScancodeName(
			static_cast<SDL_Scancode>(binding.scancode));
}

void control_bind_key(
	Config& config,
	std::uint32_t action,
	std::uint16_t scancode,
	std::uint8_t modifier)
{
	if (action >= kControlActionCount)
	{
		return;
	}
	for (std::uint32_t index = 0; index < kControlActionCount; ++index)
	{
		if (index != action
			&& config.bindings[index].scancode == scancode
			&& config.bindings[index].modifier == modifier)
		{
			config.bindings[index].scancode = SDL_SCANCODE_UNKNOWN;
			config.bindings[index].modifier = 0;
			break;
		}
	}
	config.bindings[action].scancode = scancode;
	config.bindings[action].modifier = modifier;
}

void control_bind_joystick(
	Config& config,
	std::uint32_t action,
	std::int16_t control)
{
	if (action >= kControlActionCount)
	{
		return;
	}
	for (std::uint32_t index = 0; index < kControlActionCount; ++index)
	{
		if (index != action
			&& config.bindings[index].joystick_control == control)
		{
			config.bindings[index].joystick_control = -1;
			break;
		}
	}
	config.bindings[action].joystick_control = control;
}

void control_bind_mouse(
	Config& config,
	std::uint32_t action,
	std::int16_t control)
{
	if (action >= kControlActionCount)
	{
		return;
	}
	for (std::uint32_t index = 0; index < kControlActionCount; ++index)
	{
		if (index != action
			&& config.bindings[index].mouse_control == control)
		{
			config.bindings[index].mouse_control = -1;
			break;
		}
	}
	config.bindings[action].mouse_control = control;
}

std::int16_t control_joystick_hat(
	std::uint8_t hat,
	std::uint8_t direction)
{
	if (hat >= kControlJoystickHatCount || direction >= 4)
	{
		return -1;
	}
	return static_cast<std::int16_t>(
		kControlJoystickHatBase + hat * 4 + direction);
}
}
