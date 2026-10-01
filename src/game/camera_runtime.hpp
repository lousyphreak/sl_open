#pragma once

#include "core/math.hpp"

#include <cstdint>

namespace sl_open::assets
{
struct ShipStatsTable;
}

namespace sl_open::input
{
struct GameplayInput;
}

namespace sl_open::game
{
struct MissileRuntime;
struct World;

struct CameraRuntime
{
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	glm::vec3 previous_position{0.0f};
	glm::mat3 previous_orientation{1.0f};
	glm::vec3 staged_position{0.0f};
	glm::mat3 staged_orientation{1.0f};
	glm::mat3 installed_orientation{1.0f};
	glm::vec3 orbit_vector{0.0f};
	glm::vec3 aim_near_position{0.0f};
	glm::mat3 aim_near_orientation{1.0f};
	glm::vec3 aim_far_position{0.0f};
	glm::mat3 aim_far_orientation{1.0f};
	glm::vec3 cockpit_local_position{0.0f};
	glm::mat3 cockpit_orientation{1.0f};
	glm::mat3 cockpit_component_orientation{1.0f};
	float cockpit_recoil_offset{};
	float follow_distance{1500.0f};
	float smooth_pitch{};
	float smooth_yaw{};
	float smooth_roll{};
	float chase_distance{};
	float chase_yaw_degrees{};
	float chase_pitch_degrees{};
	float chase_yaw_velocity{};
	float chase_pitch_velocity{};
	float projection_progress{};
	float projection_rate{};
	float horizontal_tangent{0.5f / 0.6000000238418579f};
	float vertical_tangent{0.625f};
	float recoil{};
	float warp_camera_scalar{-5000.0f};
	std::uint32_t current_time_tick{};
	std::uint32_t installed_tick{};
	std::uint32_t target_command_expiry_tick{};
	std::uint32_t missile_grace_deadline_tick{};
	std::uint16_t target{UINT16_MAX};
	std::uint16_t missile_cursor{};
	std::uint16_t active_missile{UINT16_MAX};
	std::uint16_t spectator_cursor{};
	std::int16_t mode{};
	std::uint8_t missile_phase{};
	std::uint8_t graphics_quality{2};
	std::uint8_t view_state{};
	bool multiplayer{};
	bool spectator_active{};
	bool spectator_complete{};
	bool switch_guard{};
	bool locked{};
	bool hat_camera_active{};
};

std::int16_t camera_runtime_update_user_controls(
	CameraRuntime& camera,
	const input::GameplayInput& input,
	std::uint32_t frame_ticks);
void camera_runtime_publish_world_state(
	const CameraRuntime& camera,
	World& world);
bool camera_runtime_select_next_spectator(
	CameraRuntime& camera,
	World& world,
	const MissileRuntime& missiles,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_ticks,
	std::uint32_t simulation_tick);
void camera_runtime_reset(
	CameraRuntime& camera,
	World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick);
bool camera_runtime_request(
	CameraRuntime& camera,
	World& world,
	const MissileRuntime& missiles,
	const assets::ShipStatsTable& stats,
	std::int16_t mode,
	std::uint16_t target,
	bool lock,
	bool override_lock,
	std::uint32_t frame_ticks,
	std::uint32_t simulation_tick,
	bool accept_departed_target = false);
void camera_runtime_service(
	CameraRuntime& camera,
	World& world,
	const MissileRuntime& missiles,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_ticks,
	std::uint32_t simulation_tick);
}
