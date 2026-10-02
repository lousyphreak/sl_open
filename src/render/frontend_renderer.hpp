#pragma once

#include "core/math.hpp"
#include "render/model_gpu.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <utility>

namespace sl_open::assets
{
struct Font;
struct TextureImage;
struct GameplayModel;
struct SpriteList;
}

namespace sl_open::render
{
struct FrameGeometryBuffersState;

struct FrameGeometryBuffers
{
	FrameGeometryBuffersState* state{};
};

struct FrameVertexBuffer
{
	std::uint8_t* data{};
	bgfx::DynamicVertexBufferHandle handle{bgfx::kInvalidHandle};
	std::uint32_t start_vertex{};
	std::uint32_t vertex_count{};
	std::uint32_t byte_size{};
	std::uint8_t* uploaded{};
};

struct FrameIndexBuffer
{
	std::uint8_t* data{};
	bgfx::DynamicIndexBufferHandle handle{bgfx::kInvalidHandle};
	std::uint32_t start_index{};
	std::uint32_t index_count{};
	std::uint32_t byte_size{};
	std::uint8_t* uploaded{};
};

constexpr std::uint16_t kFrontendWidth = 640;
constexpr std::uint16_t kFrontendHeight = 480;
constexpr std::uint32_t kMaxFrontendCommands = 4096;
constexpr std::uint32_t kMaxFrontendGlyphs = 336;
constexpr std::uint32_t kMaxFrontendAtlasPages = 8;
constexpr std::uint32_t kEarlyBriefingShapeCount = 183;
constexpr std::uint32_t kLateBriefingShapeCount = 169;
constexpr std::uint32_t kBriefingExitShapeCount = 62;
constexpr std::uint32_t kVrAmbientShapeCount = 116;
constexpr std::uint32_t kLoadoutShapeCount = 42;
constexpr std::uint32_t kDebriefShapeCount = 28;
constexpr std::uint32_t kRestartShapeCount = 20;
constexpr std::uint32_t kCreditsVisualCount = 6;
constexpr std::uint32_t kGameplayHudShapeCount = 411;
constexpr std::uint32_t kGameplayScoreboardShapeCount = 27;

struct FrontendSpriteAtlas
{
	FrontendTexture pages[kMaxFrontendAtlasPages];
	std::uint8_t page_count{};
};

struct FrontendSpriteAtlasSource
{
	const assets::SpriteList* sprites{};
	std::uint32_t shape{};
	FrontendTexture* destination{};
};

struct FrontendTextureStats
{
	std::uint32_t created{};
	std::uint32_t destroyed{};
	std::uint32_t live{};
	std::uint32_t high_water{};
};

struct FrontendGlyph
{
	std::uint16_t x{};
	std::uint16_t y{};
	std::uint16_t width{};
};

enum class FrontendCommandType : std::uint8_t
{
	rgba_quad,
	rgba_additive_quad,
	indexed_quad,
	scissor,
	loadout_scene,
	loadout_text,
	loadout_bar,
	loadout_cursor,
};

struct LoadoutRenderState
{
	std::uint8_t selected_ship{};
	std::uint8_t available_ships{};
	std::uint16_t available_ship_mask{};
	std::uint8_t selector_lod{};
	std::uint8_t page{};
	std::uint8_t previous_ship{};
	std::uint16_t missile_mask{};
	std::uint8_t missile_layout_tier{};
	std::uint8_t previous_page{};
	bool reverse{};
	float spin{};
	float activation{};
	float ship_selection{1.0f};
	float page_transition{1.0f};
	float page_elapsed{};
	float name_flip{};
	float info_flip{};
	std::int8_t pressed_button{-1};
	bool blink_launch{};
	float previous_spin{};
	std::int16_t mounted_loadout[20]{};
	bool missile_animation_active[20]{};
	bool missile_animation_removing[20]{};
	std::uint8_t missile_animation_item[20]{};
	float missile_animation_progress[20]{};
};

struct FrontendCommand
{
	FrontendCommandType type{};
	bgfx::TextureHandle texture{bgfx::kInvalidHandle};
	bgfx::TextureHandle palette{bgfx::kInvalidHandle};
	float x{};
	float y{};
	float width{};
	float height{};
	float u{};
	float v{};
	float uv_width{1.0f};
	float uv_height{1.0f};
	float rotation{};
	std::uint32_t rgba{0xffffffff};
	char text[96]{};
};

static_assert(
	sizeof(FrontendCommand) <= 160,
	"Per-command frontend storage must remain compact");

struct FrontendCommands
{
	FrontendCommand items[kMaxFrontendCommands]{};
	std::uint32_t count{};
	LoadoutRenderState loadout;
};

struct FrontendMedalAssets
{
	FrontendSpriteAtlas early_medal_atlases[3];
	FrontendSpriteAtlas early_bar_atlases[3];
	FrontendSpriteAtlas late_medal_atlases[6];
	FrontendSpriteAtlas late_bar_atlases[5];
	FrontendTexture early_medals[3][17];
	FrontendTexture early_bars[3][15];
	FrontendTexture late_medals[6][8];
	FrontendTexture late_bars[5][8];
	bgfx::TextureHandle early_medal_palettes[3]{
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle};
	bgfx::TextureHandle early_bar_palettes[3]{
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle};
	bgfx::TextureHandle late_medal_palettes[6]{
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle};
	bgfx::TextureHandle late_bar_palettes[5]{
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle, bgfx::kInvalidHandle};
	bool ready{};
};

struct FrontendBriefingAssets
{
	FrontendTexture doors[2];
	FrontendSpriteAtlas early_atlas;
	FrontendSpriteAtlas late_atlas;
	FrontendSpriteAtlas early_exit_atlas;
	FrontendSpriteAtlas late_exit_atlas;
	FrontendTexture early[kEarlyBriefingShapeCount];
	FrontendTexture late[kLateBriefingShapeCount];
	FrontendTexture early_exit[kBriefingExitShapeCount];
	FrontendTexture late_exit[kBriefingExitShapeCount];
	bgfx::TextureHandle palettes[2]{
		bgfx::kInvalidHandle, bgfx::kInvalidHandle};
	bool ready{};
};

struct FrontendLoadoutAssets
{
	FrontendTexture backgrounds[2];
	FrontendSpriteAtlas sprite_atlas;
	FrontendTexture shapes[kLoadoutShapeCount];
	FrontendTexture title_font;
	FrontendGlyph title_glyphs[kMaxFrontendGlyphs]{};
	std::uint32_t title_glyph_count{};
	std::uint32_t title_font_height{};
	FrontendTexture info_font;
	FrontendGlyph info_glyphs[kMaxFrontendGlyphs]{};
	std::uint32_t info_glyph_count{};
	std::uint32_t info_font_height{};
	bgfx::TextureHandle palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle title_font_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle info_font_palette{bgfx::kInvalidHandle};
	bool ready{};
};

struct LoadoutRenderer
{
	FrontendTexture panels;
	FrontendTexture disc[4];
	bgfx::VertexBufferHandle disc_vertices{bgfx::kInvalidHandle};
	FrontendTexture glow;
	bgfx::VertexBufferHandle glow_vertices{bgfx::kInvalidHandle};
	FrontendTexture hardpoints;
	bgfx::VertexBufferHandle hardpoint_vertices{bgfx::kInvalidHandle};
	MissionGpuModel ship_models[12];
	MissionGpuModel gun_models[12];
	MissionGpuModel missile_models[10];
	bool ready{};
};

struct FrontendDebriefAssets
{
	FrontendTexture background;
	FrontendSpriteAtlas sprite_atlas;
	FrontendTexture cursor[21];
	FrontendTexture scroll_arrow[2];
	FrontendTexture action_button[2];
	bgfx::TextureHandle cursor_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle control_palette{bgfx::kInvalidHandle};
	bool ready{};
};

struct FrontendRestartAssets
{
	FrontendSpriteAtlas sprite_atlas;
	FrontendTexture background;
	FrontendTexture highlight;
	FrontendTexture cursor[16];
	bgfx::TextureHandle cursor_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle screen_palette{bgfx::kInvalidHandle};
	bool ready{};
};

struct FrontendSimPodAssets
{
	FrontendTexture backgrounds[2];
	FrontendSpriteAtlas sprite_atlas;
	FrontendTexture shapes[8];
	bgfx::TextureHandle palette{bgfx::kInvalidHandle};
	bool ready{};
};

struct FrontendCdAssets
{
	FrontendTexture early_background;
	FrontendTexture late_background;
	FrontendSpriteAtlas sprite_atlas;
	FrontendTexture shapes[10];
	FrontendTexture font_atlas;
	FrontendGlyph glyphs[kMaxFrontendGlyphs]{};
	std::uint32_t glyph_count{};
	std::uint32_t font_height{};
	bgfx::TextureHandle palette{bgfx::kInvalidHandle};
	bool ready{};
};

struct FrontendCampaignAssets
{
	FrontendTexture background;
	FrontendTexture save_load_background;
	FrontendSpriteAtlas sprite_atlas;
	FrontendTexture cursor[16];
	FrontendTexture pilots[2];
	FrontendTexture controls[11];
	FrontendTexture difficulty_panels[2];
	bgfx::TextureHandle cursor_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle pilot_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle control_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle difficulty_palette{bgfx::kInvalidHandle};
	bool ready{};
};

struct FrontendMultiplayerAssets
{
	FrontendTexture background;
	FrontendTexture lobby_background;
	FrontendSpriteAtlas sprite_atlas;
	FrontendTexture cursor[16];
	FrontendTexture action_button;
	FrontendTexture action_button_hover;
	FrontendTexture provider_button;
	FrontendTexture provider_button_selected;
	FrontendTexture lobby_scroll;
	FrontendTexture lobby_scroll_hover;
	FrontendTexture lobby_checkbox;
	FrontendTexture lobby_checkmark;
	bgfx::TextureHandle cursor_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle screen_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle lobby_checkbox_palette{bgfx::kInvalidHandle};
	bool ready{};
};

struct FrontendVrAssets
{
	FrontendSpriteAtlas sprite_atlas;
	FrontendTexture cursor[50];
	bgfx::TextureHandle cursor_palette{bgfx::kInvalidHandle};
	bool ready{};
};

struct FrontendVrAmbientAssets
{
	FrontendSpriteAtlas sprite_atlas;
	FrontendTexture shapes[kVrAmbientShapeCount];
	bgfx::TextureHandle palette{bgfx::kInvalidHandle};
	std::uint32_t shape_count{};
	bool ready{};
};

struct FrontendShellAssets
{
	FrontendTexture splash;
	FrontendTexture mission_loading_splash;
	FrontendTexture options_background;
	FrontendTexture options_detail_background;
	FrontendTexture in_game_options_background;
	FrontendTexture in_game_options_detail_background;
	FrontendSpriteAtlas frontend_sprite_atlas;
	FrontendSpriteAtlas options_sprite_atlas;
	FrontendSpriteAtlas options_detail_sprite_atlas;
	FrontendSpriteAtlas control_options_sprite_atlas;
	FrontendSpriteAtlas in_game_options_sprite_atlas;
	FrontendSpriteAtlas pause_sprite_atlas;
	FrontendSpriteAtlas gameplay_hud_sprite_atlas;
	FrontendSpriteAtlas gameplay_scoreboard_sprite_atlas;
	FrontendSpriteAtlas about_sprite_atlas;
	FrontendSpriteAtlas quit_sprite_atlas;
	FrontendTexture cursor[16];
	FrontendTexture highlights[3];
	FrontendTexture bottom_icon;
	FrontendTexture bottom_icon_hover;
	FrontendTexture options_highlights[3];
	FrontendTexture options_bottom_icon;
	FrontendTexture options_bottom_icon_hover;
	FrontendTexture options_detail_cursor[16];
	FrontendTexture control_options_cursor[16];
	FrontendTexture in_game_options_cursor[16];
	FrontendTexture in_game_options_highlights[5];
	FrontendTexture in_game_options_bottom_icon;
	FrontendTexture in_game_options_bottom_icon_hover;
	FrontendTexture pause_button;
	FrontendTexture pause_button_hover;
	FrontendTexture pause_panels[6];
	FrontendTexture gameplay_hud_shapes[kGameplayHudShapeCount];
	FrontendTexture gameplay_scoreboard_shapes[
		kGameplayScoreboardShapeCount];
	FrontendTexture gameplay_powerball;
	FrontendTexture gameplay_hud_movie;
	FrontendTexture about_panel;
	FrontendTexture about_button;
	FrontendTexture about_button_hover;
	FrontendTexture quit_background;
	FrontendTexture quit_button;
	FrontendTexture quit_button_hover;
	FrontendTexture font_atlas;
	FrontendGlyph glyphs[kMaxFrontendGlyphs]{};
	std::uint32_t glyph_count{};
	std::uint32_t font_height{};
	FrontendTexture pause_small_font_atlas;
	FrontendGlyph pause_small_glyphs[kMaxFrontendGlyphs]{};
	std::uint32_t pause_small_glyph_count{};
	std::uint32_t pause_small_font_height{};
	FrontendTexture gameplay_hud_font_atlas;
	FrontendGlyph gameplay_hud_glyphs[kMaxFrontendGlyphs]{};
	std::uint32_t gameplay_hud_glyph_count{};
	std::uint32_t gameplay_hud_font_height{};
	FrontendTexture gameplay_scoreboard_font_atlas;
	FrontendGlyph gameplay_scoreboard_glyphs[kMaxFrontendGlyphs]{};
	std::uint32_t gameplay_scoreboard_glyph_count{};
	std::uint32_t gameplay_scoreboard_font_height{};
	std::uint8_t gameplay_powerball_lookup[256 * 256]{};
	std::int16_t gameplay_powerball_coordinates[62 * 62]{};
	std::uint8_t gameplay_powerball_lighting[62 * 62]{};
	std::uint8_t gameplay_powerball_ramp[32 * 32 * 4]{};
	std::uint8_t gameplay_powerball_rgba[62 * 62 * 4]{};
	std::int16_t gameplay_powerball_half_width[62]{};
	std::uint16_t gameplay_powerball_offset{};
	bool gameplay_powerball_valid{};
	FrontendTexture gameplay_message_font_atlas;
	FrontendGlyph gameplay_message_glyphs[kMaxFrontendGlyphs]{};
	std::uint32_t gameplay_message_glyph_count{};
	std::uint32_t gameplay_message_font_height{};
	bgfx::TextureHandle cursor_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle highlight_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle icon_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle options_highlight_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle options_icon_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle options_detail_cursor_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle control_options_cursor_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle in_game_options_cursor_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle in_game_options_highlight_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle in_game_options_icon_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle pause_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle gameplay_hud_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle gameplay_scoreboard_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle gameplay_hud_target_palette[3]{
		bgfx::kInvalidHandle,
		bgfx::kInvalidHandle,
		bgfx::kInvalidHandle,
	};
	bgfx::TextureHandle about_panel_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle about_button_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle quit_background_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle quit_button_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle font_gold_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle font_blue_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle font_white_palette{bgfx::kInvalidHandle};
	std::uint32_t gameplay_hud_rgba[256]{};
	std::uint32_t gameplay_scoreboard_rgba[256]{};
	bool ready{};
};

struct FrontendItacAssets
{
	FrontendTexture backgrounds[8];
	FrontendSpriteAtlas news_atlas;
	FrontendSpriteAtlas video_atlas;
	FrontendSpriteAtlas gfx_atlas;
	FrontendSpriteAtlas squad_atlas;
	FrontendSpriteAtlas person_atlas;
	FrontendSpriteAtlas capital_atlas;
	FrontendSpriteAtlas fighter_atlas;
	FrontendSpriteAtlas kills_atlas;
	FrontendTexture news[30];
	FrontendTexture video[7];
	FrontendTexture faction_icons[2];
	FrontendTexture squads[36];
	FrontendTexture persons[41];
	FrontendTexture capitals[56];
	FrontendTexture fighters[28];
	FrontendTexture kills[22];
	FrontendTexture font_atlas;
	FrontendGlyph glyphs[kMaxFrontendGlyphs]{};
	std::uint32_t glyph_count{};
	std::uint32_t font_height{};
	bgfx::TextureHandle news_palettes[3]{
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle};
	bgfx::TextureHandle video_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle video_thumbnail_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle faction_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle squad_palettes[10]{
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle};
	bgfx::TextureHandle person_palettes[6]{
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle};
	bgfx::TextureHandle capital_palettes[19]{
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle};
	bgfx::TextureHandle fighter_palettes[7]{
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle};
	bgfx::TextureHandle kills_palettes[2]{
		bgfx::kInvalidHandle, bgfx::kInvalidHandle};
	bool ready{};
};

struct FrontendCreditsAssets
{
	FrontendSpriteAtlas sprite_atlas;
	FrontendTexture backgrounds[kCreditsVisualCount];
	FrontendTexture font_atlas;
	FrontendGlyph glyphs[kMaxFrontendGlyphs]{};
	std::uint32_t glyph_count{};
	std::uint32_t font_height{};
	bgfx::TextureHandle background_palettes[kCreditsVisualCount]{
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle,
		bgfx::kInvalidHandle, bgfx::kInvalidHandle, bgfx::kInvalidHandle};
	bgfx::TextureHandle orange_palette{bgfx::kInvalidHandle};
	bgfx::TextureHandle white_palette{bgfx::kInvalidHandle};
	bool ready{};
};

struct FrontendRenderer
{
	mutable FrameGeometryBuffers frame_geometry;
	bgfx::VertexLayout layout;
	bgfx::VertexLayout model_layout;
	bgfx::VertexLayout lit_model_layout;
	bgfx::VertexBufferHandle quad_vertices{bgfx::kInvalidHandle};
	bgfx::IndexBufferHandle quad_indices{bgfx::kInvalidHandle};
	bgfx::ProgramHandle rgba_program{bgfx::kInvalidHandle};
	bgfx::ProgramHandle mission_rgba_program{bgfx::kInvalidHandle};
	bgfx::ProgramHandle indexed_program{bgfx::kInvalidHandle};
	bgfx::ProgramHandle model_rgba_program{bgfx::kInvalidHandle};
	bgfx::ProgramHandle model_mode_six_program{bgfx::kInvalidHandle};
	bgfx::ProgramHandle model_mode_seven_program{bgfx::kInvalidHandle};
	bgfx::ProgramHandle model_mode_eight_program{bgfx::kInvalidHandle};
	bgfx::ProgramHandle planet_rgba_program{bgfx::kInvalidHandle};
	bgfx::ProgramHandle planet_mode_seven_program{bgfx::kInvalidHandle};
	bgfx::UniformHandle lighting_response_sampler{bgfx::kInvalidHandle};
	bgfx::TextureHandle lighting_response_texture{bgfx::kInvalidHandle};
	bgfx::ProgramHandle lit_model_rgba_program{bgfx::kInvalidHandle};
	bgfx::ProgramHandle lit_model_mode_six_program{bgfx::kInvalidHandle};
	bgfx::ProgramHandle lit_model_mode_seven_program{bgfx::kInvalidHandle};
	bgfx::ProgramHandle lit_model_mode_eight_program{bgfx::kInvalidHandle};
	bgfx::UniformHandle texture_sampler{bgfx::kInvalidHandle};
	bgfx::UniformHandle material_texture_sampler{bgfx::kInvalidHandle};
	bgfx::UniformHandle palette_sampler{bgfx::kInvalidHandle};
	bgfx::UniformHandle uv_rect_uniform{bgfx::kInvalidHandle};
	bgfx::UniformHandle tint_uniform{bgfx::kInvalidHandle};
	bgfx::UniformHandle material_diffuse_uniform{bgfx::kInvalidHandle};
	bgfx::UniformHandle model_clip_plane_uniform{bgfx::kInvalidHandle};
	bgfx::UniformHandle lighting_environment_u_uniform{bgfx::kInvalidHandle};
	bgfx::UniformHandle lighting_environment_v_uniform{bgfx::kInvalidHandle};
	bgfx::UniformHandle lighting_base_uniform{bgfx::kInvalidHandle};
	bgfx::UniformHandle lighting_params_uniform{bgfx::kInvalidHandle};
	bgfx::UniformHandle lighting_position_radius_uniform{bgfx::kInvalidHandle};
	bgfx::UniformHandle lighting_direction_type_uniform{bgfx::kInvalidHandle};
	bgfx::UniformHandle lighting_color_intensity_uniform{bgfx::kInvalidHandle};

