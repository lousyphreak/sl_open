#pragma once

#include "assets/ship_stats.hpp"
#include "game/world.hpp"
#include "mission/dte.hpp"
#include "mission/executor.hpp"
#include "mission/runtime.hpp"

#include <cstdint>

namespace sl_open::mission
{
enum class EventType : std::uint8_t
{
	shot_at = 0,
	destroyed = 1,
	launched = 2,
	camera_reached = 3,
	ship_reached = 4,
	close_proximity = 5,
	proximity = 6,
	object_scooped = 7,
	player_ready_to_jump = 8,
	jumped_in = 9,
	fixed_gate_jumped_in = 10,
	player_ready_to_warp = 11,
	jumped_through_hoop = 12,
	player_wants_backup = 13,
	ripper_grabbed_object = 14,
	ripper_dropped_object = 15,
	cloaked = 16,
	decloaked = 17,
	targetted = 18,
	player_l1_double_tap = 19,
	player_l2_double_tap = 20,
	player_r1_double_tap = 21,
	player_r2_double_tap = 22,
	player_all_shoulders = 23,
	player_l1_r1 = 24,
	game_timer_expired = 25,
	tractor_locked = 26,
	tractor_broken = 27,
	inside_object = 28,
	outside_object = 29,
	docked = 30,
	undocked = 31,
	being_chased = 32,
	call_reinforcements = 33,
	explosion_ship = 34,
};

constexpr std::uint8_t kEventTypeCount = 35;

std::uint8_t events_argument_count(EventType type);
std::uint8_t events_capture_slot(EventType type);
bool events_emit_direct(
	Runtime& runtime,
	EventType type,
	std::uint16_t source,
	const std::uint32_t* arguments,
	std::uint8_t argument_count,
	std::uint8_t selector = UINT8_MAX);
bool events_emit_propagating(
	Runtime& runtime,
	EventType type,
	std::uint16_t source,
	const std::uint32_t* arguments,
	std::uint8_t argument_count,
	std::uint8_t selector = UINT8_MAX);
bool events_emit_camera_reached(
	Runtime& runtime,
	std::uint16_t source);
bool events_emit_ship_reached(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t actor);

bool events_emit_destroyed(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t attacker);
bool events_emit_component_destroyed(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t attacker,
	std::uint8_t selector);
bool events_emit_shot_at(
	Runtime& runtime,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t source,
	std::uint16_t attacker,
	std::uint8_t selector);
bool events_emit_launched(
	Runtime& runtime,
	std::uint16_t source);
bool events_emit_object_scooped(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t object);
bool events_emit_ripper_grabbed_object(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t object);
bool events_emit_ripper_dropped_object(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t object);
bool events_emit_explosion_ship(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t object);
bool events_emit_jumped_in(
	Runtime& runtime,
	std::uint16_t source);
bool events_emit_fixed_gate_jumped_in(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t actor);
bool events_commit_player_jump_or_warp_requests(
	Runtime& runtime,
	std::uint32_t script_tick);
void events_service_proximity(
	Runtime& runtime,
	const game::World& world);
bool events_flush(
	Runtime& runtime,
	Executor& executor,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	std::uint32_t script_tick,
	std::uint32_t simulation_tick);
}
