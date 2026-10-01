#pragma once

#include <cstddef>
#include <cstdint>

namespace sl_open::assets
{
constexpr std::size_t kPilotPresentationCount = 194;

struct PilotPresentationDefinition
{
	std::int16_t portrait_or_movie_id{};
	std::int16_t presentation_group{};
	std::int16_t presentation_flags{};
	std::int16_t presentation_mode{};
	const char* movies[4]{};
};

const PilotPresentationDefinition* pilot_presentation(
	std::uint16_t pilot);
}
