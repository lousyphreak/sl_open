#include "game/chaff.hpp"

#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "game/missiles.hpp"

#include <algorithm>
#include <cmath>

namespace sl_open::game
{
namespace
{
constexpr std::uint32_t kLifetimeTicks = 1000;
constexpr float kEmissionProbability = 0.5f;
constexpr float kParticleMidpointSize = 75.0f;
constexpr float kParticleRenderScale = 150.0f;

std::uint16_t random15(std::uint32_t& seed)
{
	// Chaff_deploy, both particle emitters, and the explosion billboard all
	// consume the process-wide MSVC CRT stream used by the recovered owner.
	seed = seed * 0x343fdu + 0x269ec3u;
	return static_cast<std::uint16_t>((seed >> 16) & 0x7fffu);
}

float random_unit(std::uint32_t& seed)
{
	return static_cast<float>(random15(seed)) * (1.0f / 32767.0f);
}

bool same_handle(ObjectHandle left, ObjectHandle right)
{
	return left.index == right.index && left.generation == right.generation;
}

void spawn_particle(
	ChaffRuntime& runtime,
	const ChaffRecord& record,
	float local_z,
	float direction_z,
	std::uint32_t simulation_tick,
	std::uint32_t elapsed_ticks,
	std::uint32_t& random_seed)
{
	std::uint32_t slot_index = kMaxChaffParticles;
	for (std::uint32_t index = 0; index < kMaxChaffParticles; ++index)
	{
		const ChaffParticle& candidate = runtime.particles[index];
		if (!candidate.active
			|| candidate.birth_tick + candidate.lifetime_ticks
				<= simulation_tick)
		{
			slot_index = index;
			break;
		}
	}
	if (slot_index == kMaxChaffParticles)
	{
		return;
	}

	const std::uint16_t lifetime = static_cast<std::uint16_t>(
		100u + random15(random_seed) % 10u);
	glm::vec3 direction{
		(random_unit(random_seed) - 0.5f) * 0.25f,
		(random_unit(random_seed) - 0.5f) * 0.25f,
		direction_z,
	};
	const float direction_length = glm::length(direction);
	float speed = 0.0f;
	if (direction_length > 0.0f)
	{
		direction /= direction_length;
		speed = 5.0f + random_unit(random_seed);
	}

	ChaffParticle& particle = runtime.particles[slot_index];
	particle.active = true;
	particle.birth_tick = simulation_tick;
	particle.lifetime_ticks = lifetime;
	particle.velocity = record.orientation * (direction * speed);
	particle.position =
		record.position + record.orientation * glm::vec3{0.0f, 0.0f, local_z};
	// Particle_emitter_update pre-advances newly spawned BMO positions by
	// the elapsed tick count before the common particle render pass.
	particle.position += particle.velocity
		* static_cast<float>(elapsed_ticks);
	runtime.particle_high_water = static_cast<std::uint16_t>(
		std::max<std::uint32_t>(
			runtime.particle_high_water,
			slot_index + 1));
}

void update_emitter(
	ChaffRuntime& runtime,
	const ChaffRecord& record,
	float local_z,
	float direction_z,
	std::uint32_t simulation_tick,
	std::uint32_t elapsed_ticks,
	const glm::vec3& camera_position,
	const glm::mat3& camera_orientation,
	std::uint32_t& random_seed)
{
	std::uint32_t count = 0;
	for (std::uint32_t tick = 0; tick < elapsed_ticks; ++tick)
	{
		if (random_unit(random_seed) < kEmissionProbability)
		{
			++count;
		}
	}
	const glm::vec3 emitter_position =
		record.position + record.orientation * glm::vec3{0.0f, 0.0f, local_z};
	const glm::vec3 camera_local =
		glm::transpose(camera_orientation)
		* (emitter_position - camera_position);
	if (camera_local.z < 0.0f)
	{
		count /= 2;
	}
	const float distance = glm::distance(camera_position, emitter_position);
	const float attenuation = distance <= 0.0f
		? 1.0f
		: std::min(
			1.0f,
			kParticleMidpointSize * kParticleRenderScale / distance);
	count = static_cast<std::uint32_t>(
		static_cast<float>(count) * attenuation);
	for (std::uint32_t particle = 0; particle < count; ++particle)
	{
		spawn_particle(
			runtime,
			record,
			local_z,
			direction_z,
			simulation_tick,
			elapsed_ticks,
			random_seed);
	}
}
}

void chaff_reset(ChaffRuntime& runtime)
{
	runtime = {};
}

void chaff_set_model_bounds(
	ChaffRuntime& runtime,
	const glm::vec3& minimum,
	const glm::vec3& maximum)
{
	runtime.model_bounds_min = minimum;
	runtime.model_bounds_max = maximum;
	runtime.model_bounds_ready = true;
}

bool chaff_deploy(
	ChaffRuntime& runtime,
	MissileRuntime& missiles,
	World& world,
	const assets::MissileStatsTable& missile_stats,
	const assets::PilotStatsTable& pilot_stats,
	ObjectHandle source_handle,
	std::uint32_t simulation_tick,
	std::uint32_t& random_seed)
{
	WorldObject* source = world_resolve(world, source_handle);
	if (source == nullptr || source->chaff_count <= 0)
	{
		return false;
	}
	--source->chaff_count;

	std::int16_t free_slot = -1;
	for (std::int16_t index = 0;
		index < static_cast<std::int16_t>(kMaxChaff);
		++index)
	{
		if (!runtime.records[index].active)
		{
			free_slot = index;
			break;
		}
	}
	if (free_slot < 0)
	{
		if (!runtime.pool_exhaustion_reported)
		{
			diagnostics::mission_log(
				"chaff pool exhausted capacity=%u inventory=%d",
				kMaxChaff,
				source->chaff_count);
			runtime.pool_exhaustion_reported = true;
		}
		return true;
	}

	ChaffRecord& record = runtime.records[free_slot];
	record = {};
	record.active = true;
	record.expiry_tick = simulation_tick + kLifetimeTicks;
	record.source_index = source_handle.index;
	record.orientation = source->orientation;
	record.position = source->position
		+ source->orientation
			* glm::vec3{0.0f, 0.0f, source->bounds_min.z};
	record.velocity = source->linear_velocity * 0.25f
		+ (source->orientation[1] - source->orientation[2]) * 5.0f;
	++runtime.live_count;
	runtime.pool_exhaustion_reported = false;

	std::int16_t diverted = -1;
	for (std::int16_t index = 0;
		index < static_cast<std::int16_t>(kMaxMissiles);
		++index)
	{
		Missile& missile = missiles.missiles[index];
		if (!missile.active
			|| missile.launch_tick < 0
			|| missile.guidance_state != -1
			|| !same_handle(missile.target, source_handle)
			|| missile.type < 0
			|| static_cast<std::size_t>(missile.type)
				>= assets::kMissileStatsCount)
		{
			continue;
		}
		std::int32_t threshold =
			missile_stats.records[missile.type].chaff_diversion_percent;
		if (source->player)
		{
			threshold += 30;
		}
		else if (source->pilot < assets::kPilotStatsCount
			&& pilot_stats.records[source->pilot].timing_2_min == 50)
		{
			threshold += 50;
		}
		if (static_cast<std::int32_t>(random15(random_seed) % 100u)
			< threshold)
		{
			missile.guidance_state = free_slot;
			diverted = index;
			break;
		}
	}
	// Routine AI countermeasure cadence is frequent. Keep the mission log
	// event-oriented: local launches and actual missile diversions remain
	// visible without one line per AI inventory decrement.
	if (source->player || diverted >= 0)
	{
		diagnostics::mission_log(
			"chaff deploy source=%u mission=%u slot=%d inventory=%d "
			"diverted_missile=%d expiry=%u",
			static_cast<unsigned>(source_handle.index),
			static_cast<unsigned>(source->mission_index),
			free_slot,
			source->chaff_count,
			diverted,
			record.expiry_tick);
	}
	return true;
}

void chaff_destroy(
	ChaffRuntime& runtime,
	MissileRuntime& missiles,
	World& world,
	std::int16_t slot,
	std::uint32_t simulation_tick,
	const char* reason)
{
	if (slot < 0 || slot >= static_cast<std::int16_t>(kMaxChaff)
		|| !runtime.records[slot].active)
	{
		return;
	}
	for (Missile& missile : missiles.missiles)
	{
		if (missile.guidance_state == slot)
		{
			missile.guidance_state = -1;
		}
	}
	const ChaffRecord record = runtime.records[slot];
	(void)explosion_billboard_create(
		world.death_effects,
		world,
		record.position,
		record.velocity,
		ExplosionBillboardType::sheet,
		200.0f,
		50,
		false,
		0,
		false,
		false,
		simulation_tick);
	runtime.records[slot].active = false;
	if (runtime.live_count != 0)
	{
		--runtime.live_count;
	}
	diagnostics::mission_log(
		"chaff destroy slot=%d reason=%s tick=%u live=%u",
		slot,
		reason == nullptr ? "unknown" : reason,
		simulation_tick,
		runtime.live_count);
}

void chaff_step(
	ChaffRuntime& runtime,
	MissileRuntime& missiles,
	World& world,
	std::uint32_t simulation_tick,
	std::uint32_t elapsed_ticks,
	const glm::vec3& camera_position,
	const glm::mat3& camera_orientation,
	std::uint32_t& random_seed)
{
	const glm::mat3 rotation = math::rotation_from_euler({
		0.0f,
		(static_cast<float>(elapsed_ticks) + 1.0f) * 0.01f,
		0.0f,
	});
	for (std::int16_t slot = 0;
		slot < static_cast<std::int16_t>(kMaxChaff);
		++slot)
	{
		ChaffRecord& record = runtime.records[slot];
		if (!record.active)
		{
			continue;
		}
		if (record.expiry_tick < simulation_tick)
		{
			chaff_destroy(
				runtime,
				missiles,
				world,
				slot,
				simulation_tick,
				"expired");
			continue;
		}
		record.orientation *= rotation;
		record.position += record.velocity
			* static_cast<float>(elapsed_ticks);
		if (runtime.model_bounds_ready)
		{
			update_emitter(
				runtime,
				record,
				runtime.model_bounds_max.z,
				1.0f,
				simulation_tick,
				elapsed_ticks,
				camera_position,
				camera_orientation,
				random_seed);
			update_emitter(
				runtime,
				record,
				runtime.model_bounds_min.z,
				-1.0f,
				simulation_tick,
				elapsed_ticks,
				camera_position,
				camera_orientation,
				random_seed);
		}
	}

	std::uint16_t high_water = 0;
	for (std::uint16_t index = 0;
		index < runtime.particle_high_water;
		++index)
	{
		ChaffParticle& particle = runtime.particles[index];
		if (!particle.active)
		{
			continue;
		}
		if (simulation_tick >= particle.birth_tick + particle.lifetime_ticks)
		{
			particle.active = false;
			continue;
		}
		particle.position += particle.velocity
			* static_cast<float>(elapsed_ticks);
		high_water = static_cast<std::uint16_t>(index + 1);
	}
	runtime.particle_high_water = high_water;
}

const ChaffRecord* chaff_resolve(
	const ChaffRuntime& runtime,
	std::int16_t slot)
{
	if (slot < 0 || slot >= static_cast<std::int16_t>(kMaxChaff)
		|| !runtime.records[slot].active)
	{
		return nullptr;
	}
	return &runtime.records[slot];
}
}
