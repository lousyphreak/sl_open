#include "game/player_controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sl_open::game
{
namespace
{
constexpr float kKeyboardDemandStep = 0x1.333334p-2f;
constexpr std::int32_t kMouseDisplacementLimit = 800;
constexpr float kMouseDeadZone = 0x1.333334p-2f;
constexpr float kThrottleDemandStep = 0x1.47ae14p-6f;
constexpr float kJoystickDeadZone = 20.0f / 10000.0f;
constexpr float kMouseInterfaceScale = 64.0f / 800.0f;
constexpr float kModernMouseDemandScale = 1.0f / 32.0f;

float bipolar_axis(std::int16_t value)
{
	const float normalized = value < 0
		? static_cast<float>(value) / 32768.0f
		: static_cast<float>(value) / 32767.0f;
	// Joystick enumeration sets DirectInput DIPROP_DEADZONE to 20, whose
	// documented scale is 0..10,000. Values in that central 0.2 percent
	// therefore report the configured axis center in the executable.
	return std::abs(normalized) <= kJoystickDeadZone
		? 0.0f
		: normalized;
}

float throttle_axis(std::int16_t value)
{
	const float position =
		static_cast<float>(
			static_cast<std::int32_t>(value) + 32768)
		/ 65535.0f;
	return 1.0f - position;
}

float mouse_axis(std::int16_t displacement)
{
	const float normalized =
		static_cast<float>(displacement)
		/ static_cast<float>(kMouseDisplacementLimit);
	if (normalized >= kMouseDeadZone)
	{
		return (kMouseDeadZone + 1.0f) * normalized
			- kMouseDeadZone;
	}
	if (normalized <= -kMouseDeadZone)
	{
		return (kMouseDeadZone + 1.0f) * normalized
			+ kMouseDeadZone;
	}
	return 0.0f;
}

float digital_roll(const input::GameplayInput& input)
{
	if (input.held[29])
	{
		return 1.0f;
	}
	if (input.held[30])
	{
		return -1.0f;
	}
	return 0.0f;
}

void update_keyboard_rotation(
	const Config& config,
	const input::GameplayInput& input,
	std::int16_t camera_mode,
	FlightDemand& demand)
{
	demand.roll = digital_roll(input);
	// Target and external views consume the fixed cursor keys as camera
	// orbit input. The keyboard-controller branch in Player_update_controls
	// explicitly clears only ship pitch and yaw in those two modes.
	if (camera_mode == 6 || camera_mode == 12)
	{
		demand.pitch = 0.0f;
		demand.yaw = 0.0f;
		return;
	}
	const float pitch_sign = config.invert_pitch ? 1.0f : -1.0f;
	if (input.held[33])
	{
		demand.yaw -= kKeyboardDemandStep;
	}
	else if (input.held[34])
	{
		demand.yaw += kKeyboardDemandStep;
	}
	else
	{
		demand.yaw = 0.0f;
	}
	if (input.held[31])
	{
		demand.pitch += pitch_sign * kKeyboardDemandStep;
	}
	else if (input.held[32])
	{
		demand.pitch -= pitch_sign * kKeyboardDemandStep;
	}
	else
	{
		demand.pitch = 0.0f;
	}
}

void update_mouse_rotation(
	const Config& config,
	const input::GameplayInput& input,
	PlayerControlState& state,
	FlightDemand& demand)
{
	const float accumulated_x =
		state.mouse_fraction_x + input.mouse_relative_x;
	const float accumulated_y =
		state.mouse_fraction_y + input.mouse_relative_y;
	const std::int32_t relative_x =
		static_cast<std::int32_t>(std::lround(accumulated_x));
	const std::int32_t relative_y =
		static_cast<std::int32_t>(std::lround(accumulated_y));
	state.mouse_fraction_x =
		accumulated_x - static_cast<float>(relative_x);
	state.mouse_fraction_y =
		accumulated_y - static_cast<float>(relative_y);
	const std::int32_t next_x =
		static_cast<std::int32_t>(state.mouse_displacement_x)
		+ relative_x;
	const std::int32_t next_y =
		static_cast<std::int32_t>(state.mouse_displacement_y)
		+ relative_y;
	state.mouse_displacement_x = static_cast<std::int16_t>(
		std::clamp(
			next_x,
			-kMouseDisplacementLimit,
			kMouseDisplacementLimit));
	state.mouse_displacement_y = static_cast<std::int16_t>(
		std::clamp(
			next_y,
			-kMouseDisplacementLimit,
			kMouseDisplacementLimit));

	const float pitch_sign = config.invert_pitch ? 1.0f : -1.0f;
	demand.roll = digital_roll(input);
	demand.pitch =
		-mouse_axis(state.mouse_displacement_y) * pitch_sign;
	demand.yaw = mouse_axis(state.mouse_displacement_x);
}

void update_modern_mouse_rotation(
	const Config& config,
	const input::GameplayInput& input,
	PlayerControlState& state,
	FlightDemand& demand)
{
	// Unlike the retail mouse controller, this mode treats each relative
	// motion packet as rotation input and retains no virtual joystick offset.
	state = {};
	const float pitch_sign = config.invert_pitch ? 1.0f : -1.0f;
	demand.roll = digital_roll(input);
	demand.pitch = std::clamp(
		-input.mouse_relative_y * kModernMouseDemandScale * pitch_sign,
		-1.0f,
		1.0f);
	demand.yaw = std::clamp(
		input.mouse_relative_x * kModernMouseDemandScale,
		-1.0f,
		1.0f);
}

void update_joystick_rotation(
	const Config& config,
	const input::GameplayInput& input,
	FlightDemand& demand)
{
	const float pitch_sign = config.invert_pitch ? 1.0f : -1.0f;
	const float horizontal =
		input.joystick_axis_available[0]
			? bipolar_axis(input.joystick_axes[0])
			: 0.0f;
	const float vertical =
		input.joystick_axis_available[1]
			? bipolar_axis(input.joystick_axes[1])
			: 0.0f;
	demand.pitch = vertical * pitch_sign;
	demand.yaw = 0.0f;
	demand.roll = 0.0f;
	if (config.twist_control
		&& input.joystick_axis_available[5])
	{
		demand.yaw = horizontal;
		demand.roll = bipolar_axis(input.joystick_axes[5]);
	}
	else if (input.held[37])
	{
		demand.roll = horizontal;
	}
	else
	{
		demand.yaw = horizontal;
		demand.roll = digital_roll(input);
	}
}

void update_keyboard_throttle(
	const input::GameplayInput& input,
	float& throttle)
{
	if (input.held[25])
	{
		throttle += kThrottleDemandStep;
	}
	else if (input.held[26])
	{
		throttle -= kThrottleDemandStep;
	}
	if (input.pressed[27])
	{
		throttle = 0.0f;
	}
	if (input.pressed[28])
	{
		throttle = 1.0f;
	}
	throttle = std::clamp(throttle, 0.0f, 1.0f);
}

PlayerControlAxes capture_interface_axes(
	const Config& config,
	const input::GameplayInput& input)
{
	PlayerControlAxes axes;
	switch (config.controller)
	{
	case 0:
		axes.horizontal =
			input.joystick_axis_available[0]
				? bipolar_axis(input.joystick_axes[0])
				: 0.0f;
		axes.vertical =
			input.joystick_axis_available[1]
				? bipolar_axis(input.joystick_axes[1])
				: 0.0f;
		break;
	case kControllerMouse:
	case kControllerModernMouse:
		// The three interface-owned branches at 0x00413841 consume the
		// immediate relative mouse values in the radius-64 HUD domain;
		// they do not reuse the retained ±800 flight displacement.
		axes.horizontal =
			input.mouse_relative_x * kMouseInterfaceScale;
		axes.vertical =
			input.mouse_relative_y * kMouseInterfaceScale;
		break;
	case 2:
	default:
		axes.horizontal =
			static_cast<float>(input.held[34])
				- static_cast<float>(input.held[33]);
		axes.vertical =
			static_cast<float>(input.held[32])
				- static_cast<float>(input.held[31]);
		break;
	}
	return axes;
}
}

PlayerControlAxes player_controls_update_flight_demand(
	const Config& config,
	const input::GameplayInput& input,
	PlayerControlState& state,
	FlightDemand& demand,
	float& manual_throttle,
	std::int16_t camera_mode,
	bool interface_owns_rotation)
{
	PlayerControlAxes interface_axes;
	if (interface_owns_rotation)
	{
		interface_axes = capture_interface_axes(config, input);
		demand.roll = 0.0f;
		demand.pitch = 0.0f;
		demand.yaw = 0.0f;
	}
	else
	{
		switch (config.controller)
		{
		case kControllerJoystick:
			update_joystick_rotation(config, input, demand);
			break;
		case kControllerMouse:
			update_mouse_rotation(config, input, state, demand);
			break;
		case kControllerModernMouse:
			update_modern_mouse_rotation(config, input, state, demand);
			break;
		case kControllerKeyboard:
		default:
			update_keyboard_rotation(
				config, input, camera_mode, demand);
			break;
		}
		demand.roll += demand.yaw * 0.5f;
	}

	demand.strafe = 0.0f;
	if (input.held[35])
	{
		demand.strafe = -1.0f;
	}
	if (input.held[36])
	{
		// The second retail query writes last when both bindings are held.
		demand.strafe = 1.0f;
	}
	// The three retail axis-capture branches join the common strafe path
	// below 0x00413a7e, but do not pass the throttle helper at 0x004132c0.
	if (interface_owns_rotation)
	{
		return interface_axes;
	}
	if (config.controller == kControllerJoystick
		&& input.joystick_axis_available[2])
	{
		demand.throttle = throttle_axis(input.joystick_axes[2]);
	}
	else if (config.controller == kControllerJoystick
		&& input.joystick_axis_available[6])
	{
		demand.throttle = throttle_axis(input.joystick_axes[6]);
	}
	else
	{
		// Player_update_throttle retains the digital target separately in
		// DAT_0051cf7c. Analog throttle axes write only GameObject+0x5b8;
		// switching back to digital control therefore resumes this value.
		update_keyboard_throttle(input, manual_throttle);
		demand.throttle = manual_throttle;
	}
	return interface_axes;
}
}
