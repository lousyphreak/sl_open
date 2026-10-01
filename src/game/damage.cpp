#include "game/damage.hpp"

#include "ai/runtime.hpp"
#include "ai/scripted_commands.hpp"
#include "assets/ship_stats.hpp"
#include "game/world.hpp"
#include "mission/network_runtime.hpp"
#include "mission/player_comms.hpp"
#include "mission/runtime.hpp"

#include <algorithm>
#include <iterator>

namespace sl_open::game
{
namespace
{
constexpr float kImpactFeedbackScale = 0.05000000074505806f;
constexpr float kForcedPlayerExplosionDamage = 1000.0f;
constexpr float kEasyOutgoingDamageScale = 1.5f;
constexpr float kHardOutgoingDamageScale = 0.75f;
constexpr float kIncomingDamageBaseScale = 0.5f;
constexpr float kEasyIncomingDifficultyScale = 0.75f;
constexpr float kHardIncomingDifficultyScale = 1.5f;

bool is_local_player(
	const World& world,
	const WorldObject& object)
{
	const auto index = static_cast<std::uint16_t>(
		&object - std::begin(world.objects));
	return world.player.index == index
		&& world.player.generation == object.generation;
}

void trigger_local_hit_distortion(
	World& world,
	const WorldObject& target)
{
	if (is_local_player(world, target)
		&& world.player_hit_distortion_triggers != UINT8_MAX)
	{
		++world.player_hit_distortion_triggers;
	}
}

void publish_schematic_hit(
	World& world,
	const WorldObject& target,
	std::uint8_t bank)
{
	if (is_local_player(world, target))
	{
		world.player_schematic_hits[bank] = 1;
	}
	const WorldObject* selected =
		world_resolve(world, world.selected_target);
	if (selected == &target)
	{
		world.target_schematic_hits[bank] = 1;
	}
}

void add_local_impact_feedback(
	World& world,
	const WorldObject& target,
	float damage,
	std::uint8_t cause,
	bool enabled)
{
	if (!enabled || !is_local_player(world, target))
	{
		return;
	}
	const float contribution = damage * kImpactFeedbackScale;
	if (cause == 0)
	{
		// The directionless projectile branch only changes values below one
		// and caps that branch at one.
		if (world.player_camera_disturbance < 1.0f)
		{
			world.player_camera_disturbance = std::min(
				1.0f,
				world.player_camera_disturbance + contribution);
		}
		return;
	}
	// Direction-bearing collision causes retain their larger two-point cap.
	world.player_camera_disturbance = std::min(
		2.0f,
		world.player_camera_disturbance + contribution);
}

bool local_owns_damage(
	const World& world,
	const mission::Runtime& mission,
	std::uint16_t target_index,
	std::uint16_t attacker_index,
	std::uint8_t cause)
{
	if (mission.network.role == mission::NetworkRole::offline)
	{
		return true;
	}
	std::uint16_t owner = attacker_index;
	if (cause == 2)
	{
		// The rigid-collision path is owned by the lower live index unless
		// the attacker is compound, in which case the victim owns it.
		owner = target_index;
		if (attacker_index < kMaxGameObjects
			&& (world.objects[attacker_index].runtime_flags
				& kObjectFlagCompound) == 0)
		{
			owner = std::min(target_index, attacker_index);
		}
	}
	return owner < kMaxGameObjects
		&& mission::network_local_owns_object(
			mission.network,
			owner,
			mission.player_prefix_count,
			world.player.index);
}

bool same_deathmatch_team(
	const mission::Runtime& mission,
	std::uint16_t target_index,
	std::uint16_t attacker_index)
{
	return mission::network_is_deathmatch_mission(
			mission.mission_number)
		&& mission.network.team_mode
		&& attacker_index < mission::kNetworkPlayerCapacity
		&& target_index < kMaxGameObjects
		&& mission.network.object_team[target_index]
			== mission.network.object_team[attacker_index];
}

const WorldObject* damage_attacker(
	const World& world,
	std::uint16_t attacker_index)
{
	return attacker_index < kMaxGameObjects
		? &world.objects[attacker_index]
		: nullptr;
}

void select_damage_target(
	World& world,
	WorldObject& target,
	std::int16_t component)
{
	const auto target_index = static_cast<std::uint16_t>(
		&target - std::begin(world.objects));
	world.selected_target = {
		target_index,
		target.generation,
	};
	world.target_component = component;
	if (WorldObject* player = world_resolve(world, world.player))
	{
		player->selected_target_index = target_index;
		player->selected_target_component = component;
		if (player->ai.command_count != 0
			&& player->ai.commands[0].id == 100)
		{
			ai::Command& command = player->ai.commands[0];
			command.target_kind = ai::TargetKind::world_object;
			command.target = target_index;
			command.target_component = component;
		}
	}
	world.target_panel_refresh_requested = true;
}

void damage_owner_tail(
	World& world,
	WorldObject& target,
	std::uint8_t bank,
	std::uint16_t attacker_index,
	std::uint8_t cause,
	bool full_target_set,
	bool publish_schematic)
{
	if (attacker_index == world.player.index
		&& world.smart_target_enabled
		&& cause != 2)
	{
		select_damage_target(
			world,
			target,
			full_target_set
				? -1
				: world.target_component);
	}
	if (publish_schematic)
	{
		publish_schematic_hit(world, target, bank);
		const auto target_index = static_cast<std::uint16_t>(
			&target - std::begin(world.objects));
		if (world.selected_target.index == target_index
			&& world.selected_target.generation == target.generation)
		{
			world.target_panel_refresh_requested = true;
		}
	}
	if (!world.damage_event_suppressed)
	{
		world_emit_mission_event(
			world,
			WorldMissionEventType::shot_at,
			target,
			damage_attacker(world, attacker_index),
			UINT8_MAX);
	}
}
}

void damage_select_target(
	World& world,
	WorldObject& target,
	std::int16_t component)
{
	select_damage_target(
		world,
		target,
		component);
}

float scale_damage_by_difficulty(
	const World& world,
	const mission::Runtime& mission,
	const WorldObject& target,
	float raw_damage,
	std::uint16_t attacker_index)
{
	// GameObject_scale_damage_by_difficulty, LANCER.EXE 0x00463d70:
	// the global network-session byte bypasses difficulty in every network
	// mode, including cooperative missions.
	if (mission.network.role != mission::NetworkRole::offline)
	{
		return raw_damage;
	}

	const std::uint8_t difficulty =
		std::min<std::uint8_t>(mission.difficulty, 2);
	float scaled = raw_damage;
	// GameObject_apply_primary/structural_bank_damage place the victim's
	// live slot in ECX and the attacker's live slot in EDX before this call.
	if (attacker_index == world.player.index
		&& target.allegiance_class == 1)
	{
		if (difficulty == 0)
		{
			scaled *= kEasyOutgoingDamageScale;
		}
		else if (difficulty == 2)
		{
			scaled *= kHardOutgoingDamageScale;
		}
	}
	if (is_local_player(world, target))
	{
		if (difficulty == 0)
		{
			scaled *= kEasyIncomingDifficultyScale;
		}
		else if (difficulty == 2)
		{
			scaled *= kHardIncomingDifficultyScale;
		}
		scaled *= kIncomingDamageBaseScale;
	}
	return scaled;
}

bool apply_primary_bank_damage(
	World& world,
	mission::Runtime& mission,
	WorldObject& target,
	const assets::ShipStatsTable& stats,
	std::uint8_t bank,
	float raw_damage,
	float structural_ratio,
	std::uint16_t attacker_index,
	std::uint8_t cause,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick)
{
	if (bank >= 4
		|| (target.runtime_flags & kObjectFlagSimulationSuspended) != 0)
	{
		return false;
	}
	trigger_local_hit_distortion(world, target);
	if (target.collision_class == 6)
	{
		return false;
	}

	const std::uint16_t target_index = static_cast<std::uint16_t>(
		&target - std::begin(world.objects));
	const float structural_input =
		target.primary_shields[bank] <= raw_damage
			? raw_damage - target.primary_shields[bank]
			: 0.0f;
	const float primary_damage = scale_damage_by_difficulty(
		world, mission, target, raw_damage, attacker_index);
	const bool local_friendly_hit =
		attacker_index == world.player.index
		&& target.allegiance_class == 0
		&& (cause == 0 || cause == 5 || cause == 2);
	if (local_friendly_hit)
	{
		ai::friendly_fire_accumulate_damage(
			world,
			mission,
			target,
			primary_damage,
			simulation_tick);
	}
	if (is_local_player(world, target))
	{
		if (cause == 0 || cause == 1 || cause == 5)
		{
			mission::player_comms_on_player_damaged(
				mission,
				world,
				attacker_index,
				simulation_tick);
		}
		add_local_impact_feedback(
			world,
			target,
			primary_damage,
			cause,
			impact_feedback_enabled);
	}
	if (cause == 0 || cause == 1 || cause == 5)
	{
		target.attack_pressure += primary_damage;
	}

	const bool damage_owned = local_owns_damage(
		world,
		mission,
		target_index,
		attacker_index,
		cause);
	if (damage_owned
		&& !same_deathmatch_team(
			mission, target_index, attacker_index))
	{
		if (target.primary_shields[bank] >= 0.0f
			&& target.protection_state != 4)
		{
			target.primary_shields[bank] -= primary_damage;
		}
		if (target.primary_shields[bank] < 0.0f)
		{
			(void)apply_structural_bank_damage(
				world,
				mission,
				target,
				stats,
				bank,
				structural_input * structural_ratio,
				attacker_index,
				cause,
				impact_feedback_enabled,
				simulation_tick);
		}
		target.last_attacker_index = attacker_index;
		if (mission.network.role != mission::NetworkRole::offline
			&& target.protection_state != 4)
		{
			// GameObject_apply_primary_bank_damage publishes only after
			// the local damage owner has stored the final attacker and
			// completed any nested structural-bank damage
			// (LANCER.EXE 0x004640d7..0x004640fe). A bank which was
			// already negative still publishes the attempted primary
			// damage; protection state four is the sole publication gate.
			(void)mission::network_defer_bank_damage(
				mission.network,
				target,
				stats,
				target_index,
				mission.player_prefix_count,
				world.player.index,
				bank,
				primary_damage);
		}
	}

	// Target selection, status-panel/schematic publication, and ShotAt are
	// deliberately retained after network-owner and same-team suppression.
	damage_owner_tail(
		world,
		target,
		bank,
		attacker_index,
		cause,
		true,
		true);
	return true;
}

bool apply_structural_bank_damage(
	World& world,
	mission::Runtime& mission,
	WorldObject& target,
	const assets::ShipStatsTable& stats,
	std::uint8_t bank,
	float raw_damage,
	std::uint16_t attacker_index,
	std::uint8_t cause,
	bool impact_feedback_enabled,
	std::uint32_t simulation_tick)
{
	if (bank >= 4
		|| (target.runtime_flags & kObjectFlagSimulationSuspended) != 0)
	{
		return false;
	}
	trigger_local_hit_distortion(world, target);
	if (target.collision_class == 6)
	{
		return false;
	}

	const std::uint16_t target_index = static_cast<std::uint16_t>(
		&target - std::begin(world.objects));
	const float feedback_damage = scale_damage_by_difficulty(
		world, mission, target, raw_damage, attacker_index);
	const bool local_friendly_hit =
		mission.network.role == mission::NetworkRole::offline
		&& attacker_index == world.player.index
		&& target.allegiance_class == 0
		&& (target.runtime_flags & kObjectFlagDestroyed) == 0
		&& (cause == 0 || cause == 5 || cause == 2);
	if (local_friendly_hit)
	{
		ai::friendly_fire_accumulate_damage(
			world,
			mission,
			target,
			feedback_damage,
			simulation_tick);
	}
	if (is_local_player(world, target)
		&& (cause == 0 || cause == 1 || cause == 5))
	{
		mission::player_comms_on_player_damaged(
			mission,
			world,
			attacker_index,
			simulation_tick);
	}
	add_local_impact_feedback(
		world,
		target,
		feedback_damage,
		cause,
		impact_feedback_enabled);
	if (cause == 0 || cause == 1 || cause == 5)
	{
		target.attack_pressure += feedback_damage;
	}

	const bool damage_owned = local_owns_damage(
			world,
			mission,
			target_index,
			attacker_index,
			cause);
	if (!damage_owned
		|| same_deathmatch_team(
			mission, target_index, attacker_index))
	{
		damage_owner_tail(
			world,
			target,
			bank,
			attacker_index,
			cause,
			false,
			true);
		return true;
	}
	if ((target.runtime_flags & kObjectFlagDestroyed) != 0)
	{
		return false;
	}
	// Cause-zero damage to a compound root is routed through component
	// damage by the retail owner and never mutates a root structural bank.
	if (cause == 0
		&& (target.runtime_flags & kObjectFlagCompound) != 0)
	{
		// This jump bypasses smart targeting and schematic publication, but
		// reaches 0x004645a5 after, rather than through, the global event
		// suppressor test.
		world_emit_mission_event(
			world,
			WorldMissionEventType::shot_at,
			target,
			damage_attacker(world, attacker_index),
			UINT8_MAX);
		return true;
	}

	float structural_damage = scale_damage_by_difficulty(
		world, mission, target, feedback_damage, attacker_index);
	const float updated_structure =
		target.secondary_shields[bank] - structural_damage;
	const bool protected_negative =
		updated_structure < 0.0f
		&& (target.protection_state == 2
			|| (target.protection_state == 1
				// Retail retains the no-attacker sentinel as signed -1;
				// it is below the player-prefix boundary, not an AI slot.
				&& attacker_index != UINT16_MAX
				&& attacker_index
					>= mission.player_prefix_count));
	if (target.protection_state == 4 || protected_negative)
	{
		// Retail also clears the local damage variable. The death selector's
		// force-explosion argument therefore remains false if a previously
		// negative protected bank reaches this path.
		structural_damage = 0.0f;
	}
	else
	{
		target.secondary_shields[bank] = updated_structure;
	}
	world_update_shield_ratios(target, stats);
	target.last_attacker_index = attacker_index;
	if (mission.network.role != mission::NetworkRole::offline
		&& structural_damage > 0.0f)
	{
		// The structural owner stores its post-protection damage in bank
		// slots four through seven. Protected crossings clear the local
		// damage value and therefore deliberately publish nothing
		// (LANCER.EXE 0x004643e5..0x0046441f).
		(void)mission::network_defer_bank_damage(
			mission.network,
			target,
			stats,
			target_index,
			mission.player_prefix_count,
			world.player.index,
			static_cast<std::uint8_t>(bank + 4),
			structural_damage);
	}

	if (target.secondary_shields[bank] < 0.0f
		&& (mission.network.role == mission::NetworkRole::offline
			|| mission::network_local_owns_object(
				mission.network,
				target_index,
				mission.player_prefix_count,
				world.player.index)))
	{
		(void)ai::schedule_death_command(
			target,
			world,
			mission,
			1,
			structural_damage > kForcedPlayerExplosionDamage);
	}
	if (target.secondary_shields[bank] < 0.0f
		&& local_friendly_hit
		&& ((target.runtime_flags & 0x00000800u) == 0
			|| cause != 2
			|| target.protection_state == 1))
	{
		if (WorldObject* player =
			world_resolve(world, world.player))
		{
			ai::friendly_fire_flag_offender(
				*player,
				mission,
				target.protection_state == 1);
		}
	}
	damage_owner_tail(
		world,
		target,
		bank,
		attacker_index,
		cause,
		false,
		true);
	return true;
}
}
