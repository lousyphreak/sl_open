#pragma once

#include "frontend/gui.hpp"

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
constexpr gui::Rect kQuitDialogRegions[] = {
	{286, 269, 25, 16},
	{326, 269, 25, 16},
};

void quit_dialog_build(
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::int32_t hovered);
}
