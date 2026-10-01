#pragma once

#include "frontend/options.hpp"

namespace sl_open::frontend
{
void options_audio_enter(OptionsMenu& menu, const Config& config);
void options_audio_set_pointer(OptionsMenu& menu, float x, float y);
OptionsSelection options_audio_select(
	OptionsMenu& menu,
	Config& config,
	bool hrtf_supported);
bool options_audio_drag(OptionsMenu& menu, Config& config);
void options_audio_build(
	const OptionsMenu& menu,
	const render::FrontendRenderer& renderer,
	const Config& config,
	bool hrtf_supported,
	render::FrontendCommands& commands);
}
