#include "game/disruption_effects.hpp"

#include "ai/runtime.hpp"
#include "assets/gameplay_model.hpp"
#include "assets/ship_stats.hpp"
#include "core/mission_log.hpp"
#include "game/damage.hpp"
#include "game/death_effects.hpp"
#include "game/model_animation.hpp"
#include "game/particle_emitters.hpp"
#include "game/shields.hpp"
#include "game/world.hpp"
#include "mission/events.hpp"
#include "mission/player_comms.hpp"
#include "mission/runtime.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstring>

namespace sl_open::game
{
namespace
{
constexpr float kRandScale = 1.0f / 32767.0f;

struct CapitalExplosionSequence
{
	std::uint16_t object_type;
	std::int16_t breakaway_object_type;
	std::uint8_t mode;
	float billboard_size_base;
	float final_billboard_size_override;
	float fragment_mesh_scale;
	std::uint8_t fragment_count;
	float heavy_fragment_chance;
	std::uint16_t phase_end_tick;
};

// ExplodeType table, LANCER.EXE 0x004ffb50..0x0050022f. The unused
// secondary type and the invariant candidate-child count are intentionally
// not retained here; no instruction in the retail controller reads the
// former and every compiled count is one.
constexpr std::array<CapitalExplosionSequence, 40>
	kCapitalExplosionSequences{{
		{33, 93, 1, 500.0f, 5000.0f, 0.3f, 3, 0.0f, 100},
		{22, 75, 0, 900.0f, -1.0f, 0.5f, 3, 0.05f, 900},
		{30, -1, 1, 500.0f, -1.0f, 0.3f, 2, 0.0f, 100},
		{32, -1, 1, 500.0f, -1.0f, 0.3f, 2, 0.0f, 100},
		{58, -1, 1, 500.0f, -1.0f, 0.3f, 3, 0.0f, 100},
		{60, -1, 1, 500.0f, -1.0f, 0.3f, 2, 0.0f, 100},
		{55, 117, 0, 2300.0f, -1.0f, 0.45f, 3, 0.05f, 900},
		{24, -1, 1, 900.0f, -1.0f, 0.25f, 1, 0.0f, 100},
		{70, -1, 1, 1600.0f, -1.0f, 0.4f, 3, 0.0f, 100},
		{129, 183, 1, 12000.0f, 30000.0f, 1.2f, 2, 0.0f, 900},
		{128, -1, 1, 4000.0f, 10000.0f, 1.0f, 2, 0.0f, 100},
		{62, 130, 1, 500.0f, -1.0f, 0.3f, 2, 0.0f, 100},
		{17, 197, 0, 7500.0f, -1.0f, 0.9f, 3, 0.0f, 800},
		{56, 143, 0, 8000.0f, 30000.0f, 1.0f, 3, 0.05f, 1200},
		{63, -1, 0, 1700.0f, -1.0f, 0.5f, 3, 0.05f, 550},
		{12, 150, 0, 3300.0f, -1.0f, 0.8f, 3, 0.05f, 1200},
		{20, -1, 0, 5000.0f, -1.0f, 0.75f, 3, 0.05f, 750},
		{120, -1, 0, 2500.0f, -1.0f, 0.8f, 3, 0.05f, 1000},
		{52, -1, 0, 3000.0f, -1.0f, 0.65f, 3, 0.05f, 1100},
		{13, 170, 0, 6000.0f, -1.0f, 1.0f, 3, 0.05f, 2000},
		{165, 173, 0, 6500.0f, 20000.0f, 1.0f, 2, 0.05f, 1200},
		{176, 186, 0, 1600.0f, -1.0f, 0.3f, 3, 0.05f, 850},
		{132, -1, 0, 12000.0f, 30000.0f, 2.0f, 4, 0.0f, 1400},
		{61, 187, 0, 1500.0f, -1.0f, 0.5f, 1, 0.05f, 900},
		{69, 189, 1, 13000.0f, 30000.0f, 1.5f, 3, 0.0f, 500},
		{149, -1, 1, 13000.0f, 30000.0f, 1.2f, 2, 0.0f, 200},
		{67, 191, 0, 8000.0f, 25000.0f, 1.2f, 2, 0.05f, 1600},
		{155, 217, 0, 8000.0f, 30000.0f, 1.0f, 3, 0.05f, 1200},
		{192, 217, 0, 8000.0f, -1.0f, 0.8f, 4, 0.05f, 1200},
		{194, 195, 0, 2300.0f, -1.0f, 0.5f, 3, 0.05f, 800},
		{54, 195, 0, 3000.0f, -1.0f, 0.5f, 3, 0.05f, 800},
		{94, 196, 0, 6000.0f, -1.0f, 1.0f, 2, 0.05f, 1200},
		{68, 169, 0, 6000.0f, -1.0f, 0.8f, 3, 0.0f, 1200},
		{19, 198, 0, 6500.0f, -1.0f, 1.0f, 3, 0.05f, 1200},
		{73, -1, 1, 500.0f, 5000.0f, 0.1f, 2, 0.1f, 50},
		{15, -1, 0, 2600.0f, -1.0f, 0.5f, 2, 0.05f, 800},
		{168, -1, 1, 12000.0f, 30000.0f, 1.5f, 3, 0.0f, 400},
		{156, -1, 0, 3000.0f, -1.0f, 0.65f, 3, 0.05f, 1100},
		{71, 107, 0, 7500.0f, -1.0f, 1.0f, 1, 0.05f, 1000},
		{72, -1, 0, 13000.0f, 30000.0f, 1.5f, 3, 0.0f, 50},
	}};

const CapitalExplosionSequence* capital_explosion_sequence(
	std::uint16_t type,
	std::uint8_t* sequence_index = nullptr)
{
	for (std::uint8_t index = 0;
		index < kCapitalExplosionSequences.size();
		++index)
	{
		if (kCapitalExplosionSequences[index].object_type != type)
		{
			continue;
		}
		if (sequence_index != nullptr)
		{
			*sequence_index = index;
		}
		return &kCapitalExplosionSequences[index];
	}
	return nullptr;
}

std::int32_t authored_destruction_lifetime(std::uint16_t type)
{
	const CapitalExplosionSequence* sequence =
		capital_explosion_sequence(type);
	return sequence == nullptr ? 0 : sequence->phase_end_tick;
}

float explosion_random(World& world)
{
	return static_cast<float>(world_rand15(world)) * kRandScale;
}

std::uint16_t find_model_reference_exact(
	const WorldObject& object,
	const char* name)
{
	for (std::uint16_t index = 0;
		index < object.model_references.size();
		++index)
	{
		if (std::strcmp(object.model_references[index].name, name) == 0)
		{
			return index;
		}
	}
	return UINT16_MAX;
}

void set_model_reference_hidden(
	WorldObject& object,
	const char* name,
	bool hidden)
{
	const std::uint16_t reference =
		find_model_reference_exact(object, name);
	if (reference == UINT16_MAX)
	{
		return;
	}
	if (hidden)
	{
		object.model_references[reference].runtime_flags |= 0x00000020u;
	}
	else
	{
		object.model_references[reference].runtime_flags &= ~0x00000020u;
	}
}

void set_model_subtree_hidden(
	WorldObject& object,
	std::uint16_t root,
	bool hidden)
{
	for (std::uint16_t reference = 0;
		reference < object.model_references.size();
		++reference)
	{
		std::int16_t current = static_cast<std::int16_t>(reference);
		while (current >= 0)
		{
			if (static_cast<std::uint16_t>(current) == root)
			{
				if (hidden)
				{
					object.model_references[reference].runtime_flags |=
						0x00000020u;
				}
				else
				{
					object.model_references[reference].runtime_flags &=
						~0x00000020u;
				}
				break;
			}
			current = object.model_references[
				static_cast<std::uint16_t>(current)].parent_reference;
		}
	}
}

glm::mat4 model_reference_world_transform(
	const WorldObject& object,
	std::uint16_t reference)
{
	const glm::mat4 root = math::model_transform(
		object.orientation, 1.0f, object.position);
	return reference < object.model_references.size()
		? root * model_animation_render_transform(object, reference, 1.0f)
		: root;
}

glm::vec3 model_point_world(
	const WorldObject& object,
	std::uint16_t reference,
	const glm::vec3& point)
{
	return glm::vec3(
		model_reference_world_transform(object, reference)
			* glm::vec4(point, 1.0f));
}

CapitalExplosionController* allocate_explosion_controller(
	DisruptionEffectsRuntime& runtime)
{
	for (CapitalExplosionController& controller
		: runtime.explosion_controllers)
	{
		if (!controller.active)
		{
			return &controller;
		}
	}
	// Explosion_controller_find_slot returns slot zero when all ten owning
	// pointers are occupied. Both retail constructors overwrite that slot.
	return &runtime.explosion_controllers[0];
}

void synchronize_breakaway_object(
	WorldObject& breakaway,
	const WorldObject& owner)
{
	// Runtime models are rebased around their own aggregate mass centers at
	// load time. Preserve the common authored model origin when a separately
	// loaded breakaway resource adopts the owner's pose.
	breakaway.previous_position = owner.previous_position
		+ owner.previous_orientation
			* (breakaway.center_of_mass - owner.center_of_mass);
	breakaway.position = owner.position
		+ owner.orientation
			* (breakaway.center_of_mass - owner.center_of_mass);
	breakaway.previous_orientation = owner.previous_orientation;
	breakaway.orientation = owner.orientation;
	breakaway.linear_velocity = owner.linear_velocity;
	breakaway.angular_x = owner.angular_x;
	breakaway.angular_y = owner.angular_y;
	breakaway.angular_z = owner.angular_z;
}

void trigger_near_explosion_disturbance(
	World& world,
	const WorldObject& owner)
{
	const WorldObject* player = world_resolve(world, world.player);
	if (player != nullptr
		&& glm::distance(player->position, owner.position)
			< owner.radius * 5.0f)
	{
		world.player_exhaust_exposure_percent = 100;
	}
}

void explosion_schedule_insert(
	std::vector<glm::vec3>& schedule,
	const glm::vec3& point,
	bool compare_z)
{
	// Explosion_schedule_node_insert, 0x0046f6d0. Preserve its unusual
	// head/previous test literally: a value which belongs between the first
	// and second nodes is inserted at the head, not after the first node.
	if (schedule.empty())
	{
		schedule.push_back(point);
		return;
	}
	std::size_t current = 0;
	std::size_t previous = 0;
	for (;;)
	{
		if (compare_z && point.z <= schedule[current].z)
		{
			break;
		}
		previous = current;
		++current;
		if (current == schedule.size())
		{
			break;
		}
	}
	if (previous == 0)
	{
		schedule.insert(schedule.begin(), point);
	}
	else
	{
		schedule.insert(
			schedule.begin()
				+ static_cast<std::ptrdiff_t>(previous + 1),
			point);
	}
}

void collect_explosion_schedule(
	const WorldObject& object,
	CapitalExplosionController& controller)
{
	for (const ObjectModelReference& model : object.model_references)
	{
		if (model.point_groups == nullptr)
		{
			continue;
		}
		for (const assets::GameplayPointGroup& group : *model.point_groups)
		{
			if (group.type != 2)
			{
				continue;
			}
			for (const assets::GameplayPoint& point : group.points)
			{
				const glm::vec3 object_point = glm::vec3(
					model.local_transform * glm::vec4(point.position, 1.0f));
				explosion_schedule_insert(
					controller.schedule,
					object_point,
					object.type != 129);
			}
		}
	}
}

glm::vec3 scheduled_world_point(
	const WorldObject& owner,
	const glm::vec3& local_point)
{
	return owner.position + owner.orientation * local_point;
}

void set_explosion_portal_subtree(
	WorldObject& object,
	std::uint16_t root_reference,
	std::uint8_t group)
{
	if (root_reference >= object.model_references.size())
	{
		return;
	}
	for (std::uint16_t reference = 0;
		reference < object.model_references.size();
		++reference)
	{
		std::int32_t current = reference;
		while (current >= 0
			&& static_cast<std::size_t>(current)
				< object.model_references.size())
		{
			if (current == root_reference)
			{
				object.model_references[reference]
					.explosion_portal_group = group;
				break;
			}
			current = object.model_references[
				static_cast<std::uint16_t>(current)].parent_reference;
		}
	}
}

void set_named_explosion_portal_subtree(
	WorldObject& object,
	const char* name,
	std::uint8_t group)
{
	set_explosion_portal_subtree(
		object, find_model_reference_exact(object, name), group);
}

glm::vec3 scheduled_anchor_world_point(
	const WorldObject&,
	const CapitalExplosionController& controller,
	const glm::vec3& local_point)
{
	// collect_explosion_schedule has already composed the authored point
	// through its source model into object space. The retained moving model's
	// absolute frame therefore contributes only the constructor-time object
	// root here; applying its scene_transform again double-translates every
	// point (most visibly placing the split plane outside capital hulls).
	const glm::mat4 portal_root = math::model_transform(
		controller.portal_orientation,
		1.0f,
		controller.original_owner_position);
	return glm::vec3(portal_root * glm::vec4(local_point, 1.0f));
}

void create_capital_billboard(
	World& world,
	const glm::vec3& position,
	float size,
	std::int32_t delay,
	std::uint32_t simulation_tick)
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
		delay,
		false,
		false,
		simulation_tick);
}