	FrontendTexture white;
	FrontendTexture model_modifier_textures[8];
	FrontendShellAssets shell;
	FrontendCampaignAssets campaign;
	FrontendMultiplayerAssets multiplayer;
	FrontendVrAssets vr;
	FrontendVrAmbientAssets vr_ambient[2];
	FrontendBriefingAssets briefing;
	FrontendLoadoutAssets loadout;
	LoadoutRenderer loadout_renderer;
	FrontendDebriefAssets debrief;
	FrontendRestartAssets restart;
	FrontendSimPodAssets sim_pod;
	FrontendCdAssets cd;
	FrontendMedalAssets medals;
	FrontendItacAssets itac;
	FrontendCreditsAssets credits;
	bool ready{};
};

bool frame_geometry_init(
	FrameGeometryBuffers& buffers,
	const bgfx::VertexLayout& frontend_layout,
	const bgfx::VertexLayout& model_layout);
bool frame_geometry_register_layout(
	FrameGeometryBuffers& buffers,
	const bgfx::VertexLayout& layout);
void frame_geometry_shutdown(FrameGeometryBuffers& buffers);
void frame_geometry_begin(FrameGeometryBuffers& buffers);
std::uint32_t get_available_frame_vertices(
	const FrameGeometryBuffers& buffers,
	std::uint32_t vertex_count,
	const bgfx::VertexLayout& layout);
std::uint32_t get_available_frame_indices(
	const FrameGeometryBuffers& buffers,
	std::uint32_t index_count,
	bool index32 = false);
void alloc_frame_vertex_buffer(
	FrameGeometryBuffers& buffers,
	FrameVertexBuffer* buffer,
	std::uint32_t vertex_count,
	const bgfx::VertexLayout& layout);
void alloc_frame_index_buffer(
	FrameGeometryBuffers& buffers,
	FrameIndexBuffer* buffer,
	std::uint32_t index_count,
	bool index32 = false);
void set_frame_vertex_buffer(
	std::uint8_t stream,
	FrameVertexBuffer* buffer);
void set_frame_index_buffer(
	FrameIndexBuffer* buffer,
	std::uint32_t first_index = 0,
	std::uint32_t index_count = UINT32_MAX);

bool frontend_renderer_init(
	FrontendRenderer& renderer,
	const assets::TextureImage& splash,
	const assets::TextureImage& mission_loading_splash,
	const assets::TextureImage& options_background,
	const assets::TextureImage& options_detail_background,
	const assets::TextureImage& in_game_options_background,
	const assets::TextureImage& in_game_options_detail_background,
	const assets::SpriteList& sprites,
	const assets::SpriteList& options_sprites,
	const assets::SpriteList& options_detail_sprites,
	const assets::SpriteList& control_options_sprites,
	const assets::SpriteList& in_game_options_sprites,
	const assets::SpriteList& gameplay_hud_sprites,
	const assets::SpriteList& gameplay_scoreboard_sprites,
	const assets::TextureImage& gameplay_powerball,
	const assets::SpriteList& about_sprites,
	const assets::SpriteList& quit_sprites,
	const assets::Font& font,
	const assets::Font& pause_small_font,
	const assets::Font& gameplay_hud_font,
	const assets::Font& gameplay_scoreboard_font,
	const assets::Font& gameplay_message_font);
bool frontend_campaign_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage& campaign_background,
	const assets::TextureImage& save_load_background,
	const assets::SpriteList& campaign_sprites);
