#pragma once

#include "assets/gun_stats.hpp"
#include "assets/ship_stats.hpp"
#include "game/world.hpp"
#include "hud/runtime.hpp"
#include "mission/runtime.hpp"
#include "render/frontend_renderer.hpp"
#include "render/mission_renderer.hpp"

#include <cstdint>

namespace sl_open
{
struct Config;
struct LanguageTable;
}

namespace sl_open::hud
{
void render(
	Runtime& runtime,
	game::World& world,
	const mission::Runtime& mission,
	const assets::ShipStatsTable& ship_stats,
	const assets::GunStatsTable& gun_stats,
	const Config& config,
	const LanguageTable& language,
	const render::MissionRenderer& mission_renderer,
	const render::MissionRenderFrame& mission_frame,
	render::FrontendRenderer& frontend,
	render::FrontendCommands& commands,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint32_t simulation_tick,
	std::uint16_t mission_number);
}
