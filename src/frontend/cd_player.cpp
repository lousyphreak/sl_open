#include "frontend/cd_player.hpp"

#include "frontend/gui.hpp"

#include <cstdio>

namespace sl_open::frontend
{
namespace
{
struct Region
{
	float x;
	float y;
	float width;
	float height;
};

constexpr Region kControls[] = {
	{564, 124, 50, 43},
	{564, 170, 56, 27},
	{564, 199, 55, 26},
	{564, 227, 55, 27},
	{564, 256, 56, 27},
	{564, 285, 55, 26},
	{564, 314, 56, 26},
	{564, 343, 54, 38},
	{21, 341, 56, 22},
	{21, 363, 56, 22},
};

constexpr const char* kEarlyTracks[] = {
	"new_mission01",
	"new_mission02",
	"new_mission03",
	"new_mission04",
	"new_mission05",
	"new_mission06",
	"new_mission10",
	"New_Searching Mission 01",
	"New_Searching Mission 03",
	"New_Searching Mission 05",
	"New_Searching Mission 06",
	"New_Searching Mission 10",
};

constexpr const char* kLateTracks[] = {
	"New_Sim01",
	"New_Sim02",
	"New_Sim04",
	"New_Sim05",
	"New_Sim07",
	"New_Sim08",
	"New_Sim10",
	"new_launch",
	"New_Takeoff - Music",
	"new_victory",
	"new_defeat",
	"new_mission01",
};

}

void cd_player_reset(CdPlayer& player, bool late_campaign)
{
	player = {};
	player.hovered = -1;
	player.selected = -1;
	player.pointer_x = 320.0f;
	player.pointer_y = 240.0f;
	player.late_campaign = late_campaign;
}

void cd_player_set_pointer(CdPlayer& player, float x, float y, bool inside)
{
	player.pointer_x = x;
	player.pointer_y = y;
	player.hovered = -1;
	if (!inside)
	{
		return;
	}
	for (std::uint8_t index = 0;
		index < sizeof(kControls) / sizeof(kControls[0]);
		++index)
	{
		if (gui::hit_open(kControls[index], x, y))
		{
			player.hovered = static_cast<std::int8_t>(index);
			return;
		}
	}
	for (std::uint8_t index = 0; index < 12; ++index)
	{
		const Region row{117.0f, 176.0f + 16.0f * index, 400.0f, 14.0f};
		if (gui::hit_open(row, x, y))
		{
			player.hovered = static_cast<std::int8_t>(index + 10);
			return;
		}
	}
}

CdAction cd_player_select(CdPlayer& player)
{
	switch (player.hovered)
	{
	case 0:
		player.retail_navigation_path = false;
		return player.selected >= 0 ? CdAction::play : CdAction::none;
	case 1:
		if (player.started)
		{
			player.paused = !player.paused;
			return CdAction::pause;
		}
		return CdAction::none;
	case 2:
		player.selected = -1;
		player.started = false;
		player.paused = false;
		return CdAction::stop;
	case 3:
		if (player.selected < 11)
		{
			++player.selected;
			player.retail_navigation_path = true;
			return CdAction::play;
		}
		return CdAction::none;
	case 4:
		if (player.selected > 0)
		{
			--player.selected;
			player.retail_navigation_path = true;
			return CdAction::play;
		}
		return CdAction::none;
	case 5:
		player.repeat = !player.repeat;
		player.random = false;
		return CdAction::none;
	case 6:
		player.random = !player.random;
		player.repeat = false;
		return CdAction::none;
	case 7:
		return CdAction::exit;
	case 8:
		return CdAction::volume_up;
	case 9:
		return CdAction::volume_down;
	default:
		if (player.hovered >= 10 && player.hovered < 22)
		{
			const std::int8_t track = player.hovered - 10;
			if (player.selected == track)
			{
				player.retail_navigation_path = false;
				return CdAction::play;
			}
			player.selected = track;
		}
		return CdAction::none;
	}
}

const char* cd_player_track_path(
	const CdPlayer& player,
	char* path,
	std::uint32_t capacity)
{
	if (player.selected < 0 || player.selected >= 12 || capacity == 0)
	{
		return nullptr;
	}
	const char* const* tracks =
		player.late_campaign && !player.retail_navigation_path
			? kLateTracks
			: kEarlyTracks;
	std::snprintf(
		path,
		capacity,
		"music/%s.wav",
		tracks[static_cast<std::uint8_t>(player.selected)]);
	return path;
}

}