bool frontend_multiplayer_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage& background,
	const assets::TextureImage& lobby_background,
	const assets::SpriteList& sprites);
bool frontend_vr_assets_init(
	FrontendRenderer& renderer,
	const assets::SpriteList& vr_sprites);
bool frontend_vr_ambient_assets_init(
	FrontendRenderer& renderer,
	std::uint32_t slot,
	const assets::SpriteList* sprites,
	std::uint32_t shape_count);
bool frontend_briefing_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage (&doors)[2],
	const assets::SpriteList& early_sprites,
	const assets::SpriteList& late_sprites,
	const assets::SpriteList& early_exit_sprites,
	const assets::SpriteList& late_exit_sprites);
bool frontend_loadout_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage (&backgrounds)[2],
	const assets::TextureImage& panels,
	const assets::TextureImage (&disc)[4],
	const assets::TextureImage& glow,
	const assets::TextureImage& hardpoints,
	const assets::SpriteList& sprites,
	const assets::GameplayModel (&ships)[12],
	const assets::GameplayModel (&guns)[12],
	const assets::GameplayModel (&missiles)[10],
	const assets::Font& title_font,
	const assets::Font& info_font,
	const std::uint8_t (&palette)[256 * 4]);
bool frontend_debrief_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage& background,
	const assets::SpriteList& sprites);
