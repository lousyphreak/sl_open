#pragma once

#include "assets/gun_stats.hpp"
#include "assets/missile_stats.hpp"
#include "assets/pilot_stats.hpp"
#include "assets/ship_stats.hpp"

namespace sl_open::io
{
struct Vfs;
}

namespace sl_open::assets
{
// Retail keeps these four gameplay tables as process globals shared by
// frontend presentation code and live mission systems.
struct GameStats
{
	ShipStatsTable ships;
	PilotStatsTable pilots;
	GunStatsTable guns;
	MissileStatsTable missiles;
	bool ready{};
};

bool game_stats_load(io::Vfs& vfs, GameStats& stats);
}
