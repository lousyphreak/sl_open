#pragma once

#include "ai/types.hpp"
#include "game/world.hpp"
#include "mission/dte.hpp"
#include "mission/runtime.hpp"

#include <cstdint>

namespace sl_open::assets
{
struct ShipStatsTable;
}

namespace sl_open::ai
{
bool scripted_command_begin(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick);

bool scripted_command_update(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	const game::FlightDemand& player_demand,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight);

// Dispatch the indirect AI-owned callback stored at GameObject+0x640. The
// common object integrator supplies the retained physics snapshot and commits
// the velocity/angular step produced here at its 25 Hz service boundary.
bool scripted_flight_callback(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick);

void scripted_mission_frame_dispatch(
	game::World& world,
	mission::Runtime& mission);

void friendly_fire_flag_offender(
	game::WorldObject& offender,
	mission::Runtime& mission,
	bool promote_all_players);
void friendly_fire_accumulate_damage(
	game::World& world,
	mission::Runtime& mission,
	const game::WorldObject& victim,
	float damage,
	std::uint32_t tick);
}
