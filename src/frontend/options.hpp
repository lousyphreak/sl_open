#pragma once

#include "config/config.hpp"

#include <cstdint>

namespace sl_open
{
struct LanguageTable;
}

namespace sl_open::render
{
struct FrontendCommands;
struct FrontendRenderer;
}

namespace sl_open::frontend
{
enum class OptionsSelection : std::uint8_t
{
	none,
	changed,
	audio,
	controls,
	video,
	options,
	load,
	save,
	back,
	apply_options,
	apply_main_menu,
	main_menu,
	quit,
};

enum class OptionsPage : std::uint8_t
{
	hub,
	audio,
	controls,
	video,
};

enum class OptionsModal : std::uint8_t
{
	none,
	about,
	quit_confirmation,
};

struct DisplayResolution
{
	std::uint32_t width{};
	std::uint32_t height{};
};

constexpr std::uint32_t kMaxDisplayResolutions = 128;

struct DisplayModes
{
	// Display enumeration is cold startup work; a fixed list avoids making the
	// options screen own a dynamically sized container.
	DisplayResolution items[kMaxDisplayResolutions]{};
	std::uint32_t count{};
};

struct OptionsMenu
{
	float pointer_x{320.0f};
	float pointer_y{200.0f};
	std::int32_t hovered{-1};
	std::uint64_t entered_at{};
	std::uint8_t audio_snapshot[4]{};
	std::uint32_t video_width_snapshot{};
	std::uint32_t video_height_snapshot{};
	std::uint8_t video_snapshot[3]{};
	std::uint8_t brightness_snapshot{};
	std::uint8_t controller_snapshot{};
	DisplayMode display_mode_snapshot{DisplayMode::windowed};
	PositionalAudio positional_audio_snapshot{PositionalAudio::standard};
	bool video_flags_snapshot[5]{};
	bool control_flags_snapshot[4]{};
	ControlBinding control_snapshot[kControlActionCount]{};
	std::uint32_t first_control_row{};
	std::int32_t capture_action{-1};
	std::uint64_t control_wheel_suppressed_until{};
	OptionsPage page{OptionsPage::hub};
	OptionsModal modal{OptionsModal::none};
	bool in_game{};
};

void options_enter_page(
	OptionsMenu& menu,
	OptionsPage page,
	const Config& config,
	std::uint64_t now);
void options_set_pointer(OptionsMenu& menu, float x, float y, bool inside);
OptionsSelection options_select(
	OptionsMenu& menu,
	Config& config,
	const DisplayModes& modes,
	bool hrtf_supported,
	bool joystick_available);
bool options_drag(OptionsMenu& menu, Config& config);
bool options_capture_key(
	OptionsMenu& menu,
	Config& config,
	std::uint16_t scancode,
	std::uint8_t modifier);
bool options_capture_joystick(
	OptionsMenu& menu,
	Config& config,
	std::int16_t control);
bool options_capture_mouse(
	OptionsMenu& menu,
	Config& config,
	std::int16_t control);
void options_scroll_controls(OptionsMenu& menu, int direction);
void options_build(
	const OptionsMenu& menu,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	const Config& config,
	bool hrtf_supported,
	bool joystick_available,
	render::FrontendCommands& commands,
	std::uint64_t now);
}
