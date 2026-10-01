#include "ai/scripted_commands.hpp"

#include "ai/runtime.hpp"
#include "assets/gameplay_model.hpp"
#include "assets/ship_stats.hpp"
#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "game/attachments.hpp"
#include "game/model_animation.hpp"
#include "io/endian.hpp"
#include "mission/events.hpp"
#include "mission/network_runtime.hpp"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>

namespace sl_open::ai
{
namespace
{
constexpr float kCurveSamples = 32.0f;
constexpr float kCurveDurationScale = 100.0f;
constexpr float kPatrolSpread = 5000.0f;
constexpr float kPatrolInitialSpeed = 0.15f;
constexpr float kPatrolMediumSpeed = 0.2094395102f;
constexpr float kPatrolFastSpeed = 1.0471975512f;

bool sequence_sync(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& runtime,
	std::uint8_t sync_index)
{
	if (runtime.network.role == mission::NetworkRole::offline)
	{
		return true;
	}
	if (sync_index >= std::size(actor.ai_sequence_sync))
	{
		return false;
	}
	const std::uint8_t local = runtime.network.local_player;
	const std::uint8_t local_bit =
		static_cast<std::uint8_t>(1u << local);
	std::uint8_t& reached = actor.ai_sequence_sync[sync_index];
	if ((reached & local_bit) == 0)
	{
		reached |= local_bit;
		mission::network_publish_ai_sequence_sync(
			runtime.network,
			static_cast<std::uint16_t>(
				&actor - std::begin(world.objects)),
			sync_index);
	}
	for (std::uint8_t player = 0;
		player < runtime.network.player_count;
		++player)
	{
		if (runtime.network.connected[player]
			&& (reached & static_cast<std::uint8_t>(1u << player))
				== 0)
		{
			return false;
		}
	}
	reached = 0;
	return true;
}

float read_float(const std::uint8_t* source)
{
	float value = 0.0f;
	std::memcpy(&value, source, sizeof(value));
	return value;
}

glm::vec3 read_vec3(const std::uint8_t* source)
{
	return {
		read_float(source),
		read_float(source + 4),
		read_float(source + 8),
	};
}

const std::uint8_t* curve_record(
	const mission::DteFile& file,
	std::uint16_t curve)
{
	return curve < file.sections[16].count
		? mission::dte_section_data(file, 16)
			+ static_cast<std::size_t>(curve) * 0x44
		: nullptr;
}

std::uint16_t curve_endpoint(
	const std::uint8_t* curve,
	bool end)
{
	return curve == nullptr
		? UINT16_MAX
		: static_cast<std::uint16_t>(
			io::read_le32(curve + (end ? 4 : 0)));
}

glm::vec3 curve_sample(
	const std::uint8_t* curve,
	float parameter)
{
	// MissionCurve_sample (LANCER.EXE 0x00457050) evaluates the Hermite
	// continuously. MissionCurve_sample_fast quantizes to the precomputed
	// 1/32 table, but retail uses that variant only for length estimation.
	const float t = parameter;
	const glm::vec3 p0 = read_vec3(curve + 8);
	const glm::vec3 p1 = read_vec3(curve + 0x14);
	const glm::vec3 tangent0 = read_vec3(curve + 0x28) * 10.0f;
	const glm::vec3 tangent1 = read_vec3(curve + 0x34) * -10.0f;
	const float t2 = t * t;
	const float t3 = t2 * t;
	const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
	const float h01 = -2.0f * t3 + 3.0f * t2;
	const float h10 = t3 - 2.0f * t2 + t;
	const float h11 = t3 - t2;
	return h00 * p0 + h01 * p1
		+ h10 * tangent0 + h11 * tangent1;
}

float curve_length(const std::uint8_t* curve)
{
	if (curve == nullptr)
	{
		return 0.0f;
	}
	float length = 0.0f;
	glm::vec3 previous = curve_sample(curve, 0.0f);
	for (std::uint32_t index = 1; index <= 32; ++index)
	{
		const glm::vec3 point = curve_sample(
			curve, static_cast<float>(index) / kCurveSamples);
		length += glm::length(point - previous);
		previous = point;
	}
	return length;
}

std::uint16_t connected_curve(
	const mission::DteFile& file,
	std::uint16_t current,
	std::uint16_t endpoint)
{
	for (std::uint16_t index = 0;
		index < file.sections[16].count;
		++index)
	{
		if (index == current)
		{
			continue;
		}
		const std::uint8_t* candidate = curve_record(file, index);
		if (curve_endpoint(candidate, false) == endpoint
			|| curve_endpoint(candidate, true) == endpoint)
		{
			return index;
		}
	}
	return UINT16_MAX;
}

float curve_chain_length(
	const mission::DteFile& file,
	std::uint16_t first)
{
	float length = 0.0f;
	std::uint16_t current = first;
	for (std::uint16_t visited = 0;
		current != UINT16_MAX
			&& visited <= file.sections[16].count;
		++visited)
	{
		const std::uint8_t* curve = curve_record(file, current);
		if (curve == nullptr)
		{
			break;
		}
		length += curve_length(curve);
		const std::uint16_t endpoint = curve_endpoint(curve, true);
		if (endpoint == UINT16_MAX)
		{
			break;
		}
		current = connected_curve(file, current, endpoint);
	}
	return length;
}

std::uint16_t terminal_curve(
	const mission::DteFile& file,
	std::uint16_t first)
{
	std::uint16_t current = first;
	for (std::uint16_t visited = 0;
		current != UINT16_MAX
			&& visited <= file.sections[16].count;
		++visited)
	{
		const std::uint8_t* curve = curve_record(file, current);
		if (curve == nullptr
			|| curve_endpoint(curve, true) == UINT16_MAX)
		{
			break;
		}
		const std::uint16_t next = connected_curve(
			file, current, curve_endpoint(curve, true));
		if (next == UINT16_MAX)
		{
			break;
		}
		current = next;
	}
	return current;
}

std::uint16_t previous_curve(
	const mission::DteFile& file,
	std::uint16_t first,
	std::uint16_t current)
{
	if (current == first)
	{
		return UINT16_MAX;
	}
	std::uint16_t candidate = first;
	for (std::uint16_t visited = 0;
		candidate != UINT16_MAX
			&& visited <= file.sections[16].count;
		++visited)
	{
		const std::uint8_t* curve = curve_record(file, candidate);
		if (curve == nullptr)
		{
			break;
		}
		const std::uint16_t next = connected_curve(
			file, candidate, curve_endpoint(curve, true));
		if (next == current)
		{
			return candidate;
		}
		candidate = next;
	}
	return UINT16_MAX;
}

std::uint16_t first_reference_object(
	const mission::Runtime& mission,
	std::uint32_t kind,
	std::uint16_t reference)
{
	if (reference == UINT16_MAX)
	{
		return UINT16_MAX;
	}
	if (kind == 1)
	{
		return reference < mission.object_count
			? reference
			: UINT16_MAX;
	}
	mission::ReferenceKind reference_kind;
	if (kind == 2)
	{
		reference_kind = mission::ReferenceKind::group;
	}
	else if (kind == 3)
	{
		reference_kind = mission::ReferenceKind::set;
	}
	else
	{
		return UINT16_MAX;
	}
	std::uint16_t object = UINT16_MAX;
	return mission::runtime_expand_reference(
		mission, reference_kind, reference, &object, 1) == 1
		? object
		: UINT16_MAX;
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
		// AI_Land_begin dereferences command +0x04 through the live
		// GameObject pointer table. World-object targets are the stable
		// representation of that retail command field when a command
		// originates from live gameplay rather than the DTE.
		if (command.target >= std::size(world.objects))
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
	game::WorldObject* selected = nullptr;
	float nearest = 0.0f;
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
		if (selected == nullptr || distance < nearest)
		{
			selected = candidate;
			nearest = distance;
		}
	}
	return selected;
}

glm::mat3 look_at(
	const glm::vec3& from,
	const glm::vec3& to)
{
	glm::vec3 delta = to - from;
	if (glm::dot(delta, delta) <= 0.000001f)
	{
		return glm::mat3{1.0f};
	}
	glm::mat3 result{1.0f};
	result = math::postrotate(
		result,
		std::atan2(delta.x, delta.z),
		{0.0f, 1.0f, 0.0f});
	delta = glm::transpose(result) * delta;
	return math::postrotate(
		result,
		-std::atan2(delta.y, delta.z),
		{1.0f, 0.0f, 0.0f});
}

game::FlightDemand steer_toward(
	const game::WorldObject& actor,
	const assets::ShipStatsTable& stats,
	const glm::vec3& target,
	float throttle,
	std::uint32_t frame_delta,
	float maximum_control = 1.0f,
	float retention = 0.0f)
{
	game::FlightDemand demand;
	demand.throttle = throttle;
	const glm::vec3 delta = target - actor.position;
	const float length_squared = glm::dot(delta, delta);
	if (length_squared <= 0.0001f
		|| actor.type >= assets::kShipStatsCount)
	{
		return demand;
	}
	const assets::FlightStats& flight = stats.records[actor.type].flight;
	const glm::vec3 local =
		glm::transpose(actor.orientation)
		* (delta / std::sqrt(length_squared));
	float pitch = 0.0f;
	float yaw = 0.0f;
	float roll = 0.0f;
	if (flight.simplified_steering)
	{
		if (local.z >= 0.0f)
		{
			pitch = -std::atan2(local.y, local.z);
			yaw = std::atan2(local.x, local.z);
		}
		else
		{
			yaw = local.x < 0.0f ? -1.0f : 1.0f;
		}
	}
	else if (std::abs(local.z) >= 0.95f)
	{
		yaw = std::atan2(local.x, local.z);
	}
	else
	{
		roll = local.y < 0.0f
			? -std::atan2(-local.x, -local.y)
			: -std::atan2(local.x, local.y);
		if (std::abs(roll) < 0.8f)
		{
			pitch = -std::atan2(local.y, local.z);
		}
	}
	// AI_steer_toward (LANCER.EXE 0x00401380) dampens every small
	// requested angle and the shared control limit after a slow rendered
	// frame. The original reads the complete gameplay-frame tick delta.
	if (frame_delta > 10
		&& (std::abs(pitch) < 0.3926990926f
			|| std::abs(yaw) < 0.3926990926f
			|| std::abs(roll) < 0.3926990926f))
	{
		pitch =
			std::abs(pitch) < 0.3926990926f ? pitch * 0.5f : pitch;
		yaw =
			std::abs(yaw) < 0.3926990926f ? yaw * 0.5f : yaw;
		roll =
			std::abs(roll) < 0.3926990926f ? roll * 0.5f : roll;
		maximum_control *= 0.5f;
	}
	demand.pitch = std::clamp(
		(pitch - (1.0f - retention) * 6.0f * actor.angular_x)
			* 11.4591551f,
		-maximum_control,
		maximum_control);
	demand.yaw = std::clamp(
		(yaw - (1.0f - retention) * 6.0f * actor.angular_y)
			* 11.4591551f,
		-maximum_control,
		maximum_control);
	demand.roll = std::clamp(
		(roll - (1.0f - retention) * 6.0f * actor.angular_z)
			* 11.4591551f,
		-maximum_control,
		maximum_control);
	return demand;
}

float next_curve_event(
	const mission::DteFile& file,
	std::uint16_t curve,
	float after,
	std::uint16_t& object);

void select_curve_segment(
	game::WorldObject& actor,
	const Command& command,
	const mission::DteFile& file,
	std::uint16_t curve,
	std::uint32_t tick)
{
	CurveWork& work = actor.ai.work.curve;
	work.current_curve = curve;
	work.segment_start_tick = tick;
	const float length = curve_length(curve_record(file, curve));
	const float duration =
		length / work.chain_length
			* static_cast<float>(command.state[1])
			* kCurveDurationScale;
	work.segment_duration = static_cast<std::uint16_t>(
		static_cast<std::uint32_t>(duration));
	std::uint16_t ignored = UINT16_MAX;
	work.next_event_parameter = next_curve_event(
		file,
		curve,
		0.0f,
		ignored);
}

float next_curve_event(
	const mission::DteFile& file,
	std::uint16_t curve,
	float after,
	std::uint16_t& object)
{
	float next = 0.0f;
	object = UINT16_MAX;
	const std::uint8_t* records = mission::dte_section_data(file, 3);
	for (std::uint16_t index = 0;
		index < file.sections[3].count;
		++index)
	{
		const std::uint8_t* record =
			records + static_cast<std::size_t>(index) * 0x4c;
		if (io::read_le16(record + 0x18) != 0x03e3
			|| io::read_le16(record + 0x40) != curve)
		{
			continue;
		}
		const float parameter = read_float(record + 0x44);
		if (parameter <= after)
		{
			if (parameter == after)
			{
				// MissionCurve_next_event retains the last marker at an
				// exactly matching parameter when duplicates exist.
				object = index;
			}
		}
		else if (next == 0.0f || parameter < next)
		{
			next = parameter;
		}
	}
	return next;
}

glm::vec3 anchored_curve_point(
	const game::WorldObject& actor,
	const mission::DteFile& file,
	float parameter)
{
	const CurveWork& work = actor.ai.work.curve;
	glm::vec3 point = curve_sample(
		curve_record(file, work.current_curve), parameter);
	if (work.moving_anchor_active)
	{
		point += work.moving_anchor;
	}
	return point;
}

void prepare_provider_motion(
	game::WorldObject& actor,
	const assets::ShipStatsTable& stats,
	const glm::vec3& target,
	const glm::vec3& provider_up,
	float maximum_normalized_speed,
	bool reverse)
{
	if (actor.type >= assets::kShipStatsCount)
	{
		return;
	}
	const assets::FlightStats& flight = stats.records[actor.type].flight;
	const glm::mat3 wanted = look_at(actor.previous_position, target);
	glm::vec3 error = math::rotation_to_euler(
		glm::transpose(actor.previous_orientation) * wanted);
	if ((provider_up.x != 0.0f
			|| provider_up.y != 0.0f
			|| provider_up.z != 0.0f)
		&& std::abs(error.x) < 0.01f
		&& std::abs(error.y) < 0.01f)
	{
		const glm::vec3 local_up =
			glm::transpose(actor.previous_orientation) * provider_up;
		error.z = -std::atan2(local_up.x, local_up.y);
	}
	if (reverse)
	{
		error = -error;
	}
	const float pitch = std::clamp(
		(error.x - 6.0f * actor.angular_x) * 11.4591551f,
		-1.0f,
		1.0f);
	const float yaw = std::clamp(
		(error.y - 6.0f * actor.angular_y) * 11.4591551f,
		-1.0f,
		1.0f);
	const float roll = std::clamp(
		(error.z - 6.0f * actor.angular_z) * 11.4591551f,
		-1.0f,
		1.0f);
	actor.control_demand.pitch = pitch;
	actor.control_demand.yaw = yaw;
	actor.control_demand.roll = roll;
	actor.angular_x =
		actor.angular_x * flight.pitch_retention
		+ (1.0f - flight.pitch_retention) * flight.pitch_rate * pitch;
	actor.angular_y =
		actor.angular_y * flight.yaw_retention
		+ (1.0f - flight.yaw_retention) * flight.yaw_rate * yaw;
	actor.angular_z =
		actor.angular_z * flight.roll_retention
		+ (1.0f - flight.roll_retention) * flight.roll_rate * roll;
	actor.inertial_angular_step = math::rotation_from_euler({
		actor.angular_x, actor.angular_y, actor.angular_z});
	actor.linear_velocity = target - actor.previous_position;
	const float distance = glm::length(actor.linear_velocity);
	const float normalized_speed = distance / flight.max_speed;
	if (normalized_speed > maximum_normalized_speed)
	{
		actor.linear_velocity *=
			flight.max_speed * maximum_normalized_speed / distance;
		actor.throttle = maximum_normalized_speed;
	}
	else
	{
		actor.throttle = normalized_speed;
	}
	actor.control_demand.throttle = actor.throttle;
	actor.exhaust_scalar = actor.throttle;
}

bool curve_begin(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& runtime,
	const mission::DteFile& file,
	std::uint32_t tick)
{
	const std::uint16_t first =
		static_cast<std::uint16_t>(command.state[0]);
	if (curve_record(file, first) == nullptr)
	{
		diagnostics::mission_log(
			"ai curve invalid actor=%u command=%d curve=%u",
			static_cast<unsigned>(actor.mission_index),
			static_cast<int>(command.id),
			static_cast<unsigned>(first));
		command_pop(world, actor);
		return true;
	}
	CurveWork& work = actor.ai.work.curve;
	work.first_curve = first;
	work.chain_length = curve_chain_length(file, first);
	work.moving_object = first_reference_object(
		runtime,
		command.state[3],
		static_cast<std::uint16_t>(command.state[2]));
	if (const game::WorldObject* moving = mission::runtime_resolve_object(
		runtime, work.moving_object, world))
	{
		if (work.moving_object < file.sections[3].count)
		{
			const std::uint8_t* record =
				mission::dte_section_data(file, 3)
				+ static_cast<std::size_t>(work.moving_object)
					* 0x4c;
			// MissionCurve_apply_moving_origin(flag=1), 0x004574a0:
			// translate the authored curve by the object's initial runtime
			// displacement, not by its absolute runtime position.
			work.moving_anchor =
				moving->position - read_vec3(record + 0x1c);
			work.moving_anchor_active = true;
		}
	}
	const std::uint16_t selected =
		command.id == 119 ? terminal_curve(file, first) : first;
	select_curve_segment(actor, command, file, selected, tick);
	return true;
}

bool curve_update(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& runtime,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	CurveWork& curve = actor.ai.work.curve;
	Work& work = actor.ai.work;
	if (curve.current_curve == UINT16_MAX)
	{
		command_pop(world, actor);
		return true;
	}
	if (work.stage == 0)
	{
		if (command.id == 119)
		{
			// AI_ShipFollowCurveBackwards_update stage zero
			// (LANCER.EXE 0x0040375d) explicitly restores the ordinary
			// forward flight callback before making its approach. Command
			// 17 deliberately leaves the incoming callback alone.
			actor.flight_callback_mode =
				game::FlightCallbackMode::standard_forward;
		}
		const float direction =
			command.id == 119 ? -1.0f : 1.0f;
		const float start_parameter =
			command.id == 119 ? 1.0f : 0.0f;
		const float next_parameter =
			start_parameter
			+ direction
				* (4.0f
					/ static_cast<float>(curve.segment_duration));
		const glm::vec3 start = anchored_curve_point(
			actor, file, start_parameter);
		const glm::vec3 next = anchored_curve_point(
			actor, file, next_parameter);
		const float effective_maximum =
			game::world_effective_max_speed(
				actor, stats, world.camera_mode);
		const float minimum_throttle =
			glm::distance(start, next) / effective_maximum;
		if (runtime_approach_point(
			actor,
			start,
			look_at(start, next),
			minimum_throttle,
			world,
			stats,
			runtime.frame_delta_ticks,
			demand))
		{
			++work.stage;
		}
		curve.segment_start_tick = tick;
		return true;
	}
	if (work.stage == 1)
	{
		// AI_ShipFollowCurveBackwards_update 0x00403825 uses retail
		// sequence barrier zero before transferring flight ownership.
		if (sequence_sync(actor, world, runtime, 0))
		{
			++work.stage;
		}
		return true;
	}
	if (work.stage == 3)
	{
		// Its terminal barrier is index one (0x00403887); the command
		// remains active until every connected player reaches it.
		if (sequence_sync(actor, world, runtime, 1))
		{
			command_pop(world, actor);
		}
		return true;
	}

	if (command.id == 119)
	{
		// Stage two installs the forward provider even though the provider
		// samples the linked curve in reverse (0x00403866).
		actor.flight_callback_mode =
			game::FlightCallbackMode::provider_forward;
	}
	else if (
		actor.flight_callback_mode
			== game::FlightCallbackMode::standard_reverse
		|| actor.flight_callback_mode
			== game::FlightCallbackMode::provider_reverse)
	{
		// ShipFollowCurve preserves reverse-flight ownership across the
		// provider handoff (0x004034cd).
		actor.flight_callback_mode =
			game::FlightCallbackMode::provider_reverse;
	}
	else
	{
		actor.flight_callback_mode =
			game::FlightCallbackMode::provider_forward;
	}

	// Stage two only installs the indirect provider. The provider samples
	// and advances the curve when the common 25 Hz flight service invokes
	// it; the rendered-frame AI owner does not move the object.
	applies_flight = true;
	return true;
}

glm::vec3 curve_provider_target(
	game::WorldObject& actor,
	Command& command,
	mission::Runtime& runtime,
	const mission::DteFile& file,
	std::uint32_t tick)
{
	CurveWork& curve = actor.ai.work.curve;
	const float elapsed = static_cast<float>(
		tick - curve.segment_start_tick);
	const float t = curve.segment_duration == 0
		? std::numeric_limits<float>::infinity()
		: elapsed / static_cast<float>(curve.segment_duration);
	const bool backwards = command.id == 119;
	const float parameter = backwards ? 1.0f - t : t;
	// MissionCurve_provider samples the current segment before it changes
	// the linked-segment state. The sampled point therefore remains the
	// provider output on the terminal service too.
	const glm::vec3 target = anchored_curve_point(
		actor, file, parameter);
	if (!backwards)
	{
		std::uint16_t marker = UINT16_MAX;
		if (curve.next_event_parameter > 0.0f
			&& curve.next_event_parameter < parameter)
		{
			const float reached = curve.next_event_parameter;
			curve.next_event_parameter = next_curve_event(
				file,
				curve.current_curve,
				reached,
				marker);
			if (marker != UINT16_MAX)
			{
				mission::events_emit_ship_reached(
					runtime,
					marker,
					actor.mission_index);
			}
		}
	}
	if (t >= 1.0f)
	{
		std::uint16_t next = UINT16_MAX;
		if (backwards)
		{
			next = previous_curve(
				file, curve.first_curve, curve.current_curve);
		}
		else
		{
			const std::uint8_t* current =
				curve_record(file, curve.current_curve);
			const std::uint16_t endpoint =
				curve_endpoint(current, true);
			if (endpoint != UINT16_MAX)
			{
				next = connected_curve(
					file, curve.current_curve, endpoint);
			}
		}
		if (next == UINT16_MAX)
		{
			++actor.ai.work.stage;
		}
		else
		{
			select_curve_segment(actor, command, file, next, tick);
		}
	}
	return target;
}

std::uint16_t route_group_begin(
	const mission::Runtime& runtime,
	std::uint16_t pair)
{
	if (pair >= runtime.subtype997_pair_count)
	{
		return UINT16_MAX;
	}
	const std::uint16_t group = runtime.subtype997_pairs[pair].group;
	while (pair != 0
		&& runtime.subtype997_pairs[pair - 1].group == group)
	{
		--pair;
	}
	return pair;
}

std::uint16_t route_group_end(
	const mission::Runtime& runtime,
	std::uint16_t pair)
{
	if (pair >= runtime.subtype997_pair_count)
	{
		return UINT16_MAX;
	}
	const std::uint16_t group = runtime.subtype997_pairs[pair].group;
	while (pair + 1 < runtime.subtype997_pair_count
		&& runtime.subtype997_pairs[pair + 1].group == group)
	{
		++pair;
	}
	return pair;
}

std::uint16_t route_next(
	const mission::Runtime& runtime,
	std::uint16_t pair)
{
	const std::uint16_t begin = route_group_begin(runtime, pair);
	const std::uint16_t end = route_group_end(runtime, pair);
	if (begin == UINT16_MAX || end == UINT16_MAX)
	{
		return UINT16_MAX;
	}
	return pair == end ? begin : static_cast<std::uint16_t>(pair + 1);
}

const std::uint8_t* formation_record(
	const mission::DteFile& file,
	std::uint16_t index)
{
	return index < file.sections[15].count
		? mission::dte_section_data(file, 15)
			+ static_cast<std::size_t>(index) * 0x10
		: nullptr;
}

std::uint16_t formation_route_start(
	const mission::DteFile& file,
	std::uint16_t formation)
{
	const std::uint8_t* record = formation_record(file, formation);
	if (record == nullptr)
	{
		return UINT16_MAX;
	}
	const std::uint16_t route =
		static_cast<std::uint16_t>(io::read_le16(record));
	if (route >= file.sections[14].count)
	{
		return UINT16_MAX;
	}
	const std::uint8_t* table =
		mission::dte_section_data(file, 14)
			+ static_cast<std::size_t>(route) * 8;
	const std::uint16_t start =
		static_cast<std::uint16_t>(io::read_le16(table + 4));
	return start < file.sections[15].count ? start : UINT16_MAX;
}

std::uint16_t nearest_formation_route_record(
	const mission::DteFile& file,
	std::uint16_t formation)
{
	const std::uint16_t start =
		formation_route_start(file, formation);
	if (start == UINT16_MAX)
	{
		return UINT16_MAX;
	}
	std::uint16_t selected = UINT16_MAX;
	float selected_distance = 0.0f;
	for (std::uint16_t index = start;
		index < file.sections[15].count;
		++index)
	{
		const std::uint8_t* record =
			formation_record(file, index);
		const float distance =
			glm::length(read_vec3(record + 4));
		if (selected == UINT16_MAX || distance < selected_distance)
		{
			selected = index;
			selected_distance = distance;
		}
	}
	return selected;
}

float formation_route_extent(
	const mission::DteFile& file,
	std::uint16_t formation)
{
	const std::uint16_t start =
		formation_route_start(file, formation);
	if (start == UINT16_MAX)
	{
		return 0.0f;
	}
	glm::vec3 minimum = read_vec3(
		formation_record(file, start) + 4);
	glm::vec3 maximum = minimum;
	for (std::uint16_t index = start + 1;
		index < file.sections[15].count;
		++index)
	{
		const glm::vec3 point =
			read_vec3(formation_record(file, index) + 4);
		minimum = glm::min(minimum, point);
		maximum = glm::max(maximum, point);
	}
	return glm::distance(minimum, maximum);
}

bool formation_member_exempt(
	const mission::DteFile& file,
	std::uint16_t mission_object)
{
	if (mission_object >= file.sections[3].count)
	{
		return true;
	}
	const std::uint8_t* record =
		mission::dte_section_data(file, 3)
			+ static_cast<std::size_t>(mission_object) * 0x4c;
	return (record[0x17] & 1u) != 0;
}

std::uint16_t formation_peer_object(
	const game::WorldObject& actor,
	const mission::DteFile& file,
	std::uint16_t formation)
{
	const std::uint16_t nearest =
		nearest_formation_route_record(file, formation);
	if (nearest == UINT16_MAX)
	{
		return UINT16_MAX;
	}
	for (std::uint16_t index = 0;
		index < file.sections[3].count;
		++index)
	{
		const std::uint8_t* object =
			mission::dte_section_data(file, 3)
				+ static_cast<std::size_t>(index) * 0x4c;
		if (object[0x14] == actor.group
			&& static_cast<std::uint16_t>(
				io::read_le16(object + 0x34)) == nearest)
		{
			return index;
		}
	}
	return UINT16_MAX;
}

std::int16_t object_formation(
	const mission::DteFile& file,
	std::uint16_t mission_object)
{
	if (mission_object >= file.sections[3].count)
	{
		return -1;
	}
	const std::uint8_t* record =
		mission::dte_section_data(file, 3)
			+ static_cast<std::size_t>(mission_object) * 0x4c;
	return static_cast<std::int16_t>(io::read_le16(record + 0x34));
}

glm::vec3 formation_group_center(
	const mission::Runtime& runtime,
	std::uint8_t group)
{
	if (group >= runtime.group_count)
	{
		return {};
	}
	const mission::GroupRecord& record = runtime.groups[group];
	if (record.member_count == 0)
	{
		return {};
	}
	glm::vec3 minimum =
		runtime.objects[
			runtime.group_members[record.first_member]].script_position;
	glm::vec3 maximum = minimum;
	for (std::uint16_t ordinal = 1;
		ordinal < record.member_count;
		++ordinal)
	{
		const glm::vec3 position =
			runtime.objects[
				runtime.group_members[
					record.first_member + ordinal]].script_position;
		minimum = glm::min(minimum, position);
		maximum = glm::max(maximum, position);
	}
	return minimum + (maximum - minimum) * 0.5f;
}

glm::vec3 formation_regroup_curve_sample(
	const FormationRegroupWork& work,
	float parameter)
{
	const float t = parameter;
	const float t2 = t * t;
	const float t3 = t2 * t;
	const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
	const float h01 = -2.0f * t3 + 3.0f * t2;
	const float h10 = t3 - 2.0f * t2 + t;
	return h00 * work.curve_start
		+ h01 * work.rear_target
		+ h10 * work.curve_tangent;
}

bool formation_regroup_begin(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& runtime,
	const mission::DteFile& file)
{
	FormationRegroupWork& work = actor.ai.work.formation_regroup;
	work = {};
	const std::int16_t formation =
		object_formation(file, actor.mission_index);
	const std::uint8_t* point =
		formation < 0
			? nullptr
			: formation_record(
				file, static_cast<std::uint16_t>(formation));
	if (point == nullptr
		|| actor.group >= runtime.group_count)
	{
		command_pop(world, actor);
		return true;
	}

	work.rear_target =
		formation_group_center(runtime, actor.group)
		+ read_vec3(point + 4);
	work.rear_target.z -= 10000.0f;
	work.forward_target = work.rear_target;
	work.forward_target.z += 10000.0f;
	work.coordinator = formation_peer_object(
		actor,
		file,
		static_cast<std::uint16_t>(formation));
	work.stage = 0;
	work.barrier_updates = 0;
	work.ready = false;
	actor.control_demand.throttle = 0.0f;
	return true;
}

void formation_regroup_coordinate(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& runtime,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick)
{
	if (actor.group >= runtime.group_count)
	{
		return;
	}
	const mission::GroupRecord& group = runtime.groups[actor.group];
	bool all_ready = true;
	for (std::uint16_t ordinal = 0;
		ordinal < group.member_count;
		++ordinal)
	{
		const std::uint16_t mission_object =
			runtime.group_members[group.first_member + ordinal];
		if (formation_member_exempt(file, mission_object))
		{
			continue;
		}
		const game::WorldObject* member =
			mission::runtime_resolve_object(
				runtime, mission_object, world);
		if (member == nullptr
			|| member->ai.command_count == 0
			|| member->ai.commands[0].id != 14
			|| !member->ai.work.formation_regroup.ready)
		{
			all_ready = false;
		}
	}

	for (std::uint16_t ordinal = 0;
		ordinal < group.member_count;
		++ordinal)
	{
		const std::uint16_t mission_object =
			runtime.group_members[group.first_member + ordinal];
		game::WorldObject* member =
			game::world_resolve(
				world, runtime.objects[mission_object].live);
		if (member == nullptr
			|| member->ai.command_count == 0
			|| member->ai.commands[0].id != 14)
		{
			continue;
		}
		FormationRegroupWork& work =
			member->ai.work.formation_regroup;
		if (!all_ready)
		{
			work.barrier_updates = 0;
			continue;
		}
		++work.barrier_updates;
		if (work.barrier_updates < 15)
		{
			continue;
		}
		work.ready = false;
		work.barrier_updates = 0;
		if (work.stage == 0)
		{
			const float distance =
				glm::distance(member->position, work.rear_target);
			if (distance >= 20000.0f && distance <= 400000.0f)
			{
				work.curve_start = member->position;
				const glm::vec3 half_delta =
					(work.rear_target - member->position) * 0.5f;
				work.curve_tangent =
					math::postrotate(
						glm::mat3{1.0f},
						glm::half_pi<float>(),
						{0.0f, 1.0f, 0.0f})
					* half_delta * 10.0f;
				work.curve_start_tick = tick;
				work.direct_approach = false;
			}
			else
			{
				work.direct_approach = true;
				member->control_demand.throttle =
					60.0f
					/ std::max(
						game::world_effective_max_speed(
							*member, stats, world.camera_mode),
						1.0f);
			}
		}
		++work.stage;
	}
}

bool formation_regroup_update(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& runtime,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	FormationRegroupWork& work = actor.ai.work.formation_regroup;
	if (actor.mission_index == work.coordinator)
	{
		formation_regroup_coordinate(
			actor, world, runtime, file, stats, tick);
	}

	const auto aligned_with = [&actor](const glm::vec3& target)
	{
		const glm::vec3 delta = target - actor.position;
		const float length_squared = glm::dot(delta, delta);
		return length_squared <= 0.0001f
			|| glm::dot(
				actor.orientation[2],
				delta / std::sqrt(length_squared)) >= 0.9f;
	};
	const auto arrived_at = [&actor](const glm::vec3& target)
	{
		return glm::distance(actor.position, target)
			< actor.radius * 2.0f;
	};

	switch (work.stage)
	{
	case 0:
		demand = steer_toward(
			actor,
			stats,
			work.rear_target,
			actor.control_demand.throttle,
			runtime.frame_delta_ticks,
			0.75f,
			0.1f);
		work.ready = aligned_with(work.rear_target);
		return true;
	case 1:
		if (work.ready)
		{
			return true;
		}
		if (work.direct_approach)
		{
			const float maximum = std::max(
				game::world_effective_max_speed(
					actor, stats, world.camera_mode),
				1.0f);
			demand = steer_toward(
				actor,
				stats,
				work.rear_target,
				60.0f / maximum,
				runtime.frame_delta_ticks,
				0.75f,
				0.0f);
		}
		else
		{
			// AI_curve_motion_update increments the 3000-ms path clock
			// before sampling. It publishes the sampled position into both
			// object snapshots, constructs a zero-roll basis from the
			// t+0.05 look-ahead, and publishes that basis into both
			// orientation snapshots (0x00404c76..0x00404e6c).
			const float parameter =
				static_cast<float>(
					tick - work.curve_start_tick + 1u)
				/ 300.0f;
			const glm::vec3 position =
				formation_regroup_curve_sample(work, parameter);
			const glm::vec3 look_ahead =
				formation_regroup_curve_sample(
					work, parameter + 0.05f);
			const glm::mat3 orientation =
				look_at(position, look_ahead);
			actor.previous_position = position;
			actor.position = position;
			actor.previous_orientation = orientation;
			actor.orientation = orientation;
			actor.control_demand.throttle = 0.0f;
			demand.throttle = 0.0f;
			applies_flight = false;
			if (parameter >= 1.0f)
			{
				// The generated curve has no linked successor. The shared
				// motion provider therefore executes its ordinary terminal
				// path: stop, clear rotational controls, and pop.
				actor.control_demand.roll = 0.0f;
				actor.control_demand.pitch = 0.0f;
				actor.control_demand.yaw = 0.0f;
				command_pop(world, actor);
				return true;
			}
		}
		if (arrived_at(work.rear_target))
		{
			demand.throttle = 0.0f;
			work.ready = true;
		}
		return true;
	case 2:
		demand = steer_toward(
			actor,
			stats,
			work.forward_target,
			actor.control_demand.throttle,
			runtime.frame_delta_ticks,
			0.75f,
			0.1f);
		work.ready = aligned_with(work.forward_target);
		return true;
	case 3:
		// AI_FormationRegroup_finish clears the three rotational control
		// inputs at GameObject+0x5bc/+0x5c0/+0x5c4. It deliberately leaves
		// throttle and the retained angular velocities untouched.
		demand.roll = 0.0f;
		demand.pitch = 0.0f;
		demand.yaw = 0.0f;
		command_pop(world, actor);
		return true;
	default:
		command_pop(world, actor);
		applies_flight = false;
		return true;
	}
}

void patrol_initialize_leg(
	game::WorldObject& actor,
	const game::World& world,
	const mission::Runtime& runtime,
	const mission::DteFile& file,
	std::uint16_t route_pair)
{
	PatrolWork& work = actor.ai.work.patrol;
	work.route_pair = route_pair;
	work.route_object =
		route_pair < runtime.subtype997_pair_count
			? runtime.subtype997_pairs[route_pair].object
			: UINT16_MAX;
	const bool formed = work.formation_id >= 0
		&& formation_record(
			file,
			static_cast<std::uint16_t>(work.formation_id))
			!= nullptr;
	work.route_offset = formed
		? read_vec3(
			formation_record(
				file,
				static_cast<std::uint16_t>(work.formation_id))
				+ 4)
		: glm::vec3{0.0f};
	work.peer_object = formed
		? formation_peer_object(
			actor,
			file,
			static_cast<std::uint16_t>(work.formation_id))
		: UINT16_MAX;
	work.coordinator_lookahead =
		formed && actor.mission_index == work.peer_object
			? actor.orientation[2] * (actor.radius * 8.0f)
			: glm::vec3{0.0f};
	if (work.route_object < runtime.object_count)
	{
		const mission::ObjectRecord& route =
			runtime.objects[work.route_object];
		const game::WorldObject* live =
			mission::runtime_resolve_object(
				runtime, work.route_object, world);
		work.arrival_target =
			(live != nullptr
				? live->position
				: route.authored_position)
			+ (formed ? glm::vec3{0.0f} : work.route_offset);
		if (!formed && live != nullptr
			&& actor.group < runtime.group_count)
		{
			const mission::GroupRecord& group =
				runtime.groups[actor.group];
			float maximum_radius = 0.0f;
			std::uint16_t ordinal = 0;
			for (std::uint16_t member = 0;
				member < group.member_count;
				++member)
			{
				const std::uint16_t mission_object =
					runtime.group_members[
						group.first_member + member];
				const game::WorldObject* object =
					mission::runtime_resolve_object(
						runtime, mission_object, world);
				if (object != nullptr)
				{
					maximum_radius =
						std::max(maximum_radius, object->radius);
				}
				if (mission_object == actor.mission_index)
				{
					ordinal = member;
				}
			}
			const std::uint16_t next_pair =
				route_next(runtime, route_pair);
			const game::WorldObject* next =
				next_pair < runtime.subtype997_pair_count
					? mission::runtime_resolve_object(
						runtime,
						runtime.subtype997_pairs[next_pair].object,
						world)
					: nullptr;
			if (next != nullptr && group.member_count != 0)
			{
				glm::vec3 span = live->position - next->position;
				const float length = glm::length(span);
				if (length > 0.0001f)
				{
					span *=
						(maximum_radius * 6.0f
							* static_cast<float>(
								group.member_count))
						/ length;
					const float parameter =
						static_cast<float>(ordinal)
						/ static_cast<float>(group.member_count);
					work.route_offset =
						glm::mix(
							span * -0.5f,
							span * 0.5f,
							parameter);
					work.arrival_target =
						live->position + work.route_offset;
				}
			}
		}
	}
	else
	{
		work.arrival_target = actor.position;
	}
	work.active_target = work.arrival_target;
	work.previous_target = actor.position;
	work.member_target = work.active_target;
	work.initial_distance = std::max(
		0.0f,
		glm::distance(actor.position, work.active_target)
			- actor.radius * 2.0f);
	work.speed_parameter = kPatrolInitialSpeed;
	work.route_extent = formed
		? formation_route_extent(
			file,
			static_cast<std::uint16_t>(work.formation_id))
		: 0.0f;
	work.mode = formed ? 1 : 2;
	work.ready = false;
	work.transition_pending = false;
	work.leg_complete = false;
}

bool patrol_begin(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	const mission::Runtime& runtime,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats)
{
	const std::uint16_t route_pair =
		command.target;
	if (route_pair >= runtime.subtype997_pair_count)
	{
		diagnostics::mission_log(
			"ai patrol invalid actor=%u route-pair=%u",
			static_cast<unsigned>(actor.mission_index),
			static_cast<unsigned>(route_pair));
		command_pop(world, actor);
		return true;
	}
	PatrolWork& work = actor.ai.work.patrol;
	work.formation_id = -1;
	if (actor.mission_index < file.sections[3].count)
	{
		const std::uint8_t* record =
			mission::dte_section_data(file, 3)
				+ static_cast<std::size_t>(actor.mission_index) * 0x4c;
		work.formation_id = static_cast<std::int16_t>(
			io::read_le16(record + 0x34));
	}
	if (work.formation_id != -1)
	{
		// Formed routes require the section-14/15 formation tables. Retail
		// indexes section 14 by this signed slot and section 15 through its
		// +4 start record; do not synthesize offsets when either table is
		// absent or the slot is outside the retained DTE data.
		if (static_cast<std::uint16_t>(work.formation_id)
				>= file.sections[15].count
			|| formation_route_start(
				file,
				static_cast<std::uint16_t>(work.formation_id))
				== UINT16_MAX)
		{
			diagnostics::mission_log(
				"ai patrol formation unavailable actor=%u slot=%d",
				static_cast<unsigned>(actor.mission_index),
				static_cast<int>(work.formation_id));
			command_pop(world, actor);
			return true;
		}
	}
	patrol_initialize_leg(actor, world, runtime, file, route_pair);
	actor.control_demand.throttle =
		work.mode == 1
			? 0.0f
			: 60.0f
				/ std::max(
					game::world_effective_max_speed(
						actor, stats, world.camera_mode),
					1.0f);
	return true;
}

bool patrol_refresh_formed_targets(
	game::WorldObject& actor,
	game::World& world,
	const mission::Runtime& runtime)
{
	PatrolWork& work = actor.ai.work.patrol;
	if (work.mode == 2
		|| work.route_object >= runtime.object_count)
	{
		return false;
	}
	const game::WorldObject* route =
		mission::runtime_resolve_object(
			runtime, work.route_object, world);
	const game::WorldObject* leader =
		mission::runtime_resolve_object(
			runtime, work.peer_object, world);
	if (route == nullptr || leader == nullptr)
	{
		return false;
	}
	work.previous_target = work.active_target;
	if (&actor == leader)
	{
		work.member_target = route->position;
		work.active_target =
			route->position + work.coordinator_lookahead;
		work.active_target.y = route->position.y;
	}
	else
	{
		work.member_target =
			leader->position
				+ leader->orientation * work.route_offset;
		work.active_target =
			work.member_target
				+ leader->orientation[2] * (leader->radius * 8.0f);
	}
	return true;
}

void patrol_coordinate_group(
	game::WorldObject& actor,
	game::World& world,
	const mission::Runtime& runtime,
	const mission::DteFile& file)
{
	PatrolWork& leader = actor.ai.work.patrol;
	if (actor.group >= runtime.group_count)
	{
		return;
	}
	const mission::GroupRecord& group =
		runtime.groups[actor.group];
	float minimum = 0.0f;
	float maximum = 0.0f;
	bool first = true;
	bool all_ready = true;
	for (std::uint16_t ordinal = 0;
		ordinal < group.member_count;
		++ordinal)
	{
		const std::uint16_t mission_object =
			runtime.group_members[group.first_member + ordinal];
		if (formation_member_exempt(file, mission_object))
		{
			continue;
		}
		game::WorldObject* member =
			game::world_resolve(
				world, runtime.objects[mission_object].live);
		if (member == nullptr
			|| member->ai.command_count == 0
			|| member->ai.commands[0].id != 15)
		{
			all_ready = false;
			continue;
		}
		if (!patrol_refresh_formed_targets(
			*member, world, runtime))
		{
			all_ready = false;
			continue;
		}
		const PatrolWork& work = member->ai.work.patrol;
		const float progress = glm::distance(
			member->position, work.active_target)
			- member->radius * 2.0f;
		if (first)
		{
			minimum = progress;
			maximum = progress;
			first = false;
		}
		else
		{
			minimum = std::min(minimum, progress);
			maximum = std::max(maximum, progress);
		}
		all_ready = all_ready && work.ready;
	}
	const bool compact = !first && maximum - minimum < kPatrolSpread;
	for (std::uint16_t ordinal = 0;
		ordinal < group.member_count;
		++ordinal)
	{
		const std::uint16_t mission_object =
			runtime.group_members[group.first_member + ordinal];
		game::WorldObject* member =
			game::world_resolve(
				world, runtime.objects[mission_object].live);
		if (member == nullptr
			|| member->ai.command_count == 0
			|| member->ai.commands[0].id != 15)
		{
			continue;
		}
		PatrolWork& work = member->ai.work.patrol;
		work.group_compact = compact;
		if (!all_ready)
		{
			continue;
		}
		work.ready = false;
		if (work.mode == 0)
		{
			work.mode = 1;
			continue;
		}
		if (!leader.leg_complete)
		{
			work.transition_pending = true;
			continue;
		}
		const std::uint16_t next =
			route_next(runtime, work.route_pair);
		if (next != UINT16_MAX)
		{
			patrol_initialize_leg(
				*member, world, runtime, file, next);
		}
	}
	leader.group_compact = compact;
}

bool patrol_update(
	game::WorldObject& actor,
	game::World& world,
	const mission::Runtime& runtime,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	game::FlightDemand& demand)
{
	PatrolWork& work = actor.ai.work.patrol;
	if (work.route_object == UINT16_MAX)
	{
		command_pop(world, actor);
		return true;
	}
	const bool coordinator =
		actor.mission_index == work.peer_object;
	if (coordinator)
	{
		patrol_coordinate_group(actor, world, runtime, file);
	}
	if (work.mode == 1
		&& work.route_object < runtime.object_count)
	{
		if (!patrol_refresh_formed_targets(
			actor, world, runtime))
		{
			command_pop(world, actor);
			return true;
		}
	}
	const glm::vec3 delta = work.active_target - actor.position;
	const float distance = glm::length(delta);
	const float facing = distance > 0.0f
		? glm::dot(actor.orientation[2], delta / distance)
		: 1.0f;
	if (work.mode == 0)
	{
		demand = steer_toward(
			actor,
			stats,
			work.active_target,
			actor.control_demand.throttle,
			runtime.frame_delta_ticks,
			0.05f,
			0.1f);
		if (facing >= 0.9f)
		{
			work.ready = true;
		}
		return true;
	}
	if (work.mode == 2)
	{
		demand = steer_toward(
			actor,
			stats,
			work.arrival_target,
			actor.control_demand.throttle,
			runtime.frame_delta_ticks,
			0.5f,
			0.0f);
		if (glm::distance(actor.position, work.arrival_target)
			< actor.radius * 2.0f)
		{
			const std::uint16_t next =
				route_next(runtime, work.route_pair);
			if (next != UINT16_MAX)
			{
				patrol_initialize_leg(
					actor, world, runtime, file, next);
			}
		}
		return true;
	}

	const game::WorldObject* leader =
		mission::runtime_resolve_object(
			runtime, work.peer_object, world);
	if (leader == nullptr)
	{
		command_pop(world, actor);
		return true;
	}
	const PatrolWork& leader_work = leader->ai.work.patrol;
	const float maximum =
		std::max(
			game::world_effective_max_speed(
				actor, stats, world.camera_mode),
			1.0f);
	const bool transitioning = work.transition_pending;
	float requested_speed =
		leader_work.speed_parameter * (coordinator ? 1.0f : 1.1f);
	const float arrival_radius =
		coordinator ? work.route_extent * 0.5f : actor.radius * 2.0f;
	const float arrival_distance =
		glm::distance(actor.position, work.member_target);
	const bool arrived = arrival_distance < arrival_radius;
	if (coordinator)
	{
		// The route coordinator advertises readiness throughout mode one;
		// completion of the waypoint leg is retained separately at +0x66.
		work.ready = true;
	}
	if (!transitioning && !arrived)
	{
		const glm::vec3 member_delta =
			work.member_target - actor.position;
		const float member_length = glm::length(member_delta);
		const bool ahead = member_length <= 0.0001f
			|| glm::dot(
				actor.orientation[2],
				member_delta / member_length) >= 0.0f;
		requested_speed =
			leader_work.speed_parameter * (ahead ? 3.0f : 0.05f);
	}
	demand = steer_toward(
		actor,
		stats,
		work.active_target,
		requested_speed * 200.0f / maximum,
		runtime.frame_delta_ticks,
		0.05f,
		0.1f);
	if (!coordinator
		&& facing >= 0.9f
		&& !arrived
		&& arrival_distance <= actor.radius * 3.2f)
	{
		const glm::vec3 position =
			actor.position
				+ (work.member_target - actor.position) * 0.005f;
		actor.previous_position = position;
		actor.position = position;
	}
	if (coordinator && arrived)
	{
		work.leg_complete = true;
	}
	if (transitioning)
	{
		work.transition_pending = false;
		const float angle = std::acos(
			std::clamp(facing, -1.0f, 1.0f));
		work.speed_parameter =
			angle >= kPatrolFastSpeed
				? kPatrolInitialSpeed
				: angle < kPatrolMediumSpeed
					? 0.5f
					: 0.25f;
	}
	else if (!coordinator && facing >= 0.9f && arrived)
	{
		work.ready = true;
	}
	return true;
}

bool find_dock_locator(
	const game::WorldObject& object,
	std::uint16_t global_ordinal,
	std::uint16_t& model_reference,
	std::uint16_t& model_ordinal)
{
	for (std::uint16_t reference = 0;
		reference < object.model_references.size();
		++reference)
	{
		for (std::uint16_t ordinal = 0;; ++ordinal)
		{
			const assets::GameplayLocator* locator =
				game::model_animation_find_locator(
					object, reference, 9, ordinal);
			if (locator == nullptr)
			{
				break;
			}
			if (global_ordinal-- == 0)
			{
				model_reference = reference;
				model_ordinal = ordinal;
				return true;
			}
		}
	}
	return false;
}

glm::mat4 object_world_transform(
	const game::WorldObject& object)
{
	glm::mat4 transform{object.orientation};
	transform[3] = glm::vec4(object.position, 1.0f);
	return transform;
}

void set_all_position_states(
	game::WorldObject& object,
	const glm::vec3& position)
{
	// Objects_set_all_position_states (LANCER.EXE 0x0049b600).
	object.previous_position = position;
	object.position = position;
	object.scene_position = position;
}

void set_all_orientation_states(
	game::WorldObject& object,
	const glm::mat3& orientation)
{
	// Objects_set_all_orientation_states (LANCER.EXE 0x0049b650).
	object.previous_orientation = orientation;
	object.orientation = orientation;
	object.scene_orientation = orientation;
}

void dock_queue_2d(
	mission::Runtime& runtime,
	std::uint8_t sample)
{
	if (runtime.sound_2d_count
		>= std::size(runtime.sound_2d_events))
	{
		return;
	}
	const std::uint8_t slot = static_cast<std::uint8_t>(
		(runtime.sound_2d_read + runtime.sound_2d_count)
			% std::size(runtime.sound_2d_events));
	runtime.sound_2d_events[slot] = sample;
	++runtime.sound_2d_count;
}

void dock_publish_completion(
	mission::Runtime& runtime,
	const game::WorldObject& actor,
	mission::EventType type)
{
	// AI_Dock_update (LANCER.EXE 0x00406c30) resolves mission-event
	// descriptors 30 and 31 and publishes them against the docking actor.
	// These are script notifications, not same-numbered 2-D sound samples.
	mission::events_emit_direct(
		runtime,
		type,
		actor.mission_index,
		nullptr,
		0);
}

void dock_queue_object_spatial(
	game::World& world,
	const game::WorldObject& object,
	std::uint8_t definition,
	std::uint8_t requested_class)
{
	// Sound3D source kind four applies the player-only 200-unit forward
	// source offset before the initial listener-relative transform.
	game::world_queue_sound_object(
		world,
		{static_cast<std::uint16_t>(
			&object - std::begin(world.objects)), object.generation},
		definition,
		requested_class);
}

void dock_queue_model_spatial(
	game::World& world,
	const game::WorldObject& object,
	std::uint16_t model_reference,
	std::uint8_t definition,
	std::uint8_t requested_class)
{
	if (model_reference == UINT16_MAX
		|| model_reference >= object.model_references.size())
	{
		return;
	}
	const glm::mat4 frame =
		object_world_transform(object)
			* game::model_animation_render_transform(
				object, model_reference, 1.0f);
	game::world_queue_sound_model_frame(
		world,
		{static_cast<std::uint16_t>(
			&object - std::begin(world.objects)), object.generation},
		static_cast<std::int16_t>(model_reference),
		glm::vec3(frame[3]),
		glm::mat3(frame)[2],
		object.linear_velocity,
		definition,
		requested_class);
}

bool dock_desired_frame(
	const game::WorldObject& actor,
	const game::WorldObject& target,
	const DockWork& work,
	const glm::vec3& target_offset,
	glm::vec3& position,
	glm::mat3& orientation)
{
	const assets::GameplayLocator* actor_locator =
		game::model_animation_find_locator(
			actor,
			work.actor_model_reference,
			9,
			work.actor_locator_ordinal);
	const assets::GameplayLocator* target_locator =
		game::model_animation_find_locator(
			target,
			work.target_model_reference,
			9,
			work.target_model_locator_ordinal);
	if (actor_locator == nullptr || target_locator == nullptr)
	{
		return false;
	}
	const glm::mat4 actor_model =
		game::model_animation_render_transform(
			actor, work.actor_model_reference, 1.0f);
	const glm::mat4 target_model =
		game::model_animation_render_transform(
			target, work.target_model_reference, 1.0f);
	const glm::vec3 actor_locator_object = glm::vec3(
		actor_model * glm::vec4(actor_locator->position, 1.0f));
	const glm::vec3 target_locator_world = glm::vec3(
		object_world_transform(target)
		* target_model
		* glm::vec4(target_locator->position, 1.0f));
	orientation =
		target.orientation
		* glm::mat3(target_model)
		* target_locator->basis;
	position =
		target_locator_world
		- orientation * actor_locator_object
		+ orientation * target_offset;
	return true;
}

bool dock_target_model_point(
	const game::WorldObject& target,
	const DockWork& work,
	const glm::vec3& local_position,
	glm::vec3& position)
{
	if (work.target_model_reference
		>= target.model_references.size())
	{
		return false;
	}
	const glm::mat4 model =
		game::model_animation_render_transform(
			target, work.target_model_reference, 1.0f);
	position = glm::vec3(
		object_world_transform(target)
		* model
		* glm::vec4(local_position, 1.0f));
	return true;
}

std::uint16_t dock_model_reference_at(
	const game::WorldObject& object,
	std::uint16_t index)
{
	return index < object.model_references.size()
		? index
		: UINT16_MAX;
}

void dock_start_animation(
	game::WorldObject& object,
	std::uint16_t model_reference,
	const char* name,
	float rate)
{
	if (model_reference == UINT16_MAX)
	{
		return;
	}
	float time = 0.0f;
	if (rate < 0.0f)
	{
		time = -1.0f;
		const game::ObjectModelReference& model =
			object.model_references[model_reference];
		if (std::strcmp(name, "opendoor") == 0
			&& model.sequences != nullptr)
		{
			for (const assets::GameplaySequence& sequence
				: *model.sequences)
			{
				if (std::strcmp(sequence.name, name) == 0)
				{
					time = static_cast<float>(sequence.duration);
					break;
				}
			}
		}
	}
	game::model_animation_start_named(
		object,
		model_reference,
		name,
		time,
		-1,
		rate);
}

void dock_rearm_actor(
	game::WorldObject& actor,
	const assets::ShipStatsTable& stats,
	mission::Runtime& runtime)
{
	// AI_Dock_mode1_update state 3 (0x004079c9..0x00407b4c) rebuilds
	// only the existing hardpoint loadout. The apparent write of 0x1d at
	// object+0x5ec is the signed chaff inventory (29), not the object type.
	// The actor's instantiated model, component graph, and type remain live.
	game::attachments_rebuild_selected_loadout(
		actor,
		mission::network_is_deathmatch_mission(
			runtime.mission_number),
		false);
	actor.chaff_count = 29;
	if (actor.type < assets::kShipStatsCount)
	{
		const assets::ObjectTypeStats& object =
			stats.records[actor.type].object;
		actor.afterburner_fuel = object.afterburner_seconds * 100;
		actor.ammunition = object.ammunition;
		actor.gun_energy = object.gun_energy_max;
	}
	for (std::uint8_t index = 0;
		index < actor.attachment_count;
		++index)
	{
		if (actor.attachments[index].definition_index == 10)
		{
			actor.afterburner_fuel += 5000;
		}
	}
	if (actor.player)
	{
		++runtime.player_ordnance_rebuild_serial;
	}
}

void dock_stop_object(game::WorldObject& object)
{
	game::world_zero_motion_controls(object);
}

float docking_roll_demand(
	const game::WorldObject& actor,
	const glm::mat3& desired)
{
	const glm::vec3 local_up =
		glm::transpose(actor.orientation) * desired[1];
	const float error = std::atan2(local_up.x, local_up.y);
	return std::clamp(
		(-error - actor.angular_z * 2.0f) * 1.4323945f,
		-1.0f,
		1.0f);
}

bool dock_approach_point(
	const game::WorldObject& actor,
	const game::World& world,
	const assets::ShipStatsTable& stats,
	const glm::vec3& target_position,
	const glm::mat3& target_orientation,
	float maximum_throttle,
	std::uint32_t frame_delta,
	game::FlightDemand& demand)
{
	const glm::vec3 displacement = actor.position - target_position;
	const float distance = glm::length(displacement);
	if (distance < 2000.0f)
	{
		demand = {};
		demand.throttle = maximum_throttle;
		return true;
	}
	if (actor.type >= assets::kShipStatsCount)
	{
		demand = {};
		return false;
	}
	const assets::FlightStats& flight = stats.records[actor.type].flight;
	const glm::vec3 target_local =
		glm::transpose(target_orientation) * displacement;
	const glm::vec3 actor_forward_local =
		glm::transpose(target_orientation) * actor.orientation[2];
	const float braking_distance =
		std::max(
			game::world_effective_max_speed(
				actor, stats, world.camera_mode) * 4.0f
				/ std::max(1.0f - flight.linear_retention, 0.000001f),
			0.000001f);
	if (distance * 0.95f < -target_local.z
		&& actor_forward_local.z > 0.98f)
	{
		demand = runtime_steer_toward_point(
			actor,
			target_position,
			1.0f,
			1.0f,
			0.0f,
			3,
			world,
			stats,
			frame_delta);
		demand.roll =
			docking_roll_demand(actor, target_orientation);
		demand.throttle = std::min(
			distance / braking_distance - 0.1f,
			maximum_throttle);
		return false;
	}

	glm::vec3 lateral{
		target_local.x,
		target_local.y,
		0.0f,
	};
	const float lateral_length = glm::length(lateral);
	if (lateral_length > 0.0f)
	{
		lateral /= lateral_length;
	}
	else
	{
		lateral = {0.0f, 0.0f, 1.0f / 33554432.0f};
	}
	const float horizontal = glm::dot(target_local, lateral);
	const float longitudinal = target_local.z;
	const float circle_radius =
		horizontal == 0.0f
			? 0.0f
			: std::abs(
				(horizontal * horizontal
					+ longitudinal * longitudinal)
				/ (horizontal * 2.0f));
	float angle = std::atan2(
		longitudinal, circle_radius - horizontal);
	float turn_angle =
		angle <= -0.5f || angle >= 0.0f
			? angle + 0.5f
			: 0.0f;
	const float minimum_radius = flight.speed_pitch_ratio * 2.0f;
	float radius = circle_radius;
	if (longitudinal > 0.0f)
	{
		turn_angle = 3.14159265358979323846f;
		radius = minimum_radius;
	}
	else
	{
		radius = std::max(radius, minimum_radius);
	}
	const glm::vec3 arc_local =
		lateral * (radius - std::cos(turn_angle) * radius)
		+ glm::vec3{0.0f, 0.0f, 1.0f}
			* (std::sin(turn_angle) * radius);
	const glm::vec3 arc_point =
		target_position + target_orientation * arc_local;
	demand = runtime_steer_toward_point(
		actor,
		arc_point,
		1.0f,
		1.0f,
		0.0f,
		3,
		world,
		stats,
		frame_delta);
	if (turn_angle < 0.0f)
	{
		turn_angle += 6.28318530717958647692f;
	}
	const float arc_length =
		(6.28318530717958647692f - turn_angle) * radius;
	demand.throttle = std::min(
		arc_length / braking_distance - 0.1f,
		maximum_throttle);
	return false;
}

bool dock_provider_target(
	const game::WorldObject& actor,
	const game::WorldObject& target,
	const DockWork& work,
	std::uint32_t tick,
	glm::vec3& provider_target,
	glm::vec3& provider_up)
{
	glm::vec3 target_position;
	glm::mat3 target_orientation;
	if (!dock_desired_frame(
		actor, target, work, {}, target_position, target_orientation))
	{
		return false;
	}
	const float remaining =
		work.interpolation_deadline_tick > tick
		? static_cast<float>(
			work.interpolation_deadline_tick - tick) * 0.001f
		: 0.0f;
	const float phase = std::max(remaining, 0.0f);
	const glm::vec3 target_forward = target_orientation[2];
	const float initial_distance =
		glm::distance(work.interpolation_start, target_position);
	// AI_Dock_interpolated_motion_provider (0x00406f20) multiplies the
	// complete captured distance by phase squared; its separate 0.5 value
	// is the generic provider's speed cap, not a distance coefficient.
	provider_target =
		target_position
		- target_forward
			* (initial_distance * phase * phase);
	provider_up = target_orientation[1];
	return true;
}

bool dock_slot_claimed(
	const game::WorldObject& actor,
	const game::World& world,
	std::uint16_t target,
	std::uint16_t locator)
{
	for (const game::WorldObject& candidate : world.objects)
	{
		if (!candidate.active
			|| &candidate == &actor
			|| candidate.ai.command_count == 0
			|| candidate.ai.commands[0].id != 109)
		{
			continue;
		}
		const Command& command = candidate.ai.commands[0];
		if ((command.target_kind == TargetKind::object
				&& command.target == target
				&& command.target_component
					== static_cast<std::int16_t>(locator))
			|| (candidate.ai.work.dock.target_object == target
				&& candidate.ai.work.dock.target_locator == locator))
		{
			return true;
		}
	}
	return false;
}

game::WorldObject* dock_choose_free_target(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& runtime,
	std::uint16_t& locator)
{
	std::uint16_t objects[game::kMaxMissionObjects];
	std::uint16_t count = 0;
	if (command.target_kind == TargetKind::object)
	{
		objects[0] = command.target;
		count = 1;
	}
	else if (command.target_kind == TargetKind::group
		|| command.target_kind == TargetKind::set)
	{
		count = mission::runtime_expand_reference(
			runtime,
			command.target_kind == TargetKind::group
				? mission::ReferenceKind::group
				: mission::ReferenceKind::set,
			command.target,
			objects,
			static_cast<std::uint16_t>(std::size(objects)));
	}
	for (std::uint16_t object_ordinal = 0;
		object_ordinal < count;
		++object_ordinal)
	{
		game::WorldObject* target = mission::runtime_resolve_object(
			runtime, objects[object_ordinal], world);
		if (target == nullptr)
		{
			continue;
		}
		for (std::uint16_t slot = 0;; ++slot)
		{
			std::uint16_t reference = UINT16_MAX;
			std::uint16_t model_ordinal = UINT16_MAX;
			if (!find_dock_locator(
				*target, slot, reference, model_ordinal))
			{
				break;
			}
			if (dock_slot_claimed(
				actor, world, target->mission_index, slot))
			{
				continue;
			}
			locator = slot;
			command.target_kind = TargetKind::object;
			command.target = target->mission_index;
			command.target_component =
				static_cast<std::int16_t>(slot);
			return target;
		}
	}
	return nullptr;
}

bool dock_begin(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& runtime)
{
	std::uint16_t selected_locator =
		command.target_component < 0
			? UINT16_MAX
			: static_cast<std::uint16_t>(command.target_component);
	game::WorldObject* target =
		command.target_kind != TargetKind::object
			|| selected_locator == UINT16_MAX
			? dock_choose_free_target(
				actor,
				command,
				world,
				runtime,
				selected_locator)
			: resolve_target(actor, command, world, runtime);
	if (target == nullptr)
	{
		command_pop(world, actor);
		return true;
	}
	if (!actor.components_initialized
		|| !target->components_initialized)
	{
		actor.ai.work.begin_pending = true;
		return true;
	}
	DockWork& work = actor.ai.work.dock;
	work = {};
	work.target_object = target->mission_index;
	work.target_locator = selected_locator;
	work.mode =
		actor.type == 0x1d
			? static_cast<std::uint8_t>(target->type == 0x84 ? 3 : 2)
			: actor.type == 0xbc
				? 4
				: static_cast<std::uint8_t>(target->type == 0x18 ? 1 : 0);
	// AI_Dock_begin writes the selected dispatch-table ordinal into byte
	// zero of the live command payload. AI_Dock_end reads that byte from
	// the command record, not from the shared work allocation.
	command.state[0] =
		(command.state[0] & 0xffffff00u) | work.mode;
	diagnostics::mission_log(
		"ai dock actor=%u target=%u slot=%u mode=%u",
		static_cast<unsigned>(actor.mission_index),
		static_cast<unsigned>(target->mission_index),
		static_cast<unsigned>(selected_locator),
		static_cast<unsigned>(work.mode));
	if (work.mode == 4)
	{
		return true;
	}

	if (work.target_locator == UINT16_MAX
		|| !find_dock_locator(
			*target,
			work.target_locator,
			work.target_model_reference,
			work.target_model_locator_ordinal))
	{
		diagnostics::mission_log(
			"ai dock invalid locator actor=%u target=%u slot=%u",
			static_cast<unsigned>(actor.mission_index),
			static_cast<unsigned>(target->mission_index),
			static_cast<unsigned>(work.target_locator));
		command_pop(world, actor);
		return true;
	}
	work.stage = 0;
	if (work.mode == 1)
	{
		actor.interaction_target_link = static_cast<std::uint16_t>(
			target - std::begin(world.objects));
		const assets::GameplayLocator* locator =
			game::model_animation_find_locator(
				*target,
				work.target_model_reference,
				9,
				work.target_model_locator_ordinal);
		if (locator == nullptr)
		{
			command_pop(world, actor);
			return true;
		}
		work.locator_local_position = locator->position;
		work.locator_local_position.y -= actor.bounds_max.y;
		work.first_door_model = dock_model_reference_at(
			*target,
			selected_locator == 0 ? 0 : 1);
		work.second_door_model = dock_model_reference_at(
			*target,
			selected_locator == 0 ? 4 : 3);
		return true;
	}

	// Modes zero, two, and three share AI_Dock_resolve_locator_frames:
	// the actor consumes its first type-9 locator and the selected target
	// locator starts its authored deploy sequence at rate four.
	if (!find_dock_locator(
			actor,
			0,
			work.actor_model_reference,
			work.actor_locator_ordinal))
	{
		diagnostics::mission_log(
			"ai dock actor locator missing actor=%u",
			static_cast<unsigned>(actor.mission_index));
		command_pop(world, actor);
		return true;
	}
	game::model_animation_start_named(
		*target,
		work.target_model_reference,
		"deploy",
		0.0f,
		-1,
		4.0f);
	if (work.mode == 2 || work.mode == 3)
	{
		actor.interaction_target_link = static_cast<std::uint16_t>(
			target - std::begin(world.objects));
		return true;
	}

	glm::vec3 target_position;
	glm::mat3 target_orientation;
	if (!dock_desired_frame(
		actor,
		*target,
		work,
		{},
		target_position,
		target_orientation))
	{
		command_pop(world, actor);
		return true;
	}
	const glm::vec3 local =
		glm::transpose(target_orientation)
			* (actor.position - target_position);
	work.mirrored = local.x > 0.0f;
	if (local.z < -100000.0f)
	{
		work.stage =
			std::abs(local.x / local.z) > 0.2f ? 2 : 3;
	}
	else
	{
		work.stage = local.z < 0.0f ? 1 : 0;
	}
	return true;
}

bool dock_mode0_update(
	game::WorldObject& actor,
	game::WorldObject& target,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	DockWork& work = actor.ai.work.dock;
	if (work.stage <= 4)
	{
		glm::vec3 offset{0.0f};
		switch (work.stage)
		{
		case 0:
			offset = {-100000.0f, 0.0f, 0.0f};
			break;
		case 1:
			offset = {-100000.0f, 0.0f, -100000.0f};
			break;
		case 2:
		{
			const assets::FlightStats& flight =
				stats.records[actor.type].flight;
			offset = {
				game::world_effective_max_speed(
					actor, stats, world.camera_mode) * -2.0f
					/ std::max(flight.yaw_rate, 0.000001f),
				0.0f,
				-100000.0f};
			break;
		}
		case 3:
			offset = {0.0f, 0.0f, -50000.0f};
			break;
		case 4:
			offset = {0.0f, 0.0f, -10000.0f};
			break;
		}
		if (work.mirrored)
		{
			offset.x = -offset.x;
		}
		glm::vec3 position;
		glm::mat3 orientation;
		if (!dock_desired_frame(
			actor, target, work, offset, position, orientation))
		{
			command_pop(world, actor);
			return true;
		}
		demand = runtime_steer_toward_point(
			actor,
			position,
			1.0f,
			1.0f,
			0.0f,
			0,
			world,
			stats,
			runtime.frame_delta_ticks);
		demand.roll = docking_roll_demand(actor, orientation);
		if (glm::dot(position - actor.position, position - actor.position)
			< 4000000.0f)
		{
			++work.stage;
		}
		return true;
	}
	if (work.stage == 5)
	{
		actor.runtime_flags |= game::kObjectFlagKinematic;
		work.interpolation_start = actor.position;
		work.interpolation_deadline_tick = tick + 1000;
		actor.flight_callback_mode =
			game::FlightCallbackMode::provider_forward;
		// Mode zero clears only the target's retained world velocity here;
		// modes two/three use the full AI_stop_object helper instead.
		target.linear_velocity = {};
		target.speed = 0.0f;
		++work.stage;
		applies_flight = true;
		return true;
	}
	if (work.stage == 6)
	{
		// The installed indirect provider owns motion and advances to stage
		// seven when its strict deadline test succeeds.
		applies_flight = true;
		return true;
	}
	dock_stop_object(actor);
	dock_queue_object_spatial(world, actor, 55, 0);
	dock_publish_completion(
		runtime, actor, mission::EventType::docked);
	actor.runtime_flags &= ~game::kObjectFlagKinematic;
	command_pop(world, actor);
	applies_flight = false;
	return true;
}

bool dock_mode1_update(
	game::WorldObject& actor,
	game::WorldObject& target,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	DockWork& work = actor.ai.work.dock;
	if (work.stage < 5)
	{
		// Mode one installs Flight_update_linear_with_exhaust at begin and
		// keeps it through the state-four deadline transition.
		demand.throttle = actor.throttle;
		demand.linear_with_exhaust = true;
	}
	switch (work.stage)
	{
	case 0:
		if (actor.player)
		{
			runtime.requested_camera_mode = 43;
			runtime.requested_camera_target =
				static_cast<std::uint16_t>(
					&target - std::begin(world.objects));
			runtime.requested_camera_lock = true;
			runtime.requested_camera_override_lock = true;
			mission::runtime_publish_camera_request(runtime);
		}
		dock_start_animation(
			target, work.first_door_model, "opendoor", 1.0f);
		dock_queue_model_spatial(
			world, target, work.first_door_model, 46, 4);
		++work.stage;
		return true;
	case 1:
	{
		glm::vec3 local = work.locator_local_position;
		local.y -= 1500.0f;
		local.z += 20000.0f;
		glm::vec3 approach;
		if (!dock_target_model_point(
			target, work, local, approach))
		{
			command_pop(world, actor);
			return true;
		}
		const float distance = glm::distance(actor.position, approach);
		const float throttle =
			distance < 10000.0f
				? 100.0f / std::max(
					game::world_effective_max_speed(
						actor, stats, world.camera_mode),
					1.0f)
				: 1.0f;
		demand = runtime_steer_toward_point(
			actor,
			approach,
			throttle,
			1.0f,
			0.0f,
			0,
			world,
			stats,
			runtime.frame_delta_ticks);
		if (distance < 2000.0f)
		{
			++work.stage;
		}
		return true;
	}
	case 2:
	{
		glm::vec3 position;
		if (!dock_target_model_point(
			target,
			work,
			work.locator_local_position,
			position))
		{
			command_pop(world, actor);
			return true;
		}
		const float distance = glm::distance(actor.position, position);
		glm::vec3 steering_point = position;
		if (distance < 1000.0f)
		{
			steering_point +=
				target.orientation
				* (glm::vec3{0.0f, -0.075f, 1.0f}
					* (distance * 0.6f));
		}
		demand = runtime_steer_toward_point(
			actor,
			steering_point,
			1.0f,
			1.0f,
			0.0f,
			0,
			world,
			stats,
			runtime.frame_delta_ticks);
		const glm::vec3 to_target = position - actor.position;
		if (glm::dot(actor.orientation[2], to_target)
			> distance * 0.98f)
		{
			demand.roll =
				docking_roll_demand(actor, target.orientation);
		}
		demand.throttle = std::min(
			distance * 0.0001f - 0.02f,
			100.0f / std::max(
				game::world_effective_max_speed(
					actor, stats, world.camera_mode),
				1.0f));
		if (distance < 500.0f)
		{
			dock_queue_object_spatial(world, actor, 47, 4);
			demand = {};
			demand.linear_with_exhaust = true;
			dock_start_animation(
				target,
				work.first_door_model,
				"opendoor",
				-1.0f);
			dock_queue_model_spatial(
				world, target, work.first_door_model, 46, 4);
			++work.stage;
			work.deadline = tick + 500;
		}
		return true;
	}
	case 3:
		if (work.deadline < tick)
		{
			dock_rearm_actor(actor, stats, runtime);
			diagnostics::mission_log(
				"ai dock actor=%u loadout rebuilt chaff=%d",
				static_cast<unsigned>(actor.mission_index),
				static_cast<int>(actor.chaff_count));
			dock_start_animation(
				target,
				work.second_door_model,
				"opendoor",
				1.0f);
			dock_queue_model_spatial(
				world, target, work.second_door_model, 46, 4);
			++work.stage;
			work.deadline = tick + 400;
		}
		applies_flight = true;
		return true;
	case 4:
		if (work.deadline < tick)
		{
			dock_publish_completion(
				runtime, actor, mission::EventType::docked);
			++work.stage;
			work.deadline = tick + 150;
			demand.linear_with_exhaust = false;
		}
		applies_flight = true;
		return true;
	default:
		demand.afterburner = true;
		applies_flight = true;
		if (work.deadline < tick)
		{
			dock_start_animation(
				target,
				work.second_door_model,
				"opendoor",
				-1.0f);
			dock_queue_model_spatial(
				world, target, work.second_door_model, 46, 4);
			command_pop(world, actor);
			if (actor.player)
			{
				runtime.requested_camera_mode = 0;
				runtime.requested_camera_target =
					static_cast<std::uint16_t>(
						&actor - std::begin(world.objects));
				runtime.requested_camera_lock = false;
				runtime.requested_camera_override_lock = true;
				mission::runtime_publish_camera_request(runtime);
			}
		}
		return true;
	}
}

bool dock_modes23_update(
	game::WorldObject& actor,
	game::WorldObject& target,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	DockWork& work = actor.ai.work.dock;
	if ((target.runtime_flags & game::kObjectFlagDestroyed) != 0)
	{
		if (work.stage == 0)
		{
			command_pop(world, actor);
			return true;
		}
		if (work.stage < 5)
		{
			// AI_Dock_modes_2_3_update, LANCER.EXE
			// 0x00407d8c..0x00407dcd and
			// 0x0040823a..0x00408277. A lost carrier during approach
			// goes through the full death selector, preserving ejection,
			// player-death, and active-command end semantics. Once stage
			// five has begun retail ignores the lost-carrier flag and
			// completes the remaining undock sequence.
			(void)schedule_death_command(
				actor, world, runtime, 0, false);
			return true;
		}
	}
	glm::vec3 position;
	glm::mat3 orientation;
	if (work.stage == 0)
	{
		if (!dock_desired_frame(
			actor,
			target,
			work,
			{0.0f, 0.0f, -10000.0f},
			position,
			orientation))
		{
			command_pop(world, actor);
			return true;
		}
		if (dock_approach_point(
			actor,
			world,
			stats,
			position,
			orientation,
			1.0f,
			runtime.frame_delta_ticks,
			demand))
		{
			++work.stage;
		}
		return true;
	}
	if (work.stage == 1)
	{
		actor.runtime_flags |= game::kObjectFlagKinematic;
		work.interpolation_start = actor.position;
		work.interpolation_deadline_tick = tick + 1000;
		actor.flight_callback_mode =
			game::FlightCallbackMode::provider_forward;
		dock_stop_object(target);
		++work.stage;
		applies_flight = true;
		return true;
	}
	if (work.stage == 2)
	{
		applies_flight = true;
		return true;
	}
	if (work.stage == 3)
	{
		dock_stop_object(actor);
		dock_queue_object_spatial(world, actor, 55, 0);
		dock_start_animation(actor, 1, "rotate", 4.0f);
		dock_start_animation(actor, 2, "rotate", 4.0f);
		if (work.mode == 2 && !actor.model_references.empty())
		{
			game::ObjectModelReference& root =
				actor.model_references[0];
			if ((root.runtime_flags & 0x20u) == 0)
			{
				root.runtime_flags |= 0x20u;
				if (actor.model_references.size() > 1)
					{
						const glm::mat4 paired_frame =
							object_world_transform(actor)
							* game::model_animation_render_transform(
								actor, 0, 1.0f);
					const game::ObjectHandle paired =
						game::world_create(
							world,
							0xbc,
							glm::vec3(paired_frame[3]),
							glm::mat3(paired_frame),
							stats,
							false);
					if (game::WorldObject* created =
						game::world_resolve(world, paired))
					{
						work.paired_object = paired.index;
						work.paired_generation = paired.generation;
						actor.docking_pair_link = paired.index;
						created->interaction_target_link =
							static_cast<std::uint16_t>(
								&target
									- std::begin(world.objects));
						created->docking_pair_link =
							static_cast<std::uint16_t>(
								&actor - std::begin(world.objects));
						created->runtime_flags |= game::kObjectFlagKinematic;
						command_push(world,
							*created,
							109,
							TargetKind::object,
							target.mission_index,
							static_cast<std::int16_t>(
								work.target_locator));
						diagnostics::mission_log(
							"ai dock paired actor=%u pair=%u "
							"target=%u",
							static_cast<unsigned>(
								actor.mission_index),
							static_cast<unsigned>(paired.index),
							static_cast<unsigned>(
								target.mission_index));
					}
				}
			}
			else
			{
				root.runtime_flags &= ~0x20u;
				for (std::uint16_t object_index = 0;
					object_index < game::kMaxGameObjects;
					++object_index)
				{
					game::WorldObject& candidate =
						world.objects[object_index];
					if (!candidate.active
						|| candidate.type != 0xbc)
					{
						continue;
					}
					game::world_destroy(
						world,
						{object_index, candidate.generation});
					break;
				}
			}
		}
		work.deadline =
			tick + (work.mode == 3 ? 12000 : 400);
		if (work.mode == 3)
		{
			dock_publish_completion(
				runtime, actor, mission::EventType::docked);
		}
		++work.stage;
		applies_flight = false;
		return true;
	}
	applies_flight = true;
	demand.throttle = actor.throttle;
	if (work.stage == 4 && work.deadline < tick)
	{
		actor.throttle = 1.0f;
		demand.throttle = 1.0f;
		dock_queue_object_spatial(world, actor, 56, 0);
		work.deadline = tick + 400;
		++work.stage;
	}
	else if (work.stage == 5 && work.deadline < tick)
	{
		actor.throttle = 0.0f;
		demand.throttle = 0.0f;
		dock_start_animation(actor, 1, "rotate", -4.0f);
		dock_start_animation(actor, 2, "rotate", -4.0f);
		work.deadline = tick + 400;
		++work.stage;
	}
	else if (work.stage == 6 && work.deadline < tick)
	{
		if (work.mode == 3)
		{
			dock_publish_completion(
				runtime, actor, mission::EventType::undocked);
		}
		else
		{
			const bool deployed =
				!actor.model_references.empty()
				&& (actor.model_references[0].runtime_flags
					& 0x20u) != 0;
			dock_publish_completion(
				runtime,
				actor,
				deployed
					? mission::EventType::docked
					: mission::EventType::undocked);
		}
		actor.runtime_flags &= ~game::kObjectFlagKinematic;
		command_pop(world, actor);
	}
	(void)world;
	return true;
}

bool dock_update(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	DockWork& work = actor.ai.work.dock;
	game::WorldObject* target = mission::runtime_resolve_object(
		runtime, work.target_object, world);
	if (work.mode == 4)
	{
		if (target == nullptr
			|| (target->runtime_flags & game::kObjectFlagDestroyed) != 0)
		{
			// AI_Dock_mode_4_update, LANCER.EXE
			// 0x004084b0..0x004084d7, uses the same complete death
			// selector rather than inserting Explode directly.
			(void)schedule_death_command(
				actor, world, runtime, 0, false);
		}
		return true;
	}
	if (target == nullptr)
	{
		if ((work.mode == 2 || work.mode == 3)
			&& work.stage != 0
			&& work.stage < 5)
		{
			actor.runtime_flags &= ~game::kObjectFlagKinematic;
			command_push(world,
				actor,
				11,
				TargetKind::none,
				UINT16_MAX);
		}
		else
		{
			actor.runtime_flags &= ~game::kObjectFlagKinematic;
			command_pop(world, actor);
		}
		return true;
	}
	if (work.mode == 0)
	{
		return dock_mode0_update(
			actor,
			*target,
			world,
			runtime,
			stats,
			tick,
			demand,
			applies_flight);
	}
	if (work.mode == 1)
	{
		return dock_mode1_update(
			actor,
			*target,
			world,
			runtime,
			stats,
			tick,
			demand,
			applies_flight);
	}
	return dock_modes23_update(
		actor,
		*target,
		world,
		runtime,
		stats,
		tick,
		demand,
		applies_flight);
}

std::size_t primary_model_count(
	const game::WorldObject& object)
{
	return object.primary_model_reference_count;
}

std::uint16_t primary_model_reference(
	const game::WorldObject& object,
	std::uint16_t index)
{
	// GameObject+0x128 is the fixed source-order pointer array. Landing's
	// hard-coded +0x04/+0x18 accesses therefore mean indices one and six;
	// they are not hierarchy-child ordinals.
	return index < primary_model_count(object)
		&& index < object.model_references.size()
		? index
		: UINT16_MAX;
}

glm::mat4 model_world_transform(
	const game::WorldObject& object,
	std::uint16_t reference)
{
	return object_world_transform(object)
		* game::model_animation_render_transform(
			object, reference, 1.0f);
}

constexpr float kLaunchPi = 3.14159265358979323846f;

bool launch_locator_eligible(
	const game::WorldObject&,
	std::uint16_t reference,
	const assets::GameplayLocator& locator)
{
	if (locator.type == 8)
	{
		return true;
	}
	if (locator.type != 5)
	{
		return false;
	}
	// The retained walk tests the six 16-byte-spaced SHP globals beginning
	// at 0x005392e8. Retail startup fills slots 0, 1, and 5 with
	// 01_cargo_pod.shp, 02_rus_pod.shp, and fuel_pod1.shp respectively;
	// type-five locators on those references are therefore unavailable.
	// This is not a destroyed-component or model-runtime-flag test.
	return reference != 0 && reference != 1 && reference != 5;
}

std::uint16_t launch_eligible_point_count(
	const game::WorldObject& carrier,
	std::int32_t selected_ordinal,
	std::uint16_t* selected_reference = nullptr,
	const assets::GameplayLocator** selected_locator = nullptr)
{
	std::uint16_t count = 0;
	const std::size_t primary_count =
		carrier.primary_model_reference_count;
	for (std::uint16_t reference = 0;
		reference < primary_count;
		++reference)
	{
		const game::ObjectModelReference& model =
			carrier.model_references[reference];
		if (model.locators == nullptr)
		{
			continue;
		}
		for (const assets::GameplayLocator& locator : *model.locators)
		{
			if (locator.source_node != model.source_node
				|| !launch_locator_eligible(
					carrier, reference, locator))
			{
				continue;
			}
			if (selected_ordinal == count)
			{
				if (selected_reference != nullptr)
				{
					*selected_reference = reference;
				}
				if (selected_locator != nullptr)
				{
					*selected_locator = &locator;
				}
			}
			++count;
		}
	}
	return count;
}

bool launch_resolve_automatic_target(
	Command& command,
	game::World& world,
	mission::Runtime& runtime,
	LaunchWork& work)
{
	std::uint16_t candidates[game::kMaxMissionObjects];
	std::uint16_t candidate_count = 0;
	if (command.target_kind == TargetKind::object)
	{
		if (command.target != UINT16_MAX)
		{
			candidates[0] = command.target;
			candidate_count = 1;
		}
	}
	else
	{
		if (command.target_kind != TargetKind::group
			&& command.target_kind != TargetKind::set)
		{
			return false;
		}
		const mission::ReferenceKind kind =
			command.target_kind == TargetKind::group
				? mission::ReferenceKind::group
				: mission::ReferenceKind::set;
		candidate_count = mission::runtime_expand_reference(
			runtime,
			kind,
			command.target,
			candidates,
			static_cast<std::uint16_t>(std::size(candidates)));
	}
	std::int32_t remaining = command.sequence;
	std::uint16_t resolved = UINT16_MAX;
	std::int16_t point = -1;
	for (std::uint16_t index = 0; index < candidate_count; ++index)
	{
		game::WorldObject* carrier =
			mission::runtime_resolve_object(
				runtime, candidates[index], world);
		if (carrier == nullptr)
		{
			continue;
		}
		const std::uint16_t points =
			launch_eligible_point_count(*carrier, -1);
		resolved = candidates[index];
		point = static_cast<std::int16_t>(points);
		if (remaining < points)
		{
			point = static_cast<std::int16_t>(remaining);
			break;
		}
		remaining -= points;
	}
	if (resolved == UINT16_MAX)
	{
		return false;
	}
	command.target_kind = TargetKind::object;
	command.target = resolved;
	command.target_component = point;
	work.requested_ordinal = command.sequence;
	return true;
}

bool launch_place_at_locator(
	game::WorldObject& actor,
	const game::WorldObject& carrier,
	LaunchWork& work,
	std::int16_t point)
{
	std::uint16_t reference = UINT16_MAX;
	const assets::GameplayLocator* locator = nullptr;
	launch_eligible_point_count(
		carrier, point, &reference, &locator);
	if (reference == UINT16_MAX || locator == nullptr)
	{
		return false;
	}
	const glm::mat4 child =
		model_world_transform(carrier, reference);
	const glm::mat3 child_basis{child};
	const glm::vec3 child_position{child[3]};
	const glm::vec3 locator_offset =
		locator->position + locator->basis * actor.center_of_mass;
	actor.position = child_position + child_basis * locator_offset;
	actor.orientation = child_basis * locator->basis;
	actor.previous_position = actor.position;
	actor.previous_orientation = actor.orientation;
	actor.linear_velocity = {};
	actor.speed = 0.0f;
	work.launch_model = reference;
	return true;
}

void launch_place_at_side_bay(
	game::WorldObject& actor,
	const game::WorldObject& carrier,
	LaunchWork& work,
	std::uint16_t reference,
	const glm::vec3& local_offset,
	float yaw)
{
	if (reference == UINT16_MAX
		|| reference >= primary_model_count(carrier))
	{
		return;
	}
	const glm::mat4 frame = model_world_transform(carrier, reference);
	actor.position = glm::vec3(
		frame * glm::vec4(local_offset, 1.0f));
	// AI_Launch_strategy1_begin builds the side-facing yaw first, then
	// FUN_004c1da0 replaces it with child_basis * yaw
	// (LANCER.EXE 0x004193af..0x00419436). The order is material for the
	// Yamato bay nodes, whose authored bases exchange the Y and Z axes.
	const glm::mat3 side_facing =
		math::rotation_from_euler({0.0f, yaw, 0.0f});
	actor.orientation = glm::mat3(frame) * side_facing;
	actor.previous_position = actor.position;
	actor.previous_orientation = actor.orientation;
	actor.linear_velocity = {};
	actor.speed = 0.0f;
	work.launch_model = reference;
}

void launch_place_at_sloped_deck(
	game::WorldObject& actor,
	const game::WorldObject& carrier,
	LaunchWork& work,
	std::uint16_t reference,
	const glm::vec3& local_offset,
	float yaw,
	float pitch)
{
	if (reference == UINT16_MAX
		|| reference >= primary_model_count(carrier))
	{
		return;
	}
	const glm::mat4 frame = model_world_transform(carrier, reference);
	actor.position = glm::vec3(
		frame * glm::vec4(local_offset, 1.0f));
	// AI_Launch_strategy2_begin copies the concatenated model basis and
	// post-rotates yaw and pitch in place
	// (LANCER.EXE 0x00419fe8..0x0041a0ed).
	actor.orientation = math::postrotate(
		glm::mat3(frame), yaw, {0.0f, 1.0f, 0.0f});
	actor.orientation = math::postrotate(
		actor.orientation, pitch, {1.0f, 0.0f, 0.0f});
	actor.previous_position = actor.position;
	actor.previous_orientation = actor.orientation;
	actor.linear_velocity = {};
	actor.speed = 0.0f;
	work.launch_model = reference;
}

void launch_start_model_animation(
	game::WorldObject& object,
	std::uint16_t reference,
	float rate)
{
	if (reference == UINT16_MAX
		|| reference >= object.model_references.size())
	{
		return;
	}
	game::model_animation_start_named(
		object, reference, "opendoor", -1.0f, 1, rate);
}

void launch_request_camera(
	mission::Runtime& runtime,
	const game::World& world,
	game::WorldObject& actor,
	std::uint8_t mode,
	bool lock = true)
{
	runtime.requested_camera_mode = mode;
	runtime.requested_camera_target = static_cast<std::uint16_t>(
		&actor - std::begin(world.objects));
	runtime.requested_camera_lock = lock;
	runtime.requested_camera_override_lock = true;
	mission::runtime_publish_camera_request(runtime);
}

game::WorldObject* launch_cinematic_object(
	game::World& world,
	const LaunchWork& work)
{
	return game::world_resolve(
		world,
		{work.cinematic_object, work.cinematic_generation});
}

bool launch_cinematic_object_ready(
	game::World& world,
	const LaunchWork& work)
{
	const game::WorldObject* scene =
		launch_cinematic_object(world, work);
	return scene != nullptr && scene->components_initialized;
}

game::WorldObject* launch_create_cinematic_object(
	game::World& world,
	const assets::ShipStatsTable& stats,
	LaunchWork& work,
	std::uint16_t type,
	const glm::vec3& position,
	const glm::mat3& orientation)
{
	// All player launch strategies share DAT_0057e04e, the reserved final
	// GameObject slot. Strategy one (yam_tube.shp) and strategy six
	// (reliant_hang.shp) both pass this exact slot to GameObject_create;
	// ordinary mission/effect allocation must not change its traversal or
	// lifetime semantics.
	constexpr std::uint16_t kLaunchScene =
		static_cast<std::uint16_t>(game::kMaxGameObjects - 1u);
	const game::ObjectHandle handle = game::world_create_at(
		world,
		kLaunchScene,
		type,
		position,
		orientation,
		stats,
		false,
		UINT16_MAX,
		UINT8_MAX,
		UINT8_MAX);
	game::WorldObject* object = game::world_resolve(world, handle);
	if (object == nullptr)
	{
		return nullptr;
	}
	object->runtime_flags |= 0x00000004u;
	object->targetable = false;
	object->runtime_flags &= ~0x00000200u;
	work.cinematic_object = handle.index;
	work.cinematic_generation = handle.generation;
	return object;
}

void launch_service_cinematic_light_masks(
	game::World& world,
	const LaunchWork& work)
{
	game::WorldObject* scene = launch_cinematic_object(world, work);
	if (scene == nullptr)
	{
		return;
	}
	for (std::uint16_t reference = 0;
		reference < scene->model_references.size() && reference < 3;
		++reference)
	{
		scene->model_references[reference].light_exclusion_mask = 0x3bu;
	}
}

void launch_destroy_cinematic_object(
	game::World& world,
	LaunchWork& work)
{
	if (work.cinematic_object == UINT16_MAX)
	{
		return;
	}
	game::world_destroy(
		world,
		{work.cinematic_object, work.cinematic_generation});
	work.cinematic_object = UINT16_MAX;
	work.cinematic_generation = 0;
	work.cinematic_placement_pending = false;
	work.cinematic_attachment = false;
}

void launch_capture_attachment(
	game::WorldObject& actor,
	const game::WorldObject& carrier,
	LaunchWork& work);

void launch_service_cinematic_placement(
	game::WorldObject& actor,
	game::World& world,
	LaunchWork& work)
{
	if (!work.cinematic_placement_pending)
	{
		return;
	}
	game::WorldObject* scene = launch_cinematic_object(world, work);
	if (scene == nullptr || !scene->components_initialized)
	{
		return;
	}
	if (work.strategy == 1)
	{
		// AI_Launch_strategy1_begin (LANCER.EXE
		// 0x00419784..0x0041980d) anchors the synthetic tube by the
		// horizontal center and maximum Z of its aggregate bounds. It
		// rotates that local anchor by the launch actor's orientation,
		// subtracts it from the actor position, then assigns the same
		// orientation to the tube.
		const glm::vec3 local_anchor{
			(scene->bounds_min.x + scene->bounds_max.x) * 0.5f,
			(scene->bounds_min.y + scene->bounds_max.y) * 0.5f,
			scene->bounds_max.z,
		};
		set_all_position_states(
			*scene,
			work.cinematic_anchor_position
				- work.cinematic_anchor_orientation * local_anchor);
		set_all_orientation_states(
			*scene, work.cinematic_anchor_orientation);
		work.cinematic_placement_pending = false;
		return;
	}
	if (work.strategy != 6)
	{
		work.cinematic_placement_pending = false;
		return;
	}

	// AI_Launch_strategy6_begin (LANCER.EXE
	// 0x0041b17a..0x0041b201) first measures the selected hangar locator at
	// the origin, translates the synthetic hangar to the retained carrier-bay
	// midpoint, then places the actor through the same locator again.
	const glm::vec3 desired_actor_position =
		work.cinematic_anchor_position;
	const std::int16_t hangar_locator =
		(work.launch_point & 1) == 0 ? 1 : 0;
	set_all_position_states(*scene, {});
	glm::mat3 scene_orientation = work.cinematic_anchor_orientation;
	if ((work.launch_point & 1) == 0)
	{
		scene_orientation = math::postrotate(
			scene_orientation,
			kLaunchPi,
			{0.0f, 1.0f, 0.0f});
	}
	set_all_orientation_states(*scene, scene_orientation);
	if (!launch_place_at_locator(actor, *scene, work, hangar_locator))
	{
		return;
	}
	set_all_position_states(
		*scene,
		desired_actor_position - actor.position);
	if (!launch_place_at_locator(actor, *scene, work, hangar_locator))
	{
		return;
	}
	launch_capture_attachment(actor, *scene, work);
	work.cinematic_attachment = true;
	work.cinematic_placement_pending = false;
}

void launch_capture_attachment(
	game::WorldObject& actor,
	const game::WorldObject& carrier,
	LaunchWork& work)
{
	const glm::mat4 frame =
		work.launch_model < primary_model_count(carrier)
			? model_world_transform(carrier, work.launch_model)
			: object_world_transform(carrier);
	const glm::mat4 actor_frame = object_world_transform(actor);
	const glm::mat4 relative = glm::inverse(frame) * actor_frame;
	work.relative_position = glm::vec3(relative[3]);
	work.relative_orientation = glm::mat3(relative);
	work.launch_active = true;
}

void launch_service_attachment(
	game::WorldObject& actor,
	const game::WorldObject& carrier,
	game::World& world,
	const LaunchWork& work)
{
	if (!work.launch_active)
	{
		game::world_clear_scene_attachment(actor);
		return;
	}
	const glm::mat4 frame =
		work.launch_model < primary_model_count(carrier)
			? model_world_transform(carrier, work.launch_model)
			: object_world_transform(carrier);
	const glm::mat4 attached =
		frame
		* glm::translate(
			glm::mat4{1.0f}, work.relative_position)
		* glm::mat4{work.relative_orientation};
	actor.position = glm::vec3(attached[3]);
	actor.orientation = glm::mat3(attached);
	actor.previous_position = actor.position;
	actor.previous_orientation = actor.orientation;
	game::world_set_scene_attachment(
		world,
		actor,
		carrier,
		work.launch_model < primary_model_count(carrier)
			? static_cast<std::int16_t>(work.launch_model)
			: -1,
		work.relative_position,
		work.relative_orientation);
}

enum LaunchControlReset : std::uint8_t
{
	kLaunchResetThrottle = 1u << 0,
	kLaunchResetPitch = 1u << 1,
	kLaunchResetRoll = 1u << 2,
	kLaunchResetYaw = 1u << 3,
	kLaunchResetAll =
		kLaunchResetThrottle
			| kLaunchResetPitch
			| kLaunchResetRoll
			| kLaunchResetYaw,
};

void launch_complete(
	game::World& world,
	game::WorldObject& actor,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	bool clear_carrier_link,
	std::uint8_t control_reset)
{
	if (clear_carrier_link)
	{
		actor.interaction_target_link = UINT16_MAX;
	}
	if ((control_reset & kLaunchResetThrottle) != 0)
	{
		// Launch completion stores target the retained control request
		// fields at GameObject+0x5b8..+0x5c4, not the derived throttle or
		// angular rates at +0x5d8..+0x5e4.
		actor.control_demand.throttle = 0.0f;
	}
	if ((control_reset & kLaunchResetPitch) != 0)
	{
		actor.control_demand.pitch = 0.0f;
	}
	if ((control_reset & kLaunchResetRoll) != 0)
	{
		actor.control_demand.roll = 0.0f;
	}
	if ((control_reset & kLaunchResetYaw) != 0)
	{
		actor.control_demand.yaw = 0.0f;
	}
	actor.flight_callback_mode =
		game::FlightCallbackMode::standard_forward;
	actor.targetable =
		actor.type < assets::kShipStatsCount
		&& stats.records[actor.type].object.targetable_capability;
	if (actor.targetable)
	{
		actor.runtime_flags |= 0x200u;
	}
	if (actor.player && actor.ai.work.launch.cinematic_active)
	{
		runtime.launch_title_active = false;
	}
	mission::events_emit_launched(runtime, actor.mission_index);
	command_pop(world, actor);
}

void launch_start_mission_title(
	mission::Runtime& runtime,
	std::uint32_t now_tick)
{
	// The player launch cinematics set HUD's objective/typewriter flag,
	// snapshot the 100 Hz audio clock, and reset the reveal cursor.
	runtime.launch_title_active = true;
	runtime.launch_title_next_tick = now_tick;
	runtime.launch_title_reveal = 0;
}

std::uint16_t launch_strategy0_model(
	const game::WorldObject& carrier,
	std::int16_t point,
	bool second)
{
	std::int32_t index = -1;
	switch (carrier.type)
	{
	case 0x11:
	case 0xa0:
		if (point >= 0 && point < 4)
		{
			index = 3 + point * 2 + (second ? 1 : 0);
		}
		break;
	case 0x12:
	{
		const std::int32_t first[] = {13, 15, 19, 17};
		if (point >= 0 && point < 4)
		{
			index = first[point] + (second ? 1 : 0);
		}
		break;
	}
	case 0x14:
		if (point == 0)
		{
			index = second ? 7 : 6;
		}
		else if (point == 1)
		{
			index = second ? 5 : 4;
		}
		break;
	case 0x38:
	case 0x9b:
		if (!second && point >= 0 && point < 4)
		{
			index = point < 2 ? 15 : 16;
		}
		break;
	case 0x78:
		if (!second && point >= 0 && point < 3)
		{
			index = 3 + point;
		}
		break;
	case 0xc2:
		if (!second && point == 0)
		{
			index = 8;
		}
		else if (second && point == 1)
		{
			index = 7;
		}
		break;
	default:
		break;
	}
	return index >= 0
		&& index < static_cast<std::int32_t>(
			primary_model_count(carrier))
		? static_cast<std::uint16_t>(index)
		: UINT16_MAX;
}

void launch_spawn_particle(
	mission::LaunchParticle& particle,
	game::World& world,
	const glm::vec3& world_position,
	const glm::mat3& world_orientation,
	const glm::vec3& local_direction,
	const glm::vec3& spread,
	std::uint32_t now_tick,
	std::uint32_t elapsed_ticks)
{
	// Particle_spawn_into_slot (0x0049c1c0) consumes the shared CRT
	// stream in lifetime, Z, Y, X, and speed order.
	particle.lifetime_ticks = static_cast<std::uint16_t>(
		50u + game::world_rand15(world) % 10u);
	const glm::vec3 jitter{
		0.0f,
		0.0f,
		(static_cast<float>(game::world_rand15(world))
				* (1.0f / 32767.0f)
			- 0.5f) * spread.z,
	};
	glm::vec3 ordered_jitter = jitter;
	ordered_jitter.y =
		(static_cast<float>(game::world_rand15(world))
				* (1.0f / 32767.0f)
			- 0.5f) * spread.y;
	ordered_jitter.x =
		(static_cast<float>(game::world_rand15(world))
				* (1.0f / 32767.0f)
			- 0.5f) * spread.x;
	glm::vec3 direction = local_direction + ordered_jitter;
	float speed = 0.0f;
	if (glm::dot(direction, direction) > 0.000001f)
	{
		direction = glm::normalize(direction);
		speed =
			10.0f
			+ static_cast<float>(game::world_rand15(world))
				* (1.0f / 32767.0f) * 3.0f;
	}
	particle.position = world_position;
	particle.velocity = world_orientation * direction * speed;
	particle.birth_tick = now_tick;
	particle.active = true;
	// Automatic mode-zero emission consumes one further rand() and
	// pre-advances a fresh BMO position by the complete
	// elapsed simulation-tick delta for this rendered frame.
	game::world_rand15(world);
	particle.position += particle.velocity
		* static_cast<float>(elapsed_ticks);
}

void launch_service_emitters(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& runtime,
	std::uint32_t now_tick)
{
	LaunchWork& work = actor.ai.work.launch;
	launch_service_cinematic_light_masks(world, work);
	static constexpr glm::vec3 positions[6] = {
		{-779.0f, -600.0f, 40.0f},
		{-370.0f, -610.0f, -25.0f},
		{-370.0f, 450.0f, -25.0f},
		{370.0f, -610.0f, -25.0f},
		{370.0f, 450.0f, -25.0f},
		{935.0f, -600.0f, 40.0f},
	};
	static constexpr glm::vec3 directions[6] = {
		{1.0f, 0.0f, 0.0f},
		{0.5f, 0.5f, -1.0f},
		{0.5f, -0.5f, -1.0f},
		{-0.5f, 0.5f, -1.0f},
		{-0.5f, -0.5f, -1.0f},
		{-1.0f, 0.0f, 0.0f},
	};
	static constexpr glm::vec3 spreads[6] = {
		{0.0f, 0.1f, 0.1f},
		{0.1f, 0.1f, 0.1f},
		{0.1f, 0.1f, 0.1f},
		{0.1f, 0.1f, 0.1f},
		{0.1f, 0.1f, 0.1f},
		{0.0f, 0.1f, 0.1f},
	};
	for (std::uint8_t emitter = 0; emitter < 6; ++emitter)
	{
		const bool outer = emitter == 0 || emitter == 5;
		if (outer
			&& work.emitter_burst_active[emitter]
			&& now_tick >= work.emitter_end_tick[emitter])
		{
			work.emitter_burst_active[emitter] = false;
		}
		if (outer
			&& !work.emitter_burst_active[emitter]
			&& now_tick > work.emitter_next_tick[emitter])
		{
			work.emitter_burst_active[emitter] = true;
			work.emitter_end_tick[emitter] =
				now_tick
					+ 20u
					+ game::world_rand15(world) % 80u;
			work.emitter_next_tick[emitter] =
				work.emitter_end_tick[emitter]
					+ 20u
					+ game::world_rand15(world) % 100u;
		}
		if (!work.emitter_burst_active[emitter]
			|| now_tick >= work.emitter_end_tick[emitter]
			|| now_tick < work.emitter_next_tick[emitter])
		{
			continue;
		}
		glm::vec3 emitter_position =
			work.cinematic_anchor_position
				+ work.cinematic_anchor_orientation
					* positions[emitter];
		glm::mat3 emitter_orientation =
			work.cinematic_anchor_orientation;
		static constexpr std::uint16_t parent_reference[6] = {
			2, 1, 1, 0, 0, 2,
		};
		if (const game::WorldObject* scene =
				launch_cinematic_object(world, work);
			scene != nullptr
			&& parent_reference[emitter]
				< scene->model_references.size())
		{
			const glm::mat4 parent = model_world_transform(
				*scene, parent_reference[emitter]);
			emitter_position = glm::vec3(
				parent * glm::vec4(positions[emitter], 1.0f));
			emitter_orientation = glm::mat3(parent);
		}
		// Particle_emitter_update (LANCER.EXE 0x0049c680) performs one
		// Bernoulli trial for every elapsed simulation tick.
		std::int32_t spawn_count = 0;
		for (std::uint32_t elapsed = 0;
			elapsed < runtime.frame_delta_ticks;
			++elapsed)
		{
			if (static_cast<float>(game::world_rand15(world))
					* (1.0f / 32767.0f)
				< 0.5f)
			{
				++spawn_count;
			}
		}
		const glm::vec3 camera_delta =
			emitter_position - runtime.particle_camera_position;
		if (glm::dot(
				camera_delta, runtime.particle_camera_forward) < 0.0f)
		{
			spawn_count /= 2;
		}
		const float distance = glm::length(camera_delta);
		const float attenuation = std::min(
			10500.0f / distance,
			1.0f);
		spawn_count = static_cast<std::int32_t>(
			std::lrint(static_cast<float>(spawn_count) * attenuation));
		for (mission::LaunchParticle& particle
			: runtime.launch_particles)
		{
			if (spawn_count < 1)
			{
				break;
			}
			if (particle.active
				&& particle.birth_tick + particle.lifetime_ticks
					> now_tick)
			{
				continue;
			}
			launch_spawn_particle(
				particle,
				world,
				emitter_position,
				emitter_orientation,
				directions[emitter],
				spreads[emitter],
				now_tick,
				runtime.frame_delta_ticks);
			--spawn_count;
		}
	}
}

void launch_append_external_trail(
	mission::Runtime& runtime,
	game::World& world,
	game::WorldObject& actor,
	std::uint32_t now_tick)
{
	mission::LaunchTrailRing& ring = runtime.launch_trail_rings[
		runtime.launch_trail_cursor
			% std::size(runtime.launch_trail_rings)];
	++runtime.launch_trail_cursor;
	const glm::vec3 local[4] = {
		{actor.bounds_max.x, actor.bounds_max.y, actor.bounds_min.z},
		{actor.bounds_min.x, actor.bounds_max.y, actor.bounds_min.z},
		{actor.bounds_min.x, actor.bounds_min.y, actor.bounds_min.z},
		{actor.bounds_max.x, actor.bounds_min.y, actor.bounds_min.z},
	};
	for (std::uint8_t point = 0; point < 4; ++point)
	{
		ring.points[point] =
			actor.position + actor.orientation * local[point];
	}
	ring.birth_tick = now_tick;
	ring.sequence = runtime.launch_trail_cursor;
	ring.owner = static_cast<std::uint16_t>(
		&actor - std::begin(world.objects));
	ring.generation = actor.generation;
	ring.active = true;
}

bool launch_begin(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats)
{
	LaunchWork& work = actor.ai.work.launch;
	if ((command.target_kind != TargetKind::object
			|| command.target_component == -1)
		&& !launch_resolve_automatic_target(
			command, world, runtime, work))
	{
		diagnostics::mission_log(
			"ai launch unresolved actor=%u target=%u sequence=%d",
			static_cast<unsigned>(actor.mission_index),
			static_cast<unsigned>(command.target),
			static_cast<int>(command.sequence));
		command_pop(world, actor);
		return true;
	}
	game::WorldObject* carrier =
		resolve_target(actor, command, world, runtime);
	if (carrier == nullptr)
	{
		diagnostics::mission_log(
			"ai launch missing carrier actor=%u target=%u",
			static_cast<unsigned>(actor.mission_index),
			static_cast<unsigned>(command.target));
		command_pop(world, actor);
		return true;
	}
	if (!actor.components_initialized || !carrier->components_initialized)
	{
		actor.ai.work.begin_pending = true;
		return true;
	}
	if (actor.type == 0x4a || actor.type == 0x5c)
	{
		work.strategy = 3;
	}
	else if (actor.type == 0x4d)
	{
		work.strategy = 4;
	}
	else if (actor.type == 0x90)
	{
		work.strategy = 7;
	}
	else
	{
		switch (carrier->type)
		{
		case 0x0c: work.strategy = 6; break;
		case 0x0d: work.strategy = 1; break;
		case 0x25: work.strategy = 5; break;
		case 0x37:
		case 0x9a: work.strategy = 2; break;
		case 0xa5:
			work.strategy =
				command.target_component <= 5 ? 8 : 0;
			break;
		case 0xb0: work.strategy = 9; break;
		case 0x11:
		case 0x12:
		case 0x13:
		case 0x14:
		case 0x34:
		case 0x38:
		case 0x47:
		case 0x78:
		case 0x9b:
		case 0x9c:
		case 0xa0:
		case 0xc2:
			work.strategy = 0;
			break;
		default:
			// AI_Launch_begin's retained
			// "Error. Trying to launch from %s." assertion is reachable
			// only outside the complete retail carrier table recovered
			// from 0x00418eb0: 0c,0d,11,12,13,14,25,34,37,38,47,
			// 78,9a,9b,9c,a0,a5,b0,c2 (actor rows take precedence).
			diagnostics::mission_log(
				"ai launch corrupt carrier actor=%u carrier=%u type=%u",
				static_cast<unsigned>(actor.mission_index),
				static_cast<unsigned>(carrier->mission_index),
				static_cast<unsigned>(carrier->type));
			command_pop(world, actor);
			return true;
		}
	}
	work.carrier_object = carrier->mission_index;
	if (actor.player)
	{
		mission::player_comms_retain_carrier(
			runtime,
			static_cast<std::uint16_t>(
				carrier - std::begin(world.objects)));
	}
	work.launch_point = command.target_component;
	work.stage = 0;
	actor.control_demand.throttle = 0.0f;
	actor.control_demand.roll = 0.0f;
	actor.control_demand.pitch = 0.0f;
	actor.control_demand.yaw = 0.0f;
	switch (work.strategy)
	{
	case 0:
		launch_place_at_locator(
			actor, *carrier, work, work.launch_point);
		work.animation_a = launch_strategy0_model(
			*carrier, work.launch_point, false);
		work.animation_b = launch_strategy0_model(
			*carrier, work.launch_point, true);
		break;
	case 1:
	{
		const std::uint16_t child_index =
			static_cast<std::uint16_t>(work.launch_point + 3);
		const std::uint16_t child =
			child_index < primary_model_count(*carrier)
				? child_index
				: UINT16_MAX;
		if (child != UINT16_MAX)
		{
			carrier->model_references[child].runtime_flags &= ~0x0020u;
			std::uint8_t enabled = 0;
			for (std::uint16_t reference = 0;
				reference < primary_model_count(*carrier);
				++reference)
			{
				game::ObjectModelReference& nested =
					carrier->model_references[reference];
				if (nested.parent_reference
						== static_cast<std::int16_t>(child)
					&& enabled < 2)
				{
					nested.render_flags |= 0x00000002u;
					if (enabled == 0)
					{
						work.animation_a = reference;
					}
					else
					{
						work.animation_b = reference;
					}
					++enabled;
				}
			}
		}
		glm::vec3 local_position{0.0f};
		if (child < primary_model_count(*carrier))
		{
			const game::ObjectModelReference& reference =
				carrier->model_references[child];
			local_position.x = work.launch_point < 8
				? reference.bounds_min.x
				: reference.bounds_max.x;
			local_position.y =
				(reference.bounds_min.y + reference.bounds_max.y)
				* 0.5f;
			local_position.z =
				(reference.bounds_min.z + reference.bounds_max.z)
				* 0.5f;
		}
		launch_place_at_side_bay(
			actor,
			*carrier,
			work,
			child,
			local_position,
			work.launch_point < 8
				? kLaunchPi * 0.5f
				: -kLaunchPi * 0.5f);
		// Retail retains the carrier root (+0x28), not the animated
		// point+3 model, as this strategy's launch attachment parent.
		work.launch_model = UINT16_MAX;
		if (child < primary_model_count(*carrier))
		{
			// Launch_strategy1_begin computes a second, distinct bay point
			// for yam_tube.shp at 0x004196c0..0x0041977f. Unlike the actor
			// placement above, its X coordinate is always bounds_max.x;
			// Y and Z remain centered. For points below eight the actor is
			// at bounds_min.x, so reusing actor.position displaces the entire
			// synthetic tube by the full bay width.
			const game::ObjectModelReference& reference =
				carrier->model_references[child];
			const glm::vec3 tube_local_anchor{
				reference.bounds_max.x,
				(reference.bounds_min.y + reference.bounds_max.y) * 0.5f,
				(reference.bounds_min.z + reference.bounds_max.z) * 0.5f,
			};
			work.cinematic_anchor_position = glm::vec3(
				model_world_transform(*carrier, child)
					* glm::vec4(tube_local_anchor, 1.0f));
		}
		else
		{
			work.cinematic_anchor_position = actor.position;
		}
		work.cinematic_anchor_orientation = actor.orientation;
		if (actor.player)
		{
			work.cinematic_active = true;
			if (launch_create_cinematic_object(
					world,
					stats,
					work,
					0xd4,
					{},
					glm::mat3{1.0f}) != nullptr)
			{
				// AI_Launch_strategy1_begin positions yam_tube.shp only after
				// GameObject_create has published its aggregate bounds. World
				// component initialization is deferred in this implementation,
				// so retain the exact placement operation for the first ready
				// update instead of leaving the tube at its creation origin.
				work.cinematic_placement_pending = true;
			}
			else
			{
				diagnostics::mission_log(
					"ai launch cinematic allocation failed actor=%u type=%u",
					static_cast<unsigned>(actor.mission_index),
					0xd4u);
			}
			work.emitter_next_tick[0] =
				actor.ai.work.entered_tick
					+ game::world_rand15(world) % 100u;
			work.emitter_next_tick[5] =
				actor.ai.work.entered_tick
					+ game::world_rand15(world) % 100u;
			launch_request_camera(runtime, world, actor, 0);
		}
		break;
	}
	case 2:
	{
		const std::uint16_t child =
			primary_model_count(*carrier) > 4
				? 4
				: UINT16_MAX;
		glm::vec3 offset{0.0f};
		if (child < primary_model_count(*carrier))
		{
			const game::ObjectModelReference& reference =
				carrier->model_references[child];
			const glm::vec3 extent =
				reference.bounds_max - reference.bounds_min;
			offset =
				(reference.bounds_max + reference.bounds_min)
				* 0.5f;
			if (work.launch_point < 5)
			{
				offset.x += extent.x * 0.5f;
				offset.z +=
					static_cast<float>(work.launch_point - 2)
						* extent.z * 0.2f;
			}
			else
			{
				offset.x -= extent.x * 0.5f;
				offset.z +=
					static_cast<float>(work.launch_point - 7)
						* extent.z * 0.2f;
			}
			offset.y += extent.y * 0.2f;
		}
		launch_place_at_sloped_deck(
			actor,
			*carrier,
			work,
			child,
			offset,
			work.launch_point < 5
				? kLaunchPi * 0.5f
				: -kLaunchPi * 0.5f,
			-0.377f);
		work.animation_a =
			primary_model_count(*carrier) > 1 ? 1 : UINT16_MAX;
		work.animation_b =
			primary_model_count(*carrier) > 2 ? 2 : UINT16_MAX;
		break;
	}
	case 3:
		actor.runtime_flags |= 0x00000004u;
		launch_place_at_locator(
			actor, *carrier, work, work.launch_point);
		break;
	case 4:
	case 5:
	case 7:
		launch_place_at_locator(
			actor, *carrier, work, work.launch_point);
		break;
	case 6:
	{
		const std::uint16_t first = static_cast<std::uint16_t>(
			std::max<std::int16_t>(work.launch_point, 0));
		const std::uint16_t second =
			static_cast<std::uint16_t>(first + 6);
		if (second < primary_model_count(*carrier))
		{
			const glm::mat4 first_frame =
				model_world_transform(*carrier, first);
			const glm::mat4 second_frame =
				model_world_transform(*carrier, second);
			glm::vec3 first_center =
				(carrier->model_references[first].bounds_min
					+ carrier->model_references[first].bounds_max)
				* 0.5f;
			glm::vec3 second_center =
				(carrier->model_references[second].bounds_min
					+ carrier->model_references[second].bounds_max)
				* 0.5f;
			const float side_offset =
				(work.launch_point & 1) == 0 ? 400.0f : -400.0f;
			first_center.x += side_offset;
			second_center.x += side_offset;
			const glm::vec3 first_position = glm::vec3(
				first_frame * glm::vec4(first_center, 1.0f));
			const glm::vec3 second_position = glm::vec3(
				second_frame * glm::vec4(second_center, 1.0f));
			actor.position =
				(first_position + second_position) * 0.5f;
			actor.orientation = carrier->orientation;
			actor.previous_position = actor.position;
			actor.previous_orientation = actor.orientation;
			actor.linear_velocity = {};
			actor.speed = 0.0f;
			work.launch_model = UINT16_MAX;
		}
		else if (launch_place_at_locator(
			actor, *carrier, work, work.launch_point))
		{
			work.launch_model = UINT16_MAX;
		}
		// Strategy six constructs its synthetic hangar at the origin first,
		// measures the selected locator's placed actor position, and then
		// translates the hangar so that locator lands on this world-space
		// midpoint (LANCER.EXE 0x0041b17a..0x0041b201).
		work.cinematic_anchor_position = actor.position;
		work.cinematic_anchor_orientation = carrier->orientation;
		if (actor.player)
		{
			work.cinematic_active = true;
			game::WorldObject* scene =
				launch_create_cinematic_object(
					world,
					stats,
					work,
					0xd6,
					{},
					glm::mat3{1.0f});
			if (scene != nullptr)
			{
				work.cinematic_placement_pending = true;
			}
			else
			{
				diagnostics::mission_log(
					"ai launch cinematic allocation failed actor=%u type=%u",
					static_cast<unsigned>(actor.mission_index),
					0xd6u);
			}
			launch_request_camera(runtime, world, actor, 0);
		}
		break;
	}
	case 8:
		launch_place_at_locator(
			actor, *carrier, work, work.launch_point);
		actor.position += actor.orientation * glm::vec3{0, 0, -500};
		actor.previous_position = actor.position;
		break;
	case 9:
		launch_place_at_locator(
			actor, *carrier, work, work.launch_point);
		actor.position += actor.orientation
			* glm::vec3{0, 0, actor.bounds_min.z};
		actor.previous_position = actor.position;
		break;
	default:
		break;
	}
	launch_capture_attachment(actor, *carrier, work);
	// AI_Launch_begin (LANCER.EXE 0x0041918a) stores
	// current simulation tick + 200 directly. It is a 100 Hz deadline,
	// not a millisecond deadline.
	work.deadline_tick = actor.ai.work.entered_tick + 200;
	actor.interaction_target_link = static_cast<std::uint16_t>(
		carrier - std::begin(world.objects));
	actor.targetable = false;
	actor.runtime_flags &= ~0x200u;
	diagnostics::mission_log(
		"ai launch prepared actor=%u carrier=%u strategy=%u point=%d",
		static_cast<unsigned>(actor.mission_index),
		static_cast<unsigned>(carrier->mission_index),
		static_cast<unsigned>(work.strategy),
		static_cast<int>(work.launch_point));
	return true;
}

bool launch_strategy0_update(
	game::WorldObject& actor,
	game::WorldObject& carrier,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	std::uint32_t now_tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	LaunchWork& work = actor.ai.work.launch;
	if (work.stage == 2 && work.deadline_tick < now_tick)
	{
		launch_start_model_animation(
			carrier, work.animation_a, 4.0f);
		launch_start_model_animation(
			carrier, work.animation_b, 4.0f);
		dock_queue_model_spatial(
			world, carrier, work.animation_a, 53, 5);
		work.stage = 3;
		work.deadline_tick = now_tick + 200;
	}
	else if (work.stage == 3 && work.deadline_tick < now_tick)
	{
		actor.linear_velocity = carrier.linear_velocity;
		actor.flight_callback_mode =
			game::FlightCallbackMode::linear_with_exhaust;
		work.launch_active = false;
		work.stage = 4;
		work.deadline_tick = now_tick
			+ ((carrier.type == 0x38 || carrier.type == 0x9b)
				? 400u
				: 200u);
	}
	else if (work.stage == 4)
	{
		demand.throttle = 2.0f;
		demand.linear_with_exhaust = true;
		if ((carrier.type == 0x38 || carrier.type == 0x9b)
			&& work.deadline_tick > now_tick
			&& work.deadline_tick - now_tick < 100)
		{
			demand.yaw = 0.1f;
		}
		if (work.deadline_tick < now_tick)
		{
			bool shared = false;
			for (const game::WorldObject& candidate : world.objects)
			{
				if (!candidate.active
					|| &candidate == &actor
					|| candidate.ai.command_count == 0
					|| candidate.ai.commands[0].id != 104
					|| candidate.ai.work.begin_pending)
				{
					continue;
				}
				const LaunchWork& peer = candidate.ai.work.launch;
				if (peer.carrier_object == work.carrier_object
					&& peer.animation_a == work.animation_a
					&& peer.animation_b == work.animation_b
					&& peer.stage > 0
					&& peer.stage < 5)
				{
					shared = true;
					break;
				}
			}
			if (!shared)
			{
				launch_start_model_animation(
					carrier, work.animation_a, -4.0f);
				launch_start_model_animation(
					carrier, work.animation_b, -4.0f);
				dock_queue_model_spatial(
					world, carrier, work.animation_a, 54, 5);
			}
			work.stage = 5;
			work.deadline_tick = now_tick + 300;
			demand.yaw = 0.0f;
		}
	}
	else if (work.stage == 5)
	{
		demand.throttle = 2.0f;
		demand.linear_with_exhaust = true;
		if (work.deadline_tick < now_tick)
		{
			launch_complete(
				world,
				actor, runtime, stats, true, kLaunchResetAll);
			applies_flight = false;
			return true;
		}
	}
	applies_flight = !work.launch_active;
	return true;
}

bool launch_strategy1_update(
	game::WorldObject& actor,
	game::WorldObject& carrier,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	std::uint32_t now_tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	LaunchWork& work = actor.ai.work.launch;
	if (actor.player
		&& work.stage < 6
		&& launch_cinematic_object_ready(world, work))
	{
		launch_service_emitters(actor, world, runtime, now_tick);
	}
	if (actor.player
		&& work.stage == 6
		&& work.cinematic_branch == 0
		&& work.deadline_tick > now_tick
		&& work.deadline_tick - now_tick < 50)
	{
		if (runtime.active_camera_mode != 15)
		{
			launch_request_camera(runtime, world, actor, 15);
		}
	}
	if (work.stage >= 5)
	{
		demand.throttle = 2.0f;
		demand.linear_with_exhaust = true;
	}
	if (work.deadline_tick >= now_tick)
	{
		applies_flight = !work.launch_active;
		return true;
	}
	if (actor.player
		&& work.stage >= 2
		&& work.stage <= 5
		&& !launch_cinematic_object_ready(world, work))
	{
		applies_flight = !work.launch_active;
		return true;
	}
	switch (work.stage)
	{
	case 2:
		actor.flight_callback_mode =
			game::FlightCallbackMode::linear_with_exhaust;
		work.launch_active = false;
		work.stage = 3;
		work.deadline_tick = now_tick + 100;
		if (actor.player)
		{
			world.player_camera_disturbance =
				std::max(world.player_camera_disturbance, 0.1f);
			for (std::uint8_t emitter = 1; emitter <= 4; ++emitter)
			{
				work.emitter_burst_active[emitter] = true;
				work.emitter_end_tick[emitter] =
					now_tick + 200;
				work.emitter_next_tick[emitter] =
					now_tick;
			}
		}
		break;
	case 3:
		if (actor.player)
		{
			if (game::WorldObject* scene =
					launch_cinematic_object(world, work);
				scene != nullptr)
			{
				game::model_animation_start_named(
					*scene, 0, "opendoor", 0.0f, -1, 4.0f);
				game::model_animation_start_named(
					*scene, 1, "opendoor", 0.0f, -1, 4.0f);
				dock_queue_model_spatial(
					world, *scene, 1, 53, 5);
			}
			dock_queue_object_spatial(world, actor, 5, 5);
			world.player_camera_disturbance =
				std::max(world.player_camera_disturbance, 0.2f);
		}
		work.stage = 4;
		work.deadline_tick = now_tick + 300;
		break;
	case 4:
		demand.throttle = 2.0f;
		demand.linear_with_exhaust = true;
		launch_start_model_animation(
			carrier, work.animation_a, 4.0f);
		launch_start_model_animation(
			carrier, work.animation_b, 4.0f);
		dock_queue_model_spatial(
			world, carrier, work.animation_a, 54, 5);
		if (actor.player)
		{
			world.player_camera_disturbance =
				std::max(world.player_camera_disturbance, 0.3f);
		}
		work.stage = 5;
		work.deadline_tick = now_tick + 50;
		break;
	case 5:
		if (actor.player)
		{
			launch_start_mission_title(runtime, now_tick);
			launch_destroy_cinematic_object(world, work);
			// Strategy-one state five positions retail's camera object from
			// the direct point+3 model before selecting modes 15..17
			// (LANCER.EXE 0x00419cec..0x00419dba). The hardware-renderer
			// branch uses bounds_min.z.
			const std::uint16_t reference =
				static_cast<std::uint16_t>(
					work.launch_point + 3);
			if (reference < primary_model_count(carrier))
			{
				const game::ObjectModelReference& model =
					carrier.model_references[reference];
				const glm::mat4 frame =
					model_world_transform(carrier, reference);
				const glm::vec3 local{
					work.launch_point < 8
						? model.bounds_max.x + 1000.0f
						: model.bounds_min.x - 1000.0f,
					model.bounds_min.y - 1000.0f,
					model.bounds_min.z,
				};
				work.cinematic_anchor_position = glm::vec3(
					frame * glm::vec4(local, 1.0f));
				work.cinematic_anchor_orientation =
					glm::mat3(frame);
			}
			work.cinematic_branch = static_cast<std::uint8_t>(
				game::world_rand15(world) % 3u);
			if (work.cinematic_branch != 0)
			{
				launch_request_camera(
					runtime,
					world,
					actor,
					static_cast<std::uint8_t>(
						15 + work.cinematic_branch));
			}
		}
		work.stage = 6;
		work.deadline_tick = now_tick + 150;
		break;
	case 6:
		work.stage = 7;
		work.deadline_tick = now_tick + 300;
		break;
	case 7:
	{
		const std::uint16_t child_index =
			static_cast<std::uint16_t>(work.launch_point + 3);
		const std::uint16_t child =
			child_index < primary_model_count(carrier)
				? child_index
				: UINT16_MAX;
		if (child != UINT16_MAX)
		{
			carrier.model_references[child].runtime_flags |= 0x0020u;
			for (game::ObjectModelReference& nested
				: carrier.model_references)
			{
				if (nested.parent_reference
					== static_cast<std::int16_t>(child))
				{
					nested.render_flags &= ~0x00000002u;
				}
			}
		}
		if (actor.player)
		{
			launch_destroy_cinematic_object(world, work);
			if (runtime.active_camera_mode >= 15
				&& runtime.active_camera_mode <= 17)
			{
				launch_request_camera(
					runtime, world, actor, 0, false);
			}
		}
		launch_complete(
			world,
			actor, runtime, stats, true, kLaunchResetAll);
		applies_flight = false;
		return true;
	}
	default:
		break;
	}
	applies_flight = !work.launch_active;
	return true;
}

bool launch_strategy2_update(
	game::WorldObject& actor,
	game::WorldObject& carrier,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	std::uint32_t now_tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	LaunchWork& work = actor.ai.work.launch;
	if (work.stage == 2)
	{
		if (work.animation_a >= primary_model_count(carrier)
			|| carrier.model_references[work.animation_a].sequence_mode == 0
			|| carrier.model_references[work.animation_a].sequence_rate
				== 0.0f)
		{
			game::model_animation_start_named(
				carrier,
				work.animation_a,
				"opendoor",
				0.0f,
				1,
				4.0f);
			game::model_animation_start_named(
				carrier,
				work.animation_b,
				"opendoor",
				0.0f,
				1,
				4.0f);
			dock_queue_model_spatial(
				world, carrier, work.animation_a, 53, 5);
		}
		work.stage = 3;
		work.deadline_tick =
			now_tick + 200u + game::world_object_rand15(actor) % 100u;
	}
	else if (work.stage == 3 && work.deadline_tick < now_tick)
	{
		actor.linear_velocity = carrier.linear_velocity;
		actor.flight_callback_mode =
			game::FlightCallbackMode::linear_with_exhaust;
		work.launch_active = false;
		work.stage = 4;
		work.deadline_tick = now_tick + 300;
	}
	else if (work.stage == 4)
	{
		demand.throttle = 2.0f;
		demand.linear_with_exhaust = true;
		float yaw =
			(static_cast<float>(work.launch_point % 5) - 2.5f)
				* 0.2f;
		if (work.launch_point < 5)
		{
			yaw = -yaw;
		}
		demand.yaw = yaw;
		if (work.deadline_tick < now_tick)
		{
			launch_complete(
				world,
				actor, runtime, stats, true, kLaunchResetAll);
			applies_flight = false;
			return true;
		}
	}
	applies_flight = !work.launch_active;
	return true;
}

bool launch_strategy_simple_update(
	game::WorldObject& actor,
	game::WorldObject& carrier,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	std::uint32_t now_tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	LaunchWork& work = actor.ai.work.launch;
	switch (work.strategy)
	{
	case 3:
		if (work.stage == 2)
		{
			actor.flight_callback_mode =
				game::FlightCallbackMode::linear_with_exhaust;
			work.launch_active = false;
			work.stage = 3;
			work.deadline_tick = now_tick + 200;
			dock_queue_object_spatial(world, actor, 24, 5);
			launch_append_external_trail(
				runtime, world, actor, now_tick);
			actor.external_trail_active = true;
		}
		demand.throttle = 2.0f;
		demand.linear_with_exhaust = true;
		if (work.stage == 3 && work.deadline_tick <= now_tick)
		{
			actor.runtime_flags &= ~0x00000004u;
			launch_complete(world, actor, runtime, stats, false, 0);
			applies_flight = false;
			return true;
		}
		break;
	case 4:
		if (work.stage == 2)
		{
			actor.flight_callback_mode =
				game::FlightCallbackMode::linear_with_exhaust;
			work.launch_active = false;
			work.stage = 3;
			work.deadline_tick = now_tick + 200;
			work.flight_scalar =
				static_cast<float>(game::world_rand15(world))
					* (1.0f / 32767.0f);
			dock_queue_object_spatial(world, actor, 51, 5);
		}
		demand.throttle =
			2.0f
			+ 0.5f * work.flight_scalar;
		demand.linear_with_exhaust = true;
		demand.yaw = work.launch_point < 7
			? static_cast<float>(work.launch_point - 3) / 12.0f
			: static_cast<float>(work.launch_point - 7) / 16.0f;
		if (work.stage == 3 && work.deadline_tick < now_tick)
		{
			launch_complete(
				world,
				actor,
				runtime,
				stats,
				false,
				kLaunchResetThrottle);
			applies_flight = false;
			return true;
		}
		break;
	case 5:
		if (work.stage == 2)
		{
			actor.flight_callback_mode =
				game::FlightCallbackMode::linear_with_exhaust;
			work.launch_active = false;
			work.stage = 3;
			work.deadline_tick = now_tick + 400;
		}
		if (work.stage == 3)
		{
			demand.throttle = 2.0f;
			demand.linear_with_exhaust = true;
			if (work.deadline_tick < now_tick)
			{
				demand.throttle = 0.0f;
				demand.linear_with_exhaust = false;
				work.stage = 4;
				work.deadline_tick = now_tick + 200;
			}
		}
		else if (work.stage == 4)
		{
			demand.throttle = 0.0f;
			demand.linear_with_exhaust = true;
			if (work.deadline_tick >= now_tick)
			{
				break;
			}
			for (std::uint16_t reference = 0;
				reference < primary_model_count(carrier);
				++reference)
			{
				launch_start_model_animation(
					carrier, reference, 4.0f);
			}
			launch_complete(world, actor, runtime, stats, true, 0);
			applies_flight = false;
			return true;
		}
		break;
	case 7:
		if (work.stage == 2)
		{
			dock_queue_object_spatial(world, actor, 51, 5);
			work.stage = 3;
		}
		else if (work.stage == 3)
		{
			actor.flight_callback_mode =
				game::FlightCallbackMode::linear_with_exhaust;
			work.launch_active = false;
			work.stage = 4;
			work.deadline_tick = now_tick + 200;
		}
		if (work.stage == 4)
		{
			demand.throttle = 2.0f;
			demand.linear_with_exhaust = true;
			if (work.deadline_tick < now_tick)
			{
				launch_complete(
					world,
					actor,
					runtime,
					stats,
					false,
					kLaunchResetThrottle | kLaunchResetYaw);
				applies_flight = false;
				return true;
			}
		}
		break;
	case 8:
		if (work.stage == 2)
		{
			actor.flight_callback_mode =
				game::FlightCallbackMode::linear_no_exhaust;
			work.launch_active = false;
			work.stage = 3;
			work.deadline_tick = now_tick + 100;
		}
		demand.throttle = -2.0f;
		demand.linear_no_exhaust = true;
		if (work.stage == 3 && work.deadline_tick < now_tick)
		{
			launch_complete(
				world,
				actor,
				runtime,
				stats,
				true,
				kLaunchResetThrottle | kLaunchResetYaw);
			applies_flight = false;
			return true;
		}
		break;
	case 9:
		if (work.stage == 2)
		{
			actor.flight_callback_mode =
				game::FlightCallbackMode::linear_with_exhaust;
			work.launch_active = false;
			work.stage = 3;
			work.deadline_tick = now_tick + 100;
		}
		demand.throttle = 2.0f;
		demand.linear_with_exhaust = true;
		if (work.stage == 3 && work.deadline_tick < now_tick)
		{
			launch_complete(
				world,
				actor,
				runtime,
				stats,
				false,
				kLaunchResetThrottle | kLaunchResetYaw);
			applies_flight = false;
			return true;
		}
		break;
	default:
		break;
	}
	applies_flight = !work.launch_active;
	return true;
}

bool launch_strategy6_update(
	game::WorldObject& actor,
	game::WorldObject& carrier,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	std::uint32_t now_tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	constexpr std::uint16_t kRetainerReference = 3;
	LaunchWork& work = actor.ai.work.launch;
	launch_service_cinematic_light_masks(world, work);
	if (work.alternate_flight)
	{
		demand.throttle = 1.0f;
		demand.linear_no_exhaust = true;
	}
	if (work.deadline_tick >= now_tick)
	{
		applies_flight = !work.launch_active;
		return true;
	}
	if (actor.player
		&& work.stage >= 2
		&& work.stage <= 4
		&& !launch_cinematic_object_ready(world, work))
	{
		applies_flight = !work.launch_active;
		return true;
	}
	switch (work.stage)
	{
	case 2:
		work.stage = 3;
		work.deadline_tick = now_tick + 100;
		if (actor.player)
		{
			dock_queue_object_spatial(world, actor, 5, 5);
			world.player_camera_disturbance =
				std::max(world.player_camera_disturbance, 0.1f);
			work.cinematic_branch = static_cast<std::uint8_t>(
				game::world_rand15(world) % 3u + 1u);
			if (work.cinematic_branch == 1)
			{
				launch_request_camera(runtime, world, actor, 32);
			}
		}
		{
			const std::uint16_t reference =
				static_cast<std::uint16_t>(
					std::max<std::int16_t>(
						work.launch_point, 0)
					+ 6);
			game::model_animation_start_named(
				carrier,
				reference,
				"opendoor",
				0.0f,
				3,
				-1.0f);
			if (reference < primary_model_count(carrier))
			{
				carrier.model_references[
					reference].runtime_flags &= ~0x0020u;
			}
		}
		break;
	case 3:
		if (actor.player)
		{
			if (game::WorldObject* scene =
					launch_cinematic_object(world, work);
				scene != nullptr
					&& scene->model_references.size()
						> kRetainerReference)
			{
				game::model_animation_start_named(
					*scene,
					kRetainerReference,
					"deploy",
					0.0f,
					-1,
					2.0f);
			}
			// Retail passes sample 6 in EDX and 0x7f as the volume
			// argument to sound_2d_play (LANCER.EXE
			// 0x0041b35c..0x0041b36f).
			dock_queue_2d(runtime, 6);
		}
		work.stage = 4;
		work.deadline_tick = now_tick + 250;
		break;
	case 4:
		if (actor.player)
		{
			if (game::WorldObject* scene =
					launch_cinematic_object(world, work);
				scene != nullptr
					&& scene->model_references.size()
						> kRetainerReference)
			{
				game::model_animation_start_named(
					*scene,
					kRetainerReference,
					"deploy",
					scene->model_references[
						kRetainerReference].sequence_time,
					-1,
					-2.0f);
			}
		}
		work.launch_active = false;
		work.stage = 5;
		work.deadline_tick = now_tick + 50;
		break;
	case 5:
		game::model_animation_start_named(
			carrier,
			static_cast<std::uint16_t>(
				std::max<std::int16_t>(
					work.launch_point, 0)),
			"opendoor",
			0.0f,
			-1,
			2.0f);
		if (actor.player)
		{
			// Reliant launch state five opens the separately instantiated
			// hangar's model reference two as well as the matching door on
			// the live carrier (LANCER.EXE 0x0041b42e..0x0041b44c).
			// This panel starts in its authored closed pose; omitting this
			// call leaves it across the player's view inside the bay.
			if (game::WorldObject* scene =
					launch_cinematic_object(world, work);
				scene != nullptr && scene->model_references.size() > 2)
			{
				game::model_animation_start_named(
					*scene,
					2,
					"opendoor",
					0.0f,
					-1,
					4.0f);
			}
		}
		if (actor.player && work.cinematic_branch == 3)
		{
			launch_request_camera(runtime, world, actor, 34);
			launch_destroy_cinematic_object(world, work);
		}
		if (actor.player)
		{
			// The second cinematic cue is sample 5; as above, the pushed
			// 0x7f is its volume rather than its sample index
			// (LANCER.EXE 0x0041b451..0x0041b464).
			dock_queue_2d(runtime, 5);
		}
		work.stage = 6;
		work.deadline_tick = now_tick + 150;
		break;
	case 6:
		work.alternate_flight = true;
		actor.flight_callback_mode =
			game::FlightCallbackMode::linear_no_exhaust;
		demand.throttle = 1.0f;
		demand.linear_no_exhaust = true;
		work.stage = 7;
		work.deadline_tick = now_tick + 50;
		if (actor.player)
		{
			launch_start_mission_title(runtime, now_tick);
		}
		break;
	case 7:
		if (actor.player)
		{
			if (runtime.active_camera_mode != 32)
			{
				launch_destroy_cinematic_object(world, work);
			}
			if (work.cinematic_branch == 2)
			{
				launch_request_camera(runtime, world, actor, 33);
			}
		}
		work.stage = 8;
		work.deadline_tick = now_tick + 150;
		break;
	case 8:
		work.stage = 9;
		work.deadline_tick = now_tick + 300;
		break;
	case 9:
		work.alternate_flight = false;
		actor.flight_callback_mode =
			game::FlightCallbackMode::standard_forward;
		demand.throttle = 0.0f;
		demand.roll = 0.0f;
		demand.pitch = 0.0f;
		demand.yaw = 0.0f;
		actor.control_demand.throttle = 0.0f;
		actor.control_demand.roll = 0.0f;
		actor.control_demand.pitch = 0.0f;
		actor.control_demand.yaw = 0.0f;
		work.stage = 10;
		work.deadline_tick =
			actor.player ? now_tick : now_tick + 200;
		break;
	case 10:
		if (actor.player)
		{
			launch_destroy_cinematic_object(world, work);
			if (runtime.active_camera_mode >= 32
				&& runtime.active_camera_mode <= 34)
			{
				launch_request_camera(
					runtime, world, actor, 0, false);
			}
		}
		launch_complete(
			world,
			actor, runtime, stats, true, kLaunchResetAll);
		applies_flight = false;
		return true;
	default:
		break;
	}
	applies_flight = !work.launch_active;
	return true;
}

bool launch_update(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	LaunchWork& work = actor.ai.work.launch;
	game::WorldObject* carrier =
		mission::runtime_resolve_object(
			runtime, work.carrier_object, world);
	if (work.stage < 2)
	{
		if (carrier == nullptr)
		{
			diagnostics::mission_log(
				"ai launch crash actor=%u carrier=%u",
				static_cast<unsigned>(actor.mission_index),
				static_cast<unsigned>(work.carrier_object));
			command_push(world,
				actor, 11, TargetKind::none, UINT16_MAX);
			applies_flight = false;
			return true;
		}
		if ((carrier->type == 1001
				|| (carrier->runtime_flags & game::kObjectFlagDestroyed) != 0)
			&& !(carrier->type == 0x16 && actor.type == 0x4d))
		{
			// AI_Launch_update, LANCER.EXE
			// 0x00419206..0x004192b1, routes a lost carrier through
			// the complete death selector. The Boridin type-0x4d/type-
			// 0x16 pair is the sole authored exception.
			(void)schedule_death_command(
				actor, world, runtime, 0, false);
			applies_flight = false;
			return true;
		}
	}
	if (carrier == nullptr)
	{
		command_push(world, actor, 11, TargetKind::none, UINT16_MAX);
		applies_flight = false;
		return true;
	}
	// GameObject_create is synchronous in retail, so both synthetic launch
	// scenes are already positioned while the command is still waiting in
	// state zero. This renderer initializes the model tree after AI service;
	// retry placement here before the state-zero/one early return so the
	// initial cinematic frame contains the authored hangar geometry.
	launch_service_cinematic_placement(actor, world, work);
	launch_service_cinematic_light_masks(world, work);
	// AI_Launch_update reads only command-record byte +0x0a. The upper
	// three bytes share state[0] in this aligned representation and may
	// retain replay/deferred payload, but they do not signal release.
	if (work.stage == 0 && (command.state[0] & 0xffu) != 0)
	{
		work.stage = 1;
		work.deadline_tick =
			tick + game::world_object_rand15(actor) % 200u;
		if (actor.player)
		{
			mission::player_comms_on_player_launch(
				runtime,
				world,
				static_cast<std::uint16_t>(
					carrier - std::begin(world.objects)),
				tick);
		}
		diagnostics::mission_log(
			"ai launch released actor=%u delay_ticks=%u",
			static_cast<unsigned>(actor.mission_index),
			work.deadline_tick - tick);
	}
	if (work.stage == 1 && work.deadline_tick < tick)
	{
		work.stage = 2;
	}
	const game::WorldObject* attachment_parent = carrier;
	if (work.cinematic_attachment)
	{
		if (const game::WorldObject* scene =
				launch_cinematic_object(world, work);
			scene != nullptr)
		{
			attachment_parent = scene;
		}
	}
	launch_service_attachment(actor, *attachment_parent, world, work);
	if (work.stage < 2)
	{
		// AI_Launch_strategy1_update is still dispatched for common waiting
		// states zero and one. Its state switch does nothing there, but the
		// trailing six-emitter service at 0x00419a7d..0x00419b69 remains
		// active. Preserve that initial-scene service without dispatching the
		// other strategies early (their portable state handlers assume release).
		if (work.strategy == 1
			&& actor.player
			&& launch_cinematic_object_ready(world, work))
		{
			launch_service_emitters(actor, world, runtime, tick);
		}
		applies_flight = false;
		return true;
	}
	switch (work.strategy)
	{
	case 0:
		return launch_strategy0_update(
			actor,
			*carrier,
			world,
			runtime,
			stats,
			tick,
			demand,
			applies_flight);
	case 1:
		return launch_strategy1_update(
			actor,
			*carrier,
			world,
			runtime,
			stats,
			tick,
			demand,
			applies_flight);
	case 2:
		return launch_strategy2_update(
			actor,
			*carrier,
			world,
			runtime,
			stats,
			tick,
			demand,
			applies_flight);
	case 6:
		return launch_strategy6_update(
			actor,
			*carrier,
			world,
			runtime,
			stats,
			tick,
			demand,
			applies_flight);
	default:
		return launch_strategy_simple_update(
			actor,
			*carrier,
			world,
			runtime,
			stats,
			tick,
			demand,
			applies_flight);
	}
}

void land_freeze_world(
	game::World& world,
	const game::WorldObject& actor,
	const game::WorldObject* paired)
{
	world.cinematic_mode = 3;
	// AI_Land's loop covers the ordinary [0, DAT_00539aa0) object range.
	// Reserved transition object DAT_0057e04e (slot 399) is deliberately
	// outside it and must remain serviced for its hangar animation.
	for (std::uint16_t index = 0;
		index + 1u < game::kMaxGameObjects;
		++index)
	{
		game::WorldObject& object = world.objects[index];
		if (!object.active)
		{
			continue;
		}
		if (&object == &actor)
		{
			// Type 0x0c explicitly clears both exemptions. Type 0x0d's
			// local-player loop only skips the player comparison.
			if (paired != nullptr)
			{
				object.runtime_flags &= ~game::kObjectFlagDisabled;
			}
			continue;
		}
		if (&object == paired)
		{
			object.runtime_flags &= ~game::kObjectFlagDisabled;
			continue;
		}
		object.runtime_flags |= game::kObjectFlagDisabled;
	}
}

bool land_begin(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick)
{
	game::WorldObject* target =
		resolve_target(actor, command, world, runtime);
	if (target == nullptr
		|| (target->type != 0x0d && target->type != 0x0c))
	{
		diagnostics::mission_log(
			"ai land invalid actor=%u target=%u type=%u",
			static_cast<unsigned>(actor.mission_index),
			static_cast<unsigned>(command.target),
			target == nullptr
				? std::numeric_limits<unsigned>::max()
				: static_cast<unsigned>(target->type));
		command_pop(world, actor);
		return true;
	}
	if (!actor.components_initialized
		|| !target->components_initialized)
	{
		actor.ai.work.begin_pending = true;
		return true;
	}
	LandWork& work = actor.ai.work.land;
	work.mode = target->type == 0x0d ? 0 : 1;
	work.target_world_index = static_cast<std::uint16_t>(
		target - std::begin(world.objects));
	if (work.mode == 0 && actor.player)
	{
		// AI_Land_type13_begin creates yamhanger.shp as type 0xd5 in the
		// executable's reserved DAT_0057e04e slot, then marks it with object
		// flag four. Every later type-13 landing state uses this scene object,
		// not the carrier supplied by command 8.
		constexpr std::uint16_t kLandingScene =
			static_cast<std::uint16_t>(game::kMaxGameObjects - 1u);
		const game::ObjectHandle scene = game::world_create_at(
			world,
			kLandingScene,
			0xd5,
			{},
			glm::mat3{1.0f},
			stats,
			false);
		if (game::WorldObject* landing_scene =
				game::world_resolve(world, scene);
			landing_scene != nullptr)
		{
			landing_scene->runtime_flags |= 0x00000004u;
		}
		work.target_world_index = kLandingScene;
	}
	work.deadline =
		work.mode == 1
			&& (runtime.game_mode == 6
				|| runtime.game_mode == 7)
			? tick
			: tick + 700;
	work.stage = 0;
	work.prepared = false;
	return true;
}

bool land_prepare_type13(
	game::WorldObject& actor,
	game::WorldObject& target,
	game::World& world,
	mission::Runtime& runtime,
	LandWork& work)
{
	// AI_Land_prepare_type13_local_player first isolates yamhanger.shp at
	// the retail transition coordinates. Both setters publish every retained
	// pose snapshot, so neither the renderer nor the camera interpolates from
	// its creation pose at the origin.
	set_all_orientation_states(target, glm::mat3{1.0f});
	set_all_position_states(target, {0.0f, -1000000.0f, 0.0f});
	// AI_Land_prepare_type13 reads fixed pointer-array entry six from the
	// reserved yamhanger.shp transition object.
	const std::uint16_t child = primary_model_reference(target, 6);
	if (child == UINT16_MAX)
	{
		return false;
	}
	// SR render-object +0xdc on fixed model entry six.
	target.model_references[child].light_exclusion_mask = 0x3bu;
	runtime.requested_camera_mode =
		(game::world_rand15(world) & 1u) == 0 ? 14 : 37;
	runtime.requested_camera_target = static_cast<std::uint16_t>(
		&actor - std::begin(world.objects));
	runtime.requested_camera_lock = true;
	runtime.requested_camera_override_lock = true;
	mission::runtime_publish_camera_request(runtime);
	const game::ObjectModelReference& model =
		target.model_references[child];
	const glm::mat4 frame = model_world_transform(target, child);
	const glm::vec3 aim = glm::vec3(
		frame
			* glm::vec4(
				(model.bounds_min + model.bounds_max) * 0.5f,
				1.0f));
	const glm::vec3 local_position{
		target.bounds_min.x * 0.8f + target.bounds_max.x * 0.2f,
		target.bounds_min.y * 0.8f + target.bounds_max.y * 0.2f,
		target.bounds_min.z * 0.7f + target.bounds_max.z * 0.3f,
	};
	const glm::vec3 position =
		target.position + target.orientation * local_position;
	set_all_position_states(actor, position);
	set_all_orientation_states(actor, look_at(position, aim));
	dock_stop_object(actor);
	// AI_Land_prepare_type13 writes the retained request at +0x5b8 after
	// AI_stop_object; it does not write the derived throttle at +0x5d8.
	actor.control_demand.throttle = 0.5f;
	work.transition_model = child;
	work.prepared = true;
	return true;
}

bool land_type13_update(
	game::WorldObject& actor,
	game::WorldObject& target,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	const game::FlightDemand& player_demand,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	LandWork& work = actor.ai.work.land;
	if (!actor.player)
	{
		// AI_Land_type13_update's remote-player path participates in the
		// multiplayer terminal handoff before it pops command 8. It applies
		// only to the reserved player prefix, only with networking active,
		// and only when the local player is already departed/destroyed under
		// the executable's exact 0x10000840 state mask.
		const std::uint16_t actor_index =
			static_cast<std::uint16_t>(
				&actor - std::begin(world.objects));
		const game::WorldObject* local_player =
			game::world_resolve(world, world.player);
		if (actor_index < runtime.player_prefix_count
			&& runtime.network.role
				!= mission::NetworkRole::offline
			&& local_player != nullptr
			&& (local_player->runtime_flags & 0x10000840u) != 0)
		{
			// Retail owns one shared transition latch. This port splits it by
			// source so the surrounding camera/result owner can retain the
			// correct path; this branch is explicitly the dead-local fallback.
			runtime.player_death_transition_complete = true;
		}
		command_pop(world, actor);
		return true;
	}
	if (work.stage == 0)
	{
		demand = player_demand;
		if (work.deadline <= tick)
		{
			if (!land_prepare_type13(
				actor, target, world, runtime, work))
			{
				command_pop(world, actor);
				return true;
			}
			work.deadline = tick + 600;
			actor.flight_callback_mode =
				game::FlightCallbackMode::linear_with_exhaust;
			actor.runtime_flags |= 0x00000004u;
			++work.stage;
			applies_flight = false;
		}
		return true;
	}
	if (work.stage == 1)
	{
		// The ordinary object range freezes except for the player. The
		// separately serviced reserved hangar slot lies beyond that range.
		land_freeze_world(world, actor, nullptr);
		const std::uint16_t child = primary_model_reference(target, 6);
		if (child == UINT16_MAX)
		{
			command_pop(world, actor);
			return true;
		}
		const glm::mat4 frame = model_world_transform(target, child);
		const game::ObjectModelReference& model =
			target.model_references[child];
		glm::vec3 local =
			(model.bounds_min + model.bounds_max) * 0.5f;
		local.y -= 1000.0f;
		const glm::vec3 point = glm::vec3(
			frame * glm::vec4(local, 1.0f));
		demand = steer_toward(
			actor,
			stats,
			point,
			1.0f,
			runtime.frame_delta_ticks);
		demand.throttle =
			100.0f
			/ std::max(
				game::world_effective_max_speed(
					actor, stats, world.camera_mode),
				1.0f);
		demand.linear_with_exhaust = true;
		if (glm::dot(point - actor.position, point - actor.position)
			< 4000000.0f)
		{
			++work.stage;
		}
		return true;
	}
	if (work.stage == 2)
	{
		const std::uint16_t child = primary_model_reference(target, 1);
		if (child == UINT16_MAX)
		{
			command_pop(world, actor);
			return true;
		}
		const glm::mat4 frame = model_world_transform(target, child);
		const game::ObjectModelReference& model =
			target.model_references[child];
		const glm::vec3 local =
			(model.bounds_min + model.bounds_max) * 0.5f;
		glm::vec3 point =
			glm::vec3(frame * glm::vec4(local, 1.0f));
		point.y += actor.bounds_min.y;
		point.z += 500.0f;
		const float distance = glm::distance(actor.position, point);
		// AI_Land_type13 state two takes the forward column from the floor
		// model's concatenated matrix, scales it by 500, and uses that offset
		// point for steering. The unshifted deck point remains the distance
		// and transition reference.
		const glm::vec3 steering_point =
			point + glm::mat3(frame)[2] * 500.0f;
		demand = steer_toward(
			actor,
			stats,
			steering_point,
			1.0f,
			runtime.frame_delta_ticks);
		demand.throttle = std::min(
			distance * 0.0001f - 0.02f,
			100.0f
				/ std::max(
					game::world_effective_max_speed(
						actor, stats, world.camera_mode),
					1.0f));
		demand.linear_with_exhaust = true;
		if (distance < 500.0f)
		{
			work.deck_position = point;
			work.deck_orientation = glm::mat3(frame);
			++work.stage;
		}
		return true;
	}
	if (work.stage == 3)
	{
		const glm::vec3 error = math::rotation_to_euler(
			glm::transpose(actor.orientation)
				* work.deck_orientation);
		demand.pitch = std::clamp(
			(error.x - actor.angular_x * 6.0f) * 11.4591551f,
			-1.0f,
			1.0f);
		demand.yaw = std::clamp(
			(error.y - actor.angular_y * 6.0f) * 11.4591551f,
			-1.0f,
			1.0f);
		demand.roll = 0.0f;
		demand.throttle = 0.0f;
		demand.linear_with_exhaust = true;
		if (std::abs(demand.pitch) < 0.05f
			&& std::abs(demand.yaw) < 0.05f)
		{
			const std::uint16_t floor_model =
				primary_model_reference(target, 1);
			const glm::mat4 floor_frame =
				model_world_transform(target, floor_model);
			const glm::mat4 actor_frame =
				object_world_transform(actor);
			// Retail converts the actor's pose at the instant alignment
			// succeeds into floor-node-local space and parents it there. It
			// does not snap to the earlier 500-unit approach point.
			const glm::mat4 relative =
				glm::inverse(floor_frame) * actor_frame;
			work.attachment_local_position =
				glm::vec3(relative[3]);
			work.attachment_local_orientation =
				glm::mat3(relative);
			const glm::mat4 attached =
				floor_frame * relative;
			set_all_position_states(
				actor, glm::vec3(attached[3]));
			set_all_orientation_states(
				actor, glm::mat3(attached));
			actor.linear_velocity = {};
			actor.angular_x = 0.0f;
			actor.angular_y = 0.0f;
			actor.angular_z = 0.0f;
			work.attachment_deadline = tick + 200;
			++work.stage;
		}
		return true;
	}
	demand = {};
	applies_flight = false;
	if (work.stage >= 4)
	{
		// States four and five write GameObject+0x5b8..+0x5c4 directly.
		// The shared AI bridge intentionally does not publish `demand` when
		// ordinary motion is disabled, so retain those four zeroes here.
		actor.control_demand = {};
		const std::uint16_t floor_model =
			primary_model_reference(target, 1);
		if (floor_model != UINT16_MAX)
		{
			const glm::mat4 attached =
				model_world_transform(target, floor_model)
				* glm::translate(
					glm::mat4{1.0f},
					work.attachment_local_position)
				* glm::mat4{
					work.attachment_local_orientation};
			actor.previous_position = actor.position;
			actor.previous_orientation = actor.orientation;
			actor.position = glm::vec3(attached[3]);
			actor.orientation = glm::mat3(attached);
			actor.linear_velocity =
				actor.position - actor.previous_position;
			actor.speed = glm::length(actor.linear_velocity);
			game::world_set_scene_attachment(
				world,
				actor,
				target,
				static_cast<std::int16_t>(floor_model),
				work.attachment_local_position,
				work.attachment_local_orientation);
		}
	}
	if (work.stage == 4 && work.attachment_deadline < tick)
	{
		++work.stage;
	}
	if (work.stage == 5)
	{
		const std::uint16_t floor_model =
			primary_model_reference(target, 1);
		game::model_animation_start_named(
			target,
			floor_model,
			"floor",
			0.0f,
			-1,
			1.0f);
		dock_queue_object_spatial(world, actor, 62, 0);
		// AI_Land_type13 state five retains the 400-tick hold at work +0x08.
		// Work +0x04 remains the mode-14 camera timestamp established by
		// state zero.
		work.attachment_deadline = tick + 400;
		++work.stage;
	}
	else if (work.stage == 6 && work.attachment_deadline < tick)
	{
		runtime.landing_transition_complete = true;
	}
	return true;
}

bool land_prepare_type12(
	game::WorldObject& actor,
	game::WorldObject& target,
	game::World& world,
	mission::Runtime& runtime,
	LandWork& work)
{
	// AI_Land_prepare_type12_pair calls CRT rand before it mutates either
	// object. Zero selects camera 38; odd selects camera 35.
	const std::uint8_t camera_mode =
		(game::world_rand15(world) & 1u) == 0 ? 38 : 35;
	runtime.requested_camera_mode = camera_mode;
	runtime.requested_camera_target = static_cast<std::uint16_t>(
		&actor - std::begin(world.objects));
	runtime.requested_camera_lock = true;
	runtime.requested_camera_override_lock = true;
	mission::runtime_publish_camera_request(runtime);
	// AI_Land_prepare_type12_pair moves the paired type-0x0c object into the
	// same isolated retail landing space before deriving either bay model's
	// frame. Leaving it at its mission pose makes the cinematic cross the
	// visible carrier hull and allows unrelated mission-space geometry into
	// the shot.
	set_all_orientation_states(target, glm::mat3{1.0f});
	set_all_position_states(target, {0.0f, -1000000.0f, 0.0f});
	const std::uint16_t first = primary_model_reference(target, 0);
	const std::uint16_t second = primary_model_reference(target, 6);
	if (first == UINT16_MAX || second == UINT16_MAX)
	{
		return false;
	}
	const glm::mat4 first_frame =
		model_world_transform(target, first);
	const glm::mat4 second_frame =
		model_world_transform(target, second);
	const game::ObjectModelReference& first_model =
		target.model_references[first];
	const game::ObjectModelReference& second_model =
		target.model_references[second];
	const glm::vec3 first_center = glm::vec3(
		first_frame
			* glm::vec4(
				(first_model.bounds_min + first_model.bounds_max)
					* 0.5f,
				1.0f));
	const glm::vec3 second_center = glm::vec3(
		second_frame
			* glm::vec4(
				(second_model.bounds_min + second_model.bounds_max)
					* 0.5f,
				1.0f));
	const glm::vec3 midpoint =
		(first_center + second_center) * 0.5f;
	work.bay_axis = midpoint;
	work.deck_position =
		midpoint
			+ target.orientation
				* glm::vec3{0.0f, -5000.0f, 20000.0f};
	const glm::vec3 aim =
		midpoint
			+ target.orientation
				* glm::vec3{0.0f, -3000.0f, 0.0f};
	work.deck_orientation = look_at(work.deck_position, aim);
	set_all_position_states(actor, work.deck_position);
	set_all_orientation_states(actor, work.deck_orientation);
	dock_stop_object(actor);
	command_clear(world, target);
	dock_stop_object(target);
	work.prepared = true;
	return true;
}

bool land_type12_update(
	game::WorldObject& actor,
	game::WorldObject& target,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	const game::FlightDemand& player_demand,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	LandWork& work = actor.ai.work.land;
	if (work.stage == 0)
	{
		demand = actor.player ? player_demand : game::FlightDemand{};
		if (work.deadline <= tick)
		{
			if (!land_prepare_type12(
				actor, target, world, runtime, work))
			{
				command_pop(world, actor);
				return true;
			}
			land_freeze_world(world, actor, &target);
			actor.flight_callback_mode =
				game::FlightCallbackMode::linear_with_exhaust;
			actor.runtime_flags |= 0x00000004u;
			const std::uint16_t door =
				primary_model_reference(target, 6);
			dock_queue_model_spatial(
				world, target, door, 53, 4);
			if (door != UINT16_MAX)
			{
				game::model_animation_start_named(
					target,
					door,
					"opendoor2",
					0.0f,
					-1,
					1.23f);
			}
			++work.stage;
			applies_flight = false;
		}
		return true;
	}
	if (work.stage == 1)
	{
		const glm::vec3 point =
			work.bay_axis
				+ target.orientation
					* glm::vec3{0.0f, -3000.0f, 0.0f};
		const float distance = glm::distance(actor.position, point);
		demand = steer_toward(
			actor,
			stats,
			point,
			std::min(0.5f, distance * 0.0001f),
			runtime.frame_delta_ticks);
		demand.linear_with_exhaust = true;
		if (distance < 200.0f)
		{
			demand = {};
			demand.linear_with_exhaust = true;
			work.deadline = tick + 50;
			++work.stage;
		}
		return true;
	}
	if (work.stage == 2)
	{
		demand = {};
		demand.linear_with_exhaust = true;
		if (work.deadline < tick)
		{
			actor.flight_callback_mode =
				game::FlightCallbackMode::linear_no_exhaust;
			demand.linear_with_exhaust = false;
			demand.linear_no_exhaust = true;
			++work.stage;
		}
		return true;
	}
	if (work.stage == 3)
	{
		// Vector_subtract at 0x0040fba5 forms actor - bay axis. After the
		// inverse target transform, 0x0040fbe4 reads the second component:
		// the retail completion gate is local y > -100, not local z. The
		// wrong component leaves the ship descending through the bay forever.
		const glm::vec3 to_bay =
			actor.position - work.bay_axis;
		const glm::vec3 local =
			glm::transpose(target.orientation) * to_bay;
		demand.throttle = glm::length(local) / 15000.0f;
		demand.linear_no_exhaust = true;
		if (local.y > -100.0f)
		{
			demand = {};
			demand.linear_no_exhaust = true;
			dock_queue_object_spatial(world, actor, 62, 0);
			work.deadline = tick + 270;
			++work.stage;
		}
		return true;
	}
	demand = {};
	applies_flight = false;
	if (work.deadline < tick)
	{
		runtime.landing_transition_complete = true;
	}
	return true;
}

bool land_update(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	const game::FlightDemand& player_demand,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	LandWork& work = actor.ai.work.land;
	game::WorldObject* target =
		work.target_world_index < std::size(world.objects)
			? &world.objects[work.target_world_index]
			: nullptr;
	if (target != nullptr && !target->active)
	{
		target = nullptr;
	}
	if (target == nullptr)
	{
		command_pop(world, actor);
		return true;
	}
	return work.mode == 0
		? land_type13_update(
			actor,
			*target,
			world,
			runtime,
			stats,
			player_demand,
			tick,
			demand,
			applies_flight)
		: land_type12_update(
			actor,
			*target,
			world,
			runtime,
			stats,
			player_demand,
			tick,
			demand,
			applies_flight);
}

bool friendly_fire_update(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& runtime,
	const assets::ShipStatsTable& stats,
	const game::FlightDemand& player_demand,
	std::uint32_t tick,
	game::FlightDemand& demand)
{
	FriendlyFireWork& work = actor.ai.work.friendly_fire;
	if (work.stage == 0)
	{
		demand = actor.player ? player_demand : game::FlightDemand{};
		if (work.deadline < tick)
		{
			work.stage = 1;
			work.deadline = tick + 700;
		}
		return true;
	}
	if (work.stage == 1)
	{
		game::WorldObject* focus =
			game::world_resolve(world, world.action_center);
		game::FlightDemand steered;
		if (focus != nullptr)
		{
			steered = steer_toward(
				actor,
				stats,
				focus->position,
				0.0f,
				runtime.frame_delta_ticks);
		}
		const game::FlightDemand saved =
			actor.player ? player_demand : game::FlightDemand{};
		demand.throttle = saved.throttle * 0.7f;
		demand.roll = saved.roll * 0.7f + steered.roll * 0.3f;
		demand.pitch = saved.pitch * 0.7f + steered.pitch * 0.3f;
		demand.yaw = saved.yaw * 0.7f + steered.yaw * 0.3f;
		if (work.deadline < tick)
		{
			work.stage = 2;
		}
		return true;
	}
	if (work.stage == 2)
	{
		game::WorldObject* focus =
			game::world_resolve(world, world.action_center);
		game::WorldObject* player =
			game::world_resolve(world, world.player);
		if (player == nullptr)
		{
			command_pop(world, actor);
			return true;
		}
		player->runtime_flags &= ~0x10000000u;
		command_pop(world, actor);
		if (runtime.network.role != mission::NetworkRole::offline
			&& actor.ai.friendly_fire_status != 2
			&& actor.ai.friendly_fire_status != 3)
		{
			command_push(world,
				*player,
				20,
				TargetKind::object,
				player->mission_index,
				-1);
			player->runtime_flags |= 0x10000000u;
			return true;
		}
		if (focus != nullptr)
		{
			focus->runtime_flags &= ~game::kObjectFlagPlaceholder;
			command_push(world,
				*player,
				8,
				TargetKind::object,
				focus->mission_index);
			if (glm::distance(player->position, focus->position)
				> 1000000.0f)
			{
				command_push(world,
					*player,
					20,
					TargetKind::object,
					player->mission_index);
			}
		}
		player->runtime_flags |= 0x10000000u;
		return true;
	}
	return true;
}

void queue_friendly_fire_consequence_speech(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& mission)
{
	const std::uint8_t status = actor.ai.friendly_fire_status;
	unsigned line = 0;
	if (status == 1)
	{
		line = 13u + game::world_rand15(world) % 11u;
		std::snprintf(
			mission.presentation.standalone_speech_path,
			sizeof(mission.presentation.standalone_speech_path),
			"ms_speech/ff_%03u",
			line);
		mission.presentation.comms_category =
			mission.mission_number > 13 ? 2 : 4;
	}
	else
	{
		std::snprintf(
			mission.presentation.standalone_speech_path,
			sizeof(mission.presentation.standalone_speech_path),
			"%s",
			"ms_speech/abrt_001");
		const game::WorldObject* focus =
			game::world_resolve(world, world.action_center);
		mission.presentation.comms_category =
			focus != nullptr && focus->type == 0x0c ? 0x1b : 0x1c;
	}
	mission.presentation.speech_pending = true;
	mission.presentation.speech_request_serial =
		++mission.presentation.slot_zero_request_serial;
	diagnostics::mission_log(
		"friendly-fire consequence actor=%u status=%u speech=%s "
		"category=%u",
		static_cast<unsigned>(actor.mission_index),
		static_cast<unsigned>(status),
		mission.presentation.standalone_speech_path,
		static_cast<unsigned>(
			mission.presentation.comms_category));
}
}

