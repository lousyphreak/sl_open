#pragma once

#include "frontend/itac_shell.hpp"

namespace sl_open::frontend
{
void itac_build_archive(
	ItacShell& shell,
	const LanguageTable& language,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands);
void itac_build_news(
	ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands);
void itac_build_media(
	ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands);
void itac_build_fighters(
	ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands);
void itac_build_capitals(
	ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands);
void itac_build_squads(
	ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands);
void itac_build_personnel(
	ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands);
void itac_build_kills(
	const ItacShell& shell,
	const LanguageTable& itac_language,
	const render::FrontendRenderer& renderer,
	render::FrontendCommands& commands);
}
