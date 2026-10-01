#pragma once

#include "ai/types.hpp"
#include "assets/gun_stats.hpp"
#include "assets/missile_stats.hpp"
#include "assets/pilot_stats.hpp"
#include "assets/ship_stats.hpp"
#include "game/chaff.hpp"
#include "game/missiles.hpp"
#include "game/weapons.hpp"
#include "game/world.hpp"
#include "mission/runtime.hpp"

#include <cstdint>

namespace sl_open::ai
{
bool command_push(
	game::World& world,
	game::WorldObject& object,
	std::int16_t command_id,
	TargetKind target_kind,
	std::uint16_t target,
	std::int16_t target_component = -1,
	std::int16_t selector = 0,
	std::int16_t sequence = 0,
	bool* inserted = nullptr);
bool command_defer(
	game::WorldObject& object,
	const Command& command,
	std::uint32_t current_tick,
	std::uint32_t delay_ticks,
	std::uint8_t publication_seed = 0);
bool command_pop(game::World& world, game::WorldObject& object);
void command_clear(game::World& world, game::WorldObject& object);
// GameObject_mark_destroyed, LANCER.EXE 0x00401f00, asks the active
// command for its command-11 replacement cleanup and then drops the complete
// live queue without popping queued records or resetting the shared work.
void command_mark_destroyed(
	game::World& world,
	game::WorldObject& object);
bool command_try_clear_fast(
	game::World& world,
	game::WorldObject& object);
bool command_signal_launch(game::WorldObject& object);
bool command_is_jump_or_launch(const game::WorldObject& object);
bool command_owns_flight(const game::WorldObject& object);
bool command_has_positive_priority(const game::WorldObject& object);
// Command-definition flag 0x400 is consumed by retail's continuous object
// state publisher. It forces the motion/control section into opcode 0x1d
// even when neither retained transform-publication bit is set.
bool command_requires_network_motion(
	const game::WorldObject& object);
void promote_deferred_commands(
	game::World& world,
	game::WorldObject& object,
	std::uint32_t simulation_tick);
// AI_schedule_death_command, LANCER.EXE 0x00401f30. `cause` is copied to
// queued command state byte zero; `force_explode_player` is the retail third
// argument which bypasses the local-player Eject Player path.
bool schedule_death_command(
	game::WorldObject& object,
	game::World& world,
	mission::Runtime& mission,
	std::uint8_t cause,
	bool force_explode_player);
game::FlightDemand runtime_steer_toward_point(
	const game::WorldObject& actor,
	const glm::vec3& target,
	float throttle,
	float maximum_control,
	float response_retention,
	std::uint32_t options,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_delta);
bool runtime_approach_point(
	const game::WorldObject& actor,
	const glm::vec3& target,
	const glm::mat3& target_basis,
	float minimum_throttle,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_delta,
	game::FlightDemand& demand);
bool runtime_service_object_command(
	game::WorldObject& object,
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	const assets::PilotStatsTable& pilot_stats,
	const assets::GunStatsTable& gun_stats,
	const assets::MissileStatsTable& missile_stats,
	game::WeaponRuntime& weapons,
	game::MissileRuntime& missiles,
	game::ChaffRuntime& chaff,
	const game::FlightDemand& player_demand,
	std::uint32_t& random_seed,
	std::uint32_t simulation_tick);

void runtime_tick(
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	const assets::PilotStatsTable& pilot_stats,
	const assets::GunStatsTable& gun_stats,
	const assets::MissileStatsTable& missile_stats,
	game::WeaponRuntime& weapons,
	game::MissileRuntime& missiles,
	game::ChaffRuntime& chaff,
	const game::FlightDemand& player_demand,
	std::uint32_t& random_seed,
	std::uint32_t simulation_tick);
}