bool frontend_restart_assets_init(
	FrontendRenderer& renderer,
	const assets::SpriteList& sprites);
bool frontend_sim_pod_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage (&backgrounds)[2],
	const assets::SpriteList& sprites);
bool frontend_cd_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage& early_background,
	const assets::TextureImage& late_background,
	const assets::SpriteList& sprites,
	const assets::Font& font);
bool frontend_medal_assets_init(
	FrontendRenderer& renderer,
	const assets::SpriteList (&early_medals)[3],
	const assets::SpriteList (&early_bars)[3],
	const assets::SpriteList (&late_medals)[6],
	const assets::SpriteList (&late_bars)[5]);
bool frontend_itac_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage (&backgrounds)[8],
	const assets::Font& font,
	const assets::SpriteList& news_sprites,
	const assets::SpriteList& video_sprites,
	const assets::SpriteList& gfx_sprites,
	const assets::SpriteList& squad_sprites,
	const assets::SpriteList& person_sprites,
	const assets::SpriteList& capital_sprites,
	const assets::SpriteList& fighter_sprites,
	const assets::SpriteList& kills_sprites);
bool frontend_credits_assets_init(
	FrontendRenderer& renderer,
	const assets::SpriteList& sprites,
	const assets::Font& font);
void frontend_renderer_shutdown(FrontendRenderer& renderer);
bool frontend_movie_texture_init(
	FrontendTexture& texture,
	std::uint16_t width,
	std::uint16_t height);
