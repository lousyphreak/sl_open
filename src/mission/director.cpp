#include "mission/director.hpp"

#include "core/math.hpp"
#include "core/mission_log.hpp"
#include "io/endian.hpp"
#include "mission/events.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

namespace sl_open::mission
{
namespace
{
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
	const DteFile& file,
	std::uint16_t curve)
{
	return curve < file.sections[16].count
		? dte_section_data(file, 16)
			+ static_cast<std::size_t>(curve) * 0x44
		: nullptr;
}

struct HermiteBasis
{
	long double h00;
	long double h01;
	long double h10;
	long double h11;
};

HermiteBasis hermite_basis(float parameter)
{
	// The four helpers at 0x00457130..0x004571c0 receive a stored float but
	// return their polynomial on the x87 stack. Retain that precision here;
	// the two curve evaluators below deliberately spill at different points.
	const long double t = static_cast<long double>(parameter);
	const long double t2 = t * t;
	const long double t3 = t2 * t;
	return {
		t3 * 2.0L - t2 * 3.0L + 1.0L,
		t3 * -2.0L + t2 * 3.0L,
		t3 - t2 * 2.0L + t,
		t3 - t2,
	};
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

glm::vec3 sample_curve(
	const std::uint8_t* curve,
	float t)
{
	const glm::vec3 p0 = read_vec3(curve + 8);
	const glm::vec3 p1 = read_vec3(curve + 0x14);
	const glm::vec3 tangent0 =
		read_vec3(curve + 0x28) * 10.0f;
	const glm::vec3 tangent1 =
		read_vec3(curve + 0x34) * -10.0f;
	const HermiteBasis basis = hermite_basis(t);
	// MissionCurve_sample_component_exact (0x00457090) rounds the endpoint
	// pair and the H11 contribution through two float stack temporaries
	// before adding H10. Keep that accumulation order at the large world
	// coordinates used by the compiled missions.
	glm::vec3 point;
	for (std::size_t axis = 0; axis < 3; ++axis)
	{
		float component = static_cast<float>(
			basis.h01 * static_cast<long double>(p1[axis]));
		component = static_cast<float>(
			basis.h00 * static_cast<long double>(p0[axis])
				+ static_cast<long double>(component));
		component = static_cast<float>(
			basis.h11 * static_cast<long double>(tangent1[axis])
				+ static_cast<long double>(component));
		point[axis] = static_cast<float>(
			basis.h10 * static_cast<long double>(tangent0[axis])
				+ static_cast<long double>(component));
	}
	return point;
}

glm::vec3 sample_curve_length_point(
	const std::uint8_t* curve,
	std::uint32_t index)
{
	// MissionCurve_initialize_basis_table builds 33 float coefficient rows,
	// and MissionCurve_measure samples those rows through the table-based
	// evaluator at 0x00456f70 rather than Director's exact evaluator at
	// 0x00457050. The table inputs are exact multiples of 1/32; round each
	// completed x87 coefficient once when publishing that table row.
	const float parameter = static_cast<float>(index) * (1.0f / 32.0f);
	const HermiteBasis extended = hermite_basis(parameter);
	const float h00 = static_cast<float>(extended.h00);
	const float h01 = static_cast<float>(extended.h01);
	const float h10 = static_cast<float>(extended.h10);
	const float h11 = static_cast<float>(extended.h11);
	const glm::vec3 p0 = read_vec3(curve + 8);
	const glm::vec3 p1 = read_vec3(curve + 0x14);
	const glm::vec3 tangent0 = read_vec3(curve + 0x28) * 10.0f;
	const glm::vec3 tangent1 = read_vec3(curve + 0x34) * -10.0f;
	glm::vec3 point;
	for (std::size_t axis = 0; axis < 3; ++axis)
	{
		// MissionCurve_sample_component_table keeps the four products and
		// additions in x87 precision and rounds only the returned component.
		long double component =
			static_cast<long double>(h01)
				* static_cast<long double>(p1[axis]);
		component =
			static_cast<long double>(h00)
				* static_cast<long double>(p0[axis])
				+ component;
		component =
			static_cast<long double>(h11)
				* static_cast<long double>(tangent1[axis])
				+ component;
		component =
			static_cast<long double>(h10)
				* static_cast<long double>(tangent0[axis])
				+ component;
		point[axis] = static_cast<float>(component);
	}
	return point;
}

float curve_length(const std::uint8_t* curve)
{
	if (curve == nullptr)
	{
		return 0.0f;
	}
	float length = 0.0f;
	glm::vec3 previous = sample_curve_length_point(curve, 0);
	for (std::uint32_t index = 1; index <= 32; ++index)
	{
		const glm::vec3 point = sample_curve_length_point(curve, index);
		const long double x =
			static_cast<long double>(point.x)
				- static_cast<long double>(previous.x);
		const long double y =
			static_cast<long double>(point.y)
				- static_cast<long double>(previous.y);
		const long double z =
			static_cast<long double>(point.z)
				- static_cast<long double>(previous.z);
		const float segment = static_cast<float>(
			std::sqrt(x * x + y * y + z * z));
		length += segment;
		previous = point;
	}
	return length;
}

std::uint16_t connected_curve(
	const DteFile& file,
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
		const std::uint8_t* candidate =
			curve_record(file, index);
		// MissionCurve_find_connected is called with its reverse-endpoint
		// flag clear by both DirectorCamera_advance and
		// MissionCurve_chain_length (LANCER.EXE 0x00450f5e and
		// 0x0045734e). It therefore accepts only a candidate whose first
		// endpoint is the current curve's second endpoint. Treating either
		// end as a connection can select an authored incoming curve and then
		// sample it in the wrong direction.
		if (curve_endpoint(candidate, false) == endpoint)
		{
			return index;
		}
	}
	return UINT16_MAX;
}

float chain_length(
	const DteFile& file,
	std::uint16_t first)
{
	float length = 0.0f;
	std::uint16_t current = first;
	for (std::uint16_t visited = 0;
		current != UINT16_MAX
			&& visited <= file.sections[16].count;
		++visited)
	{
		const std::uint8_t* curve =
			curve_record(file, current);
		if (curve == nullptr)
		{
			break;
		}
		length += curve_length(curve);
		const std::uint16_t endpoint =
			curve_endpoint(curve, true);
		if (endpoint == UINT16_MAX)
		{
			break;
		}
		current = connected_curve(file, current, endpoint);
	}
	return length;
}

std::uint16_t first_reference_object(
	const Runtime& runtime,
	const MissionReferenceValue& reference)
{
	if (reference.index == UINT16_MAX)
	{
		return UINT16_MAX;
	}
	if (reference.kind == ReferenceKind::object)
	{
		return reference.index < runtime.object_count
			? reference.index
			: UINT16_MAX;
	}
	std::uint16_t object = UINT16_MAX;
	return runtime_expand_reference(
		runtime,
		reference.kind,
		reference.index,
		&object,
		1) == 1
		? object
		: UINT16_MAX;
}

void set_disabled(
	Runtime& runtime,
	game::World& world,
	const MissionReferenceValue& reference,
	bool disabled)
{
	if (reference.index == UINT16_MAX)
	{
		return;
	}
	std::uint16_t objects[game::kMaxMissionObjects];
	const std::uint16_t count = runtime_expand_reference(
		runtime,
		reference.kind,
		reference.index,
		objects,
		static_cast<std::uint16_t>(std::size(objects)));
	for (std::uint16_t index = 0; index < count; ++index)
	{
		game::WorldObject* object =
			runtime_resolve_object(runtime, objects[index], world);
		if (object == nullptr)
		{
			continue;
		}
		if (disabled) object->runtime_flags |= game::kObjectFlagSimulationSuspended;
		else object->runtime_flags &= ~game::kObjectFlagSimulationSuspended;
	}
}

float authored_angle(
	const DteFile& file,
	std::uint16_t object,
	std::size_t offset)
{
	if (object >= file.sections[3].count)
	{
		return 0.0f;
	}
	const std::uint8_t* record =
		dte_section_data(file, 3)
		+ static_cast<std::size_t>(object) * 0x4c;
	return static_cast<float>(
		static_cast<std::int16_t>(
			io::read_le16(record + offset)));
}

glm::mat3 look_at(
	const glm::vec3& from,
	const glm::vec3& to)
{
	// SR_mat3_look_at_points (LANCER.EXE 0x004c1940) constructs the
	// zero-roll result by yawing identity first, transforming the remaining
	// delta into that frame, then applying pitch. It is not an arbitrary
	// cross-product basis.
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

void find_next_marker(
	const Runtime& runtime,
	const DteFile& file,
	std::uint16_t curve,
	float after,
	float& next,
	std::uint16_t* exact)
{
	next = 2.0f;
	if (exact != nullptr)
	{
		*exact = UINT16_MAX;
	}
	const std::uint8_t* records = dte_section_data(file, 3);
	for (std::uint16_t index = 0;
		index < runtime.object_count;
		++index)
	{
		const std::uint8_t* record =
			records + static_cast<std::size_t>(index) * 0x4c;
		if (runtime.objects[index].type != 0x03e3
			|| io::read_le16(record + 0x40) != curve)
		{
			continue;
		}
		const float time = read_float(record + 0x44);
		if (time == after && exact != nullptr)
		{
			// The scanner deliberately retains the last exact-time object.
			*exact = index;
		}
		else if (time > after && time < next)
		{
			next = time;
		}
	}
}

void configure_segment(
	Runtime& runtime,
	const DteFile& file,
	std::uint16_t curve)
{
	DirectorState& director = runtime.director;
	director.current_curve = curve;
	director.active.elapsed = 0;
	const float segment_length =
		curve == UINT16_MAX
			? 0.0f
			: curve_length(curve_record(file, curve));
	director.segment_ticks =
		director.chain_length != 0.0f
			? static_cast<std::uint32_t>(
				static_cast<long double>(segment_length)
					/ static_cast<long double>(director.chain_length)
					* static_cast<long double>(
						director.requested_ticks))
			: director.requested_ticks;
	director.next_marker_time = 0.0f;
	director.marker_object = UINT16_MAX;
	if (curve != UINT16_MAX)
	{
		find_next_marker(
			runtime,
			file,
			curve,
			0.0f,
			director.next_marker_time,
			nullptr);
	}
}

void begin_shot(
	Runtime& runtime,
	game::World& world,
	const DteFile& file)
{
	DirectorState& director = runtime.director;
	DirectorShot& shot = director.active;
	director.requested_ticks = shot.duration;
	director.static_object =
		shot.first_is_curve
			? UINT16_MAX
			: first_reference_object(runtime, shot.position);
	director.chain_length =
		shot.first_is_curve
			? chain_length(file, shot.curve)
			: 0.0f;
	configure_segment(
		runtime,
		file,
		shot.first_is_curve ? shot.curve : UINT16_MAX);
	const std::uint16_t moving = first_reference_object(
		runtime, shot.moving_origin);
	const game::WorldObject* moving_object =
		runtime_resolve_object(runtime, moving, world);
	director.moving_start = moving_object != nullptr
		? moving_object->position
		: glm::vec3{0.0f};
	set_disabled(runtime, world, shot.disable, true);
	director.disable_applied = true;
	director.shot_started = true;
	director.mode = 13;
	director.waiting = true;
	runtime.active_camera_mode = 13;
	director.transition_order =
		++runtime.camera_transition_order;
	++director.serial;
	diagnostics::mission_log(
		"director start kind=%s reference=%u target=%u duration=%u "
		"queued=%u",
		shot.first_is_curve ? "curve" : "object",
		static_cast<unsigned>(
			shot.first_is_curve
				? shot.curve
				: shot.position.index),
		static_cast<unsigned>(shot.target.index),
		static_cast<unsigned>(director.requested_ticks),
		static_cast<unsigned>(director.stack_count));
}

void clear_active_disable(
	Runtime& runtime,
	game::World& world)
{
	DirectorState& director = runtime.director;
	if (!director.disable_applied)
	{
		return;
	}
	set_disabled(
		runtime, world, director.active.disable, false);
	director.disable_applied = false;
}

void request_ordinary_camera(
	Runtime& runtime,
	game::World& world)
{
	runtime.requested_camera_mode = 0;
	runtime.requested_camera_target = world.player.index;
	runtime.requested_camera_lock = false;
	runtime.requested_camera_override_lock = true;
	runtime_publish_camera_request(runtime);
}

bool finish_active(
	Runtime& runtime,
	game::World& world,
	const DteFile& file)
{
	DirectorState& director = runtime.director;
	clear_active_disable(runtime, world);
	if (director.stack_count != 0)
	{
		director.active = director.stack[0];
		// Retail copies queue records 1..9 over 0..8 unconditionally. The
		// active assignment above is copy one; these are the remaining eight
		// fixed record copies.
		for (std::size_t index = 1;
			index < std::size(director.stack);
			++index)
		{
			director.stack[index - 1] = director.stack[index];
		}
		--director.stack_count;
		begin_shot(runtime, world, file);
		return true;
	}
	director.active = {};
	director.mode = 0;
	director.waiting = false;
	director.shot_started = false;
	director.current_curve = UINT16_MAX;
	director.static_object = UINT16_MAX;
	runtime.active_camera_mode = 0;
	request_ordinary_camera(runtime, world);
	++director.serial;
	diagnostics::mission_log("director complete");
	return false;
}
}

void director_enqueue(
	Runtime& runtime,
	game::World& world,
	const DteFile& file,
	const DirectorShot& shot,
	bool clear,
	std::uint32_t initial_steps)
{
	DirectorState& director = runtime.director;
	if (clear)
	{
		// StartDirectorCam resets the ten-record queue, but the subsequent
		// mode-13 installation first clears the old head's suspension scope.
		clear_active_disable(runtime, world);
		director.active = {};
		director.stack_count = 0;
		director.shot_started = false;
		director.current_curve = UINT16_MAX;
		director.static_object = UINT16_MAX;
	}
	if (!director.active.valid)
	{
		director.active = shot;
		begin_shot(runtime, world, file);
		// Camera_switch_mode services the installed mode immediately with
		// the current fixed-frame delta.
		director_service(
			runtime, world, file, initial_steps);
		return;
	}
	if (director.stack_count < std::size(director.stack))
	{
		director.stack[director.stack_count++] = shot;
		return;
	}
	if (!director.queue_overflow_logged)
	{
		director.queue_overflow_logged = true;
		diagnostics::mission_log(
			"director queue saturated capacity=%zu",
			std::size(director.stack) + 1);
	}
}

void director_stop(
	Runtime& runtime,
	game::World& world,
	const DteFile&)
{
	runtime.director.stop_requested = false;
	// StopDirectorCam (0x00458e30) only requests ordinary camera mode zero.
	// The ten Director request records/count are not cleared by this
	// callback. The accepted camera replacement below owns suspension-scope
	// release while deliberately leaving the dormant queue intact.
	request_ordinary_camera(runtime, world);
	diagnostics::mission_log("director stop");
}

void director_camera_replaced(
	Runtime& runtime,
	game::World& world)
{
	DirectorState& director = runtime.director;
	if (director.mode != 13)
	{
		return;
	}
	clear_active_disable(runtime, world);
	director.mode = 0;
	director.waiting = false;
	director.stop_requested = false;
	diagnostics::mission_log("director replaced by camera request");
}

void director_service(
	Runtime& runtime,
	game::World& world,
	const DteFile& file,
	std::uint32_t simulation_steps)
{
	DirectorState& director = runtime.director;
	if (director.stop_requested)
	{
		director_stop(runtime, world, file);
		return;
	}
	if (director.mode != 13)
	{
		return;
	}
	if (!director.active.valid)
	{
		return;
	}
	if (!director.shot_started)
	{
		begin_shot(runtime, world, file);
	}
	DirectorShot& shot = director.active;
	shot.elapsed += simulation_steps;
	const float t =
		director.segment_ticks == 0
			? std::numeric_limits<float>::infinity()
			: static_cast<float>(
				static_cast<long double>(shot.elapsed)
					/ static_cast<long double>(director.segment_ticks));
	std::uint16_t start_object = director.static_object;
	std::uint16_t end_object = director.static_object;
	if (director.current_curve != UINT16_MAX)
	{
		const std::uint8_t* curve =
			curve_record(file, director.current_curve);
		if (curve == nullptr)
		{
			if (finish_active(runtime, world, file))
			{
				director_service(
					runtime, world, file, simulation_steps);
			}
			return;
		}
		start_object = curve_endpoint(curve, false);
		end_object = curve_endpoint(curve, true);
		director.camera_position = sample_curve(curve, t);
	}
	else
	{
		const game::WorldObject* object =
			runtime_resolve_object(
				runtime, director.static_object, world);
		if (object != nullptr)
		{
			// DirectorCamera_sample_position obtains the live GameObject
			// through MissionObject_get_live_object and copies GameObject+0x3c
			// directly (LANCER.EXE 0x004511cd..0x004511e1). Unlike the target
			// path below, it does not dereference GameObject+0x30 to read the
			// interpolated scene root.
			director.camera_position = object->position;
		}
	}
	const std::uint16_t moving = first_reference_object(
		runtime, shot.moving_origin);
	const game::WorldObject* moving_object =
		runtime_resolve_object(runtime, moving, world);
	if (moving_object != nullptr && moving < runtime.object_count)
	{
		// MissionCurve_apply_moving_reference reads GameObject+0x3c, not the
		// interpolated scene node. It also preserves these as two separately
		// rounded additions (LANCER.EXE 0x004574ba..0x00457502), which matters
		// at mission-scale world coordinates even though they collapse
		// algebraically.
		director.camera_position +=
			moving_object->position - director.moving_start;
		director.camera_position +=
			director.moving_start
				- runtime.objects[moving].authored_position;
	}
	const std::uint16_t target = first_reference_object(
		runtime, shot.target);
	const game::WorldObject* target_object =
		runtime_resolve_object(runtime, target, world);
	if (target_object != nullptr)
	{
		director.camera_orientation = look_at(
			director.camera_position,
			target_object->scene_position);
	}
	else
	{
		float heading0 =
			authored_angle(file, start_object, 0x2c);
		float heading1 =
			authored_angle(file, end_object, 0x2c);
		float pitch0 =
			authored_angle(file, start_object, 0x38);
		float pitch1 =
			authored_angle(file, end_object, 0x38);
		if (heading1 < heading0) heading1 += 360.0f;
		if (pitch1 < pitch0) pitch1 += 360.0f;
		director.camera_orientation = glm::mat3{1.0f};
		director.camera_orientation = math::postrotate(
			director.camera_orientation,
			glm::radians(
				heading0 + (heading1 - heading0) * t),
			{0.0f, 1.0f, 0.0f});
		director.camera_orientation = math::postrotate(
			director.camera_orientation,
			glm::radians(
				pitch0 + (pitch1 - pitch0) * t),
			{1.0f, 0.0f, 0.0f});
	}
	if (director.next_marker_time > 0.0f
		&& director.current_curve != UINT16_MAX
		&& director.next_marker_time <= t)
	{
		std::uint16_t reached = UINT16_MAX;
		float next = 2.0f;
		find_next_marker(
			runtime,
			file,
			director.current_curve,
			director.next_marker_time,
			next,
			&reached);
		director.next_marker_time = next;
		director.marker_object = reached;
		if (reached != UINT16_MAX)
		{
			events_emit_camera_reached(runtime, reached);
		}
	}
	if (!(t >= 1.0f))
	{
		return;
	}
	// Static Director cameras never emit Camera Reached and cannot acquire
	// a curve merely because their object happens to be a curve endpoint.
	if (director.current_curve != UINT16_MAX)
	{
		events_emit_camera_reached(runtime, end_object);
	}
	const std::uint16_t next =
		director.current_curve == UINT16_MAX
			|| end_object == UINT16_MAX
		? UINT16_MAX
		: connected_curve(
			file, director.current_curve, end_object);
	if (next != UINT16_MAX)
	{
		configure_segment(runtime, file, next);
		diagnostics::mission_log(
			"director segment curve=%u",
			static_cast<unsigned>(next));
		return;
	}
	if (finish_active(runtime, world, file))
	{
		// Starting the next queued request re-enters mode 13 through the
		// general switcher, whose contract includes one immediate update.
		director_service(
			runtime, world, file, simulation_steps);
	}
}
}
