#pragma once

#include "game/world.hpp"
#include "mission/dte.hpp"
#include "mission/runtime.hpp"

#include <cstdint>

namespace sl_open::mission
{
void director_enqueue(
	Runtime& runtime,
	game::World& world,
	const DteFile& file,
	const DirectorShot& shot,
	bool clear,
	std::uint32_t initial_steps);
void director_stop(
	Runtime& runtime,
	game::World& world,
	const DteFile& file);
void director_camera_replaced(
	Runtime& runtime,
	game::World& world);
void director_service(
	Runtime& runtime,
	game::World& world,
	const DteFile& file,
	std::uint32_t simulation_steps);
}
