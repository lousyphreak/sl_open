#include "mission/events.hpp"

#include "assets/gameplay_model.hpp"
#include "core/mission_log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sl_open::mission
{
namespace
{
struct EventArgumentDescriptor
{
	std::uint32_t flags{};
	bool condition{};
};

struct EventDescriptor
{
	const char* name{};
	EventArgumentDescriptor arguments[5]{};
	std::uint8_t argument_count{};
	std::uint8_t cache_slot{UINT8_MAX};
	std::uint8_t repeat_gate{UINT8_MAX};
	enum class Scope : std::uint8_t
	{
		none,
		shot_at,
		destroyed,
		all_targets,
	} scope{Scope::none};
};

constexpr EventArgumentDescriptor object_argument{
	0x400u, true};
constexpr EventArgumentDescriptor weapon_argument{
	0x80000u, true};
constexpr EventArgumentDescriptor scalar_argument{
	0x80u, false};
constexpr EventArgumentDescriptor damage_argument{
	0x1000u, false};

constexpr EventDescriptor kEvents[35] = {
	{"ShotAt", {object_argument, damage_argument, damage_argument,
		object_argument, weapon_argument}, 5, 0, UINT8_MAX,
		EventDescriptor::Scope::shot_at},
	{"Destroyed", {object_argument, object_argument}, 2, 1, 1,
		EventDescriptor::Scope::destroyed},
	{"Launched", {object_argument}, 1},
	{"CameraReached"},
	{"ShipReached", {object_argument}, 1},
	{"CloseProximity", {object_argument, scalar_argument}, 2},
	{"Proximity", {object_argument, scalar_argument}, 2},
	{"ObjectScooped", {object_argument}, 1},
	{"PlayerReadyToJump"},
	{"JumpedIn", {object_argument}, 1},
	{"FixedGateJumpedIn", {object_argument}, 1},
	{"PlayerReadyToWarp"},
	{"JumpedThroughHoop"},
	{"PlayerWantsBackup"},
	{"RipperGrabbedObject", {object_argument}, 1},
	{"RipperDroppedObject", {object_argument}, 1},
	{"Cloaked", {object_argument}, 1, UINT8_MAX, 1,
		EventDescriptor::Scope::all_targets},
	{"Decloaked", {object_argument}, 1, UINT8_MAX, 1,
		EventDescriptor::Scope::all_targets},
	{"Targetted", {object_argument}, 1},
	{"Player_L1_DoubleTap"},
	{"Player_L2_DoubleTap"},
	{"Player_R1_DoubleTap"},
	{"Player_R2_DoubleTap"},
	{"Player_L1_L2_R1_R2 Pressed"},
	{"Player_L1_R1 Pressed"},
	{"Game Timer Expired"},
	{"Tractor Beam Locked", {object_argument, object_argument}, 2},
	{"Tractor Beam Broken", {object_argument, object_argument}, 2},
	{"Inside Object"},
	{"Outside Object"},
	{"Docked"},
	{"UnDocked"},
	{"I'm Being Chased", {object_argument, object_argument}, 2},
	{"Player_CallReinforcements", {object_argument}, 1},
	{"ExplosionShip", {object_argument}, 1},
};

constexpr std::uint32_t packed_object(std::uint16_t index)
{
	return index == UINT16_MAX
		? UINT32_MAX
		: 0xff000000u | index;
}

int span_group_owner(
	const Runtime& runtime,
	std::uint16_t span_index)
{
	for (std::uint16_t index = 0;
		index < runtime.group_count;
		++index)
	{
		if (runtime.groups[index].reference_span == span_index)
		{
			return index;
		}
	}
	return -1;
}

int span_set_owner(
	const Runtime& runtime,
	std::uint16_t span_index)
{
	for (std::uint16_t index = 0;
		index < runtime.reference_set_count;
		++index)
	{
		if (runtime.reference_sets[index].reference_span == span_index)
		{
			return index;
		}
	}
	return -1;
}

bool set_contains(
	const Runtime& runtime,
	std::uint16_t set_index,
	std::uint16_t object_index,
	std::uint8_t selector)
{
	ExpandedTargetReference targets[game::kMaxMissionObjects];
	const std::uint16_t count = runtime_expand_target_reference(
		runtime,
		ReferenceKind::set,
		set_index,
		targets,
		static_cast<std::uint16_t>(std::size(targets)));
	for (std::uint16_t index = 0; index < count; ++index)
	{
		if (targets[index].object == object_index
			&& (targets[index].model < 0
				? UINT8_MAX
				: static_cast<std::uint8_t>(
					targets[index].model)) == selector)
		{
			return true;
		}
	}
	return false;
}

bool object_destroyed(
	const Runtime& runtime,
	std::uint16_t object,
	std::uint8_t selector)
{
	if (object >= runtime.object_count)
	{
		return false;
	}
	if (selector == UINT8_MAX)
	{
		return (runtime.objects[object].script_flags & 1u) != 0;
	}
	return (runtime.objects[object].live_component_mask
		& (1u << (selector & 31u))) == 0;
}

std::uint16_t damage_percent(
	const Runtime& runtime,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t object,
	std::uint8_t selector)
{
	const game::WorldObject* live =
		runtime_resolve_object(runtime, object, world);
	if (live == nullptr)
	{
		return 0;
	}
	float current = 0.0f;
	float maximum = 100.0f;
	if (selector == UINT8_MAX)
	{
		current = *std::min_element(
			std::begin(live->secondary_shields),
			std::end(live->secondary_shields));
		if (live->type < assets::kShipStatsCount)
		{
			maximum = static_cast<float>(
				stats.records[live->type].object
					.structural_bank_max * 6);
		}
	}
	else if (selector < live->component_count)
	{
		current = live->components[selector].health;
		maximum =
			live->components[selector].maximum_health;
	}
	if (current < 0.0f)
	{
		return 100;
	}
	const float result =
		100.0f * (maximum - current) / maximum;
	return std::isfinite(result)
		? static_cast<std::uint16_t>(
			static_cast<std::int32_t>(result))
		: 0;
}

bool packed_condition_matches(
	const Runtime& runtime,
	std::uint32_t candidate,
	std::uint32_t expected,
	std::uint32_t flags,
	std::uint8_t event_type)
{
	if ((flags & 0x80u) != 0)
	{
		return (event_type == 5 || event_type == 6)
			? candidate <= expected
			: candidate == expected;
	}
	if ((flags & 0x400u) != 0
		&& static_cast<std::uint16_t>(expected) != UINT16_MAX
		&& (expected & 0x2000u) != 0)
	{
		if ((candidate & 0xffff0000u) != 0xff000000u)
		{
			return false;
		}
			const std::uint16_t object =
				static_cast<std::uint16_t>(candidate);
			return object < runtime.player_prefix_count;
	}
	const std::uint16_t index =
		static_cast<std::uint16_t>(expected);
	const std::uint8_t kind =
		static_cast<std::uint8_t>(expected >> 16);
	std::uint32_t resolved = UINT32_MAX;
	if (kind == 0 && index < runtime.object_count)
	{
		resolved = packed_object(index);
	}
	else if (kind == 1 && index < runtime.group_count)
	{
		resolved = 0xff010000u | index;
	}
	else if (kind == 0x16 && index < runtime.reference_set_count)
	{
		resolved = 0xff160000u | index;
	}
	return candidate == resolved;
}

bool trigger_conditions_match(
	const Runtime& runtime,
	const TriggerRecord& trigger,
	const QueuedEvent& event)
{
	if (event.type >= std::size(kEvents))
	{
		return false;
	}
	const EventDescriptor& descriptor = kEvents[event.type];
	for (std::uint8_t index = 0;
		index < event.argument_count
			&& index < descriptor.argument_count
			&& index < std::size(trigger.condition);
		++index)
	{
		const std::uint32_t expected =
			trigger.condition[index];
		if (static_cast<std::uint16_t>(expected) == UINT16_MAX
			|| !descriptor.arguments[index].condition)
		{
			continue;
		}
		if (!packed_condition_matches(
			runtime,
			event.arguments[index],
			expected,
			descriptor.arguments[index].flags,
			event.type))
		{
			return false;
		}
	}
	return true;
}

bool trigger_matches(
	const Runtime& runtime,
	const TriggerRecord& trigger,
	const QueuedEvent& event,
	std::uint8_t selector,
	bool aggregate)
{
	if (trigger.enabled == 0
		|| trigger.type != event.type
		|| trigger.selector != selector
		|| trigger.script_word == UINT16_MAX)
	{
		return false;
	}
	if (!aggregate
		&& trigger.repeat_mode
			!= kEvents[event.type].repeat_gate)
	{
		return false;
	}
	return trigger_conditions_match(runtime, trigger, event);
}

void cache_arguments(
	Runtime& runtime,
	std::uint16_t span,
	const QueuedEvent& event)
{
	if (span >= runtime.reference_span_count
		|| event.type >= std::size(kEvents))
	{
		return;
	}
	const std::uint8_t slot =
		kEvents[event.type].cache_slot;
	if (slot == UINT8_MAX)
	{
		return;
	}
	std::copy_n(
		event.arguments,
		std::min<std::uint8_t>(event.argument_count, 5),
		runtime.event_captures[span][slot]);
}

bool span_has_match(
	Runtime& runtime,
	std::uint16_t span_index,
	const QueuedEvent& event,
	std::uint8_t selector)
{
	if (span_index == UINT16_MAX
		|| span_index >= runtime.reference_span_count)
	{
		return false;
	}
	cache_arguments(runtime, span_index, event);
	const ReferenceSpan& span =
		runtime.reference_spans[span_index];
	for (std::uint16_t ordinal = 0;
		ordinal < span.trigger_count;
		++ordinal)
	{
		if (trigger_matches(
			runtime,
			runtime.triggers[span.first_trigger + ordinal],
			event,
			selector,
			true))
		{
			return true;
		}
	}
	return false;
}

bool enqueue(Runtime& runtime, const QueuedEvent& event)
{
	if (event.source >= runtime.object_count)
	{
		return false;
	}
	if (runtime.event_count >= game::kMaxMissionEvents)
	{
		if (!runtime.event_overflow_logged)
		{
			runtime.event_overflow_logged = true;
			diagnostics::mission_log(
				"event queue full capacity=%u dropping type=%u source=%u",
				static_cast<unsigned>(game::kMaxMissionEvents),
				static_cast<unsigned>(event.type),
				static_cast<unsigned>(event.source));
		}
		return false;
	}
	runtime.events[runtime.event_count++] = event;
	runtime.event_high_water =
		std::max(runtime.event_high_water, runtime.event_count);
	return true;
}

bool enqueue_direct_if_matched(
	Runtime& runtime,
	const QueuedEvent& event)
{
	return span_has_match(
		runtime,
		runtime.objects[event.source].reference_span,
		event,
		event.selector)
		? enqueue(runtime, event)
		: true;
}

bool enqueue_propagating_if_matched(
	Runtime& runtime,
	const QueuedEvent& event)
{
	if (span_has_match(
		runtime,
		runtime.objects[event.source].reference_span,
		event,
		event.selector))
	{
		return enqueue(runtime, event);
	}
	const std::uint8_t group = runtime.objects[event.source].group;
	if (group != UINT8_MAX && group < runtime.group_count
		&& span_has_match(
			runtime,
			runtime.groups[group].reference_span,
			event,
			UINT8_MAX))
	{
		return enqueue(runtime, event);
	}
	for (std::uint16_t set = 0;
		set < runtime.reference_set_count;
		++set)
	{
		if (set_contains(
			runtime, set, event.source, event.selector)
			&& span_has_match(
				runtime,
				runtime.reference_sets[set].reference_span,
				event,
				UINT8_MAX))
		{
			return enqueue(runtime, event);
		}
	}
	return true;
}

void aggregate_targets(
	const Runtime& runtime,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	const ExpandedTargetReference* targets,
	std::uint16_t count,
	const QueuedEvent& source,
	QueuedEvent& delivered,
	bool& aggregate)
{
	aggregate = true;
	if (source.type
		== static_cast<std::uint8_t>(EventType::shot_at))
	{
		std::uint16_t first = 0;
		std::uint16_t second = 0;
		for (std::uint16_t index = 0; index < count; ++index)
		{
			const std::uint16_t damage = damage_percent(
				runtime,
				world,
				stats,
				targets[index].object,
				targets[index].model < 0
					? UINT8_MAX
					: static_cast<std::uint8_t>(
						targets[index].model));
			first =
				static_cast<std::uint16_t>(first + damage);
			second =
				static_cast<std::uint16_t>(second + damage);
		}
		if (count != 0)
		{
			delivered.arguments[1] =
				static_cast<std::int16_t>(first)
				/ static_cast<std::int32_t>(count);
			delivered.arguments[2] =
				static_cast<std::int16_t>(second)
				/ static_cast<std::int32_t>(count);
		}
	}
	else if (source.type
		== static_cast<std::uint8_t>(EventType::destroyed))
	{
		for (std::uint16_t index = 0; index < count; ++index)
		{
			aggregate = aggregate && object_destroyed(
				runtime,
				targets[index].object,
				targets[index].model < 0
					? UINT8_MAX
					: static_cast<std::uint8_t>(
						targets[index].model));
		}
	}
}

bool dispatch_span(
	Runtime& runtime,
	Executor& executor,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	const QueuedEvent& source,
	std::uint16_t span_index,
	std::uint8_t selector,
	bool scope,
	std::uint32_t script_tick,
	std::uint32_t simulation_tick)
{
	if (span_index == UINT16_MAX
		|| span_index >= runtime.reference_span_count)
	{
		return true;
	}
	QueuedEvent event = source;
	bool aggregate = true;
	if (scope && source.type < std::size(kEvents)
		&& kEvents[source.type].scope
			!= EventDescriptor::Scope::none)
	{
		ExpandedTargetReference targets[
			game::kMaxMissionObjects];
		std::uint16_t count = 0;
		const int group = span_group_owner(runtime, span_index);
		const int set = span_set_owner(runtime, span_index);
		if (group >= 0)
		{
			count = runtime_expand_target_reference(
				runtime,
				ReferenceKind::group,
				static_cast<std::uint16_t>(group),
				targets,
				static_cast<std::uint16_t>(
					std::size(targets)));
		}
		else if (set >= 0)
		{
			count = runtime_expand_target_reference(
				runtime,
				ReferenceKind::set,
				static_cast<std::uint16_t>(set),
				targets,
				static_cast<std::uint16_t>(
					std::size(targets)));
		}
		aggregate_targets(
			runtime,
			world,
			stats,
			targets,
			count,
			source,
			event,
			aggregate);
	}
	cache_arguments(runtime, span_index, event);
	const ReferenceSpan& span =
		runtime.reference_spans[span_index];
	for (std::uint16_t ordinal = 0;
		ordinal < span.trigger_count;
		++ordinal)
	{
		const std::uint16_t trigger_index =
			static_cast<std::uint16_t>(
				span.first_trigger + ordinal);
		TriggerRecord& trigger =
			runtime.triggers[trigger_index];
		if (!trigger_matches(
			runtime, trigger, event, selector, aggregate))
		{
			continue;
		}
		executor_start_trigger(
			executor,
			runtime,
			world,
			stats,
			file,
			trigger.script_word,
			static_cast<std::uint8_t>(trigger_index),
			event.arguments,
			event.argument_count,
			trigger.deferred != 0,
			script_tick,
			simulation_tick);
		if (runtime.trigger_log_tick[event.type] != script_tick)
		{
			runtime.trigger_log_tick[event.type] = script_tick;
			diagnostics::mission_log(
				"trigger event=%s source=%u trigger=%u script_word=%u tick=%u",
				kEvents[event.type].name,
				static_cast<unsigned>(event.source),
				static_cast<unsigned>(trigger_index),
				static_cast<unsigned>(trigger.script_word),
				script_tick);
		}
		if (trigger.repeat_mode == 0)
		{
			trigger.enabled = 0;
		}
		else if (trigger.repeat_mode == 2)
		{
			if (trigger.repeats_remaining == 0
				|| --trigger.repeats_remaining == 0)
			{
				trigger.enabled = 0;
			}
		}
	}
	return !executor.failed;
}

QueuedEvent make_event(
	EventType type,
	std::uint16_t source,
	const std::uint32_t* arguments,
	std::uint8_t argument_count,
	std::uint8_t selector,
	bool propagate)
{
	QueuedEvent event;
	event.propagate_scopes = propagate;
	event.source = source;
	event.type = static_cast<std::uint8_t>(type);
	event.argument_count = argument_count;
	event.selector = selector;
	if (arguments != nullptr)
	{
		std::copy_n(
			arguments,
			std::min<std::uint8_t>(
				argument_count,
				game::kMissionEventArguments),
			event.arguments);
	}
	return event;
}
}

std::uint8_t events_argument_count(EventType type)
{
	const std::uint8_t index = static_cast<std::uint8_t>(type);
	return index < std::size(kEvents)
		? kEvents[index].argument_count
		: UINT8_MAX;
}

std::uint8_t events_capture_slot(EventType type)
{
	const std::uint8_t index = static_cast<std::uint8_t>(type);
	return index < std::size(kEvents)
		? kEvents[index].cache_slot
		: UINT8_MAX;
}

bool events_emit_direct(
	Runtime& runtime,
	EventType type,
	std::uint16_t source,
	const std::uint32_t* arguments,
	std::uint8_t argument_count,
	std::uint8_t selector)
{
	if (source >= runtime.object_count)
	{
		return false;
	}
	return enqueue_direct_if_matched(
		runtime,
		make_event(
			type,
			source,
			arguments,
			argument_count,
			selector,
			false));
}

bool events_emit_propagating(
	Runtime& runtime,
	EventType type,
	std::uint16_t source,
	const std::uint32_t* arguments,
	std::uint8_t argument_count,
	std::uint8_t selector)
{
	if (source >= runtime.object_count)
	{
		return false;
	}
	return enqueue_propagating_if_matched(
		runtime,
		make_event(
			type,
			source,
			arguments,
			argument_count,
			selector,
			true));
}

bool events_emit_camera_reached(
	Runtime& runtime,
	std::uint16_t source)
{
	return events_emit_direct(
		runtime,
		EventType::camera_reached,
		source,
		nullptr,
		0);
}

bool events_emit_ship_reached(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t actor)
{
	const std::uint32_t argument = packed_object(actor);
	return events_emit_direct(
		runtime,
		EventType::ship_reached,
		source,
		&argument,
		1);
}

bool events_emit_destroyed(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t attacker)
{
	if (source >= runtime.object_count
		|| (runtime.objects[source].script_flags & 1u) != 0)
	{
		return false;
	}
	// MissionEvent_destroyed_publish (LANCER.EXE 0x0045aa60) suppresses a
	// duplicate whole-object event by setting mission-record byte +0x17
	// bit zero. WillsBlag is the only ordinary runtime clear.
	runtime.objects[source].script_flags |= 1u;
	const std::uint32_t arguments[2] = {
		packed_object(attacker),
		packed_object(source),
	};
	return events_emit_propagating(
		runtime,
		EventType::destroyed,
		source,
		arguments,
		2);
}

bool events_emit_component_destroyed(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t attacker,
	std::uint8_t selector)
{
	if (source >= runtime.object_count)
	{
		return false;
	}
	runtime.objects[source].live_component_mask &=
		~(1u << (selector & 31u));
	const std::uint32_t arguments[2] = {
		packed_object(attacker),
		packed_object(source),
	};
	return events_emit_propagating(
		runtime,
		EventType::destroyed,
		source,
		arguments,
		2,
		selector);
}

bool events_emit_shot_at(
	Runtime& runtime,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	std::uint16_t source,
	std::uint16_t attacker,
	std::uint8_t selector)
{
	if (source >= runtime.object_count)
	{
		return false;
	}
	const std::uint16_t damage =
		damage_percent(runtime, world, stats, source, selector);
	const std::uint32_t arguments[5] = {
		packed_object(attacker),
		damage,
		damage,
		packed_object(source),
		UINT32_MAX,
	};
	return events_emit_propagating(
		runtime,
		EventType::shot_at,
		source,
		arguments,
		5,
		selector);
}

bool events_emit_launched(
	Runtime& runtime,
	std::uint16_t source)
{
	const std::uint32_t argument = packed_object(source);
	return events_emit_propagating(
		runtime, EventType::launched, source, &argument, 1);
}

bool events_emit_object_scooped(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t object)
{
	const std::uint32_t argument = packed_object(object);
	return events_emit_propagating(
		runtime, EventType::object_scooped, source, &argument, 1);
}

bool events_emit_ripper_grabbed_object(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t object)
{
	const std::uint32_t argument = packed_object(object);
	return events_emit_propagating(
		runtime,
		EventType::ripper_grabbed_object,
		source,
		&argument,
		1);
}

bool events_emit_ripper_dropped_object(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t object)
{
	const std::uint32_t argument = packed_object(object);
	return events_emit_propagating(
		runtime,
		EventType::ripper_dropped_object,
		source,
		&argument,
		1);
}

bool events_emit_explosion_ship(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t object)
{
	const std::uint32_t argument = packed_object(object);
	return events_emit_propagating(
		runtime, EventType::explosion_ship, source, &argument, 1);
}

bool events_emit_jumped_in(
	Runtime& runtime,
	std::uint16_t source)
{
	// MissionEvent_JumpedIn, LANCER.EXE 0x0045b300, is a one-object
	// self event: the source mission record is also its sole argument.
	// The multiplayer first-live/root publication at 0x0041fa15 belongs
	// to the Warp In / Jump In completion callers, not this wrapper.
	const std::uint32_t argument = packed_object(source);
	return events_emit_propagating(
		runtime, EventType::jumped_in, source, &argument, 1);
}

bool events_emit_fixed_gate_jumped_in(
	Runtime& runtime,
	std::uint16_t source,
	std::uint16_t actor)
{
	const std::uint32_t argument = packed_object(actor);
	return events_emit_propagating(
		runtime,
		EventType::fixed_gate_jumped_in,
		source,
		&argument,
		1);
}

bool events_commit_player_jump_or_warp_requests(
	Runtime& runtime,
	std::uint32_t script_tick)
{
	// Player_commit_jump_or_warp_request (LANCER.EXE 0x00412b20)
	// tests the two mission-session dwords independently, clears each
	// admitted request, and publishes direct events 8 and 11 from the
	// root mission-object record at 0x0052951c.
	if (runtime.object_count == 0
		|| runtime.gameplay_state != 0
		|| (runtime.network.role != NetworkRole::offline
			&& !runtime.network.gameplay_ready)
		|| (runtime.session_state[0] == 0
			&& runtime.session_state[1] == 0))
	{
		return false;
	}
	// Player_commit_jump_or_warp_request stores the dedicated mission-script
	// counter, the same clock read by WhenPlayerLastJumped.
	runtime.last_player_jump_tick = script_tick;
	bool committed = false;
	if (runtime.session_state[0] != 0)
	{
		runtime.session_state[0] = 0;
		committed = events_emit_direct(
			runtime,
			EventType::player_ready_to_jump,
			0,
			nullptr,
			0) || committed;
	}
	if (runtime.session_state[1] != 0)
	{
		runtime.session_state[1] = 0;
		committed = events_emit_direct(
			runtime,
			EventType::player_ready_to_warp,
			0,
			nullptr,
			0) || committed;
	}
	return committed;
}

void events_service_proximity(
	Runtime& runtime,
	const game::World& world)
{
	// MissionProximity_service walks its three retained trigger-link lists
	// in this order, rather than interleaving descriptor types per object.
	// Event insertion order is observable when the queue is flushed.
	constexpr std::uint8_t passes[] = {
		static_cast<std::uint8_t>(EventType::close_proximity),
		static_cast<std::uint8_t>(EventType::proximity),
		static_cast<std::uint8_t>(EventType::ship_reached),
	};
	for (const std::uint8_t pass : passes)
	{
		for (std::uint16_t source_index = 0;
			source_index < runtime.object_count;
			++source_index)
		{
			const game::WorldObject* source =
				runtime_resolve_object(runtime, source_index, world);
			if (source == nullptr
				|| (runtime.objects[source_index].script_flags & 1u) != 0)
			{
				continue;
			}
			const std::uint16_t span_index =
				runtime.objects[source_index].reference_span;
			if (span_index == UINT16_MAX
				|| span_index >= runtime.reference_span_count)
			{
				continue;
			}
			const ReferenceSpan& span =
				runtime.reference_spans[span_index];
			for (std::uint16_t ordinal = 0;
				ordinal < span.trigger_count;
				++ordinal)
			{
				const TriggerRecord& trigger =
					runtime.triggers[span.first_trigger + ordinal];
				if (trigger.enabled == 0 || trigger.type != pass)
				{
					continue;
				}
				float radius_squared = 0.0f;
				if (pass
					== static_cast<std::uint8_t>(
						EventType::ship_reached))
				{
					if (runtime.objects[source_index].type != 997
						&& runtime.objects[source_index].type != 999)
					{
						continue;
					}
					radius_squared = 16000000.0f;
				}
				else if (pass
					== static_cast<std::uint8_t>(
						EventType::close_proximity))
				{
					if (source->radius <= 0.0f) continue;
					const float radius = source->radius * 20.0f;
					radius_squared = radius * radius;
				}
				else
				{
					if (static_cast<std::uint16_t>(
						trigger.condition[1]) == UINT16_MAX)
					{
						continue;
					}
					const float radius =
						static_cast<float>(
							trigger.condition[1]) * source->radius;
					radius_squared = radius * radius;
				}
				for (std::uint16_t neighbor_index = 0;
					neighbor_index < runtime.object_count;
					++neighbor_index)
				{
					if (neighbor_index == source_index) continue;
					const game::WorldObject* neighbor =
						runtime_resolve_object(
							runtime, neighbor_index, world);
					if (neighbor == nullptr) continue;
					const glm::vec3 delta =
						neighbor->position - source->position;
					const float distance_squared =
						glm::dot(delta, delta);
					if (distance_squared >= radius_squared) continue;
					if (pass
						== static_cast<std::uint8_t>(
							EventType::ship_reached))
					{
						const std::uint32_t argument =
							packed_object(neighbor_index);
						events_emit_direct(
							runtime,
							EventType::ship_reached,
							source_index,
							&argument,
							1);
					}
					else
					{
						const std::uint32_t arguments[2] = {
							packed_object(neighbor_index),
							source->radius > 0.0f
								? static_cast<std::uint32_t>(
									std::sqrt(distance_squared)
									/ source->radius)
								: 0,
						};
						events_emit_direct(
							runtime,
							static_cast<EventType>(pass),
							source_index,
							arguments,
							2);
					}
				}
			}
		}
	}
}

bool events_flush(
	Runtime& runtime,
	Executor& executor,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	std::uint32_t script_tick,
	std::uint32_t simulation_tick)
{
	std::uint16_t index = 0;
	while (index < runtime.event_count)
	{
		const QueuedEvent event = runtime.events[index++];
		if (event.source >= runtime.object_count)
		{
			continue;
		}
		if (!dispatch_span(
			runtime,
			executor,
			world,
			stats,
			file,
			event,
			runtime.objects[event.source].reference_span,
			event.selector,
			false,
			script_tick,
			simulation_tick))
		{
			runtime.event_count = 0;
			runtime.event_overflow_logged = false;
			return false;
		}
		if (!event.propagate_scopes)
		{
			continue;
		}
		const std::uint8_t group =
			runtime.objects[event.source].group;
		if (group != UINT8_MAX && group < runtime.group_count
			&& !dispatch_span(
				runtime,
				executor,
				world,
				stats,
				file,
				event,
				runtime.groups[group].reference_span,
				UINT8_MAX,
				true,
				script_tick,
				simulation_tick))
		{
			runtime.event_count = 0;
			runtime.event_overflow_logged = false;
			return false;
		}
		for (std::uint16_t set = 0;
			set < runtime.reference_set_count;
			++set)
		{
			if (!set_contains(
				runtime, set, event.source, event.selector))
			{
				continue;
			}
			if (!dispatch_span(
				runtime,
				executor,
				world,
				stats,
				file,
				event,
				runtime.reference_sets[set].reference_span,
					UINT8_MAX,
					true,
					script_tick,
					simulation_tick))
			{
				runtime.event_count = 0;
				runtime.event_overflow_logged = false;
				return false;
			}
		}
	}
	runtime.event_count = 0;
	runtime.event_overflow_logged = false;
	return !executor.failed;
}
}