bool frontend_texture_upload(
	FrontendTexture& texture,
	const assets::TextureImage& image);
bool mission_texture_upload(
	FrontendTexture& texture,
	const assets::TextureImage& image);
bool frontend_sprite_atlas_upload(
	FrontendSpriteAtlas& atlas,
	const FrontendSpriteAtlasSource* sources,
	std::uint32_t source_count);
void frontend_movie_texture_update(
	FrontendTexture& texture,
	const std::uint8_t* rgba);
void frontend_gameplay_powerball_update(
	FrontendRenderer& renderer,
	float cursor_x,
	float cursor_y);
void frontend_texture_shutdown(FrontendTexture& texture);
FrontendTextureStats frontend_texture_stats();

void frontend_commands_begin(FrontendCommands& commands);
void frontend_rgba_quad(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	float x,
	float y,
	float width,
	float height,
	std::uint32_t rgba = 0xffffffff);
void frontend_rgba_additive_quad(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	float x,
	float y,
	float width,
	float height,
	std::uint32_t rgba);
void frontend_rgba_region(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	float destination_x,
	float destination_y,
	float destination_width,
	float destination_height,
	std::uint16_t source_x,
	std::uint16_t source_y,
	std::uint16_t source_width,
	std::uint16_t source_height,
	std::uint32_t rgba = 0xffffffff);
