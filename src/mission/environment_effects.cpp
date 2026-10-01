#include "mission/environment_effects.hpp"

#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "game/world.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace sl_open::mission
{
namespace
{
constexpr std::uint32_t kAllowedEffectMask = 5u;
constexpr std::uint8_t kEffectCount = 13;
constexpr bool kDelayed[kEffectCount] = {
	true, true, false,
	false, false, false, false, false, false, false, false, false, false,
};
constexpr glm::vec3 kNebulaLightColors[7] = {
	{0.24f, 0.50f, 1.0f},
	{0.00f, 1.00f, 0.80001f},
	{0.33f, 0.46f, 1.0f},
	{0.74f, 1.00f, 0.320001f},
	{0.00f, 0.75f, 1.0f},
	{0.92f, 0.66000003f, 0.330001f},
	{0.00f, 1.00f, 1.0f},
};

glm::mat3 look_at_origin(const glm::vec3& target)
{
	// SR_mat3_look_at_points (LANCER.EXE 0x004c1940), with the zero
	// origin and roll passed by Space_scene_bind_controller_lights.
	glm::vec3 delta = target;
	glm::mat3 orientation{1.0f};
	orientation = math::postrotate(
		orientation,
		std::atan2(delta.x, delta.z),
		{0.0f, 1.0f, 0.0f});
	delta = glm::transpose(orientation) * delta;
	return math::postrotate(
		orientation,
		-std::atan2(delta.y, delta.z),
		{1.0f, 0.0f, 0.0f});
}

void apply_pending_nebula(EnvironmentState& state)
{
	const std::int32_t index = state.requested_nebula_material;
	// Retail tests only index >= 7 and consequently indexes before both
	// tables for a negative value. Compiled mission data supplies 0..6; keep
	// malformed data from turning that original out-of-bounds defect into
	// memory corruption in the reimplementation.
	if (index < 0 || index >= 7)
	{
		diagnostics::mission_log(
			"environment nebula material rejected index=%d",
			index);
		return;
	}
	state.nebula_light_rgb = kNebulaLightColors[index];
	state.applied_nebula_material = index;
}

}

void environment_effects_reset(EnvironmentState& state)
{
	std::fill(
		std::begin(state.pending_object_state),
		std::end(state.pending_object_state),
		std::uint8_t{0});
	state.current_mask = 0;
	state.desired_mask = 0;
	state.sun_direction =
		glm::normalize(glm::vec3{1.0f, -0.5f, 0.2f});
	state.ambient_direction =
		glm::normalize(glm::vec3{-1.0f, 0.5f, 0.0f});
	state.nebula_dome_orientation = glm::mat3{1.0f};
	state.nebula_grid_orientation = math::postrotate(
		glm::mat3{1.0f},
		-glm::half_pi<float>(),
		{0.0f, 1.0f, 0.0f});
}

void environment_effect_set(
	EnvironmentState& state,
	std::uint32_t effect_id,
	bool enabled)
{
	// x86 masks CL for the shift. The following record lookup still uses the
	// original ID; valid retail mission data therefore remains restricted to
	// the 13 compiled definitions even though congruent malformed IDs can
	// pass this first test in LANCER.EXE.
	const std::uint32_t bit = 1u << (effect_id & 31u);
	if ((bit & kAllowedEffectMask) == 0)
	{
		return;
	}
	if (effect_id >= kEffectCount)
	{
		diagnostics::mission_log(
			"environment effect rejected out-of-table id=%u",
			effect_id);
		return;
	}

	std::uint32_t& target = kDelayed[effect_id]
		? state.desired_mask
		: state.current_mask;
	if (enabled)
	{
		target |= bit;
	}
	else
	{
		target &= ~bit;
	}
	// Every compiled callback is null. Immediate changes still publish the
	// entire current mask as the new desired state, exactly canceling any
	// delayed transition that had not yet reached the common commit point.
	if (!kDelayed[effect_id])
	{
		state.desired_mask = state.current_mask;
	}
}

void environment_nebula_request(
	EnvironmentState& state,
	std::int32_t material_index)
{
	state.requested_nebula_material = material_index;
}

void environment_queue_object_state(
	EnvironmentState& state,
	std::uint16_t live_object_index,
	bool disabled)
{
	if (live_object_index < std::size(state.pending_object_state))
	{
		state.pending_object_state[live_object_index] =
			disabled ? 1u : 2u;
	}
}

void environment_state_commit(
	EnvironmentState& state,
	game::World& world)
{
	// Environment_state_commit (0x00469d30) is a transaction boundary. Its
	// ordering is visible to scripts and render consumers: nebula first,
	// deferred live-object flags second, delayed effect callbacks/mask last.
	if (state.requested_nebula_material
		!= state.applied_nebula_material)
	{
		apply_pending_nebula(state);
	}

	for (std::uint16_t index = 0;
		index < std::size(state.pending_object_state);
		++index)
	{
		const std::uint8_t pending = state.pending_object_state[index];
		if (pending == 0)
		{
			continue;
		}
		game::WorldObject& object = world.objects[index];
		object.disabled = pending == 1;
		if (object.disabled)
		{
			object.runtime_flags |= game::kObjectFlagDisabled;
		}
		else
		{
			object.runtime_flags &= ~game::kObjectFlagDisabled;
		}
		state.pending_object_state[index] = 0;
	}

	if (state.desired_mask != state.current_mask)
	{
		// The retail transition loop scans all 13 definitions. All shipped
		// enable/disable callbacks are null, leaving only this final copy.
		state.current_mask = state.desired_mask;
	}
	++state.update_serial;
	diagnostics::mission_log(
		"environment commit serial=%u mask=0x%x nebula=%d",
		state.update_serial,
		state.current_mask,
		state.applied_nebula_material);
}

void environment_bind_controller_lights(
	EnvironmentState& state,
	const game::World& world)
{
	// Space_scene_bind_controller_lights (0x004a5a00) restores only the
	// primary sun's compiled default before scanning. Ambient/grid state is
	// retained until a later type-989 controller overwrites it.
	const glm::vec3 default_sun_direction =
		glm::normalize(glm::vec3{1.0f, -0.5f, 0.2f});
	const glm::mat3 default_sun_orientation =
		look_at_origin(default_sun_direction);
	state.sun_direction = default_sun_direction;
	for (const game::WorldObject& object : world.objects)
	{
		if (!object.active)
		{
			continue;
		}
		if (object.type == 988)
		{
			// Retail first postmultiplies diag(-1,-1,-1) by the authored
			// render basis. It publishes column two to the lights, then
			// composes the nebula dome with the transpose-multiply helper
			// at 0x004c2020 and the default-sun look-at basis built above.
			const glm::mat3 inverted_orientation =
				-object.scene_orientation;
			state.sun_direction = -object.scene_orientation[2];
			state.nebula_dome_orientation =
				inverted_orientation
				* glm::transpose(default_sun_orientation);
		}
		else if (object.type == 989)
		{
			state.ambient_direction = object.scene_orientation[2];
			state.nebula_grid_orientation = object.scene_orientation;
		}
	}
}
}
