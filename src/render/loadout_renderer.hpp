#pragma once

#include "render/frontend_renderer.hpp"

namespace sl_open::render
{
bool loadout_renderer_build(
	const FrontendRenderer& renderer,
	LoadoutRenderer& destination,
	const assets::TextureImage& panels,
	const assets::TextureImage (&disc)[4],
	const assets::TextureImage& glow,
	const assets::TextureImage& hardpoints,
	const assets::GameplayModel (&ships)[12],
	const assets::GameplayModel (&guns)[12],
	const assets::GameplayModel (&missiles)[10]);
void loadout_renderer_commit(
	LoadoutRenderer& destination,
	LoadoutRenderer& source);
void loadout_renderer_shutdown(LoadoutRenderer& renderer);
}