bool scripted_command_begin(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	std::uint32_t tick)
{
	switch (command.id)
	{
	case 8:
		return land_begin(
			actor, command, world, mission, stats, tick);
	case 14:
		return formation_regroup_begin(
			actor, world, mission, file);
	case 15:
		return patrol_begin(
			actor, command, world, mission, file, stats);
	case 17:
	case 119:
		return curve_begin(
			actor, command, world, mission, file, tick);
	case 104:
		return launch_begin(actor, command, world, mission, stats);
	case 109:
		return dock_begin(actor, command, world, mission);
	case 117:
		actor.ai.work.friendly_fire.stage = 0;
		actor.ai.work.friendly_fire.deadline = tick + 300;
		queue_friendly_fire_consequence_speech(
			actor, world, mission);
		return true;
	default:
		return false;
	}
}

bool scripted_command_update(
	game::WorldObject& actor,
	Command& command,
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	const game::FlightDemand& player_demand,
	std::uint32_t tick,
	game::FlightDemand& demand,
	bool& applies_flight)
{
	switch (command.id)
	{
	case 8:
		return land_update(
			actor,
			world,
			mission,
			stats,
			player_demand,
			tick,
			demand,
			applies_flight);
	case 14:
		return formation_regroup_update(
			actor,
			world,
			mission,
			file,
			stats,
			tick,
			demand,
			applies_flight);
	case 15:
		return patrol_update(
			actor, world, mission, file, stats, demand);
	case 17:
	case 119:
		return curve_update(
			actor,
			command,
			world,
			mission,
			file,
			stats,
			tick,
			demand,
			applies_flight);
	case 104:
		return launch_update(
			actor,
			command,
			world,
			mission,
			stats,
			tick,
			demand,
			applies_flight);
	case 109:
		return dock_update(
			actor,
			world,
			mission,
			stats,
			tick,
			demand,
			applies_flight);
	case 117:
		return friendly_fire_update(
			actor,
			world,
			mission,
			stats,
			player_demand,
			tick,
			demand);
	default:
		return false;
	}
}

