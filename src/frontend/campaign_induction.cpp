#include "frontend/campaign_induction.hpp"

#include <cstdlib>

namespace sl_open::frontend
{
namespace
{
constexpr const char* kPreRoll[] = {
	"new_intro.bik",
	"vr/rel_ladd_bunk.bik",
	"vr/rel_t2l.bik",
	"vr/rel_c_tv.bik",
};

struct NarratedStop
{
	const char* narration;
	const char* backdrop;
	const char* transitions[3];
	std::uint8_t transition_count;
};

constexpr NarratedStop kStops[] = {
	{"enr_intro.box", "vr/rel_tv_enriq.bik", {}, 0},
	{"enr_locker.box", "vr/single_rel_c2lock.bik",
		{"vr/rel_tv_c.bik", "vr/rel_c2lock.bik"}, 2},
	{"enr_simpod.box", "vr/rel_podmon_loop.bik",
		{"vr/rel_lock2c.bik", "vr/rel_t2itac.bik",
			"vr/rel_itac2pod.bik"}, 3},
	{"enr_cd.box", "vr/rel_cdloop.bik",
		{"vr/rel_pod2cd.bik"}, 1},
	{"enr_itac.box", "vr/rel_itacloop.bik",
		{"vr/rel_cd2pod.bik", "vr/rel_pod2itac.bik"}, 2},
	{"enr_outro.box", "vr/rel_tv_enriq.bik",
		{"vr/rel_itac2t.bik", "vr/rel_t2l.bik", "vr/rel_c_tv.bik"}, 3},
};

constexpr const char* kFinalMovie = "vr/rel_tv_c.bik";
constexpr const char* kPostRoll[] = {
	"vr/rel_l2t.bik",
	"vr/rel_t2itac.bik",
};
}

void campaign_induction_reset(CampaignInduction& induction)
{
	induction = {};
}

const char* campaign_induction_movie(const CampaignInduction& induction)
{
	switch (induction.phase)
	{
	case InductionPhase::pre_roll:
		return kPreRoll[induction.pre_roll_index];
	case InductionPhase::transition:
		return kStops[induction.narration_index]
			.transitions[induction.transition_index];
	case InductionPhase::narration:
		return kStops[induction.narration_index].backdrop;
	case InductionPhase::final_movie:
		return kFinalMovie;
	case InductionPhase::post_roll:
		return kPostRoll[induction.post_roll_index];
	case InductionPhase::complete:
		return nullptr;
	}
	std::abort();
}

const char* campaign_induction_narration(
	const CampaignInduction& induction)
{
	return induction.phase == InductionPhase::narration
		? kStops[induction.narration_index].narration
		: nullptr;
}

bool campaign_induction_movie_finished(CampaignInduction& induction)
{
	switch (induction.phase)
	{
	case InductionPhase::pre_roll:
		++induction.pre_roll_index;
		if (induction.pre_roll_index
			< sizeof(kPreRoll) / sizeof(kPreRoll[0]))
		{
			return true;
		}
		induction.phase = InductionPhase::narration;
		return true;
	case InductionPhase::transition:
		++induction.transition_index;
		if (induction.transition_index
			< kStops[induction.narration_index].transition_count)
		{
			return true;
		}
		induction.phase = InductionPhase::narration;
		return true;
	case InductionPhase::final_movie:
		induction.phase = InductionPhase::post_roll;
		return true;
	case InductionPhase::post_roll:
		++induction.post_roll_index;
		if (induction.post_roll_index
			< sizeof(kPostRoll) / sizeof(kPostRoll[0]))
		{
			return true;
		}
		induction.phase = InductionPhase::complete;
		return false;
	case InductionPhase::narration:
	case InductionPhase::complete:
		return false;
	}
	std::abort();
}

void campaign_induction_narration_finished(CampaignInduction& induction)
{
	if (induction.phase != InductionPhase::narration)
	{
		return;
	}
	++induction.narration_index;
	induction.transition_index = 0;
	if (induction.narration_index
		< sizeof(kStops) / sizeof(kStops[0]))
	{
		induction.phase = InductionPhase::transition;
	}
	else
	{
		induction.phase = InductionPhase::final_movie;
	}
}
}
