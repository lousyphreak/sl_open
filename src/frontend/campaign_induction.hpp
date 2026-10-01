#pragma once

#include <cstdint>

namespace sl_open::frontend
{
enum class InductionPhase : std::uint8_t
{
	pre_roll,
	transition,
	narration,
	final_movie,
	post_roll,
	complete,
};

struct CampaignInduction
{
	InductionPhase phase{InductionPhase::pre_roll};
	std::uint8_t pre_roll_index{};
	std::uint8_t narration_index{};
	std::uint8_t transition_index{};
	std::uint8_t post_roll_index{};
};

void campaign_induction_reset(CampaignInduction& induction);
const char* campaign_induction_movie(const CampaignInduction& induction);
const char* campaign_induction_narration(const CampaignInduction& induction);
bool campaign_induction_movie_finished(CampaignInduction& induction);
void campaign_induction_narration_finished(CampaignInduction& induction);
}
