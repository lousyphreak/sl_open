#include "game/world.hpp"

#include "ai/runtime.hpp"
#include "ai/scripted_commands.hpp"
#include "assets/gameplay_model.hpp"
#include "assets/player_ship.hpp"
#include "core/mission_log.hpp"
#include "game/attachments.hpp"
#include "game/exhaust_hazard.hpp"
#include "game/model_animation.hpp"
#include "mission/runtime.hpp"
#include "mission/deathmatch_scenarios.hpp"
#include "mission/events.hpp"
#include "mission/player_comms.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sl_open::game
{
namespace
{
bool type_has_planet_atmosphere(std::uint16_t type)
{
	switch (type)
	{
	case 0x5f:
	case 0x61:
	case 0x64:
	case 0x69:
	case 0xc9:
	case 0xcb:
	case 0xce:
	case 0xd3:
		return true;
	default:
		return false;
	}
}

void register_planet_atmosphere(
	World& world,
	ObjectHandle owner)
{
	if (world.atmosphere_count >= kPlanetAtmosphereCapacity)
	{
		if (!world.atmosphere_overflow_reported)
		{
			diagnostics::mission_log(
				"planet atmosphere registry exhausted capacity=%u",
				static_cast<unsigned>(kPlanetAtmosphereCapacity));
			world.atmosphere_overflow_reported = true;
		}
		return;
	}
	world.atmospheres[world.atmosphere_count++] = {owner, 0.0f};
}

std::uint16_t hud_icon_for_type(std::uint16_t type)
{
	// LANCER.EXE 0x004f8890..0x004f88ef is the exact 24-entry
	// type-to-primary-HUD-shape table consumed while the class-zero
	// (player-wing) object list is built at 0x00493c6d.
	constexpr struct
	{
		std::uint16_t type;
		std::uint16_t shape;
	} mappings[] = {
		{1, 250}, {5, 251}, {0, 252}, {10, 253},
		{8, 254}, {3, 255}, {6, 256}, {2, 257},
		{4, 258}, {7, 259}, {11, 260}, {9, 261},
		{244, 252}, {245, 250}, {246, 257}, {247, 255},
		{248, 258}, {249, 251}, {250, 256}, {251, 259},
		{252, 254}, {253, 261}, {254, 253}, {255, 260},
	};
	for (const auto& mapping : mappings)
	{
		if (mapping.type == type)
		{
			return mapping.shape;
		}
	}
	return 0;
}

float signed_square_root(float value)
{
	return value >= 0.0f
		? std::sqrt(value)
		: -std::sqrt(-value);
}

const char* cloak_phase_name(CloakPhase phase)
{
	switch (phase)
	{
	case CloakPhase::none: return "none";
	case CloakPhase::cloaking: return "cloaking";
	case CloakPhase::cloaked: return "cloaked";
	case CloakPhase::decloaking: return "decloaking";
	}
	return "unknown";
}

}

namespace
{
std::uint16_t advance_random(std::uint32_t& seed)
{
	seed = seed * 0x343fdu + 0x269ec3u;
	return static_cast<std::uint16_t>((seed >> 16) & 0x7fffu);
}

bool handle_matches(
	ObjectHandle left,
	ObjectHandle right)
{
	return left.index == right.index
		&& left.generation == right.generation;
}

void release_object_owned_runtime(
	World& world,
	ObjectHandle owner,
	WorldObject& object)
{
	// GameObject_destroy tears down object-owned retained state before
	// replacing the allocation. The port's effect pools are shared owners,
	// so explicitly remove records whose retail pointer lived on this object.
	attachments_destroy_live_models(object);
	world.shields.sphere[owner.index] = {};
	for (CapShieldSlot& cap : world.shields.cap)
	{
		if (cap.object_index == owner.index
			&& cap.object_generation == owner.generation)
		{
			cap = {};
		}
	}
	for (ElectricRayEffect& ray
		: world.disruption_effects.electric_rays)
	{
		if (ray.owner_index == owner.index
			&& ray.owner_generation == owner.generation)
		{
			ray = {};
		}
	}
	for (ParticleEmitter& emitter : world.particles.emitters)
	{
		if (emitter.model_owned
			&& emitter.owner_index == owner.index
			&& emitter.owner_generation == owner.generation)
		{
			particle_emitter_release_automatic(emitter);
		}
	}
	transition_effects_release_owner(
		world.transition_effects, owner);
}
}

void world_reset(World& world, std::uint32_t random_seed)
{
	for (std::uint16_t index = 0; index < kMaxGameObjects; ++index)
	{
		WorldObject& object = world.objects[index];
		if (object.active)
		{
			release_object_owned_runtime(
				world,
				{index, object.generation},
				object);
		}
		const std::uint16_t generation = object.generation;
		object = {};
		object.generation = static_cast<std::uint16_t>(generation + 1);
	}
	world.player = {};
	world.player_cloak_control_active = false;
	shields_reset(world.shields);
	disruption_effects_reset(world.disruption_effects);
	world.action_center = {};
	world.action_center_radius = 220000.0f;
	world.selected_target = {};
	world.target_component = -1;
	std::fill(
		std::begin(world.player_schematic_hits),
		std::end(world.player_schematic_hits),
		std::uint16_t{0});
	std::fill(
		std::begin(world.target_schematic_hits),
		std::end(world.target_schematic_hits),
		std::uint16_t{0});
	world.smart_target_enabled = false;
	world.damage_event_suppressed = false;
	world.player_inside_type45_compound = false;
	world.target_panel_refresh_requested = false;
	world.player_hit_distortion_triggers = 0;
	world.player_camera_disturbance = 0.0f;
	world.player_exhaust_exposure_percent = 0;
	world.player_in_exhaust = false;
	world.camera_mode = 0;
	world.player_prefix_count = 1;
	world.multiplayer_spectator_target = 0;
	world.multiplayer_spectator_active = false;
	world.cinematic_mode = 0;
	death_effects_reset(world.death_effects);
	world.live_count = 0;
	world.mission_slot_count = 0;
	world.object_high_water = 0;
	world.mission_event_count = 0;
	world.exhaust_hazard_count = 0;
	world.exhaust_hazard_registry_valid = false;
	std::fill(
		std::begin(world.atmospheres),
		std::end(world.atmospheres),
		PlanetAtmosphere{});
	world.atmosphere_count = 0;
	world.sound_read = 0;
	world.sound_count = 0;
	world.ai_threat_clear_deadline = 0;
	world.mission_event_overflow_reported = false;
	world.atmosphere_overflow_reported = false;
	world.random_seed = random_seed;
	world.next_creation_serial = 0;
	if (!world.particles.initialized
		&& !particle_system_initialize(world.particles, 2))
	{
		return;
	}
	particle_system_reset(world.particles);
	if (!world.transition_effects.jump.initialized
		&& !transition_effects_initialize(world.transition_effects, 2))
	{
		return;
	}
	transition_effects_reset(world.transition_effects);
}

void world_reserve_mission_slots(
	World& world,
	std::uint16_t count)
{
	world.mission_slot_count = std::min<std::uint16_t>(
		count,
		static_cast<std::uint16_t>(kMaxGameObjects));
	world.object_high_water = std::max(
		world.object_high_water,
		world.mission_slot_count);
}

namespace
{
ObjectHandle create_at_index(
	World& world,
	std::uint16_t index,
	std::uint16_t type,
	const glm::vec3& position,
	const glm::mat3& orientation,
	const assets::ShipStatsTable& stats,
	bool player,
	std::uint16_t mission_index,
	std::uint8_t group,
	std::uint8_t pilot)
{
	if (!stats.ready || index >= kMaxGameObjects
		|| world.objects[index].active)
	{
		return {};
	}
	WorldObject& object = world.objects[index];
	world.object_high_water = std::max<std::uint16_t>(
		world.object_high_water,
		static_cast<std::uint16_t>(index + 1u));
	object.active = true;
	object.visible = true;
	object.player_slot = player;
	object.player = player;
	object.type = type;
	object.cloak_supported =
		type < std::size(world.model_cloak_supported_by_type)
		&& world.model_cloak_supported_by_type[type];
	object.blindfire_supported =
		player && assets::player_ship_has_capability(
			type, assets::PlayerShipCapability::blind_fire);
	object.blindfire_enabled = object.blindfire_supported;
	object.spectral_supported =
		player && assets::player_ship_has_capability(
			type, assets::PlayerShipCapability::spectral_shields);
	object.hud_icon = hud_icon_for_type(type);
	object.mission_index = mission_index;
	object.group = group;
	object.pilot = pilot;
	object.random_seed = advance_random(world.random_seed);
	// GameObject_construct (LANCER.EXE 0x0046fb60) stores
	// truncate(rand()*(1/32768)*100) at +0x634. The retained object
	// seed is the construction draw in this deterministic runtime.
	object.random_phase = static_cast<std::uint16_t>(
		(object.random_seed * 100u) >> 15);
	object.ejection_roll =
		static_cast<std::uint32_t>(
			advance_random(world.random_seed) % 100u);
	object.creation_serial = ++world.next_creation_serial;
	// GameObject construction at LANCER.EXE 0x00467505 installs the
	// fixed offline inventory of 29 countermeasures.
	object.chaff_count = 29;
	object.previous_position = position;
	object.position = position;
	object.previous_orientation = orientation;
	object.orientation = orientation;
	object.scene_position = position;
	object.scene_orientation = orientation;
	if (type < assets::kShipStatsCount)
	{
		const assets::ObjectTypeStats& object_stats =
			stats.records[type].object;
		object.afterburner_fuel =
			object_stats.afterburner_seconds * 100;
		object.ammunition = object_stats.ammunition;
		object.gun_energy = object_stats.gun_energy_max;
		object.collision_class = object_stats.collision_class;
		object.allegiance_class = object_stats.allegiance_class;
		object.object_class = object_stats.object_class;
		object.targetable = object_stats.targetable_capability;
		if (object.targetable)
		{
			object.runtime_flags |= 0x200u;
		}
		object.hostile = object.allegiance_class == 1;
		const float primary =
			static_cast<float>(object_stats.primary_bank_max * 6 - 1);
		const float secondary =
			static_cast<float>(
				object_stats.structural_bank_max * 6 - 1);
		std::fill(
			std::begin(object.primary_shields),
			std::end(object.primary_shields),
			primary);
		std::fill(
			std::begin(object.secondary_shields),
			std::end(object.secondary_shields),
			secondary);
	}
	else
	{
		// GameObject_create_runtime's >255 fast path installs the retained
		// placeholder flag set without binding ship statistics or resources.
		// Mission anchor records 995/996/997/999 arrive here as type 1000.
		object.runtime_flags |= 0x3cu;
		object.radius = 4000.0f;
	}
	++world.live_count;
	const ObjectHandle handle{index, object.generation};
	if (player)
	{
		world.player = handle;
		world.action_center = handle;
	}
	if (type_has_planet_atmosphere(type))
	{
		register_planet_atmosphere(world, handle);
	}
	return handle;
}
}

ObjectHandle world_create(
	World& world,
	std::uint16_t type,
	const glm::vec3& position,
	const glm::mat3& orientation,
	const assets::ShipStatsTable& stats,
	bool player,
	std::uint16_t mission_index,
	std::uint8_t group,
	std::uint8_t pilot)
{
	// DAT_0057e04e is the dedicated final object slot used by player
	// launch/landing scenes and ejection outcomes. Objects_service_render
	// substitutes it after the ordinary [0, DAT_00539aa0) range; it is not
	// part of the general runtime allocation cursor.
	for (std::uint16_t index = world.mission_slot_count;
		index + 1u < kMaxGameObjects;
		++index)
	{
		if (!world.objects[index].active)
		{
			return create_at_index(
				world, index, type, position, orientation, stats,
				player, mission_index, group, pilot);
		}
	}
	return {};
}

ObjectHandle world_create_at(
	World& world,
	std::uint16_t index,
	std::uint16_t type,
	const glm::vec3& position,
	const glm::mat3& orientation,
	const assets::ShipStatsTable& stats,
	bool player,
	std::uint16_t mission_index,
	std::uint8_t group,
	std::uint8_t pilot)
{
	return create_at_index(
		world,
		index,
		type,
		position,
		orientation,
		stats,
		player,
		mission_index,
		group,
		pilot);
}

ObjectHandle world_recreate(
	World& world,
	ObjectHandle existing,
	std::uint16_t type,
	const glm::vec3& position,
	const glm::mat3& orientation,
	const assets::ShipStatsTable& stats,
	bool player,
	std::uint16_t mission_index,
	std::uint8_t group,
	std::uint8_t pilot)
{
	if (world_resolve(world, existing) == nullptr
		|| !world_destroy(world, existing))
	{
		return {};
	}
	return create_at_index(
		world,
		existing.index,
		type,
		position,
		orientation,
		stats,
		player,
		mission_index,
		group,
		pilot);
}

bool world_mark_departed(World& world, ObjectHandle handle)
{
	WorldObject* object = world_resolve(world, handle);
	if (object == nullptr)
	{
		return false;
	}
	// GameObject_mark_departed_placeholder, LANCER.EXE 0x004688e0.
	// This deliberately preserves the allocation, generation, model/resource
	// ownership, and live high-water slot. It then drains command-owned
	// resources through the ordinary AI clear path. A later CreateFlightGroup
	// performs the full destroy/recreate boundary at this same explicit index.
	object->type = 1001;
	object->targetable = false;
	object->runtime_flags =
		(object->runtime_flags | 0x7cu) & ~0x200u;
	ai::command_clear(world, *object);
	object->external_trail_active = false;
	return true;
}

bool world_mark_destroyed(
	World& world,
	mission::Runtime& mission,
	WorldObject& object)
{
	if (!object.active)
	{
		return false;
	}
	// GameObject_mark_destroyed, LANCER.EXE 0x00401f00. Unlike the Explode
	// command family, this component-owned boundary is synchronous: perform
	// the command-11 replacement cleanup, discard the queue, set the dead
	// TargetRef bit, and publish the whole-object Destroyed event immediately.
	ai::command_mark_destroyed(world, object);
	object.runtime_flags |= kObjectFlagDestroyed;
	if (object.mission_index < mission.object_count)
	{
		std::uint16_t attacker = UINT16_MAX;
		if (object.last_attacker_index < kMaxGameObjects)
		{
			const WorldObject& live_attacker =
				world.objects[object.last_attacker_index];
			if (live_attacker.active)
			{
				attacker = live_attacker.mission_index;
			}
		}
		(void)mission::events_emit_destroyed(
			mission, object.mission_index, attacker);
	}
	return true;
}

std::uint16_t world_object_rand15(WorldObject& object)
{
	return advance_random(object.random_seed);
}

std::uint16_t world_rand15(World& world)
{
	// The retail visual constructors call the process CRT rand() stream.
	// World::random_seed is the retained equivalent: object construction
	// already consumes this same MSVC recurrence before publishing each
	// object's private seed.
	return advance_random(world.random_seed);
}

namespace
{
void clear_cloak_vertex_state(WorldObject& object)
{
	for (ObjectModelReference& reference : object.model_references)
	{
		std::fill(
			reference.cloak.hit_alpha.begin(),
			reference.cloak.hit_alpha.end(),
			0.0f);
		std::fill(
			reference.cloak.secondary_uv.begin(),
			reference.cloak.secondary_uv.end(),
			glm::vec2{0.0f});
		reference.cloak.initialized_secondary_vertices = 0;
		reference.cloak.installed = false;
	}
	for (AttachmentSlot& attachment : object.attachments)
	{
		attachment.cloak_install_pending = false;
		for (CloakMeshRuntime& cloak : attachment.cloak_models)
		{
			std::fill(
				cloak.hit_alpha.begin(), cloak.hit_alpha.end(), 0.0f);
			std::fill(
				cloak.secondary_uv.begin(),
				cloak.secondary_uv.end(),
				glm::vec2{0.0f});
			cloak.initialized_secondary_vertices = 0;
			cloak.installed = false;
		}
	}
	object.cloak_meshes_initialized = false;
}

void initialize_secondary_mesh(
	World& world,
	CloakMeshRuntime& cloak)
{
	std::fill(
		cloak.hit_alpha.begin(), cloak.hit_alpha.end(), 0.0f);
	std::fill(
		cloak.secondary_uv.begin(),
		cloak.secondary_uv.end(),
		glm::vec2{0.0f});
	cloak.initialized_secondary_vertices = 0;
	cloak.installed = cloak.eligible
		&& cloak.selected_lod < cloak.normal_lods.size()
		&& cloak.normal_lods[cloak.selected_lod] != nullptr;
	if (!cloak.installed
		|| cloak.selected_lod >= cloak.secondary_lods.size()
		|| cloak.secondary_lods[cloak.selected_lod] == nullptr)
	{
		return;
	}
	const std::size_t vertex_count =
		cloak.secondary_lods[cloak.selected_lod]->size();
	if (vertex_count > cloak.secondary_uv.size())
	{
		return;
	}
	for (std::size_t vertex = 0; vertex < vertex_count; ++vertex)
	{
		cloak.secondary_uv[vertex] = {
			static_cast<float>(world_rand15(world)) / 32767.0f,
			static_cast<float>(world_rand15(world)) / 32767.0f,
		};
	}
	cloak.initialized_secondary_vertices =
		static_cast<std::uint32_t>(vertex_count);
}

void service_cloak_mesh(
	CloakMeshRuntime& cloak,
	float elapsed_ticks,
	bool decay_hit_alpha,
	bool rotate_secondary)
{
	if (!cloak.installed)
	{
		return;
	}
	if (cloak.selected_lod < cloak.normal_lods.size()
		&& cloak.normal_lods[cloak.selected_lod] != nullptr
		&& decay_hit_alpha)
	{
		const std::size_t vertex_count = std::min(
			cloak.hit_alpha.size(),
			cloak.normal_lods[cloak.selected_lod]->size());
		const float decay = elapsed_ticks * 0.01f;
		for (std::size_t vertex = 0; vertex < vertex_count; ++vertex)
		{
			if (cloak.hit_alpha[vertex] > 0.0f)
			{
				cloak.hit_alpha[vertex] = std::max(
					0.0f, cloak.hit_alpha[vertex] - decay);
			}
		}
	}
	if (!rotate_secondary
		|| cloak.selected_lod >= cloak.secondary_lods.size()
		|| cloak.secondary_lods[cloak.selected_lod] == nullptr)
	{
		return;
	}
	const std::size_t vertex_count = std::min({
		cloak.secondary_uv.size(),
		cloak.secondary_lods[cloak.selected_lod]->size(),
		static_cast<std::size_t>(
			cloak.initialized_secondary_vertices),
	});
	for (std::size_t vertex = 0; vertex < vertex_count; ++vertex)
	{
		const glm::vec2 source = cloak.secondary_uv[vertex];
		const float denominator = glm::dot(source, source);
		const float angle =
			elapsed_ticks * 0.008f / denominator;
		const float cosine = std::cos(angle);
		const float sine = std::sin(angle);
		cloak.secondary_uv[vertex] = {
			cosine * source.x - sine * source.y,
			sine * source.x + cosine * source.y,
		};
	}
}

void begin_cloak_transition(
	World& world,
	WorldObject& object,
	bool active,
	std::uint32_t simulation_tick)
{
	object.cloak_transition_tick = simulation_tick;
	object.cloak_last_update_tick = simulation_tick;
	if (active)
	{
		clear_cloak_vertex_state(object);
		object.runtime_flags |= 0x00000100u;
		object.cloak_phase = CloakPhase::cloaking;
		object.cloak_phase_value = 0.0f;
		object.cloak_normal_alpha = 1.0f;
		world_emit_mission_event(
			world,
			WorldMissionEventType::cloaked,
			object,
			nullptr,
			UINT8_MAX);
		world_initialize_cloak_meshes(world, object);
	}
	else
	{
		// Decloaking retains flag 0x100 for the complete 250-tick
		// (2.5-second)
		// transition. Targeting and weapons therefore continue to treat the
		// object as cloaked until the transition owner destroys its state.
		object.cloak_phase = CloakPhase::decloaking;
		object.cloak_phase_value = 1.0f;
		object.cloak_normal_alpha = 0.0f;
		world_emit_mission_event(
			world,
			WorldMissionEventType::decloaked,
			object,
			nullptr,
			UINT8_MAX);
	}
	world_queue_sound_object(
		world,
		{static_cast<std::uint16_t>(
			&object - std::begin(world.objects)), object.generation},
		43,
		static_cast<std::uint8_t>(
			&object - std::begin(world.objects) == world.player.index
				? 2 : 0));
	diagnostics::mission_log(
		"cloak transition actor=%u phase=%s tick=%u",
		static_cast<unsigned>(object.mission_index),
		cloak_phase_name(object.cloak_phase),
		simulation_tick);
}
}

void world_initialize_cloak_meshes(
	World& world,
	WorldObject& object)
{
	if (!object.active
		|| (object.runtime_flags & 0x00000100u) == 0
		|| object.cloak_phase == CloakPhase::none
		|| object.cloak_meshes_initialized
		|| object.cloak_model_order.empty())
	{
		return;
	}
	for (const std::uint16_t model_index : object.cloak_model_order)
	{
		if (model_index < object.model_references.size())
		{
			initialize_secondary_mesh(
				world, object.model_references[model_index].cloak);
		}
	}
	// Mounted hardpoint objects are appended as children of their owning
	// model node. Their retained model arrays are already in recursive source
	// order when the renderer publishes them.
	for (AttachmentSlot& attachment : object.attachments)
	{
		if (!attachment.live_model)
		{
			continue;
		}
		if (attachment.cloak_model_order.empty())
		{
			// The retained renderer may not have published a rebuilt child
			// model yet. Remember that it was present in the recursive scene
			// walk at cloak installation time; children created after this
			// point must retain their ordinary callbacks.
			attachment.cloak_install_pending = true;
			continue;
		}
		for (const std::uint16_t model_index
			: attachment.cloak_model_order)
		{
			if (model_index < attachment.cloak_models.size())
			{
				initialize_secondary_mesh(
					world, attachment.cloak_models[model_index]);
			}
		}
		attachment.cloak_install_pending = false;
	}
	object.cloak_meshes_initialized = true;
}

void world_initialize_cloak_attachment(
	World& world,
	AttachmentSlot& attachment)
{
	for (const std::uint16_t model_index
		: attachment.cloak_model_order)
	{
		if (model_index < attachment.cloak_models.size())
		{
			initialize_secondary_mesh(
				world, attachment.cloak_models[model_index]);
		}
	}
	attachment.cloak_install_pending = false;
}

bool world_set_cloak_active(
	World& world,
	WorldObject& object,
	bool active,
	std::uint32_t simulation_tick)
{
	// Cloak_set_active (LANCER.EXE 0x00463560) first compares the live
	// gameplay flag with the requested Boolean. A matching state is a no-op,
	// including an "on" request during decloaking while flag 0x100 remains
	// set. A differing request calls the busy-sensitive Cloak_toggle, then
	// still publishes the requested local state and forwards it to children.
	if (!object.active || !object.cloak_supported
		|| ((object.runtime_flags & 0x00000100u) != 0) == active)
	{
		return false;
	}
	const bool transition_started =
		object.cloak_phase != CloakPhase::cloaking
		&& object.cloak_phase != CloakPhase::decloaking;
	if (transition_started)
	{
		begin_cloak_transition(
			world, object, active, simulation_tick);
	}
	const ObjectHandle object_handle{
		static_cast<std::uint16_t>(
			&object - std::begin(world.objects)),
		object.generation,
	};
	if (handle_matches(world.player, object_handle))
	{
		world.player_cloak_control_active = active;
	}

	// The retail owner recursively forwards every differing request to live
	// launch children whose current command 104 targets this object.
	for (WorldObject& candidate : world.objects)
	{
		if (!candidate.active
			|| candidate.ai.command_count == 0
			|| candidate.ai.commands[0].id != 104
			|| candidate.ai.commands[0].target_kind
				!= ai::TargetKind::object
			|| candidate.ai.commands[0].target
				!= object.mission_index)
		{
			continue;
		}
		world_set_cloak_active(
			world, candidate, active, simulation_tick);
	}
	return transition_started;
}

bool world_force_cloak(
	World& world,
	WorldObject& object,
	std::uint32_t simulation_tick)
{
	// DMPowerup_Cloak_pickup and HuntTheShadow_set_holder call
	// Cloak_begin_cloak (LANCER.EXE 0x00463640) directly rather than the
	// flag-comparing public toggle. That entry point does not inspect an
	// existing cloak flag or transition before installing a fresh state.
	if (!object.active)
	{
		return false;
	}
	begin_cloak_transition(world, object, true, simulation_tick);
	return true;
}

bool world_force_decloak(
	World& world,
	WorldObject& object,
	std::uint32_t simulation_tick)
{
	// Manual eject calls Cloak_begin_decloak directly at
	// LANCER.EXE 0x00413e1d..0x00413e27. That owner restarts the retained
	// decloak callback even while the ordinary cloak toggle would reject a
	// busy transition.
	if (!object.active
		|| (object.runtime_flags & 0x00000100u) == 0
		|| object.cloak_phase == CloakPhase::none)
	{
		return false;
	}
	begin_cloak_transition(
		world, object, false, simulation_tick);
	return true;
}

void world_service_cloaks(
	World& world,
	std::uint32_t simulation_tick)
{
	for (WorldObject& object : world.objects)
	{
		if (!object.active
			|| object.cloak_phase == CloakPhase::none)
		{
			continue;
		}
		world_initialize_cloak_meshes(world, object);
		const std::uint32_t update_elapsed_ticks =
			simulation_tick - object.cloak_last_update_tick;
		object.cloak_last_update_tick = simulation_tick;
		const auto service_meshes = [&](
			float elapsed_ticks,
			bool decay_hit_alpha,
			bool rotate_secondary)
		{
			for (ObjectModelReference& reference : object.model_references)
			{
				service_cloak_mesh(
					reference.cloak,
					elapsed_ticks,
					decay_hit_alpha,
					rotate_secondary);
			}
			for (AttachmentSlot& attachment : object.attachments)
			{
				for (CloakMeshRuntime& cloak : attachment.cloak_models)
				{
					service_cloak_mesh(
						cloak,
						elapsed_ticks,
						decay_hit_alpha,
						rotate_secondary);
				}
			}
		};
		if (object.cloak_phase == CloakPhase::cloaked)
		{
			service_meshes(
				static_cast<float>(update_elapsed_ticks),
				true,
				object.type != 0x95u);
			continue;
		}
		// Cloak_update subtracts DAT_005883b0, the retail 100 Hz gameplay
		// clock, compares the result with 250, and scales it by 0.004.
		const std::uint32_t elapsed_ticks =
			simulation_tick - object.cloak_transition_tick;
		const float transition =
			std::min(1.0f, static_cast<float>(elapsed_ticks) * 0.004f);
		const bool cloaking =
			object.cloak_phase == CloakPhase::cloaking;
		if (elapsed_ticks >= 250)
		{
			if (cloaking)
			{
				// At cloak completion 0x00463946 supplies the complete
				// 250-tick value to both recursive mesh updates. This is in
				// addition to the preceding per-frame secondary rotations.
				service_meshes(250.0f, true, true);
				// The render callback still runs after Cloak_update publishes
				// the final phase. Its elapsed interval is separate from the
				// explicit 250-tick completion update above.
				object.cloak_phase = CloakPhase::cloaked;
				object.cloak_phase_value = 1.0f;
				object.cloak_normal_alpha = 0.0f;
				service_meshes(
					static_cast<float>(update_elapsed_ticks),
					true,
					object.type != 0x95u);
			}
			else
			{
				// Decloak completion restores the ordinary material/alpha
				// streams and destroys the cloak state without a final
				// secondary update.
				clear_cloak_vertex_state(object);
				object.cloak_phase = CloakPhase::none;
				object.runtime_flags &= ~0x00000100u;
				object.cloak_phase_value = 0.0f;
				object.cloak_normal_alpha = 1.0f;
			}
			diagnostics::mission_log(
				"cloak transition actor=%u phase=%s tick=%u",
				static_cast<unsigned>(object.mission_index),
				cloak_phase_name(object.cloak_phase),
				simulation_tick);
			continue;
		}
		if (cloaking)
		{
			object.cloak_phase_value = transition;
			object.cloak_normal_alpha = 1.0f - transition;
		}
		else
		{
			object.cloak_phase_value = 1.0f - transition;
			object.cloak_normal_alpha = transition;
		}
		service_meshes(
			static_cast<float>(update_elapsed_ticks),
			object.cloak_normal_alpha == 0.0f,
			object.type != 0x95u
				&& object.cloak_phase_value != 0.0f);
	}
}

void world_register_cloak_hit(
	WorldObject& object,
	const glm::vec3& position,
	std::uint32_t simulation_tick)
{
	if (!object.active
		|| (object.runtime_flags & 0x00000100u) == 0)
	{
		return;
	}
	object.cloak_last_hit_position = position;
	object.cloak_last_hit_tick = simulation_tick;
	for (const std::uint16_t model_index : object.cloak_model_order)
	{
		if (model_index >= object.model_references.size())
		{
			continue;
		}
		ObjectModelReference& reference =
			object.model_references[model_index];
		CloakMeshRuntime& cloak = reference.cloak;
		if (!cloak.installed
			|| cloak.selected_lod >= cloak.normal_lods.size()
			|| cloak.normal_lods[cloak.selected_lod] == nullptr)
		{
			continue;
		}
		const std::vector<assets::GameplayVertex>& vertices =
			*cloak.normal_lods[cloak.selected_lod];
		const std::size_t vertex_count = std::min(
			vertices.size(), cloak.hit_alpha.size());
		float radius = object.radius;
		if (object.type == 0x95u
			&& cloak.selected_lod < cloak.lod_bounds_min.size()
			&& cloak.selected_lod < cloak.lod_bounds_max.size())
		{
			const glm::vec3 extent =
				cloak.lod_bounds_max[cloak.selected_lod]
					- cloak.lod_bounds_min[cloak.selected_lod];
			radius = std::max(extent.x, std::max(extent.y, extent.z));
		}
		if (!(radius > 0.0f))
		{
			continue;
		}
		const float outer = radius * 1.1230000257492065f;
		const float falloff = radius * 0.44999998807907104f;
		for (std::size_t vertex = 0; vertex < vertex_count; ++vertex)
		{
			const assets::GameplayVertex& source = vertices[vertex];
			const glm::vec3 object_position = glm::vec3(
				reference.local_transform
					* glm::vec4(source.x, source.y, source.z, 1.0f));
			const glm::vec3 world_position =
				object.position + object.orientation * object_position;
			const float distance = glm::distance(world_position, position);
			if (distance >= outer)
			{
				continue;
			}
			cloak.hit_alpha[vertex] = std::min(
				1.0f,
				cloak.hit_alpha[vertex]
					+ std::min(1.0f, (outer - distance) / falloff));
		}
	}
	// Hardpoint models are live child scene objects too. Their cloak meshes
	// receive the same recursive hit callback after the owning tag-1 node.
	for (AttachmentSlot& attachment : object.attachments)
	{
		if (!attachment.live_model || attachment.cloak_models.empty())
		{
			continue;
		}
		glm::mat4 hardpoint_transform =
			glm::translate(
				glm::mat4{1.0f}, attachment.local_position)
				* glm::mat4(attachment.local_orientation);
		if (attachment.model_reference >= 0
			&& static_cast<std::size_t>(attachment.model_reference)
				< object.model_references.size())
		{
			hardpoint_transform =
				object.model_references[
					attachment.model_reference].local_transform
					* attachment.hardpoint_from_model;
		}
		hardpoint_transform = hardpoint_transform
			* glm::mat4(sl_open::math::rotation_from_euler(
				{0.0f, -glm::pi<float>(), 0.0f}));
		for (const std::uint16_t model_index
			: attachment.cloak_model_order)
		{
			if (model_index >= attachment.cloak_models.size())
			{
				continue;
			}
			CloakMeshRuntime& cloak =
				attachment.cloak_models[model_index];
			if (!cloak.installed
				|| cloak.selected_lod >= cloak.normal_lods.size()
				|| cloak.normal_lods[cloak.selected_lod] == nullptr)
			{
				continue;
			}
			const std::vector<assets::GameplayVertex>& vertices =
				*cloak.normal_lods[cloak.selected_lod];
			const std::size_t vertex_count = std::min(
				vertices.size(), cloak.hit_alpha.size());
			float radius = object.radius;
			if (object.type == 0x95u
				&& cloak.selected_lod < cloak.lod_bounds_min.size()
				&& cloak.selected_lod < cloak.lod_bounds_max.size())
			{
				const glm::vec3 extent =
					cloak.lod_bounds_max[cloak.selected_lod]
						- cloak.lod_bounds_min[cloak.selected_lod];
				radius = std::max(
					extent.x, std::max(extent.y, extent.z));
			}
			if (!(radius > 0.0f))
			{
				continue;
			}
			const float outer = radius * 1.1230000257492065f;
			const float falloff = radius * 0.44999998807907104f;
			const glm::mat4 model_transform =
				hardpoint_transform * cloak.local_transform;
			for (std::size_t vertex = 0; vertex < vertex_count; ++vertex)
			{
				const assets::GameplayVertex& source = vertices[vertex];
				const glm::vec3 object_position = glm::vec3(
					model_transform
						* glm::vec4(
							source.x, source.y, source.z, 1.0f));
				const glm::vec3 world_position =
					object.position + object.orientation * object_position;
				const float distance =
					glm::distance(world_position, position);
				if (distance >= outer)
				{
					continue;
				}
				cloak.hit_alpha[vertex] = std::min(
					1.0f,
					cloak.hit_alpha[vertex]
						+ std::min(
							1.0f, (outer - distance) / falloff));
			}
		}
	}
}

bool world_pop_sound(
	World& world,
	WorldSoundEvent& event)
{
	if (world.sound_count == 0)
	{
		return false;
	}
	event = world.sound_events[world.sound_read];
	world.sound_read = static_cast<std::uint8_t>(
		(world.sound_read + 1) % std::size(world.sound_events));
	--world.sound_count;
	return true;
}

namespace
{
void queue_world_sound(World& world, const WorldSoundEvent& event)
{
	if (world.sound_count >= std::size(world.sound_events))
	{
		return;
	}
	const std::uint8_t slot = static_cast<std::uint8_t>(
		(world.sound_read + world.sound_count)
			% std::size(world.sound_events));
	world.sound_events[slot] = event;
	++world.sound_count;
}
}

void world_queue_sound_explicit(
	World& world,
	const glm::vec3& position,
	const glm::vec3& direction,
	const glm::vec3& velocity,
	std::uint8_t definition,
	std::uint8_t requested_class)
{
	WorldSoundEvent event;
	event.position = position;
	event.direction = direction;
	event.velocity = velocity;
	event.definition = definition;
	event.requested_class = requested_class;
	queue_world_sound(world, event);
}

void world_queue_sound_object(
	World& world,
	ObjectHandle object,
	std::uint8_t definition,
	std::uint8_t requested_class)
{
	WorldSoundEvent event;
	event.object = object;
	event.definition = definition;
	event.requested_class = requested_class;
	event.binding = WorldSoundEvent::Binding::object;
	if (const WorldObject* owner = world_resolve(world, object))
	{
		event.position = owner->position
			+ owner->orientation[2] * (owner->player ? 200.0f : 0.0f);
		event.direction = owner->orientation[2];
		event.velocity = owner->linear_velocity;
	}
	queue_world_sound(world, event);
}

void world_queue_sound_missile(
	World& world,
	std::uint16_t missile_index,
	std::uint32_t missile_serial,
	ObjectHandle owning_object,
	const glm::vec3& position,
	const glm::vec3& direction,
	const glm::vec3& velocity,
	std::uint8_t definition,
	std::uint8_t requested_class)
{
	WorldSoundEvent event;
	event.position = position;
	event.direction = direction;
	event.velocity = velocity;
	event.object = owning_object;
	event.source_serial = missile_serial;
	event.source_index = missile_index;
	event.definition = definition;
	event.requested_class = requested_class;
	event.binding = WorldSoundEvent::Binding::missile;
	queue_world_sound(world, event);
}

void world_queue_sound_model_frame(
	World& world,
	ObjectHandle object,
	std::int16_t model_reference,
	const glm::vec3& position,
	const glm::vec3& direction,
	const glm::vec3& velocity,
	std::uint8_t definition,
	std::uint8_t requested_class)
{
	WorldSoundEvent event;
	event.position = position;
	event.direction = direction;
	event.velocity = velocity;
	event.object = object;
	event.model_reference = model_reference;
	event.definition = definition;
	event.requested_class = requested_class;
	event.binding = WorldSoundEvent::Binding::model_frame;
	queue_world_sound(world, event);
}

float world_object_rand_unit(WorldObject& object)
{
	return static_cast<float>(world_object_rand15(object))
		* (1.0f / 32767.0f);
}

void world_update_shield_ratios(
	WorldObject& object,
	const assets::ShipStatsTable& stats)
{
	if (!object.active || object.type >= assets::kShipStatsCount)
	{
		return;
	}
	// ship_update_shield_ratios_and_warning
	// (LANCER.EXE 0x00492370..0x00492413) derives the three subsystem
	// fractions from the four structural banks in this exact weighted
	// arrangement. These values drive the damage panel as well as shield,
	// engine, and gun performance.
	const float maximum = static_cast<float>(
		stats.records[object.type].object.structural_bank_max * 6 - 1);
	const float bank_0 = object.secondary_shields[0] / maximum;
	const float bank_1 = object.secondary_shields[1] / maximum;
	const float bank_2 = object.secondary_shields[2] / maximum;
	const float bank_3 = object.secondary_shields[3] / maximum;
	const float lateral = (bank_0 + bank_1) * 0.25f;
	object.shields_health = bank_2 * 0.5f + lateral;
	object.engines_health = bank_3 * 0.75f + 0.25f;
	object.weapons_health =
		(bank_2 + bank_3) * 0.25f + lateral;
}

void world_emit_mission_event(
	World& world,
	WorldMissionEventType type,
	const WorldObject& source,
	const WorldObject* attacker,
	std::uint8_t selector)
{
	if (source.mission_index == UINT16_MAX)
	{
		return;
	}
	if (world.mission_event_count >= kMaxMissionEvents)
	{
		if (!world.mission_event_overflow_reported)
		{
			diagnostics::mission_log(
				"world mission-event queue exhausted capacity=%u",
				static_cast<unsigned>(kMaxMissionEvents));
			world.mission_event_overflow_reported = true;
		}
		return;
	}
	WorldMissionEvent& event =
		world.mission_events[world.mission_event_count++];
	event.type = type;
	event.source_mission_index = source.mission_index;
	event.attacker_mission_index =
		attacker != nullptr ? attacker->mission_index : UINT16_MAX;
	event.selector = selector;
}

namespace
{
float world_random_unit(World& world)
{
	return static_cast<float>(world_rand15(world)) * (1.0f / 32767.0f);
}

const ObjectModelReference* find_destroyed_model(
	const WorldObject& object,
	const char* name)
{
	for (const ObjectModelReference& reference : object.model_references)
	{
		if (!reference.removed && std::strcmp(reference.name, name) == 0)
		{
			return &reference;
		}
	}
	return nullptr;
}

void set_destroyed_model_hidden(
	WorldObject& object,
	const char* name,
	bool hidden)
{
	for (ObjectModelReference& reference : object.model_references)
	{
		if (std::strcmp(reference.name, name) != 0)
		{
			continue;
		}
		if (hidden)
		{
			reference.runtime_flags |= 0x0020u;
		}
		else
		{
			reference.runtime_flags &= ~0x0020u;
		}
		return;
	}
}

void destroyed_model_world_pose(
	const WorldObject& object,
	const ObjectModelReference& reference,
	glm::vec3& position,
	glm::mat3& orientation)
{
	const std::uint16_t index = static_cast<std::uint16_t>(
		&reference - object.model_references.data());
	const glm::mat4 local = model_animation_render_transform(
		object, index, 1.0f);
	position = object.position + object.orientation * glm::vec3(local[3]);
	orientation = object.orientation * glm::mat3(local);
}

WorldObject* create_component_fragment(
	World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t type,
	const glm::vec3& position,
	const glm::mat3& orientation)
{
	const ObjectHandle handle = world_create(
		world, type, position, orientation, stats, false);
	WorldObject* fragment = world_resolve(world, handle);
	if (fragment != nullptr)
	{
		fragment->runtime_flags |= 0x00000008u;
	}
	return fragment;
}

void create_component_flashes(
	World& world,
	const glm::vec3& position,
	float radius,
	std::uint32_t simulation_tick)
{
	for (std::int32_t delay = 0; delay < 30; delay += 10)
	{
		glm::vec3 offset;
		offset.z = (world_random_unit(world) - 0.5f) * radius;
		offset.y = (world_random_unit(world) - 0.5f) * radius;
		offset.x = (world_random_unit(world) - 0.5f) * radius;
		(void)explosion_billboard_create(
			world.death_effects,
			world,
			position + offset,
			glm::vec3{0.0f},
			ExplosionBillboardType::separate_frames,
			radius,
			150,
			true,
			delay,
			false,
			false,
			simulation_tick);
	}
}

void create_component_particle_burst(
	World& world,
	mission::Runtime& mission,
	const WorldObject& owner,
	const glm::vec3& position,
	const glm::mat3& orientation,
	std::uint32_t simulation_tick)
{
	(void)particle_emitter_burst_world(
		world,
		position,
		orientation * math::rotation_from_euler({
			-glm::pi<float>() / 3.0f,
			0.0f,
			world_random_unit(world) * glm::two_pi<float>(),
		}),
		glm::vec3{0.0f},
		glm::vec3{1.0f, 1.0f, 0.2f},
		20.0f,
		5.0f,
		200,
		ParticleEmitterStyle::expanding_explosion,
		simulation_tick,
		mission.particle_camera_position,
		mission.particle_camera_forward,
		owner.linear_velocity * 0.25f);
}

void spawn_kras_engine_fragment(
	World& world,
	const assets::ShipStatsTable& stats,
	WorldObject& owner,
	const ObjectModelReference& source,
	bool left,
	std::uint32_t simulation_tick)
{
	const char* hidden_left[] = {
		"Kras l bot eng", "Kras l bot eng DEST",
		"Kras l eng block", "Kras l top eng DEST",
		"Kras l top eng", "Kras l eng suprt"};
	const char* hidden_right[] = {
		"Kras r bot eng DEST", "Kras r bot eng",
		"Kras r eng block", "Kras r top eng DEST",
		"Kras r top eng", "Kras r eng suprt"};
	const char* const* hidden = left ? hidden_left : hidden_right;
	for (std::size_t index = 0; index < std::size(hidden_left); ++index)
	{
		set_destroyed_model_hidden(owner, hidden[index], true);
	}
	set_destroyed_model_hidden(
		owner,
		left ? "Kras l eng block DEST" : "Kras r eng block DEST",
		false);
	set_destroyed_model_hidden(
		owner,
		left ? "Kras l eng DEST" : "Kras r eng DEST",
		false);

	glm::vec3 position;
	glm::mat3 orientation;
	destroyed_model_world_pose(owner, source, position, orientation);
	position += orientation * glm::vec3{0.0f, 3300.0f, -14000.0f};
	if (WorldObject* fragment = create_component_fragment(
			world, stats, 163, position, orientation))
	{
		fragment->angular_z =
			(world_random_unit(world) - 0.5f) * 0.005f;
		fragment->angular_y =
			(world_random_unit(world) - 0.5f) * 0.005f;
		fragment->angular_x =
			(world_random_unit(world) - 0.5f) * 0.005f;
		fragment->linear_velocity = orientation
			* glm::vec3{0.0f, 0.0f,
				-(20.0f + world_random_unit(world) * 20.0f)};
	}
	create_component_flashes(
		world, position, source.radius, simulation_tick);
	world.player_exhaust_exposure_percent = 100;
}

// Returns true only for the Protogate plate path, whose executable branch
// returns before the ordinary recursive component breakup and common burst.
bool dispatch_authored_component_explosion(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	WorldObject& owner,
	const ObjectModelReference& destroyed,
	std::uint32_t group,
	std::uint32_t simulation_tick)
{
	const bool root_group = destroyed.parent_reference < 0;
	const ObjectHandle owner_handle{
		static_cast<std::uint16_t>(&owner - std::begin(world.objects)),
		owner.generation};
	if (owner.type == 109 && root_group && group == 1)
	{
		const ObjectModelReference* core =
			find_destroyed_model(owner, "Inner Core01");
		if (core != nullptr)
		{
			powercore_effect_create(
				world,
				owner_handle,
				static_cast<std::uint16_t>(
					core - owner.model_references.data()),
				ParticleEmitterStyle::red_sparks_dense,
				simulation_tick);
		}
	}
	else if (owner.type == 68 && root_group && group == 17)
	{
		powercore_effect_release(world, owner_handle);
	}
	else if (owner.type == 168 && root_group && group == 2)
	{
		const ObjectModelReference* core =
			find_destroyed_model(owner, "Bor brk away CORE");
		if (core != nullptr)
		{
			powercore_effect_create(
				world,
				owner_handle,
				static_cast<std::uint16_t>(
					core - owner.model_references.data()),
				ParticleEmitterStyle::red_sparks_large,
				simulation_tick);
		}
	}
	else if (owner.type == 168 && root_group && group == 3)
	{
		powercore_effect_release(world, owner_handle);
	}
	const ObjectModelReference* source = &destroyed;
	std::uint16_t fragment_type = UINT16_MAX;
	float angular_scale = 0.0f;
	float speed_base = 0.0f;
	float speed_random = 0.0f;
	float local_speed_sign = 1.0f;

	if (owner.type == 72 && root_group && group == 15)
	{
		source = find_destroyed_model(owner, "Bor Ion can bot DEST");
		fragment_type = 167;
		angular_scale = 0.005f;
		speed_base = 20.0f;
		speed_random = 20.0f;
		local_speed_sign = -1.0f;
	}
	else if (owner.type == 71 && root_group && group == 9)
	{
		source = find_destroyed_model(owner, "Kron arm3");
		fragment_type = 166;
		angular_scale = 0.005f;
		speed_base = 20.0f;
		speed_random = 20.0f;
		local_speed_sign = -1.0f;
	}
	else if (owner.type == 69 && root_group && group >= 1 && group <= 3)
	{
		fragment_type = 151;
		angular_scale = 0.02f;
		speed_base = 100.0f;
		speed_random = 50.0f;
	}
	else if (owner.type == 109 && root_group && group >= 2 && group <= 11)
	{
		fragment_type = 116;
		angular_scale = 0.1f;
	}
	else if (owner.type == 120 && root_group)
	{
		const bool left = std::strcmp(destroyed.name, "Kras l eng block") == 0;
		const bool right = std::strcmp(destroyed.name, "Kras r eng block") == 0;
		if (!left && !right)
		{
			return false;
		}
		spawn_kras_engine_fragment(
			world, stats, owner, destroyed, left, simulation_tick);
		return false;
	}
	else if (owner.type == 69 && !root_group)
	{
		const ObjectModelReference* cargo =
			find_destroyed_model(owner, "Cargo pod ");
		if (cargo == nullptr)
		{
			cargo = find_destroyed_model(owner, "Cargo pod");
		}
		if (cargo != nullptr)
		{
			glm::vec3 position;
			glm::mat3 orientation;
			destroyed_model_world_pose(owner, *cargo, position, orientation);
			create_component_particle_burst(
				world, mission, owner, position, orientation, simulation_tick);
			create_component_flashes(
				world, position, cargo->radius, simulation_tick);
		}
		return false;
	}

	if (owner.type == 72 && root_group && group == 6)
	{
		owner.runtime_flags &= ~0x00000008u;
	}
	if (fragment_type == UINT16_MAX || source == nullptr)
	{
		return false;
	}

	glm::vec3 position;
	glm::mat3 orientation;
	destroyed_model_world_pose(owner, *source, position, orientation);
	WorldObject* fragment = create_component_fragment(
		world, stats, fragment_type, position, orientation);
	if (fragment != nullptr)
	{
		fragment->angular_z =
			(world_random_unit(world) - 0.5f) * angular_scale;
		fragment->angular_y =
			(world_random_unit(world) - 0.5f) * angular_scale;
		fragment->angular_x =
			(world_random_unit(world) - 0.5f) * angular_scale;
		if (fragment_type == 151)
		{
			fragment->explosion_model_variant = static_cast<std::uint8_t>(group);
		}
		else if (fragment_type == 116)
		{
			const std::uint8_t remainder = static_cast<std::uint8_t>(
				world_rand15(world) % 3u);
			fragment->explosion_model_variant =
				remainder == 0 ? 3 : remainder == 1 ? 1 : 2;
			const ObjectModelReference* center =
				find_destroyed_model(owner, "Protogate");
			glm::vec3 direction = position - owner.position;
			if (center != nullptr)
			{
				glm::vec3 center_position;
				glm::mat3 center_orientation;
				destroyed_model_world_pose(
					owner, *center, center_position, center_orientation);
				direction = position - center_position;
			}
			const float length = glm::length(direction);
			if (length != 0.0f)
			{
				direction /= length;
			}
			fragment->linear_velocity = direction
				* (200.0f + world_random_unit(world) * 100.0f);
		}
		else
		{
			fragment->linear_velocity = orientation
				* glm::vec3{0.0f, 0.0f,
					local_speed_sign
						* (speed_base
							+ world_random_unit(world) * speed_random)};
		}
	}

	if (fragment_type == 151 || fragment_type == 116)
	{
		world_queue_sound_explicit(
			world, position, orientation[2], owner.linear_velocity, 52, 2);
	}
	if (fragment_type == 116)
	{
		create_component_particle_burst(
			world, mission, owner, position, orientation, simulation_tick);
		(void)explosion_billboard_create(
			world.death_effects,
			world,
			position,
			glm::vec3{0.0f},
			ExplosionBillboardType::separate_frames,
			source->radius,
			150,
			true,
			0,
			false,
			false,
			simulation_tick);
		return true;
	}
	create_component_flashes(
		world, position, source->radius, simulation_tick);
	return false;
}
}

void world_spawn_capital_engine_breakaways(
	World& world,
	const assets::ShipStatsTable& stats,
	WorldObject& owner,
	std::uint32_t simulation_tick)
{
	const ObjectModelReference* left =
		find_destroyed_model(owner, "Kras l eng block");
	if (left != nullptr)
	{
		spawn_kras_engine_fragment(
			world, stats, owner, *left, true, simulation_tick);
	}
	const ObjectModelReference* right =
		find_destroyed_model(owner, "Kras r eng block");
	if (right != nullptr)
	{
		spawn_kras_engine_fragment(
			world, stats, owner, *right, false, simulation_tick);
	}
}

void world_service_component_destruction(
	World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick)
{
	for (std::uint16_t object_index = 0;
		object_index < kMaxGameObjects;
		++object_index)
	{
		WorldObject& object = world.objects[object_index];
		if (!object.active || !object.component_destruction_pending)
		{
			continue;
		}
		for (std::uint32_t model_index = 0;
			model_index < object.model_references.size();
			++model_index)
		{
			ObjectModelReference& destroyed =
				object.model_references[model_index];
			if (destroyed.removed
				|| destroyed.health >= 0.0f
				|| (destroyed.runtime_flags & 0x0010u) != 0)
			{
				continue;
			}
			destroyed.runtime_flags |= 0x0010u;
			if (destroyed.model_type == 5
				&& object.engine_component_count != 0)
			{
				// The type-5 branch of the render-time destruction
				// traversal at 0x0049ad43 subtracts exactly one over
				// the construction-time engine-component count.
				object.engine_component_scale -=
					1.0f
					/ static_cast<float>(
						object.engine_component_count);
			}
			if (destroyed.model_type == 6)
			{
				object.runtime_flags &= ~0x00004000u;
			}
			const std::uint32_t group = destroyed.part_group_id;
			// A type-one node accepted by GameObject+0x614 follows the
			// custom type-seven callback and awards at 0x0046fff4. The
			// callback calls GameObject_mark_destroyed at 0x00470018 and
			// returns false, bypassing the sound and complete ordinary
			// sibling-group removal at 0x0049b1d8.
			const bool custom_type_seven_callback =
				destroyed.model_type == 1
				&& world.disruption_effects
					.destruction_shockwave_created[object_index]
				&& world.disruption_effects
					.destruction_shockwave_generation[object_index]
						== object.generation;
			if (custom_type_seven_callback)
			{
				continue;
			}
			const bool authored_early_return =
				dispatch_authored_component_explosion(
					world,
					mission,
					stats,
					object,
					destroyed,
					group,
					simulation_tick);
			if (!authored_early_return)
			{
			float group_radius = 0.0f;
			for (const ObjectModelReference& candidate
				: object.model_references)
			{
				if (!candidate.removed
					&& candidate.owner_scope == destroyed.owner_scope
					&& candidate.parent_reference
						== destroyed.parent_reference
					&& candidate.part_group_id == group)
				{
					group_radius += candidate.radius;
				}
			}
			for (const ObjectModelReference& candidate
				: object.model_references)
			{
				if (!candidate.removed
					&& candidate.owner_scope == destroyed.owner_scope
					&& candidate.parent_reference
						== destroyed.parent_reference
					&& candidate.part_group_id == group)
				{
					explosion_component_breakup_world(
						world,
						object,
						candidate,
						group_radius,
						simulation_tick);
				}
			}

			// Object_component_destruction_effect_dispatch receives the
			// parent whose direct child group was removed. Its common particle
			// burst and sound use that parent's cached transform, while each
			// matching child owns its own delayed mesh breakup above.
			glm::vec3 effect_position = object.position;
			glm::mat3 effect_orientation = object.orientation;
			if (destroyed.parent_reference >= 0)
			{
				const std::uint16_t parent_reference =
					static_cast<std::uint16_t>(
						destroyed.parent_reference);
				const glm::mat4 parent_transform =
					model_animation_render_transform(
						object, parent_reference, 1.0f);
				effect_position += object.orientation
					* glm::vec3(parent_transform[3]);
				effect_orientation = object.orientation
					* glm::mat3(parent_transform);
			}
			(void)particle_emitter_burst_world(
				world,
				effect_position,
				effect_orientation
					* math::rotation_from_euler({
						-glm::pi<float>() / 3.0f,
						0.0f,
						static_cast<float>(world_rand15(world))
							* (1.0f / 32767.0f)
							* glm::two_pi<float>(),
					}),
				glm::vec3{0.0f},
				glm::vec3{1.0f, 1.0f, 0.2f},
				20.0f,
				5.0f,
				200,
				ParticleEmitterStyle::expanding_explosion,
				simulation_tick,
				mission.particle_camera_position,
				mission.particle_camera_forward,
				object.linear_velocity * 0.25f);
			world_queue_sound_explicit(
				world,
				effect_position,
				effect_orientation[2],
				object.linear_velocity,
				11,
				3);
			}

			for (ObjectModelReference& candidate
				: object.model_references)
			{
				// Object_model_update_scene scans only the current
				// RenderObject's child array. The flattened equivalent is
				// the exact owning-tree/immediate-parent pair, not every
				// model which happens to reuse the authored group number.
				if (candidate.owner_scope != destroyed.owner_scope
					|| candidate.parent_reference
						!= destroyed.parent_reference
					|| candidate.part_group_id != group)
				{
					continue;
				}
				candidate.runtime_flags |= 0x0010u;
				if ((candidate.runtime_flags & 0x0020u) != 0)
				{
					// A hidden authored DEST node consumes one traversal
					// by clearing suppression instead of being removed.
					candidate.runtime_flags &= ~0x0020u;
				}
				else
				{
					const WorldObject* attacker =
						object.last_attacker_index
								< kMaxGameObjects
							&& world.objects[
								object.last_attacker_index].active
							? &world.objects[
								object.last_attacker_index]
							: nullptr;
					const std::uint16_t attacker_mission_index =
						attacker != nullptr
							? attacker->mission_index
							: UINT16_MAX;
					// Object_model_update_scene publishes the selected
					// component before the type-one score/chatter and the
					// whole-object Destroyed boundary
					// (LANCER.EXE 0x0049b23f..0x0049b2b7).
					if (candidate.component_index >= 0
						&& candidate.component_index <= UINT8_MAX)
					{
						(void)mission::events_emit_component_destroyed(
							mission,
							object.mission_index,
							attacker_mission_index,
							static_cast<std::uint8_t>(
								candidate.component_index));
					}
					// Object_model_update_scene, LANCER.EXE
					// 0x0049b26f..0x0049b2b7. This ordinary component
					// producer awards once for every qualifying removed
					// type-one node, not once per group. Retail suppresses
					// its chatter in every multiplayer session; network
					// play still receives the score and explicit stats
					// publication.
					if (candidate.model_type == 1
						&& object.last_attacker_index
							== world.player.index
						&& (object.type == 0x3c
							|| object.type == 0x46
							|| object.type == 0x3e))
					{
						(void)mission::runtime_add_player_score(
							mission,
							world,
							world.player.index,
							1,
							false);
						if (mission.network.role
							== mission::NetworkRole::offline)
						{
							mission::player_comms_on_player_destroyed_target(
								mission,
								world,
								stats,
								object_index,
								simulation_tick,
								simulation_tick);
						}
						if (mission.network.role
							!= mission::NetworkRole::offline)
						{
							mission::network_publish_player_stats(
								mission.network,
								world.player.index);
						}
					}
					if (candidate.model_type == 1)
					{
						(void)world_mark_destroyed(
							world, mission, object);
					}
					candidate.removed = true;
				}
				if (candidate.component_index >= 0
					&& candidate.component_index
						< object.component_count)
				{
					object.components[
						candidate.component_index]
						.runtime_flags |= 0x0010u;
				}
			}
			diagnostics::mission_log(
				"component group destroyed target=%u mission=%u "
				"model=%u group=%u engine-scale=%.3f "
				"shield-generator=%u",
				static_cast<unsigned>(object_index),
				static_cast<unsigned>(object.mission_index),
				static_cast<unsigned>(model_index),
				static_cast<unsigned>(group),
				object.engine_component_scale,
				(object.runtime_flags & 0x00004000u) != 0
					? 1u
					: 0u);
		}
		if (world.selected_target.index == object_index
			&& world.selected_target.generation == object.generation
			&& world.target_component >= 0
			&& world.target_component < object.component_count
			&& (object.components[world.target_component].runtime_flags
				& 0x0010u) != 0)
		{
			world.target_component = -1;
			if (WorldObject* player =
					world_resolve(world, world.player))
			{
				player->selected_target_component = -1;
				for (std::uint8_t command = 0;
					command < player->ai.command_count;
					++command)
				{
					if (player->ai.commands[command].id == 100)
					{
						player->ai.commands[command]
							.target_component = -1;
						break;
					}
				}
			}
		}
		object.component_destruction_pending = false;
	}
}

void world_service_resources(
	World& world,
	const assets::ShipStatsTable& stats,
	const mission::Runtime& mission)
{
	if (!stats.ready)
	{
		return;
	}
	// GameObjects_quarter_tick_update (0x004774d0) services these paths
	// once per four 100 Hz simulation steps, i.e. at 25 Hz.
	for (std::uint16_t object_index = 0;
		object_index < std::size(world.objects);
		++object_index)
	{
		WorldObject& object = world.objects[object_index];
		if (!object.active
			|| object.type >= assets::kShipStatsCount
			|| (object.runtime_flags & 0x00000420u) != 0)
		{
			continue;
		}
		const assets::ObjectTypeStats& type =
			stats.records[object.type].object;

		// GameObject_regenerate_primary_shields
		// (0x00476fc0): all four banks receive the same 25 Hz increment
		// before the local player's paired auxiliary-budget adjustment.
		if ((object.runtime_flags & kObjectFlagCompound) == 0
			&& object.protection_state == 5)
		{
			// GameObject_regenerate_primary_shields, LANCER.EXE
			// 0x00476fc0: state five empties every bank and returns before
			// recharge. Gun-energy service remains a separate caller.
			std::fill(
				std::begin(object.primary_shields),
				std::end(object.primary_shields),
				0.0f);
		}
		else if ((object.runtime_flags & kObjectFlagCompound) == 0
			&& (object.ai.command_count == 0
				|| object.ai.commands[0].id != 8)
			&& mission::deathmatch_scenarios_shield_recharge_allowed(
				mission, object_index))
		{
			const float maximum =
				static_cast<float>(type.primary_bank_max * 6 - 1);
			const float increment =
				maximum
				* object.shield_recharge_scale
				* object.weapons_health
				/ (type.primary_recharge_time * 25.0f);
			for (std::uint8_t bank = 0; bank < 4; ++bank)
			{
				float bank_maximum = maximum;
				if (object_index == world.player.index)
				{
					float paired_total = 0.0f;
					if (bank == 2)
					{
						paired_total = object.auxiliary_shields[1]
							+ object.primary_shields[3];
					}
					else if (bank == 3)
					{
						paired_total = object.auxiliary_shields[0]
							+ object.primary_shields[2];
					}
					if (paired_total > maximum)
					{
						// 0x00477097..0x004770a8 subtracts the
						// opposite bank/auxiliary excess from this
						// bank's ordinary maximum.
						bank_maximum =
							maximum - (paired_total - maximum);
					}
				}
				object.primary_shields[bank] = std::min(
					object.primary_shields[bank] + increment,
					bank_maximum);
			}
		}

		// The leading resource branch of GameObject_update_simple_guns
		// (0x004770e0) regenerates energy even when the later traversal is
		// suppressed, but not while a Nova cannon retains charge.
		if (object.nova_charge != 0.0f)
		{
			continue;
		}
		const float increment =
			type.gun_energy_max
			* object.gun_recharge_scale
			* object.shields_health
			/ (type.gun_recharge_time * 25.0f);
		object.gun_energy = std::min(
			object.gun_energy + increment,
			type.gun_energy_max);
	}
}

bool world_destroy(World& world, ObjectHandle handle)
{
	WorldObject* object = world_resolve(world, handle);
	if (object == nullptr)
	{
		return false;
	}
	// GameObject_destroy, LANCER.EXE 0x004688b0, performs the complete AI
	// queue teardown while the object and its command-owned resources are
	// still live. In particular, Respawn Effect must release its slot and
	// clear the recursive model modifier before generic owner cleanup.
	ai::command_clear(world, *object);
	death_effects_release_owner(world.death_effects, handle);
	if (handle_matches(world.player, handle))
	{
		world.player = {};
		world.player_cloak_control_active = false;
	}
	if (handle_matches(world.action_center, handle))
	{
		world.action_center = {};
	}
	if (handle_matches(world.selected_target, handle))
	{
		world.selected_target = {};
		world.target_component = -1;
		if (WorldObject* player = world_resolve(world, world.player))
		{
			player->selected_target_index = UINT16_MAX;
			player->selected_target_component = -1;
			for (std::uint8_t command = 0;
				command < player->ai.command_count;
				++command)
			{
				if (player->ai.commands[command].id == 100)
				{
					player->ai.commands[command].target_kind =
						ai::TargetKind::world_object;
					player->ai.commands[command].target =
						UINT16_MAX;
					player->ai.commands[command]
						.target_component = -1;
					break;
				}
			}
		}
	}
	world_release_planet_atmosphere(world, handle);
	for (std::uint16_t registered = 0;
		registered < world.exhaust_hazard_count;
		++registered)
	{
		if (world.exhaust_hazard_indices[registered] == handle.index)
		{
			exhaust_hazard_invalidate(world);
			break;
		}
	}
	release_object_owned_runtime(world, handle, *object);
	const std::uint16_t generation = object->generation;
	*object = {};
	object->generation = static_cast<std::uint16_t>(generation + 1);
	if (world.live_count != 0)
	{
		--world.live_count;
	}
	return true;
}

void world_release_planet_atmosphere(
	World& world,
	ObjectHandle owner)
{
	for (std::uint8_t index = 0; index < world.atmosphere_count; ++index)
	{
		const PlanetAtmosphere& atmosphere = world.atmospheres[index];
		if (atmosphere.owner.index != owner.index
			|| atmosphere.owner.generation != owner.generation)
		{
			continue;
		}
		for (std::uint8_t move = index + 1;
			move < world.atmosphere_count;
			++move)
		{
			world.atmospheres[move - 1] = world.atmospheres[move];
		}
		--world.atmosphere_count;
		world.atmospheres[world.atmosphere_count] = {};
		diagnostics::mission_log(
			"planet atmosphere released owner=%u remaining=%u",
			static_cast<unsigned>(owner.index),
			static_cast<unsigned>(world.atmosphere_count));
		return;
	}
}

void world_depart_planet_atmosphere(
	World& world,
	ObjectHandle owner)
{
	// DestroyFlightGroup_command, LANCER.EXE 0x00457ffb..0x00458038,
	// decrements the registry count before searching the remaining prefix.
	// It neither compacts the matched entry nor visits the old last entry.
	// Preserve that command-specific ownership behavior separately from a
	// complete GameObject destructor.
	if (world.atmosphere_count == 0)
	{
		diagnostics::mission_log(
			"planet atmosphere departure underflow owner=%u",
			static_cast<unsigned>(owner.index));
		return;
	}
	--world.atmosphere_count;
	for (std::uint8_t index = 0;
		index < world.atmosphere_count;
		++index)
	{
		const PlanetAtmosphere& atmosphere = world.atmospheres[index];
		if (atmosphere.owner.index != owner.index
			|| atmosphere.owner.generation != owner.generation)
		{
			continue;
		}
		world.atmospheres[index] = {};
		return;
	}
}

bool world_type_has_planet_atmosphere(std::uint16_t type)
{
	return type_has_planet_atmosphere(type);
}

WorldObject* world_resolve(World& world, ObjectHandle handle)
{
	if (handle.index >= kMaxGameObjects)
	{
		return nullptr;
	}
	WorldObject& object = world.objects[handle.index];
	return object.active && object.generation == handle.generation
		? &object
		: nullptr;
}

const WorldObject* world_resolve(const World& world, ObjectHandle handle)
{
	if (handle.index >= kMaxGameObjects)
	{
		return nullptr;
	}
	const WorldObject& object = world.objects[handle.index];
	return object.active && object.generation == handle.generation
		? &object
		: nullptr;
}

float world_effective_max_speed(
	const WorldObject& object,
	const assets::ShipStatsTable& stats,
	std::uint8_t camera_mode)
{
	if (object.type >= assets::kShipStatsCount)
	{
		return 0.0f;
	}
	// GameObject_effective_max_speed, LANCER.EXE 0x00403060.
	float maximum = stats.records[object.type].flight.max_speed
		* object.engine_power_scale
		* object.engine_component_scale;
	if (camera_mode != 13 && object.protection_state == 0)
	{
		maximum *= object.engines_health;
	}
	return maximum;
}

void world_step_object(
	WorldObject& object,
	const assets::ShipStatsTable& stats,
	const FlightDemand& demand,
	std::uint8_t camera_mode,
	const FlightServiceContext* service)
{
	if (!object.active || object.type >= assets::kShipStatsCount)
	{
		return;
	}
	object.transform_state_flags |= kObjectTransformChanged;
	object.control_demand = demand;
	const assets::FlightStats& flight =
		stats.records[object.type].flight;
	const auto retain_network_publication_state = [&object]()
	{
		// GameObject_integrate ORs these bits after every completed motion
		// callback. They are deliberately sticky: opcode 0x1d continues to
		// carry position/orientation after the triggering motion subsides.
		if (object.speed > 0.0f)
		{
			object.state_publication_flags |= kObjectStatePublishPosition;
		}
		if (object.angular_x != 0.0f
			|| object.angular_y != 0.0f
			|| object.angular_z != 0.0f)
		{
			object.state_publication_flags |= kObjectStatePublishOrientation;
		}
	};
	object.previous_position = object.position;
	object.previous_orientation = object.orientation;
	if ((object.runtime_flags & kObjectFlagSimulationSuspended) != 0
		&& object.accumulated_impulse_count == 0
		&& (object.runtime_flags & 0x00000008u) == 0)
	{
		// GameObject_integrate skips a suspended transform owner only when
		// it has no pending force. Collision impulses still cross the same
		// integrator and can move an otherwise suspended object.
		return;
	}
	if (object.accumulated_impulse_count != 0)
	{
		// GameObject_apply_pending_forces, LANCER.EXE 0x00476270. A queued
		// collision impulse suppresses the ordinary flight callback for
		// this integration and is consumed as one complete accumulator.
		object.accumulated_impulse_count = 0;
		object.linear_velocity +=
			object.accumulated_linear_impulse / object.physics_mass;
		object.accumulated_linear_impulse = glm::vec3{0.0f};
		if (object.accumulated_angular_impulse.x != 0.0f
			|| object.accumulated_angular_impulse.y != 0.0f
			|| object.accumulated_angular_impulse.z != 0.0f)
		{
			// GameObject_apply_pending_forces, 0x004762f4. Torque is
			// accumulated in world space, transformed into the preceding
			// body frame, and multiplied by the authored inverse inertia.
			const glm::vec3 angular_delta =
				object.inverse_inertia
				* glm::transpose(object.previous_orientation)
				* object.accumulated_angular_impulse;
			const glm::mat3 impulse_step{
				{1.0f, angular_delta.z, -angular_delta.y},
				{-angular_delta.z, 1.0f, angular_delta.x},
				{angular_delta.y, -angular_delta.x, 1.0f},
			};
			object.inertial_angular_step *= impulse_step;

			// Matrix_orthonormalize, 0x004c2690, preserves the third
			// column first and reconstructs the other two in this order.
			glm::vec3 third =
				glm::normalize(object.inertial_angular_step[2]);
			glm::vec3 second = glm::normalize(glm::cross(
				third, object.inertial_angular_step[0]));
			glm::vec3 first = glm::cross(second, third);
			object.inertial_angular_step[0] = first;
			object.inertial_angular_step[1] = second;
			object.inertial_angular_step[2] = third;
			const glm::vec3 angular_euler =
				math::rotation_to_euler(
					object.inertial_angular_step);
			object.angular_x = angular_euler.x;
			object.angular_y = angular_euler.y;
			object.angular_z = angular_euler.z;
		}
		object.accumulated_angular_impulse = glm::vec3{0.0f};
		object.orientation =
			object.previous_orientation
			* object.inertial_angular_step;
		object.position =
			object.previous_position + object.linear_velocity;
		object.speed = glm::length(object.linear_velocity);
		retain_network_publication_state();
		return;
	}
	// Runtime flag 0x8 and the ejection split's two tiny callbacks (or a
	// literal null callback) bypass the ordinary flight controller. The
	// generic object owner subsequently integrates retained velocity and
	// GameObject+0x56c angular motion; 0x00474610 additionally clears
	// throttle.
	if ((object.runtime_flags & 0x00000008u) != 0
		|| object.flight_callback_mode
			== FlightCallbackMode::damp_velocity_0_97
		|| object.flight_callback_mode
			== FlightCallbackMode::damp_velocity_0_99
		|| object.flight_callback_mode == FlightCallbackMode::none)
	{
		if (object.flight_callback_mode
			== FlightCallbackMode::damp_velocity_0_97)
		{
			object.linear_velocity *= 0x1.f0a3d8p-1f;
			object.throttle = 0.0f;
		}
		else if (object.flight_callback_mode
			== FlightCallbackMode::damp_velocity_0_99)
		{
			object.linear_velocity *= 0x1.fae148p-1f;
		}
		object.orientation =
			object.previous_orientation
			* object.inertial_angular_step;
		object.position += object.linear_velocity;
		object.speed = glm::length(object.linear_velocity);
		retain_network_publication_state();
		return;
	}
	if (service != nullptr
		&& ai::scripted_flight_callback(
			object,
			service->world,
			service->mission,
			service->file,
			stats,
			service->simulation_tick))
	{
		object.orientation =
			object.previous_orientation
			* object.inertial_angular_step;
		object.position =
			object.previous_position + object.linear_velocity;
		object.speed = glm::length(object.linear_velocity);
		retain_network_publication_state();
		return;
	}
	// The two linear callbacks (0x004744e0/0x00474570) consume the raw
	// object throttle. Launch strategy eight deliberately supplies -2.
	// Only Flight_update_standard clamps ordinary demand to [0,1].
	const bool linear_no_exhaust =
		object.flight_callback_mode
			== FlightCallbackMode::linear_no_exhaust;
	const bool linear_with_exhaust =
		object.flight_callback_mode
			== FlightCallbackMode::linear_with_exhaust;
	const bool linear = linear_no_exhaust || linear_with_exhaust;
	object.throttle = linear
		? demand.throttle
		: std::clamp(demand.throttle, 0.0f, 1.0f);
	object.reverse_thrust_active = demand.reverse;
	object.afterburner_active = demand.afterburner;
	float applied_throttle = object.throttle;
	if (!linear && demand.afterburner)
	{
		applied_throttle = 2.0f;
		object.afterburner_fuel =
			std::max(0, object.afterburner_fuel - 4);
	}
	else if (!linear && demand.reverse)
	{
		applied_throttle = -1.0f;
		object.afterburner_fuel =
			std::max(0, object.afterburner_fuel - 4);
	}
	object.throttle = applied_throttle;
	object.control_demand.throttle = applied_throttle;

	const float divisor = linear
		? 1.0f
		: std::max(
			1.0f, 3.0f - 2.0f * std::abs(applied_throttle));
	const float pitch = std::clamp(demand.pitch, -1.0f, 1.0f);
	const float yaw = std::clamp(demand.yaw, -1.0f, 1.0f);
	const float roll = std::clamp(demand.roll, -1.0f, 1.0f);
	object.control_demand.pitch = pitch;
	object.control_demand.yaw = yaw;
	object.control_demand.roll = roll;
	object.angular_x =
		object.angular_x * flight.pitch_retention
		+ (1.0f - flight.pitch_retention)
			* flight.pitch_rate * pitch / divisor;
	object.angular_y =
		object.angular_y * flight.yaw_retention
		+ (1.0f - flight.yaw_retention)
			* flight.yaw_rate * yaw / divisor;
	object.angular_z =
		object.angular_z * flight.roll_retention
		+ (1.0f - flight.roll_retention)
			* flight.roll_rate * roll / divisor;
	const glm::mat3 angular_step = math::rotation_from_euler({
		object.angular_x,
		object.angular_y,
		object.angular_z,
	});
	object.inertial_angular_step = angular_step;
	if (linear)
	{
		object.exhaust_scalar = linear_with_exhaust
			? object.throttle
			: 0.0f;
		// Flight_update_linear_no_exhaust/with_exhaust
		// (LANCER.EXE 0x004744e0/0x00474570) retain world velocity. The
		// no-exhaust callback accelerates along local Y; the exhaust-bearing
		// callback accelerates along the ordinary local-Z flight axis.
		// Collision-class one aliases type-zero data unconditionally in
		// 0x004744e0. 0x00474570 alone exempts Ripper type 0x1f.
		const assets::FlightStats& linear_flight =
			object.collision_class == 1
				&& (linear_no_exhaust || object.type != 0x1f)
				? stats.records[0].flight
				: flight;
		const float retention = linear_flight.linear_retention;
		object.linear_velocity *= retention;
		const glm::vec3 acceleration_axis =
			linear_no_exhaust
				? object.orientation[1]
				: object.orientation[2];
		object.linear_velocity +=
			acceleration_axis
				* ((1.0f - retention)
					* object.throttle
					* linear_flight.max_speed);
		object.orientation =
			object.previous_orientation * angular_step;
		object.position =
			object.previous_position + object.linear_velocity;
		object.speed = glm::length(object.linear_velocity);
		retain_network_publication_state();
		return;
	}

	glm::vec3 local_velocity =
		glm::transpose(object.orientation) * object.linear_velocity;
	const float retention = flight.linear_retention;
	const float response = 1.0f - retention;
	const float maximum =
		demand.afterburner || demand.reverse
			? flight.max_speed
			: world_effective_max_speed(
				object, stats, camera_mode);
	local_velocity.x =
		retention * local_velocity.x
		+ response * demand.strafe * maximum * 0.25f;
	local_velocity.y = retention * local_velocity.y;
	// Flight_update_standard_reverse is the same controller with the
	// compiled -1 direction scalar (LANCER.EXE 0x004744d0). Ripper drop
	// uses it to back away at a positive 0.2 throttle.
	const float drive =
		object.flight_callback_mode
				== FlightCallbackMode::standard_reverse
			? -applied_throttle
			: applied_throttle;
	object.exhaust_scalar = applied_throttle;
	local_velocity.z = signed_square_root(
		retention * local_velocity.z * std::abs(local_velocity.z)
		+ response * drive * std::abs(drive)
			* maximum * maximum);
	object.linear_velocity = object.orientation * local_velocity;
	object.orientation = object.previous_orientation * angular_step;
	object.position =
		object.previous_position + object.linear_velocity;
	object.speed = glm::length(object.linear_velocity);
	retain_network_publication_state();
}

void world_service_ordinary_motion(
	World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick)
{
	// GameObjects_service_phase, LANCER.EXE 0x004774d0, invokes
	// GameObjects_integrate (0x00468fa0) only after its four 100 Hz phases
	// wrap. The integrator skips hidden/suspended transform owners before
	// dispatching GameObject_integrate (0x00473ff0).
	const FlightServiceContext service{
		world, mission, file, simulation_tick};
	for (WorldObject& object : world.objects)
	{
		if (!object.active
			|| !object.ordinary_motion_enabled
			|| object.type >= assets::kShipStatsCount
			|| (object.runtime_flags & 0x00000430u) != 0)
		{
			continue;
		}
		world_step_object(
			object,
			stats,
			object.control_demand,
			world.camera_mode,
			&service);
		const std::uint16_t object_index =
			static_cast<std::uint16_t>(
				&object - std::begin(world.objects));
		if (world.player.index == object_index
			&& world.player.generation == object.generation)
		{
			// GameObject_integrate 0x004740f5..0x00474134 raises the
			// common camera disturbance for the local player's ordinary
			// overspeed, including afterburner flight:
			// speed/effective_max*0.2 - 0.2.
			const float disturbance =
				object.speed
					/ world_effective_max_speed(
						object, stats, world.camera_mode)
					* 0.2f
				- 0.2f;
			if (world.player_camera_disturbance < disturbance)
			{
				world.player_camera_disturbance = disturbance;
			}
		}
	}
}

void world_publish_scene_poses(
	World& world,
	std::uint8_t service_phase)
{
	// Object_model_update_scene, LANCER.EXE 0x0049a460, receives the
	// fixed-service phase multiplied by exactly 0.25. Phase zero therefore
	// publishes the previous physics sample, giving retail its deliberate
	// one-sample interpolation delay.
	const float fraction =
		static_cast<float>(service_phase & 3u) * 0.25f;
	std::vector<std::uint8_t> traversal_state;
	for (WorldObject& object : world.objects)
	{
		if (!object.active)
		{
			continue;
		}
		object.scene_position =
			object.previous_position
				+ (object.position - object.previous_position) * fraction;
		object.scene_orientation = math::interpolate_scene_orientation(
			object.previous_orientation,
			object.orientation,
			fraction);
		// Publish the fully parent-composed render tree once. Runtime meshes,
		// locators and lights all consume these matrices; physics continues to
		// use local_transform and the current fixed-service pose separately.
		model_animation_publish_scene_transforms(
			object, fraction, traversal_state);
	}

	// AI_ObjectAttach and the launch/landing parent stages deliberately copy
	// a parent's current fixed-rate pose into both child physics snapshots.
	// Their retained scene nodes are nevertheless children of the parent's
	// rendered scene node. Recompose those roots after all ordinary scene and
	// model poses have been published so parented objects share interpolation
	// with the mesh or model node that carries them.
	std::array<std::uint8_t, kMaxGameObjects> attachment_state{};
	const auto publish_attachment = [&world, &attachment_state](
		auto&& self,
		std::uint16_t object_index) -> bool
	{
		if (object_index >= kMaxGameObjects)
		{
			return false;
		}
		if (attachment_state[object_index] == 2)
		{
			return true;
		}
		if (attachment_state[object_index] == 1)
		{
			return false;
		}
		attachment_state[object_index] = 1;
		WorldObject& object = world.objects[object_index];
		if (!object.active || !object.scene_attachment_active)
		{
			attachment_state[object_index] = 2;
			return true;
		}
		WorldObject* parent = world_resolve(
			world, object.scene_attachment_parent);
		if (parent == nullptr)
		{
			attachment_state[object_index] = 2;
			return false;
		}
		const std::uint16_t parent_index = static_cast<std::uint16_t>(
			parent - std::begin(world.objects));
		if (!self(self, parent_index))
		{
			attachment_state[object_index] = 2;
			return false;
		}
		glm::mat4 parent_frame = math::model_transform(
			parent->scene_orientation, 1.0f, parent->scene_position);
		if (object.scene_attachment_model >= 0
			&& static_cast<std::size_t>(object.scene_attachment_model)
				< parent->model_references.size())
		{
			parent_frame *= parent->model_references[
				static_cast<std::uint16_t>(
					object.scene_attachment_model)].scene_transform;
		}
		const glm::mat4 attached =
			parent_frame
			* glm::translate(
				glm::mat4{1.0f}, object.scene_attachment_position)
			* glm::mat4{object.scene_attachment_orientation};
		object.scene_position = glm::vec3(attached[3]);
		object.scene_orientation = glm::mat3(attached);
		attachment_state[object_index] = 2;
		return true;
	};
	for (std::uint16_t object_index = 0;
		object_index < kMaxGameObjects;
		++object_index)
	{
		publish_attachment(publish_attachment, object_index);
	}
}

void world_set_scene_attachment(
	World& world,
	WorldObject& object,
	const WorldObject& parent,
	std::int16_t parent_model,
	const glm::vec3& local_position,
	const glm::mat3& local_orientation)
{
	object.scene_attachment_parent = {
		static_cast<std::uint16_t>(&parent - std::begin(world.objects)),
		parent.generation};
	object.scene_attachment_position = local_position;
	object.scene_attachment_orientation = local_orientation;
	object.scene_attachment_model = parent_model;
	object.scene_attachment_active = true;
}

void world_clear_scene_attachment(WorldObject& object)
{
	object.scene_attachment_parent = {};
	object.scene_attachment_model = -1;
	object.scene_attachment_active = false;
}

void world_zero_motion_controls(WorldObject& object)
{
	// GameObject_zero_motion_controls, LANCER.EXE 0x00403000.
	object.control_demand = {};
	object.linear_velocity = {0.0f, 0.0f, 0.0f};
	object.throttle = 0.0f;
	object.exhaust_scalar = 0.0f;
	object.speed = 0.0f;
	object.angular_x = 0.0f;
	object.angular_y = 0.0f;
	object.angular_z = 0.0f;
	object.inertial_angular_step = glm::mat3{1.0f};
	object.afterburner_active = false;
	object.reverse_thrust_active = false;
}

}
