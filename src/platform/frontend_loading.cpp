#include "platform/frontend_loading.hpp"

#include "assets/object_type_catalog.hpp"
#include "assets/player_ship.hpp"
#include "assets/texture_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace sl_open::platform
{
namespace
{
template <std::size_t N>
bool load_tga_set(
	io::Vfs& vfs,
	const char* const (&paths)[N],
	assets::TextureImage (&images)[N])
{
	bool ready = true;
	for (std::size_t index = 0; index < N; ++index)
	{
		ready = assets::load_tga(vfs, paths[index], images[index]) && ready;
	}
	return ready;
}

bool load_hologram_materials(
	assets::GameplayModel& model, const assets::TextureCache& cache, char prefix)
{
	// Loadout_preload (0x00441aa0) selects green ship textures and
	// red missile/gun textures through Model_convert_lod_to_sro.
	for (assets::GameplayMaterial& material : model.materials)
	{
		char name[66];
		std::snprintf(name, sizeof(name), "%c%s", prefix, material.basename);
		if (!assets::texture_cache_decode(cache, name, material.image))
		{
			return false;
		}
	}
	return true;
}

bool load_shell_stage(App& app)
{
	bool ready =
		load_language_dll(app.vfs, "LANGUAGE.DLL", app.language)
		&& load_language_dll(app.vfs, "ITACLANG.DLL", app.itac_language);
	ready = assets::load_tga(
		app.vfs, "interface/sl_splash2.tga", app.upload_assets.splash)
		&& assets::load_tga(
			app.vfs,
			"interface/sl_splash.tga",
			app.upload_assets.mission_loading_splash)
		&& assets::load_tga(
			app.vfs,
			"interface/main2opt.tga",
			app.upload_assets.options_background)
		&& assets::load_tga(
			app.vfs,
			"interface/optfade.tga",
			app.upload_assets.options_detail_background)
		&& assets::load_tga(
			app.vfs,
			"interface/ingameop.tga",
			app.upload_assets.in_game_options_background)
		&& assets::load_tga(
			app.vfs,
			"interface/igoptfad.tga",
			app.upload_assets.in_game_options_detail_background)
		&& assets::load_tga(
			app.vfs,
			"interface/main2sin.tga",
			app.upload_assets.campaign_background)
		&& assets::load_tga(
			app.vfs,
			"interface/sinfade.tga",
			app.upload_assets.save_load_background)
		&& assets::load_tga(
			app.vfs,
			"interface/main2mul.tga",
			app.upload_assets.multiplayer_background)
		&& assets::load_tga(
			app.vfs,
			"interface/mulfade.tga",
			app.upload_assets.multiplayer_lobby_background)
		&& ready;
	ready = assets::load_sprite_list(
		app.vfs,
		"interface/frontend.spr",
		app.upload_assets.frontend_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"interface/frntend4.spr",
			app.upload_assets.options_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"interface/frntend5.spr",
			app.upload_assets.options_detail_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"interface/frntend6.spr",
			app.upload_assets.control_options_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"interface/frntend7.spr",
			app.upload_assets.in_game_options_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"hudhard.spr",
			app.upload_assets.gameplay_hud_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"dmicons.spr",
			app.upload_assets.gameplay_scoreboard_sprites)
		&& assets::load_tga(
			app.vfs,
			"powerball.tga",
			app.upload_assets.gameplay_powerball)
		&& assets::load_sprite_list(
			app.vfs,
			"interface/frntend2.spr",
			app.upload_assets.about_sprites)
		&& assets::load_sprite_list(
			app.vfs, "quit.spr", app.upload_assets.quit_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"interface/frntend2.spr",
			app.upload_assets.campaign_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"interface/frntend3.spr",
			app.upload_assets.multiplayer_sprites)
		&& assets::load_sprite_list(
			app.vfs, "interface/vrgfx.spr", app.upload_assets.vr_sprites)
		&& assets::load_font(
			app.vfs, "interface/optfnt.fnt", app.upload_assets.options_font)
		&& assets::load_font(
			app.vfs,
			"interface/smlfnt2.fnt",
			app.upload_assets.pause_small_font)
		&& assets::load_font(
			app.vfs,
			"blufont.fnt",
			app.upload_assets.gameplay_hud_font)
		&& assets::load_font(
			app.vfs,
			"blk2orng.fnt",
			app.upload_assets.gameplay_scoreboard_font)
		&& assets::load_font(
			app.vfs,
			"newfont.fnt",
			app.upload_assets.gameplay_message_font)
		&& ready;

	assets::IndexedImage highlight;
	return ready
		&& assets::decode_sprite_shape(
			app.upload_assets.frontend_sprites, 18, highlight);
}

bool load_briefing_loadout_stage(App& app)
{
	static constexpr const char* kBriefingDoors[] = {
		"inter/rbriefdor.tga",
		"inter/briefdoor.tga",
	};
	static constexpr const char* kBriefingSprites[] = {
		"rbrief.spr",
		"brief.spr",
		"rbrief2.spr",
		"brief2.spr",
	};
	static constexpr const char* kLoadoutBackgrounds[] = {
		"rbackground.tga",
		"background.tga",
	};
	bool ready = assets::game_stats_load(app.vfs, app.game_stats);
	ready = load_tga_set(
		app.vfs, kBriefingDoors, app.upload_assets.briefing_doors)
		&& ready;
	ready = assets::load_sprite_list(
		app.vfs, kBriefingSprites[0], app.upload_assets.early_briefing_sprites)
		&& assets::load_sprite_list(
			app.vfs, kBriefingSprites[1], app.upload_assets.late_briefing_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			kBriefingSprites[2],
			app.upload_assets.early_briefing_exit_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			kBriefingSprites[3],
			app.upload_assets.late_briefing_exit_sprites)
		&& ready;
	ready = load_tga_set(
		app.vfs, kLoadoutBackgrounds, app.upload_assets.loadout_backgrounds)
		&& assets::load_tga(
			app.vfs, "fpanels.tga", app.upload_assets.loadout_panels)
		&& assets::load_tga(
			app.vfs, "plate-nw.tga", app.upload_assets.loadout_disc[0])
		&& assets::load_tga(
			app.vfs, "plate-ne.tga", app.upload_assets.loadout_disc[1])
		&& assets::load_tga(
			app.vfs, "plate-sw.tga", app.upload_assets.loadout_disc[2])
		&& assets::load_tga(
			app.vfs, "plate-se.tga", app.upload_assets.loadout_disc[3])
		&& assets::load_tga(
			app.vfs, "hologlow.tga", app.upload_assets.loadout_glow)
		&& assets::load_tga(
			app.vfs, "hpoints.tga", app.upload_assets.loadout_hardpoints)
		&& assets::load_sprite_list(
			app.vfs, "LOADOUT.SPR", app.upload_assets.loadout_sprites, false)
		&& assets::load_font(
			app.vfs, "ld_handel.fnt", app.upload_assets.loadout_title_font)
		&& assets::load_font(
			app.vfs, "handels.fnt", app.upload_assets.loadout_info_font)
		&& assets::load_tga_palette(
			app.vfs, "palette3.tga", app.upload_assets.loadout_palette)
		&& ready;

	assets::TextureCache texture_cache;
	// Loadout_preload (0x00441c9a) installs palette3.ccb before loading
	// the indexed g*/r* display textures.
	ready = assets::texture_cache_load(
		app.vfs, "tcachehw.dat", "palette3.ccb", texture_cache) && ready;
	static constexpr const char* kGuns[] = {
		"predator_gun.SHP", "naginata_gun.shp", "grendal_gun.shp",
		"crusader_gun.SHP", "coyote_gun.shp", "mirage_gun.SHP",
		"tempest_gun.shp", "patriot_gun.shp", "wolverine_gun.shp",
		"reaper_gun.shp", "shroud_gun.shp", "phoenix_gun.shp",
	};
	for (std::size_t index = 0; index < assets::kPlayerShipCount; ++index)
	{
		ready = assets::load_gameplay_model(
			app.vfs,
			assets::object_type_resource(
				static_cast<std::uint16_t>(index)).model_path,
			texture_cache,
			app.upload_assets.loadout_ships[index])
			&& assets::load_gameplay_model(
				app.vfs, kGuns[index], texture_cache,
				app.upload_assets.loadout_guns[index])
			&& ready;
		// Display objects use the same mass-centered model tree as gameplay
		// (LoadoutGunModel_create -> 0x00476130 -> 0x004769f0).
		ready = load_hologram_materials(app.upload_assets.loadout_ships[index], texture_cache, 'g') && ready;
		ready = load_hologram_materials(app.upload_assets.loadout_guns[index], texture_cache, 'r') && ready;
	}

	static constexpr const char* kMissiles[] = {
		"21_screamer_pod.shp", "22_raptor_pod.shp", "23_havoc.shp",
		"24_jackhammer.shp", "25_bandit.shp", "26_vagabond.shp",
		"27_solomon_pod.shp", "28_imp.shp", "29_hawk_pod.shp",
		"31_fuel_pod.SHP",
	};
	for (std::size_t index = 0; index < std::size(kMissiles); ++index)
	{
		ready = assets::load_gameplay_model(
			app.vfs,
			kMissiles[index],
			texture_cache,
			app.upload_assets.loadout_missiles[index])
			&& ready;
		ready = load_hologram_materials(app.upload_assets.loadout_missiles[index], texture_cache, 'r') && ready;
	}
	return ready;
}

bool load_secondary_stage(App& app)
{
	bool ready = assets::load_tga(
		app.vfs, "mpdebr.TGA", app.upload_assets.debrief_background)
		&& assets::load_sprite_list(
			app.vfs, "MPDEBR.SPR", app.upload_assets.debrief_sprites)
		&& assets::load_sprite_list(
			app.vfs, "RESTART.SPR", app.upload_assets.restart_sprites);
	static constexpr const char* kSimPodBackgrounds[] = {
		"inter/simpod/training_00000.tga",
		"inter/simpod/training_00035.tga",
	};
	ready = load_tga_set(
		app.vfs, kSimPodBackgrounds, app.upload_assets.sim_pod_backgrounds)
		&& assets::load_sprite_list(
			app.vfs,
			"inter/simpod/simgfx.spr",
			app.upload_assets.sim_pod_sprites)
		&& assets::load_tga(
			app.vfs,
			"interface/rel_bunk2cd.tga",
			app.upload_assets.cd_early_background)
		&& assets::load_tga(
			app.vfs,
			"interface/brd2cd.tga",
			app.upload_assets.cd_late_background)
		&& assets::load_sprite_list(
			app.vfs, "cdplay.spr", app.upload_assets.cd_sprites)
		&& assets::load_font(
			app.vfs, "inter/itac/itacbig.fnt", app.upload_assets.itac_font)
		&& assets::load_font(
			app.vfs,
			"inter/itac/itacsml.fnt",
			app.upload_assets.itac_small_font)
		&& ready;

	ready = assets::load_sprite_list(
		app.vfs, "newsrep.spr", app.upload_assets.itac_news_sprites)
		&& assets::load_sprite_list(
			app.vfs, "vidrep.spr", app.upload_assets.itac_video_sprites)
		&& assets::load_sprite_list(
			app.vfs, "itacgfx.spr", app.upload_assets.itac_gfx_sprites)
		&& assets::load_sprite_list(
			app.vfs, "squads.spr", app.upload_assets.itac_squad_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"inter/itac/persons.spr",
			app.upload_assets.itac_person_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"inter/itac/capships.spr",
			app.upload_assets.itac_capital_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"inter/itac/fighters.spr",
			app.upload_assets.itac_fighter_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"inter/itac/kills.spr",
			app.upload_assets.itac_kills_sprites)
		&& assets::load_sprite_list(
			app.vfs,
			"credits.spr",
			app.upload_assets.credits_sprites)
		&& assets::load_font(
			app.vfs,
			"interface/smlfnt2.fnt",
			app.upload_assets.credits_font)
		&& ready;

	static constexpr const char* kItacBackgrounds[] = {
		"itactrans_00030.tga", "itactrans_00051.tga",
		"itactrans_00072.tga", "itactrans_00093.tga",
		"itactrans_00114.tga", "itactrans_00135.tga",
		"itactrans_00156.tga", "itactrans_00177.tga",
	};
	ready = load_tga_set(
		app.vfs, kItacBackgrounds, app.upload_assets.itac_backgrounds)
		&& ready;

	char path[32];
	for (std::uint32_t index = 0; index < 3; ++index)
	{
		std::snprintf(path, sizeof(path), "rmedal%u.spr", index + 1);
		ready = assets::load_sprite_list(
			app.vfs, path, app.upload_assets.early_medal_sprites[index])
			&& ready;
		std::snprintf(path, sizeof(path), "rbar%u.spr", index + 1);
		ready = assets::load_sprite_list(
			app.vfs, path, app.upload_assets.early_bar_sprites[index])
			&& ready;
	}
	for (std::uint32_t index = 0; index < 6; ++index)
	{
		std::snprintf(path, sizeof(path), "medal%u.spr", index + 1);
		ready = assets::load_sprite_list(
			app.vfs, path, app.upload_assets.late_medal_sprites[index])
			&& ready;
		if (index < 5)
		{
			std::snprintf(path, sizeof(path), "bar%u.spr", index + 1);
			ready = assets::load_sprite_list(
				app.vfs, path, app.upload_assets.late_bar_sprites[index])
				&& ready;
		}
	}
	return ready;
}
}

bool frontend_load_assets(App& app)
{
	SDL_Log("Frontend load stage 1/3: shell and localization");
	if (!load_shell_stage(app))
	{
		SDL_Log("Frontend shell/localization loading failed");
		return false;
	}
	SDL_Log("Frontend load stage 2/3: briefing and loadout");
	if (!load_briefing_loadout_stage(app))
	{
		SDL_Log("Frontend briefing/loadout loading failed");
		return false;
	}
	SDL_Log("Frontend load stage 3/3: auxiliary screens");
	if (!load_secondary_stage(app))
	{
		SDL_Log("Frontend auxiliary-screen loading failed");
		return false;
	}
	SDL_Log(
		"Frontend assets: splash=%ux%u sprites=%u font=%ux%u strings=0..%u",
		app.upload_assets.splash.width,
		app.upload_assets.splash.height,
		app.upload_assets.frontend_sprites.shape_count,
		app.upload_assets.options_font.directory_count,
		app.upload_assets.options_font.height,
		app.language.highest_id);
	return true;
}

bool frontend_upload_assets(App& app)
{
	if (!render::frontend_renderer_init(
			app.frontend_renderer,
			app.upload_assets.splash,
			app.upload_assets.mission_loading_splash,
			app.upload_assets.options_background,
			app.upload_assets.options_detail_background,
			app.upload_assets.in_game_options_background,
			app.upload_assets.in_game_options_detail_background,
			app.upload_assets.frontend_sprites,
			app.upload_assets.options_sprites,
			app.upload_assets.options_detail_sprites,
			app.upload_assets.control_options_sprites,
			app.upload_assets.in_game_options_sprites,
			app.upload_assets.gameplay_hud_sprites,
			app.upload_assets.gameplay_scoreboard_sprites,
			app.upload_assets.gameplay_powerball,
			app.upload_assets.about_sprites,
			app.upload_assets.quit_sprites,
			app.upload_assets.options_font,
			app.upload_assets.pause_small_font,
			app.upload_assets.gameplay_hud_font,
			app.upload_assets.gameplay_scoreboard_font,
			app.upload_assets.gameplay_message_font)
		|| !render::frontend_campaign_assets_init(
			app.frontend_renderer,
			app.upload_assets.campaign_background,
			app.upload_assets.save_load_background,
			app.upload_assets.campaign_sprites)
		|| !render::frontend_multiplayer_assets_init(
			app.frontend_renderer,
			app.upload_assets.multiplayer_background,
			app.upload_assets.multiplayer_lobby_background,
			app.upload_assets.multiplayer_sprites)
		|| !render::frontend_vr_assets_init(
			app.frontend_renderer, app.upload_assets.vr_sprites)
		|| !render::frontend_briefing_assets_init(
			app.frontend_renderer,
			app.upload_assets.briefing_doors,
			app.upload_assets.early_briefing_sprites,
			app.upload_assets.late_briefing_sprites,
			app.upload_assets.early_briefing_exit_sprites,
			app.upload_assets.late_briefing_exit_sprites)
		|| !render::frontend_loadout_assets_init(
			app.frontend_renderer,
			app.upload_assets.loadout_backgrounds,
			app.upload_assets.loadout_panels,
			app.upload_assets.loadout_disc,
			app.upload_assets.loadout_glow,
			app.upload_assets.loadout_hardpoints,
			app.upload_assets.loadout_sprites,
			app.upload_assets.loadout_ships,
			app.upload_assets.loadout_guns,
			app.upload_assets.loadout_missiles,
			app.upload_assets.loadout_title_font,
			app.upload_assets.loadout_info_font,
			app.upload_assets.loadout_palette)
		|| !render::frontend_debrief_assets_init(
			app.frontend_renderer,
			app.upload_assets.debrief_background,
			app.upload_assets.debrief_sprites)
		|| !render::frontend_restart_assets_init(
			app.frontend_renderer, app.upload_assets.restart_sprites)
		|| !render::frontend_sim_pod_assets_init(
			app.frontend_renderer,
			app.upload_assets.sim_pod_backgrounds,
			app.upload_assets.sim_pod_sprites)
		|| !render::frontend_cd_assets_init(
			app.frontend_renderer,
			app.upload_assets.cd_early_background,
			app.upload_assets.cd_late_background,
			app.upload_assets.cd_sprites,
			app.upload_assets.itac_font)
		|| !render::frontend_medal_assets_init(
			app.frontend_renderer,
			app.upload_assets.early_medal_sprites,
			app.upload_assets.early_bar_sprites,
			app.upload_assets.late_medal_sprites,
			app.upload_assets.late_bar_sprites)
		|| !render::frontend_itac_assets_init(
			app.frontend_renderer,
			app.upload_assets.itac_backgrounds,
			app.upload_assets.itac_small_font,
			app.upload_assets.itac_news_sprites,
			app.upload_assets.itac_video_sprites,
			app.upload_assets.itac_gfx_sprites,
			app.upload_assets.itac_squad_sprites,
			app.upload_assets.itac_person_sprites,
			app.upload_assets.itac_capital_sprites,
			app.upload_assets.itac_fighter_sprites,
			app.upload_assets.itac_kills_sprites)
		|| !render::frontend_credits_assets_init(
			app.frontend_renderer,
			app.upload_assets.credits_sprites,
			app.upload_assets.credits_font))
	{
		SDL_Log("Frontend GPU resource initialization failed");
		return false;
	}
	frontend::loadout_catalog_init(
		app.loadout_catalog,
		app.frontend_renderer.loadout_renderer,
		app.game_stats);
	app.upload_assets = {};
	return true;
}
}
