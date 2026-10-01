#include "assets/game_stats.hpp"

#include "io/vfs.hpp"

namespace sl_open::assets
{
bool game_stats_load(io::Vfs& vfs, GameStats& stats)
{
	stats = {};
	if (!ship_stats_load(vfs, "shipstats.bin", stats.ships)
		|| !pilot_stats_load(vfs, "pilotstats.bin", stats.pilots)
		|| !gun_stats_load(vfs, "gunstats.bin", stats.guns)
		|| !missile_stats_load(vfs, "missilestats.bin", stats.missiles))
	{
		stats = {};
		return false;
	}
	stats.ready = true;
	return true;
}
}
