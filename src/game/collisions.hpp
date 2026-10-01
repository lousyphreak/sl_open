#pragma once

#include <cstdint>

namespace sl_open::assets
{
struct ShipStatsTable;
}

namespace sl_open::mission
{
struct Runtime;
}

namespace sl_open::game
{
struct World;

// GameObjects_integrate collision phase, LANCER.EXE 0x00468fa0.
void world_service_collisions(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick);
}
