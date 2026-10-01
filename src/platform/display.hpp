#pragma once

#include "platform/app_internal.hpp"

namespace sl_open::platform
{
bool display_start_error(App& app, StartupError error);
void display_draw_startup_error(App& app);
bool display_create(App& app);
bool display_apply_config(App& app);
void display_resize(App& app, int width, int height);
void display_add_resolution(
	frontend::DisplayModes& modes,
	std::uint32_t width,
	std::uint32_t height);
void display_shutdown(App& app);
}
