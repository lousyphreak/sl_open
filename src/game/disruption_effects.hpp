#pragma once

#include "game/runtime_limits.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace sl_open::game
{
struct World;
}

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

struct ElectricRayGroup
{
	glm::vec3 color{1.0f};
	float alpha{0.5f};
};

struct ElectricRayEffect
{
	glm::vec3 start{0.0f};
	glm::vec3 end{0.0f};
	ElectricRayGroup groups[5];
	float displacement{};
	float radius{};
	float intensity{1.0f};
	std::uint32_t last_tick{};
	std::uint32_t phase_start_tick{};
	std::uint32_t on_ticks{};
	std::uint32_t off_ticks{};
	std::int32_t lifetime_ticks{};
	std::uint16_t owner_index{UINT16_MAX};
	std::uint16_t owner_generation{};
	std::uint16_t parent_model_reference{UINT16_MAX};
	std::uint8_t flags{};
	std::int8_t group_count{};
	bool phase_on{true};
	bool active{};
};

struct Shockwave
{
	glm::vec3 position{0.0f};
	glm::mat3 orientation{1.0f};
	glm::vec3 velocity{0.0f};
	float final_radius{};
	float previous_radius{};
	std::uint32_t start_tick{};
	float lifetime_ticks{};
	std::int32_t source_object_index{-1};
	std::int8_t source_affiliation{};
	std::uint8_t type{};
	bool active{};
};

struct CapitalExplosionController
{
	std::vector<glm::vec3> schedule;
	glm::vec3 original_owner_position{0.0f};
	glm::vec3 portal_position{0.0f};
	glm::mat3 portal_orientation{1.0f};
	std::uint32_t start_tick{};
	std::uint16_t owner_index{UINT16_MAX};
	std::uint16_t owner_generation{};
	std::uint16_t breakaway_index{UINT16_MAX};
	std::uint16_t breakaway_generation{};
	std::uint16_t secondary_breakaway_index{UINT16_MAX};
	std::uint16_t secondary_breakaway_generation{};
	std::int16_t selected_child{-1};
	std::int16_t moving_anchor{-1};
	std::uint16_t schedule_counter{};
	std::uint8_t sequence_index{};
	bool portal_submitted{};
	// The mode-zero and Ulysses parents are opposing render portals: their
	// position/orientation define a clipping plane, not a replacement root
	// transform for the attached model trees.
	bool portal_clipping{};
	bool ulysses{};
	bool active{};
};

struct DisruptionEffectsRuntime
{
	ElectricRayEffect electric_rays[kMaxElectricRays];
	Shockwave shockwaves[kMaxShockwaves];
	CapitalExplosionController
		explosion_controllers[kMaxExplosionControllers];
	std::uint16_t destruction_shockwave_generation[kMaxGameObjects]{};
	bool destruction_shockwave_created[kMaxGameObjects]{};
};

void disruption_effects_reset(DisruptionEffectsRuntime& runtime);
ElectricRayEffect* electric_ray_create(
	World& world,
	std::int16_t group_count,
	std::int32_t lifetime_ticks,
	float displacement,
	float radius,
	std::uint8_t flags);
void electric_ray_set_parent(
	ElectricRayEffect& effect,
	std::uint16_t object_index,
	std::uint16_t object_generation,
	std::uint16_t model_reference = UINT16_MAX);
void electric_ray_set_group_color(
	ElectricRayEffect& effect,
	std::int16_t group,
	const glm::vec3& color);
bool shockwave_create(
	World& world,
	const glm::vec3& position,
	const glm::mat3& orientation,
	const glm::vec3& velocity,
	std::uint8_t type,
	float final_radius,
	float lifetime_ticks,
	std::int8_t source_affiliation,
	std::int32_t source_object_index,
	std::uint32_t simulation_tick);
bool shockwave_create_object_destruction(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& ship_stats,
	std::uint16_t object_index,
	std::uint32_t simulation_tick);
void disruption_effects_service(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& ship_stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick,
	std::uint32_t frame_ticks);
void disruption_effects_service_component_destruction_callbacks(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick);
void disruption_effects_service_explosion_controllers(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick);
}