const assets::GameplayPointGroup* find_point_group(
	const WorldObject& object,
	std::uint16_t reference,
	std::int16_t type)
{
	if (reference >= object.model_references.size())
	{
		return nullptr;
	}
	const ObjectModelReference& model = object.model_references[reference];
	if (model.point_groups == nullptr)
	{
		return nullptr;
	}
	for (const assets::GameplayPointGroup& group : *model.point_groups)
	{
		if (group.type == type)
		{
			return &group;
		}
	}
	return nullptr;
}

void create_reference_point_flashes(
	World& world,
	const WorldObject& owner,
	std::uint16_t reference,
	float size,
	std::int32_t first_delay,
	std::int32_t delay_step,
	std::uint16_t count,
	std::uint32_t simulation_tick)
{
	const assets::GameplayPointGroup* group =
		find_point_group(owner, reference, 5);
	if (group == nullptr)
	{
		return;
	}
	const std::uint16_t point_count = std::min<std::uint16_t>(
		count, static_cast<std::uint16_t>(group->points.size()));
	for (std::uint16_t point = 0; point < point_count; ++point)
	{
		create_capital_billboard(
			world,
			model_point_world(
				owner, reference, group->points[point].position),
			size,
			first_delay + static_cast<std::int32_t>(point) * delay_step,
			simulation_tick);
	}
}

void spawn_point_pair_destruction_effects(
	World& world,
	const WorldObject& owner,
	const char* reference_name,
	bool short_on_phase,
	bool fade_off_phase,
	bool suppress_attached_effects,
	std::uint32_t simulation_tick)
{
	const std::uint16_t reference =
		find_model_reference_exact(owner, reference_name);
	const assets::GameplayPointGroup* pairs =
		find_point_group(owner, reference, 1);
	if (pairs == nullptr)
	{
		return;
	}
	const long rounded_half = std::lrint(
		static_cast<double>(pairs->points.size()) * 0.5);
	const std::size_t pair_count = rounded_half <= 0
		? 0
		: static_cast<std::size_t>(rounded_half - 1);
	for (std::size_t pair = 0; pair < pair_count; ++pair)
	{
		const std::size_t first = pair * 2u;
		const std::size_t second = first + 1u;
		if (second >= pairs->points.size())
		{
			break;
		}
		std::uint8_t flags = short_on_phase ? 2u : 6u;
		if (fade_off_phase)
		{
			flags |= 1u;
		}
		ElectricRayEffect* ray = electric_ray_create(
			world, 1, 5000, 0.2f, 260.0f, flags);
		if (ray == nullptr)
		{
			continue;
		}
		ray->start = pairs->points[first].position;
		ray->end = pairs->points[second].position;
		electric_ray_set_parent(
			*ray,
			static_cast<std::uint16_t>(&owner - std::begin(world.objects)),
			owner.generation,
			reference);
		electric_ray_set_group_color(
			*ray, 0, {0.6f, 1.0f, 1.0f});
	}
	if (suppress_attached_effects
		|| reference >= owner.model_references.size()
		|| owner.model_references[reference].point_groups == nullptr)
	{
		return;
	}
	const ObjectHandle owner_handle{
		static_cast<std::uint16_t>(&owner - std::begin(world.objects)),
		owner.generation,
	};
	for (const assets::GameplayPointGroup& group
		: *owner.model_references[reference].point_groups)
	{
		if (group.type == 4)
		{
			if (!group.points.empty())
			{
				destruction_light_create(
					world.death_effects,
					owner_handle,
					reference,
					group.points.front().position);
			}
		}
		else if (group.type == 3)
		{
			for (const assets::GameplayPoint& point : group.points)
			{
				(void)particle_emitter_create_model_owned(
					world,
					owner_handle,
					reference,
					point.position,
					point.direction,
					{0.5f, 0.5f, 0.0f},
					3.0f,
					0.5f,
					short_on_phase ? 9999999u : 5000u,
					ParticleEmitterStyle::gray_explosion,
					simulation_tick);
			}
		}
	}
}

