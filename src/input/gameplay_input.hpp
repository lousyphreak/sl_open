#pragma once

#include "config/config.hpp"

#include <SDL3/SDL.h>

#include <cstdint>

namespace sl_open::input
{
constexpr std::size_t kGameplayJoystickAxisCount = 8;

struct GameplayDeviceState
{
	bool keyboard[SDL_SCANCODE_COUNT]{};
	// These arrays are the inverse of the retail per-physical-control
	// pressed latches: true means that the current down transition has not
	// yet been accepted by a pressed query. They deliberately survive
	// frames where an input owner is gated off.
	bool keyboard_pressed[SDL_SCANCODE_COUNT]{};
	bool joystick_buttons[kControlJoystickButtonCount]{};
	bool joystick_pressed[kControlJoystickButtonCount]{};
	std::uint8_t joystick_hats[kControlJoystickHatCount]{};
	std::uint8_t joystick_hat_pressed[kControlJoystickHatCount]{};
	bool joystick_hat_available[kControlJoystickHatCount]{};
	bool gamepad_triggers[2]{};
	bool gamepad_trigger_pressed[2]{};
	bool mouse_buttons[kControlMouseButtonCount + 1]{};
	bool mouse_pressed[kControlMouseButtonCount + 1]{};
	std::uint16_t mouse_wheel[4]{};
	// A frontend resume input remains physically held after the menu owner
	// returns. Retain its identity so weapon dispatch can wait for release
	// without falsifying the underlying device state.
	SDL_Scancode fire_release_key{SDL_SCANCODE_UNKNOWN};
	std::uint8_t fire_release_mouse{};
	bool fire_release_mouse_active{};
	bool modifier_latched[3]{};
	std::int16_t joystick_axes[kGameplayJoystickAxisCount]{};
	bool joystick_axis_available[kGameplayJoystickAxisCount]{};
	float mouse_relative_x{};
	float mouse_relative_y{};
};

struct GameplayInput
{
	bool held[kControlActionCount]{};
	bool pressed[kControlActionCount]{};
	// WaitForKey has its own retail polling contract. Unlike ordinary
	// gameplay actions it reads held device state directly, accepts an
	// unmodified key even while modifiers are down, supports only the Shift
	// and Control keyboard modifier branches, and then tries the joystick
	// binding independently.
	bool wait_for_key_accepted[kControlActionCount]{};
	bool comms_option_pressed[8]{};
	bool camera_yaw_negative{};
	bool camera_yaw_positive{};
	bool camera_pitch_positive{};
	bool camera_pitch_negative{};
	bool camera_zoom_in{};
	bool camera_zoom_out{};
	std::int16_t camera_hat_mode{-1};
	std::int16_t joystick_axes[kGameplayJoystickAxisCount]{};
	bool joystick_axis_available[kGameplayJoystickAxisCount]{};
	float mouse_relative_x{};
	float mouse_relative_y{};
	bool powerball_interface{};
	bool shield_balance_interface{};
};

struct GameplayInputPoller
{
	const Config* config{};
	GameplayDeviceState* devices{};
	// The generic retail helper suppresses keyboard DIK 2..9 while the
	// communications menu is fully open (state three). Joystick bindings
	// remain eligible.
	std::uint8_t communications_state{};
};

void gameplay_input_begin_frame(
	const Config& config,
	GameplayDeviceState& devices,
	std::uint8_t communications_state,
	GameplayInputPoller& poller);
void gameplay_input_gate_fire_until_key_released(
	GameplayDeviceState& devices,
	SDL_Scancode scancode);
void gameplay_input_gate_fire_until_mouse_released(
	GameplayDeviceState& devices,
	std::uint8_t button);
bool gameplay_input_fire_release_gated(
	const GameplayInputPoller& poller);
void gameplay_input_set_communications_state(
	GameplayInputPoller& poller,
	std::uint8_t communications_state);
void gameplay_input_capture_analog(
	const GameplayInputPoller& poller,
	GameplayInput& input);
void gameplay_input_capture_wait_for_key(
	const GameplayInputPoller& poller,
	GameplayInput& input);
bool gameplay_input_action_held(
	GameplayInputPoller& poller,
	std::uint8_t action);
bool gameplay_input_action_pressed(
	GameplayInputPoller& poller,
	std::uint8_t action);
bool gameplay_input_raw_keyboard_held(
	GameplayInputPoller& poller,
	SDL_Scancode scancode,
	std::uint8_t modifier);
bool gameplay_input_raw_keyboard_pressed(
	GameplayInputPoller& poller,
	SDL_Scancode scancode,
	std::uint8_t modifier);
void gameplay_input_poll(
	const Config& config,
	GameplayDeviceState& devices,
	const bool* previous,
	GameplayInput& input);
}
