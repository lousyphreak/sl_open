#pragma once

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
struct MainMenu
{
	float pointer_x{320.0f};
	float pointer_y{200.0f};
	std::int32_t hovered{-1};
	std::uint64_t entered_at{};
	bool quit_confirmation{};
};

void main_menu_set_pointer(MainMenu& menu, float x, float y, bool inside);
void main_menu_build(
	const MainMenu& menu,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now);
}