bool start_ulysses_component_destruction(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& ship_stats,
	std::uint16_t object_index,
	std::uint16_t destroyed_reference,
	std::uint32_t simulation_tick)
{
	WorldObject& owner = world.objects[object_index];
	std::uint16_t affected_reference = destroyed_reference;
	if (destroyed_reference < owner.model_references.size()
		&& owner.model_references[destroyed_reference].parent_reference >= 0)
	{
		affected_reference = static_cast<std::uint16_t>(
			owner.model_references[destroyed_reference].parent_reference);
	}
	const char* affected_name =
		affected_reference < owner.model_references.size()
			? owner.model_references[affected_reference].name
			: "";
	if (std::strcmp(affected_name, "Ulysses Top") != 0
		&& std::strcmp(affected_name, "Ulysses Fin") != 0
		&& destroyed_reference < owner.model_references.size())
	{
		affected_reference = destroyed_reference;
		affected_name = owner.model_references[destroyed_reference].name;
	}

	owner.runtime_flags |= 0x00000008u;
	const bool top = std::strcmp(affected_name, "Ulysses Top") == 0;
	const bool fin = std::strcmp(affected_name, "Ulysses Fin") == 0;
	if (top)
	{
		set_model_reference_hidden(owner, "Uly frnt dest", false);
		CapitalExplosionController* controller =
			allocate_explosion_controller(world.disruption_effects);
		*controller = {};
		controller->active = true;
		controller->ulysses = true;
		controller->owner_index = object_index;
		controller->owner_generation = owner.generation;
		controller->start_tick = simulation_tick;
		controller->original_owner_position = owner.position;
		controller->portal_position = owner.position;
		controller->portal_orientation = owner.orientation;
		controller->portal_clipping = true;
		set_named_explosion_portal_subtree(
			owner, "Ulysses Top", 1);
		set_named_explosion_portal_subtree(
			owner, "Uly frnt dest", 2);
		const ObjectHandle breakaway_handle = world_create(
			world, 75, owner.position, owner.orientation, ship_stats, false);
		if (WorldObject* breakaway =
			world_resolve(world, breakaway_handle))
		{
			controller->breakaway_index = breakaway_handle.index;
			controller->breakaway_generation = breakaway_handle.generation;
			synchronize_breakaway_object(*breakaway, owner);
			breakaway->runtime_flags |= 0x00000048u;
			set_named_explosion_portal_subtree(
				*breakaway, "Uly back dest", 2);
		}

		// Ulysses_component_destruction_callback, LANCER.EXE
		// 0x0046ec08..0x0046ec35. The Top split publishes the first
		// retained component before synchronously crossing the whole-object
		// Destroyed boundary.
		if (owner.component_count != 0)
		{
			std::uint16_t attacker = UINT16_MAX;
			if (owner.last_attacker_index < kMaxGameObjects
				&& world.objects[owner.last_attacker_index].active)
			{
				attacker = world.objects[
					owner.last_attacker_index].mission_index;
			}
			(void)mission::events_emit_component_destroyed(
				mission,
				owner.mission_index,
				attacker,
				0);
		}
		(void)world_mark_destroyed(world, mission, owner);
	}

	if ((top || fin) && (owner.ulysses_destruction_state & 1u) == 0)
	{
		owner.ulysses_destruction_state |= 1u;
		const ObjectHandle secondary_handle = world_create(
			world, 76, owner.position, owner.orientation, ship_stats, false);
		WorldObject* secondary = world_resolve(world, secondary_handle);
		if (secondary != nullptr)
		{
			synchronize_breakaway_object(*secondary, owner);
			secondary->runtime_flags |= 0x00000048u;
			secondary->angular_x = 0.002f;
			secondary->angular_y = -0.0002f;
			secondary->angular_z = -0.0001f;
			secondary->linear_velocity =
				owner.orientation * glm::vec3{5.0f, 15.0f, -1.0f};
		}

		const std::uint16_t fin_reference =
			find_model_reference_exact(owner, "Ulysses Fin");
		create_reference_point_flashes(
			world,
			owner,
			fin_reference,
			owner.radius * 0.3f,
			0,
			0,
			3,
			simulation_tick);
		const assets::GameplayPointGroup* fin_points =
			find_point_group(owner, fin_reference, 5);
		if (fin_points != nullptr)
		{
			const std::uint16_t count = std::min<std::uint16_t>(
				3, static_cast<std::uint16_t>(fin_points->points.size()));
			for (std::uint16_t point = 0; point < count; ++point)
			{
				create_capital_billboard(
					world,
					model_point_world(
						owner,
						fin_reference,
						fin_points->points[point].position),
					owner.radius * 0.3f,
					60 + static_cast<std::int32_t>(point) * 30,
					simulation_tick);
			}
		}

		set_model_reference_hidden(owner, "Uly mid sec dest", false);
		set_model_reference_hidden(owner, "Ulysses Low gen", true);
		set_model_reference_hidden(owner, "Ulysses Fin", true);
		set_model_reference_hidden(owner, "Uly mid sec", true);
		set_model_reference_hidden(owner, "Uly mid dest", true);
		if (fin && !top)
		{
			trigger_near_explosion_disturbance(world, owner);
			world_queue_sound_object(
				world, {object_index, owner.generation}, 65, 2);
		}
		if (secondary != nullptr)
		{
			spawn_point_pair_destruction_effects(
				world,
				*secondary,
				"Uly bfin dest",
				false,
				true,
				false,
				simulation_tick);
		}
		// Ulysses_component_destruction_callback, LANCER.EXE
		// 0x0046eec7..0x0046eeed. The one-shot Fin split publishes the
		// second retained component after installing its breakaway effects.
		if (owner.component_count > 1)
		{
			std::uint16_t attacker = UINT16_MAX;
			if (owner.last_attacker_index < kMaxGameObjects
				&& world.objects[owner.last_attacker_index].active)
			{
				attacker = world.objects[
					owner.last_attacker_index].mission_index;
			}
			(void)mission::events_emit_component_destroyed(
				mission,
				owner.mission_index,
				attacker,
				1);
		}
	}

	world.disruption_effects.destruction_shockwave_generation[object_index] =
		owner.generation;
	world.disruption_effects.destruction_shockwave_created[object_index] = true;
	// Ulysses_component_destruction_callback clears AL on every compiled
	// exit (0x0046eeed..0x0046eef6); the render traversal therefore never
	// performs its ordinary sibling-group destruction after this callback.
	return false;
}

void create_mode_one_burst(
	World& world,
	const CapitalExplosionSequence& sequence,
	const CapitalExplosionController& controller,
	const WorldObject& owner,
	std::uint32_t simulation_tick,
	const glm::vec3* final_position,
	bool play_sound)
{
	const glm::vec3& local_position = final_position == nullptr
		? controller.schedule[
			world_rand15(world) % controller.schedule.size()]
		: *final_position;
	const glm::vec3 position =
		scheduled_anchor_world_point(owner, controller, local_position);
	if (final_position == nullptr)
	{
		create_capital_billboard(
			world,
			position,
			(2.0f + explosion_random(world))
				* sequence.billboard_size_base,
			0,
			simulation_tick);
		const std::int32_t delay = world_rand15(world) % 150;
		create_capital_billboard(
			world,
			position,
			(1.0f + explosion_random(world))
				* sequence.billboard_size_base,
			delay,
			simulation_tick);
		glm::vec3 direction = position - owner.position;
		const float direction_length = glm::length(direction);
		if (direction_length > 0.0f)
		{
			direction /= direction_length;
		}
		for (std::uint16_t fragment = 0;
			fragment < static_cast<std::uint16_t>(
				sequence.fragment_count * 4u);
			++fragment)
		{
			const float z = explosion_random(world) * 50.0f;
			const float y = explosion_random(world) * 50.0f;
			const float x = explosion_random(world) * 50.0f;
			particle_fragment_directional_burst(
				world,
				position + glm::vec3{x, y, z},
				direction,
				sequence.heavy_fragment_chance,
				sequence.fragment_mesh_scale,
				0.2f,
				1,
				simulation_tick);
		}
		if (play_sound)
		{
			world_queue_sound_explicit(
				world,
				position,
				owner.orientation[2],
				owner.linear_velocity,
				11,
				3);
		}
		return;
	}

	const std::int32_t final_delay = world_rand15(world) % 75;
	const float final_size = (1.0f + explosion_random(world)) * 0.5f
		* sequence.billboard_size_base;
	create_capital_billboard(
		world,
		position,
		final_size,
		final_delay,
		simulation_tick);
	glm::vec3 direction = position - owner.position;
	const float direction_length = glm::length(direction);
	if (direction_length > 0.0f)
	{
		direction /= direction_length;
	}
	for (std::uint8_t fragment = 0;
		fragment < sequence.fragment_count;
		++fragment)
	{
		// 0x004703bb..0x0047044e offsets each final fragment in world
		// axes by independent [0,50] draws before calling the common
		// fragment constructor. MSVC evaluates Z, Y, X.
		const float z = explosion_random(world) * 50.0f;
		const float y = explosion_random(world) * 50.0f;
		const float x = explosion_random(world) * 50.0f;
		particle_fragment_directional_burst(
			world,
			position + glm::vec3{x, y, z},
			direction,
			sequence.heavy_fragment_chance,
			sequence.fragment_mesh_scale,
			sequence.fragment_mesh_scale * 0.5f,
			1,
			simulation_tick);
	}
}

