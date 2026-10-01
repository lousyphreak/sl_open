#pragma once

#include "campaign/campaign.hpp"

#include <cstdint>

namespace sl_open::campaign
{
constexpr std::uint8_t kMaximumCampaignMovies = 12;

struct CampaignMovieSequence
{
	const char* movies[kMaximumCampaignMovies]{};
	std::uint8_t count{};
	const char* audio_bank{};
	std::uint16_t audio_sample{};
	std::uint8_t audio_movie_index{};
	bool set_chapter2_thread3_seen{};
};

CampaignMovieSequence campaign_post_mission_sequence(
	const CampaignState& state,
	std::uint16_t mission,
	MissionGrade grade,
	bool alternate_mission_25);
CampaignMovieSequence campaign_final_sequence(const CampaignState& state);
const char* campaign_transfer_movie(
	const CampaignState& state,
	std::uint16_t mission);
const char* campaign_ejection_exhausted_movie(std::uint16_t mission);
}
