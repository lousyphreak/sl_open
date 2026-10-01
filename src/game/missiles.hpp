#pragma once

#include "assets/missile_stats.hpp"
#include "assets/ship_stats.hpp"
#include "game/runtime_limits.hpp"
#include "game/world.hpp"

#include <array>
#include <cstdint>

namespace sl_open::mission
{
struct Runtime;
}

namespace sl_open::game
{
struct ChaffRuntime;
struct WeaponRuntime;

constexpr std::size_t kMaxMissileTrailPoints = 70;

struct MissileTrailRing
{
	glm::vec3 points[4]{};
	float alpha{};
	float intensity{};
};

struct MissileTrailRibbon
{
	std::array<MissileTrailRing, kMaxMissileTrailPoints> rings;
	std::uint16_t ring_count{};
};

struct MissileTrail
{
	std::array<MissileTrailRing, kMaxMissileTrailPoints> rings;
	glm::vec3 color{1.0f};
	MissileTrailRibbon side_ribbons[4];
	glm::vec3 side_color{1.0f};
	glm::vec3 glow_position{0.0f};
	glm::vec3 burst_position{0.0f};
	glm::mat3 burst_orientation{1.0f};
	float glow_size{};
	float burst_v_offset{};
	std::uint32_t last_update_tick{};
	std::uint32_t burst_last_tick{};
	std::uint16_t ring_count{};
	std::uint8_t type{};
	std::uint8_t flags{};
	std::uint8_t fade_ticks{};
	std::uint8_t side_fade_ticks{};
	std::uint8_t side_count{};
	bool active{};
	bool attached{};
};

struct Missile
{
	glm::vec3 previous_position{0.0f};
	glm::vec3 position{0.0f};
	glm::vec3 scene_position{0.0f};
	glm::vec3 velocity{0.0f};
	glm::mat3 previous_orientation{1.0f};
	glm::mat3 orientation{1.0f};
	glm::mat3 scene_orientation{1.0f};
	ObjectHandle shooter;
	ObjectHandle target;
	std::int16_t allegiance_class{2};
	std::int32_t launch_tick{-1};
	std::int32_t behavior_tick{-1};
	std::uint32_t sound_generation{};
	std::int16_t target_component{-1};
	std::int16_t guidance_state{-1};
	std::int16_t trail_slot{-1};
	std::int16_t type{-1};
	std::int16_t behavior{-1};
	std::int16_t object_kind{};
	std::uint8_t model_variant{};
	float throttle{};
	float pitch_control{};
	float yaw_control{};
	float smoothed_pitch{};
	float smoothed_yaw{};
	float radius{};
	glm::vec3 bounds_min{0.0f};
	glm::vec3 bounds_max{0.0f};
	bool active{};
};

struct MissileRuntime
{
	Missile missiles[kMaxMissiles];
	MissileTrail trails[kMaxMissileTrails];
	std::uint32_t live_count{};
	std::uint32_t next_sound_generation{};
	bool pool_exhaustion_reported{};
};

void missiles_reset(MissileRuntime& runtime);
bool missiles_launch_from_ship_mount(
	MissileRuntime& runtime,
	World& world,
	const assets::MissileStatsTable& stats,
	ObjectHandle shooter,
	std::uint8_t attachment_index,
	ObjectHandle target,
	std::int16_t target_component,
	std::uint32_t simulation_tick);
bool missiles_launch_type0_from_transform(
	MissileRuntime& runtime,
	World& world,
	const assets::MissileStatsTable& stats,
	ObjectHandle shooter,
	const glm::vec3& position,
	const glm::mat3& orientation,
	ObjectHandle target,
	std::int16_t target_component,
	std::uint32_t simulation_tick);
void missiles_integrate(
	MissileRuntime& runtime,
	const assets::MissileStatsTable& stats);
void missiles_publish_scene_poses(
	MissileRuntime& runtime,
	float service_fraction);
void missiles_step(
	MissileRuntime& runtime,
	ChaffRuntime& chaff,
	World& world,
	WeaponRuntime& weapons,
	mission::Runtime& mission,
	const assets::MissileStatsTable& stats,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick,
	std::uint32_t& random_seed,
	bool local_player_lock_held);
bool missiles_local_shot_has_target(
	const MissileRuntime& runtime,
	ObjectHandle shooter);
}
