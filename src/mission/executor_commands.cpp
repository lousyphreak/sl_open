#include "mission/executor_internal.hpp"

#include "ai/runtime.hpp"
#include "ai/scripted_commands.hpp"
#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "game/attachments.hpp"
#include "game/model_animation.hpp"
#include "game/retained_components.hpp"
#include "game/weapons.hpp"
#include "io/endian.hpp"
#include "mission/director.hpp"
#include "mission/environment_effects.hpp"
#include "mission/network_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <strings.h>

namespace sl_open::mission
{
namespace
{
const char* string_value(
	const ExecutorValue& value,
	const DteFile& file)
{
	if (value.kind != ValueKind::string
		|| value.value >= file.image.size)
	{
		return nullptr;
	}
	return reinterpret_cast<const char*>(
		file.image.data + value.value);
}

MissionReferenceValue mission_reference(
	const ExecutorValue& value)
{
	MissionReferenceValue result;
	result.index = static_cast<std::uint16_t>(value.value);
	result.selector =
		value.component == UINT8_MAX ? -1 : value.component;
	if (value.kind == ValueKind::group)
	{
		result.kind = ReferenceKind::group;
	}
	else if (value.kind == ValueKind::reference_set)
	{
		result.kind = ReferenceKind::set;
	}
	else if (value.kind != ValueKind::object)
	{
		result.index = UINT16_MAX;
	}
	return result;
}

template <typename Callback>
void visit_reference(
	const ExecutorValue& reference,
	Runtime& runtime,
	game::World& world,
	Callback callback)
{
	std::uint16_t first_expanded_object = UINT16_MAX;
	const auto visit = [&](
		std::uint16_t object,
		std::int16_t component,
		std::uint16_t ordinal)
	{
		// MissionReference_prepare_entity, LANCER.EXE 0x0045d720:
		// publish the previously captured first expansion member before
		// invoking the visitor, then capture the current object if it was
		// the first. Retail stores a packed null (0xffffffff) for that
		// first member. No GameObject+0x698 read exists in LANCER.EXE, but
		// preserving the producer keeps the expansion contract complete.
		if (game::WorldObject* live =
				runtime_resolve_object(runtime, object, world);
			live != nullptr)
		{
			live->first_expanded_mission_object =
				first_expanded_object;
		}
		if (first_expanded_object == UINT16_MAX)
		{
			first_expanded_object = object;
		}
		callback(object, component, ordinal);
	};
	if (reference.kind == ValueKind::object)
	{
		if (reference.value < runtime.object_count)
		{
			visit(
				static_cast<std::uint16_t>(reference.value),
				reference.component == UINT8_MAX
					? -1
					: static_cast<std::int16_t>(
						reference.component),
				std::uint16_t{1});
		}
		return;
	}
	if (reference.kind != ValueKind::group
		&& reference.kind != ValueKind::reference_set)
	{
		return;
	}
	if (reference.kind == ValueKind::group)
	{
		std::uint16_t objects[game::kMaxMissionObjects];
		const std::uint16_t count = runtime_expand_reference(
			runtime,
			ReferenceKind::group,
			static_cast<std::uint16_t>(reference.value),
			objects,
			static_cast<std::uint16_t>(std::size(objects)));
		std::uint8_t visit_ordinal = 0;
		for (std::uint16_t ordinal = 0;
			ordinal < count;
			++ordinal)
		{
			const std::uint16_t object = objects[ordinal];
			if (runtime.reference_skip_player_prefix
				&& object < runtime.player_prefix_count)
			{
				continue;
			}
			++visit_ordinal;
			visit(
				object,
				std::int16_t{-1},
				visit_ordinal);
		}
		return;
	}
	ExpandedTargetReference targets[game::kMaxMissionObjects];
	const std::uint16_t count = runtime_expand_target_reference(
		runtime,
		ReferenceKind::set,
		static_cast<std::uint16_t>(reference.value),
		targets,
		static_cast<std::uint16_t>(std::size(targets)));
	std::uint8_t visit_ordinal = 0;
	for (std::uint16_t ordinal = 0; ordinal < count; ++ordinal)
	{
		const std::uint16_t object = targets[ordinal].object;
		if (runtime.reference_skip_player_prefix
			&& object < runtime.player_prefix_count)
		{
			continue;
		}
		++visit_ordinal;
		visit(
			object,
			targets[ordinal].model,
			visit_ordinal);
	}
}

bool valid_target_reference(
	const game::WorldObject& target,
	std::int16_t component)
{
	if (!target.active
		|| !target.targetable
		|| (target.runtime_flags & 0x10000d40u) != 0)
	{
		return false;
	}
	return component < 0
		|| (component < target.component_count
			&& (target.components[component].runtime_flags
				& 0x0030u) == 0);
}

std::uint16_t reference_span(
	const Runtime& runtime,
	const ExecutorValue& reference)
{
	if (reference.kind == ValueKind::object
		&& reference.value < runtime.object_count)
	{
		return runtime.objects[reference.value].reference_span;
	}
	if (reference.kind == ValueKind::group
		&& reference.value < runtime.group_count)
	{
		return runtime.groups[reference.value].reference_span;
	}
	if (reference.kind == ValueKind::reference_set
		&& reference.value < runtime.reference_set_count)
	{
		return runtime.reference_sets[reference.value].reference_span;
	}
	return UINT16_MAX;
}

void set_trigger_state(
	const Executor& executor,
	Runtime& runtime,
	const ExecutorValue& owner,
	std::uint8_t type,
	std::uint32_t occurrence,
	bool all,
	std::uint8_t enabled)
{
	const std::uint16_t span_index =
		reference_span(runtime, owner);
	if (span_index == UINT16_MAX
		|| span_index >= runtime.reference_span_count)
	{
		return;
	}
	const ReferenceSpan& span = runtime.reference_spans[span_index];
	std::uint8_t matched = 0;
	for (std::uint16_t ordinal = 0;
		ordinal < span.trigger_count;
		++ordinal)
	{
		TriggerRecord& trigger =
			runtime.triggers[span.first_trigger + ordinal];
		if (trigger.type != type)
		{
			continue;
		}
		// SetAnyTriggerState (LANCER.EXE 0x0045d3a0) counts every
		// descriptor of the requested type. A component/watch rejection
		// suppresses mutation of that occurrence; it does not renumber the
		// later descriptors into its place.
		if (!all
			&& static_cast<std::uint32_t>(matched++) != occurrence)
		{
			continue;
		}
		bool selector_active = trigger.selector == UINT8_MAX;
		if (!selector_active)
		{
			for (const ExecutorWatch& watch : executor.watches)
			{
				if (watch.context != UINT8_MAX
					&& watch.component == trigger.selector)
				{
					selector_active = true;
					break;
				}
			}
		}
		if (!selector_active)
		{
			continue;
		}
		trigger.enabled = enabled;
		if (enabled != 0 && all)
		{
			// SetTriggerState calls MissionTrigger_set_enabled and reloads
			// the authored repeat byte. SetAnyTriggerState writes only the
			// enabled byte before mirroring it to the runtime link lists.
			trigger.repeats_remaining =
				trigger.authored_repeats;
		}
		// Retail mirrors this byte into three pointer-keyed trigger-link
		// caches at 0x00536dd8, 0x00536758, and 0x0052a5d0. The
		// reimplementation's dispatch/proximity paths hold the canonical
		// TriggerRecord index instead of duplicating those caches, so every
		// consumer observes this write directly.
		if (!all)
		{
			return;
		}
	}
}

void queue_ai_curve(
	game::World& world,
	game::WorldObject& object,
	std::int16_t command,
	const ExecutorValue& curve,
	std::uint32_t duration,
	const ExecutorValue& moving)
{
	// ShipFollowCurve_configure_entity (0x00458600) deliberately ignores
	// AI_command_push's return and writes the three payload dwords through
	// the object's queue-head pointer even when insertion was rejected.
	ai::command_push(world,
		object,
		command,
		// The two-argument AI_queue_command wrapper at 0x0040cbf0
		// supplies target classification zero (direct object), even for
		// its conventional (-1,-1) target tuple.
		ai::TargetKind::object,
		UINT16_MAX,
		-1);
	ai::Command& queued = object.ai.commands[0];
	queued.state[0] = curve.value;
	queued.state[1] = duration;
	queued.state[2] = moving.value;
	queued.state[3] = static_cast<std::uint32_t>(moving.kind);
}

void queue_director(
	Runtime& runtime,
	game::World& world,
	const DteFile& file,
	const ExecutorValue* arguments,
	bool clear)
{
	DirectorShot shot;
	shot.first_is_curve = arguments[0].kind == ValueKind::curve;
	shot.curve = shot.first_is_curve
		? static_cast<std::uint16_t>(arguments[0].value)
		: UINT16_MAX;
	shot.position = mission_reference(arguments[0]);
	shot.target = mission_reference(arguments[1]);
	shot.duration = arguments[2].value * game::kSimulationHz;
	shot.moving_origin = mission_reference(arguments[3]);
	shot.disable = mission_reference(arguments[4]);
	shot.valid = true;
	// Camera_switch_mode immediately services the newly installed mode.
	// Director_update (0x00450fa0) consumes the complete rendered-frame
	// gameplay-tick delta, even when a slow frame admitted several ticks.
	director_enqueue(
		runtime,
		world,
		file,
		shot,
		clear,
		runtime.frame_delta_ticks);
}

void set_position(
	game::WorldObject& object,
	const glm::vec3& position)
{
	object.previous_position = position;
	object.position = position;
}

void set_orientation(
	game::WorldObject& object,
	const glm::mat3& orientation)
{
	object.previous_orientation = orientation;
	object.orientation = orientation;
}

glm::mat3 look_at(
	const glm::vec3& from,
	const glm::vec3& to)
{
	// DarrensNaughtyBlag (LANCER.EXE 0x0045a290) calls the common
	// SR_mat3_look_at_points helper at 0x004c1940 with roll zero. That
	// helper does not construct an arbitrary orthonormal basis: it yaws an
	// identity matrix toward the destination, transforms the remaining
	// delta into that partial frame, then applies pitch.
	glm::vec3 delta = to - from;
	glm::mat3 orientation{1.0f};
	orientation = math::postrotate(
		orientation,
		std::atan2(delta.x, delta.z),
		{0.0f, 1.0f, 0.0f});
	delta = glm::transpose(orientation) * delta;
	return math::postrotate(
		orientation,
		-std::atan2(delta.y, delta.z),
		{1.0f, 0.0f, 0.0f});
}

void destroy_matching_timers(
	Executor& executor,
	std::uint16_t identity)
{
	// DestroyTimer_command, LANCER.EXE 0x0045d290. The callback snapshots
	// the active count, visits that many non-free records, and marks every
	// matching 16-bit identity free by changing only function to -1.
	const std::uint16_t snapshot = executor.active_timers;
	std::uint16_t visited = 0;
	for (ExecutorTimer& timer : executor.timers)
	{
		if (timer.function == -1)
		{
			continue;
		}
		if (++visited > snapshot)
		{
			break;
		}
		if (timer.identity != identity)
		{
			continue;
		}
		timer.function = -1;
		--executor.active_timers;
	}
}
}

CommandFlow executor_execute_command(
	std::uint8_t command,
	const ExecutorValue* arguments,
	ExecutorContext& context,
	Executor& executor,
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	std::uint32_t script_tick,
	std::uint32_t simulation_tick)
{
	++executor.command_count;
	switch (command)
	{
	case 0: // PrintShipName
		// The registered retail callback at LANCER.EXE 0x00458ab0 is
		// exactly `mov eax, 1; ret`. Despite the descriptor's two
		// arguments, it reads neither one and has no side effects.
		return CommandFlow::continue_execution;
	case 22: // ResetCodePriority
		// Retail LANCER.EXE 0x00458a90 invokes the standard reference visitor
		// (LANCER.EXE 0x0045d460) with LANCER.EXE 0x00458ab0. That per-entity callback is
		// exactly `return 1`; only the visitor's expansion bookkeeping
		// side effects occur.
		visit_reference(
			arguments[0],
			runtime,
			world,
			[](std::uint16_t, std::int16_t, std::uint16_t) {});
		return CommandFlow::continue_execution;
	case 1: // CreateTimer
	{
		const std::uint16_t identity =
			static_cast<std::uint16_t>(arguments[0].value);
		destroy_matching_timers(executor, identity);
		for (ExecutorTimer& timer : executor.timers)
		{
			if (timer.function != -1)
			{
				continue;
			}
			timer.function =
				static_cast<std::int32_t>(arguments[1].value);
			timer.interval =
				static_cast<std::uint16_t>(arguments[2].value);
			timer.activations =
				static_cast<std::uint16_t>(arguments[3].value);
			timer.countdown =
				timer.interval == 1 ? 2 : timer.interval;
			timer.identity = identity;
			timer.last_fire_tick = 0;
			++executor.active_timers;
			executor.timer_high_water =
				std::max(
					executor.timer_high_water,
					executor.active_timers);
			return CommandFlow::continue_execution;
		}
		// CreateTimer_command (LANCER.EXE 0x0045d210) has no bounds check
		// and falls through the 16-record allocation when every record is
		// occupied. Valid mission data never reaches that corrupting path;
		// the safe containment must retain the callback's normal success
		// flow rather than introducing a spurious one-tick script yield.
		if (!executor.timer_overflow_logged)
		{
			executor.timer_overflow_logged = true;
			diagnostics::mission_log(
				"executor timer pool saturated capacity=%zu",
				std::size(executor.timers));
		}
		return CommandFlow::continue_execution;
	}
	case 2: // DestroyTimer
		destroy_matching_timers(
			executor,
			static_cast<std::uint16_t>(arguments[0].value));
		return CommandFlow::continue_execution;
	case 3: // CreateFlightGroup
		if (arguments[0].kind == ValueKind::group)
		{
			runtime_activate_group(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world,
				stats);
		}
		return CommandFlow::continue_execution;
	case 4: // DestroyFlightGroup
		if (arguments[0].kind == ValueKind::group)
		{
			runtime_deactivate_group(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
		}
		return CommandFlow::continue_execution;
	case 5: // Wait
		// Wait_command, LANCER.EXE 0x0045d2e0, stores the wrapping dword
		// sum of the current 1 Hz script clock and the raw operand, then
		// returns zero to yield without rewinding the command.
		context.wait_until_tick =
			script_tick + arguments[0].value;
		return CommandFlow::yield;
	case 6: // PlaySpeech
	{
		const char* path = string_value(arguments[0], file);
		if (path != nullptr)
		{
			// PlaySpeech_command (LANCER.EXE 0x00458090) stops CBOX slot
			// zero, frees its previous owned sample, loads this path through
			// HOG_bigread2, and starts it once at full speech volume. The
			// platform bridge performs those ordered ownership operations
			// when it consumes this request.
			std::snprintf(
				runtime.presentation.standalone_speech_path,
				sizeof(runtime.presentation.standalone_speech_path),
				"%s",
				path);
			runtime.presentation.speech_pending = true;
			runtime.presentation.speech_request_serial =
				++runtime.presentation.slot_zero_request_serial;
		}
		return CommandFlow::continue_execution;
	}
	case 7: // WaitForSpeech
		// WaitForSpeech_command (LANCER.EXE 0x00458100) tests only CBOX
		// slot zero. While nonzero it subtracts exactly two bytes from the
		// interpreter PC and returns zero, replaying this zero-argument
		// command on the next admitted mission update.
		return presentation_slot_zero_busy(runtime.presentation)
			? CommandFlow::rewind_two_and_yield
			: CommandFlow::continue_execution;
	case 8: // PlayCommsMovie
	{
		const char* movie = string_value(arguments[0], file);
		const char* speech = string_value(arguments[1], file);
		if (movie != nullptr && speech != nullptr)
		{
			// PlayCommsMovie_command (LANCER.EXE 0x00458120) formats both
			// strings through 52-byte locals, prefixes the movie with
			// "pilots\\", and invokes CommsVoice_play_or_queue in immediate
			// mode with category 5 and no speaker or expiry.
			char fm8[50];
			std::snprintf(fm8, sizeof(fm8), "pilots/%s", movie);
			player_comms_play_or_queue(
				runtime,
				world,
				fm8,
				speech,
				CommsPlaybackMode::immediate,
				// The command bridge loads the full VM dword at
				// 0x0045812e, but CommsVoice_play_or_queue consumes
				// WORD [esp+0x9c] at 0x00456460 and stores WORD at
				// 0x005295a4. The retail comm voice field is 16-bit.
				static_cast<std::int16_t>(arguments[2].value),
				5,
				-1,
				-1,
				simulation_tick);
		}
		return CommandFlow::continue_execution;
	}
	case 9: // WaitForMovie
		// WaitForMovie_command (LANCER.EXE 0x00458180) tests the HUD movie
		// active dword and, while nonzero, rewinds the interpreter PC by two
		// bytes so this zero-argument command is retried next update.
		return presentation_movie_busy(runtime.presentation)
			? CommandFlow::rewind_two_and_yield
			: CommandFlow::continue_execution;
	case 10: // PrintDebugMessage
	{
		const char* message = string_value(arguments[0], file);
		if (message != nullptr)
		{
			// PrintDebugMessage_command (LANCER.EXE 0x004581a0) formats
			// "DEBUG: %s" into the shared HUD buffer, sets its 16-bit
			// display latch, stores the wrapping gameplay tick plus 500,
			// and emits the same text through the engine debug-output hook.
			std::snprintf(
				runtime.presentation.debug_message,
				sizeof(runtime.presentation.debug_message),
				"DEBUG: %s",
				message);
			runtime.presentation.debug_expiry =
				simulation_tick + 500;
			diagnostics::mission_log(
				"%s", runtime.presentation.debug_message);
		}
		return CommandFlow::continue_execution;
	}
	case 11: // SetAI
	{
		ai::TargetKind target_kind = ai::TargetKind::none;
		std::uint16_t target = UINT16_MAX;
		std::int16_t target_component = -1;
		bool supported_target = true;
		switch (arguments[3].kind)
		{
		case ValueKind::group:
			target_kind = ai::TargetKind::group;
			target = static_cast<std::uint16_t>(
				arguments[3].value);
			break;
		case ValueKind::reference_set:
			target_kind = ai::TargetKind::set;
			target = static_cast<std::uint16_t>(
				arguments[3].value);
			break;
		case ValueKind::object:
			target_kind = ai::TargetKind::object;
			if (arguments[3].value < runtime.object_count)
			{
				// MissionObject_index_from_pointer (0x0045ac50) converts
				// the authored DTE record directly. It deliberately does
				// not require the target to have a current live object.
				target = static_cast<std::uint16_t>(
					arguments[3].value);
				target_component =
					arguments[3].component == UINT8_MAX
						? -1
						: arguments[3].component;
			}
			break;
		case ValueKind::null_value:
			// MissionReference_classify returns 0xffff for the null
			// sentinel and follows the same direct/unresolved branch:
			// kind zero, object -1, component -1.
			target_kind = ai::TargetKind::object;
			break;
		default:
			// SetAI_apply_entity (LANCER.EXE 0x00458220) ignores every
			// non-sentinel classification other than object/group/set.
			supported_target = false;
			break;
		}
		std::uint16_t sequence = 0;
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				bool inserted = false;
				if (supported_target && object != nullptr)
				{
					// SetAI_apply_entity (LANCER.EXE 0x00458220) ignores
					// operand two, classifies operand three as
					// object/group/reference-set, and inserts operand one
					// as the command ID for each expanded source entity.
					ai::command_push(world,
						*object,
						static_cast<std::int16_t>(
							arguments[1].value),
						target_kind,
						target,
						target_component,
						target_kind == ai::TargetKind::group
							? 1
							: target_kind == ai::TargetKind::set
								? 2 : 0,
						static_cast<std::int16_t>(sequence),
						&inserted);
				}
				// AI_queue_command_numbered (0x0040cbc0) advances the
				// shared batch number only when AI_command_insert created
				// a real queue record. Head duplicates succeed but retain
				// the number for the next entity; rejections do likewise.
				if (inserted)
				{
					++sequence;
				}
			});
		return CommandFlow::continue_execution;
	}
	case 12: // ClearAI
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object != nullptr
					&& object_index >= runtime.player_prefix_count)
				{
					// ClearAI_apply_entity (LANCER.EXE 0x004588e0)
					// compares MissionObject_index_from_pointer against
					// the signed player-prefix count before invoking
					// AI_try_clear_command_queue_fast (0x0040cf50).
					// This is the stable mission index, not the live
					// world's current allocation slot.
					ai::command_try_clear_fast(world, *object);
				}
			});
		return CommandFlow::continue_execution;
	case 13: // SetPatrolRoute
	{
		std::uint16_t route_pair = UINT16_MAX;
		if (arguments[1].kind == ValueKind::group)
		{
			// SetPatrolRoute_apply_entity (LANCER.EXE 0x00458880) searches
			// the 8-byte route-pair table through 0x00458940 using the raw
			// group pointer, retaining the first matching pair.
			for (std::uint16_t index = 0;
				index < runtime.subtype997_pair_count;
				++index)
			{
				if (runtime.subtype997_pairs[index].group
					== arguments[1].value)
				{
					route_pair = index;
					break;
				}
			}
		}
		if (route_pair == UINT16_MAX)
		{
			return CommandFlow::continue_execution;
		}
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object != nullptr)
				{
					// SetPatrolRoute_apply_entity (0x00458880) passes the
					// subtype-997 pair index as command target +0x04. The
					// common wrapper supplies selector zero and component
					// -1; this is not a queue-selector payload.
					ai::command_push(world,
						*object,
						15,
						// SetPatrolRoute uses the same 0x0040cbf0
						// wrapper, whose implicit classification is
						// direct-object kind zero.
						ai::TargetKind::object,
						route_pair,
						-1);
				}
			});
		return CommandFlow::continue_execution;
	}
	case 14: // SetPilot
		if (arguments[0].kind == ValueKind::object)
		{
			game::WorldObject* object = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
			if (object != nullptr)
			{
				// SetPilot_command (LANCER.EXE 0x00458830) calls
				// GameObject_set_pilot (0x0049cce0), which stores this ID
				// and two pointers derived from the pilot tables. This
				// runtime derives those table records from `pilot` at each
				// use, so the ID is the complete equivalent state.
				object->pilot =
					static_cast<std::uint8_t>(
						arguments[1].value);
			}
		}
		return CommandFlow::continue_execution;
	case 15: // SetTriggerState
		// SetTriggerState_command (LANCER.EXE 0x0045d300) visits every
		// matching descriptor in the owner's trigger span. Its 0x0045d390
		// helper writes the low enabled byte and, only when nonzero, reloads
		// the runtime repeat byte from the authored repeat byte.
		set_trigger_state(
			executor,
			runtime,
			arguments[0],
			static_cast<std::uint8_t>(arguments[1].value),
			0,
			true,
			static_cast<std::uint8_t>(arguments[2].value));
		return CommandFlow::continue_execution;
	case 16: // StartDirectorCam
		// StartDirectorCam_command (LANCER.EXE 0x004582e0) resets the
		// ten-record request count, converts these five operands through
		// 0x00458300, appends the six-dword director record at 0x00461d30,
		// and immediately installs/services camera mode 13 for the new head.
		queue_director(runtime, world, file, arguments, true);
		return CommandFlow::continue_execution;
	case 17: // StartShipAnimation
	{
		const char* name = string_value(arguments[1], file);
		if (name != nullptr && arguments[0].kind == ValueKind::object)
		{
			game::WorldObject* object = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
			if (object != nullptr)
			{
				// StartShipAnimation_command (LANCER.EXE 0x00458720)
				// resolves one direct mission object, visits each non-null
				// top-level model instance, and starts the first
				// case-insensitive name match at time 0, default mode, and
				// rate +4.0.
				game::model_animation_start_named_direct_children(
					*object, name);
			}
		}
		return CommandFlow::continue_execution;
	}
	case 61: // StartShipAnimationReverse
	{
		const char* name = string_value(arguments[1], file);
		if (name != nullptr && arguments[0].kind == ValueKind::object)
		{
			game::WorldObject* object = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
			if (object != nullptr)
			{
				// LANCER.EXE 0x4587d0 resolves one direct object, visits
				// its non-null top-level models, and starts the named
				// sequence at its current time with mode -1/rate -4.0.
				game::model_animation_reverse_named_direct_children(
					*object, name);
			}
		}
		return CommandFlow::continue_execution;
	}
	case 72: // StopShipAnimation
	{
		const char* name = string_value(arguments[1], file);
		if (name != nullptr && arguments[0].kind == ValueKind::object)
		{
			game::WorldObject* object = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
			if (object != nullptr)
			{
				// LANCER.EXE 0x458770 resolves one direct object and
				// restarts each non-null top-level model's named sequence
				// at its current time with mode zero/rate +4.0.
				game::model_animation_stop_named_direct_children(
					*object, name);
			}
		}
		return CommandFlow::continue_execution;
	}
	case 18: // ShipFollowCurve
	case 27: // MovingShipFollowCurve
		// ShipFollowCurve_command (LANCER.EXE 0x004585d0) expands its
		// source reference and calls 0x00458600 with a literal zero moving
		// origin. That helper inserts AI command 17, then writes the curve,
		// raw duration, and zero through the queue-head pointer regardless
		// of insertion success. MovingShipFollowCurve_command at 0x004585a0
		// instead has its 0x004585c0 visitor forward operand four as the
		// third payload dword.
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object != nullptr)
				{
					queue_ai_curve(
						world,
						*object,
						17,
						arguments[1],
						arguments[2].value,
						command == 27
							? arguments[3]
							: ExecutorValue{
								0,
								ValueKind::scalar});
				}
			});
		return CommandFlow::continue_execution;
	case 19: // SetupLaunch
	{
		ai::TargetKind source_kind = ai::TargetKind::none;
		std::uint16_t source_target = UINT16_MAX;
		bool supported_source = true;
		switch (arguments[1].kind)
		{
		case ValueKind::object:
			source_kind = ai::TargetKind::object;
			if (arguments[1].value < runtime.object_count)
			{
				// SetupLaunch_apply_entity (LANCER.EXE 0x004589a0)
				// calls MissionObject_index_from_pointer directly; an
				// authored launch source retains identity while inactive.
				source_target = static_cast<std::uint16_t>(
					arguments[1].value);
			}
			break;
		case ValueKind::null_value:
			// MissionReference_classify returns 0xffff for the sentinel;
			// SetupLaunch follows the direct-object branch with target -1.
			source_kind = ai::TargetKind::object;
			break;
		case ValueKind::group:
			source_kind = ai::TargetKind::group;
			source_target = static_cast<std::uint16_t>(
				arguments[1].value);
			break;
		case ValueKind::reference_set:
			source_kind = ai::TargetKind::set;
			source_target = static_cast<std::uint16_t>(
				arguments[1].value);
			break;
		default:
			supported_source = false;
			break;
		}
		std::uint16_t sequence = 0;
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t ordinal)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (supported_source && object != nullptr)
				{
					// The callback inserts AI command 104. Direct/null
					// sources receive low16(launch point + visitor ordinal
					// - 1); group/set sources receive component -1.
					bool inserted = false;
					ai::command_push(world,
						*object,
						104,
						source_kind,
						source_target,
						source_kind == ai::TargetKind::object
							? static_cast<std::int16_t>(
								arguments[2].value
								+ ordinal - 1)
							: -1,
						0,
						static_cast<std::int16_t>(
							sequence),
						&inserted);
					// SetupLaunch's launch-point selector remains tied to
					// the visitor ordinal above. Its AI batch sequence is
					// the independent insertion-only counter owned by
					// AI_queue_command_numbered_targeted (0x0040cbe0).
					if (inserted)
					{
						++sequence;
					}
				}
			});
		return CommandFlow::continue_execution;
	}
	case 20: // StartLaunch
		// StartLaunch_command (LANCER.EXE 0x00458a40) expands the source
		// reference and invokes AI_signal_launch at 0x00418db0 for each
		// entity. That helper signals only the first queued command 104.
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object != nullptr)
				{
					ai::command_signal_launch(*object);
				}
			});
		return CommandFlow::continue_execution;
	case 21: // DisplaySubTitle
		// Retail LANCER.EXE 0x00458a80 copies the command's single 32-bit operand
		// verbatim to the subtitle identifier and returns immediately. The
		// HUD owns language lookup and the 0x90 "no subtitle" sentinel.
		runtime.presentation.subtitle_id = arguments[0].value;
		return CommandFlow::continue_execution;
	case 23: // InterruptTriggerCode
		// Retail LANCER.EXE 0x0045d450 sets byte +0xae on the current executor
		// context and returns zero. The trigger dispatcher clears that byte
		// when the same trigger is enabled again.
		context.suspended = true;
		return CommandFlow::yield;
	case 24: // CommsFromShip
	case 41: // CommsFromShipOnce
	{
		// Retail LANCER.EXE 0x00458ac0 converts the authored object pointer to its
		// live-object index, formats the third operand through a 52-byte
		// local, and calls LANCER.EXE 0x004561c0 in immediate mode with category 5
		// and speaker expiry -1, then returns zero. Command 41's
		// LANCER.EXE 0x00458fd0 has the same operand widths and return contract but
		// selects one-shot category 6.
		if (arguments[0].kind != ValueKind::object)
		{
			return CommandFlow::yield;
		}
		const std::uint16_t speaker =
			static_cast<std::uint16_t>(arguments[0].value);
		const game::WorldObject* object =
			runtime_resolve_object(runtime, speaker, world);
		if (object != nullptr)
		{
			const std::uint16_t live_index =
				static_cast<std::uint16_t>(
					object - std::begin(world.objects));
			player_comms_play_live_pilot(
				runtime,
				world,
				live_index,
				arguments[1].value,
				string_value(arguments[2], file),
				CommsPlaybackMode::immediate,
				command == 41 ? 6 : 5,
				-1,
				simulation_tick);
		}
		return CommandFlow::yield;
	}
	case 25: // CommsFromPilot
	case 42: // CommsFromPilotOnce
	{
		// Retail LANCER.EXE 0x00458b10 consumes the pilot operand as a 16-bit ID,
		// passes the full 32-bit head/face variant to LANCER.EXE 0x00456250, formats
		// the third operand through a 52-byte local, and returns zero after
		// immediate category-5 playback. Command 42's LANCER.EXE 0x00459020 retains
		// the same widths and zero return but selects category 6.
		const char* voice_path = string_value(arguments[2], file);
		if (!player_comms_play_compiled_pilot(
			runtime,
			world,
			static_cast<std::uint16_t>(arguments[0].value),
			arguments[1].value,
			voice_path,
			CommsPlaybackMode::immediate,
			command == 42 ? 6 : 5,
			-1,
			simulation_tick))
		{
			diagnostics::mission_log(
				"script comms rejected source=pilot pilot=%u face=%u "
				"voice=%s command=%u",
				static_cast<unsigned>(
					static_cast<std::uint16_t>(arguments[0].value)),
				static_cast<unsigned>(arguments[1].value),
				voice_path != nullptr ? voice_path : "<invalid>",
				static_cast<unsigned>(command));
		}
		return CommandFlow::yield;
	}
	case 26: // SetInvulnerability
		// Retail LANCER.EXE 0x00458bc0 expands the reference through LANCER.EXE 0x0045d460.
		// LANCER.EXE 0x00458be0 exempts the authored player-prefix objects outside
		// simulator/training modes, writes a byte at live object +0xb95 for
		// a whole-object target, or a word in the selected 12-byte component
		// record for a component target.
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t component,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object == nullptr) return;
				if (!runtime.simulator_mode
					&& (runtime.mission_number < 30
						|| runtime.mission_number > 35)
					&& object_index < runtime.player_prefix_count)
				{
					return;
				}
				const std::uint16_t state =
					static_cast<std::uint16_t>(
						arguments[1].value);
				if (component < 0)
				{
					object->protection_state =
						static_cast<std::uint8_t>(state);
				}
				else if (static_cast<std::size_t>(component)
					< game::kMaxObjectComponents)
				{
					const std::uint64_t bit =
						std::uint64_t{1} << component;
					object->component_protection_override_mask |=
						bit;
					object->component_protection_override[
						component] = state;
					if (component < object->component_count)
					{
						object->components[
							component].protection_state = state;
					}
				}
			});
		return CommandFlow::continue_execution;
	case 28: // DisableObject
		// Retail LANCER.EXE 0x004583c0 expands the reference and LANCER.EXE 0x004583e0
		// toggles live-object flag 0x400 for selector 0xff. For a component
		// it walks the selected model container, toggling node flag 0x20 on
		// every sibling with the same part identity; source flag 0x04
		// reverses the requested state for authored DEST alternates.
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t component,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object == nullptr) return;
				const bool disabled = arguments[1].value != 0;
				if (component < 0)
				{
					object->disabled = disabled;
					if (disabled)
					{
						object->runtime_flags |= game::kObjectFlagDisabled;
					}
					else
					{
						object->runtime_flags &= ~game::kObjectFlagDisabled;
					}
				}
				else if (component < object->component_count)
				{
					game::retained_set_component_disabled(
						*object, component, disabled);
				}
			});
		return CommandFlow::continue_execution;
	case 29: // PositionRelative
		// Retail LANCER.EXE 0x004584f0 subtracts the marker's immutable mission
		// origin (+0x1c) from its live position (+0x3c), accumulates that
		// displacement into each target's mutable mission position (+0x08),
		// then publishes all three coordinates through LANCER.EXE 0x004521e0.
		if (arguments[1].kind == ValueKind::object)
		{
			const std::uint16_t marker_index =
				static_cast<std::uint16_t>(arguments[1].value);
			const game::WorldObject* marker =
				runtime_resolve_object(
					runtime, marker_index, world);
			if (marker != nullptr)
			{
				const glm::vec3 displacement =
					marker->position
					- runtime.objects[
						marker_index].authored_position;
				visit_reference(
					arguments[0],
					runtime,
					world,
					[&](std::uint16_t object_index,
						std::int16_t,
						std::uint16_t)
					{
						game::WorldObject* object =
							runtime_resolve_object(
								runtime,
								object_index,
								world);
						if (object != nullptr)
						{
							ObjectRecord& record =
								runtime.objects[object_index];
							record.script_position +=
								displacement;
							set_position(
								*object,
								record.script_position);
						}
					});
			}
		}
		return CommandFlow::continue_execution;
	case 30: // WhenPlayerLastJumped
	{
		// Retail LANCER.EXE 0x00458580 performs a 32-bit subtract, returns 0xffff
		// when its sign flag is set, maps an exact zero to one, and otherwise
		// returns the unscaled difference verbatim.
		const std::uint32_t elapsed =
			script_tick - runtime.last_player_jump_tick;
		context.last_result =
			(elapsed & 0x80000000u) != 0
				? UINT16_MAX
				: elapsed == 0 ? 1u : elapsed;
		return CommandFlow::continue_execution;
	}
	case 31: // StartMissileCam
	{
		// Retail LANCER.EXE 0x00458b60 converts the authored ship pointer to its
		// 16-bit live-table index and calls Camera_switch_mode through
		// LANCER.EXE 0x00461c40 with mode 18 and both lock arguments set to one.
		// The command callback itself always returns zero.
		game::WorldObject* source =
			arguments[0].kind == ValueKind::object
				? runtime_resolve_object(
					runtime,
					static_cast<std::uint16_t>(
						arguments[0].value),
					world)
				: nullptr;
		runtime.requested_camera_mode = 18;
		runtime.requested_camera_target =
			source == nullptr
				? UINT16_MAX
				: static_cast<std::uint16_t>(
					source - std::begin(world.objects));
		runtime.requested_camera_lock = true;
		runtime.requested_camera_override_lock = true;
		runtime_publish_camera_request(runtime);
		return CommandFlow::yield;
	}
	case 32: // StartChaseCam
	{
		// Retail LANCER.EXE 0x00458b80 maps a null object to cockpit mode zero on
		// the local player with (lock, override) = (0, 1). A non-null ship
		// selects chase mode four on its 16-bit live-table index with
		// (lock, override) = (1, 1). Both branches return zero.
		if (arguments[0].kind == ValueKind::null_value)
		{
			runtime.requested_camera_mode = 0;
			runtime.requested_camera_target = world.player.index;
			runtime.requested_camera_lock = false;
			runtime.requested_camera_override_lock = true;
		}
		else
		{
			game::WorldObject* source =
				arguments[0].kind == ValueKind::object
					? runtime_resolve_object(
						runtime,
						static_cast<std::uint16_t>(
							arguments[0].value),
						world)
					: nullptr;
			runtime.requested_camera_mode = 4;
			runtime.requested_camera_target =
				source == nullptr
					? UINT16_MAX
					: static_cast<std::uint16_t>(
						source - std::begin(world.objects));
			runtime.requested_camera_lock = true;
			runtime.requested_camera_override_lock = true;
		}
		runtime_publish_camera_request(runtime);
		return CommandFlow::yield;
	}
	case 33: // SetPlayerTarget
		// Retail LANCER.EXE 0x00458c80 admits the local player or an authored object
		// strictly above the player-prefix boundary, validates the target
		// through
		// LANCER.EXE 0x00401870 with exclusion mask zero, then replaces the target
		// words on AI command 100 (local) or 101 (remote). The local branch
		// refreshes HUD targeting and clears match speed.
		if (arguments[0].kind == ValueKind::object
			&& arguments[1].kind == ValueKind::object)
		{
			const std::uint16_t player_index =
				static_cast<std::uint16_t>(
					arguments[0].value);
			game::WorldObject* player = runtime_resolve_object(
				runtime,
				player_index,
				world);
			game::WorldObject* target = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[1].value),
				world);
			const std::int16_t component =
				arguments[1].component == UINT8_MAX
					? -1
					: arguments[1].component;
			if (player != nullptr
				&& target != nullptr
				&& valid_target_reference(*target, component))
			{
				const std::uint16_t player_live =
					static_cast<std::uint16_t>(
						player - std::begin(world.objects));
				const bool local =
					player_live == world.player.index;
				if (!local
					&& player_index
						<= runtime.player_prefix_count)
				{
					return CommandFlow::continue_execution;
				}
				const std::uint16_t target_index =
					static_cast<std::uint16_t>(
						arguments[1].value);
				const std::uint16_t target_live =
					static_cast<std::uint16_t>(
						target - std::begin(world.objects));
				bool target_written = false;
				for (std::uint8_t index = 0;
					index < player->ai.command_count;
					++index)
				{
					ai::Command& queued =
						player->ai.commands[index];
					if (queued.id
						!= (local ? 100 : 101))
					{
						continue;
					}
					queued.target_kind = ai::TargetKind::object;
					queued.target = target_index;
					queued.target_component = component;
					target_written = true;
					break;
				}
				if (!target_written)
				{
					return CommandFlow::continue_execution;
				}
				if (local)
				{
					player->match_speed_active = false;
					runtime_publish_match_speed_request(
						runtime, false);
					player->selected_target_index = target_live;
					player->selected_target_component = component;
					world.selected_target = {
						target_live,
						target->generation,
					};
					world.target_component = component;
				}
				else
				{
					// AI_MultiplayerControl's TargetRef is also mirrored
					// on WorldObject for the reimplementation's weapon
					// cadence and remote-target consumers.
					player->selected_target_index = target_live;
					player->selected_target_component = component;
				}
			}
		}
		return CommandFlow::continue_execution;
	case 34: // SetTargetable
		// Retail LANCER.EXE 0x00458d50 expands the reference. LANCER.EXE 0x00458d70 calls
		// LANCER.EXE 0x00401830 for selector 0xff, which clears object flag 0x200 and
		// restores it only when requested and the ship definition permits
		// targeting. A component selector directly toggles node flag 0x2000
		// when its retained node exists.
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t component,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object == nullptr) return;
				const bool enabled = arguments[1].value != 0;
				if (component < 0)
				{
					const bool capability =
						object->type < assets::kShipStatsCount
						&& stats.records[
							object->type].object
							.targetable_capability;
					object->targetable = enabled && capability;
					object->runtime_flags &= ~0x200u;
					if (object->targetable)
					{
						object->runtime_flags |= 0x200u;
					}
				}
				else if (static_cast<std::size_t>(component)
					< game::kMaxObjectComponents)
				{
					const std::uint64_t bit =
						std::uint64_t{1} << component;
					object->component_targetable_override_mask |=
						bit;
					if (enabled)
						object->component_targetable_value_mask |=
							bit;
					else
						object->component_targetable_value_mask &=
							~bit;
					if (component < object->component_count)
					{
						game::ObjectComponent& selected =
							object->components[component];
						if (enabled)
							selected.runtime_flags |= 0x2000u;
						else
							selected.runtime_flags &= ~0x2000u;
						// Retail's component entry is the live scene node.
						// Keep the reimplementation's separate render-node
						// mirror in step with that authoritative flag.
						if (selected.model_reference >= 0
							&& static_cast<std::size_t>(
								selected.model_reference)
								< object->model_references.size())
						{
							game::ObjectModelReference& model =
								object->model_references[
									selected.model_reference];
							if (enabled)
								model.runtime_flags |= 0x2000u;
							else
								model.runtime_flags &= ~0x2000u;
						}
					}
				}
			});
		return CommandFlow::continue_execution;
	case 35: // PlayMusic
	{
		// Retail LANCER.EXE 0x00458df0 formats "music\\%s" through a 128-byte local
		// and calls LANCER.EXE 0x00482a80 with loop count zero, volume 0x50, and the
		// full second operand as the mode. Only mode one starts immediately;
		// every other value enters the fade/pending path.
		const char* path = string_value(arguments[0], file);
		if (path != nullptr)
		{
			std::snprintf(
				runtime.presentation.music_path,
				sizeof(runtime.presentation.music_path),
				"music/%s",
				path);
			runtime.presentation.music_mode =
				arguments[1].value;
			runtime.presentation.music_pending = true;
		}
		return CommandFlow::continue_execution;
	}
	case 36: // StopDirectorCam
		// Retail LANCER.EXE 0x00458e30 does nothing while the local player is the
		// departed placeholder type 1001. Otherwise it requests camera mode
		// zero with (lock, override) = (0, 1) and returns one; it does not
		// clear the Director request records.
		if (const game::WorldObject* player =
			game::world_resolve(world, world.player);
			player != nullptr && player->type != 1001)
		{
			director_stop(runtime, world, file);
		}
		return CommandFlow::continue_execution;
	case 37: // SetActionCentre
		// Retail LANCER.EXE 0x00458e60 stores MissionObject_index_from_pointer
		// verbatim as the action-centre mission index, converts the radius to
		// float, and substitutes 220000.0 only when that conversion is zero.
		// The retained mission index resolves through the live table after
		// departure/recreation.
		world.action_center = {};
		runtime.action_center_object = UINT16_MAX;
		if (arguments[0].kind == ValueKind::object)
		{
			runtime.action_center_object =
				static_cast<std::uint16_t>(
					arguments[0].value);
			game::WorldObject* center = runtime_resolve_object(
				runtime,
				runtime.action_center_object,
				world);
			if (center != nullptr)
			{
				world.action_center = {
					static_cast<std::uint16_t>(
						center - std::begin(world.objects)),
					center->generation,
				};
			}
		}
		world.action_center_radius =
			arguments[1].value == 0
				? 220000.0f
				: static_cast<float>(arguments[1].value);
		return CommandFlow::continue_execution;
	case 38: // Dock
		// Retail LANCER.EXE 0x00458eb0 classifies operand two by its source-table
		// address. Direct objects use target kind zero plus the low words of
		// the stable mission index and docking-port operand; groups and sets
		// use kinds one/two and selector -1. LANCER.EXE 0x0040cc10 inserts AI command
		// 109. A direct target need not currently have a live object.
		if (arguments[0].kind == ValueKind::object)
		{
			game::WorldObject* object = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
			if (object != nullptr)
			{
				ai::TargetKind target_kind = ai::TargetKind::none;
				std::uint16_t target = UINT16_MAX;
				std::int16_t component = -1;
				bool supported_target = true;
				switch (arguments[1].kind)
				{
				case ValueKind::object:
					target_kind = ai::TargetKind::object;
					target = static_cast<std::uint16_t>(
						arguments[1].value);
					component = static_cast<std::int16_t>(
						arguments[2].value);
					break;
				case ValueKind::null_value:
					target_kind = ai::TargetKind::object;
					component = static_cast<std::int16_t>(
						arguments[2].value);
					break;
				case ValueKind::group:
					target_kind = ai::TargetKind::group;
					target = static_cast<std::uint16_t>(
						arguments[1].value);
					break;
				case ValueKind::reference_set:
					target_kind = ai::TargetKind::set;
					target = static_cast<std::uint16_t>(
						arguments[1].value);
					break;
				default:
					supported_target = false;
					break;
				}
				if (supported_target)
				{
					ai::command_push(world,
						*object,
						109,
						target_kind,
						target,
						component);
				}
			}
		}
		return CommandFlow::continue_execution;
	case 39: // DisableTaunts
		// Retail LANCER.EXE 0x00458f40 copies only the low 16 bits of its operand to
		// the taunts-disabled word and returns one. It performs no boolean
		// normalization and dispatches no secondary work.
		runtime.taunts_disabled =
			static_cast<std::uint16_t>(arguments[0].value);
		return CommandFlow::continue_execution;
	case 40: // Fly
	{
		// Retail LANCER.EXE 0x00458f50 expands the source reference. Its
		// LANCER.EXE 0x00458f70 visitor converts a direct destination to the stable
		// low-16 mission index (or 0xffff for null), inserts AI command 6
		// with selector -1, then writes the full speed dword through the
		// queue-head pointer regardless of insertion success.
		std::uint16_t destination = UINT16_MAX;
		if (arguments[1].kind == ValueKind::object)
		{
			// The reimplementation's direct AI references retain the
			// mission index and resolve it through the stable live handle.
			destination =
				static_cast<std::uint16_t>(arguments[1].value);
		}
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object != nullptr)
				{
					ai::command_push(world,
						*object,
						// Fly_apply_entity at 0x00458f70 passes command
						// ID 6 to AI_command_push. Command 119 belongs
						// exclusively to MovingShipBackupCurve.
						6,
						ai::TargetKind::object,
						destination);
					object->ai.commands[0].state[0] =
						arguments[2].value;
				}
			});
		return CommandFlow::continue_execution;
	}
	case 43: // DisableLights
		// Retail LANCER.EXE 0x00459070 expands the reference without consulting its
		// component selector. LANCER.EXE 0x00459100 toggles live-object flag 0x2000,
		// then LANCER.EXE 0x00459090 recursively clears mesh flag 0x40000 when
		// disabling (sets it when enabling) on source-flag-0x40 nodes.
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object != nullptr)
				{
					game::retained_set_lights_disabled(
						*object,
						arguments[1].value != 0);
				}
			});
		return CommandFlow::continue_execution;
	case 44: // SetEnvironmentFX
		// LANCER.EXE 0x459170: pass the full effect id and normalize the
		// second dword to a byte boolean before applying it immediately.
		environment_effect_set(
			runtime.environment,
			arguments[0].value,
			arguments[1].value != 0);
		return CommandFlow::continue_execution;
	case 45: // MultiPlayerSync
		// LANCER.EXE 0x4591e0 -> 0x4b58d0: arm the session barrier
		// (a no-op offline), then return zero to yield unconditionally.
		network_mark_session_ready(runtime.network);
		return CommandFlow::yield;
	case 46: // DisableGenericComms
		// LANCER.EXE 0x4591f0: copy only the first operand word to the
		// generic-comms disable latch; values are retained, not normalized.
		runtime.generic_comms_disabled =
			static_cast<std::uint16_t>(arguments[0].value);
		return CommandFlow::continue_execution;
	case 47: // DisableGuns
	case 51: // DisableMissiles
	case 52: // DisableEngines
	case 53: // DisableEject
	case 59: // DoNotDisturb
	case 73: // SetShipAvoidance
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object == nullptr) return;
				const bool state = arguments[1].value != 0;
				std::uint32_t flag = 0;
				if (command == 47)
				{
					// LANCER.EXE 0x459200/0x459220: the expanded-object
					// callback ignores components and toggles flag 0x8000
					// from the second operand's full-dword zero test.
					object->guns_disabled = state;
					flag = 0x8000u;
				}
				else if (command == 51)
				{
					// LANCER.EXE 0x459370/0x459390: toggle gameplay
					// flag 0x10000 for every expanded live object.
					object->missiles_disabled = state;
					flag = 0x10000u;
				}
				else if (command == 52)
				{
					// LANCER.EXE 0x4593e0/0x459400: toggle gameplay
					// flag 0x20000; this is distinct from 0x200000.
					object->engines_disabled = state;
					flag = 0x20000u;
				}
				else if (command == 53)
				{
					// LANCER.EXE 0x459450/0x459470: toggle gameplay
					// flag 0x40000 for manual-ejection admission.
					object->eject_disabled = state;
					flag = 0x40000u;
				}
				else if (command == 59)
				{
					// LANCER.EXE 0x459640/0x459660: toggle gameplay
					// flag 0x80000 on each expanded live object.
					object->do_not_disturb = state;
					flag = 0x80000u;
				}
				else
				{
					// LANCER.EXE 0x459a30/0x459a50: type 1001 is
					// immutable; all other expanded objects toggle
					// avoidance bit 0x100000 from a full-dword test.
					if (object->type == 1001)
					{
						return;
					}
					object->avoidance_disabled = state;
					flag = 0x100000u;
				}
				if (state) object->runtime_flags |= flag;
				else object->runtime_flags &= ~flag;
			});
		return CommandFlow::continue_execution;
	case 48: // SetNavPoint
	case 49: // SetEscortPoint
	{
		std::uint16_t target = UINT16_MAX;
		if (arguments[1].kind == ValueKind::object
			&& arguments[1].value < runtime.object_count)
		{
			const game::ObjectHandle handle =
				runtime.objects[arguments[1].value].live;
			if (game::world_resolve(world, handle) != nullptr)
			{
				// Retail stores the resolved live-object array index at
				// +0x720/+0x724, not the compiled mission-object index.
				target = handle.index;
			}
		}
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object != nullptr)
				{
					if (command == 48)
					{
						// LANCER.EXE 0x459270/0x459290: store the
						// resolved live index (or 0xffff) at +0x720;
						// component metadata is ignored.
						object->nav_point = target;
					}
					else
					{
						// LANCER.EXE 0x4592f0/0x459310: the escort
						// counterpart stores the same live index/null
						// convention at source object +0x724.
						object->escort_point = target;
					}
				}
			});
		return CommandFlow::continue_execution;
	}
	case 50: // ResetAfterBurners
		// LANCER.EXE 0x4594c0: refill only the current player's +0x5e8
		// fuel dword from ship stats +8 multiplied by 100. Active state
		// and gameplay flags are deliberately untouched.
		if (game::WorldObject* player =
			game::world_resolve(world, world.player))
		{
			if (player->type < assets::kShipStatsCount)
			{
				player->afterburner_fuel =
					stats.records[player->type].object
						.afterburner_seconds * 100;
			}
		}
		return CommandFlow::continue_execution;
	case 54: // SetHostile
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object != nullptr)
				{
					const bool hostile = arguments[1].value != 0;
					// LANCER.EXE 0x4594f0/0x459510 expands the selector,
					// tests operand two at full width, and writes canonical
					// zero/one to allegiance field +0x644.
					object->allegiance_class = hostile ? 1 : 0;
					object->hostile = hostile;
				}
			});
		return CommandFlow::continue_execution;
	case 55: // ResetToSpawnPositions
		if (runtime.network.role == NetworkRole::offline
			|| runtime.network.role == NetworkRole::host)
		{
			// LANCER.EXE 0x4591b0 accepts role zero and host role one,
			// selects the current scenario lazily, then calls 0x4af1d0
			// for the local slot with spawn -1 and publication enabled.
			// The respawn owner suppresses packet output offline.
			network_reset_player_to_spawn(
				runtime,
				world,
				stats,
				world.player.index,
				-1,
				true);
		}
		return CommandFlow::continue_execution;
	case 56: // UpdateEnvironmentFXState
		// LANCER.EXE 0x4591a0: commit pending environment state through
		// 0x469d30, then rebuild controller lights through 0x4a5a00.
		environment_state_commit(runtime.environment, world);
		environment_bind_controller_lights(
			runtime.environment, world);
		return CommandFlow::continue_execution;
	case 57: // SetPrimaryTarget
		// LANCER.EXE 0x459550: store the resolved live index first, then
		// resolve component-watch slot zero only for a non-null target.
		runtime.primary_target = UINT16_MAX;
		runtime.primary_target_component = -1;
		if (arguments[0].kind == ValueKind::object)
		{
			const std::int16_t component =
				arguments[0].component == UINT8_MAX
					? -1
					: arguments[0].component;
			const game::WorldObject* target =
				runtime_resolve_object(
					runtime,
					static_cast<std::uint16_t>(
						arguments[0].value),
					world);
			if (target != nullptr)
			{
				runtime.primary_target =
					static_cast<std::uint16_t>(
						target - std::begin(world.objects));
				runtime.primary_target_component = component;
			}
		}
		return CommandFlow::continue_execution;
	case 58: // WaitForJumpOrLaunch
	{
		// LANCER.EXE 0x4595a0/0x4595e0: latch any expanded live object
		// whose state passes mask 0x460 and whose head command is one of
		// the nine jump/launch IDs; a hit rewinds the PC by four and yields.
		bool waiting = false;
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				const game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				waiting = waiting
					|| (object != nullptr
						&& (object->runtime_flags & 0x460u) == 0
						&& ai::command_is_jump_or_launch(*object));
			});
		return waiting
			? CommandFlow::rewind_four_and_yield
			: CommandFlow::continue_execution;
	}
	case 60: // SetEnvironmentFXNebula
		// LANCER.EXE 0x459190: stage the operand's complete dword as the
		// pending nebula request; validation is deferred to command 56.
		environment_nebula_request(
			runtime.environment,
			static_cast<std::int32_t>(arguments[0].value));
		return CommandFlow::continue_execution;
	case 62: // SnapToPoint
		if (arguments[0].kind == ValueKind::object
			&& arguments[1].kind == ValueKind::object)
		{
			game::WorldObject* object = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
			const game::WorldObject* point =
				runtime_resolve_object(
					runtime,
					static_cast<std::uint16_t>(
						arguments[1].value),
					world);
			// LANCER.EXE 0x4596a0 rejects source state mask 0x10000840,
			// copies the point's position/orientation through the five
			// retained transform views, then zeroes motion at 0x403000.
			// Current/previous are the canonical equivalents of those
			// aliased transform views in this runtime.
			if (object != nullptr
				&& point != nullptr
				&& (object->runtime_flags & 0x10000840u) == 0)
			{
				set_position(*object, point->position);
				set_orientation(*object, point->orientation);
				object->scene_position = point->position;
				object->scene_orientation = point->orientation;
				game::world_zero_motion_controls(*object);
				const std::uint16_t live_index =
					static_cast<std::uint16_t>(
						object - std::begin(world.objects));
				if (live_index == world.player.index)
				{
					++runtime.player_motion_clear_serial;
				}
				(void)network_publish_object_state(
					runtime.network,
					world,
					stats,
					live_index,
					-1,
					true,
					true);
			}
		}
		return CommandFlow::continue_execution;
	case 63: // PlayFostersLastStand
		// LANCER.EXE 0x459740 publishes literal dword one to the Foster's
		// Last Stand latch and returns that same one as the callback result.
		runtime.fosters_last_stand = true;
		context.last_result = 1;
		return CommandFlow::continue_execution;
	case 64: // OpenInstrument
	case 65: // CloseInstrument
		if (arguments[0].value < std::size(runtime.instruments))
		{
			// LANCER.EXE 0x45d9d0/0x45da30 dispatches the HUD helper
			// before publishing the retained instrument open word.
			InstrumentState& instrument =
				runtime.instruments[arguments[0].value];
			instrument.open = command == 64;
			++instrument.serial;
			if (command == 64 && arguments[0].value == 11)
			{
				// OpenInstrument_command 0x0045d9e9 writes the COMMS
				// root sentinel before invoking its complete rebuild.
				player_comms_open_menu(runtime, world, stats);
			}
			else if (command == 65 && arguments[0].value == 11)
			{
				// CloseInstrument 0x45da30 clears the retained open word
				// after its HUD close request; also discard COMMS options.
				player_comms_close_menu(runtime);
			}
			if (command == 64 && arguments[0].value == 10)
			{
				// OpenInstrument_command (LANCER.EXE 0x0045da06):
				// instrument ten conditionally invokes the common close
				// helper for incompatible instrument thirteen. The HUD
				// consumer performs the same open/opening-state gate.
				runtime.instruments[13].open = false;
				++runtime.instruments[13].serial;
			}
		}
		return CommandFlow::continue_execution;
	case 66: // DestroySubObject
		if (arguments[0].kind == ValueKind::object)
		{
			game::WorldObject* object = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
			const std::int16_t component =
				arguments[0].component == UINT8_MAX
					? -1
					: arguments[0].component;
			if (object != nullptr && component >= 0)
			{
				// LANCER.EXE 0x459750 resolves one direct object and
				// component-watch slot zero, then visits every model
				// member sharing the selected part-group identity.
				game::retained_destroy_component_group(
					*object,
					component,
					arguments[1].value != 0);
			}
		}
		return CommandFlow::continue_execution;
	case 67: // SetObjective
		// LANCER.EXE 0x459870: missions 0..35 store operand two's low word
		// in the ten-entry mission table; full dword state 2 additionally
		// publishes operand one's low word as the current objective.
		if (runtime.mission_number >= 1
			&& runtime.mission_number <= 36
			&& arguments[0].value < std::size(runtime.objectives))
		{
			const std::uint16_t objective =
				static_cast<std::uint16_t>(arguments[0].value);
			runtime.objectives[objective] =
				static_cast<std::int16_t>(arguments[1].value);
			if (arguments[1].value == 2)
			{
				runtime.current_objective = objective;
			}
			diagnostics::mission_log(
				"objective index=%u state=%u current=%u tick=%u",
				arguments[0].value,
				arguments[1].value,
				static_cast<unsigned>(runtime.current_objective),
				simulation_tick);
		}
		return CommandFlow::continue_execution;
	case 68: // SetRescueProbabilities
		// LANCER.EXE 0x4598d0 copies all three operand dwords verbatim.
		runtime.rescue_probability =
			arguments[0].value;
		runtime.capture_probability =
			arguments[1].value;
		runtime.destroyed_probability =
			arguments[2].value;
		return CommandFlow::continue_execution;
	case 69: // IsShipThisPlayer
		// LANCER.EXE 0x4598f0 returns (resolved live index != player) + 1:
		// exactly one for the player and two for null or every other ship.
		context.last_result = 2;
		if (arguments[0].kind == ValueKind::object)
		{
			const game::WorldObject* object =
				runtime_resolve_object(
					runtime,
					static_cast<std::uint16_t>(
						arguments[0].value),
					world);
			if (object != nullptr
				&& static_cast<std::uint16_t>(
					object - std::begin(world.objects))
					== world.player.index)
			{
				context.last_result = 1;
			}
		}
		return CommandFlow::continue_execution;
	case 70: // SetFlybackMarker
		// LANCER.EXE 0x459910/0x459960 resets both ten-entry tables,
		// expands operand one, skips type 1001, stores each live index,
		// and converts operand two through zero-extended FILD qword.
		runtime.flyback_marker_count = 0;
		std::fill(
			std::begin(runtime.flyback_markers),
			std::end(runtime.flyback_markers),
			UINT16_MAX);
		std::fill(
			std::begin(runtime.flyback_radii),
			std::end(runtime.flyback_radii),
			0.0f);
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				const game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object == nullptr || object->type == 1001)
				{
					return;
				}
				if (runtime.flyback_marker_count
					>= std::size(runtime.flyback_markers))
				{
					return;
				}
				const std::uint8_t slot =
					runtime.flyback_marker_count++;
				runtime.flyback_markers[slot] =
					static_cast<std::uint16_t>(
						object - std::begin(world.objects));
				runtime.flyback_radii[slot] =
					static_cast<float>(arguments[1].value);
			});
		return CommandFlow::continue_execution;
	case 71: // ResetFlybackMarker
		// LANCER.EXE 0x4599e0 clears both ten-entry tables and count,
		// then writes dword -1 to the current player's nav point +0x720.
		runtime.flyback_marker_count = 0;
		std::fill(
			std::begin(runtime.flyback_markers),
			std::end(runtime.flyback_markers),
			UINT16_MAX);
		std::fill(
			std::begin(runtime.flyback_radii),
			std::end(runtime.flyback_radii),
			0.0f);
		if (game::WorldObject* player =
			game::world_resolve(world, world.player))
		{
			player->nav_point = UINT16_MAX;
		}
		return CommandFlow::continue_execution;
	case 74: // MatchSpeed
		// LANCER.EXE 0x459a90 accepts only the resolved current player;
		// nonzero calls 0x412c10 before publishing word one, while zero
		// clears the retained word without restoring saved throttle.
		if (arguments[0].kind == ValueKind::object)
		{
			game::WorldObject* object = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
			if (object != nullptr
				&& static_cast<std::uint16_t>(
					object - std::begin(world.objects))
					== world.player.index)
			{
				runtime_publish_match_speed_request(
					runtime,
					arguments[1].value != 0);
			}
		}
		return CommandFlow::continue_execution;
	case 75: // MovingShipBackupCurve
		// LANCER.EXE 0x458670/0x458690 expands the source, queues AI
		// command 119, writes curve/duration, and maps null moving origin
		// -1 to literal zero in the third payload dword.
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object != nullptr)
				{
					queue_ai_curve(
						world,
						*object,
						119,
						arguments[1],
						arguments[2].value,
						arguments[3].kind
								== ValueKind::null_value
							? ExecutorValue{
								0, ValueKind::scalar}
							: arguments[3]);
				}
			});
		return CommandFlow::continue_execution;
	case 76: // WaitForKey
	{
		// LANCER.EXE 0x459ae0 publishes the action dword, accepts a held
		// configured primary/fallback input, otherwise rewinds four bytes
		// and yields; acceptance clears the published action to -1.
		const std::uint32_t action = arguments[0].value;
		runtime.waiting_control = action;
		if (action < kControlActionCount
			&& runtime.wait_for_key_accepted[action])
		{
			runtime.waiting_control = UINT32_MAX;
			return CommandFlow::continue_execution;
		}
		return CommandFlow::rewind_four_and_yield;
	}
	case 77: // TerminateMission
		// LANCER.EXE 0x459bb0 increments the termination-request counter as
		// a wrapping dword; the boolean is the derived nonzero latch.
		runtime.terminate_requested = true;
		++runtime.terminate_request_count;
		return CommandFlow::continue_execution;
	case 78: // TurretSetTarget
		// LANCER.EXE 0x459bd0 expands the first reference through the
		// standard visitor. Its 0x459bf0 callback resolves the watched
		// component and assigns argument one's live index to every
		// matching kind-one gun mount, clearing each target component.
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t component,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object != nullptr && component >= 0)
				{
					std::uint16_t target_live = UINT16_MAX;
					if (arguments[1].kind == ValueKind::object)
					{
						const game::WorldObject* target =
							runtime_resolve_object(
								runtime,
								static_cast<std::uint16_t>(
									arguments[1].value),
								world);
						if (target != nullptr)
						{
							target_live =
								static_cast<std::uint16_t>(
									target
										- std::begin(world.objects));
						}
					}
					game::retained_set_turret_target(
						*object, component, target_live);
				}
			});
		return CommandFlow::continue_execution;
	case 79: // SetAnyTriggerState
		// LANCER.EXE 0x45d3a0 scans the owner's trigger span, counts
		// type matches with a wrapping byte even when their selector is
		// inactive, and changes only the requested occurrence's enabled
		// byte. It deliberately leaves repeats_remaining untouched.
		set_trigger_state(
			executor,
			runtime,
			arguments[0],
			static_cast<std::uint8_t>(arguments[1].value),
			arguments[2].value,
			false,
			static_cast<std::uint8_t>(arguments[3].value));
		return CommandFlow::continue_execution;
	case 80: // WaitForDirectorCam
		// LANCER.EXE 0x459c90 waits solely while the camera-mode word is
		// 13, rewinding the command's two-byte encoding before yielding.
		// It does not inspect queued shots or the director waiting latch.
		return runtime.director.mode == 13
			? CommandFlow::rewind_two_and_yield
			: CommandFlow::continue_execution;
	case 81: // KillAllScriptExecutionExceptMe
		// LANCER.EXE 0x45d990 scans all 32 context records, clears only
		// each active non-current record's execution marker, and decrements
		// the active count. Stacks, watches, timers, and other record state
		// are deliberately left intact for normal slot reuse.
		for (ExecutorContext& candidate : executor.contexts)
		{
			if (&candidate == &context || !candidate.active)
			{
				continue;
			}
			candidate.active = false;
			if (executor.active_contexts != 0)
			{
				--executor.active_contexts;
			}
		}
		return CommandFlow::continue_execution;
	case 82: // StackDirectorCam
		// LANCER.EXE 0x458300 converts and appends the same six-dword shot
		// as StartDirectorCam, but does not clear the ten-record queue.
		// The 0x461d30 append path starts mode 13 immediately only when
		// this shot becomes the sole (active) queue record.
		queue_director(runtime, world, file, arguments, false);
		return CommandFlow::continue_execution;
	case 83: // Scanner
		// LANCER.EXE 0x459cb0 publishes the target's live index (or -1)
		// and zeros the scanner-beep deadline plus the lock-animation
		// deadline and frame. The serial makes the HUD repeat all three
		// resets even when a script reissues the same target.
		runtime.scanner_target = UINT16_MAX;
		if (arguments[0].kind == ValueKind::object)
		{
			const game::WorldObject* target =
				runtime_resolve_object(
					runtime,
					static_cast<std::uint16_t>(
						arguments[0].value),
					world);
			if (target != nullptr)
			{
				runtime.scanner_target =
					static_cast<std::uint16_t>(
						target - std::begin(world.objects));
			}
		}
		++runtime.scanner_serial;
		return CommandFlow::continue_execution;
	case 84: // ReplaceSubObject
		// LANCER.EXE 0x459cf0 handles exactly one watched source component
		// and one replacement object. It publishes the component's
		// concatenated world pose to every replacement pose snapshot,
		// applies the type-0x91 basis correction, then hides only that node.
		if (arguments[0].kind == ValueKind::object
			&& arguments[0].component != UINT8_MAX
			&& arguments[1].kind == ValueKind::object)
		{
			game::WorldObject* source = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
			game::WorldObject* replacement = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[1].value),
				world);
			if (source != nullptr && replacement != nullptr
				&& arguments[0].component
					< source->component_count)
			{
				game::retained_replace_component(
					*source,
					arguments[0].component,
					*replacement);
			}
		}
		return CommandFlow::continue_execution;
	case 85: // Fire
		// LANCER.EXE 0x459dd0 resolves exactly one object and passes the
		// second operand unchanged to ship_apply_gun_cooldown (0x47b1f0),
		// which adds it to the wrapping current simulation tick.
		if (arguments[0].kind == ValueKind::object)
		{
			game::WorldObject* object = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
			if (object != nullptr)
			{
				game::weapons_apply_gun_cooldown(
					world,
					*object,
					simulation_tick,
					arguments[1].value);
			}
		}
		return CommandFlow::continue_execution;
	case 86: // MultiplayerScriptSync
	{
		// LANCER.EXE 0x459df0 indexes one of 128 twelve-byte states. Offline
		// execution passes immediately; networked states rewind four bytes
		// and yield until the host/client opcode-0x47/0x48 handshake and
		// strict signed deadline comparison have completed.
		if (arguments[0].value >= std::size(runtime.script_sync))
		{
			return CommandFlow::continue_execution;
		}
		const std::uint8_t sync_index =
			static_cast<std::uint8_t>(arguments[0].value);
		return network_script_sync_poll(
			runtime.network,
			runtime.script_sync[sync_index],
			sync_index)
			? CommandFlow::continue_execution
			: CommandFlow::rewind_four_and_yield;
	}
	case 87: // FriendlyFire
		if (game::WorldObject* player =
			game::world_resolve(world, world.player))
		{
			// LANCER.EXE 0x459f30 passes zero to 0x474e00, assigning local
			// player status one without sending a packet. The later
			// consequence dispatcher publishes opcode 0x49/payload zero;
			// payload-one promotion belongs only to protected damage.
			network_flag_local_friendly_fire(
				runtime.network, *player, false);
		}
		return CommandFlow::continue_execution;
	case 88: // Cloak
		// LANCER.EXE 0x459f40 expands the first reference through the
		// standard visitor; callback 0x459f60 ignores components, resolves
		// each live index, and passes the second operand's full-dword
		// zero/nonzero state to Cloak_set_active (0x463560).
		visit_reference(
			arguments[0],
			runtime,
			world,
			[&](std::uint16_t object_index,
				std::int16_t,
				std::uint16_t)
			{
				game::WorldObject* object =
					runtime_resolve_object(
						runtime, object_index, world);
				if (object != nullptr)
				{
					game::world_set_cloak_active(
						world,
						*object,
						arguments[1].value != 0,
						simulation_tick);
				}
			});
		return CommandFlow::continue_execution;
	case 89: // ReplenishWeapons
		// LANCER.EXE 0x459fa0 resolves one direct object, destroys and
		// rebuilds its ordnance inventory, restores ship resources and all
		// eight shield banks, refreshes derived subsystem ratios, and
		// rebuilds the local HUD summary. Deathmatch preserves fuel and
		// leaves the cleared attachment list empty.
		if (arguments[0].kind == ValueKind::object)
		{
			if (game::WorldObject* object = runtime_resolve_object(
					runtime,
					static_cast<std::uint16_t>(
						arguments[0].value),
					world))
			{
				game::attachments_replenish(
					*object,
					stats,
					network_is_deathmatch_mission(
						runtime.mission_number),
					false);
				game::world_update_shield_ratios(*object, stats);
				if (static_cast<std::uint16_t>(
						object - std::begin(world.objects))
					== world.player.index)
				{
					++runtime.player_ordnance_rebuild_serial;
				}
			}
		}
		return CommandFlow::continue_execution;
	case 90: // WillsBlag
		// LANCER.EXE 0x45a1c0 clears bit zero of the referenced mission
		// record at +0x17, writes 100 to the live object's ejection roll
		// at +0x70c, then clears runtime flag 0x800 at +0x08.
		if (arguments[0].kind == ValueKind::object)
		{
			const std::uint16_t mission_index =
				static_cast<std::uint16_t>(arguments[0].value);
			if (mission_index < runtime.object_count)
			{
				runtime.objects[mission_index].script_flags &=
					static_cast<std::uint8_t>(~1u);
			}
			game::WorldObject* object = runtime_resolve_object(
				runtime,
				mission_index,
				world);
			if (object != nullptr)
			{
				object->ejection_roll = 100;
				object->runtime_flags &= ~0x800u;
			}
		}
		return CommandFlow::continue_execution;
	case 91: // ShowHudIcon
		// LANCER.EXE 0x45a1f0 treats operand zero as an index into the
		// twenty eight-byte HUD channels, copies operand one to the mode
		// dword, and zeroes that channel's blink phase. The serial is the
		// reimplementation's handoff token which makes the HUD-owned phase
		// perform that same reset when it consumes the command.
		if (arguments[0].value < std::size(runtime.hud_icons))
		{
			HudIconCommandState& icon =
				runtime.hud_icons[
					static_cast<std::uint32_t>(
						arguments[0].value)];
			icon.mode =
				static_cast<std::int32_t>(arguments[1].value);
			++icon.serial;
		}
		return CommandFlow::continue_execution;
	case 92: // DisableListing
		// LANCER.EXE 0x45a210 resolves one direct object and maps the
		// second operand's full-dword zero/nonzero value directly onto
		// runtime flag 0x20000000. Player-comms listing filters consume the
		// resulting state.
		if (arguments[0].kind == ValueKind::object)
		{
			game::WorldObject* object = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
			if (object != nullptr)
			{
				if (arguments[1].value != 0)
					object->runtime_flags |= 0x20000000u;
				else
					object->runtime_flags &= ~0x20000000u;
			}
		}
		return CommandFlow::continue_execution;
	case 93: // DisableObjectAtNextJump
		// LANCER.EXE 0x45a250 resolves one direct object, indexes the
		// deferred table at 0x55230c by its live slot, and stores one for
		// nonzero or two for zero. Environment_state_commit consumes and
		// clears those bytes at the next jump transaction boundary.
		if (arguments[0].kind == ValueKind::object)
		{
			const game::WorldObject* object =
				runtime_resolve_object(
					runtime,
					static_cast<std::uint16_t>(
						arguments[0].value),
					world);
			if (object != nullptr)
			{
				const std::uint16_t live_index =
					static_cast<std::uint16_t>(
						object - std::begin(world.objects));
				environment_queue_object_state(
					runtime.environment,
					live_index,
					arguments[1].value != 0);
			}
		}
		return CommandFlow::continue_execution;
	case 94: // DarrensNaughtyBlag
		// LANCER.EXE 0x45a290 converts both direct references to live
		// slots, constructs a zero-roll look-at basis from the first
		// object's +0x84 position to the second's, then 0x49b650 copies
		// it to all five of the first object's orientation caches. Current
		// and previous orientation are those caches' retained equivalents.
		if (arguments[0].kind == ValueKind::object
			&& arguments[1].kind == ValueKind::object)
		{
			game::WorldObject* first = runtime_resolve_object(
				runtime,
				static_cast<std::uint16_t>(arguments[0].value),
				world);
			const game::WorldObject* second =
				runtime_resolve_object(
					runtime,
					static_cast<std::uint16_t>(
						arguments[1].value),
					world);
			if (first != nullptr && second != nullptr)
			{
				set_orientation(
					*first,
					look_at(first->position, second->position));
			}
		}
		return CommandFlow::continue_execution;
	default:
		diagnostics::mission_log(
			"executor invalid command=%u",
			static_cast<unsigned>(command));
		return CommandFlow::yield;
	}
}
}
