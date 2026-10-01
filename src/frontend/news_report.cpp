#include "frontend/news_report.hpp"

#include <cstdio>

namespace sl_open::frontend
{
void news_report_reset(
	NewsReport& report,
	std::uint8_t mission,
	bool late_campaign)
{
	report = {};
	report.mission = mission < 1 ? 1 : (mission > 28 ? 28 : mission);
	report.late_campaign = late_campaign;
}

const char* news_report_movie(const NewsReport& report)
{
	if (report.mission == 1 && report.part == 1)
	{
		return "vr/tv_cald.bik";
	}
	return report.late_campaign
		? "vr/b_tv_news_.bik"
		: "vr/rel_tv_in_loop.bik";
}

bool news_report_movie_loops(const NewsReport& report)
{
	return !(report.mission == 1 && report.part == 1);
}

const char* news_report_speech(
	const NewsReport& report,
	char* path,
	std::uint32_t capacity)
{
	if (path == nullptr || capacity == 0)
	{
		return nullptr;
	}
	if (report.mission == 1)
	{
		std::snprintf(
			path,
			capacity,
			"0005%c.box",
			static_cast<char>('a' + report.part));
	}
	else
	{
		std::snprintf(
			path,
			capacity,
			"%04u.box",
			static_cast<unsigned>((report.mission - 1) * 10 + 5));
	}
	return path;
}

bool news_report_speech_finished(NewsReport& report)
{
	if (report.mission == 1 && report.part < 2)
	{
		++report.part;
		return true;
	}
	report.complete = true;
	return false;
}
}
