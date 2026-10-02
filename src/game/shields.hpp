#pragma once

#include "game/runtime_limits.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace sl_open::game
{
struct World;
struct WorldObject;

constexpr std::uint32_t kShieldLodCount = 6;
constexpr std::uint32_t kShieldLod0VertexCount = 210;
constexpr std::uint32_t kShieldLod0IndexCount = 1248;
constexpr std::uint32_t kCapShieldSlotCount = 50;
constexpr std::uint32_t kSparkCount = 256;

struct ShieldSphereGeometry
{
	std::array<glm::vec3, kShieldLod0VertexCount> points{};
	std::array<std::uint16_t, kShieldLod0IndexCount> indices{};
	std::uint32_t point_count{};
	std::uint32_t index_count{};
};

struct ShieldSphereState
{
	std::array<
		std::array<float, kShieldLod0VertexCount>, 8> hit{};
	std::array<glm::vec2, kShieldLod0VertexCount> uv{};
	std::uint32_t last_render_tick{};
	std::uint32_t last_hit_tick{};
	std::int32_t flicker_until_tick{-1};
	float uv_center_x{0.3f};
	float uv_center_y{0.3f};
	std::uint8_t next_hit_buffer{};
	std::uint8_t lod{};
	bool active{};
	bool initialized{};
};

struct CapShieldHit
{
	glm::vec3 local_center{0.0f};
	float radius{};
	std::uint32_t tick{};
	bool active{};
};

struct CapShieldSlot
{
	CapShieldHit hit[8];
	std::uint32_t expiry_tick{};
	std::uint32_t birth_tick{};
	std::uint16_t object_index{UINT16_MAX};
	std::uint16_t object_generation{};
	std::int16_t model_index{-1};
	std::uint8_t next_hit_buffer{};
	bool forcefield{};
};

struct Spark
{
	glm::vec3 position{0.0f};
	glm::vec3 damped_velocity{0.0f};
	glm::vec3 unit_drift{0.0f};
	std::uint32_t birth_tick{};
	std::uint8_t type{};
	bool active{};
};

struct ShieldRuntime
{
	ShieldSphereState sphere[kMaxGameObjects];
	CapShieldSlot cap[kCapShieldSlotCount];
	Spark sparks[kSparkCount];
	glm::vec3 camera_position{0.0f};
	glm::vec3 camera_forward{0.0f, 0.0f, 1.0f};
	std::int16_t camera_mode{};
	std::uint32_t replacement_cursor{};
	std::uint32_t player_sound_deadline{};
	std::uint8_t spark_cursor{};
};

void shields_reset(ShieldRuntime& runtime);
const ShieldSphereGeometry& shield_sphere_geometry(std::uint32_t lod);
void shields_initialize_sphere_state(ShieldSphereState& state);
void shields_register_hit(
	World& world,
	WorldObject& object,
	std::int16_t model_index,
	const glm::vec3& world_impact,
	std::uint8_t effect_type,
	std::uint32_t simulation_tick);
void shields_create_impact_effect(
	World& world,
	WorldObject* owner,
	const glm::vec3& position,
	const glm::vec3& direction,
	std::uint8_t effect_type,
	std::uint32_t simulation_tick);
void shields_emit_sparks(
	World& world,
	std::uint8_t type,
	const glm::vec3& position,
	const glm::vec3& axis,
	float base_speed,
	float angular_spread,
	float speed_spread,
	std::int16_t count,
	std::uint32_t simulation_tick);
void shields_service(
	World& world,
	std::uint32_t simulation_tick,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward,
	std::int16_t camera_mode);
glm::vec3 shield_color_lookup(
	float value,
	bool forcefield);
}
