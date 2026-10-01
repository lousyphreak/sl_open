#pragma once

#include "config/config.hpp"
#include "game/world.hpp"
#include "input/gameplay_input.hpp"

#include <cstdint>

namespace sl_open::game
{
struct PlayerControlState
{
	std::int16_t mouse_displacement_x{};
	std::int16_t mouse_displacement_y{};
	float mouse_fraction_x{};
	float mouse_fraction_y{};
};

struct PlayerControlAxes
{
	float horizontal{};
	float vertical{};
};

PlayerControlAxes player_controls_update_flight_demand(
	const Config& config,
	const input::GameplayInput& input,
	PlayerControlState& state,
	FlightDemand& demand,
	float& manual_throttle,
	std::int16_t camera_mode,
	bool interface_owns_rotation);
}
