#include "game/weapons.hpp"

#include "ai/scripted_commands.hpp"
#include "assets/gameplay_model.hpp"
#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "game/damage.hpp"
#include "game/model_animation.hpp"
#include "mission/deathmatch_scenarios.hpp"
#include "mission/network_runtime.hpp"
#include "mission/player_comms.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace sl_open::game
{
namespace
{
constexpr std::uint32_t kDamageLogInterval = kSimulationHz;
constexpr std::uint8_t kBulletBehavior[assets::kGunStatsCount] = {
	0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 1, 1, 1, 1, 1,
};
constexpr std::uint8_t kBulletSoundDefinition[
	assets::kGunStatsCount] = {
	0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 2, 2, 9, 9,
};

bool guns_suppressed(const WorldObject& object)
{
	// DisableGuns_apply_entity writes gameplay flag 0x8000. Every retail
	// gun-owner path tests that flag; the mirrored boolean preserves the
	// same state for platform-facing consumers.
	return object.guns_disabled
		|| (object.runtime_flags & 0x00008000u) != 0;
}

void queue_weapon_sound(
	WeaponRuntime& runtime,
	std::uint8_t definition,
	const glm::vec3& position,
	const glm::vec3& direction,
	std::uint8_t requested_class,
	const GunProjectile* projectile = nullptr)
{
	if (runtime.sound_count >= std::size(runtime.sound_events))
	{
		return;
	}
	const std::uint8_t slot = static_cast<std::uint8_t>(
		(runtime.sound_read + runtime.sound_count)
		% std::size(runtime.sound_events));
	WeaponSoundEvent& event = runtime.sound_events[slot];
	event = {};
	event.position = position;
	event.direction = direction;
	event.definition = definition;
	event.requested_class = requested_class;
	if (projectile != nullptr)
	{
		event.velocity = projectile->velocity;
		event.source_generation = projectile->sound_generation;
		event.source_index = static_cast<std::uint16_t>(
			projectile - std::begin(runtime.projectiles));
		event.projectile = true;
	}
	++runtime.sound_count;
}

void queue_impact_effect(
	World& world,
	const glm::vec3& position,
	const glm::vec3& direction,
	float size,
	std::uint8_t type,
	std::uint32_t simulation_tick)
{
	if (type >= 13)
	{
		(void)explosion_billboard_create(
			world.death_effects,
			world,
			position,
			glm::vec3{0.0f},
			ExplosionBillboardType::separate_frames,
			size,
			150,
			true,
			0,
			false,
			false,
			simulation_tick);
		return;
	}
	if (type == 5)
	{
		// ShieldFX_create_impact type five (LANCER.EXE 0x004a0375)
		// creates no atlas sprite. It plays definition 71 and routes the
		// impact through Rock_chunk_spawn's 300-record retained pool.
		rock_chunk_spawn_world(
			world, position, direction, false, simulation_tick);
		world_queue_sound_explicit(
			world,
			position,
			direction,
			glm::vec3{0.0f},
			71,
			0);
	}
}

void create_nova_expiry_effect(
	World& world,
	const mission::Runtime& mission,
	const GunProjectile& projectile,
	std::uint32_t simulation_tick)
{
	// gun_projectiles_update_and_render, LANCER.EXE
	// 0x0047ad19..0x0047b058. This is the terminal effect for the ordinary
	// type-eleven Nova projectile, distinct from the charged-beam owner.
	world_queue_sound_explicit(
		world,
		projectile.position,
		projectile.orientation[2],
		projectile.velocity,
		10,
		3);
	const glm::vec3 camera_delta =
		mission.particle_camera_position - projectile.position;
	if (glm::dot(camera_delta, camera_delta) >= 100000000.0f)
	{
		(void)explosion_billboard_create(
			world.death_effects,
			world,
			projectile.position,
			glm::vec3{0.0f},
			ExplosionBillboardType::separate_frames,
			600.0f,
			40,
			true,
			0,
			false,
			true,
			simulation_tick);
		return;
	}

	(void)explosion_billboard_create(
		world.death_effects,
		world,
		projectile.position,
		glm::vec3{0.0f},
		ExplosionBillboardType::separate_frames,
		280.0f,
		130,
		true,
		0,
		false,
		true,
		simulation_tick);
	const std::int32_t secondary_delay = 20
		- static_cast<std::int32_t>(
			static_cast<float>(world_rand15(world))
				* (1.0f / 32767.0f) * -20.0f);
	(void)explosion_billboard_create(
		world.death_effects,
		world,
		projectile.position,
		glm::vec3{0.0f},
		ExplosionBillboardType::separate_frames,
		245.0f,
		130,
		false,
		secondary_delay,
		false,
		true,
		simulation_tick);
	(void)particle_emitter_burst_world(
		world,
		projectile.position,
		glm::mat3{1.0f},
		glm::vec3{0.0f},
		glm::vec3{1.0f},
		1.4f,
		0.35f,
		30,
		ParticleEmitterStyle::nova_expiry,
		simulation_tick,
		mission.particle_camera_position,
		mission.particle_camera_forward);

	for (std::uint32_t ordinal = 0; ordinal < 10; ++ordinal)
	{
		const float velocity_y =
			static_cast<float>(world_rand15(world))
				* (1.0f / 32767.0f) * glm::two_pi<float>();
		const float velocity_x =
			static_cast<float>(world_rand15(world))
				* (1.0f / 32767.0f) * glm::two_pi<float>();
		const glm::vec3 velocity = math::rotation_from_euler({
			velocity_x, velocity_y, 0.0f})
			* glm::vec3{0.0f, 0.0f, 10.5f};
		const std::int32_t delay = static_cast<std::int32_t>(
			static_cast<float>(world_rand15(world))
				* (1.0f / 32767.0f) * 20.0f);
		const std::int32_t duration = 50
			- static_cast<std::int32_t>(
				static_cast<float>(world_rand15(world))
					* (1.0f / 32767.0f) * -70.0f);
		const float size = 14.0f
			+ static_cast<float>(world_rand15(world))
				* (1.0f / 32767.0f) * 56.0f;
		const ExplosionBillboardType type =
			world_rand15(world) % 2u == 0
				? ExplosionBillboardType::separate_frames
				: ExplosionBillboardType::sheet;
		(void)explosion_billboard_create(
			world.death_effects,
			world,
			projectile.position,
			velocity,
			type,
			size,
			duration,
			false,
			delay,
			true,
			false,
			simulation_tick);
	}
	for (std::uint32_t ordinal = 0; ordinal < 10; ++ordinal)
	{
		const float velocity_y =
			static_cast<float>(world_rand15(world))
				* (1.0f / 32767.0f) * glm::two_pi<float>();
		const float velocity_x =
			static_cast<float>(world_rand15(world))
				* (1.0f / 32767.0f) * glm::two_pi<float>();
		const glm::vec3 velocity = math::rotation_from_euler({
			velocity_x, velocity_y, 0.0f})
			* glm::vec3{0.0f, 0.0f, 10.5f};
		const std::int32_t delay = static_cast<std::int32_t>(
			static_cast<float>(world_rand15(world))
				* (1.0f / 32767.0f) * 20.0f);
		const std::int32_t duration = 50
			- static_cast<std::int32_t>(
				static_cast<float>(world_rand15(world))
					* (1.0f / 32767.0f) * -70.0f);
		const float size = 14.0f
			+ static_cast<float>(world_rand15(world))
				* (1.0f / 32767.0f) * 56.0f;
		(void)explosion_billboard_create(
			world.death_effects,
			world,
			projectile.position,
			velocity,
			ExplosionBillboardType::separate_frames,
			size,
			duration,
			false,
			delay,
			true,
			true,
			simulation_tick);
	}
}

std::uint8_t compound_impact_type(std::uint16_t object_type)
{
	// gun_projectile_process_collisions (0x00479b40) selects the
	// explosion-only ShieldFX type 5 for these three exact object-type
	// ranges; every other compound feature receives the ordinary
	// immediate 20-particle type-3 burst.
	return ((object_type >= 0x79u && object_type <= 0x7fu)
			|| (object_type >= 0x85u && object_type <= 0x8bu)
			|| (object_type >= 0xf0u && object_type <= 0xf3u))
		? 5
		: 3;
}

void queue_muzzle_flash(
	WeaponRuntime& runtime,
	const GunProjectile& projectile,
	std::uint32_t simulation_tick)
{
	// gun_fire_projectile_from_model writes this compiled duration table
	// from 0x00500f64 to the cloned muzzle-effect mesh.
	constexpr std::uint8_t duration[assets::kGunStatsCount] = {
		50, 50, 30, 20, 50, 50, 50, 50, 50, 50,
		0, 0, 0, 0, 0,
	};
	if (projectile.type_index < 0
		|| projectile.type_index
			>= static_cast<std::int32_t>(std::size(duration))
		|| duration[projectile.type_index] == 0)
	{
		return;
	}
	for (MuzzleFlash& flash : runtime.muzzle_flashes)
	{
		if (!flash.active
			|| simulation_tick >= flash.expiration_tick)
		{
			flash = {
				projectile.position,
				projectile.orientation,
				simulation_tick,
				simulation_tick + duration[projectile.type_index],
				projectile.shooter_affiliation,
				static_cast<std::uint8_t>(
					projectile.type_index),
				true,
			};
			return;
		}
	}
}

bool segment_intersects_aabb(
	const glm::vec3& start,
	const glm::vec3& end,
	const glm::vec3& minimum,
	const glm::vec3& maximum)
{
	// segment_intersect_aabb (LANCER.EXE 0x0049b6a0) treats only a
	// strictly interior start as an immediate hit. It then tests the
	// boundary crossed from each outside axis, using inclusive bounds on
	// the other two coordinates, and rejects an endpoint-only crossing.
	if (glm::all(glm::greaterThan(start, minimum))
		&& glm::all(glm::lessThan(start, maximum)))
	{
		return true;
	}
	for (std::uint32_t axis = 0; axis < 3; ++axis)
	{
		if ((start[axis] < minimum[axis]
				&& end[axis] < minimum[axis])
			|| (start[axis] > maximum[axis]
				&& end[axis] > maximum[axis]))
		{
			return false;
		}
	}
	const glm::vec3 direction = end - start;
	float earliest = 1.0f;
	bool crossed = false;
	for (std::uint32_t axis = 0; axis < 3; ++axis)
	{
		float boundary = 0.0f;
		if (start[axis] < minimum[axis])
		{
			boundary = minimum[axis];
		}
		else if (start[axis] > maximum[axis])
		{
			boundary = maximum[axis];
		}
		else
		{
			continue;
		}
		if (direction[axis] == 0.0f)
		{
			continue;
		}
		const float fraction =
			(boundary - start[axis]) / direction[axis];
		if (!(fraction < earliest))
		{
			continue;
		}
		const glm::vec3 point = start + direction * fraction;
		const std::uint32_t other_a = (axis + 1) % 3;
		const std::uint32_t other_b = (axis + 2) % 3;
		if (point[other_a] >= minimum[other_a]
			&& point[other_a] <= maximum[other_a]
			&& point[other_b] >= minimum[other_b]
			&& point[other_b] <= maximum[other_b])
		{
			earliest = fraction;
			crossed = true;
		}
	}
	return crossed && earliest < 1.0f;
}

void segment_to_model_space(
	const WorldObject& object,
	const ObjectModelReference& model,
	const glm::vec3& start,
	const glm::vec3& end,
	glm::vec3& local_start,
	glm::vec3& local_end)
{
	const glm::mat3 model_basis =
		object.orientation * glm::mat3(model.local_transform);
	const glm::vec3 model_center =
		object.position
		+ object.orientation * glm::vec3(model.local_transform[3]);
	const glm::mat3 inverse_basis = glm::transpose(model_basis);
	local_start = inverse_basis * (start - model_center);
	local_end = inverse_basis * (end - model_center);
}

bool segment_intersects_collision_node(
	const assets::GameplayCollisionNode& node,
	const glm::vec3& model_start,
	const glm::vec3& model_end)
{
	const glm::mat3 inverse_orientation =
		glm::transpose(node.orientation);
	const glm::vec3 start =
		inverse_orientation * (model_start - node.center);
	const glm::vec3 end =
		inverse_orientation * (model_end - node.center);
	return segment_intersects_aabb(
		start, end, -node.half_extents, node.half_extents);
}

bool segment_intersects_triangle(
	const glm::vec3& start,
	const glm::vec3& end,
	const assets::GameplayCollisionTriangle& triangle,
	float& fraction)
{
	// segment_intersect_triangle (0x004ad700) solves the segment
	// parameter and two barycentric coordinates through an explicit
	// 3-by-3 inverse, then includes every boundary in [0,1].
	const glm::vec3 segment = end - start;
	const glm::vec3 edge_a =
		triangle.points[1] - triangle.points[0];
	const glm::vec3 edge_b =
		triangle.points[2] - triangle.points[0];
	const glm::vec3 cross_edges = glm::cross(edge_a, edge_b);
	const float determinant = glm::dot(segment, cross_edges);
	if (determinant == 0.0f)
	{
		return false;
	}
	const glm::vec3 origin_delta =
		triangle.points[0] - start;
	const float inverse_determinant = 1.0f / determinant;
	const float segment_parameter =
		glm::dot(origin_delta, cross_edges) * inverse_determinant;
	const float barycentric_a =
		-glm::dot(
			segment,
			glm::cross(origin_delta, edge_b))
		* inverse_determinant;
	const float barycentric_b =
		-glm::dot(
			segment,
			glm::cross(edge_a, origin_delta))
		* inverse_determinant;
	if (segment_parameter < 0.0f
		|| segment_parameter > 1.0f
		|| barycentric_a < 0.0f
		|| barycentric_a > 1.0f
		|| barycentric_b < 0.0f
		|| barycentric_b > 1.0f
		|| barycentric_a + barycentric_b < 0.0f
		|| barycentric_a + barycentric_b > 1.0f)
	{
		return false;
	}
	fraction = segment_parameter;
	return true;
}

template<typename LeafCallback>
void traverse_collision_tree(
	const assets::GameplayCollisionTree& tree,
	const glm::vec3& model_start,
	const glm::vec3& model_end,
	LeafCallback&& leaf_callback)
{
	if (tree.nodes.empty())
	{
		return;
	}
	std::uint32_t stack[200]{};
	std::uint32_t stack_count = 1;
	stack[0] = 0;
	while (stack_count != 0)
	{
		const std::uint32_t index = stack[--stack_count];
		if (index >= tree.nodes.size())
		{
			continue;
		}
		const assets::GameplayCollisionNode& node = tree.nodes[index];
		if (!segment_intersects_collision_node(
				node, model_start, model_end))
		{
			continue;
		}
		if (node.polygons.empty())
		{
			// object_traverse_collision_obb_tree (0x0049bd30) pushes A
			// and then B; its LIFO stack consequently visits B first.
			if (stack_count + 2 <= std::size(stack))
			{
				stack[stack_count++] = node.child_a;
				stack[stack_count++] = node.child_b;
			}
			continue;
		}
		leaf_callback(node);
	}
}

void collect_compound_candidates(
	GunProjectile& projectile,
	const WorldObject& object,
	std::uint16_t object_index,
	const glm::vec3& sweep_end)
{
	// gun_projectile_collect_compound_candidate (0x0047bc90) is invoked
	// once for each live SR model wrapper. It tests the wrapper's transformed
	// mesh bounds and appends that model exactly once. It does not traverse
	// the model's collision OBB tree here; that happens when the projectile
	// reaches the object in gun_projectile_process_collisions. Appending one
	// entry per intersected OBB leaf exhausts retail's 20-entry list on large
	// hulls and can discard both later models of that hull and later objects.
	for (std::uint32_t model_index = 0;
		model_index < object.model_references.size()
			&& projectile.candidate_count < kMaxProjectileCandidates;
		++model_index)
	{
		const ObjectModelReference& model =
			object.model_references[model_index];
		if (model.collision == nullptr
			|| model.collision->nodes.empty()
			|| model.removed
			|| (model.runtime_flags & 0x0020u) != 0)
		{
			continue;
		}
		glm::vec3 model_start;
		glm::vec3 model_end;
		segment_to_model_space(
			object,
			model,
			projectile.position,
			sweep_end,
			model_start,
			model_end);
		if (!segment_intersects_aabb(
				model_start,
				model_end,
				model.bounds_min,
				model.bounds_max))
		{
			continue;
		}
		GunProjectile::CollisionCandidate& record =
			projectile.candidates[projectile.candidate_count++];
		record.object_index = object_index;
		record.model_index = static_cast<std::int16_t>(model_index);
	}
}

GunProjectile* allocate_projectile(WeaponRuntime& runtime)
{
	// gun_fire_projectile_from_model (LANCER.EXE 0x0047c5f0) scans the
	// fixed 200-record pool from its beginning and silently drops a shot
	// when no free record remains.
	for (GunProjectile& projectile : runtime.projectiles)
	{
		if (projectile.type_index == -1)
		{
			return &projectile;
		}
	}
	if (!runtime.pool_exhaustion_reported)
	{
		diagnostics::mission_log(
			"weapons projectile-pool exhausted capacity=%u",
			static_cast<unsigned>(kMaxGunProjectiles));
		runtime.pool_exhaustion_reported = true;
	}
	return nullptr;
}

glm::mat3 projectile_look_at_points(
	const glm::vec3& from,
	const glm::vec3& to)
{
	// SR_mat3_look_at_points, LANCER.EXE 0x004c1940, with zero roll.
	glm::vec3 delta = to - from;
	glm::mat3 orientation{1.0f};
	orientation = math::postrotate(
		orientation,
		std::atan2(delta.x, delta.z),
		glm::vec3{0.0f, 1.0f, 0.0f});
	delta = glm::transpose(orientation) * delta;
	return math::postrotate(
		orientation,
		-std::atan2(delta.y, delta.z),
		glm::vec3{1.0f, 0.0f, 0.0f});
}

void spawn_projectile(
	WeaponRuntime& runtime,
	World& world,
	const WorldObject& shooter,
	const GunMount& mount,
	const assets::GunStatsTable& gun_stats,
	const assets::ShipStatsTable& ship_stats,
	std::uint8_t bullet_type,
	std::uint32_t simulation_tick)
{
	GunProjectile* projectile = allocate_projectile(runtime);
	if (projectile == nullptr)
	{
		return;
	}
	// gun_fire_projectile_from_model (LANCER.EXE 0x0047c67c..0x0047c693)
	// performs this only after a pool record has been secured.  Authored
	// one-based type 12 flak becomes type 13 on two of every five CRT-rand
	// residues; the substituted projectile uses the complete adjacent stats
	// record, not merely its visual definition.
	if (bullet_type == 12 && world_rand15(world) % 5u < 2u)
	{
		bullet_type = 13;
	}
	const assets::GunStats& bullet =
		gun_stats.records[bullet_type - 1];
	*projectile = {};
	projectile->type_index =
		static_cast<std::int32_t>(bullet_type) - 1;
	projectile->spawn_tick = simulation_tick;
	projectile->expiration_tick =
		static_cast<std::int32_t>(simulation_tick)
		+ bullet.lifetime_ticks;
	glm::vec3 emitter_position = mount.local_position;
	glm::mat3 emitter_orientation = mount.local_orientation;
	if (mount.emitter_reference >= 0
		&& mount.emitter_reference
			< static_cast<std::int16_t>(
				shooter.model_references.size()))
	{
		const glm::mat4 emitter =
			shooter.model_references[
				mount.emitter_reference].local_transform
			* mount.emitter_from_part;
		emitter_position = glm::vec3(emitter[3]);
		emitter_orientation = glm::mat3(emitter);
	}
	projectile->orientation =
		shooter.orientation * emitter_orientation;
	projectile->position =
		shooter.position + shooter.orientation * emitter_position;
	projectile->previous_position = projectile->position;
	projectile->scene_position = projectile->position;
	projectile->velocity =
		projectile->orientation[2] * bullet.speed;
	// gun_projectile_spawn, LANCER.EXE 0x0047c1c2..0x0047c2e3.
	// Blindfire redirects ordinary projectiles after the muzzle transform is
	// built. Zero-based projectile types 10..14 retain their authored axis.
	if (shooter.blindfire_active
		&& (projectile->type_index < 10
			|| projectile->type_index > 14))
	{
		glm::vec3 direction =
			shooter.blindfire_aim_point - projectile->position;
		const float length = glm::length(direction);
		if (length != 0.0f)
		{
			direction /= length;
		}
		else
		{
			// Vec3_normalize's exact zero-vector result at 0x004c1370.
			direction = glm::vec3{0.0f, 0.0f, 0x1p-120f};
		}
		projectile->velocity = direction * bullet.speed;
		projectile->orientation = projectile_look_at_points(
			projectile->position,
			shooter.blindfire_aim_point);
	}
	projectile->shooter_index = static_cast<std::uint16_t>(
		&shooter - std::begin(world.objects));
	projectile->shooter_generation = shooter.generation;
	projectile->shooter_affiliation = shooter.allegiance_class;
	projectile->active = true;
	// gun_projectile_create_type_visuals (0x0047d9a0) consumes CRT
	// rand() only for these three constructors. Retain the normalized
	// samples because the replacement renderer constructs the same child
	// transforms after the simulation/render boundary.
	std::uint32_t visual_random_count = 0;
	switch (projectile->type_index)
	{
	case 1:
		visual_random_count = 3;
		break;
	case 2:
		visual_random_count = 6;
		break;
	case 8:
		visual_random_count = 12;
		break;
	default:
		break;
	}
	for (std::uint32_t index = 0;
		index < visual_random_count;
		++index)
	{
		projectile->visual_random[index] =
			static_cast<float>(world_rand15(world))
				* (1.0f / 32767.0f);
	}

	// gun_projectile_spawn (0x0047bdb0) builds this fixed candidate list
	// once, in live-object index order. For an ordinary root, its broad
	// phase projects the center onto the projectile's full lifetime and
	// expands the radius by effective_max_speed * projected_time.
	const float speed_squared =
		glm::dot(projectile->velocity, projectile->velocity);
	if (speed_squared > 0.0f)
	{
		for (std::uint16_t index = 0;
			index < kMaxGameObjects
				&& projectile->candidate_count
					< kMaxProjectileCandidates;
			++index)
		{
			const WorldObject& candidate = world.objects[index];
			if (!candidate.active
				|| candidate.type >= 0x100
				|| (candidate.runtime_flags & 0x00000004u) != 0
				|| (&candidate == &shooter
					&& (candidate.type != 0x45
						|| !world.player_inside_type45_compound)))
			{
				continue;
			}
			const glm::vec3 delta =
				candidate.position - projectile->position;
			const float projected_time = std::clamp(
				glm::dot(delta, projectile->velocity) / speed_squared,
				0.0f,
				static_cast<float>(std::max(0, bullet.lifetime_ticks)));
			const glm::vec3 closest =
				projectile->position
				+ projectile->velocity * projected_time;
			const float conservative_radius =
				candidate.radius
				+ world_effective_max_speed(
					candidate, ship_stats, world.camera_mode)
					* projected_time;
			const glm::vec3 separation =
				candidate.position - closest;
			if (glm::dot(separation, separation)
				>= conservative_radius * conservative_radius)
			{
				continue;
			}
			if ((candidate.runtime_flags & kObjectFlagCompound) != 0)
			{
				collect_compound_candidates(
					*projectile,
					candidate,
					index,
					projectile->position
						+ projectile->velocity
							* static_cast<float>(
								std::max(
									0,
									bullet.lifetime_ticks)));
				continue;
			}
			GunProjectile::CollisionCandidate& record =
				projectile->candidates[projectile->candidate_count++];
			record.object_index = index;
			record.model_index = -1;
		}
	}
	++runtime.live_projectiles;
	++runtime.shot_serial;
	if (world.player.index == projectile->shooter_index
		&& world.player.generation == projectile->shooter_generation)
	{
		// gun_projectile_spawn, LANCER.EXE 0x0047be1f..0x0047be41,
		// overwrites the cockpit recoil scalar for every local-player shot.
		runtime.camera_recoil = 1.0f;
	}
	projectile->sound_generation = runtime.shot_serial;
	runtime.pool_exhaustion_reported = false;
	queue_muzzle_flash(runtime, *projectile, simulation_tick);
	queue_weapon_sound(
		runtime,
		kBulletSoundDefinition[bullet_type - 1],
		projectile->position,
		projectile->orientation[2],
		static_cast<std::uint8_t>(
			bullet_type >= 14 ? 4 : shooter.player ? 1 : 0),
		projectile);
}

bool mount_eligible(
	const GunMount& mount,
	const assets::GunStatsTable& gun_stats)
{
	return (mount.mount_kind == 0 || mount.mount_kind == 2)
		&& mount.bullet_type != 0
		&& mount.bullet_type <= assets::kGunStatsCount
		&& gun_stats.ready;
}

void destroy_projectile(
	WeaponRuntime& runtime,
	GunProjectile& projectile)
{
	projectile = {};
	projectile.type_index = -1;
	projectile.expiration_tick = -1;
	if (runtime.live_projectiles != 0)
	{
		--runtime.live_projectiles;
	}
}

std::uint8_t shield_bank_for_impact(
	const WorldObject& target,
	const glm::vec3& impact)
{
	// GameObject_directional_bank_from_world_point (0x00463d30):
	// transpose into root-local space, normalize X and Z by the authored
	// full bounds, prefer Z on ties, then select the signed side.
	const glm::vec3 local =
		glm::transpose(target.orientation) * (impact - target.position);
	const float normalized_x =
		local.x / (target.bounds_max.x - target.bounds_min.x);
	const float normalized_z =
		local.z / (target.bounds_max.z - target.bounds_min.z);
	if (std::abs(normalized_x) > std::abs(normalized_z))
	{
		return normalized_x > 0.0f ? 1 : 0;
	}
	return normalized_z > 0.0f ? 2 : 3;
}

void log_damage_transition(
	WeaponRuntime& runtime,
	const WorldObject& target,
	std::uint16_t target_index,
	std::uint16_t shooter_index,
	std::uint8_t bank,
	bool outer_depleted,
	std::uint32_t simulation_tick)
{
	if (!outer_depleted
		&& runtime.last_damage_log_target == target_index
		&& runtime.last_damage_log_bank == bank
		&& simulation_tick - runtime.last_damage_log_tick
			< kDamageLogInterval)
	{
		return;
	}
	diagnostics::mission_log(
		"damage target=%u mission=%u shooter=%u bank=%u "
		"shield=%.1f structure=%.1f%s",
		static_cast<unsigned>(target_index),
		static_cast<unsigned>(target.mission_index),
		static_cast<unsigned>(shooter_index),
		static_cast<unsigned>(bank),
		target.primary_shields[bank],
		target.secondary_shields[bank],
		outer_depleted ? " outer-depleted" : "");
	runtime.last_damage_log_tick = simulation_tick;
	runtime.last_damage_log_target = target_index;
	runtime.last_damage_log_bank = bank;
}

std::int32_t exposed_submodel_for_sweep(
	const WorldObject& target,
	const GunProjectile& projectile)
{
	// gun_projectile_hit_exposed_submodel (0x00479940) scans every live
	// tag-one SR model wrapper in GameObject+0x128 in source order and
	// retains the last wrapper whose transformed live mesh bounds intersect
	// the projectile sweep. It reads SR_Mesh+0x14/+0x20 through the BMO's
	// active geometry pointer; SR_mesh_calculate_bounds derives those fields
	// from the mesh point stream. The tag tested at wrapper+0 is the SR
	// scene-node tag shared by all instantiated model wrappers; it is not the
	// authored SRO model type stored in the wrapper's model data.
	std::int32_t retained = -1;
	for (std::uint32_t index = 0;
		index < target.model_references.size();
		++index)
	{
		const ObjectModelReference& model =
			target.model_references[index];
		if (model.removed
			|| (model.runtime_flags & 0x0020u) != 0)
		{
			continue;
		}
		glm::vec3 local_start;
		glm::vec3 local_end;
		segment_to_model_space(
			target,
			model,
			projectile.previous_position,
			projectile.position,
			local_start,
			local_end);
		if (segment_intersects_aabb(
				local_start,
				local_end,
				model.bounds_min,
				model.bounds_max))
		{
			retained = static_cast<std::int32_t>(index);
		}
	}
	return retained;
}

void apply_exposed_structural_hit(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	const assets::GunStats& bullet,
	GunProjectile& projectile,
	std::uint8_t cause,
	WorldObject& target,
	std::uint16_t target_index,
	std::uint8_t bank,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick)
{
	float damage = bullet.hull_damage;
	if ((projectile.type_index == 11
			|| projectile.type_index == 12)
		&& target_index < mission.player_prefix_count)
	{
		damage *= 2.5f;
	}
	if (!apply_structural_bank_damage(
			world,
			mission,
			target,
			ship_stats,
			bank,
			damage,
			projectile.shooter_index,
			cause,
			runtime.feedback_enabled,
			simulation_tick))
	{
		return;
	}
	log_damage_transition(
		runtime,
		target,
		target_index,
		projectile.shooter_index,
		bank,
		false,
		simulation_tick);
}

bool apply_ordinary_projectile_hit(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	const assets::GunStats& bullet,
	GunProjectile& projectile,
	std::uint8_t cause,
	WorldObject& target,
	std::uint16_t target_index,
	const glm::vec3& impact,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick)
{
	const std::uint8_t bank = shield_bank_for_impact(target, impact);

	// gun_projectile_process_collisions (0x00479b40) takes the exposed-hull
	// branch before testing spectral-shield bit 27. The structural owner has
	// its own invulnerability/dead-object gates, but spectral shields never
	// make an already exposed hull immune.
	if (projectile.type_index != 13
		&& projectile.type_index != 14
		&& (target.primary_shields[bank] < 0.0f
			|| target.protection_state == 4
			|| target.protection_state == 5))
	{
		if (exposed_submodel_for_sweep(target, projectile) >= 0)
		{
			apply_exposed_structural_hit(
				runtime,
				world,
				mission,
				bullet,
				projectile,
				cause,
				target,
				target_index,
				bank,
				ship_stats,
				simulation_tick);
		}
		// Retail returns from gun_projectile_process_collisions itself after
		// entering the exposed-hull helper, whether or not that helper finds
		// a live submodel.  In particular it never falls through to the
		// ordinary HShield_register_hit call at 0x0047a391.
		return false;
	}
	// gun_projectile_process_collisions reaches Cloak_register_hit only
	// after the exposed-hull helper has declined the hit. An exposed cloaked
	// model therefore takes structural damage without publishing a cloak
	// hit, exactly like the early return at 0x0047a180..0x0047a194.
	if ((target.runtime_flags & 0x00000100u) != 0)
	{
		world_register_cloak_hit(target, impact, simulation_tick);
	}
	if ((target.runtime_flags & 0x08000000u) != 0)
	{
		return true;
	}
	if (bullet.shield_damage <= 0.0f)
	{
		return true;
	}

	const bool local_player =
		world.player.index == target_index
		&& world.player.generation == target.generation;

	float shield_damage = bullet.shield_damage;
	if (local_player && (bank == 2 || bank == 3))
	{
		// The two flight-HUD shield pools at 0x0051cf78/0x0051cf34 are
		// consumed before their matching directional banks. Retail returns
		// while a pool remains positive; on the crossing hit it clamps the
		// pool to zero and forwards the full projectile damage.
		float& auxiliary = target.auxiliary_shields[bank - 2];
		if (auxiliary > 0.0f)
		{
			auxiliary -= shield_damage;
			if (auxiliary > 0.0f)
			{
				return true;
			}
			auxiliary = 0.0f;
		}
	}
	if ((projectile.type_index == 11 || projectile.type_index == 12)
		&& target_index < mission.player_prefix_count)
	{
		// The retail comparison is against the reserved player-object range
		// below 0x0058832c, including non-local multiplayer slots.
		shield_damage *= 2.5f;
	}

	const float previous_primary = target.primary_shields[bank];
	if (!apply_primary_bank_damage(
			world,
			mission,
			target,
			ship_stats,
			bank,
			shield_damage,
			bullet.hull_damage / bullet.shield_damage,
			projectile.shooter_index,
			cause,
			runtime.feedback_enabled,
			simulation_tick))
	{
		return true;
	}
	const bool outer_depleted =
		previous_primary >= 0.0f
		&& target.primary_shields[bank] < 0.0f;
	log_damage_transition(
		runtime,
		target,
		target_index,
		projectile.shooter_index,
		bank,
		outer_depleted,
		simulation_tick);
	return true;
}

bool segment_sphere_hit(
	const glm::vec3& start,
	const glm::vec3& end,
	const glm::vec3& center,
	float radius,
	glm::vec3& impact,
	float& projection,
	float* hit_fraction = nullptr)
{
	const glm::vec3 segment = end - start;
	const float length_squared = glm::dot(segment, segment);
	if (length_squared < 1.0f)
	{
		return false;
	}
	projection = std::clamp(
		glm::dot(center - start, segment) / length_squared,
		0.0f,
		1.0f);
	const glm::vec3 closest = start + segment * projection;
	const glm::vec3 closest_delta = closest - center;
	if (glm::dot(closest_delta, closest_delta) > radius * radius)
	{
		return false;
	}

	const glm::vec3 offset = start - center;
	const float a = length_squared;
	const float b = 2.0f * glm::dot(offset, segment);
	const float c = glm::dot(offset, offset) - radius * radius;
	const float discriminant = std::max(0.0f, b * b - 4.0f * a * c);
	const float entry =
		(-b - std::sqrt(discriminant)) / (2.0f * a);
	const float hit_time = std::clamp(entry, 0.0f, 1.0f);
	if (hit_fraction != nullptr)
	{
		*hit_fraction = hit_time;
	}
	impact = start + segment * hit_time;
	return true;
}

bool collision_model_hit(
	const WorldObject& target,
	const ObjectModelReference& model,
	const glm::vec3& world_start,
	const glm::vec3& world_end,
	glm::vec3& world_impact)
{
	if (model.collision == nullptr
		|| model.collision->nodes.empty()
		|| model.removed
		|| (model.runtime_flags & 0x0020u) != 0)
	{
		return false;
	}
	glm::vec3 model_start;
	glm::vec3 model_end;
	segment_to_model_space(
		target,
		model,
		world_start,
		world_end,
		model_start,
		model_end);
	if (!segment_intersects_aabb(
			model_start,
			model_end,
			model.bounds_min,
			model.bounds_max))
	{
		return false;
	}
	bool hit = false;
	float retained_fraction = 0.0f;
	traverse_collision_tree(
		*model.collision,
		model_start,
		model_end,
		[&](const assets::GameplayCollisionNode& leaf)
		{
			for (const std::uint32_t polygon_index : leaf.polygons)
			{
				if (polygon_index >= model.collision->polygons.size())
				{
					continue;
				}
				const assets::GameplayCollisionPolygon& polygon =
					model.collision->polygons[polygon_index];
				for (std::uint32_t triangle_index = 0;
					triangle_index < polygon.triangle_count;
					++triangle_index)
				{
					const std::uint32_t index =
						polygon.first_triangle + triangle_index;
					if (index >= model.collision->triangles.size())
					{
						continue;
					}
					float fraction = 0.0f;
					if (segment_intersects_triangle(
							model_start,
							model_end,
							model.collision->triangles[index],
							fraction))
					{
						// collision_leaf_intersect_polygons
						// (0x0049bae0) scans every referenced fan
						// triangle and retains the last accepted hit.
						retained_fraction = fraction;
						hit = true;
					}
				}
			}
		});
	if (hit)
	{
		world_impact =
			world_start
			+ (world_end - world_start) * retained_fraction;
	}
	return hit;
}

void synchronize_component_health(
	WorldObject& target,
	std::uint32_t model_index)
{
	if (model_index >= target.model_references.size())
	{
		return;
	}
	const ObjectModelReference& model =
		target.model_references[model_index];
	if (model.component_index < 0
		|| model.component_index >= target.component_count)
	{
		return;
	}
	target.components[model.component_index].health = model.health;
}

bool same_model_sibling_scope(
	const ObjectModelReference& left,
	const ObjectModelReference& right)
{
	return left.owner_scope == right.owner_scope
		&& left.parent_reference == right.parent_reference;
}

void apply_component_projectile_hit(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	const assets::GunStats& bullet,
	GunProjectile& projectile,
	std::uint8_t cause,
	WorldObject& target,
	std::uint16_t target_index,
	std::uint32_t hit_model_index,
	std::uint32_t simulation_tick)
{
	if (hit_model_index >= target.model_references.size()
		|| (target.runtime_flags & kObjectFlagSimulationSuspended) != 0
		|| cause == 2)
	{
		return;
	}
	std::uint32_t damage_model_index = hit_model_index;
	ObjectModelReference* model =
		&target.model_references[damage_model_index];
	ObjectModelReference* const event_source_model = model;
	if ((model->source_flags & 0x0004u) != 0)
	{
		return;
	}
	// GameObject_apply_component_damage receives the raw projectile owner
	// slot and forwards that object to both ShotAt publications.  The event
	// tail does not generation-check the retained projectile handle: a
	// projectile which outlives its shooter still publishes the slot's
	// current mission identity.
	const WorldObject* attacker =
		projectile.shooter_index < kMaxGameObjects
			? &world.objects[projectile.shooter_index]
			: nullptr;
	const auto publish_tail = [&]()
	{
		if (projectile.shooter_index == world.player.index
			&& world.smart_target_enabled
			&& target.allegiance_class == 1)
		{
			if (model->component_index >= 0
				&& model->component_index < target.component_count)
			{
				damage_select_target(
					world,
					target,
					model->component_index);
			}
			else if (world.selected_target.index != target_index
				|| world.selected_target.generation
					!= target.generation)
			{
				damage_select_target(world, target, -1);
			}
		}
		if (cause != 4)
		{
			world_emit_mission_event(
				world,
				WorldMissionEventType::shot_at,
				target,
				attacker,
				UINT8_MAX);
		}

		// GameObject_apply_component_damage resolves the event proxy after
		// group redirection. Selector +0x108 takes precedence; a zero
		// selector falls back to the +0xd4 part group. Only authored
		// component nodes (source bit 0x2) can publish the second event.
		ObjectModelReference* event_model = nullptr;
		for (ObjectModelReference& candidate
			: target.model_references)
		{
			const bool same_proxy =
				event_source_model->damage_group_selector != 0
					? candidate.damage_group_selector
						== event_source_model->damage_group_selector
					: candidate.part_group_id
						== event_source_model->part_group_id;
			if (same_model_sibling_scope(
					candidate, *event_source_model)
				&& same_proxy
				&& (candidate.source_flags & 0x0002u) != 0)
			{
				event_model = &candidate;
				break;
			}
		}
		if (event_model != nullptr
			&& event_model->component_index >= 0
			&& event_model->component_index <= UINT8_MAX)
		{
			world_emit_mission_event(
				world,
				WorldMissionEventType::shot_at,
				target,
				attacker,
				static_cast<std::uint8_t>(
					event_model->component_index));
		}
	};

	float damage = scale_damage_by_difficulty(
		world,
		mission,
		target,
		bullet.hull_damage,
		projectile.shooter_index);
	const bool local_friendly_hit =
		mission.network.role == mission::NetworkRole::offline
		&& projectile.shooter_index == world.player.index
		&& target.allegiance_class == 0
		&& (target.runtime_flags & kObjectFlagDestroyed) == 0
		&& (cause == 0 || cause == 5);
	const bool local_friendly_offender =
		mission.network.role == mission::NetworkRole::offline
		&& projectile.shooter_index == world.player.index
		&& target.allegiance_class == 0
		&& (cause == 0 || cause == 5);
	if (local_friendly_hit)
	{
		ai::friendly_fire_accumulate_damage(
			world,
			mission,
			target,
			damage,
			simulation_tick);
	}
	const bool damage_owned =
		mission.network.role == mission::NetworkRole::offline
		|| (projectile.shooter_index < kMaxGameObjects
			&& mission::network_local_owns_object(
				mission.network,
				projectile.shooter_index,
				mission.player_prefix_count,
				world.player.index));
	if (!damage_owned)
	{
		publish_tail();
		return;
	}

	if (model->part_group_id != 0)
	{
		// Grouped damage is redirected only on the machine which owns the
		// attacker. Network nonowners retain the initially hit model for
		// smart-target and event-proxy publication.
		for (std::uint32_t index = 0;
			index < target.model_references.size();
			++index)
		{
			ObjectModelReference& candidate =
				target.model_references[index];
			if (same_model_sibling_scope(candidate, *model)
				&& candidate.part_group_id == model->part_group_id
				&& candidate.maximum_health > 0.0f)
			{
				damage_model_index = index;
				model = &candidate;
				break;
			}
		}
	}
	const bool network_friendly_engine =
		mission.network.role != mission::NetworkRole::offline
		&& projectile.shooter_index == world.player.index
		&& target.allegiance_class == 0
		&& model->model_type == 5;
	// High-durability models always require a 500-point hit. Causes three
	// and four waive only the authored source-0x200 capability.
	if (model->maximum_health == 0.0f
		|| network_friendly_engine
		|| (model->maximum_health > 2499.0f
			&& (damage < 500.0f
				|| ((model->source_flags & 0x0200u) == 0
					&& cause != 3
					&& cause != 4))))
	{
		publish_tail();
		return;
	}
	if (target.protection_state != 5
		&& (target.runtime_flags & 0x00004000u) != 0
		&& damage < 1000.0f)
	{
		damage *= 0.25f;
	}
	const float previous_health = model->health;
	bool protected_health =
		target.protection_state == 2;
	std::uint16_t component_protection = 0;
	// Retail compares the attacker as a signed live index, so its -1
	// sentinel does not count as a non-player attacker.
	const bool attacker_is_non_player =
		projectile.shooter_index != UINT16_MAX
		&& projectile.shooter_index
			>= mission.player_prefix_count;
	protected_health =
		protected_health
		|| (target.protection_state == 1
			&& attacker_is_non_player);
	if (model->component_index >= 0
		&& model->component_index < target.component_count)
	{
		component_protection =
			target.components[
				model->component_index].protection_state;
		protected_health =
			protected_health
			|| component_protection == 2
			|| (component_protection == 1
				&& attacker_is_non_player);
	}
	const float updated_health = previous_health - damage;
	const bool protected_crossing =
		updated_health < 0.0f && protected_health;
	if (!protected_crossing)
	{
		model->health = updated_health;
		if (mission.network.role != mission::NetworkRole::offline
			&& model->health < 0.0f
			&& previous_health >= 0.0f
			&& !mission::network_local_owns_object(
				mission.network,
				target_index,
				mission.player_prefix_count,
				world.player.index))
		{
			model->health = 0.0f;
		}
		synchronize_component_health(
			target, damage_model_index);
	}
	target.last_attacker_index = projectile.shooter_index;
	const bool destroyed =
		previous_health >= 0.0f && model->health < 0.0f;
	const bool target_owned =
		mission.network.role == mission::NetworkRole::offline
		|| mission::network_local_owns_object(
			mission.network,
			target_index,
			mission.player_prefix_count,
			world.player.index);
	if (model->health < 0.0f)
	{
		if (target_owned)
		{
			target.component_destruction_pending = true;
		}
		if (local_friendly_offender)
		{
			if (WorldObject* player =
				world_resolve(world, world.player))
			{
				ai::friendly_fire_flag_offender(
					*player,
					mission,
					model->model_type == 1
						&& target.protection_state == 1);
			}
		}
	}
	if (mission.network.role != mission::NetworkRole::offline
		&& model->network_model_id != UINT32_MAX)
	{
		// GameObject_apply_component_damage publishes after group
		// redirection, protection handling, non-owner zero-clamping, the
		// latest-attacker store, and the destruction flag
		// (LANCER.EXE 0x004648e6..0x00464987). The adjusted damage is
		// retained even when a protected crossing left health unchanged.
		(void)mission::network_defer_component_damage(
			mission.network,
			target,
			*model,
			model->network_model_id,
			target_index,
			mission.player_prefix_count,
			world.player.index,
			damage);
	}
	if (!protected_crossing
		&& (destroyed
		|| runtime.last_component_log_target != target_index
		|| runtime.last_component_log_model != damage_model_index
		|| simulation_tick - runtime.last_component_log_tick
			>= kDamageLogInterval))
	{
		diagnostics::mission_log(
			"component damage target=%u mission=%u shooter=%u "
			"model=%u group=%u hp=%.1f/%.1f%s",
			static_cast<unsigned>(target_index),
			static_cast<unsigned>(target.mission_index),
			static_cast<unsigned>(projectile.shooter_index),
			static_cast<unsigned>(damage_model_index),
			static_cast<unsigned>(model->part_group_id),
			model->health,
			model->maximum_health,
			destroyed ? " destroyed" : "");
		runtime.last_component_log_tick = simulation_tick;
		runtime.last_component_log_target = target_index;
		runtime.last_component_log_model =
			static_cast<std::uint16_t>(damage_model_index);
	}
	publish_tail();
}

bool process_ordinary_collisions(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	const assets::GunStatsTable& gun_stats,
	const assets::ShipStatsTable& ship_stats,
	GunProjectile& projectile,
	std::uint32_t simulation_tick)
{
	if (projectile.type_index < 0
		|| static_cast<std::size_t>(projectile.type_index)
			>= assets::kGunStatsCount)
	{
		return false;
	}
	const float radius_inflation =
		projectile.type_index == 13
			? 1200.0f
			: projectile.type_index == 14 ? 3000.0f : 0.0f;
	for (std::uint8_t candidate_index = 0;
		candidate_index < projectile.candidate_count;)
	{
		const GunProjectile::CollisionCandidate candidate =
			projectile.candidates[candidate_index];
		if (candidate.object_index >= kMaxGameObjects)
		{
			++candidate_index;
			continue;
		}
		WorldObject& target = world.objects[candidate.object_index];
		if (!target.active
			|| target.type >= 0x100
			|| (target.runtime_flags & 0x00000004u) != 0
			|| (target.type == 0x45
				&& world.player_inside_type45_compound))
		{
			++candidate_index;
			continue;
		}

		glm::vec3 impact{0.0f};
		float projection = 0.0f;
		if (segment_sphere_hit(
				projectile.previous_position,
				projectile.position,
				target.position,
				target.radius + radius_inflation,
				impact,
				projection))
		{
			if (candidate.model_index >= 0)
			{
				std::uint32_t retained_model = UINT32_MAX;
				glm::vec3 retained_impact{0.0f};
				std::uint8_t group_end = candidate_index;
				while (group_end < projectile.candidate_count
					&& projectile.candidates[group_end].object_index
						== candidate.object_index)
				{
					const std::int16_t model_index =
						projectile.candidates[group_end].model_index;
					if (model_index >= 0
						&& static_cast<std::size_t>(model_index)
							< target.model_references.size())
					{
						glm::vec3 model_impact;
						if (collision_model_hit(
								target,
								target.model_references[
									model_index],
								projectile.previous_position,
								projectile.position,
								model_impact))
						{
							// gun_projectile_process_collisions
							// (0x00479b40) coalesces adjacent pairs
							// for one object and retains the last
							// model whose authored polygons hit.
							retained_model =
								static_cast<std::uint32_t>(
									model_index);
							retained_impact = model_impact;
						}
					}
					++group_end;
				}
				if (retained_model != UINT32_MAX)
				{
					if (candidate.object_index
							< mission::
								kDeathmatchScenarioPlayerCapacity
						&& projectile.shooter_index
							< mission::
								kDeathmatchScenarioPlayerCapacity)
					{
						mission::
							deathmatch_scenarios_player_weapon_hit(
								mission,
								world,
								candidate.object_index,
								projectile.shooter_index,
								simulation_tick);
					}
					const std::uint8_t impact_type =
						compound_impact_type(target.type);
					if (impact_type == 3)
					{
						shields_create_impact_effect(
							world,
							&target,
							retained_impact,
							glm::normalize(
								projectile.velocity),
							3,
							simulation_tick);
					}
					else
					{
						queue_impact_effect(
							world,
							retained_impact,
							glm::normalize(
								projectile.velocity),
							target.radius,
							impact_type,
							simulation_tick);
					}
					if (projectile.type_index >= 13)
					{
						shields_emit_sparks(
							world,
							static_cast<std::uint8_t>(
								projectile.type_index == 13
									? 0
									: 4),
							retained_impact,
							glm::normalize(
								projectile.velocity),
							30.0f,
							1.6f,
							10.0f,
							40,
							simulation_tick);
						queue_impact_effect(
							world,
							retained_impact,
							glm::normalize(projectile.velocity),
							5000.0f,
							static_cast<std::uint8_t>(
								projectile.type_index),
							simulation_tick);
						world_queue_sound_explicit(
							world,
							retained_impact,
							glm::normalize(projectile.velocity),
							projectile.velocity,
							11,
							3);
					}
					else
					{
						shields_emit_sparks(
							world,
							1,
							retained_impact,
							glm::normalize(
								projectile.velocity),
							20.0f,
							1.0f,
							10.0f,
							10,
							simulation_tick);
					}
					apply_component_projectile_hit(
						runtime,
						world,
						mission,
						gun_stats.records[
							projectile.type_index],
						projectile,
						0,
						target,
						candidate.object_index,
						retained_model,
						simulation_tick);
					if ((target.runtime_flags & 0x00000100u) != 0)
					{
						world_register_cloak_hit(
							target, retained_impact, simulation_tick);
						return true;
					}
					if (target.model_references[
							retained_model].forcefield)
					{
						shields_register_hit(
							world,
							target,
							static_cast<std::int16_t>(
								retained_model),
							retained_impact,
							4,
							simulation_tick);
					}
					return true;
				}
				candidate_index = group_end;
				continue;
			}
			const bool target_cloaked =
				(target.runtime_flags & 0x00000100u) != 0;
			if (candidate.object_index
					< mission::kDeathmatchScenarioPlayerCapacity
				&& projectile.shooter_index
					< mission::kDeathmatchScenarioPlayerCapacity)
			{
				mission::deathmatch_scenarios_player_weapon_hit(
					mission,
					world,
					candidate.object_index,
					projectile.shooter_index,
					simulation_tick);
			}
			const bool shield_impact = apply_ordinary_projectile_hit(
				runtime,
				world,
				mission,
					gun_stats.records[projectile.type_index],
				projectile,
				0,
				target,
				candidate.object_index,
				impact,
				ship_stats,
				simulation_tick);
			if (shield_impact && !target_cloaked)
			{
				shields_register_hit(
					world,
					target,
					-1,
					impact,
					2,
					simulation_tick);
			}
			return true;
		}
		if (projection == 0.0f)
		{
			// Once a missed target projects behind the sweep start,
			// 0x00479b40 packs the tail left and permanently narrows the
			// candidate list.
			std::move(
				projectile.candidates + candidate_index + 1,
				projectile.candidates + projectile.candidate_count,
				projectile.candidates + candidate_index);
			--projectile.candidate_count;
			continue;
		}
		++candidate_index;
	}
	return false;
}

float wrap_angle(float angle)
{
	while (angle > glm::pi<float>()) angle -= glm::two_pi<float>();
	while (angle < -glm::pi<float>()) angle += glm::two_pi<float>();
	return angle;
}

bool retained_target_valid(
	const WorldObject& target,
	std::int16_t component)
{
	if (!target.active
		|| !target.targetable
		|| (target.runtime_flags & 0x10000d40u) != 0)
	{
		return false;
	}
	if (component < 0)
	{
		return true;
	}
	return component < target.component_count
		&& (target.components[component].runtime_flags & 0x0030u) == 0;
}

bool target_point_and_radius(
	const WorldObject& target,
	std::int16_t component,
	bool validate_target,
	glm::vec3& point,
	float& radius)
{
	if ((validate_target && !retained_target_valid(target, component))
		|| (!validate_target
			&& component >= target.component_count))
	{
		return false;
	}
	// gun_projectile_intercept (LANCER.EXE 0x00401180) resolves the
	// retained target model and reads its live SR scene-node position.  The
	// target's velocity remains the current physics velocity, but the base
	// point is deliberately the same interpolated pose that is visible to
	// the articulated gun.
	point = target.scene_position;
	radius = target.radius;
	if (component < 0)
	{
		return true;
	}
	for (const ObjectModelReference& model : target.model_references)
	{
		if (model.component_index != component || model.removed)
		{
			continue;
		}
		point += target.scene_orientation
			* glm::vec3(model.scene_transform[3]);
		return true;
	}
	point += target.scene_orientation
		* target.components[component].local_position;
	return true;
}

bool angle_in_joint_limits(
	float angle,
	const ObjectModelReference& model,
	std::uint32_t axis)
{
	const float minimum = model.joint_min_degrees[axis];
	const float maximum = model.joint_max_degrees[axis];
	if (minimum == maximum)
	{
		return true;
	}
	const float degrees = glm::degrees(angle);
	return degrees >= minimum && degrees <= maximum;
}

bool clearance_cell(
	const GunMount& mount,
	std::uint32_t row,
	std::uint32_t column)
{
	const std::uint32_t bit =
		(row & 31u) * 16u + (column & 15u);
	return (mount.clearance_mask[bit >> 3]
		& (1u << (bit & 7u))) != 0;
}

bool aim_within_clearance(
	const WorldObject& owner,
	const GunMount& mount,
	const glm::vec3& target_point)
{
	if (!mount.has_clearance_mask
		|| mount.part_references[1] < 0
		|| mount.part_references[1]
			>= static_cast<std::int16_t>(
				owner.model_references.size()))
	{
		return true;
	}
	const ObjectModelReference& pitch =
		owner.model_references[mount.part_references[1]];
	const ObjectModelReference& yaw =
		owner.model_references[mount.part_references[0]];
	const glm::mat3 root_object_basis =
		yaw.object_space_joint_basis
			* glm::transpose(yaw.source_local_basis);
	const glm::mat3 root_world_basis =
		owner.scene_orientation * root_object_basis;
	const glm::vec3 pitch_origin = owner.scene_position
		+ owner.scene_orientation
			* glm::vec3(pitch.scene_transform[3]);
	const glm::vec3 local =
		glm::transpose(root_world_basis) * (target_point - pitch_origin);
	const float angle_a = std::atan2(local.x, local.z);
	const float sine = std::sin(angle_a);
	const float angle_b = std::atan2(local.x / sine, local.y);
	// gun_aim_within_limits_and_clearance_mask
	// (LANCER.EXE 0x0047cb10): round the baked angular coordinates,
	// wrap to the 32-by-16 table, then require the conservative 2x2
	// neighborhood.
	const std::int32_t row_value = static_cast<std::int32_t>(
		std::round(
			(angle_a + glm::two_pi<float>())
				* (32.0f / glm::two_pi<float>())
			- 1.0f));
	const std::int32_t column_value = static_cast<std::int32_t>(
		std::round(
			(angle_b + glm::two_pi<float>())
				* (64.0f / glm::two_pi<float>())
			- 1.0f));
	const std::uint32_t row =
		static_cast<std::uint32_t>(row_value) & 31u;
	const std::uint32_t column =
		static_cast<std::uint32_t>(-column_value) & 15u;
	return clearance_cell(mount, row, column)
		&& clearance_cell(mount, row + 1u, column)
		&& clearance_cell(mount, row, column + 1u)
		&& clearance_cell(mount, row + 1u, column + 1u);
}

void refresh_mount_emitter(
	WorldObject& object,
	GunMount& mount)
{
	if (mount.emitter_reference < 0
		|| mount.emitter_reference
			>= static_cast<std::int16_t>(
				object.model_references.size()))
	{
		return;
	}
	const glm::mat4 transform =
		object.model_references[
			mount.emitter_reference].local_transform
		* mount.emitter_from_part;
	mount.local_position = glm::vec3(transform[3]);
	mount.local_orientation = glm::mat3(transform);
}

bool track_kind_one_target(
	World& world,
	WorldObject& owner,
	GunMount& mount,
	const WorldObject& target,
	const assets::GunStatsTable& gun_stats,
	std::uint32_t simulation_tick,
	bool commit_tracking)
{
	if ((commit_tracking
			&& !retained_target_valid(
				target, mount.target_component))
		|| mount.bullet_type == 0
		|| mount.bullet_type > assets::kGunStatsCount
		|| mount.part_references[0] < 0
		|| mount.part_references[1] < 0
		|| mount.part_references[0]
			>= static_cast<std::int16_t>(
				owner.model_references.size())
		|| mount.part_references[1]
			>= static_cast<std::int16_t>(
				owner.model_references.size()))
	{
		return false;
	}
	const ObjectModelReference& yaw_model =
		owner.model_references[mount.part_references[0]];
	const ObjectModelReference& pitch_model =
		owner.model_references[mount.part_references[1]];
	glm::vec3 target_center;
	float target_radius = 0.0f;
	if (!target_point_and_radius(
			target,
			mount.target_component,
			commit_tracking,
			target_center,
			target_radius))
	{
		return false;
	}
	const glm::vec3 yaw_origin =
		owner.scene_position
		+ owner.scene_orientation
			* glm::vec3(yaw_model.scene_transform[3]);
	const assets::GunStats& bullet =
		gun_stats.records[mount.bullet_type - 1];
	if (!(bullet.speed > 0.0f))
	{
		return false;
	}
	const float travel_ticks =
		glm::distance(yaw_origin, target_center) / bullet.speed;
	const float lead_factor =
		(target.runtime_flags & 0x04000000u) == 0
			? 1.0f
			// Turret_track_target 0x0047cff1..0x0047d043 gives ECM
			// a random 0.5..0.8 velocity-lead scalar. The projectile
			// lifetime test inside 0x00401180 precedes that scalar.
			: 0.5f
				+ static_cast<float>(world_rand15(world))
					* (1.0f / 32767.0f) * 0.3f;
	const float lifetime = mount.bullet_type == 12
		? static_cast<float>(gun_stats.records[0].lifetime_ticks) * 3.0f
		: static_cast<float>(bullet.lifetime_ticks);
	if (travel_ticks > lifetime * 0.25f)
	{
		return false;
	}
	const glm::vec3 lead =
		target_center
		+ target.orientation[2]
			* target.speed * travel_ticks * lead_factor;
	const glm::vec3 world_aim = lead - yaw_origin;
	if (glm::dot(world_aim, world_aim) == 0.0f)
	{
		return false;
	}
	const glm::vec3 local =
		glm::transpose(yaw_model.object_space_joint_basis)
		* (glm::transpose(owner.scene_orientation) * world_aim);
	float yaw = wrap_angle(std::atan2(local.y, -local.z));
	const float temporary =
		std::cos(yaw) * local.z - std::sin(yaw) * local.y;
	float pitch = wrap_angle(-std::atan2(local.x, -temporary));
	if (!angle_in_joint_limits(yaw, yaw_model, 0))
	{
		return false;
	}
	if ((mount.bullet_type == 14 || mount.bullet_type == 15)
		&& pitch_model.joint_min_degrees.y
			!= pitch_model.joint_max_degrees.y)
	{
		const float degrees = glm::degrees(pitch);
		const float minimum = pitch_model.joint_min_degrees.y;
		const float maximum = pitch_model.joint_max_degrees.y;
		if (degrees > maximum && degrees < maximum + 20.0f)
		{
			pitch = glm::radians(maximum);
		}
		else if (degrees < minimum && degrees > minimum - 20.0f)
		{
			pitch = glm::radians(minimum);
		}
	}
	if (!angle_in_joint_limits(pitch, pitch_model, 1)
		|| !aim_within_clearance(owner, mount, lead))
	{
		return false;
	}
	// Turret_find_target, LANCER.EXE 0x0047d1f0..0x0047d3c4,
	// only proves that the candidate/component can be led and traversed by
	// the mount. Desired angles, firing requests, and model sequences are
	// first committed by Turret_track_target on the following frame.
	if (!commit_tracking)
	{
		return true;
	}
	mount.desired_yaw =
		wrap_angle(yaw - yaw_model.joint_euler_delta.x);
	mount.desired_pitch =
		wrap_angle(pitch - pitch_model.joint_euler_delta.y);

	glm::mat4 emitter_scene = yaw_model.scene_transform;
	if (mount.emitter_reference >= 0
		&& mount.emitter_reference
			< static_cast<std::int16_t>(
				owner.model_references.size()))
	{
		emitter_scene = owner.model_references[
			mount.emitter_reference].scene_transform
			* mount.emitter_from_part;
	}
	// Turret_target_ray_intersects_sphere, LANCER.EXE
	// 0x0047cf10, receives the live type-three gun child's composed
	// orientation but the yaw part's composed position. The predicted
	// intercept point passed to gun_aim_within_limits_and_clearance_mask is
	// also passed as the sphere center here; testing the target's present
	// center rejects correctly aimed shots whenever lateral lead exceeds the
	// hull radius.
	const glm::vec3 ray_origin = yaw_origin;
	const glm::vec3 ray_direction =
		owner.scene_orientation * glm::mat3(emitter_scene)[2];
	const glm::vec3 center_delta = lead - ray_origin;
	const float projection =
		glm::dot(center_delta, ray_direction);
	if (projection > 0.0f
		&& glm::dot(center_delta, center_delta)
			- projection * projection
				<= 4.0f * target_radius * target_radius)
	{
		mount.action_tick = simulation_tick + 1;
		for (const std::int16_t part : mount.part_references)
		{
			if (part < 0
				|| part >= static_cast<std::int16_t>(
					owner.model_references.size()))
			{
				continue;
			}
			ObjectModelReference& model =
				owner.model_references[
					static_cast<std::uint16_t>(part)];
			// Turret_track_target, LANCER.EXE
			// 0x0047d17f..0x0047d1b3, starts sequence slot one
			// only when the retained part is stopped. Restarting a live
			// sequence here every rendered update pins it at time zero,
			// so its type-zero projectile events can never be crossed.
			if (model.sequence_mode == 0
				|| model.sequence_rate == 0.0f)
			{
				model_animation_start_builtin(
					owner,
					static_cast<std::uint16_t>(part),
					BuiltinModelSequence::fire,
					0.0f,
					-1,
					2.0f);
			}
		}
	}
	return true;
}

void rotate_joint(
	WorldObject& object,
	std::int16_t reference,
	std::uint32_t axis,
	float step)
{
	if (reference < 0
		|| reference
			>= static_cast<std::int16_t>(
				object.model_references.size()))
	{
		return;
	}
	ObjectModelReference& model =
		object.model_references[reference];
	model.joint_euler_delta[axis] += step;
	const float minimum = model.joint_min_degrees[axis];
	const float maximum = model.joint_max_degrees[axis];
	if (minimum == maximum)
	{
		model.joint_euler_delta[axis] =
			wrap_angle(model.joint_euler_delta[axis]);
	}
	else
	{
		model.joint_euler_delta[axis] = std::clamp(
			model.joint_euler_delta[axis],
			glm::radians(minimum),
			glm::radians(maximum));
	}
	model_animation_recompute_pose(
		object, static_cast<std::uint16_t>(reference));
}

void service_kind_two_animation(
	WorldObject& owner,
	GunMount& mount,
	std::uint32_t simulation_tick,
	std::uint32_t frame_ticks)
{
	const std::int16_t driver_reference = mount.part_references[0];
	if (driver_reference < 0
		|| driver_reference >= static_cast<std::int16_t>(
			owner.model_references.size()))
	{
		return;
	}
	ObjectModelReference& driver =
		owner.model_references[driver_reference];
	if (driver.sequence_mode == 0)
	{
		// Gun_kind_two_update, LANCER.EXE 0x0047c9ca..0x0047c9e3,
		// selects the compiled "fire" sequence slot, looping from time zero
		// with an initially stationary playback rate.
		model_animation_start_builtin(
			owner,
			static_cast<std::uint16_t>(driver_reference),
			BuiltinModelSequence::fire,
			0.0f,
			2,
			0.0f);
	}
	const bool deploy = mount.action_tick >= simulation_tick;
	const float step = static_cast<float>(frame_ticks)
		* (deploy
			? 0.10000000149011612f
			: 0.019999999552965164f);
	const float playback_rate = std::clamp(
		driver.sequence_rate + (deploy ? step : -step),
		0.0f,
		4.0f);
	model_animation_set_playback(
		owner,
		static_cast<std::uint16_t>(driver_reference),
		2,
		playback_rate);

	const std::int16_t synchronized_reference =
		mount.part_references[1];
	if (synchronized_reference >= 0
		&& synchronized_reference < static_cast<std::int16_t>(
			owner.model_references.size()))
	{
		if (deploy)
		{
			model_animation_set_playback(
				owner,
				static_cast<std::uint16_t>(synchronized_reference),
				2,
				playback_rate);
		}
		else
		{
			model_animation_start_builtin(
				owner,
				static_cast<std::uint16_t>(synchronized_reference),
				BuiltinModelSequence::fire,
				0.0f,
				0,
				4.0f);
		}
	}
	for (std::uint32_t slot = 2; slot < 4; ++slot)
	{
		const std::int16_t reference = mount.part_references[slot];
		if (reference < 0
			|| reference >= static_cast<std::int16_t>(
				owner.model_references.size()))
		{
			continue;
		}
		model_animation_set_playback(
			owner,
			static_cast<std::uint16_t>(reference),
			1,
			deploy ? 4.0f : -4.0f);
	}
}

ObjectHandle object_handle_for(
	const World& world,
	const WorldObject& object)
{
	const std::uint16_t index = static_cast<std::uint16_t>(
		&object - std::begin(world.objects));
	return {index, object.generation};
}

bool kind_three_mount_transform(
	const WorldObject& owner,
	const GunMount& mount,
	glm::vec3& origin,
	glm::mat3& basis)
{
	const std::int16_t reference = mount.part_references[1];
	if (reference < 0
		|| reference >= static_cast<std::int16_t>(
			owner.model_references.size()))
	{
		return false;
	}
	// Gun_kind_three_update reads the retained SR node's live position and
	// orientation, not the fixed-service physics pose.
	const glm::mat4& transform =
		owner.model_references[reference].scene_transform;
	origin =
		owner.scene_position
		+ owner.scene_orientation * glm::vec3(transform[3]);
	basis = owner.scene_orientation * glm::mat3(transform);
	return true;
}

bool kind_three_target_geometry(
	const WorldObject& owner,
	const GunMount& mount,
	const WorldObject& target,
	float maximum_distance,
	glm::vec3& local,
	float& distance)
{
	glm::vec3 origin;
	glm::mat3 basis;
	if (!kind_three_mount_transform(owner, mount, origin, basis))
	{
		return false;
	}
	const glm::vec3 delta = target.position - origin;
	distance = glm::length(delta);
	if (!(distance > 0.0f) || distance >= maximum_distance)
	{
		return false;
	}
	local = glm::transpose(basis) * delta;
	// Gun_kind_three_update 0x0047d665..0x0047d68f gates the
	// non-traversing local axis only. The forward cosine is a ranking value,
	// not a 0.7 acquisition-cone test.
	return std::abs(local.y)
		<= distance * 0.699999988079071f;
}

bool kind_three_acquisition_solution(
	const WorldObject& owner,
	const GunMount& mount,
	const WorldObject& target,
	float& score)
{
	// Gun_kind_three_update, LANCER.EXE 0x0047d5f1..0x0047d6b0.
	// Unlike kind-one target search, this path does not reject neutral or
	// cloaked objects; it requires the ordinary targetable bit, rejects the
	// exact 0x462 mask, and excludes only the owner's allegiance class.
	glm::vec3 local{0.0f};
	float distance = 0.0f;
	if (!target.active
		|| !target.targetable
		|| target.allegiance_class == owner.allegiance_class
		|| (target.runtime_flags & 0x00000462u) != 0
		|| !kind_three_target_geometry(
			owner, mount, target, 50000.0f, local, distance))
	{
		return false;
	}
	score = local.z / distance;
	return true;
}

void service_kind_three_autoturret(
	WeaponRuntime& runtime,
	World& world,
	MissileRuntime& missiles,
	const assets::MissileStatsTable& missile_stats,
	const assets::ShipStatsTable& ship_stats,
	WorldObject& owner,
	GunMount& mount,
	std::uint32_t simulation_tick,
	bool network_active,
	std::uint16_t mission_number)
{
	(void)runtime;
	(void)ship_stats;
	switch (mount.autonomous_state)
	{
	case 0:
	{
		if (mount.autonomous_rounds == 0)
		{
			mount.autonomous_state = 2;
			mount.autonomous_deadline = simulation_tick + 100;
			return;
		}
		if (simulation_tick < mount.autonomous_deadline)
		{
			return;
		}
		mount.autonomous_target = UINT16_MAX;
		float best_score = -1.0f;
		for (std::uint16_t index = 0;
			index < kMaxGameObjects;
			++index)
		{
			const WorldObject& candidate = world.objects[index];
			if (&candidate == &owner
				|| (network_active
					&& index == owner.excluded_interaction_index))
			{
				continue;
			}
			float score = 0.0f;
			if (kind_three_acquisition_solution(
					owner, mount, candidate, score)
				&& score > best_score)
			{
				best_score = score;
				mount.autonomous_target = index;
			}
		}
		if (mount.autonomous_target != UINT16_MAX)
		{
			mount.autonomous_state = 1;
		}
		return;
	}
	case 1:
	{
		if (mount.autonomous_rounds == 0)
		{
			mount.autonomous_state = 2;
			mount.autonomous_deadline = simulation_tick + 100;
			return;
		}
		if (mount.autonomous_target >= kMaxGameObjects)
		{
			mount.autonomous_state = 0;
			mount.autonomous_deadline = simulation_tick + 20;
			return;
		}
		WorldObject& target =
			world.objects[mount.autonomous_target];
		glm::vec3 local{0.0f};
		float distance = 0.0f;
		if (!retained_target_valid(target, -1)
			|| !kind_three_target_geometry(
				owner, mount, target, 25000.0f, local, distance))
		{
			mount.autonomous_state = 0;
			mount.autonomous_target = UINT16_MAX;
			mount.autonomous_deadline = simulation_tick + 20;
			return;
		}
		// Gun_kind_three_update applies fixed 0.1-radian steps outside its
		// dead band. It does not clamp an atan2-derived angle.
		if (local.x < distance * -0.10000000149011612f)
		{
			rotate_joint(
				owner, mount.part_references[0], 1, -0.1f);
		}
		if (local.x > distance * 0.10000000149011612f)
		{
			rotate_joint(
				owner, mount.part_references[0], 1, 0.1f);
		}
		refresh_mount_emitter(owner, mount);
		// The same distance*0.7 temporary used by the non-traversing-axis
		// gate is retained as the firing cone. Equality does not fire.
		if (local.z <= distance * 0.699999988079071f
			|| simulation_tick <= mount.autonomous_deadline)
		{
			return;
		}
		if (static_cast<float>(world_rand15(world))
				* (1.0f / 32767.0f)
			< 0.20000000298023224f)
		{
			const ObjectHandle target_handle =
				object_handle_for(world, target);
			glm::vec3 origin;
			glm::mat3 basis;
			if (!kind_three_mount_transform(
					owner, mount, origin, basis))
			{
				return;
			}
			missiles_launch_type0_from_transform(
				missiles,
				world,
				missile_stats,
				object_handle_for(world, owner),
				origin,
				basis,
				target_handle,
				-1,
				simulation_tick);
			if (mount.autonomous_rounds != 0)
			{
				--mount.autonomous_rounds;
			}
		}
		mount.autonomous_deadline =
			simulation_tick
			+ (mission_number == 28 ? 1000u : 2000u);
		return;
	}
	case 2:
		if (simulation_tick > mount.autonomous_deadline)
		{
			mount.autonomous_state = 3;
			mount.autonomous_deadline = simulation_tick + 800;
			if (mount.part_references[1] >= 0)
			{
				model_animation_start_named(
					owner,
					static_cast<std::uint16_t>(
						mount.part_references[1]),
					"reload",
					0.0f,
					1,
					4.0f);
			}
		}
		return;
	case 3:
		if (simulation_tick > mount.autonomous_deadline)
		{
			mount.autonomous_state = 4;
			mount.autonomous_deadline = simulation_tick + 300;
			if (mount.part_references[1] >= 0)
			{
				model_animation_start_named(
					owner,
					static_cast<std::uint16_t>(
						mount.part_references[1]),
					"reload",
					-1.0f,
					1,
					-4.0f);
			}
		}
		return;
	case 4:
		if (simulation_tick > mount.autonomous_deadline)
		{
			mount.autonomous_rounds = 6;
			mount.autonomous_state = 0;
		}
		return;
	default:
		mount.autonomous_state = 0;
		return;
	}
}
}

void weapons_reset(WeaponRuntime& runtime)
{
	runtime = {};
	for (GunProjectile& projectile : runtime.projectiles)
	{
		projectile.type_index = -1;
		projectile.expiration_tick = -1;
	}
}

bool weapons_scene_segment_hit(
	const WorldObject& target,
	const glm::vec3& world_start,
	const glm::vec3& world_end,
	float& fraction)
{
	bool hit = false;
	float nearest = 1.0f;
	for (const ObjectModelReference& model : target.model_references)
	{
		if (model.collision == nullptr
			|| model.collision->nodes.empty()
			|| model.removed
			|| (model.runtime_flags & 0x0020u) != 0)
		{
			continue;
		}
		const glm::mat3 model_basis =
			target.scene_orientation * glm::mat3(model.scene_transform);
		const glm::vec3 model_center =
			target.scene_position
				+ target.scene_orientation
					* glm::vec3(model.scene_transform[3]);
		const glm::mat3 inverse_basis = glm::transpose(model_basis);
		const glm::vec3 model_start =
			inverse_basis * (world_start - model_center);
		const glm::vec3 model_end =
			inverse_basis * (world_end - model_center);
		if (!segment_intersects_aabb(
				model_start,
				model_end,
				model.bounds_min,
				model.bounds_max))
		{
			continue;
		}
		traverse_collision_tree(
			*model.collision,
			model_start,
			model_end,
			[&](const assets::GameplayCollisionNode& leaf)
			{
				for (const std::uint32_t polygon_index : leaf.polygons)
				{
					if (polygon_index >= model.collision->polygons.size())
					{
						continue;
					}
					const assets::GameplayCollisionPolygon& polygon =
						model.collision->polygons[polygon_index];
					for (std::uint32_t triangle_index = 0;
						triangle_index < polygon.triangle_count;
						++triangle_index)
					{
						const std::uint32_t index =
							polygon.first_triangle + triangle_index;
						if (index >= model.collision->triangles.size())
						{
							continue;
						}
						float candidate = 0.0f;
						if (segment_intersects_triangle(
								model_start,
								model_end,
								model.collision->triangles[index],
								candidate)
							&& candidate < nearest)
						{
							nearest = candidate;
							hit = true;
						}
					}
				}
			});
	}
	if (hit)
	{
		fraction = nearest;
	}
	return hit;
}

bool weapons_pop_sound(
	WeaponRuntime& runtime,
	WeaponSoundEvent& event)
{
	if (runtime.sound_count == 0)
	{
		return false;
	}
	event = runtime.sound_events[runtime.sound_read];
	runtime.sound_read = static_cast<std::uint8_t>(
		(runtime.sound_read + 1) % std::size(runtime.sound_events));
	--runtime.sound_count;
	return true;
}

bool weapons_fire_charged_nova(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	WorldObject& object,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick)
{
	if (!object.active || guns_suppressed(object)
		|| object.nova_charge == 0.0f)
	{
		return false;
	}
	NovaBeam* slot = nullptr;
	for (NovaBeam& beam : runtime.nova_beams)
	{
		if (!beam.active || simulation_tick >= beam.expiration_tick)
		{
			slot = &beam;
			break;
		}
	}
	// nova_cannon_fire_charged_beam leaves the owner's charge untouched
	// when all eight retained slots are occupied.
	if (slot == nullptr)
	{
		return false;
	}
	const float charge = object.nova_charge;
	if (charge < 0.5f)
	{
		object.nova_charge = 0.0f;
		return false;
	}
	const std::uint16_t owner_index = static_cast<std::uint16_t>(
		&object - std::begin(world.objects));
	const glm::vec3 start = object.position;
	const glm::vec3 end =
		start + object.orientation[2] * 40000.0f;
	*slot = {
		start,
		end,
		object.orientation,
		simulation_tick + 80,
		owner_index,
		charge,
		true,
		charge == 1.0f,
	};

	GunProjectile cause;
	cause.type_index = 10;
	cause.shooter_index = owner_index;
	cause.shooter_generation = object.generation;
	cause.shooter_affiliation = object.allegiance_class;
	assets::GunStats component_damage;
	component_damage.hull_damage = charge * 4.0f;
	if (owner_index == world.player.index)
	{
		// nova_cannon_fire_charged_beam (0x0047b3d0) performs its
		// collision/damage traversal only for the local player object.
		// Network receive calls still construct the retained beam below,
		// but never mutate targets on the receiving peer.
		const float primary_damage =
			object.shields_health * charge
			* (mission.network.role != mission::NetworkRole::offline
				? 0.25f
				: 1.0f);
		for (std::uint16_t target_index = 0;
			target_index < kMaxGameObjects;
			++target_index)
		{
			WorldObject& target = world.objects[target_index];
			if (!target.active || &target == &object
				|| target.type >= 0x100u
				|| (target.runtime_flags & 0x00000420u) != 0)
			{
				continue;
			}
			glm::vec3 impact;
			float projection = 0.0f;
			if (!segment_sphere_hit(
					start,
					end,
					target.position,
					target.radius,
					impact,
					projection))
			{
				continue;
			}
			bool component_hit = false;
			if ((target.runtime_flags & kObjectFlagCompound) != 0)
			{
				for (std::uint32_t model_index = 0;
					model_index < target.model_references.size();
					++model_index)
				{
					glm::vec3 model_impact;
					if (!collision_model_hit(
							target,
							target.model_references[model_index],
							start,
							end,
							model_impact))
					{
						continue;
					}
					apply_component_projectile_hit(
						runtime,
						world,
						mission,
						component_damage,
						cause,
						0,
						target,
						target_index,
						model_index,
						simulation_tick);
					const bool target_cloaked =
						(target.runtime_flags & 0x00000100u) != 0;
					if (target_cloaked)
					{
						world_register_cloak_hit(
							target, model_impact, simulation_tick);
					}
					const std::uint8_t impact_type =
						compound_impact_type(target.type);
					if (!target_cloaked
						&& target.model_references[
							model_index].forcefield)
					{
						shields_register_hit(
							world,
							target,
							static_cast<std::int16_t>(
								model_index),
							model_impact,
							4,
							simulation_tick);
					}
					if (impact_type == 3)
					{
						shields_create_impact_effect(
							world,
							&target,
							model_impact,
							object.orientation[2],
							3,
							simulation_tick);
					}
					else
					{
						queue_impact_effect(
							world,
							model_impact,
							object.orientation[2],
							target.radius,
							impact_type,
							simulation_tick);
					}
					component_hit = true;
				}
			}
			if (!component_hit)
			{
				// The ordinary Nova owner calls the primary-bank owner
				// directly.  It neither performs the gun/missile exposed
				// hull sweep nor consumes either local auxiliary pool.
				const std::uint8_t bank =
					shield_bank_for_impact(target, impact);
				(void)apply_primary_bank_damage(
					world,
					mission,
					target,
					ship_stats,
					bank,
					primary_damage,
					4.0f,
					owner_index,
					0,
					runtime.feedback_enabled,
					simulation_tick);
				if ((target.runtime_flags & 0x00000100u) != 0)
				{
					world_register_cloak_hit(
						target, impact, simulation_tick);
				}
				else
				{
					shields_register_hit(
						world,
						target,
						-1,
						impact,
						2,
						simulation_tick);
				}
			}
		}
	}
	// nova_cannon_fire_charged_beam (0x0047b3d0) publishes the network
	// beam and local force-feedback response but does not create a
	// Sound3D sample; ordinary projectile sound ownership is not reused.
	object.nova_charge = 0.0f;
	++runtime.shot_serial;
	return true;
}

void weapons_fire_model_animation_event(
	WeaponRuntime& runtime,
	World& world,
	WorldObject& object,
	std::uint16_t source_model_reference,
	const assets::GunStatsTable& gun_stats,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick)
{
	if (!gun_stats.ready
		|| source_model_reference >= object.model_references.size())
	{
		return;
	}
	const ObjectModelReference& source =
		object.model_references[source_model_reference];
	for (std::uint16_t ordinal = 0;; ++ordinal)
	{
		const assets::GameplayLocator* locator =
			model_animation_find_locator(
				object, source_model_reference, 3, ordinal);
		if (locator == nullptr)
		{
			break;
		}
		const std::uint32_t authored_type = locator->exporter_id;
		const std::uint8_t bullet_type = static_cast<std::uint8_t>(
			authored_type == 0 ? 1 : authored_type);
		if (bullet_type > assets::kGunStatsCount)
		{
			continue;
		}
		// object_model_sequence_event type zero, LANCER.EXE
		// 0x0047c7b0, fires every direct type-four runtime child. Those
		// children are the source node's type-three SHP locators; they are
		// not one logical GunMount. This is significant for capital guns:
		// Allied_Cap_Gun has two simultaneous muzzle children.
		const glm::mat4 emitter = source.local_transform
			* glm::translate(glm::mat4{1.0f}, locator->position)
			* glm::mat4(locator->basis);
		GunMount event_mount;
		event_mount.local_position = glm::vec3(emitter[3]);
		event_mount.local_orientation = glm::mat3(emitter);
		spawn_projectile(
			runtime,
			world,
			object,
			event_mount,
			gun_stats,
			ship_stats,
			bullet_type,
			simulation_tick);
	}
}

std::uint8_t weapons_selected_gun_mounts(
	const WorldObject& object,
	std::uint8_t* output,
	std::uint8_t capacity)
{
	if (output == nullptr || capacity == 0)
	{
		return 0;
	}
	std::uint8_t count = 0;
	if ((object.active_weapon_selection_bits & 0x0010u) != 0)
	{
		for (std::uint8_t mount = 0;
			mount < object.gun_mount_count && count < capacity;
			++mount)
		{
			output[count++] = mount;
		}
		return count;
	}
	if (object.gun_pair_count == 0)
	{
		return 0;
	}
	const std::uint8_t pair_index =
		static_cast<std::uint8_t>(
			(object.active_weapon_selection_bits & 0x0007u)
				% object.gun_pair_count);
	const GunPair& pair = object.gun_pairs[pair_index];
	if (pair.first >= 0
		&& static_cast<std::uint8_t>(pair.first)
			< object.gun_mount_count)
	{
		output[count++] = static_cast<std::uint8_t>(pair.first);
	}
	if (count < capacity
		&& pair.second >= 0
		&& static_cast<std::uint8_t>(pair.second)
			< object.gun_mount_count)
	{
		output[count++] = static_cast<std::uint8_t>(pair.second);
	}
	return count;
}

bool weapons_compute_lead_point(
	const WorldObject& actor,
	const WorldObject& target,
	std::int16_t target_component,
	const assets::GunStatsTable& gun_stats,
	float lead_factor,
	glm::vec3& point)
{
	if (!gun_stats.ready)
	{
		return false;
	}

	// AI_compute_best_weapon_lead_point, LANCER.EXE 0x00401280. Native
	// GunMount+0 is the zero-based projectile definition, represented by
	// this port's one-based bullet_type. The fastest selected projectile
	// wins; an empty/invalid bank deliberately retains definition zero.
	std::uint8_t selected[20];
	const std::uint8_t selected_count = weapons_selected_gun_mounts(
		actor,
		selected,
		static_cast<std::uint8_t>(std::size(selected)));
	std::uint8_t bullet_index = 0;
	float fastest = -1.0f;
	for (std::uint8_t ordinal = 0; ordinal < selected_count; ++ordinal)
	{
		const GunMount& mount = actor.gun_mounts[selected[ordinal]];
		if (mount.bullet_type == 0
			|| mount.bullet_type > assets::kGunStatsCount)
		{
			continue;
		}
		const std::uint8_t candidate_index = mount.bullet_type - 1;
		const float candidate_speed =
			gun_stats.records[candidate_index].speed;
		if (candidate_speed > fastest)
		{
			fastest = candidate_speed;
			bullet_index = candidate_index;
		}
	}

	point = target.position;
	if (target_component >= 0
		&& target_component < target.component_count)
	{
		bool resolved_model = false;
		for (const ObjectModelReference& model : target.model_references)
		{
			if (model.component_index == target_component
				&& !model.removed)
			{
				point += target.orientation
					* glm::vec3(model.local_transform[3]);
				resolved_model = true;
				break;
			}
		}
		if (!resolved_model)
		{
			point += target.orientation
				* target.components[target_component].local_position;
		}
	}

	// AI_compute_weapon_lead_point, LANCER.EXE 0x00401180.
	const assets::GunStats& bullet = gun_stats.records[bullet_index];
	const float travel_ticks =
		glm::distance(point, actor.position) / bullet.speed;
	const float lifetime = bullet_index == 11
		? static_cast<float>(gun_stats.records[0].lifetime_ticks) * 3.0f
		: static_cast<float>(bullet.lifetime_ticks);
	if (travel_ticks > lifetime * 0.25f)
	{
		return false;
	}
	point += target.orientation[2]
		* target.speed
		* travel_ticks
		* lead_factor;
	return true;
}

void weapons_apply_gun_cooldown(
	World& world,
	WorldObject& object,
	std::uint32_t simulation_tick,
	std::uint32_t delay)
{
	// ship_apply_gun_cooldown, LANCER.EXE 0x0047b1f0.
	if (guns_suppressed(object) || object.gun_mount_count == 0)
	{
		return;
	}
	std::uint8_t selected[20];
	const std::uint8_t selected_count =
		weapons_selected_gun_mounts(
			object,
			selected,
			static_cast<std::uint8_t>(std::size(selected)));
	const bool all_guns =
		(object.active_weapon_selection_bits & 0x0010u) != 0;
	if ((object.type == 11 || object.type == 255)
		&& !all_guns
		&& selected_count != 0
		&& object.gun_mounts[selected[0]].bullet_type == 11)
	{
		const bool player =
			world_resolve(world, world.player) == &object;
		const float charge =
			object.nova_charge
			+ object.gun_recharge_scale
				* 0.0024999999441206455f;
		object.nova_charge = charge;
		if (charge > 1.0f)
		{
			object.nova_charge = 1.0f;
			if (player)
			{
				// Publish through the shared camera/HUD disturbance scalar.
				world.player_camera_disturbance = 0.6f;
			}
		}
		else if (player && charge > 0.5f)
		{
			world.player_camera_disturbance = 0.3f;
		}
	}
	for (std::uint8_t ordinal = 0;
		ordinal < selected_count;
		++ordinal)
	{
		GunMount& mount = object.gun_mounts[selected[ordinal]];
		if (mount.bullet_type == 11
			|| (all_guns
				&& (mount.mount_kind == 1 || mount.mount_kind < 0)))
		{
			continue;
		}
		mount.action_tick = simulation_tick + delay;
	}
}

void weapons_service_simple_guns(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	const assets::GunStatsTable& gun_stats,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick)
{
	if (!gun_stats.ready)
	{
		return;
	}
	// GameObject_update_simple_guns (0x004770e0) executes in the 25 Hz
	// object service pass. Preserve its asymmetric prepass: equality still
	// contributes to the volley cost, while the firing pass is strict.
	for (WorldObject& object : world.objects)
	{
		if (!object.active
			|| (object.runtime_flags & 0x00200422u) != 0
			|| guns_suppressed(object))
		{
			continue;
		}
		std::int32_t volley_cost = 0;
		for (std::uint8_t index = 0;
			index < object.gun_mount_count;
			++index)
		{
			const GunMount& mount = object.gun_mounts[index];
			if (!mount_eligible(mount, gun_stats)
				|| mount.action_tick < simulation_tick)
			{
				continue;
			}
			const std::uint8_t bullet_index = mount.bullet_type - 1;
			if (mount.mount_kind == 0
				&& kBulletBehavior[bullet_index] == 0
				&& mount.next_fire_tick <= simulation_tick)
			{
				volley_cost +=
					gun_stats.records[bullet_index].power_cost;
			}
		}
		const bool have_energy = volley_cost < object.gun_energy;
		bool alternating_mount_used = false;
		for (std::uint8_t index = 0;
			index < object.gun_mount_count;
			++index)
		{
			GunMount& mount = object.gun_mounts[index];
			if (!mount_eligible(mount, gun_stats)
				|| simulation_tick >= mount.action_tick
				|| simulation_tick < mount.next_fire_tick)
			{
				continue;
			}
			const std::uint8_t bullet_index = mount.bullet_type - 1;
			const assets::GunStats& bullet =
				gun_stats.records[bullet_index];
			const std::uint32_t cooldown =
				object.blindfire_active
					? static_cast<std::uint32_t>(
						std::max(
							0,
							bullet.cooldown_ticks * 135 / 100))
					: static_cast<std::uint32_t>(
						std::max(0, bullet.cooldown_ticks));
			if (mount.mount_kind == 2)
			{
				mount.next_fire_tick = simulation_tick + cooldown;
				if (object.ammunition > 0)
				{
					spawn_projectile(
						runtime,
						world,
						object,
						mount,
						gun_stats,
						ship_stats,
						mount.bullet_type,
						simulation_tick);
					--object.ammunition;
				}
				continue;
			}
			bool alternating_mount_selected = true;
			bool alternating_pair = false;
			if ((object.active_weapon_selection_bits & 0x0030u) == 0
				&& object.gun_pair_count != 0)
			{
				const std::uint8_t pair_index =
					static_cast<std::uint8_t>(
						(object.active_weapon_selection_bits & 0x0007u)
							% object.gun_pair_count);
				if (object.gun_pairs[pair_index].second >= 0)
				{
					alternating_pair = true;
					alternating_mount_selected =
						mount.alternating_side
							== object.alternating_gun_side;
				}
			}
			const bool probability_pass =
				object.shields_health >= 0.9f
				|| static_cast<float>(world_rand15(world))
						* (1.0f / 32767.0f)
					<= object.shields_health + 0.1f;
			bool fired = false;
			if (alternating_mount_selected
				&& probability_pass
				&& kBulletBehavior[bullet_index] == 0
				&& have_energy)
			{
				if (alternating_pair)
				{
					alternating_mount_used = true;
				}
				spawn_projectile(
					runtime,
					world,
					object,
					mount,
					gun_stats,
					ship_stats,
					mount.bullet_type,
					simulation_tick);
				object.gun_energy -= bullet.power_cost;
				fired = true;
			}
			else if (alternating_mount_selected
				&& probability_pass
				&& kBulletBehavior[bullet_index] == 1
				&& object.ammunition > 0)
			{
				if (alternating_pair)
				{
					alternating_mount_used = true;
				}
				spawn_projectile(
					runtime,
					world,
					object,
					mount,
					gun_stats,
					ship_stats,
					mount.bullet_type,
					simulation_tick);
				--object.ammunition;
				fired = true;
			}
			// Retail advances the cooldown for every attempted kind-zero
			// cycle, even when energy or the damage probability blocks it.
			mount.next_fire_tick = simulation_tick + cooldown;
			(void)fired;
		}
		if (alternating_mount_used)
		{
			object.alternating_gun_side ^= 1u;
		}
	}
	(void)mission;
}

void weapons_service_articulated_guns(
	WeaponRuntime& runtime,
	World& world,
	MissileRuntime& missiles,
	const assets::GunStatsTable& gun_stats,
	const assets::MissileStatsTable& missile_stats,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick,
	std::uint32_t frame_ticks,
	bool network_active,
	std::uint16_t mission_number)
{
	if (!gun_stats.ready)
	{
		return;
	}
	const float maximum_step =
		static_cast<float>(frame_ticks) * 0.02f;
	for (std::uint16_t owner_index = 0;
		owner_index < kMaxGameObjects;
		++owner_index)
	{
		WorldObject& owner = world.objects[owner_index];
		if (!owner.active
			|| (owner.runtime_flags & 0x00000440u) != 0
			|| guns_suppressed(owner)
			|| (owner.attachment_count != 0
				&& owner.attachments[0].kind == 0x6d))
		{
			continue;
		}
		for (std::uint8_t mount_index = 0;
			mount_index < owner.gun_mount_count;
			++mount_index)
		{
			GunMount& mount = owner.gun_mounts[mount_index];
			if (mount.mount_kind == 2)
			{
				service_kind_two_animation(
					owner,
					mount,
					simulation_tick,
					frame_ticks);
				continue;
			}
			if (mount.mount_kind == 3)
			{
				refresh_mount_emitter(owner, mount);
				service_kind_three_autoturret(
					runtime,
					world,
					missiles,
					missile_stats,
					ship_stats,
					owner,
					mount,
					simulation_tick,
					network_active,
					mission_number);
				continue;
			}
			if (mount.mount_kind != 1)
			{
				continue;
			}
			refresh_mount_emitter(owner, mount);
			const bool tracked_at_frame_start =
				mount.target_object < kMaxGameObjects;
			if (mount.target_object < kMaxGameObjects)
			{
				WorldObject& target =
					world.objects[mount.target_object];
				if (!track_kind_one_target(
					world,
					owner,
					mount,
					target,
					gun_stats,
					simulation_tick,
					true))
				{
					mount.target_object = UINT16_MAX;
					mount.target_component = -1;
				}
			}
			// Gun_kind_one_update (LANCER.EXE 0x0047d3d0) enters the
			// slew block according to the target state at callback entry.
			// A tracker failure can clear the target inside that block, but
			// retail still applies exactly this one final angular step.  It
			// never continues consuming stale desired angles on later
			// targetless frames.
			if (tracked_at_frame_start)
			{
				const float yaw_step = std::clamp(
					mount.desired_yaw,
					-maximum_step,
					maximum_step);
				const float pitch_step = std::clamp(
					mount.desired_pitch,
					-maximum_step,
					maximum_step);
				rotate_joint(
					owner, mount.part_references[0], 0, yaw_step);
				rotate_joint(
					owner, mount.part_references[1], 1, pitch_step);
				if (mount.part_references[2]
					!= mount.part_references[1])
				{
					rotate_joint(
						owner,
						mount.part_references[2],
						1,
						pitch_step);
				}
				refresh_mount_emitter(owner, mount);
			}
			if (simulation_tick > mount.target_search_deadline)
			{
				if (mount.target_object == UINT16_MAX)
				{
					const bool capital_projectile =
						mount.bullet_type == 14
						|| mount.bullet_type == 15;
					mount.desired_yaw = 0.0f;
					mount.desired_pitch = 0.0f;
					for (std::uint16_t candidate_index = 0;
						candidate_index < kMaxGameObjects;
						++candidate_index)
					{
						WorldObject& candidate =
							world.objects[candidate_index];
						if (candidate_index == owner_index
							|| !candidate.active
							|| candidate.type >= 0x100u
							|| candidate.allegiance_class
								== owner.allegiance_class
							|| candidate.allegiance_class == 2
							|| (network_active
								&& candidate_index
									== owner.excluded_interaction_index)
							|| (capital_projectile
								&& (candidate.runtime_flags
									& kObjectFlagCompound) == 0))
						{
							continue;
						}
						bool selected = false;
						const bool component_targeting_owner =
							(owner.runtime_flags
								& kObjectFlagCompound) != 0
							&& owner.type != 60
							&& owner.type != 70
							&& owner.type != 24
							&& owner.type != 30;
						// gun_mount_select_aimable_target tests huge rounds against
						// the compound object's full sphere. Component traversal is
						// only the ordinary-round path for eligible compound owners.
						if (capital_projectile
							|| candidate.component_count == 0)
						{
							mount.target_object = candidate_index;
							mount.target_component = -1;
							selected = track_kind_one_target(
								world,
								owner,
								mount,
								candidate,
								gun_stats,
								simulation_tick,
								false);
						}
						else if (component_targeting_owner)
						{
							for (const ObjectModelReference& model
								: candidate.model_references)
							{
								if (model.removed
									|| model.component_index < 0)
								{
									continue;
								}
								mount.target_object =
									candidate_index;
								mount.target_component =
									model.component_index;
								if (track_kind_one_target(
									world,
									owner,
									mount,
									candidate,
									gun_stats,
									simulation_tick,
									false))
								{
									selected = true;
									break;
								}
							}
						}
						if (selected)
						{
							break;
						}
						mount.target_object = UINT16_MAX;
						mount.target_component = -1;
					}
				}
				mount.target_search_deadline =
					simulation_tick + 100u
						+ world_rand15(world) % 100u;
				// Gun_kind_one_update returns immediately after the search
				// attempt. A newly accepted target is first tracked and rotated
				// by the next rendered-frame callback.
				continue;
			}
		}
	}
}

void weapons_service_projectile_physics(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	const assets::GunStatsTable& gun_stats,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick)
{
	for (GunProjectile& projectile : runtime.projectiles)
	{
		if (projectile.type_index == -1)
		{
			continue;
		}
		projectile.previous_position = projectile.position;
		projectile.position += projectile.velocity;
		if (process_ordinary_collisions(
				runtime,
				world,
				mission,
				gun_stats,
				ship_stats,
				projectile,
				simulation_tick))
		{
			destroy_projectile(runtime, projectile);
		}
	}
}

void weapons_step_projectiles(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	std::uint32_t simulation_tick,
	std::uint8_t service_phase)
{
	for (GunProjectile& projectile : runtime.projectiles)
	{
		if (projectile.type_index == -1)
		{
			continue;
		}
		// gun_projectiles_update_and_render (0x0047a54d..0x0047a58b)
		// publishes the live SR node from the last collision-processed point
		// to the current 25 Hz position using the global quarter phase.
		projectile.scene_position =
			projectile.previous_position
				+ (projectile.position - projectile.previous_position)
					* (static_cast<float>(service_phase & 3u) * 0.25f);
		if (static_cast<std::int32_t>(simulation_tick)
			>= projectile.expiration_tick)
		{
			if (projectile.type_index == 10)
			{
				// gun_projectiles_update_and_render retains the ordinary
				// Nova projectile's expiry burst independently of the
				// charged-beam owner.
				create_nova_expiry_effect(
					world, mission, projectile, simulation_tick);
			}
			destroy_projectile(runtime, projectile);
			continue;
		}
		// gun_projectiles_update_and_render consumes the process CRT stream
		// every live frame for the Neutron random axial rotation and for
		// the two huge-gun particle offsets. Retain the sampled values on
		// the projectile so rendering remains a read-only projection.
		if (projectile.type_index == 6)
		{
			projectile.visual_random[0] =
				static_cast<float>(world_rand15(world))
					* (1.0f / 32767.0f);
		}
		else if (projectile.type_index == 13
			|| projectile.type_index == 14)
		{
			projectile.visual_random[0] =
				static_cast<float>(world_rand15(world))
					* (1.0f / 32767.0f);
			projectile.visual_random[1] =
				static_cast<float>(world_rand15(world))
					* (1.0f / 32767.0f);
		}
	}
}

bool weapons_process_missile_collision(
	WeaponRuntime& runtime,
	World& world,
	mission::Runtime& mission,
	Missile& missile,
	const assets::MissileStats& stats,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick)
{
	if (!missile.active)
	{
		return false;
	}
	GunProjectile damage_source;
	damage_source.previous_position = missile.scene_position;
	damage_source.position = missile.position;
	damage_source.velocity = missile.velocity;
	damage_source.orientation = missile.orientation;
	damage_source.type_index = 0;
	damage_source.shooter_index = missile.shooter.index;
	damage_source.shooter_generation = missile.shooter.generation;
	damage_source.shooter_affiliation = missile.allegiance_class;
	damage_source.active = true;
	const std::uint8_t damage_cause =
		missile.object_kind == 0 ? 5 : 1;

	for (std::uint16_t object_index = 0;
		object_index < kMaxGameObjects;
		++object_index)
	{
		WorldObject& target = world.objects[object_index];
		if (!target.active
			|| target.type >= 0x100
			|| (target.runtime_flags & 0x00000004u) != 0
			|| (missile.shooter.index == object_index
				&& missile.shooter.generation == target.generation))
		{
			continue;
		}

		if ((target.runtime_flags & kObjectFlagCompound) != 0)
		{
			std::uint32_t retained_model = UINT32_MAX;
			glm::vec3 retained_impact{0.0f};
			for (std::uint32_t model_index = 0;
				model_index < target.model_references.size();
				++model_index)
			{
				glm::vec3 impact;
				if (collision_model_hit(
						target,
						target.model_references[model_index],
						missile.scene_position,
						missile.position,
						impact))
				{
					retained_model = model_index;
					retained_impact = impact;
				}
			}
			if (retained_model == UINT32_MAX)
			{
				continue;
			}
			if (missile.object_kind != 2
				&& missile.object_kind != 7)
			{
				const glm::vec3 direction =
					glm::dot(
						missile.velocity,
						missile.velocity) > 0.0f
						? glm::normalize(missile.velocity)
						: missile.orientation[2];
				if ((target.runtime_flags & 0x00000100u) != 0)
				{
					world_register_cloak_hit(
						target,
						retained_impact,
						simulation_tick);
				}
				else
				{
					shields_create_impact_effect(
						world,
						&target,
						retained_impact,
						direction,
						3,
						simulation_tick);
				}
				assets::GunStats subsystem_hit;
				subsystem_hit.hull_damage =
					stats.subsystem_damage;
				apply_component_projectile_hit(
					runtime,
					world,
					mission,
					subsystem_hit,
					damage_source,
					damage_cause,
					target,
					object_index,
					retained_model,
					simulation_tick);
			}
			diagnostics::mission_log(
				"missile impact target=%u mission=%u shooter=%u "
				"type=%d kind=%d model=%u static=1",
				static_cast<unsigned>(object_index),
				static_cast<unsigned>(target.mission_index),
				static_cast<unsigned>(missile.shooter.index),
				missile.type,
				missile.object_kind,
				static_cast<unsigned>(retained_model));
			return true;
		}

		glm::vec3 impact{0.0f};
		float projection = 0.0f;
		if (!segment_sphere_hit(
				missile.scene_position,
				missile.position,
				target.scene_position,
				target.radius,
				impact,
				projection))
		{
			continue;
		}
		if (missile.object_kind != 2
			&& missile.object_kind != 7)
		{
			assets::GunStats missile_hit;
			missile_hit.shield_damage = stats.shield_damage;
			missile_hit.hull_damage = stats.hull_damage;
			const std::uint8_t bank =
				shield_bank_for_impact(target, impact);
			const bool exposed_path =
				target.primary_shields[bank] < 0.0f
				|| target.protection_state == 4;
			const std::int32_t exposed_model =
				exposed_path
					? exposed_submodel_for_sweep(
						target, damage_source)
					: -1;
			if (exposed_model >= 0)
			{
				// missile_hit_exposed_submodel (0x00495bb0) is entered
				// only for an already-negative bank or protection state
				// four.  State five deliberately continues through the
				// primary-bank owner below.
				apply_exposed_structural_hit(
					runtime,
					world,
					mission,
					missile_hit,
					damage_source,
					damage_cause,
					target,
					object_index,
					bank,
					ship_stats,
					simulation_tick);
			}
			else if (!exposed_path && stats.shield_damage > 0.0f)
			{
				bool forward_primary =
					object_index != world.player.index;
				if (!forward_primary && bank == 2)
				{
					// missile_process_collision
					// (0x00495f69..0x00496067) consumes the
					// two player pools in this exact order.  A
					// positive first pool prevents access to the
					// second one, and primary damage is forwarded
					// only by the hit which crosses a pool to zero.
					float* auxiliary = nullptr;
					if (target.auxiliary_shields[0] > 0.0f)
					{
						auxiliary =
							&target.auxiliary_shields[0];
					}
					else if (target.auxiliary_shields[1] > 0.0f)
					{
						auxiliary =
							&target.auxiliary_shields[1];
					}
					if (auxiliary != nullptr)
					{
						*auxiliary -= stats.shield_damage;
						if (*auxiliary <= 0.0f)
						{
							*auxiliary = 0.0f;
							forward_primary = true;
						}
					}
				}
				if (forward_primary)
				{
					const float primary_damage =
						stats.shield_damage
						* (mission.network.role
								!= mission::NetworkRole::offline
							? 5.0f
							: 1.0f);
					(void)apply_primary_bank_damage(
						world,
						mission,
						target,
						ship_stats,
						bank,
						primary_damage,
						stats.hull_damage
							/ stats.shield_damage,
						missile.shooter.index,
						damage_cause,
						runtime.feedback_enabled,
						simulation_tick);
				}
			}
			if (exposed_model >= 0)
			{
				shields_create_impact_effect(
					world,
					&target,
					impact,
					glm::normalize(
						impact - target.position),
					2,
					simulation_tick);
			}
			else if ((target.runtime_flags & 0x00000100u) != 0)
			{
				world_register_cloak_hit(
					target, impact, simulation_tick);
			}
			else
			{
				if (!exposed_path)
				{
					// Retail still publishes the ordinary impact when
					// shield damage is zero, or when the local bank/pool
					// gates suppress the primary owner.
					shields_register_hit(
						world,
						target,
						-1,
						impact,
						2,
					simulation_tick);
				}
			}
		}
		diagnostics::mission_log(
			"missile impact target=%u mission=%u shooter=%u "
			"type=%d kind=%d bank=%u static=0",
			static_cast<unsigned>(object_index),
			static_cast<unsigned>(target.mission_index),
			static_cast<unsigned>(missile.shooter.index),
			missile.type,
			missile.object_kind,
			static_cast<unsigned>(
				shield_bank_for_impact(target, impact)));
		return true;
	}
	return false;
}
}
