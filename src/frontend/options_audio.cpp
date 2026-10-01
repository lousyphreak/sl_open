#include "frontend/options_audio.hpp"

#include "frontend/gui_render.hpp"
#include "frontend/options_render_internal.hpp"
#include "render/frontend_renderer.hpp"

namespace sl_open::frontend
{
namespace
{
constexpr gui::Rect kRegions[] = {
	{300, 119, 205, 36},
	{300, 179, 205, 36},
	{300, 239, 205, 36},
	{300, 299, 205, 36},
	{300, 369, 19, 26},
	{322, 369, 19, 26},
	{390, 414, 115, 27},
	{340, 414, 45, 27},
	{340, 434, 85, 27},
	{425, 434, 135, 27},
};

using options_render::draw_label;

std::uint8_t pointer_volume(float x)
{
	const float clamped = x < 313.0f ? 313.0f : (x > 488.0f ? 488.0f : x);
	return static_cast<std::uint8_t>(
		(clamped - 313.0f) * 127.0f / 175.0f + 0.5f);
}

void set_audio_volume(Config& config, std::int32_t index, std::uint8_t value)
{
	switch (index)
	{
	case 0: config.speech_volume = value; break;
	case 1: config.effects_volume = value; break;
	case 2: config.music_volume = value; break;
	case 3: config.master_volume = value; break;
	default: break;
	}
}

void draw_audio_slider(
	render::FrontendCommands& commands,
	const render::FrontendRenderer& renderer,
	const char* label,
	float y,
	std::uint8_t value,
	bool selected)
{
	draw_label(commands, renderer, label, 90.0f, y - 4.0f, selected, 0.72f);
	gui::slider(
		commands,
		renderer.white,
		313.0f,
		y,
		175.0f,
		value / 127.0f,
		selected);
}

}

void options_audio_enter(OptionsMenu& menu, const Config& config)
{
	menu.audio_snapshot[0] = config.effects_volume;
	menu.audio_snapshot[1] = config.music_volume;
	menu.audio_snapshot[2] = config.speech_volume;
	menu.audio_snapshot[3] = config.master_volume;
	menu.positional_audio_snapshot = config.positional_audio;
}

void options_audio_set_pointer(OptionsMenu& menu, float x, float y)
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

OptionsSelection options_audio_select(
	OptionsMenu& menu,
	Config& config,
	bool hrtf_supported)
{
	if (menu.hovered >= 0 && menu.hovered < 4)
	{
		set_audio_volume(
			config, menu.hovered, pointer_volume(menu.pointer_x));
		return OptionsSelection::changed;
	}
	if (menu.hovered == 4 || menu.hovered == 5)
	{
		int mode = static_cast<int>(config.positional_audio);
		const int step = menu.hovered == 4 ? -1 : 1;
		do
		{
			mode = (mode + step + 3) % 3;
		}
		while (mode == static_cast<int>(PositionalAudio::hrtf)
			&& !hrtf_supported);
		config.positional_audio = static_cast<PositionalAudio>(mode);
		return OptionsSelection::changed;
	}
	if (menu.hovered == 6)
	{
		config.effects_volume = 80;
		config.music_volume = 80;
		config.speech_volume = 127;
		config.master_volume = 127;
		config.positional_audio = PositionalAudio::standard;
		return OptionsSelection::changed;
	}
	if (menu.hovered == 7) return OptionsSelection::options;
	if (menu.hovered == 8) return OptionsSelection::main_menu;
	if (menu.hovered == 9)
	{
		config.effects_volume = menu.audio_snapshot[0];
		config.music_volume = menu.audio_snapshot[1];
		config.speech_volume = menu.audio_snapshot[2];
		config.master_volume = menu.audio_snapshot[3];
		config.positional_audio = menu.positional_audio_snapshot;
		return OptionsSelection::changed;
	}
	return OptionsSelection::none;
}

bool options_audio_drag(OptionsMenu& menu, Config& config)
{
	if (menu.hovered < 0 || menu.hovered >= 4)
	{
		return false;
	}
	set_audio_volume(config, menu.hovered, pointer_volume(menu.pointer_x));
	return true;
}

void options_audio_build(
	const OptionsMenu& menu,
	const render::FrontendRenderer& renderer,
	const Config& config,
	bool hrtf_supported,
	render::FrontendCommands& commands)
{
	draw_label(
		commands, renderer, "SOUND CONFIGURATION", 185.0f, 75.0f, false);
	draw_audio_slider(
		commands, renderer, "SPEECH VOLUME", 126.0f,
		config.speech_volume, menu.hovered == 0);
	draw_audio_slider(
		commands, renderer, "SOUND EFFECTS VOLUME", 186.0f,
		config.effects_volume, menu.hovered == 1);
	draw_audio_slider(
		commands, renderer, "MUSIC VOLUME", 246.0f,
		config.music_volume, menu.hovered == 2);
	draw_audio_slider(
		commands, renderer, "MASTER VOLUME", 306.0f,
		config.master_volume, menu.hovered == 3);

	const char* provider = "STANDARD POSITIONAL AUDIO";
	if (config.positional_audio == PositionalAudio::off)
	{
		provider = "OFF";
	}
	else if (config.positional_audio == PositionalAudio::hrtf)
	{
		provider = hrtf_supported ? "HRTF" : "HRTF (UNAVAILABLE)";
	}
	draw_label(commands, renderer, "POSITIONAL AUDIO", 90.0f, 365.0f, false, 0.72f);
	draw_label(commands, renderer, "<", 302.0f, 367.0f, menu.hovered == 4);
	draw_label(commands, renderer, ">", 324.0f, 367.0f, menu.hovered == 5);
	draw_label(commands, renderer, provider, 360.0f, 369.0f, false, 0.62f);

	draw_label(
		commands, renderer, "OK", 350.0f, 420.0f, menu.hovered == 7, 0.58f);
	draw_label(
		commands, renderer, "RESET DEFAULTS", 395.0f, 420.0f,
		menu.hovered == 6, 0.58f);
	draw_label(
		commands, renderer, "MAIN MENU", 350.0f, 440.0f,
		menu.hovered == 8, 0.58f);
	draw_label(
		commands, renderer, "CANCEL CHANGES", 430.0f, 440.0f,
		menu.hovered == 9, 0.58f);
}
}
