#pragma once

#include "io/vfs.hpp"

#include <cstdint>

namespace sl_open
{
enum class DisplayMode : std::uint8_t
{
	windowed,
	borderless,
	fullscreen,
};

enum class PositionalAudio : std::uint8_t
{
	off,
	standard,
	hrtf,
};

constexpr std::uint32_t kControlActionCount = 74;

constexpr std::uint8_t kControllerJoystick = 0;
constexpr std::uint8_t kControllerMouse = 1;
constexpr std::uint8_t kControllerKeyboard = 2;
constexpr std::uint8_t kControllerModernMouse = 3;

constexpr std::uint8_t kControlJoystickButtonCount = 64;
constexpr std::uint8_t kControlJoystickHatCount = 8;
constexpr std::int16_t kControlJoystickHatBase =
	kControlJoystickButtonCount;
constexpr std::int16_t kControlJoystickLeftTrigger =
	kControlJoystickHatBase + kControlJoystickHatCount * 4;
constexpr std::int16_t kControlJoystickRightTrigger =
	kControlJoystickLeftTrigger + 1;

constexpr std::uint8_t kControlMouseButtonCount = 32;
constexpr std::int16_t kControlMouseWheelUp =
	kControlMouseButtonCount + 1;
constexpr std::int16_t kControlMouseWheelDown =
	kControlMouseWheelUp + 1;
constexpr std::int16_t kControlMouseWheelLeft =
	kControlMouseWheelDown + 1;
constexpr std::int16_t kControlMouseWheelRight =
	kControlMouseWheelLeft + 1;

constexpr bool controller_uses_mouse(std::uint8_t controller)
{
	return controller == kControllerMouse
		|| controller == kControllerModernMouse;
}

struct ControlBinding
{
	std::uint16_t scancode{};
	std::uint8_t modifier{};
	std::int16_t joystick_control{-1};
	std::int16_t mouse_control{-1};
};

struct Config
{
	// Retail persists Multiplayer/IPAddress and restores it when the
	// direct-connect editor is opened again. The native transport accepts
	// dotted IPv4 text, including the terminating NUL.
	char multiplayer_address[16]{};
	std::uint32_t display_width{};
	std::uint32_t display_height{};
	std::uint8_t effects_volume{};
	std::uint8_t music_volume{};
	std::uint8_t speech_volume{};
	std::uint8_t master_volume{};
	std::uint8_t texture_detail{};
	std::uint8_t graphics_detail{};
	std::uint8_t default_view{};
	std::uint8_t brightness{};
	std::uint8_t controller{};
	DisplayMode display_mode{DisplayMode::windowed};
	PositionalAudio positional_audio{PositionalAudio::standard};
	bool vsync{};
	bool light_maps{};
	bool transitions{};
	bool expand_widescreen_movies{};
	bool pause_in_background{};
	bool force_feedback{};
	bool invert_pitch{};
	bool hat_control{};
	bool twist_control{};
	ControlBinding bindings[kControlActionCount]{};
};

void config_defaults(Config& config);
bool config_load(SDL_EMFS_Context* filesystem, Config& config);
bool config_save(SDL_EMFS_Context* filesystem, const Config& config);
}
