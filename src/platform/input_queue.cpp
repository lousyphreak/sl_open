#include "platform/input_queue.hpp"

#include <cstdio>

namespace sl_open::platform
{
bool input_queue_push(InputQueue& queue, const SDL_Event& source)
{
	switch (source.type)
	{
	case SDL_EVENT_QUIT:
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP:
	case SDL_EVENT_TEXT_INPUT:
	case SDL_EVENT_MOUSE_MOTION:
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_RESIZED:
	case SDL_EVENT_WINDOW_FOCUS_GAINED:
	case SDL_EVENT_WINDOW_FOCUS_LOST:
	case SDL_EVENT_MOUSE_WHEEL:
	case SDL_EVENT_JOYSTICK_ADDED:
	case SDL_EVENT_JOYSTICK_REMOVED:
	case SDL_EVENT_GAMEPAD_ADDED:
	case SDL_EVENT_GAMEPAD_REMOVED:
	case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
	case SDL_EVENT_JOYSTICK_BUTTON_UP:
	case SDL_EVENT_JOYSTICK_AXIS_MOTION:
	case SDL_EVENT_JOYSTICK_HAT_MOTION:
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		break;
	default:
		return true;
	}
	if (queue.count == InputQueue::kCapacity)
	{
		return false;
	}

	const std::uint16_t write = static_cast<std::uint16_t>(
		(queue.read + queue.count) % InputQueue::kCapacity);
	InputEvent& event = queue.events[write];
	event = {};
	event.type = source.type;
	switch (source.type)
	{
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP:
		event.key.scancode = source.key.scancode;
		event.key.mod = source.key.mod;
		event.key.repeat = source.key.repeat;
		break;
	case SDL_EVENT_TEXT_INPUT:
		std::snprintf(
			event.text.text,
			InputEvent::kTextCapacity,
			"%s",
			source.text.text != nullptr ? source.text.text : "");
		break;
	case SDL_EVENT_MOUSE_MOTION:
		event.motion.x = source.motion.x;
		event.motion.y = source.motion.y;
		event.motion.x_relative = source.motion.xrel;
		event.motion.y_relative = source.motion.yrel;
		event.motion.state = source.motion.state;
		break;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
		event.button.x = source.button.x;
		event.button.y = source.button.y;
		event.button.button = source.button.button;
		break;
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_RESIZED:
		event.window.data1 = source.window.data1;
		event.window.data2 = source.window.data2;
		break;
	case SDL_EVENT_MOUSE_WHEEL:
	{
		const float sign = source.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
			? -1.0f
			: 1.0f;
		event.wheel.x = source.wheel.x * sign;
		event.wheel.y = source.wheel.y * sign;
		event.wheel.integer_x = source.wheel.integer_x
			* static_cast<std::int32_t>(sign);
		event.wheel.integer_y = source.wheel.integer_y
			* static_cast<std::int32_t>(sign);
		break;
	}
	case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
	case SDL_EVENT_JOYSTICK_BUTTON_UP:
		event.jbutton.which = source.jbutton.which;
		event.jbutton.button = source.jbutton.button;
		break;
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
		event.gbutton.which = source.gbutton.which;
		event.gbutton.button = source.gbutton.button;
		break;
	case SDL_EVENT_JOYSTICK_AXIS_MOTION:
		event.jaxis.which = source.jaxis.which;
		event.jaxis.axis = source.jaxis.axis;
		event.jaxis.value = source.jaxis.value;
		break;
	case SDL_EVENT_JOYSTICK_HAT_MOTION:
		event.jhat.which = source.jhat.which;
		event.jhat.hat = source.jhat.hat;
		event.jhat.value = source.jhat.value;
		break;
	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		event.gaxis.which = source.gaxis.which;
		event.gaxis.axis = source.gaxis.axis;
		event.gaxis.value = source.gaxis.value;
		break;
	case SDL_EVENT_JOYSTICK_ADDED:
	case SDL_EVENT_JOYSTICK_REMOVED:
		event.jdevice.which = source.jdevice.which;
		break;
	case SDL_EVENT_GAMEPAD_ADDED:
	case SDL_EVENT_GAMEPAD_REMOVED:
		event.gdevice.which = source.gdevice.which;
		break;
	default:
		break;
	}
	++queue.count;
	return true;
}

bool input_queue_pop(InputQueue& queue, InputEvent& event)
{
	if (queue.count == 0)
	{
		return false;
	}
	event = queue.events[queue.read];
	queue.read = static_cast<std::uint16_t>(
		(queue.read + 1) % InputQueue::kCapacity);
	--queue.count;
	return true;
}
}
