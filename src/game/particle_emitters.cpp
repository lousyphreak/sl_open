#include "game/particle_emitters.hpp"

#include "assets/ship_stats.hpp"
#include "assets/gameplay_model.hpp"
#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "game/world.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace sl_open::game
{
namespace
{
// The executable multiplies rand() results by the single-precision constant
// at 0x004dc4c8 (0x38000100), i.e. 1/32768.  The upper endpoint is therefore
// deliberately just below one.
constexpr float kRandScale = 1.0f / 32768.0f;
constexpr std::uint8_t kLargeSmokeStyle = 5;
constexpr std::uint8_t kEjectionStyle = 10;
constexpr std::uint8_t kWGateStyle = 11;
constexpr std::uint8_t kWGateLargeStyle = 12;
constexpr std::uint8_t kNovaExpiryStyle = 13;
constexpr std::uint8_t kDamageLightStyle = 14;
constexpr std::uint8_t kDamageMediumStyle = 15;
constexpr std::uint8_t kDamageHeavyStyle = 16;
constexpr std::uint8_t kShieldImpactStyle = 17;
constexpr std::uint8_t kShieldGrayStyle = 18;

std::int32_t retail_tick(std::uint32_t simulation_tick)
{
	return static_cast<std::int32_t>(simulation_tick);
}

std::int32_t retail_elapsed_ticks(std::uint32_t elapsed_simulation_ticks)
{
	return static_cast<std::int32_t>(elapsed_simulation_ticks);
}

float random_normalized(World& world)
{
	return static_cast<float>(world_rand15(world)) * kRandScale;
}

std::int32_t retail_trunc(float value)
{
	// MSVC's helper at LANCER.EXE 0x004cf28c explicitly selects the x87
	// truncate-toward-zero rounding mode before converting the value.
	return static_cast<std::int32_t>(value);
}

bool tick_end_strictly_before(
	std::int32_t birth,
	std::int32_t lifetime,
	std::int32_t now)
{
	return static_cast<std::int64_t>(birth)
			+ static_cast<std::int64_t>(lifetime)
		< static_cast<std::int64_t>(now);
}

bool tick_end_at_or_before(
	std::int32_t birth,
	std::int32_t lifetime,
	std::int32_t now)
{
	return static_cast<std::int64_t>(birth)
			+ static_cast<std::int64_t>(lifetime)
		<= static_cast<std::int64_t>(now);
}

void set_style(
	ParticleStyle& style,
	std::uint8_t array_index,
	std::int32_t lifetime_base,
	std::int32_t lifetime_random,
	const glm::vec3& emission,
	const glm::vec3& size,
	const glm::vec3& red,
	const glm::vec3& green,
	const glm::vec3& blue,
	float distance_scale = 1.0f)
{
	style = {};
	style.emission_mode = ParticleEmissionMode::ordinary;
	style.lifetime_base_ticks = lifetime_base;
	style.lifetime_random_ticks = lifetime_random;
	style.emission = particle_curve_fit_start_mid_end(
		emission.x, emission.y, emission.z);
	style.size = particle_curve_fit_start_mid_end(
		size.x, size.y, size.z);
	style.red = particle_curve_fit_start_mid_end(
		red.x, red.y, red.z);
	style.green = particle_curve_fit_start_mid_end(
		green.x, green.y, green.z);
	style.blue = particle_curve_fit_start_mid_end(
		blue.x, blue.y, blue.z);
	style.array_index = array_index;
	style.burst_distance_scale = distance_scale;
	style.active = true;
}

void initialize_explosion_styles(ParticleRuntime& runtime)
{
	// The first ten records belong to explode.cpp. The remaining retained
	// records mirror the styles allocated by Player, WGate, Guns, mission
	// damage effects, and ShieldFX against the same global particle owner.
	runtime.styles.resize(kParticleStyleCount);
	const std::uint8_t array = runtime.default_array;
	set_style(
		runtime.styles[0], array, 100, 10,
		{30.0f, 20.0f, 10.0f},
		{25.0f, 50.0f, 75.0f},
		{1.0f, 0.25f, 0.0f},
		{0.5f, 0.25f, 0.0f},
		{0.0f, 0.25f, 0.0f});
	set_style(
		runtime.styles[1], array, 500, 100,
		{0.0f, 0.0f, 0.0f},
		{0.0f, 100.0f, 400.0f},
		{1.0f, 0.75f, 0.0f},
		{1.0f, 0.25f, 0.0f},
		{0.0f, 0.0f, 0.0f},
		0.05f);
	set_style(
		runtime.styles[2], array, 100, 500,
		{0.0f, 0.0f, 0.0f},
		{0.0f, 25.0f, 25.0f},
		{1.0f, 1.0f, 0.0f},
		{1.0f, 1.0f, 0.0f},
		{1.0f, 1.0f, 0.0f});
	set_style(
		runtime.styles[3], array, 100, 10,
		{20.0f, 15.0f, 10.0f},
		{50.0f, 100.0f, 150.0f},
		{0.5f, 0.2f, 0.0f},
		{0.5f, 0.2f, 0.0f},
		{0.5f, 0.2f, 0.0f});
	set_style(
		runtime.styles[4], array, 500, 30,
		{60.0f, 40.0f, 20.0f},
		{25.0f, 50.0f, 75.0f},
		{0.75f, 0.05f, 0.0f},
		{0.6f, 0.0f, 0.0f},
		{1.0f, 0.4f, 0.0f});
	set_style(
		runtime.styles[5], array, 200, 20,
		{8.0f, 5.0f, 2.0f},
		{1000.0f, 1500.0f, 2500.0f},
		{0.3f, 0.1f, 0.0f},
		{0.3f, 0.1f, 0.0f},
		{0.3f, 0.1f, 0.0f});
	set_style(
		runtime.styles[6], array, 200, 20,
		{90.0f, 90.0f, 90.0f},
		{200.0f, 175.0f, 150.0f},
		{1.0f, 0.5f, 0.0f},
		{0.0f, 0.0f, 0.0f},
		{0.0f, 0.0f, 0.0f});
	set_style(
		runtime.styles[7], array, 200, 20,
		{90.0f, 90.0f, 90.0f},
		{200.0f, 200.0f, 200.0f},
		{1.0f, 0.8f, 0.0f},
		{0.0f, 0.0f, 0.0f},
		{0.0f, 0.0f, 0.0f});
	set_style(
		runtime.styles[8], array, 130, 20,
		{150.0f, 150.0f, 150.0f},
		{200.0f, 200.0f, 200.0f},
		{1.0f, 0.8f, 0.0f},
		{0.0f, 0.0f, 0.0f},
		{0.0f, 0.0f, 0.0f});
	set_style(
		runtime.styles[9], array, 130, 20,
		{150.0f, 150.0f, 150.0f},
		{500.0f, 500.0f, 500.0f},
		{1.0f, 0.8f, 0.0f},
		{0.0f, 0.0f, 0.0f},
		{0.0f, 0.0f, 0.0f});
	set_style(
		runtime.styles[kEjectionStyle], array, 50, 50,
		{0.0f, 0.0f, 0.0f},
		{0.0f, 50.0f, 100.0f},
		{1.0f, 0.75f, 0.0f},
		{1.0f, 0.25f, 0.0f},
		{0.0f, 0.0f, 0.0f});
	set_style(
		runtime.styles[kWGateStyle], array, 120, 10,
		{1200.0f, 1200.0f, 1200.0f},
		{50.0f, 33.0f, 25.0f},
		{0.3f, 0.03f, 0.0f},
		{0.3f, 0.0f, 0.0f},
		{0.3f, 0.15f, 0.0f});
	set_style(
		runtime.styles[kWGateLargeStyle], array, 120, 10,
		{1200.0f, 1200.0f, 1200.0f},
		{1000.0f, 750.0f, 500.0f},
		{0.3f, 0.03f, 0.0f},
		{0.3f, 0.0f, 0.0f},
		{0.3f, 0.15f, 0.0f});
	set_style(
		runtime.styles[kNovaExpiryStyle], array, 150, 10,
		{0.0f, 0.0f, 0.0f},
		{40.0f, 105.0f, 119.0f},
		{0.5f, 0.25f, 0.0f},
		{0.5f, 0.25f, 0.0f},
		{0.5f, 0.25f, 0.0f});
	runtime.styles[kNovaExpiryStyle].emission_mode =
		ParticleEmissionMode::rare_debris;
	set_style(
		runtime.styles[kDamageLightStyle], runtime.damage_arrays[0], 70, 10,
		{0.0f, 0.0f, 0.0f},
		{37.5f, 150.0f, 300.0f},
		{0.3f, 0.15f, 0.0f},
		{0.3f, 0.15f, 0.0f},
		{0.3f, 0.15f, 0.0f});
	set_style(
		runtime.styles[kDamageMediumStyle], runtime.damage_arrays[1], 100, 10,
		{0.0f, 0.0f, 0.0f},
		{62.5f, 250.0f, 500.0f},
		{0.5f, 0.25f, 0.0f},
		{0.5f, 0.25f, 0.0f},
		{0.5f, 0.25f, 0.0f});
	set_style(
		runtime.styles[kDamageHeavyStyle], runtime.damage_arrays[2], 100, 10,
		{0.0f, 0.0f, 0.0f},
		{75.0f, 300.0f, 600.0f},
		{0.5f, 0.25f, 0.0f},
		{0.5f, 0.25f, 0.0f},
		{0.5f, 0.25f, 0.0f});
	runtime.styles[kDamageHeavyStyle].emission_mode =
		ParticleEmissionMode::rare_debris;
	set_style(
		runtime.styles[kShieldImpactStyle], array, 100, 10,
		{15.0f, 10.0f, 0.0f},
		{50.0f, 75.0f, 100.0f},
		{1.0f, 0.25f, 0.0f},
		{0.5f, 0.25f, 0.0f},
		{0.0f, 0.25f, 0.0f});
	set_style(
		runtime.styles[kShieldGrayStyle], array, 50, 10,
		{15.0f, 15.0f, 15.0f},
		{25.0f, 35.0f, 50.0f},
		{0.75f, 0.5f, 0.0f},
		{0.75f, 0.5f, 0.0f},
		{0.75f, 0.5f, 0.0f});
}

bool refresh_emitter_transform(World& world, ParticleEmitter& emitter)
{
	if (!emitter.model_owned)
	{
		emitter.world_position = emitter.local_position;
		emitter.world_basis = emitter.local_basis;
		return true;
	}
	const WorldObject* owner = world_resolve(
		world,
		{emitter.owner_index, emitter.owner_generation});
	if (owner == nullptr
		|| emitter.model_reference >= owner->model_references.size())
	{
		return false;
	}
	glm::mat4 root = sl_open::math::model_transform(
		owner->scene_orientation, 1.0f, owner->scene_position);
	if (owner->model_references[emitter.model_reference]
			.explosion_portal_group != 0)
	{
		for (const CapitalExplosionController& controller
			: world.disruption_effects.explosion_controllers)
		{
			if (!controller.active)
			{
				continue;
			}
			const bool controller_owner =
				controller.owner_index == emitter.owner_index
				&& controller.owner_generation == emitter.owner_generation;
			const bool controller_breakaway =
				controller.breakaway_index == emitter.owner_index
				&& controller.breakaway_generation == emitter.owner_generation;
			if (controller_owner || controller_breakaway)
			{
				root = sl_open::math::model_transform(
					controller.portal_orientation,
					1.0f,
					controller.portal_position);
				break;
			}
		}
	}
	const glm::mat4 parent = root
		* owner->model_references[emitter.model_reference].scene_transform;
	emitter.world_position =
		glm::vec3(parent * glm::vec4(emitter.local_position, 1.0f));
	emitter.world_basis = glm::mat3(parent) * emitter.local_basis;
	return true;
}

glm::vec3 random_emitter_velocity(
	World& world,
	const ParticleEmitter& emitter,
	bool inherit_velocity = true)
{
	// MSVC evaluates the three source arguments right-to-left. The shipped
	// code therefore consumes Z, Y, X and finally speed.
	glm::vec3 direction;
	direction.z = emitter.direction_center.z
		+ (random_normalized(world) - 0.5f)
			* emitter.direction_spread.z;
	direction.y = emitter.direction_center.y
		+ (random_normalized(world) - 0.5f)
			* emitter.direction_spread.y;
	direction.x = emitter.direction_center.x
		+ (random_normalized(world) - 0.5f)
			* emitter.direction_spread.x;
	const float length = glm::length(direction);
	if (length > 0.0f)
	{
		const float speed = emitter.speed_base
			+ random_normalized(world) * emitter.speed_random;
		direction *= speed / length;
	}
	return emitter.world_basis * direction
		+ (inherit_velocity
			? emitter.inherited_velocity
			: glm::vec3{0.0f});
}

void spawn_particle(
	World& world,
	ParticleEmitter& emitter,
	std::int32_t slot_index,
	std::int32_t now_tick)
{
	ParticleRuntime& runtime = world.particles;
	if (emitter.style_index >= runtime.styles.size())
	{
		return;
	}
	ParticleStyle& style = runtime.styles[emitter.style_index];
	if (!style.active || style.array_index >= runtime.arrays.size())
	{
		return;
	}
	ParticleArray& array = runtime.arrays[style.array_index];
	if (!array.active
		|| slot_index < 0
		|| slot_index >= array.capacity)
	{
		return;
	}
	array.high_water = std::max(array.high_water, slot_index + 1);
	ParticleSlot& slot = array.slots[slot_index];
	slot.birth_tick = now_tick;
	slot.lifetime_ticks = style.lifetime_base_ticks;
	slot.style_index = emitter.style_index;
	if (style.lifetime_random_ticks > 0)
	{
		slot.lifetime_ticks +=
			world_rand15(world) % style.lifetime_random_ticks;
	}
	slot.velocity = random_emitter_velocity(world, emitter);
	ParticleVisual& visual = array.visuals[slot_index];
	visual.position = emitter.world_position;
	visual.rectangle = emitter.rectangle;
	// Particle_spawn_into_slot (0x0049c1c0) writes the BMO transform and
	// texture rectangle, but not its extents, colour, alpha, or activity
	// byte. Continuous emitters run after Particle_update_all, so their new
	// records remain hidden until the following array service. Bursts which
	// happen before that service are activated and styled by the common pass.
}

std::uint16_t standard_fragment_resource(World& world)
{
	const float selection = random_normalized(world);
	if (selection < 0.25f)
	{
		return 78;
	}
	if (selection < 0.5f)
	{
		return 87;
	}
	const std::int32_t offset =
		retail_trunc((selection - 0.5f) * -16.0f);
	return static_cast<std::uint16_t>(
		std::clamp<std::int32_t>(79 - offset, 79, 87));
}

void spawn_fragment_from_particle(
	World& world,
	const glm::vec3& position,
	const glm::vec3& velocity,
	float scale,
	std::int32_t now_tick)
{
	ParticleRuntime& runtime = world.particles;
	if (runtime.fragment_capacity == 0)
	{
		return;
	}
	ParticleFragment& fragment =
		runtime.fragments[runtime.fragment_cursor];
	if (fragment.active && runtime.fragment_live_count != 0)
	{
		--runtime.fragment_live_count;
	}
	fragment = {};
	fragment.active = true;
	fragment.render_active = false;
	fragment.model_resource = standard_fragment_resource(world);
	fragment.scale =
		1.5f * scale * (0.5f + random_normalized(world));
	fragment.position = position;
	fragment.velocity = velocity;
	// The same right-to-left source-argument evaluation consumes Z, Y, X.
	fragment.angular_step.z =
		(random_normalized(world) - 0.5f) * 0.1f;
	fragment.angular_step.y =
		(random_normalized(world) - 0.5f) * 0.1f;
	fragment.angular_step.x =
		(random_normalized(world) - 0.5f) * 0.1f;
	fragment.start_tick = now_tick;
	fragment.duration_ticks = 100 - retail_trunc(
		(random_normalized(world) - 0.5f) * -400.0f);
	++runtime.fragment_live_count;
	++runtime.fragment_cursor;
	if (runtime.fragment_cursor == runtime.fragment_capacity)
	{
		runtime.fragment_cursor = 0;
	}
}

void spawn_explosion_fragment(
	World& world,
	const glm::vec3& position,
	const glm::vec3& direction,
	float heavy_chance,
	float mesh_scale,
	float velocity_multiplier,
	bool force_rock_family,
	std::int32_t now_tick)
{
	ParticleRuntime& runtime = world.particles;
	if (runtime.fragment_capacity == 0)
	{
		return;
	}
	ParticleFragment& fragment =
		runtime.fragments[runtime.fragment_cursor];
	if (fragment.active && runtime.fragment_live_count != 0)
	{
		--runtime.fragment_live_count;
	}
	fragment = {};
	fragment.active = true;
	fragment.render_active = false;

	// Explosion_fragment_spawn, LANCER.EXE 0x004717d0. The resource
	// payloads are enlarged during Explosion_system_init at
	// 0x0046b9df..0x0046ba0f; apply those factors to the unmodified SHP
	// resources retained by this implementation.
	bool heavy = false;
	if (force_rock_family)
	{
		// The nonzero fourth stack argument takes the dedicated five-entry
		// asteroid family before the heavy/ordinary probability branch.
		fragment.model_resource = static_cast<std::uint16_t>(
			178u + world_rand15(world) % 5u);
	}
	else
	{
		heavy = heavy_chance == 1.0f;
		if (!heavy)
		{
			heavy = random_normalized(world) < heavy_chance;
		}
	}
	if (heavy)
	{
		// The 1/32768 scale keeps rand()==32767 just above -3, so truncation
		// produces -2. Cached resource 91 is consequently unreachable.
		const std::int32_t resource_offset = std::clamp(
			-retail_trunc(random_normalized(world) * -3.0f),
			0,
			2);
		fragment.model_resource = static_cast<std::uint16_t>(
			88 + resource_offset);
		fragment.scale = 2.5f * (
			mesh_scale > 0.1f ? 2.5f : 0.75f);
	}
	else if (!force_rock_family)
	{
		fragment.model_resource = standard_fragment_resource(world);
		fragment.scale =
			1.5f * mesh_scale * (0.5f + random_normalized(world));
	}
	else
	{
		fragment.scale =
			mesh_scale * (0.5f + random_normalized(world));
	}

	glm::vec3 velocity = direction
		* ((0.5f + random_normalized(world))
			* 3000.0f * velocity_multiplier);
	// MSVC evaluates the Euler source arguments right to left.
	const float rotation_z =
		(random_normalized(world) - 0.5f) * 0.5f;
	const float rotation_y =
		(random_normalized(world) - 0.5f) * 0.5f;
	const float rotation_x =
		(random_normalized(world) - 0.5f) * 0.5f;
	fragment.velocity = sl_open::math::rotation_from_euler({
		rotation_x, rotation_y, rotation_z}) * velocity;
	fragment.position = position;
	fragment.angular_step.z =
		(random_normalized(world) - 0.5f) * 0.1f;
	fragment.angular_step.y =
		(random_normalized(world) - 0.5f) * 0.1f;
	fragment.angular_step.x =
		(random_normalized(world) - 0.5f) * 0.1f;
	fragment.start_tick = now_tick;
	fragment.duration_ticks = 2000 - retail_trunc(
		(random_normalized(world) - 0.5f) * -500.0f);

	++runtime.fragment_live_count;
	++runtime.fragment_cursor;
	if (runtime.fragment_cursor == runtime.fragment_capacity)
	{
		runtime.fragment_cursor = 0;
	}
}

void emit_fragment(World& world, ParticleEmitter& emitter, std::int32_t now_tick)
{
	const glm::vec3 velocity =
		random_emitter_velocity(world, emitter, false) * 100.0f;
	spawn_fragment_from_particle(
		world,
		emitter.world_position,
		velocity,
		0.1f,
		now_tick);
}

float midpoint_render_size(const ParticleStyle& style)
{
	return particle_curve_evaluate(style.size, 0.5f) * 150.0f;
}

bool behind_camera(
	const glm::vec3& position,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward)
{
	return glm::dot(position - camera_position, camera_forward) < 0.0f;
}

std::int32_t halve_signed(std::int32_t value)
{
	return value / 2;
}

bool update_emitter(
	World& world,
	ParticleEmitter& emitter,
	std::int32_t now_tick,
	std::int32_t elapsed_ticks,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward)
{
	ParticleRuntime& runtime = world.particles;
	if (static_cast<std::int64_t>(emitter.start_tick)
			+ emitter.duration_ticks
		<= now_tick)
	{
		return false;
	}
	if (emitter.style_index >= runtime.styles.size())
	{
		return false;
	}
	const ParticleStyle& style = runtime.styles[emitter.style_index];
	if (!style.active || style.array_index >= runtime.arrays.size())
	{
		return false;
	}
	ParticleArray& array = runtime.arrays[style.array_index];
	if (!array.active || !refresh_emitter_transform(world, emitter))
	{
		return false;
	}
	const float age = static_cast<float>(now_tick - emitter.start_tick)
		/ static_cast<float>(emitter.duration_ticks);
	const float probability =
		particle_curve_evaluate(style.emission, age) * 0.01f;
	std::int32_t requested = 0;
	if (probability > 0.0f)
	{
		for (std::int32_t tick = 0; tick < elapsed_ticks; ++tick)
		{
			if (random_normalized(world) < probability)
			{
				++requested;
			}
		}
	}
	if (requested <= 0)
	{
		return true;
	}
	if (behind_camera(
			emitter.world_position,
			camera_position,
			camera_forward))
	{
		requested = halve_signed(requested);
	}
	const float distance =
		glm::distance(emitter.world_position, camera_position);
	const float attenuation =
		std::min(1.0f, midpoint_render_size(style) / distance);
	requested = retail_trunc(
		static_cast<float>(requested) * attenuation);
	if (requested <= 0)
	{
		return true;
	}

	for (std::int32_t index = 0;
		index < array.capacity && requested > 0;
		++index)
	{
		ParticleEmissionMode mode = style.emission_mode;
		if (mode == ParticleEmissionMode::rare_debris)
		{
			mode = world_rand15(world) % 200u == 0
				? ParticleEmissionMode::debris_only
				: ParticleEmissionMode::ordinary;
		}
		if (mode == ParticleEmissionMode::ordinary)
		{
			ParticleSlot& slot = array.slots[index];
			if (!tick_end_at_or_before(
					slot.birth_tick, slot.lifetime_ticks, now_tick))
			{
				continue;
			}
			spawn_particle(world, emitter, index, now_tick);
			// 0x0049c884 consumes one draw whose value is not retained.
			(void)world_rand15(world);
			array.visuals[index].position +=
				array.slots[index].velocity
				* static_cast<float>(elapsed_ticks);
			--requested;
		}
		else if (mode == ParticleEmissionMode::debris_only)
		{
			// Retail neither tests slot expiry nor consumes requested count.
			emit_fragment(world, emitter, now_tick);
		}
	}
	return true;
}

void update_fragments(
	ParticleRuntime& runtime,
	std::int32_t now_tick,
	std::int32_t elapsed_ticks)
{
	for (std::uint16_t index = 0;
		index < runtime.fragment_capacity;
		++index)
	{
		ParticleFragment& fragment = runtime.fragments[index];
		fragment.render_active = false;
		if (!fragment.active)
		{
			continue;
		}
		fragment.position +=
			fragment.velocity * (static_cast<float>(elapsed_ticks) * 0.01f);
		fragment.orientation =
			fragment.orientation
			* sl_open::math::rotation_from_euler(fragment.angular_step);
		if (static_cast<std::int64_t>(now_tick)
			> static_cast<std::int64_t>(fragment.start_tick)
				+ fragment.duration_ticks)
		{
			fragment.active = false;
			if (runtime.fragment_live_count != 0)
			{
				--runtime.fragment_live_count;
			}
			continue;
		}
		fragment.render_active = true;
	}
}

void update_particle_arrays(
	ParticleRuntime& runtime,
	std::int32_t now_tick,
	std::int32_t elapsed_ticks)
{
	for (ParticleArray& array : runtime.arrays)
	{
		if (!array.active)
		{
			continue;
		}
		std::int32_t highest_live = 0;
		for (std::int32_t index = 0;
			index < array.high_water;
			++index)
		{
			ParticleSlot& slot = array.slots[index];
			ParticleVisual& visual = array.visuals[index];
			if (tick_end_at_or_before(
					slot.birth_tick, slot.lifetime_ticks, now_tick))
			{
				visual.active = false;
				continue;
			}
			visual.position +=
				slot.velocity
					* static_cast<float>(elapsed_ticks);
			if (slot.style_index >= runtime.styles.size())
			{
				visual.active = false;
				continue;
			}
			const ParticleStyle& style =
				runtime.styles[slot.style_index];
			const float age =
				static_cast<float>(now_tick - slot.birth_tick)
				/ static_cast<float>(slot.lifetime_ticks);
			visual.size = particle_curve_evaluate(style.size, age);
			visual.color = glm::clamp(
				glm::vec3{
					particle_curve_evaluate(style.red, age),
					particle_curve_evaluate(style.green, age),
					particle_curve_evaluate(style.blue, age),
				},
				glm::vec3{0.0f},
				glm::vec3{1.0f});
			visual.alpha = 1.0f;
			visual.active = true;
			highest_live = index;
		}
		// The retail retained index begins at zero, so an occupied array
		// always submits at least record zero with its activity byte.
		array.high_water = std::min(array.capacity, highest_live + 1);
	}
}

std::uint8_t damage_effect_severity(
	const WorldObject& object,
	const assets::ShipStatsTable& ship_stats)
{
	const assets::ObjectTypeStats& stats =
		ship_stats.records[object.type].object;
	// The helper chained at 0x00492f75..0x00492fb3 is 0x004c0df0.
	// It returns the lesser of its two arguments, so damage severity follows
	// the weakest structural bank rather than the strongest one.
	float structural = std::min(
		object.secondary_shields[3], object.secondary_shields[2]);
	structural = std::min(structural, object.secondary_shields[0]);
	structural = std::min(structural, object.secondary_shields[1]);
	const float structural_max =
		static_cast<float>(stats.structural_bank_max * 6);
	// Threshold constants are 0.5, 0.7, and 0.9 at
	// 0x004dc408/0x004dc484/0x004dc470.
	std::uint8_t severity = structural >= structural_max * 0.5f
		? structural >= structural_max * 0.7f
			? structural >= structural_max * 0.9f ? 0u : 1u
			: 2u
		: 3u;
	const float primary_maximum =
		static_cast<float>(stats.primary_bank_max * 6);
	// Preserve the compiled sequence at 0x00492fdf..0x00493055 exactly.
	// It reduces banks 3 and 2, then bank 0, then bank 3 a second time;
	// bank 1 is not read by this damage-effect decision.
	float primary_ratio = std::min(
		object.primary_shields[3] / primary_maximum,
		object.primary_shields[2] / primary_maximum);
	primary_ratio = std::min(
		primary_ratio, object.primary_shields[0] / primary_maximum);
	primary_ratio = std::min(
		primary_ratio, object.primary_shields[3] / primary_maximum);
	if (primary_ratio > 0.9f)
	{
		// 0x004930d2 retains severity one after a damaged ship's primary
		// bank recovers above 90%; an undamaged object stays at zero.
		severity = object.damage_effect_severity != 0 ? 1u : 0u;
	}
	return severity;
}

void replace_damage_effect(
	World& world,
	WorldObject& object,
	std::uint16_t object_index,
	std::uint8_t severity,
	std::uint32_t simulation_tick)
{
	std::uint16_t model_reference = UINT16_MAX;
	const assets::GameplayLocator* effect_locator = nullptr;
	for (std::uint16_t reference = 0;
		reference < object.model_references.size() && effect_locator == nullptr;
		++reference)
	{
		const ObjectModelReference& model =
			object.model_references[reference];
		if (model.locators == nullptr)
		{
			continue;
		}
		for (const assets::GameplayLocator& locator : *model.locators)
		{
			if (locator.source_node == model.source_node
				&& locator.type == 2)
			{
				model_reference = reference;
				effect_locator = &locator;
				break;
			}
		}
	}
	object.damage_effect_severity = severity;
	if (effect_locator == nullptr)
	{
		return;
	}
	object.damage_emitter = {};
	if (severity == 0 || severity > 3)
	{
		return;
	}
	const std::uint32_t style = kDamageLightStyle + severity - 1u;
	if (!particle_emitter_initialize(
			world.particles,
			object.damage_emitter,
			style,
			999999u,
			simulation_tick))
	{
		return;
	}
	ParticleEmitter& emitter = object.damage_emitter;
	// mission_replace_object_damage_effect, 0x00494446..0x00494639,
	// copies the selected type-two tag-9 locator's position and complete
	// authored matrix.  A negative locator +0x50 scalar reverses the third
	// matrix axis; that field is GameplayLocator::dimensions.z.
	emitter.local_position = effect_locator->position;
	emitter.local_basis = effect_locator->basis;
	if (effect_locator->dimensions.z < 0.0f)
	{
		emitter.local_basis[2] = -emitter.local_basis[2];
	}
	emitter.direction_center = {0.0f, 0.0f, 1.0f};
	const float spread = severity == 1 ? 0.15f : 0.25f;
	emitter.direction_spread = {spread, spread, 0.0f};
	emitter.speed_base = 30.0f;
	emitter.speed_random = 6.0f;
	emitter.owner_index = object_index;
	emitter.owner_generation = object.generation;
	emitter.model_reference = model_reference;
	emitter.model_owned = true;
}

void service_damage_effects(
	World& world,
	const assets::ShipStatsTable& ship_stats,
	std::int32_t now_tick,
	std::int32_t elapsed_ticks,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward)
{
	for (std::uint16_t index = 0; index < kMaxGameObjects; ++index)
	{
		WorldObject& object = world.objects[index];
		if (!object.active
			|| object.type >= assets::kShipStatsCount
			|| object.type == 0x1fu
			|| (object.runtime_flags & kObjectSimulationExcludedFlags) != 0)
		{
			continue;
		}
		if (object.damage_emitter.active)
		{
			ParticleStyle& style = world.particles.styles[
				object.damage_emitter.style_index];
			// mission_update_frame rewrites the selected template's emission
			// curve to (50,0,0) and restarts the emitter every frame.
			style.emission = particle_curve_fit_start_mid_end(
				50.0f, 0.0f, 0.0f);
			object.damage_emitter.start_tick = now_tick;
			object.damage_emitter.inherited_velocity =
				object.linear_velocity * 0.25f;
			if (!update_emitter(
					world,
					object.damage_emitter,
					now_tick,
					elapsed_ticks,
					camera_position,
					camera_forward))
			{
				object.damage_emitter = {};
			}
			else if (object.damage_effect_severity == 3
				&& world_rand15(world) % 10u == 0u)
			{
				(void)explosion_billboard_create(
					world.death_effects,
					world,
					object.damage_emitter.world_position
						+ object.linear_velocity * 0.25f,
					glm::vec3{0.0f},
					ExplosionBillboardType::separate_frames,
					// 0x00492f32..0x00492f51 multiplies rand() by
					// 1/32768, then 0.2, adds 0.1, and finally applies
					// the object's radius. This is a subtle 0.1..0.3-radius
					// flash, not the 0.5..1.5-radius debris scaling used by
					// other explosion paths.
					(0.1f + random_normalized(world) * 0.2f)
						* object.radius,
					90,
					false,
					0,
					false,
					false,
					static_cast<std::uint32_t>(now_tick));
			}
		}
		const std::uint8_t severity =
			damage_effect_severity(object, ship_stats);
		if (severity != object.damage_effect_severity)
		{
			replace_damage_effect(
				world,
				object,
				index,
				severity,
				static_cast<std::uint32_t>(now_tick));
		}
	}
}
}

ParticleCurve particle_curve_fit_start_mid_end(
	float start,
	float midpoint,
	float end)
{
	return {
		2.0f * (start + end) - 4.0f * midpoint,
		4.0f * midpoint - (3.0f * start + end),
		start,
	};
}

float particle_curve_evaluate(const ParticleCurve& curve, float time)
{
	return curve.quadratic * time * time
		+ curve.linear * time
		+ curve.constant;
}

bool particle_system_initialize(
	ParticleRuntime& runtime,
	std::uint8_t graphics_quality)
{
	if (runtime.initialized)
	{
		particle_system_configure_fragments(runtime, graphics_quality);
		return true;
	}
	particle_system_shutdown(runtime);
	runtime.fragment_capacity = 500;
	particle_system_configure_fragments(runtime, graphics_quality);
	const std::int16_t default_array = particle_array_create(
		runtime,
		static_cast<std::int32_t>(kMaxSharedParticles),
		ParticleTexture::partic4,
		true);
	if (default_array < 0)
	{
		if (!runtime.initialization_failure_reported)
		{
			diagnostics::mission_log(
				"particle system initialization failed stage=default-array");
			runtime.initialization_failure_reported = true;
		}
		particle_system_shutdown(runtime);
		return false;
	}
	runtime.default_array = static_cast<std::uint8_t>(default_array);
	for (std::uint8_t damage = 0; damage < 3; ++damage)
	{
		const std::int16_t array = particle_array_create(
			runtime,
			static_cast<std::int32_t>(kMaxSharedParticles),
			damage == 2
				? ParticleTexture::partic7
				: ParticleTexture::partic4,
			damage != 2);
		if (array < 0)
		{
			if (!runtime.initialization_failure_reported)
			{
				diagnostics::mission_log(
					"particle system initialization failed "
					"stage=damage-array kind=%u",
					static_cast<unsigned>(damage + 1u));
				runtime.initialization_failure_reported = true;
			}
			particle_system_shutdown(runtime);
			return false;
		}
		runtime.damage_arrays[damage] = static_cast<std::uint8_t>(array);
	}
	try
	{
		initialize_explosion_styles(runtime);
	}
	catch (...)
	{
		if (!runtime.initialization_failure_reported)
		{
			diagnostics::mission_log(
				"particle system initialization failed stage=styles");
			runtime.initialization_failure_reported = true;
		}
		particle_system_shutdown(runtime);
		return false;
	}
	runtime.initialized = true;
	runtime.initialization_failure_reported = false;
	return true;
}

void particle_system_reset(ParticleRuntime& runtime)
{
	if (!runtime.initialized)
	{
		return;
	}
	for (ParticleArray& array : runtime.arrays)
	{
		if (!array.active)
		{
			continue;
		}
		for (ParticleSlot& slot : array.slots)
		{
			slot.birth_tick = 0;
			slot.lifetime_ticks = 0;
		}
		array.high_water = 0;
	}
	for (ParticleEmitter& emitter : runtime.emitters)
	{
		emitter = {};
	}
	for (ParticleFragment& fragment : runtime.fragments)
	{
		fragment = {};
	}
	runtime.fragment_cursor = 0;
	runtime.fragment_live_count = 0;
	runtime.array_saturation_reported = false;
	runtime.emitter_saturation_reported = false;
}

void particle_system_shutdown(ParticleRuntime& runtime)
{
	for (std::uint8_t index = 0; index < runtime.arrays.size(); ++index)
	{
		particle_array_destroy(runtime, index);
	}
	std::vector<ParticleStyle>{}.swap(runtime.styles);
	runtime.emitters = {};
	runtime.fragments = {};
	runtime.fragment_capacity = 500;
	runtime.fragment_cursor = 0;
	runtime.fragment_live_count = 0;
	runtime.default_array = 0;
	std::fill(
		std::begin(runtime.damage_arrays),
		std::end(runtime.damage_arrays),
		0);
	runtime.initialized = false;
	runtime.array_saturation_reported = false;
	runtime.emitter_saturation_reported = false;
}

bool particle_system_ready(const ParticleRuntime& runtime)
{
	return runtime.initialized
		&& runtime.default_array < runtime.arrays.size()
		&& runtime.arrays[runtime.default_array].active;
}

void particle_system_configure_fragments(
	ParticleRuntime& runtime,
	std::uint8_t graphics_quality)
{
	std::uint16_t capacity = runtime.fragment_capacity;
	switch (graphics_quality)
	{
	case 0: capacity = 100; break;
	case 1: capacity = 300; break;
	case 2: capacity = 500; break;
	default: return;
	}
	if (capacity == runtime.fragment_capacity)
	{
		return;
	}
	for (std::uint16_t index = capacity;
		index < runtime.fragment_capacity;
		++index)
	{
		runtime.fragments[index] = {};
	}
	runtime.fragment_capacity = capacity;
	runtime.fragment_cursor = 0;
	runtime.fragment_live_count = 0;
	for (ParticleFragment& fragment : runtime.fragments)
	{
		fragment = {};
	}
}

std::int16_t particle_array_create(
	ParticleRuntime& runtime,
	std::int32_t capacity,
	ParticleTexture texture,
	bool additive)
{
	for (std::uint8_t index = 0; index < runtime.arrays.size(); ++index)
	{
		ParticleArray& array = runtime.arrays[index];
		if (array.active)
		{
			continue;
		}
		if (capacity <= 0)
		{
			return -1;
		}
		try
		{
			array.slots.assign(
				static_cast<std::size_t>(capacity), ParticleSlot{});
			array.visuals.assign(
				static_cast<std::size_t>(capacity), ParticleVisual{});
		}
		catch (...)
		{
			std::vector<ParticleSlot>{}.swap(array.slots);
			std::vector<ParticleVisual>{}.swap(array.visuals);
			array = {};
			return -1;
		}
		array.capacity = capacity;
		array.high_water = 0;
		array.texture = texture;
		array.additive = additive;
		array.active = true;
		return index;
	}
	if (!runtime.array_saturation_reported)
	{
		diagnostics::mission_log(
			"particle array registry exhausted capacity=%u",
			static_cast<unsigned>(kParticleArrayCount));
		runtime.array_saturation_reported = true;
	}
	return -1;
}

void particle_array_destroy(
	ParticleRuntime& runtime,
	std::uint8_t array_index)
{
	if (array_index >= runtime.arrays.size())
	{
		return;
	}
	ParticleArray& array = runtime.arrays[array_index];
	std::vector<ParticleSlot>{}.swap(array.slots);
	std::vector<ParticleVisual>{}.swap(array.visuals);
	array.capacity = 0;
	array.high_water = 0;
	array.active = false;
}

std::int32_t particle_style_create(
	ParticleRuntime& runtime,
	std::uint8_t array_index)
{
	if (array_index >= runtime.arrays.size()
		|| !runtime.arrays[array_index].active)
	{
		return -1;
	}
	for (std::uint32_t index = 0; index < runtime.styles.size(); ++index)
	{
		if (runtime.styles[index].active)
		{
			continue;
		}
		ParticleStyle& style = runtime.styles[index];
		style = {};
		style.array_index = array_index;
		style.burst_distance_scale = 1.0f;
		style.active = true;
		return static_cast<std::int32_t>(index);
	}
	try
	{
		ParticleStyle style;
		style.array_index = array_index;
		style.burst_distance_scale = 1.0f;
		style.active = true;
		runtime.styles.push_back(style);
	}
	catch (...)
	{
		return -1;
	}
	return static_cast<std::int32_t>(runtime.styles.size() - 1);
}

void particle_style_destroy(
	ParticleRuntime& runtime,
	std::uint32_t style_index)
{
	if (style_index >= runtime.styles.size())
	{
		return;
	}
	runtime.styles[style_index] = {};
}

bool particle_emitter_initialize(
	const ParticleRuntime& runtime,
	ParticleEmitter& emitter,
	std::uint32_t style_index,
	std::uint32_t duration_ticks,
	std::uint32_t simulation_tick)
{
	if (!particle_system_ready(runtime)
		|| style_index >= runtime.styles.size()
		|| !runtime.styles[style_index].active)
	{
		return false;
	}
	emitter = {};
	emitter.duration_ticks = static_cast<std::int32_t>(duration_ticks);
	emitter.start_tick = retail_tick(simulation_tick);
	emitter.local_basis = glm::mat3{1.0f};
	emitter.rectangle = {0.0f, 1.0f, 0.0f, 1.0f};
	emitter.style_index = style_index;
	emitter.active = true;
	return true;
}

ParticleEmitter* particle_emitter_reserve_automatic(
	World& world,
	std::uint32_t style_index,
	std::uint32_t duration_ticks,
	std::uint32_t simulation_tick)
{
	ParticleRuntime& runtime = world.particles;
	for (ParticleEmitter& emitter : runtime.emitters)
	{
		if (emitter.active)
		{
			continue;
		}
		if (!particle_emitter_initialize(
				runtime,
				emitter,
				style_index,
				duration_ticks,
				simulation_tick))
		{
			return nullptr;
		}
		return &emitter;
	}
	if (!runtime.emitter_saturation_reported)
	{
		diagnostics::mission_log(
			"particle emitter pool exhausted capacity=%u",
			static_cast<unsigned>(kParticleEmitterCount));
		runtime.emitter_saturation_reported = true;
	}
	return nullptr;
}

void particle_emitter_release_automatic(ParticleEmitter& emitter)
{
	emitter = {};
}

bool particle_emitter_service_owned(
	World& world,
	ParticleEmitter& emitter,
	std::uint32_t simulation_tick,
	std::uint32_t elapsed_simulation_ticks,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward)
{
	if (!particle_system_ready(world.particles)
		|| !emitter.active
		|| elapsed_simulation_ticks == 0)
	{
		return false;
	}
	return update_emitter(
		world,
		emitter,
		retail_tick(simulation_tick),
		retail_elapsed_ticks(elapsed_simulation_ticks),
		camera_position,
		camera_forward);
}

void particle_system_service(
	World& world,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick,
	std::uint32_t elapsed_simulation_ticks,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward)
{
	ParticleRuntime& runtime = world.particles;
	if (!particle_system_ready(runtime)
		|| elapsed_simulation_ticks == 0)
	{
		return;
	}
	const std::int32_t now_tick = retail_tick(simulation_tick);
	const std::int32_t elapsed_ticks =
		retail_elapsed_ticks(elapsed_simulation_ticks);
	update_fragments(runtime, now_tick, elapsed_ticks);
	// Particle_array service runs before Explosion_system_service in the
	// retail scene traversal. Particle_emitter_update then advances each
	// newly allocated BMO once by the current service delta so it catches
	// up with the entries which were already present. Servicing arrays
	// after emitters advanced every new particle twice in this port.
	update_particle_arrays(runtime, now_tick, elapsed_ticks);
	service_damage_effects(
		world,
		ship_stats,
		now_tick,
		elapsed_ticks,
		camera_position,
		camera_forward);
	for (ParticleEmitter& emitter : runtime.emitters)
	{
		if (!emitter.active)
		{
			continue;
		}
		if (!update_emitter(
				world,
				emitter,
				now_tick,
				elapsed_ticks,
				camera_position,
				camera_forward))
		{
			emitter = {};
		}
	}
}

namespace
{
bool particle_emitter_burst_at_tick(
	World& world,
	ParticleEmitter& emitter,
	std::int32_t requested_count,
	std::int32_t now_tick,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward)
{
	ParticleRuntime& runtime = world.particles;
	if (!particle_system_ready(runtime)
		|| emitter.style_index >= runtime.styles.size())
	{
		return false;
	}
	const ParticleStyle& style = runtime.styles[emitter.style_index];
	if (!style.active
		|| style.array_index >= runtime.arrays.size()
		|| !refresh_emitter_transform(world, emitter))
	{
		return false;
	}
	ParticleArray& array = runtime.arrays[style.array_index];
	if (!array.active)
	{
		return false;
	}
	if (behind_camera(
			emitter.world_position,
			camera_position,
			camera_forward))
	{
		requested_count = halve_signed(requested_count);
	}
	float attenuation = midpoint_render_size(style);
	if (style.burst_distance_scale > 0.0f)
	{
		attenuation /=
			glm::distance(emitter.world_position, camera_position)
			* style.burst_distance_scale;
	}
	attenuation = std::min(attenuation, 1.0f);
	requested_count = retail_trunc(
		static_cast<float>(requested_count) * attenuation);
	for (std::int32_t index = 0;
		index < array.capacity && requested_count > 0;
		++index)
	{
		const ParticleSlot& slot = array.slots[index];
		if (!tick_end_strictly_before(
				slot.birth_tick, slot.lifetime_ticks, now_tick))
		{
			continue;
		}
		spawn_particle(world, emitter, index, now_tick);
		--requested_count;
	}
	return true;
}
}

bool particle_emitter_burst(
	World& world,
	ParticleEmitter& emitter,
	std::int32_t requested_count,
	std::uint32_t simulation_tick,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward)
{
	return particle_emitter_burst_at_tick(
		world,
		emitter,
		requested_count,
		retail_tick(simulation_tick),
		camera_position,
		camera_forward);
}

bool particle_emitter_create_model_owned(
	World& world,
	ObjectHandle owner,
	std::uint16_t model_reference,
	const glm::vec3& local_position,
	const glm::vec3& local_direction,
	const glm::vec3& direction_spread,
	float speed_base,
	float speed_random,
	std::uint32_t duration_ticks,
	ParticleEmitterStyle style,
	std::uint32_t simulation_tick)
{
	ParticleRuntime& runtime = world.particles;
	if (!particle_system_ready(runtime)
		|| world_resolve(world, owner) == nullptr)
	{
		return false;
	}
	const std::uint8_t style_index =
		static_cast<std::uint8_t>(style);
	ParticleEmitter* emitter = particle_emitter_reserve_automatic(
		world,
		style_index,
		duration_ticks,
		simulation_tick);
	if (emitter == nullptr)
	{
		return false;
	}
	emitter->local_position = local_position;
	emitter->direction_center = local_direction;
	emitter->direction_spread = direction_spread;
	emitter->speed_base = speed_base;
	emitter->speed_random = speed_random;
	emitter->owner_index = owner.index;
	emitter->owner_generation = owner.generation;
	emitter->model_reference = model_reference;
	emitter->model_owned = true;
	if (!refresh_emitter_transform(world, *emitter))
	{
		particle_emitter_release_automatic(*emitter);
		return false;
	}
	return true;
}

bool particle_emitter_burst_world(
	World& world,
	const glm::vec3& position,
	const glm::mat3& basis,
	const glm::vec3& direction,
	const glm::vec3& direction_spread,
	float speed_base,
	float speed_random,
	std::int32_t requested_count,
	ParticleEmitterStyle style,
	std::uint32_t simulation_tick,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward,
	const glm::vec3& inherited_velocity)
{
	ParticleEmitter emitter;
	if (!particle_emitter_initialize(
			world.particles,
			emitter,
			static_cast<std::uint8_t>(style),
			0,
			simulation_tick))
	{
		return false;
	}
	emitter.local_position = position;
	emitter.local_basis = basis;
	emitter.direction_center = direction;
	emitter.direction_spread = direction_spread;
	emitter.speed_base = speed_base;
	emitter.speed_random = speed_random;
	emitter.inherited_velocity = inherited_velocity;
	emitter.model_owned = false;
	return particle_emitter_burst(
		world,
		emitter,
		requested_count,
		simulation_tick,
		camera_position,
		camera_forward);
}

bool particle_emitter_burst_world_delayed(
	World& world,
	const glm::vec3& position,
	const glm::mat3& basis,
	const glm::vec3& direction,
	const glm::vec3& direction_spread,
	float speed_base,
	float speed_random,
	std::int32_t requested_count,
	ParticleEmitterStyle style,
	std::uint32_t simulation_tick,
	std::uint32_t delay_ticks,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward)
{
	ParticleEmitter emitter;
	if (!particle_emitter_initialize(
			world.particles,
			emitter,
			static_cast<std::uint8_t>(style),
			0,
			simulation_tick))
	{
		return false;
	}
	emitter.local_position = position;
	emitter.local_basis = basis;
	emitter.direction_center = direction;
	emitter.direction_spread = direction_spread;
	emitter.speed_base = speed_base;
	emitter.speed_random = speed_random;
	emitter.model_owned = false;
	return particle_emitter_burst_at_tick(
		world,
		emitter,
		requested_count,
		retail_tick(simulation_tick)
			+ static_cast<std::int32_t>(delay_ticks),
		camera_position,
		camera_forward);
}

void particle_fragment_burst(
	World& world,
	const glm::vec3& position,
	float heavy_chance,
	float mesh_scale,
	float velocity_multiplier,
	std::uint16_t count,
	std::uint32_t simulation_tick)
{
	const std::int32_t now_tick = retail_tick(simulation_tick);
	for (std::uint16_t index = 0; index < count; ++index)
	{
		// The constructor arguments are evaluated Z, Y, X by retail MSVC.
		const float direction_z = random_normalized(world) - 0.5f;
		const float direction_y = random_normalized(world) - 0.5f;
		const float direction_x = random_normalized(world) - 0.5f;
		glm::vec3 direction{direction_x, direction_y, direction_z};
		const float length = glm::length(direction);
		if (length > 0.0f)
		{
			direction /= length;
		}
		spawn_explosion_fragment(
			world,
			position,
			direction,
			heavy_chance,
			mesh_scale,
			velocity_multiplier,
			false,
			now_tick);
	}
}

void particle_fragment_directional_burst(
	World& world,
	const glm::vec3& position,
	const glm::vec3& direction,
	float heavy_chance,
	float mesh_scale,
	float velocity_multiplier,
	std::uint16_t count,
	std::uint32_t simulation_tick)
{
	const std::int32_t now_tick = retail_tick(simulation_tick);
	for (std::uint16_t index = 0; index < count; ++index)
	{
		spawn_explosion_fragment(
			world,
			position,
			direction,
			heavy_chance,
			mesh_scale,
			velocity_multiplier,
			false,
			now_tick);
	}
}

void particle_uber_debris_spawn(
	World& world,
	const glm::vec3& position,
	const glm::vec3& direction,
	std::uint32_t simulation_tick)
{
	spawn_explosion_fragment(
		world,
		position,
		direction,
		0.0f,
		0.1f,
		1.0f,
		false,
		retail_tick(simulation_tick));
}
}