std::uint32_t float_bits(float value)
{
	std::uint32_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

float shockwave_age(
	const Shockwave& wave,
	std::uint32_t simulation_tick)
{
	return static_cast<float>(simulation_tick - wave.start_tick)
		/ static_cast<float>(wave.lifetime_ticks);
}

bool shell_crossed(
	const Shockwave& wave,
	const WorldObject& object,
	float current_radius)
{
	const float distance = glm::distance(wave.position, object.position);
	return wave.previous_radius <= distance
		&& distance < current_radius;
}

void raise_camera_disturbance(
	World& world,
	float age,
	bool clamp_to_two)
{
	float disturbance = age * 10.0f;
	if (clamp_to_two)
	{
		disturbance = std::min(2.0f, disturbance);
	}
	world.player_camera_disturbance =
		std::max(world.player_camera_disturbance, disturbance);
}

bool common_disruption_candidate(
	const Shockwave& wave,
	const WorldObject& object,
	std::uint32_t simulation_tick)
{
	// Shockwave_update_all, LANCER.EXE 0x004a1081..0x004a10c2.
	// Types 74 and 31 are unconditional rejections. Every other object must
	// also have an affiliation different from the source wave.
	return object.active
		&& (object.runtime_flags & 0x00000462u) == 0
		&& object.shockwave_immunity_deadline <= simulation_tick
		&& object.type != 74
		&& object.type != 31
		&& object.allegiance_class != wave.source_affiliation;
}

void apply_equipment_disruption(
	World& world,
	WorldObject& object,
	const Shockwave& wave,
	float age)
{
	const float falloff = std::min(1.0f, 1.5f * (1.0f - age));
	if (!ai::command_push(world,
			object,
			114,
			ai::TargetKind::none,
			UINT16_MAX,
			-1,
			0,
			0))
	{
		return;
	}

	ai::Command& command = object.ai.commands[0];
	command.state[0] = static_cast<std::uint32_t>(
		std::trunc(falloff * 500.0f));
	glm::vec3 direction = object.position - wave.position;
	const float length = glm::length(direction);
	if (length > 0.0f)
	{
		direction /= length;
	}
	else
	{
		direction = glm::vec3{0.0f};
	}
	// The retail payload is a local force whose mass term is divided back
	// out by GameObject_integrate_accumulated_forces. This runtime stores
	// the resulting velocity delta directly, retaining the same shell,
	// falloff, radius, and lifetime relationship.
	const float velocity_delta =
		wave.final_radius * falloff
		/ static_cast<float>(wave.lifetime_ticks);
	const glm::vec3 local_direction =
		glm::transpose(object.orientation)
		* direction * velocity_delta;
	command.state[1] = float_bits(local_direction.x);
	command.state[2] = float_bits(local_direction.y);
	command.state[3] = float_bits(local_direction.z);
}

void apply_shield_disruption(
	World& world,
	mission::Runtime& mission,
	WorldObject& object,
	std::uint16_t object_index,
	const Shockwave& wave,
	const assets::ShipStatsTable& ship_stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick)
{
	ShieldSphereState& sphere = world.shields.sphere[object_index];
	// GameObject+0x20 is absent for collision-class-six objects. Every
	// ordinary candidate has a persistent HShield owner even if it has not
	// previously rendered a projectile hit, so the disruption must activate
	// the corresponding retained sphere rather than requiring it to be live.
	if (object.collision_class != 6)
	{
		shields_initialize_sphere_state(sphere);
		sphere.active = true;
		sphere.flicker_until_tick =
			static_cast<std::int32_t>(simulation_tick + 100);
		sphere.last_hit_tick = simulation_tick;
	}
	for (std::uint8_t bank = 0; bank < 4; ++bank)
	{
		// 0x004a1175..0x004a119e supplies the current bank plus fifty,
		// zero structural transfer, the victim's own index, and cause one.
		(void)apply_primary_bank_damage(
			world,
			mission,
			object,
			ship_stats,
			bank,
			object.primary_shields[bank] + 50.0f,
			0.0f,
			object_index,
			1,
			impact_feedback_enabled,
			simulation_tick);
	}
	(void)wave;
}

bool local_disruption_candidate(
	const WorldObject& player,
	std::uint32_t simulation_tick)
{
	// Types seven and eight use the dedicated local-player branch at
	// 0x004a12bd. It intentionally has no type, affiliation, or source-side
	// exclusions from the type-five/six traversal.
	return player.active
		&& (player.runtime_flags & 0x00000462u) == 0
		&& player.shockwave_immunity_deadline <= simulation_tick;
}

bool uses_reduced_type_seven_damage(std::uint16_t source_type)
{
	switch (source_type)
	{
	case 0x36:
	case 0x44:
	case 0x45:
	case 0x9b:
	case 0xa8:
		return true;
	default:
		return false;
	}
}

float local_damage_strength(
	const World& world,
	const Shockwave& wave,
	float age)
{
	const float remaining = 1.0f - age;
	if (wave.type == 8)
	{
		return remaining
			* wave.final_radius
			* 0.05000000074505806f;
	}

	std::uint16_t source_type = UINT16_MAX;
	if (wave.source_object_index >= 0
		&& wave.source_object_index
			< static_cast<std::int32_t>(kMaxGameObjects))
	{
		source_type =
			world.objects[wave.source_object_index].type;
	}
	float damage = remaining * wave.final_radius;
	damage *= remaining;
	damage *= remaining;
	damage *= uses_reduced_type_seven_damage(source_type)
		? 0.004500000271946192f
		: 0.014999999664723873f;
	return damage;
}

void apply_local_damage_wave(
	World& world,
	mission::Runtime& mission,
	WorldObject& player,
	const Shockwave& wave,
	const assets::ShipStatsTable& ship_stats,
	float age,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick)
{
	const float damage = local_damage_strength(
		world, wave, age);
	// EDI remains zero throughout retail's dedicated type-seven/eight
	// branch. These effects are authored for offline play, where slot zero
	// is the local player, but retaining the literal owner matters to the
	// cause-two network arbitration path.
	constexpr std::uint16_t attacker_index = 0;
	for (std::uint8_t bank = 0; bank < 4; ++bank)
	{
		float owner_damage = damage;
		float* crossed_auxiliary = nullptr;
		if (bank == 2 || bank == 3)
		{
			float& auxiliary =
				player.auxiliary_shields[bank - 2];
			if (auxiliary > 0.0f)
			{
				auxiliary -= damage;
				if (auxiliary >= 0.0f)
				{
					continue;
				}
				// At 0x004a1421 and 0x004a148f retail reconstructs the
				// pre-hit auxiliary value, passes that to the primary
				// owner, then clamps the auxiliary pool after that owner
				// and its structural overflow have both returned.
				owner_damage = auxiliary + damage;
				crossed_auxiliary = &auxiliary;
			}
		}
		(void)apply_primary_bank_damage(
			world,
			mission,
			player,
			ship_stats,
			bank,
			owner_damage,
			1.0f,
			attacker_index,
			2,
			impact_feedback_enabled,
			simulation_tick);
		if (crossed_auxiliary != nullptr)
		{
			*crossed_auxiliary = 0.0f;
		}
	}
}

void create_final_point_flashes(
	World& world,
	const WorldObject& owner,
	const CapitalExplosionController& controller,
	std::uint32_t simulation_tick)
{
	if (controller.moving_anchor < 0
		|| static_cast<std::size_t>(controller.moving_anchor)
			>= owner.model_references.size())
	{
		return;
	}
	create_reference_point_flashes(
		world,
		owner,
		static_cast<std::uint16_t>(controller.moving_anchor),
		owner.radius * 0.15f,
		0,
		50,
		3,
		simulation_tick);
}

void set_breakaway_motion(
	WorldObject& object,
	const glm::vec3& local_velocity,
	const glm::vec3& angular_velocity)
{
	object.linear_velocity = object.orientation * local_velocity;
	object.angular_x = angular_velocity.x;
	object.angular_y = angular_velocity.y;
	object.angular_z = angular_velocity.z;
	object.runtime_flags &= ~kObjectFlagDisabled;
}

void finalize_capital_explosion(
	World& world,
	CapitalExplosionController& controller,
	WorldObject& owner,
	const CapitalExplosionSequence& sequence,
	std::uint32_t simulation_tick)
{
	trigger_near_explosion_disturbance(world, owner);
	world_queue_sound_object(
		world,
		{controller.owner_index, controller.owner_generation},
		65,
		2);
	create_final_point_flashes(
		world, owner, controller, simulation_tick);

	WorldObject* breakaway = world_resolve(
		world,
		{controller.breakaway_index, controller.breakaway_generation});
	if (breakaway != nullptr)
	{
		if (sequence.mode == 1)
		{
			set_breakaway_motion(
				*breakaway,
				{-1.5f, -10.0f, -2.0f},
				{0.0005f, 0.002f, 0.002f});
		}
		else if (sequence.breakaway_object_type == 107
			|| sequence.breakaway_object_type == 197)
		{
			set_breakaway_motion(
				*breakaway,
				{-1.5f, 20.0f, -2.0f},
				{});
		}
		else if (sequence.breakaway_object_type == 173
			|| sequence.breakaway_object_type == 183)
		{
			set_breakaway_motion(
				*breakaway,
				{-1.5f, -40.0f, -2.0f},
				{0.0005f, 0.002f, 0.002f});
		}
		else if (sequence.breakaway_object_type == 189)
		{
			set_breakaway_motion(
				*breakaway,
				{0.0f, -20.0f, 0.0f},
				{0.0f, 0.0005f, 0.0f});
		}
		else
		{
			set_breakaway_motion(
				*breakaway,
				{-1.5f, -10.0f, -2.0f},
				{0.0005f, 0.002f, 0.002f});
		}
		if (sequence.mode == 1 && owner.type == 129)
		{
			world.player_exhaust_exposure_percent = 100;
			breakaway->linear_velocity =
				owner.orientation * glm::vec3{-5.0f, 0.0f, 0.0f};
		}
		else if (sequence.mode == 1 && owner.type == 69)
		{
			world.player_exhaust_exposure_percent = 100;
			breakaway->linear_velocity = glm::vec3{0.0f};
			breakaway->orientation = glm::mat3{1.0f};
			breakaway->previous_orientation = glm::mat3{1.0f};
		}
	}

	if (controller.selected_child >= 0
		&& static_cast<std::size_t>(controller.selected_child)
			< owner.model_references.size())
	{
		spawn_point_pair_destruction_effects(
			world,
			owner,
			owner.model_references[controller.selected_child].name,
			false,
			true,
			false,
			simulation_tick);
		owner.model_references[controller.selected_child]
			.runtime_flags &= ~0x00000020u;
	}
	for (std::uint16_t reference = 0;
		reference < owner.model_references.size();
		++reference)
	{
		const ObjectModelReference& model =
			owner.model_references[reference];
		if (model.owner_scope != 0 || model.parent_reference >= 0)
		{
			continue;
		}
		if ((model.source_flags & 0x0004u) == 0)
		{
			if (owner.type == 132 && model.part_group_id == 10)
			{
				continue;
			}
			set_model_subtree_hidden(owner, reference, true);
			continue;
		}
	}
	if (breakaway != nullptr)
	{
		for (const ObjectModelReference& reference
			: breakaway->model_references)
		{
			if (reference.owner_scope != 0
				|| reference.parent_reference >= 0)
			{
				continue;
			}
			spawn_point_pair_destruction_effects(
				world,
				*breakaway,
				reference.name,
				false,
				true,
				false,
				simulation_tick);
		}
	}
	owner.runtime_flags |= 0x00000048u;
	if (sequence.mode == 1
		&& (owner.type == 69 || owner.type == 129))
	{
		owner.linear_velocity = {};
		owner.angular_x = 0.0f;
		owner.angular_y = 0.0f;
		owner.angular_z = 0.0f;
	}
	else
	{
		owner.linear_velocity =
			owner.orientation * glm::vec3{2.0f, 1.4f, -5.0f};
		if (owner.type != 129)
		{
			owner.angular_x = -0.00004f;
			owner.angular_y = 0.002f;
			owner.angular_z = -0.0013f;
		}
	}
	if (sequence.mode == 0
		&& (owner.type == 17
			|| owner.type == 67
			|| owner.type == 68
			|| owner.type == 69
			|| owner.type == 71
			|| owner.type == 132
			|| owner.type == 149))
	{
		owner.linear_velocity = {};
		owner.angular_x = 0.0f;
		owner.angular_y = 0.0f;
		owner.angular_z = 0.0f;
	}
	controller = {};
}

void service_ulysses_explosion_controller(
	World& world,
	CapitalExplosionController& controller,
	WorldObject& owner,
	std::uint32_t simulation_tick)
{
	const std::uint16_t top_reference =
		find_model_reference_exact(owner, "Ulysses Top");
	if (top_reference == UINT16_MAX)
	{
		controller = {};
		return;
	}
	const ObjectModelReference& top =
		owner.model_references[top_reference];
	set_explosion_portal_subtree(owner, top_reference, 1);
	set_named_explosion_portal_subtree(owner, "Uly frnt dest", 2);
	if (WorldObject* breakaway = world_resolve(
			world,
			{controller.breakaway_index, controller.breakaway_generation}))
	{
		set_named_explosion_portal_subtree(
			*breakaway, "Uly back dest", 2);
	}
	const std::uint32_t elapsed = simulation_tick - controller.start_tick;
	if (elapsed < 900u)
	{
		const glm::mat4 portal_root = math::model_transform(
			controller.portal_orientation,
			1.0f,
			controller.portal_position);
		const glm::mat4 top_transform = portal_root
			* model_animation_render_transform(owner, top_reference, 1.0f);
		if (top.point_groups != nullptr
			&& !top.point_groups->empty()
			&& static_cast<float>(controller.schedule_counter)
				< static_cast<float>(elapsed) * 0.07444444298744202f)
		{
			const assets::GameplayPointGroup& points =
				top.point_groups->front();
			if (controller.schedule_counter < points.points.size())
			{
				const std::uint16_t counter = controller.schedule_counter;
				const glm::vec3 position = glm::vec3(
					top_transform
						* glm::vec4(
							points.points[counter].position, 1.0f));
				create_capital_billboard(
					world, position, 3200.0f, 0, simulation_tick);
				glm::vec3 direction = -owner.orientation[2];
				const float length = glm::length(direction);
				if (length > 0.0f)
				{
					direction /= length;
				}
				particle_fragment_directional_burst(
					world,
					position,
					direction,
					0.1f,
					1.0f,
					1.0f,
					static_cast<std::uint16_t>(
						5u + (counter & 1u)),
					simulation_tick);
				++controller.schedule_counter;
				if (controller.schedule_counter % 15u == 0)
				{
					world_queue_sound_explicit(
						world,
						position,
						owner.orientation[2],
						owner.linear_velocity,
						(world_rand15(world) & 1u) == 0 ? 12 : 11,
						3);
				}
			}
		}
		if (top.point_groups != nullptr
			&& !top.point_groups->empty()
			&& !top.point_groups->front().points.empty())
		{
			const auto& points = top.point_groups->front().points;
			const std::size_t current = std::min<std::size_t>(
				controller.schedule_counter, points.size() - 1u);
			const std::size_t previous = current == 0 ? 0 : current - 1u;
			const glm::vec3 previous_position = glm::vec3(
				top_transform
					* glm::vec4(points[previous].position, 1.0f));
			const glm::vec3 current_position = glm::vec3(
				top_transform
					* glm::vec4(points[current].position, 1.0f));
			controller.portal_position =
				current_position.z < previous_position.z
					? current_position
					: previous_position;
			controller.portal_submitted =
				controller.schedule_counter < 66u;
		}

		glm::vec3 shaken = controller.original_owner_position;
		for (std::uint8_t axis = 0; axis < 3; ++axis)
		{
			shaken[axis] += explosion_random(world) - 0.5f < 0.0f
				? -25.0f
				: 25.0f;
		}
		owner.previous_position = shaken;
		owner.position = shaken;
		return;
	}

	if ((owner.ulysses_destruction_state & 2u) != 0)
	{
		return;
	}
	owner.ulysses_destruction_state |= 2u;
	if (WorldObject* breakaway = world_resolve(
			world,
			{controller.breakaway_index, controller.breakaway_generation}))
	{
		set_breakaway_motion(
			*breakaway,
			{-1.5f, -10.0f, -2.0f},
			{0.0005f, 0.002f, 0.002f});
	}
	set_model_reference_hidden(owner, "Ulysses Top", true);
	set_model_reference_hidden(owner, "Uly mid sec dest", true);
	trigger_near_explosion_disturbance(world, owner);
	world_queue_sound_explicit(
		world,
		owner.position,
		owner.orientation[2],
		owner.linear_velocity,
		11,
		3);
	create_reference_point_flashes(
		world,
		owner,
		top_reference,
		owner.radius * 0.15f,
		0,
		50,
		3,
		simulation_tick);
	if ((owner.ulysses_destruction_state & 3u) == 3u)
	{
		owner.runtime_flags |= 0x00000048u;
		owner.angular_x = 0.00004f;
		owner.angular_y = 0.0013f;
		owner.angular_z = -0.002f;
		owner.linear_velocity =
			owner.orientation * glm::vec3{-2.0f, 3.0f, 20.0f};
		if (WorldObject* breakaway = world_resolve(
				world,
				{controller.breakaway_index,
				 controller.breakaway_generation}))
		{
			spawn_point_pair_destruction_effects(
				world,
				*breakaway,
				"Uly back dest",
				false,
				true,
				false,
				simulation_tick);
		}
	}
	spawn_point_pair_destruction_effects(
		world,
		owner,
		"Uly frnt dest",
		false,
		true,
		false,
		simulation_tick);
	controller = {};
}
}