void frontend_rgba_rotated_region(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	float destination_x,
	float destination_y,
	float destination_width,
	float destination_height,
	std::uint16_t source_x,
	std::uint16_t source_y,
	std::uint16_t source_width,
	std::uint16_t source_height,
	float rotation,
	std::uint32_t rgba = 0xffffffff);
void frontend_loadout_scene(
	FrontendCommands& commands,
	const LoadoutRenderState& state);
void frontend_loadout_text(
	FrontendCommands& commands,
	std::uint8_t panel,
	float x,
	float y,
	const char* text);
void frontend_loadout_bar(
	FrontendCommands& commands,
	float x,
	float y,
	float width,
	float height,
	bool active);
void frontend_loadout_cursor(
	FrontendCommands& commands,
	float x,
	float y);
void frontend_rgba_line(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	float x1,
	float y1,
	float x2,
	float y2,
	float thickness,
	std::uint32_t rgba);
void frontend_indexed_scaled_quad(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	bgfx::TextureHandle palette,
	float x,
	float y,
	float width,
	float height,
	std::uint32_t rgba = 0xffffffff);
void frontend_indexed_quad(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	bgfx::TextureHandle palette,
	float x,
	float y,
	std::uint32_t rgba = 0xffffffff);
void frontend_indexed_region(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	bgfx::TextureHandle palette,
	float destination_x,
	float destination_y,
	std::uint16_t source_x,
	std::uint16_t source_y,
	std::uint16_t width,
	std::uint16_t height,
	std::uint32_t rgba = 0xffffffff);
