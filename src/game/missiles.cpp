#include "game/missiles.hpp"

#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "game/chaff.hpp"
#include "game/disruption_effects.hpp"
#include "game/weapons.hpp"
#include "mission/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace sl_open::game
{
namespace
{
constexpr std::uint8_t kMissileSoundDefinitions[
	assets::kMissileStatsCount] = {
	15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 0,
};
constexpr float kMovementRetention = 0.8399999737739563f;
constexpr float kSteeringRetention = 0.7099999785423279f;
constexpr float kGuidanceGain = 5.729577541351318f;
constexpr float kMaximumGuidanceDistanceSquared = 999999995904.0f;
constexpr float kProximityFuseDistanceSquared = 100000000.0f;
constexpr float kReacquisitionForwardDot = 0.699999988079071f;
constexpr std::uint8_t kMissileTrailFlags[assets::kMissileStatsCount] = {
	0x29, 0x05, 0x03, 0x09, 0x25, 0x31, 0x29, 0x03, 0x23, 0x29, 0x00,
};
constexpr std::uint8_t
	kMissileTrailPointCounts[assets::kMissileStatsCount] = {
	25, 5, 35, 60, 35, 40, 45, 30, 40, 70, 0,
};
constexpr float kMissileTrailColors[assets::kMissileStatsCount][3] = {
	{0.3f, 0.3f, 0.6f},
	{0.7f, 0.9f, 1.0f},
	{0.8f, 0.6f, 1.0f},
	{0.5f, 0.5f, 0.5f},
	{0.2f, 0.1f, 0.5f},
	{1.0f, 1.0f, 1.0f},
	{0.0f, 0.0f, 1.0f},
	{0.2f, 0.0f, 0.5f},
	{0.5f, 0.5f, 0.5f},
	{1.0f, 1.0f, 1.0f},
	{0.0f, 0.0f, 0.0f},
};
constexpr std::uint8_t kMissileSideFadeTicks[assets::kMissileStatsCount] = {
	0, 5, 0, 0, 10, 0, 0, 0, 0, 25, 0,
};
constexpr std::uint8_t kMissileSideCounts[assets::kMissileStatsCount] = {
	0, 3, 0, 4, 3, 0, 0, 0, 0, 3, 0,
};
constexpr float kMissileSideColors[assets::kMissileStatsCount][3] = {
	{1.0f, 1.0f, 1.0f},
	{0.3f, 0.7f, 0.8f},
	{1.0f, 1.0f, 1.0f},
	{1.0f, 1.0f, 1.0f},
	{0.5f, 0.5f, 0.5f},
	{1.0f, 1.0f, 1.0f},
	{1.0f, 1.0f, 1.0f},
	{1.0f, 1.0f, 1.0f},
	{1.0f, 1.0f, 1.0f},
	{1.0f, 1.0f, 1.0f},
	{1.0f, 1.0f, 1.0f},
};
constexpr glm::vec3 kHostileTorpedoTrailColor{
	0.8901963233947754f,
	0.7803921699523926f,
	0.545098066329956f,
};

std::uint16_t missile_random15(std::uint32_t& seed)
{
	seed = seed * 0x343fdu + 0x269ec3u;
	return static_cast<std::uint16_t>((seed >> 16) & 0x7fffu);
}

float missile_random_unit(std::uint32_t& seed)
{
	return static_cast<float>(missile_random15(seed))
		* (1.0f / 32767.0f);
}

glm::mat3 missile_visual_orientation(const Missile& missile)
{
	// Attached and fired ordnance SROs both cross this authored half-turn.
	// Trail bounds are taken from that same SRO and must use the identical
	// visual basis rather than the missile's forward-control basis.
	return math::postrotate(
		missile.scene_orientation,
		-glm::pi<float>(),
		{0.0f, 1.0f, 0.0f});
}

MissileTrailRing missile_trail_ring(
	const Missile& missile,
	std::uint8_t flags,
	std::uint32_t simulation_tick,
	std::uint32_t* random_seed)
{
	MissileTrailRing ring;
	const glm::mat3 visual_orientation = missile_visual_orientation(missile);
	glm::vec3 local[4] = {
		{missile.bounds_max.x, missile.bounds_max.y, missile.bounds_min.z},
		{missile.bounds_min.x, missile.bounds_max.y, missile.bounds_min.z},
		{missile.bounds_min.x, missile.bounds_min.y, missile.bounds_min.z},
		{missile.bounds_max.x, missile.bounds_min.y, missile.bounds_min.z},
	};
	if (random_seed != nullptr && (flags & 0x10u) != 0)
	{
		const float angular_sum =
			missile.smoothed_pitch + missile.smoothed_yaw;
		const float cosine = std::cos(
			static_cast<float>(simulation_tick) * angular_sum * 0.01f);
		const float sine = std::sin(
			static_cast<float>(simulation_tick) * angular_sum * 0.01f);
		for (glm::vec3& point : local)
		{
			const float x = point.x * (angular_sum * 0.5f + 1.0f);
			const float y = point.y * 0.2f;
			point.x = x * cosine - y * sine;
			point.y = x * sine + y * cosine;
		}
	}
	if (random_seed != nullptr && (flags & 0x20u) != 0)
	{
		for (glm::vec3& point : local)
		{
			point.x *= missile_random_unit(*random_seed) * 1.5f + 0.5f;
			point.y *= missile_random_unit(*random_seed) * 1.5f + 0.5f;
		}
	}
	for (std::size_t index = 0; index < std::size(local); ++index)
	{
		ring.points[index] =
			missile.scene_position + visual_orientation * local[index];
	}
	ring.alpha = 1.0f;
	ring.intensity = std::max(missile.throttle, 0.5f);
	return ring;
}

MissileTrailRing missile_side_trail_ring(
	const Missile& missile,
	std::uint8_t side,
	std::uint8_t side_count,
	std::uint32_t simulation_tick,
	std::uint32_t& random_seed)
{
	MissileTrailRing ring;
	const glm::mat3 visual_orientation = missile_visual_orientation(missile);
	glm::vec3 local[4] = {
		{missile.bounds_max.x, missile.bounds_max.y, missile.bounds_min.z},
		{missile.bounds_min.x, missile.bounds_max.y, missile.bounds_min.z},
		{missile.bounds_min.x, missile.bounds_min.y, missile.bounds_min.z},
		{missile.bounds_max.x, missile.bounds_min.y, missile.bounds_min.z},
	};
	const float angular_sum =
		missile.smoothed_pitch + missile.smoothed_yaw;
	const float phase = static_cast<float>(side) * glm::two_pi<float>()
		/ static_cast<float>(side_count)
		+ static_cast<float>(simulation_tick) * angular_sum * 0.01f;
	const float cosine = std::cos(phase);
	const float sine = std::sin(phase);
	for (std::size_t index = 0; index < std::size(local); ++index)
	{
		glm::vec3& point = local[index];
		const float x_scale = missile_random_unit(random_seed) + 1.0f;
		const float y_scale = missile_random_unit(random_seed) + 1.0f;
		const float x_offset =
			missile_random_unit(random_seed) * 0.3f + 0.5f;
		const float x = x_scale * point.x * 0.2f
			+ x_offset * missile.bounds_max.x;
		const float y = y_scale * point.y * 0.2f;
		point.x = x * cosine - y * sine;
		point.y = x * sine + y * cosine;
		ring.points[index] =
			missile.scene_position + visual_orientation * point;
	}
	ring.alpha = 1.0f;
	ring.intensity = std::max(missile.throttle, 0.5f);
	return ring;
}

void fade_missile_trails(
	MissileRuntime& runtime,
	std::uint32_t simulation_tick)
{
	for (MissileTrail& trail : runtime.trails)
	{
		if (!trail.active || trail.fade_ticks == 0)
		{
			continue;
		}
		const std::uint32_t elapsed =
			simulation_tick - trail.last_update_tick;
		trail.last_update_tick = simulation_tick;
		const float fade = static_cast<float>(elapsed) * 0.015f
			/ (static_cast<float>(trail.fade_ticks) * 0.04f);
		bool expired = true;
		for (std::uint16_t index = 0; index < trail.ring_count; ++index)
		{
			MissileTrailRing& ring = trail.rings[index];
			ring.alpha -= fade;
			ring.intensity = std::max(ring.alpha, 0.0f);
			expired = expired && ring.alpha < 0.0f;
		}
		if (trail.side_fade_ticks != 0)
		{
			const float side_fade = static_cast<float>(elapsed) * 0.015f
				/ (static_cast<float>(trail.side_fade_ticks) * 0.04f);
			for (std::uint8_t side = 0; side < trail.side_count; ++side)
			{
				MissileTrailRibbon& ribbon = trail.side_ribbons[side];
				bool side_expired = true;
				for (std::uint16_t index = 0;
					index < ribbon.ring_count;
					++index)
				{
					MissileTrailRing& ring = ribbon.rings[index];
					ring.alpha -= side_fade;
					ring.intensity = std::max(ring.alpha, 0.0f);
					side_expired = side_expired && ring.alpha < 0.0f;
				}
				if (!trail.attached && side_expired)
				{
					ribbon = {};
				}
			}
		}
		if (!trail.attached && expired)
		{
			trail = {};
		}
	}
}

void update_missile_trail(
	MissileRuntime& runtime,
	Missile& missile,
	std::uint32_t simulation_tick,
	std::uint32_t& random_seed)
{
	if (missile.trail_slot < 0
		|| static_cast<std::size_t>(missile.trail_slot)
			>= std::size(runtime.trails))
	{
		return;
	}
	MissileTrail& trail = runtime.trails[missile.trail_slot];
	if (!trail.active || !trail.attached || trail.ring_count == 0
		|| trail.type >= std::size(kMissileTrailPointCounts))
	{
		return;
	}
	const std::uint16_t capacity =
		kMissileTrailPointCounts[trail.type];
	if (capacity < 2)
	{
		return;
	}
	if ((trail.flags & 4u) != 0 && trail.side_count != 0)
	{
		MissileTrailRing side_rings[4];
		for (std::uint8_t side = 0; side < trail.side_count; ++side)
		{
			side_rings[side] = missile_side_trail_ring(
				missile,
				side,
				trail.side_count,
				simulation_tick,
				random_seed);
		}
		MissileTrailRibbon& first = trail.side_ribbons[0];
		if (first.ring_count >= 2)
		{
			const float width =
				missile.bounds_max.x - missile.bounds_min.x;
			const float distance_from_fixed_ring = glm::distance(
				first.rings[first.ring_count - 2u].points[0],
				side_rings[0].points[0]) / width;
			for (std::uint8_t side = 0; side < trail.side_count; ++side)
			{
				MissileTrailRibbon& ribbon = trail.side_ribbons[side];
				ribbon.rings[ribbon.ring_count - 1u] = side_rings[side];
			}
			if (distance_from_fixed_ring > 3.0f)
			{
				for (std::uint8_t side = 0;
					side < trail.side_count;
					++side)
				{
					MissileTrailRibbon& ribbon = trail.side_ribbons[side];
					const std::uint16_t side_capacity =
						trail.side_fade_ticks;
					if (ribbon.ring_count == side_capacity)
					{
						std::move(
							ribbon.rings.begin() + 1,
							ribbon.rings.begin() + side_capacity,
							ribbon.rings.begin());
						--ribbon.ring_count;
					}
					ribbon.rings[ribbon.ring_count++] = side_rings[side];
				}
			}
		}
	}
	const MissileTrailRing ring = missile_trail_ring(
		missile, trail.flags, simulation_tick, &random_seed);
	MissileTrailRing& current = trail.rings[trail.ring_count - 1u];
	const float width =
		missile.bounds_max.x - missile.bounds_min.x;
	const float distance_from_fixed_ring = glm::distance(
		trail.rings[trail.ring_count - 2u].points[0],
		ring.points[0]) / width;
	current = ring;
	if ((trail.flags & 2u) != 0)
	{
		const glm::mat3 visual_orientation =
			missile_visual_orientation(missile);
		trail.burst_position = missile.scene_position + visual_orientation
			* glm::vec3{0.0f, 0.0f, missile.bounds_min.z};
		const float pitch =
			(missile_random_unit(random_seed) - 0.5f) * 0.1f;
		const float yaw =
			(missile_random_unit(random_seed) - 0.5f) * 0.1f;
		trail.burst_orientation = visual_orientation
			* math::rotation_from_euler({pitch, yaw, 0.0f});
		trail.burst_v_offset += static_cast<float>(
			simulation_tick - trail.burst_last_tick) * 0.091f;
		trail.burst_last_tick = simulation_tick;
	}
	if ((trail.flags & 8u) != 0)
	{
		trail.glow_position = missile.scene_position
			+ missile_visual_orientation(missile)
			* glm::vec3{0.0f, 0.0f, missile.bounds_min.z - 50.0f};
		trail.glow_size = std::abs(missile.bounds_max.x) * 2.0f
			* (1.0f + missile_random_unit(random_seed)
				* (1.0f / 6.0f));
	}
	if (distance_from_fixed_ring <= 3.0f)
	{
		return;
	}
	if (trail.ring_count == capacity)
	{
		std::move(
			trail.rings.begin() + 1,
			trail.rings.begin() + capacity,
			trail.rings.begin());
		--trail.ring_count;
	}
	trail.rings[trail.ring_count++] = ring;
}

void start_missile_trail(
	MissileRuntime& runtime,
	Missile& missile,
	std::uint32_t simulation_tick)
{
	if (missile.type < 0
		|| static_cast<std::size_t>(missile.type)
			>= std::size(kMissileTrailFlags)
		|| (kMissileTrailFlags[missile.type] & 1u) == 0)
	{
		return;
	}
	for (std::size_t slot = 0; slot < std::size(runtime.trails); ++slot)
	{
		MissileTrail& trail = runtime.trails[slot];
		if (trail.active)
		{
			continue;
		}
		trail = {};
		trail.active = true;
		trail.attached = true;
		trail.type = static_cast<std::uint8_t>(missile.type);
		trail.flags = kMissileTrailFlags[missile.type];
		trail.fade_ticks = kMissileTrailPointCounts[missile.type];
		trail.side_fade_ticks = kMissileSideFadeTicks[missile.type];
		trail.side_count = (trail.flags & 4u) != 0
			? std::min<std::uint8_t>(kMissileSideCounts[missile.type], 4)
			: 0;
		trail.color = {
			kMissileTrailColors[missile.type][0],
			kMissileTrailColors[missile.type][1],
			kMissileTrailColors[missile.type][2],
		};
		// Missile_trail_update_main_ribbon (LANCER.EXE 0x00495280)
		// replaces type nine's compiled white with this tint when the owning
		// torpedo has allegiance class one.
		if (missile.type == 9 && missile.allegiance_class == 1)
		{
			trail.color = kHostileTorpedoTrailColor;
		}
		trail.last_update_tick = simulation_tick;
		trail.burst_last_tick = simulation_tick;
		missile.trail_slot = static_cast<std::int16_t>(slot);
		MissileTrailRing initial = missile_trail_ring(
			missile, trail.flags, simulation_tick, nullptr);
		initial.intensity = 1.0f;
		trail.rings[0] = initial;
		trail.rings[1] = initial;
		trail.ring_count = 2;
		trail.side_color = {
			kMissileSideColors[missile.type][0],
			kMissileSideColors[missile.type][1],
			kMissileSideColors[missile.type][2],
		};
		for (std::uint8_t side = 0; side < trail.side_count; ++side)
		{
			MissileTrailRing side_initial = initial;
			side_initial.intensity = 0.0f;
			trail.side_ribbons[side].rings[0] = side_initial;
			trail.side_ribbons[side].rings[1] = side_initial;
			trail.side_ribbons[side].ring_count = 2;
		}
		if ((trail.flags & 8u) != 0)
		{
			trail.glow_position = missile.scene_position
				+ missile_visual_orientation(missile)
				* glm::vec3{0.0f, 0.0f, missile.bounds_min.z - 50.0f};
			trail.glow_size = std::abs(missile.bounds_max.x) * 2.0f;
		}
		return;
	}
}

void detach_missile_trail(
	MissileRuntime& runtime,
	Missile& missile,
	std::uint32_t simulation_tick)
{
	if (missile.trail_slot < 0
		|| static_cast<std::size_t>(missile.trail_slot)
			>= std::size(runtime.trails))
	{
		return;
	}
	MissileTrail& trail = runtime.trails[missile.trail_slot];
	if (trail.active && trail.attached)
	{
		trail.attached = false;
	}
	(void)simulation_tick;
}

Missile* allocate_missile(MissileRuntime& runtime)
{
	// Missile_launch_from_ship_mount (LANCER.EXE 0x00496290) requires
	// both the object field and launch-time sentinel to be free, scanning
	// the fixed 200-record pool from its beginning.
	for (Missile& missile : runtime.missiles)
	{
		if (!missile.active && missile.launch_tick == -1)
		{
			return &missile;
		}
	}
	if (!runtime.pool_exhaustion_reported)
	{
		diagnostics::mission_log(
			"missiles pool exhausted capacity=%u",
			static_cast<unsigned>(kMaxMissiles));
		runtime.pool_exhaustion_reported = true;
	}
	return nullptr;
}

void destroy_missile(
	MissileRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	Missile& missile,
	std::uint32_t simulation_tick)
{
	const bool local = missile.shooter.index != UINT16_MAX;
	// Missile_destroy delegates to Explosion_missile_destroy
	// (LANCER.EXE 0x0046e370). Its style-two burst temporarily narrows the
	// authored random lifetime from 500 to 200 ticks, then restores it.
	ParticleStyle& debris_style = world.particles.styles[
		static_cast<std::size_t>(ParticleEmitterStyle::white_debris)];
	const std::int32_t saved_random_lifetime =
		debris_style.lifetime_random_ticks;
	debris_style.lifetime_random_ticks = 200;
	particle_emitter_burst_world(
		world,
		missile.position,
		missile.orientation,
		glm::vec3{0.0f},
		glm::vec3{1.0f},
		0.0f,
		4.0f,
		50,
		ParticleEmitterStyle::white_debris,
		simulation_tick,
		mission.particle_camera_position,
		mission.particle_camera_forward,
		missile.velocity * 0.25f);
	debris_style.lifetime_random_ticks = saved_random_lifetime;
	(void)explosion_billboard_create(
		world.death_effects,
		world,
		missile.position,
		missile.velocity * 0.25f,
		ExplosionBillboardType::sheet,
		missile.radius * 3.0f,
		100,
		true,
		0,
		false,
		false,
		simulation_tick);
	world_queue_sound_explicit(
		world,
		missile.position,
		missile.orientation[2],
		missile.velocity,
		11,
		3);
	if (missile.object_kind == 2 || missile.object_kind == 7)
	{
		// missile_destroy (0x004958c3) forwards the authored value 500
		// directly to shockwave_create; that owner uses the same retail
		// gameplay-tick domain as every other shockwave deadline.
		shockwave_create(
			world,
			missile.position,
			missile.orientation,
			glm::vec3{0.0f},
			missile.object_kind == 2 ? 5u : 6u,
			50000.0f,
			500,
			static_cast<std::int8_t>(missile.allegiance_class),
			-1,
			simulation_tick);
	}
	detach_missile_trail(runtime, missile, simulation_tick);
	missile = {};
	missile.launch_tick = -1;
	missile.behavior_tick = -1;
	missile.type = -1;
	missile.behavior = -1;
	missile.guidance_state = -1;
	missile.trail_slot = -1;
	if (runtime.live_count != 0)
	{
		--runtime.live_count;
	}
	if (local)
	{
		runtime.pool_exhaustion_reported = false;
	}
}

void set_behavior(
	Missile& missile,
	std::int16_t behavior,
	std::uint32_t simulation_tick);

bool target_reference_valid(
	const World& world,
	ObjectHandle target_handle,
	std::int16_t target_component,
	std::uint32_t allowance = 0);

glm::vec3 target_reference_point(
	const WorldObject& target,
	std::int16_t target_component)
{
	if (target_component >= 0
		&& target_component < target.component_count)
	{
		const std::int16_t model_reference =
			target.components[target_component].model_reference;
		if (model_reference >= 0
			&& static_cast<std::size_t>(model_reference)
				< target.model_references.size())
		{
			return target.scene_position + target.scene_orientation
				* glm::vec3(
					target.model_references[model_reference].scene_transform[3]);
		}
	}
	return target.scene_position;
}

bool home_on_target(
	MissileRuntime& runtime,
	ChaffRuntime& chaff,
	Missile& missile,
	World& world,
	mission::Runtime& mission,
	const assets::MissileStats& stats,
	std::uint32_t simulation_tick)
{
	const WorldObject* target = world_resolve(world, missile.target);
	if (target == nullptr
		|| !target_reference_valid(
			world,
			missile.target,
			missile.target_component,
			missile.object_kind == 5 ? 0x00000100u : 0u))
	{
		// Missile_home_on_target (0x00496c90) validates the authored
		// TargetRef before it considers a selected chaff record. Ordnance
		// object kind five supplies the retail cloak-bit allowance.
		if (missile.type == 6)
		{
			missile.throttle = 1.0f;
			missile.pitch_control = 0.0f;
			missile.yaw_control = 0.0f;
			return true;
		}
		destroy_missile(runtime, world, mission, missile, simulation_tick);
		return false;
	}

	glm::vec3 target_point{0.0f};
	bool using_decoy = false;
	if (missile.guidance_state >= 0)
	{
		const ChaffRecord* decoy =
			chaff_resolve(chaff, missile.guidance_state);
		if (decoy != nullptr)
		{
			using_decoy = true;
			target_point = decoy->position;
			const glm::vec3 decoy_delta =
				target_point - missile.scene_position;
			if (glm::dot(decoy_delta, decoy_delta) < 1000000.0f)
			{
				const std::int16_t slot = missile.guidance_state;
				chaff_destroy(
					chaff,
					runtime,
					world,
					slot,
					simulation_tick,
					"missile-impact");
				destroy_missile(
					runtime, world, mission, missile, simulation_tick);
				diagnostics::mission_log(
					"missile chaff impact slot=%d tick=%u",
					slot,
					simulation_tick);
				return false;
			}
		}
		else
		{
			missile.guidance_state = -1;
		}
	}

	if (!using_decoy)
	{
		target_point = target_reference_point(
			*target, missile.target_component);
		const float missile_speed = stats.speed;
		if (missile_speed > 0.0f)
		{
			const float travel_time =
				glm::distance(target_point, missile.scene_position) / missile_speed;
			// The retail predictor consumes GameObject +0x5d8 and its root
			// scene forward basis, not the component basis or accumulated
			// +0x590 velocity vector.
			target_point += target->scene_orientation[2]
				* target->speed * travel_time;
		}
	}

	const glm::vec3 aim = target_point - missile.scene_position;
	const float distance_squared = glm::dot(aim, aim);
	if (distance_squared <= 0.0f
		|| distance_squared > kMaximumGuidanceDistanceSquared)
	{
		if (missile.type == 6)
		{
			missile.throttle = 1.0f;
			missile.pitch_control = 0.0f;
			missile.yaw_control = 0.0f;
			return true;
		}
		destroy_missile(runtime, world, mission, missile, simulation_tick);
		return false;
	}

	const glm::vec3 direction = aim / std::sqrt(distance_squared);
	const glm::vec3 local =
		glm::transpose(missile.scene_orientation) * direction;
	missile.throttle = std::max(0.0f, local.z);
	if (local.z >= 0.0f)
	{
		missile.pitch_control = std::clamp(
			(-std::atan2(local.y, local.z)
				- missile.smoothed_pitch * 2.0f)
				* kGuidanceGain,
			-1.0f,
			1.0f);
		missile.yaw_control = std::clamp(
			(std::atan2(local.x, local.z)
				- missile.smoothed_yaw * 2.0f)
				* kGuidanceGain,
			-1.0f,
			1.0f);
	}
	else
	{
		missile.pitch_control = 0.0f;
		missile.yaw_control = local.x >= 0.0f ? 1.0f : -1.0f;
	}
	return true;
}

bool target_reference_valid(
	const World& world,
	ObjectHandle target_handle,
	std::int16_t target_component,
	std::uint32_t allowance)
{
	const WorldObject* target = world_resolve(world, target_handle);
	if (target == nullptr
		// TargetRef_is_valid (LANCER.EXE 0x00401870) requires live-object
		// flag 0x0200 and rejects this exact object-state mask.
		|| (target->runtime_flags & 0x00000200u) == 0
		|| (target->runtime_flags & ~allowance & 0x10000d40u) != 0)
	{
		return false;
	}
	if (target_component < 0)
	{
		return true;
	}
	if (target_component >= target->component_count)
	{
		return false;
	}
	const ObjectComponent& component = target->components[target_component];
	return component.model_reference >= 0
		&& static_cast<std::size_t>(component.model_reference)
			< target->model_references.size()
		&& (component.runtime_flags & 0x0030u) == 0;
}

struct ReacquisitionCandidate
{
	ObjectHandle target;
	std::int16_t component{-1};
	float distance{std::numeric_limits<float>::max()};
};

void consider_reacquisition_candidate(
	const Missile& missile,
	ObjectHandle target,
	std::int16_t component,
	const glm::vec3& target_point,
	ReacquisitionCandidate& forward,
	ReacquisitionCandidate& any)
{
	const glm::vec3 delta = target_point - missile.scene_position;
	const float distance = glm::length(delta);
	if (distance < forward.distance
		&& distance > 0.0f
		&& glm::dot(delta / distance, missile.scene_orientation[2])
			> kReacquisitionForwardDot)
	{
		forward = {target, component, distance};
	}
	if (distance < any.distance)
	{
		any = {target, component, distance};
	}
}

bool apply_reacquisition_candidate(
	Missile& missile,
	const ReacquisitionCandidate& candidate)
{
	if (candidate.target.index == UINT16_MAX)
	{
		return false;
	}
	missile.target = candidate.target;
	missile.target_component = candidate.component;
	return true;
}

bool reacquire_target(Missile& missile, const World& world)
{
	if (target_reference_valid(
			world,
			missile.target,
			missile.target_component))
	{
		return true;
	}

	ReacquisitionCandidate forward;
	ReacquisitionCandidate any;
	for (std::uint16_t index = 0; index < kMaxGameObjects; ++index)
	{
		const WorldObject& target = world.objects[index];
		if (!target.active
			|| target.allegiance_class == missile.allegiance_class
			|| (target.runtime_flags & kObjectFlagCompound) != 0)
		{
			continue;
		}
		const ObjectHandle handle{index, target.generation};
		if (!target_reference_valid(world, handle, -1))
		{
			continue;
		}
		consider_reacquisition_candidate(
			missile,
			handle,
			-1,
			target_reference_point(target, -1),
			forward,
			any);
	}
	// Missile_behavior_reacquire_target (0x00497f80..0x004980f4)
	// gives the first, simple-object scan absolute priority: its nearest
	// forward candidate wins, then its nearest candidate of any bearing.
	if (apply_reacquisition_candidate(missile, forward)
		|| apply_reacquisition_candidate(missile, any))
	{
		return true;
	}

	for (std::uint16_t index = 0; index < kMaxGameObjects; ++index)
	{
		const WorldObject& target = world.objects[index];
		if (!target.active
			|| target.allegiance_class == missile.allegiance_class
			|| (target.runtime_flags & kObjectFlagCompound) == 0)
		{
			continue;
		}
		const ObjectHandle handle{index, target.generation};
		if (!target_reference_valid(world, handle, -1))
		{
			continue;
		}

		bool found_component = false;
		for (std::uint8_t component = 0;
			component < target.component_count;
			++component)
		{
			if (!target_reference_valid(world, handle, component))
			{
				continue;
			}
			found_component = true;
			consider_reacquisition_candidate(
				missile,
				handle,
				static_cast<std::int16_t>(component),
				target_reference_point(
					target, static_cast<std::int16_t>(component)),
				forward,
				any);
		}
		// Retail considers the whole-object center only when none of the
		// compound object's fixed model references validate.
		if (!found_component)
		{
			consider_reacquisition_candidate(
				missile,
				handle,
				-1,
				target_reference_point(target, -1),
				forward,
				any);
		}
	}
	return apply_reacquisition_candidate(missile, forward)
		|| apply_reacquisition_candidate(missile, any);
}

void update_behavior(
	MissileRuntime& runtime,
	ChaffRuntime& chaff,
	Missile& missile,
	World& world,
	mission::Runtime& mission,
	const assets::MissileStatsTable& stats,
	std::uint32_t simulation_tick,
	std::uint32_t& random_seed)
{
	if (!missile.active || missile.type < 0
		|| static_cast<std::size_t>(missile.type)
			>= assets::kMissileStatsCount)
	{
		return;
	}
	const assets::MissileStats& definition = stats.records[missile.type];
	const std::uint32_t elapsed =
		simulation_tick - static_cast<std::uint32_t>(missile.behavior_tick);
	switch (missile.behavior)
	{
	case 0:
		if (elapsed < 50)
		{
			missile.throttle = 2.0f;
			missile.pitch_control = 0.0f;
			missile.yaw_control = 0.0f;
			return;
		}
		set_behavior(
			missile,
			definition.behavior,
			simulation_tick);
		// Missile_set_behavior (0x00496aa0) invokes the newly selected
		// updater immediately after its initializer. This also makes
		// type 9's class-0 redispatch restart at elapsed tick zero.
		update_behavior(
			runtime,
			chaff,
			missile,
			world,
			mission,
			stats,
			simulation_tick,
			random_seed);
		return;
	case 1:
		missile.pitch_control = 0.0f;
		missile.yaw_control = 0.0f;
		if (elapsed < 25)
		{
			missile.throttle = 0.0f;
			missile.velocity += missile.orientation[2]
				* definition.speed * 0.004999999888241291f;
			return;
		}
		if (elapsed < 50)
		{
			missile.throttle = 0.0f;
			missile.velocity -= missile.orientation[2]
				* definition.speed * 0.004999999888241291f;
			return;
		}
		if (elapsed < 100)
		{
			missile.throttle = 1.0f;
			return;
		}
		set_behavior(
			missile,
			definition.behavior,
			simulation_tick);
		update_behavior(
			runtime,
			chaff,
			missile,
			world,
			mission,
			stats,
			simulation_tick,
			random_seed);
		return;
	case 2:
		// The offline local-player branch at 0x00497f10 uses the straight
		// control helper; remote/non-player missiles enter ordinary homing.
		if (!world.network_active
			&& missile.shooter.index < world.player_prefix_count)
		{
			missile.throttle = 1.0f;
			missile.pitch_control = 0.0f;
			missile.yaw_control = 0.0f;
			return;
		}
		home_on_target(
			runtime,
			chaff,
			missile,
			world,
			mission,
			definition,
			simulation_tick);
		return;
	case 4:
	case 9:
	{
		const WorldObject* target = world_resolve(world, missile.target);
		const glm::vec3 target_delta = target == nullptr
			? glm::vec3{0.0f}
			: target->scene_position - missile.scene_position;
		if (target != nullptr
			&& glm::dot(target_delta, target_delta)
				< kProximityFuseDistanceSquared)
		{
			destroy_missile(
				runtime, world, mission, missile, simulation_tick);
			return;
		}
		home_on_target(
			runtime,
			chaff,
			missile,
			world,
			mission,
			definition,
			simulation_tick);
		return;
	}
	case 3:
	case 5:
	case 6:
	case 7:
	case 10:
		home_on_target(
			runtime,
			chaff,
			missile,
			world,
			mission,
			definition,
			simulation_tick);
		return;
	case 8:
		if (reacquire_target(missile, world))
		{
			home_on_target(
				runtime,
				chaff,
				missile,
				world,
				mission,
				definition,
				simulation_tick);
		}
		else
		{
			// The no-candidate branch at 0x00498387 coasts; unlike ordinary
			// guidance failure, it does not consume the missile.
			missile.throttle = 1.0f;
			missile.pitch_control = 0.0f;
			missile.yaw_control = 0.0f;
		}
		return;
	case 11:
		missile.throttle = 0.0f;
		missile.pitch_control = 0.0f;
		missile.yaw_control = 0.0f;
		if (elapsed >= 100)
		{
			destroy_missile(
				runtime, world, mission, missile, simulation_tick);
		}
		return;
	default:
		destroy_missile(runtime, world, mission, missile, simulation_tick);
		return;
	}
}

void set_behavior(
	Missile& missile,
	std::int16_t behavior,
	std::uint32_t simulation_tick)
{
	missile.behavior = behavior;
	missile.behavior_tick = static_cast<std::int32_t>(simulation_tick);
	if (behavior == 11)
	{
		// Missile_behavior_ballistic_init (0x004983c0) adds exactly 50
		// units along the missile object's forward basis.
		missile.velocity += missile.orientation[2] * 50.0f;
	}
}

void integrate_missile(
	Missile& missile,
	const assets::MissileStats& stats)
{
	missile.previous_position = missile.position;
	missile.previous_orientation = missile.orientation;
	missile.smoothed_pitch =
		kSteeringRetention * missile.smoothed_pitch
		+ (1.0f - kSteeringRetention)
			* missile.pitch_control * stats.steering_factor;
	missile.smoothed_yaw =
		kSteeringRetention * missile.smoothed_yaw
		+ (1.0f - kSteeringRetention)
			* missile.yaw_control * stats.steering_factor;
	missile.orientation = missile.previous_orientation
		* math::rotation_from_euler({
			missile.smoothed_pitch,
			missile.smoothed_yaw,
			0.0f,
		});

	// Missile_integrate_objects (0x00495720) omits the normal velocity
	// retention for staging behaviors 0, 1, and 11, then adds the same
	// forward drive term to every behavior.
	if (missile.behavior != 0
		&& missile.behavior != 1
		&& missile.behavior != 11)
	{
		missile.velocity *= kMovementRetention;
	}
	missile.velocity += missile.orientation[2]
		* ((1.0f - kMovementRetention) * missile.throttle * stats.speed);
	missile.position += missile.velocity;
}
}

void missiles_reset(MissileRuntime& runtime)
{
	runtime.~MissileRuntime();
	::new (static_cast<void*>(&runtime)) MissileRuntime{};
	for (Missile& missile : runtime.missiles)
	{
		missile.launch_tick = -1;
		missile.behavior_tick = -1;
		missile.type = -1;
		missile.behavior = -1;
		missile.guidance_state = -1;
		missile.trail_slot = -1;
	}
}

bool missiles_launch_from_ship_mount(
	MissileRuntime& runtime,
	World& world,
	const assets::MissileStatsTable& stats,
	ObjectHandle shooter_handle,
	std::uint8_t attachment_index,
	ObjectHandle target,
	std::int16_t target_component,
	std::uint32_t simulation_tick)
{
	WorldObject* shooter = world_resolve(world, shooter_handle);
	if (!stats.ready || shooter == nullptr
		|| (shooter->runtime_flags & 0x00010000u) != 0
		|| attachment_index >= shooter->attachment_count)
	{
		return false;
	}
	AttachmentSlot& attachment = shooter->attachments[attachment_index];
	if (attachment.definition_index < 0
		|| static_cast<std::size_t>(attachment.definition_index)
			>= assets::kMissileStatsCount
		|| attachment.remaining_count < 0)
	{
		return false;
	}
	if (attachment.model_reference >= 0
		&& attachment.model_reference
			< static_cast<std::int16_t>(
				shooter->model_references.size()))
	{
		const glm::mat4 transform =
			shooter->model_references[
				attachment.model_reference].local_transform
			* attachment.hardpoint_from_model;
		attachment.local_position = glm::vec3(transform[3]);
		attachment.local_orientation = glm::mat3(transform);
	}
	Missile* missile = allocate_missile(runtime);
	if (missile == nullptr)
	{
		return false;
	}
	const std::int16_t type = attachment.definition_index;
	*missile = {};
	missile->active = true;
	missile->sound_generation = ++runtime.next_sound_generation;
	missile->launch_tick = static_cast<std::int32_t>(simulation_tick);
	missile->behavior_tick = static_cast<std::int32_t>(simulation_tick);
	missile->type = type;
	// The attachment object's first word is the selected 12-byte ordnance
	// slot kind, not the source hardpoint model type.
	missile->object_kind = attachment.kind;
	const bool has_projectile_resource =
		world.attachment_model_loaded[type][1];
	const bool instantiate_projectile = !world.network_active
		&& has_projectile_resource
		&& attachment.remaining_count > 0;
	const std::size_t model_variant =
		instantiate_projectile || attachment.alternate_model ? 1u : 0u;
	missile->model_variant = static_cast<std::uint8_t>(model_variant);
	missile->radius = world.attachment_model_radius[type][model_variant];
	missile->bounds_min =
		world.attachment_model_bounds_min[type][model_variant];
	missile->bounds_max =
		world.attachment_model_bounds_max[type][model_variant];
	missile->guidance_state = -1;
	missile->shooter = shooter_handle;
	missile->target = target;
	missile->target_component = target_component;
	missile->allegiance_class = shooter->allegiance_class;
	missile->orientation =
		shooter->orientation * attachment.local_orientation;
	missile->previous_orientation = missile->orientation;
	missile->scene_orientation = missile->orientation;
	missile->position =
		shooter->position
		+ shooter->orientation * attachment.local_position;
	missile->previous_position = missile->position;
	missile->scene_position = missile->position;
	missile->velocity = shooter->linear_velocity;
	--attachment.remaining_count;
	if (has_projectile_resource && attachment.remaining_count < 0)
	{
		set_behavior(*missile, 11, simulation_tick);
		missile->throttle = 0.0f;
	}
	else if (has_projectile_resource)
	{
		set_behavior(*missile, 0, simulation_tick);
		missile->throttle = 2.0f;
	}
	else if (type == 10)
	{
		set_behavior(*missile, 11, simulation_tick);
		missile->throttle = 0.0f;
	}
	else
	{
		set_behavior(*missile, 1, simulation_tick);
		missile->throttle = 0.0f;
		missile->velocity += missile->orientation[2]
			* stats.records[type].speed * 0.004999999888241291f;
	}
	missile->pitch_control = 0.0f;
	missile->yaw_control = 0.0f;
	if (missile->behavior != 11)
	{
		start_missile_trail(runtime, *missile, simulation_tick);
	}
	if (!instantiate_projectile)
	{
		// The primary hardpoint SRO becomes the missile object. It must no
		// longer be submitted as an attached child of the firing ship.
		attachment.live_model = false;
	}
	++runtime.live_count;
	runtime.pool_exhaustion_reported = false;
	const std::uint16_t missile_index = static_cast<std::uint16_t>(
		missile - std::begin(runtime.missiles));
	world_queue_sound_missile(
		world,
		missile_index,
		missile->sound_generation,
		shooter_handle,
		missile->position,
		missile->orientation[2],
		missile->velocity,
		kMissileSoundDefinitions[type],
		static_cast<std::uint8_t>(shooter->player ? 4 : 0));
	diagnostics::mission_log(
		"missile launch shooter=%u mission=%u type=%d mount=%u "
		"target=%d component=%d mount_count=%d live=%u",
		static_cast<unsigned>(shooter_handle.index),
		static_cast<unsigned>(shooter->mission_index),
		type,
		static_cast<unsigned>(attachment_index),
		target.index == UINT16_MAX ? -1 : static_cast<int>(target.index),
		target_component,
		attachment.remaining_count,
		runtime.live_count);
	if (!world.network_active && has_projectile_resource
		&& attachment.remaining_count == 0)
	{
		// The offline retail tail call launches the now-empty retained pod as
		// behavior eleven. The recursive call observes count zero, detaches
		// the primary mount SRO, and decrements the authored counter to -1.
		(void)missiles_launch_from_ship_mount(
			runtime,
			world,
			stats,
			shooter_handle,
			attachment_index,
			{},
			-1,
			simulation_tick);
	}
	return true;
}

bool missiles_launch_type0_from_transform(
	MissileRuntime& runtime,
	World& world,
	const assets::MissileStatsTable& stats,
	ObjectHandle shooter_handle,
	const glm::vec3& position,
	const glm::mat3& orientation,
	ObjectHandle target,
	std::int16_t target_component,
	std::uint32_t simulation_tick)
{
	WorldObject* shooter = world_resolve(world, shooter_handle);
	if (!stats.ready || shooter == nullptr
		|| (shooter->runtime_flags & 0x00010000u) != 0)
	{
		return false;
	}
	Missile* missile = allocate_missile(runtime);
	if (missile == nullptr)
	{
		return false;
	}
	*missile = {};
	missile->active = true;
	missile->sound_generation = ++runtime.next_sound_generation;
	missile->launch_tick = static_cast<std::int32_t>(simulation_tick);
	missile->behavior_tick = static_cast<std::int32_t>(simulation_tick);
	missile->type = 0;
	// Missile_launch_type0_from_transform (0x004967f0) unconditionally
	// clones definition zero's separately loaded projectile resource.
	constexpr std::size_t model_variant = 1u;
	missile->model_variant = static_cast<std::uint8_t>(model_variant);
	missile->radius = world.attachment_model_radius[0][model_variant];
	missile->bounds_min = world.attachment_model_bounds_min[0][model_variant];
	missile->bounds_max = world.attachment_model_bounds_max[0][model_variant];
	missile->guidance_state = -1;
	missile->shooter = shooter_handle;
	missile->target = target;
	missile->target_component = target_component;
	missile->allegiance_class = shooter->allegiance_class;
	missile->orientation = orientation;
	missile->previous_orientation = orientation;
	missile->scene_orientation = orientation;
	missile->position = position;
	missile->previous_position = position;
	missile->scene_position = position;
	missile->velocity = shooter->linear_velocity;
	set_behavior(*missile, 0, simulation_tick);
	missile->throttle = 2.0f;
	start_missile_trail(runtime, *missile, simulation_tick);
	++runtime.live_count;
	runtime.pool_exhaustion_reported = false;
	world_queue_sound_missile(
		world,
		static_cast<std::uint16_t>(
			missile - std::begin(runtime.missiles)),
		missile->sound_generation,
		shooter_handle,
		missile->position,
		missile->orientation[2],
		missile->velocity,
		15,
		static_cast<std::uint8_t>(shooter->player ? 4 : 0));
	return true;
}

void missiles_integrate(
	MissileRuntime& runtime,
	const assets::MissileStatsTable& stats)
{
	// Missile_integrate_objects (0x00495720) is the 25 Hz object-simulator
	// pass. Guidance, expiry, collision and trail ownership remain in the
	// separate once-per-frame Missile_system_update owner.
	for (Missile& missile : runtime.missiles)
	{
		if (!missile.active || missile.type < 0
			|| static_cast<std::size_t>(missile.type)
				>= assets::kMissileStatsCount)
		{
			continue;
		}
		integrate_missile(missile, stats.records[missile.type]);
	}
}

void missiles_publish_scene_poses(
	MissileRuntime& runtime,
	float service_fraction)
{
	const float fraction = std::clamp(service_fraction, 0.0f, 1.0f);
	for (Missile& missile : runtime.missiles)
	{
		if (!missile.active)
		{
			continue;
		}
		// Object_model_update_scene publishes the missile GameObject's live
		// root between +0x3c (previous 25 Hz pose) and +0x84 (current pose).
		// Missile_process_collision then sweeps from this retained node to
		// the current physics endpoint.
		missile.scene_position =
			missile.previous_position
				+ (missile.position - missile.previous_position) * fraction;
		missile.scene_orientation = math::interpolate_scene_orientation(
			missile.previous_orientation,
			missile.orientation,
			fraction);
	}
}

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
	bool local_player_lock_held)
{
	for (WorldObject& object : world.objects)
	{
		if (object.active)
		{
			object.incoming_missile = false;
		}
	}
	for (std::uint16_t index = 0;
		index < kMaxMissiles;
		++index)
	{
		Missile& missile = runtime.missiles[index];
		if (!missile.active || missile.type < 0
			|| static_cast<std::size_t>(missile.type)
				>= assets::kMissileStatsCount)
		{
			continue;
		}
		if (world.network_active)
		{
			if (missile.target.index != UINT16_MAX
				&& missile.shooter.index == world.player.index
				&& !local_player_lock_held)
			{
				// Missile_system_update clears a network local-player target
				// whenever the shared HUD lock state is not retail state three.
				missile.target = {};
				missile.target_component = -1;
			}
			const WorldObject* shooter =
				world_resolve(world, missile.shooter);
			if (shooter == nullptr || shooter->type == 1001
				|| (shooter->ai.command_count != 0
					&& shooter->ai.commands[0].id == 11))
			{
				// The multiplayer prepass at 0x0049613c consumes shots from
				// departed type-1001 owners and owners in AI command 11.
				destroy_missile(
					runtime, world, mission, missile, simulation_tick);
				continue;
			}
		}
		update_behavior(
			runtime,
			chaff,
			missile,
			world,
			mission,
			stats,
			simulation_tick,
			random_seed);
		if (!missile.active)
		{
			continue;
		}
		const assets::MissileStats& definition =
			stats.records[missile.type];
		if (simulation_tick
				- static_cast<std::uint32_t>(missile.launch_tick)
			>= static_cast<std::uint32_t>(
				std::max(0, definition.lifetime_ticks)))
		{
			destroy_missile(
				runtime, world, mission, missile, simulation_tick);
			continue;
		}
		if (weapons_process_missile_collision(
				weapons,
				world,
				mission,
				missile,
				definition,
				ship_stats,
				simulation_tick))
		{
			destroy_missile(
				runtime, world, mission, missile, simulation_tick);
			continue;
		}
		if (missile.guidance_state < 0)
		{
			WorldObject* target = world_resolve(world, missile.target);
			if (target != nullptr
				// Missile_system_update (0x004961ec..0x00496218)
				// suppresses this transient flag only outside deathmatch,
				// for a player-prefix shot whose detached ordnance object
				// kind is zero. AI slots publish it regardless of kind.
				&& (mission::network_is_deathmatch_mission(
						mission.mission_number)
					|| missile.shooter.index
						>= mission.player_prefix_count
					|| missile.object_kind != 0))
			{
				target->incoming_missile = true;
			}
		}
	}
	// Retail updates the independently owned trail list after missile
	// behavior and collision. A missile consumed above has already detached
	// its trail and therefore cannot append one last phantom ring.
	fade_missile_trails(runtime, simulation_tick);
	for (Missile& missile : runtime.missiles)
	{
		if (missile.active)
		{
			update_missile_trail(
				runtime, missile, simulation_tick, random_seed);
		}
	}
}

bool missiles_local_shot_has_target(
	const MissileRuntime& runtime,
	ObjectHandle shooter)
{
	// Missile_local_shot_has_target (0x004af190) walks the live list and
	// checks only shooter identity plus a nonnegative target object word.
	for (const Missile& missile : runtime.missiles)
	{
		if (missile.active
			&& missile.shooter.index == shooter.index
			&& missile.target.index != UINT16_MAX)
		{
			return true;
		}
	}
	return false;
}
}
