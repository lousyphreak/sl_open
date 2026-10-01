#include "platform/display.hpp"

#include <cstdint>
#include <cstring>

namespace sl_open::platform
{
namespace
{
constexpr bgfx::ViewId kMainView = 0;

bool set_native_window(bgfx::PlatformData& platform, SDL_Window* window)
{
#if defined(__EMSCRIPTEN__)
	(void)window;
	platform.nwh = const_cast<char*>("#canvas");
	return true;
#else
	const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
	if (properties == 0)
	{
		return false;
	}

#if defined(_WIN32)
	platform.nwh = SDL_GetPointerProperty(
		properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
	return platform.nwh != nullptr;
#elif defined(__linux__)
	const char* driver = SDL_GetCurrentVideoDriver();
	if (driver != nullptr && std::strcmp(driver, "wayland") == 0)
	{
		platform.ndt = SDL_GetPointerProperty(
			properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
		platform.nwh = SDL_GetPointerProperty(
			properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
		platform.type = bgfx::NativeWindowHandleType::Wayland;
	}
	else
	{
		platform.ndt = SDL_GetPointerProperty(
			properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
		const Sint64 x11_window = SDL_GetNumberProperty(
			properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
		platform.nwh = reinterpret_cast<void*>(
			static_cast<std::uintptr_t>(x11_window));
		platform.type = bgfx::NativeWindowHandleType::Default;
	}
	return platform.ndt != nullptr && platform.nwh != nullptr;
#else
#error Add the SDL native-window handoff before enabling this platform.
#endif
#endif
}

void enumerate_display_modes(App& app)
{
	app.display_modes = {};
	const SDL_DisplayID display = SDL_GetDisplayForWindow(app.window);
	int count = 0;
	SDL_DisplayMode** modes =
		SDL_GetFullscreenDisplayModes(display, &count);
	for (int index = 0; modes != nullptr && index < count; ++index)
	{
		if (modes[index] != nullptr)
		{
			display_add_resolution(
				app.display_modes,
				static_cast<std::uint32_t>(modes[index]->w),
				static_cast<std::uint32_t>(modes[index]->h));
		}
	}
	SDL_free(modes);
	display_add_resolution(
		app.display_modes,
		app.config.display_width,
		app.config.display_height);
	if (app.display_modes.count == 0)
	{
		display_add_resolution(app.display_modes, 960, 720);
	}
}

bool init_bgfx(App& app)
{
	int pixel_width = 0;
	int pixel_height = 0;
	if (!SDL_GetWindowSizeInPixels(app.window, &pixel_width, &pixel_height))
	{
		SDL_Log("SDL_GetWindowSizeInPixels failed: %s", SDL_GetError());
		return false;
	}

	bgfx::Init init;
	init.type = bgfx::RendererType::Count;
	if (!set_native_window(init.platformData, app.window))
	{
		SDL_Log("SDL did not provide a native window handle: %s", SDL_GetError());
		return false;
	}

	init.resolution.width = static_cast<std::uint32_t>(pixel_width);
	init.resolution.height = static_cast<std::uint32_t>(pixel_height);
	init.resolution.reset = app.config.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
	// Capital ships contain dozens of independently submitted model nodes
	// and material sections. Keep bgfx's 65,535-draw default instead of
	// truncating the scene at 2,048 submissions, which could cut through a
	// capital ship while leaving small fighters apparently unaffected.

	if (!bgfx::init(init))
	{
		SDL_Log("bgfx initialization failed");
		return false;
	}

	app.width = init.resolution.width;
	app.height = init.resolution.height;
	app.bgfx_ready = true;
	bgfx::setDebug(BGFX_DEBUG_TEXT);
	bgfx::setViewClear(
		kMainView,
		BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
		0x081426ff,
		1.0f,
		0);
	return true;
}
}

void display_add_resolution(
	frontend::DisplayModes& modes,
	std::uint32_t width,
	std::uint32_t height)
{
	if (width < 640 || height < 480)
	{
		return;
	}
	for (std::uint32_t index = 0; index < modes.count; ++index)
	{
		if (modes.items[index].width == width
			&& modes.items[index].height == height)
		{
			return;
		}
	}
	if (modes.count >= frontend::kMaxDisplayResolutions)
	{
		return;
	}
	std::uint32_t position = modes.count;
	while (position > 0
		&& (modes.items[position - 1].width > width
			|| (modes.items[position - 1].width == width
				&& modes.items[position - 1].height > height)))
	{
		modes.items[position] = modes.items[position - 1];
		--position;
	}
	modes.items[position] = {width, height};
	++modes.count;
}

bool display_apply_config(App& app)
{
	if (app.window == nullptr)
	{
		return false;
	}
	bool ready = SDL_SetWindowFullscreen(app.window, false);
	if (app.config.display_mode == DisplayMode::windowed)
	{
		ready = SDL_SetWindowBordered(app.window, true) && ready;
		ready = SDL_SetWindowSize(
			app.window,
			static_cast<int>(app.config.display_width),
			static_cast<int>(app.config.display_height)) && ready;
		SDL_SetWindowPosition(
			app.window,
			SDL_WINDOWPOS_CENTERED,
			SDL_WINDOWPOS_CENTERED);
	}
	else
	{
		const SDL_DisplayID display = SDL_GetDisplayForWindow(app.window);
		if (app.config.display_mode == DisplayMode::fullscreen)
		{
			SDL_DisplayMode closest{};
			ready = SDL_GetClosestFullscreenDisplayMode(
				display,
				static_cast<int>(app.config.display_width),
				static_cast<int>(app.config.display_height),
				0.0f,
				false,
				&closest) && ready;
			ready = SDL_SetWindowFullscreenMode(
				app.window, ready ? &closest : nullptr) && ready;
		}
		else
		{
			ready = SDL_SetWindowFullscreenMode(app.window, nullptr) && ready;
		}
		ready = SDL_SetWindowFullscreen(app.window, true) && ready;
	}
	if (!ready)
	{
		SDL_Log("Display configuration could not be applied: %s", SDL_GetError());
	}
#if !defined(__EMSCRIPTEN__)
	else
	{
		SDL_SyncWindow(app.window);
	}
#endif
	return ready;
}

bool display_create(App& app)
{
	app.window = SDL_CreateWindow(
		"sl_open",
		static_cast<int>(app.config.display_width),
		static_cast<int>(app.config.display_height),
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (app.window == nullptr)
	{
		SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
		return false;
	}
	enumerate_display_modes(app);
	display_apply_config(app);
	if (init_bgfx(app))
	{
		return true;
	}
	SDL_DestroyWindow(app.window);
	app.window = nullptr;
	return false;
}

void display_resize(App& app, int width, int height)
{
	if (!app.bgfx_ready || width <= 0 || height <= 0)
	{
		return;
	}

	app.width = static_cast<std::uint32_t>(width);
	app.height = static_cast<std::uint32_t>(height);
	bgfx::reset(
		app.width,
		app.height,
		app.config.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE);
}

bool display_start_error(App& app, StartupError error)
{
	app.window = SDL_CreateWindow(
		"sl_open",
		640,
		240,
		SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (app.window == nullptr)
	{
		SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
		return false;
	}
	app.startup_renderer = SDL_CreateRenderer(app.window, nullptr);
	if (app.startup_renderer == nullptr)
	{
		SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
		SDL_DestroyWindow(app.window);
		app.window = nullptr;
		return false;
	}
	app.startup_error = error;
	return true;
}

void display_draw_startup_error(App& app)
{
	SDL_SetRenderDrawColor(app.startup_renderer, 5, 15, 22, 255);
	SDL_RenderClear(app.startup_renderer);
	SDL_SetRenderScale(app.startup_renderer, 1.5f, 1.5f);
	SDL_SetRenderDrawColor(app.startup_renderer, 255, 205, 72, 255);
	SDL_RenderDebugText(app.startup_renderer, 16.0f, 16.0f, "sl_open");
	SDL_SetRenderDrawColor(app.startup_renderer, 230, 238, 240, 255);
	if (app.startup_error == StartupError::data_not_found)
	{
		SDL_RenderDebugText(
			app.startup_renderer,
			16.0f,
			40.0f,
			"Original StarLancer game data was not found.");
		SDL_RenderDebugText(
			app.startup_renderer,
			16.0f,
			56.0f,
			"Start with:");
		SDL_RenderDebugText(
			app.startup_renderer,
			16.0f,
			68.0f,
			"sl_open --data /path/to/game");
	}
	else
	{
		SDL_RenderDebugText(
			app.startup_renderer,
			16.0f,
			40.0f,
			"Required game archives could not be opened.");
		SDL_RenderDebugText(
			app.startup_renderer,
			16.0f,
			56.0f,
			"Check the directory supplied with --data.");
	}
	SDL_SetRenderDrawColor(app.startup_renderer, 135, 160, 170, 255);
	SDL_RenderDebugText(
		app.startup_renderer,
		16.0f,
		92.0f,
		"Press Escape, Enter, Space, or click to close.");
	SDL_RenderPresent(app.startup_renderer);
}

void display_shutdown(App& app)
{
	if (app.startup_renderer != nullptr)
	{
		SDL_DestroyRenderer(app.startup_renderer);
		app.startup_renderer = nullptr;
	}
	if (app.bgfx_ready)
	{
		bgfx::shutdown();
		app.bgfx_ready = false;
	}
	if (app.window != nullptr)
	{
		SDL_DestroyWindow(app.window);
		app.window = nullptr;
	}
}
}
