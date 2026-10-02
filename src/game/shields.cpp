#include "game/shields.hpp"

#include "core/math.hpp"
#include "game/particle_emitters.hpp"
#include "game/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sl_open::game
{
namespace
{
constexpr std::uint32_t kShieldSphereSegments[kShieldLodCount][2] = {
	{16, 14}, {12, 10}, {10, 8},
	{8, 6}, {6, 4}, {4, 4},
};

void build_shield_sphere(
	std::uint32_t longitudes,
	std::uint32_t latitudes,
	ShieldSphereGeometry& geometry)
{
	geometry.point_count = 0;
	geometry.index_count = 0;
	const auto add_point = [&geometry](const glm::vec3& point)
	{
		geometry.points[geometry.point_count++] = point;
	};
	const auto add_index = [&geometry](std::uint32_t index)
	{
		geometry.indices[geometry.index_count++] =
			static_cast<std::uint16_t>(index);
	};
	for (std::uint32_t latitude = 0;
		latitude <= latitudes;
		++latitude)
	{
		const float phi =
			static_cast<float>(latitude)
				* glm::pi<float>()
				/ static_cast<float>(latitudes);
		const float sine_phi = std::sin(phi);
		const float cosine_phi = std::cos(phi);
		if (latitude == 0 || latitude == latitudes)
		{
			add_point({0.0f, 0.0f, cosine_phi});
			continue;
		}
		for (std::uint32_t longitude = 0;
			longitude < longitudes;
			++longitude)
		{
			const float theta =
				static_cast<float>(longitude)
					* glm::two_pi<float>()
					/ static_cast<float>(longitudes);
			add_point({
				std::sin(theta) * sine_phi,
				std::cos(theta) * sine_phi,
				cosine_phi,
			});
		}
	}
	for (std::uint32_t longitude = 0;
		longitude < longitudes;
		++longitude)
	{
		add_index(0);
		add_index(1 + (longitude + 1) % longitudes);
		add_index(1 + longitude);
	}
	for (std::uint32_t latitude = 1;
		latitude < latitudes - 1;
		++latitude)
	{
		const std::uint32_t upper =
			1 + (latitude - 1) * longitudes;
		const std::uint32_t lower = upper + longitudes;
		for (std::uint32_t longitude = 0;
			longitude < longitudes;
			++longitude)
		{
			const std::uint32_t following =
				(longitude + 1) % longitudes;
			add_index(upper + longitude);
			add_index(upper + following);
			add_index(lower + longitude);
		}
		for (std::uint32_t longitude = 0;
			longitude < longitudes;
			++longitude)
		{
			const std::uint32_t following =
				(longitude + 1) % longitudes;
			add_index(upper + following);
			add_index(lower + following);
			add_index(lower + longitude);
		}
	}
	const std::uint32_t south = geometry.point_count - 1;
	const std::uint32_t final_ring = south - longitudes;
	for (std::int32_t longitude =
		static_cast<std::int32_t>(longitudes) - 1;
		longitude >= 0;
		--longitude)
	{
		const std::uint32_t previous =
			(static_cast<std::uint32_t>(longitude)
				+ longitudes - 1)
			% longitudes;
		add_index(final_ring + longitude);
		add_index(final_ring + previous);
		add_index(south);
	}
}

glm::mat3 impact_orientation(const glm::vec3& direction)
{
	const glm::vec3 forward = glm::normalize(direction);
	const glm::vec3 side = glm::normalize(glm::cross(
		forward, glm::vec3{1.0f, 0.0f, 0.0f}));
	const glm::vec3 up = glm::cross(side, forward);
	return glm::mat3{side, up, forward};
}

float random_unit(World& world)
{
	return static_cast<float>(world_rand15(world))
		* (1.0f / 32768.0f);
}

void enqueue_effect(
	World& world,
	const WorldObject*,
	const glm::vec3& position,
	const glm::vec3& direction,
	std::uint8_t effect_type,
	std::uint32_t simulation_tick)
{
	// ShieldFX_create (LANCER.EXE 0x004a0310) allocates an emitter for
	// types 2, 3, and 4, but only type 3 calls Particle_emitter_burst with
	// an exact count of 20. The object-effect update at 0x0049b19c calls
	// the unconditional-success stub at 0x00458ab0; it never services the
	// retained emitter through Particle_emitter_update (0x0049c680).
	// Consequently types 2 and 4 emit no particles, and type 3 is a single
	// synchronous burst rather than a 1000-tick continuous effect.
	if (effect_type != 3)
	{
		return;
	}
	ParticleEmitter emitter;
	if (!particle_emitter_initialize(
			world.particles,
			emitter,
			static_cast<std::uint8_t>(
				ParticleEmitterStyle::shield_impact),
			1000u,
			simulation_tick))
	{
		return;
	}
	emitter.local_position = position;
	emitter.local_basis = impact_orientation(direction);
	emitter.direction_center = {0.0f, 0.0f, 1.0f};
	emitter.direction_spread = {0.25f, 0.25f, 0.0f};
	emitter.speed_base = 10.0f;
	emitter.speed_random = 2.0f;
	(void)particle_emitter_burst(
		world,
		emitter,
		20,
		simulation_tick,
		world.shields.camera_position,
		world.shields.camera_forward);
}

void register_sphere(
	World& world,
	WorldObject& object,
	const glm::vec3& world_impact,
	std::uint8_t effect_type,
	std::uint32_t simulation_tick)
{
	const std::uint16_t object_index =
		static_cast<std::uint16_t>(
			&object - std::begin(world.objects));
	ShieldSphereState& state =
		world.shields.sphere[object_index];
	shields_initialize_sphere_state(state);
	state.active = true;
	state.last_hit_tick = simulation_tick;
	const std::uint8_t buffer = state.next_hit_buffer;
	const ShieldSphereGeometry& geometry =
		shield_sphere_geometry(state.lod);
	const float radius = object.radius * 1.1f;
	const glm::vec3 local_hit =
		glm::transpose(object.orientation)
			* (world_impact - object.position);
	const glm::vec3 normalized_hit =
		radius != 0.0f ? local_hit / radius : local_hit;
	for (std::uint32_t index = 0;
		index < geometry.point_count;
		++index)
	{
		// HShield_register_hit normalizes both transformed vectors,
		// divides their dot product by the length product, and passes the
		// result through acos before applying the 1.2/1.4 radial band.
		const float relation = std::abs(std::acos(std::clamp(
			glm::dot(
				glm::normalize(geometry.points[index]),
				glm::normalize(normalized_hit)),
			-1.0f,
			1.0f)));
		// HShield_register_hit (LANCER.EXE 0x0049f3cf..0x0049f418)
		// leaves the selected history buffer untouched outside the 1.4
		// angular band. It also walks the BMO's currently selected LOD,
		// not the constructor's LOD-0 mesh.
		if (relation <= 1.4f)
		{
			state.hit[buffer][index] = std::min(
				2.0f,
				(relation - 1.2f)
					* (5.0f / 6.0f)
					+ 1.5f);
		}
	}
	state.next_hit_buffer =
		static_cast<std::uint8_t>((buffer + 1u) & 7u);
	enqueue_effect(
		world,
		&object,
		world_impact,
		glm::normalize(world_impact - object.position),
		effect_type,
		simulation_tick);

	const bool local_player =
		world.player.index == object_index
		&& world.player.generation == object.generation;
	if (local_player)
	{
		if (simulation_tick >= world.shields.player_sound_deadline)
		{
			world.shields.player_sound_deadline =
				simulation_tick + 30;
			world_queue_sound_explicit(
				world,
				world_impact,
				glm::normalize(world_impact - object.position),
				glm::vec3{0.0f},
				74,
				4);
		}
	}
	else
	{
		world_queue_sound_explicit(
			world,
			world_impact,
			glm::normalize(world_impact - object.position),
			glm::vec3{0.0f},
			14,
			0);
	}
	if (!local_player
		|| (object.runtime_flags & kObjectFlagRenderSuppressed) == 0)
	{
		shields_emit_sparks(
			world,
			3,
			world_impact,
			-glm::normalize(
				world_impact - object.position),
			10.0f,
			1.0f,
			5.0f,
			10,
			simulation_tick);
	}
}

void register_cap(
	World& world,
	WorldObject& object,
	std::int16_t model_index,
	const glm::vec3& world_impact,
	std::uint8_t effect_type,
	std::uint32_t simulation_tick)
{
	const std::uint16_t object_index =
		static_cast<std::uint16_t>(
			&object - std::begin(world.objects));
	CapShieldSlot* selected = nullptr;
	for (CapShieldSlot& slot : world.shields.cap)
	{
		if (slot.expiry_tick != 0
			&& slot.object_index == object_index
			&& slot.object_generation == object.generation
			&& slot.model_index == model_index)
		{
			selected = &slot;
			break;
		}
	}
	if (selected == nullptr)
	{
		selected = &world.shields.cap[
			world.shields.replacement_cursor
				% kCapShieldSlotCount];
		*selected = {};
		selected->object_index = object_index;
		selected->object_generation = object.generation;
		selected->model_index = model_index;
		selected->birth_tick = simulation_tick;
		world.shields.replacement_cursor =
			(world.shields.replacement_cursor + 1u)
				% kCapShieldSlotCount;
	}
	selected->expiry_tick = simulation_tick + 200;
	float radius = 8000.0f;
	if (model_index >= 0
		&& static_cast<std::size_t>(model_index)
			< object.model_references.size())
	{
		const ObjectModelReference& model =
			object.model_references[model_index];
		radius = std::min(
			glm::distance(model.bounds_min, model.bounds_max)
				* 0.3f,
			8000.0f);
	}
	CapShieldHit& hit =
		selected->hit[selected->next_hit_buffer];
	glm::vec3 local_center = world_impact - object.position;
	if (model_index >= 0
		&& static_cast<std::size_t>(model_index)
			< object.model_references.size())
	{
		const ObjectModelReference& model =
			object.model_references[model_index];
		const glm::mat3 basis =
			object.orientation
			* glm::mat3(model.local_transform);
		const glm::vec3 origin =
			object.position
			+ object.orientation
				* glm::vec3(model.local_transform[3]);
		local_center =
			glm::transpose(basis) * (world_impact - origin);
	}
	hit = {
		local_center,
		radius,
		simulation_tick,
		true,
	};
	selected->next_hit_buffer =
		static_cast<std::uint8_t>(
			(selected->next_hit_buffer + 1u) & 7u);
	enqueue_effect(
		world,
		&object,
		world_impact,
		glm::normalize(world_impact - object.position),
		effect_type,
		simulation_tick);
}
}

const ShieldSphereGeometry& shield_sphere_geometry(std::uint32_t lod)
{
	// Shield_init (LANCER.EXE 0x0049ee90) builds these six global SR_Mesh
	// instances once from the exact longitude/latitude pairs at 0x0050884c.
	static const std::array<ShieldSphereGeometry, kShieldLodCount> geometry =
		[]
		{
			std::array<ShieldSphereGeometry, kShieldLodCount> result{};
			for (std::uint32_t index = 0;
				index < kShieldLodCount;
				++index)
			{
				build_shield_sphere(
					kShieldSphereSegments[index][0],
					kShieldSphereSegments[index][1],
					result[index]);
			}
			return result;
		}();
	return geometry[lod];
}

void shields_initialize_sphere_state(ShieldSphereState& state)
{
	if (state.initialized)
	{
		return;
	}
	const ShieldSphereGeometry& geometry = shield_sphere_geometry(0);
	for (std::uint32_t index = 0;
		index < geometry.point_count;
		++index)
	{
		state.uv[index] = {
			geometry.points[index].x,
			geometry.points[index].y};
	}
	state.initialized = true;
}

void shields_reset(ShieldRuntime& runtime)
{
	runtime = {};
	for (ShieldSphereState& state : runtime.sphere)
	{
		state.flicker_until_tick = -1;
		state.uv_center_x = 0.3f;
		state.uv_center_y = 0.3f;
	}
}

void shields_register_hit(
	World& world,
	WorldObject& object,
	std::int16_t model_index,
	const glm::vec3& world_impact,
	std::uint8_t effect_type,
	std::uint32_t simulation_tick)
{
	if (!object.active)
	{
		return;
	}
	bool have_bank = false;
	for (float bank : object.primary_shields)
	{
		have_bank = have_bank || bank != 0.0f;
	}
	if (!have_bank)
	{
		return;
	}
	// ShieldSphere_register_hit (LANCER.EXE 0x0049f1e0) first rejects an
	// object whose four banks are all exactly zero, then redirects a cloaked
	// impact to Cloak_register_hit before constructing shield visuals.
	if ((object.runtime_flags & 0x00000100u) != 0)
	{
		world_register_cloak_hit(object, world_impact, simulation_tick);
		return;
	}
	if (model_index >= 0)
	{
		// CapShield_register_hit (0x0049f4a0) suppresses an owning object
		// in protection state five. A FORCEFIELD node is also suppressed
		// by the owner's exact runtime protection bit 0x40.
		if (object.protection_state == 5)
		{
			return;
		}
		if (static_cast<std::size_t>(model_index)
				< object.model_references.size()
			&& object.model_references[model_index].forcefield
			&& (object.runtime_flags & kObjectFlagDestroyed) != 0)
		{
			return;
		}
	}
	if (model_index >= 0)
	{
		register_cap(
			world,
			object,
			model_index,
			world_impact,
			effect_type,
			simulation_tick);
	}
	else
	{
		register_sphere(
			world,
			object,
			world_impact,
			effect_type,
			simulation_tick);
	}
}

void shields_create_impact_effect(
	World& world,
	WorldObject* owner,
	const glm::vec3& position,
	const glm::vec3& direction,
	std::uint8_t effect_type,
	std::uint32_t simulation_tick)
{
	if (effect_type < 2 || effect_type > 5)
	{
		return;
	}
	enqueue_effect(
		world,
		owner,
		position,
		direction,
		effect_type,
		simulation_tick);
}

void shields_emit_sparks(
	World& world,
	std::uint8_t type,
	const glm::vec3& position,
	const glm::vec3& axis,
	float base_speed,
	float angular_spread,
	float speed_spread,
	std::int16_t count,
	std::uint32_t simulation_tick)
{
	if (count <= 0)
	{
		return;
	}
	if ((type == 2 || type == 3)
		&& glm::distance(
			position, world.shields.camera_position) > 20000.0f)
	{
		return;
	}
	const glm::mat3 frame = impact_orientation(axis);
	for (std::int16_t index = 0; index < count; ++index)
	{
		const float deviation_x =
			(random_unit(world) - 0.5f) * angular_spread;
		const float deviation_y =
			(random_unit(world) - 0.5f) * angular_spread;
		const glm::vec3 direction =
			(frame * math::rotation_from_euler({
				deviation_x,
				deviation_y,
				0.0f,
			}))[2];
		const float speed =
			base_speed
			+ (random_unit(world) - 0.5f) * speed_spread;
		Spark& spark =
			world.shields.sparks[
				world.shields.spark_cursor++];
		spark = {
			position,
			direction * speed,
			direction,
			simulation_tick,
			type,
			true,
		};
	}
}

void shields_service(
	World& world,
	std::uint32_t simulation_tick,
	const glm::vec3& camera_position,
	const glm::vec3& camera_forward,
	std::int16_t camera_mode)
{
	world.shields.camera_position = camera_position;
	world.shields.camera_forward = camera_forward;
	world.shields.camera_mode = camera_mode;
	for (CapShieldSlot& slot : world.shields.cap)
	{
		if (slot.expiry_tick != 0
			&& simulation_tick >= slot.expiry_tick)
		{
			slot = {};
		}
	}
	constexpr std::uint32_t lifetimes[5] = {
		300, 100, 100, 100, 300,
	};
	constexpr float drag[5] = {
		0.9999f, 0.995f, 0.995f, 0.995f, 0.9999f,
	};
	for (Spark& spark : world.shields.sparks)
	{
		if (!spark.active || spark.type >= 5)
		{
			continue;
		}
		const std::uint32_t elapsed =
			simulation_tick - spark.birth_tick;
		if (elapsed > lifetimes[spark.type])
		{
			spark = {};
			continue;
		}
		spark.position += spark.damped_velocity;
		spark.position += spark.unit_drift;
		spark.damped_velocity *= drag[spark.type];
	}
}

glm::vec3 shield_color_lookup(float value, bool forcefield)
{
	if (!(value > 0.0f) || !(value < 1.0f))
	{
		return {0.0f, 0.0f, 0.0f};
	}
	const std::uint32_t index = static_cast<std::uint32_t>(
		std::floor(value * 1023.0f));
	const float table_value =
		static_cast<float>(index) / 1024.0f;
	const auto cosine_lerp = [](float a, float b, float t)
	{
		return a + (b - a)
			* (1.0f - std::cos(glm::pi<float>() * t))
			* 0.5f;
	};
	float green = 0.0f;
	float blue = 0.0f;
	if (table_value <= 1.0f)
	{
		if (table_value > 0.6f)
		{
			const float amount =
				(1.0f - table_value) * 2.5f;
			green = cosine_lerp(0.0f, 0.7f, amount);
			blue = cosine_lerp(0.0f, 1.0f, amount);
		}
		else if (table_value > 0.4f)
		{
			const float amount =
				(0.6f - table_value) * 5.0f;
			green = cosine_lerp(0.7f, 0.0f, amount);
			blue = cosine_lerp(1.0f, 0.3f, amount);
		}
		else if (table_value > 0.35f)
		{
			blue = cosine_lerp(
				0.3f,
				0.0f,
				(0.6f - table_value) * (5.0f / 3.0f));
		}
	}
	if (forcefield)
	{
		std::swap(green, blue);
		return glm::vec3{0.0f, green, blue}
			* (0.8f * 0.07f);
	}
	return glm::vec3{0.0f, green, blue} * 0.07f;
}
}