bool scripted_flight_callback(
	game::WorldObject& actor,
	game::World& world,
	mission::Runtime& mission,
	const mission::DteFile& file,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick)
{
	if (actor.flight_callback_mode
		== game::FlightCallbackMode::follow_quadratic_curve)
	{
		const JumpWork& jump = actor.ai.work.jump;
		const float phase = static_cast<float>(
			simulation_tick - jump.deadline) * 0.004f;
		const glm::vec3 point =
			jump.hidden_position
				+ (jump.corridor_endpoint - jump.hidden_position)
					* phase * phase;
		actor.linear_velocity = point - actor.previous_position;
		actor.inertial_angular_step = glm::mat3{1.0f};
		actor.exhaust_scalar = 0.0f;
		return true;
	}
	if (actor.flight_callback_mode
		== game::FlightCallbackMode::jump_transition)
	{
		const JumpWork& jump = actor.ai.work.jump;
		const float initial_speed =
			(actor.runtime_flags & game::kObjectFlagCompound) != 0
				? 2400.0f : 600.0f;
		const float phase = static_cast<float>(
			simulation_tick - jump.transition_start_tick) * 0.003f;
		const float speed = std::max(
			initial_speed * (1.0f - phase),
			game::world_effective_max_speed(
				actor, stats, world.camera_mode));
		actor.linear_velocity = actor.previous_orientation[2] * speed;
		actor.inertial_angular_step = glm::mat3{1.0f};
		actor.exhaust_scalar = 0.0f;
		return true;
	}
	if (actor.flight_callback_mode
		!= game::FlightCallbackMode::provider_forward
		&& actor.flight_callback_mode
			!= game::FlightCallbackMode::provider_reverse)
	{
		return false;
	}
	if (actor.ai.command_count == 0)
	{
		return false;
	}
	Command& command = actor.ai.commands[0];
	if ((command.id == 17 || command.id == 119)
		&& actor.ai.work.stage == 2)
	{
		const glm::vec3 target = curve_provider_target(
			actor, command, mission, file, simulation_tick);
		prepare_provider_motion(
			actor,
			stats,
			target,
			{},
			1.0f,
			actor.flight_callback_mode
				== game::FlightCallbackMode::provider_reverse);
		return true;
	}
	if (command.id == 109)
	{
		DockWork& work = actor.ai.work.dock;
		const bool interpolation_stage =
			(work.mode == 0 && work.stage == 6)
				|| ((work.mode == 2 || work.mode == 3)
					&& work.stage == 2);
		if (!interpolation_stage)
		{
			return false;
		}
		game::WorldObject* target = mission::runtime_resolve_object(
			mission, work.target_object, world);
		if (target == nullptr)
		{
			return true;
		}
		glm::vec3 provider_target;
		glm::vec3 provider_up;
		if (!dock_provider_target(
			actor,
			*target,
			work,
			simulation_tick,
			provider_target,
			provider_up))
		{
			return true;
		}
		prepare_provider_motion(
			actor,
			stats,
			provider_target,
			provider_up,
			0.5f,
			false);
		if (work.interpolation_deadline_tick < simulation_tick)
		{
			// The provider's terminal path installs 0x004744d0 before it
			// advances the docking state.
			actor.flight_callback_mode =
				game::FlightCallbackMode::standard_reverse;
			++work.stage;
		}
		return true;
	}
	return false;
}

