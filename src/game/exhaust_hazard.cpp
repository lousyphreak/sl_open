#include "game/exhaust_hazard.hpp"

#include "assets/ship_stats.hpp"
#include "core/mission_log.hpp"
#include "game/damage.hpp"
#include "game/model_animation.hpp"
#include "game/world.hpp"
#include "mission/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace sl_open::game
{
namespace
{
constexpr float kExhaustBroadRadiusScale = 1.2000000476837158f;
constexpr float kExhaustBoxScale = 1.7000000476837158f;
constexpr float kExhaustDamage = 23.0f;

bool object_has_exhaust_locator(const WorldObject& object)
{
	return (object.runtime_flags & kObjectFlagCompound) != 0
		&& !object.exhaust_hazard_volumes.empty();
}

void register_index(World& world, std::uint16_t index)
{
	if (world.exhaust_hazard_count >= kExhaustHazardCapacity)
	{
		return;
	}
	for (std::uint16_t registered = 0;
		registered < world.exhaust_hazard_count;
		++registered)
	{
		if (world.exhaust_hazard_indices[registered] == index)
		{
			return;
		}
	}
	world.exhaust_hazard_indices[world.exhaust_hazard_count++] = index;
}

void rebuild_registry(World& world)
{
	world.exhaust_hazard_count = 0;
	for (std::uint16_t index = 0; index < kMaxGameObjects; ++index)
	{
		const WorldObject& object = world.objects[index];
		if (object.active && object_has_exhaust_locator(object))
		{
			register_index(world, index);
		}
	}
	world.exhaust_hazard_registry_valid = true;
	diagnostics::mission_log(
		"exhaust registry rebuilt hazards=%u",
		static_cast<unsigned>(world.exhaust_hazard_count));
}

float volume_exposure(
	const WorldObject& hazard,
	const ExhaustHazardVolume& volume,
	const glm::vec3& player_position)
{
	if (volume.driver_model_reference
		>= hazard.model_references.size()
		|| hazard.model_references[
			volume.driver_model_reference].removed)
	{
		return 0.0f;
	}
	const glm::mat4 object_transform =
		glm::translate(glm::mat4{1.0f}, hazard.position)
		* glm::mat4(hazard.orientation);
	const glm::mat4 volume_transform =
		object_transform
		* model_animation_render_transform(
			hazard, volume.driver_model_reference, 1.0f)
		* volume.locator_from_driver;
	const glm::vec3 local_point = glm::vec3(
		glm::inverse(volume_transform)
			* glm::vec4(player_position, 1.0f));

	// ExhaustHazard_accumulate_tree_exposure (0x00469a10) takes the
	// dedicated Engine_Mesh helper's min/max bounds and multiplies every
	// axis by abs(exhaust_scalar * effect_scale) * 1.7
	// * tag-2 dimensions[axis].
	// Only Z is exchanged afterwards; exporter-authored X/Y signs remain
	// part of the inclusive-box contract.
	const float intensity = std::abs(
		hazard.exhaust_scalar * hazard.effect_scale);
	const glm::vec3 scale =
		glm::vec3{kExhaustBoxScale * intensity}
		* volume.dimensions;
	glm::vec3 maximum = volume.bounds_max * scale;
	glm::vec3 minimum = volume.bounds_min * scale;
	if (maximum.z < minimum.z)
	{
		std::swap(maximum.z, minimum.z);
	}
	for (std::uint32_t axis = 0; axis < 3; ++axis)
	{
		if (maximum[axis] < local_point[axis]
			|| local_point[axis] < minimum[axis])
		{
			return 0.0f;
		}
	}
	const float corner_distance = glm::length(maximum);
	if (corner_distance == 0.0f)
	{
		return 0.0f;
	}
	return 1.0f - glm::length(local_point) / corner_distance;
}

float object_exposure(
	const WorldObject& hazard,
	const glm::vec3& player_position)
{
	float exposure = 0.0f;
	for (const ExhaustHazardVolume& volume
		: hazard.exhaust_hazard_volumes)
	{
		exposure += volume_exposure(
			hazard, volume, player_position);
	}
	return exposure;
}

void apply_player_bank_two_damage(
	World& world,
	mission::Runtime& mission,
	WorldObject& player,
	const assets::ShipStatsTable& stats,
	bool impact_feedback_enabled,
	float raw_damage,
	std::uint32_t simulation_tick)
{
	(void)apply_primary_bank_damage(
		world,
		mission,
		player,
		stats,
		2,
		raw_damage,
		0.5f,
		world.player.index,
		2,
		impact_feedback_enabled,
		simulation_tick);
}
}

void exhaust_hazard_register_object(
	World& world,
	WorldObject& object)
{
	if (!world.exhaust_hazard_registry_valid
		|| !object.active
		|| !object_has_exhaust_locator(object))
	{
		return;
	}
	const auto difference = &object - std::begin(world.objects);
	if (difference < 0
		|| static_cast<std::size_t>(difference)
			>= std::size(world.objects))
	{
		return;
	}
	register_index(world, static_cast<std::uint16_t>(difference));
}

void exhaust_hazard_invalidate(World& world)
{
	world.exhaust_hazard_registry_valid = false;
}

void exhaust_hazard_service(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick)
{
	if (!world.exhaust_hazard_registry_valid)
	{
		rebuild_registry(world);
	}
	WorldObject* player = world_resolve(world, world.player);
	if (player == nullptr
		|| player->ai.command_count == 0
		|| player->ai.commands[0].id != 100)
	{
		return;
	}

	const bool previously_exposed = world.player_in_exhaust;
	world.player_in_exhaust = false;
	for (std::uint16_t registered = 0;
		registered < world.exhaust_hazard_count;
		++registered)
	{
		const std::uint16_t index =
			world.exhaust_hazard_indices[registered];
		if (index >= kMaxGameObjects)
		{
			continue;
		}
		const WorldObject& hazard = world.objects[index];
		if (!hazard.active
			|| (hazard.runtime_flags & kObjectFlagCompound) == 0
			|| hazard.exhaust_scalar == 0.0f)
		{
			continue;
		}
		const float hazard_radius =
			hazard.radius * kExhaustBroadRadiusScale;
		const float player_radius = player->radius;
		const glm::vec3 separation =
			hazard.position - player->position;
		const float distance_squared =
			glm::dot(separation, separation);
		if (distance_squared
			> player_radius * player_radius
				+ hazard_radius * hazard_radius)
		{
			continue;
		}

		const float exposure =
			object_exposure(hazard, player->position);
		if (exposure > 0.0f)
		{
			world.player_in_exhaust = true;
		}
		// Retail publishes this inside the qualifying-hazard loop. The
		// final broad-phase-qualified object therefore owns the displayed
		// percentage, while the Boolean accumulates across all hazards.
		world.player_exhaust_exposure_percent =
			static_cast<std::int32_t>(exposure * 100.0f);
		if ((simulation_tick % 7u) == 0 && exposure > 0.0f)
		{
			apply_player_bank_two_damage(
				world,
				mission,
				*player,
				stats,
				impact_feedback_enabled,
				std::min(exposure, 1.0f) * kExhaustDamage,
				simulation_tick);
		}
	}

	if (world.player_in_exhaust != previously_exposed)
	{
		diagnostics::mission_log(
			"player exhaust exposure active=%u percent=%d tick=%u",
			world.player_in_exhaust ? 1u : 0u,
			world.player_exhaust_exposure_percent,
			simulation_tick);
	}
}
}
