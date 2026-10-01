#pragma once

#include "assets/gun_stats.hpp"
#include "assets/missile_stats.hpp"
#include "game/missiles.hpp"
#include "game/runtime_limits.hpp"
#include "game/world.hpp"

#include <cstdint>

namespace sl_open::mission
{
struct Runtime;
}

namespace sl_open::game
{
struct GunProjectile
{
	struct CollisionCandidate
	{
		std::uint16_t object_index{UINT16_MAX};
		std::int16_t model_index{-1};
	};

	glm::vec3 previous_position{0.0f};
	glm::vec3 position{0.0f};
	glm::vec3 scene_position{0.0f};
	glm::vec3 velocity{0.0f};
	glm::mat3 orientation{1.0f};
	CollisionCandidate candidates[kMaxProjectileCandidates];
	std::int32_t type_index{-1};
	std::int32_t expiration_tick{-1};
	std::uint32_t spawn_tick{};
	std::uint32_t sound_generation{};
	float visual_random[12]{};
	std::uint16_t shooter_index{UINT16_MAX};
	std::uint16_t shooter_generation{};
	std::int16_t shooter_affiliation{};
	std::uint8_t candidate_count{};
	bool active{};
};

struct NovaBeam
{
	glm::vec3 start{0.0f};
	glm::vec3 end{0.0f};
	glm::mat3 orientation{1.0f};
	std::uint32_t expiration_tick{};
	std::uint16_t owner_index{UINT16_MAX};
	float charge{};
	bool active{};
	bool fully_charged{};
};

struct WeaponSoundEvent
{
	glm::vec3 position{0.0f};
	glm::vec3 direction{0.0f, 0.0f, 1.0f};
	glm::vec3 velocity{0.0f};
	std::uint32_t source_generation{};
	std::uint16_t source_index{UINT16_MAX};
	std::uint8_t definition{};
	std::uint8_t requested_class{};
	bool projectile{};
};

struct MuzzleFlash
{
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	std::uint32_t start_tick{};
	std::uint32_t expiration_tick{};
	std::int16_t shooter_affiliation{};
	std::uint8_t type{};
	bool active{};
};

struct WeaponRuntime
{
	GunProjectile projectiles[kMaxGunProjectiles];
	NovaBeam nova_beams[8];
	WeaponSoundEvent sound_events[32];
	MuzzleFlash muzzle_flashes[64];
	std::uint32_t live_projectiles{};
	std::uint32_t shot_serial{};
	std::uint8_t sound_read{};
	std::uint8_t sound_count{};
	std::uint32_t last_damage_log_tick{};
	std::uint32_t last_component_log_tick{};
	std::uint16_t last_damage_log_target{UINT16_MAX};
	std::uint16_t last_component_log_target{UINT16_MAX};
	std::uint16_t last_component_log_model{UINT16_MAX};
	std::uint8_t last_damage_log_bank{UINT8_MAX};
	float camera_recoil{};
	bool pool_exhaustion_reported{};
	bool feedback_enabled{};
};

void weapons_reset(WeaponRuntime& runtime);
void weapons_fire_model_animation_event(
	WeaponRuntime& runtime,
	World& world,
	WorldObject& object,
	std::uint16_t source_model_reference,
	const assets::GunStatsTable& gun_stats,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick);
std::uint8_t weapons_selected_gun_mounts(
	const WorldObject& object,
	std::uint8_t* output,
	std::uint8_t capacity);
bool weapons_compute_lead_point(
	const WorldObject& actor,
	const WorldObject& target,
	std::int16_t target_component,
	const assets::GunStatsTable& gun_stats,
	float lead_factor,
	glm::vec3& point);
bool weapons_scene_segment_hit(
	const WorldObject& target,
	const glm::vec3& world_start,
	const glm::vec3& world_end,
	float& fraction);
void weapons_apply_gun_cooldown(
	World& world,
	WorldObject& object,
	std::uint32_t simulation_tick,
	std::uint32_t delay);
bool weapons_fire_charged_nova(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	WorldObject& object,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick);
bool weapons_pop_sound(
	WeaponRuntime& runtime,
	WeaponSoundEvent& event);
void weapons_service_simple_guns(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	const assets::GunStatsTable& gun_stats,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick);
void weapons_service_articulated_guns(
	WeaponRuntime& runtime,
	World& world,
	MissileRuntime& missiles,
	const assets::GunStatsTable& gun_stats,
	const assets::MissileStatsTable& missile_stats,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick,
	std::uint32_t frame_ticks,
	bool network_active,
	std::uint16_t mission_number);
void weapons_service_projectile_physics(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	const assets::GunStatsTable& gun_stats,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick);
void weapons_step_projectiles(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	std::uint32_t simulation_tick,
	std::uint8_t service_phase);
bool weapons_process_missile_collision(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	Missile& missile,
	const assets::MissileStats& stats,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick);
}
