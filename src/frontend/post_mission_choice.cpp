#include "frontend/post_mission_choice.hpp"

#include "frontend/gui_render.hpp"
#include "localization/language.hpp"
#include "render/frontend_renderer.hpp"

#include <algorithm>

namespace sl_open::frontend
{
namespace
{
constexpr gui::Rect kRegions[] = {
	{162.0f, 371.0f, 30.0f, 21.0f},
	{162.0f, 398.0f, 30.0f, 21.0f},
	{162.0f, 425.0f, 30.0f, 21.0f},
};
}

void post_mission_choice_reset(
	PostMissionChoice& choice,
	std::uint64_t now)
{
	choice = {};
	choice.pointer_x = 320.0f;
	choice.pointer_y = 240.0f;
	choice.hovered = -1;
	choice.entered_at = now;
}

void post_mission_choice_pointer(
	PostMissionChoice& choice,
	float x,
	float y,
	bool inside)
{
	choice.pointer_x = x;
	choice.pointer_y = y;
	choice.hovered = -1;
	if (!inside)
	{
		return;
	}
	for (std::uint8_t index = 0; index < 3; ++index)
	{
		if (gui::hit_open(kRegions[index], x, y))
		{
			choice.hovered = static_cast<std::int8_t>(index);
			return;
		}
	}
}

PostMissionChoiceAction post_mission_choice_click(
	const PostMissionChoice& choice)
{
	switch (choice.hovered)
	{
	case 0: return PostMissionChoiceAction::replay_from_briefing;
	case 1: return PostMissionChoiceAction::replay_from_launch;
	case 2: return PostMissionChoiceAction::main_menu;
	default: return PostMissionChoiceAction::none;
	}
}

void post_mission_choice_build(
	const PostMissionChoice& choice,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now)
{
	gui::begin_screen(commands);
	const float fade = std::clamp(
		static_cast<float>(now - choice.entered_at) / 500.0f,
		0.0f,
		1.0f);
	const std::uint8_t alpha =
		static_cast<std::uint8_t>(fade * 255.0f);
	const std::uint32_t tint = 0xffffff00u | alpha;
	render::frontend_indexed_quad(
		commands,
		renderer.restart.background,
		renderer.restart.screen_palette,
		1.0f,
		1.0f,
		tint);
	if (choice.hovered >= 0)
	{
		const float y = 373.0f + choice.hovered * 27.0f;
		render::frontend_indexed_quad(
			commands,
			renderer.restart.highlight,
			renderer.restart.screen_palette,
			164.0f,
			y,
			tint);
	}
	const std::uint16_t labels[] = {0x32d, 0x32e, 0x32f};
	const float label_y[] = {374.0f, 402.0f, 428.0f};
	for (std::uint8_t index = 0; index < 3; ++index)
	{
		render::frontend_text(
			commands,
			renderer,
			language_text(language, labels[index]),
			199.0f,
			label_y[index],
			renderer.shell.font_white_palette,
			tint,
			0.56f);
	}
	render::frontend_indexed_quad(
		commands,
		renderer.restart.cursor[
			static_cast<std::uint32_t>((now / 40) % 16)],
		renderer.restart.cursor_palette,
		choice.pointer_x,
		choice.pointer_y,
		tint);
}
}