void frontend_indexed_scaled_region(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	bgfx::TextureHandle palette,
	float destination_x,
	float destination_y,
	float destination_width,
	float destination_height,
	std::uint16_t source_x,
	std::uint16_t source_y,
	std::uint16_t source_width,
	std::uint16_t source_height,
	std::uint32_t rgba = 0xffffffff);
void frontend_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	bgfx::TextureHandle palette,
	std::uint32_t rgba = 0xffffffff,
	float scale = 1.0f);
void frontend_pause_small_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	bgfx::TextureHandle palette,
	std::uint32_t rgba = 0xffffffff,
	float scale = 1.0f);
void frontend_gameplay_hud_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	std::uint32_t rgba = 0xffffffff,
	float scale = 1.0f);
void frontend_gameplay_scoreboard_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	std::uint32_t rgba = 0xffffffff,
	float scale = 1.0f);
void frontend_gameplay_hud_target_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	std::uint8_t allegiance_class,
	std::uint32_t rgba = 0xffffffff,
	float scale = 1.0f);
void frontend_gameplay_message_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	std::uint32_t rgba = 0xffffffff,
	float scale = 1.0f);
float frontend_pause_small_text_width(
	const FrontendRenderer& renderer,
	const char* text,
	float scale = 1.0f);
void frontend_cd_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	bgfx::TextureHandle palette,
	std::uint32_t rgba = 0xffffffff,
	float scale = 1.0f);
