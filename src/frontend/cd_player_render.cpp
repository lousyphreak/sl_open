#include "frontend/cd_player.hpp"

#include "frontend/gui_render.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

#include <cstdio>

namespace sl_open::frontend
{
namespace
{
struct ControlPosition
{
	float x;
	float y;
};

constexpr ControlPosition kControlPositions[] = {
	{571, 134},
	{571, 174},
	{571, 204},
	{571, 231},
	{571, 259},
	{571, 288},
	{571, 316},
	{571, 354},
};
}

void cd_player_build(
	const CdPlayer& player,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	gui::begin_screen(commands);
	render::frontend_rgba_quad(
		commands,
		player.late_campaign
			? renderer.cd.late_background
			: renderer.cd.early_background,
		0,
		0,
		render::kFrontendWidth,
		render::kFrontendHeight);

	if (player.hovered >= 0 && player.hovered < 8)
	{
		render::frontend_indexed_quad(
			commands,
			renderer.cd.shapes[player.hovered + 2],
			renderer.cd.palette,
			kControlPositions[player.hovered].x,
			kControlPositions[player.hovered].y);
	}
	const std::int8_t playback_control =
		player.started ? (player.paused ? 1 : 0) : -1;
	if (playback_control >= 0 && player.hovered != playback_control)
	{
		render::frontend_indexed_quad(
			commands,
			renderer.cd.shapes[playback_control + 2],
			renderer.cd.palette,
			kControlPositions[playback_control].x,
			kControlPositions[playback_control].y);
	}
	if (player.repeat)
	{
		render::frontend_indexed_quad(
			commands, renderer.cd.shapes[7], renderer.cd.palette, 571, 288);
	}
	if (player.random)
	{
		render::frontend_indexed_quad(
			commands, renderer.cd.shapes[8], renderer.cd.palette, 571, 316);
	}

	render::frontend_cd_text(
		commands,
		renderer,
		language_text(language, 0x3b4),
		129,
		117,
		renderer.shell.font_gold_palette,
		0xffffffff,
		0.62f);
	for (std::uint8_t index = 0; index < 12; ++index)
	{
		char number[4];
		std::snprintf(number, sizeof(number), "%02u", index + 1);
		const float y = 171.0f + index * 16.0f;
		const bgfx::TextureHandle palette =
			player.selected == static_cast<std::int8_t>(index)
				? renderer.shell.font_white_palette
				: renderer.shell.font_gold_palette;
		render::frontend_cd_text(
			commands,
			renderer,
			number,
			117,
			y,
			palette,
			0xffffffff,
			0.62f);
		const std::uint16_t title =
			static_cast<std::uint16_t>(
				(player.late_campaign ? 0x531 : 0x319) + index);
		render::frontend_cd_text(
			commands,
			renderer,
			language_text(language, title),
			137,
			y,
			palette,
			0xffffffff,
			0.62f);
	}
	if (player.hovered >= 0 && player.hovered < 10)
	{
		render::frontend_cd_text(
			commands,
			renderer,
			language_text(
				language,
				static_cast<std::uint16_t>(0x2f6 + player.hovered)),
			440,
			320,
			renderer.shell.font_white_palette,
			0xffffffff,
			0.62f);
	}

	gui::animated_cursor(
		commands,
		renderer.shell.cursor,
		renderer.shell.cursor_palette,
		player.pointer_x,
		player.pointer_y,
		now,
		60);
}
}
