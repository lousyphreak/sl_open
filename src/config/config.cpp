#include "config/config.hpp"

#include "input/controls.hpp"

#include <SDL3/SDL.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sl_open
{
namespace
{
bool parse_u32(const char* text, std::uint32_t& value)
{
	if (text == nullptr || *text == '\0' || *text == '-')
	{
		return false;
	}
	errno = 0;
	char* end = nullptr;
	const unsigned long parsed = std::strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX)
	{
		return false;
	}
	value = static_cast<std::uint32_t>(parsed);
	return true;
}

bool in_range(
	const char* text,
	std::uint32_t minimum,
	std::uint32_t maximum,
	std::uint32_t& value)
{
	std::uint32_t parsed = 0;
	if (!parse_u32(text, parsed) || parsed < minimum || parsed > maximum)
	{
		return false;
	}
	value = parsed;
	return true;
}

void copy_text(char* destination, std::size_t capacity, const char* text)
{
	if (capacity == 0)
	{
		return;
	}
	std::snprintf(destination, capacity, "%s", text);
}

void parse_setting(Config& config, const char* key, const char* value)
{
	std::uint32_t parsed = 0;
	if (std::strcmp(key, "multiplayer_address") == 0)
	{
		copy_text(
			config.multiplayer_address,
			sizeof(config.multiplayer_address),
			value);
	}
	else if (std::strcmp(key, "display_width") == 0
		&& in_range(value, 640, 16384, parsed))
	{
		config.display_width = parsed;
	}
	else if (std::strcmp(key, "display_height") == 0
		&& in_range(value, 480, 16384, parsed))
	{
		config.display_height = parsed;
	}
	else if (std::strcmp(key, "display_mode") == 0
		&& in_range(value, 0, 2, parsed))
	{
		config.display_mode = static_cast<DisplayMode>(parsed);
	}
	else if (std::strcmp(key, "vsync") == 0
		&& in_range(value, 0, 1, parsed))
	{
		config.vsync = parsed != 0;
	}
	else if (std::strcmp(key, "brightness") == 0
		&& in_range(value, 50, 200, parsed))
	{
		config.brightness = static_cast<std::uint8_t>(parsed);
	}
	else if (std::strcmp(key, "texture_detail") == 0
		&& in_range(value, 0, 1, parsed))
	{
		config.texture_detail = static_cast<std::uint8_t>(parsed);
	}
	else if (std::strcmp(key, "graphics_detail") == 0
		&& in_range(value, 0, 2, parsed))
	{
		config.graphics_detail = static_cast<std::uint8_t>(parsed);
	}
	else if (std::strcmp(key, "light_maps") == 0
		&& in_range(value, 0, 1, parsed))
	{
		config.light_maps = parsed != 0;
	}
	else if (std::strcmp(key, "default_view") == 0
		&& in_range(value, 0, 2, parsed))
	{
		config.default_view = static_cast<std::uint8_t>(parsed);
	}
	else if (std::strcmp(key, "transitions") == 0
		&& in_range(value, 0, 1, parsed))
	{
		config.transitions = parsed != 0;
	}
	else if (std::strcmp(key, "expand_widescreen_movies") == 0
		&& in_range(value, 0, 1, parsed))
	{
		config.expand_widescreen_movies = parsed != 0;
	}
	else if (std::strcmp(key, "pause_in_background") == 0
		&& in_range(value, 0, 1, parsed))
	{
		config.pause_in_background = parsed != 0;
	}
	else if (std::strcmp(key, "positional_audio") == 0
		&& in_range(value, 0, 2, parsed))
	{
		config.positional_audio = static_cast<PositionalAudio>(parsed);
	}
	else if (std::strcmp(key, "effects_volume") == 0
		&& in_range(value, 0, 127, parsed))
	{
		config.effects_volume = static_cast<std::uint8_t>(parsed);
	}
	else if (std::strcmp(key, "music_volume") == 0
		&& in_range(value, 0, 127, parsed))
	{
		config.music_volume = static_cast<std::uint8_t>(parsed);
	}
	else if (std::strcmp(key, "speech_volume") == 0
		&& in_range(value, 0, 127, parsed))
	{
		config.speech_volume = static_cast<std::uint8_t>(parsed);
	}
	else if (std::strcmp(key, "master_volume") == 0
		&& in_range(value, 0, 127, parsed))
	{
		config.master_volume = static_cast<std::uint8_t>(parsed);
	}
	else if (std::strcmp(key, "controller") == 0
		&& in_range(value, 0, 3, parsed))
	{
		config.controller = static_cast<std::uint8_t>(parsed);
	}
	else if (std::strcmp(key, "force_feedback") == 0
		&& in_range(value, 0, 1, parsed))
	{
		config.force_feedback = parsed != 0;
	}
	else if (std::strcmp(key, "invert_pitch") == 0
		&& in_range(value, 0, 1, parsed))
	{
		config.invert_pitch = parsed != 0;
	}
	else if (std::strcmp(key, "hat_control") == 0
		&& in_range(value, 0, 1, parsed))
	{
		config.hat_control = parsed != 0;
	}
	else if (std::strcmp(key, "twist_control") == 0
		&& in_range(value, 0, 1, parsed))
	{
		config.twist_control = parsed != 0;
	}
	else if (std::strncmp(key, "binding_", 8) == 0
		&& std::strlen(key) == 10
		&& key[8] >= '0' && key[8] <= '9'
		&& key[9] >= '0' && key[9] <= '9')
	{
		const std::uint32_t index =
			static_cast<std::uint32_t>(key[8] - '0') * 10
			+ static_cast<std::uint32_t>(key[9] - '0');
		unsigned scancode = 0;
		unsigned modifier = 0;
		int joystick = -1;
		int mouse = index < kControlActionCount
			? config.bindings[index].mouse_control
			: -1;
		const int fields = std::sscanf(
			value,
			"%u,%u,%d,%d",
			&scancode,
			&modifier,
			&joystick,
			&mouse);
		if (index < kControlActionCount
			&& fields >= 3
			&& scancode < SDL_SCANCODE_COUNT
			&& modifier <= 3
			&& joystick >= -1 && joystick <= INT16_MAX
			&& mouse >= -1 && mouse <= kControlMouseWheelRight)
		{
			config.bindings[index].scancode =
				static_cast<std::uint16_t>(scancode);
			config.bindings[index].modifier =
				static_cast<std::uint8_t>(modifier);
			config.bindings[index].joystick_control =
				static_cast<std::int16_t>(joystick);
			config.bindings[index].mouse_control =
				static_cast<std::int16_t>(mouse);
		}
	}
}
}