void disruption_effects_reset(DisruptionEffectsRuntime& runtime)
{
	runtime = {};
}

ElectricRayEffect* electric_ray_create(
	World& world,
	std::int16_t group_count,
	std::int32_t lifetime_ticks,
	float displacement,
	float radius,
	std::uint8_t flags)
{
	ElectricRayEffect* retained = nullptr;
	for (ElectricRayEffect& effect :
		world.disruption_effects.electric_rays)
	{
		if (!effect.active)
		{
			retained = &effect;
			break;
		}
	}
	if (retained == nullptr)
	{
		// Electric_ray_create (0x0046ac50) evicts slot zero when all one
		// hundred owning pointers are occupied.
		retained = &world.disruption_effects.electric_rays[0];
	}
	*retained = {};
	retained->active = true;
	retained->flags = flags;
	retained->displacement = displacement;
	retained->radius = radius;
	retained->intensity = 1.0f;
	retained->phase_on = true;
	// Electric_ray_create stores both intervals in 100 Hz gameplay ticks.
	retained->on_ticks = static_cast<std::uint32_t>(
		std::trunc(world_rand15(world) * kRandScale * 150.0f));
	retained->lifetime_ticks = lifetime_ticks;
	retained->owner_index = UINT16_MAX;
	retained->group_count = static_cast<std::int8_t>(
		std::clamp<std::int16_t>(group_count, 0, 5));
	for (ElectricRayGroup& group : retained->groups)
	{
		group = {};
		group.color = glm::vec3{1.0f};
		group.alpha = 0.5f;
	}
	return retained;
}

