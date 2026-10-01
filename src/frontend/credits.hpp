#pragma once

#include <cstdint>

namespace sl_open
{
struct LanguageTable;
}

namespace sl_open::render
{
struct FrontendCommands;
struct FrontendRenderer;
}

namespace sl_open::frontend
{
struct Credits
{
	std::uint64_t deadline{};
	std::uint8_t page{};
	std::uint8_t fade_state{};
	float alpha{};
};

void credits_reset(Credits& credits, std::uint64_t now);
bool credits_update(Credits& credits, std::uint64_t now);
void credits_build(
	const Credits& credits,
	const LanguageTable& language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands);
}