void config_defaults(Config& config)
{
	config = {};
	config.display_width = 960;
	config.display_height = 720;
	config.effects_volume = 80;
	config.music_volume = 80;
	config.speech_volume = 127;
	config.master_volume = 127;
	config.texture_detail = 1;
	config.graphics_detail = 2;
	config.default_view = 0;
	config.brightness = 100;
	config.display_mode = DisplayMode::windowed;
	config.positional_audio = PositionalAudio::standard;
	config.vsync = true;
	config.light_maps = true;
	config.transitions = true;
	config.expand_widescreen_movies = true;
	config.pause_in_background = true;
	controls_defaults(config);
}

bool config_load(SDL_EMFS_Context* filesystem, Config& config)
{
	std::size_t size = 0;
	auto* text = static_cast<char*>(SDL_EMFS_LoadFile(
		filesystem, SDL_EMFS_ROOT_USER, "config.cfg", &size));
	if (text == nullptr)
	{
		return false;
	}

	char* cursor = text;
	char* const end = text + size;
	while (cursor < end)
	{
		char* line = cursor;
		while (cursor < end && *cursor != '\r' && *cursor != '\n')
		{
			++cursor;
		}
		if (cursor < end)
		{
			*cursor++ = '\0';
			while (cursor < end && (*cursor == '\r' || *cursor == '\n'))
			{
				++cursor;
			}
		}
		if (line[0] == '\0' || line[0] == '#')
		{
			continue;
		}
		char* separator = std::strchr(line, '=');
		if (separator == nullptr)
		{
			continue;
		}
		*separator = '\0';
		parse_setting(config, line, separator + 1);
	}
	SDL_free(text);
	return true;
}

bool config_save(SDL_EMFS_Context* filesystem, const Config& config)
{
	SDL_IOStream* file = SDL_IOFromDynamicMem();
	if (file == nullptr)
	{
		return false;
	}
	bool write_complete = SDL_IOprintf(
		file,
		"# sl_open configuration\n"
		"multiplayer_address=%s\n"
		"display_width=%u\n"
		"display_height=%u\n"
		"display_mode=%u\n"
		"vsync=%u\n"
		"brightness=%u\n"
		"texture_detail=%u\n"
		"graphics_detail=%u\n"
		"light_maps=%u\n"
		"default_view=%u\n"
		"transitions=%u\n"
		"expand_widescreen_movies=%u\n"
		"pause_in_background=%u\n"
		"positional_audio=%u\n"
		"effects_volume=%u\n"
		"music_volume=%u\n"
		"speech_volume=%u\n"
		"master_volume=%u\n"
		"controller=%u\n"
		"force_feedback=%u\n"
		"invert_pitch=%u\n"
		"hat_control=%u\n"
		"twist_control=%u\n",
		config.multiplayer_address,
		config.display_width,
		config.display_height,
		static_cast<unsigned>(config.display_mode),
		config.vsync ? 1u : 0u,
		config.brightness,
		config.texture_detail,
		config.graphics_detail,
		config.light_maps ? 1u : 0u,
		config.default_view,
		config.transitions ? 1u : 0u,
		config.expand_widescreen_movies ? 1u : 0u,
		config.pause_in_background ? 1u : 0u,
		static_cast<unsigned>(config.positional_audio),
		config.effects_volume,
		config.music_volume,
		config.speech_volume,
		config.master_volume,
		config.controller,
		config.force_feedback ? 1u : 0u,
		config.invert_pitch ? 1u : 0u,
		config.hat_control ? 1u : 0u,
		config.twist_control ? 1u : 0u) > 0;
	for (std::uint32_t index = 0;
		index < kControlActionCount && write_complete;
		++index)
	{
		const ControlBinding& binding = config.bindings[index];
		write_complete = SDL_IOprintf(
			file,
			"binding_%02u=%u,%u,%d,%d\n",
			index,
			binding.scancode,
			binding.modifier,
			binding.joystick_control,
			binding.mouse_control) > 0;
	}
	const Sint64 size = SDL_GetIOSize(file);
	const void* data = SDL_GetPointerProperty(
		SDL_GetIOProperties(file),
		SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER,
		nullptr);
	const bool complete = write_complete && size >= 0 && data != nullptr
		&& SDL_EMFS_SaveUserFile(
			filesystem, "config.cfg.tmp", data, static_cast<std::size_t>(size))
		&& SDL_EMFS_RenameUserPath(
			filesystem, "config.cfg.tmp", "config.cfg");
	SDL_CloseIO(file);
	if (!complete)
	{
		SDL_EMFS_RemoveUserPath(filesystem, "config.cfg.tmp");
	}
	return complete;
}
}