void electric_ray_set_parent(
	ElectricRayEffect& effect,
	std::uint16_t object_index,
	std::uint16_t object_generation,
	std::uint16_t model_reference)
{
	effect.owner_index = object_index;
	effect.owner_generation = object_generation;
	effect.parent_model_reference = model_reference;
}

void electric_ray_set_group_color(
	ElectricRayEffect& effect,
	std::int16_t group,
	const glm::vec3& color)
{
	if (group >= 0 && group < effect.group_count)
	{
		effect.groups[group].color = color;
	}
}

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
	std::uint32_t simulation_tick)
{
	for (Shockwave& wave : world.disruption_effects.shockwaves)
	{
		if (wave.active)
		{
			continue;
		}
		wave = {};
		wave.active = true;
		wave.position = position;
		wave.orientation = orientation;
		wave.velocity = velocity;
		wave.type = type;
		wave.final_radius = final_radius;
		wave.start_tick = simulation_tick;
		wave.lifetime_ticks = lifetime_ticks;
		wave.source_affiliation = source_affiliation;
		wave.source_object_index = source_object_index;
		diagnostics::mission_log(
			"shockwave create type=%u radius=%.0f lifetime=%.1f tick=%u",
			static_cast<unsigned>(type),
			final_radius,
			lifetime_ticks,
			simulation_tick);
		return true;
	}
	diagnostics::mission_log(
		"shockwave pool exhausted capacity=%u",
		static_cast<unsigned>(kMaxShockwaves));
	return false;
}

bool shockwave_create_object_destruction(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& ship_stats,
	std::uint16_t object_index,
	std::uint32_t simulation_tick)
{
	if (object_index >= kMaxGameObjects)
	{
		return false;
	}
	WorldObject& object = world.objects[object_index];
	if (!object.active)
	{
		return false;
	}
	// Object creation installs the dedicated Ulysses callback at 0x0046ea50
	// instead of Generic_destruction_sequence_start. The render-time
	// component boundary below supplies the affected model reference.
	if (object.type == 22)
	{
		return false;
	}
	const std::int32_t authored_lifetime =
		authored_destruction_lifetime(object.type);
	if (authored_lifetime == 0)
	{
		return true;
	}
	DisruptionEffectsRuntime& runtime = world.disruption_effects;
	if (runtime.destruction_shockwave_created[object_index]
		&& runtime.destruction_shockwave_generation[object_index]
			== object.generation)
	{
		return false;
	}
	std::uint8_t sequence_index = 0;
	const CapitalExplosionSequence* sequence =
		capital_explosion_sequence(object.type, &sequence_index);
	if (sequence == nullptr)
	{
		return true;
	}
	CapitalExplosionController* controller =
		allocate_explosion_controller(runtime);
	*controller = {};
	controller->active = true;
	controller->owner_index = object_index;
	controller->owner_generation = object.generation;
	controller->sequence_index = sequence_index;
	controller->start_tick = simulation_tick;
	controller->original_owner_position = object.position;
	controller->portal_position = object.position;
	controller->portal_orientation = object.orientation;
	if (object.type == 120)
	{
		world_spawn_capital_engine_breakaways(
			world, ship_stats, object, simulation_tick);
	}
	if (object.type == 168)
	{
		powercore_effect_release(
			world, {object_index, object.generation});
	}
	collect_explosion_schedule(object, *controller);
	for (std::size_t model_index = 0;
		model_index < object.model_references.size();
		++model_index)
	{
		const ObjectModelReference& model =
			object.model_references[model_index];
		if (model.owner_scope != 0 || model.parent_reference >= 0)
		{
			continue;
		}
		if ((model.source_flags & 0x0004u) != 0
			&& model.model_type == 1
			&& controller->selected_child < 0)
		{
			controller->selected_child =
				static_cast<std::int16_t>(model_index);
		}
		else if ((model.source_flags & 0x0004u) == 0)
		{
			if (!(object.type == 132 && model.part_group_id == 10))
			{
				// Generic_destruction_sequence_start,
				// 0x0046fc17..0x0046fc2e, reparents ordinary direct
				// models to portal1 for both compiled sequence modes.
				set_explosion_portal_subtree(
					object,
					static_cast<std::uint16_t>(model_index),
					1);
			}
			if (model.model_type == 1)
			{
				// Generic_destruction_sequence_start retains the last
				// ordinary direct type-one child at +0x34.
				controller->moving_anchor =
					static_cast<std::int16_t>(model_index);
			}
		}
	}
	// Mode one never advances the two portals: portal1 remains at the
	// constructor snapshot installed at 0x0046fd10..0x0046fd56 while the
	// ordinary hull is rendered beneath it.  Mode zero starts submitting its
	// portals from the first scheduled split at 0x00470a02..0x00470a1f.
	controller->portal_submitted = sequence->mode == 1;
	controller->portal_clipping = sequence->mode == 0;
	if (sequence->mode == 0 && controller->selected_child >= 0)
	{
		// 0x0046fbe1 calls Model_node_show_named before binding the selected
		// DEST candidate to portal two.  It is visible during the progressive
		// split, but only on the opposite side of the moving clip plane.
		object.model_references[
			static_cast<std::uint16_t>(controller->selected_child)]
			.runtime_flags &= ~0x00000020u;
		set_explosion_portal_subtree(
			object,
			static_cast<std::uint16_t>(controller->selected_child),
			2);
	}
	if (object.type == 129)
	{
		for (std::uint16_t reference = 0;
			reference < object.model_references.size();
			++reference)
		{
			const ObjectModelReference& model =
				object.model_references[reference];
			if (model.owner_scope == 0
				&& model.parent_reference < 0
				&& model.model_type == 1
				&& (model.source_flags & 0x0004u) != 0)
			{
				object.model_references[reference].runtime_flags &=
					~0x00000020u;
			}
		}
	}
	object.runtime_flags |= 0x00020048u;
	if (sequence->breakaway_object_type >= 0)
	{
		const ObjectHandle breakaway_handle = world_create(
			world,
			static_cast<std::uint16_t>(sequence->breakaway_object_type),
			object.position,
			object.orientation,
			ship_stats,
			false);
		if (WorldObject* breakaway =
			world_resolve(world, breakaway_handle))
		{
			controller->breakaway_index = breakaway_handle.index;
			controller->breakaway_generation = breakaway_handle.generation;
			synchronize_breakaway_object(*breakaway, object);
			breakaway->runtime_flags |= 0x00020448u;
			for (std::uint16_t reference = 0;
				reference < breakaway->primary_model_reference_count;
				++reference)
			{
				ObjectModelReference& model =
					breakaway->model_references[reference];
				if (model.parent_reference < 0
					&& model.model_type == 1
					&& (model.source_flags & 0x0004u) != 0)
				{
					// 0x0046fac6..0x0046fb2b exposes the compiled
					// breakaway candidate and binds its complete tree to
					// portal two before the progressive controller starts.
					model.runtime_flags &= ~0x00000020u;
					set_explosion_portal_subtree(
						*breakaway, reference, 2);
					break;
				}
			}
		}
	}

	// GameObject_explode_callback, LANCER.EXE
	// 0x0046fd5b..0x0046fd8e. The signed arithmetic sequence
	// (3 * authored - sign) >> 1 is truncation toward zero.
	const std::int32_t lifetime_ticks =
		authored_lifetime * 3 / 2;
	runtime.destruction_shockwave_generation[object_index] =
		object.generation;
	runtime.destruction_shockwave_created[object_index] = true;
	(void)shockwave_create(
		world,
		object.position,
		object.orientation,
		object.linear_velocity,
		7,
		object.radius * 2.0f,
		static_cast<float>(lifetime_ticks),
		0,
		object_index,
		simulation_tick);

	if (sequence->mode == 1 && !controller->schedule.empty())
	{
		// Generic_destruction_sequence_start, 0x0046fdb6..0x0046ffc0,
		// redraws the 5..7 loop limit after every opening burst.
		std::uint16_t burst = 0;
		while (burst < static_cast<std::uint16_t>(
			5u + world_rand15(world) % 3u))
		{
			create_mode_one_burst(
				world,
				*sequence,
				*controller,
				object,
				simulation_tick,
				nullptr,
				burst % 3u == 0u);
			++burst;
		}
	}

	// GameObject_explode_callback, LANCER.EXE
	// 0x0046ffc8..0x00470018. These three callback-owned objects award the
	// local attacker once when their type-seven destruction path is
	// accepted. Retail suppresses the player-kill chatter in every
	// multiplayer session.
	if (object.last_attacker_index == world.player.index
		&& (object.type == 0x3cu
			|| object.type == 0x46u
			|| object.type == 0x3eu))
	{
		(void)mission::runtime_add_player_score(
			mission,
			world,
			world.player.index,
			1,
			false);
		if (mission.network.role == mission::NetworkRole::offline)
		{
			mission::player_comms_on_player_destroyed_target(
				mission,
				world,
				ship_stats,
				object_index,
				simulation_tick,
				simulation_tick);
		}
		if (mission.network.role != mission::NetworkRole::offline)
		{
			mission::network_publish_player_stats(
				mission.network, world.player.index);
		}
	}
	// GameObject_explode_callback, LANCER.EXE 0x00470018..0x00470021.
	// This synchronous boundary is part of the accepted type-seven callback,
	// not the later progressive controller. It clears the Explode command and
	// publishes the whole-object Destroyed event before returning false to
	// suppress ordinary sibling-group removal.
	(void)world_mark_destroyed(world, mission, object);
	return false;
}

