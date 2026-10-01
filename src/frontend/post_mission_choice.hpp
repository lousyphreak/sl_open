#pragma once

#include <cstdint>

namespace sl_open
{
struct LanguageTable;
}

namespace sl_open::render
{
struct FrontendCommands;
struct FrontendRenderer;
}

namespace sl_open::frontend
{
enum class PostMissionChoiceAction : std::uint8_t
{
	none,
	replay_from_briefing,
	replay_from_launch,
	main_menu,
};

struct PostMissionChoice
{
	float pointer_x{320.0f};
	float pointer_y{240.0f};
	std::int8_t hovered{-1};
	std::uint64_t entered_at{};
};

void post_mission_choice_reset(
	PostMissionChoice& choice,
	std::uint64_t now);
void post_mission_choice_pointer(
	PostMissionChoice& choice,
	float x,
	float y,
	bool inside);
PostMissionChoiceAction post_mission_choice_click(
	const PostMissionChoice& choice);
void post_mission_choice_build(
	const PostMissionChoice& choice,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now);
}
