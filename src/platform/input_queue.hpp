#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

namespace sl_open::platform
{
struct InputEvent
{
	static constexpr std::uint32_t kTextCapacity = 64;

	struct Key
	{
		SDL_Scancode scancode{};
		SDL_Keymod mod{};
		bool repeat{};
	} key;
	struct Text
	{
		char text[kTextCapacity]{};
	} text;
	struct Motion
	{
		float x{};
		float y{};
		float x_relative{};
		float y_relative{};
		SDL_MouseButtonFlags state{};
	} motion;
	struct Button
	{
		float x{};
		float y{};
		std::uint8_t button{};
	} button;
	struct Window
	{
		std::int32_t data1{};
		std::int32_t data2{};
	} window;
	struct Wheel
	{
		float x{};
		float y{};
		std::int32_t integer_x{};
		std::int32_t integer_y{};
	} wheel;
	struct ControllerButton
	{
		SDL_JoystickID which{};
		std::uint8_t button{};
	} gbutton, jbutton;
	struct ControllerAxis
	{
		SDL_JoystickID which{};
		std::uint8_t axis{};
		std::int16_t value{};
	} gaxis, jaxis;
	struct ControllerHat
	{
		SDL_JoystickID which{};
		std::uint8_t hat{};
		std::uint8_t value{SDL_HAT_CENTERED};
	} jhat;
	struct ControllerDevice
	{
		SDL_JoystickID which{};
	} gdevice, jdevice;

	std::uint32_t type{};
};

struct InputQueue
{
	static constexpr std::uint16_t kCapacity = 256;

	InputEvent events[kCapacity]{};
	std::uint16_t read{};
	std::uint16_t count{};
	bool overflow_reported{};
};

bool input_queue_push(InputQueue& queue, const SDL_Event& event);
bool input_queue_pop(InputQueue& queue, InputEvent& event);
}