void scripted_mission_frame_dispatch(
	game::World& world,
	mission::Runtime& mission)
{
	for (game::WorldObject& object : world.objects)
	{
		const std::uint8_t status =
			object.ai.friendly_fire_status;
		if (!object.active || status < 1 || status > 3)
		{
			continue;
		}
		if (command_has_positive_priority(object))
		{
			continue;
		}
		if (object.player)
		{
			if (mission.gameplay_state != 0)
			{
				continue;
			}
			mission.gameplay_state =
				status == 3 ? 7 : 6;
			const Command previous =
				object.ai.command_count != 0
					? object.ai.commands[0]
					: Command{};
			if (command_push(world,
				object,
				117,
				previous.target_kind,
				previous.target,
				previous.target_component))
			{
				object.runtime_flags |= 0x10000000u;
				// FriendlyFire_dispatch_consequence 0x00474bfd
				// publishes the non-global status after installing the
				// local consequence command.
				mission::network_publish_friendly_fire_status(
					mission.network);
			}
		}
		else if (command_push(world,
			object, 20, TargetKind::none, UINT16_MAX))
		{
			object.runtime_flags |= 0x10000000u;
		}
	}
}

void friendly_fire_flag_offender(
	game::WorldObject& offender,
	mission::Runtime& mission,
	bool promote_all_players)
{
	mission::network_flag_local_friendly_fire(
		mission.network, offender, promote_all_players);
}