void disruption_effects_service(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& ship_stats,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick,
	std::uint32_t frame_ticks)
{
	for (ElectricRayEffect& effect :
		world.disruption_effects.electric_rays)
	{
		if (!effect.active)
		{
			continue;
		}
		if (effect.owner_index != UINT16_MAX
			&& (effect.owner_index >= kMaxGameObjects
				|| !world.objects[effect.owner_index].active
				|| world.objects[effect.owner_index].generation
					!= effect.owner_generation))
		{
			effect.active = false;
			continue;
		}
		const std::uint32_t delta_ticks =
			effect.last_tick == 0
				? 0
				: simulation_tick - effect.last_tick;
		if ((effect.flags & 0x04u) != 0)
		{
			effect.lifetime_ticks -=
				static_cast<std::int32_t>(delta_ticks);
			if (effect.lifetime_ticks <= 0)
			{
				effect.active = false;
				continue;
			}
		}
		if ((effect.flags & 0x01u) != 0)
		{
			if (effect.phase_on
				&& simulation_tick
					> effect.phase_start_tick + effect.on_ticks)
			{
				effect.phase_on = false;
				if ((effect.flags & 0x02u) == 0)
				{
					effect.intensity = 0.0f;
				}
				effect.off_ticks = static_cast<std::uint32_t>(
					std::trunc(
						world_rand15(world)
						* kRandScale * 10.0f));
				effect.phase_start_tick = simulation_tick;
			}
			else if (!effect.phase_on
				&& simulation_tick
					> effect.phase_start_tick + effect.off_ticks)
			{
				effect.phase_on = true;
				effect.intensity = 1.0f;
				effect.on_ticks = static_cast<std::uint32_t>(
					std::trunc(
						world_rand15(world)
						* kRandScale * 150.0f));
				effect.phase_start_tick = simulation_tick;
			}
			else if (!effect.phase_on
				&& (effect.flags & 0x02u) != 0)
			{
				effect.intensity = std::max(
					0.0f,
					effect.intensity
						- static_cast<float>(delta_ticks)
							* 0.3f);
			}
		}
		effect.last_tick = simulation_tick;
	}

	for (Shockwave& wave : world.disruption_effects.shockwaves)
	{
		if (!wave.active || wave.lifetime_ticks <= 0)
		{
			continue;
		}
		const float age = shockwave_age(wave, simulation_tick);
		if (age >= 1.0f)
		{
			wave.active = false;
			continue;
		}
		wave.position += wave.velocity
			* static_cast<float>(frame_ticks);
		const float current_radius = age * wave.final_radius;
		if (wave.type <= 2)
		{
			const WorldObject* player =
				world_resolve(world, world.player);
			if (player != nullptr
				&& shell_crossed(wave, *player, current_radius))
			{
				raise_camera_disturbance(world, age, true);
			}
		}
		else if (wave.type == 5 || wave.type == 6)
		{
			for (std::uint16_t object_index = 0;
				object_index < kMaxGameObjects;
				++object_index)
			{
				WorldObject& object = world.objects[object_index];
				if (!common_disruption_candidate(
						wave, object, simulation_tick)
					|| !shell_crossed(wave, object, current_radius))
				{
					continue;
				}
				object.shockwave_immunity_deadline =
					simulation_tick + 50;
				if (world.player.index == object_index
					&& world.player.generation == object.generation)
				{
					raise_camera_disturbance(
						world, age, false);
				}
				if (wave.type == 5)
				{
					apply_equipment_disruption(
						world, object, wave, age);
				}
				else
				{
					apply_shield_disruption(
						world,
						mission,
						object,
						object_index,
						wave,
						ship_stats,
						impact_feedback_enabled,
						simulation_tick);
				}
			}
		}
		else if (wave.type == 7 || wave.type == 8)
		{
			WorldObject* player =
				world_resolve(world, world.player);
			if (player != nullptr
				&& local_disruption_candidate(
					*player, simulation_tick)
				&& shell_crossed(
					wave, *player, current_radius))
			{
				raise_camera_disturbance(
					world, age, false);
				player->shockwave_immunity_deadline =
					simulation_tick + 50;
				apply_local_damage_wave(
					world,
					mission,
					*player,
					wave,
					ship_stats,
					age,
					impact_feedback_enabled,
					simulation_tick);
			}
		}
		wave.previous_radius = current_radius;
	}
}

