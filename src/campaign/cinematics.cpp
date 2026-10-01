#include "campaign/cinematics.hpp"

namespace sl_open::campaign
{
namespace
{
void append(CampaignMovieSequence& sequence, const char* movie)
{
	if (sequence.count < kMaximumCampaignMovies)
	{
		sequence.movies[sequence.count++] = movie;
	}
}

bool chapter_mission(std::uint16_t mission)
{
	return mission == 7 || mission == 11 || mission == 19
		|| mission == 21 || mission == 25;
}

const char* chapter_movie(std::uint16_t mission)
{
	switch (mission)
	{
	case 7: return "new_chapter1.bik";
	case 11: return "new_chapter2.bik";
	case 19: return "new_chapter3.bik";
	case 21: return "new_chapter4.bik";
	case 25: return "new_chapter5.bik";
	default: return nullptr;
	}
}
}

CampaignMovieSequence campaign_post_mission_sequence(
	const CampaignState& state,
	std::uint16_t mission,
	MissionGrade grade,
	bool alternate_mission_25)
{
	CampaignMovieSequence sequence;
	const bool has_result = grade != MissionGrade::none;
	if (has_result
		&& (grade < MissionGrade::failed || grade > MissionGrade::perfect))
	{
		return sequence;
	}

	const bool transfer_branch =
		state.branch_variables[branch_0052a480] == 0;
	if ((mission == 25 && !alternate_mission_25)
		|| (mission == 25 && alternate_mission_25 && transfer_branch)
		|| (mission == 27 && transfer_branch))
	{
		return sequence;
	}

	if (has_result && chapter_mission(mission))
	{
		// The retail table selects the seemingly reversed zoom names:
		// Reliant-era chapters use thread_zoom, Yamato-era chapters use
		// rthread_zoom.
		append(
			sequence,
			mission < 18 ? "thread_zoom.bik" : "rthread_zoom.bik");
		append(sequence, chapter_movie(mission));
		sequence.audio_bank = "newsloop.fat";
		// Retail assigns this bank's only member global sound ID 80.
		// FatBank keeps bank-local indices, so the same member is index zero.
		sequence.audio_sample = 0;
		sequence.audio_movie_index = 1;

		if (mission == 7
			&& state.branch_variables[branch_0052a438] != 1)
		{
			append(sequence, "acntran.bik");
			append(sequence, "new_chapter1_thread1.bik");
		}
		else if (mission == 11)
		{
			if (state.branch_variables[branch_0052a464] != 1)
			{
				append(sequence, "acntran.bik");
				append(sequence, "new_chapter2_thread1.bik");
			}
			if (state.branch_variables[branch_0052a434] != 1)
			{
				append(sequence, "acntran.bik");
				append(sequence, "new_chapter2_thread2.bik");
			}
			if (state.branch_variables[branch_0052a408] != 1)
			{
				append(sequence, "acntran.bik");
				append(sequence, "new_chapter2_thread3.bik");
				sequence.set_chapter2_thread3_seen = true;
			}
		}
		return sequence;
	}

	static constexpr const char* kEarlyThread[] = {
		"rthread_d.bik",
		"rthread_d.bik",
		"rthread_c.bik",
		"rthread_b.bik",
		"rthread_a.bik",
	};
	static constexpr const char* kEarlyAudio[] = {
		"rlande.fat",
		"rlandd.fat",
		"rlandc.fat",
		"rlandb.fat",
		"rlanda.fat",
	};
	static constexpr const char* kLateThread[] = {
		"thread04.bik",
		"thread04.bik",
		"thread03.bik",
		"thread02.bik",
		"thread01.bik",
	};
	static constexpr const char* kLateAudio[] = {
		"ylande.fat",
		"ylandd.fat",
		"ylandc.fat",
		"ylandb.fat",
		"ylanda.fat",
	};
	const auto slot = has_result
		? static_cast<std::uint8_t>(grade)
		: std::uint8_t{0};
	const bool late =
		mission == 7
		|| (mission == 8
			&& state.branch_variables[branch_0052a470] == 0)
		|| mission >= 18;
	append(sequence, late ? "yamland_generic.bik" : "r_h_land.bik");
	append(sequence, late ? kLateThread[slot] : kEarlyThread[slot]);
	sequence.audio_bank =
		late ? kLateAudio[slot] : kEarlyAudio[slot];
	// Retail assigns the selected bank's only member global sound ID 127.
	// FatBank keeps bank-local indices, so the same member is index zero.
	sequence.audio_sample = 0;
	sequence.audio_movie_index = 0;
	return sequence;
}

CampaignMovieSequence campaign_final_sequence(const CampaignState& state)
{
	CampaignMovieSequence sequence;
	append(sequence, "new_chapter6_thread1.bik");
	if (state.branch_variables[branch_0052a410] != 1)
	{
		append(sequence, "acntran.bik");
		append(sequence, "new_chapter6_thread2.bik");
	}
	if (state.branch_variables[branch_0052a40c] != 1)
	{
		append(sequence, "acntran.bik");
		append(sequence, "new_chapter6_thread3.bik");
	}
	append(sequence, "acntran.bik");
	append(
		sequence,
		state.branch_variables[branch_0052a424] == 1
			? "new_chapter6_thread5.bik"
			: "new_chapter6_thread4.bik");
	append(sequence, "acntran.bik");
	append(sequence, "new_chapter6.bik");
	return sequence;
}

const char* campaign_transfer_movie(
	const CampaignState& state,
	std::uint16_t mission)
{
	if ((mission == 25 || mission == 27)
		&& state.branch_variables[branch_0052a480] == 0)
	{
		return "fortbearshuttle_.bik";
	}
	if (mission >= 19
		|| state.branch_variables[branch_0052a470] == 0)
	{
		return "new_a y trans.bik";
	}
	return "new_reliant_transfer.bik";
}

const char* campaign_ejection_exhausted_movie(std::uint16_t mission)
{
	// The third cumulative ejection takes the dedicated branch at
	// LANCER.EXE 0x004aa50d. Unlike coordinator state five, it does not
	// consult either campaign transfer variable or select Fort Bear.
	return mission >= 19
		? "new_a y trans.bik"
		: "new_reliant_transfer.bik";
}
}
