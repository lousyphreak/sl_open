#pragma once

#include "frontend/options.hpp"

namespace sl_open::frontend
{
void options_video_enter(OptionsMenu& menu, const Config& config);
void options_video_set_pointer(OptionsMenu& menu, float x, float y);
OptionsSelection options_video_select(
	const OptionsMenu& menu,
	Config& config,
	const DisplayModes& modes);
bool options_video_drag(const OptionsMenu& menu, Config& config);
void options_video_build(
	const OptionsMenu& menu,
	const render::FrontendRenderer& renderer,
	const Config& config,
	render::FrontendCommands& commands);
}
