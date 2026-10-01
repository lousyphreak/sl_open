#pragma once

#include "frontend/gui.hpp"
#include "render/frontend_renderer.hpp"

#include <cstddef>
#include <cstdint>

namespace sl_open::frontend::gui
{
inline void begin_screen(render::FrontendCommands& commands)
{
	render::frontend_commands_begin(commands);
	render::frontend_scissor(
		commands, 0, 0, render::kFrontendWidth, render::kFrontendHeight);
}

template<std::size_t FrameCount>
void animated_cursor(
	render::FrontendCommands& commands,
	const render::FrontendTexture (&frames)[FrameCount],
	bgfx::TextureHandle palette,
	float x,
	float y,
	std::uint64_t elapsed,
	std::uint32_t frame_duration)
{
	static_assert(FrameCount > 0);
	const auto frame = static_cast<std::size_t>(
		(elapsed / frame_duration) % FrameCount);
	render::frontend_indexed_quad(
		commands, frames[frame], palette, x, y);
}

inline void slider(
	render::FrontendCommands& commands,
	const render::FrontendTexture& white,
	float x,
	float y,
	float width,
	float value,
	bool selected)
{
	render::frontend_rgba_quad(
		commands, white, x, y + 5.0f, width, 3.0f, 0x8dc5d0a0);
	const float handle_x = x + value * width;
	render::frontend_rgba_quad(
		commands,
		white,
		handle_x - 4.0f,
		y - 5.0f,
		8.0f,
		23.0f,
		selected ? 0x70efffff : 0xe1b84fff);
}

inline void checkbox(
	render::FrontendCommands& commands,
	const render::FrontendTexture& white,
	float x,
	float y,
	float size,
	bool checked)
{
	render::frontend_rgba_quad(
		commands,
		white,
		x,
		y,
		size,
		size,
		checked ? 0x70efffff : 0x203040a0);
}
}
