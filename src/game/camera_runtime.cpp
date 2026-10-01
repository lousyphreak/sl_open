#include "game/camera_runtime.hpp"

#include "assets/ship_stats.hpp"
#include "game/missiles.hpp"
#include "game/world.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace sl_open::game
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kLandType13FirstPitch = -1.1707963943481445f;
constexpr float kLandType13SecondYaw = 2.8415927886962891f;

WorldObject* resolve_index(World& world, std::uint16_t index)
{
	if (index == UINT16_MAX || index >= std::size(world.objects))
	{
		return nullptr;
	}
	WorldObject& object = world.objects[index];
	return object.active ? &object : nullptr;
}

const WorldObject* resolve_index(
	const World& world,
	std::uint16_t index)
{
	if (index == UINT16_MAX || index >= std::size(world.objects))
	{
		return nullptr;
	}
	const WorldObject& object = world.objects[index];
	return object.active ? &object : nullptr;
}

bool target_reference_valid(
	const WorldObject& target,
	std::int16_t component)
{
	// TargetRef_is_valid, LANCER.EXE 0x00401870. Camera mode six supplies
	// no exemption mask.
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

glm::mat3 look_at_zero_roll(
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

glm::vec3 normalized_or_retail_zero(glm::vec3 value)
{
	const float length = glm::length(value);
	if (length == 0.0f)
	{
		return {0.0f, 0.0f, 7.523164e-37f};
	}
	return value / length;
}

void blend_toward_player(
	CameraRuntime& camera,
	const WorldObject& player,
	float factor)
{
	const glm::mat3 desired =
		look_at_zero_roll(camera.position, player.scene_position);
	const glm::vec3 current_euler =
		math::rotation_to_euler(camera.orientation);
	const glm::vec3 desired_euler =
		math::rotation_to_euler(desired);
	camera.orientation = math::rotation_from_euler(
		current_euler + (desired_euler - current_euler) * factor);
}

glm::mat4 object_transform(const WorldObject& object)
{
	glm::mat4 transform{object.scene_orientation};
	transform[3] = glm::vec4(object.scene_position, 1.0f);
	return transform;
}

glm::mat3 follow_offset_orientation(
	const glm::mat3& orientation,
	float pitch,
	float yaw)
{
	glm::mat3 result = math::postrotate(
		orientation, pitch, {1.0f, 0.0f, 0.0f});
	return math::postrotate(
		result, yaw, {0.0f, 1.0f, 0.0f});
}

void publish_follow_pose(
	CameraRuntime& camera,
	const glm::vec3& target_position,
	const glm::mat3& target_orientation,
	const glm::vec3& local_offset)
{
	const glm::mat3 offset_orientation =
		follow_offset_orientation(
			target_orientation,
			camera.smooth_pitch,
			camera.smooth_yaw);
	camera.position =
		target_position + offset_orientation * local_offset;
	camera.orientation = math::postrotate(
		target_orientation,
		camera.smooth_roll,
		{0.0f, 0.0f, 1.0f});
}

float camera_random_signed(World& world)
{
	return static_cast<float>(world_rand15(world))
		* (1.0f / 32768.0f) - 0.5f;
}

void apply_camera_shake(
	CameraRuntime& camera,
	World& world,
	float scale)
{
	camera.orientation = math::postrotate(
		camera.orientation,
		camera_random_signed(world) * scale,
		{0.0f, 1.0f, 0.0f});
	camera.orientation = math::postrotate(
		camera.orientation,
		camera_random_signed(world) * scale,
		{0.0f, 0.0f, 1.0f});
}

void service_cockpit_pose(
	CameraRuntime& camera,
	World& world,
	const WorldObject& target,
	const assets::ShipStatsTable& stats,
	float shake_scale)
{
	camera.position =
		target.scene_position
			+ target.scene_orientation * target.camera_offset;
	camera.orientation = target.scene_orientation;
	if (target.type >= assets::kShipStatsCount)
	{
		return;
	}
	const assets::FlightStats& flight =
		stats.records[target.type].flight;
	const float pitch = std::clamp(
		target.angular_x / flight.pitch_rate, -1.0f, 1.0f);
	const float yaw = std::clamp(
		target.angular_y / flight.yaw_rate, -1.0f, 1.0f);
	const float roll = std::clamp(
		target.angular_z / flight.roll_rate, -1.0f, 1.0f);
	const float forward = std::clamp(
		target.speed
			/ world_effective_max_speed(
				target, stats, world.camera_mode),
		-1.0f,
		1.0f);

	// Camera_update_frame mode zero applies motion/recoil to the dedicated
	// camera-attached cockpit object, not to the world camera. Its first
	// random pair is consumed even when the shake scalar is zero.
	const float first_roll =
		camera_random_signed(world) * shake_scale * 0.5f;
	const float first_yaw =
		camera_random_signed(world) * shake_scale * 0.5f;
	camera.cockpit_orientation =
		math::rotation_from_euler(
			{0.0f, first_yaw, first_roll});
	camera.cockpit_orientation = math::postrotate(
		camera.cockpit_orientation,
		pitch * -0.1f,
		{1.0f, 0.0f, 0.0f});
	camera.cockpit_orientation = math::postrotate(
		camera.cockpit_orientation,
		yaw * 0.03f,
		{0.0f, 1.0f, 0.0f});
	camera.cockpit_orientation = math::postrotate(
		camera.cockpit_orientation,
		roll * -0.1f,
		{0.0f, 0.0f, 1.0f});
	// The cockpit root is parented to the camera. Camera_update_frame writes
	// this local forward displacement first; mission rendering subtracts the
	// selected cockpit SRO's own camera offset when it instantiates the root.
	camera.cockpit_local_position =
		{0.0f, 0.0f, forward * 50.0f};
	camera.cockpit_component_orientation =
		math::rotation_from_euler({
			pitch * 0.15f,
			0.0f,
			(roll + yaw) * 0.2f,
		});
	camera.cockpit_recoil_offset = -camera.recoil * 30.0f;
	camera.recoil *= 0.95f;
	// The second pair is the actual world-camera shake and is conditional
	// on the pre-decay common shake scale.
	if (shake_scale > 0.0f)
	{
		apply_camera_shake(
			camera,
			world,
			world.player_camera_disturbance * 0.03f);
	}
}

void service_side_pose(
	CameraRuntime& camera,
	const WorldObject& target)
{
	const float yaw = camera.mode == 1
		? glm::radians(-90.0f)
		: camera.mode == 2
			? glm::radians(90.0f)
			: kPi;
	camera.orientation = math::postrotate(
		target.scene_orientation, yaw, {0.0f, 1.0f, 0.0f});
	camera.position = target.scene_position;
	if (camera.mode == 3 && target.type == 45)
	{
		camera.position += camera.orientation[2] * 1500.0f;
	}
	else
	{
		camera.position +=
			target.scene_orientation * target.camera_offset;
	}
}

void service_chase_pose(
	CameraRuntime& camera,
	const WorldObject& target,
	float maximum_radius_scale)
{
	camera.chase_distance = std::clamp(
		camera.chase_distance,
		target.radius * 1.8f,
		target.radius * maximum_radius_scale);
	glm::mat3 orbit{1.0f};
	orbit = math::postrotate(
		orbit,
		glm::radians(camera.chase_yaw_degrees),
		{0.0f, 1.0f, 0.0f});
	orbit = math::postrotate(
		orbit,
		glm::radians(camera.chase_pitch_degrees),
		{1.0f, 0.0f, 0.0f});
	camera.position =
		target.scene_position + orbit[2] * camera.chase_distance;
	camera.orientation =
		look_at_zero_roll(camera.position, target.scene_position);
}

void finalize_fixed_pose(
	CameraRuntime& camera,
	const WorldObject& player,
	const WorldObject& target,
	const glm::vec3& local)
{
	// Camera_switch_mode's shared scripted-camera finalizer first stages all
	// position snapshots through the hidden mission camera object. It then
	// constructs the zero-roll target direction in the local-player matrix
	// frame and composes that result back into the camera orientation.
	camera.previous_position = camera.position;
	camera.previous_orientation = camera.orientation;
	camera.staged_position =
		target.scene_position + target.scene_orientation * local;
	camera.position = camera.staged_position;
	const glm::vec3 staged_in_player_frame =
		glm::transpose(player.scene_orientation) * camera.staged_position;
	const glm::vec3 target_in_player_frame =
		glm::transpose(player.scene_orientation) * target.scene_position;
	camera.staged_orientation = look_at_zero_roll(
		staged_in_player_frame, target_in_player_frame);
	camera.orientation =
		player.scene_orientation * camera.staged_orientation;
}

void service_staged_target_pose(
	CameraRuntime& camera,
	const WorldObject& player)
{
	camera.position = camera.staged_position;
	const glm::vec3 staged_in_player_frame =
		glm::transpose(player.scene_orientation) * camera.staged_position;
	const glm::vec3 player_in_player_frame =
		glm::transpose(player.scene_orientation) * player.scene_position;
	camera.staged_orientation = look_at_zero_roll(
		staged_in_player_frame, player_in_player_frame);
	camera.orientation =
		player.scene_orientation * camera.staged_orientation;
}

void service_flyby_pose(
	CameraRuntime& camera,
	const WorldObject& player)
{
	glm::vec3 relative = camera.position - player.scene_position;
	float distance = glm::length(relative);
	if (distance > 23000.0f)
	{
		camera.position =
			player.scene_position
			+ player.scene_orientation
				* glm::vec3{
					0.0f,
					player.radius,
					player.radius * 4.0f};
		relative = camera.position - player.scene_position;
		distance = glm::length(relative);
	}
	if (distance < player.radius)
	{
		relative =
			normalized_or_retail_zero(relative) * player.radius;
		camera.position = player.scene_position + relative;
	}
	camera.orientation =
		look_at_zero_roll(camera.position, player.scene_position);
}

void service_ship_follow(
	CameraRuntime& camera,
	World& world,
	const MissileRuntime& missiles,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_ticks,
	std::uint32_t simulation_tick);

void fallback_to_player(
	CameraRuntime& camera,
	World& world,
	const MissileRuntime& missiles,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_ticks,
	std::uint32_t simulation_tick)
{
	camera_runtime_request(
		camera,
		world,
		missiles,
		stats,
		0,
		world.player.index,
		false,
		true,
		frame_ticks,
		simulation_tick);
}

void service_ship_follow(
	CameraRuntime& camera,
	World& world,
	const MissileRuntime& missiles,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_ticks,
	std::uint32_t simulation_tick)
{
	const WorldObject* target = resolve_index(world, camera.target);
	if (target == nullptr || target->type >= 256)
	{
		fallback_to_player(
			camera,
			world,
			missiles,
			stats,
			frame_ticks,
			simulation_tick);
		return;
	}

	float trailing_y = -750.0f;
	float distance_scale = 1800.0f;
	switch (target->type)
	{
	case 2:
		trailing_y = -650.0f;
		break;
	case 8:
		trailing_y = -850.0f;
		distance_scale = 2000.0f;
		break;
	case 9:
		trailing_y = -800.0f;
		distance_scale = 2400.0f;
		break;
	case 45:
		trailing_y = -1000.0f;
		distance_scale = 3400.0f;
		break;
	default:
		break;
	}
	const float distance_scalar =
		target->afterburner_active ? 1.5f : target->throttle;
	const float desired_distance =
		-(distance_scalar * 400.0f + distance_scale);
	camera.follow_distance +=
		(desired_distance - camera.follow_distance) * 0.1f;

	float pitch = std::clamp(
		-5.7f * target->angular_x,
		-kPi / 8.0f,
		kPi / 8.0f);
	pitch *= pitch < 0.0f ? 0.5f : 1.5f;
	const float yaw = std::clamp(
		-5.0f * target->angular_y,
		-kPi / 5.0f,
		kPi / 5.0f);
	const float roll = std::clamp(
		-3.0f * target->angular_z,
		-kPi / 5.0f,
		kPi / 5.0f);
	camera.smooth_pitch += (pitch - camera.smooth_pitch) * 0.05f;
	camera.smooth_yaw += (yaw - camera.smooth_yaw) * 0.05f;
	camera.smooth_roll +=
		(roll + yaw - camera.smooth_roll) * 0.05f;
	publish_follow_pose(
		camera,
		target->scene_position,
		target->scene_orientation,
		{0.0f, trailing_y, camera.follow_distance});
	if (const WorldObject* player =
			world_resolve(world, world.player))
	{
		camera.aim_near_position =
			player->scene_position
				+ player->scene_orientation
					* glm::vec3{0.0f, 0.0f, 1500.0f};
		camera.aim_near_orientation = player->scene_orientation;
		camera.aim_far_position =
			player->scene_position
				+ player->scene_orientation
					* glm::vec3{0.0f, 0.0f, 12000.0f};
		camera.aim_far_orientation = player->scene_orientation;
	}
}

void service_missile_follow(
	CameraRuntime& camera,
	World& world,
	const MissileRuntime& missiles,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_ticks,
	std::uint32_t simulation_tick)
{
	const std::uint32_t now_tick = camera.current_time_tick;
	if (camera.missile_phase != 0)
	{
		if (camera.missile_grace_deadline_tick < now_tick)
		{
			fallback_to_player(
				camera,
				world,
				missiles,
				stats,
				frame_ticks,
				simulation_tick);
		}
		return;
	}
	const Missile* missile =
		camera.active_missile < std::size(missiles.missiles)
			? &missiles.missiles[camera.active_missile]
			: nullptr;
	if (missile == nullptr
		|| !missile->active
		|| missile->type < 0)
	{
		camera.missile_grace_deadline_tick = now_tick + 150;
		camera.missile_phase = 1;
		return;
	}
	const WorldObject* source = resolve_index(world, camera.target);
	if (source == nullptr || source->type >= 256)
	{
		fallback_to_player(
			camera,
			world,
			missiles,
			stats,
			frame_ticks,
			simulation_tick);
		return;
	}

	const float desired_distance =
		-(missile->throttle * 400.0f + 800.0f);
	camera.follow_distance +=
		(desired_distance - camera.follow_distance) * 0.1f;
	const float pitch = std::clamp(
		-5.7f * missile->smoothed_pitch,
		-kPi / 8.0f,
		kPi / 8.0f);
	const float yaw = std::clamp(
		-5.0f * missile->smoothed_yaw,
		-kPi / 5.0f,
		kPi / 5.0f);
	const float roll = 0.0f;
	camera.smooth_pitch += (pitch - camera.smooth_pitch) * 0.005f;
	camera.smooth_yaw += (yaw - camera.smooth_yaw) * 0.005f;
	camera.smooth_roll +=
		(roll - camera.smooth_roll) * 0.005f
		+ yaw * 0.05f;
	publish_follow_pose(
		camera,
		missile->scene_position,
		missile->scene_orientation,
		{0.0f, -300.0f, camera.follow_distance});
}

void service_land_type13_bounds_camera(
	CameraRuntime& camera,
	const World& world)
{
	// Camera_update_frame mode 14, LANCER.EXE 0x004606c1.
	// The linked timestamp is command-8 work +0x04. The authored bounds
	// belong to the transition model retained by the landing controller.
	const WorldObject* actor = resolve_index(world, camera.target);
	if (actor == nullptr)
	{
		return;
	}
	const ai::LandWork& work = actor->ai.work.land;
	const WorldObject* scene =
		resolve_index(world, work.target_world_index);
	if (scene == nullptr
		|| work.transition_model >= scene->model_references.size())
	{
		return;
	}
	const ObjectModelReference& model =
		scene->model_references[work.transition_model];
	const float parameter = std::clamp(
		static_cast<float>(
			static_cast<std::int32_t>(
				camera.current_time_tick
					- work.deadline))
			* 0.0009090909152291715f,
		0.0f,
		1.0f);
	const float cosine = std::cos(parameter * kPi);
	const float blend =
		(1.0f - (cosine + 1.0f) * 0.5f) * 0.44999998807907104f
		+ 0.5f + 0.30000001192092896f;
	const glm::vec3 local{
		(model.bounds_max.x + model.bounds_min.x) * 0.5f,
		model.bounds_min.y * 0.94999998807907104f
			+ model.bounds_max.y * 0.05000000074505806f,
		blend * model.bounds_max.z
			+ (1.0f - blend * 0.5f) * model.bounds_min.z,
	};
	const glm::mat4 frame =
		object_transform(*scene)
		* model.scene_transform;
	camera.position = glm::vec3(frame * glm::vec4(local, 1.0f));
	const glm::vec3 relative =
		camera.position - actor->scene_position;
	camera.orientation = math::rotation_from_euler({
		0.0f,
		std::atan2(-relative.y, -relative.z),
		0.0f,
	});
}

void service_land_type13_timed_camera(
	CameraRuntime& camera,
	const World& world)
{
	// Camera_update_frame mode 37, LANCER.EXE 0x0046086a.
	const WorldObject* player = world_resolve(world, world.player);
	if (player == nullptr)
	{
		return;
	}
	const std::uint32_t now_tick = camera.current_time_tick;
	const std::uint32_t elapsed = now_tick - camera.installed_tick;
	glm::vec3 local;
	glm::vec3 euler;
	if (elapsed < 700)
	{
		local = {
			0.0f,
			-1600.0f - static_cast<float>(elapsed) * 2.5f,
			-500.0f,
		};
		euler = {kLandType13FirstPitch, 0.0f, 0.0f};
	}
	else if (elapsed < 1500)
	{
		local = {
			-1000.0f,
			-470.0f,
			3000.0f + static_cast<float>(elapsed - 700),
		};
		euler = {0.0f, kLandType13SecondYaw, 0.0f};
	}
	else
	{
		// Retail leaves both retained camera transforms untouched.
		return;
	}
	camera.position =
		player->scene_position + player->scene_orientation * local;
	camera.orientation =
		player->scene_orientation * math::rotation_from_euler(euler);
}

void service_dock_door_camera(
	CameraRuntime& camera,
	const World& world)
{
	// Camera_update_frame mode 43 transforms the retained docking target's
	// authored offset (10000,-1800,-1400) and looks at the local player.
	const WorldObject* target = resolve_index(world, camera.target);
	const WorldObject* player = world_resolve(world, world.player);
	if (target == nullptr || player == nullptr)
	{
		return;
	}
	camera.position =
		target->scene_position
			+ target->scene_orientation
				* glm::vec3{10000.0f, -1800.0f, -1400.0f};
	camera.orientation =
		look_at_zero_roll(camera.position, player->scene_position);
}

void service_launch_camera(
	CameraRuntime& camera,
	const World& world)
{
	const WorldObject* player = world_resolve(world, world.player);
	if (player == nullptr)
	{
		return;
	}
	const WorldObject& actor = *player;
	const ai::LaunchWork& work = actor.ai.work.launch;
	const std::uint32_t now_tick = camera.current_time_tick;
	const float elapsed = static_cast<float>(
		now_tick - camera.installed_tick);
	switch (camera.mode)
	{
	case 15:
	{
		// Camera_update_frame mode 15, LANCER.EXE
		// 0x00460b4a..0x00460ca1.
		const glm::vec3 first{1500.0f, -600.0f, 500.0f};
		const glm::vec3 second{1500.0f, 300.0f, 500.0f};
		const float factor = elapsed * 0.0013000000035390258f;
		const glm::vec3 local = first + (second - first) * factor;
		camera.position =
			actor.scene_position + actor.scene_orientation * local;
		camera.orientation = actor.scene_orientation;
		if (work.stage < 5
			|| (work.stage == 5
				&& now_tick <= work.deadline_tick + 150u))
		{
			camera.orientation =
				look_at_zero_roll(
					camera.position, player->scene_position);
		}
		return;
	}
	case 32:
	{
		// Mode 32's setup owns the position. Its updater applies the fixed
		// -0.9424779 X rotation until launch state six, then subtracts
		// 0.0007 radians per retained simulation tick.
		if (work.stage < 6)
		{
			camera.installed_tick = now_tick;
		}
		const float rotation_elapsed =
			static_cast<float>(
				now_tick - camera.installed_tick);
		const float angle =
			-0.9424778819f
			- (work.stage >= 6
				? rotation_elapsed * 0.0007f
				: 0.0f);
		camera.orientation = math::postrotate(
			actor.scene_orientation,
			angle,
			{1.0f, 0.0f, 0.0f});
		return;
	}
	default:
		return;
	}
}

void service_current(
	CameraRuntime& camera,
	World& world,
	const MissileRuntime& missiles,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_ticks,
	std::uint32_t simulation_tick)
{
	const std::uint32_t now_tick = camera.current_time_tick;
	world.player_camera_disturbance = std::min(
		2.0f, world.player_camera_disturbance);
	const float shake_scale =
		world.player_camera_disturbance > 0.0f
			? world.player_camera_disturbance * 0.1f
			: 0.0f;
	world.player_camera_disturbance = std::max(
		0.0f,
		world.player_camera_disturbance
			- static_cast<float>(frame_ticks) * 0.02f);
	if (camera.projection_rate != 0.0f)
	{
		camera.projection_progress +=
			static_cast<float>(frame_ticks)
			* camera.projection_rate * 0.001f;
		camera.projection_progress = std::clamp(
			camera.projection_progress, 0.0f, 0.1f);
		if (camera.projection_progress == 0.0f
			|| camera.projection_progress == 0.1f)
		{
			camera.projection_rate = 0.0f;
		}
		// SR_view_set_projection_bounds receives left/right fractions 0/1
		// with divisor .6 and symmetric top/bottom fractions with divisor
		// .8. These are independent projection slopes; framebuffer aspect
		// never participates in the retail camera frustum.
		camera.horizontal_tangent =
			0.5f / 0.6000000238418579f;
		camera.vertical_tangent =
			(0.5f - camera.projection_progress) / 0.8f;
	}

	const WorldObject* player = world_resolve(world, world.player);
	const WorldObject* target = resolve_index(world, camera.target);
	const std::uint32_t elapsed = now_tick - camera.installed_tick;
	switch (camera.mode)
	{
	case 0:
		if (camera.view_state == 2)
		{
			service_ship_follow(
				camera,
				world,
				missiles,
				stats,
				frame_ticks,
				simulation_tick);
		}
		else if (target != nullptr)
		{
			service_cockpit_pose(
				camera, world, *target, stats, shake_scale);
		}
		return;
	case 1:
	case 2:
	case 3:
		if (target != nullptr)
		{
			service_side_pose(camera, *target);
		}
		return;
	case 4:
	case 30:
		service_ship_follow(
			camera,
			world,
			missiles,
			stats,
			frame_ticks,
			simulation_tick);
		return;
	case 5:
		if (player != nullptr)
		{
			service_staged_target_pose(camera, *player);
		}
		return;
	case 6:
		if (player == nullptr)
		{
			return;
		}
		if (const WorldObject* selected =
				world_resolve(world, world.selected_target);
			selected != nullptr
				&& target_reference_valid(
					*selected, world.target_component))
		{
			service_chase_pose(camera, *selected, 5.8f);
			return;
		}
		fallback_to_player(
			camera,
			world,
			missiles,
			stats,
			frame_ticks,
			simulation_tick);
		return;
	case 7:
		if (target != nullptr)
		{
			const float angle =
				static_cast<float>(elapsed) * 0.005f;
			camera.orientation = math::postrotate(
				target->scene_orientation,
				angle,
				{0.0f, 1.0f, 0.0f});
			camera.position =
				target->scene_position
				+ camera.orientation * camera.orbit_vector;
		}
		return;
	case 8:
		if (target != nullptr)
		{
			const float angle =
				static_cast<float>(elapsed) * 0.005f;
			camera.orientation = math::postrotate(
				target->scene_orientation,
				angle,
				{0.0f, 1.0f, 0.0f});
			camera.position =
				target->scene_position
				+ camera.orientation[2]
					* (-static_cast<float>(elapsed) * 10.0f
						- 3000.0f);
		}
		return;
	case 9:
	{
		if (player == nullptr)
		{
			return;
		}
		WGateEffectsRuntime& wgate =
			world.transition_effects.wgate;
		const float phase =
			0.5f - static_cast<float>(elapsed) * 0.0005f;
		camera.orientation = math::postrotate(
			player->ai.work.warp.saved_orientation,
			phase,
			{0.0f, 1.0f, 0.0f});
		camera.position =
			player->scene_position
				+ camera.orientation
					* glm::vec3{0.0f, -350.0f, -2000.0f};
		wgate.camera_target_position = camera.position;
		wgate.camera_target_orientation = camera.orientation;
		return;
	}
	case 10:
		camera.position =
			world.transition_effects.wgate.camera_target_position;
		camera.orientation =
			world.transition_effects.wgate.camera_target_orientation;
		return;
	case 11:
	{
		const WGateEffectsRuntime& wgate =
			world.transition_effects.wgate;
		const WGateContext* context =
			wgate.camera_context_index >= 0
				&& static_cast<std::size_t>(
					wgate.camera_context_index)
					< wgate.contexts.size()
				? &wgate.contexts[
					static_cast<std::size_t>(
						wgate.camera_context_index)]
				: nullptr;
		if (context == nullptr || !context->active || player == nullptr)
		{
			return;
		}
		camera.position =
			context->position
			+ context->orientation[2]
				* (camera.graphics_quality < 2
					? 3000.0f
					: 10000.0f);
		camera.orientation =
			look_at_zero_roll(camera.position, player->scene_position);
		camera.orientation = math::postrotate(
			camera.orientation,
			static_cast<float>(elapsed) * -0.003f,
			{0.0f, 0.0f, 1.0f});
		return;
	}
	case 12:
		if (player != nullptr)
		{
			service_chase_pose(camera, *player, 3.8f);
		}
		return;
	case 13:
		// The Director owner publishes its own pose and queue transitions.
		return;
	case 14:
		service_land_type13_bounds_camera(camera, world);
		return;
	case 15:
		service_launch_camera(camera, world);
		return;
	case 16:
		// The switcher-installed transform is retained.
		return;
	case 17:
		if (player != nullptr)
		{
			camera.orientation =
				look_at_zero_roll(
					camera.position, player->scene_position);
		}
		return;
	case 33:
		if (player != nullptr)
		{
			blend_toward_player(camera, *player, 1.0f);
		}
		return;
	case 34:
		world.cinematic_mode = 0;
		if (player != nullptr)
		{
			blend_toward_player(camera, *player, 1.0f);
		}
		return;
	case 32:
		// Camera_update_frame mode 32 republishes these authored projection
		// bounds after the common transition prelude on every frame.
		camera.horizontal_tangent =
			0.5f / 0.34999999403953552f;
		camera.vertical_tangent =
			0.5f / 0.46700000762939453f;
		service_launch_camera(camera, world);
		return;
	case 18:
		service_missile_follow(
			camera,
			world,
			missiles,
			stats,
			frame_ticks,
			simulation_tick);
		return;
	case 19:
	case 21:
	case 22:
	case 24:
	case 31:
		// These modes deliberately retain the transform installed by the
		// switcher.
		return;
	case 20:
	case 25:
		if (player != nullptr)
		{
			camera.orientation =
				look_at_zero_roll(
					camera.position, player->scene_position);
		}
		return;
	case 23:
		if (player != nullptr)
		{
			const float distance = elapsed < 50
				? 1200.0f
				: elapsed >= 70
					? 1400.0f
					: 1200.0f
						+ static_cast<float>(elapsed - 50) * 10.0f;
			camera.position =
				player->scene_position
					+ player->scene_orientation
						* glm::vec3{200.0f, -500.0f, distance};
		}
		if (player != nullptr)
		{
			camera.orientation =
				look_at_zero_roll(
					camera.position, player->scene_position);
			if (shake_scale > 0.0f)
			{
				apply_camera_shake(
						camera,
						world,
						world.player_camera_disturbance * 0.03f);
			}
		}
		return;
	case 26:
		if (target != nullptr)
		{
			camera.orientation =
				look_at_zero_roll(
					camera.position, target->scene_position);
		}
		return;
	case 27:
		if (world.death_effects.camera_mode27_anchor_valid)
		{
			camera.orientation = look_at_zero_roll(
				camera.position,
				world.death_effects.camera_mode27_anchor);
		}
		return;
	case 28:
		if (target != nullptr)
		{
			const float angle =
				static_cast<float>(elapsed) * 0.002f
				+ kPi * 0.25f;
			camera.orientation = math::postrotate(
				target->scene_orientation,
				angle,
				{0.0f, 1.0f, 0.0f});
			const float distance = std::max(
				1000.0f,
				10000.0f - static_cast<float>(elapsed) * 2.0f);
			camera.position =
				target->scene_position
				+ camera.orbit_vector
				- camera.orientation[2] * distance;
		}
		return;
	case 29:
		if (player != nullptr)
		{
			const float distance =
				(player->runtime_flags & kObjectFlagDestroyed) != 0
					? 1000.0f
						+ static_cast<float>(elapsed) * 20.0f
					: 1000.0f;
			if ((player->runtime_flags & kObjectFlagDestroyed) == 0)
			{
				camera.installed_tick = now_tick;
			}
			camera.position =
				player->scene_position
				- camera.orbit_vector * distance;
			if (target != nullptr)
			{
				camera.orientation = look_at_zero_roll(
					camera.position, target->scene_position);
			}
		}
		return;
	case 35:
		if (target != nullptr)
		{
			camera.position =
				target->scene_position
				+ target->scene_orientation
					* glm::vec3{500.0f, 0.0f, -500.0f};
			if (player != nullptr)
			{
				blend_toward_player(camera, *player, 1.0f);
			}
		}
		return;
	case 38:
		if (target != nullptr)
		{
			camera.position =
				target->scene_position
				+ target->scene_orientation
					* glm::vec3{
						-3500.0f, -1000.0f, -3000.0f};
			camera.orientation = look_at_zero_roll(
				camera.position, target->scene_position);
		}
		return;
	case 36:
		if (player != nullptr)
		{
			service_flyby_pose(camera, *player);
		}
		return;
	case 37:
		service_land_type13_timed_camera(camera, world);
		return;
	case 39:
		if (target != nullptr)
		{
			camera.orientation =
				look_at_zero_roll(
					camera.position, target->scene_position);
		}
		return;
	case 40:
		if (target != nullptr)
		{
			const float phase =
				static_cast<float>(
					static_cast<std::int32_t>(
						camera.target_command_expiry_tick
							- now_tick))
				* (1.0f / 300.0f);
			const float angle =
				std::sin(phase * kPi)
				* (1.0f - phase) * kPi;
			glm::mat3 orbit = math::postrotate(
				target->scene_orientation,
				angle,
				{0.0f, 1.0f, 0.0f});
			camera.position =
				target->scene_position
				+ orbit * glm::vec3{0.0f, 200.0f, 1150.0f};
			camera.orientation =
				look_at_zero_roll(
					camera.position, target->scene_position);
		}
		return;
	case 41:
	case 42:
		if (target != nullptr)
		{
			const glm::vec3 local = camera.mode == 41
				? glm::vec3{0.0f, -700.0f, 800.0f}
				: glm::vec3{1500.0f, 0.0f, -300.0f};
			camera.position =
				target->scene_position
					+ target->scene_orientation * local;
			camera.orientation =
				look_at_zero_roll(
					camera.position, target->scene_position);
		}
		return;
	case 43:
		service_dock_door_camera(camera, world);
		return;
	default:
		return;
	}
}
}

void camera_runtime_publish_world_state(
	const CameraRuntime& camera,
	World& world)
{
	world.camera_mode = static_cast<std::uint8_t>(camera.mode);
	world.multiplayer_spectator_target = camera.spectator_cursor;
	world.multiplayer_spectator_active = camera.spectator_active;
}

bool camera_runtime_select_next_spectator(
	CameraRuntime& camera,
	World& world,
	const MissileRuntime& missiles,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_ticks,
	std::uint32_t simulation_tick)
{
	// Spectator_select_next, LANCER.EXE 0x004775d0..0x00477662.
	// The retail selector is bounded by DAT_0058832c, not the 400-slot
	// allocation table, and tests only the local slot and flags 0x460.
	const std::uint16_t player_count = std::min<std::uint16_t>(
		world.player_prefix_count,
		static_cast<std::uint16_t>(std::size(world.objects)));
	for (std::uint16_t attempt = 0; attempt < player_count; ++attempt)
	{
		++camera.spectator_cursor;
		if (camera.spectator_cursor == player_count)
		{
			camera.spectator_cursor = 0;
		}
		const WorldObject& candidate =
			world.objects[camera.spectator_cursor];
		if (camera.spectator_cursor == world.player.index
			|| (candidate.runtime_flags & kObjectSpatialQueryExcludedFlags) != 0)
		{
			continue;
		}
		camera.spectator_complete = false;
		const bool switched = camera_runtime_request(
			camera,
			world,
			missiles,
			stats,
			30,
			camera.spectator_cursor,
			true,
			true,
			frame_ticks,
			simulation_tick);
		// The selector clears only cinematic state four, and does so after
		// the camera request even when Camera_switch_mode rejects it.
		if (world.cinematic_mode == 4)
		{
			world.cinematic_mode = 0;
		}
		camera_runtime_publish_world_state(camera, world);
		return switched;
	}

	// No eligible peer writes the local index and terminal latch. The
	// mission owner translates spectator_complete to its retained latch.
	camera.spectator_cursor = world.player.index;
	camera.spectator_complete = true;
	camera_runtime_publish_world_state(camera, world);
	return false;
}

void camera_runtime_reset(
	CameraRuntime& camera,
	World& world,
	const assets::ShipStatsTable& stats,
	std::uint32_t simulation_tick)
{
	camera = {};
	camera.follow_distance = 1500.0f;
	camera.horizontal_tangent =
		0.5f / 0.6000000238418579f;
	camera.vertical_tangent = 0.625f;
	camera.target = world.player.index;
	camera.current_time_tick = simulation_tick;
	camera.installed_tick = camera.current_time_tick;
	if (WorldObject* player = resolve_index(world, camera.target))
	{
		player->runtime_flags |= kObjectFlagRenderSuppressed;
		service_cockpit_pose(
			camera, world, *player, stats, 0.0f);
	}
	camera.installed_orientation = camera.orientation;
	camera.previous_position = camera.position;
	camera.previous_orientation = camera.orientation;
	// mission_runtime_init creates the hidden camera staging object at
	// authored world position (0,0,-8000).
	camera.staged_position = {0.0f, 0.0f, -8000.0f};
	camera.staged_orientation = glm::mat3{1.0f};
	camera_runtime_publish_world_state(camera, world);
}

bool camera_runtime_request(
	CameraRuntime& camera,
	World& world,
	const MissileRuntime& missiles,
	const assets::ShipStatsTable& stats,
	std::int16_t mode,
	std::uint16_t target,
	bool lock,
	bool override_lock,
	std::uint32_t frame_ticks,
	std::uint32_t simulation_tick,
	bool accept_departed_target)
{
	if (camera.switch_guard)
	{
		return false;
	}
	if (camera.locked && !override_lock)
	{
		return false;
	}
	const bool reinstall_current_mode = mode == -1;
	if (mode == -1)
	{
		mode = camera.mode;
	}
	if (mode < 0 || mode > 43)
	{
		return false;
	}
	WorldObject* target_object = resolve_index(world, target);
	// Camera_switch_mode checks the multiplayer dead-local redirect at
	// 0x0045f1ea..0x0045f220 before rejecting a departed type-1001 target
	// at 0x0045f22b..0x0045f247. The local slot is already that placeholder
	// when ordinary multiplayer death reaches spectator selection.
	if (camera.multiplayer
		&& mode == 0
		&& target == world.player.index
		&& target_object != nullptr
		&& (target_object->runtime_flags & 0x10000840u) != 0)
	{
		// Camera_switch_mode clears the camera lock at 0x0045f212 before
		// entering Spectator_select_next. Its callers, rather than the
		// selector, own the persistent DAT_0057e058 spectator latch.
		camera.locked = false;
		(void)camera_runtime_select_next_spectator(
			camera,
			world,
			missiles,
			stats,
			frame_ticks,
			simulation_tick);
		// Camera_switch_mode returns true after the redirect regardless of
		// whether the selector found a candidate.
		return true;
	}
	// Ordinary requests observe the retail departed-target rejection.
	// A bridged death request was issued synchronously while this object was
	// still live; Explode can install its placeholder before the session sees
	// that request, but preserves the pose needed by modes 8, 26, and 27.
	if (target_object != nullptr
		&& target_object->type == 1001
		&& !accept_departed_target)
	{
		return false;
	}

	std::uint16_t selected_missile = UINT16_MAX;
	std::uint16_t next_cursor = camera.missile_cursor;
	if (mode == 18)
	{
		for (std::uint16_t attempt = 0;
			attempt < std::size(missiles.missiles);
			++attempt)
		{
			next_cursor = static_cast<std::uint16_t>(
				(next_cursor + 1) % std::size(missiles.missiles));
			const Missile& missile = missiles.missiles[next_cursor];
			if (missile.active
				&& missile.type >= 0
				&& missile.shooter.index == target)
			{
				selected_missile = next_cursor;
				break;
			}
		}
		if (selected_missile == UINT16_MAX)
		{
			return false;
		}
	}

	const std::int16_t previous_mode = camera.mode;
	const std::uint16_t previous_target = camera.target;
	if (WorldObject* previous = resolve_index(world, previous_target))
	{
		previous->runtime_flags &= ~kObjectFlagRenderSuppressed;
	}
	camera.mode = mode;
	camera.target = target;
	camera.locked = lock;
	camera.installed_tick = camera.current_time_tick;
	camera.installed_orientation = camera.orientation;
	camera.previous_position = camera.position;
	camera.previous_orientation = camera.orientation;
	if (!reinstall_current_mode)
	{
		camera.horizontal_tangent = mode == 32
			? 0.5f / 0.34999999403953552f
			: 0.5f / 0.6000000238418579f;
		camera.vertical_tangent = mode == 32
			? 0.5f / 0.46700000762939453f
			: 0.625f;
		const bool projection_transition =
			(mode >= 7 && mode <= 11)
			|| (mode >= 13 && mode <= 39)
			|| mode == 43;
		if (projection_transition)
		{
			camera.projection_rate = 1.0f;
		}
		else
		{
			camera.projection_progress = 0.0f;
			camera.projection_rate = 0.0f;
		}
	}
	if (mode >= 0 && mode <= 3 && target_object != nullptr
		&& !(mode == 0 && camera.view_state == 2))
	{
		target_object->runtime_flags |= kObjectFlagRenderSuppressed;
	}
	if (mode == 9)
	{
		camera.warp_camera_scalar = -5000.0f;
	}
	if (mode == 40)
	{
		camera.target_command_expiry_tick =
			target_object != nullptr
				&& target_object->ai.command_count != 0
				&& target_object->ai.commands[0].id == 121
			? target_object->ai.work.deadline
			: camera.current_time_tick;
	}
	if (mode == 4 || mode == 30
		|| (mode == 0 && camera.view_state == 2))
	{
		camera.smooth_pitch = 0.0f;
		camera.smooth_yaw = 0.0f;
		camera.smooth_roll = 0.0f;
		if (mode == 4
			&& (previous_mode != 4 || previous_target != target))
		{
			camera.follow_distance = 1500.0f;
		}
	}
	if (mode == 6 || mode == 12)
	{
		camera.chase_distance = 0.0f;
		camera.chase_yaw_degrees = 0.0f;
		camera.chase_pitch_degrees = 0.0f;
		camera.chase_yaw_velocity = 0.0f;
		camera.chase_pitch_velocity = 0.0f;
	}
	if (mode == 7 && target_object != nullptr)
	{
		camera.orbit_vector =
			target_object->scene_orientation[0] * 5000.0f;
	}
	if (mode == 18)
	{
		camera.missile_cursor = selected_missile;
		camera.active_missile = selected_missile;
		camera.missile_phase = 0;
	}
	const WorldObject* player = world_resolve(world, world.player);
	if (mode == 15 && player != nullptr)
	{
		camera.orientation = glm::mat3{1.0f};
		camera.orientation =
			look_at_zero_roll(
				camera.position,
				player->scene_position);
	}
	else if (mode == 16 && player != nullptr)
	{
		camera.position =
			player->scene_position
				+ player->scene_orientation
					* glm::vec3{0.0f, -600.0f, 40000.0f};
		camera.orientation = math::postrotate(
			player->scene_orientation,
			0.33f,
			{1.0f, 0.0f, 0.0f});
		camera.orientation = math::postrotate(
			camera.orientation,
			kPi,
			{0.0f, 1.0f, 0.0f});
	}
	else if (mode == 17 && player != nullptr)
	{
		camera.position =
			player->scene_position
				+ player->scene_orientation
					* glm::vec3{-800.0f, 200.0f, 5000.0f};
	}
	else if (mode == 19 && target_object != nullptr)
	{
		if (player != nullptr)
		{
			finalize_fixed_pose(
				camera,
				*player,
				*target_object,
				{-300.0f, -400.0f, -1400.0f});
		}
	}
	else if (mode == 20 && target_object != nullptr)
	{
		if (player != nullptr)
		{
			finalize_fixed_pose(
				camera,
				*player,
				*target_object,
				{0.0f, 800.0f, -600.0f});
		}
	}
	else if (mode == 21 && target_object != nullptr)
	{
		if (player != nullptr)
		{
			finalize_fixed_pose(
				camera,
				*player,
				*target_object,
				{0.0f, -300.0f, 45000.0f});
		}
	}
	else if (mode == 23 && player != nullptr)
	{
		camera.position =
			player->scene_position
				+ player->scene_orientation
					* glm::vec3{0.0f, -500.0f, 50000.0f};
		if (player != nullptr)
		{
			camera.orientation =
				look_at_zero_roll(
					camera.position, player->scene_position);
		}
	}
	else if (mode == 24 && target_object != nullptr)
	{
		if (player != nullptr)
		{
			finalize_fixed_pose(
				camera,
				*player,
				*target_object,
				{0.0f, 300.0f, 27000.0f});
		}
	}
	else if (mode == 25 && target_object != nullptr)
	{
		if (player != nullptr)
		{
			finalize_fixed_pose(
				camera,
				*player,
				*target_object,
				{-2000.0f, -100.0f, 25000.0f});
		}
	}
	else if (mode == 26 && player != nullptr)
	{
		camera.position.x += player->radius;
	}
	else if (mode == 28 && target_object != nullptr && player != nullptr)
	{
		camera.orbit_vector =
			normalized_or_retail_zero(
				player->scene_position
					- target_object->scene_position);
		camera.orbit_vector *= 0.5f;
	}
	else if (mode == 29 && player != nullptr && target_object != nullptr)
	{
		camera.orbit_vector =
			normalized_or_retail_zero(
				player->scene_position
					- target_object->scene_position);
	}
	else if (mode == 32 && player != nullptr)
	{
		const ai::LaunchWork& work = player->ai.work.launch;
		const float side =
			(work.launch_point & 1) == 0 ? -750.0f : 750.0f;
		camera.position =
			player->scene_position
				+ player->scene_orientation
					* glm::vec3{side, -600.0f, -300.0f};
		camera.orientation = player->scene_orientation;
	}
	else if (mode == 33 && player != nullptr)
	{
		camera.position =
			player->scene_position
				+ player->scene_orientation
					* glm::vec3{600.0f, 10000.0f, 100.0f};
	}
	else if (mode == 34 && target_object != nullptr)
	{
		camera.position =
			target_object->scene_position
				+ target_object->scene_orientation
					* glm::vec3{-1700.0f, 1500.0f, 0.0f};
		camera.orientation = target_object->scene_orientation;
	}
	else if (mode == 36 && player != nullptr)
	{
		camera.position =
			player->scene_position
				+ player->scene_orientation
					* glm::vec3{
						0.0f,
						player->radius,
						player->radius * 4.0f};
	}
	else if (mode == 39 && target_object != nullptr)
	{
		const glm::vec3 local{8000.0f};
		camera.position =
			target_object->scene_position
				+ target_object->scene_orientation * local;
	}
	service_current(
		camera,
		world,
		missiles,
		stats,
		frame_ticks,
		simulation_tick);
	camera_runtime_publish_world_state(camera, world);
	return true;
}

void camera_runtime_service(
	CameraRuntime& camera,
	World& world,
	const MissileRuntime& missiles,
	const assets::ShipStatsTable& stats,
	std::uint32_t frame_ticks,
	std::uint32_t simulation_tick)
{
	camera.previous_position = camera.position;
	camera.previous_orientation = camera.orientation;
	service_current(
		camera,
		world,
		missiles,
		stats,
		frame_ticks,
		simulation_tick);
}
}
