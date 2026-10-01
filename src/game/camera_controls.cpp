#include "game/camera_runtime.hpp"

#include "input/gameplay_input.hpp"

#include <algorithm>
#include <cstdint>

namespace sl_open::game
{
namespace
{
void update_orbit_controls(
	CameraRuntime& camera,
	const input::GameplayInput& input,
	std::uint32_t frame_ticks)
{
	if (camera.mode != 6 && camera.mode != 12)
	{
		return;
	}

	const float delta = static_cast<float>(frame_ticks);
	if (input.camera_yaw_negative)
	{
		camera.chase_yaw_velocity -= delta * 0.1f;
	}
	else if (input.camera_yaw_positive)
	{
		camera.chase_yaw_velocity += delta * 0.1f;
	}
	camera.chase_yaw_velocity = std::clamp(
		camera.chase_yaw_velocity, -5.0f, 5.0f);
	if (camera.chase_yaw_velocity > 0.0f)
	{
		camera.chase_yaw_velocity -= delta * 0.05f;
		if (camera.chase_yaw_velocity < 0.0f)
		{
			camera.chase_yaw_velocity = 0.0f;
		}
	}
	else if (camera.chase_yaw_velocity < 0.0f)
	{
		camera.chase_yaw_velocity += delta * 0.05f;
		if (camera.chase_yaw_velocity > 0.0f)
		{
			camera.chase_yaw_velocity = 0.0f;
		}
	}
	camera.chase_yaw_degrees +=
		delta * camera.chase_yaw_velocity;
	if (camera.chase_yaw_degrees >= 0.0f)
	{
		if (camera.chase_yaw_degrees > 360.0f)
		{
			camera.chase_yaw_degrees -= 360.0f;
		}
	}
	else
	{
		camera.chase_yaw_degrees += 360.0f;
	}

	if (input.camera_zoom_in)
	{
		camera.chase_distance -= delta * 60.0f;
	}
	else if (input.camera_zoom_out)
	{
		camera.chase_distance += delta * 60.0f;
	}
	else if (input.camera_pitch_positive)
	{
		camera.chase_pitch_velocity += delta * 0.1f;
	}
	else if (input.camera_pitch_negative)
	{
		camera.chase_pitch_velocity -= delta * 0.1f;
	}
	camera.chase_pitch_velocity = std::clamp(
		camera.chase_pitch_velocity, -5.0f, 5.0f);
	if (camera.chase_pitch_velocity > 0.0f)
	{
		camera.chase_pitch_velocity -= delta * 0.05f;
		if (camera.chase_pitch_velocity < 0.0f)
		{
			camera.chase_pitch_velocity = 0.0f;
		}
	}
	else if (camera.chase_pitch_velocity < 0.0f)
	{
		camera.chase_pitch_velocity += delta * 0.05f;
		if (camera.chase_pitch_velocity > 0.0f)
		{
			camera.chase_pitch_velocity = 0.0f;
		}
	}
	camera.chase_pitch_degrees = std::clamp(
		camera.chase_pitch_degrees
			+ delta * camera.chase_pitch_velocity,
		-89.5f,
		89.5f);
	if (camera.chase_pitch_degrees == -89.5f
		|| camera.chase_pitch_degrees == 89.5f)
	{
		camera.chase_pitch_velocity = 0.0f;
	}
}
}

std::int16_t camera_runtime_update_user_controls(
	CameraRuntime& camera,
	const input::GameplayInput& input,
	std::uint32_t frame_ticks)
{
	// Player_update_controls services orbit movement before camera selection,
	// so a view-change key cannot move the newly installed camera until the
	// next admitted gameplay update.
	update_orbit_controls(camera, input, frame_ticks);

	std::int16_t request = input.camera_hat_mode;
	const bool hat_direction = request >= 0;
	if (camera.hat_camera_active && !hat_direction)
	{
		request = 0;
	}
	camera.hat_camera_active = hat_direction;

	// Retail polls all eight actions independently in this order. A later
	// pressed action wins, while the mode-zero view-state cycle still occurs
	// even when a later camera action replaces its request.
	if (input.pressed[0])
	{
		if (camera.mode == 0 && !camera.locked)
		{
			camera.view_state = static_cast<std::uint8_t>(
				(camera.view_state + 1u) % 3u);
			if (camera.view_state == 2)
			{
				camera.follow_distance = 1500.0f;
			}
		}
		request = 0;
	}
	if (input.pressed[1]) request = 1;
	if (input.pressed[2]) request = 2;
	if (input.pressed[3]) request = 3;
	if (input.pressed[4]) request = 36;
	if (input.pressed[5]) request = 6;
	if (input.pressed[6]) request = 12;
	if (input.pressed[7]) request = 18;
	return request;
}
}
