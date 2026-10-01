#include "hud/runtime.hpp"

#include "assets/pilot_presentation.hpp"
#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "frontend/gui.hpp"
#include "game/weapons.hpp"
#include "hud/layout.hpp"
#include "mission/objectives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace sl_open::hud
{
namespace
{
constexpr std::int32_t kPanelDurations[kPanelCount] = {
	400, 1200, 200, 2000, 1000,
	200, 200, 1000, 2000, 1000,
	1000, 1500, 1000, 1000, 2000,
};
constexpr float kTargetLimit = 660000.0f;

bool decode_utf8_codepoint(
	const std::uint8_t*& cursor,
	const std::uint8_t* end,
	std::uint32_t& codepoint)
{
	if (cursor >= end)
	{
		return false;
	}
	const std::uint8_t lead = *cursor++;
	if (lead < 0x80u)
	{
		codepoint = lead;
		return true;
	}
	std::uint8_t continuation_count = 0;
	std::uint32_t minimum = 0;
	if ((lead & 0xe0u) == 0xc0u)
	{
		codepoint = lead & 0x1fu;
		continuation_count = 1;
		minimum = 0x80u;
	}
	else if ((lead & 0xf0u) == 0xe0u)
	{
		codepoint = lead & 0x0fu;
		continuation_count = 2;
		minimum = 0x800u;
	}
	else if ((lead & 0xf8u) == 0xf0u)
	{
		codepoint = lead & 0x07u;
		continuation_count = 3;
		minimum = 0x10000u;
	}
	else
	{
		return false;
	}
	for (std::uint8_t index = 0;
		index < continuation_count;
		++index)
	{
		if (cursor >= end
			|| (*cursor & 0xc0u) != 0x80u)
		{
			return false;
		}
		codepoint = (codepoint << 6) | (*cursor++ & 0x3fu);
	}
	return codepoint >= minimum
		&& codepoint <= 0x10ffffu
		&& (codepoint < 0xd800u || codepoint > 0xdfffu);
}

bool windows_1252_character(
	std::uint32_t codepoint,
	std::uint8_t& character)
{
	if ((codepoint >= 0x20u && codepoint <= 0x7eu)
		|| (codepoint >= 0xa0u && codepoint <= 0xffu))
	{
		character = static_cast<std::uint8_t>(codepoint);
		return true;
	}
	struct Mapping
	{
		std::uint16_t codepoint;
		std::uint8_t character;
	};
	static constexpr Mapping kMappings[] = {
		{0x20acu, 0x80u}, {0x201au, 0x82u},
		{0x0192u, 0x83u}, {0x201eu, 0x84u},
		{0x2026u, 0x85u}, {0x2020u, 0x86u},
		{0x2021u, 0x87u}, {0x02c6u, 0x88u},
		{0x2030u, 0x89u}, {0x0160u, 0x8au},
		{0x2039u, 0x8bu}, {0x0152u, 0x8cu},
		{0x017du, 0x8eu}, {0x2018u, 0x91u},
		{0x2019u, 0x92u}, {0x201cu, 0x93u},
		{0x201du, 0x94u}, {0x2022u, 0x95u},
		{0x2013u, 0x96u}, {0x2014u, 0x97u},
		{0x02dcu, 0x98u}, {0x2122u, 0x99u},
		{0x0161u, 0x9au}, {0x203au, 0x9bu},
		{0x0153u, 0x9cu}, {0x017eu, 0x9eu},
		{0x0178u, 0x9fu},
	};
	for (const Mapping& mapping : kMappings)
	{
		if (mapping.codepoint == codepoint)
		{
			character = mapping.character;
			return true;
		}
	}
	return false;
}

std::uint16_t advance_distortion_random(Runtime& runtime)
{
	// LANCER.EXE imports the MSVC CRT rand implementation at 0x004cf555.
	runtime.distortion_random_seed =
		runtime.distortion_random_seed * 0x343fdu + 0x269ec3u;
	return static_cast<std::uint16_t>(
		(runtime.distortion_random_seed >> 16) & 0x7fffu);
}

void queue_sound(Runtime& runtime, std::uint8_t event)
{
	// HUD_play_ui_sound, LANCER.EXE 0x0048ce70..0x0048cead, rejects
	// every event in its six-entry HUD table outside camera modes 0..3.
	if (runtime.sound_camera_mode > 3)
	{
		return;
	}
	if (runtime.sound_count >= std::size(runtime.sound_events))
	{
		return;
	}
	const std::uint8_t slot = static_cast<std::uint8_t>(
		(runtime.sound_read + runtime.sound_count)
		% std::size(runtime.sound_events));
	runtime.sound_events[slot] = {
		static_cast<std::uint8_t>(15 + event),
		60,
	};
	++runtime.sound_count;
}

void queue_target_sound(
	Runtime& runtime,
	const game::World& world,
	std::uint8_t event)
{
	// Target actions carry the live world camera explicitly; publish it
	// before entering the same common HUD event player as every other UI.
	runtime.sound_camera_mode = world.camera_mode;
	queue_sound(runtime, event);
}

void queue_sample(Runtime& runtime, std::uint8_t sample)
{
	if (runtime.sound_count >= std::size(runtime.sound_events))
	{
		return;
	}
	const std::uint8_t slot = static_cast<std::uint8_t>(
		(runtime.sound_read + runtime.sound_count)
		% std::size(runtime.sound_events));
	runtime.sound_events[slot] = {sample, 127};
	++runtime.sound_count;
}

void rebuild_ordnance(Runtime& runtime, const game::WorldObject& player)
{
	for (OrdnanceEntry& entry : runtime.ordnance)
	{
		entry = {};
		entry.count = -1;
	}
	runtime.ordnance_count = 0;
	constexpr std::int16_t shapes[9] = {
		86, 76, 36, 66, 26, 106, 96, 56, 46,
	};
	constexpr std::int16_t language_ids[9] = {
		292, 291, 289, 286, 295, 294, 293, 288, 290,
	};
	for (std::uint8_t slot = 0; slot < player.attachment_count; ++slot)
	{
		const game::AttachmentSlot& attachment = player.attachments[slot];
		const std::int16_t type = attachment.definition_index;
		if (type == 10 || type < 0 || type > 8)
		{
			continue;
		}
		OrdnanceEntry* destination = nullptr;
		for (std::uint8_t index = 0;
			index < runtime.ordnance_count;
			++index)
		{
			if (runtime.ordnance[index].type == type)
			{
				destination = &runtime.ordnance[index];
				break;
			}
		}
		if (destination == nullptr)
		{
			if (runtime.ordnance_count == std::size(runtime.ordnance))
			{
				break;
			}
			destination = &runtime.ordnance[runtime.ordnance_count++];
			destination->count = 0;
			destination->shape = shapes[type];
			destination->language_id = language_ids[type];
			destination->type = type;
		}
		destination->count = static_cast<std::int16_t>(
			destination->count + attachment.remaining_count);
	}
	runtime.aggregate_ordnance = 0;
	for (std::uint8_t index = 0;
		index < runtime.ordnance_count;
		++index)
	{
		runtime.aggregate_ordnance += runtime.ordnance[index].count;
	}
	runtime.selected_ordnance =
		static_cast<std::uint8_t>(runtime.ordnance_count / 2);
	for (std::uint8_t index = 0;
		index < runtime.ordnance_count;
		++index)
	{
		std::int16_t position = static_cast<std::int16_t>(
			runtime.selected_ordnance - index);
		if (position < 0)
		{
			position += 10;
		}
		runtime.ordnance[index].ring_position = position;
	}
	runtime.ordnance_initialized = true;
	diagnostics::mission_log(
		"hud ordnance types=%u selected=%d count=%d aggregate=%d",
		static_cast<unsigned>(runtime.ordnance_count),
		runtime.ordnance_count == 0
			? -1
			: runtime.ordnance[runtime.selected_ordnance].type,
		runtime.ordnance_count == 0
			? 0
			: runtime.ordnance[runtime.selected_ordnance].count,
		runtime.aggregate_ordnance);
}

void rotate_ordnance(
	Runtime& runtime,
	game::World& world,
	std::int16_t delta)
{
	// Player_process_discrete_input 0x0048c0a4..0x0048c0b9 and
	// 0x0048c1f4..0x0048c209 keep the ordnance panel open while either
	// cycle command is used, including an unsuccessful boundary attempt.
	if (runtime_open_panel(runtime, 2))
	{
		runtime.panels[2].hold = true;
	}

	bool changed = false;
	if (delta > 0)
	{
		std::uint8_t next = static_cast<std::uint8_t>(
			runtime.selected_ordnance + 1);
		if (next > 9)
		{
			next = 0;
		}
		changed = runtime.ordnance[next].count != -1;
	}
	else
	{
		changed = runtime.selected_ordnance != 0;
	}

	if (changed)
	{
		for (std::uint8_t index = 0;
			index < std::size(runtime.ordnance);
			++index)
		{
			OrdnanceEntry& entry = runtime.ordnance[index];
			if (entry.count == -1)
			{
				continue;
			}
			entry.ring_position = static_cast<std::int16_t>(
				entry.ring_position + delta);
			if (entry.ring_position > 9)
			{
				entry.ring_position = 0;
			}
			else if (entry.ring_position < 0)
			{
				entry.ring_position = 9;
			}
			if (entry.ring_position == 0)
			{
				runtime.selected_ordnance = index;
				// Both retail rotation loops call Sound_play on definition
				// 0x4b at the local player when the new slot reaches zero.
				game::world_queue_sound_object(
					world, world.player, 0x4b, 0);
			}
		}
	}
	queue_sound(runtime, changed ? 0 : 3);

	constexpr std::uint8_t betty_samples[9] = {
		2, 8, 3, 4, 7, 5, 9, 6, 10,
	};
	const OrdnanceEntry& selected =
		runtime.ordnance[runtime.selected_ordnance];
	if (selected.count != -1
		&& selected.type >= 0
		&& static_cast<std::size_t>(selected.type)
			< std::size(betty_samples))
	{
		queue_sample(runtime, betty_samples[selected.type]);
	}
	diagnostics::mission_log(
		"hud ordnance selected type=%d count=%d changed=%u",
		selected.type,
		selected.count,
		changed ? 1u : 0u);
}

bool project_center(
	const game::CameraRuntime& camera,
	const game::WorldObject& object,
	std::uint32_t width,
	std::uint32_t height,
	float& x,
	float& y,
	float& depth)
{
	if (width == 0 || height == 0)
	{
		return false;
	}
	const glm::vec3 camera_space =
		glm::transpose(camera.orientation)
		* (object.scene_position - camera.position);
	depth = camera_space.z;
	if (camera_space.z <= 0.0f)
	{
		return false;
	}
	const glm::vec2 framebuffer = math::camera_plane_to_framebuffer(
		{
			camera_space.x / camera_space.z,
			camera_space.y / camera_space.z,
		},
		{camera.horizontal_tangent, camera.vertical_tangent},
		{static_cast<float>(width), static_cast<float>(height)});
	x = framebuffer.x;
	y = framebuffer.y;
	return true;
}

bool first_command_is_player_control(
	const game::WorldObject* player)
{
	return player != nullptr
		&& player->ai.command_count != 0
		&& player->ai.commands[0].id == 100;
}

ai::Command* player_target_command(game::WorldObject* player)
{
	if (player == nullptr)
	{
		return nullptr;
	}
	for (std::uint8_t index = 0;
		index < player->ai.command_count;
		++index)
	{
		if (player->ai.commands[index].id == 100)
		{
			return &player->ai.commands[index];
		}
	}
	return nullptr;
}

bool target_reference_valid(
	const game::World& world,
	game::ObjectHandle handle,
	std::int16_t component,
	std::uint32_t allowed_runtime_flags)
{
	// TargetRef_is_valid, LANCER.EXE 0x00401870. Object flag 0x200 is
	// required; the allowance mask selectively admits otherwise rejected
	// runtime states. Component validity does not require targetable 0x2000.
	const game::WorldObject* target =
		game::world_resolve(world, handle);
	if (target == nullptr
		|| (target->runtime_flags & 0x00000200u) == 0
		|| (target->runtime_flags
			& ~allowed_runtime_flags
			& 0x10000d40u) != 0)
	{
		return false;
	}
	return component < 0
		|| (component < target->component_count
			&& target->components[component].model_reference >= 0
			&& static_cast<std::size_t>(
				target->components[component].model_reference)
				< target->model_references.size()
			&& (target->components[component].runtime_flags
				& 0x0030u) == 0);
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

game::ObjectHandle raw_target_handle(
	const game::World& world,
	std::uint16_t index)
{
	if (index >= std::size(world.objects))
	{
		return {index, 0};
	}
	return {index, world.objects[index].generation};
}

void write_target_reference(
	game::World& world,
	game::WorldObject& player,
	game::ObjectHandle handle,
	std::int16_t component)
{
	world.selected_target = handle;
	world.target_component = component;
	player.selected_target_index = handle.index;
	player.selected_target_component = component;
	if (ai::Command* command = player_target_command(&player))
	{
		command->target_kind = ai::TargetKind::world_object;
		command->target = handle.index;
		command->target_component = component;
	}
}

const game::WorldObject* resolve_target(const game::World& world)
{
	return game::world_resolve(world, world.selected_target);
}

glm::vec3 target_scene_point(
	const game::WorldObject& target,
	std::int16_t component)
{
	if (component >= 0 && component < target.component_count)
	{
		const std::int16_t model_reference =
			target.components[component].model_reference;
		if (model_reference >= 0
			&& static_cast<std::size_t>(model_reference)
				< target.model_references.size())
		{
			// TargetRef_resolve_model returns the live articulated scene node;
			// the lock mesh is transformed about that node's origin.
			return target.scene_position + target.scene_orientation
				* glm::vec3(
					target.model_references[model_reference].scene_transform[3]);
		}
	}
	return target.scene_position;
}

const OrdnanceEntry* selected_ordnance(const Runtime& runtime)
{
	if (runtime.selected_ordnance >= runtime.ordnance_count)
	{
		return nullptr;
	}
	return &runtime.ordnance[runtime.selected_ordnance];
}

bool missile_lock_candidate_valid(
	const Runtime& runtime,
	const game::World& world,
	const assets::MissileStatsTable& stats,
	bool local_missile_has_target)
{
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	const game::WorldObject* target = resolve_target(world);
	const OrdnanceEntry* ordnance = selected_ordnance(runtime);
	if (player == nullptr || target == nullptr || ordnance == nullptr
		// HUD_missile_lock_candidate_valid 0x004913a9..0x004913c6
		// rejects the local player while DisableMissiles flag 0x10000 is
		// set, independently of the selected target's TargetRef flags.
		|| (player->runtime_flags & 0x00010000u) != 0
		|| (ordnance->count <= 0 && !local_missile_has_target)
		|| ordnance->type < 0
		|| ordnance->type == 6
		|| static_cast<std::size_t>(ordnance->type)
			>= assets::kMissileStatsCount
		// HUD_missile_lock_candidate_valid 0x0049138f calls
		// TargetRef_is_valid with an allowance mask of zero.
		|| !target_reference_valid(
			world,
			world.selected_target,
			world.target_component,
			0)
		// HUD_missile_lock_candidate_valid 0x004913c7..0x00491413
		// admits every allegiance and ordnance type zero in deathmatch.
		|| (!runtime.deathmatch_mission
			&& target->allegiance_class != 1)
		|| (!runtime.deathmatch_mission
			&& ordnance->type == 0)
		|| !stats.ready)
	{
		return false;
	}

	const glm::vec3 delta =
		target_scene_point(*target, world.target_component)
			- player->scene_position;
	const float distance_squared = glm::dot(delta, delta);
	const float lock_range = stats.records[ordnance->type].lock_range;
	if (distance_squared <= 0.0f
		|| distance_squared > lock_range * lock_range)
	{
		return false;
	}
	return glm::dot(
		delta / std::sqrt(distance_squared),
		player->scene_orientation[2]) >= 0.7f;
}

bool missile_lock_subject_unchanged(
	const Runtime& runtime,
	const game::World& world)
{
	const OrdnanceEntry* ordnance = selected_ordnance(runtime);
	return ordnance != nullptr
		&& runtime.missile_lock.target == world.selected_target
		&& runtime.missile_lock.component == world.target_component
		&& runtime.missile_lock.ordnance_type == ordnance->type;
}

const char* missile_lock_phase_name(MissileLockPhase phase)
{
	switch (phase)
	{
	case MissileLockPhase::idle: return "idle";
	case MissileLockPhase::opening: return "opening";
	case MissileLockPhase::acquiring: return "acquiring";
	case MissileLockPhase::locked: return "locked";
	case MissileLockPhase::forced_release: return "forced-release";
	case MissileLockPhase::closing: return "closing";
	}
	return "unknown";
}

void begin_missile_lock_release(MissileLockState& lock)
{
	if (lock.acquisition_phase > 0)
	{
		lock.carried_rotation_degrees = lock.acquisition_phase;
		lock.acquisition_phase = 0;
	}
	lock.rotation_degrees %= 90;
	if (lock.rotation_degrees == 0)
	{
		lock.rotation_degrees = 90;
	}
	lock.phase = MissileLockPhase::closing;
}

void update_missile_lock(
	Runtime& runtime,
	const game::World& world,
	const assets::MissileStatsTable& stats,
	bool local_missile_has_target,
	std::uint32_t simulation_steps)
{
	MissileLockState& lock = runtime.missile_lock;
	const MissileLockPhase phase_at_entry = lock.phase;
	const std::int32_t delta =
		static_cast<std::int32_t>(simulation_steps);
	const bool valid =
		missile_lock_candidate_valid(
			runtime, world, stats, local_missile_has_target);
	const bool unchanged =
		missile_lock_subject_unchanged(runtime, world);
	switch (lock.phase)
	{
	case MissileLockPhase::idle:
		// HUD_update_missile_lock_ring state zero writes 100 on every
		// update before testing whether a lock candidate exists. The
		// targeting overlay shares this value for its bracket opacity and
		// lead-line shortening even while the ring itself is idle.
		lock.opening_percent = 100;
		if (valid)
		{
			const OrdnanceEntry& ordnance =
				*selected_ordnance(runtime);
			lock.opening_percent = 100;
			lock.acquisition_phase =
				-stats.records[ordnance.type].lock_ticks;
			lock.rotation_degrees = 0;
			lock.carried_rotation_degrees = 0;
			lock.target = world.selected_target;
			lock.component = world.target_component;
			lock.ordnance_type = ordnance.type;
			lock.phase = MissileLockPhase::opening;
		}
		break;
	case MissileLockPhase::opening:
		if (!valid || !unchanged)
		{
			begin_missile_lock_release(lock);
			break;
		}
		lock.opening_percent -= delta;
		lock.acquisition_phase += delta;
		if (lock.opening_percent <= 0)
		{
			lock.opening_percent = 0;
			lock.phase = MissileLockPhase::acquiring;
		}
		break;
	case MissileLockPhase::acquiring:
		if (!valid || !unchanged)
		{
			begin_missile_lock_release(lock);
			break;
		}
		lock.acquisition_phase += delta;
		if (lock.acquisition_phase > -50)
		{
			lock.rotation_degrees += delta;
		}
		if (lock.acquisition_phase >= 0)
		{
			lock.phase = MissileLockPhase::locked;
		}
		break;
	case MissileLockPhase::locked:
		if (!valid || !unchanged)
		{
			begin_missile_lock_release(lock);
			break;
		}
		lock.acquisition_phase += delta;
		lock.rotation_degrees += delta;
		break;
	case MissileLockPhase::forced_release:
		begin_missile_lock_release(lock);
		break;
	case MissileLockPhase::closing:
		lock.opening_percent += delta;
		lock.rotation_degrees =
			std::min(90, lock.rotation_degrees + delta);
		lock.acquisition_phase -= delta;
		if (lock.opening_percent > 99)
		{
			lock = {};
		}
		break;
	}

	// The active retail states reach 0x00491a41 and refresh the retained
	// TargetRef scene point after their state transition. Release enters
	// state five first and skips that refresh, so a changed/invalid target
	// closes the ring around the last accepted point.
	if (valid
		&& (lock.phase == MissileLockPhase::opening
			|| lock.phase == MissileLockPhase::acquiring
			|| lock.phase == MissileLockPhase::locked))
	{
		const game::WorldObject* target = resolve_target(world);
		if (target != nullptr)
		{
			lock.retained_point =
				target_scene_point(*target, world.target_component);
		}
	}

	// HUD_update_missile_lock_ring checks the state before dispatching its
	// state handler. This intentionally starts and stops the protected tone
	// one update after entering or leaving state one.
	lock.tone_active =
		phase_at_entry == MissileLockPhase::opening;
	if (lock.phase != phase_at_entry)
	{
		const bool new_subject =
			lock.target != runtime.missile_lock_log_target
			|| lock.ordnance_type != runtime.missile_lock_log_type;
		const bool significant_transition =
			lock.phase == MissileLockPhase::acquiring
			|| lock.phase == MissileLockPhase::locked
			|| (lock.phase == MissileLockPhase::opening && new_subject)
			|| (lock.phase == MissileLockPhase::closing
				&& phase_at_entry == MissileLockPhase::locked);
		if (significant_transition)
		{
			diagnostics::mission_log(
				"hud missile-lock phase=%s target=%d component=%d type=%d",
				missile_lock_phase_name(lock.phase),
				lock.target.index == UINT16_MAX
					? -1
					: static_cast<int>(lock.target.index),
				lock.component,
				lock.ordnance_type);
			runtime.missile_lock_log_target = lock.target;
			runtime.missile_lock_log_type = lock.ordnance_type;
		}
	}
}

void close_target_panels(Runtime& runtime)
{
	runtime_close_panel(runtime, 3);
	runtime_close_panel(runtime, 8);
}

void open_target_panel(
	Runtime& runtime,
	const game::WorldObject& target,
	bool hold)
{
	const std::uint8_t wanted = target.object_class == 1 ? 8 : 3;
	const std::uint8_t other = wanted == 8 ? 3 : 8;
	runtime_close_panel(runtime, other);
	if (runtime_open_panel(runtime, wanted))
	{
		// Selection must replace a closing panel's retained snapshot in the
		// same frame. Waiting for the ordinary 60-tick transition makes the
		// selected target appear at a variable delay based on panel phase.
		PanelState& panel = runtime.panels[wanted];
		panel.animation = PanelAnimation::open;
		panel.animation_time = 60;
		if (hold)
		{
			panel.hold = true;
		}
	}
}

void refresh_target_panel(Runtime& runtime, game::World& world)
{
	const game::WorldObject* target =
		game::world_resolve(world, world.selected_target);
	if (target == nullptr)
	{
		close_target_panels(runtime);
		runtime.previous_target = {};
		return;
	}
	open_target_panel(runtime, *target, true);
	runtime.previous_target = world.selected_target;
}

void publish_target_reference(
	Runtime& runtime,
	game::World& world,
	game::WorldObject& player,
	game::ObjectHandle target_handle,
	std::int16_t component)
{
	write_target_reference(
		world, player, target_handle, component);
	diagnostics::mission_log(
		"hud target object=%d component=%d",
		target_handle.index == UINT16_MAX
			? -1
			: static_cast<int>(target_handle.index),
		component);
	refresh_target_panel(runtime, world);
}

void cycle_target_component(
	Runtime& runtime,
	game::World& world,
	int direction)
{
	game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (!first_command_is_player_control(player))
	{
		return;
	}
	game::WorldObject* target =
		game::world_resolve(world, world.selected_target);
	if (target == nullptr
		|| (target->runtime_flags & game::kObjectFlagCompound) == 0
		|| target->allegiance_class == 0)
	{
		queue_target_sound(runtime, world, 3);
		return;
	}

	// Player_cycle_target_submodel opens only the matching closed target
	// panel. It does not close the opposite panel or force either hold bit.
	if (target->object_class == 1
		&& runtime.panels[8].animation == PanelAnimation::closed)
	{
		runtime_open_panel(runtime, 8);
	}
	else if (target->object_class == 0
		&& runtime.panels[3].animation == PanelAnimation::closed)
	{
		runtime_open_panel(runtime, 3);
	}

	// With no component records retail leaves the current component word
	// untouched and still reports a successful enemy subtarget command.
	if (target->component_count != 0)
	{
		std::int32_t component = world.target_component;
		bool found = false;
		for (std::uint8_t attempt = 0;
			attempt < target->component_count;
			++attempt)
		{
			if (direction > 0)
			{
				++component;
				if (component == target->component_count)
				{
					component = 0;
				}
			}
			else
			{
				if (component < 1)
				{
					component = target->component_count;
				}
				--component;
			}
			const game::ObjectComponent& candidate =
				target->components[component];
			if ((candidate.runtime_flags & 0x2000u) == 0
				|| (candidate.runtime_flags & 0x0030u) != 0)
			{
				continue;
			}
			world.target_component =
				static_cast<std::int16_t>(component);
			found = true;
			diagnostics::mission_log(
				"hud subtarget object=%u component=%d type=%u health=%.0f/%.0f",
				static_cast<unsigned>(world.selected_target.index),
				component,
				candidate.model_type,
				candidate.health,
				candidate.maximum_health);
			break;
		}
		if (!found)
		{
			world.target_component = -1;
			diagnostics::mission_log(
				"hud subtarget object=%u component=none",
				static_cast<unsigned>(
					world.selected_target.index));
		}
		if (ai::Command* command = player_target_command(player))
		{
			command->target_component = world.target_component;
		}
		player->selected_target_component =
			world.target_component;
	}
	queue_target_sound(runtime, world, 0);
}

void select_nearest(
	Runtime& runtime,
	game::World& world,
	std::int16_t allegiance)
{
	game::WorldObject* player =
		game::world_resolve(world, world.player);
	if ((world.camera_mode != 0 && world.camera_mode != 4)
		|| !first_command_is_player_control(player))
	{
		return;
	}
	const game::WorldObject* nearest = nullptr;
	float nearest_distance = kTargetLimit;
	for (const game::WorldObject& object : world.objects)
	{
		if (!object.active
			|| &object == player
			|| object.type == 1001
			|| object.allegiance_class != allegiance
			|| (object.runtime_flags & game::kObjectFlagDestroyed) != 0
			|| (allegiance == 1
				&& (object.runtime_flags & 0x00000100u) != 0))
		{
			continue;
		}
		const float distance =
			glm::distance(
				object.scene_position,
				player->scene_position);
		if (distance < nearest_distance)
		{
			nearest = &object;
			nearest_distance = distance;
		}
	}
	if (nearest == nullptr)
	{
		queue_target_sound(runtime, world, 3);
		return;
	}
	// The input sound is queued before FUN_00415270 publishes the new
	// TargetRef and refreshes the corresponding panel.
	queue_target_sound(runtime, world, 0);
	publish_target_reference(
		runtime, world, *player, object_handle(world, *nearest), -1);
}

std::uint16_t collect_live_handles(
	const game::World& world,
	game::ObjectHandle* handles)
{
	std::uint16_t count = 0;
	for (const game::WorldObject& object : world.objects)
	{
		if (object.active)
		{
			handles[count++] = object_handle(world, object);
		}
	}
	return count;
}

bool cycle_target(
	Runtime& runtime,
	game::World& world,
	std::int16_t allegiance,
	int direction,
	bool hold_when_reopening)
{
	game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (!first_command_is_player_control(player))
	{
		return false;
	}

	if (runtime.panels[3].animation == PanelAnimation::closed
		&& runtime.panels[8].animation == PanelAnimation::closed
		&& target_reference_valid(
			world,
			world.selected_target,
			world.target_component,
			0))
	{
		const game::WorldObject* target =
			game::world_resolve(world, world.selected_target);
		queue_target_sound(runtime, world, 0);
		open_target_panel(
			runtime, *target, hold_when_reopening);
		return true;
	}

	game::ObjectHandle handles[game::kMaxGameObjects];
	const std::uint16_t count =
		collect_live_handles(world, handles);
	std::int32_t current = -1;
	for (std::uint16_t index = 0; index < count; ++index)
	{
		if (handles[index] == world.selected_target)
		{
			current = index;
			break;
		}
	}

	for (std::uint16_t attempt = 0; attempt < count; ++attempt)
	{
		current = direction > 0
			? (current + 1) % count
			: (current < 1 ? count - 1 : current - 1);
		const game::ObjectHandle handle = handles[current];
		const game::WorldObject& candidate =
			world.objects[handle.index];
		const std::uint32_t allowed_runtime_flags =
			candidate.allegiance_class == 0 ? 0x900u : 0x800u;
		if (!target_reference_valid(
				world, handle, -1, allowed_runtime_flags)
			|| handle.index == world.player.index
			|| candidate.allegiance_class != allegiance)
		{
			continue;
		}
		if (glm::distance(
				candidate.scene_position,
				player->scene_position) > kTargetLimit)
		{
			continue;
		}
		publish_target_reference(
			runtime, world, *player, handle, -1);
		queue_target_sound(runtime, world, 0);
		return true;
	}

	publish_target_reference(
		runtime, world, *player, {}, -1);
	queue_target_sound(runtime, world, 3);
	return true;
}

void cycle_torpedo(Runtime& runtime, game::World& world)
{
	game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (player_target_command(player) == nullptr)
	{
		queue_target_sound(runtime, world, 3);
		return;
	}

	// Retail acknowledges the command immediately. Exhausting the scan is
	// not a second failure and therefore emits no additional sound.
	queue_target_sound(runtime, world, 0);
	game::ObjectHandle handles[game::kMaxGameObjects];
	const std::uint16_t count =
		collect_live_handles(world, handles);
	std::int32_t current = -1;
	for (std::uint16_t index = 0; index < count; ++index)
	{
		if (handles[index] == world.selected_target)
		{
			current = index;
			break;
		}
	}

	for (std::uint16_t attempt = 0; attempt < count; ++attempt)
	{
		current = (current + 1) % count;
		const game::ObjectHandle handle = handles[current];
		game::WorldObject& candidate =
			world.objects[handle.index];
		// The retail loop mutates the TargetRef before validating each
		// raw slot, including clearing its component word.
		write_target_reference(
			world, *player, handle, -1);
		if (!target_reference_valid(world, handle, -1, 0x800u)
			|| handle.index == world.player.index
			|| candidate.allegiance_class != 1
			|| (candidate.type != 0x5c
				&& candidate.type != 0x2d
				&& candidate.type != 0x30))
		{
			continue;
		}
		if (glm::distance(
				candidate.scene_position,
				player->scene_position) > kTargetLimit)
		{
			continue;
		}
		diagnostics::mission_log(
			"hud torpedo target object=%u",
			static_cast<unsigned>(handle.index));
		refresh_target_panel(runtime, world);
		return;
	}

	// FUN_0048c580 is deliberately not called on scan exhaustion. Mark the
	// raw mutation observed so the implementation's external-change bridge
	// does not invent that missing retail publication.
	runtime.previous_target = world.selected_target;
}

void update_center_contact(
	Runtime& runtime,
	const game::World& world,
	const game::CameraRuntime& camera,
	std::uint32_t width,
	std::uint32_t height)
{
	runtime.center_contact = {};
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (player == nullptr)
	{
		return;
	}
	const std::int32_t center_x = static_cast<std::int32_t>(
		std::nearbyint(static_cast<float>(width) * 0.5f));
	const std::int32_t center_y = static_cast<std::int32_t>(
		std::nearbyint(static_cast<float>(height) * 0.5f));
	const glm::vec3 ray_end =
		camera.position + camera.orientation[2] * kTargetLimit;
	float nearest_hit = 1.0f;
	game::ObjectHandle ray_contact;
	const std::uint16_t scan_count = std::min<std::uint16_t>(
		world.object_high_water,
		static_cast<std::uint16_t>(std::size(world.objects)));
	// The retail selector considers only projected object origins. Resolve an
	// exact live collision-tree hit first so a capital ship remains selectable
	// when its visible hull, but not its root, lies beneath the reticle.
	for (std::uint16_t object_index = 0;
		object_index < scan_count;
		++object_index)
	{
		const game::WorldObject& object =
			world.objects[object_index];
		if (!object.active
			|| object_index == world.player.index
			|| object.type >= 256)
		{
			continue;
		}
		const game::ObjectHandle handle{
			object_index,
			object.generation,
		};
		if (!target_reference_valid(world, handle, -1, 0))
		{
			continue;
		}
		float hit_fraction = 0.0f;
		if (game::weapons_scene_segment_hit(
				object,
				camera.position,
				ray_end,
				hit_fraction)
			&& hit_fraction < nearest_hit)
		{
			nearest_hit = hit_fraction;
			ray_contact = handle;
		}
	}
	if (game::world_resolve(world, ray_contact) != nullptr)
	{
		runtime.center_contact = ray_contact;
		return;
	}

	float best_screen_distance = std::numeric_limits<float>::max();
	float best_depth = std::numeric_limits<float>::max();
	// Preserve retail's forgiving 64-by-64 small-target acquisition window,
	// but admit only valid targets and rank them instead of allowing an earlier
	// object-table entry to block the visually closer contact.
	for (std::uint16_t object_index = 0;
		object_index < scan_count;
		++object_index)
	{
		const game::WorldObject& object = world.objects[object_index];
		if (!object.active
			|| object_index == world.player.index
			|| object.type >= 256)
		{
			continue;
		}
		const game::ObjectHandle handle{
			object_index,
			object.generation,
		};
		if (!target_reference_valid(world, handle, -1, 0))
		{
			continue;
		}
		float x;
		float y;
		float depth;
		if (project_center(camera, object, width, height, x, y, depth)
			&& static_cast<std::int32_t>(std::nearbyint(x))
				> center_x - 32
			&& static_cast<std::int32_t>(std::nearbyint(x))
				< center_x + 32
			&& static_cast<std::int32_t>(std::nearbyint(y))
				> center_y - 32
			&& static_cast<std::int32_t>(std::nearbyint(y))
				< center_y + 32)
		{
			const float dx = x - static_cast<float>(center_x);
			const float dy = y - static_cast<float>(center_y);
			const float screen_distance = dx * dx + dy * dy;
			if (screen_distance < best_screen_distance
				|| (screen_distance == best_screen_distance
					&& depth < best_depth))
			{
				best_screen_distance = screen_distance;
				best_depth = depth;
				runtime.center_contact = handle;
			}
		}
	}
}

void update_scanner(
	Runtime& runtime,
	const game::World& world,
	const mission::Runtime& mission,
	std::uint32_t simulation_tick)
{
	if (runtime.scanner_serial != mission.scanner_serial)
	{
		runtime.scanner_serial = mission.scanner_serial;
		runtime.tracked_contact = mission.scanner_target;
		runtime.lock_animation_deadline = 0;
		runtime.lock_animation_frame = 0;
		runtime.scanner_beep_deadline = 0;
		runtime.scanner_tone_continuous = false;
	}
	if (runtime.tracked_contact == UINT16_MAX)
	{
		runtime.scanner_tone_continuous = false;
		return;
	}
	const game::WorldObject* tracked =
		runtime.tracked_contact < std::size(world.objects)
			? &world.objects[runtime.tracked_contact]
			: nullptr;
	if (tracked != nullptr && !tracked->active)
	{
		tracked = nullptr;
	}
	const game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (tracked == nullptr || player == nullptr)
	{
		runtime.tracked_contact = UINT16_MAX;
		runtime.scanner_tone_continuous = false;
		return;
	}

	// LANCER.EXE 0x00492a04..0x00492a8e computes the scanner interval
	// from the tracked-point displacement and the player's forward row:
	// trunc((2 * distance - forward dot displacement) * 0.001), clamped.
	const glm::vec3 displacement =
		tracked->position - player->position;
	const float distance = glm::length(displacement);
	const float forward =
		glm::dot(displacement, player->orientation[2]);
	const std::int32_t interval = std::clamp(
		static_cast<std::int32_t>(
			std::trunc((2.0f * distance - forward) * 0.001f)),
		10,
		200);
	if (runtime.scanner_beep_deadline
			+ static_cast<std::uint32_t>(interval)
		>= simulation_tick)
	{
		return;
	}
	runtime.scanner_beep_deadline = simulation_tick;
	if (interval <= 10)
	{
		runtime.scanner_tone_continuous = true;
	}
	else
	{
		runtime.scanner_tone_continuous = false;
		++runtime.scanner_beep_serial;
	}
}

bool update_hud_blink_channel(
	HudBlinkChannel& channel,
	std::uint32_t simulation_steps)
{
	switch (channel.mode)
	{
	case 0:
		return false;
	case 1:
		return true;
	case 2:
		channel.phase += simulation_steps;
		if (channel.phase < 50)
		{
			return true;
		}
		if (channel.phase > 100)
		{
			channel.phase -= 100;
			return true;
		}
		return false;
	default:
		return false;
	}
}

enum StatusIcon : std::uint8_t
{
	status_match_speed,
	status_blindfire,
	status_smart_target,
	status_threat,
	status_lock_warning,
	status_ecm,
	status_cloak,
	status_spectral,
	status_reverse_thrust,
};

void update_jump_warp_request_blink(
	Runtime& runtime,
	mission::Runtime& mission,
	std::uint32_t simulation_steps)
{
	runtime.request_blink_shape = UINT16_MAX;
	// HUD_draw_jump_warp_request_blink 0x00482fa0 gives the warp request
	// at mission-session offset +4 precedence over the jump request at +0.
	std::uint32_t* request = nullptr;
	std::uint16_t shape = UINT16_MAX;
	const char* name = nullptr;
	if (mission.session_state[1] != 0)
	{
		request = &mission.session_state[1];
		shape = 201;
		name = "warp";
	}
	else if (mission.session_state[0] != 0)
	{
		request = &mission.session_state[0];
		shape = 206;
		name = "jump";
	}
	if (request == nullptr)
	{
		return;
	}
	if (*request == 1)
	{
		runtime.request_blink_phase = 0;
		*request = 2;
		diagnostics::mission_log("hud %s request armed", name);
		return;
	}
	if (*request != 2)
	{
		return;
	}
	runtime.request_blink_phase += simulation_steps;
	if (runtime.request_blink_phase > 100)
	{
		runtime.request_blink_phase = 0;
	}
	if (runtime.request_blink_phase < 50)
	{
		runtime.request_blink_shape = shape;
	}
}

void update_scripted_hud_icons(
	Runtime& runtime,
	const game::WorldObject* player,
	const game::World& world,
	const mission::Runtime& mission,
	std::uint32_t simulation_steps)
{
	for (std::uint32_t index = 0;
		index < std::size(runtime.hud_icons);
		++index)
	{
		HudBlinkChannel& destination = runtime.hud_icons[index];
		const mission::HudIconCommandState& source =
			mission.hud_icons[index];
		if (destination.serial != source.serial)
		{
			destination.mode = source.mode;
			destination.phase = 0;
			destination.serial = source.serial;
		}
	}
	std::fill(
		std::begin(runtime.hud_icon_grid_slot),
		std::end(runtime.hud_icon_grid_slot),
		UINT8_MAX);
	std::fill(
		std::begin(runtime.hud_icon_draw),
		std::end(runtime.hud_icon_draw),
		false);
	std::fill(
		std::begin(runtime.status_icon_draw),
		std::end(runtime.status_icon_draw),
		false);

	std::uint8_t grid_slot = 0;
	const auto grid_icon =
		[&](StatusIcon icon, bool active)
		{
			if (!active)
			{
				return;
			}
			runtime.hud_icon_grid_slot[icon] = grid_slot++;
			runtime.status_icon_draw[icon] = true;
		};

	// HUD_render_frame_callback 0x00484b65..0x004852ce lays every native
	// and scripted status into one two-column grid in this exact order.
	grid_icon(
		status_match_speed,
		player != nullptr && player->match_speed_active);

	// Shape 203 uses the gate at 0x00484c9e: capability, enabled state,
	// then weapon-selection bit 0x10 clear.
	grid_icon(
		status_blindfire,
		player != nullptr
			&& player->blindfire_supported
			&& player->blindfire_enabled
			&& (player->active_weapon_selection_bits & 0x0010u) == 0);

	grid_icon(
		status_smart_target,
		runtime.smart_target
			|| update_hud_blink_channel(
				runtime.hud_icons[4], simulation_steps));

	// Channels zero and one share the independent phase at 0x0057bc44.
	const bool incoming =
		player != nullptr
		&& player->incoming_missile;
	bool native_threat = false;
	if (player != nullptr && !incoming)
	{
		for (const game::WorldObject& object : world.objects)
		{
			if (!object.active
				|| object.ai.command_count == 0
				|| object.ai.commands[0].id != 105
				|| object.ai.work.target != player->mission_index
				|| !object.ai.work.attack_permission)
			{
				continue;
			}
			native_threat = true;
			break;
		}
	}
	// HUD_render_frame_callback 0x00484cf2..0x00484f29 admits shape 195
	// when any live Fight command targeting the player has work byte +0x2f
	// set, provided player+0x64c is clear. Script channel zero is an
	// independent arm. An incoming missile suppresses the native arm but
	// retains an already-playing threat tone until +0x64c clears.
	const bool threat =
		native_threat
		|| update_hud_blink_channel(
			runtime.hud_icons[0], simulation_steps);
	runtime.threat_warning_tone =
		threat || (incoming && runtime.threat_warning_tone);
	if (threat)
	{
		runtime.hud_icon_grid_slot[status_threat] = grid_slot++;
		runtime.threat_warning_phase += simulation_steps;
		if (runtime.threat_warning_phase < 50)
		{
			runtime.status_icon_draw[status_threat] = true;
		}
		else if (runtime.threat_warning_phase > 100)
		{
			runtime.threat_warning_phase = 0;
		}
	}
	// Shape 196 is the actual incoming-missile warning. Incoming state
	// short-circuits scripted channel one exactly as at
	// 0x00484f29..0x00484f53.
	const bool lock_warning =
		incoming
		|| update_hud_blink_channel(
			runtime.hud_icons[1], simulation_steps);
	if (lock_warning)
	{
		runtime.hud_icon_grid_slot[status_lock_warning] = grid_slot++;
		runtime.threat_warning_phase += simulation_steps;
		if (runtime.threat_warning_phase < 25)
		{
			runtime.status_icon_draw[status_lock_warning] = true;
		}
		else if (runtime.threat_warning_phase > 50)
		{
			runtime.threat_warning_phase = 0;
		}
	}
	grid_icon(
		status_ecm,
		(player != nullptr
			&& (player->runtime_flags & 0x04000000u) != 0)
			// HUD_render_frame_callback 0x00484ff5..0x00485005 uses a
			// short-circuit OR here.  While native ECM is active the
			// scripted channel must not advance; its blink phase resumes
			// only after native ECM clears.
			|| update_hud_blink_channel(
				runtime.hud_icons[2], simulation_steps));
	grid_icon(
		status_cloak,
		player != nullptr
			// HUD_render_frame_callback 0x004850ea..0x00485110 keeps
			// shape 199 in the grid whenever cloak equipment is
			// available, including while it is switched off.  Deathmatch
			// suppresses the native cloak status entirely.
			&& !mission.network.deathmatch_mode
			&& player->cloak_supported);
	grid_icon(
		status_spectral,
		player != nullptr
			&& (player->runtime_flags & 0x08000000u) != 0);
	grid_icon(
		status_reverse_thrust,
		player != nullptr && player->reverse_thrust_active);
	std::uint16_t status_mask = 0;
	for (std::uint8_t icon = 0;
		icon < std::size(runtime.hud_icon_grid_slot);
		++icon)
	{
		if (runtime.hud_icon_grid_slot[icon] != UINT8_MAX)
		{
			status_mask |= static_cast<std::uint16_t>(1u << icon);
		}
	}
	if (status_mask != runtime.status_icon_mask)
	{
		runtime.status_icon_mask = status_mask;
		diagnostics::mission_log(
			"hud status mask=0x%03x",
			static_cast<unsigned>(status_mask));
	}

	// 0x004856dd..0x004857cf deliberately draws the normal chaff
	// counter for state zero. Nonzero state is passed through the common
	// channel helper, allowing state two to flash the counter.
	runtime.hud_icon_draw[3] =
		runtime.hud_icons[3].mode == 0
		|| update_hud_blink_channel(
			runtime.hud_icons[3], simulation_steps);

	// The death/scripted channel has a second blink timer of its own.
	if (update_hud_blink_channel(
		runtime.hud_icons[5], simulation_steps))
	{
		runtime.death_warning_phase += simulation_steps;
		if (runtime.death_warning_phase < 50)
		{
			runtime.hud_icon_draw[5] = true;
		}
		else if (runtime.death_warning_phase > 100)
		{
			runtime.death_warning_phase = 0;
		}
	}
}

void apply_equipment_action(
	Runtime& runtime,
	game::World& world,
	mission::NetworkRuntime* network,
	game::WorldObject& player,
	std::uint8_t action,
	std::uint32_t simulation_tick)
{
	if (action == 64
		&& !runtime.deathmatch_mission
		&& player.cloak_supported)
	{
		const bool active =
			(player.runtime_flags & 0x00000100u) == 0;
		// Player_set_cloak, 0x004153e0, publishes the requested state before
		// applying it locally. Busy transitions can therefore reject the local
		// toggle while peers receive the same no-op request.
		if (network != nullptr)
		{
			(void)mission::network_publish_cloak_state(
				*network, active);
		}
		if (game::world_set_cloak_active(
				world, player, active, simulation_tick))
		{
			queue_sound(runtime, active ? 4 : 5);
			// AI_PlayerControl_update
			// 0x00413d21..0x00413d62 plays the local confirmation
			// only when Cloak_set_active accepts the transition:
			// standard FAT 16 on, 17 off.
			queue_sample(runtime, active ? 16 : 17);
			diagnostics::mission_log(
				"player cloak active=%u charge=%d",
				active ? 1u : 0u,
				runtime.cloak_charge);
		}
		return;
	}
	// Game-session setup at 0x00493745 always enables ECM support. Its
	// control path toggles object flag 0x04000000 and the HUD state.
	if (action == 65)
	{
		const bool active =
			(player.runtime_flags & 0x04000000u) == 0;
		if (active)
		{
			player.runtime_flags |= 0x04000000u;
		}
		else
		{
			player.runtime_flags &= ~0x04000000u;
		}
		queue_sound(runtime, active ? 4 : 5);
		diagnostics::mission_log(
			"player ECM active=%u charge=%d",
			active ? 1u : 0u,
			runtime.ecm_charge);
		return;
	}
	if (action == 66 && player.spectral_supported)
	{
		const bool active =
			(player.runtime_flags & 0x08000000u) == 0;
		if (active)
		{
			player.runtime_flags |= 0x08000000u;
		}
		else
		{
			player.runtime_flags &= ~0x08000000u;
		}
		queue_sound(runtime, active ? 4 : 5);
		// Player_process_discrete_input 0x00414ec0..0x00414f1d uses
		// standard FAT sample 20 for enable and 21 for disable.
		queue_sample(runtime, active ? 20 : 21);
		diagnostics::mission_log(
			"player spectral-shields active=%u charge=%d",
			active ? 1u : 0u,
			runtime.spectral_charge);
	}
}

void service_equipment_status(
	Runtime& runtime,
	game::World& world,
	mission::NetworkRuntime& network,
	game::WorldObject& player,
	std::uint32_t simulation_steps,
	std::uint32_t simulation_tick)
{
	const std::int32_t elapsed_ticks =
		static_cast<std::int32_t>(simulation_steps);
	if ((player.runtime_flags & 0x04000000u) != 0)
	{
		runtime.ecm_charge -= elapsed_ticks;
		if (runtime.ecm_charge < 0)
		{
			runtime.ecm_charge = 0;
			player.runtime_flags &= ~0x04000000u;
			diagnostics::mission_log("player ECM expired");
		}
	}
	else
	{
		runtime.ecm_charge = std::min(
			2000,
			runtime.ecm_charge + elapsed_ticks);
	}

	if (!runtime.deathmatch_mission
		&& world.player_cloak_control_active)
	{
		runtime.cloak_charge -= elapsed_ticks;
		if (runtime.cloak_charge < 0)
		{
			runtime.cloak_charge = 0;
			(void)mission::network_publish_cloak_state(network, false);
			if (game::world_set_cloak_active(
					world, player, false, simulation_tick))
			{
				diagnostics::mission_log("player cloak expired");
			}
		}
	}
	else if (!runtime.deathmatch_mission)
	{
		runtime.cloak_charge = std::min(
			10000,
			runtime.cloak_charge + elapsed_ticks);
	}
	if ((player.runtime_flags & 0x08000000u) != 0)
	{
		runtime.spectral_charge -=
			elapsed_ticks * 6;
		if (runtime.spectral_charge < 0)
		{
			runtime.spectral_charge = 0;
			player.runtime_flags &= ~0x08000000u;
			diagnostics::mission_log(
				"player spectral-shields expired");
		}
	}
	else
	{
		runtime.spectral_charge = std::min(
			6000,
			runtime.spectral_charge + elapsed_ticks);
	}
}

void update_power(game::WorldObject& player, float x, float y)
{
	const float length = std::sqrt(x * x + y * y);
	if (length > 64.0f)
	{
		x *= 64.0f / length;
		y *= 64.0f / length;
	}
	player.power_cursor_x = x;
	player.power_cursor_y = y;
	const glm::vec2 directions[3] = {
		{0.0f, 1.0f},
		{0.8660254f, -0.5f},
		{-0.8660254f, -0.5f},
	};
	float weights[3];
	float total = 0.0f;
	for (std::uint32_t index = 0; index < 3; ++index)
	{
		// LANCER.EXE Powerball_ray_circle_distance (0x004124e0):
		// take the non-negative forward intersection distance from the
		// cursor to the radius-64 circle along each allocation ray.
		const float dot =
			x * directions[index].x + y * directions[index].y;
		const float discriminant =
			dot * dot - (x * x + y * y - 4096.0f);
		weights[index] = discriminant < 0.0f
			? 0.0f
			: std::max(0.0f, dot + std::sqrt(discriminant));
		total += weights[index];
	}
	for (float& weight : weights)
	{
		weight = total == 0.0f ? 0.0f : weight / total;
	}
	const auto scale = [](float weight)
	{
		return 0.5f + (1.75f - 0.75f * weight) * weight;
	};
	player.shield_recharge_scale = scale(weights[0]);
	player.gun_recharge_scale = scale(weights[1]);
	player.engine_power_scale = scale(weights[2]);
}

bool ordinary_gun_mount(const game::WorldObject& player, std::int8_t mount)
{
	if (mount < 0
		|| static_cast<std::uint8_t>(mount) >= player.gun_mount_count)
	{
		return false;
	}
	const std::int8_t kind =
		player.gun_mounts[static_cast<std::uint8_t>(mount)].mount_kind;
	return kind == 0 || kind == 2;
}

void synchronize_full_gun_cooldowns(game::WorldObject& player)
{
	// FULL GUNS, LANCER.EXE 0x004149ca..0x00414a6a. The special
	// two-pair path samples the first ordinary mount in each pair, then
	// publishes the greatest next-fire tick to every ordinary mount in
	// both pairs so the newly combined volley starts together.
	if (player.gun_pair_count != 2)
	{
		return;
	}
	std::uint32_t next_fire_tick = 0;
	for (std::uint8_t pair_index = 0; pair_index < 2; ++pair_index)
	{
		const std::int8_t mount = player.gun_pairs[pair_index].first;
		if (ordinary_gun_mount(player, mount))
		{
			next_fire_tick = std::max(
				next_fire_tick,
				player.gun_mounts[
					static_cast<std::uint8_t>(mount)].next_fire_tick);
		}
	}
	for (std::uint8_t pair_index = 0; pair_index < 2; ++pair_index)
	{
		const game::GunPair& pair = player.gun_pairs[pair_index];
		const std::int8_t mounts[2] = {pair.first, pair.second};
		for (const std::int8_t mount : mounts)
		{
			if (ordinary_gun_mount(player, mount))
			{
				player.gun_mounts[
					static_cast<std::uint8_t>(mount)].next_fire_tick =
						next_fire_tick;
			}
		}
	}
}

void toggle_panel(Runtime& runtime, std::uint8_t panel, bool hold)
{
	if (runtime.panels[panel].animation == PanelAnimation::closed)
	{
		if (runtime_open_panel(runtime, panel))
		{
			runtime.panels[panel].hold = hold;
		}
	}
	else if (runtime.panels[panel].animation == PanelAnimation::open)
	{
		runtime_close_panel(runtime, panel);
	}
}
}

void runtime_reset(
	Runtime& runtime,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	std::uint32_t random_seed)
{
	runtime = {};
	runtime.distortion_random_seed = random_seed;
	runtime.sensor_mode = 2;
	runtime.sensor_shape = 363;
	runtime.sensor_destination = 363;
	runtime.sensor_direction = -1;
	runtime.previous_target = {};
	runtime.previous_objective = UINT16_MAX;
	runtime.chat_destination = -1;
	runtime.pointer_x = static_cast<float>(drawable_width) * 0.5f;
	runtime.pointer_y = static_cast<float>(drawable_height) * 0.5f;
	runtime.reticle_x = runtime.pointer_x;
	runtime.reticle_y = runtime.pointer_y;
	runtime.reticle_initialized = true;
}

void runtime_update_power(
	game::WorldObject& player,
	float x,
	float y)
{
	update_power(player, x, y);
}

void runtime_set_pointer(
	Runtime& runtime,
	float x,
	float y,
	bool inside)
{
	runtime.pointer_x = x;
	runtime.pointer_y = y;
	runtime.pointer_inside = inside;
}

void runtime_consume_player_hit_triggers(
	Runtime& runtime,
	game::World& world,
	std::uint32_t simulation_tick)
{
	// Both retail directional-bank owners call mission_trigger_hit_distortion
	// (0x00494890) immediately after the protected-owner gate and before
	// their collision-class gate.
	for (std::uint8_t trigger = 0;
		trigger < world.player_hit_distortion_triggers;
		++trigger)
	{
		// The owner consumes rand on every call. Once the strict 15..29
		// tick interval has elapsed it buffers standard FAT sample 12 at
		// the player position.
		const std::uint16_t random = advance_distortion_random(runtime);
		const std::uint32_t interval =
			static_cast<std::uint32_t>(random % 15u) + 15u;
		if (interval
			< simulation_tick - runtime.last_hit_sound_tick)
		{
			runtime.last_hit_sound_tick = simulation_tick;
			runtime.hit_sound_pending = true;
		}
		runtime.hit_distortion = 0.30000001192092896f;
	}
	world.player_hit_distortion_triggers = 0;
}

bool runtime_open_panel(Runtime& runtime, std::uint8_t panel)
{
	if (panel >= kPanelCount)
	{
		return false;
	}
	// HUD_panel_open, LANCER.EXE 0x0048b510, rejects exactly the
	// ordnance, scanner-side, and objective instruments in a deathmatch
	// mission before it touches their retained panel records.
	if (runtime.deathmatch_mission
		&& (panel == 2 || panel == 9 || panel == 10))
	{
		return false;
	}
	PanelState& state = runtime.panels[panel];
	if (state.animation == PanelAnimation::closed)
	{
		queue_sound(runtime, 1);
		state.animation = PanelAnimation::opening;
		state.animation_time = 0;
		state.hold = false;
	}
	state.countdown = kPanelDurations[panel];
	return true;
}

void runtime_close_panel(Runtime& runtime, std::uint8_t panel)
{
	if (panel >= kPanelCount)
	{
		return;
	}
	PanelState& state = runtime.panels[panel];
	if (state.animation != PanelAnimation::open
		&& state.animation != PanelAnimation::opening)
	{
		return;
	}
	state.animation = PanelAnimation::closing;
	state.animation_time = 60;
	state.hold = false;
	queue_sound(runtime, 2);
}

void runtime_prepare_targeting_input(
	Runtime& runtime,
	game::World& world)
{
	// Player_update publishes the newly requested camera before processing
	// the frame's targeting, panel, equipment, and other HUD actions.
	runtime.sound_camera_mode = world.camera_mode;
}

void runtime_apply_targeting_input(
	Runtime& runtime,
	game::World& world,
	const game::CameraRuntime& camera,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height,
	const input::GameplayInput& input)
{
	game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (input.pressed[17]) cycle_torpedo(runtime, world);
	if (input.pressed[15]) select_nearest(runtime, world, 1);
	if (input.pressed[16]) select_nearest(runtime, world, 0);
	if (input.pressed[18])
	{
		runtime.smart_target = !runtime.smart_target;
		world.smart_target_enabled = runtime.smart_target;
		queue_target_sound(
			runtime,
			world,
			runtime.smart_target ? 4 : 5);
	}
	if (input.pressed[8])
	{
		cycle_target(runtime, world, 1, 1, true);
	}
	if (input.pressed[9])
	{
		cycle_target(runtime, world, 1, -1, false);
	}
	if (input.pressed[12]) cycle_target_component(runtime, world, 1);
	if (input.pressed[13]) cycle_target_component(runtime, world, -1);
	if (input.pressed[10])
	{
		cycle_target(runtime, world, 0, 1, false);
	}
	if (input.pressed[11])
	{
		cycle_target(runtime, world, 0, -1, false);
	}
	if (input.pressed[14])
	{
		update_center_contact(
			runtime,
			world,
			camera,
			drawable_width,
			drawable_height);
		if (!target_reference_valid(
				world, runtime.center_contact, -1, 0))
		{
			queue_target_sound(runtime, world, 3);
		}
		else
		{
			queue_target_sound(runtime, world, 0);
			if (first_command_is_player_control(player))
			{
				game::WorldObject* target =
					game::world_resolve(
						world, runtime.center_contact);
				write_target_reference(
					world,
					*player,
					runtime.center_contact,
					-1);
				// Target-under-reticle changes panels directly instead of
				// calling the shared publish helper and never forces hold.
				open_target_panel(runtime, *target, false);
				runtime.previous_target =
					world.selected_target;
				diagnostics::mission_log(
					"hud reticle target object=%u",
					static_cast<unsigned>(
						world.selected_target.index));
			}
		}
	}
	if (input.pressed[45])
	{
		queue_sound(runtime, 0);
		// Player_process_discrete_input 0x0048c040..0x0048c07b
		// opens the ordnance panel with its hold word cleared. Only the
		// cycle-ordnance commands below pin this panel open.
		toggle_panel(runtime, 2, false);
	}
	if (player == nullptr)
	{
		return;
	}
	if (!runtime.ordnance_initialized
		&& player->attachment_count != 0)
	{
		rebuild_ordnance(runtime, *player);
	}
	if (input.pressed[46]) rotate_ordnance(runtime, world, 1);
	if (input.pressed[47]) rotate_ordnance(runtime, world, -1);
}

void runtime_apply_equipment_action(
	Runtime& runtime,
	game::World& world,
	mission::Runtime& mission,
	game::WorldObject& player,
	std::uint8_t action,
	std::uint32_t simulation_tick)
{
	apply_equipment_action(
		runtime,
		world,
		&mission.network,
		player,
		action,
		simulation_tick);
}

void runtime_apply_player_action(
	Runtime& runtime,
	game::World& world,
	mission::Runtime& mission,
	std::uint8_t action,
	bool active,
	std::uint32_t simulation_tick)
{
	game::WorldObject* player =
		game::world_resolve(world, world.player);
	if (action == 49)
	{
		// POWERBALL WINDOW is the one momentary Player_update action whose
		// release also has retained state.
		if (active)
		{
			runtime_open_panel(runtime, 7);
			if (!runtime.powerball_window_held)
			{
				queue_sound(runtime, 0);
			}
			runtime.powerball_window_held = true;
		}
		else
		{
			runtime.powerball_window_held = false;
		}
		return;
	}
	if (action == 61)
	{
		if (active && !runtime.shield_balance_held)
		{
			queue_sound(runtime, 0);
		}
		runtime.shield_balance_held = active;
		return;
	}
	if (!active)
	{
		return;
	}
	switch (action)
	{
	case 19:
		if (mission.primary_target == UINT16_MAX
			&& mission.primary_target_component == -1)
		{
			queue_target_sound(runtime, world, 3);
			break;
		}
		queue_target_sound(runtime, world, 0);
		if (player_target_command(player) != nullptr)
		{
			publish_target_reference(
				runtime,
				world,
				*player,
				raw_target_handle(world, mission.primary_target),
				mission.primary_target_component);
		}
		diagnostics::mission_log(
			"hud primary target object=%d component=%d",
			static_cast<int>(mission.primary_target),
			mission.primary_target_component);
		for (std::uint16_t objective = 0;
			objective < std::size(mission.objectives);
			++objective)
		{
			if (mission.objectives[objective] != 2)
			{
				continue;
			}
			mission.current_objective = objective;
			break;
		}
		if (runtime_open_panel(runtime, 10))
		{
			runtime.panels[10].hold = false;
		}
		break;
	case 39:
		if (player == nullptr || player->gun_pair_count <= 1)
		{
			break;
		}
		queue_sound(runtime, 0);
		player->active_weapon_selection_bits ^= 0x0010u;
		if ((player->active_weapon_selection_bits & 0x0010u) != 0)
		{
			synchronize_full_gun_cooldowns(*player);
		}
		player->gun_synchronized =
			(player->active_weapon_selection_bits & 0x0010u) != 0;
		runtime_open_panel(runtime, 1);
		diagnostics::mission_log(
			"hud guns full=%u",
			player->gun_synchronized ? 1u : 0u);
		break;
	case 40:
		if (player == nullptr)
		{
			break;
		}
		queue_sound(runtime, 0);
		runtime_open_panel(runtime, 1);
		if ((player->active_weapon_selection_bits & 0x0010u) != 0)
		{
			player->active_weapon_selection_bits &= ~0x0010u;
		}
		else
		{
			std::uint16_t selected =
				static_cast<std::uint16_t>(
					(player->active_weapon_selection_bits & 0x0007u)
					+ 1u);
			if (selected == player->gun_pair_count)
			{
				selected = 0;
			}
			player->active_weapon_selection_bits =
				static_cast<std::uint16_t>(
					(player->active_weapon_selection_bits & ~0x0007u)
					| selected);
		}
		player->selected_gun_group =
			static_cast<std::uint8_t>(
				player->active_weapon_selection_bits & 0x0007u);
		player->gun_synchronized =
			(player->active_weapon_selection_bits & 0x0010u) != 0;
		diagnostics::mission_log(
			"hud guns selected group=%u/%u",
			static_cast<unsigned>(player->selected_gun_group + 1),
			static_cast<unsigned>(player->gun_group_count));
		break;
	case 41:
		if (player != nullptr)
		{
			if (runtime.panels[1].animation == PanelAnimation::open)
			{
				runtime_close_panel(runtime, 1);
			}
			else if (runtime_open_panel(runtime, 1))
			{
				runtime.panels[1].hold = true;
			}
		}
		break;
	case 42:
		if (player == nullptr)
		{
			break;
		}
		runtime_open_panel(runtime, 1);
		player->active_weapon_selection_bits ^= 0x0020u;
		player->gun_sync_frame = static_cast<std::uint8_t>(
			(player->active_weapon_selection_bits >> 5) & 1u);
		queue_sound(
			runtime,
			(player->active_weapon_selection_bits & 0x0020u) != 0
				? 4
				: 5);
		diagnostics::mission_log(
			"hud guns synchronized=%u",
			(player->active_weapon_selection_bits & 0x0020u) != 0
				? 1u
				: 0u);
		break;
	case 43:
		if (player != nullptr && player->blindfire_supported)
		{
			player->blindfire_enabled = !player->blindfire_enabled;
			queue_sample(
				runtime, player->blindfire_enabled ? 18 : 19);
			diagnostics::mission_log(
				"player blindfire enabled=%u",
				player->blindfire_enabled ? 1u : 0u);
		}
		break;
	case 50:
		if (runtime.panels[7].animation == PanelAnimation::open)
		{
			runtime_close_panel(runtime, 7);
		}
		else if (runtime_open_panel(runtime, 7))
		{
			runtime.panels[7].hold = true;
		}
		break;
	case 51:
	case 52:
	case 53:
	case 54:
		if (player != nullptr)
		{
			constexpr float x[4] = {
				54.17f, -55.79f, 0.699f, 1.0f};
			constexpr float y[4] = {
				-30.32f, -27.94f, 61.98f, 1.0f};
			queue_sound(runtime, 0);
			update_power(*player, x[action - 51], y[action - 51]);
			runtime_open_panel(runtime, 7);
		}
		break;
	case 55:
		queue_sound(runtime, 0);
		if (runtime.panels[13].animation == PanelAnimation::open
			|| runtime.panels[13].animation
				== PanelAnimation::opening)
		{
			runtime_close_panel(runtime, 13);
		}
		if (runtime.panels[10].animation == PanelAnimation::open)
		{
			mission::objectives_cycle(mission);
		}
		else
		{
			runtime_open_panel(runtime, 10);
		}
		break;
	case 56:
		queue_sound(runtime, 0);
		if (runtime.panels[10].animation == PanelAnimation::open
			|| runtime.panels[10].animation
				== PanelAnimation::opening)
		{
			runtime_close_panel(runtime, 10);
		}
		if (runtime.panels[13].animation == PanelAnimation::open
			|| runtime.panels[13].animation
				== PanelAnimation::opening)
		{
			runtime_close_panel(runtime, 13);
		}
		else
		{
			runtime_open_panel(runtime, 13);
		}
		break;
	case 57:
		if (runtime.panels[10].animation == PanelAnimation::open
			|| runtime.panels[10].animation
				== PanelAnimation::opening)
		{
			runtime_close_panel(runtime, 10);
		}
		if (runtime.panels[13].animation == PanelAnimation::open
			|| runtime.panels[13].animation
				== PanelAnimation::opening)
		{
			runtime_close_panel(runtime, 13);
		}
		else if (runtime_open_panel(runtime, 13))
		{
			runtime.panels[13].hold = true;
		}
		break;
	case 58:
		queue_sound(runtime, 0);
		if (runtime.panels[4].animation == PanelAnimation::open
			|| runtime.panels[4].animation
				== PanelAnimation::opening)
		{
			runtime_close_panel(runtime, 4);
		}
		else
		{
			runtime_open_panel(runtime, 4);
		}
		break;
	case 59:
		if (runtime.panels[4].animation == PanelAnimation::open
			|| runtime.panels[4].animation
				== PanelAnimation::opening)
		{
			runtime_close_panel(runtime, 4);
		}
		else if (runtime_open_panel(runtime, 4))
		{
			runtime.panels[4].hold = true;
		}
		break;
	case 60:
		if (world.camera_mode == 0 && !runtime.sensor_transition)
		{
			runtime.sensor_mode =
				static_cast<std::uint8_t>(
					(runtime.sensor_mode + 1) % 3);
			constexpr std::int8_t directions[3] = {-1, 1, 1};
			constexpr std::int16_t destinations[3] = {353, 358, 363};
			runtime.sensor_direction =
				directions[runtime.sensor_mode];
			runtime.sensor_destination =
				destinations[runtime.sensor_mode];
			runtime.sensor_deadline = simulation_tick + 50;
			runtime.sensor_transition = true;
		}
		break;
	case 64:
	case 65:
	case 66:
		if (player != nullptr)
		{
			apply_equipment_action(
				runtime,
				world,
				&mission.network,
				*player,
				action,
				simulation_tick);
		}
		break;
	default:
		break;
	}
}

void runtime_set_scoreboard_held(Runtime& runtime, bool held)
{
	runtime.scoreboard_held = held;
}

void runtime_update(
	Runtime& runtime,
	game::World& world,
	mission::Runtime& mission,
	const assets::MissileStatsTable& missile_stats,
	bool local_missile_has_target,
	std::uint32_t simulation_steps,
	std::uint32_t simulation_tick,
	std::uint32_t drawable_width,
	std::uint32_t drawable_height)
{
	runtime.sound_camera_mode = world.camera_mode;
	const bool deathmatch_mission =
		mission::network_is_deathmatch_mission(
			mission.mission_number);
	if (deathmatch_mission && !runtime.deathmatch_mission)
	{
		// Mission setup opens the ordnance panel before the first fixed
		// HUD update in this implementation. Retail already knows its
		// deathmatch mission flag at that point, so discard those otherwise
		// impossible states without playing a close sound.
		runtime.panels[2] = {};
		runtime.panels[9] = {};
		runtime.panels[10] = {};
	}
	runtime.deathmatch_mission = deathmatch_mission;

	if (mission.presentation.comms_request_serial
		!= runtime.comms_request_serial)
	{
		runtime.comms_request_serial =
			mission.presentation.comms_request_serial;
		// hudmovie_play_resource, LANCER.EXE 0x0048d120, starts the
		// 92-tick static lead-in only when no previous HUD movie is active.
		// Replacing an active communication tears it down and starts the new
		// movie immediately.
		runtime.comms_static_active = !runtime.movie.active;
		runtime.comms_static_time = 0;
	}
	if (runtime.comms_static_active)
	{
		runtime.comms_static_time +=
			static_cast<std::int32_t>(simulation_steps);
		if (runtime.comms_static_time > 0x5b)
		{
			runtime.comms_static_active = false;
		}
	}

	// Camera_update_frame owns the one retail disturbance scalar, including
	// decay. Presentation samples that same value without consuming it before
	// the camera applies shake and force-feedback.
	runtime.camera_disturbance = std::clamp(
		world.player_camera_disturbance, 0.0f, 2.0f);
	// mission_update_whiteout_overlay 0x00494940 gives the shared exposure
	// accumulator priority over the red hit contribution. Exhaust uses
	// equal RGB channels at exposure*0.012 and decays the retained integer
	// by the current frame delta after publishing it.
	runtime.whiteout_exhaust =
		world.player_exhaust_exposure_percent > 0
			? std::min(
				1.0f,
				static_cast<float>(
					world.player_exhaust_exposure_percent)
					* 0.012000000104308128f)
			: 0.0f;
	if (world.player_exhaust_exposure_percent > 0)
	{
		world.player_exhaust_exposure_percent -=
			static_cast<std::int32_t>(simulation_steps);
	}
	// The red contribution is published before applying its
	// 0.005-per-tick decay.
	runtime.whiteout_red =
		world.player_in_exhaust
			&& runtime.whiteout_exhaust <= 0.0f
			? 0.0f
			: runtime.hit_distortion;
	runtime.hit_distortion = std::max(
		0.0f,
		runtime.hit_distortion
			- static_cast<float>(simulation_steps) * 0.005f);
	for (std::uint32_t bank = 0; bank < 4; ++bank)
	{
		runtime.player_schematic_hits[bank] =
			runtime.player_schematic_hits[bank]
			|| world.player_schematic_hits[bank] != 0;
		runtime.target_schematic_hits[bank] =
			runtime.target_schematic_hits[bank]
			|| world.target_schematic_hits[bank] != 0;
		world.player_schematic_hits[bank] = 0;
		world.target_schematic_hits[bank] = 0;
	}
	update_scanner(runtime, world, mission, simulation_tick);
	if (runtime.tracked_contact != UINT16_MAX
		&& simulation_tick > runtime.lock_animation_deadline)
	{
		runtime.lock_animation_deadline = simulation_tick + 25;
		runtime.lock_animation_frame = static_cast<std::uint8_t>(
			(runtime.lock_animation_frame + 1) % 5);
	}

	update_missile_lock(
		runtime,
		world,
		missile_stats,
		local_missile_has_target,
		simulation_steps);
	game::WorldObject* player =
		game::world_resolve(world, world.player);

	if (player != nullptr)
	{
		service_equipment_status(
			runtime,
			world,
			mission.network,
			*player,
			simulation_steps,
			simulation_tick);
		if (!runtime.ordnance_initialized
			&& player->attachment_count != 0)
		{
			rebuild_ordnance(runtime, *player);
		}
	}
	update_jump_warp_request_blink(
		runtime, mission, simulation_steps);
	update_scripted_hud_icons(
		runtime, player, world, mission, simulation_steps);

	if (runtime.sensor_transition
		&& simulation_tick < runtime.sensor_deadline)
	{
		runtime.sensor_deadline = simulation_tick + 50;
		runtime.sensor_shape = static_cast<std::int16_t>(
			runtime.sensor_shape + runtime.sensor_direction);
		if (runtime.sensor_shape == runtime.sensor_destination)
		{
			runtime.sensor_transition = false;
		}
	}

	if (world.selected_target != runtime.previous_target
		|| world.target_panel_refresh_requested)
	{
		refresh_target_panel(runtime, world);
		world.target_panel_refresh_requested = false;
	}
	if (mission.presentation.comms_pending)
	{
		const game::WorldObject* speaker =
			mission::runtime_resolve_object(
				mission,
				mission.presentation.speaker_mission_index,
				world);
		if (speaker != nullptr)
		{
			runtime.comms_contact_class =
				speaker->allegiance_class;
		}
		else
		{
			const assets::PilotPresentationDefinition* pilot =
				assets::pilot_presentation(
					mission.presentation.pilot);
			// The compiled-pilot branch of CommsVoice_play_or_queue
			// reads the signed word at record +4, the presentation flags
			// field, into the same static-family selector used by an
			// ordinary speaker's object +0x644 allegiance class.
			runtime.comms_contact_class =
				pilot == nullptr
					? 0
					: pilot->presentation_flags;
		}
	}
	if (mission.presentation.comms_pending
		|| (mission.presentation.comms_active
			&& (mission.presentation.movie_active
				|| runtime.movie.active)))
	{
		if (runtime_open_panel(runtime, 0))
		{
			runtime.panels[0].hold = true;
		}
	}
	else if (runtime.panels[0].hold)
	{
		runtime_close_panel(runtime, 0);
	}
	if (mission.current_objective != runtime.previous_objective)
	{
		runtime.previous_objective = mission.current_objective;
		runtime_open_panel(runtime, 10);
	}

	for (std::uint8_t panel_index = 0;
		panel_index < kPanelCount;
		++panel_index)
	{
		// HUD_render_frame_callback, LANCER.EXE
		// 0x0048640f..0x0048644f, skips the complete animation/countdown
		// owner for panels 1, 2, and 13 during mission 25A. Mission 25B
		// (the mission251.dte continuation) resumes ordinary progression.
		if (mission.mission_number == 25
			&& !mission.mission_25_alternate
			&& (panel_index == 1
				|| panel_index == 2
				|| panel_index == 13))
		{
			continue;
		}
		PanelState& panel = runtime.panels[panel_index];
		switch (panel.animation)
		{
		case PanelAnimation::opening:
			panel.animation_time +=
				static_cast<std::int32_t>(simulation_steps);
			if (panel.animation_time >= 60)
			{
				panel.animation_time = 60;
				panel.animation = PanelAnimation::open;
			}
			break;
		case PanelAnimation::closing:
			panel.animation_time -=
				static_cast<std::int32_t>(simulation_steps);
			if (panel.animation_time <= 0)
			{
				panel = {};
			}
			break;
		case PanelAnimation::open:
			if (panel.countdown < 0 && !panel.hold)
			{
				// HUD_render_frame_callback 0x004866e8..0x00486717
				// tests the retained countdown before subtracting this
				// frame's delta. A panel that crosses below zero therefore
				// remains fully open until the following HUD callback.
				panel.countdown = 0;
				panel.animation = PanelAnimation::closing;
				panel.animation_time = 60;
				queue_sound(runtime, 2);
			}
			else
			{
				panel.countdown -=
					static_cast<std::int32_t>(simulation_steps);
			}
			break;
		default:
			break;
		}
	}

	while (runtime.message_count != 0
		&& simulation_tick > runtime.messages[0].expiry)
	{
		for (std::uint8_t index = 1;
			index < runtime.message_count;
			++index)
		{
			runtime.messages[index - 1] = runtime.messages[index];
		}
		--runtime.message_count;
		break;
	}
	const Layout layout = make_layout(drawable_width, drawable_height);
	// HUD_render_frame_callback consumes the current frame-tick delta in
	// the same callback which evaluates the previous lead-marker position.
	runtime.reticle_return_amount =
		static_cast<float>(simulation_steps) * layout.element_scale;
}

void runtime_enqueue_message(
	Runtime& runtime,
	const char* text,
	std::uint32_t mission_time)
{
	if (text == nullptr)
	{
		return;
	}
	if (runtime.message_count == kTimedMessageCount)
	{
		for (std::uint8_t index = 1;
			index < runtime.message_count;
			++index)
		{
			runtime.messages[index - 1] = runtime.messages[index];
		}
		--runtime.message_count;
	}
	TimedMessage& message = runtime.messages[runtime.message_count++];
	std::snprintf(message.text, sizeof(message.text), "%s", text);
	message.expiry = mission_time + 1000;
}

void runtime_begin_chat(
	Runtime& runtime,
	std::int16_t destination)
{
	// CommsMenu_start_text_input, LANCER.EXE 0x004547a0, leaves an
	// already-active edit untouched and otherwise enables the shared
	// 64-byte text buffer. Action 72 supplies -1 for broadcast.
	if (!runtime.chat_active)
	{
		runtime.chat_destination = destination;
	}
	runtime.chat_active = true;
}

bool runtime_append_chat(
	Runtime& runtime,
	const render::FrontendRenderer& renderer,
	const char* text)
{
	if (!runtime.chat_active || text == nullptr)
	{
		return false;
	}
	std::size_t length = std::strlen(runtime.chat_message);
	bool appended = false;
	const auto* cursor =
		reinterpret_cast<const std::uint8_t*>(text);
	const std::uint8_t* const end = cursor + std::strlen(text);
	while (cursor < end
		&& length + 1 < sizeof(runtime.chat_message))
	{
		const std::uint8_t* const previous = cursor;
		std::uint32_t codepoint = 0;
		std::uint8_t character = 0;
		if (!decode_utf8_codepoint(cursor, end, codepoint))
		{
			cursor = previous + 1;
			continue;
		}
		// CommsMenu_text_input is a one-byte Win32 editor. SDL delivers
		// Unicode text, so convert every representable glyph to the Windows
		// Western byte used by the retail European resources.
		if (!windows_1252_character(codepoint, character)
			|| character >= renderer.shell.gameplay_hud_glyph_count)
		{
			continue;
		}
		runtime.chat_message[length] = static_cast<char>(character);
		runtime.chat_message[length + 1] = '\0';
		const float width = frontend::gui::text_width(
			renderer.shell.gameplay_hud_glyphs,
			renderer.shell.gameplay_hud_glyph_count,
			runtime.chat_message);
		if (width > kChatMessageWidth)
		{
			runtime.chat_message[length] = '\0';
			break;
		}
		++length;
		appended = true;
	}
	return appended;
}

void runtime_backspace_chat(Runtime& runtime)
{
	if (!runtime.chat_active)
	{
		return;
	}
	const std::size_t length = std::strlen(runtime.chat_message);
	if (length != 0)
	{
		runtime.chat_message[length - 1] = '\0';
	}
}

bool runtime_submit_chat(
	Runtime& runtime,
	std::int16_t& destination,
	char (&text)[kChatMessageBytes])
{
	if (!runtime.chat_active)
	{
		return false;
	}
	destination = runtime.chat_destination;
	std::memcpy(text, runtime.chat_message, sizeof(text));
	runtime.chat_active = false;
	runtime.chat_destination = -1;
	std::memset(runtime.chat_message, 0, sizeof(runtime.chat_message));
	return true;
}

void runtime_enqueue_ui_sound(Runtime& runtime, std::uint8_t event)
{
	queue_sound(runtime, event);
}

void runtime_enqueue_sample(Runtime& runtime, std::uint8_t sample)
{
	queue_sample(runtime, sample);
}

bool runtime_pop_sound(
	Runtime& runtime,
	std::uint8_t& sample,
	std::uint8_t& volume)
{
	if (runtime.sound_count == 0)
	{
		return false;
	}
	const HudSoundEvent& event =
		runtime.sound_events[runtime.sound_read];
	sample = event.sample;
	volume = event.volume;
	runtime.sound_read = static_cast<std::uint8_t>(
		(runtime.sound_read + 1) % std::size(runtime.sound_events));
	--runtime.sound_count;
	return true;
}

bool runtime_pop_hit_sound(Runtime& runtime)
{
	if (!runtime.hit_sound_pending)
	{
		return false;
	}
	runtime.hit_sound_pending = false;
	return true;
}
}
