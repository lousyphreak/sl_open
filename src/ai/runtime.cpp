#include "ai/runtime.hpp"

#include "ai/scripted_commands.hpp"
#include "assets/gameplay_model.hpp"
#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "game/chaff.hpp"
#include "game/death_effects.hpp"
#include "game/missiles.hpp"
#include "game/model_animation.hpp"
#include "game/particle_emitters.hpp"
#include "game/retained_components.hpp"
#include "game/weapons.hpp"
#include "mission/environment_effects.hpp"
#include "mission/deathmatch_scenarios.hpp"
#include "mission/events.hpp"
#include "mission/network_runtime.hpp"
#include "mission/player_comms.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

namespace sl_open::ai
{
namespace
{
struct CommandDefinitionPolicy
{
	std::uint32_t flags{};
	std::int32_t priority{};
};

constexpr std::uint32_t kCommandFlagPlayerSlotAdmission = 0x001u;
constexpr std::uint32_t kCommandFlagImmediateDispatch = 0x020u;
constexpr std::uint32_t kCommandFlagRetaliation = 0x040u;
constexpr std::uint32_t kCommandFlagBuildNearbyLists = 0x080u;
constexpr std::uint32_t kCommandFlagSerializeMotion = 0x400u;

// Exact flags/priority columns from the two compiled 0x18-byte definition
// tables at LANCER.EXE 0x004e0050 and 0x004e04a0. Exhaustive references to
// The retail mode-mask table shows runtime masks 0x01 (player-slot
// admission), 0x20 (immediate dispatch), 0x40 (retaliation), 0x80 (proximity
// list), and 0x400 (network motion/control serialization). Bits 0x02, 0x04,
// 0x08, and 0x10 are retained table metadata: no shipped definition-table
// consumer tests them. The offline runtime therefore has no missing dispatch
// behavior for those four bits; 0x400 belongs to the absent multiplayer
// snapshot writer rather than command execution.
constexpr std::array<CommandDefinitionPolicy, 46> kLowCommandPolicies{{
	{0x040u, 0}, {0x4c0u, 0}, {0x022u, 0}, {0x022u, 0},
	{0x003u, 1}, {0x003u, 1}, {0x4c2u, 0}, {0x4c0u, 0},
	{0x003u, 1}, {0x48eu, 0}, {0x00eu, 0}, {0x003u, 99},
	{0x482u, 0}, {0x002u, 0}, {0x010u, 0}, {0x014u, 0},
	{0x020u, 0}, {0x412u, 0}, {0x000u, 0}, {0x003u, 1},
	{0x003u, 1}, {0x482u, 0}, {0x000u, 0}, {0x000u, 0},
	{0x000u, 0}, {0x003u, 1}, {0x003u, 1}, {0x4ceu, 0},
	{0x000u, 0}, {0x000u, 0}, {0x001u, 98}, {0x000u, 0},
	{0x002u, 0}, {0x40eu, 0}, {0x403u, 0}, {0x000u, 0},
	{0x000u, 0}, {0x000u, 0}, {0x000u, 0}, {0x482u, 0},
	{0x003u, 1}, {0x003u, 1}, {0x000u, 0}, {0x000u, 0},
	{0x000u, 0}, {0x000u, 0},
}};

constexpr std::array<CommandDefinitionPolicy, 23> kHighCommandPolicies{{
	{0x400u, 0}, {0x000u, 0}, {0x480u, 0}, {0x400u, 0},
	{0x000u, 1}, {0x4c0u, 0}, {0x000u, 98}, {0x480u, 0},
	{0x000u, 98}, {0x480u, 1}, {0x000u, 0}, {0x000u, 0},
	{0x000u, 0}, {0x000u, 0}, {0x000u, 0}, {0x400u, 0},
	{0x400u, 0}, {0x400u, 1}, {0x400u, 97}, {0x400u, 0},
	{0x480u, 0}, {0x400u, 0}, {0x000u, 0},
}};

const CommandDefinitionPolicy* command_policy(std::int16_t id)
{
	if (id >= 0
		&& static_cast<std::size_t>(id) < kLowCommandPolicies.size())
	{
		return &kLowCommandPolicies[static_cast<std::size_t>(id)];
	}
	if (id >= 100
		&& static_cast<std::size_t>(id - 100)
			< kHighCommandPolicies.size())
	{
		return &kHighCommandPolicies[
			static_cast<std::size_t>(id - 100)];
	}
	return nullptr;
}

std::uint32_t command_flags(std::int16_t id)
{
	const CommandDefinitionPolicy* policy = command_policy(id);
	return policy == nullptr ? 0u : policy->flags;
}

std::uint16_t last_attacker_mission_index(
	const game::World& world,
	const game::WorldObject& victim)
{
	// MissionEvent_destroyed_publish resolves victim+0x694 as a live world
	// slot, then maps that object back to its authored mission record.
	// Passing the slot itself can falsely attribute a kill to an unrelated
	// mission object with the same numeric index.
	if (victim.last_attacker_index >= std::size(world.objects))
	{
		return UINT16_MAX;
	}
	const game::WorldObject& attacker =
		world.objects[victim.last_attacker_index];
	return attacker.active ? attacker.mission_index : UINT16_MAX;
}

const char* command_name(std::int16_t id)
{
	switch (id)
	{
	case 0: return "Do Nothing";
	case 1: return "Fly Aimlessly";
	case 2: return "Launch Missile";
	case 3: return "Launch Kind-3 Missile";
	case 4: return "Warp In";
	case 5: return "Warp Out";
	case 6: return "Fly";
	case 7: return "Run Away";
	case 8: return "Land";
	case 9: return "Escort";
	case 10: return "Find New Target";
	case 11: return "Explode";
	case 12: return "Ripper Grabs Target";
	case 13: return "Object Attach";
	case 14: return "Formation Regroup";
	case 15: return "Patrol Route";
	case 16: return "Toggle Cloak";
	case 17: return "Ship Follow Curve";
	case 18: return "Slow Rotate";
	case 19: return "Jump In";
	case 20: return "Jump Out";
	case 21: return "Find Scoop Up";
	case 22: return "Random Spin Slow";
	case 23: return "Random Spin Medium";
	case 24: return "Random Spin Fast";
	case 25: return "Fixed Gate Jump In";
	case 26: return "Fixed Gate Jump Out";
	case 27: return "Formation";
	case 28: return "Fixed Gate Open";
	case 29: return "Fixed Gate Close";
	case 30: return "Eject";
	case 31: return "Fixed Gate Collapse";
	case 32: return "Match Speed";
	case 33: return "Dark Reign Shoot Request";
	case 34: return "Move To Spawn Position";
	case 35: return "Object Lights On";
	case 36: return "Boridin Section Breakaway";
	case 37: return "Boridin Projector Rotate";
	case 38: return "Boridin Warp Projection";
	case 39: return "Ripper Drop Cargo";
	case 40: return "Jump In";
	case 41: return "Jump Out";
	case 42: return "Object Lights Off";
	case 43: return "Huge Explosion";
	case 44: return "Zero Motion";
	case 45: return "Fly Backwards";
	case 100: return "Player Control";
	case 101: return "Multiplayer Control";
	case 102: return "Avoid Target";
	case 103: return "Torpedo";
	case 104: return "Launch";
	case 105: return "Fight";
	case 106: return "Eject";
	case 107: return "Scoop Up";
	case 108: return "Eject Spin";
	case 109: return "Dock";
	case 110: return "Dark Reign Shoot";
	case 111: return "Ripper End Drop";
	case 112: return "Ripper Attach Cargo Pod";
	case 113: return "Eject Fighter Attack";
	case 114: return "Disrupted";
	case 115: return "Capship List Left";
	case 116: return "Capship List Right";
	case 117: return "Friendly Fire";
	case 118: return "Eject Player";
	case 119: return "Ship Follow Curve Backwards";
	case 120: return "Mill";
	case 121: return "Deathmatch Respawn Effect";
	case 122: return "Deathmatch Dark Reign Target";
	default: return "Unknown";
	}
}

bool same_command(const Command& left, const Command& right)
{
	// AI_command_insert, LANCER.EXE 0x0040cc10, compares only the four
	// packed signed 16-bit fields at command +0x00..+0x07. TargetKind is
	// reimplementation publication metadata and cannot affect identity.
	return left.id == right.id
		&& left.selector == right.selector
		&& left.target == right.target
		&& left.target_component == right.target_component;
}

bool same_deferred_identity(const Command& left, const Command& right)
{
	// AI_defer_command, LANCER.EXE 0x00402660, compares only the packed
	// first four signed 16-bit command fields. Target-kind is publication
	// metadata in the reimplementation, not part of retail's identity.
	return left.id == right.id
		&& left.selector == right.selector
		&& left.target == right.target
		&& left.target_component == right.target_component;
}

void remove_deferred_command(
	game::WorldObject& object,
	std::uint8_t removed)
{
	for (std::uint8_t index = removed + 1;
		index < object.ai.deferred_command_count;
		++index)
	{
		object.ai.deferred_commands[index - 1] =
			object.ai.deferred_commands[index];
	}
	--object.ai.deferred_command_count;
	if (object.ai.deferred_command_count
		< game::kMaxAiCommandsPerObject)
	{
		object.ai.deferred_commands[
			object.ai.deferred_command_count] = {};
	}
}

std::int32_t command_priority(std::int16_t id)
{
	const CommandDefinitionPolicy* policy = command_policy(id);
	return policy == nullptr ? 0 : policy->priority;
}

bool command_is_immediate(std::int16_t id)
{
	return (command_flags(id) & kCommandFlagImmediateDispatch) != 0;
}

void reset_work(game::WorldObject& object)
{
	object.ai.work = {};
	object.ai.work.begin_pending = true;
	object.selected_target_index = UINT16_MAX;
}

void ion_cannon_cleanup(Work& work);

void end_dock_command(
	game::WorldObject& object,
	const Command& command)
{
	// AI_Dock_end dispatches through the mode table. Modes zero/one only
	// clear GameObject+0x618. Modes two/three additionally restore ordinary
	// motion ownership, clear 0x00400000, and unlink +0x61c. Mode four only
	// unlinks the two retained object references.
	const std::uint8_t mode =
		static_cast<std::uint8_t>(command.state[0] & 0xffu);
	object.interaction_target_link = UINT16_MAX;
	if (mode >= 2)
	{
		object.docking_pair_link = UINT16_MAX;
	}
	if (mode == 2 || mode == 3)
	{
		object.flight_callback_mode =
			game::FlightCallbackMode::standard_forward;
		object.runtime_flags &= ~game::kObjectFlagKinematic;
	}
}

void end_active_command(
	game::World& world,
	game::WorldObject& object,
	const Command& command)
{
	if (object.ai.work.begin_pending)
	{
		return;
	}
	const std::int16_t command_id = command.id;
	if (command_id == 8 || command_id == 13 || command_id == 104)
	{
		game::world_clear_scene_attachment(object);
	}
	if (command_id == 109)
	{
		end_dock_command(object, command);
	}
	else if (command_id == 17)
	{
		// AI_ShipFollowCurve_end, LANCER.EXE 0x00403550. Only the reverse
		// provider returns to standard reverse; every other incoming mode
		// returns to standard forward.
		object.flight_callback_mode =
			object.flight_callback_mode
					== game::FlightCallbackMode::provider_reverse
				? game::FlightCallbackMode::standard_reverse
				: game::FlightCallbackMode::standard_forward;
	}
	else if (command_id == 107)
	{
		// AI_ScoopUp_end, LANCER.EXE 0x0041bc70. The command and shared
		// work are deliberately still live here: retail first releases the
		// target reservation, then the tractor allocation, then publication.
		const std::uint16_t target_index =
			object.ai.work.scoop_up.target_object;
		if (target_index < game::kMaxGameObjects)
		{
			// The retail command stores a raw live-object index and does not
			// generation-check this release.
			world.objects[target_index].runtime_flags &= ~0x00001000u;
		}
		game::tractor_effect_release(
			world.death_effects,
			object.ai.work.scoop_up.effect_slot);
		object.state_publication_flags &= ~game::kObjectStateScoopActive;
	}
	else if (command_id == 121)
	{
		const std::int16_t slot =
			object.ai.work.respawn.effect_slot;
		// AI_DeathmatchRespawnEffect_end, LANCER.EXE 0x004b0e80,
		// returns without touching the ship when its global slot is gone.
		if (game::respawn_effect_active(
				world.death_effects, slot))
		{
			object.runtime_flags &= ~0x00000004u;
			game::respawn_effect_release(
				world.death_effects, slot);
			game::respawn_effect_clear_model_override(object);
			object.protection_state = 0;
		}
	}
	else if (command_id == 119)
	{
		// AI_ShipFollowCurveBackwards_end, LANCER.EXE 0x004038c0.
		object.flight_callback_mode =
			game::FlightCallbackMode::standard_forward;
	}
	else if (command_id == 114)
	{
		// AI_Disrupted_end, LANCER.EXE 0x0040c390.
		object.runtime_flags &= ~0x00000008u;
	}
	else if (command_id == 12)
	{
		// AI_RipperGrab_end, LANCER.EXE 0x00410b50.
		object.runtime_flags &= ~game::kObjectFlagKinematic;
		game::ripper_grab_effect_release(
			world.death_effects,
			object.ai.work.ripper_grab.effect_slot);
		object.ai.work.ripper_grab.effect_slot = -1;
	}
	else if (command_id == 110)
	{
		// AI_DarkReignShoot_end, LANCER.EXE 0x0040d1e0.
		std::fill(
			std::begin(object.ai_sequence_sync),
			std::end(object.ai_sequence_sync),
			0);
		ion_cannon_cleanup(object.ai.work);
	}
}

float command_state_float(std::uint32_t bits)
{
	float value;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

game::WorldObject* resolve_target(
	game::WorldObject& actor,
	const Command& command,
	game::World& world,
	mission::Runtime& mission)
{
	if (command.target_kind == TargetKind::object)
	{
		return mission::runtime_resolve_object(
			mission, command.target, world);
	}
	if (command.target_kind == TargetKind::world_object)
	{
		if (command.target >= game::kMaxGameObjects)
		{
			return nullptr;
		}
		game::WorldObject& target = world.objects[command.target];
		return target.active ? &target : nullptr;
	}
	mission::ReferenceKind kind;
	if (command.target_kind == TargetKind::group)
	{
		kind = mission::ReferenceKind::group;
	}
	else if (command.target_kind == TargetKind::set)
	{
		kind = mission::ReferenceKind::set;
	}
	else
	{
		return nullptr;
	}
	std::uint16_t candidates[game::kMaxMissionObjects];
	const std::uint16_t count = mission::runtime_expand_reference(
		mission,
		kind,
		command.target,
		candidates,
		static_cast<std::uint16_t>(std::size(candidates)));
	game::WorldObject* nearest = nullptr;
	float nearest_distance = 0.0f;
	for (std::uint16_t ordinal = 0; ordinal < count; ++ordinal)
	{
		game::WorldObject* candidate = mission::runtime_resolve_object(
			mission, candidates[ordinal], world);
		if (candidate == nullptr || candidate == &actor)
		{
			continue;
		}
		const glm::vec3 delta = candidate->position - actor.position;
		const float distance = glm::dot(delta, delta);
		if (nearest == nullptr || distance < nearest_distance)
		{
			nearest = candidate;
			nearest_distance = distance;
		}
	}
	return nearest;
}

std::uint16_t named_model_reference(
	const game::WorldObject& object,
	const char* name)
{
	const game::ObjectModelReference* model =
		game::retained_find_named_model(object, name);
	return model == nullptr
		? UINT16_MAX
		: static_cast<std::uint16_t>(
			model - object.model_references.data());
}

glm::mat4 object_model_world_transform(
	const game::WorldObject& object,
	std::uint16_t reference)
{
	const glm::mat4 root = math::model_transform(
		object.orientation, 1.0f, object.position);
	return reference < object.model_references.size()
		? root * game::model_animation_render_transform(
			object, reference, 1.0f)
		: root;
}

void ion_cannon_cleanup(Work& work)
{
	IonCannonWork& ion = work.ion_cannon;
	ion.target_field_fraction = 0.0f;
	ion.focus_active = false;
	ion.beam_active = false;
	ion.impact_active = false;
}

glm::vec3 resolved_target_point(
	const game::WorldObject& target,
	std::int16_t component)
{
	if (component >= 0 && component < target.component_count)
	{
		for (const game::ObjectModelReference& model
			: target.model_references)
		{
			if (model.component_index == component
				&& !model.removed)
			{
				return target.position
					+ target.orientation
						* glm::vec3(model.local_transform[3]);
			}
		}
		return target.position
			+ target.orientation
				* target.components[component].local_position;
	}
	return target.position;
}

glm::mat3 look_at_points(
	const glm::vec3& from,
	const glm::vec3& to,
	float roll)
{
	// SR_mat3_look_at_points, LANCER.EXE 0x004c1940. Mill stores this
	// matrix rather than a radial vector: first yaw the identity toward
	// the actor, transform the remaining delta into that partial frame,
	// then apply pitch and the requested roll.
	glm::vec3 delta = to - from;
	glm::mat3 orientation{1.0f};
	orientation = math::postrotate(
		orientation,
		std::atan2(delta.x, delta.z),
		glm::vec3{0.0f, 1.0f, 0.0f});
	delta = glm::transpose(orientation) * delta;
	orientation = math::postrotate(
		orientation,
		-std::atan2(delta.y, delta.z),
		glm::vec3{1.0f, 0.0f, 0.0f});
	return math::postrotate(
		orientation,
		roll,
		glm::vec3{0.0f, 0.0f, 1.0f});
}

float resolved_target_radius(
	const game::WorldObject& target,
	std::int16_t component)
{
	if (component >= 0 && component < target.component_count)
	{
		for (const game::ObjectModelReference& model
			: target.model_references)
		{
			if (model.component_index == component
				&& !model.removed)
			{
				return model.radius;
			}
		}
		return target.components[component].radius;
	}
	return target.radius;
}

bool target_reference_valid(
	const game::WorldObject& target,
	std::int16_t component,
	std::uint32_t allowed_runtime_flags = 0)
{
	// TargetRef_is_valid, LANCER.EXE 0x00401870. `active` owns retail's
	// live-object bit; this is the exact object rejection mask and the
	// selected model must not be hidden or destroyed.
	if (!target.active
		|| !target.targetable
		|| (target.runtime_flags
			& ~allowed_runtime_flags
			& 0x10000d40u) != 0)
	{
		return false;
	}
	if (component < 0)
	{
		return true;
	}
	return component < target.component_count
		&& target.components[component].model_reference >= 0
		&& static_cast<std::size_t>(
			target.components[component].model_reference)
			< target.model_references.size()
		&& (target.components[component].runtime_flags & 0x0030u) == 0;
}

game::ObjectHandle object_handle(
	const game::World& world,
	const game::WorldObject& object)
{
	return {
		static_cast<std::uint16_t>(
			&object - std::begin(world.objects)),
		object.generation,
	};
}

bool is_local_actor(
	const game::World& world,
	const game::WorldObject& actor)
{
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	return player == &actor;
}

void emit_jumped_in_completion(
	game::World& world,
	mission::Runtime& mission,
	std::uint16_t actor_index,
	std::uint16_t actor_mission_index)
{
	// AI_WarpIn_update 0x0041f9de..0x0041fa2f and AI_JumpIn_update use
	// the same caller-owned multiplayer publication rule. The first live
	// reserved player object also triggers JumpedIn on mission root zero;
	// a nonzero actor then receives its ordinary self event as well.
	bool actor_event_emitted = false;
	if (mission.network.role != mission::NetworkRole::offline)
	{
		const std::uint16_t player_limit =
			std::min<std::uint16_t>(
				mission.player_prefix_count,
				static_cast<std::uint16_t>(
					game::kMaxGameObjects));
		std::uint16_t first_live = 0;
		while (first_live < player_limit
			&& (!world.objects[first_live].active
				|| (world.objects[first_live].runtime_flags
					& 0x10000840u) != 0))
		{
			++first_live;
		}
		if (first_live < player_limit
			&& first_live == actor_index)
		{
			mission::events_emit_jumped_in(
				mission,
				world.objects[0].mission_index);
			actor_event_emitted = actor_index == 0;
		}
	}
	if (!actor_event_emitted)
	{
		mission::events_emit_jumped_in(
			mission, actor_mission_index);
	}
}

bool ai_sequence_sync(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& mission,
	std::uint8_t sync_index)
{
	if (mission.network.role == mission::NetworkRole::offline)
	{
		return true;
	}
	if (sync_index >= std::size(actor.ai_sequence_sync))
	{
		return false;
	}
	const std::uint8_t local = mission.network.local_player;
	const std::uint8_t local_bit =
		static_cast<std::uint8_t>(1u << local);
	std::uint8_t& reached = actor.ai_sequence_sync[sync_index];
	if ((reached & local_bit) == 0)
	{
		reached |= local_bit;
		mission::network_publish_ai_sequence_sync(
			mission.network,
			object_handle(world, actor).index,
			sync_index);
	}
	for (std::uint8_t player = 0;
		player < mission.network.player_count;
		++player)
	{
		if (mission.network.connected[player]
			&& (reached & static_cast<std::uint8_t>(1u << player))
				== 0)
		{
			return false;
		}
	}
	reached = 0;
	return true;
}

void transition_request_camera(
	mission::Runtime& runtime,
	const game::World& world,
	std::uint8_t mode,
	bool lock,
	bool override_lock)
{
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	runtime.requested_camera_mode = mode;
	runtime.requested_camera_target =
		player == nullptr ? UINT16_MAX : player->mission_index;
	runtime.requested_camera_lock = lock;
	runtime.requested_camera_override_lock = override_lock;
	mission::runtime_publish_camera_request(runtime);
}

void death_request_camera(
	mission::Runtime& runtime,
	const game::World& world,
	const game::WorldObject& target,
	std::uint8_t mode)
{
	runtime.requested_camera_mode = mode;
	runtime.requested_camera_target =
		target.mission_index != UINT16_MAX
			? target.mission_index
			: static_cast<std::uint16_t>(
				&target - std::begin(world.objects));
	runtime.requested_camera_lock = true;
	runtime.requested_camera_override_lock = true;
	mission::runtime_publish_camera_request(runtime, true);
}

void death_queue_speech(
	mission::Runtime& mission,
	const char* path)
{
	std::snprintf(
		mission.presentation.standalone_speech_path,
		sizeof(mission.presentation.standalone_speech_path),
		"%s",
		path);
	mission.presentation.speech_pending = true;
	mission.presentation.speech_request_serial =
		++mission.presentation.slot_zero_request_serial;
}

glm::vec3 death_particle_camera_position(
	const mission::Runtime& mission)
{
	return mission.particle_camera_position;
}

glm::vec3 death_particle_camera_forward(
	const mission::Runtime& mission)
{
	return mission.particle_camera_forward;
}

glm::vec3 ripper_grab_point(
	const game::WorldObject& target,
	std::int16_t component,
	std::uint16_t mission_number)
{
	if (component >= 0 && component < target.component_count)
	{
		const std::int16_t reference =
			target.components[component].model_reference;
		if (reference >= 0
			&& static_cast<std::size_t>(reference)
				< target.model_references.size())
		{
			const glm::mat4 frame =
				math::model_transform(
					target.orientation, 1.0f, target.position)
				* game::model_animation_render_transform(
					target,
					static_cast<std::uint16_t>(reference),
					1.0f);
			// AI_RipperGrab_component_offset (0x00412480) supplies the
			// authored 2500-unit Y offset for target types 0x21/0x45.
			const glm::vec3 local =
				target.type == 0x21u || target.type == 0x45u
					? glm::vec3{0.0f, 2500.0f, 0.0f}
					: glm::vec3{0.0f};
			return glm::vec3(frame * glm::vec4(local, 1.0f));
		}
	}
	if (component < 0 && mission_number == 26)
	{
		return target.position
			+ target.orientation * glm::vec3{0.0f, -1000.0f, 0.0f};
	}
	return target.position;
}

glm::vec3 ripper_named_node_position(
	const game::WorldObject& actor,
	const char* name)
{
	const game::ObjectModelReference* model =
		game::retained_find_named_model(actor, name);
	if (model == nullptr)
	{
		return actor.position;
	}
	const std::uint16_t reference = static_cast<std::uint16_t>(
		model - actor.model_references.data());
	const glm::mat4 world_transform =
		math::model_transform(
			actor.orientation, 1.0f, actor.position)
		* game::model_animation_render_transform(
			actor, reference, 1.0f);
	const glm::vec3 local =
		model->mesh_quad_vertex_count != 0
			? model->mesh_quad_vertices[0]
			: glm::vec3{0.0f};
	return glm::vec3(world_transform * glm::vec4(local, 1.0f));
}

void ripper_beam_origins(
	const game::WorldObject& actor,
	glm::vec3 (&origins)[4])
{
	static constexpr const char* kPincerNames[4] = {
		"Ripper Back pincer 2",
		"Ripper Back pincer 03",
		"Ripper Back pincer 04",
		"Ripper Back pincer 05",
	};
	for (std::size_t index = 0; index < std::size(origins); ++index)
	{
		origins[index] =
			ripper_named_node_position(actor, kPincerNames[index]);
	}
}

void ripper_beam_targets(
	const game::WorldObject& target,
	const glm::vec3& fallback,
	glm::vec3 (&targets)[4])
{
	std::fill(std::begin(targets), std::end(targets), fallback);
	if (target.type != 0x91u && target.type != 0xe1u)
	{
		return;
	}
	const game::ObjectModelReference* cargo =
		game::retained_find_named_model(target, "Cargo pod");
	if (cargo == nullptr || cargo->mesh_quad_vertex_count < 4)
	{
		return;
	}
	const std::uint16_t reference = static_cast<std::uint16_t>(
		cargo - target.model_references.data());
	const glm::mat4 frame =
		math::model_transform(
			target.orientation, 1.0f, target.position)
		* game::model_animation_render_transform(
			target, reference, 1.0f);
	const glm::vec3 first =
		fallback
		+ glm::vec3(
			frame
			* glm::vec4(
				(cargo->mesh_quad_vertices[0]
						+ cargo->mesh_quad_vertices[1])
					* 0.5f,
				1.0f))
		- target.position;
	const glm::vec3 second =
		fallback
		+ glm::vec3(
			frame
			* glm::vec4(
				(cargo->mesh_quad_vertices[2]
						+ cargo->mesh_quad_vertices[3])
					* 0.5f,
				1.0f))
		- target.position;
	targets[0] = first;
	targets[1] = first;
	targets[2] = second;
	targets[3] = second;
}

void ripper_set_cargo_node_hidden(
	game::WorldObject& object,
	bool hidden)
{
	(void)game::retained_set_named_model_hidden(
		object, "Cargo pod", hidden);
}

void ripper_start_animation_all(
	game::WorldObject& actor,
	const char* name,
	float time,
	std::int16_t mode,
	float rate)
{
	// SR_model_play_sequence_recursive, LANCER.EXE 0x0049a400, applies
	// the named sequence to the retained root and every child model.
	for (std::uint16_t reference = 0;
		reference < actor.model_references.size();
		++reference)
	{
		game::model_animation_start_named(
			actor, reference, name, time, mode, rate);
	}
}

float ripper_cosine_interpolate(float start, float end, float time)
{
	const float fraction =
		(std::cos(time * glm::pi<float>() + glm::pi<float>())
				+ 1.0f)
			* 0.5f;
	return start + (end - start) * fraction;
}

game::ObjectModelReference* ripper_component_model(
	game::WorldObject& target,
	std::int16_t component)
{
	if (component < 0 || component >= target.component_count)
	{
		return nullptr;
	}
	const std::int16_t reference =
		target.components[component].model_reference;
	if (reference < 0
		|| static_cast<std::size_t>(reference)
			>= target.model_references.size()
		|| target.model_references[reference].removed)
	{
		return nullptr;
	}
	return &target.model_references[reference];
}

void ripper_attach_component_frame(
	game::WorldObject& target,
	std::int16_t component,
	glm::vec3& approach,
	glm::vec3& component_position,
	glm::vec3& component_rotation)
{
	game::ObjectModelReference* model =
		ripper_component_model(target, component);
	if (model == nullptr)
	{
		return;
	}
	const std::uint16_t reference = static_cast<std::uint16_t>(
		model - target.model_references.data());
	const glm::mat4 frame =
		object_model_world_transform(target, reference);
	const float approach_y =
		target.type == 0x3du || target.type == 0x48u
			? -2500.0f
			: 2500.0f;
	approach = glm::vec3(
		frame * glm::vec4(0.0f, approach_y, 0.0f, 1.0f));
	component_position = glm::vec3(frame[3]);
	glm::mat3 adjustment{1.0f};
	switch (target.type)
	{
	case 0x21u:
	case 0x3du:
	case 0x48u:
	case 0xe3u:
	case 0xe4u:
	case 0xe5u:
	case 0xe6u:
	case 0xe7u:
	case 0xe8u:
	case 0xe9u:
	case 0xeau:
	case 0xebu:
	case 0xecu:
	case 0xedu:
	case 0xeeu:
	case 0xefu:
		adjustment = glm::mat3(
			glm::rotate(
				glm::mat4(1.0f),
				-glm::half_pi<float>(),
				glm::vec3{1.0f, 0.0f, 0.0f}));
		break;
	default:
		adjustment = glm::mat3(
			glm::rotate(
				glm::mat4(1.0f),
				-glm::half_pi<float>(),
				glm::vec3{0.0f, 0.0f, 1.0f}));
		break;
	}
	component_rotation =
		math::rotation_to_euler(glm::mat3(frame) * adjustment);
}

bool eject_split_cockpit(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick)
{
	Work& work = actor.ai.work;
	std::int16_t cockpit_reference = -1;
	for (std::size_t index = 0;
		index < actor.model_references.size();
		++index)
	{
		if (!actor.model_references[index].removed
			&& actor.model_references[index].model_type == 2)
		{
			cockpit_reference = static_cast<std::int16_t>(index);
			break;
		}
	}
	if (cockpit_reference < 0)
	{
		diagnostics::mission_log(
			"ejection split rejected actor=%u reason=no-class-two-model",
			static_cast<unsigned>(actor.mission_index));
		return false;
	}
	// AI_Eject_split_cockpit destroys GameObject+0x65c and clears +0x660
	// at LANCER.EXE 0x00415712..0x00415730 before moving the hull models.
	// The retained launch trail is that externally serviced render effect.
	actor.external_trail_active = false;
	work.ejection.saved_protection_state = actor.protection_state;
	const game::ObjectModelReference& cockpit =
		actor.model_references[
			static_cast<std::size_t>(cockpit_reference)];
	glm::vec3 ejection_direction = actor.orientation[2];
	glm::vec3 marker_position{0.0f};
	glm::mat3 marker_basis{1.0f};
	bool has_ejection_marker = false;
	if (cockpit.point_groups != nullptr)
	{
		const glm::mat4 transform =
			math::model_transform(
				actor.orientation, 1.0f, actor.position)
			* game::model_animation_render_transform(
				actor,
				static_cast<std::uint16_t>(cockpit_reference),
				1.0f);
		for (const assets::GameplayPointGroup& group
			: *cockpit.point_groups)
		{
			if (group.type != 6 || group.points.empty())
			{
				continue;
			}
			const assets::GameplayPoint& marker = group.points[0];
			marker_position =
				glm::vec3(
					transform * glm::vec4(marker.position, 1.0f));
			marker_basis = glm::mat3(transform);
			has_ejection_marker = true;
			ejection_direction =
				marker_basis * marker.direction;
		}
	}

	const game::ObjectHandle separated_handle = game::world_create(
		world,
		1000,
		actor.position,
		actor.orientation,
		stats,
		false);
	game::WorldObject* separated =
		game::world_resolve(world, separated_handle);
	if (separated == nullptr)
	{
		diagnostics::mission_log(
			"ejection split rejected actor=%u reason=object-pool",
			static_cast<unsigned>(actor.mission_index));
		return false;
	}
	separated->type = actor.type;
	separated->radius = 4000.0f;
	command_push(world,
		*separated,
		106,
		TargetKind::world_object,
		static_cast<std::uint16_t>(
			&actor - std::begin(world.objects)));
	separated->components_initialized = true;
	separated->model_references = actor.model_references;
	separated->primary_model_reference_count =
		actor.primary_model_reference_count;
	separated->embedded_model_trees = actor.embedded_model_trees;
	separated->linear_velocity = actor.linear_velocity;
	separated->speed = actor.speed;
	const float tumble_z =
		(static_cast<float>(game::world_rand15(world))
				* 0x1.0002p-15f
			- 0.5f)
		* 0.01f;
	const float tumble_y =
		(static_cast<float>(game::world_rand15(world))
				* 0x1.0002p-15f
			- 0.5f)
		* 0.01f;
	actor.inertial_angular_step = math::rotation_from_euler({
		actor.angular_x,
		actor.angular_y,
		actor.angular_z,
	});
	separated->inertial_angular_step =
		actor.inertial_angular_step
		* math::rotation_from_euler({
			0.02f,
			tumble_y,
			tumble_z,
		});
	separated->runtime_flags = 0x01000800u;
	separated->flight_callback_mode =
		game::FlightCallbackMode::damp_velocity_0_99;
	game::world_queue_sound_object(
		world,
		separated_handle,
		48,
		static_cast<std::uint8_t>(
			is_local_actor(world, actor) ? 4 : 0));
	separated->interaction_target_link =
		static_cast<std::uint16_t>(&actor - std::begin(world.objects));
	actor.interaction_target_link = separated_handle.index;

	// The original object becomes the ejecting cockpit/pilot owner. The
	// linked object receives every other direct model owner. Retail moves
	// the references rather than cloning two complete component systems.
	for (std::size_t index = 0;
		index < actor.model_references.size();
		++index)
	{
		const bool removed = actor.model_references[index].removed;
		actor.model_references[index].removed =
			removed
			|| index != static_cast<std::size_t>(cockpit_reference);
		separated->model_references[index].removed =
			removed
			|| index == static_cast<std::size_t>(cockpit_reference);
	}
	actor.component_count = 0;
	separated->component_count = 0;
	actor.attachment_count = 0;
	actor.gun_mount_count = 0;
	actor.gun_pair_count = 0;
	actor.gun_group_count = 0;
	std::fill(
		std::begin(actor.primary_shields),
		std::end(actor.primary_shields),
		0.0f);
	std::fill(
		std::begin(actor.secondary_shields),
		std::end(actor.secondary_shields),
		0.0f);
	if (has_ejection_marker)
	{
		game::particle_emitter_burst_world(
			world,
			marker_position,
			marker_basis,
			{0.0f, 0.0f, 0.0f},
			{1.0f, 1.0f, 0.2f},
			20.0f,
			5.0f,
			100,
			game::ParticleEmitterStyle::ejection_cockpit,
			tick,
			death_particle_camera_position(mission),
			death_particle_camera_forward(mission));
	}
	actor.linear_velocity += ejection_direction * -50.0f;
	actor.speed = glm::length(actor.linear_velocity);
	actor.control_demand = {};
	actor.runtime_flags |= 0x00000808u;
	actor.protection_state = 2;
	actor.flight_callback_mode =
		game::FlightCallbackMode::none;
	work.ejection.separated_object = separated_handle.index;
	work.ejection.separated_generation = separated_handle.generation;
	work.ejection.split_complete = true;
	work.deadline = tick + 100;
	work.stage = 0;
	diagnostics::mission_log(
		"ejection split actor=%u cockpit_model=%d separated=%u tick=%u",
		static_cast<unsigned>(actor.mission_index),
		static_cast<int>(cockpit_reference),
		static_cast<unsigned>(separated_handle.index),
		tick);
	return true;
}

game::WorldObject* eject_create_outcome(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	std::uint8_t outcome,
	std::uint32_t tick)
{
	const bool hostile = outcome == 1;
	const std::uint16_t type =
		outcome == 2 ? 0x18u : outcome == 3 ? 0x46u : 0x2bu;
	glm::vec3 position = actor.position;
	glm::mat3 orientation = actor.orientation;
	if (hostile)
	{
		position += glm::vec3{20000.0f, 10000.0f, -20000.0f};
		orientation = math::postrotate(
			orientation,
			-1.57079632679489661923f,
			{0.0f, 1.0f, 0.0f});
	}
	else
	{
		position.z -= 15000.0f;
	}
	// AI_Eject_update always constructs the outcome actor in retail's
	// dedicated live-object slot DAT_0057e04e (initialized to 399 at
	// 0x004935ef), rather than consuming the ordinary allocation cursor.
	const game::ObjectHandle handle = game::world_create_at(
		world,
		static_cast<std::uint16_t>(game::kMaxGameObjects - 1u),
		type,
		position,
		orientation,
		stats,
		false);
	game::WorldObject* result = game::world_resolve(world, handle);
	if (result == nullptr)
	{
		return nullptr;
	}
	const std::uint16_t actor_index = static_cast<std::uint16_t>(
		&actor - std::begin(world.objects));
	if (hostile)
	{
		command_push(world,
			*result,
			113,
			TargetKind::world_object,
			actor_index);
		death_request_camera(mission, world, *result, 29);
		actor.radius *= 2.0f;
	}
	else
	{
		command_push(world,
			*result,
			107,
			TargetKind::world_object,
			actor_index);
		death_request_camera(mission, world, *result, 28);
	}
	diagnostics::mission_log(
		"ejection outcome actor=%u result=%u object=%u type=%u tick=%u",
		static_cast<unsigned>(actor.mission_index),
		static_cast<unsigned>(outcome),
		static_cast<unsigned>(handle.index),
		static_cast<unsigned>(type),
		tick);
	return result;
}

void eject_choose_outcome(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick)
{
	// LANCER.EXE 0x00415cb0 writes four to DAT_00587cd4 before
	// 0x00415cdd..0x00415cf7 publishes the actual campaign coordinator
	// result. The port retains that shared retail dword in both the mission
	// result bridge and the world/camera bridge, so publish both views.
	mission.mission_result_code = 4;
	world.cinematic_mode = 4;
	// LANCER.EXE 0x415cbf..0x415ced loads all three command-68 dwords as
	// signed ints, adds them with 32-bit wraparound, and uses signed IDIV.
	const std::int32_t rescue =
		static_cast<std::int32_t>(mission.rescue_probability);
	const std::int32_t capture =
		static_cast<std::int32_t>(mission.capture_probability);
	const std::int32_t destroyed =
		static_cast<std::int32_t>(mission.destroyed_probability);
	const std::uint32_t total_bits =
		static_cast<std::uint32_t>(rescue)
		+ static_cast<std::uint32_t>(capture)
		+ static_cast<std::uint32_t>(destroyed);
	const std::int32_t total =
		static_cast<std::int32_t>(total_bits);
	if (total == 0)
	{
		// Retail raises an integer divide exception here. Shipped mission
		// state starts at 100/0/0 and never supplies a zero total.
		diagnostics::mission_log(
			"ejection outcome rejected actor=%u reason=zero-weight",
			static_cast<unsigned>(actor.mission_index));
		return;
	}
	const std::int32_t roll =
		static_cast<std::int32_t>(game::world_rand15(world)) % total;
	const std::int32_t capture_limit = static_cast<std::int32_t>(
		static_cast<std::uint32_t>(rescue)
		+ static_cast<std::uint32_t>(capture));
	const std::uint8_t outcome =
		roll < rescue ? 2
			: roll < capture_limit ? 3
			: 1;
	mission.gameplay_state = outcome;
	// AI_Eject_update constructs Vector(0, -10000000, 0) at
	// LANCER.EXE 0x00415cfc before replacing the cockpit's live pose.
	actor.previous_position = {0.0f, -10000000.0f, 0.0f};
	actor.position = actor.previous_position;
	actor.previous_orientation = glm::mat3{1.0f};
	actor.orientation = actor.previous_orientation;
	const char* speech = nullptr;
	if (outcome == 2)
	{
		speech = (game::world_rand15(world) & 1u) != 0
			? "nanpkup_002.ut" : "nanpkup_001.ut";
	}
	else if (outcome == 3)
	{
		speech = (game::world_rand15(world) & 1u) != 0
			? "antpkup_002.ut" : "antpkup_001.ut";
	}
	else
	{
		speech = (game::world_rand15(world) & 1u) != 0
			? "ejtkll_002.ut" : "ejtkll_001.ut";
	}
	death_queue_speech(mission, speech);
	eject_create_outcome(actor, world, mission, stats, outcome, tick);
	actor.ai.work.ejection.outcome = outcome;
}

void death_finalize_object(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick)
{
	const game::ObjectHandle handle = object_handle(world, actor);
	const std::uint16_t world_index = handle.index;
	const std::uint16_t mission_index = actor.mission_index;
	const bool deathmatch_respawn =
		mission.network.role != mission::NetworkRole::offline
		&& mission.network.deathmatch_mode;
	if (!actor.ai.work.explode.destruction_notified
		&& actor.sound3d_slot == 0)
	{
		mission::runtime_mark_multiplayer_pilot_destroyed(
			mission, actor.pilot);
		if (mission.player_comms.all_channels_open)
		{
			mission::player_comms_on_pilot_death(
				mission, world, world_index, tick);
		}
	}
	if (actor.player && !deathmatch_respawn)
	{
		mission.player_death_transition_complete = true;
	}
	if (mission_index < mission.object_count)
	{
		if (!actor.ai.work.explode.destruction_notified)
		{
			mission::events_emit_destroyed(
				mission,
				mission_index,
				last_attacker_mission_index(world, actor));
		}
	}
	if (deathmatch_respawn)
	{
		// AI_ExplodeOrdinary_update, LANCER.EXE
		// 0x00408ac1..0x00408b94. Deathmatch deaths retain the player
		// slot instead of converting it to the departed placeholder.
		// The local owner reconstructs immediately and publishes the spawn;
		// remote slots wait under Multiplayer Control for opcode 0x2c.
		command_pop(world, actor);
		if (world_index < mission.player_prefix_count
			&& world_index
				< mission::kDeathmatchScenarioPlayerCapacity)
		{
			mission.deathmatch.player_pickup_ready_tick[world_index] =
				static_cast<std::int32_t>(tick + 100u);
		}
		actor.runtime_flags &= ~game::kObjectFlagDestroyed;
		if (world_index == world.player.index)
		{
			if (mission::network_reset_player_to_spawn(
					mission,
					world,
					stats,
					world_index,
					-1,
					true))
			{
				game::WorldObject& respawned =
					world.objects[world_index];
				respawned.runtime_flags &= ~0x00000008u;
				game::world_zero_motion_controls(respawned);
				transition_request_camera(
					mission, world, 0, false, true);
			}
		}
		else
		{
			command_push(
				world,
				actor,
				101,
				TargetKind::none,
				UINT16_MAX);
		}
		return;
	}
	diagnostics::mission_log(
		"death object departed actor=%u world=%u tick=%u",
		static_cast<unsigned>(mission_index),
		static_cast<unsigned>(world_index),
		tick);
	game::world_mark_departed(world, handle);
}

void death_notify_object(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick)
{
	ExplodeWork& explode = actor.ai.work.explode;
	if (explode.destruction_notified)
	{
		return;
	}
	explode.destruction_notified = true;
	const std::uint16_t world_index =
		static_cast<std::uint16_t>(
			&actor - std::begin(world.objects));
	actor.targetable = false;
	actor.runtime_flags &= ~0x00000200u;
	if (actor.sound3d_slot == 0)
	{
		mission::runtime_mark_multiplayer_pilot_destroyed(
			mission, actor.pilot);
		if (mission.player_comms.all_channels_open)
		{
			mission::player_comms_on_pilot_death(
				mission,
				world,
				world_index,
				tick);
		}
	}
	if (actor.mission_index < mission.object_count)
	{
		mission::events_emit_destroyed(
			mission,
			actor.mission_index,
			last_attacker_mission_index(world, actor));
	}
	// AI_ExplodeOrdinary_begin has two independent camera owners:
	// 0x0040893b..0x004089bb covers the remote player currently selected
	// by multiplayer spectator state, while 0x004089c0..0x00408a38 covers
	// the local player. Both branches require cinematic state zero.
	const bool owns_death_camera =
		actor.player
		|| (world.multiplayer_spectator_active
			&& world.multiplayer_spectator_target == world_index);
	if (owns_death_camera && world.cinematic_mode == 0)
	{
		// AI_ExplodeOrdinary_begin calls GameObject_effective_max_speed
		// (0x00403060), multiplies it by GameObject+0x5b8 throttle, and
		// compares the product with 10000.0f at 0x004dc440.
		const float commanded_speed =
			game::world_effective_max_speed(
				actor, stats, world.camera_mode)
			* actor.throttle;
		const std::uint8_t camera_mode =
			explode.variant == 1 ? 27
				: explode.variant == 2 ? 8
				: commanded_speed >= 10000.0f ? 26 : 8;
		if (camera_mode == 27)
		{
			world.death_effects.camera_mode27_anchor = actor.position;
			world.death_effects.camera_mode27_anchor_owner =
				world_index;
			world.death_effects.camera_mode27_anchor_generation =
				actor.generation;
			world.death_effects.camera_mode27_anchor_valid = true;
		}
		death_request_camera(
			mission, world, actor, camera_mode);
		if (actor.player)
		{
			// The local branch publishes coordinator result one after its
			// camera dispatch at 0x00408a3d. The spectator branch above
			// deliberately leaves the mission result untouched.
			mission.gameplay_state = 1;
		}
	}
}

void death_award_ordinary_player_kill(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick)
{
	// AI_ExplodeOrdinary_begin's score producer, LANCER.EXE
	// 0x00408500..0x004085ca.
	if (!mission.player_comms.all_channels_open)
	{
		return;
	}
	const std::uint16_t actor_index = static_cast<std::uint16_t>(
		&actor - std::begin(world.objects));
	const bool multiplayer =
		mission.network.role != mission::NetworkRole::offline;
	const bool qualifying_class =
		actor.type < assets::kShipStatsCount
		&& stats.records[actor.type].object.collision_class == 1;
	// Retail temporarily clears runtime bit 0x40 while publishing the
	// award and its offline chatter, then restores the complete flag word.
	const std::uint32_t saved_runtime_flags = actor.runtime_flags;
	actor.runtime_flags &= ~game::kObjectFlagDestroyed;
	bool local_score_adjusted = false;
	const bool eligible_target =
		(actor.allegiance_class == 1
			|| (multiplayer && actor_index < 8))
		&& (qualifying_class
			|| actor.type == 0x2du
			|| actor.type == 0x3cu
			|| actor.type == 0x3eu)
		&& (!multiplayer
			|| !mission.network.team_mode
			|| mission.network.object_team[actor_index]
				!= mission.network.object_team[
					world.player.index]);
	if (actor.last_attacker_index == world.player.index
		&& eligible_target)
	{
		// Player_score_add receives publish=false at 0x004085a0. The
		// enclosing routine performs its one explicit stats send below.
		(void)mission::runtime_add_player_score(
			mission,
			world,
			world.player.index,
			1,
			false);
		local_score_adjusted = multiplayer;
		if (!multiplayer)
		{
			mission::player_comms_on_player_destroyed_target(
				mission,
				world,
				stats,
				actor_index,
				tick,
				tick);
		}
	}
	if (multiplayer
		&& (actor_index == world.player.index
			|| local_score_adjusted))
	{
		// 0x004085dd..0x004085f9 sends after the score mutation whenever
		// either the victim or the scorer is local. A local victim's death
		// increment has already performed its own publication.
		mission::network_publish_player_stats(
			mission.network, world.player.index);
	}
	actor.runtime_flags = saved_runtime_flags;
}

void death_notify_deathmatch_result(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& mission)
{
	if (mission.network.role == mission::NetworkRole::offline)
	{
		return;
	}
	const std::uint16_t victim = static_cast<std::uint16_t>(
		&actor - std::begin(world.objects));
	if (victim >= mission::kNetworkPlayerCapacity)
	{
		return;
	}
	actor.protection_state = 4;

	// AI_ExplodeOrdinary_select_variant, LANCER.EXE
	// 0x00408752..0x00408866, applies to every multiplayer player death,
	// not only deathmatch-rule missions. A valid enemy attacker gets the
	// kill form; an absent attacker or a same-team attacker gets the
	// "bought the farm" form.
	const std::uint16_t attacker = actor.last_attacker_index;
	if (attacker < mission::kNetworkPlayerCapacity
		&& (!mission.network.team_mode
			|| mission.network.object_team[victim]
				!= mission.network.object_team[attacker]))
	{
		game::death_effects_enqueue_hud_notification(
			world.death_effects,
			{
				game::DeathHudNotificationKind::ordinary_kill,
				static_cast<std::uint8_t>(victim),
				static_cast<std::uint8_t>(attacker),
				attacker == world.player.index,
			});
	}
	else if (attacker == UINT16_MAX
		|| (attacker < mission::kNetworkPlayerCapacity
			&& mission.network.team_mode
			&& mission.network.object_team[victim]
				== mission.network.object_team[attacker]))
	{
		game::death_effects_enqueue_hud_notification(
			world.death_effects,
			{
				game::DeathHudNotificationKind::bought_the_farm,
				static_cast<std::uint8_t>(victim),
				0,
				false,
			});
	}
	if (victim == world.player.index)
	{
		(void)mission::runtime_add_player_death(
			mission, world, victim, 1, true);
	}
}

void death_emit_standard_destruction(
	game::WorldObject& actor,
	game::World& world,
	const mission::Runtime& mission,
	std::uint32_t tick)
{
	// GameObject_create_death_visuals, LANCER.EXE 0x0046c980. The retained
	// runtime represents its exploding mesh and Explosion BMO systems with
	// the corresponding debris and particle pools.
	game::explosion_mesh_breakup_world(world, actor, false, tick);
	const bool reduced_fragments =
		actor.type == 0x4du
		|| actor.type == 0x90u
		|| actor.type == 0xdfu
		|| actor.type == 0xe0u
		|| actor.type == 0x6fu
		|| (actor.runtime_flags & 0x00000800u) != 0;
	game::particle_fragment_burst(
		world,
		actor.position,
		0.0f,
		reduced_fragments ? 0.2f : 0.4f,
		reduced_fragments ? 0.1f : 0.2f,
		reduced_fragments ? 5 : 25,
		tick);
	const glm::mat3 expanding_basis = math::rotation_from_euler({
		-glm::pi<float>() / 3.0f,
		0.0f,
		static_cast<float>(game::world_rand15(world))
			* 0x1.0002p-15f * glm::two_pi<float>(),
	});
	const glm::vec3 expanding_velocity =
		actor.linear_velocity
		* ((static_cast<float>(game::world_rand15(world))
				* 0x1.0002p-15f
			+ 1.0f)
			* 0.25f);
	game::particle_emitter_burst_world(
		world,
		actor.position,
		expanding_basis,
		{0.0f, 0.0f, 0.0f},
		{1.0f, 1.0f, 0.2f},
		200.0f,
		300.0f,
		400,
		game::ParticleEmitterStyle::expanding_explosion,
		tick,
		death_particle_camera_position(mission),
		death_particle_camera_forward(mission),
		expanding_velocity);
	if (game::world_rand15(world) % 4u == 0u)
	{
		const float lifetime = static_cast<float>(
			100u + game::world_rand15(world) % 50u);
		const std::uint8_t type = static_cast<std::uint8_t>(
			game::world_rand15(world) % 3u);
		(void)game::shockwave_create(
			world,
			actor.position,
			expanding_basis,
			expanding_velocity,
			type,
			actor.radius * 10.0f,
			lifetime,
			0,
			static_cast<std::int32_t>(
				&actor - std::begin(world.objects)),
			tick);
	}
	game::particle_emitter_burst_world(
		world,
		actor.position,
		glm::mat3{1.0f},
		{0.0f, 0.0f, 0.0f},
		{1.0f, 1.0f, 1.0f},
		0.0f,
		7.0f,
		150,
		game::ParticleEmitterStyle::white_debris,
		tick,
		death_particle_camera_position(mission),
		death_particle_camera_forward(mission),
		actor.linear_velocity * 0.25f);
	game::explosion_billboard_create(
		world.death_effects,
		world,
		actor.position,
		actor.linear_velocity * 0.25f,
		game::ExplosionBillboardType::separate_frames,
		actor.radius,
		150,
		true,
		0,
		false,
		false,
		tick);
	const glm::vec3 camera_delta =
		actor.position - death_particle_camera_position(mission);
	game::world_queue_sound_explicit(
		world,
		actor.position,
		actor.orientation[2],
		actor.linear_velocity,
		11,
		glm::dot(camera_delta, camera_delta) >= 400000000.0f
			? 3 : 4);
}

void death_emit_variant_one_destruction(
	game::WorldObject& actor,
	game::World& world,
	const mission::Runtime& mission,
	std::uint32_t tick)
{
	// Explode variant-one finalizer, LANCER.EXE 0x00471db0.
	game::explosion_mesh_breakup_world(world, actor, true, tick);
	game::particle_fragment_burst(
		world,
		actor.position,
		0.0f,
		0.2f,
		0.2f,
		25,
		tick);
	game::particle_emitter_burst_world(
		world,
		actor.position,
		math::rotation_from_euler({
			-glm::pi<float>() / 3.0f,
			0.0f,
			static_cast<float>(game::world_rand15(world))
				* 0x1.0002p-15f * glm::two_pi<float>(),
		}),
		{0.0f, 0.0f, 0.0f},
		{1.0f, 1.0f, 0.2f},
		20.0f,
		5.0f,
		200,
		game::ParticleEmitterStyle::expanding_explosion,
		tick,
		death_particle_camera_position(mission),
		death_particle_camera_forward(mission),
		actor.linear_velocity * 0.25f);
	game::particle_emitter_burst_world(
		world,
		actor.position,
		glm::mat3{1.0f},
		{0.0f, 0.0f, 0.0f},
		{1.0f, 1.0f, 1.0f},
		0.0f,
		7.0f,
		150,
		game::ParticleEmitterStyle::white_debris,
		tick,
		death_particle_camera_position(mission),
		death_particle_camera_forward(mission),
		actor.linear_velocity * 0.5f);
	for (std::uint8_t explosion = 0; explosion < 18; ++explosion)
	{
		const float distance =
			static_cast<float>(game::world_rand15(world))
				* 0x1.0002p-15f
				* actor.radius
				* 0.3f;
		const float angle_z =
			static_cast<float>(game::world_rand15(world))
				* 0x1.0002p-15f
				* glm::two_pi<float>();
		const float angle_y =
			static_cast<float>(game::world_rand15(world))
				* 0x1.0002p-15f
				* glm::two_pi<float>();
		const float angle_x =
			static_cast<float>(game::world_rand15(world))
				* 0x1.0002p-15f
				* glm::two_pi<float>();
		const glm::vec3 local =
			math::rotation_from_euler(
				{angle_x, angle_y, angle_z})
				* glm::vec3{distance, 0.0f, 0.0f};
		const std::uint32_t delay =
			static_cast<std::uint32_t>(
				static_cast<float>(game::world_rand15(world))
					* 0x1.0002p-15f * 10.0f);
		game::explosion_billboard_create(
			world.death_effects,
			world,
			actor.position + local,
			actor.linear_velocity * 0.5f,
			game::ExplosionBillboardType::separate_frames,
			actor.radius * 0.8f,
			150,
			true,
			delay,
			false,
			false,
			tick);
	}
	game::world_queue_sound_explicit(
		world,
		actor.position,
		actor.orientation[2],
		actor.linear_velocity,
		11,
		3);
}

float cosine_ease(float start, float midpoint, float end, float phase)
{
	if (phase <= 0.0f)
	{
		return start;
	}
	if (phase >= 1.0f)
	{
		return end;
	}
	const float eased =
		0.5f - 0.5f * std::cos(phase * 3.14159265358979323846f);
	const float linear =
		phase < 0.5f
			? glm::mix(start, midpoint, phase * 2.0f)
			: glm::mix(midpoint, end, (phase - 0.5f) * 2.0f);
	return glm::mix(start, end, eased) * 0.5f + linear * 0.5f;
}

bool tractor_attachment(
	game::WorldObject& actor,
	glm::vec3 origins[2],
	glm::vec3& pull_anchor,
	float forward_distance)
{
	const char* name = actor.type == 0x18u ? "Nanny" : "Antanov";
	game::ObjectModelReference* model =
		game::retained_find_named_model(actor, name);
	if (model == nullptr || model->point_groups == nullptr)
	{
		return false;
	}
	const assets::GameplayPointGroup* attachment = nullptr;
	for (const assets::GameplayPointGroup& group : *model->point_groups)
	{
		if (group.type == 6 && group.points.size() >= 2)
		{
			attachment = &group;
			break;
		}
	}
	if (attachment == nullptr)
	{
		return false;
	}
	const std::uint16_t reference = static_cast<std::uint16_t>(
		model - actor.model_references.data());
	const glm::mat4 transform =
		math::model_transform(actor.orientation, 1.0f, actor.position)
		* game::model_animation_render_transform(actor, reference, 1.0f);
	origins[0] = glm::vec3(
		transform
			* glm::vec4(attachment->points[0].position, 1.0f));
	origins[1] = glm::vec3(
		transform
			* glm::vec4(attachment->points[1].position, 1.0f));
	pull_anchor =
		(origins[0] + origins[1]) * 0.5f
		+ actor.orientation[2] * forward_distance;
	return true;
}

void tractor_set_doors(
	game::WorldObject& actor,
	bool opening)
{
	const char* name = actor.type == 0x18u ? "Nanny" : "Antanov";
	game::ObjectModelReference* parent =
		game::retained_find_named_model(actor, name);
	if (parent == nullptr)
	{
		return;
	}
	const std::int16_t parent_reference = static_cast<std::int16_t>(
		parent - actor.model_references.data());
	std::uint8_t door_count = 0;
	for (std::uint16_t index = 0;
		index < actor.model_references.size();
		++index)
	{
		if (actor.model_references[index].parent_reference
				!= parent_reference
			|| actor.model_references[index].removed)
		{
			continue;
		}
		game::model_animation_start_named(
			actor,
			index,
			"opendoor",
			0.0f,
			opening ? 1 : -1,
			1.0f);
		if (++door_count >= (actor.type == 0x46u ? 2u : 1u))
		{
			break;
		}
	}
}

void transition_queue_actor_sound(
	game::World& world,
	const game::WorldObject& actor,
	std::uint8_t definition,
	std::uint8_t requested_class)
{
	game::world_queue_sound_object(
		world,
		object_handle(world, actor),
		definition,
		requested_class);
}

void transition_log_stage(
	const game::WorldObject& actor,
	const char* family,
	std::uint8_t stage,
	std::uint32_t tick)
{
	diagnostics::mission_log(
		"%s actor=%u stage=%u tick=%u",
		family,
		static_cast<unsigned>(actor.mission_index),
		static_cast<unsigned>(stage),
		tick);
}

void transition_create_protogate_arcs(
	game::World& world,
	game::WorldObject& actor)
{
	game::ObjectModelReference* model =
		game::retained_find_named_model(actor, "Protogate");
	if (model == nullptr || model->point_groups == nullptr)
	{
		return;
	}
	const assets::GameplayPointGroup* points = nullptr;
	for (const assets::GameplayPointGroup& group : *model->point_groups)
	{
		if (group.type == 1)
		{
			points = &group;
			break;
		}
	}
	if (points == nullptr)
	{
		return;
	}
	const std::size_t pair_count = points->points.size() / 2u;
	if (pair_count <= 1)
	{
		return;
	}
	const std::uint16_t actor_index = static_cast<std::uint16_t>(
		&actor - std::begin(world.objects));
	const std::uint16_t model_index = static_cast<std::uint16_t>(
		model - actor.model_references.data());
	// Model_create_point_arcs (0x00471290) deliberately omits the final
	// authored pair and creates 5-second cyan arcs for every earlier pair.
	for (std::size_t pair = 0; pair < pair_count - 1u; ++pair)
	{
		game::ElectricRayEffect* effect =
			game::electric_ray_create(
				world, 1, 500, 0.2f, 260.0f, 0x07u);
		effect->start = points->points[pair * 2u].position;
		effect->end = points->points[pair * 2u + 1u].position;
		game::electric_ray_set_parent(
			*effect,
			actor_index,
			actor.generation,
			model_index);
		game::electric_ray_set_group_color(
			*effect, 0, glm::vec3{0.6f, 1.0f, 1.0f});
	}
}

float transition_phase_delta(std::uint32_t elapsed_ticks)
{
	// Transition controllers such as AI_JumpIn_update (0x00416570) use
	// (current gameplay tick - previous tick) * 0.001 directly. This is an
	// authored phase scale, not a conversion to seconds.
	return static_cast<float>(elapsed_ticks) * 0.001f;
}

void transition_emit_gate_explosion(
	game::World& world,
	game::WorldObject& actor,
	std::uint16_t ordinal,
	float size,
	bool play_sound,
	std::uint32_t tick)
{
	const char* model_name =
		actor.type == 0x6du ? "Protogate" : "OuterRing";
	game::ObjectModelReference* model =
		game::retained_find_named_model(actor, model_name);
	if (model == nullptr || model->point_groups == nullptr)
	{
		return;
	}
	const std::uint16_t model_index = static_cast<std::uint16_t>(
		model - actor.model_references.data());
	const assets::GameplayPoint* selected = nullptr;
	for (const assets::GameplayPointGroup& group
		: *model->point_groups)
	{
		// FixedGateCollapse selects the retained type-2 point group. Its
		// 20-byte point records explain the apparent ordinal*5 arithmetic
		// in LANCER.EXE 0x00421c81-0x00421c8c.
		if (group.type != 2)
		{
			continue;
		}
		if (ordinal < group.points.size())
		{
			selected = &group.points[ordinal];
		}
		// Retail stops at the first type-2 group. Shipped gate assets contain
		// all 55 authored collapse points in that group.
		break;
	}
	if (selected == nullptr)
	{
		return;
	}
	const glm::mat4 transform =
		math::model_transform(
			actor.orientation, 1.0f, actor.position)
		* game::model_animation_render_transform(
			actor, model_index, 1.0f);
	const glm::vec3 position{
		transform * glm::vec4(selected->position, 1.0f)};
	game::transition_explosion_create(
		world,
		position,
		size,
		150,
		tick);
	if (play_sound)
	{
		const glm::vec3 direction = glm::normalize(
			glm::vec3{transform[2]});
		game::world_queue_sound_explicit(
			world,
			position,
			direction,
			glm::vec3{0.0f},
			12,
			0);
		// Definition 12 randomizes its playback rate synchronously in
		// Sound3D_play, consuming the shared retail rand() stream.
		(void)game::world_rand15(world);
	}
}

void transition_set_type3_visibility(
	game::WorldObject& actor,
	bool visible)
{
	for (game::ObjectModelReference& model : actor.model_references)
	{
		if (model.model_type != 3)
		{
			continue;
		}
		if (visible)
		{
			model.runtime_flags &= ~0x20u;
		}
		else
		{
			model.runtime_flags |= 0x20u;
		}
	}
}

void transition_set_jump_high_detail_override(
	game::WorldObject& actor,
	bool enabled)
{
	// Jump_set_model_high_detail, LANCER.EXE 0x00417dc0, recursively
	// toggles mesh-pass flag eight. SR_mesh_select_lod_and_classify_aabb
	// interprets that bit as a highest-detail override while the ship follows
	// its jump curve; it does not suppress the mesh. Retained references are
	// stored in the same complete preorder, so applying it to every entry
	// reproduces the root recursion.
	for (game::ObjectModelReference& model : actor.model_references)
	{
		if (enabled)
		{
			model.runtime_flags |= 0x0008u;
		}
		else
		{
			model.runtime_flags &= ~0x0008u;
		}
	}
}

void transition_set_fixed_quad_half_extent(
	game::TransitionMesh& quad,
	float half_extent)
{
	for (std::size_t vertex = 0;
		vertex < quad.vertices.size()
			&& vertex < quad.base_positions.size();
		++vertex)
	{
		const glm::vec3& base = quad.base_positions[vertex];
		quad.vertices[vertex].position = {
			std::copysign(half_extent, base.x),
			std::copysign(half_extent, base.y),
			base.z,
		};
	}
}

void transition_emit_krasny_split_visuals(
	game::World& world,
	game::WorldObject& actor,
	std::uint32_t tick)
{
	game::ObjectModelReference* model =
		game::retained_find_named_model(actor, "bad front slice");
	if (model == nullptr || model->point_groups == nullptr)
	{
		return;
	}
	const std::uint16_t model_index = static_cast<std::uint16_t>(
		model - actor.model_references.data());
	const glm::mat4 transform =
		math::model_transform(
			actor.orientation, 1.0f, actor.position)
		* game::model_animation_render_transform(
			actor, model_index, 1.0f);
	std::uint32_t ordinal = 0;
	for (const assets::GameplayPointGroup& group
		: *model->point_groups)
	{
		// Krasny_split, LANCER.EXE 0x00422e78, selects the named model's
		// exact type-two point group.
		if (group.type != 2)
		{
			continue;
		}
		for (const assets::GameplayPoint& point : group.points)
		{
			const glm::vec3 position{
				transform * glm::vec4(point.position, 1.0f)};
			// Retail creates three ExplodeDebris meshes at every point and
			// schedules a 1500-unit, 150-tick explosion every 10 ticks.
			game::particle_fragment_directional_burst(
				world,
				position,
				-actor.orientation[2],
				0.3f,
				1.0f,
				0.3f,
				3,
				tick);
			game::transition_explosion_create_delayed(
				world,
				position,
				1500.0f,
				150,
				tick,
				ordinal * 10u);
			++ordinal;
		}
	}
}

bool segment_intersects_aabb(
	const glm::vec3& start,
	const glm::vec3& end,
	const glm::vec3& minimum,
	const glm::vec3& maximum,
	glm::vec3& intersection);

void transition_mark_corridor_dependents(
	game::World& world,
	game::WorldObject& actor,
	const glm::vec3& endpoint)
{
	const glm::vec3 corridor = endpoint - actor.position;
	if (glm::dot(corridor, corridor) <= 0.00000001f)
	{
		return;
	}
	bool corridor_hit = false;
	std::array<bool, game::kMaxGameObjects> propagation_source{};
	for (game::WorldObject& candidate : world.objects)
	{
		if (!candidate.active
			|| (candidate.runtime_flags & game::kObjectSimulationExcludedFlags) != 0)
		{
			continue;
		}
		// JumpOut_initialize excludes every member of the synchronized
		// Command-20 group before testing the 500,000-unit corridor. This
		// includes the departing actor itself.
		if (actor.ai.command_count != 0
			&& actor.ai.commands[0].id == 20
			&& candidate.ai.command_count != 0
			&& candidate.ai.commands[0].id == 20
			&& candidate.ai.commands[0].target
				== actor.ai.commands[0].target)
		{
			continue;
		}
		const glm::mat3 inverse =
			glm::transpose(candidate.orientation);
		const glm::vec3 local_start =
			inverse * (actor.position - candidate.position);
		const glm::vec3 local_finish =
			inverse * (endpoint - candidate.position);
		glm::vec3 intersection;
		if (!segment_intersects_aabb(
				local_start,
				local_finish,
				candidate.bounds_min,
				candidate.bounds_max,
				intersection))
		{
			continue;
		}
		candidate.runtime_flags |= game::kObjectFlagSimulationSuspended;
		propagation_source[
			static_cast<std::size_t>(
				&candidate - std::begin(world.objects))] = true;
		corridor_hit = true;
	}
	if (!corridor_hit)
	{
		return;
	}
	bool changed = true;
	while (changed)
	{
		changed = false;
		for (game::WorldObject& candidate : world.objects)
		{
			if (!candidate.active
				|| (candidate.runtime_flags & game::kObjectSimulationExcludedFlags) != 0
				|| (candidate.runtime_flags & game::kObjectFlagSimulationSuspended) != 0)
			{
				continue;
			}
			bool depends = false;
			if (candidate.type == 0xe1u)
			{
				// JumpOut_mark_corridor_dependents (0x00418470)
				// unconditionally carries every live type-E1 child once
				// recursion has started.
				depends = true;
			}
			if (!depends && candidate.ai.command_count != 0)
			{
				const Command& head = candidate.ai.commands[0];
				depends =
					(head.id == 104 || head.id == 109)
					&& head.target < game::kMaxGameObjects
					&& propagation_source[head.target];
			}
			if (depends)
			{
				candidate.runtime_flags |= game::kObjectFlagSimulationSuspended;
				propagation_source[
					static_cast<std::size_t>(
						&candidate - std::begin(world.objects))] =
							true;
				changed = true;
			}
		}
	}
}

struct TargetSelection
{
	std::uint16_t primary{UINT16_MAX};
	std::int16_t primary_component{-1};
	float primary_score{std::numeric_limits<float>::max()};
	std::uint16_t secondary{UINT16_MAX};
	std::int16_t secondary_component{-1};
	float secondary_score{std::numeric_limits<float>::max()};
};

void consider_find_target_candidate(
	game::WorldObject& actor,
	std::uint16_t candidate_mission_index,
	std::int16_t candidate_component,
	game::World& world,
	mission::Runtime& mission,
	std::uint32_t tick,
	TargetSelection& selection)
{
	// AI_FindNewTarget_candidate_visitor, LANCER.EXE 0x0040ae90.
	// Find New Target deliberately permits flag 0x100 through the common
	// TargetReference validator, then excludes cloaked objects only from
	// the primary Fight/Torpedo slot. They remain eligible for Mill.
	game::WorldObject* target = mission::runtime_resolve_object(
		mission, candidate_mission_index, world);
	if (target == nullptr
		|| target == &actor
		|| !target_reference_valid(
			*target, candidate_component, 0x00000100u))
	{
		return;
	}

	const glm::vec3 separation =
		resolved_target_point(*target, candidate_component)
			- actor.position;
	const float distance_squared = glm::dot(separation, separation);
	std::uint32_t fight_count = 1;
	std::uint32_t mill_count = 1;
	for (const game::WorldObject& object : world.objects)
	{
		// Retail's pressure count walks only the ordinary-object array.
		// The local player is stored separately and is not part of this scan.
		if (!object.active
			|| object.player
			|| object.type >= assets::kShipStatsCount
			|| object.ai.command_count == 0)
		{
			continue;
		}
		const Command& active = object.ai.commands[0];
		if (active.target != candidate_mission_index
			|| active.target_component != candidate_component)
		{
			continue;
		}
		if (active.id == 105)
		{
			++fight_count;
		}
		else if (active.id == 120)
		{
			++mill_count;
		}
	}

	if (candidate_mission_index != actor.find_target_exclusion_index)
	{
		// The retail offline layout places the separately updated local
		// object immediately after the ordinary-object scan. Consequently
		// its post-scan comparison applies this uniform 0.7 factor to every
		// primary score; retaining it also preserves the stored work value.
		const float score =
			distance_squared * static_cast<float>(fight_count) * 0.7f;
		if (score < selection.primary_score
			&& (target->runtime_flags & 0x00000100u) == 0
			&& (actor.collision_class != 1 || fight_count < 3))
		{
			selection.primary = candidate_mission_index;
			selection.primary_component = candidate_component;
			selection.primary_score = score;
		}
	}
	else if (actor.find_target_exclusion_deadline < tick)
	{
		actor.find_target_exclusion_index = UINT16_MAX;
		actor.find_target_exclusion_deadline = 0;
	}

	const float secondary_score =
		distance_squared
			* static_cast<float>(fight_count + mill_count);
	if (secondary_score < selection.secondary_score)
	{
		selection.secondary = candidate_mission_index;
		selection.secondary_component = candidate_component;
		selection.secondary_score = secondary_score;
	}
}

TargetSelection find_new_target_candidates(
	game::WorldObject& actor,
	const Command& command,
	game::World& world,
	mission::Runtime& mission,
	std::uint32_t tick)
{
	TargetSelection selection;
	if (command.target_kind == TargetKind::object)
	{
		consider_find_target_candidate(
			actor,
			command.target,
			command.target_component,
			world,
			mission,
			tick,
			selection);
		return selection;
	}

	mission::ReferenceKind kind;
	if (command.target_kind == TargetKind::group)
	{
		kind = mission::ReferenceKind::group;
	}
	else if (command.target_kind == TargetKind::set)
	{
		kind = mission::ReferenceKind::set;
	}
	else
	{
		return selection;
	}
	mission::ExpandedTargetReference candidates[
		game::kMaxMissionObjects];
	const std::uint16_t count = mission::runtime_expand_target_reference(
		mission,
		kind,
		command.target,
		candidates,
		static_cast<std::uint16_t>(std::size(candidates)));
	for (std::uint16_t ordinal = 0; ordinal < count; ++ordinal)
	{
		consider_find_target_candidate(
			actor,
			candidates[ordinal].object,
			candidates[ordinal].model,
			world,
			mission,
			tick,
			selection);
	}
	return selection;
}

bool command_allows_retaliation(std::int16_t command_id)
{
	return (command_flags(command_id) & kCommandFlagRetaliation) != 0;
}

bool maybe_retaliate_against_attacker(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats)
{
	// AI_maybe_retaliate_against_attacker, LANCER.EXE 0x0040c520.
	if (actor.ai.command_count == 0
		|| !command_allows_retaliation(actor.ai.commands[0].id)
		|| actor.type >= assets::kShipStatsCount
		|| actor.collision_class != 1
		|| actor.attack_pressure
			< static_cast<float>(
				stats.records[actor.type].object.structural_bank_max * 6)
				* 0.7f
		|| (actor.runtime_flags & 0x00080000u) != 0
		|| actor.last_attacker_index >= game::kMaxGameObjects)
	{
		return false;
	}
	game::WorldObject& attacker =
		world.objects[actor.last_attacker_index];
	if (!target_reference_valid(attacker, -1)
		|| attacker.allegiance_class == actor.allegiance_class
		|| attacker.collision_class != 1)
	{
		return false;
	}
	const Command& active = actor.ai.commands[0];
	if (active.target_kind == TargetKind::object)
	{
		const game::WorldObject* current_target =
			mission::runtime_resolve_object(
				mission, active.target, world);
		if (current_target == &attacker)
		{
			return false;
		}
	}
	if (!command_push(world,
			actor,
			105,
			TargetKind::object,
			attacker.mission_index,
			-1))
	{
		return false;
	}
	diagnostics::mission_log(
		"ai retaliation actor=%u attacker=%u pressure=%.1f",
		static_cast<unsigned>(actor.mission_index),
		static_cast<unsigned>(attacker.mission_index),
		actor.attack_pressure);
	return true;
}

glm::vec3 compound_pushout_direction(
	const game::WorldObject& target,
	const glm::vec3& target_point)
{
	// AI_compound_pushout, LANCER.EXE 0x00402500. Retail visits every
	// instantiated model's collision spheres, accumulates only points
	// lying outside a sphere but within 20,000 units of its surface, and
	// normalizes the result once after the complete traversal.
	glm::vec3 accumulated{0.0f};
	for (const game::ObjectModelReference& model
		: target.model_references)
	{
		if (model.collision == nullptr)
		{
			continue;
		}
		const glm::mat3 model_basis =
			target.orientation * glm::mat3(model.local_transform);
		const glm::vec3 model_origin =
			target.position
			+ target.orientation
				* glm::vec3(model.local_transform[3]);
		for (const assets::GameplayCollisionNode& sphere
			: model.collision->nodes)
		{
			const glm::vec3 center =
				model_origin + model_basis * sphere.center;
			const float radius = glm::length(sphere.half_extents);
			const glm::vec3 separation = center - target_point;
			const float center_distance = glm::length(separation);
			const float surface_distance = center_distance - radius;
			if (surface_distance <= 0.0f
				|| surface_distance >= 20000.0f
				|| center_distance <= 0.0f)
			{
				continue;
			}
			accumulated +=
				separation / center_distance
				* (surface_distance - 20000.0f);
		}
	}
	const float length = glm::length(accumulated);
	return length > 0.0f
		? accumulated / length
		: glm::vec3{0.0f, 0.0f, 0x1p-120f};
}

bool segment_intersects_aabb(
	const glm::vec3& start,
	const glm::vec3& end,
	const glm::vec3& minimum,
	const glm::vec3& maximum,
	glm::vec3& intersection)
{
	// segment_intersect_aabb, LANCER.EXE 0x0049b6a0. The strict
	// interior case and endpoint rejection are both observable retail
	// behavior.
	if (glm::all(glm::greaterThan(start, minimum))
		&& glm::all(glm::lessThan(start, maximum)))
	{
		intersection = start;
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
		float boundary;
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
		const std::uint32_t a = (axis + 1) % 3;
		const std::uint32_t b = (axis + 2) % 3;
		if (point[a] >= minimum[a] && point[a] <= maximum[a]
			&& point[b] >= minimum[b] && point[b] <= maximum[b])
		{
			earliest = fraction;
			intersection = point;
			crossed = true;
		}
	}
	return crossed && earliest < 1.0f;
}

bool apply_dynamic_avoidance(
	const game::WorldObject& actor,
	const game::World& world,
	glm::vec3& target)
{
	// AI_dynamic_avoidance, LANCER.EXE 0x004028f0.
	if ((actor.runtime_flags & game::kObjectFlagAvoidanceDisabled) != 0
		|| actor.dynamic_neighbor_count == 0)
	{
		return false;
	}
	glm::vec3 direction = target - actor.position;
	const float direction_length = glm::length(direction);
	if (direction_length == 0.0f)
	{
		// Vec3_normalize, LANCER.EXE 0x004c1370, publishes this exact
		// denormal-sized Z value for a zero vector.
		direction = {0.0f, 0.0f, 0x1p-120f};
	}
	else
	{
		direction /= direction_length;
	}
	bool replaced = false;
	for (std::uint8_t ordinal = 0;
		ordinal < actor.dynamic_neighbor_count;
		++ordinal)
	{
		const std::uint16_t index = actor.dynamic_neighbors[ordinal];
		if (index >= game::kMaxGameObjects)
		{
			continue;
		}
		const game::WorldObject& neighbor = world.objects[index];
		if (!neighbor.active
			|| (neighbor.runtime_flags & game::kObjectSpatialQueryExcludedFlags) != 0)
		{
			continue;
		}
		const glm::vec3 separation =
			neighbor.position - actor.position;
		const float combined_radius = actor.radius + neighbor.radius;
		if (glm::dot(separation, direction) < -combined_radius)
		{
			continue;
		}
		const glm::vec3 relative_motion =
			neighbor.linear_velocity - actor.linear_velocity;
		if (glm::dot(relative_motion, direction) >= 0.0f)
		{
			continue;
		}
		const float relative_speed = glm::length(relative_motion);
		const float approach_time =
			(glm::length(separation) - combined_radius)
			/ relative_speed;
		if (approach_time > 250.0f)
		{
			continue;
		}
		const glm::vec3 predicted_center =
			neighbor.position
			+ neighbor.linear_velocity * approach_time;
		const glm::mat3 inverse = glm::transpose(neighbor.orientation);
		const glm::vec3 local_start =
			inverse * (actor.position - predicted_center)
				* neighbor.effect_scale;
		const glm::vec3 local_end =
			inverse * (target - predicted_center)
				* neighbor.effect_scale;
		const glm::vec3 radius(actor.radius);
		const glm::vec3 test_minimum =
			neighbor.bounds_min - radius;
		const glm::vec3 test_maximum =
			neighbor.bounds_max + radius;
		glm::vec3 hit;
		if (!segment_intersects_aabb(
				local_start,
				local_end,
				test_minimum,
				test_maximum,
				hit))
		{
			continue;
		}

		// Retail expands the already swept test box by one more actor
		// radius, selects the closest face, then the closest one of the
		// four face-edge points to the segment hit.
		const glm::vec3 bypass_minimum = test_minimum - radius;
		const glm::vec3 bypass_maximum = test_maximum + radius;
		glm::vec3 face_point = hit;
		float closest_face = 3.402823466e+38f;
		std::uint32_t face_axis = 0;
		for (std::uint32_t axis = 0; axis < 3; ++axis)
		{
			const float minimum_distance =
				std::abs(hit[axis] - bypass_minimum[axis]);
			const float maximum_distance =
				std::abs(bypass_maximum[axis] - hit[axis]);
			const float distance =
				std::min(minimum_distance, maximum_distance);
			face_point[axis] =
				minimum_distance <= maximum_distance
					? bypass_minimum[axis]
					: bypass_maximum[axis];
			if (distance < closest_face)
			{
				closest_face = distance;
				face_axis = axis;
			}
		}
		const std::uint32_t a = (face_axis + 1) % 3;
		const std::uint32_t b = (face_axis + 2) % 3;
		glm::vec3 candidates[4] = {
			hit, hit, hit, hit,
		};
		for (glm::vec3& candidate : candidates)
		{
			candidate[face_axis] = face_point[face_axis];
		}
		// AI_dynamic_object_avoidance (0x00402bc5) tests these in this
		// exact order. The comparison below is strict, so preserving the
		// order also preserves retail's choice when two edges tie.
		candidates[0][b] = bypass_minimum[b];
		candidates[1][b] = bypass_maximum[b];
		candidates[2][a] = bypass_minimum[a];
		candidates[3][a] = bypass_maximum[a];
		glm::vec3 bypass = candidates[0];
		float closest = glm::distance(candidates[0], local_end);
		for (std::uint32_t candidate = 1;
			candidate < 4;
			++candidate)
		{
			const float distance =
				glm::distance(candidates[candidate], local_end);
			if (distance < closest)
			{
				closest = distance;
				bypass = candidates[candidate];
			}
		}
		target =
			predicted_center
				+ neighbor.orientation
					* (bypass * neighbor.effect_scale);
		replaced = true;
	}
	return replaced;
}

bool apply_proximity_avoidance(
	const game::WorldObject& actor,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	glm::vec3& target)
{
	// AI_proximity_avoidance, LANCER.EXE 0x00402dc0.
	if ((actor.runtime_flags & 0x00100002u) != 0
		|| actor.proximity_neighbor_count == 0)
	{
		return false;
	}
	bool replaced = false;
	for (std::uint8_t ordinal = 0;
		ordinal < actor.proximity_neighbor_count;
		++ordinal)
	{
		const std::uint16_t index = actor.proximity_neighbors[ordinal];
		if (index >= game::kMaxGameObjects)
		{
			continue;
		}
		const game::WorldObject& neighbor = world.objects[index];
		if (!neighbor.active)
		{
			continue;
		}
		const glm::vec3 desired = target - actor.position;
		const float flight_time =
			glm::distance(neighbor.position, actor.position)
			/ std::max(
				game::world_effective_max_speed(
					actor, stats, world.camera_mode),
				0.000001f);
		const glm::vec3 predicted =
			neighbor.position
			+ neighbor.linear_velocity * flight_time;
		const glm::vec3 predicted_delta =
			predicted - actor.position;
		float projection = glm::dot(predicted_delta, desired);
		if (projection > 0.0f)
		{
			const float desired_squared = glm::dot(desired, desired);
			projection =
				desired_squared == 0.0f
					? 1.0f
					: std::min(projection / desired_squared, 1.0f);
		}
		else
		{
			projection = 0.0f;
		}
		const glm::vec3 lateral =
			predicted_delta - desired * projection;
		const bool same_allegiance =
			actor.allegiance_class == neighbor.allegiance_class;
		const float influence =
			same_allegiance ? 1000.0f : 500.0f;
		if (glm::dot(lateral, lateral) >= influence * influence)
		{
			continue;
		}
		glm::vec3 local =
			glm::transpose(actor.orientation)
			* (actor.position - predicted);
		const float clearance =
			actor.radius + neighbor.radius
			+ (same_allegiance ? 2000.0f : 1000.0f);
		const bool negative_requested_world_y = target.y < 0.0f;
		local.x = 0.0f;
		local.y =
			negative_requested_world_y ? -clearance : clearance;
		local.z = glm::distance(actor.position, predicted);
		target = actor.position + actor.orientation * local;
		replaced = true;
	}
	return replaced;
}

game::FlightDemand steer_toward(
	const game::WorldObject& actor,
	const glm::vec3& requested_target,
	float throttle,
	float maximum_control = 1.0f,
	float response_retention = 0.0f,
	std::uint32_t options = 0,
	const game::World* world = nullptr,
	const assets::ShipStatsTable* stats = nullptr,
	std::uint32_t frame_delta = 0,
	bool* avoidance_applied = nullptr)
{
	game::FlightDemand demand;
	demand.throttle = throttle;
	glm::vec3 target = requested_target;
	bool avoided = false;
	if (world != nullptr && stats != nullptr)
	{
		if ((options & 1u) != 0)
		{
			avoided = apply_dynamic_avoidance(actor, *world, target);
		}
		if ((options & 2u) != 0)
		{
			avoided = apply_proximity_avoidance(
				actor, *world, *stats, target) || avoided;
		}
	}
	if (avoided)
	{
		maximum_control = 1.0f;
		response_retention = 0.0f;
		options &= ~8u;
	}
	if (avoidance_applied != nullptr)
	{
		*avoidance_applied = avoided;
	}
	const glm::vec3 world_delta = target - actor.position;
	const float length_squared = glm::dot(world_delta, world_delta);
	if (length_squared <= 0.0001f)
	{
		return demand;
	}
	glm::vec3 local =
		glm::transpose(actor.orientation)
		* (world_delta / std::sqrt(length_squared));
	float wanted_pitch = 0.0f;
	float wanted_yaw = 0.0f;
	float wanted_roll = 0.0f;
	const bool simplified =
		stats != nullptr
		&& actor.type < assets::kShipStatsCount
		&& stats->records[actor.type].flight.simplified_steering;
	// AI_steer_normal (LANCER.EXE 0x00401710) negates the normalized
	// object-space target vector only for the standard reverse callback.
	// The simplified steering path at 0x00401690 deliberately does not.
	if (!simplified
		&& actor.flight_callback_mode
			== game::FlightCallbackMode::standard_reverse)
	{
		local *= -1.0f;
	}
	if (simplified)
	{
		if (local.z >= 0.0f)
		{
			wanted_pitch = -std::atan2(local.y, local.z);
			wanted_yaw = std::atan2(local.x, local.z);
		}
		else
		{
			wanted_yaw = local.x < 0.0f ? -1.0f : 1.0f;
		}
	}
	else
	{
		const glm::vec3 normalized = glm::normalize(local);
		if (std::abs(normalized.z) >= 0.95f)
		{
			wanted_yaw = std::atan2(normalized.x, normalized.z);
		}
		else if (normalized.y < 0.0f || (options & 8u) != 0)
		{
			wanted_roll =
				-std::atan2(-normalized.x, -normalized.y);
		}
		else
		{
			wanted_roll =
				-std::atan2(normalized.x, std::abs(normalized.z));
		}
		if (std::abs(wanted_roll) < 0.8f)
		{
			wanted_pitch =
				-std::atan2(normalized.y, normalized.z);
		}
	}
	if (frame_delta > 10
		&& (std::abs(wanted_pitch) < 0.3926990926f
			|| std::abs(wanted_yaw) < 0.3926990926f
			|| std::abs(wanted_roll) < 0.3926990926f))
	{
		wanted_pitch =
			std::abs(wanted_pitch) < 0.3926990926f
				? wanted_pitch * 0.5f : wanted_pitch;
		wanted_yaw =
			std::abs(wanted_yaw) < 0.3926990926f
				? wanted_yaw * 0.5f : wanted_yaw;
		wanted_roll =
			std::abs(wanted_roll) < 0.3926990926f
				? wanted_roll * 0.5f : wanted_roll;
		maximum_control *= 0.5f;
	}
	constexpr float kControlScale = 11.459155f;
	demand.pitch = std::clamp(
		(wanted_pitch
			- (1.0f - response_retention) * 6.0f * actor.angular_x)
			* kControlScale,
		-maximum_control,
		maximum_control);
	demand.yaw = std::clamp(
		(wanted_yaw
			- (1.0f - response_retention) * 6.0f * actor.angular_y)
			* kControlScale,
		-maximum_control,
		maximum_control);
	demand.roll = std::clamp(
		(wanted_roll
			- (1.0f - response_retention) * 6.0f * actor.angular_z)
			* kControlScale,
		-maximum_control,
		maximum_control);
	if (!avoided && (options & 4u) != 0)
	{
		const float distance = std::sqrt(length_squared);
		if (distance * 0.95f
			< glm::dot(world_delta, actor.orientation[2]))
		{
			const glm::vec3 local_up =
				glm::transpose(actor.orientation)
				* glm::vec3{0.0f, 1.0f, 0.0f};
			const float level_roll =
				-std::atan2(local_up.x, local_up.y);
			demand.roll = std::clamp(
				(level_roll - actor.angular_z * 6.0f)
					* kControlScale,
				-1.0f,
				1.0f);
		}
	}
	if ((options & 8u) != 0 && demand.pitch < 0.2f)
	{
		demand.pitch = 0.2f;
	}
	return demand;
}

bool approach_point(
	const game::WorldObject& actor,
	const glm::vec3& target,
	const glm::mat3& target_basis,
	float minimum_throttle,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_delta,
	game::FlightDemand& demand)
{
	// AI_approach_point, LANCER.EXE 0x00402160..0x004024db.
	const glm::vec3 delta = target - actor.position;
	const float distance = glm::length(delta);
	if (distance < 2000.0f)
	{
		demand = {};
		demand.throttle = minimum_throttle;
		return true;
	}

	const glm::vec3 local_delta = glm::transpose(target_basis) * delta;
	const glm::vec3 local_actor_forward =
		glm::transpose(target_basis) * actor.orientation[2];
	const assets::FlightStats& flight =
		stats.records[actor.type].flight;
	const float acceleration_distance =
		(game::world_effective_max_speed(
			actor, stats, world.camera_mode) * 4.0f)
		/ (1.0f - flight.linear_retention);
	const auto bounded_throttle = [&] (float remaining)
	{
		return std::max(
			minimum_throttle,
			remaining / acceleration_distance - 0.1f);
	};

	if (distance * 0.95f < -local_delta.z
		&& local_actor_forward.z > 0.98f)
	{
		demand = steer_toward(
			actor,
			target,
			1.0f,
			1.0f,
			0.0f,
			3,
			&world,
			&stats,
			frame_delta);
		float roll_angle = std::atan2(
			local_actor_forward.x,
			local_actor_forward.y);
		if (roll_angle < -glm::pi<float>())
		{
			roll_angle += glm::two_pi<float>();
		}
		if (roll_angle > glm::pi<float>())
		{
			roll_angle -= glm::two_pi<float>();
		}
		demand.roll =
			(roll_angle - actor.angular_z * 12.0f)
			* 1.4323943853378296f;
		demand.throttle = bounded_throttle(distance);
		return false;
	}

	glm::vec3 plane{local_delta.x, local_delta.y, 0.0f};
	const float plane_length = glm::length(plane);
	if (plane_length == 0.0f)
	{
		// Vec3_normalize at 0x004c1370 publishes this exact tiny Z value
		// for a zero vector instead of leaving all components zero.
		plane = {0.0f, 0.0f, 7.523164e-37f};
	}
	else
	{
		plane /= plane_length;
	}
	const glm::vec3 local_forward{0.0f, 0.0f, 1.0f};
	const float lateral = glm::dot(local_delta, plane);
	const float forward = glm::dot(local_delta, local_forward);
	const float tangent_radius = std::abs(
		(lateral * lateral + forward * forward)
		/ (lateral + lateral));
	float tangent_angle =
		std::atan2(forward, tangent_radius - lateral);
	float arc_angle =
		tangent_angle <= -0.5f || tangent_angle >= 0.0f
			? tangent_angle + 0.5f
			: 0.0f;
	float turn_radius = flight.speed_pitch_ratio * 2.0f;
	if (local_delta.z <= 0.0f)
	{
		turn_radius = std::max(tangent_radius, turn_radius);
	}
	else
	{
		arc_angle = glm::pi<float>();
	}
	const glm::vec3 local_steering_point =
		plane * ((1.0f - std::cos(arc_angle)) * turn_radius)
		+ local_forward * (std::sin(arc_angle) * turn_radius);
	demand = steer_toward(
		actor,
		target + target_basis * local_steering_point,
		1.0f,
		1.0f,
		0.0f,
		3,
		&world,
		&stats,
		frame_delta);
	if (tangent_angle < 0.0f)
	{
		tangent_angle += glm::two_pi<float>();
	}
	const float remaining =
		(glm::two_pi<float>() - tangent_angle) * turn_radius;
	demand.throttle = bounded_throttle(remaining);
	return false;
}

const assets::PilotRuntimeStats* pilot_for(
	const game::WorldObject& actor,
	const assets::PilotStatsTable& pilots)
{
	if (!pilots.ready || actor.pilot >= assets::kPilotStatsCount)
	{
		return nullptr;
	}
	return &pilots.records[actor.pilot];
}

std::uint32_t random_duration(
	game::WorldObject& actor,
	std::uint16_t lower,
	std::uint16_t upper)
{
	if (upper <= lower)
	{
		return lower;
	}
	return static_cast<std::uint32_t>(lower)
		+ game::world_object_rand15(actor)
			% static_cast<std::uint32_t>(upper - lower);
}

float random_range(
	game::WorldObject& actor,
	float lower,
	float upper)
{
	return lower
		+ (upper - lower) * game::world_object_rand_unit(actor);
}

struct ManeuverBounds
{
	std::uint16_t lower;
	std::uint16_t upper;
};

struct FightManeuverSelection
{
	std::uint16_t duration{};
	std::uint16_t ship{UINT16_MAX};
	std::uint8_t maneuver{};
};

// AIDefend's descriptor table at 0x004e1070. The exact source corpus is
// retained by the executable and extracted by tools/ai_defend.py.
constexpr ManeuverBounds kManeuverBounds[10] = {
	{400, 1000},
	{400, 1000},
	{400, 1000},
	{1000, 1500},
	{200, 300},
	{200, 300},
	{20000, 25000},
	{10000, 15000},
	{200, 1000},
	{10000, 15000},
};

const char* maneuver_name(std::uint8_t maneuver)
{
	constexpr const char* names[10] = {
		"defend dodge1",
		"defend dodge2",
		"defend dodge3",
		"out of action sphere",
		"defend runaway",
		"attack pursue",
		"attack massive object",
		"attack medium fighter",
		"loop the loop",
		"run to ship",
	};
	return maneuver < std::size(names) ? names[maneuver] : "invalid";
}

bool fight_try_choose_maneuver3(
	game::WorldObject& actor,
	const game::WorldObject& target,
	const game::World& world)
{
	// AI_Fight_try_choose_maneuver3 (0x0040a230) uses the default action
	// focus object-table index zero and 220,000-unit radius installed by
	// AI initialization at 0x0040c9b0.
	const game::WorldObject* focus_object =
		game::world_resolve(world, world.action_center);
	if (focus_object == nullptr)
	{
		return false;
	}
	const game::WorldObject& focus = *focus_object;
	const float focus_radius_squared =
		world.action_center_radius * world.action_center_radius;
	// The first gate compares the target object-table index with the live
	// player count. Offline that count is one, so a Fight aimed at the
	// local player can never select maneuver three.
	if (target.player
		|| glm::dot(
				actor.position - focus.position,
				actor.position - focus.position)
			< focus_radius_squared)
	{
		return false;
	}
	const float target_distance_squared = glm::dot(
		actor.position - target.position,
		actor.position - target.position);
	if (target_distance_squared > 40000000000.0f
		&& glm::dot(
				target.position - focus.position,
				target.position - focus.position)
			< focus_radius_squared)
	{
		return false;
	}
	// AI_Fight_try_choose_maneuver3 scans every retail player slot, not
	// only the local player. This is one object offline and all active
	// player ships in a network mission.
	for (const game::WorldObject& player : world.objects)
	{
		if (!player.active
			|| !player.player
			|| (player.runtime_flags & 0x10000840u) != 0)
		{
			continue;
		}
		const glm::vec3 separation =
			actor.position - player.position;
		if (glm::dot(separation, separation) < 10000000000.0f)
		{
			return false;
		}
	}
	return true;
}

std::uint8_t fight_choose_geometry_maneuver(
	game::WorldObject& actor,
	const game::WorldObject& target,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	const assets::PilotRuntimeStats& pilot,
	std::uint16_t& maneuver_ship)
{
	const glm::vec3 separation = target.position - actor.position;
	const float distance = glm::length(separation);
	if (distance <= 0.0001f)
	{
		return 4;
	}
	float target_speed_fraction = 0.25f;
	if (target.type < assets::kShipStatsCount)
	{
		const float maximum = stats.records[target.type].flight.max_speed;
		if (maximum != 0.0f)
		{
			target_speed_fraction =
				std::max(
					target.speed / maximum,
					0.25f);
		}
	}
	constexpr float distance_scales[3] = {
		300000.0f, 200000.0f, 100000.0f};
	const std::uint32_t category = static_cast<std::uint32_t>(
		std::clamp<std::int16_t>(pilot.behavior_22, 0, 2));
	if (distance > target_speed_fraction * distance_scales[category])
	{
		return 5;
	}

	const glm::vec3 direction = separation / distance;
	const float actor_facing = glm::dot(actor.orientation[2], direction);
	const std::uint8_t actor_bin =
		actor_facing > 0.5f ? 0
		: actor_facing > -0.5f ? 1
		: 2;
	const float target_facing = -glm::dot(target.orientation[2], direction);
	const std::uint8_t target_bin =
		target_facing > 0.5f ? 0
		: target_facing > -0.1f ? 1
		: 2;
	if (actor_bin == 2
		&& game::world_object_rand15(actor) % 10 == 0)
	{
		const game::WorldObject* nearest = nullptr;
		float nearest_distance = 90000000000.0f;
		bool blocked = false;
		for (const game::WorldObject& candidate : world.objects)
		{
			if (!candidate.active
				|| (&candidate == &actor)
				|| (candidate.runtime_flags & game::kObjectSpatialQueryExcludedFlags) != 0
				|| (candidate.runtime_flags & game::kObjectFlagCompound) == 0
				|| candidate.allegiance_class != actor.allegiance_class
				|| (candidate.collision_class != 2
					&& candidate.collision_class != 3))
			{
				continue;
			}
			const glm::vec3 candidate_delta =
				candidate.position - actor.position;
			const float candidate_distance =
				glm::dot(candidate_delta, candidate_delta);
			const float exclusion = candidate.radius + 50000.0f;
			if (candidate_distance < exclusion * exclusion)
			{
				// AI_Fight_find_nearby_large_friendly returns false
				// immediately for this too-close case. Its caller then
				// continues the ordinary geometry selection.
				blocked = true;
				break;
			}
			if (candidate_distance < nearest_distance)
			{
				nearest = &candidate;
				nearest_distance = candidate_distance;
			}
		}
		if (!blocked && nearest != nullptr)
		{
			maneuver_ship = static_cast<std::uint16_t>(
				nearest - std::begin(world.objects));
			return 9;
		}
	}
	if (distance < 10000.0f)
	{
		return 4;
	}
	constexpr std::uint8_t choices[9][5] = {
		{4, 0, 1, 2, 8},
		{5, 5, 5, 5, 5},
		{5, 5, 5, 5, 5},
		{4, 0, 1, 2, 8},
		{4, 0, 1, 2, 8},
		{5, 5, 5, 5, 5},
		{4, 0, 1, 2, 8},
		{4, 0, 1, 2, 8},
		{5, 5, 5, 5, 5},
	};
	const std::uint8_t counts[9] = {5, 1, 1, 5, 5, 1, 5, 5, 1};
	const std::uint8_t bin =
		static_cast<std::uint8_t>(actor_bin * 3 + target_bin);
	return choices[bin][
		game::world_object_rand15(actor) % counts[bin]];
}

FightManeuverSelection fight_choose_maneuver(
	game::WorldObject& actor,
	const game::WorldObject& target,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	const assets::PilotRuntimeStats& pilot,
	std::uint16_t retained_ship)
{
	FightManeuverSelection selection;
	selection.ship = retained_ship;
	std::uint32_t duration = 0;
	if ((target.runtime_flags & game::kObjectFlagCompound) != 0)
	{
		selection.maneuver = 6;
		duration = 20000;
	}
	else if ((actor.runtime_flags & game::kObjectFlagCompound) != 0)
	{
		selection.maneuver = 7;
		duration = 10000;
	}
	else if (fight_try_choose_maneuver3(actor, target, world))
	{
		selection.maneuver = 3;
		duration = 500;
	}
	else
	{
		selection.maneuver =
			fight_choose_geometry_maneuver(
				actor,
				target,
				world,
				stats,
				pilot,
				selection.ship);
	}
	if (duration == 0)
	{
		const ManeuverBounds bounds =
			kManeuverBounds[selection.maneuver];
		duration = random_duration(actor, bounds.lower, bounds.upper);
	}
	selection.duration = static_cast<std::uint16_t>(duration);
	return selection;
}

FightManeuverSelection fight_command_maneuver(
	const Command& command)
{
	FightManeuverSelection selection;
	selection.duration =
		static_cast<std::uint16_t>(command.state[0] & 0xffffu);
	selection.ship =
		static_cast<std::uint16_t>(command.state[0] >> 16u);
	selection.maneuver =
		static_cast<std::uint8_t>(command.state[1] & 0xffu);
	return selection;
}

bool fight_command_maneuver_pending(const Command& command)
{
	return (command.state[1] & 0x0000ff00u) != 0;
}

void fight_set_command_maneuver(
	Command& command,
	const FightManeuverSelection& selection)
{
	// Fight's packed state begins at retail command +0x0a: duration and
	// optional RunToShip slot occupy state[0], while the maneuver and
	// pending bytes occupy the low half of state[1]. The upper half and
	// state[2..3] are unrelated retained payload and must survive.
	command.state[0] =
		static_cast<std::uint32_t>(selection.duration)
		| static_cast<std::uint32_t>(selection.ship) << 16u;
	command.state[1] =
		(command.state[1] & 0xffff0000u)
		| static_cast<std::uint32_t>(selection.maneuver)
		| 0x00000100u;
}

void fight_stage_maneuver(
	game::WorldObject& actor,
	const FightManeuverSelection& selection)
{
	Work& work = actor.ai.work;
	// AI_Fight_select_next_maneuver writes the selected descriptor, duration,
	// and optional RunToShip object into the packed command first. Fight's
	// update installs that pending record after selection returns. Keeping the
	// two phases distinct preserves the retail RNG order during Fight begin:
	// selection/duration, missile deadline, then interpreter mirror bits.
	work.pending_maneuver_index = selection.maneuver;
	work.pending_maneuver_duration = selection.duration;
	work.pending_maneuver_ship = selection.ship;
	work.maneuver_refresh_pending = true;
}

void fight_select_maneuver(
	game::WorldObject& actor,
	const game::WorldObject& target,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	const assets::PilotRuntimeStats& pilot)
{
	fight_stage_maneuver(
		actor,
		fight_choose_maneuver(
			actor,
			target,
			world,
			stats,
			pilot,
			UINT16_MAX));
}

void fight_install_pending_maneuver(
	game::WorldObject& actor,
	const game::WorldObject& target,
	std::uint32_t tick)
{
	Work& work = actor.ai.work;
	if (!work.maneuver_refresh_pending)
	{
		return;
	}
	const std::uint8_t maneuver = work.pending_maneuver_index;
	const std::uint32_t duration = work.pending_maneuver_duration;
	const std::uint16_t maneuver_ship = work.pending_maneuver_ship;
	const bool maneuver_changed =
		!work.maneuver_initialized
		|| work.maneuver_index != maneuver;
	// AI_Fight_update (0x0040a65b..0x0040a6b6) clears the complete retail
	// 0x90-byte work record, then restores only descriptor, line, random
	// mirror bits, expiration, and the optional RunToShip object.
	work.fight_aim_point = {};
	work.fight_target_motion = {};
	work.maneuver_vector = {};
	work.fight_demand = {};
	work.target_refresh_deadline = 0;
	work.weapon_deadline = 0;
	work.cloak_deadline = 0;
	work.instruction_deadline = 0;
	work.weapon_solution = false;
	work.attack_permission = false;
	work.requested_cloak = false;
	work.maneuver_index = maneuver;
	work.maneuver_initialized = true;
	work.maneuver_expiration = tick + duration;
	work.maneuver_ship = maneuver_ship;
	work.script_line = UINT8_MAX;
	work.instruction_active = false;
	work.afterburner_latch = false;
	work.requested_cloak = false;
	work.runaway_aligned = false;
	work.crash_avoidance_active = false;
	work.mirror_mask =
		game::world_object_rand15(actor)
			& (maneuver <= 2 ? 5u : 0u);
	work.pending_maneuver_duration = 0;
	work.pending_maneuver_ship = UINT16_MAX;
	work.pending_maneuver_index = 0;
	work.maneuver_refresh_pending = false;
	work.maneuver_preselection_latched = false;
	if (maneuver_changed)
	{
		diagnostics::mission_log(
			"ai fight state actor=%u target=%u maneuver=%u(%s) "
			"duration=%u",
			static_cast<unsigned>(actor.mission_index),
			static_cast<unsigned>(target.mission_index),
			static_cast<unsigned>(maneuver),
			maneuver_name(maneuver),
			duration);
	}
}

void fight_begin(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	const assets::PilotStatsTable& pilots,
	std::uint32_t tick)
{
	game::WorldObject* target =
		resolve_target(actor, command, world, mission);
	const assets::PilotRuntimeStats* pilot = pilot_for(actor, pilots);
	if (target == nullptr || pilot == nullptr)
	{
		diagnostics::mission_log(
			"ai fight blocked actor=%u reason=%s pilot=%u",
			static_cast<unsigned>(actor.mission_index),
			target == nullptr ? "target" : "pilot",
			static_cast<unsigned>(actor.pilot));
		return;
	}
	if (mission.network.role == mission::NetworkRole::offline)
	{
		fight_select_maneuver(
			actor, *target, world, stats, *pilot);
	}
	else
	{
		// AI_Fight_begin (0x0040a517..0x0040a530) does not choose a
		// maneuver independently on each peer. It publishes a pending
		// descriptor-zero placeholder with a 200-tick duration; Fight
		// update then installs it through the same packed-state path used
		// by subsequent authoritative selections.
		FightManeuverSelection initial =
			fight_command_maneuver(command);
		initial.duration = 200;
		initial.maneuver = 0;
		fight_set_command_maneuver(command, initial);
	}
	actor.missile_action_deadline =
		tick + random_duration(
			actor,
			static_cast<std::uint16_t>(
				std::max<std::int16_t>(0, pilot->timing_1_min)),
			static_cast<std::uint16_t>(
				std::max<std::int16_t>(0, pilot->timing_1_max)));
	actor.ai.work.target = target->mission_index;
	actor.selected_target_index = static_cast<std::uint16_t>(
		target - std::begin(world.objects));
	++target->attacker_count;
	actor.attack_pressure = 0.0f;
	diagnostics::mission_log(
		"ai fight engaged actor=%u target=%u attackers=%u",
		static_cast<unsigned>(actor.mission_index),
		static_cast<unsigned>(target->mission_index),
		static_cast<unsigned>(target->attacker_count));
}

enum class DefendOp : std::uint8_t
{
	set_yaw,
	set_pitch,
	set_roll,
	set_speed,
	wait,
	go_to,
	label,
	set_afterburner,
	runaway,
	attack,
	attack_massive,
	out_of_action_sphere,
	set_mirror,
	if_going_to_crash,
	end_if,
	avoid,
	cloak,
	attack_medium,
	new_attack_run,
	run_to_ship,
	end_script,
};

struct DefendInstruction
{
	DefendOp op;
	float lower{};
	float upper{};
	std::uint8_t destination{};
};

constexpr DefendInstruction kDodge1[] = {
	{DefendOp::cloak, 1},
	{DefendOp::set_afterburner, 0},
	{DefendOp::label},
	{DefendOp::set_mirror},
	{DefendOp::set_afterburner, 1},
	{DefendOp::runaway, 50, 100},
	{DefendOp::set_pitch, 1, 1},
	{DefendOp::set_roll, 1, 1},
	{DefendOp::set_yaw, 0, 0},
	{DefendOp::set_speed, 1, 1},
	{DefendOp::set_afterburner, 1},
	{DefendOp::wait, 40, 80},
	{DefendOp::set_roll, -1, -1},
	{DefendOp::set_pitch, 0, 0},
	{DefendOp::set_afterburner, 1},
	{DefendOp::wait, 100, 200},
	{DefendOp::go_to, 0, 0, 2},
};
constexpr DefendInstruction kDodge2[] = {
	{DefendOp::cloak, 1},
	{DefendOp::set_afterburner, 0},
	{DefendOp::set_yaw, 0, 0},
	{DefendOp::set_speed, 1, 1},
	{DefendOp::label},
	{DefendOp::set_mirror},
	{DefendOp::set_pitch, 0.5f, 1},
	{DefendOp::set_roll, 0, 0},
	{DefendOp::set_afterburner, 1},
	{DefendOp::wait, 200, 400},
	{DefendOp::set_pitch, 0, 0},
	{DefendOp::set_roll, 1, 1},
	{DefendOp::set_afterburner, 1},
	{DefendOp::wait, 50, 100},
	{DefendOp::go_to, 0, 0, 4},
};
constexpr DefendInstruction kDodge3[] = {
	{DefendOp::cloak, 1},
	{DefendOp::set_afterburner, 0},
	{DefendOp::set_yaw, 0, 0},
	{DefendOp::set_speed, 1, 1},
	{DefendOp::label},
	{DefendOp::set_mirror},
	{DefendOp::set_roll, 1, 1},
	{DefendOp::set_pitch, 0.5f, 1},
	{DefendOp::set_afterburner, 1},
	{DefendOp::wait, 200, 400},
	{DefendOp::go_to, 0, 0, 4},
};
constexpr DefendInstruction kOutOfSphere[] = {
	{DefendOp::set_afterburner, 0},
	{DefendOp::label},
	{DefendOp::out_of_action_sphere, 1000, 1000},
	{DefendOp::go_to, 0, 0, 1},
};
constexpr DefendInstruction kRunaway[] = {
	{DefendOp::set_afterburner, 0},
	{DefendOp::cloak, 1},
	{DefendOp::label},
	{DefendOp::set_afterburner, 1},
	{DefendOp::runaway, 100, 100},
	{DefendOp::go_to, 0, 0, 2},
};
constexpr DefendInstruction kAttack[] = {
	{DefendOp::cloak, 0},
	{DefendOp::set_afterburner, 0},
	{DefendOp::label},
	{DefendOp::if_going_to_crash, 0, 0, 5},
	{DefendOp::avoid, 300, 300},
	{DefendOp::end_if},
	{DefendOp::attack, 25, 25},
	{DefendOp::go_to, 0, 0, 2},
};
constexpr DefendInstruction kAttackMassive[] = {
	{DefendOp::cloak, 1},
	{DefendOp::set_afterburner, 0},
	{DefendOp::new_attack_run, 1},
	{DefendOp::label},
	{DefendOp::cloak, 0},
	{DefendOp::attack_massive, 0, 0},
	{DefendOp::cloak, 1},
	{DefendOp::new_attack_run, 0},
	{DefendOp::go_to, 0, 0, 3},
};
constexpr DefendInstruction kAttackMedium[] = {
	{DefendOp::set_afterburner, 0},
	{DefendOp::label},
	{DefendOp::attack_medium, 100, 100},
	{DefendOp::go_to, 0, 0, 1},
};
constexpr DefendInstruction kLoop[] = {
	{DefendOp::cloak, 1},
	{DefendOp::set_afterburner, 0},
	{DefendOp::set_pitch, 1, 1},
	{DefendOp::set_yaw, 0, 0},
	{DefendOp::set_roll, 0, 0},
	{DefendOp::set_speed, 0.5f, 0.5f},
	{DefendOp::label},
	{DefendOp::wait, 1000, 1000},
	{DefendOp::go_to, 0, 0, 6},
};
constexpr DefendInstruction kRunToShip[] = {
	{DefendOp::set_afterburner, 0},
	{DefendOp::run_to_ship},
	{DefendOp::end_script},
};

struct DefendProgram
{
	const DefendInstruction* instructions;
	std::uint8_t count;
};

constexpr DefendProgram kDefendPrograms[10] = {
	{kDodge1, static_cast<std::uint8_t>(std::size(kDodge1))},
	{kDodge2, static_cast<std::uint8_t>(std::size(kDodge2))},
	{kDodge3, static_cast<std::uint8_t>(std::size(kDodge3))},
	{kOutOfSphere, static_cast<std::uint8_t>(std::size(kOutOfSphere))},
	{kRunaway, static_cast<std::uint8_t>(std::size(kRunaway))},
	{kAttack, static_cast<std::uint8_t>(std::size(kAttack))},
	{kAttackMassive, static_cast<std::uint8_t>(std::size(kAttackMassive))},
	{kAttackMedium, static_cast<std::uint8_t>(std::size(kAttackMedium))},
	{kLoop, static_cast<std::uint8_t>(std::size(kLoop))},
	{kRunToShip, static_cast<std::uint8_t>(std::size(kRunToShip))},
};

void copy_fight_demand(
	const ControlDemand& source,
	game::FlightDemand& destination)
{
	destination = {};
	destination.throttle = source.throttle;
	destination.roll = source.roll;
	destination.pitch = source.pitch;
	destination.yaw = source.yaw;
	destination.afterburner = source.afterburner;
}

void store_fight_demand(
	const game::FlightDemand& source,
	ControlDemand& destination)
{
	const bool retained_afterburner = destination.afterburner;
	destination.throttle = source.throttle;
	destination.roll = source.roll;
	destination.pitch = source.pitch;
	destination.yaw = source.yaw;
	// The retail steering helpers write only the four scalar controls.
	// AIDefend's persistent SetAfterburner latch is published before the
	// active instruction runs, so steering must not clear it.
	destination.afterburner =
		retained_afterburner || source.afterburner;
}

bool predict_object_collision(
	const game::WorldObject& actor,
	const game::WorldObject& target,
	const assets::ShipStatsTable& stats,
	std::uint8_t camera_mode,
	float padding,
	float horizon,
	std::uint32_t* hit_model = nullptr,
	std::uint32_t* hit_volume = nullptr)
{
	// AI_predict_object_collision, LANCER.EXE 0x00401980.
	const glm::vec3 separation = actor.position - target.position;
	const float reach =
		actor.radius + target.radius + padding
		+ horizon
			* (game::world_effective_max_speed(
					actor, stats, camera_mode)
				+ game::world_effective_max_speed(
					target, stats, camera_mode));
	if (glm::dot(separation, separation) > reach * reach)
	{
		return false;
	}
	if ((target.runtime_flags & game::kObjectFlagCompound) != 0)
	{
		// Compound targets replace the ordinary closest-approach narrow
		// phase with the retail runtime-model and collision-volume walk.
		// The per-model reach deliberately includes only actor speed and
		// caller padding. The volume test also deliberately adds squared
		// terms instead of squaring the sum.
		const float movement_reach =
			game::world_effective_max_speed(
				actor, stats, camera_mode) * horizon
			+ padding;
		const float movement_reach_squared =
			movement_reach * movement_reach;
		for (std::uint32_t model_index = 0;
			model_index < target.model_references.size();
			++model_index)
		{
			const game::ObjectModelReference& model =
				target.model_references[model_index];
			if (model.removed
				|| (model.runtime_flags & 0x0020u) != 0
				|| model.collision == nullptr
				|| model.collision->nodes.empty())
			{
				continue;
			}
			const glm::mat3 model_basis =
				target.orientation * glm::mat3(model.local_transform);
			const glm::vec3 model_origin =
				target.position
				+ target.orientation
					* glm::vec3(model.local_transform[3]);
			const float model_reach =
				movement_reach + model.radius;
			const glm::vec3 model_separation =
				model_origin - actor.position;
			if (glm::dot(model_separation, model_separation)
				> model_reach * model_reach)
			{
				continue;
			}
			for (std::uint32_t volume_index = 0;
				volume_index < model.collision->nodes.size();
				++volume_index)
			{
				const assets::GameplayCollisionNode& volume =
					model.collision->nodes[volume_index];
				const glm::vec3 center =
					model_origin + model_basis * volume.center;
				const glm::vec3 volume_separation =
					center - actor.position;
				if (glm::dot(volume_separation, volume_separation)
					< glm::dot(
						volume.half_extents,
						volume.half_extents)
						+ movement_reach_squared)
				{
					if (hit_model != nullptr)
					{
						*hit_model = model_index;
					}
					if (hit_volume != nullptr)
					{
						*hit_volume = volume_index;
					}
					return true;
				}
			}
		}
		return false;
	}
	// Ordinary targets deliberately differ from a generic symmetric
	// swept-sphere test: the candidate must lie ahead, must be closing
	// under the recovered two-tick relative motion, and only then reaches
	// the capped closest-approach test.
	if (glm::dot(actor.orientation[2], separation) > 0.0f)
	{
		return false;
	}
	const glm::vec3 relative_motion =
		actor.linear_velocity - target.linear_velocity * 2.0f;
	const float closing = glm::dot(separation, relative_motion);
	if (closing > 0.0f)
	{
		return false;
	}
	const float motion_squared =
		glm::dot(relative_motion, relative_motion);
	if (motion_squared < 0.0010000000474974513f)
	{
		return false;
	}
	const float time = std::min(-closing / motion_squared, horizon);
	const glm::vec3 closest =
		separation + relative_motion * time;
	const float radius = actor.radius + target.radius + padding;
	return glm::dot(closest, closest) < radius * radius;
}

bool command_uses_nearby_lists(std::int16_t command_id)
{
	return (command_flags(command_id) & kCommandFlagBuildNearbyLists) != 0;
}

void rebuild_nearby_lists(
	game::World& world,
	game::WorldObject& actor,
	const assets::ShipStatsTable& stats)
{
	// AI_rebuild_nearby_object_lists, LANCER.EXE 0x00492190.
	if ((actor.runtime_flags & game::kObjectFlagAvoidanceDisabled) != 0
		|| actor.ai.command_count == 0
		|| !command_uses_nearby_lists(actor.ai.commands[0].id))
	{
		return;
	}
	actor.dynamic_neighbor_count = 0;
	actor.proximity_neighbor_count = 0;
	const std::uint16_t actor_index = static_cast<std::uint16_t>(
		&actor - std::begin(world.objects));
	for (std::uint16_t candidate_index = 0;
		candidate_index < game::kMaxGameObjects;
		++candidate_index)
	{
		const game::WorldObject& candidate =
			world.objects[candidate_index];
		if (!candidate.active
			|| (candidate.runtime_flags & game::kObjectSimulationExcludedFlags) != 0
			|| candidate_index == actor_index
			|| candidate.collision_class == 8
			|| actor.interaction_target_link == candidate_index
			|| actor.selected_target_index == candidate_index
			|| candidate.interaction_target_link == actor_index)
		{
			continue;
		}
		if ((candidate.runtime_flags & game::kObjectFlagCompound) == 0)
		{
			if ((actor.runtime_flags & game::kObjectFlagCompound) == 0
				&& actor.proximity_neighbor_count < 10
				&& predict_object_collision(
					actor,
					candidate,
					stats,
					world.camera_mode,
					2000.0f,
					50.0f))
			{
				actor.proximity_neighbors[
					actor.proximity_neighbor_count++] =
						candidate_index;
			}
			continue;
		}
		const float reach =
			actor.radius + candidate.radius + 10000.0f;
		const glm::vec3 separation =
			candidate.position - actor.position;
		if (actor.dynamic_neighbor_count < 10
			&& glm::dot(separation, separation) < reach * reach)
		{
			actor.dynamic_neighbors[actor.dynamic_neighbor_count++] =
				candidate_index;
		}
	}
}

void defend_steer_runaway(
	game::WorldObject& actor,
	Work& work,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	const assets::PilotRuntimeStats& pilot,
	std::uint32_t frame_delta)
{
	work.fight_demand.throttle = 1.0f;
	if (actor.dynamic_neighbor_count != 0
		|| actor.proximity_neighbor_count != 0)
	{
		game::FlightDemand demand = steer_toward(
			actor,
			work.maneuver_vector,
			1.0f,
			1.0f,
			0.0f,
			3,
			&world,
			&stats,
			frame_delta);
		store_fight_demand(demand, work.fight_demand);
		return;
	}
	const glm::vec3 delta = work.maneuver_vector - actor.position;
	const float distance = glm::length(delta);
	if (distance <= 0.0001f)
	{
		return;
	}
	const float alignment =
		glm::dot(actor.orientation[2], delta / distance);
	if (work.runaway_aligned)
	{
		if (alignment < 0.7f)
		{
			work.runaway_aligned = false;
		}
		work.fight_demand.pitch = pilot.control_gain;
		work.fight_demand.yaw = 0.0f;
		work.fight_demand.roll = 0.0f;
		return;
	}
	game::FlightDemand demand = steer_toward(
		actor,
		work.maneuver_vector,
		1.0f,
		pilot.control_gain,
		pilot.control_bias,
		11,
		&world,
		&stats,
		frame_delta);
	store_fight_demand(demand, work.fight_demand);
	if (alignment > 0.9f)
	{
		work.runaway_aligned = true;
	}
}

void defend_attack(
	game::WorldObject& actor,
	const game::WorldObject& target,
	Work& work,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	const assets::PilotRuntimeStats& pilot,
	std::uint32_t frame_delta)
{
	const float distance = glm::distance(actor.position, target.position);
	game::FlightDemand demand = steer_toward(
		actor,
		work.fight_aim_point,
		work.fight_demand.throttle,
		pilot.control_gain,
		pilot.control_bias,
		3,
		&world,
		&stats,
		frame_delta);
	const glm::vec3 previous_forward = actor.previous_orientation[2];
	const glm::vec3 projected_separation =
		target.position
		- (actor.position + actor.linear_velocity * 20.0f);
	if (glm::dot(previous_forward, projected_separation) < 0.0f)
	{
		if (pilot.behavior_22 == 2)
		{
			demand.afterburner = true;
		}
		else
		{
			demand.throttle = 1.0f;
		}
	}
	else
	{
		if (distance >= actor.radius + target.radius + 12000.0f)
		{
			demand.throttle = 1.0f;
		}
		else
		{
			const float maximum = std::max(
				game::world_effective_max_speed(
					actor, stats, world.camera_mode), 1.0f);
			demand.throttle =
				glm::dot(previous_forward, work.fight_target_motion)
					/ maximum;
		}
	}
	demand.throttle = std::max(demand.throttle, 0.5f);
	store_fight_demand(demand, work.fight_demand);
}

void defend_avoid(
	game::WorldObject& actor,
	const game::WorldObject& target,
	Work& work)
{
	const glm::vec3 local =
		glm::transpose(actor.orientation)
		* (target.position - actor.position);
	work.fight_demand.roll = 0.0f;
	work.fight_demand.pitch = 0.0f;
	work.fight_demand.yaw = 0.0f;
	if (local.z < 0.0f)
	{
		work.fight_demand.throttle = 1.0f;
		return;
	}
	work.fight_demand.pitch = local.y >= 0.0f ? 1.0f : -1.0f;
	work.fight_demand.throttle = 0.5f;
}

bool defend_begin_instruction(
	game::WorldObject& actor,
	const game::WorldObject& target,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	const assets::PilotRuntimeStats& pilot,
	const DefendInstruction& instruction,
	std::uint32_t tick)
{
	Work& work = actor.ai.work;
	switch (instruction.op)
	{
	case DefendOp::set_yaw:
		work.fight_demand.yaw =
			random_range(actor, instruction.lower, instruction.upper)
			* pilot.control_gain
			* ((work.mirror_mask & 1u) != 0 ? -1.0f : 1.0f);
		return false;
	case DefendOp::set_pitch:
		work.fight_demand.pitch =
			random_range(actor, instruction.lower, instruction.upper)
			* pilot.control_gain
			* ((work.mirror_mask & 2u) != 0 ? -1.0f : 1.0f);
		return false;
	case DefendOp::set_roll:
		work.fight_demand.roll =
			random_range(actor, instruction.lower, instruction.upper)
			* pilot.control_gain
			* ((work.mirror_mask & 4u) != 0 ? -1.0f : 1.0f);
		return false;
	case DefendOp::set_speed:
		work.fight_demand.throttle =
			random_range(actor, instruction.lower, instruction.upper);
		return false;
	case DefendOp::wait:
	case DefendOp::attack:
	case DefendOp::attack_massive:
	case DefendOp::avoid:
	case DefendOp::attack_medium:
		work.instruction_deadline = tick
			+ static_cast<std::uint32_t>(
				std::lrint(
					random_range(
						actor,
						instruction.lower,
						instruction.upper)));
		return true;
	case DefendOp::go_to:
		work.script_line = instruction.destination;
		return false;
	case DefendOp::label:
	case DefendOp::end_if:
		return false;
	case DefendOp::set_afterburner:
		work.afterburner_latch = false;
		if (instruction.lower == 0.0f)
		{
			return false;
		}
		if (pilot.control_gain == 2.0f)
		{
			for (const game::WorldObject& player : world.objects)
			{
				if (!player.active
					|| !player.player
					|| (player.runtime_flags & 0x10000840u) != 0)
				{
					continue;
				}
				const glm::vec3 separation =
					player.position - actor.position;
				if (glm::dot(separation, separation)
					< 2500000000.0f)
				{
					work.afterburner_latch = true;
					break;
				}
			}
		}
		return false;
	case DefendOp::runaway:
		work.instruction_deadline = tick
			+ static_cast<std::uint32_t>(
				std::lrint(
					random_range(
						actor,
						instruction.lower,
						instruction.upper)));
	{
		glm::vec3 away = actor.position - target.position;
		if (glm::dot(away, away) > 0.0001f)
		{
			away = glm::normalize(away);
		}
		work.maneuver_vector = actor.position + away * 1000000.0f;
		work.runaway_aligned = false;
	}
		return true;
	case DefendOp::out_of_action_sphere:
		work.instruction_deadline = tick
			+ static_cast<std::uint32_t>(
				std::lrint(
					random_range(
						actor,
						instruction.lower,
						instruction.upper)));
	{
		const game::WorldObject* focus =
			game::world_resolve(world, world.action_center);
		work.maneuver_vector =
			focus != nullptr ? focus->position : actor.position;
		work.runaway_aligned = false;
	}
		return true;
	case DefendOp::set_mirror:
		work.mirror_mask = game::world_object_rand15(actor) & 7u;
		return false;
	case DefendOp::if_going_to_crash:
	{
		bool collision = false;
		if ((target.runtime_flags & game::kObjectFlagCompound) != 0)
		{
			const std::int16_t component =
				actor.ai.commands[0].target_component;
			const float actor_reach =
				game::world_effective_max_speed(
					actor, stats, world.camera_mode) * 50.0f
				+ actor.radius
				+ resolved_target_radius(target, component);
			const glm::vec3 separation =
				resolved_target_point(target, component)
				- actor.position;
			collision =
				glm::dot(separation, separation)
				<= actor_reach * actor_reach;
		}
		else
		{
			float padding = 5000.0f;
			if (pilot.behavior_22 == 1)
			{
				padding = 3500.0f;
			}
			else if (pilot.behavior_22 == 2)
			{
				padding = 2000.0f;
			}
			collision = predict_object_collision(
				actor,
				target,
				stats,
				world.camera_mode,
				padding,
				100.0f);
		}
		if (collision != work.crash_avoidance_active)
		{
			diagnostics::mission_log(
				"ai fight collision actor=%u target=%u state=%s "
				"distance=%.1f radii=%.1f behavior=%d",
				static_cast<unsigned>(actor.mission_index),
				static_cast<unsigned>(target.mission_index),
				collision ? "avoid" : "clear",
				glm::distance(actor.position, target.position),
				actor.radius + target.radius,
				static_cast<int>(pilot.behavior_22));
			work.crash_avoidance_active = collision;
		}
		if (!collision)
		{
			work.script_line = instruction.destination;
		}
		return false;
	}
	case DefendOp::cloak:
		work.requested_cloak = instruction.lower != 0.0f;
		// AI_Defend_Cloak_begin adds the compiled 500 directly to the
		// retail 100 Hz gameplay clock (LANCER.EXE 0x004063e0).
		work.cloak_deadline = tick + 500;
		return false;
	case DefendOp::new_attack_run:
		if (actor.ai.commands[0].target_component >= 0
			&& actor.ai.commands[0].target_component
				< target.attack_run_direction_count)
		{
			work.maneuver_vector =
				target.attack_run_directions[
					actor.ai.commands[0].target_component];
		}
		else
		{
			const glm::vec3 point = resolved_target_point(
				target, actor.ai.commands[0].target_component);
			const glm::vec3 direction =
				compound_pushout_direction(target, point);
			work.maneuver_vector =
				glm::transpose(target.orientation) * direction;
		}
		if (instruction.lower != 0.0f)
		{
			actor.selected_target_index = UINT16_MAX;
		}
		return true;
	case DefendOp::run_to_ship:
	{
		const game::WorldObject* ship =
			work.maneuver_ship < game::kMaxGameObjects
				&& world.objects[work.maneuver_ship].active
			? &world.objects[work.maneuver_ship]
			: nullptr;
		work.maneuver_vector =
			ship != nullptr ? ship->position : actor.position;
		work.runaway_aligned = false;
		return true;
	}
	case DefendOp::end_script:
		return true;
	}
	return false;
}

bool defend_update_instruction(
	game::WorldObject& actor,
	const game::WorldObject& target,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	const assets::PilotRuntimeStats& pilot,
	const DefendInstruction& instruction,
	std::uint32_t frame_delta,
	std::uint32_t tick)
{
	Work& work = actor.ai.work;
	switch (instruction.op)
	{
	case DefendOp::wait:
		return tick < work.instruction_deadline;
	case DefendOp::runaway:
	case DefendOp::out_of_action_sphere:
		if (work.instruction_deadline < tick)
		{
			return false;
		}
		defend_steer_runaway(
			actor, work, world, stats, pilot, frame_delta);
		return true;
	case DefendOp::attack:
		if (work.instruction_deadline < tick)
		{
			return false;
		}
		defend_attack(
			actor,
			target,
			work,
			world,
			stats,
			pilot,
			frame_delta);
		return true;
	case DefendOp::avoid:
		if (work.instruction_deadline < tick)
		{
			return false;
		}
		defend_avoid(actor, target, work);
		return true;
	case DefendOp::attack_massive:
	{
		const std::int16_t component =
			actor.ai.commands[0].target_component;
		const glm::vec3 target_point =
			resolved_target_point(target, component);
		const float envelope =
			game::world_effective_max_speed(
				actor, stats, world.camera_mode) * 50.0f
			+ actor.radius + resolved_target_radius(target, component);
		const glm::vec3 separation =
			actor.position - target_point;
		if (glm::dot(separation, separation)
			< envelope * envelope)
		{
			return false;
		}
		game::FlightDemand demand = steer_toward(
			actor,
			work.fight_aim_point,
			1.0f,
			pilot.control_gain,
			pilot.control_bias,
			3,
			&world,
			&stats,
			frame_delta);
		demand.throttle = 1.0f;
		store_fight_demand(demand, work.fight_demand);
		return true;
	}
	case DefendOp::attack_medium:
		if (work.instruction_deadline < tick)
		{
			return false;
		}
	{
		const glm::vec3 relative =
			target.position - actor.position;
			const float distance = glm::length(relative);
			glm::vec3 point;
			if (distance < 10000.0f
				&& glm::dot(actor.orientation[2], relative)
					< distance * 0.95f)
			{
				point =
					actor.position + actor.orientation[2] * 20000.0f;
		}
		else
		{
			glm::vec3 direction =
				target.linear_velocity
				- actor.linear_velocity * 0.1f;
			direction *= distance / std::max(
				game::world_effective_max_speed(
					actor, stats, world.camera_mode),
				0.000001f);
			direction += relative;
			const float direction_length = glm::length(direction);
			if (direction_length > 0.0f)
			{
				direction /= direction_length;
			}
			point = actor.position + direction * 20000.0f;
		}
		game::FlightDemand demand = steer_toward(
			actor,
			point,
			1.0f,
			pilot.control_gain,
			pilot.control_bias,
			1,
			&world,
			&stats,
			frame_delta);
		demand.throttle = 1.0f;
		store_fight_demand(demand, work.fight_demand);
	}
		return true;
	case DefendOp::new_attack_run:
	{
		glm::mat3 model_orientation{1.0f};
		const std::int16_t component =
			actor.ai.commands[0].target_component;
		if (component >= 0 && component < target.component_count)
		{
			std::int16_t reference = -1;
			for (std::uint32_t index = 0;
				index < target.model_references.size();
				++index)
			{
				if (target.model_references[index].component_index
					== component)
				{
					reference = static_cast<std::int16_t>(index);
					break;
				}
			}
			// AIDefend_NewAttackRun_update (0x00406680) follows the
			// selected runtime node's +0xec parent chain, but retains the
			// last node before the root. Its render basis supplies the
			// approach direction. ObjectModelReference transforms are
			// already cumulative in object space.
			while (reference >= 0
				&& reference
					< static_cast<std::int16_t>(
						target.model_references.size()))
			{
				const game::ObjectModelReference& model =
					target.model_references[reference];
				if (model.parent_reference < 0)
				{
					model_orientation =
						glm::mat3(model.local_transform);
					break;
				}
				const std::int16_t parent = model.parent_reference;
				if (parent < 0
					|| parent
						>= static_cast<std::int16_t>(
							target.model_references.size())
					|| target.model_references[parent].parent_reference < 0)
				{
					model_orientation =
						glm::mat3(model.local_transform);
					break;
				}
				reference = parent;
			}
		}
		const glm::vec3 direction =
			target.orientation
			* model_orientation
			* work.maneuver_vector;
		const bool large_run =
			instruction.lower != 0.0f
			&& target.radius > 50000.0f;
		const glm::vec3 point =
			resolved_target_point(
				target, actor.ai.commands[0].target_component)
			+ direction
				* (large_run ? target.radius * 2.0f : 50000.0f);
		const glm::vec3 delta = point - actor.position;
		if (glm::dot(delta, delta) < 4000000.0f)
		{
			actor.selected_target_index = static_cast<std::uint16_t>(
				&target - std::begin(world.objects));
			return false;
		}
		game::FlightDemand demand = steer_toward(
			actor,
			point,
			1.0f,
			pilot.control_gain,
			pilot.control_bias,
			3,
			&world,
			&stats,
			frame_delta);
		demand.throttle =
			glm::dot(direction, actor.linear_velocity) >= 0.0f
				? 1.0f : 0.5f;
		demand.afterburner = demand.throttle == 1.0f;
		store_fight_demand(demand, work.fight_demand);
		return true;
	}
	case DefendOp::run_to_ship:
	{
		const game::WorldObject* ship =
			work.maneuver_ship < game::kMaxGameObjects
				&& world.objects[work.maneuver_ship].active
			? &world.objects[work.maneuver_ship]
			: nullptr;
		if (ship == nullptr)
		{
			return false;
		}
		work.maneuver_vector = ship->position;
		defend_steer_runaway(
			actor, work, world, stats, pilot, frame_delta);
		const float distance =
			glm::distance(actor.position, ship->position);
		if ((ship->runtime_flags & game::kObjectFlagCompound) != 0)
		{
			return distance >= ship->radius + 5000.0f;
		}
		const float projection =
			glm::dot(
				actor.orientation[2],
				ship->linear_velocity)
			/ std::max(
				game::world_effective_max_speed(
					actor, stats, world.camera_mode),
				0.000001f);
		work.fight_demand.throttle = std::max(
			0.5f,
			projection + (distance - 5000.0f) * 0.0002f);
		return true;
	}
	case DefendOp::end_script:
		work.maneuver_expiration = tick - 1;
		return true;
	default:
		return false;
	}
}

void defend_update(
	game::WorldObject& actor,
	const game::WorldObject& target,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	const assets::PilotRuntimeStats& pilot,
	std::uint32_t frame_delta,
	std::uint32_t tick)
{
	Work& work = actor.ai.work;
	const DefendProgram& program =
		kDefendPrograms[work.maneuver_index];
	work.fight_demand.afterburner = work.afterburner_latch;
	if (work.instruction_active)
	{
		const DefendInstruction& instruction =
			program.instructions[work.script_line];
		work.instruction_active = defend_update_instruction(
			actor,
			target,
			world,
			stats,
			pilot,
			instruction,
			frame_delta,
			tick);
		return;
	}
	for (std::uint32_t collapsed = 0;
		collapsed < 32;
		++collapsed)
	{
		++work.script_line;
		if (work.script_line >= program.count)
		{
			work.script_line = 0;
		}
		const DefendInstruction& instruction =
			program.instructions[work.script_line];
		work.instruction_active = defend_begin_instruction(
			actor, target, world, stats, pilot, instruction, tick);
		if (work.instruction_active)
		{
			return;
		}
	}
}

bool fight_compute_weapon_lead(
	const game::WorldObject& actor,
	const game::WorldObject& target,
	std::int16_t target_component,
	const assets::GunStatsTable& stats,
	glm::vec3& point)
{
	return game::weapons_compute_lead_point(
		actor,
		target,
		target_component,
		stats,
		1.0f,
		point);
}

void fight_refresh_target(
	game::WorldObject& actor,
	const game::WorldObject& target,
	const Command& command,
	const assets::PilotRuntimeStats& pilot,
	const assets::GunStatsTable& gun_stats,
	std::uint32_t tick,
	std::uint32_t frame_delta)
{
	Work& work = actor.ai.work;
	if (work.target_refresh_deadline < tick)
	{
		const std::uint32_t refresh =
			static_cast<std::uint32_t>(
				std::max<std::int16_t>(1, pilot.control_threshold));
		work.target_refresh_deadline = tick + refresh;
		work.weapon_solution = fight_compute_weapon_lead(
			actor,
			target,
			command.target_component,
			gun_stats,
			work.fight_aim_point);
		if (!work.weapon_solution)
		{
			work.fight_aim_point = resolved_target_point(
				target, command.target_component);
		}
		// AI_Fight_refresh_target_and_attack (0x00409be0) seeds one
		// quarter of current target motion, then repeatedly transforms
		// that retained displacement by the target's angular-step matrix.
		work.fight_target_motion = target.linear_velocity * 0.25f;
		const glm::mat3 angular_step =
			glm::transpose(target.previous_orientation)
			* target.orientation;
		for (std::uint32_t step = 0; step < refresh / 2; ++step)
		{
			work.fight_target_motion =
				angular_step * work.fight_target_motion;
		}
	}
	// AI_Fight_refresh_target_and_attack advances this retained displacement
	// by DAT_00588330, the complete rendered-frame gameplay tick delta.
	work.fight_aim_point +=
		work.fight_target_motion * static_cast<float>(frame_delta);
}

bool point_inside_forward_cylinder(
	const game::WorldObject& actor,
	const glm::vec3& point,
	float radius)
{
	const glm::vec3 local =
		glm::transpose(actor.orientation) * (point - actor.position);
	return local.z > 0.0f
		&& local.x * local.x + local.y * local.y < radius * radius;
}

void fight_run_attack_controller(
	game::WorldObject& actor,
	const game::WorldObject& target,
	const Command& command,
	const assets::PilotRuntimeStats& pilot,
	const assets::PilotStatsTable& pilot_stats,
	const assets::MissileStatsTable& missile_stats,
	game::World& world,
	game::MissileRuntime& missiles,
	game::ChaffRuntime& chaff,
	std::uint32_t& random_seed,
	std::uint32_t tick)
{
	Work& work = actor.ai.work;
	if ((actor.runtime_flags & 0x00000100u) != 0)
	{
		return;
	}
	if ((target.runtime_flags & 0x00000100u) != 0)
	{
		const glm::vec3 previous_separation =
			target.previous_position - actor.previous_position;
		const float margin =
			actor.radius + target.radius + 10000.0f;
		if (glm::dot(previous_separation, previous_separation)
			<= margin * margin)
		{
			return;
		}
	}
	if (actor.gun_action_deadline < tick)
	{
		const float cylinder_radius =
			resolved_target_radius(
				target, command.target_component)
			* pilot.aim_error_scalar;
		const glm::vec3 target_point =
			resolved_target_point(
				target, command.target_component);
		const float gun_range =
			40000.0f;
		if (point_inside_forward_cylinder(
				actor, work.fight_aim_point, cylinder_radius)
			&& glm::distance(target_point, actor.position) < gun_range)
		{
			bool player_obstructs = false;
			if (actor.allegiance_class == 0)
			{
				for (const game::WorldObject& player : world.objects)
				{
					if (!player.active
						|| !player.player
						|| (player.runtime_flags & 0x10000840u) != 0)
					{
						continue;
					}
					const glm::vec3 separation =
						player.position - actor.position;
					const float projection =
						glm::dot(
							actor.orientation[2],
							separation);
					if (projection < 0.0f || projection > 50000.0f)
					{
						continue;
					}
					const glm::vec3 lateral =
						separation
							- actor.orientation[2] * projection;
					const float obstruction_radius =
						0.1f * projection
							+ player.radius + 500.0f;
					if (glm::length(lateral) < obstruction_radius)
					{
						player_obstructs = true;
						break;
					}
				}
			}
			if (actor.allegiance_class != 0 || !player_obstructs)
			{
				actor.primary_weapon_requested = true;
				work.primary_weapon_delay =
					static_cast<std::uint32_t>(
						std::max<std::int16_t>(
							0, pilot.timing_0_min));
			}
		}
		actor.gun_action_deadline = tick
			+ static_cast<std::uint32_t>(
				std::max<std::int16_t>(0, pilot.timing_0_max));
	}

	work.attack_permission = false;
	std::uint8_t missile_mount = UINT8_MAX;
	for (std::uint8_t index = 0;
		index < actor.attachment_count;
		++index)
	{
		const game::AttachmentSlot& attachment =
			actor.attachments[index];
		if (attachment.remaining_count <= 0
			|| attachment.kind == 3
			|| attachment.definition_index < 0
			|| static_cast<std::size_t>(attachment.definition_index)
				>= assets::kMissileStatsCount)
		{
			continue;
		}
		missile_mount = index;
		const assets::MissileStats& missile =
			missile_stats.records[attachment.definition_index];
		const glm::vec3 delta =
			resolved_target_point(target, command.target_component)
			- actor.position;
		const float distance_squared = glm::dot(delta, delta);
		bool in_envelope =
			missile_stats.ready
			&& distance_squared <= missile.lock_range * missile.lock_range;
		if (in_envelope && distance_squared > 0.0001f)
		{
			in_envelope =
				glm::dot(
					glm::normalize(delta),
					actor.orientation[2])
				>= 0.7f;
		}
		if (!in_envelope)
		{
			work.weapon_deadline = tick
				+ static_cast<std::uint32_t>(
					std::max(0, missile.lock_ticks));
		}
		work.attack_permission = work.weapon_deadline < tick;
		if (work.attack_permission
			&& actor.missile_action_deadline < tick)
		{
			random_seed = random_seed * 0x343fdu + 0x269ec3u;
			const float launch_trial =
				static_cast<float>((random_seed >> 16) & 0x7fffu)
				* (1.0f / 32768.0f);
			if (launch_trial < 0.2f)
			{
				actor.secondary_weapon_requested = true;
				work.secondary_weapon_mount = missile_mount;
				work.secondary_weapon_target =
					static_cast<std::uint16_t>(
						&target - std::begin(world.objects));
				work.secondary_weapon_component =
					command.target_component;
			}
			actor.missile_action_deadline = tick
				+ random_duration(
					actor,
					static_cast<std::uint16_t>(
						std::max<std::int16_t>(
							0, pilot.timing_1_min)),
					static_cast<std::uint16_t>(
						std::max<std::int16_t>(
							0, pilot.timing_1_max)));
			work.weapon_deadline = tick
				+ static_cast<std::uint32_t>(
					std::max(0, missile.lock_ticks));
			work.attack_permission = false;
		}
		break;
	}
	if (!work.attack_permission)
	{
		actor.missile_action_deadline = tick
			+ random_duration(
				actor,
				static_cast<std::uint16_t>(
					std::max<std::int16_t>(
						0, pilot.timing_1_min)),
				static_cast<std::uint16_t>(
					std::max<std::int16_t>(
						0, pilot.timing_1_max)));
	}

	if (!actor.incoming_missile)
	{
		actor.countermeasure_deadline = tick
			+ static_cast<std::uint32_t>(
				std::max<std::int16_t>(0, pilot.timing_2_min));
	}
	else if (actor.countermeasure_deadline < tick)
	{
		actor.countermeasure_deadline = tick
			+ random_duration(
				actor,
				static_cast<std::uint16_t>(
					std::max<std::int16_t>(
						0, pilot.timing_2_min)),
				static_cast<std::uint16_t>(
					std::max<std::int16_t>(
						0, pilot.timing_2_max)));
		game::chaff_deploy(
			chaff,
			missiles,
			world,
			missile_stats,
			pilot_stats,
			object_handle(world, actor),
			tick,
			random_seed);
	}
}

void fight_reinforce_attacked_player(
	game::WorldObject& actor,
	const game::WorldObject& target,
	game::World& world,
	const assets::ShipStatsTable& stats)
{
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (player == nullptr
		|| &target != player
		|| actor.type >= assets::kShipStatsCount)
	{
		return;
	}
	const float type_strength = static_cast<float>(
		stats.records[actor.type].object.structural_bank_max * 6);
	if (actor.attack_pressure < type_strength * 0.2f
		|| actor.last_attacker_index != world.player.index)
	{
		return;
	}
	bool bank_below_half = false;
	for (const float bank : actor.secondary_shields)
	{
		if (bank < type_strength * 0.5f)
		{
			bank_below_half = true;
			break;
		}
	}
	if (!bank_below_half)
	{
		return;
	}
	actor.attack_pressure = 0.0f;

	const std::uint16_t actor_index = static_cast<std::uint16_t>(
		&actor - std::begin(world.objects));
	std::uint16_t selected = UINT16_MAX;
	float selected_distance = 90000000000.0f;
	bool mill_seen = false;
	for (std::uint16_t index = 0;
		index < game::kMaxGameObjects;
		++index)
	{
		const game::WorldObject& candidate = world.objects[index];
		if (index == actor_index
			|| !candidate.active
			|| (candidate.runtime_flags & 0x00080460u) != 0
			|| candidate.allegiance_class != actor.allegiance_class
			|| candidate.collision_class != 1
			|| candidate.ai.command_count == 0)
		{
			continue;
		}
		const std::int16_t command_id =
			candidate.ai.commands[0].id;
		if (command_id != 105 && command_id != 120)
		{
			continue;
		}
		const glm::vec3 separation =
			candidate.position - actor.position;
		const float distance =
			glm::dot(separation, separation);
		if (command_id == 120 && !mill_seen)
		{
			selected_distance = distance;
			selected = index;
			mill_seen = true;
		}
		else if (distance < selected_distance)
		{
			selected_distance = distance;
			selected = index;
		}
	}
	if (selected == UINT16_MAX)
	{
		return;
	}
	game::WorldObject& ally = world.objects[selected];
	if (command_push(world,
			ally,
			105,
			TargetKind::object,
			target.mission_index,
			-1))
	{
		diagnostics::mission_log(
			"ai fight reinforcement requester=%u ally=%u target=%u",
			static_cast<unsigned>(actor.mission_index),
			static_cast<unsigned>(ally.mission_index),
			static_cast<unsigned>(target.mission_index));
	}
}

bool fight_update(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& mission,
	const assets::ShipStatsTable& stats,
	const assets::PilotStatsTable& pilots,
	const assets::GunStatsTable& gun_stats,
	const assets::MissileStatsTable& missile_stats,
	game::MissileRuntime& missiles,
	game::ChaffRuntime& chaff,
	std::uint32_t& random_seed,
	std::uint32_t tick,
	game::FlightDemand& demand)
{
	game::WorldObject* target =
		resolve_target(actor, command, world, mission);
	const assets::PilotRuntimeStats* pilot = pilot_for(actor, pilots);
	if (target == nullptr
		|| !target_reference_valid(*target, command.target_component)
		|| pilot == nullptr)
	{
		command_pop(world, actor);
		return false;
	}
	Work& work = actor.ai.work;
	if (mission.network.role == mission::NetworkRole::offline)
	{
		if (work.maneuver_expiration < tick)
		{
			fight_select_maneuver(
				actor, *target, world, stats, *pilot);
		}
	}
	else if (work.maneuver_expiration < tick + 100u
		&& !work.maneuver_preselection_latched)
	{
		// AI_Fight_update (0x0040a627..0x0040a654) latches the
		// preselection on every peer, but only the object's owner consumes
		// maneuver RNG and publishes the replacement. Non-owners continue
		// the installed maneuver until the guaranteed deferred command
		// arrives.
		work.maneuver_preselection_latched = true;
		const std::uint16_t actor_index =
			object_handle(world, actor).index;
		if (mission::network_local_owns_object(
			mission.network,
			actor_index,
			mission.player_prefix_count,
			world.player.index))
		{
			Command selected = command;
			const FightManeuverSelection retained =
				fight_command_maneuver(selected);
			fight_set_command_maneuver(
				selected,
				fight_choose_maneuver(
					actor,
					*target,
					world,
					stats,
					*pilot,
					retained.ship));
			const std::uint32_t delay_ticks =
				work.instruction_deadline - tick;
			const std::uint8_t publication_seed =
				static_cast<std::uint8_t>(
					game::world_rand15(world));
			if (command_defer(
				actor,
				selected,
				tick,
				delay_ticks,
				publication_seed))
			{
				mission::network_publish_ai_deferred_command(
					mission.network,
					actor_index,
					selected,
					delay_ticks,
					publication_seed);
			}
		}
	}
	if (fight_command_maneuver_pending(command))
	{
		fight_stage_maneuver(
			actor, fight_command_maneuver(command));
		fight_install_pending_maneuver(actor, *target, tick);
		// Retail clears only command byte +0x0f after consuming the packed
		// descriptor, preserving the other fifteen state bytes verbatim.
		command.state[1] &= ~0x0000ff00u;
	}
	else
	{
		fight_install_pending_maneuver(actor, *target, tick);
	}
	fight_refresh_target(
		actor,
		*target,
		command,
		*pilot,
		gun_stats,
		tick,
		mission.frame_delta_ticks);
	fight_run_attack_controller(
		actor,
		*target,
		command,
		*pilot,
		pilots,
		missile_stats,
		world,
		missiles,
		chaff,
		random_seed,
		tick);
	fight_reinforce_attacked_player(
		actor, *target, world, stats);

	if (actor.cloak_supported)
	{
		if (!work.requested_cloak)
		{
			game::world_set_cloak_active(
				world, actor, false, tick);
		}
		else if (work.cloak_deadline <= tick)
		{
			game::world_set_cloak_active(
				world, actor, true, tick);
		}
	}
	defend_update(
		actor,
		*target,
		world,
		stats,
		*pilot,
		mission.frame_delta_ticks,
		tick);
	copy_fight_demand(work.fight_demand, demand);
	return true;
}

void begin_command(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	const assets::PilotStatsTable& pilots,
	std::uint32_t tick)
{
	Work& work = actor.ai.work;
	work.begin_pending = false;
	work.entered_tick = tick;
	work.stage = 0;
	diagnostics::mission_log(
		"ai begin actor=%u type=%u command=%d(%s) target_kind=%u "
		"target=%u component=%d sequence=%d tick=%u",
		static_cast<unsigned>(actor.mission_index),
		static_cast<unsigned>(actor.type),
		static_cast<int>(command.id),
		command_name(command.id),
		static_cast<unsigned>(command.target_kind),
		static_cast<unsigned>(command.target),
		static_cast<int>(command.target_component),
		static_cast<int>(command.sequence),
		tick);
	if (scripted_command_begin(
		actor,
		command,
		world,
		mission,
		file,
		stats,
		tick))
	{
		return;
	}
	if (command.id == 1)
	{
		// AI_FlyAimlessly_begin, LANCER.EXE 0x0040a8c0.
		work.strategy = static_cast<std::uint8_t>(
			game::world_rand15(world) % 3u + 1u);
		work.deadline = 1;
		work.vector = actor.position;
		work.orientation = actor.orientation;
		if ((game::world_rand15(world) & 1u) != 0)
		{
			work.orientation[0] *= -1.0f;
		}
		work.phase =
			static_cast<float>(game::world_rand15(world))
				* (1.0f / 32768.0f)
				* 0.3f
			+ 0.4f;
	}
	else if (command.id == 9)
	{
		work.target = UINT16_MAX;
		if (command.selector == 0)
		{
			work.target = command.target;
		}
		else
		{
			mission::ExpandedTargetReference targets[
				game::kMaxMissionObjects];
			const mission::ReferenceKind kind =
				command.target_kind == TargetKind::group
					? mission::ReferenceKind::group
					: mission::ReferenceKind::set;
			const std::uint16_t count =
				mission::runtime_expand_target_reference(
					mission,
					kind,
					command.target,
					targets,
					static_cast<std::uint16_t>(
						std::size(targets)));
			const std::int32_t ordinal = command.sequence;
			if (ordinal >= 0
				&& static_cast<std::uint32_t>(ordinal) < count)
			{
				work.target = targets[ordinal].object;
			}
		}
	}
	else if (command.id == 102)
	{
		// AI_AvoidTarget_begin, LANCER.EXE 0x0040b310.
		// reset_work already clears the first work dword; the callback
		// initializes the second dword to fifty simulation ticks ahead.
		work.deadline = tick + 50u;
	}
	else if (command.id == 103)
	{
		// AI_Torpedo_begin, LANCER.EXE 0x00496f70, writes only the
		// retained throttle control. Update subsequently owns rotation
		// without disturbing the fifth control channel.
		actor.control_demand.throttle = 1.0f;
	}
	else if (command.id == 21
		|| command.id == 115
		|| command.id == 116)
	{
		work.stage = 0;
	}
	else if (command.id >= 22 && command.id <= 24)
	{
		const float scale =
			command.id == 22 ? 0.3f
				: command.id == 23 ? 0.5f
				: 0.9f;
		// AI_RandomSpin{Slow,Medium,Fast}_begin
		// (0x0040b6a0/0x0040b730/0x0040b7c0) writes the retained object
		// controls directly. Their definition update is the shared literal
		// RET at 0x004983a0, so these values must not live only in transient
		// work storage.
		actor.control_demand.throttle = 0.0f;
		actor.control_demand.yaw =
			game::world_object_rand_unit(actor) * scale + 0.1f;
		actor.control_demand.roll =
			game::world_object_rand_unit(actor) * scale + 0.1f;
		actor.control_demand.pitch =
			game::world_object_rand_unit(actor) * scale + 0.1f;
	}
	else if (command.id == 27)
	{
		const std::int32_t ordinal = command.sequence + 2;
		const std::int32_t lane =
			((ordinal & 1) * 2 - 1) * (ordinal / 2);
		work.vector = {
			static_cast<float>(lane) * 3000.0f,
			0.0f,
			0.0f,
		};
	}
	else if (command.id == 110)
	{
		// AI_DarkReignShoot_begin, LANCER.EXE 0x0040d020. The original
		// IonCannonEffects allocator has one static slot and initializes
		// the shared search clock to -1 on every shot.
		IonCannonWork& ion = work.ion_cannon;
		ion = {};
		ion.target_object = UINT16_MAX;
		ion.lower_model = UINT16_MAX;
		ion.upper_model = UINT16_MAX;
		ion.emitter_model = UINT16_MAX;
		ion.beam_model = UINT16_MAX;
		ion.plasma_model = UINT16_MAX;
		ion.impact_origin_model = UINT16_MAX;
		ion.last_plasma = -1;
		ion.previous_tick = tick;
		ion.stage_start_tick = 0;
		world.ion_cannon_search_ticks = -1;
		world.ion_cannon_effect_owner =
			object_handle(world, actor).index;

		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		if (target != nullptr)
		{
			ion.target_object = object_handle(world, *target).index;
		}
		const char* lower_name =
			actor.type == 0x44u ? "Dark Low Body"
				: actor.type == 0x48u ? "Bor Ion Cannon"
				: actor.type == 0xa5u ? "cannon"
				: nullptr;
		if (lower_name == nullptr)
		{
			return;
		}
		ion.lower_model = named_model_reference(actor, lower_name);
		ion.upper_model = actor.type == 0x44u
			? named_model_reference(actor, "Dark Focus")
			: ion.lower_model;
		if (ion.lower_model == UINT16_MAX
			|| ion.upper_model == UINT16_MAX)
		{
			return;
		}
		// The numeric children requested by SR_model_get_locator are
		// authored locator types 11 through 15 on these named roots.
		ion.emitter_model = ion.upper_model;
		ion.beam_model = ion.lower_model;
		ion.plasma_model = ion.lower_model;
		ion.impact_origin_model = ion.lower_model;
		ion.plasma_count = actor.type == 0x48u ? 3u : 4u;
		ion.beam_radius = 2000.0f;
		ion.initialized = true;
	}
	else if (command.id == 35)
	{
		// AI_LightsOn_begin, LANCER.EXE 0x0040bbd0. The retail lookup of
		// "hanger01" has no later data dependency, but does perform the
		// complete recursive named-model traversal before work is initialized.
		(void)game::retained_find_named_model(actor, "hanger01");
		work.deadline = tick + 500;
		work.stage = 0;
		work.strategy = 5;
	}
	else if (command.id == 36)
	{
		// AI_BoridinBreakaway_begin, LANCER.EXE 0x0040bf60.
		// These names, including the trailing space, are the exact compiled
		// strings passed to model_hide_named_recursive.
		constexpr const char* hidden_models[] = {
			"Bor break off section",
			"Bor brk away CORE",
			"Bor brk off sec Cylinder",
			"Bor brk proj gen DEST",
			"Bor brk projector gen",
			"Bor brkawy proj ",
			"Bor launch dr 1",
			"Bor launch dr 2",
			"Object02",
		};
		for (const char* name : hidden_models)
		{
			game::retained_set_named_model_hidden(
				actor, name, true);
		}
		const game::ObjectModelReference* main_body =
			game::retained_find_named_model(
				actor, "Bor main body");
		if (main_body != nullptr && main_body->point_groups != nullptr)
		{
			const std::uint16_t main_body_index =
				static_cast<std::uint16_t>(
					main_body - actor.model_references.data());
			const game::ObjectHandle actor_handle =
				object_handle(world, actor);
			bool pool_available = true;
			for (const assets::GameplayPointGroup& group
				: *main_body->point_groups)
			{
				if (group.type != 3)
				{
					continue;
				}
				for (const assets::GameplayPoint& point : group.points)
				{
					pool_available =
						game::particle_emitter_create_model_owned(
							world,
							actor_handle,
							main_body_index,
							point.position,
							point.direction,
							{0.5f, 0.5f, 0.0f},
							30.0f,
							5.0f,
							5000,
							game::ParticleEmitterStyle::
								large_destruction_smoke,
							tick);
					if (!pool_available)
					{
						break;
					}
				}
				if (!pool_available)
				{
					break;
				}
			}
		}

		game::WorldObject* breakaway = nullptr;
		for (game::WorldObject& candidate : world.objects)
		{
			if (candidate.active && candidate.type == 0x00a8u)
			{
				breakaway = &candidate;
				break;
			}
		}
		const game::ObjectModelReference* section =
			game::retained_find_named_model(
				actor, "Bor break off section");
		if (breakaway == nullptr || section == nullptr)
		{
			// Retail asserts at aifuncs.cpp:1044 when the authored A8
			// companion is absent. Contain malformed mission/model data.
			diagnostics::mission_log(
				"ai Boridin breakaway rejected actor=%u "
				"breakaway=%s section=%s",
				static_cast<unsigned>(actor.mission_index),
				breakaway == nullptr ? "missing" : "present",
				section == nullptr ? "missing" : "present");
			return;
		}
		const std::uint16_t section_index =
			static_cast<std::uint16_t>(
				section - actor.model_references.data());
		// The retained component transform is object-local. Retail reads
		// the instantiated node's world position/basis, so compose the
		// actor root exactly as the mission renderer does before placing
		// the independent type-A8 breakaway object.
		const glm::mat4 section_world =
			math::model_transform(
				actor.orientation, 1.0f, actor.position)
			* game::model_animation_render_transform(
				actor, section_index, 1.0f);
		const glm::mat3 section_orientation{section_world};
		const glm::vec3 section_position{section_world[3]};
		// Vector_get_forward at 0x004c18a0 extracts the third basis axis
		// before the compiled 4000-unit scale and position addition.
		const glm::vec3 breakaway_position =
			section_position + section_orientation[2] * 4000.0f;
		breakaway->previous_position = breakaway_position;
		breakaway->position = breakaway_position;
		breakaway->previous_orientation = section_orientation;
		breakaway->orientation = section_orientation;
	}
	else if (command.id == 37)
	{
		const game::ObjectModelReference* projector =
			game::retained_find_named_model(
				actor, "Bor brkawy proj ");
		if (projector != nullptr)
		{
			const std::uint16_t model =
				static_cast<std::uint16_t>(
					projector - actor.model_references.data());
			game::model_animation_start_named(
				actor,
				model,
				"Rotate Proj ",
				projector->sequence_time,
				2,
				4.0f);
		}
		// AI_BoridinRotateProjector_begin tail-calls AI_command_pop.
		command_pop(world, actor);
	}
	else if (command.id == 11)
	{
		ExplodeWork& explode = work.explode;
		// `destruction_notified` is reimplementation bookkeeping rather
		// than a field in retail's shared 0x90-byte work allocation.
		// AI_schedule_death_command deliberately preserves that allocation,
		// so initialize this guard at the actual Explode begin boundary.
		explode.destruction_notified = false;
		explode.mode =
			actor.type >= 0x79u && actor.type <= 0x7fu ? 3
				: (actor.runtime_flags & game::kObjectFlagCompound) != 0
					? command.target_component < 0 ? 1 : 2
					: 0;
		if (actor.type == 0x49u)
		{
			explode.mode = 0;
		}
		else if (actor.type == 0x1du)
		{
			explode.mode = 4;
		}
		if (explode.mode == 3)
		{
			// AI_ExplodeAsteroid_begin, LANCER.EXE 0x00409270. This mode
			// bypasses every ordinary explosion variant and becomes ready
			// on the current gameplay tick, while its update uses a
			// strict comparison and therefore runs on the next service.
			work.deadline = tick;
			actor.runtime_flags |= 0x00000018u;
			diagnostics::mission_log(
				"death explode begin actor=%u mode=3 tick=%u",
				static_cast<unsigned>(actor.mission_index),
				tick);
			return;
		}
		if (explode.mode == 1)
		{
			// AI_ExplodeCompound_begin, LANCER.EXE 0x00409170. Retail
			// marks only visible-authored type-one runtime models dead; the
			// normal render-time destruction traversal performs the actual
			// group removal and effects after this callback.
			for (game::ObjectModelReference& model
				: actor.model_references)
			{
				if ((model.source_flags & 0x0004u) != 0
					|| model.model_type != 1)
				{
					continue;
				}
				model.health = -1.0f;
				actor.component_destruction_pending = true;
			}
			diagnostics::mission_log(
				"death explode begin actor=%u mode=1 tick=%u",
				static_cast<unsigned>(actor.mission_index),
				tick);
			return;
		}
		if (explode.mode == 2)
		{
			// AI_ExplodeCompoundComponent_begin,
			// LANCER.EXE 0x00409200. The selected fixed component points
			// at one retained runtime model. Hidden authored alternates are
			// ignored; otherwise render-time destruction owns the group.
			const std::int16_t component = command.target_component;
			if (component >= 0 && component < actor.component_count)
			{
				const std::int16_t model_index =
					actor.components[component].model_reference;
				if (model_index >= 0
					&& static_cast<std::size_t>(model_index)
						< actor.model_references.size())
				{
					game::ObjectModelReference& model =
						actor.model_references[model_index];
					if ((model.runtime_flags & 0x00000020u) == 0)
					{
						model.health = -1.0f;
						actor.component_destruction_pending = true;
					}
				}
			}
			diagnostics::mission_log(
				"death explode begin actor=%u mode=2 component=%d tick=%u",
				static_cast<unsigned>(actor.mission_index),
				static_cast<int>(component),
				tick);
			return;
		}
		if (explode.mode == 4)
		{
			// AI_ExplodeType1D_begin, LANCER.EXE 0x004094d0.
			if (actor.mission_index < mission.object_count)
			{
				mission::events_emit_destroyed(
					mission, actor.mission_index, UINT16_MAX);
			}
			command.sequence = 50;
			actor.linear_velocity = {};
			actor.speed = 0.0f;
			actor.throttle = 0.0f;
			work.deadline = 0;
			actor.runtime_flags |= 0x00000008u;
			const auto centered_process_random = [&world]()
			{
				return static_cast<float>(game::world_rand15(world))
						* 0x1.0002p-15f
					- 0.5f;
			};
			const float roll = centered_process_random() * 0.3f;
			const float pitch = centered_process_random() * 0.05f;
			const float yaw = centered_process_random() * 0.05f;
			explode.emission_direction = {yaw, pitch, roll};
			actor.control_demand = {};
			actor.control_demand.roll = roll;
			actor.control_demand.pitch = yaw;
			actor.control_demand.yaw = pitch;
			(void)game::explosion_billboard_create(
				world.death_effects,
				world,
				actor.position,
				glm::vec3{0.0f},
				game::ExplosionBillboardType::separate_frames,
				actor.radius,
				150,
				false,
				0,
				false,
				false,
				tick);
			diagnostics::mission_log(
				"death explode begin actor=%u mode=4 tick=%u",
				static_cast<unsigned>(actor.mission_index),
				tick);
			return;
		}
		const bool multiplayer =
			mission.network.role != mission::NetworkRole::offline;
		const bool multiplayer_proximity_mine =
			multiplayer && actor.type == 0x6fu;
		// AI_ExplodeOrdinary_begin, LANCER.EXE
		// 0x004086f0..0x004088ce. Multiplayer forces the ordinary
		// explosion variant to zero for every non-mine object; only the
		// proximity mine selects variant two. The per-object random draw
		// belongs exclusively to offline play.
		explode.variant =
			multiplayer
				? static_cast<std::uint8_t>(
					actor.type == 0x6fu ? 2u : 0u)
				: actor.type == 0x4au || actor.type == 0x5cu
					|| (world.cinematic_mode != 0
						&& is_local_actor(world, actor))
					? 2
					: static_cast<std::uint8_t>(
						game::world_object_rand15(actor) % 3u);
		const std::uint16_t deathmatch_victim =
			static_cast<std::uint16_t>(
				&actor - std::begin(world.objects));
		mission::deathmatch_scenarios_player_death(
			mission,
			world,
			stats,
			deathmatch_victim,
			tick);
		death_notify_deathmatch_result(
			actor,
			world,
			mission);
		if (multiplayer_proximity_mine)
		{
			// The proximity-mine path at 0x00408877..0x00408884 writes
			// variant two and immediately converts the object to retail's
			// departed placeholder before score/chatter, Destroyed, and
			// the directly invoked variant begin callback continue.
			game::world_mark_departed(
				world, object_handle(world, actor));
			// Draining the command queue resets the shared work block.
			// The retail caller retained its direct variant selection, so
			// restore that selector for the remaining begin callback.
			explode.variant = 2;
		}
		death_award_ordinary_player_kill(
			actor,
			world,
			mission,
			stats,
			tick);
		if (explode.variant == 0)
		{
			// AI_ExplodeOrdinaryVariant0_begin,
			// LANCER.EXE 0x00408bc0.
			const glm::vec3 camera_delta =
				actor.position - death_particle_camera_position(mission);
			if (glm::dot(camera_delta, camera_delta) < 400000000.0f)
			{
				game::world_queue_sound_explicit(
					world,
					actor.position,
					actor.orientation[2],
					actor.linear_velocity,
					11,
					4);
			}
			death_notify_object(actor, world, mission, stats, tick);
			command.sequence = 50;
			if (actor.collision_class == 5
				|| (command.state[0] & 0xffu) == 0)
			{
				actor.linear_velocity = {};
				actor.speed = 0.0f;
				actor.throttle = 0.0f;
				work.deadline = 0;
			}
			else
			{
				// AI_ExplodeOrdinaryVariant0_begin multiplies by the
				// compiled -200.0f at 0x004dc4c4, truncates, then
				// subtracts that negative value at 0x00408c1c.
				const std::uint32_t duration_ticks =
					200u
						+ static_cast<std::uint32_t>(
							static_cast<float>(
								game::world_rand15(world))
							* 0x1.0002p-15f
							* 200.0f);
				work.deadline = tick + duration_ticks;
			}
			actor.runtime_flags |= 0x00000008u;
			const auto centered_process_random = [&world]()
			{
				return static_cast<float>(game::world_rand15(world))
						* 0x1.0002p-15f
					- 0.5f;
			};
			const float roll = centered_process_random() * 0.3f;
			const float pitch = centered_process_random() * 0.05f;
			const float yaw = centered_process_random() * 0.05f;
			explode.emission_direction = {yaw, pitch, roll};
			(void)game::explosion_billboard_create(
				world.death_effects,
				world,
				actor.position,
				glm::vec3{0.0f},
				game::ExplosionBillboardType::separate_frames,
				actor.radius,
				150,
				false,
				0,
				false,
				false,
				tick);
			diagnostics::mission_log(
				"death explode begin actor=%u mode=0 variant=0 "
				"deadline=%u tick=%u",
				static_cast<unsigned>(actor.mission_index),
				work.deadline,
				tick);
			return;
		}
		if (explode.variant == 1)
		{
			// AI_ExplodeOrdinaryVariant1_begin,
			// LANCER.EXE 0x004090f0.
			const glm::vec3 camera_delta =
				actor.position - death_particle_camera_position(mission);
			if (glm::dot(camera_delta, camera_delta) < 400000000.0f)
			{
				game::world_queue_sound_explicit(
					world,
					actor.position,
					actor.orientation[2],
					actor.linear_velocity,
					11,
					4);
			}
			death_notify_object(actor, world, mission, stats, tick);
			command.sequence = 50;
			work.deadline = 0;
			actor.runtime_flags |= 0x00000008u;
			actor.control_demand.roll = 0.0f;
			actor.control_demand.pitch = 0.0f;
			actor.control_demand.yaw = 0.0f;
			actor.angular_x = 0.0f;
			actor.angular_y = 0.0f;
			actor.angular_z = 0.0f;
			diagnostics::mission_log(
				"death explode begin actor=%u mode=0 variant=1 tick=%u",
				static_cast<unsigned>(actor.mission_index),
				tick);
			return;
		}
		if (explode.variant == 2)
		{
			// AI_ExplodeOrdinaryVariant2_begin,
			// LANCER.EXE 0x00408d20.
			const glm::vec3 camera_delta =
				actor.position - death_particle_camera_position(mission);
			if (glm::dot(camera_delta, camera_delta) < 400000000.0f)
			{
				game::world_queue_sound_explicit(
					world,
					actor.position,
					actor.orientation[2],
					actor.linear_velocity,
					11,
					4);
			}
			death_notify_object(actor, world, mission, stats, tick);
			command.sequence = 50;
			actor.linear_velocity = {};
			actor.speed = 0.0f;
			actor.throttle = 0.0f;
			work.deadline = 0;
			actor.runtime_flags |= 0x00000008u;
			const auto centered_process_random = [&world]()
			{
				return static_cast<float>(game::world_rand15(world))
						* 0x1.0002p-15f
					- 0.5f;
			};
			const float roll = centered_process_random() * 0.3f;
			const float pitch = centered_process_random() * 0.05f;
			const float yaw = centered_process_random() * 0.05f;
			explode.emission_direction = {yaw, pitch, roll};
			(void)game::explosion_billboard_create(
				world.death_effects,
				world,
				actor.position,
				glm::vec3{0.0f},
				game::ExplosionBillboardType::separate_frames,
				actor.radius,
				150,
				false,
				0,
				false,
				false,
				tick);
			if (actor.type == 0x4au || actor.type == 0x5cu)
			{
				for (std::uint8_t ordinal = 0;
					ordinal < 150;
					ordinal = static_cast<std::uint8_t>(
						ordinal + 30))
				{
					const float offset_z =
						centered_process_random() * 1500.0f;
					const float offset_y =
						centered_process_random() * 1500.0f;
					const float offset_x =
						centered_process_random() * 1500.0f;
					const glm::vec3 position =
						actor.position
							+ actor.orientation
								* glm::vec3{
									offset_x,
									offset_y,
									offset_z};
					const std::uint32_t delay =
						static_cast<std::uint32_t>(ordinal)
							+ static_cast<std::uint32_t>(
								static_cast<float>(
									game::world_rand15(world))
								* 0x1.0002p-15f
									* 20.0f);
					const float radius =
						static_cast<float>(
							game::world_rand15(world))
							* 0x1.0002p-15f
							* 500.0f
						+ 1000.0f;
					(void)game::explosion_billboard_create(
						world.death_effects,
						world,
						position,
						glm::vec3{0.0f},
						game::ExplosionBillboardType::separate_frames,
						radius,
						150,
						true,
						delay,
						false,
						false,
						tick);
				}
				game::shockwave_create(
					world,
					actor.position,
					actor.orientation,
					actor.linear_velocity,
					8,
					6000.0f,
					100,
					static_cast<std::int8_t>(
						actor.allegiance_class),
					static_cast<std::int32_t>(
						&actor - std::begin(world.objects)),
					tick);
			}
			diagnostics::mission_log(
				"death explode begin actor=%u mode=0 variant=2 tick=%u",
				static_cast<unsigned>(actor.mission_index),
				tick);
			return;
		}
	}
	else if (command.id == 12)
	{
		// AI_RipperGrab_begin, LANCER.EXE 0x0040fd10.
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		if (target == nullptr)
		{
			command_pop(world, actor);
			return;
		}
		RipperGrabWork& grab = work.ripper_grab;
		grab.stage_start_tick = tick;
		grab.stage = 0;
		grab.target_object = static_cast<std::uint16_t>(
			target - std::begin(world.objects));
		grab.target_generation = target->generation;
		grab.saved_target_capture_state =
			target->protection_state;
		target->protection_state = 2;
		grab.saved_flight_callback = static_cast<std::uint8_t>(
			actor.flight_callback_mode);
		actor.flight_callback_mode =
			game::FlightCallbackMode::linear_with_exhaust;
		actor.interaction_target_link = grab.target_object;
		target->interaction_target_link = static_cast<std::uint16_t>(
			&actor - std::begin(world.objects));
		actor.runtime_flags |= game::kObjectFlagKinematic;
		grab.close_approach =
			command.target_component < 0
			&& mission.mission_number == 26;
		grab.approach_position = ripper_grab_point(
			*target,
			command.target_component,
			mission.mission_number);
		grab.effect_slot = game::ripper_grab_effect_allocate(
			world.death_effects,
			object_handle(world, actor),
			object_handle(world, *target));
		diagnostics::mission_log(
			"ripper grab begin actor=%u target=%u component=%d tick=%u",
			static_cast<unsigned>(actor.mission_index),
			static_cast<unsigned>(target->mission_index),
			static_cast<int>(command.target_component),
			tick);
	}
	else if (command.id == 30)
	{
		if (mission.network.role != mission::NetworkRole::offline)
		{
			return;
		}
		if (!eject_split_cockpit(
				actor, world, mission, stats, tick))
		{
			return;
		}
		if (actor.mission_index < mission.object_count)
		{
			mission::events_emit_destroyed(
				mission,
				actor.mission_index,
				last_attacker_mission_index(world, actor));
		}
		const std::uint16_t actor_index =
			static_cast<std::uint16_t>(
				&actor - std::begin(world.objects));
		if (actor_index >= mission.player_prefix_count
			&& actor.sound3d_slot == 0)
		{
			mission::player_comms_on_pilot_ejected(
				mission,
				world,
				actor_index,
				tick);
		}
	}
	else if (command.id == 106)
	{
		work.deadline = tick + 200;
	}
	else if (command.id == 107)
	{
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		if (target == nullptr)
		{
			command_pop(world, actor);
			return;
		}
		ScoopUpWork& scoop = work.scoop_up;
		scoop.target_object = static_cast<std::uint16_t>(
			target - std::begin(world.objects));
		scoop.target_generation = target->generation;
		scoop.previous_tick = tick;
		scoop.state_start_tick = 0;
		scoop.state = 0;
		scoop.effect_slot = game::tractor_effect_allocate(
			world.death_effects,
			object_handle(world, actor),
			object_handle(world, *target));
		actor.interaction_target_link = scoop.target_object;
		target->interaction_target_link = static_cast<std::uint16_t>(
			&actor - std::begin(world.objects));
		actor.state_publication_flags |= game::kObjectStateScoopActive;
	}
	else if (command.id == 108)
	{
		work.deadline = tick + 200;
		actor.runtime_flags |= 0x00000808u;
		const auto tumble_angle = [&world]()
		{
			return (
				static_cast<float>(game::world_rand15(world))
					* 0x1.0002p-15f
				- 0.5f) * 0.2f;
		};
		const float tumble_z = tumble_angle();
		const float tumble_y = tumble_angle();
		const float tumble_x = tumble_angle();
		actor.inertial_angular_step =
			math::rotation_from_euler({
				tumble_x, tumble_y, tumble_z});
		// AI_EjectSpin_begin publishes whole-object Destroyed after
		// installing the tumble (LANCER.EXE 0x00416174). The later Eject
		// begin attempts the same publication, which the mission record's
		// one-shot bit suppresses.
		(void)mission::events_emit_destroyed(
			mission,
			actor.mission_index,
			last_attacker_mission_index(world, actor));
	}
	else if (command.id == 118)
	{
		actor.runtime_flags |= 0x00000808u;
		// Command 118 writes coordinator state eight at 0x00416310. The
		// outer gameplay teardown later normalizes an actual player death
		// to state one; an ejection may replace it with states 1, 2, or 3.
		mission.gameplay_state = 8;
		work.deadline = tick + 400u
			+ static_cast<std::uint32_t>(
				game::world_rand15(world) % 200u);
		const std::uint8_t line = static_cast<std::uint8_t>(
			game::world_rand15(world) % 8u + 1u);
		char speech[16];
		std::snprintf(
			speech, sizeof(speech), "ejt_%03u.ut",
			static_cast<unsigned>(line));
		death_queue_speech(mission, speech);
		diagnostics::mission_log(
			"player death transition actor=%u deadline=%u tick=%u",
			static_cast<unsigned>(actor.mission_index),
			work.deadline,
			tick);
	}
	else if (command.id == 121)
	{
		// AI_DeathmatchRespawnEffect_begin, LANCER.EXE 0x004b0db0.
		// Active work +0 is current simulation tick + 300.
		work.deadline = tick + 300u;
		work.respawn.deadline = work.deadline;
		work.respawn.effect_slot = game::respawn_effect_allocate(
			world.death_effects,
			object_handle(world, actor),
			actor);
		// Deathmatch_respawn_sound_for_local_ship, 0x0049dcb0,
		// selects the materialization cue from the local ship type even
		// when a remote actor owns this command.
		std::uint16_t local_type = 0xffffu;
		if (const game::WorldObject* local =
			game::world_resolve(world, world.player))
		{
			local_type = local->type;
			if (local_type > 0xf3u)
			{
				local_type =
					static_cast<std::uint16_t>(local_type - 0xf4u);
			}
		}
		const std::uint16_t respawn_sound =
			local_type == 0x2du
				? 0x25u
				: local_type < 0x0du
					? static_cast<std::uint16_t>(
						local_type + 0x1fu)
					: 0x1fu;
		game::world_queue_sound_object(
			world,
			object_handle(world, actor),
			respawn_sound,
			static_cast<std::uint8_t>(actor.player ? 5 : 0));
		actor.protection_state = 4;
		actor.targetable = mission.network.respawn_targetable;
		if (mission.network.respawn_targetable)
		{
			actor.runtime_flags |= 0x00000200u;
		}
		else
		{
			actor.runtime_flags &= ~0x00000200u;
		}
		actor.runtime_flags |= 0x00000004u;
		if (actor.player)
		{
			death_request_camera(mission, world, actor, 40);
		}
		game::world_queue_sound_object(
			world,
			object_handle(world, actor),
			0x43,
			static_cast<std::uint8_t>(actor.player ? 2 : 0));
		diagnostics::mission_log(
			"deathmatch respawn effect actor=%u deadline=%u slot=%d",
			static_cast<unsigned>(actor.mission_index),
			work.deadline,
			static_cast<int>(work.respawn.effect_slot));
	}
	else if (command.id == 102)
	{
		work.stage = 0;
		work.deadline = tick + 50;
	}
	else if (command.id == 13)
	{
		// AI_ObjectAttach_begin, LANCER.EXE 0x0040b4a0. Work +0 retains
		// the actor-to-target displacement in the target's previous-pose
		// local frame. This command is also used by type-1000 mission points.
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		if (target != nullptr)
		{
			work.vector =
				glm::transpose(target->previous_orientation)
				* (actor.previous_position
					- target->previous_position);
		}
	}
	else if (command.id == 6)
	{
		// AI_Fly_begin, LANCER.EXE 0x0040ac00, extracts the actor's
		// current forward basis from its orientation matrix. Direction-mode
		// Fly reuses this snapshot for its complete lifetime.
		work.vector = actor.orientation[2];
	}
	else if (command.id == 4)
	{
		WarpWork& warp = work.warp;
		warp = {};
		warp.previous_tick = tick;
		warp.saved_position = actor.position;
		warp.saved_orientation = actor.orientation;
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		const glm::mat3 target_orientation =
			target == nullptr ? actor.orientation : target->orientation;
		const glm::vec3 target_position =
			target == nullptr ? actor.position : target->position;
		const std::int32_t sequence =
			std::max<std::int32_t>(command.sequence, 0);
		const std::int32_t lane = sequence == 0
			? 0
			: (sequence & 1) != 0
				? (sequence + 1) / 2
				: -(sequence / 2);
		warp.destination =
			target_position
				+ target_orientation
					* glm::vec3{
						static_cast<float>(lane) * 3000.0f,
						0.0f,
						0.0f};
		warp.destination_orientation = target_orientation;
		warp.context = game::wgate_context_find_owner(
			world.transition_effects.wgate,
			static_cast<std::uint16_t>(
				&actor - std::begin(world.objects)));
		if (warp.context < 0)
		{
			warp.context = game::wgate_context_allocate(
				world,
				world.transition_effects.wgate,
				game::WGateMode::ship,
				object_handle(world, actor),
				warp.destination,
				warp.destination_orientation,
				tick);
		}
		if (game::WGateContext* context =
				game::wgate_context_get(
					world.transition_effects.wgate,
					warp.context))
		{
			context->position = warp.destination;
			context->orientation = warp.destination_orientation;
		}
		actor.control_demand = {};
		actor.throttle = 0.0f;
		const std::uint16_t actor_index =
			static_cast<std::uint16_t>(
				&actor - std::begin(world.objects));
		if (actor_index < mission.player_prefix_count)
		{
			// AI_WarpIn_begin, LANCER.EXE 0x0041e550, forces every
			// player-prefix actor out of cloak after resetting its movement.
			game::world_set_cloak_active(world, actor, false, tick);
		}
		warp.local_transition = is_local_actor(world, actor);
		transition_log_stage(actor, "warp-in", 0, tick);
	}
	else if (command.id == 5)
	{
		WarpWork& warp = work.warp;
		warp = {};
		warp.previous_tick = tick;
		warp.saved_position = actor.position;
		warp.saved_orientation = actor.orientation;
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		warp.destination =
			target == nullptr || target == &actor
				? actor.position
					+ actor.orientation[2] * 10000000.0f
				: target->position;
		warp.destination_orientation =
			target == nullptr ? actor.orientation : target->orientation;
		warp.local_transition = is_local_actor(world, actor);
		actor.control_demand = {};
		actor.throttle = 0.0f;
		transition_log_stage(actor, "warp-out", 0, tick);
	}
	else if (command.id == 19 || command.id == 40)
	{
		// AI_JumpIn_begin, LANCER.EXE 0x00416540. Command 40 shares
		// this controller and differs only in state three below.
		JumpWork& jump = work.jump;
		jump = {};
		jump.previous_tick = tick;
		jump.saved_position = actor.position;
		jump.saved_orientation = actor.orientation;
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		if (target != nullptr)
		{
			const std::int32_t sequence = command.sequence;
			const std::int32_t ordinal = sequence + 1;
			const std::int32_t lane =
				(2 * (ordinal & 1) - 1) * (ordinal / 2);
			jump.destination = target->position
				+ target->orientation
					* glm::vec3{
						3000.0f * static_cast<float>(lane),
						0.0f,
						0.0f};
			jump.destination_orientation = target->orientation;
		}
		actor.runtime_flags |= 0x00200004u;
		jump.local_transition = is_local_actor(world, actor);
		if (jump.local_transition)
		{
			world.transition_effects.jump.transition_owner_index =
				static_cast<std::uint16_t>(
					&actor - std::begin(world.objects));
			world.transition_effects.jump.transition_owner_generation =
				actor.generation;
		}
		transition_log_stage(actor, "jump-in", 0, tick);
	}
	else if (command.id == 20 || command.id == 41)
	{
		JumpWork& jump = work.jump;
		jump = {};
		jump.previous_tick = tick;
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		const game::WorldObject* player =
			game::world_resolve(world, world.player);
		jump.synchronized =
			player != nullptr
			&& player->ai.command_count != 0
			&& player->ai.commands[0].id == 20
			&& player->ai.commands[0].target == command.target;
		// JumpOut_initialize, LANCER.EXE 0x004184f0, retains a complete
		// destination frame. A synchronized jump aimed at the player (or
		// with no explicit target) advances that frame 100,000 units along
		// the player's forward axis; the ordinary no-target path uses ten
		// million units along the actor's own forward axis.
		if (jump.synchronized
			&& (target == nullptr || target == player))
		{
			jump.destination =
				player->position + player->orientation[2] * 100000.0f;
			jump.destination_orientation = player->orientation;
		}
		else if (target != nullptr && target != &actor)
		{
			jump.destination = target->position;
			jump.destination_orientation = target->orientation;
		}
		else
		{
			jump.destination =
				actor.position + actor.orientation[2] * 10000000.0f;
			jump.destination_orientation = actor.orientation;
		}
		if (jump.synchronized)
		{
			// JumpOut_initialize (LANCER.EXE 0x004185de..0x004185f9)
			// constructs a zero-roll frame from the formation leader to the
			// resolved destination. The formation offsets, every participant's
			// orientation, and its initial velocity all use this new frame.
			jump.destination_orientation = look_at_points(
				player->position, jump.destination, 0.0f);
		}
		if (jump.synchronized && command.sequence < 45)
		{
			std::int32_t row = 1;
			std::int32_t remainder = command.sequence;
			while (remainder >= row && row < 10)
			{
				remainder -= row;
				++row;
			}
			const float lateral =
				2.0f
				* (static_cast<float>(remainder)
					- static_cast<float>(row - 1) * 0.5f)
				* 3000.0f;
			actor.position = player->position
				+ jump.destination_orientation
					* glm::vec3{
						lateral,
						0.0f,
						-static_cast<float>(row) * 3000.0f};
			actor.orientation = jump.destination_orientation;
			actor.previous_position = actor.position;
			actor.previous_orientation = actor.orientation;
			game::world_zero_motion_controls(actor);
			actor.linear_velocity =
				jump.destination_orientation
					* glm::vec3{0.0f, 0.0f, 150.0f};
			actor.speed = 150.0f;
			actor.runtime_flags |= 0x00000004u;
			const float maximum =
				game::world_effective_max_speed(
					actor, stats, world.camera_mode);
			actor.throttle =
				maximum == 0.0f ? 0.0f : 150.0f / maximum;
		}
		jump.saved_position = actor.position;
		jump.saved_orientation = actor.orientation;
		const glm::vec3 corridor = jump.destination - actor.position;
		const float corridor_length = glm::length(corridor);
		jump.corridor_endpoint = corridor_length > 0.0001f
			? actor.position
				+ corridor * (500000.0f / corridor_length)
			: actor.position + actor.orientation[2] * 500000.0f;
		if (jump.synchronized)
		{
			transition_mark_corridor_dependents(
				world,
				actor,
				actor.position + actor.orientation[2] * 500000.0f);
		}
		jump.local_transition = is_local_actor(world, actor);
		game::JumpEffectsRuntime& effects =
			world.transition_effects.jump;
		effects.overlay_countdown = 15;
		effects.overlay_next_tick = tick + 10;
		effects.overlay_scalar = 1.0f / 15.0f;
		if (jump.local_transition)
		{
			effects.transition_owner_index =
				static_cast<std::uint16_t>(
					&actor - std::begin(world.objects));
			effects.transition_owner_generation = actor.generation;
			world.transition_effects.wgate.camera_target_index =
				actor.mission_index;
			transition_request_camera(
				mission, world, 39, true, true);
		}
		transition_queue_actor_sound(
			world,
			actor,
			29,
			jump.local_transition ? 2 : 0);
		const std::uint16_t actor_index =
			static_cast<std::uint16_t>(
				&actor - std::begin(world.objects));
		if (actor_index < mission.player_prefix_count)
		{
			game::world_set_cloak_active(world, actor, false, tick);
		}
		actor.control_demand = {};
		if (!jump.synchronized)
		{
			actor.throttle = 0.0f;
		}
		else
		{
			actor.control_demand.throttle = actor.throttle;
		}
		transition_log_stage(actor, "jump-out", 0, tick);
	}
	else if (command.id == 28)
	{
		FixedGateWork& fixed = work.fixed_gate;
		fixed = {};
		fixed.previous_tick = tick;
		game::WGateMode mode = game::WGateMode::prototype;
		for (const game::WorldObject& candidate : world.objects)
		{
			if (candidate.active && candidate.type == 0x6eu)
			{
				mode = game::WGateMode::advanced;
				break;
			}
		}
		fixed.context = game::wgate_context_allocate(
			world,
			world.transition_effects.wgate,
			mode,
			object_handle(world, actor),
			actor.position,
			actor.orientation,
			tick);
		game::world_queue_sound_object(
			world, object_handle(world, actor), 49, 4);
		transition_log_stage(actor, "fixed-gate-open", 0, tick);
	}
	else if (command.id == 29)
	{
		// AI_FixedGateClose_begin is sound-only
		// (LANCER.EXE 0x00421920).
		game::world_queue_sound_object(
			world, object_handle(world, actor), 50, 4);
		transition_log_stage(actor, "fixed-gate-close", 0, tick);
	}
	else if (command.id == 25)
	{
		FixedGateWork& fixed = work.fixed_gate;
		fixed = {};
		fixed.previous_tick = tick;
		fixed.gate_object = command.target;
		game::WorldObject* gate =
			resolve_target(actor, command, world, mission);
		fixed.context = game::wgate_context_find_owner(
			world.transition_effects.wgate,
			gate == nullptr
				? UINT16_MAX
				: static_cast<std::uint16_t>(
					gate - std::begin(world.objects)));
		game::WGateContext* context = game::wgate_context_get(
			world.transition_effects.wgate, fixed.context);
		if (gate != nullptr && context != nullptr)
		{
			fixed.gate_position = context->position;
			fixed.gate_orientation = context->orientation;
			fixed.local_transition = is_local_actor(world, actor);
			const glm::vec3 gate_local_start =
				fixed.local_transition
					? glm::vec3{0.0f, 2000.0f, 18000.0f}
					: glm::vec3{0.0f, 0.0f, 26000.0f};
			fixed.start_position = context->position
				+ context->orientation * gate_local_start;
			const bool simplified =
				actor.type < assets::kShipStatsCount
				&& stats.records[actor.type]
					.flight.simplified_steering;
			const float exit_distance =
				simplified ? -25000.0f : -53000.0f;
			glm::vec3 exit_offset{0.0f, 0.0f, exit_distance};
			const bool unrotated_krasny =
				actor.type == 0x9au
				&& (mission.game_mode == 0x42u
					|| mission.game_mode == 0x10u);
			if (!unrotated_krasny)
			{
				const float yaw =
					static_cast<float>(
						world.transition_effects.wgate
							.fixed_jump_angle_index)
					* 0.3f;
				++world.transition_effects.wgate
					.fixed_jump_angle_index;
				exit_offset =
					math::rotation_from_euler({0.0f, yaw, 0.0f})
					* exit_offset;
			}
			fixed.end_position = fixed.start_position
				+ context->orientation * exit_offset;
			actor.orientation = look_at_points(
				fixed.start_position,
				fixed.end_position,
				0.0f);
			actor.previous_orientation = actor.orientation;
			if (world.transition_effects.wgate
					.fixed_jump_angle_index > 2)
			{
				world.transition_effects.wgate
					.fixed_jump_angle_index = -2;
			}
			if (fixed.local_transition)
			{
				world.transition_effects.wgate
					.fixed_jump_angle_index = 0;
			}
			else if (world.transition_effects.wgate
					.fixed_jump_angle_index == 0)
			{
				world.transition_effects.wgate
					.fixed_jump_angle_index = 1;
			}
			actor.targetable = false;
			actor.runtime_flags =
				(actor.runtime_flags & ~0x00000200u)
				| 0x00000018u;
			transition_set_type3_visibility(actor, false);
			if ((actor.runtime_flags & game::kObjectFlagCompound) == 0)
			{
				actor.runtime_flags |= 0x00000004u;
			}
			game::wgate_context_register_deformation(
				world.transition_effects.wgate,
				static_cast<std::uint16_t>(
					&actor - std::begin(world.objects)));
		}
		transition_log_stage(actor, "fixed-gate-in", 0, tick);
	}
	else if (command.id == 26)
	{
		FixedGateWork& fixed = work.fixed_gate;
		fixed = {};
		fixed.previous_tick = tick;
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		fixed.context = game::wgate_context_find_owner(
			world.transition_effects.wgate,
			target == nullptr
				? UINT16_MAX
				: static_cast<std::uint16_t>(
					target - std::begin(world.objects)));
		float nearest = std::numeric_limits<float>::max();
		for (std::int16_t index = 0;
			index < static_cast<std::int16_t>(
				world.transition_effects.wgate.contexts.size());
			++index)
		{
			const game::WGateContext* context =
				game::wgate_context_get(
					world.transition_effects.wgate, index);
			if (context == nullptr
				|| (context->mode != game::WGateMode::prototype
					&& context->mode != game::WGateMode::advanced))
			{
				continue;
			}
			const glm::vec3 delta =
				context->position - actor.position;
			const float distance = glm::dot(delta, delta);
			if (distance < nearest)
			{
				nearest = distance;
				fixed.context = index;
			}
		}
		if (game::WGateContext* context =
				game::wgate_context_get(
					world.transition_effects.wgate,
					fixed.context))
		{
			fixed.gate_position = context->position;
			fixed.gate_orientation = context->orientation;
			fixed.local_transition = is_local_actor(world, actor);
			fixed.end_position =
				context->position
				+ context->orientation
					* glm::vec3{
						0.0f,
						0.0f,
						fixed.local_transition
							? 12000.0f : 26000.0f};
			game::wgate_context_register_deformation(
				world.transition_effects.wgate,
				static_cast<std::uint16_t>(
					&actor - std::begin(world.objects)));
			if (fixed.local_transition)
			{
				game::wgate_wormhole_create(
					world.transition_effects.wgate,
					context->position,
					context->orientation);
				// Retail allocates the Worm mesh here, but the shared
				// worm-visibility latch keeps it out of the renderer until
				// state one completes.
				world.transition_effects.wgate.wormhole.active = false;
				world.transition_effects.wgate.wormhole_active = false;
				world.transition_effects.wgate
					.wormhole_owner_index =
						static_cast<std::uint16_t>(
							&actor - std::begin(world.objects));
				world.transition_effects.wgate
					.wormhole_owner_generation =
						actor.generation;
			}
		}
		// AI_FixedGateJumpOut_begin clears only the three rotational input
		// requests, scalar speed, and three angular rates. Throttle is
		// deliberately retained.
		actor.control_demand.roll = 0.0f;
		actor.control_demand.pitch = 0.0f;
		actor.control_demand.yaw = 0.0f;
		actor.speed = 0.0f;
		actor.angular_x = 0.0f;
		actor.angular_y = 0.0f;
		actor.angular_z = 0.0f;
		actor.targetable = false;
		actor.runtime_flags =
			(actor.runtime_flags & ~0x00000200u)
			| 0x0000001cu;
		transition_log_stage(actor, "fixed-gate-out", 0, tick);
	}
	else if (command.id == 31)
	{
		FixedGateWork& fixed = work.fixed_gate;
		fixed = {};
		world.player_exhaust_exposure_percent = 100;
		if (actor.type == 0x6du)
		{
			transition_create_protogate_arcs(world, actor);
		}
		if ((mission.game_mode == 0x42u || mission.game_mode == 0x10u)
			&& world.transition_effects.wgate
				.krasny_gate_effect_active)
		{
			for (const game::WorldObject& candidate : world.objects)
			{
				if (candidate.active && candidate.type == 0x9au
					&& candidate.ai.command_count != 0
					&& candidate.ai.commands[0].id == 25)
				{
					world.transition_effects.wgate
						.krasny_split_ready = true;
					break;
				}
			}
		}
		diagnostics::mission_log(
			"fixed-gate-collapse start actor=%u tick=%u",
			static_cast<unsigned>(actor.mission_index),
			tick);
	}
	else if (command.id == 38)
	{
		WarpProjectorWork& projector = work.warp_projector;
		projector = {};
		projector.start_tick = tick;
		projector.previous_tick = tick;
		projector.initialized = actor.type == 0xa8u;
		if (!projector.initialized)
		{
			diagnostics::mission_log(
				"warp-projector rejected actor=%u type=%u",
				static_cast<unsigned>(actor.mission_index),
				static_cast<unsigned>(actor.type));
		}
		else
		{
			// AI_BoridinWarpProjection_begin performs this otherwise-unused
			// named lookup before allocating the six projector meshes.
			(void)game::retained_find_named_model(
				actor, "Warp projector 03");
			game::WGateEffectsRuntime& effects =
				world.transition_effects.wgate;
			game::wgate_projectors_create(
				world,
				effects,
				object_handle(world, actor),
				tick);
			actor.control_demand.throttle = 0.0f;
			actor.control_demand.roll = 0.0f;
			actor.control_demand.pitch = 0.0f;
			actor.control_demand.yaw = 0.0f;
			transition_log_stage(
				actor, "warp-projector", 0, tick);
		}
	}
	else if (command.id == 39)
	{
		// AI_RipperDrop_begin, LANCER.EXE 0x00410b90.
		work.entered_tick = tick;
		work.stage = 0;
		actor.control_demand.throttle = 0.0f;
		actor.control_demand.roll = 0.0f;
		actor.control_demand.pitch = 0.0f;
		actor.control_demand.yaw = 0.0f;
		actor.control_demand.linear_with_exhaust = true;
		actor.flight_callback_mode =
			game::FlightCallbackMode::linear_with_exhaust;
		actor.runtime_flags |= game::kObjectFlagKinematic;
	}
	else if (command.id == 111)
	{
		// AI_RipperEndDrop_begin, LANCER.EXE 0x00410e60.
		actor.runtime_flags &= ~game::kObjectFlagKinematic;
		work.entered_tick = tick;
		work.stage = 0;
	}
	else if (command.id == 112)
	{
		// AI_RipperAttachCargoPod_begin, LANCER.EXE 0x00411200.
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		game::WorldObject* cargo =
			actor.interaction_target_link < game::kMaxGameObjects
				? &world.objects[actor.interaction_target_link]
				: nullptr;
		if (target == nullptr || cargo == nullptr || !cargo->active)
		{
			command_pop(world, actor);
			return;
		}
		RipperAttachWork& attach = work.ripper_attach;
		attach = {};
		attach.stage_start_tick = tick;
		attach.stage = 0;
		attach.carried_object = static_cast<std::uint16_t>(
			cargo - std::begin(world.objects));
		attach.carried_generation = cargo->generation;
		ripper_attach_component_frame(
			*target,
			command.target_component,
			attach.approach_position,
			attach.component_position,
			attach.component_rotation);
		actor.flight_callback_mode =
			game::FlightCallbackMode::standard_reverse;
		actor.control_demand.linear_no_exhaust = false;
		actor.control_demand.linear_with_exhaust = false;
		actor.runtime_flags |= game::kObjectFlagKinematic;
		attach.effect_slot = game::ripper_grab_effect_allocate(
			world.death_effects,
			object_handle(world, actor),
			object_handle(world, *cargo));
		diagnostics::mission_log(
			"ripper attach begin actor=%u cargo=%u target=%u "
			"component=%d tick=%u",
			static_cast<unsigned>(actor.mission_index),
			static_cast<unsigned>(cargo->mission_index),
			static_cast<unsigned>(target->mission_index),
			static_cast<int>(command.target_component),
			tick);
	}
	else if (command.id == 120)
	{
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		if (target != nullptr
			&& target_reference_valid(
				*target,
				command.target_component,
				0x00000100u))
		{
			// AI_Mill_begin stores the current simulation tick at work +0
			// before constructing the target-to-actor orbital frame.
			work.deadline = tick;
			work.orientation = look_at_points(
				resolved_target_point(
					*target, command.target_component),
				actor.position,
				0.0f);
		}
	}
	else if (command.id == 105)
	{
		fight_begin(
			actor, command, world, mission, stats, pilots, tick);
	}
	else if (command.id == 114)
	{
		// AI_Disrupted_begin, LANCER.EXE 0x0040c140..0x0040c366.
		// Packed +0x0a is both the lifetime of all fifteen arcs and the
		// work deadline. Packed +0x0e is the local force supplied by the
		// type-five shockwave dispatcher.
		actor.runtime_flags |= 0x00000008u;
		work.deadline = tick + command.state[0];
		const glm::vec3 local_impulse{
			command_state_float(command.state[1]),
			command_state_float(command.state[2]),
			command_state_float(command.state[3]),
		};
		actor.linear_velocity += actor.orientation * local_impulse;
		const auto perturb = [&world]()
		{
			return (
				static_cast<float>(game::world_rand15(world))
					/ 32767.0f
				- 0.5f) * 0.1f;
		};
		actor.angular_z += perturb();
		actor.angular_y += perturb();
		actor.angular_x += perturb();
		actor.inertial_angular_step =
			math::rotation_from_euler({
				actor.angular_x,
				actor.angular_y,
				actor.angular_z,
			});

		const std::uint16_t actor_index =
			static_cast<std::uint16_t>(
				&actor - std::begin(world.objects));
		for (std::uint32_t index = 0; index < 15; ++index)
		{
			game::ElectricRayEffect* effect =
				game::electric_ray_create(
					world,
					1,
					static_cast<std::int32_t>(command.state[0]),
					0.6f,
					90.0f,
					0x07u);
			effect->start = glm::vec3{0.0f};
			const float angle_z =
				static_cast<float>(game::world_rand15(world))
					/ 32767.0f * 6.2831853071795864769f;
			const float angle_y =
				static_cast<float>(game::world_rand15(world))
					/ 32767.0f * 6.2831853071795864769f;
			const float angle_x =
				static_cast<float>(game::world_rand15(world))
					/ 32767.0f * 6.2831853071795864769f;
			effect->end =
				math::rotation_from_euler({
					angle_x, angle_y, angle_z})
				* glm::vec3{0.0f, 0.0f, actor.radius};
			game::electric_ray_set_parent(
				*effect, actor_index, actor.generation);
			game::electric_ray_set_group_color(
				*effect,
				0,
				(index & 1u) == 0
					? glm::vec3{0.8f, 0.8f, 1.0f}
					: glm::vec3{0.3f, 0.5f, 1.0f});
		}
	}
}

bool update_command(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	const assets::PilotStatsTable& pilots,
	const assets::GunStatsTable& gun_stats,
	const assets::MissileStatsTable& missile_stats,
	game::MissileRuntime& missiles,
	game::ChaffRuntime& chaff,
	const game::FlightDemand& player_demand,
	std::uint32_t& random_seed,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	if (scripted_command_update(
		actor,
		command,
		world,
		mission,
		file,
		stats,
		player_demand,
		tick,
		demand,
		applies_flight))
	{
		return actor.ai.command_count != 0;
	}
	Work& work = actor.ai.work;
	switch (command.id)
	{
	case 0:
		demand = {};
		return true;
	case 1:
	{
		// AI_FlyAimlessly_update, LANCER.EXE 0x0040a980.
		const float angle =
			static_cast<float>(work.deadline)
			* 0.05f
			* 6.2831853071795864769f;
		const glm::vec3 local{
			0.0f,
			(std::cos(angle) - 1.0f)
				* static_cast<float>(work.strategy + 1u)
				* 25000.0f,
			std::sin(
				static_cast<float>(work.strategy) * angle)
				* 50000.0f,
		};
		const glm::vec3 destination =
			work.vector + work.orientation * local;
		demand = steer_toward(
			actor,
			destination,
			work.phase,
			1.0f,
			0.0f,
			3,
			&world,
			&stats,
			mission.frame_delta_ticks);
		const glm::vec3 separation = actor.position - destination;
		if (glm::dot(separation, separation) < 1000000.0f)
		{
			++work.deadline;
		}
		return true;
	}
	case 2:
	case 3:
	{
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		game::ObjectHandle target_handle;
		if (target != nullptr)
		{
			target_handle = object_handle(world, *target);
		}
		for (std::uint8_t index = 0;
			index < actor.attachment_count;
			++index)
		{
			const game::AttachmentSlot& attachment =
				actor.attachments[index];
			if (attachment.remaining_count <= 0
				|| (command.id == 2
					? attachment.kind == 3
					: attachment.kind != 3))
			{
				continue;
			}
			game::missiles_launch_from_ship_mount(
				missiles,
				world,
				missile_stats,
				object_handle(world, actor),
				index,
				target_handle,
				command.target_component,
				tick);
			break;
		}
		command_pop(world, actor);
		applies_flight = false;
		return false;
	}
	case 7:
	{
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		if (target == nullptr || target->type == 1001)
		{
			command_pop(world, actor);
			return false;
		}
		// AI_RunAway_update, LANCER.EXE 0x0040ade0. Vector_subtract at
		// 0x0040ae1e forms actor - target, then the callback scales that
		// raw separation by 100,000 and adds it back to the actor position.
		glm::vec3 destination =
			actor.position
				+ (actor.position - target->position) * 100000.0f;
		apply_dynamic_avoidance(actor, world, destination);
		demand = steer_toward(
			actor,
			destination,
			1.0f,
			1.0f,
			0.1f,
			3,
			&world,
			&stats,
			mission.frame_delta_ticks);
		demand.throttle = 0.5f;
		return true;
	}
	case 9:
	{
		game::WorldObject* target =
			mission::runtime_resolve_object(
				mission, work.target, world);
		if (target == nullptr || target->type == 1001)
		{
			command_pop(world, actor);
			return false;
		}
		const glm::vec3 destination =
			target->position + target->orientation[2] * 10000.0f;
		const float distance =
			glm::distance(actor.position, destination);
		const bool far = distance * distance > 25000000.0f;
		demand = steer_toward(
			actor,
			destination,
			1.0f,
			far ? 1.0f : 0.5f,
			0.0f,
			far ? 3u : 4u,
			&world,
			&stats,
			mission.frame_delta_ticks);
		demand.throttle =
			target->speed
				/ game::world_effective_max_speed(
					actor, stats, world.camera_mode)
			+ distance * 0.0001f;
		return true;
	}
	case 6:
	{
		// AI_Fly_update, LANCER.EXE 0x0040ac20.
		// GameObject_create_runtime installs the provider table for every
		// byte-sized type and leaves it null on the >255 anchor fast path.
		const bool has_flight_provider = actor.type <= UINT8_MAX;
		float throttle = 1.0f;
		if (command.state[0] != 0)
		{
			const float requested_speed = static_cast<float>(
				static_cast<std::int32_t>(command.state[0]));
			if (!has_flight_provider)
			{
				// The providerless path at 0x0040ac67 reads work +0x08:
				// the forward vector captured once by AI_Fly_begin.
				const glm::vec3 position =
					actor.position + work.vector
					* (requested_speed
						* static_cast<float>(
							mission.frame_delta_ticks)
						* 0.25f);
				// GameObject_set_all_position_states at 0x0040ac9d
				// publishes the same point to every retained pose cache.
				actor.previous_position = position;
				actor.position = position;
				actor.scene_position = position;
				applies_flight = false;
				return true;
			}
			throttle =
				requested_speed
				/ game::world_effective_max_speed(
					actor, stats, world.camera_mode);
		}

		glm::vec3 destination;
		if (command.target != UINT16_MAX)
		{
			game::WorldObject* target =
				resolve_target(actor, command, world, mission);
			if (target == nullptr)
			{
				command_pop(world, actor);
				return false;
			}
			const glm::vec3 delta = target->position - actor.position;
			if (glm::dot(delta, delta) < 4000000.0f)
			{
				demand = {};
				command_pop(world, actor);
				return false;
			}
			destination = target->position;
		}
		else
		{
			destination = actor.position + work.vector * 20000.0f;
		}
		if (!has_flight_provider)
		{
			applies_flight = false;
			return true;
		}

		bool avoided = false;
		demand = steer_toward(
			actor,
			destination,
			throttle,
			1.0f,
			0.0f,
			7,
			&world,
			&stats,
			mission.frame_delta_ticks,
			&avoided);
		if (avoided)
		{
			demand.throttle *= 0.5f;
		}
		return true;
	}
	case 16:
		game::world_set_cloak_active(
			world,
			actor,
			(actor.runtime_flags & 0x00000100u) == 0,
			tick);
		command_pop(world, actor);
		applies_flight = false;
		return false;
	case 114:
		if (work.deadline < tick)
		{
			command_pop(world, actor);
			return false;
		}
		return true;
	case 10:
	{
		// AI_FindNewTarget_update, LANCER.EXE 0x0040b040. The selected
		// combat command is inserted ahead of Find New Target; the search
		// therefore resumes after that temporary command ends.
		const TargetSelection selection =
			find_new_target_candidates(
				actor, command, world, mission, tick);
		std::int16_t followup = -1;
		std::uint16_t target = UINT16_MAX;
		std::int16_t component = -1;
		if (selection.secondary != UINT16_MAX)
		{
			if (selection.primary != UINT16_MAX)
			{
				followup =
					actor.collision_class == 5 ? 103 : 105;
				target = selection.primary;
				component = selection.primary_component;
				// AI_FindNewTarget_update checks local ownership only
				// before installing Fight (0x0040b097..0x0040b0ae).
				// Torpedo and Mill bypass this network gate.
				if (followup == 105
					&& mission.network.role
						!= mission::NetworkRole::offline
					&& !mission::network_local_owns_object(
						mission.network,
						object_handle(world, actor).index,
						mission.player_prefix_count,
						world.player.index))
				{
					return true;
				}
			}
			else
			{
				followup = 120;
				target = selection.secondary;
				component = selection.secondary_component;
			}
		}
		else if (selection.primary != UINT16_MAX)
		{
			demand = {};
			return true;
		}
		else
		{
			command_pop(world, actor);
			return false;
		}
		if (command_push(world,
			actor,
			followup,
			TargetKind::object,
			target,
			component))
		{
			diagnostics::mission_log(
				"ai target selected actor=%u command=%d(%s) "
				"target=%u component=%d",
				static_cast<unsigned>(actor.mission_index),
				static_cast<int>(followup),
				command_name(followup),
				static_cast<unsigned>(target),
				static_cast<int>(component));
		}
		// All three insertion exits at 0x0040b0b4..0x0040b0ec leave the
		// retained controls untouched. The selected maneuver takes ownership
		// on its next service.
		return false;
	}
	case 43:
	{
		// AI_HugeExplosion_update, LANCER.EXE 0x004086c0, starts the
		// singleton Uber explosion at the actor's complete root frame with
		// the compiled 50,000-unit size and 1,500-tick lifetime. Its global
		// callback owns all later visuals, damage propagation, and sounds.
		game::uber_explosion_start(
			world,
			object_handle(world, actor),
			actor.position,
			actor.orientation,
			50000.0f,
			1500,
			mission.network.role
				!= mission::NetworkRole::offline,
			stats,
			tick);
		command_pop(world, actor);
		return false;
	}
	case 11:
	{
		ExplodeWork& explode = work.explode;
		if (explode.mode == 1)
		{
			// AI_ExplodeCompound_update, LANCER.EXE 0x004091e0.
			if ((actor.runtime_flags & game::kObjectFlagDisabled) != 0)
			{
				command_clear(world, actor);
				actor.runtime_flags |= game::kObjectFlagDestroyed;
				if (actor.mission_index < mission.object_count)
				{
					mission::events_emit_destroyed(
						mission,
						actor.mission_index,
						last_attacker_mission_index(
							world, actor));
				}
			}
			else
			{
				command_pop(world, actor);
			}
			applies_flight = false;
			return false;
		}
		if (explode.mode == 2)
		{
			// AI_ExplodeCompoundComponent_update,
			// LANCER.EXE 0x00409260, tail-calls AI_command_pop.
			command_pop(world, actor);
			applies_flight = false;
			return false;
		}
		if (explode.mode == 3)
		{
			// AI_ExplodeAsteroid_update, LANCER.EXE 0x004092a0.
			if (tick <= work.deadline)
			{
				demand = {};
				applies_flight = false;
				return true;
			}
			(void)game::explosion_billboard_create(
				world.death_effects,
				world,
				actor.position,
				glm::vec3{0.0f},
				game::ExplosionBillboardType::separate_frames,
				actor.radius * 1.5f,
				150,
				true,
				0,
				false,
				false,
				tick);
			const glm::vec3 parent_position = actor.position;
			const glm::mat3 parent_orientation = actor.orientation;
			const float fragment_scale = actor.effect_scale * 0.4f;
			game::world_mark_departed(
				world, object_handle(world, actor));
			if (fragment_scale < 0.16f)
			{
				applies_flight = false;
				return false;
			}
			for (std::uint8_t ordinal = 3; ordinal > 0; --ordinal)
			{
				const std::uint16_t type = static_cast<std::uint16_t>(
					0x7bu + game::world_rand15(world) % 4u);
				const glm::mat3 fragment_orientation =
					math::postrotate(
						parent_orientation,
						static_cast<float>(ordinal)
							* 0x1.e28c76p+0f,
						{1.0f, 0.0f, 0.0f});
				const float fragment_radius =
					world.model_radius_by_type[type];
				const glm::vec3 fragment_position =
					parent_position
						+ fragment_orientation[2]
							* (fragment_radius * 3.0f);
				const game::ObjectHandle fragment_handle =
					game::world_create(
						world,
						type,
						fragment_position,
						fragment_orientation,
						stats,
						false);
				if (game::WorldObject* fragment =
					game::world_resolve(world, fragment_handle))
				{
					fragment->throttle = 0.0f;
					fragment->effect_scale = fragment_scale;
					fragment->runtime_flags |= 0x00000004u;
				}
			}
			applies_flight = false;
			return false;
		}
		if (explode.mode == 4)
		{
			// AI_ExplodeType1D_update, LANCER.EXE 0x004095f0.
			const bool root_hidden =
				actor.model_references.empty()
				|| (actor.model_references[0].runtime_flags
					& 0x00000020u) != 0;
			if (root_hidden)
			{
				death_emit_standard_destruction(
					actor, world, mission, tick);
				game::world_mark_departed(
					world, object_handle(world, actor));
				applies_flight = false;
				return false;
			}
			actor.model_references[0].runtime_flags |= 0x00000020u;
			const glm::mat4 root_transform =
				math::model_transform(
					actor.orientation, 1.0f, actor.position)
				* game::model_animation_render_transform(
					actor, 0, 1.0f);
			const glm::vec3 replacement_position{root_transform[3]};
			const glm::mat3 replacement_orientation{root_transform};
			death_emit_standard_destruction(
				actor, world, mission, tick);
			const game::ObjectHandle actor_handle =
				object_handle(world, actor);
			const game::ObjectHandle successor = game::world_recreate(
				world,
				actor_handle,
				0xbcu,
				{0.0f, 0.0f, 0.0f},
				glm::mat3{1.0f},
				stats,
				false);
			if (game::WorldObject* replacement =
				game::world_resolve(world, successor))
			{
				replacement->previous_position = replacement_position;
				replacement->position = replacement_position;
				replacement->previous_orientation =
					replacement_orientation;
				replacement->orientation = replacement_orientation;
			}
			diagnostics::mission_log(
				"death type-1d replacement world=%u generation=%u",
				static_cast<unsigned>(successor.index),
				static_cast<unsigned>(successor.generation));
			applies_flight = false;
			return false;
		}
		if (explode.mode == 0 && explode.variant == 0)
		{
			// AI_ExplodeOrdinaryVariant0_update,
			// LANCER.EXE 0x00408a60/0x00408f70.
			if (tick > work.deadline)
			{
				death_emit_standard_destruction(
					actor, world, mission, tick);
				death_finalize_object(
					actor, world, mission, stats, tick);
				applies_flight = false;
				return false;
			}
			const std::int32_t remaining_ticks =
				static_cast<std::int32_t>(work.deadline - tick);
			if (static_cast<std::int32_t>(command.sequence) * 10
				> remaining_ticks)
			{
				const auto offset_random = [&world]()
				{
					return (static_cast<float>(
								game::world_rand15(world))
								* 0x1.0002p-15f
							- 0.5f)
						* 500.0f;
				};
				// MSVC evaluates the source arguments right-to-left.
				const float offset_z = offset_random();
				const float offset_y = offset_random();
				const float offset_x = offset_random();
				const glm::vec3 local_offset{
					offset_x, offset_y, offset_z};
				if ((actor.runtime_flags & 0x01000000u) == 0
					|| (command.sequence & 1) == 0)
				{
					game::particle_fragment_directional_burst(
						world,
						actor.position
							+ actor.orientation * local_offset,
						-actor.orientation[2],
						0.0f,
						0.1f,
						1.0f,
						1,
						tick);
				}
				--command.sequence;
			}
			const float angular_scale =
				static_cast<float>(remaining_ticks) * 0.005f;
			demand = {};
			demand.roll =
				explode.emission_direction.z * angular_scale;
			demand.pitch =
				explode.emission_direction.x * angular_scale;
			demand.yaw =
				explode.emission_direction.y * angular_scale;
			return true;
		}
		if (explode.mode == 0 && explode.variant == 1)
		{
			// The retail variant update is an intentional no-op. Its zero
			// deadline normally sends the first service directly through
			// the variant-one finalizer.
			if (tick <= work.deadline)
			{
				demand = actor.control_demand;
				return true;
			}
			death_emit_variant_one_destruction(
				actor, world, mission, tick);
			death_finalize_object(
				actor, world, mission, stats, tick);
			applies_flight = false;
			return false;
		}
		if (explode.mode == 0 && explode.variant == 2)
		{
			// Variant two shares 0x00408f70 with variant zero, but its
			// zero deadline normally reaches this final boundary directly.
			if (tick <= work.deadline)
			{
				const auto offset_random = [&world]()
				{
					return (static_cast<float>(
								game::world_rand15(world))
								* 0x1.0002p-15f
							- 0.5f)
						* 500.0f;
				};
				const float offset_z = offset_random();
				const float offset_y = offset_random();
				const float offset_x = offset_random();
				if ((actor.runtime_flags & 0x01000000u) == 0
					|| (command.sequence & 1) == 0)
				{
					game::particle_fragment_directional_burst(
						world,
						actor.position
							+ actor.orientation
								* glm::vec3{
									offset_x,
									offset_y,
									offset_z},
						-actor.orientation[2],
						0.0f,
						0.1f,
						1.0f,
						1,
						tick);
				}
				--command.sequence;
				demand = {};
				return true;
			}
			if (actor.type != 0x4au && actor.type != 0x5cu)
			{
				death_emit_standard_destruction(
					actor, world, mission, tick);
			}
			death_finalize_object(
				actor, world, mission, stats, tick);
			applies_flight = false;
			return false;
		}
		applies_flight = false;
		return false;
	}
	case 30:
	{
		if (mission.network.role != mission::NetworkRole::offline)
		{
			actor.protection_state = 0;
			command_pop(world, actor);
			applies_flight = false;
			return false;
		}
		if (work.stage == 0 && work.deadline < tick)
		{
			actor.flight_callback_mode =
				game::FlightCallbackMode::damp_velocity_0_97;
			actor.runtime_flags &= ~0x00000008u;
			actor.protection_state =
				work.ejection.saved_protection_state;
			if (is_local_actor(world, actor))
			{
				work.stage = 2;
				work.deadline = tick + 100;
			}
			else
			{
				work.stage = 1;
				const std::uint16_t actor_index =
					static_cast<std::uint16_t>(
						&actor - std::begin(world.objects));
				if (mission.network.role
						!= mission::NetworkRole::offline
					&& actor_index < mission.player_prefix_count)
				{
					actor.runtime_flags |= game::kObjectFlagDisabled;
				}
			}
			diagnostics::mission_log(
				"ejection actor=%u stage=%u tick=%u",
				static_cast<unsigned>(actor.mission_index),
				static_cast<unsigned>(work.stage),
				tick);
		}
		else if (work.stage == 2 && work.deadline < tick)
		{
			death_queue_speech(
				mission,
				(game::world_rand15(world) & 1u) != 0
					? "ejt_016.ut" : "ejt_015.ut");
			work.stage = 3;
			work.deadline += 500;
			diagnostics::mission_log(
				"ejection actor=%u stage=3 tick=%u",
				static_cast<unsigned>(actor.mission_index),
				tick);
		}
		else if (work.stage == 3 && work.deadline < tick)
		{
			eject_choose_outcome(
				actor, world, mission, stats, tick);
			work.stage = 4;
		}
		demand = {};
		return true;
	}
	case 106:
		if (work.deadline < tick)
		{
			// AI_Eject_update, LANCER.EXE 0x004160a0, passes the still-live
			// command 106 head to AI_schedule_death_command. That routine
			// overwrites the head directly; it does not pop/reset its work.
			schedule_death_command(
				actor, world, mission, 0, false);
			return false;
		}
		demand = {};
		return true;
	case 108:
		if (work.deadline < tick)
		{
			actor.runtime_flags &= ~0x00000800u;
			command_pop(world, actor);
			command_push(world,
				actor, 30, TargetKind::none, UINT16_MAX);
			actor.protection_state = 1;
			actor.runtime_flags |= 0x00000800u;
			return false;
		}
		return true;
	case 113:
	{
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		if (target == nullptr
			|| (target->runtime_flags & game::kObjectFlagDestroyed) != 0)
		{
			demand.throttle = 1.0f;
			return true;
		}
		work.deadline = tick;
		const glm::vec3 point = target->position;
		demand = steer_toward(
			actor, point, 1.0f, 1.0f, 0.0f, 0,
			&world, &stats, mission.frame_delta_ticks);
		const glm::vec3 delta = point - actor.position;
		const float length = glm::length(delta);
		if (length < 20000.0f)
		{
			if (length > 0.0001f
				&& glm::dot(
					actor.orientation[2], delta / length) > 0.95f)
			{
				demand.roll = 1.0f;
			}
			actor.primary_weapon_requested = true;
			work.primary_weapon_delay = 100;
		}
		return true;
	}
	case 118:
		if (work.deadline < tick)
		{
			// AI_EjectPlayer_update increments work +4 but deliberately
			// retains flag 0x800. The death scheduler sees that flag and
			// overwrites command 118 with Explode without re-enabling player
			// controls during the terminal sequence.
			++work.stage;
			schedule_death_command(
				actor, world, mission, 0, false);
			return false;
		}
		demand = player_demand;
		return true;
	case 12:
	{
		// AI_RipperGrab_update, LANCER.EXE 0x0040ff80.
		RipperGrabWork& grab = work.ripper_grab;
		game::WorldObject* target = game::world_resolve(
			world,
			{grab.target_object, grab.target_generation});
		const auto abort_grab = [&]()
		{
			game::ripper_grab_effect_release(
				world.death_effects, grab.effect_slot);
			grab.effect_slot = -1;
			actor.runtime_flags &= ~game::kObjectFlagKinematic;
			actor.flight_callback_mode =
				static_cast<game::FlightCallbackMode>(
					grab.saved_flight_callback);
			command_pop(world, actor);
			applies_flight = false;
		};
		if (target == nullptr
			|| target->type == 0x3e9u
			|| (target->runtime_flags & game::kObjectFlagDestroyed) != 0)
		{
			abort_grab();
			return false;
		}
		const std::uint32_t now_tick = tick;
		const auto advance = [&]()
		{
			++grab.stage;
			grab.stage_start_tick = now_tick;
		};
		const auto controls_settled =
			[&actor](const game::FlightDemand& active, bool throttle)
		{
			return std::abs(active.roll) <= 0.025f
				&& std::abs(active.pitch) <= 0.025f
				&& std::abs(active.yaw) <= 0.025f
				&& (!throttle
					|| std::abs(active.throttle) <= 0.025f)
				&& std::abs(actor.angular_x) <= 0.02f
				&& std::abs(actor.angular_y) <= 0.02f
				&& std::abs(actor.angular_z) <= 0.02f;
		};
		const auto update_beams =
			[&](const glm::vec3& endpoint, float alpha)
		{
			glm::vec3 origins[4];
			glm::vec3 targets[4];
			ripper_beam_origins(actor, origins);
			ripper_beam_targets(*target, endpoint, targets);
			game::ripper_grab_effect_update(
				world.death_effects,
				grab.effect_slot,
				origins,
				targets,
				alpha);
		};
		demand = actor.control_demand;
		demand.linear_with_exhaust = true;
		switch (grab.stage)
		{
		case 0:
			// AI_sequence_sync(1) succeeds immediately offline.
			advance();
			return true;
		case 1:
		{
			demand = steer_toward(
				actor,
				grab.approach_position,
				0.3f,
				1.0f,
				0.0f,
				0,
				&world,
				&stats,
				mission.frame_delta_ticks);
			demand.linear_with_exhaust = true;
			const float distance =
				glm::distance(actor.position, grab.approach_position);
			if (distance
					< game::world_effective_max_speed(
						actor, stats, world.camera_mode) * 6.0f
				&& distance > 0.0001f
				&& glm::dot(
						actor.orientation[2],
						(grab.approach_position - actor.position)
							/ distance)
					< 0.7f)
			{
				demand.throttle = 0.0f;
				return true;
			}
			const float inner = grab.close_approach ? 100.0f : 2000.0f;
			if (distance > inner + 2000.0f)
			{
				demand.throttle = 1.0f;
			}
			else if (distance > inner)
			{
				demand.throttle = 0.2f;
			}
			else
			{
				demand.throttle = 0.0f;
			}
			if (!controls_settled(demand, true))
			{
				return true;
			}
			demand.roll = 0.0f;
			demand.pitch = 0.0f;
			demand.yaw = 0.0f;
			demand.throttle = 0.0f;
			// AI_sequence_sync(2).
			advance();
			if (!actor.model_references.empty())
			{
				game::model_animation_start_named(
					actor,
					0,
					"ready to grab",
					0.0f,
					1,
					15.0f);
			}
			return true;
		}
		case 2:
			if (!grab.close_approach)
			{
				advance();
				return true;
			}
			demand = steer_toward(
				actor,
				target->position,
				0.3f,
				1.0f,
				0.0f,
				0,
				&world,
				&stats,
				mission.frame_delta_ticks);
			demand.linear_with_exhaust = true;
			if (!controls_settled(demand, false))
			{
				return true;
			}
			demand = {};
			demand.linear_with_exhaust = true;
			// AI_sequence_sync(3).
			advance();
			return true;
		case 3:
		{
			const float time = std::min(
				1.0f,
				static_cast<float>(now_tick - grab.stage_start_tick)
					/ 150.0f);
			update_beams(grab.approach_position, time * time);
			if (now_tick <= grab.stage_start_tick + 150u)
			{
				return true;
			}
			advance();
			grab.beam_start = target->position;
			grab.beam_end =
				actor.position + actor.orientation[2] * 300.0f;
			target->runtime_flags &= ~0x00000018u;
			game::world_queue_sound_object(
				world, object_handle(world, *target), 0x3d, 0);
			return true;
		}
		case 4:
		{
			const float time = std::min(
				1.0f,
				static_cast<float>(now_tick - grab.stage_start_tick)
					/ 300.0f);
			update_beams(
				glm::mix(grab.beam_start, grab.beam_end, time),
				1.0f);
			if (now_tick <= grab.stage_start_tick + 300u)
			{
				return true;
			}
			advance();
			grab.target_rotation_start =
				math::rotation_to_euler(target->orientation);
			grab.target_rotation_end =
				math::rotation_to_euler(actor.orientation);
			return true;
		}
		case 5:
		{
			const float time = std::min(
				1.0f,
				static_cast<float>(now_tick - grab.stage_start_tick)
					/ 500.0f);
			glm::vec3 rotation;
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				rotation[axis] = ripper_cosine_interpolate(
					grab.target_rotation_start[axis],
					grab.target_rotation_end[axis],
					time);
			}
			target->previous_orientation = target->orientation;
			target->orientation = math::rotation_from_euler(rotation);
			update_beams(target->position, 1.0f);
			if (now_tick <= grab.stage_start_tick + 500u)
			{
				return true;
			}
			// AI_sequence_sync(6).
			advance();
			return true;
		}
		case 6:
		{
			const float time = std::min(
				1.0f,
				(static_cast<float>(now_tick - grab.stage_start_tick)
						/ 300.0f
					+ 1.0f)
					* 0.5f);
			target->previous_orientation = target->orientation;
			target->orientation =
				math::rotation_from_euler(grab.target_rotation_end);
			update_beams(
				glm::mix(grab.beam_start, grab.beam_end, time),
				1.0f);
			if (now_tick <= grab.stage_start_tick + 300u)
			{
				return true;
			}
			// AI_sequence_sync(7).
			advance();
			game::world_queue_sound_object(
				world, object_handle(world, *target), 0x3c, 0);
			if (!actor.model_references.empty())
			{
				game::model_animation_start_named(
					actor,
					0,
					"grab pod",
					0.0f,
					1,
					10.0f);
			}
			return true;
		}
		case 7:
			target->previous_orientation = target->orientation;
			target->orientation =
				math::rotation_from_euler(grab.target_rotation_end);
			update_beams(target->position, 1.0f);
			if (now_tick > grab.stage_start_tick + 150u)
			{
				advance();
				if (!actor.model_references.empty())
				{
					game::model_animation_start_named(
						actor,
						0,
						"cabin turn",
						0.0f,
						1,
						4.5f);
				}
			}
			return true;
		case 8:
			target->previous_orientation = target->orientation;
			target->orientation =
				math::rotation_from_euler(grab.target_rotation_end);
			update_beams(target->position, 1.0f);
			if (now_tick <= grab.stage_start_tick + 500u)
			{
				return true;
			}
			// AI_sequence_sync(9).
			advance();
			return true;
		case 9:
			ripper_set_cargo_node_hidden(*target, true);
			ripper_set_cargo_node_hidden(actor, false);
			actor.flight_callback_mode =
				game::FlightCallbackMode::standard_reverse;
			target->protection_state =
				grab.saved_target_capture_state;
			target->runtime_flags |= game::kObjectFlagDisabled;
			game::ripper_grab_effect_release(
				world.death_effects, grab.effect_slot);
			grab.effect_slot = -1;
			actor.runtime_flags &= ~game::kObjectFlagKinematic;
			mission::events_emit_ripper_grabbed_object(
				mission,
				actor.mission_index,
				target->mission_index);
			diagnostics::mission_log(
				"ripper grab complete actor=%u target=%u tick=%u",
				static_cast<unsigned>(actor.mission_index),
				static_cast<unsigned>(target->mission_index),
				tick);
			command_pop(world, actor);
			applies_flight = false;
			return false;
		default:
			abort_grab();
			return false;
		}
	}
	case 13:
	{
		// AI_ObjectAttach_update, LANCER.EXE 0x0040b4f0. Position and
		// orientation use the parent's retained previous pose, matching the
		// +0x84/+0x90 caches consumed by retail before ordinary integration.
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		if (target == nullptr)
		{
			command_pop(world, actor);
			return false;
		}
		const glm::vec3 attached_position =
			target->previous_position
			+ target->previous_orientation * work.vector;
		const glm::mat3 attached_orientation =
			target->previous_orientation;
		// Objects_set_all_position/orientation_states publishes both
		// retained snapshots to the same attached frame.
		actor.previous_position = attached_position;
		actor.position = attached_position;
		actor.previous_orientation = attached_orientation;
		actor.orientation = attached_orientation;
		game::world_set_scene_attachment(
			world,
			actor,
			*target,
			-1,
			work.vector,
			glm::mat3{1.0f});
		actor.inertial_angular_step = target->inertial_angular_step;
		actor.linear_velocity = target->linear_velocity;
		actor.speed = target->speed;
		actor.angular_x = target->angular_x;
		actor.angular_y = target->angular_y;
		actor.angular_z = target->angular_z;
		applies_flight = false;
		return true;
	}
	case 18:
		demand.throttle = 0.0f;
		demand.pitch = 0.0f;
		demand.roll = 0.0f;
		demand.yaw = 0.1f;
		return true;
	case 21:
	{
		if (work.stage == 0)
		{
			// AI_FindScoopUp publishes synchronization byte fifteen for
			// every selector, but only group/set selection waits for every
			// connected peer. A literal object advances immediately.
			if (!ai_sequence_sync(actor, world, mission, 15)
				&& command.selector != 0)
			{
				return true;
			}
			work.stage = 1;
			return true;
		}
		float nearest_distance = std::numeric_limits<float>::max();
		std::uint16_t nearest_object = UINT16_MAX;
		std::int16_t nearest_model = -1;
		const auto consider = [&](
			std::uint16_t object,
			std::int16_t model)
		{
			game::WorldObject* candidate =
				mission::runtime_resolve_object(
					mission, object, world);
			if (candidate == nullptr
				|| !target_reference_valid(
					*candidate, model, 0x800u))
			{
				return;
			}
			const glm::vec3 separation =
				actor.position - candidate->position;
			const float distance = glm::length(separation);
			if (distance < nearest_distance)
			{
				nearest_distance = distance;
				nearest_object = object;
				nearest_model = model;
			}
		};
		if (command.selector == 0)
		{
			consider(command.target, command.target_component);
		}
		else if (command.selector == 1 || command.selector == 2)
		{
			mission::ExpandedTargetReference targets[
				game::kMaxMissionObjects];
			const mission::ReferenceKind kind =
				command.selector == 1
					? mission::ReferenceKind::group
					: mission::ReferenceKind::set;
			const std::uint16_t count =
				mission::runtime_expand_target_reference(
					mission,
					kind,
					command.target,
					targets,
					static_cast<std::uint16_t>(
						std::size(targets)));
			for (std::uint16_t index = 0; index < count; ++index)
			{
				consider(
					targets[index].object,
					targets[index].model);
			}
		}
		if (nearest_object == UINT16_MAX)
		{
			command_pop(world, actor);
			return false;
		}
		if (command.selector != 0
			&& mission.network.role
				!= mission::NetworkRole::offline)
		{
			const std::uint16_t actor_index =
				object_handle(world, actor).index;
			const std::uint16_t local_player_index =
				world.player.index;
			if (!mission::network_local_owns_object(
				mission.network,
				actor_index,
				mission.network.player_count,
				local_player_index))
			{
				return true;
			}
			Command scoop;
			scoop.id = 107;
			scoop.selector = 0;
			scoop.target_kind = TargetKind::object;
			scoop.target = nearest_object;
			scoop.target_component = nearest_model;
			const std::uint8_t publication_seed =
				static_cast<std::uint8_t>(
					game::world_rand15(world));
			if (command_defer(
				actor,
				scoop,
				tick,
				0,
				publication_seed))
			{
				mission::network_publish_ai_deferred_command(
					mission.network,
					actor_index,
					scoop,
					0,
					publication_seed);
			}
			return true;
		}
		command_push(world,
			actor,
			107,
			TargetKind::object,
			nearest_object,
			nearest_model);
		return false;
	}
	case 22:
	case 23:
	case 24:
		// All three definitions share nullsub 0x004983a0. Begin already
		// published the retained control fields; update changes nothing.
		return true;
	case 27:
	{
		game::WorldObject* leader =
			resolve_target(actor, command, world, mission);
		if (leader == nullptr
			|| !target_reference_valid(
				*leader, command.target_component))
		{
			command_pop(world, actor);
			return false;
		}
		const glm::vec3 formation_point =
			leader->position + leader->orientation * work.vector;
		approach_point(
			actor,
			formation_point,
			leader->orientation,
			0.0f,
			world,
			stats,
			mission.frame_delta_ticks,
			demand);
		return true;
	}
	case 4:
	{
		WarpWork& warp = work.warp;
		const std::uint32_t elapsed_ticks =
			tick - warp.previous_tick;
		const float dt = transition_phase_delta(elapsed_ticks);
		warp.previous_tick = tick;
		game::WGateContext* context = game::wgate_context_get(
			world.transition_effects.wgate, warp.context);
		if (context == nullptr)
		{
			diagnostics::mission_log(
				"warp-in missing context actor=%u",
				static_cast<unsigned>(actor.mission_index));
			command_pop(world, actor);
			return false;
		}
		game::wgate_context_scroll_uv(
			*context, 3.5f * dt, -0.6f * dt);
		if (warp.state == 0)
		{
			if (warp.local_transition)
			{
				world.transition_effects.wgate.camera_context_index =
					warp.context;
				world.transition_effects.wgate.camera_target_position =
					warp.destination;
				world.transition_effects.wgate.camera_target_orientation =
					warp.destination_orientation;
				transition_request_camera(
					mission, world, 11, true, true);
			}
			transition_queue_actor_sound(
				world, actor, 28, warp.local_transition ? 2 : 0);
			const float distance =
				world.transition_effects.wgate.radial_segments == 16
					? -20000.0f
					: -15000.0f;
			actor.previous_position = actor.position;
			actor.position =
				warp.destination
					+ warp.destination_orientation
						* glm::vec3{
							0.0f,
							0.0f,
							distance};
			actor.previous_orientation = actor.orientation;
			actor.orientation = warp.destination_orientation;
			// AI_WarpIn_update state zero stores throttle one. State two
			// retains it until the tunnel phase reaches 0.4, then ordinary
			// ships switch to the compiled throttle value two.
			actor.control_demand.throttle = 1.0f;
			context->position =
				warp.destination
				+ (actor.type == 13
					? warp.destination_orientation[2] * 210000.0f
					: glm::vec3{0.0f});
			context->orientation = warp.destination_orientation;
			for (std::uint32_t row = 0;
				row <= context->axial_segments;
				++row)
			{
				// AI_WarpIn_update state zero writes the literal 0.05 to
				// every retained radius row (0x0041f5a0), not five percent
				// of either compiled WGate radius.
				context->row_radii[row] = 0.05f;
			}
			warp.state = 1;
			transition_log_stage(actor, "warp-in", 1, tick);
			applies_flight = false;
			return true;
		}
		if (warp.state == 1)
		{
			game::wgate_context_set_alpha(*context, 1.0f);
			actor.runtime_flags |= 0x00000004u;
			warp.phase = 0.0f;
			warp.state = 2;
			transition_log_stage(actor, "warp-in", 2, tick);
			applies_flight = false;
			return true;
		}
		if (warp.state == 2)
		{
			game::wgate_context_set_ship_phase(
				*context,
				game::WGateShipStage::warp_in_open,
				warp.phase);
			if (!warp.actor_revealed && warp.phase >= 0.4f)
			{
				actor.visible = true;
				warp.actor_revealed = true;
			}
			if (actor.type == 13 && warp.phase >= 0.4f)
			{
				const float speed =
					(actor.bounds_max.z - actor.bounds_min.z)
					+ context->axial_offset * 20.5f;
				const float multiplier =
					(1.0f
						- (warp.phase - 0.4f)
							* 1.6666666269302368f)
					* dt;
				actor.previous_position = actor.position;
				actor.position +=
					actor.orientation[2]
						* (speed * multiplier);
				actor.linear_velocity =
					actor.position - actor.previous_position;
				actor.speed = glm::length(actor.linear_velocity);
				applies_flight = false;
			}
			else
			{
				if (warp.phase >= 0.4f)
				{
					demand.throttle = 2.0f;
				}
				applies_flight = true;
			}
			warp.phase += 4.5f * dt;
			if (warp.phase >= 1.0f)
			{
				warp.phase = 0.0f;
				warp.state = 3;
				transition_log_stage(actor, "warp-in", 3, tick);
			}
			return true;
		}
		if (warp.state == 3)
		{
			game::wgate_context_set_ship_phase(
				*context,
				game::WGateShipStage::warp_in_delay,
				warp.phase);
			warp.phase += 5.0f * dt;
			if (warp.phase < 1.0f)
			{
				applies_flight = false;
				return true;
			}
			warp.state = 4;
			transition_log_stage(actor, "warp-in", 4, tick);
			applies_flight = false;
			return true;
		}
		actor.runtime_flags &= ~0x0000001cu;
		actor.visible = true;
		if (warp.local_transition)
		{
			world.transition_effects.wgate.warp_transition_active = false;
			world.transition_effects.wgate.camera_context_index = -1;
			transition_request_camera(
				mission, world, 0, false, true);
		}
		game::wgate_context_free(
			world.transition_effects.wgate, warp.context);
		const std::uint16_t actor_index =
			object_handle(world, actor).index;
		const std::uint16_t actor_mission_index =
			actor.mission_index;
		command_pop(world, actor);
		emit_jumped_in_completion(
			world,
			mission,
			actor_index,
			actor_mission_index);
		applies_flight = false;
		return false;
	}
	case 5:
	{
		WarpWork& warp = work.warp;
		const std::uint32_t elapsed_ticks =
			tick - warp.previous_tick;
		const float dt = transition_phase_delta(elapsed_ticks);
		warp.previous_tick = tick;
		game::WGateContext* context = game::wgate_context_get(
			world.transition_effects.wgate, warp.context);
		if (warp.state == 0)
		{
			if (warp.local_transition
				&& (actor.runtime_flags & 0x00000100u) != 0)
			{
				return true;
			}
			if (!warp.local_transition)
			{
				demand = steer_toward(
					actor,
					warp.destination,
					0.8f,
					1.0f,
					0.0f,
					0,
					nullptr,
					nullptr,
					mission.frame_delta_ticks);
				constexpr float kAlignmentLimit = 0.05f;
				if (std::abs(demand.roll) > kAlignmentLimit
					|| std::abs(demand.pitch) > kAlignmentLimit
					|| std::abs(demand.yaw) > kAlignmentLimit
					|| std::abs(actor.angular_x) > kAlignmentLimit
					|| std::abs(actor.angular_y) > kAlignmentLimit
					|| std::abs(actor.angular_z) > kAlignmentLimit)
				{
					return true;
				}
			}
			world.transition_effects.wgate.warp_transition_active =
				warp.local_transition;
			if (warp.local_transition)
			{
				world.transition_effects.wgate
					.warp_transition_owner_index =
						static_cast<std::uint16_t>(
							&actor - std::begin(world.objects));
				world.transition_effects.wgate
					.warp_transition_owner_generation =
						actor.generation;
			}
			const glm::vec3 corridor =
				warp.destination - actor.position;
			const float corridor_length = glm::length(corridor);
			transition_mark_corridor_dependents(
				world,
				actor,
				corridor_length > 0.0001f
					? actor.position
						+ corridor
							* (1000000.0f / corridor_length)
					: actor.position
						+ actor.orientation[2] * 1000000.0f);
			transition_queue_actor_sound(
				world, actor, 30, warp.local_transition ? 2 : 0);
			if (warp.local_transition && mission.game_mode != 9)
			{
				transition_request_camera(
					mission, world, 9, true, true);
			}
			warp.state = 1;
			transition_log_stage(actor, "warp-out", 1, tick);
			return true;
		}
		if (warp.state == 1)
		{
			warp.context = game::wgate_context_allocate(
				world,
				world.transition_effects.wgate,
				game::WGateMode::ship,
				object_handle(world, actor),
				actor.position,
				actor.orientation,
				tick);
			context = game::wgate_context_get(
				world.transition_effects.wgate, warp.context);
			if (context == nullptr)
			{
				command_pop(world, actor);
				return false;
			}
			world.transition_effects.wgate.camera_context_index =
				warp.context;
			warp.state = 2;
			transition_log_stage(actor, "warp-out", 2, tick);
			applies_flight = false;
			return true;
		}
		if (context == nullptr)
		{
			command_pop(world, actor);
			return false;
		}
		game::wgate_context_scroll_uv(
			*context, -3.5f * dt, -0.6f * dt);
		if (warp.state == 2)
		{
			warp.state = 3;
			warp.phase = 0.0f;
			transition_log_stage(actor, "warp-out", 3, tick);
			applies_flight = false;
			return true;
		}
		if (warp.state == 3)
		{
			game::wgate_context_set_ship_phase(
				*context,
				game::WGateShipStage::warp_out_open,
				warp.phase);
			warp.phase += 3.0f * dt;
			if (warp.phase <= 1.0f)
			{
				applies_flight = false;
				return true;
			}
			warp.phase = 0.0f;
			warp.state = 4;
			transition_log_stage(actor, "warp-out", 4, tick);
			applies_flight = false;
			return true;
		}
		if (warp.state == 4)
		{
			game::wgate_context_set_ship_phase(
				*context,
				game::WGateShipStage::warp_out_reverse,
				warp.phase);
			warp.phase += 4.0f * dt;
			if (warp.phase <= 1.0f)
			{
				applies_flight = false;
				return true;
			}
			transition_queue_actor_sound(
				world, actor, 27, warp.local_transition ? 2 : 0);
			if (warp.local_transition)
			{
				transition_request_camera(
					mission, world, 10, true, true);
			}
			actor.throttle = 2.0f;
			warp.phase = 0.0f;
			warp.state = 5;
			transition_log_stage(actor, "warp-out", 5, tick);
			applies_flight = false;
			return true;
		}
		if (warp.state == 5)
		{
			game::wgate_context_set_ship_phase(
				*context,
				game::WGateShipStage::warp_out_depart,
				warp.phase);
			if (!context->has_special_parameters
				&& (actor.runtime_flags & game::kObjectFlagCompound) == 0
				&& warp.phase > 0.1f
				&& warp.phase < 0.6f)
			{
				const float phase =
					(warp.phase - 0.1f) * 2.5f;
				// AI_WarpOut_update 0x0041f0eb-0x0041f13f
				// writes QuadraticLerp(1,5,phase) to the local-Z
				// diagonal of the actor's render basis only.
				game::retained_set_root_model_scale(
					actor,
					{1.0f, 1.0f, 1.0f + 4.0f * phase * phase});
			}
			else
			{
				game::retained_set_root_model_scale(
					actor, glm::vec3{1.0f});
			}
			actor.previous_position = actor.position;
			const float speed =
				(context->has_special_parameters
					? (actor.bounds_max.z - actor.bounds_min.z)
						+ context->axial_offset * 5.5f
					: 100000.0f
						+ context->axial_offset * 47.70000076293945f);
			actor.position += actor.orientation[2] * (speed * dt);
			actor.linear_velocity =
				actor.position - actor.previous_position;
			actor.speed = glm::length(actor.linear_velocity);
			warp.phase += 2.7f * dt;
			if (warp.phase < 1.0f)
			{
				applies_flight = false;
				return true;
			}
			warp.state = 6;
			transition_log_stage(actor, "warp-out", 6, tick);
			applies_flight = false;
			return true;
		}
		actor.position = warp.saved_position;
		actor.orientation = warp.saved_orientation;
		game::retained_set_root_model_scale(
			actor, glm::vec3{1.0f});
		actor.previous_position = actor.position;
		actor.previous_orientation = actor.orientation;
		actor.control_demand = {};
		actor.runtime_flags |= game::kObjectFlagRenderSuppressed;
		if (warp.local_transition)
		{
			world.transition_effects.wgate.warp_transition_active = false;
			world.transition_effects.wgate
				.warp_transition_owner_index = UINT16_MAX;
			world.transition_effects.wgate
				.warp_transition_owner_generation = 0;
		}
		const std::uint16_t target = command.target;
		const TargetKind target_kind = command.target_kind;
		const std::int16_t sequence = command.sequence;
		const std::int16_t context_index = warp.context;
		const bool local_transition = warp.local_transition;
		game::WorldObject* destination =
			resolve_target(actor, command, world, mission);
		command_pop(world, actor);
		if (destination != nullptr && destination != &actor)
		{
			command_push(world,
				actor,
				4,
				target_kind,
				target,
				-1,
				0,
				sequence);
		}
		else
		{
			game::wgate_context_free(
				world.transition_effects.wgate, context_index);
			if (local_transition)
			{
				transition_request_camera(
					mission, world, 0, false, true);
			}
		}
		applies_flight = false;
		return false;
	}
	case 19:
	case 40:
	{
		// AI_JumpIn_update, LANCER.EXE 0x00416570.
		JumpWork& jump = work.jump;
		const float dt = transition_phase_delta(
			tick - jump.previous_tick);
		jump.previous_tick = tick;
		game::JumpVisualContext* visual =
			game::jump_visual_get(
				world.transition_effects.jump,
				jump.visual_context);
		if (jump.state == 0)
		{
			jump.visual_context = game::jump_visual_allocate(
				world.transition_effects.jump,
				object_handle(world, actor));
			game::WorldObject* target =
				resolve_target(actor, command, world, mission);
			if (target != nullptr)
			{
				// JumpIn_initialize stores only the destination position.
				// State zero copies the target's live orientation.
				jump.destination_orientation = target->orientation;
			}
			actor.position =
				jump.destination
					+ jump.destination_orientation
						* glm::vec3{
							0.0f,
							0.0f,
							(actor.runtime_flags
								& game::kObjectFlagCompound) != 0
								? -100000.0f : -25000.0f};
			actor.orientation = jump.destination_orientation;
			// Objects_set_all_position/orientation_states publish the new
			// jump frame to every retained pose cache.
			actor.previous_position = actor.position;
			actor.previous_orientation = actor.orientation;
			game::world_zero_motion_controls(actor);
			actor.visible = true;
			game::jump_visual_build_in(
				world,
				jump.visual_context,
				actor,
				(actor.runtime_flags & game::kObjectFlagCompound) != 0);
			transition_queue_actor_sound(
				world, actor, 26, jump.local_transition ? 2 : 0);
			if (jump.local_transition)
			{
				const std::uint8_t mode =
					static_cast<std::uint8_t>(
						23
						+ std::clamp<int>(
							static_cast<int>(std::lrint(
								2.0f
								* static_cast<float>(
									game::world_rand15(world))
								* 0x1.0002p-15f)),
							0,
							2));
				transition_request_camera(
					mission, world, mode, true, true);
				mission::environment_state_commit(
					mission.environment, world);
				world.transition_effects.jump
					.environment_transition_active = true;
			}
			jump.phase = 0.0f;
			jump.transition_start_tick = tick;
			jump.state = 1;
			transition_log_stage(actor, "jump-in", 1, tick);
			applies_flight = false;
			return true;
		}
		visual = game::jump_visual_get(
			world.transition_effects.jump,
			jump.visual_context);
		if (visual == nullptr)
		{
			command_pop(world, actor);
			return false;
		}
		if (jump.state == 1)
		{
			game::jump_visual_set_trail_phase(
				*visual, jump.phase);
			// SR object +0x48 is the retained uniform scale. The authored
			// flare quad stays at the entry aperture while it grows. Retail
			// reads GameObject+0x5ac/+0x5a0: the aggregate X extent.
			visual->flare.scale =
				(actor.bounds_max.x - actor.bounds_min.x) * jump.phase;
			visual->flare.active = true;
			jump.phase += 50.0f * dt;
			if (jump.phase > 1.0f)
			{
				jump.saved_flight_callback =
					static_cast<std::uint8_t>(
						actor.flight_callback_mode);
				actor.flight_callback_mode =
					game::FlightCallbackMode::jump_transition;
				actor.runtime_flags &= ~game::kObjectFlagSimulationSuspended;
				jump.phase = 0.0f;
				jump.state = 2;
				transition_log_stage(actor, "jump-in", 2, tick);
			}
			applies_flight = jump.state == 2;
			return true;
		}
		if (jump.state == 2)
		{
			const float remaining = 1.0f - jump.phase;
			game::jump_visual_set_trail_phase(
				*visual, remaining);
			game::jump_visual_set_burst_phase(
				visual->jump_in_burst, remaining, remaining);
			if (jump.local_transition)
			{
				// AI_JumpIn_update 0x00416b25..0x00416b37 writes
				// LinearLerp(1, 0, phase) directly to the common camera
				// disturbance scalar.
				world.player_camera_disturbance = remaining;
			}
			jump.phase += 3.0f * dt;
			if (jump.phase < 0.3f)
			{
				glm::mat3 deformation{1.0f};
				deformation[0][0] = 1.0f + 3.0f * jump.phase;
				deformation[1][1] =
					1.0f - 3.3333333f * jump.phase + 0.000001f;
				visual->flare.scale =
					actor.bounds_max.x - actor.bounds_min.x;
				visual->flare.orientation =
					visual->jump_in_basis * deformation;
				visual->flare.active = true;
			}
			else
			{
				visual->flare.active = false;
			}
			if (jump.phase <= 1.0f)
			{
				// Flight_update_jump_transition is the installed fixed-rate
				// flight callback; this rendered-frame owner only advances
				// the accompanying visual state.
				applies_flight = true;
				return true;
			}
			actor.flight_callback_mode =
				static_cast<game::FlightCallbackMode>(
					jump.saved_flight_callback);
			actor.throttle = 1.0f;
			demand.throttle = 1.0f;
			actor.runtime_flags &= ~0x0000001cu;
			jump.phase = 0.0f;
			jump.deadline = tick + 200;
			jump.state = 3;
			world.transition_effects.jump
				.environment_transition_active = false;
			transition_log_stage(actor, "jump-in", 3, tick);
			// Retail exits through the visual-submission tail here. The
			// Command-40 spread maneuver (or Command-19 cleanup) starts on
			// the following update, after one ordinary-flight step at
			// throttle one.
			return true;
		}
		if (command.id == 40 && jump.deadline > tick)
		{
			const std::int32_t ordinal = command.sequence + 1;
			const std::int32_t lane =
				(2 * (ordinal & 1) - 1) * (ordinal / 2);
			demand.pitch =
				static_cast<float>(ordinal / 2) * 0.5f;
			demand.roll = static_cast<float>(lane) * 0.5f;
			return true;
		}
		if (jump.local_transition)
		{
			transition_request_camera(
				mission, world, 0, false, true);
			world.transition_effects.jump.transition_owner_index =
				UINT16_MAX;
			world.transition_effects.jump.transition_owner_generation =
				0;
		}
		game::jump_visual_free(
			world.transition_effects.jump,
			jump.visual_context);
		const std::uint16_t actor_index =
			object_handle(world, actor).index;
		const std::uint16_t actor_mission_index =
			actor.mission_index;
		command_pop(world, actor);
		emit_jumped_in_completion(
			world,
			mission,
			actor_index,
			actor_mission_index);
		return false;
	}
	case 20:
	case 41:
	{
		JumpWork& jump = work.jump;
		const float dt = transition_phase_delta(
			tick - jump.previous_tick);
		jump.previous_tick = tick;
		game::JumpEffectsRuntime& effects =
			world.transition_effects.jump;
		if (effects.overlay_active
			&& effects.overlay_countdown != 0
			&& effects.overlay_next_tick < tick)
		{
			--effects.overlay_countdown;
			effects.overlay_next_tick = tick + 10;
		}
		if (jump.local_transition)
		{
			mission.session_state[0] = 0;
		}
		game::JumpVisualContext* visual =
			game::jump_visual_get(
				world.transition_effects.jump,
				jump.visual_context);
		if (jump.state == 0)
		{
			if (jump.local_transition)
			{
				world.transition_effects.jump.overlay_active = true;
			}
			if (jump.synchronized)
			{
				if (tick - work.entered_tick < 100)
				{
					return true;
				}
			}
			else
			{
				demand = steer_toward(
					actor,
					jump.destination,
					0.8f,
					1.0f,
					0.0f,
					0,
					nullptr,
					nullptr,
					mission.frame_delta_ticks);
				// GameObject+0x74c is the retained 3-D sound slot. Its
				// ordinary -1 value keeps the six settling thresholds
				// active after the first second; only literal zero lets
				// the timeout alone release this state.
				if ((tick - work.entered_tick < 1000
						|| actor.sound3d_slot != 0)
					&& (std::abs(actor.angular_x) > 0.05f
						|| std::abs(actor.angular_y) > 0.05f
						|| std::abs(actor.angular_z) > 0.05f
						|| std::abs(demand.pitch) > 0.02f
						|| std::abs(demand.roll) > 0.02f
						|| std::abs(demand.yaw) > 0.02f))
				{
					return true;
				}
			}
			jump.visual_context = game::jump_visual_allocate(
				world.transition_effects.jump,
				object_handle(world, actor));
			transition_queue_actor_sound(
				world, actor, 25, jump.local_transition ? 2 : 0);
			jump.deadline = tick;
			jump.state = 1;
			transition_log_stage(actor, "jump-out", 1, tick);
			return true;
		}
		visual = game::jump_visual_get(
			world.transition_effects.jump,
			jump.visual_context);
		if (visual == nullptr)
		{
			command_pop(world, actor);
			return false;
		}
		if (jump.state == 1)
		{
			actor.control_demand.roll = 0.0f;
			actor.control_demand.pitch = 0.0f;
			actor.control_demand.yaw = 0.0f;
			actor.control_demand.strafe = 0.0f;
			demand.roll = 0.0f;
			demand.pitch = 0.0f;
			demand.yaw = 0.0f;
			demand.strafe = 0.0f;
			actor.angular_x = 0.0f;
			actor.angular_y = 0.0f;
			actor.angular_z = 0.0f;
			actor.inertial_angular_step = glm::mat3{1.0f};
			actor.speed = 0.0f;
			if (jump.deadline >= tick)
			{
				return true;
			}
			jump.saved_orientation = actor.orientation;
			const glm::vec3 corridor =
				jump.destination - actor.position;
			const float corridor_length = glm::length(corridor);
			jump.corridor_endpoint =
				corridor_length > 0.0001f
					? actor.position
						+ corridor
							* (500000.0f / corridor_length)
					: actor.position
						+ actor.orientation[2] * 500000.0f;
			game::jump_visual_build_out(
				world,
				jump.visual_context,
				actor,
				(actor.runtime_flags & game::kObjectFlagCompound) != 0,
				jump.corridor_endpoint);
			if (actor.type == 0xa8u)
			{
				// The Boridin breakaway core owns the long-lived retained
				// power-core BMO/emitter which retail tears down here.
				game::powercore_effect_release(
					world, object_handle(world, actor));
			}
			jump.phase = 0.0f;
			jump.state = 2;
			transition_log_stage(actor, "jump-out", 2, tick);
			return true;
		}
		if (jump.state == 2)
		{
			if (jump.phase > 1.0f)
			{
				jump.saved_flight_callback =
					static_cast<std::uint8_t>(
						actor.flight_callback_mode);
				actor.flight_callback_mode =
					game::FlightCallbackMode::follow_quadratic_curve;
				actor.runtime_flags |= 0x00000004u;
				jump.phase = 0.0f;
				jump.deadline = tick;
				jump.hidden_position = actor.position;
				transition_set_jump_high_detail_override(actor, true);
				for (std::uint8_t index = 0;
					index < visual->light_count;
					++index)
				{
					visual->lights[index].half_extent = {};
				}
				jump.state = 3;
				transition_log_stage(actor, "jump-out", 3, tick);
				applies_flight = true;
				return true;
			}
			game::jump_visual_set_trail_phase(
				*visual, jump.phase);
			if (jump.phase < 0.8f)
			{
				const float scale = 1.25f * jump.phase * 150.0f;
				for (std::uint8_t index = 0;
					index < visual->light_count;
					++index)
				{
					visual->lights[index].half_extent =
						glm::vec2{scale};
				}
			}
			else if (visual->light_count != 0)
			{
				const std::int32_t selected =
					std::clamp<std::int32_t>(
						static_cast<std::int32_t>(
							std::lrint(
								static_cast<float>(
									visual->light_count - 1)
								* (jump.phase - 0.8f) * 5.0f)),
						0,
						visual->light_count - 1);
				visual->lights[selected].texture =
					game::TransitionTexture::jump_light_bright;
				if (selected >= 2)
				{
					visual->lights[selected - 2].half_extent = {};
				}
			}
			jump.phase += 6.0f * dt;
			return true;
		}
		if (jump.state == 3)
		{
			// Retail compares the 100 Hz gameplay clock against
			// start+250 with a strict greater-than test.
			if (jump.deadline + 250 < tick)
			{
				// The retail flare is an independent retained mesh at the
				// exit aperture, not another child of the departing ship.
				visual->flare.position = actor.position;
				visual->flare.orientation = actor.orientation;
				visual->flare.scale =
					actor.bounds_max.x - actor.bounds_min.x;
				visual->flare.active = true;
				actor.flight_callback_mode =
					game::FlightCallbackMode::standard_forward;
				actor.runtime_flags |= game::kObjectFlagSimulationSuspended;
				jump.phase = 0.0f;
				jump.state = 4;
				transition_log_stage(actor, "jump-out", 4, tick);
				return true;
			}
			const float fade =
				std::max(0.0f, 1.0f - 6.0f * jump.phase);
			game::jump_visual_set_out_burst_phase(
				visual->jump_out_burst, fade);
			game::jump_visual_set_trail_phase(
				*visual, 1.0f - jump.phase);
			jump.phase += 4.0f * dt;
			// The quadratic callback samples this same 100 Hz clock when
			// the fixed 25 Hz flight service invokes it.
			applies_flight = true;
			return true;
		}
		if (jump.state == 4)
		{
			visual->flare.scale =
				(1.0f - jump.phase)
					* (actor.bounds_max.x - actor.bounds_min.x);
			jump.phase += 10.0f * dt;
			if (jump.phase < 1.0f)
			{
				return true;
			}
			actor.runtime_flags &= ~0x00000004u;
			jump.state = 5;
			transition_log_stage(actor, "jump-out", 5, tick);
			return true;
		}
		actor.orientation = jump.saved_orientation;
		actor.previous_orientation = actor.orientation;
		actor.flight_callback_mode =
			static_cast<game::FlightCallbackMode>(
				jump.saved_flight_callback);
		actor.runtime_flags &= ~0x00000018u;
		transition_set_jump_high_detail_override(actor, false);
		game::jump_visual_free(
			world.transition_effects.jump,
			jump.visual_context);
		if (jump.local_transition)
		{
			world.transition_effects.jump.overlay_active = false;
			world.transition_effects.jump.transition_owner_index =
				UINT16_MAX;
			world.transition_effects.jump.transition_owner_generation =
				0;
			for (game::WorldObject& object : world.objects)
			{
				object.runtime_flags &= ~game::kObjectFlagSimulationSuspended;
			}
		}
		game::WorldObject* destination =
			resolve_target(actor, command, world, mission);
		const std::uint16_t target = command.target;
		const TargetKind target_kind = command.target_kind;
		const std::int16_t sequence = command.sequence;
		const std::int16_t jump_in =
			command.id == 20 ? 19 : 40;
		const glm::vec3 direct_destination = jump.destination;
		command_pop(world, actor);
		if (destination != nullptr && destination != &actor)
		{
			command_push(world,
				actor,
				jump_in,
				target_kind,
				target,
				-1,
				0,
				sequence);
		}
		else
		{
			actor.position = direct_destination;
			actor.position.y = -9900000.0f;
			actor.previous_position = actor.position;
			actor.runtime_flags &= ~game::kObjectFlagSimulationSuspended;
			const bool protected_local_actor =
				jump.local_transition
				&& (actor.runtime_flags & 0x10000000u) != 0;
			if (!protected_local_actor)
			{
				actor.runtime_flags |= game::kObjectFlagDisabled;
			}
		}
		applies_flight = false;
		return false;
	}
	case 25:
	{
		FixedGateWork& fixed = work.fixed_gate;
		const float dt = transition_phase_delta(
			tick - fixed.previous_tick);
		fixed.previous_tick = tick;
		game::WGateContext* context = game::wgate_context_get(
			world.transition_effects.wgate, fixed.context);
		if (context == nullptr)
		{
			command_pop(world, actor);
			return false;
		}
		// FixedGateJumpIn publishes orientation and position states
		// directly in every active stage; the ordinary flight integrator
		// must not apply a second transform afterward.
		applies_flight = false;
		if (fixed.state == 0)
		{
			if (context->fixed_gate_lock && !fixed.local_transition)
			{
				return true;
			}
			context->fixed_gate_lock = true;
			fixed.owns_context_lock = true;
			transition_set_fixed_quad_half_extent(
				context->fixed_quads[0], 12500.0f);
			transition_set_fixed_quad_half_extent(
				context->fixed_quads[1], 0.001f);
			actor.previous_position = fixed.start_position;
			actor.position = fixed.start_position;
			transition_queue_actor_sound(world, actor, 28, 0);
			fixed.phase = 0.0f;
			fixed.state = 1;
			transition_log_stage(actor, "fixed-gate-in", 1, tick);
			return true;
		}
		if (fixed.state == 1)
		{
			const bool slow_krasny =
				actor.type == 0x9au
				&& (mission.game_mode == 0x42u
					|| mission.game_mode == 0x10u);
			fixed.phase += (slow_krasny ? 0.13f : 2.0f) * dt;
			actor.position = glm::mix(
				fixed.start_position,
				fixed.end_position,
				fixed.phase);
			// GameObject_set_all_position_states (0x0049b600) publishes
			// the same point to every retained state; it does not derive a
			// velocity from the scripted displacement.
			actor.previous_position = actor.position;
			actor.transform_state_flags |=
				game::kObjectTransformAllStatesSynchronized;
			if (fixed.phase > 1.0f)
			{
				fixed.phase = 0.0f;
				fixed.state = 2;
			}
			else if (fixed.phase > 0.5f
				&& fixed.owns_context_lock)
			{
				context->fixed_gate_lock = false;
				fixed.owns_context_lock = false;
			}
			if (!world.transition_effects.wgate
					.fixed_gate_transition_active
				&& fixed.phase < 0.2f)
			{
				const float quad_phase = fixed.phase * 5.0f;
				const float quadratic =
					quad_phase * quad_phase;
				transition_set_fixed_quad_half_extent(
					context->fixed_quads[0],
					12500.0f
						+ (0.001f - 12500.0f) * quadratic);
				transition_set_fixed_quad_half_extent(
					context->fixed_quads[1],
					0.001f
						+ (12500.0f - 0.001f) * quadratic);
			}
			if (actor.type == 0x9au && fixed.phase > 0.33f)
			{
				if (world.transition_effects.wgate
						.krasny_split_ready)
				{
					const std::uint16_t saved_target = command.target;
					const game::WorldObject* saved_target_object =
						resolve_target(
							actor, command, world, mission);
					actor.runtime_flags &= ~0x0000001cu;
					transition_set_type3_visibility(actor, true);
					context->fixed_gate_lock = false;
					game::wgate_context_unregister_deformation(
						world.transition_effects.wgate,
						static_cast<std::uint16_t>(
							&actor - std::begin(world.objects)));
					mission::events_emit_fixed_gate_jumped_in(
						mission,
						actor.mission_index,
						saved_target);
					command_pop(world, actor);
					actor.runtime_flags |= 0x00000048u;
					for (game::ObjectModelReference& model
						: actor.model_references)
					{
						if (model.removed
							|| model.parent_reference >= 0)
						{
							continue;
						}
						if (std::strcmp(
								model.name,
								"bad front slice")
							== 0)
						{
							model.runtime_flags &= ~0x20u;
						}
						else
						{
							model.runtime_flags |= 0x20u;
						}
					}
					actor.runtime_flags &= ~0x00004000u;
					for (game::WorldObject& gate : world.objects)
					{
						if (gate.active && gate.type == 0x6eu)
						{
							gate.ai.work.fixed_gate.state = 2;
							gate.ai.work.fixed_gate.phase = 0.0f;
						}
					}
					transition_emit_krasny_split_visuals(
						world, actor, tick);
					// GameObject+0x694 is the retained attacker/target
					// slot consumed by AI retaliation. Krasny_split saves
					// the gate target there before clearing all commands.
					actor.last_attacker_index =
						saved_target_object == nullptr
							? UINT16_MAX
							: static_cast<std::uint16_t>(
								saved_target_object
								- std::begin(world.objects));
					command_clear(world, actor);
					actor.runtime_flags |= game::kObjectFlagDestroyed;
					world.transition_effects.wgate
						.krasny_split_ready = false;
					applies_flight = false;
					return false;
				}
				else
				{
					// The shared warp-projector completion latch is cleared
					// on every post-threshold update when the scripted split
					// is not armed.
					world.transition_effects.wgate
						.krasny_gate_effect_active = false;
				}
			}
			if (fixed.state == 1)
			{
				applies_flight = false;
				return true;
			}
			transition_log_stage(actor, "fixed-gate-in", 2, tick);
			applies_flight = false;
			return true;
		}
		actor.runtime_flags &= ~0x0000001cu;
		actor.control_demand = {};
		actor.visible = true;
		transition_set_type3_visibility(actor, true);
		actor.targetable =
			actor.type < assets::kShipStatsCount
			&& stats.records[actor.type]
				.object.targetable_capability;
		if (actor.targetable)
		{
			actor.runtime_flags |= 0x00000200u;
		}
		context->fixed_gate_lock = false;
		game::wgate_context_unregister_deformation(
			world.transition_effects.wgate,
			static_cast<std::uint16_t>(
				&actor - std::begin(world.objects)));
		mission::events_emit_fixed_gate_jumped_in(
			mission,
			actor.mission_index,
			command.target);
		command_pop(world, actor);
		applies_flight = false;
		return false;
	}
	case 26:
	{
		FixedGateWork& fixed = work.fixed_gate;
		const float dt = transition_phase_delta(
			tick - fixed.previous_tick);
		fixed.previous_tick = tick;
		game::WGateContext* context = game::wgate_context_get(
			world.transition_effects.wgate, fixed.context);
		if (context == nullptr)
		{
			command_pop(world, actor);
			return false;
		}
		// FixedGateJumpOut publishes orientation and position states
		// directly in every active stage; the ordinary flight integrator
		// must not apply a second transform afterward.
		applies_flight = false;
		if (fixed.state == 0)
		{
			if (world.transition_effects.wgate.fixed_departure_lock)
			{
				return true;
			}
			world.transition_effects.wgate.fixed_departure_lock = true;
			world.transition_effects.wgate
				.fixed_departure_owner_index =
					static_cast<std::uint16_t>(
						&actor - std::begin(world.objects));
			world.transition_effects.wgate
				.fixed_departure_owner_generation =
					actor.generation;
			fixed.owns_context_lock = true;
			fixed.phase = 0.0f;
			fixed.state = 1;
			transition_log_stage(actor, "fixed-gate-out", 1, tick);
			return true;
		}
		if (fixed.state == 1)
		{
			const glm::vec3 current_euler =
				math::rotation_to_euler(actor.orientation);
			const glm::vec3 gate_euler =
				math::rotation_to_euler(fixed.gate_orientation);
			actor.orientation = math::rotation_from_euler(
				glm::mix(
					current_euler,
					gate_euler,
					fixed.phase));
			actor.previous_orientation = actor.orientation;
			actor.position = glm::mix(
				actor.position,
				fixed.end_position,
				fixed.phase);
			actor.previous_position = actor.position;
			actor.transform_state_flags |=
				game::kObjectTransformAllStatesSynchronized;
			fixed.phase += 0.7f * dt;
			const float distance =
				glm::distance(actor.position, fixed.end_position);
			if (distance >= 400.0f)
			{
				return true;
			}
			const std::uint16_t actor_index =
				static_cast<std::uint16_t>(
					&actor - std::begin(world.objects));
			actor.position = {
				static_cast<float>(actor_index) * 1000000.0f,
				0.0f,
				-25000000.0f,
			};
			actor.previous_position = actor.position;
			if (fixed.local_transition)
			{
				world.transition_effects.wgate
					.fixed_gate_transition_active = true;
				world.transition_effects.wgate
					.fixed_transition_owner_index =
						static_cast<std::uint16_t>(
							&actor - std::begin(world.objects));
				world.transition_effects.wgate
					.fixed_transition_owner_generation =
						actor.generation;
				world.transition_effects.wgate
					.fixed_tunnel_camera_active = true;
				world.transition_effects.wgate.wormhole.position =
					actor.position;
				world.transition_effects.wgate.wormhole.orientation =
					actor.orientation;
				world.transition_effects.wgate.wormhole.active = true;
				world.transition_effects.wgate.wormhole_active = true;
				world.player_exhaust_exposure_percent = 100;
			}
			fixed.phase = 0.0f;
			fixed.state = 2;
			transition_log_stage(actor, "fixed-gate-out", 2, tick);
			applies_flight = false;
			return true;
		}
		if (fixed.state == 2)
		{
			if (fixed.local_transition)
			{
				game::wgate_wormhole_animate(
					world.transition_effects.wgate,
					tick,
					static_cast<std::uint32_t>(
						std::max(
							0.0f,
							std::round(dt * 1000.0f))));
				if (game::world_rand15(world)
					< static_cast<std::uint16_t>(
						0.025f * 32768.0f))
				{
					// AI_FixedGateJumpOut_update
					// 0x004217b9..0x004217ca publishes both globals
					// consumed by Camera_update_frame and the additive
					// whiteout overlay.
					world.player_camera_disturbance = 3.0f;
					world.player_exhaust_exposure_percent = 100;
					transition_queue_actor_sound(
						world, actor, 10, 0);
				}
			}
			fixed.phase += 2.5f * dt;
			if (fixed.phase <= 1.0f)
			{
				applies_flight = false;
				return true;
			}
			fixed.state = 3;
			transition_log_stage(actor, "fixed-gate-out", 3, tick);
			applies_flight = false;
			return true;
		}
		actor.runtime_flags &= ~0x0000001cu;
		world.transition_effects.wgate.fixed_departure_lock = false;
		world.transition_effects.wgate.fixed_departure_owner_index =
			UINT16_MAX;
		world.transition_effects.wgate.fixed_departure_owner_generation =
			0;
		game::wgate_context_unregister_deformation(
			world.transition_effects.wgate,
			static_cast<std::uint16_t>(
				&actor - std::begin(world.objects)));
		if (fixed.local_transition)
		{
			// Completion repeats the same retail shake/whiteout at
			// 0x00421829..0x0042183d.
			world.player_exhaust_exposure_percent = 100;
			world.player_camera_disturbance = 3.0f;
			world.transition_effects.wgate
				.fixed_gate_transition_active = false;
			world.transition_effects.wgate
				.fixed_transition_owner_index = UINT16_MAX;
			world.transition_effects.wgate
				.fixed_transition_owner_generation = 0;
			world.transition_effects.wgate
				.fixed_tunnel_camera_active = false;
			world.transition_effects.wgate.wormhole.active = false;
			world.transition_effects.wgate.wormhole_active = false;
		}
		const std::uint16_t target = command.target;
		command_pop(world, actor);
		command_push(world,
			actor,
			25,
			TargetKind::object,
			target,
			-1,
			0,
			0);
		applies_flight = false;
		return false;
	}
	case 28:
	{
		FixedGateWork& fixed = work.fixed_gate;
		if (game::WorldObject* target =
				resolve_target(actor, command, world, mission))
		{
			const std::int16_t resolved =
				game::wgate_context_find_owner(
					world.transition_effects.wgate,
					static_cast<std::uint16_t>(
						target - std::begin(world.objects)));
			if (resolved >= 0)
			{
				fixed.context = resolved;
			}
		}
		game::WGateContext* context = game::wgate_context_get(
			world.transition_effects.wgate, fixed.context);
		if (context == nullptr)
		{
			command_pop(world, actor);
			return false;
		}
		if (fixed.state == 1)
		{
			command_pop(world, actor);
			return false;
		}
		const float dt = transition_phase_delta(
			tick - context->previous_tick);
		context->previous_tick = tick;
		context->phase += 9.0f * dt;
		const float eased =
			0.5f
				- 0.5f
					* std::cos(
						context->phase
							* 3.14159265358979323846f);
		context->portal.scale =
			0.0001f + (1.0f - 0.0001f) * eased;
		if (context->phase > 1.0f)
		{
			context->phase = 0.0f;
			fixed.state = 1;
		}
		return true;
	}
	case 29:
	{
		FixedGateWork& fixed = work.fixed_gate;
		if (game::WorldObject* target =
				resolve_target(actor, command, world, mission))
		{
			fixed.context = game::wgate_context_find_owner(
				world.transition_effects.wgate,
				static_cast<std::uint16_t>(
					target - std::begin(world.objects)));
		}
		game::WGateContext* context = game::wgate_context_get(
			world.transition_effects.wgate, fixed.context);
		if (context == nullptr)
		{
			command_pop(world, actor);
			return false;
		}
		if (fixed.state == 1)
		{
			game::wgate_context_free(
				world.transition_effects.wgate, fixed.context);
			command_pop(world, actor);
			return false;
		}
		const float dt = transition_phase_delta(
			tick - context->previous_tick);
		context->previous_tick = tick;
		context->phase += 9.0f * dt;
		const float eased =
			0.5f
				- 0.5f
					* std::cos(
						context->phase
						* 3.14159265358979323846f);
		context->portal.scale =
			1.0f + (0.0001f - 1.0f) * eased;
		if (context->phase > 1.0f)
		{
			context->phase = 0.0f;
			fixed.state = 1;
		}
		return true;
	}
	case 31:
	{
		FixedGateWork& fixed = work.fixed_gate;
		fixed.context = game::wgate_context_find_owner(
			world.transition_effects.wgate,
			static_cast<std::uint16_t>(
				&actor - std::begin(world.objects)));
		game::WGateContext* context = game::wgate_context_get(
			world.transition_effects.wgate, fixed.context);
		if (context == nullptr)
		{
			diagnostics::mission_log(
				"fixed-gate-collapse missing context actor=%u tick=%u",
				static_cast<unsigned>(actor.mission_index),
				tick);
			command_pop(world, actor);
			return false;
		}
		const float dt = transition_phase_delta(
			tick - context->previous_tick);
		context->previous_tick = tick;
		if (fixed.state == 0)
		{
			fixed.phase += dt;
			const std::uint16_t desired = static_cast<std::uint16_t>(
				std::max(0.0f, fixed.phase) * 55.0f);
			// Retail advances at most one authored type-2 point per update.
			if (fixed.explosion_count < desired)
			{
				const float explosion_size =
					5500.0f
					+ static_cast<float>(
						game::world_rand15(world))
						/ 32767.0f
						* 3000.0f;
				transition_emit_gate_explosion(
					world,
					actor,
					fixed.explosion_count,
					explosion_size,
					((fixed.explosion_count + 1u) % 7u) == 0,
					tick);
				++fixed.explosion_count;
			}
			// Model_set_all_lights_enabled_recursive, 0x00422680. This is
			// the collapsing gate's irregular electrical flicker, not model
			// visibility. The random comparison is the compiled 0.3.
			game::retained_set_all_model_lights(
				actor,
				static_cast<float>(game::world_rand15(world))
						/ 32767.0f
					>= 0.3f);
			if (fixed.phase < 1.0f)
			{
				return true;
			}
			game::retained_set_all_model_lights(actor, false);
			world.player_exhaust_exposure_percent = 100;
			fixed.phase = 0.0f;
			fixed.state = 1;
			transition_log_stage(
				actor, "fixed-gate-collapse", 1, tick);
			return true;
		}
		if (fixed.state == 1)
		{
			if (fixed.phase < 0.2f)
			{
				const float scale_phase =
					std::clamp(fixed.phase * 5.0f, 0.0f, 1.0f);
				if (actor.type == 0x6eu)
				{
					game::retained_set_named_model_scale(
						actor,
						"InnerRing",
						glm::mix(4.0f, 0.0f, scale_phase));
					game::retained_set_named_model_scale(
						actor,
						"Tube11",
						glm::mix(1.0f, 0.0f, scale_phase));
				}
				else if (actor.type == 0x6du)
				{
					game::retained_set_named_model_scale(
						actor,
						"forcering",
						glm::mix(1.0f, 0.0f, scale_phase));
				}
			}
			// State one has an independent five-percent explosion chance.
			// The second random draw selects one of the 55 authored points;
			// the third is the exact 3500..4500 retail size draw.
			if (static_cast<float>(game::world_rand15(world))
					/ 32767.0f
				< 0.05f)
			{
				const std::uint16_t ordinal =
					static_cast<std::uint16_t>(
						static_cast<float>(game::world_rand15(world))
							/ 32767.0f
							* 55.0f);
				const float explosion_size =
					3500.0f
					+ static_cast<float>(
						game::world_rand15(world))
						/ 32767.0f
						* 1000.0f;
				transition_emit_gate_explosion(
					world,
					actor,
					ordinal,
					explosion_size,
					false,
					tick);
			}
			game::wgate_context_set_collapse_wave(
				*context,
				actor.type == 0x6du
					? fixed.phase
					: fixed.phase * 2.0f);
			fixed.phase +=
				(actor.type == 0x6du ? 0.5f : 0.25f) * dt;
			actor.runtime_flags |= 0x00000008u;
			const float shake_x =
				(static_cast<float>(game::world_rand15(world))
						/ 32767.0f
					- 0.5f)
					< 0.0f
				? -9.0f : 9.0f;
			const float shake_y =
				(static_cast<float>(game::world_rand15(world))
						/ 32767.0f
					- 0.5f)
					< 0.0f
				? -9.0f : 9.0f;
			const float shake_z =
				(static_cast<float>(game::world_rand15(world))
						/ 32767.0f
					- 0.5f)
					< 0.0f
				? -9.0f : 9.0f;
			actor.position += glm::vec3{shake_x, shake_y, shake_z};
			actor.previous_position = actor.position;
			if (fixed.phase < 1.0f
				|| world.transition_effects.wgate
					.krasny_split_ready)
			{
				return true;
			}
			fixed.phase = 0.0f;
			fixed.state = 2;
			transition_log_stage(
				actor, "fixed-gate-collapse", 2, tick);
			return true;
		}
		if (fixed.state == 2)
		{
			game::wgate_context_set_collapse_front(
				*context, fixed.phase);
			fixed.phase += 6.0f * dt;
			if (fixed.phase < 1.0f)
			{
				return true;
			}
			fixed.state = 3;
			transition_log_stage(
				actor, "fixed-gate-collapse", 3, tick);
			return true;
		}
		if (actor.type != 0x6du)
		{
			game::wgate_context_free(
				world.transition_effects.wgate, fixed.context);
		}
		if (actor.type == 0x6eu)
		{
			// GameObject_destroy_scripted (0x00401f00) asks command 11 to
			// supersede the active command, then writes the queue count
			// directly to zero. Command 31 has no end callback, and queued
			// tail records are deliberately left stale.
			actor.ai.command_count = 0;
			actor.runtime_flags |= game::kObjectFlagDestroyed;
			if (actor.mission_index < mission.object_count)
			{
				mission::events_emit_destroyed(
					mission,
					actor.mission_index,
					last_attacker_mission_index(
						world, actor));
			}
		}
		if (actor.type == 0x6du || actor.type == 0x6eu)
		{
			game::retained_set_named_model_hidden(
				actor, "forcefield", true);
		}
		diagnostics::mission_log(
			"fixed-gate-collapse complete actor=%u tick=%u",
			static_cast<unsigned>(actor.mission_index),
			tick);
		command_pop(world, actor);
		return false;
	}
	case 38:
	{
		WarpProjectorWork& projector = work.warp_projector;
		if (!projector.initialized)
		{
			command_pop(world, actor);
			return false;
		}
		const std::uint32_t elapsed = tick - projector.previous_tick;
		projector.previous_tick = tick;
		const game::ObjectModelReference* break_off =
			game::retained_find_named_model(
				actor, "Bor break off section");
		const game::ObjectModelReference* projector_model =
			game::retained_find_named_model(
				actor, "Bor brkawy proj ");
		if (break_off == nullptr || projector_model == nullptr
			|| projector_model->point_groups == nullptr)
		{
			return true;
		}
		const assets::GameplayPointGroup* anchors = nullptr;
		for (const assets::GameplayPointGroup& group
			: *projector_model->point_groups)
		{
			if (group.type == 10)
			{
				anchors = &group;
				break;
			}
		}
		if (anchors == nullptr || anchors->points.size() < 6u)
		{
			return true;
		}
		// Retail concatenates both named model chains before reading the
		// projector frame and the six consecutive type-10 point records.
		const std::uint16_t break_off_reference =
			static_cast<std::uint16_t>(
				break_off - actor.model_references.data());
		(void)object_model_world_transform(
			actor, break_off_reference);
		const std::uint16_t projector_reference =
			static_cast<std::uint16_t>(
				projector_model - actor.model_references.data());
		const glm::mat4 projector_frame =
			object_model_world_transform(actor, projector_reference);
		std::array<glm::vec3, 6> anchor_positions{};
		for (std::size_t index = 0;
			index < anchor_positions.size();
			++index)
		{
			anchor_positions[index] = glm::vec3(
				projector_frame
					* glm::vec4(
						anchors->points[index].position, 1.0f));
		}
		game::wgate_projectors_update(
			world,
			world.transition_effects.wgate,
			anchor_positions,
			glm::vec3(projector_frame[3]),
			glm::mat3(projector_frame),
			tick,
			elapsed,
			mission.particle_camera_position,
			mission.particle_camera_forward);
		return true;
	}
	case 39:
	{
		// AI_RipperDrop_update, LANCER.EXE 0x00410c00.
		demand.linear_with_exhaust = true;
		if (command.target != UINT16_MAX)
		{
			const std::uint16_t target = command.target;
			const std::int16_t component =
				command.target_component;
			command_pop(world, actor);
			command_push(world,
				actor,
				112,
				TargetKind::object,
				target,
				component);
			return true;
		}
		game::WorldObject* cargo =
			actor.interaction_target_link < game::kMaxGameObjects
				? &world.objects[actor.interaction_target_link]
				: nullptr;
		if (cargo == nullptr || !cargo->active)
		{
			return true;
		}
		switch (work.stage)
		{
		case 0:
			// Retail deliberately compares the signed angular responses
			// directly rather than taking their absolute values.
			if (actor.speed < 0.05f
				&& actor.angular_x < 0.05f
				&& actor.angular_y < 0.05f
				&& actor.angular_z < 0.05f)
			{
				work.stage = 1;
				work.entered_tick = tick;
				ripper_start_animation_all(
					actor, "grab pod", 350.0f, 1, -10.0f);
			}
			return true;
		case 1:
			if (!actor.model_references.empty()
				&& actor.model_references[0].sequence_time == 0.0f)
			{
				work.stage = 2;
				work.entered_tick = tick;
				game::world_queue_sound_explicit(
					world,
					glm::vec3{0.0f},
					glm::vec3{0.0f, 0.0f, 1.0f},
					glm::vec3{0.0f},
					0x3c,
					0);
			}
			return true;
		case 2:
		{
			const game::ObjectModelReference* cargo_model =
				game::retained_find_named_model(
					actor, "Cargo pod");
			if (cargo_model == nullptr)
			{
				return true;
			}
			const std::uint16_t reference =
				static_cast<std::uint16_t>(
					cargo_model
						- actor.model_references.data());
			const glm::mat4 frame =
				object_model_world_transform(actor, reference);
			cargo->previous_position = glm::vec3(frame[3]);
			cargo->position = cargo->previous_position;
			cargo->previous_orientation = glm::mat3(frame);
			cargo->orientation = cargo->previous_orientation;
			ripper_set_cargo_node_hidden(*cargo, false);
			ripper_set_cargo_node_hidden(actor, true);
			actor.runtime_flags &= ~game::kObjectFlagKinematic;
			command_pop(world, actor);
			command_push(world,
				actor,
				111,
				TargetKind::none,
				UINT16_MAX);
			return true;
		}
		default:
			return true;
		}
	}
	case 111:
	{
		// AI_RipperEndDrop_update, LANCER.EXE 0x00410e90.
		game::WorldObject* cargo =
			actor.interaction_target_link < game::kMaxGameObjects
				? &world.objects[actor.interaction_target_link]
				: nullptr;
		if (cargo == nullptr || !cargo->active)
		{
			return true;
		}
		switch (work.stage)
		{
		case 0:
			ripper_start_animation_all(
				actor, "ready to grab", 350.0f, 1, -6.0f);
			work.stage = 1;
			work.entered_tick = tick;
			return true;
		case 1:
			if (!actor.model_references.empty()
				&& actor.model_references[0].sequence_time == 0.0f)
			{
				work.stage = 2;
				work.entered_tick = tick;
				actor.flight_callback_mode =
					game::FlightCallbackMode::standard_reverse;
				actor.control_demand.linear_with_exhaust = false;
				demand.linear_with_exhaust = false;
				demand.throttle = 0.2f;
			}
			return true;
		case 2:
			demand.linear_with_exhaust = false;
			if (work.entered_tick + 50u < tick)
			{
				work.stage = 3;
				work.entered_tick = tick;
				demand.throttle = 0.0f;
				ripper_start_animation_all(
					actor, "cabin turn", 400.0f, 1, -4.5f);
			}
			return true;
		case 3:
		{
			demand.linear_with_exhaust = false;
			const game::ObjectModelReference* cabin =
				game::retained_find_named_model(
					actor, "Ripper Cabin");
			if (cabin != nullptr && cabin->sequence_time == 0.0f)
			{
				work.stage = 4;
				work.entered_tick = tick;
				work.vector =
					actor.position
					+ actor.orientation[2] * 10000.0f;
			}
			return true;
		}
		case 4:
			demand = steer_toward(
				actor,
				work.vector,
				demand.throttle,
				2.0f,
				0.0f,
				0,
				nullptr,
				nullptr,
				mission.frame_delta_ticks);
			if (std::abs(demand.yaw) < 0.025f
				&& std::abs(demand.pitch) < 0.025f
				&& std::abs(demand.roll) < 0.025f
				&& std::abs(actor.angular_y) < 0.02f
				&& std::abs(actor.angular_x) < 0.02f
				&& std::abs(actor.angular_z) < 0.02f)
			{
				work.stage = 5;
				work.entered_tick = tick;
			}
			return true;
		case 5:
			actor.flight_callback_mode =
				game::FlightCallbackMode::standard_forward;
			actor.interaction_target_link = UINT16_MAX;
			cargo->interaction_target_link = UINT16_MAX;
			mission::events_emit_ripper_dropped_object(
				mission,
				actor.mission_index,
				cargo->mission_index);
			actor.runtime_flags &= ~game::kObjectFlagKinematic;
			command_pop(world, actor);
			return false;
		default:
			return true;
		}
	}
	case 112:
	{
		// AI_RipperAttachCargoPod_update, LANCER.EXE 0x00411420.
		RipperAttachWork& attach = work.ripper_attach;
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		game::WorldObject* cargo = game::world_resolve(
			world,
			{attach.carried_object, attach.carried_generation});
		game::ObjectModelReference* component =
			target != nullptr
				? ripper_component_model(
					*target, command.target_component)
				: nullptr;
		const auto abort_attach = [&]()
		{
			game::ripper_grab_effect_release(
				world.death_effects, attach.effect_slot);
			attach.effect_slot = -1;
			command_pop(world, actor);
			applies_flight = false;
		};
		if (cargo == nullptr
			|| cargo->model_references.empty()
			|| component == nullptr)
		{
			abort_attach();
			return false;
		}
		const std::uint32_t now_tick = tick;
		const auto advance = [&]()
		{
			++attach.stage;
			attach.stage_start_tick = now_tick;
		};
		const auto rotational_controls_settled =
			[&actor](const game::FlightDemand& active)
		{
			return std::abs(active.roll) <= 0.025f
				&& std::abs(active.pitch) <= 0.025f
				&& std::abs(active.yaw) <= 0.025f
				&& std::abs(actor.angular_x) <= 0.02f
				&& std::abs(actor.angular_y) <= 0.02f
				&& std::abs(actor.angular_z) <= 0.02f;
		};
		const auto update_beams = [&](float alpha)
		{
			glm::vec3 origins[4];
			glm::vec3 targets[4];
			ripper_beam_origins(actor, origins);
			ripper_beam_targets(*cargo, cargo->position, targets);
			game::ripper_grab_effect_update(
				world.death_effects,
				attach.effect_slot,
				origins,
				targets,
				alpha);
		};
		demand = actor.control_demand;
		demand.linear_no_exhaust = false;
		demand.linear_with_exhaust =
			actor.flight_callback_mode
				== game::FlightCallbackMode::linear_with_exhaust;
		const bool locally_driven =
			mission.network.role == mission::NetworkRole::offline
			|| mission::network_local_owns_object(
				mission.network,
				object_handle(world, actor).index,
				mission.player_prefix_count,
				world.player.index);
		switch (attach.stage)
		{
		case 0:
			if (ai_sequence_sync(actor, world, mission, 1))
			{
				advance();
			}
			return true;
		case 1:
		{
			if (!locally_driven)
			{
				if (ai_sequence_sync(actor, world, mission, 2))
				{
					advance();
				}
				return true;
			}
			const float retained_throttle = demand.throttle;
			demand = steer_toward(
				actor,
				attach.approach_position,
				retained_throttle,
				0.3f,
				0.0f,
				0,
				nullptr,
				nullptr,
				mission.frame_delta_ticks);
			demand.linear_with_exhaust =
				actor.flight_callback_mode
					== game::FlightCallbackMode::linear_with_exhaust;
			const float distance = glm::distance(
				actor.position, attach.approach_position);
			if (distance
					< game::world_effective_max_speed(
						actor, stats, world.camera_mode)
						* 6.0f
				&& distance > 0.0001f)
			{
				const glm::vec3 facing =
					actor.flight_callback_mode
							== game::FlightCallbackMode::
								standard_reverse
						? (actor.position
							- attach.approach_position)
								/ distance
						: (attach.approach_position
							- actor.position)
								/ distance;
				if (glm::dot(actor.orientation[2], facing) < 0.7f)
				{
					demand.throttle = 0.0f;
					return true;
				}
			}
			if (distance > 2000.0f)
			{
				demand.throttle = 1.0f;
			}
			else if (distance > 300.0f)
			{
				actor.flight_callback_mode =
					game::FlightCallbackMode::linear_with_exhaust;
				demand.linear_with_exhaust = true;
				demand.throttle = 0.2f;
			}
			else
			{
				demand.throttle = 0.0f;
			}
			if (std::abs(demand.roll) > 0.025f
				|| std::abs(demand.pitch) > 0.025f
				|| std::abs(demand.yaw) > 0.025f
				|| std::abs(demand.throttle) > 0.025f)
			{
				return true;
			}
			actor.flight_callback_mode =
				game::FlightCallbackMode::linear_with_exhaust;
			demand.linear_with_exhaust = true;
			if (std::abs(actor.angular_x) > 0.02f
				|| std::abs(actor.angular_y) > 0.02f
				|| std::abs(actor.angular_z) > 0.02f)
			{
				return true;
			}
			demand.roll = 0.0f;
			demand.pitch = 0.0f;
			demand.yaw = 0.0f;
			demand.throttle = 0.0f;
			if (ai_sequence_sync(actor, world, mission, 2))
			{
				advance();
			}
			return true;
		}
		case 2:
		{
			if (!locally_driven)
			{
				if (ai_sequence_sync(actor, world, mission, 3))
				{
					advance();
					ripper_start_animation_all(
						actor,
						"grab pod",
						350.0f,
						1,
						-10.0f);
				}
				return true;
			}
			const float retained_throttle = demand.throttle;
			demand = steer_toward(
				actor,
				attach.component_position,
				retained_throttle,
				2.0f,
				0.0f,
				0,
				nullptr,
				nullptr,
				mission.frame_delta_ticks);
			demand.linear_with_exhaust =
				actor.flight_callback_mode
					== game::FlightCallbackMode::linear_with_exhaust;
			if (!rotational_controls_settled(demand)
				|| !ai_sequence_sync(actor, world, mission, 3))
			{
				return true;
			}
			advance();
			ripper_start_animation_all(
				actor, "grab pod", 350.0f, 1, -10.0f);
			return true;
		}
		case 3:
		{
			if (actor.model_references.empty()
				|| actor.model_references[0].sequence_time != 0.0f)
			{
				return true;
			}
			attach.stage = 4;
			attach.stage_start_tick = now_tick;
			target->runtime_flags &= ~0x00000018u;
			const game::ObjectModelReference* cargo_model =
				game::retained_find_named_model(actor, "Cargo pod");
			if (cargo_model == nullptr)
			{
				return true;
			}
			const std::uint16_t reference =
				static_cast<std::uint16_t>(
					cargo_model - actor.model_references.data());
			const glm::mat4 frame =
				object_model_world_transform(actor, reference);
			attach.cargo_start_position = glm::vec3(frame[3]);
			attach.cargo_start_rotation =
				math::rotation_to_euler(glm::mat3(frame));
			cargo->previous_position = attach.cargo_start_position;
			cargo->position = attach.cargo_start_position;
			cargo->previous_orientation = glm::mat3(frame);
			cargo->orientation = glm::mat3(frame);
			return true;
		}
		case 4:
		{
			const float time = std::min(
				1.0f,
				static_cast<float>(
					now_tick - attach.stage_start_tick)
					/ 150.0f);
			update_beams(time * time);
			if (now_tick <= attach.stage_start_tick + 150u
				|| !ai_sequence_sync(actor, world, mission, 5))
			{
				return true;
			}
			advance();
			game::world_queue_sound_object(
				world, object_handle(world, *cargo), 0x3d, 0);
			ripper_set_cargo_node_hidden(*cargo, false);
			ripper_set_cargo_node_hidden(actor, true);
			cargo->runtime_flags &= ~game::kObjectFlagDisabled;
			return true;
		}
		case 5:
		{
			const float time =
				static_cast<float>(
					now_tick - attach.stage_start_tick)
				/ 1000.0f;
			update_beams(1.0f);
			glm::vec3 position;
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				position[axis] = ripper_cosine_interpolate(
					attach.cargo_start_position[axis],
					attach.component_position[axis],
					time);
			}
			cargo->previous_position = position;
			cargo->position = position;
			ripper_attach_component_frame(
				*target,
				command.target_component,
				attach.approach_position,
				attach.component_position,
				attach.component_rotation);
			glm::vec3 rotation;
			if (time < 0.15f)
			{
				rotation = attach.cargo_start_rotation;
			}
			else if (time >= 0.5f)
			{
				rotation = attach.component_rotation;
			}
			else
			{
				const float rotation_time =
					(time - 0.15f) * 2.857142925f;
				for (std::size_t axis = 0; axis < 3; ++axis)
				{
					rotation[axis] = ripper_cosine_interpolate(
						attach.cargo_start_rotation[axis],
						attach.component_rotation[axis],
						rotation_time);
				}
			}
			const glm::mat3 orientation =
				math::rotation_from_euler(rotation);
			cargo->previous_orientation = orientation;
			cargo->orientation = orientation;
			if (now_tick <= attach.stage_start_tick + 1000u)
			{
				return true;
			}
			advance();
			game::world_queue_sound_object(
				world, object_handle(world, *cargo), 0x3c, 0);
			ripper_start_animation_all(
				actor, "ready to grab", 350.0f, 1, -6.0f);
			return true;
		}
		case 6:
			if (actor.model_references.empty()
				|| actor.model_references[0].sequence_time != 0.0f)
			{
				return true;
			}
			advance();
			ripper_start_animation_all(
				actor, "cabin turn", 400.0f, 1, -4.5f);
			return true;
		case 7:
			if (actor.model_references.empty()
				|| actor.model_references[0].sequence_time != 0.0f)
			{
				return true;
			}
			advance();
			attach.aim_position =
				actor.position
				- actor.orientation[2] * 10000.0f;
			return true;
		case 8:
		{
			if (!locally_driven)
			{
				if (ai_sequence_sync(actor, world, mission, 9))
				{
					advance();
				}
				return true;
			}
			const float retained_throttle = demand.throttle;
			demand = steer_toward(
				actor,
				attach.aim_position,
				retained_throttle,
				2.0f,
				0.0f,
				0,
				nullptr,
				nullptr,
				mission.frame_delta_ticks);
			demand.linear_with_exhaust =
				actor.flight_callback_mode
					== game::FlightCallbackMode::linear_with_exhaust;
			if (!rotational_controls_settled(demand)
				|| !ai_sequence_sync(actor, world, mission, 9))
			{
				return true;
			}
			advance();
			return true;
		}
		case 9:
			game::ripper_grab_effect_release(
				world.death_effects, attach.effect_slot);
			attach.effect_slot = -1;
			cargo->runtime_flags |= game::kObjectFlagDisabled;
			cargo->runtime_flags &= ~0x00000200u;
			component->runtime_flags &= ~0x00000020u;
			actor.flight_callback_mode =
				game::FlightCallbackMode::standard_forward;
			actor.control_demand.linear_no_exhaust = false;
			actor.control_demand.linear_with_exhaust = false;
			ripper_set_cargo_node_hidden(*cargo, true);
			actor.runtime_flags &= ~game::kObjectFlagKinematic;
			mission::events_emit_ripper_dropped_object(
				mission,
				actor.mission_index,
				cargo->mission_index);
			diagnostics::mission_log(
				"ripper attach complete actor=%u cargo=%u "
				"target=%u component=%d tick=%u",
				static_cast<unsigned>(actor.mission_index),
				static_cast<unsigned>(cargo->mission_index),
				static_cast<unsigned>(target->mission_index),
				static_cast<int>(command.target_component),
				tick);
			command_pop(world, actor);
			applies_flight = false;
			return false;
		default:
			abort_attach();
			return false;
		}
	}
	case 32:
	{
		// AI_MatchSpeed_update, LANCER.EXE 0x0040b9e0. Retail writes the
		// raw ratio: it neither clamps throttle nor guards a zero maximum.
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		if (target == nullptr
			|| !target_reference_valid(
				*target, command.target_component))
		{
			command_pop(world, actor);
			return false;
		}
		demand.throttle =
			target->speed
			/ game::world_effective_max_speed(
				actor, stats, world.camera_mode);
		return true;
	}
	case 33:
	{
		// AI_DarkReignShootRequest_update, LANCER.EXE 0x0040bad0.
		const std::uint16_t actor_index =
			object_handle(world, actor).index;
		const bool networked =
			mission.network.role != mission::NetworkRole::offline;
		const auto locally_owned = [&]()
		{
			return !networked
				|| mission::network_local_owns_object(
					mission.network,
					actor_index,
					mission.player_prefix_count,
					world.player.index);
		};
		const bool deathmatch =
			mission::network_is_deathmatch_mission(
				mission.mission_number);
		const bool requested_player_connected =
			!deathmatch
			|| (command.target < mission.network.player_count
				&& mission.network.connected[command.target]);
		if (!locally_owned() || !requested_player_connected)
		{
			command_pop(world, actor);
			return false;
		}
		float nearest_distance = std::numeric_limits<float>::max();
		std::uint16_t nearest_object = UINT16_MAX;
		std::int16_t nearest_model = -1;
		const auto consider = [&](
			std::uint16_t object,
			std::int16_t model)
		{
			game::WorldObject* candidate =
				mission::runtime_resolve_object(
					mission, object, world);
			if (candidate == nullptr
				|| !target_reference_valid(*candidate, model)
				|| candidate->protection_state == 2
				|| (candidate->runtime_flags & 0x100u) != 0)
			{
				return;
			}
			const float distance =
				glm::distance(actor.position, candidate->position);
			if (distance < nearest_distance)
			{
				nearest_distance = distance;
				nearest_object = object;
				nearest_model = model;
			}
		};
		if (command.selector == 0)
		{
			consider(command.target, command.target_component);
		}
		else if (command.selector == 1 || command.selector == 2)
		{
			mission::ExpandedTargetReference targets[
				game::kMaxMissionObjects];
			const mission::ReferenceKind kind =
				command.selector == 1
					? mission::ReferenceKind::group
					: mission::ReferenceKind::set;
			const std::uint16_t count =
				mission::runtime_expand_target_reference(
					mission,
					kind,
					command.target,
					targets,
					static_cast<std::uint16_t>(
						std::size(targets)));
			for (std::uint16_t index = 0; index < count; ++index)
			{
				consider(
					targets[index].object,
					targets[index].model);
			}
		}
		if (nearest_object == UINT16_MAX)
		{
			if (networked && !locally_owned())
			{
				return true;
			}
			command_pop(world, actor);
			return false;
		}
		if (networked && !locally_owned())
		{
			return true;
		}
		Command deferred;
		deferred.id = 110;
		deferred.target_kind = TargetKind::object;
		deferred.target = nearest_object;
		deferred.target_component = nearest_model;
		deferred.sequence = 1;
		world.ion_cannon_search_ticks = 0;
		const std::uint8_t publication_seed =
			static_cast<std::uint8_t>(
				game::world_rand15(world));
		if (command_defer(
			actor,
			deferred,
			tick,
			0,
			publication_seed)
			&& networked)
		{
			mission::network_publish_ai_deferred_command(
				mission.network,
				actor_index,
				deferred,
				0,
				publication_seed);
		}
		return true;
	}
	case 34:
	{
		// AI_MoveToSpawnPosition_update, LANCER.EXE 0x004af590. Retail
		// reconstructs this exact live slot through Deathmatch_respawn,
		// publishes only when it is the local slot, then tail-pops the
		// freshly inserted Respawn Effect command so Player/Multiplayer
		// Control becomes active immediately.
		const std::uint16_t actor_index =
			object_handle(world, actor).index;
		demand = {};
		applies_flight = false;
		if (mission::network_reset_player_to_spawn(
				mission,
				world,
				stats,
				actor_index,
				-1,
				actor_index == world.player.index))
		{
			command_pop(world, world.objects[actor_index]);
		}
		return false;
	}
	case 110:
	{
		// AI_DarkReignShoot_update, LANCER.EXE 0x0040d210.
		IonCannonWork& ion = work.ion_cannon;
		const auto abort_shot = [&]()
		{
			ion_cannon_cleanup(work);
			command_pop(world, actor);
		};
		if (!ion.initialized
			|| ion.lower_model >= actor.model_references.size()
			|| ion.upper_model >= actor.model_references.size()
			|| ion.target_object >= game::kMaxGameObjects)
		{
			abort_shot();
			return false;
		}
		game::WorldObject& target = world.objects[ion.target_object];
		if (!target.active
			|| target.type == 1001u
			|| (target.runtime_flags & game::kObjectFlagDestroyed) != 0
			|| (target.ai.command_count != 0
				&& target.ai.commands[0].id == 11))
		{
			abort_shot();
			return false;
		}

		const std::uint32_t elapsed = tick - ion.previous_tick;
		ion.previous_tick = tick;
		if (actor.type == 0x48u)
		{
			// The Coalition tower's firing controller owns all motion fields.
			actor.linear_velocity = {};
			actor.speed = 0.0f;
			actor.throttle = 0.0f;
			actor.angular_x = 0.0f;
			actor.angular_y = 0.0f;
			actor.angular_z = 0.0f;
			demand = {};
		}

		const glm::mat4 lower_frame =
			object_model_world_transform(actor, ion.lower_model);
		const glm::vec3 lower_position{lower_frame[3]};
		const glm::vec3 relative =
			glm::transpose(glm::mat3(lower_frame))
			* (target.position - lower_position);
		const float relative_length = glm::length(relative);
		ion.alignment = relative_length == 0.0f
			? 0.0f
			: std::abs(relative.z) / relative_length;
		if (ion.alignment < 0.9800000190734863f
			&& relative.x != 0.0f)
		{
			const float rate =
				ion.target_object < 5u ? 0.2f : 0.4f;
			const float step =
				std::copysign(
					rate * static_cast<float>(elapsed)
						* 0.006000000052154064f,
					relative.x);
			game::ObjectModelReference& lower =
				actor.model_references[ion.lower_model];
			lower.joint_euler_delta.y += step;
			game::model_animation_recompute_pose(
				actor, ion.lower_model);
		}

		const bool deathmatch =
			mission::network_is_deathmatch_mission(
				mission.mission_number);
		const bool check_lock =
			(!deathmatch
				|| mission.network.role == mission::NetworkRole::host)
			&& ion.stage > 1u && ion.stage < 6u
			&& target.type != 0x11u;
		if (check_lock)
		{
			bool lost =
				target.deathmatch_scenario_counter != -1
				|| (target.runtime_flags & 0x100u) != 0;
			const glm::vec3 upper_position{
				object_model_world_transform(
					actor, ion.upper_model)[3]};
			const float distance =
				glm::distance(target.position, upper_position);
			const float maximum =
				actor.type == 0x48u ? 400000.0f : 190000.0f;
			lost = lost || distance > maximum;
			if (ion.target_object < mission.player_prefix_count)
			{
				const float minimum =
					mission.mission_number == 28u
						? 110000.0f : 8000.0f;
				lost = lost || distance < minimum;
			}
			lost = lost || ion.alignment < 0.90631;
			if (lost)
			{
				const std::uint16_t restart_target = command.target;
				const std::int16_t restart_component =
					command.target_component;
				ion_cannon_cleanup(work);
				command_pop(world, actor);
				if (!deathmatch)
				{
					command_push(world,
						actor,
						110,
						TargetKind::object,
						restart_target,
						restart_component);
				}
				else if (mission.network.role
					== mission::NetworkRole::host)
				{
					mission::network_publish_dark_reign_tower_state(
						mission.network, 7);
				}
				return false;
			}
		}

		world.ion_cannon_search_ticks +=
			static_cast<std::int32_t>(elapsed);
		const bool owns_timeout =
			mission.network.role == mission::NetworkRole::offline
			|| (deathmatch
				&& mission.network.role == mission::NetworkRole::host);
		if (owns_timeout
			&& ion.stage < 5u
			&& world.ion_cannon_search_ticks > 2500)
		{
			ion_cannon_cleanup(work);
			if (deathmatch)
			{
				command_clear(world, actor);
				if (mission.network.role
					== mission::NetworkRole::host)
				{
					mission::network_publish_dark_reign_tower_state(
						mission.network, 7);
				}
			}
			else
			{
				command_pop(world, actor);
			}
			return false;
		}

		static constexpr std::uint32_t kStageDurations[8] = {
			0u, 0u, 300u, 300u, 150u, 200u, 100u, 0u,
		};
		const std::uint32_t duration = kStageDurations[ion.stage];
		const float fraction = duration == 0u
			? 0.0f
			: static_cast<float>(tick - ion.stage_start_tick)
				/ static_cast<float>(duration);
		const auto advance = [&](std::uint8_t sync)
		{
			if (!ai_sequence_sync(
					actor, world, mission, sync))
			{
				return false;
			}
			++ion.stage;
			ion.stage_start_tick = tick;
			return true;
		};
		switch (ion.stage)
		{
		case 0:
			(void)advance(1);
			return true;
		case 1:
			if (ion.alignment > 0.9063000082969666f
				&& advance(2))
			{
				ion.focus_active = true;
				ion.focus_radius =
					target.type == 0x11u ? 200.0f : 600.0f;
			}
			return true;
		case 2:
			ion.target_field_fraction =
				std::min(fraction, 1.0f);
			if (ion.target_object == world.player.index
				&& world.ion_cannon_warning_deadline < tick)
			{
				world.ion_cannon_warning_deadline = tick + 1500u;
				(void)mission::player_comms_play_compiled_pilot(
					mission,
					world,
					mission.mission_number > 13u ? 2u : 4u,
					0,
					"MOO_ICW001.ut",
					mission::CommsPlaybackMode::queue,
					5,
					-1,
					tick);
			}
			if (fraction >= 1.0f)
			{
				(void)advance(3);
			}
			return true;
		case 3:
			ion.target_field_fraction = 1.0f;
			if (fraction >= 1.0f && advance(4))
			{
				game::world_queue_sound_object(
					world, object_handle(world, actor), 58, 4);
				if (actor.type != 0xa5u)
				{
					// ElectricRayEffect_create consumes one rand() while
					// initializing each of the retained plasma arcs.
					for (std::uint8_t index = 0;
						index < ion.plasma_count;
						++index)
					{
						(void)game::world_rand15(world);
					}
				}
			}
			if (actor.type != 0xa5u)
			{
				for (std::uint8_t index = 0;
					index < (actor.type == 0x48u ? 3u : 5u);
					++index)
				{
					if (static_cast<float>(index) * 0.2f
							<= fraction
						&& fraction
							<= static_cast<float>(index + 1u)
								* 0.2f
						&& ion.last_plasma
							< static_cast<std::int8_t>(index))
					{
						ion.last_plasma =
							static_cast<std::int8_t>(index);
						game::world_queue_sound_object(
							world,
							object_handle(world, actor),
							64,
							4);
					}
				}
			}
			return true;
		case 4:
			world.ion_cannon_search_ticks = 0;
			ion.target_field_fraction = 1.0f;
			if (fraction >= 1.0f && advance(5)
				&& actor.type != 0xa5u)
			{
				ion.beam_active = true;
				ion.beam_radius = 2000.0f;
				const assets::GameplayLocator* beam_locator =
					game::model_animation_find_locator(
						actor, ion.beam_model, 11, 0);
				ion.beam_half_length = beam_locator == nullptr
					? 0.0f
					: glm::length(beam_locator->dimensions) * 0.5f;
				if (ion.beam_half_length == 0.0f)
				{
					ion.beam_half_length =
						glm::distance(
							lower_position, target.position)
						* 0.5f;
				}
			}
			return true;
		case 5:
			ion.target_field_fraction = 1.0f;
			if (actor.type != 0xa5u
				&& ion.beam_active && fraction < 0.05f)
			{
				const float time = fraction * 10.0f;
				ion.beam_radius =
					(2000.0f - 0.5f) * time * time + 0.5f;
			}
			if (fraction >= 1.0f && advance(6))
			{
				game::world_queue_sound_object(
					world, object_handle(world, actor), 59, 4);
				ion.impact_active = true;
				// The three-group impact ElectricRayEffect constructor
				// consumes its phase random before the five particles.
				(void)game::world_rand15(world);
				const float radius = target.radius * 0.7f;
				for (std::uint32_t index = 0; index < 5; ++index)
				{
					glm::vec3 local;
					local.z =
						(static_cast<float>(
							game::world_rand15(world))
								* 0x1.0002p-15f
							- 0.5f)
						* radius;
					local.y =
						(static_cast<float>(
							game::world_rand15(world))
								* 0x1.0002p-15f
							- 0.5f)
						* radius;
					local.x =
						(static_cast<float>(
							game::world_rand15(world))
								* 0x1.0002p-15f
							- 0.5f)
						* radius;
					const glm::vec3 spark_position =
						target.position
							+ target.orientation * local;
					const float spark_size =
						static_cast<float>(
							game::world_rand15(world))
							* 0x1.0002p-15f
							* radius;
					(void)game::explosion_billboard_create(
						world.death_effects,
						world,
						spark_position,
						glm::vec3{0.0f},
						game::ExplosionBillboardType::separate_frames,
						spark_size,
						150,
						true,
						static_cast<std::int32_t>(index * 20u),
						false,
						false,
						tick);
				}
			}
			return true;
		case 6:
			if (actor.type != 0xa5u)
			{
				ion.target_field_fraction = 1.0f;
				if (fraction > 0.9f)
				{
					ion.beam_radius =
						5000.0f
						+ (0.5f - 5000.0f)
							* ((fraction - 0.9f) * 10.0f);
				}
			}
			else
			{
				ion.target_field_fraction = 0.0f;
			}
			if (fraction >= 1.0f)
			{
				++ion.stage;
				ion.stage_start_tick = tick;
			}
			return true;
		case 7:
			if (world.camera_mode == 13u
				&& ion.target_object == world.player.index)
			{
				return true;
			}
			ion_cannon_cleanup(work);
			if ((target.runtime_flags & game::kObjectFlagCompound) != 0)
			{
				// GameObject+0x614 is the compound-model destruction
				// callback. Unlike the ordinary scheduler branch, retail
				// invokes it even when protection byte +0xb95 is two.
				for (game::ObjectModelReference& model
					: target.model_references)
				{
					if (!model.removed
						&& model.model_type == 1u)
					{
						(void)game::shockwave_create_object_destruction(
							world,
							mission,
							stats,
							ion.target_object,
							tick);
						model.health = -1.0f;
						target.component_destruction_pending = true;
						break;
					}
				}
			}
			else if (target.protection_state != 2)
			{
				if (mission.network.role
					!= mission::NetworkRole::offline)
				{
					target.last_attacker_index =
						mission.deathmatch.dark_holder < 0
							? UINT16_MAX
							: static_cast<std::uint16_t>(
								mission.deathmatch.dark_holder);
				}
				(void)schedule_death_command(
					target, world, mission, 1, true);
			}
			command_pop(world, actor);
			return false;
		default:
			abort_shot();
			return false;
		}
	}
	case 35:
	{
		// AI_LightsOn_update, LANCER.EXE 0x0040bc20, tests Device/Lmaps
		// before touching retained state or emitting sounds. runtime_tick is
		// only serviced in retail gameplay state one, satisfying its other
		// global termination guard by construction.
		if (!world.light_maps_enabled)
		{
			command_pop(world, actor);
			return false;
		}
		const game::ObjectHandle actor_handle = object_handle(world, actor);
		if (actor.type != 0x00a5u)
		{
			game::retained_set_all_model_lights(actor, true);
			game::world_queue_sound_object(
				world, actor_handle, 64, 4);
			command_pop(world, actor);
			return false;
		}
		if (work.deadline < tick)
		{
			command_pop(world, actor);
			return false;
		}
		const std::uint32_t elapsed =
			tick + 500u - work.deadline;
		if (work.stage >= work.strategy
			|| elapsed <= static_cast<std::uint32_t>(work.stage) * 100u)
		{
			return true;
		}
		game::world_queue_sound_object(
			world, actor_handle, 64, 4);
		if (work.stage < 4)
		{
			// Unlike the later reference walk, retail directly indexes
			// reference zero's base mesh and does not test authored flag
			// 0x80 again.
			if (!actor.model_references.empty()
				&& work.stage
					< actor.model_references[0].light_channels.size())
			{
				actor.model_references[0]
					.light_channels[work.stage] = 1;
			}
		}
		else
		{
			for (std::uint16_t model = 1;
				model < actor.model_references.size();
				++model)
			{
				game::retained_set_model_lights(
					actor, model, true);
			}
		}
		++work.stage;
		return true;
	}
	case 42:
		// AI_LightsOff_update, LANCER.EXE 0x0040be90.
		// Begin/end are the literal RET at 0x004983a0; update mutates the
		// material channels only when Device/Lmaps is exactly one.
		if (world.light_maps_enabled)
		{
			game::retained_set_all_model_lights(actor, false);
		}
		command_pop(world, actor);
		return false;
	case 36:
		// Shared update callback 0x004983a0 is a literal RET. The mission
		// command intentionally persists after applying the one-shot split.
		demand = actor.control_demand;
		return true;
	case 44:
		// AI_ZeroVelocity_update, LANCER.EXE 0x0040c4d0, calls the
		// complete GameObject_zero_motion_controls helper at 0x00403000
		// and immediately pops. Begin/end are the shared literal RET at
		// 0x004983a0. The ordinary flight callback still runs afterward
		// with the newly zeroed controls.
		game::world_zero_motion_controls(actor);
		demand = {};
		command_pop(world, actor);
		return false;
	case 45:
		// AI_FlyBackwards_update, LANCER.EXE 0x0040c4e0. This persistent
		// command has null begin/end callbacks and writes only throttle,
		// roll, pitch, and yaw at GameObject+0x5b8..+0x5c4. The fifth
		// retained control channel at +0x5c8 is deliberately preserved.
		demand.throttle = -0.5f;
		demand.roll = 0.0f;
		demand.pitch = 0.0f;
		demand.yaw = 0.0f;
		return true;
	case 100:
		// AI_PlayerControl_update, LANCER.EXE 0x00413410, has null
		// begin/end callbacks and never removes itself. Retail polls the
		// local input globals here and writes GameObject+0x5b8..+0x5c8.
		// The reimplementation prepares those same controls and performs
		// the local player's single motion pass in mission_session_update
		// before object AI is serviced. Retain the callback's control
		// writes for non-motion servicing, but do not integrate them a
		// second time from the generic command owner.
		demand = player_demand;
		applies_flight = false;
		return true;
	case 101:
		// AI_MultiplayerControl_update, LANCER.EXE 0x00414f70, has null
		// begin/end callbacks and remains installed. Its sole operation is
		// to hide/freeze a locally protected object by promoting runtime
		// flag 0x10000000 to the ordinary 0x00000400 service-suppression
		// flag. Controls are deliberately left unchanged.
		if ((actor.runtime_flags & 0x10000000u) != 0)
		{
			actor.runtime_flags |= game::kObjectFlagDisabled;
		}
		return true;
	case 102:
	{
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		if (target == nullptr || target->type >= 0x100)
		{
			command_pop(world, actor);
			return false;
		}
		if (predict_object_collision(
				actor,
				*target,
				stats,
				world.camera_mode,
				75.0f,
				2000.0f))
		{
			const glm::vec3 local =
				glm::transpose(actor.orientation)
					* (target->position - actor.position);
			demand.throttle = 0.5f;
			demand.roll = 0.0f;
			demand.pitch = local.y < 0.0f ? -1.0f : 1.0f;
			demand.yaw = 0.0f;
			work.deadline =
				tick
				+ 50u
				+ static_cast<std::uint32_t>(std::lround(
					game::world_object_rand_unit(actor)
						* 2000.0f));
			return true;
		}
		demand.throttle = 1.0f;
		demand.roll = 0.0f;
		demand.pitch = 0.0f;
		demand.yaw = 0.0f;
		if (work.deadline < tick)
		{
			command_pop(world, actor);
			return false;
		}
		return true;
	}
	case 103:
	{
		// AI_Torpedo_update, LANCER.EXE 0x00496f90.
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		if (target == nullptr
			|| !target_reference_valid(
				*target, command.target_component))
		{
			command_pop(world, actor);
			return false;
		}
		const glm::vec3 target_point =
			resolved_target_point(
				*target, command.target_component);
		const float travel =
			glm::distance(target_point, actor.position)
				/ stats.records[actor.type].flight.max_speed;
		const glm::vec3 aim =
			target_point
			+ target->orientation[2]
				* (travel * target->speed)
			- actor.linear_velocity
				* std::min(0.5f * travel, 25.0f);
		const game::FlightDemand steering = steer_toward(
			actor,
			aim,
			demand.throttle,
			2.0f,
			0.0f,
			0,
			&world,
			&stats,
			mission.frame_delta_ticks);
		// AI_steer_toward writes only roll, pitch, and yaw. Torpedo's
		// begin callback owns throttle and retail leaves +0x5c8 intact.
		demand.roll = steering.roll;
		demand.pitch = steering.pitch;
		demand.yaw = steering.yaw;
		return true;
	}
	case 105:
		return fight_update(
			actor,
			command,
			world,
			mission,
			stats,
			pilots,
			gun_stats,
			missile_stats,
			missiles,
			chaff,
			random_seed,
			tick,
			demand);
	case 107:
	{
		ScoopUpWork& scoop = work.scoop_up;
		game::WorldObject* target =
			scoop.target_object < game::kMaxGameObjects
				? game::world_resolve(
					world,
					{scoop.target_object, scoop.target_generation})
				: nullptr;
		if (target == nullptr
			|| target->type == 0x3e9u
			|| (target->runtime_flags & game::kObjectFlagDestroyed) != 0)
		{
			tractor_set_doors(actor, false);
			game::world_queue_sound_object(
				world, object_handle(world, actor), 54, 4);
			command_pop(world, actor);
			return false;
		}
		const std::uint32_t previous_tick = scoop.previous_tick;
		scoop.previous_tick = tick;
		game::tractor_effect_set_submitted(
			world.death_effects, scoop.effect_slot, false);
		if (scoop.state < 4)
		{
			demand = steer_toward(
				actor,
				target->position,
				1.0f,
				1.0f,
				0.0f,
				0,
				&world,
				&stats,
				mission.frame_delta_ticks);
		}
		if (scoop.state == 0)
		{
			if ((target->runtime_flags & 0x00001000u) != 0)
			{
				command_pop(world, actor);
				return false;
			}
			target->runtime_flags |= 0x00001000u;
			scoop.reserved = true;
			scoop.state = 1;
			scoop.state_start_tick = tick;
			diagnostics::mission_log(
				"tractor actor=%u state=1 target=%u tick=%u",
				static_cast<unsigned>(actor.mission_index),
				static_cast<unsigned>(scoop.target_object),
				tick);
			return true;
		}
		if (scoop.state == 1)
		{
			const float distance =
				glm::distance(actor.position, target->position);
			const glm::vec3 delta = target->position - actor.position;
			if (distance < 6.0f
					* game::world_effective_max_speed(
						actor, stats, world.camera_mode)
				&& distance > 0.0001f
				&& glm::dot(actor.orientation[2], delta / distance)
					< 0.7f)
			{
				demand.throttle = 0.0f;
				return true;
			}
			demand.throttle =
				distance > 20000.0f ? 1.0f
					: distance > 10000.0f ? 0.4f : 0.0f;
			const bool settled =
				std::abs(demand.pitch) <= 0.025f
				&& std::abs(demand.yaw) <= 0.025f
				&& std::abs(demand.roll) <= 0.025f
				&& std::abs(demand.throttle) <= 0.025f
				&& std::abs(actor.angular_x) <= 0.02f
				&& std::abs(actor.angular_y) <= 0.02f
				&& std::abs(actor.angular_z) <= 0.02f;
			if (!settled)
			{
				return true;
			}
			scoop.state = 2;
			return true;
		}
		glm::vec3 origins[2]{actor.position, actor.position};
		glm::vec3 anchor = actor.position;
		if (scoop.state == 2)
		{
			demand = {};
			if (!tractor_attachment(
					actor, origins, anchor, 6300.0f))
			{
				command_pop(world, actor);
				return false;
			}
			tractor_set_doors(actor, true);
			game::world_queue_sound_object(
				world, object_handle(world, actor), 53, 4);
			scoop.beam_stagger =
				game::world_object_rand_unit(actor) * 0.2f;
			scoop.state_start_tick = tick;
			scoop.state = 3;
			diagnostics::mission_log(
				"tractor actor=%u state=3 target=%u tick=%u",
				static_cast<unsigned>(actor.mission_index),
				static_cast<unsigned>(scoop.target_object),
				tick);
			return true;
		}
		if (scoop.state == 3)
		{
			tractor_attachment(actor, origins, anchor, 6300.0f);
			const float phase =
				static_cast<float>(tick - scoop.state_start_tick)
				/ 100.0f;
			if (phase < 1.0f)
			{
				const float second =
					phase <= scoop.beam_stagger ? 0.0f
						: (phase - scoop.beam_stagger)
							/ (1.0f - scoop.beam_stagger);
				game::tractor_effect_update(
					world.death_effects,
					scoop.effect_slot,
					origins,
					*target,
					phase,
					second,
					std::min(2.0f * phase, 1.0f),
					phase,
					10000.0f * phase);
				demand = {};
				return true;
			}
			target->runtime_flags |= 0x00000010u;
			scoop.state = 4;
			scoop.state_start_tick = tick;
			game::tractor_effect_update(
				world.death_effects,
				scoop.effect_slot,
				origins,
				*target,
				1.0f,
				1.0f,
				1.0f,
				1.0f,
				10000.0f);
			return true;
		}
		if (scoop.state == 4 || scoop.state == 5)
		{
			const float forward =
				scoop.state == 4 ? 6300.0f
					: actor.type == 0x18u ? 3000.0f : 3400.0f;
			if (!tractor_attachment(actor, origins, anchor, forward))
			{
				command_pop(world, actor);
				return false;
			}
			target->runtime_flags |= 0x00000004u;
			const glm::vec3 delta = anchor - target->position;
			const float distance = glm::length(delta);
			const float step_scale =
				static_cast<float>(tick - previous_tick) * 0.01f;
			const float speed =
				scoop.state == 4
					? std::min(
						cosine_ease(
							0.0f, 900.0f, 1800.0f,
							std::clamp(distance / 3000.0f, 0.0f, 1.0f)),
						1800.0f)
					: cosine_ease(0.0f, 0.0f, 900.0f, 1.0f);
			if (distance > 0.0001f)
			{
				target->previous_position = target->position;
				target->position +=
					delta / distance
					* std::min(distance, speed * step_scale);
				target->linear_velocity =
					target->position - target->previous_position;
				target->speed = glm::length(target->linear_velocity);
			}
			const float visual_phase =
				static_cast<float>(tick - scoop.state_start_tick) * 0.01f;
			game::tractor_effect_update(
				world.death_effects,
				scoop.effect_slot,
				origins,
				*target,
				1.0f,
				1.0f,
				1.0f,
				visual_phase,
				10000.0f);
			if (scoop.state == 4 && distance < 500.0f)
			{
				scoop.state = 5;
				scoop.state_start_tick = tick;
				diagnostics::mission_log(
					"tractor actor=%u state=5 target=%u tick=%u",
					static_cast<unsigned>(actor.mission_index),
					static_cast<unsigned>(scoop.target_object),
					tick);
			}
			else if (scoop.state == 5 && distance < 300.0f)
			{
				tractor_set_doors(actor, false);
				game::world_queue_sound_object(
					world, object_handle(world, actor), 54, 4);
				scoop.state = 6;
				scoop.state_start_tick = tick;
			}
			demand = {};
			return true;
		}
		if (scoop.state == 6)
		{
			tractor_attachment(actor, origins, anchor, 3400.0f);
			const float phase =
				static_cast<float>(tick - scoop.state_start_tick)
				/ 50.0f;
			if (phase < 1.0f)
			{
				game::tractor_effect_update(
					world.death_effects,
					scoop.effect_slot,
					origins,
					*target,
					1.0f - phase,
					1.0f - phase,
					std::max(1.0f - 2.0f * phase, 0.0f),
					phase,
					10000.0f * (1.0f - phase));
				demand = {};
				return true;
			}
			game::tractor_effect_set_submitted(
				world.death_effects, scoop.effect_slot, false);
			scoop.state = 7;
			scoop.state_start_tick = tick;
			return true;
		}
		if (scoop.state == 7)
		{
			if (tick - scoop.state_start_tick < 250)
			{
				demand = {};
				return true;
			}
			scoop.state = 8;
			return true;
		}
		target->runtime_flags &= ~0x00000004u;
		const game::ObjectHandle target_handle =
			object_handle(world, *target);
		const std::uint16_t completed_target = scoop.target_object;
		const std::uint16_t completed_target_mission_index =
			target->mission_index;
		command_pop(world, actor);
		// AI_ScoopUp_update, LANCER.EXE
		// 0x0041caa1..0x0041cab8, publishes ObjectScooped after the
		// Scoop Up command has ended but before cargo becomes departed.
		mission::events_emit_object_scooped(
			mission,
			actor.mission_index,
			completed_target_mission_index);
		game::world_mark_departed(world, target_handle);
		diagnostics::mission_log(
			"tractor actor=%u complete target=%u tick=%u",
			static_cast<unsigned>(actor.mission_index),
			static_cast<unsigned>(completed_target),
			tick);
		demand = {};
		return false;
	}
	case 121:
	{
		const std::int16_t effect_slot = work.respawn.effect_slot;
		if (!game::respawn_effect_active(
				world.death_effects, effect_slot))
		{
			command_pop(world, actor);
			demand = {};
			applies_flight = false;
			return false;
		}
		const std::uint32_t now_tick = tick;
		if (static_cast<std::int32_t>(now_tick)
			< static_cast<std::int32_t>(work.deadline))
		{
			const float remaining =
				static_cast<float>(work.deadline - now_tick)
					* (1.0f / 300.0f);
			game::respawn_effect_update(
				world.death_effects,
				effect_slot,
				actor,
				remaining);
			demand = {};
			applies_flight = false;
			return true;
		}
		const bool local_player = actor.player;
		command_pop(world, actor);
		if (local_player)
		{
			transition_request_camera(
				mission, world, 0, false, true);
		}
		mission::deathmatch_scenarios_post_respawn(
			mission,
			world,
			static_cast<std::uint16_t>(
				&actor - std::begin(world.objects)));
		diagnostics::mission_log(
			"deathmatch respawn effect complete actor=%u tick=%u",
			static_cast<unsigned>(actor.mission_index),
			tick);
		demand = {};
		applies_flight = false;
		return false;
	}
	case 120:
	{
		game::WorldObject* target =
			resolve_target(actor, command, world, mission);
		const std::uint32_t now_tick = tick;
		const std::uint32_t end_tick = work.deadline + 500u;
		if (target == nullptr
			|| !target_reference_valid(
				*target,
				command.target_component,
				0x00000100u)
			|| static_cast<std::int32_t>(end_tick)
				< static_cast<std::int32_t>(now_tick))
		{
			command_pop(world, actor);
			return false;
		}
		const float phase =
			game::world_effective_max_speed(
				actor, stats, world.camera_mode)
				* static_cast<float>(now_tick - work.deadline)
				* 0.000005f;
		// Retail reads rows zero and two from the stored matrix
		// (0x0040a7f9..0x0040a82f), with cosine on row zero and sine on
		// row two. GLM stores columns, so spell out those rows.
		const glm::vec3 row_zero{
			work.orientation[0][0],
			work.orientation[1][0],
			work.orientation[2][0],
		};
		const glm::vec3 row_two{
			work.orientation[0][2],
			work.orientation[1][2],
			work.orientation[2][2],
		};
		const glm::vec3 radial =
			row_zero * (std::cos(phase) * 50000.0f)
			+ row_two * (std::sin(phase) * 50000.0f);
		demand = steer_toward(
			actor,
			resolved_target_point(
				*target, command.target_component)
				+ radial,
			1.0f,
			1.0f,
			0.0f,
			3,
			&world,
			&stats,
			mission.frame_delta_ticks);
		return true;
	}
	case 122:
		// AI_DeathmatchDarkReignTarget_begin/update both point to the
		// single-instruction no-op at LANCER.EXE 0x004983a0. This is a
		// persistent marker command: retain the incoming control demand
		// and ordinary flight ownership until another command replaces it.
		return true;
	case 115:
	case 116:
	{
		// AI_CapshipListLeft/Right_update share the mirrored controller at
		// LANCER.EXE 0x0040c3a0. Left enters through thunk 0x0040c4b0
		// with -1 and right through 0x0040c4c0 with +1. Its terminal state
		// clears roll only and deliberately retains yaw.
		const float sign = command.id == 115 ? -1.0f : 1.0f;
		const assets::FlightStats& flight =
			stats.records[actor.type].flight;
		switch (work.stage)
		{
		case 0:
			demand.roll = sign * 0.01f / flight.roll_rate;
			demand.yaw = sign * 0.006f / flight.yaw_rate;
			work.stage = 1;
			work.deadline = tick + 200;
			return true;
		case 1:
			if (tick < work.deadline)
			{
				demand.roll = sign * 0.01f / flight.roll_rate;
				demand.yaw = sign * 0.006f / flight.yaw_rate;
				return true;
			}
			demand.roll = sign * -0.006f / flight.roll_rate;
			demand.yaw = sign * -0.0048f / flight.yaw_rate;
			work.stage = 2;
			work.deadline = tick + 300;
			return true;
		case 2:
			if (tick < work.deadline)
			{
				demand.roll =
					sign * -0.006f / flight.roll_rate;
				demand.yaw =
					sign * -0.0048f / flight.yaw_rate;
				return true;
			}
			work.stage = 3;
			[[fallthrough]];
		default:
			demand.roll = 0.0f;
			demand.yaw =
				sign * -0.0048f / flight.yaw_rate;
			command_pop(world, actor);
			return false;
		}
	}
	default:
		command_pop(world, actor);
		return false;
	}
}
}

bool schedule_death_command(
	game::WorldObject& object,
	game::World& world,
	mission::Runtime& mission,
	std::uint8_t cause,
	bool force_explode_player)
{
	if (!object.active)
	{
		return false;
	}
	const auto prepare_replacement =
		[&world, &object](std::int16_t incoming_id)
		{
			// AI_command_can_replace, LANCER.EXE 0x0040ca50. The death
			// scheduler ignores the return value, but a permitted
			// replacement still runs the begun head's end callback.
			if ((object.runtime_flags & 0x10000840u) != 0
				|| object.ai.command_count == 0
				|| object.ai.work.begin_pending
				|| command_is_immediate(incoming_id))
			{
				return;
			}
			if (incoming_id != 11)
			{
				const std::int32_t current_priority =
					command_priority(object.ai.commands[0].id);
				if (current_priority != 0
					&& command_priority(incoming_id)
						<= current_priority)
				{
					return;
				}
			}
			end_active_command(
				world, object, object.ai.commands[0]);
		};
	const auto install_terminal_head =
		[&object, cause](std::int16_t command_id)
		{
			// AI_schedule_death_command writes the live count directly to
			// one and overwrites only the packed head fields plus state byte
			// zero. Queued tails and the shared work allocation remain
			// stale; in particular, their end callbacks are never run.
			object.ai.command_count = 1;
			Command& head = object.ai.commands[0];
			head.id = command_id;
			head.selector = 0;
			head.target = UINT16_MAX;
			head.target_component = -1;
			head.target_kind = TargetKind::none;
			head.state[0] =
				(head.state[0] & 0xffffff00u)
				| static_cast<std::uint32_t>(cause);
			object.ai.work.begin_pending = true;
		};
	const std::uint16_t world_index = static_cast<std::uint16_t>(
		&object - std::begin(world.objects));
	const bool ordinary_ejection =
		world_index >= mission.player_prefix_count
		&& object.sound3d_slot == 0
		&& object.ejection_roll < 40;
	const bool forced_ejection = object.protection_state == 3;
	if ((object.runtime_flags & 0x00000800u) == 0
		&& (ordinary_ejection || forced_ejection))
	{
		// Retail asks whether command 30 (Eject) may replace the active
		// behavior, then installs internal command 108 (Eject Spin).
		prepare_replacement(30);
		install_terminal_head(108);
		diagnostics::mission_log(
			"death selected actor=%u path=eject-spin cause=%u roll=%u",
			static_cast<unsigned>(object.mission_index),
			static_cast<unsigned>(cause),
			static_cast<unsigned>(object.ejection_roll));
		return true;
	}
	if (object.ai.command_count != 0
		&& object.ai.commands[0].id == 11)
	{
		return true;
	}
	const bool local_player = is_local_actor(world, object);
	if (local_player
		&& (object.runtime_flags & 0x00000800u) == 0
		&& mission.network.role == mission::NetworkRole::offline
		&& !force_explode_player
		&& object.type != 45)
	{
		if (object.ai.command_count != 0
			&& object.ai.commands[0].id == 118)
		{
			if (object.ai.work.stage == 0)
			{
				return true;
			}
			command_pop(world, object);
		}
		else
		{
			const TargetKind inherited_kind =
				object.ai.command_count == 0
					? TargetKind::none
					: object.ai.commands[0].target_kind;
			const std::uint16_t inherited_target =
				object.ai.command_count == 0
					? UINT16_MAX
					: object.ai.commands[0].target;
			const std::int16_t inherited_component =
				object.ai.command_count == 0
					? -1
					: object.ai.commands[0].target_component;
			// The wrapper at 0x004020d3 forwards the current head's target
			// and component. Its insertion result is intentionally ignored
			// before the disabled-control bit is set.
			(void)command_push(
				world,
				object,
				118,
				inherited_kind,
				inherited_target,
				inherited_component);
			object.runtime_flags |= 0x00000800u;
			diagnostics::mission_log(
				"death selected actor=%u path=eject-player cause=%u",
				static_cast<unsigned>(object.mission_index),
				static_cast<unsigned>(cause));
			return true;
		}
	}
	prepare_replacement(11);
	install_terminal_head(11);
	object.runtime_flags |= game::kObjectFlagDestroyed;
	diagnostics::mission_log(
		"death selected actor=%u path=explode cause=%u",
		static_cast<unsigned>(object.mission_index),
		static_cast<unsigned>(cause));
	return true;
}

game::FlightDemand runtime_steer_toward_point(
	const game::WorldObject& actor,
	const glm::vec3& target,
	float throttle,
	float maximum_control,
	float response_retention,
	std::uint32_t options,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_delta)
{
	return steer_toward(
		actor,
		target,
		throttle,
		maximum_control,
		response_retention,
		options,
		&world,
		&stats,
		frame_delta);
}

bool runtime_approach_point(
	const game::WorldObject& actor,
	const glm::vec3& target,
	const glm::mat3& target_basis,
	float minimum_throttle,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_delta,
	game::FlightDemand& demand)
{
	return approach_point(
		actor,
		target,
		target_basis,
		minimum_throttle,
		world,
		stats,
		frame_delta,
		demand);
}

bool command_push(
	game::World& world,
	game::WorldObject& object,
	std::int16_t command_id,
	TargetKind target_kind,
	std::uint16_t target,
	std::int16_t target_component,
	std::int16_t selector,
	std::int16_t sequence,
	bool* inserted)
{
	if (inserted != nullptr)
	{
		*inserted = false;
	}
	Command command;
	command.id = command_id;
	command.target_kind = target_kind;
	command.target = target;
	command.target_component = target_component;
	command.selector = selector;
	command.sequence = sequence;
	if (object.player_slot
		&& command_id < 100
		&& (command_flags(command_id) & kCommandFlagPlayerSlotAdmission) == 0)
	{
		// AI_command_forbidden_for_player_slot, LANCER.EXE 0x0040ca00.
		// Retail runs this before duplicate detection and rejects ordinary
		// autonomous low-ID commands unless the compiled definition marks
		// them player-compatible. All internal IDs 100+ pass this guard.
		return false;
	}
	if (object.ai.command_count != 0
		&& same_command(object.ai.commands[0], command))
	{
		// AI_command_insert's exact-head duplicate succeeds without
		// restarting the active behavior or disturbing its shared work.
		return true;
	}
	if (object.type == 1001
		|| (object.runtime_flags & 0x10000840u) != 0)
	{
		// Type-1001 is the in-place departed allocation. Retail's live-object
		// state gate rejects command insertion until CreateFlightGroup fully
		// destroys and reconstructs that explicit slot.
		return false;
	}
	// AI_command_can_replace, LANCER.EXE 0x0040ca50, invokes the outgoing
	// end callback before AI_command_insert scans queued duplicates or
	// checks capacity. The old head and the shared work allocation must
	// therefore remain intact throughout the callback, even when insertion
	// subsequently fails because the queue is full.
	if (object.ai.command_count != 0
		&& !object.ai.work.begin_pending
		&& !command_is_immediate(command_id)
		&& command_id != 11)
	{
		const std::int32_t current_priority =
			command_priority(object.ai.commands[0].id);
		if (current_priority != 0
			&& command_priority(command_id) <= current_priority)
		{
			return false;
		}
	}
	if (object.ai.command_count != 0
		&& !object.ai.work.begin_pending
		&& !command_is_immediate(command_id))
	{
		end_active_command(
			world, object, object.ai.commands[0]);
	}
	for (std::uint8_t index = 1;
		index < object.ai.command_count;
		++index)
	{
		if (!same_command(object.ai.commands[index], command))
		{
			continue;
		}
		for (std::uint8_t shift = index + 1;
			shift < object.ai.command_count;
			++shift)
		{
			object.ai.commands[shift - 1] =
				object.ai.commands[shift];
		}
		--object.ai.command_count;
		break;
	}
	if (object.ai.command_count >= game::kMaxAiCommandsPerObject)
	{
		return false;
	}
	for (std::uint8_t index = object.ai.command_count;
		index > 0;
		--index)
	{
		object.ai.commands[index] = object.ai.commands[index - 1];
	}
	object.ai.commands[0] = command;
	++object.ai.command_count;
	if (!command_is_immediate(command_id))
	{
		reset_work(object);
	}
	if (inserted != nullptr)
	{
		*inserted = true;
	}
	return true;
}

bool command_defer(
	game::WorldObject& object,
	const Command& command,
	std::uint32_t current_tick,
	std::uint32_t delay_ticks,
	std::uint8_t publication_seed)
{
	const std::uint32_t activation_tick = current_tick + delay_ticks;
	for (std::uint8_t index = 0;
		index < object.ai.deferred_command_count;
		++index)
	{
		const DeferredCommand& queued =
			object.ai.deferred_commands[index];
		if (!same_deferred_identity(queued.command, command))
		{
			continue;
		}
		// AI_defer_command, LANCER.EXE 0x00402660, retains an existing
		// identical record when the new activation is earlier or equal.
		// A later replacement removes the old record and appends anew.
		if (activation_tick <= queued.activation_tick)
		{
			return true;
		}
		remove_deferred_command(object, index);
		break;
	}
	if (object.ai.deferred_command_count
		>= game::kMaxAiCommandsPerObject)
	{
		diagnostics::mission_log(
			"ai deferred queue full actor=%u command=%d(%s)",
			static_cast<unsigned>(object.mission_index),
			static_cast<int>(command.id),
			command_name(command.id));
		return false;
	}
	DeferredCommand& queued =
		object.ai.deferred_commands[
			object.ai.deferred_command_count++];
	queued.command = command;
	queued.activation_tick = activation_tick;
	queued.publication_seed = publication_seed;
	return true;
}

bool command_pop(
	game::World& world,
	game::WorldObject& object)
{
	if (object.ai.command_count == 0)
	{
		return false;
	}
	const Command completed = object.ai.commands[0];
	diagnostics::mission_log(
		"ai finish actor=%u command=%d(%s)",
		static_cast<unsigned>(object.mission_index),
		static_cast<int>(completed.id),
		command_name(completed.id));
	// AI_command_pop, LANCER.EXE 0x0040ce70, calls the end function while
	// both the completed command and its work block are still installed.
	end_active_command(world, object, object.ai.commands[0]);
	for (std::uint8_t index = 1;
		index < object.ai.command_count;
		++index)
	{
		object.ai.commands[index - 1] = object.ai.commands[index];
	}
	--object.ai.command_count;
	// Retail decrements the live count without clearing the old tail slot.
	// That stale record remains observable to the same raw queue storage
	// until a later insertion overwrites it.
	if (!command_is_immediate(completed.id))
	{
		reset_work(object);
	}
	return true;
}

void command_clear(
	game::World& world,
	game::WorldObject& object)
{
	while (object.ai.command_count != 0)
	{
		command_pop(world, object);
	}
}

void command_mark_destroyed(
	game::World& world,
	game::WorldObject& object)
{
	// GameObject_mark_destroyed, LANCER.EXE 0x00401f00, first calls
	// AI_command_can_replace with incoming command 11. That path invokes only
	// the begun head command's end callback; it does not pop queued commands
	// or reset the shared work allocation. The following WORD write drops the
	// complete queue regardless of whether replacement cleanup was admitted.
	if ((object.runtime_flags & 0x10000840u) == 0
		&& object.ai.command_count != 0
		&& !object.ai.work.begin_pending)
	{
		end_active_command(
			world, object, object.ai.commands[0]);
	}
	object.ai.command_count = 0;
}

bool command_try_clear_fast(
	game::World& world,
	game::WorldObject& object)
{
	if (object.type == 1001
		|| (object.runtime_flags & 0x10000840u) != 0)
	{
		return false;
	}
	if (object.ai.command_count == 0)
	{
		return true;
	}
	const Command& active = object.ai.commands[0];
	if (!object.ai.work.begin_pending
		&& command_priority(active.id) != 0)
	{
		// AI_try_clear_command_queue_fast prepares replacement with
		// synthetic command -1. A begun nonzero-priority head rejects that
		// replacement instead of silently tearing down protected work.
		return false;
	}
	diagnostics::mission_log(
		"ai clear-fast actor=%u command=%d(%s) queued=%u",
		static_cast<unsigned>(object.mission_index),
		static_cast<int>(active.id),
		command_name(active.id),
		static_cast<unsigned>(object.ai.command_count));
	end_active_command(world, object, active);
	// Retail writes only the queue count. Lower records and shared work
	// remain allocated/stale until a later normal insertion resets them.
	object.ai.command_count = 0;
	return true;
}

bool command_signal_launch(game::WorldObject& object)
{
	for (std::uint8_t index = 0;
		index < object.ai.command_count;
		++index)
	{
		if (object.ai.commands[index].id == 104)
		{
			// AI_signal_launch (LANCER.EXE 0x00418db0) writes only byte
			// zero at command-record +0x0a, preserving the other three
			// bytes of payload dword zero.
			object.ai.commands[index].state[0] =
				(object.ai.commands[index].state[0] & 0xffffff00u)
				| 1u;
			return true;
		}
	}
	return false;
}

bool command_is_jump_or_launch(const game::WorldObject& object)
{
	if (object.ai.command_count == 0)
	{
		return false;
	}
	switch (object.ai.commands[0].id)
	{
	case 4:
	case 5:
	case 19:
	case 20:
	case 25:
	case 26:
	case 40:
	case 41:
	case 104:
		return true;
	default:
		return false;
	}
}

bool command_owns_flight(const game::WorldObject& object)
{
	return object.ai.command_count != 0
		&& object.ai.commands[0].id != 100;
}

bool command_has_positive_priority(const game::WorldObject& object)
{
	return object.ai.command_count != 0
		&& command_priority(object.ai.commands[0].id) > 0;
}

bool command_requires_network_motion(
	const game::WorldObject& object)
{
	return object.ai.command_count != 0
		&& (command_flags(object.ai.commands[0].id)
			& kCommandFlagSerializeMotion) != 0;
}

void promote_deferred_commands(
	game::World& world,
	game::WorldObject& object,
	std::uint32_t simulation_tick)
{
	// AI_update_object_command, LANCER.EXE 0x0040c5f0, services the packed
	// +0xb8c/+0xb90 array before retaliation and normal dispatch. Removal
	// compacts in place and rechecks the new record at the same ordinal.
	for (std::uint8_t index = 0;
		index < object.ai.deferred_command_count;)
	{
		const DeferredCommand queued =
			object.ai.deferred_commands[index];
		const bool priority_eligible =
			object.ai.command_count == 0
			|| command_priority(object.ai.commands[0].id)
				<= command_priority(queued.command.id);
		if (queued.activation_tick > simulation_tick
			|| !priority_eligible)
		{
			++index;
			continue;
		}
		if (object.active
			&& command_push(world,
				object,
				queued.command.id,
				queued.command.target_kind,
				queued.command.target,
				queued.command.target_component,
				queued.command.selector,
				queued.command.sequence))
		{
			// Retail replays exactly the four unaligned dwords at
			// command +0x0a..+0x19 after insertion.
			std::copy(
				std::begin(queued.command.state),
				std::end(queued.command.state),
				std::begin(object.ai.commands[0].state));
		}
		remove_deferred_command(object, index);
	}
}

bool service_object_command(
	game::WorldObject& object,
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	const assets::PilotStatsTable& pilot_stats,
	const assets::GunStatsTable& gun_stats,
	const assets::MissileStatsTable& missile_stats,
	game::WeaponRuntime& weapons,
	game::MissileRuntime& missiles,
	game::ChaffRuntime& chaff,
	const game::FlightDemand& player_demand,
	std::uint32_t& random_seed,
	std::uint32_t simulation_tick,
	bool commit_motion)
{
	const bool providerless_command =
		object.ai.command_count != 0
		&& (object.ai.commands[0].id == 6
			|| object.ai.commands[0].id == 13)
		&& object.type > UINT8_MAX;
	if (!object.active
		|| (object.type >= assets::kShipStatsCount
			&& !providerless_command)
		|| (!object.components_initialized && !providerless_command)
		|| (object.ai.command_count == 0
			&& object.ai.deferred_command_count == 0)
		|| (object.runtime_flags & game::kObjectFlagDisabled) != 0)
	{
		return false;
	}
	promote_deferred_commands(world, object, simulation_tick);
	if (object.ai.command_count == 0)
	{
		return false;
	}
	if (!providerless_command)
	{
		maybe_retaliate_against_attacker(
			object, world, mission, stats);
	}
	Command& command = object.ai.commands[0];
	if (object.ai.work.begin_pending)
	{
		begin_command(
			object,
			command,
			world,
			mission,
			file,
			stats,
			pilot_stats,
			simulation_tick);
		if (object.ai.work.begin_pending
			|| object.ai.command_count == 0)
		{
			return false;
		}
	}
	game::FlightDemand demand = object.control_demand;
	demand.afterburner = false;
	demand.reverse = false;
	bool applies_flight = true;
	object.primary_weapon_requested = false;
	object.secondary_weapon_requested = false;
	const std::int16_t serviced_command_id = command.id;
	const bool command_continues = update_command(
		object,
		command,
		world,
		mission,
		file,
		stats,
		pilot_stats,
		gun_stats,
		missile_stats,
		missiles,
		chaff,
		player_demand,
		random_seed,
		simulation_tick,
		demand,
		applies_flight);
	if (serviced_command_id == 118 && !command_continues)
	{
		// AI_EjectPlayer_update, LANCER.EXE 0x0041647e, does not return
		// after replacing itself with Explode. It tail-calls
		// AI_update_object_command so the new death command begins and
		// receives its first update in this same gameplay tick.
		return service_object_command(
			object,
			world,
			mission,
			file,
			stats,
			pilot_stats,
			gun_stats,
			missile_stats,
			weapons,
			missiles,
			chaff,
			player_demand,
			random_seed,
			simulation_tick,
			commit_motion);
	}
	if ((object.runtime_flags & 0x00020000u) != 0)
	{
		demand.throttle = 0.0f;
		object.primary_weapon_requested = false;
		object.secondary_weapon_requested = false;
	}
	if (object.primary_weapon_requested)
	{
		game::weapons_apply_gun_cooldown(
			world,
			object,
			simulation_tick,
			object.ai.work.primary_weapon_delay);
	}
	if (object.secondary_weapon_requested
		&& object.ai.work.secondary_weapon_target
			< game::kMaxGameObjects)
	{
		game::WorldObject& target =
			world.objects[
				object.ai.work.secondary_weapon_target];
		if (target.active)
		{
			game::missiles_launch_from_ship_mount(
				missiles,
				world,
				missile_stats,
				object_handle(world, object),
				object.ai.work.secondary_weapon_mount,
				object_handle(world, target),
				object.ai.work.secondary_weapon_component,
				simulation_tick);
		}
	}
	object.primary_weapon_requested = false;
	object.secondary_weapon_requested = false;
	if (object.active)
	{
		// AI_update_all_objects (0x0040c8f0) prepares callback/control
		// state during the rendered-frame owner. GameObjects_service_phase
		// (0x004774d0) owns the only ordinary pose integration, at 25 Hz.
		// `commit_motion` distinguishes that ordinary frame service from
		// the post-construction command service, but neither is itself a
		// transform owner.
		object.ordinary_motion_enabled =
			applies_flight
				&& (object.runtime_flags & game::kObjectFlagSimulationSuspended) == 0;
		if (applies_flight || !commit_motion)
		{
			object.control_demand = demand;
		}
	}
	return true;
}

bool runtime_service_object_command(
	game::WorldObject& object,
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	const assets::PilotStatsTable& pilot_stats,
	const assets::GunStatsTable& gun_stats,
	const assets::MissileStatsTable& missile_stats,
	game::WeaponRuntime& weapons,
	game::MissileRuntime& missiles,
	game::ChaffRuntime& chaff,
	const game::FlightDemand& player_demand,
	std::uint32_t& random_seed,
	std::uint32_t simulation_tick)
{
	return service_object_command(
		object,
		world,
		mission,
		file,
		stats,
		pilot_stats,
		gun_stats,
		missile_stats,
		weapons,
		missiles,
		chaff,
		player_demand,
		random_seed,
		simulation_tick,
		false);
}

void runtime_tick(
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	const assets::PilotStatsTable& pilot_stats,
	const assets::GunStatsTable& gun_stats,
	const assets::MissileStatsTable& missile_stats,
	game::WeaponRuntime& weapons,
	game::MissileRuntime& missiles,
	game::ChaffRuntime& chaff,
	const game::FlightDemand& player_demand,
	std::uint32_t& random_seed,
	std::uint32_t simulation_tick)
{
	scripted_mission_frame_dispatch(world, mission);
	// AI_update_all_objects, LANCER.EXE 0x0040c8f0, clears accumulated
	// threat on the strict 500-tick mission-clock cadence. Retail excludes
	// the separately updated local player from this ordinary-object pass.
	if (world.ai_threat_clear_deadline < simulation_tick)
	{
		for (game::WorldObject& object : world.objects)
		{
			if (!object.player)
			{
				object.attack_pressure = 0.0f;
			}
		}
		world.ai_threat_clear_deadline = simulation_tick + 500;
	}
	for (game::WorldObject& object : world.objects)
	{
		service_object_command(
			object,
			world,
			mission,
			file,
			stats,
			pilot_stats,
			gun_stats,
			missile_stats,
			weapons,
			missiles,
			chaff,
			player_demand,
			random_seed,
			simulation_tick,
			true);
	}
	for (game::WorldObject& object : world.objects)
	{
		if (object.active)
		{
			rebuild_nearby_lists(world, object, stats);
		}
	}
}
}
