#pragma once

#include "game/runtime_limits.hpp"

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cstdint>

namespace sl_open::game
{
struct World;
}

namespace sl_open::mission
{
struct EnvironmentState
{
	std::uint8_t pending_object_state[game::kMaxGameObjects]{};
	std::uint32_t current_mask{};
	std::uint32_t desired_mask{};
	std::uint32_t update_serial{};
	std::int32_t applied_nebula_material{};
	std::int32_t requested_nebula_material{};
	glm::vec3 nebula_light_rgb{0.24f, 0.50f, 1.0f};
	glm::vec3 sun_direction{1.0f, -0.5f, 0.2f};
	glm::vec3 ambient_direction{-1.0f, 0.5f, 0.0f};
	glm::mat3 nebula_dome_orientation{1.0f};
	glm::mat3 nebula_grid_orientation{1.0f};
};

void environment_effects_reset(EnvironmentState& state);
void environment_effect_set(
	EnvironmentState& state,
	std::uint32_t effect_id,
	bool enabled);
void environment_nebula_request(
	EnvironmentState& state,
	std::int32_t material_index);
void environment_queue_object_state(
	EnvironmentState& state,
	std::uint16_t live_object_index,
	bool disabled);
void environment_state_commit(
	EnvironmentState& state,
	game::World& world);
void environment_bind_controller_lights(
	EnvironmentState& state,
	const game::World& world);
}