void disruption_effects_service_explosion_controllers(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick)
{
	(void)mission;
	(void)ship_stats;
	for (CapitalExplosionController& controller
		: world.disruption_effects.explosion_controllers)
	{
		if (!controller.active)
		{
			continue;
		}
		WorldObject* owner = world_resolve(
			world,
			{controller.owner_index, controller.owner_generation});
		if (owner == nullptr)
		{
			controller = {};
			continue;
		}
		if (controller.ulysses)
		{
			service_ulysses_explosion_controller(
				world, controller, *owner, simulation_tick);
			continue;
		}
		const CapitalExplosionSequence& sequence =
			kCapitalExplosionSequences[controller.sequence_index];
		const std::uint32_t elapsed =
			simulation_tick - controller.start_tick;

		if (sequence.mode == 1)
		{
			if (owner->type == 69 && world_rand15(world) % 40u == 0)
			{
				world.player_exhaust_exposure_percent = 100;
			}
			if (elapsed > 100u
				&& elapsed < sequence.phase_end_tick
				&& world_rand15(world)
					% (owner->type == 69 ? 5u : 10u) == 0u
				&& !controller.schedule.empty())
			{
				create_mode_one_burst(
					world,
					sequence,
					controller,
					*owner,
					simulation_tick,
					nullptr,
					true);
			}
			if (elapsed < sequence.phase_end_tick)
			{
				continue;
			}
			for (const glm::vec3& point : controller.schedule)
			{
				create_mode_one_burst(
					world,
					sequence,
					controller,
					*owner,
					simulation_tick,
					&point,
					false);
			}
			finalize_capital_explosion(
				world,
				controller,
				*owner,
				sequence,
				simulation_tick);
			continue;
		}

		if (WorldObject* breakaway = world_resolve(
				world,
				{controller.breakaway_index,
				 controller.breakaway_generation}))
		{
			// Mode zero clears this service/render-suppression flag on every
			// callback, including the first callback after construction.
			breakaway->runtime_flags &= ~kObjectFlagDisabled;
		}
		if (elapsed < sequence.phase_end_tick
			&& controller.schedule_counter < controller.schedule.size())
		{
			const float progress = static_cast<float>(elapsed)
				* static_cast<float>(controller.schedule.size())
				/ static_cast<float>(sequence.phase_end_tick);
			while (controller.schedule_counter < controller.schedule.size()
				&& static_cast<float>(controller.schedule_counter) < progress)
			{
				const std::uint16_t counter =
					controller.schedule_counter;
				const std::size_t previous = counter == 0
					? 0u
					: static_cast<std::size_t>(counter - 1u);
				// 0x00470862..0x00470910 updates both hidden portals from
				// the preceding schedule point before the owner is shaken.
				// Retail also resolves the current point through the old
				// moving child here, but does not consume that result.
				controller.portal_position = scheduled_anchor_world_point(
					*owner, controller, controller.schedule[previous]);
				controller.portal_orientation = owner->orientation;
				controller.portal_submitted =
					static_cast<std::size_t>(counter + 1u)
						< controller.schedule.size()
					&& owner->type != 129;

				// 0x00470915..0x00470a2f: after retaining the previous
				// point as both portal positions, shake around the constructor
				// snapshot by independently choosing -10 or +10 per axis.
				glm::vec3 shaken = controller.original_owner_position;
				for (std::uint8_t axis = 0; axis < 3; ++axis)
				{
					shaken[axis] += explosion_random(world) - 0.5f < 0.0f
						? -10.0f
						: 10.0f;
				}
				owner->previous_position = shaken;
				owner->position = shaken;
				if (world_rand15(world) % 30u == 0u)
				{
					const std::int32_t remaining =
						static_cast<std::int32_t>(
							controller.schedule.size())
						- static_cast<std::int32_t>(counter) - 2;
					const std::int32_t midpoint =
						static_cast<std::int32_t>(counter)
						+ remaining / 2;
					const glm::vec3 extra_position = scheduled_world_point(
						*owner,
						controller.schedule[
							static_cast<std::size_t>(midpoint)]);
					const float final_size =
						sequence.final_billboard_size_override > 0.0f
							? sequence.final_billboard_size_override
							: owner->radius * 0.35f;
					create_capital_billboard(
						world,
						extra_position,
						final_size * 0.5f,
						0,
						simulation_tick);
					glm::vec3 direction =
						extra_position - owner->position;
					const float direction_length_before_rotation =
						glm::length(direction);
					if (direction_length_before_rotation > 0.0f)
					{
						direction /= direction_length_before_rotation;
					}
					const float z =
						(explosion_random(world) - 0.5f) * 4.5f;
					const float y =
						(explosion_random(world) - 0.5f) * 4.5f;
					const float x =
						(explosion_random(world) - 0.5f) * 4.5f;
					direction = math::rotation_from_euler({x, y, z})
						* direction;
					const float direction_length = glm::length(direction);
					if (direction_length > 0.0f)
					{
						direction /= direction_length;
					}
					const std::uint16_t extra_fragments =
						static_cast<std::uint16_t>(
							static_cast<float>(sequence.fragment_count)
							* static_cast<float>(controller.schedule.size())
							* 0.05f);
					particle_fragment_directional_burst(
						world,
						extra_position,
						direction,
						sequence.heavy_fragment_chance,
						sequence.fragment_mesh_scale,
						2.0f,
						extra_fragments,
						simulation_tick);
				}

				// 0x00470c28 recomputes the event point through the owner's
				// root after the shake above; it is deliberately distinct
				// from the portal position retained before the shake.
				const glm::vec3 point = scheduled_world_point(
					*owner, controller.schedule[counter]);
				create_capital_billboard(
					world,
					point,
					(0.75f + explosion_random(world) * 0.5f)
						* sequence.billboard_size_base,
					0,
					simulation_tick);
				glm::vec3 direction;
				if ((world_rand15(world) & 1u) != 0)
				{
					direction = -owner->orientation[2];
				}
				else
				{
					direction = point - owner->position;
					const float direction_length = glm::length(direction);
					if (direction_length > 0.0f)
					{
						direction /= direction_length;
					}
				}
				if (owner->type == 129)
				{
					glm::vec3 rock_direction = point - owner->position;
					const float rock_direction_length =
						glm::length(rock_direction);
					if (rock_direction_length > 0.0f)
					{
						rock_direction /= rock_direction_length;
					}
					for (std::uint8_t fragment = 0;
						fragment < sequence.fragment_count;
						++fragment)
					{
						rock_chunk_spawn_world(
							world,
							point,
							rock_direction,
							true,
							simulation_tick);
					}
					if (counter == 29 || counter == 35 || counter == 80)
					{
						world.player_exhaust_exposure_percent = 100;
					}
				}
				else
				{
					particle_fragment_directional_burst(
						world,
						point,
						direction,
						sequence.heavy_fragment_chance,
						sequence.fragment_mesh_scale,
						1.0f,
						sequence.fragment_count,
						simulation_tick);
				}
				++controller.schedule_counter;
				const std::int32_t interval = 15
					- static_cast<std::int32_t>(
						(explosion_random(world) - 0.5f) * -3.0f);
				if (controller.schedule_counter % interval == 0)
				{
					const std::uint8_t definition =
						(world_rand15(world) & 1u) == 0 ? 11 : 12;
					(void)explosion_random(world);
					world_queue_sound_explicit(
						world,
						point,
						owner->orientation[2],
						owner->linear_velocity,
						definition,
						3);
				}
			}
		}
		if (elapsed <= sequence.phase_end_tick)
		{
			continue;
		}
		finalize_capital_explosion(
			world,
			controller,
			*owner,
			sequence,
			simulation_tick);
	}
}

void disruption_effects_service_component_destruction_callbacks(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& ship_stats,
	std::uint32_t simulation_tick)
{
	// Object_model_update_scene invokes GameObject+0x614 during the
	// post-AI render traversal, immediately before ordinary sibling-group
	// destruction. Keep this separate from Shockwave_update_all: a newly
	// created type-seven wave first advances on the following gameplay
	// update, and an object damaged earlier in the frame receives its final
	// retail AI service before this callback marks it destroyed.
	for (std::uint16_t object_index = 0;
		object_index < kMaxGameObjects;
		++object_index)
	{
		WorldObject& object = world.objects[object_index];
		if (!object.active || !object.component_destruction_pending)
		{
			continue;
		}
		for (std::uint16_t model_index = 0;
			model_index < object.model_references.size();
			++model_index)
		{
			const ObjectModelReference& model =
				object.model_references[model_index];
			if (!model.removed
				&& model.health < 0.0f
				&& (model.runtime_flags & 0x00001030u) == 0
				&& model.model_type == 1)
			{
				if (object.type == 22)
				{
					(void)start_ulysses_component_destruction(
						world,
						mission,
						ship_stats,
						object_index,
						model_index,
						simulation_tick);
				}
				else
				{
					(void)shockwave_create_object_destruction(
						world,
						mission,
						ship_stats,
						object_index,
						simulation_tick);
				}
				break;
			}
		}
	}
}
}
