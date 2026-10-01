#pragma once

#include "frontend/options.hpp"

namespace sl_open::frontend
{
void options_controls_enter(OptionsMenu& menu, const Config& config);
void options_controls_set_pointer(OptionsMenu& menu, float x, float y);
OptionsSelection options_controls_select(
	OptionsMenu& menu,
	Config& config,
	bool joystick_available);
void options_controls_build(
	const OptionsMenu& menu,
	const render::FrontendRenderer& renderer,
	const Config& config,
	bool joystick_available,
	render::FrontendCommands& commands);
}