void frontend_itac_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	bgfx::TextureHandle palette,
	std::uint32_t rgba = 0xffffffff,
	float scale = 1.0f);
float frontend_credits_text_width(
	const FrontendRenderer& renderer,
	const char* text);
void frontend_credits_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	bgfx::TextureHandle palette,
	std::uint32_t rgba = 0xffffffff);
void frontend_scissor(
	FrontendCommands& commands,
	std::uint16_t x,
	std::uint16_t y,
	std::uint16_t width,
	std::uint16_t height);
void loadout_renderer_submit(
	const FrontendRenderer& renderer,
	const FrontendCommands& commands,
	std::uint32_t scene_index,
	std::uint16_t viewport_x,
	std::uint16_t viewport_y,
	std::uint16_t viewport_width,
	std::uint16_t viewport_height,
	float brightness);
void frontend_submit(
	const FrontendRenderer& renderer,
	const FrontendCommands& commands,
	std::uint32_t backbuffer_width,
	std::uint32_t backbuffer_height,
	float brightness,
	bool full_viewport = false,
	bool native_canvas = false);

bool frontend_map_input(
	std::uint32_t backbuffer_width,
	std::uint32_t backbuffer_height,
	float physical_x,
	float physical_y,
	float& logical_x,
	float& logical_y);
}
