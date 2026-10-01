#pragma once

#include <cstdint>

namespace sl_open::frontend
{
struct NewsReport
{
	std::uint8_t mission{1};
	std::uint8_t part{};
	bool late_campaign{};
	bool complete{};
};

void news_report_reset(
	NewsReport& report,
	std::uint8_t mission,
	bool late_campaign);
const char* news_report_movie(const NewsReport& report);
bool news_report_movie_loops(const NewsReport& report);
const char* news_report_speech(
	const NewsReport& report,
	char* path,
	std::uint32_t capacity);
bool news_report_speech_finished(NewsReport& report);
}
