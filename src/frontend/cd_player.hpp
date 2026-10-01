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
enum class CdAction : std::uint8_t
{
	none,
	play,
	pause,
	stop,
	exit,
	volume_up,
	volume_down,
};

struct CdPlayer
{
	std::int8_t hovered{-1};
	std::int8_t selected{-1};
	float pointer_x{320.0f};
	float pointer_y{240.0f};
	bool late_campaign{};
	bool paused{};
	bool repeat{};
	bool random{};
	bool started{};
	bool retail_navigation_path{};
};

void cd_player_reset(CdPlayer& player, bool late_campaign);
void cd_player_set_pointer(
	CdPlayer& player,
	float x,
	float y,
	bool inside);
CdAction cd_player_select(CdPlayer& player);
const char* cd_player_track_path(
	const CdPlayer& player,
	char* path,
	std::uint32_t capacity);
void cd_player_build(
	const CdPlayer& player,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands,
	std::uint64_t now);
}