void friendly_fire_accumulate_damage(
	game::World& world,
	mission::Runtime& mission,
	const game::WorldObject& victim,
	float damage,
	std::uint32_t tick)
{
	game::WorldObject* player = game::world_resolve(world, world.player);
	if (player == nullptr || damage <= 0.0f
		|| player->ai.friendly_fire_warning_level >= 3)
	{
		return;
	}
	player->ai.friendly_fire_damage += damage;
	if (player->ai.friendly_fire_damage <= 800.0f
		|| player->ai.friendly_fire_warning_deadline >= tick)
	{
		return;
	}
	++player->ai.friendly_fire_warning_level;
	// FriendlyFire_accumulate_damage_warning at 0x00474c80 contains this
	// shipped defect, so the second and third utterance families are
	// unreachable.
	if (player->ai.friendly_fire_warning_level >= 2)
	{
		player->ai.friendly_fire_warning_level = 1;
	}
	player->ai.friendly_fire_damage = 0.0f;
	player->ai.friendly_fire_warning_deadline = tick + 3000;
	const unsigned line =
		1u + (game::world_object_rand15(*player) & 3u);
	std::snprintf(
		mission.presentation.speech_path,
		sizeof(mission.presentation.speech_path),
		"ms_speech/ff_%03u",
		line);
	mission.presentation.pilot = victim.pilot;
	mission.presentation.speaker_mission_index =
		victim.mission_index;
	mission.presentation.movie_path[0] = '\0';
	mission.presentation.comms_category =
		mission.mission_number > 13 ? 2 : 4;
	mission.presentation.comms_pending = true;
	mission.presentation.comms_request_serial =
		++mission.presentation.slot_zero_request_serial;
	diagnostics::mission_log(
		"friendly-fire warning victim=%u damage=%.1f total-reset "
		"line=ff_%03u category=%u",
		static_cast<unsigned>(victim.mission_index),
		damage,
		line,
		static_cast<unsigned>(
			mission.presentation.comms_category));
}
}
