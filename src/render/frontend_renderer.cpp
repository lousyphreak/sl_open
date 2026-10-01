#include "render/frontend_renderer.hpp"

#include "assets/image.hpp"
#include "assets/ship_model.hpp"
#include "assets/vfx.hpp"
#include "core/blob.hpp"
#include "render/loadout_renderer.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "essl/fs_indexed.sc.bin.h"
#include "essl/fs_model_mode_eight.sc.bin.h"
#include "essl/fs_model_mode_seven.sc.bin.h"
#include "essl/fs_model_mode_six.sc.bin.h"
#include "essl/fs_model_rgba.sc.bin.h"
#include "essl/fs_rgba.sc.bin.h"
#include "essl/vs_frontend.sc.bin.h"
#include "essl/vs_mission.sc.bin.h"
#include "essl/vs_model.sc.bin.h"
#include "essl/vs_model_lit.sc.bin.h"

#if !defined(__EMSCRIPTEN__)
#include "glsl/fs_indexed.sc.bin.h"
#include "glsl/fs_model_mode_eight.sc.bin.h"
#include "glsl/fs_model_mode_seven.sc.bin.h"
#include "glsl/fs_model_mode_six.sc.bin.h"
#include "glsl/fs_model_rgba.sc.bin.h"
#include "glsl/fs_rgba.sc.bin.h"
#include "glsl/vs_frontend.sc.bin.h"
#include "glsl/vs_mission.sc.bin.h"
#include "glsl/vs_model.sc.bin.h"
#include "glsl/vs_model_lit.sc.bin.h"
#include "spirv/fs_indexed.sc.bin.h"
#include "spirv/fs_model_mode_eight.sc.bin.h"
#include "spirv/fs_model_mode_seven.sc.bin.h"
#include "spirv/fs_model_mode_six.sc.bin.h"
#include "spirv/fs_model_rgba.sc.bin.h"
#include "spirv/fs_rgba.sc.bin.h"
#include "spirv/vs_frontend.sc.bin.h"
#include "spirv/vs_mission.sc.bin.h"
#include "spirv/vs_model.sc.bin.h"
#include "spirv/vs_model_lit.sc.bin.h"
#endif

#if defined(_WIN32)
#include "dxbc/fs_indexed.sc.bin.h"
#include "dxbc/fs_model_mode_eight.sc.bin.h"
#include "dxbc/fs_model_mode_seven.sc.bin.h"
#include "dxbc/fs_model_mode_six.sc.bin.h"
#include "dxbc/fs_model_rgba.sc.bin.h"
#include "dxbc/fs_rgba.sc.bin.h"
#include "dxbc/vs_frontend.sc.bin.h"
#include "dxbc/vs_mission.sc.bin.h"
#include "dxbc/vs_model.sc.bin.h"
#include "dxbc/vs_model_lit.sc.bin.h"
#endif

namespace sl_open::render
{
namespace
{
constexpr bgfx::ViewId kClearView = 0;
constexpr bgfx::ViewId kFrontendView = 10;
constexpr bgfx::ViewId kLoadoutView = 11;
constexpr bgfx::ViewId kFrontendOverlayView = 12;
constexpr std::uint64_t kTextureFlags =
	BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
	| BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
// FRONTEND.SPR stores palettes as ordinary directory records before each
// related shape group.
constexpr std::uint32_t kCursorPalette = 0;
constexpr std::uint32_t kCursorFirst = 1;
constexpr std::uint32_t kHighlightPalette = 17;
constexpr std::uint32_t kHighlightFirst = 18;
constexpr std::uint32_t kIconPalette = 22;
constexpr std::uint32_t kIconNormal = 27;
constexpr std::uint32_t kIconHover = 28;
constexpr std::uint32_t kOptionsHighlightPalette = 18;
constexpr std::uint32_t kOptionsHighlightFirst = 19;
constexpr std::uint32_t kOptionsIconPalette = 22;
constexpr std::uint32_t kOptionsIconNormal = 27;
constexpr std::uint32_t kOptionsIconHover = 28;
constexpr std::uint32_t kCampaignCursorPalette = 0;
constexpr std::uint32_t kCampaignCursorFirst = 1;
constexpr std::uint32_t kCampaignPilotPalette = 18;
constexpr std::uint32_t kCampaignPilotFirst = 19;
constexpr std::uint32_t kCampaignControlPalette = 21;
constexpr std::uint32_t kCampaignControlFirst = 22;
constexpr std::uint32_t kDifficultyPalette = 33;
constexpr std::uint32_t kDifficultyFirst = 34;
constexpr std::uint32_t kMultiplayerCursorPalette = 0;
constexpr std::uint32_t kMultiplayerCursorFirst = 1;
constexpr std::uint32_t kMultiplayerScreenPalette = 18;
constexpr std::uint32_t kMultiplayerActionButton = 19;
constexpr std::uint32_t kMultiplayerActionButtonHover = 20;
constexpr std::uint32_t kMultiplayerProviderButton = 23;
constexpr std::uint32_t kMultiplayerProviderButtonSelected = 24;
constexpr std::uint32_t kMultiplayerLobbyScroll = 25;
constexpr std::uint32_t kMultiplayerLobbyScrollHover = 26;
constexpr std::uint32_t kMultiplayerLobbyCheckboxPalette = 27;
constexpr std::uint32_t kMultiplayerLobbyCheckbox = 28;
constexpr std::uint32_t kMultiplayerLobbyCheckmark = 29;
constexpr std::uint32_t kDebriefCursorPalette = 1;
constexpr std::uint32_t kDebriefCursorFirst = 2;
constexpr std::uint32_t kDebriefControlPalette = 23;
constexpr std::uint32_t kDebriefScrollArrowFirst = 24;
constexpr std::uint32_t kDebriefActionButtonFirst = 26;
constexpr std::uint16_t kSpriteAtlasSize = 2048;
constexpr std::uint32_t kMaxSpriteAtlasEntries = 512;

FrontendTextureStats g_texture_stats;

struct SpriteAtlasEntry
{
	FrontendTexture* destination{};
	std::uint16_t x{};
	std::uint16_t y{};
	std::uint16_t width{};
	std::uint16_t height{};
	std::int16_t offset_x{};
	std::int16_t offset_y{};
	std::uint8_t page{};
};

struct SpriteAtlasBuilder
{
	sl_open::Blob pixels[kMaxFrontendAtlasPages];
	SpriteAtlasEntry entries[kMaxSpriteAtlasEntries];
	std::uint32_t entry_count{};
	std::uint16_t x{1};
	std::uint16_t y{1};
	std::uint16_t row_height{};
	std::uint8_t page_count{};
};

struct FrontendVertex
{
	float x;
	float y;
	float z;
	std::uint32_t color;
	float u;
	float v;
};
static_assert(sizeof(FrontendVertex) == sizeof(float) * 6);

struct ShaderBytes
{
	const std::uint8_t* data{};
	std::uint32_t size{};
};

enum class ShaderKind
{
	vertex,
	mission_vertex,
	model_vertex,
	model_lit_vertex,
	rgba,
	indexed,
	model_rgba,
	model_mode_eight,
	model_mode_six,
	model_mode_seven,
};

[[noreturn]] void command_overflow()
{
	std::fputs(
		"sl_open: frontend render command limit (4096) exceeded\n",
		stderr);
	std::abort();
}

FrontendCommand& push_command(
	FrontendCommands& commands,
	FrontendCommandType type)
{
	if (commands.count >= kMaxFrontendCommands)
	{
		command_overflow();
	}
	FrontendCommand& command = commands.items[commands.count++];
	command = {};
	command.type = type;
	command.rgba = 0xffffffff;
	command.uv_width = 1.0f;
	command.uv_height = 1.0f;
	return command;
}

template <std::size_t Size>
ShaderBytes shader_array(const std::uint8_t (&bytes)[Size])
{
	return {bytes, static_cast<std::uint32_t>(Size)};
}

ShaderBytes shader_bytes(bgfx::RendererType::Enum type, ShaderKind kind)
{
	switch (type)
	{
	case bgfx::RendererType::OpenGLES:
		switch (kind)
		{
		case ShaderKind::vertex: return shader_array(vs_frontend_essl);
		case ShaderKind::mission_vertex: return shader_array(vs_mission_essl);
		case ShaderKind::model_vertex: return shader_array(vs_model_essl);
		case ShaderKind::model_lit_vertex:
			return shader_array(vs_model_lit_essl);
		case ShaderKind::rgba: return shader_array(fs_rgba_essl);
		case ShaderKind::indexed: return shader_array(fs_indexed_essl);
		case ShaderKind::model_rgba:
			return shader_array(fs_model_rgba_essl);
		case ShaderKind::model_mode_eight:
			return shader_array(fs_model_mode_eight_essl);
		case ShaderKind::model_mode_six:
			return shader_array(fs_model_mode_six_essl);
		case ShaderKind::model_mode_seven:
			return shader_array(fs_model_mode_seven_essl);
		}
		break;
#if !defined(__EMSCRIPTEN__)
	case bgfx::RendererType::OpenGL:
		switch (kind)
		{
		case ShaderKind::vertex: return shader_array(vs_frontend_glsl);
		case ShaderKind::mission_vertex: return shader_array(vs_mission_glsl);
		case ShaderKind::model_vertex: return shader_array(vs_model_glsl);
		case ShaderKind::model_lit_vertex:
			return shader_array(vs_model_lit_glsl);
		case ShaderKind::rgba: return shader_array(fs_rgba_glsl);
		case ShaderKind::indexed: return shader_array(fs_indexed_glsl);
		case ShaderKind::model_rgba:
			return shader_array(fs_model_rgba_glsl);
		case ShaderKind::model_mode_eight:
			return shader_array(fs_model_mode_eight_glsl);
		case ShaderKind::model_mode_six:
			return shader_array(fs_model_mode_six_glsl);
		case ShaderKind::model_mode_seven:
			return shader_array(fs_model_mode_seven_glsl);
		}
		break;
	case bgfx::RendererType::Vulkan:
		switch (kind)
		{
		case ShaderKind::vertex: return shader_array(vs_frontend_spirv);
		case ShaderKind::mission_vertex: return shader_array(vs_mission_spirv);
		case ShaderKind::model_vertex: return shader_array(vs_model_spirv);
		case ShaderKind::model_lit_vertex:
			return shader_array(vs_model_lit_spirv);
		case ShaderKind::rgba: return shader_array(fs_rgba_spirv);
		case ShaderKind::indexed: return shader_array(fs_indexed_spirv);
		case ShaderKind::model_rgba:
			return shader_array(fs_model_rgba_spirv);
		case ShaderKind::model_mode_eight:
			return shader_array(fs_model_mode_eight_spirv);
		case ShaderKind::model_mode_six:
			return shader_array(fs_model_mode_six_spirv);
		case ShaderKind::model_mode_seven:
			return shader_array(fs_model_mode_seven_spirv);
		}
		break;
#endif
#if defined(_WIN32)
	case bgfx::RendererType::Direct3D11:
	case bgfx::RendererType::Direct3D12:
		switch (kind)
		{
		case ShaderKind::vertex: return shader_array(vs_frontend_dxbc);
		case ShaderKind::mission_vertex: return shader_array(vs_mission_dxbc);
		case ShaderKind::model_vertex: return shader_array(vs_model_dxbc);
		case ShaderKind::model_lit_vertex:
			return shader_array(vs_model_lit_dxbc);
		case ShaderKind::rgba: return shader_array(fs_rgba_dxbc);
		case ShaderKind::indexed: return shader_array(fs_indexed_dxbc);
		case ShaderKind::model_rgba:
			return shader_array(fs_model_rgba_dxbc);
		case ShaderKind::model_mode_eight:
			return shader_array(fs_model_mode_eight_dxbc);
		case ShaderKind::model_mode_six:
			return shader_array(fs_model_mode_six_dxbc);
		case ShaderKind::model_mode_seven:
			return shader_array(fs_model_mode_seven_dxbc);
		}
		break;
#endif
	default:
		break;
	}
	return {};
}

bgfx::ShaderHandle create_shader(ShaderKind kind)
{
	const ShaderBytes bytes = shader_bytes(bgfx::getRendererType(), kind);
	if (bytes.data == nullptr)
	{
		return BGFX_INVALID_HANDLE;
	}
	return bgfx::createShader(bgfx::makeRef(bytes.data, bytes.size));
}

FrontendTexture create_texture(
	std::uint16_t width,
	std::uint16_t height,
	bgfx::TextureFormat::Enum format,
	const void* pixels,
	std::uint32_t byte_count,
	std::uint64_t flags = kTextureFlags)
{
	FrontendTexture result;
	const bgfx::Memory* memory = bgfx::copy(pixels, byte_count);
	result.handle = bgfx::createTexture2D(
		width, height, false, 1, format, flags, memory);
	if (bgfx::isValid(result.handle))
	{
		result.width = width;
		result.height = height;
		result.owns_handle = true;
		++g_texture_stats.created;
		++g_texture_stats.live;
		g_texture_stats.high_water =
			std::max(g_texture_stats.high_water, g_texture_stats.live);
	}
	return result;
}

FrontendTexture create_mip_texture(
	const assets::TextureImage& image,
	std::uint64_t flags)
{
	FrontendTexture result;
	const bool rgb565 =
		image.pixel_format == assets::TexturePixelFormat::rgb565;
	const std::uint32_t bytes_per_pixel = rgb565 ? 2u : 4u;
	const bgfx::TextureFormat::Enum bgfx_format =
		rgb565
			// bimg names the little-endian word layout B5G6R5. Its unpacker
			// maps bits 11..15 to red, exactly matching the cache masks
			// {R=f800,G=07e0,B=001f}; R5G6B5 would swap red and blue.
			? bgfx::TextureFormat::B5G6R5
			: bgfx::TextureFormat::RGBA8;
	if (image.mip_levels <= 1)
	{
		return create_texture(
			static_cast<std::uint16_t>(image.width),
			static_cast<std::uint16_t>(image.height),
			bgfx_format,
			image.pixels.data,
			static_cast<std::uint32_t>(
				static_cast<std::uint64_t>(image.width)
					* image.height * bytes_per_pixel),
			flags);
	}

	// createTexture2D's boolean mip argument requires a complete chain down
	// to 1x1. Retail cache records deliberately stop at their authored level
	// count (normally 2x2), and srd3d 0x10001298..0x10001452 creates exactly
	// that many attached surfaces. KTX carries an explicit mip count, so the
	// bgfx container path preserves the same partial chain without inventing
	// a final level.
	constexpr std::uint8_t identifier[12] = {
		0xab, 'K', 'T', 'X', ' ', '1', '1', 0xbb,
		'\r', '\n', 0x1a, '\n',
	};
	std::uint64_t container_bytes = 64;
	std::uint32_t measured_width = image.width;
	std::uint32_t measured_height = image.height;
	for (std::uint32_t level = 0;
		level < image.mip_levels;
		++level)
	{
		const std::uint64_t level_bytes =
			static_cast<std::uint64_t>(measured_width)
				* measured_height * bytes_per_pixel;
		container_bytes += 4u + ((level_bytes + 3u) & ~3u);
		measured_width = std::max(measured_width / 2, 1u);
		measured_height = std::max(measured_height / 2, 1u);
	}
	if (container_bytes > UINT32_MAX)
	{
		return result;
	}
	sl_open::Blob container;
	if (!container.allocate(static_cast<std::size_t>(container_bytes)))
	{
		return result;
	}
	std::memcpy(container.data, identifier, sizeof(identifier));
	auto write_u32 = [&container](
		std::size_t offset, std::uint32_t value)
	{
		container.data[offset] =
			static_cast<std::uint8_t>(value);
		container.data[offset + 1] =
			static_cast<std::uint8_t>(value >> 8);
		container.data[offset + 2] =
			static_cast<std::uint8_t>(value >> 16);
		container.data[offset + 3] =
			static_cast<std::uint8_t>(value >> 24);
	};
	constexpr std::uint32_t kGlUnsignedByte = 0x1401;
	constexpr std::uint32_t kGlUnsignedShort565 = 0x8363;
	constexpr std::uint32_t kGlRgb = 0x1907;
	constexpr std::uint32_t kGlRgba = 0x1908;
	constexpr std::uint32_t kGlRgba8 = 0x8058;
	constexpr std::uint32_t kGlRgb565 = 0x8d62;
	const std::uint32_t header[13] = {
		0x04030201,
		rgb565 ? kGlUnsignedShort565 : kGlUnsignedByte,
		rgb565 ? 2u : 1u,
		rgb565 ? kGlRgb : kGlRgba,
		rgb565 ? kGlRgb565 : kGlRgba8,
		rgb565 ? kGlRgb : kGlRgba,
		image.width,
		image.height,
		0,
		0,
		1,
		image.mip_levels,
		0,
	};
	for (std::size_t index = 0; index < std::size(header); ++index)
	{
		write_u32(12 + index * 4, header[index]);
	}
	std::size_t source_offset = 0;
	std::size_t destination_offset = 64;
	std::uint32_t width = image.width;
	std::uint32_t height = image.height;
	for (std::uint32_t level = 0;
		level < image.mip_levels;
		++level)
	{
		const std::uint64_t level_bytes_64 =
			static_cast<std::uint64_t>(width)
				* height * bytes_per_pixel;
		if (level_bytes_64 > UINT32_MAX
			|| source_offset + level_bytes_64 > image.pixels.size)
		{
			return result;
		}
		const std::uint32_t level_bytes =
			static_cast<std::uint32_t>(level_bytes_64);
		write_u32(destination_offset, level_bytes);
		destination_offset += 4;
		std::memcpy(
			container.data + destination_offset,
			image.pixels.data + source_offset,
			level_bytes);
		source_offset += level_bytes;
		destination_offset += level_bytes;
		destination_offset =
			(destination_offset + 3u) & ~std::size_t{3u};
		width = std::max(width / 2, 1u);
		height = std::max(height / 2, 1u);
	}
	if (source_offset != image.pixels.size
		|| destination_offset != container.size)
	{
		return result;
	}
	bgfx::TextureInfo info;
	result.handle = bgfx::createTexture(
		bgfx::copy(
			container.data,
			static_cast<std::uint32_t>(container.size)),
		flags,
		0,
		&info);
	if (bgfx::isValid(result.handle))
	{
		result.width = static_cast<std::uint16_t>(image.width);
		result.height = static_cast<std::uint16_t>(image.height);
		result.owns_handle = true;
		++g_texture_stats.created;
		++g_texture_stats.live;
		g_texture_stats.high_water =
			std::max(g_texture_stats.high_water, g_texture_stats.live);
	}
	return result;
}

bool begin_atlas_page(SpriteAtlasBuilder& builder)
{
	if (builder.page_count >= kMaxFrontendAtlasPages
		|| !builder.pixels[builder.page_count].allocate(
			static_cast<std::size_t>(kSpriteAtlasSize)
				* kSpriteAtlasSize * 2))
	{
		return false;
	}
	std::memset(
		builder.pixels[builder.page_count].data,
		0,
		builder.pixels[builder.page_count].size);
	++builder.page_count;
	builder.x = 1;
	builder.y = 1;
	builder.row_height = 0;
	return true;
}

bool add_sprite_to_atlas(
	SpriteAtlasBuilder& builder,
	const assets::SpriteList& sprites,
	std::uint32_t index,
	FrontendTexture& destination,
	bool allow_empty = false)
{
	assets::IndexedImage image;
	if (!assets::decode_sprite_shape(sprites, index, image))
	{
		return false;
	}
	if (image.width == 0 || image.height == 0)
	{
		return allow_empty;
	}
	if (image.width + 2 > kSpriteAtlasSize
		|| image.height + 2 > kSpriteAtlasSize
		|| image.min_x < INT16_MIN || image.min_x > INT16_MAX
		|| image.min_y < INT16_MIN || image.min_y > INT16_MAX
		|| builder.entry_count >= kMaxSpriteAtlasEntries)
	{
		return false;
	}
	if (builder.page_count == 0 && !begin_atlas_page(builder))
	{
		return false;
	}
	if (builder.x + image.width + 1 > kSpriteAtlasSize)
	{
		builder.x = 1;
		builder.y = static_cast<std::uint16_t>(
			builder.y + builder.row_height + 1);
		builder.row_height = 0;
	}
	if (builder.y + image.height + 1 > kSpriteAtlasSize)
	{
		if (!begin_atlas_page(builder))
		{
			return false;
		}
	}

	const std::uint8_t page = static_cast<std::uint8_t>(
		builder.page_count - 1);
	const auto* source =
		reinterpret_cast<const assets::IndexedPixel*>(image.pixels.data);
	std::uint8_t* output = builder.pixels[page].data;
	for (std::uint32_t row = 0; row < image.height; ++row)
	{
		const std::size_t output_offset =
			(static_cast<std::size_t>(builder.y + row) * kSpriteAtlasSize
				+ builder.x) * 2;
		std::memcpy(
			output + output_offset,
			source + static_cast<std::size_t>(row) * image.width,
			static_cast<std::size_t>(image.width) * 2);
	}
	builder.entries[builder.entry_count++] = {
		&destination,
		builder.x,
		builder.y,
		static_cast<std::uint16_t>(image.width),
		static_cast<std::uint16_t>(image.height),
		static_cast<std::int16_t>(image.min_x),
		static_cast<std::int16_t>(image.min_y),
		page,
	};
	builder.x = static_cast<std::uint16_t>(
		builder.x + image.width + 1);
	builder.row_height = std::max(
		builder.row_height, static_cast<std::uint16_t>(image.height));
	return true;
}

bool add_sprite_range_to_atlas(
	SpriteAtlasBuilder& builder,
	const assets::SpriteList& sprites,
	std::uint32_t first,
	std::uint32_t end,
	FrontendTexture* destinations)
{
	for (std::uint32_t shape = first; shape < end; ++shape)
	{
		if (!add_sprite_to_atlas(
				builder, sprites, shape, destinations[shape]))
		{
			return false;
		}
	}
	return true;
}

bool finish_sprite_atlas(
	SpriteAtlasBuilder& builder,
	FrontendSpriteAtlas& atlas)
{
	atlas = {};
	for (std::uint8_t page = 0; page < builder.page_count; ++page)
	{
		atlas.pages[page] = create_texture(
			kSpriteAtlasSize,
			kSpriteAtlasSize,
			bgfx::TextureFormat::RG8,
			builder.pixels[page].data,
			static_cast<std::uint32_t>(builder.pixels[page].size));
		builder.pixels[page].reset();
		if (!bgfx::isValid(atlas.pages[page].handle))
		{
			return false;
		}
	}
	atlas.page_count = builder.page_count;
	for (std::uint32_t index = 0; index < builder.entry_count; ++index)
	{
		const SpriteAtlasEntry& entry = builder.entries[index];
		*entry.destination = {
			atlas.pages[entry.page].handle,
			entry.width,
			entry.height,
			entry.offset_x,
			entry.offset_y,
			static_cast<float>(entry.x) / kSpriteAtlasSize,
			static_cast<float>(entry.y) / kSpriteAtlasSize,
			static_cast<float>(entry.width) / kSpriteAtlasSize,
			static_cast<float>(entry.height) / kSpriteAtlasSize,
			false,
		};
	}
	return true;
}

bgfx::TextureHandle create_palette(const std::uint8_t* rgba)
{
	const FrontendTexture texture = create_texture(
		256, 1, bgfx::TextureFormat::RGBA8, rgba, 256 * 4);
	return texture.handle;
}

void make_font_palette(
	std::uint8_t* palette,
	std::uint8_t dark_r,
	std::uint8_t dark_g,
	std::uint8_t dark_b,
	std::uint8_t bright_r,
	std::uint8_t bright_g,
	std::uint8_t bright_b)
{
	std::memset(palette, 0, 256 * 4);
	for (std::uint32_t index = 1; index < 256; ++index)
	{
		const std::uint32_t step = std::min(index, 16u);
		palette[index * 4 + 0] = static_cast<std::uint8_t>(
			dark_r + (bright_r - dark_r) * step / 16);
		palette[index * 4 + 1] = static_cast<std::uint8_t>(
			dark_g + (bright_g - dark_g) * step / 16);
		palette[index * 4 + 2] = static_cast<std::uint8_t>(
			dark_b + (bright_b - dark_b) * step / 16);
		palette[index * 4 + 3] = 255;
	}
}

void make_credits_font_palette(
	std::uint8_t* palette,
	std::uint8_t red,
	std::uint8_t green,
	std::uint8_t blue)
{
	std::memset(palette, 0, 256 * 4);
	for (std::uint32_t index = 1; index < 16; ++index)
	{
		palette[index * 4 + 0] = static_cast<std::uint8_t>(
			static_cast<std::uint32_t>(red) * index / 15);
		palette[index * 4 + 1] = static_cast<std::uint8_t>(
			static_cast<std::uint32_t>(green) * index / 15);
		palette[index * 4 + 2] = static_cast<std::uint8_t>(
			static_cast<std::uint32_t>(blue) * index / 15);
		palette[index * 4 + 3] = 255;
	}
}

bool create_font_atlas(
	const assets::Font& font,
	FrontendTexture& atlas,
	FrontendGlyph* glyphs,
	std::uint32_t& glyph_count,
	std::uint32_t& font_height)
{
	constexpr std::uint16_t atlas_width = 512;
	constexpr std::uint16_t atlas_height = 256;
	if (font.directory_count > kMaxFrontendGlyphs)
	{
		return false;
	}

	sl_open::Blob pixels;
	if (!pixels.allocate(atlas_width * atlas_height * 2))
	{
		return false;
	}
	std::memset(pixels.data, 0, pixels.size);

	std::uint16_t x = 1;
	std::uint16_t y = 1;
	for (std::uint32_t index = 0; index < font.directory_count; ++index)
	{
		assets::GlyphView glyph;
		if (!assets::font_glyph(font, index, glyph)
			|| glyph.width > atlas_width - 2)
		{
			return false;
		}
		if (x + glyph.width + 1 > atlas_width)
		{
			x = 1;
			y = static_cast<std::uint16_t>(y + font.height + 1);
		}
		if (y + font.height + 1 > atlas_height)
		{
			return false;
		}

		glyphs[index] = {
			x, y, static_cast<std::uint16_t>(glyph.width)};
		for (std::uint32_t row = 0; row < glyph.height; ++row)
		{
			for (std::uint32_t column = 0; column < glyph.width; ++column)
			{
				const std::uint8_t value =
					glyph.pixels[row * glyph.width + column];
				const std::size_t destination =
					(static_cast<std::size_t>(y + row) * atlas_width
						+ x + column) * 2;
				pixels.data[destination] = value;
				pixels.data[destination + 1] = value == 0 ? 0 : 255;
			}
		}
		x = static_cast<std::uint16_t>(x + glyph.width + 1);
	}

	atlas = create_texture(
		atlas_width,
		atlas_height,
		bgfx::TextureFormat::RG8,
		pixels.data,
		static_cast<std::uint32_t>(pixels.size));
	glyph_count = font.directory_count;
	font_height = font.height;
	return bgfx::isValid(atlas.handle);
}

void destroy_texture(FrontendTexture& texture)
{
	if (texture.owns_handle && bgfx::isValid(texture.handle))
	{
		bgfx::destroy(texture.handle);
		++g_texture_stats.destroyed;
		if (g_texture_stats.live != 0)
		{
			--g_texture_stats.live;
		}
	}
	if (texture.owns_handle && bgfx::isValid(texture.stream_back))
	{
		bgfx::destroy(texture.stream_back);
		++g_texture_stats.destroyed;
		--g_texture_stats.live;
	}
	texture = {};
}

void destroy_texture_handle(bgfx::TextureHandle& handle)
{
	if (bgfx::isValid(handle))
	{
		bgfx::destroy(handle);
		++g_texture_stats.destroyed;
		if (g_texture_stats.live != 0)
		{
			--g_texture_stats.live;
		}
	}
	handle = BGFX_INVALID_HANDLE;
}

template <typename Handle>
void destroy_handle(Handle& handle);

void destroy_medal_assets(FrontendMedalAssets& assets)
{
	for (auto& award : assets.early_medals)
	{
		for (FrontendTexture& texture : award) destroy_texture(texture);
	}
	for (auto& award : assets.early_bars)
	{
		for (FrontendTexture& texture : award) destroy_texture(texture);
	}
	for (auto& award : assets.late_medals)
	{
		for (FrontendTexture& texture : award) destroy_texture(texture);
	}
	for (auto& award : assets.late_bars)
	{
		for (FrontendTexture& texture : award) destroy_texture(texture);
	}
	auto destroy_atlases = [](auto& atlases)
	{
		for (FrontendSpriteAtlas& atlas : atlases)
		{
			for (FrontendTexture& page : atlas.pages) destroy_texture(page);
			atlas.page_count = 0;
		}
	};
	destroy_atlases(assets.early_medal_atlases);
	destroy_atlases(assets.early_bar_atlases);
	destroy_atlases(assets.late_medal_atlases);
	destroy_atlases(assets.late_bar_atlases);
	for (bgfx::TextureHandle& palette : assets.early_medal_palettes)
	{
		destroy_texture_handle(palette);
	}
	for (bgfx::TextureHandle& palette : assets.early_bar_palettes)
	{
		destroy_texture_handle(palette);
	}
	for (bgfx::TextureHandle& palette : assets.late_medal_palettes)
	{
		destroy_texture_handle(palette);
	}
	for (bgfx::TextureHandle& palette : assets.late_bar_palettes)
	{
		destroy_texture_handle(palette);
	}
	assets.ready = false;
}

void destroy_briefing_assets(FrontendBriefingAssets& assets)
{
	auto destroy_textures = [](auto& textures)
	{
		for (FrontendTexture& texture : textures) destroy_texture(texture);
	};
	destroy_textures(assets.doors);
	destroy_textures(assets.early);
	destroy_textures(assets.late);
	destroy_textures(assets.early_exit);
	destroy_textures(assets.late_exit);
	FrontendSpriteAtlas* const atlases[] = {
		&assets.early_atlas,
		&assets.late_atlas,
		&assets.early_exit_atlas,
		&assets.late_exit_atlas,
	};
	for (FrontendSpriteAtlas* atlas : atlases)
	{
		for (FrontendTexture& page : atlas->pages) destroy_texture(page);
		atlas->page_count = 0;
	}
	for (bgfx::TextureHandle& palette : assets.palettes)
	{
		destroy_texture_handle(palette);
	}
	assets.ready = false;
}

void destroy_loadout_assets(FrontendLoadoutAssets& assets)
{
	auto destroy_textures = [](auto& textures)
	{
		for (FrontendTexture& texture : textures) destroy_texture(texture);
	};
	destroy_textures(assets.backgrounds);
	destroy_textures(assets.shapes);
	for (FrontendTexture& page : assets.sprite_atlas.pages)
	{
		destroy_texture(page);
	}
	assets.sprite_atlas.page_count = 0;
	destroy_texture(assets.title_font);
	destroy_texture(assets.info_font);
	destroy_texture_handle(assets.palette);
	destroy_texture_handle(assets.title_font_palette);
	destroy_texture_handle(assets.info_font_palette);
	assets.ready = false;
}

void destroy_debrief_assets(FrontendDebriefAssets& assets)
{
	destroy_texture(assets.background);
	for (FrontendTexture& texture : assets.cursor)
	{
		destroy_texture(texture);
	}
	for (FrontendTexture& texture : assets.scroll_arrow)
	{
		destroy_texture(texture);
	}
	for (FrontendTexture& texture : assets.action_button)
	{
		destroy_texture(texture);
	}
	for (FrontendTexture& page : assets.sprite_atlas.pages)
	{
		destroy_texture(page);
	}
	assets.sprite_atlas.page_count = 0;
	destroy_texture_handle(assets.cursor_palette);
	destroy_texture_handle(assets.control_palette);
	assets.ready = false;
}

void destroy_restart_assets(FrontendRestartAssets& assets)
{
	destroy_texture(assets.background);
	destroy_texture(assets.highlight);
	for (FrontendTexture& texture : assets.cursor)
	{
		destroy_texture(texture);
	}
	for (FrontendTexture& page : assets.sprite_atlas.pages)
	{
		destroy_texture(page);
	}
	assets.sprite_atlas.page_count = 0;
	destroy_texture_handle(assets.cursor_palette);
	destroy_texture_handle(assets.screen_palette);
	assets.ready = false;
}

void destroy_sim_pod_assets(FrontendSimPodAssets& assets)
{
	for (FrontendTexture& texture : assets.backgrounds)
	{
		destroy_texture(texture);
	}
	for (FrontendTexture& texture : assets.shapes)
	{
		destroy_texture(texture);
	}
	for (FrontendTexture& page : assets.sprite_atlas.pages)
	{
		destroy_texture(page);
	}
	assets.sprite_atlas.page_count = 0;
	destroy_texture_handle(assets.palette);
	assets.ready = false;
}

void destroy_cd_assets(FrontendCdAssets& assets)
{
	destroy_texture(assets.early_background);
	destroy_texture(assets.late_background);
	for (FrontendTexture& texture : assets.shapes)
	{
		destroy_texture(texture);
	}
	for (FrontendTexture& page : assets.sprite_atlas.pages)
	{
		destroy_texture(page);
	}
	assets.sprite_atlas.page_count = 0;
	destroy_texture(assets.font_atlas);
	destroy_texture_handle(assets.palette);
	assets.ready = false;
}

void destroy_campaign_assets(FrontendCampaignAssets& assets)
{
	destroy_texture(assets.background);
	destroy_texture(assets.save_load_background);
	auto destroy_textures = [](auto& textures)
	{
		for (FrontendTexture& texture : textures) destroy_texture(texture);
	};
	destroy_textures(assets.cursor);
	destroy_textures(assets.pilots);
	destroy_textures(assets.controls);
	destroy_textures(assets.difficulty_panels);
	for (FrontendTexture& page : assets.sprite_atlas.pages)
	{
		destroy_texture(page);
	}
	assets.sprite_atlas.page_count = 0;
	destroy_texture_handle(assets.cursor_palette);
	destroy_texture_handle(assets.pilot_palette);
	destroy_texture_handle(assets.control_palette);
	destroy_texture_handle(assets.difficulty_palette);
	assets.ready = false;
}

void destroy_multiplayer_assets(FrontendMultiplayerAssets& assets)
{
	destroy_texture(assets.background);
	destroy_texture(assets.lobby_background);
	for (FrontendTexture& texture : assets.cursor)
	{
		destroy_texture(texture);
	}
	destroy_texture(assets.action_button);
	destroy_texture(assets.action_button_hover);
	destroy_texture(assets.provider_button);
	destroy_texture(assets.provider_button_selected);
	destroy_texture(assets.lobby_scroll);
	destroy_texture(assets.lobby_scroll_hover);
	destroy_texture(assets.lobby_checkbox);
	destroy_texture(assets.lobby_checkmark);
	for (FrontendTexture& page : assets.sprite_atlas.pages)
	{
		destroy_texture(page);
	}
	assets.sprite_atlas.page_count = 0;
	destroy_texture_handle(assets.cursor_palette);
	destroy_texture_handle(assets.screen_palette);
	destroy_texture_handle(assets.lobby_checkbox_palette);
	assets.ready = false;
}

void destroy_vr_assets(FrontendVrAssets& assets)
{
	for (FrontendTexture& texture : assets.cursor)
	{
		destroy_texture(texture);
	}
	for (FrontendTexture& page : assets.sprite_atlas.pages)
	{
		destroy_texture(page);
	}
	assets.sprite_atlas.page_count = 0;
	destroy_texture_handle(assets.cursor_palette);
	assets.ready = false;
}

void destroy_vr_ambient_assets(FrontendVrAmbientAssets& assets)
{
	for (FrontendTexture& texture : assets.shapes)
	{
		destroy_texture(texture);
	}
	for (FrontendTexture& page : assets.sprite_atlas.pages)
	{
		destroy_texture(page);
	}
	assets.sprite_atlas.page_count = 0;
	destroy_texture_handle(assets.palette);
	assets.shape_count = 0;
	assets.ready = false;
}

void destroy_shell_assets(FrontendShellAssets& assets)
{
	destroy_texture(assets.splash);
	destroy_texture(assets.mission_loading_splash);
	destroy_texture(assets.options_background);
	destroy_texture(assets.options_detail_background);
	destroy_texture(assets.in_game_options_background);
	destroy_texture(assets.in_game_options_detail_background);
	auto destroy_textures = [](auto& textures)
	{
		for (FrontendTexture& texture : textures) destroy_texture(texture);
	};
	destroy_textures(assets.cursor);
	destroy_textures(assets.highlights);
	destroy_texture(assets.bottom_icon);
	destroy_texture(assets.bottom_icon_hover);
	destroy_textures(assets.options_highlights);
	destroy_texture(assets.options_bottom_icon);
	destroy_texture(assets.options_bottom_icon_hover);
	destroy_textures(assets.options_detail_cursor);
	destroy_textures(assets.control_options_cursor);
	destroy_textures(assets.in_game_options_cursor);
	destroy_textures(assets.in_game_options_highlights);
	destroy_texture(assets.in_game_options_bottom_icon);
	destroy_texture(assets.in_game_options_bottom_icon_hover);
	destroy_texture(assets.pause_button);
	destroy_texture(assets.pause_button_hover);
	destroy_textures(assets.pause_panels);
	destroy_textures(assets.gameplay_hud_shapes);
	destroy_textures(assets.gameplay_scoreboard_shapes);
	destroy_texture(assets.gameplay_powerball);
	destroy_texture(assets.gameplay_hud_movie);
	destroy_texture(assets.about_panel);
	destroy_texture(assets.about_button);
	destroy_texture(assets.about_button_hover);
	destroy_texture(assets.quit_background);
	destroy_texture(assets.quit_button);
	destroy_texture(assets.quit_button_hover);
	FrontendSpriteAtlas* const atlases[] = {
		&assets.frontend_sprite_atlas,
		&assets.options_sprite_atlas,
		&assets.options_detail_sprite_atlas,
		&assets.control_options_sprite_atlas,
		&assets.in_game_options_sprite_atlas,
		&assets.pause_sprite_atlas,
		&assets.gameplay_hud_sprite_atlas,
		&assets.gameplay_scoreboard_sprite_atlas,
		&assets.about_sprite_atlas,
		&assets.quit_sprite_atlas,
	};
	for (FrontendSpriteAtlas* atlas : atlases)
	{
		for (FrontendTexture& page : atlas->pages) destroy_texture(page);
		atlas->page_count = 0;
	}
	destroy_texture(assets.font_atlas);
	destroy_texture(assets.pause_small_font_atlas);
	destroy_texture(assets.gameplay_hud_font_atlas);
	destroy_texture(assets.gameplay_scoreboard_font_atlas);
	destroy_texture(assets.gameplay_message_font_atlas);
	bgfx::TextureHandle* const palettes[] = {
		&assets.cursor_palette,
		&assets.highlight_palette,
		&assets.icon_palette,
		&assets.options_highlight_palette,
		&assets.options_icon_palette,
		&assets.options_detail_cursor_palette,
		&assets.control_options_cursor_palette,
		&assets.in_game_options_cursor_palette,
		&assets.in_game_options_highlight_palette,
		&assets.in_game_options_icon_palette,
		&assets.pause_palette,
		&assets.gameplay_hud_palette,
		&assets.gameplay_scoreboard_palette,
		&assets.gameplay_hud_target_palette[0],
		&assets.gameplay_hud_target_palette[1],
		&assets.gameplay_hud_target_palette[2],
		&assets.about_panel_palette,
		&assets.about_button_palette,
		&assets.quit_background_palette,
		&assets.quit_button_palette,
		&assets.font_gold_palette,
		&assets.font_blue_palette,
		&assets.font_white_palette,
	};
	for (bgfx::TextureHandle* palette : palettes)
	{
		destroy_texture_handle(*palette);
	}
	assets.glyph_count = 0;
	assets.font_height = 0;
	assets.pause_small_glyph_count = 0;
	assets.pause_small_font_height = 0;
	assets.gameplay_hud_glyph_count = 0;
	assets.gameplay_hud_font_height = 0;
	assets.gameplay_scoreboard_glyph_count = 0;
	assets.gameplay_scoreboard_font_height = 0;
	assets.gameplay_message_glyph_count = 0;
	assets.gameplay_message_font_height = 0;
	assets.ready = false;
}

bool initialize_gameplay_powerball(
	FrontendShellAssets& assets,
	const assets::TextureImage& image)
{
	if (image.width != 256 || image.height != 256
		|| image.pixels.size != 256 * 256 * 4
		|| !frontend_movie_texture_init(
			assets.gameplay_powerball, 62, 62))
	{
		return false;
	}

	for (std::uint32_t index = 0; index < 256 * 256; ++index)
	{
		assets.gameplay_powerball_lookup[index] =
			static_cast<std::uint8_t>(
				image.pixels.data[index * 4] >> 3);
	}

	constexpr float kInv31 = std::bit_cast<float>(0x3d042108u);
	constexpr float kInvPi = std::bit_cast<float>(0x3ea2f983u);
	for (std::int32_t row = 0; row < 62; ++row)
	{
		const std::int32_t sphere_row = row - 31;
		assets.gameplay_powerball_half_width[row] =
			static_cast<std::int16_t>(std::trunc(std::sqrt(
				static_cast<float>(961 - sphere_row * sphere_row))));
		const float x = static_cast<float>(row - 31) * kInv31;
		for (std::int32_t column = 0; column < 62; ++column)
		{
			const float y =
				static_cast<float>(column - 31) * kInv31;
			float q = x * x + y * y;
			float u;
			float v;
			if (q > 1.0f)
			{
				u = y / q;
				v = x / q;
				q = 1.0f;
			}
			else
			{
				u = y;
				v = x;
			}
			const float denominator = std::sqrt(2.0f - q);
			const std::int32_t low = static_cast<std::int32_t>(
				std::trunc(
					std::asin(u / denominator)
					* kInvPi * 138.0f));
			const std::int32_t high = static_cast<std::int32_t>(
				std::trunc(
					std::asin(v / denominator)
					* kInvPi * -138.0f));
			assets.gameplay_powerball_coordinates[
				row * 62 + column] = static_cast<std::int16_t>(
					low - (high << 8) - 0x7f80);

			float py =
				static_cast<float>(row - 31) * kInv31 + 0.3f;
			float px =
				static_cast<float>(column - 31) * kInv31 + 0.3f;
			float light_q = px * px + py * py;
			if (light_q > 1.0f)
			{
				px /= light_q;
				py /= light_q;
				light_q = 1.0f;
			}
			const float pz = std::sqrt(1.0f - light_q);
			const float dx = -px;
			const float dy = -py;
			const float dz = 8.0f - pz;
			const float distance = static_cast<float>(
				std::sqrt(dx * dx + dy * dy + dz * dz));
			const float dot = dx * px + dy * py + dz * pz;
			const std::int32_t intensity =
				static_cast<std::int32_t>(
					std::trunc(64.0f * dot / distance));
			assets.gameplay_powerball_lighting[
				row * 62 + column] = static_cast<std::uint8_t>(
					std::clamp(intensity, 0, 63));
		}
	}

	constexpr float kInv63 = 1.0f / 63.0f;
	constexpr float kInv31Ramp = 1.0f / 31.0f;
	for (std::uint32_t row = 0; row < 32; ++row)
	{
		float t = static_cast<float>(row) * kInv63;
		t = t * t + 0.25f;
		float base = 0.0f;
		if (t > 1.0f)
		{
			base = (t - 1.0f) * 255.0f;
			t = 1.0f;
		}
		for (std::uint32_t column = 0; column < 32; ++column)
		{
			const float value =
				static_cast<float>(column) * kInv31Ramp * t;
			const auto channel = [](float input)
			{
				return static_cast<std::uint8_t>(std::clamp(
					static_cast<std::int32_t>(std::trunc(input)),
					0,
					255));
			};
			const std::uint8_t red = channel(value * 184.0f + base);
			const std::uint8_t green = channel(value * 67.0f + base);
			const std::uint8_t blue = channel(base);
			const std::uint8_t red5 =
				static_cast<std::uint8_t>(red >> 3);
			const std::uint8_t green6 =
				static_cast<std::uint8_t>(green >> 2);
			const std::uint8_t blue5 =
				static_cast<std::uint8_t>(blue >> 3);
			std::uint8_t* output =
				assets.gameplay_powerball_ramp
				+ (row * 32 + column) * 4;
			output[0] = static_cast<std::uint8_t>(
				(red5 << 3) | (red5 >> 2));
			output[1] = static_cast<std::uint8_t>(
				(green6 << 2) | (green6 >> 4));
			output[2] = static_cast<std::uint8_t>(
				(blue5 << 3) | (blue5 >> 2));
			output[3] = 255;
		}
	}
	return true;
}

void destroy_itac_assets(FrontendItacAssets& assets)
{
	auto destroy_textures = [](auto& textures)
	{
		for (FrontendTexture& texture : textures) destroy_texture(texture);
	};
	destroy_textures(assets.backgrounds);
	destroy_textures(assets.news);
	destroy_textures(assets.video);
	destroy_textures(assets.faction_icons);
	destroy_textures(assets.squads);
	destroy_textures(assets.persons);
	destroy_textures(assets.capitals);
	destroy_textures(assets.fighters);
	destroy_textures(assets.kills);
	FrontendSpriteAtlas* const atlases[] = {
		&assets.news_atlas,
		&assets.video_atlas,
		&assets.gfx_atlas,
		&assets.squad_atlas,
		&assets.person_atlas,
		&assets.capital_atlas,
		&assets.fighter_atlas,
		&assets.kills_atlas,
	};
	for (FrontendSpriteAtlas* atlas : atlases)
	{
		for (FrontendTexture& page : atlas->pages) destroy_texture(page);
		atlas->page_count = 0;
	}
	destroy_texture(assets.font_atlas);
	auto destroy_palettes = [](auto& palettes)
	{
		for (bgfx::TextureHandle& palette : palettes)
		{
			destroy_texture_handle(palette);
		}
	};
	destroy_palettes(assets.news_palettes);
	destroy_texture_handle(assets.video_palette);
	destroy_texture_handle(assets.video_thumbnail_palette);
	destroy_texture_handle(assets.faction_palette);
	destroy_palettes(assets.squad_palettes);
	destroy_palettes(assets.person_palettes);
	destroy_palettes(assets.capital_palettes);
	destroy_palettes(assets.fighter_palettes);
	destroy_palettes(assets.kills_palettes);
	assets.glyph_count = 0;
	assets.font_height = 0;
	assets.ready = false;
}

void destroy_credits_assets(FrontendCreditsAssets& assets)
{
	for (FrontendTexture& background : assets.backgrounds)
	{
		destroy_texture(background);
	}
	for (FrontendTexture& page : assets.sprite_atlas.pages)
	{
		destroy_texture(page);
	}
	assets.sprite_atlas.page_count = 0;
	destroy_texture(assets.font_atlas);
	for (bgfx::TextureHandle& palette : assets.background_palettes)
	{
		destroy_texture_handle(palette);
	}
	destroy_texture_handle(assets.orange_palette);
	destroy_texture_handle(assets.white_palette);
	assets.glyph_count = 0;
	assets.font_height = 0;
	assets.ready = false;
}

template <typename Handle>
void destroy_handle(Handle& handle)
{
	if (bgfx::isValid(handle))
	{
		bgfx::destroy(handle);
	}
	handle = BGFX_INVALID_HANDLE;
}

void destroy_renderer_core(FrontendRenderer& renderer)
{
	frame_geometry_shutdown(renderer.frame_geometry);
	destroy_texture(renderer.white);
	destroy_handle(renderer.rgba_program);
	destroy_handle(renderer.mission_rgba_program);
	destroy_handle(renderer.indexed_program);
	destroy_handle(renderer.model_rgba_program);
	destroy_handle(renderer.model_mode_six_program);
	destroy_handle(renderer.model_mode_seven_program);
	destroy_handle(renderer.model_mode_eight_program);
	destroy_handle(renderer.lit_model_rgba_program);
	destroy_handle(renderer.lit_model_mode_six_program);
	destroy_handle(renderer.lit_model_mode_seven_program);
	destroy_handle(renderer.lit_model_mode_eight_program);
	destroy_handle(renderer.texture_sampler);
	destroy_handle(renderer.material_texture_sampler);
	destroy_handle(renderer.palette_sampler);
	destroy_handle(renderer.uv_rect_uniform);
	destroy_handle(renderer.tint_uniform);
	destroy_handle(renderer.material_diffuse_uniform);
	destroy_handle(renderer.model_clip_plane_uniform);
	destroy_handle(renderer.lighting_environment_u_uniform);
	destroy_handle(renderer.lighting_environment_v_uniform);
	destroy_handle(renderer.lighting_base_uniform);
	destroy_handle(renderer.lighting_params_uniform);
	destroy_handle(renderer.lighting_position_radius_uniform);
	destroy_handle(renderer.lighting_direction_type_uniform);
	destroy_handle(renderer.lighting_color_intensity_uniform);
	destroy_handle(renderer.quad_vertices);
	destroy_handle(renderer.quad_indices);
	renderer.ready = false;
}

bool frontend_renderer_core_init(FrontendRenderer& renderer)
{
	if (renderer.ready)
	{
		return true;
	}
	renderer.layout
		.begin()
		.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
		.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
		.end();
	renderer.model_layout
		.begin()
		.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
		.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
		.add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float)
		.end();
	renderer.lit_model_layout
		.begin()
		.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Tangent, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::TexCoord2, 3, bgfx::AttribType::Float)
		.end();
	if (renderer.model_layout.getStride() != sizeof(float) * 8
		|| renderer.lit_model_layout.getStride() != sizeof(float) * 14
		|| !frame_geometry_init(
			renderer.frame_geometry,
			renderer.layout,
			renderer.model_layout))
	{
		destroy_renderer_core(renderer);
		return false;
	}
	const FrontendVertex vertices[] = {
		{0.0f, 0.0f, 0.0f, 0xffffffff, 0.0f, 0.0f},
		{1.0f, 0.0f, 0.0f, 0xffffffff, 1.0f, 0.0f},
		{1.0f, 1.0f, 0.0f, 0xffffffff, 1.0f, 1.0f},
		{0.0f, 1.0f, 0.0f, 0xffffffff, 0.0f, 1.0f},
	};
	const std::uint16_t indices[] = {0, 1, 2, 0, 2, 3};
	renderer.quad_vertices = bgfx::createVertexBuffer(
		bgfx::copy(vertices, sizeof(vertices)), renderer.layout);
	renderer.quad_indices = bgfx::createIndexBuffer(
		bgfx::copy(indices, sizeof(indices)));

	const bgfx::ShaderHandle vertex_rgba = create_shader(ShaderKind::vertex);
	const bgfx::ShaderHandle fragment_rgba = create_shader(ShaderKind::rgba);
	renderer.rgba_program = bgfx::createProgram(
		vertex_rgba, fragment_rgba, true);
	const bgfx::ShaderHandle mission_vertex_rgba =
		create_shader(ShaderKind::mission_vertex);
	const bgfx::ShaderHandle mission_fragment_rgba =
		create_shader(ShaderKind::rgba);
	renderer.mission_rgba_program = bgfx::createProgram(
		mission_vertex_rgba, mission_fragment_rgba, true);
	const bgfx::ShaderHandle vertex_indexed = create_shader(ShaderKind::vertex);
	const bgfx::ShaderHandle fragment_indexed = create_shader(ShaderKind::indexed);
	renderer.indexed_program = bgfx::createProgram(
		vertex_indexed, fragment_indexed, true);
	const bgfx::ShaderHandle vertex_model_rgba =
		create_shader(ShaderKind::model_vertex);
	const bgfx::ShaderHandle fragment_model_rgba =
		create_shader(ShaderKind::model_rgba);
	renderer.model_rgba_program = bgfx::createProgram(
		vertex_model_rgba, fragment_model_rgba, true);
	const bgfx::ShaderHandle vertex_mode_six =
		create_shader(ShaderKind::model_vertex);
	const bgfx::ShaderHandle fragment_mode_six =
		create_shader(ShaderKind::model_mode_six);
	renderer.model_mode_six_program = bgfx::createProgram(
		vertex_mode_six, fragment_mode_six, true);
	const bgfx::ShaderHandle vertex_mode_seven =
		create_shader(ShaderKind::model_vertex);
	const bgfx::ShaderHandle fragment_mode_seven =
		create_shader(ShaderKind::model_mode_seven);
	renderer.model_mode_seven_program = bgfx::createProgram(
		vertex_mode_seven, fragment_mode_seven, true);
	const bgfx::ShaderHandle vertex_mode_eight =
		create_shader(ShaderKind::model_vertex);
	const bgfx::ShaderHandle fragment_mode_eight =
		create_shader(ShaderKind::model_mode_eight);
	renderer.model_mode_eight_program = bgfx::createProgram(
		vertex_mode_eight, fragment_mode_eight, true);
	renderer.lit_model_rgba_program = bgfx::createProgram(
		create_shader(ShaderKind::model_lit_vertex),
		create_shader(ShaderKind::model_rgba),
		true);
	renderer.lit_model_mode_six_program = bgfx::createProgram(
		create_shader(ShaderKind::model_lit_vertex),
		create_shader(ShaderKind::model_mode_six),
		true);
	renderer.lit_model_mode_seven_program = bgfx::createProgram(
		create_shader(ShaderKind::model_lit_vertex),
		create_shader(ShaderKind::model_mode_seven),
		true);
	renderer.lit_model_mode_eight_program = bgfx::createProgram(
		create_shader(ShaderKind::model_lit_vertex),
		create_shader(ShaderKind::model_mode_eight),
		true);

	renderer.texture_sampler =
		bgfx::createUniform("s_texture", bgfx::UniformType::Sampler);
	renderer.material_texture_sampler =
		bgfx::createUniform(
			"s_materialTexture", bgfx::UniformType::Sampler);
	renderer.palette_sampler =
		bgfx::createUniform("s_palette", bgfx::UniformType::Sampler);
	renderer.uv_rect_uniform =
		bgfx::createUniform("u_uvRect", bgfx::UniformType::Vec4);
	renderer.tint_uniform =
		bgfx::createUniform("u_tint", bgfx::UniformType::Vec4);
	renderer.material_diffuse_uniform =
		bgfx::createUniform(
			"u_materialDiffuse", bgfx::UniformType::Vec4);
	renderer.model_clip_plane_uniform = bgfx::createUniform(
		"u_modelClipPlane", bgfx::UniformType::Vec4);
	renderer.lighting_environment_u_uniform = bgfx::createUniform(
		"u_lightingEnvironmentU", bgfx::UniformType::Vec4);
	renderer.lighting_environment_v_uniform = bgfx::createUniform(
		"u_lightingEnvironmentV", bgfx::UniformType::Vec4);
	renderer.lighting_base_uniform = bgfx::createUniform(
		"u_lightingBase", bgfx::UniformType::Vec4);
	renderer.lighting_params_uniform = bgfx::createUniform(
		"u_lightingParams", bgfx::UniformType::Vec4);
	renderer.lighting_position_radius_uniform = bgfx::createUniform(
		"u_lightingPositionRadius", bgfx::UniformType::Vec4, 32);
	renderer.lighting_direction_type_uniform = bgfx::createUniform(
		"u_lightingDirectionType", bgfx::UniformType::Vec4, 32);
	renderer.lighting_color_intensity_uniform = bgfx::createUniform(
		"u_lightingColorIntensity", bgfx::UniformType::Vec4, 32);

	const std::uint8_t white[] = {255, 255, 255, 255};
	renderer.white = create_texture(
		1, 1, bgfx::TextureFormat::RGBA8, white, sizeof(white));
	renderer.ready =
		bgfx::isValid(renderer.quad_vertices)
		&& bgfx::isValid(renderer.quad_indices)
		&& bgfx::isValid(renderer.rgba_program)
		&& bgfx::isValid(renderer.mission_rgba_program)
		&& bgfx::isValid(renderer.indexed_program)
		&& bgfx::isValid(renderer.model_rgba_program)
		&& bgfx::isValid(renderer.model_mode_six_program)
		&& bgfx::isValid(renderer.model_mode_seven_program)
		&& bgfx::isValid(renderer.model_mode_eight_program)
		&& bgfx::isValid(renderer.lit_model_rgba_program)
		&& bgfx::isValid(renderer.lit_model_mode_six_program)
		&& bgfx::isValid(renderer.lit_model_mode_seven_program)
		&& bgfx::isValid(renderer.lit_model_mode_eight_program)
		&& bgfx::isValid(renderer.texture_sampler)
		&& bgfx::isValid(renderer.material_texture_sampler)
		&& bgfx::isValid(renderer.palette_sampler)
		&& bgfx::isValid(renderer.uv_rect_uniform)
		&& bgfx::isValid(renderer.tint_uniform)
		&& bgfx::isValid(renderer.material_diffuse_uniform)
		&& bgfx::isValid(renderer.model_clip_plane_uniform)
		&& bgfx::isValid(renderer.lighting_environment_u_uniform)
		&& bgfx::isValid(renderer.lighting_environment_v_uniform)
		&& bgfx::isValid(renderer.lighting_base_uniform)
		&& bgfx::isValid(renderer.lighting_params_uniform)
		&& bgfx::isValid(renderer.lighting_position_radius_uniform)
		&& bgfx::isValid(renderer.lighting_direction_type_uniform)
		&& bgfx::isValid(renderer.lighting_color_intensity_uniform)
		&& bgfx::isValid(renderer.white.handle);
	if (!renderer.ready)
	{
		destroy_renderer_core(renderer);
	}
	return renderer.ready;
}

void canvas_viewport(
	std::uint32_t width,
	std::uint32_t height,
	float& scale,
	std::uint16_t& x,
	std::uint16_t& y,
	std::uint16_t& viewport_width,
	std::uint16_t& viewport_height)
{
	const float scale_x = static_cast<float>(width) / kFrontendWidth;
	const float scale_y = static_cast<float>(height) / kFrontendHeight;
	scale = std::min(scale_x, scale_y);
	viewport_width = static_cast<std::uint16_t>(
		std::max(1.0f, std::floor(kFrontendWidth * scale)));
	viewport_height = static_cast<std::uint16_t>(
		std::max(1.0f, std::floor(kFrontendHeight * scale)));
	x = static_cast<std::uint16_t>((width - viewport_width) / 2);
	y = static_cast<std::uint16_t>((height - viewport_height) / 2);
}
}

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
	const assets::Font& gameplay_message_font)
{
	if (splash.width > UINT16_MAX
		|| splash.height > UINT16_MAX
		|| mission_loading_splash.width > UINT16_MAX
		|| mission_loading_splash.height > UINT16_MAX
		|| !frontend_renderer_core_init(renderer))
	{
		return false;
	}
	FrontendShellAssets next;
	next.splash = create_texture(
		static_cast<std::uint16_t>(splash.width),
		static_cast<std::uint16_t>(splash.height),
		bgfx::TextureFormat::RGBA8,
		splash.pixels.data,
		static_cast<std::uint32_t>(splash.pixels.size));
	next.mission_loading_splash = create_texture(
		static_cast<std::uint16_t>(mission_loading_splash.width),
		static_cast<std::uint16_t>(mission_loading_splash.height),
		bgfx::TextureFormat::RGBA8,
		mission_loading_splash.pixels.data,
		static_cast<std::uint32_t>(mission_loading_splash.pixels.size));
	next.options_background = create_texture(
		static_cast<std::uint16_t>(options_background.width),
		static_cast<std::uint16_t>(options_background.height),
		bgfx::TextureFormat::RGBA8,
		options_background.pixels.data,
		static_cast<std::uint32_t>(options_background.pixels.size));
	next.options_detail_background = create_texture(
		static_cast<std::uint16_t>(options_detail_background.width),
		static_cast<std::uint16_t>(options_detail_background.height),
		bgfx::TextureFormat::RGBA8,
		options_detail_background.pixels.data,
		static_cast<std::uint32_t>(options_detail_background.pixels.size));
	next.in_game_options_background = create_texture(
		static_cast<std::uint16_t>(in_game_options_background.width),
		static_cast<std::uint16_t>(in_game_options_background.height),
		bgfx::TextureFormat::RGBA8,
		in_game_options_background.pixels.data,
		static_cast<std::uint32_t>(
			in_game_options_background.pixels.size));
	next.in_game_options_detail_background = create_texture(
		static_cast<std::uint16_t>(in_game_options_detail_background.width),
		static_cast<std::uint16_t>(in_game_options_detail_background.height),
		bgfx::TextureFormat::RGBA8,
		in_game_options_detail_background.pixels.data,
		static_cast<std::uint32_t>(
			in_game_options_detail_background.pixels.size));

	{
		SpriteAtlasBuilder atlas;
		for (std::uint32_t index = 0; index < 16; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					options_detail_sprites,
					index + 1,
					next.options_detail_cursor[index]))
			{
				destroy_shell_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(
				atlas, next.options_detail_sprite_atlas))
		{
			destroy_shell_assets(next);
			return false;
		}
	}
	{
		SpriteAtlasBuilder atlas;
		for (std::uint32_t index = 0; index < 16; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					control_options_sprites,
					index + 1,
					next.control_options_cursor[index]))
			{
				destroy_shell_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(
				atlas, next.control_options_sprite_atlas))
		{
			destroy_shell_assets(next);
			return false;
		}
	}
	{
		SpriteAtlasBuilder atlas;
		for (std::uint32_t index = 0; index < 16; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					sprites,
					index + kCursorFirst,
					next.cursor[index]))
			{
				destroy_shell_assets(next);
				return false;
			}
		}
		for (std::uint32_t index = 0; index < 3; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					sprites,
					index + kHighlightFirst,
					next.highlights[index]))
			{
				destroy_shell_assets(next);
				return false;
			}
		}
		if (!add_sprite_to_atlas(
				atlas, sprites, kIconNormal, next.bottom_icon)
			|| !add_sprite_to_atlas(
				atlas, sprites, kIconHover, next.bottom_icon_hover)
			|| !finish_sprite_atlas(
				atlas, next.frontend_sprite_atlas))
		{
			destroy_shell_assets(next);
			return false;
		}
	}
	if (!create_font_atlas(
			font,
			next.font_atlas,
			next.glyphs,
			next.glyph_count,
			next.font_height))
	{
		destroy_shell_assets(next);
		return false;
	}
	if (!create_font_atlas(
			pause_small_font,
			next.pause_small_font_atlas,
			next.pause_small_glyphs,
			next.pause_small_glyph_count,
			next.pause_small_font_height))
	{
		destroy_shell_assets(next);
		return false;
	}
	if (!create_font_atlas(
			gameplay_hud_font,
			next.gameplay_hud_font_atlas,
			next.gameplay_hud_glyphs,
			next.gameplay_hud_glyph_count,
			next.gameplay_hud_font_height))
	{
		destroy_shell_assets(next);
		return false;
	}
	if (!create_font_atlas(
			gameplay_scoreboard_font,
			next.gameplay_scoreboard_font_atlas,
			next.gameplay_scoreboard_glyphs,
			next.gameplay_scoreboard_glyph_count,
			next.gameplay_scoreboard_font_height))
	{
		destroy_shell_assets(next);
		return false;
	}
	if (!create_font_atlas(
			gameplay_message_font,
			next.gameplay_message_font_atlas,
			next.gameplay_message_glyphs,
			next.gameplay_message_glyph_count,
			next.gameplay_message_font_height))
	{
		destroy_shell_assets(next);
		return false;
	}
	{
		SpriteAtlasBuilder atlas;
		for (std::uint32_t index = 0; index < 3; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					options_sprites,
					index + kOptionsHighlightFirst,
					next.options_highlights[index]))
			{
				destroy_shell_assets(next);
				return false;
			}
		}
		if (!add_sprite_to_atlas(
				atlas,
				options_sprites,
				kOptionsIconNormal,
				next.options_bottom_icon)
			|| !add_sprite_to_atlas(
				atlas,
				options_sprites,
				kOptionsIconHover,
				next.options_bottom_icon_hover)
			|| !finish_sprite_atlas(
				atlas, next.options_sprite_atlas))
		{
			destroy_shell_assets(next);
			return false;
		}
	}
	{
		SpriteAtlasBuilder atlas;
		if (!add_sprite_to_atlas(
				atlas, gameplay_hud_sprites, 374, next.pause_button)
			|| !add_sprite_to_atlas(
				atlas,
				gameplay_hud_sprites,
				375,
				next.pause_button_hover))
		{
			destroy_shell_assets(next);
			return false;
		}
		for (std::uint32_t index = 0; index < 6; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					gameplay_hud_sprites,
					383 + index,
					next.pause_panels[index]))
			{
				destroy_shell_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.pause_sprite_atlas))
		{
			destroy_shell_assets(next);
			return false;
		}
	}
	{
		SpriteAtlasBuilder atlas;
		for (std::uint32_t shape = 21;
			shape < kGameplayHudShapeCount;
			++shape)
		{
			// Records 119 and 247 are the two embedded palettes. All other
			// records from 21 onward are retail VFX shapes.
			if (shape == 119 || shape == 247)
			{
				continue;
			}
			if (!add_sprite_to_atlas(
					atlas,
					gameplay_hud_sprites,
					shape,
					next.gameplay_hud_shapes[shape]))
			{
				destroy_shell_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(
				atlas, next.gameplay_hud_sprite_atlas))
		{
			destroy_shell_assets(next);
			return false;
		}
	}
	{
		// DMPowerup_draw (0x00486286) consumes records 0..9 and
		// Deathmatch_draw_scoreboard (0x004af650) consumes records 20..26
		// from DMICONS.SPR. Record 19 is the embedded palette; records 10..18
		// remain available to the scenario HUD callbacks.
		SpriteAtlasBuilder atlas;
		for (std::uint32_t shape = 0;
			shape < kGameplayScoreboardShapeCount;
			++shape)
		{
			if (shape == 19)
			{
				continue;
			}
			if (!add_sprite_to_atlas(
					atlas,
					gameplay_scoreboard_sprites,
					shape,
					next.gameplay_scoreboard_shapes[shape]))
			{
				destroy_shell_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(
				atlas, next.gameplay_scoreboard_sprite_atlas))
		{
			destroy_shell_assets(next);
			return false;
		}
	}
	if (!initialize_gameplay_powerball(next, gameplay_powerball))
	{
		destroy_shell_assets(next);
		return false;
	}
	if (!frontend_movie_texture_init(
			next.gameplay_hud_movie, 120, 100))
	{
		destroy_shell_assets(next);
		return false;
	}
	{
		SpriteAtlasBuilder atlas;
		if (!add_sprite_to_atlas(
				atlas, about_sprites, 35, next.about_panel)
			|| !add_sprite_to_atlas(
				atlas, about_sprites, 26, next.about_button)
			|| !add_sprite_to_atlas(
				atlas, about_sprites, 27, next.about_button_hover)
			|| !finish_sprite_atlas(atlas, next.about_sprite_atlas))
		{
			destroy_shell_assets(next);
			return false;
		}
	}
	{
		SpriteAtlasBuilder atlas;
		for (std::uint32_t index = 0; index < 16; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					in_game_options_sprites,
					index + 1,
					next.in_game_options_cursor[index]))
			{
				destroy_shell_assets(next);
				return false;
			}
		}
		for (std::uint32_t index = 0; index < 5; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					in_game_options_sprites,
					index + 18,
					next.in_game_options_highlights[index]))
			{
				destroy_shell_assets(next);
				return false;
			}
		}
		if (!add_sprite_to_atlas(
				atlas,
				in_game_options_sprites,
				28,
				next.in_game_options_bottom_icon)
			|| !add_sprite_to_atlas(
				atlas,
				in_game_options_sprites,
				29,
				next.in_game_options_bottom_icon_hover)
			|| !finish_sprite_atlas(
				atlas, next.in_game_options_sprite_atlas))
		{
			destroy_shell_assets(next);
			return false;
		}
	}
	{
		SpriteAtlasBuilder atlas;
		if (!add_sprite_to_atlas(
				atlas, quit_sprites, 6, next.quit_background)
			|| !add_sprite_to_atlas(
				atlas, quit_sprites, 3, next.quit_button)
			|| !add_sprite_to_atlas(
				atlas, quit_sprites, 4, next.quit_button_hover)
			|| !finish_sprite_atlas(atlas, next.quit_sprite_atlas))
		{
			destroy_shell_assets(next);
			return false;
		}
	}

	std::uint8_t palette[256 * 4];
	if (!assets::decode_sprite_palette(sprites, kCursorPalette, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.cursor_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(sprites, kHighlightPalette, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.highlight_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(sprites, kIconPalette, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.icon_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			options_sprites, kOptionsHighlightPalette, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.options_highlight_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			options_sprites, kOptionsIconPalette, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.options_icon_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			options_detail_sprites, 0, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.options_detail_cursor_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			control_options_sprites, 0, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.control_options_cursor_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			in_game_options_sprites, 0, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.in_game_options_cursor_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			in_game_options_sprites, 17, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.in_game_options_highlight_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			in_game_options_sprites, 23, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.in_game_options_icon_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(gameplay_hud_sprites, 119, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.pause_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(gameplay_hud_sprites, 119, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.gameplay_hud_palette = create_palette(palette);
	for (std::uint32_t index = 0; index < 256; ++index)
	{
		next.gameplay_hud_rgba[index] =
			(static_cast<std::uint32_t>(palette[index * 4 + 0]) << 24)
			| (static_cast<std::uint32_t>(palette[index * 4 + 1]) << 16)
			| (static_cast<std::uint32_t>(palette[index * 4 + 2]) << 8)
			| palette[index * 4 + 3];
	}
	// LANCER.EXE 0x004a2af0 builds the three exact 256-byte lookaside tables used
	// by HUD_render_targeting_overlay. Every entry remains identity except
	// source zero maps to 0xff and the font's 0xf7 color maps by object
	// allegiance class to 0xd7, 0x4c, or 0xf7.
	constexpr std::uint8_t target_color[3] = {0xd7, 0x4c, 0xf7};
	std::uint8_t target_palette[256 * 4];
	for (std::uint8_t allegiance = 0; allegiance < 3; ++allegiance)
	{
		std::memcpy(target_palette, palette, sizeof(target_palette));
		std::memcpy(target_palette, palette + 0xff * 4, 4);
		std::memcpy(
			target_palette + 0xf7 * 4,
			palette + target_color[allegiance] * 4,
			4);
		next.gameplay_hud_target_palette[allegiance] =
			create_palette(target_palette);
	}
	if (!assets::decode_sprite_palette(
			gameplay_scoreboard_sprites, 19, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.gameplay_scoreboard_palette = create_palette(palette);
	for (std::uint32_t index = 0; index < 256; ++index)
	{
		next.gameplay_scoreboard_rgba[index] =
			(static_cast<std::uint32_t>(palette[index * 4 + 0]) << 24)
			| (static_cast<std::uint32_t>(palette[index * 4 + 1]) << 16)
			| (static_cast<std::uint32_t>(palette[index * 4 + 2]) << 8)
			| palette[index * 4 + 3];
	}
	if (!assets::decode_sprite_palette(about_sprites, 33, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.about_panel_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(about_sprites, 21, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.about_button_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(quit_sprites, 5, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.quit_background_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(quit_sprites, 0, palette))
	{
		destroy_shell_assets(next);
		return false;
	}
	next.quit_button_palette = create_palette(palette);
	make_font_palette(palette, 72, 38, 4, 255, 215, 72);
	next.font_gold_palette = create_palette(palette);
	make_font_palette(palette, 18, 52, 72, 180, 240, 255);
	next.font_blue_palette = create_palette(palette);
	make_font_palette(palette, 72, 72, 72, 255, 255, 255);
	next.font_white_palette = create_palette(palette);

	next.ready =
		bgfx::isValid(next.splash.handle)
		&& bgfx::isValid(next.mission_loading_splash.handle)
		&& bgfx::isValid(next.options_background.handle)
		&& bgfx::isValid(next.options_detail_background.handle)
		&& bgfx::isValid(next.in_game_options_background.handle)
		&& bgfx::isValid(next.in_game_options_detail_background.handle)
		&& bgfx::isValid(next.cursor[0].handle)
		&& bgfx::isValid(next.font_atlas.handle)
		&& bgfx::isValid(next.cursor_palette)
		&& bgfx::isValid(next.highlight_palette)
		&& bgfx::isValid(next.icon_palette)
		&& bgfx::isValid(next.options_highlight_palette)
		&& bgfx::isValid(next.options_icon_palette)
		&& bgfx::isValid(next.options_detail_cursor_palette)
		&& bgfx::isValid(next.control_options_cursor_palette)
		&& bgfx::isValid(next.in_game_options_cursor_palette)
		&& bgfx::isValid(next.in_game_options_highlight_palette)
		&& bgfx::isValid(next.in_game_options_icon_palette)
		&& bgfx::isValid(next.pause_button.handle)
		&& bgfx::isValid(next.pause_small_font_atlas.handle)
		&& bgfx::isValid(next.gameplay_hud_font_atlas.handle)
		&& bgfx::isValid(
			next.gameplay_scoreboard_shapes[20].handle)
		&& bgfx::isValid(next.gameplay_scoreboard_font_atlas.handle)
		&& bgfx::isValid(next.gameplay_message_font_atlas.handle)
		&& bgfx::isValid(next.pause_palette)
		&& bgfx::isValid(next.gameplay_hud_palette)
		&& bgfx::isValid(next.gameplay_scoreboard_palette)
		&& bgfx::isValid(next.gameplay_hud_target_palette[0])
		&& bgfx::isValid(next.gameplay_hud_target_palette[1])
		&& bgfx::isValid(next.gameplay_hud_target_palette[2])
		&& bgfx::isValid(next.about_panel_palette)
		&& bgfx::isValid(next.about_button_palette)
		&& bgfx::isValid(next.quit_background_palette)
		&& bgfx::isValid(next.quit_button_palette)
		&& bgfx::isValid(next.font_gold_palette)
		&& bgfx::isValid(next.font_blue_palette)
		&& bgfx::isValid(next.font_white_palette);
	if (!next.ready)
	{
		destroy_shell_assets(next);
		return false;
	}
	destroy_shell_assets(renderer.shell);
	renderer.shell = static_cast<FrontendShellAssets&&>(next);
	bgfx::TextureHandle* const palettes[] = {
		&next.cursor_palette,
		&next.highlight_palette,
		&next.icon_palette,
		&next.options_highlight_palette,
		&next.options_icon_palette,
		&next.options_detail_cursor_palette,
		&next.control_options_cursor_palette,
		&next.in_game_options_cursor_palette,
		&next.in_game_options_highlight_palette,
		&next.in_game_options_icon_palette,
		&next.pause_palette,
		&next.gameplay_scoreboard_palette,
		&next.about_panel_palette,
		&next.about_button_palette,
		&next.quit_background_palette,
		&next.quit_button_palette,
		&next.font_gold_palette,
		&next.font_blue_palette,
		&next.font_white_palette,
	};
	for (bgfx::TextureHandle* palette_handle : palettes)
	{
		*palette_handle = BGFX_INVALID_HANDLE;
	}
	next.ready = false;
	return true;
}

bool frontend_campaign_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage& campaign_background,
	const assets::TextureImage& save_load_background,
	const assets::SpriteList& campaign_sprites)
{
	if (!renderer.ready
		|| campaign_background.width != kFrontendWidth
		|| campaign_background.height != kFrontendHeight
		|| save_load_background.width != kFrontendWidth
		|| save_load_background.height != kFrontendHeight)
	{
		return false;
	}

	FrontendCampaignAssets next;
	next.background = create_texture(
		kFrontendWidth,
		kFrontendHeight,
		bgfx::TextureFormat::RGBA8,
		campaign_background.pixels.data,
		static_cast<std::uint32_t>(campaign_background.pixels.size));
	next.save_load_background = create_texture(
		kFrontendWidth,
		kFrontendHeight,
		bgfx::TextureFormat::RGBA8,
		save_load_background.pixels.data,
		static_cast<std::uint32_t>(save_load_background.pixels.size));
	{
		SpriteAtlasBuilder atlas;
		for (std::uint32_t index = 0; index < 16; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					campaign_sprites,
					kCampaignCursorFirst + index,
					next.cursor[index]))
			{
				destroy_campaign_assets(next);
				return false;
			}
		}
		for (std::uint32_t index = 0; index < 2; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					campaign_sprites,
					kCampaignPilotFirst + index,
					next.pilots[index])
				|| !add_sprite_to_atlas(
					atlas,
					campaign_sprites,
					kDifficultyFirst + index,
					next.difficulty_panels[index]))
			{
				destroy_campaign_assets(next);
				return false;
			}
		}
		for (std::uint32_t index = 0; index < 11; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					campaign_sprites,
					kCampaignControlFirst + index,
					next.controls[index]))
			{
				destroy_campaign_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.sprite_atlas))
		{
			destroy_campaign_assets(next);
			return false;
		}
	}

	std::uint8_t palette[256 * 4];
	if (!assets::decode_sprite_palette(
			campaign_sprites, kCampaignCursorPalette, palette))
	{
		destroy_campaign_assets(next);
		return false;
	}
	next.cursor_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			campaign_sprites, kCampaignPilotPalette, palette))
	{
		destroy_campaign_assets(next);
		return false;
	}
	next.pilot_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			campaign_sprites, kCampaignControlPalette, palette))
	{
		destroy_campaign_assets(next);
		return false;
	}
	next.control_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			campaign_sprites, kDifficultyPalette, palette))
	{
		destroy_campaign_assets(next);
		return false;
	}
	next.difficulty_palette = create_palette(palette);

	next.ready =
		bgfx::isValid(next.background.handle)
		&& bgfx::isValid(next.save_load_background.handle)
		&& bgfx::isValid(next.cursor_palette)
		&& bgfx::isValid(next.pilot_palette)
		&& bgfx::isValid(next.control_palette)
		&& bgfx::isValid(next.difficulty_palette);
	if (!next.ready)
	{
		destroy_campaign_assets(next);
		return false;
	}
	destroy_campaign_assets(renderer.campaign);
	renderer.campaign = static_cast<FrontendCampaignAssets&&>(next);
	next.cursor_palette = BGFX_INVALID_HANDLE;
	next.pilot_palette = BGFX_INVALID_HANDLE;
	next.control_palette = BGFX_INVALID_HANDLE;
	next.difficulty_palette = BGFX_INVALID_HANDLE;
	next.ready = false;
	return true;
}

bool frontend_multiplayer_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage& background,
	const assets::TextureImage& lobby_background,
	const assets::SpriteList& sprites)
{
	if (!renderer.ready
		|| background.width != kFrontendWidth
		|| background.height != kFrontendHeight
		|| lobby_background.width != kFrontendWidth
		|| lobby_background.height != kFrontendHeight)
	{
		return false;
	}

	FrontendMultiplayerAssets next;
	next.background = create_texture(
		kFrontendWidth,
		kFrontendHeight,
		bgfx::TextureFormat::RGBA8,
		background.pixels.data,
		static_cast<std::uint32_t>(background.pixels.size));
	next.lobby_background = create_texture(
		kFrontendWidth,
		kFrontendHeight,
		bgfx::TextureFormat::RGBA8,
		lobby_background.pixels.data,
		static_cast<std::uint32_t>(lobby_background.pixels.size));
	{
		SpriteAtlasBuilder atlas;
		for (std::uint32_t index = 0; index < 16; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					sprites,
					kMultiplayerCursorFirst + index,
					next.cursor[index]))
			{
				destroy_multiplayer_assets(next);
				return false;
			}
		}
		if (!add_sprite_to_atlas(
				atlas,
				sprites,
				kMultiplayerActionButton,
				next.action_button)
			|| !add_sprite_to_atlas(
				atlas,
				sprites,
				kMultiplayerActionButtonHover,
				next.action_button_hover)
			|| !add_sprite_to_atlas(
				atlas,
				sprites,
				kMultiplayerProviderButton,
				next.provider_button)
			|| !add_sprite_to_atlas(
				atlas,
				sprites,
				kMultiplayerProviderButtonSelected,
				next.provider_button_selected)
			|| !add_sprite_to_atlas(
				atlas,
				sprites,
				kMultiplayerLobbyScroll,
				next.lobby_scroll)
			|| !add_sprite_to_atlas(
				atlas,
				sprites,
				kMultiplayerLobbyScrollHover,
				next.lobby_scroll_hover)
			|| !add_sprite_to_atlas(
				atlas,
				sprites,
				kMultiplayerLobbyCheckbox,
				next.lobby_checkbox)
			|| !add_sprite_to_atlas(
				atlas,
				sprites,
				kMultiplayerLobbyCheckmark,
				next.lobby_checkmark)
			|| !finish_sprite_atlas(atlas, next.sprite_atlas))
		{
			destroy_multiplayer_assets(next);
			return false;
		}
	}

	std::uint8_t palette[256 * 4];
	if (!assets::decode_sprite_palette(
			sprites, kMultiplayerCursorPalette, palette))
	{
		destroy_multiplayer_assets(next);
		return false;
	}
	next.cursor_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			sprites, kMultiplayerScreenPalette, palette))
	{
		destroy_multiplayer_assets(next);
		return false;
	}
	next.screen_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			sprites, kMultiplayerLobbyCheckboxPalette, palette))
	{
		destroy_multiplayer_assets(next);
		return false;
	}
	next.lobby_checkbox_palette = create_palette(palette);
	next.ready =
		bgfx::isValid(next.background.handle)
		&& bgfx::isValid(next.lobby_background.handle)
		&& bgfx::isValid(next.cursor[0].handle)
		&& bgfx::isValid(next.action_button.handle)
		&& bgfx::isValid(next.action_button_hover.handle)
		&& bgfx::isValid(next.provider_button.handle)
		&& bgfx::isValid(next.provider_button_selected.handle)
		&& bgfx::isValid(next.lobby_scroll.handle)
		&& bgfx::isValid(next.lobby_scroll_hover.handle)
		&& bgfx::isValid(next.lobby_checkbox.handle)
		&& bgfx::isValid(next.lobby_checkmark.handle)
		&& bgfx::isValid(next.cursor_palette)
		&& bgfx::isValid(next.screen_palette)
		&& bgfx::isValid(next.lobby_checkbox_palette);
	if (!next.ready)
	{
		destroy_multiplayer_assets(next);
		return false;
	}
	destroy_multiplayer_assets(renderer.multiplayer);
	renderer.multiplayer =
		static_cast<FrontendMultiplayerAssets&&>(next);
	next.cursor_palette = BGFX_INVALID_HANDLE;
	next.screen_palette = BGFX_INVALID_HANDLE;
	next.lobby_checkbox_palette = BGFX_INVALID_HANDLE;
	next.ready = false;
	return true;
}

bool frontend_vr_assets_init(
	FrontendRenderer& renderer,
	const assets::SpriteList& vr_sprites)
{
	if (!renderer.ready)
	{
		return false;
	}
	FrontendVrAssets next;
	{
		SpriteAtlasBuilder atlas;
		for (std::uint32_t index = 0; index < 50; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas, vr_sprites, index + 1, next.cursor[index]))
			{
				destroy_vr_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.sprite_atlas))
		{
			destroy_vr_assets(next);
			return false;
		}
	}
	std::uint8_t palette[256 * 4];
	if (!assets::decode_sprite_palette(vr_sprites, 0, palette))
	{
		destroy_vr_assets(next);
		return false;
	}
	next.cursor_palette = create_palette(palette);
	next.ready = bgfx::isValid(next.cursor_palette);
	if (!next.ready)
	{
		destroy_vr_assets(next);
		return false;
	}
	destroy_vr_assets(renderer.vr);
	renderer.vr = static_cast<FrontendVrAssets&&>(next);
	next.cursor_palette = BGFX_INVALID_HANDLE;
	next.ready = false;
	return true;
}

bool frontend_vr_ambient_assets_init(
	FrontendRenderer& renderer,
	std::uint32_t slot,
	const assets::SpriteList* sprites,
	std::uint32_t shape_count)
{
	if (!renderer.ready || slot >= 2)
	{
		return false;
	}
	if (sprites == nullptr || shape_count == 0)
	{
		destroy_vr_ambient_assets(renderer.vr_ambient[slot]);
		return true;
	}
	if (shape_count > kVrAmbientShapeCount)
	{
		return false;
	}

	FrontendVrAmbientAssets next;
	SpriteAtlasBuilder atlas;
	for (std::uint32_t index = 0; index < shape_count; ++index)
	{
		if (!add_sprite_to_atlas(
				atlas,
				*sprites,
				index + 1,
				next.shapes[index],
				true))
		{
			destroy_vr_ambient_assets(next);
			return false;
		}
	}
	if (!finish_sprite_atlas(atlas, next.sprite_atlas))
	{
		destroy_vr_ambient_assets(next);
		return false;
	}
	std::uint8_t palette[256 * 4];
	if (!assets::decode_sprite_palette(*sprites, 0, palette))
	{
		destroy_vr_ambient_assets(next);
		return false;
	}
	next.palette = create_palette(palette);
	next.shape_count = shape_count;
	next.ready = bgfx::isValid(next.palette);
	if (!next.ready)
	{
		destroy_vr_ambient_assets(next);
		return false;
	}

	destroy_vr_ambient_assets(renderer.vr_ambient[slot]);
	renderer.vr_ambient[slot] =
		static_cast<FrontendVrAmbientAssets&&>(next);
	next.palette = BGFX_INVALID_HANDLE;
	next.ready = false;
	return true;
}

bool frontend_briefing_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage (&doors)[2],
	const assets::SpriteList& early_sprites,
	const assets::SpriteList& late_sprites,
	const assets::SpriteList& early_exit_sprites,
	const assets::SpriteList& late_exit_sprites)
{
	if (!renderer.ready
		|| doors[0].width != kFrontendWidth
		|| doors[0].height != kFrontendHeight
		|| doors[1].width != kFrontendWidth
		|| doors[1].height != kFrontendHeight
		|| early_sprites.shape_count != kEarlyBriefingShapeCount
		|| late_sprites.shape_count != kLateBriefingShapeCount
		|| early_exit_sprites.shape_count != kBriefingExitShapeCount
		|| late_exit_sprites.shape_count != kBriefingExitShapeCount)
	{
		return false;
	}
	FrontendBriefingAssets next;
	for (std::uint32_t index = 0; index < 2; ++index)
	{
		next.doors[index] = create_texture(
			kFrontendWidth,
			kFrontendHeight,
			bgfx::TextureFormat::RGBA8,
			doors[index].pixels.data,
			static_cast<std::uint32_t>(doors[index].pixels.size));
	}
	{
		SpriteAtlasBuilder atlas;
		if (!add_sprite_to_atlas(
				atlas, early_sprites, 0, next.early[0])
			|| !add_sprite_range_to_atlas(
				atlas,
				early_sprites,
				2,
				kEarlyBriefingShapeCount,
				next.early)
			|| !finish_sprite_atlas(
				atlas, next.early_atlas))
		{
			destroy_briefing_assets(next);
			return false;
		}
	}
	{
		SpriteAtlasBuilder atlas;
		if (!add_sprite_to_atlas(
				atlas, late_sprites, 0, next.late[0])
			|| !add_sprite_range_to_atlas(
				atlas,
				late_sprites,
				2,
				kLateBriefingShapeCount,
				next.late)
			|| !finish_sprite_atlas(
				atlas, next.late_atlas))
		{
			destroy_briefing_assets(next);
			return false;
		}
	}
	{
		SpriteAtlasBuilder atlas;
		if (!add_sprite_to_atlas(
				atlas,
				early_exit_sprites,
				0,
				next.early_exit[0])
			|| !add_sprite_range_to_atlas(
				atlas,
				early_exit_sprites,
				2,
				kBriefingExitShapeCount,
				next.early_exit)
			|| !finish_sprite_atlas(
				atlas, next.early_exit_atlas))
		{
			destroy_briefing_assets(next);
			return false;
		}
	}
	{
		SpriteAtlasBuilder atlas;
		if (!add_sprite_to_atlas(
				atlas,
				late_exit_sprites,
				0,
				next.late_exit[0])
			|| !add_sprite_range_to_atlas(
				atlas,
				late_exit_sprites,
				2,
				kBriefingExitShapeCount,
				next.late_exit)
			|| !finish_sprite_atlas(
				atlas, next.late_exit_atlas))
		{
			destroy_briefing_assets(next);
			return false;
		}
	}
	std::uint8_t palette[256 * 4];
	if (!assets::decode_sprite_palette(early_sprites, 1, palette))
	{
		destroy_briefing_assets(next);
		return false;
	}
	next.palettes[0] = create_palette(palette);
	if (!assets::decode_sprite_palette(late_sprites, 1, palette))
	{
		destroy_briefing_assets(next);
		return false;
	}
	next.palettes[1] = create_palette(palette);
	next.ready =
		bgfx::isValid(next.doors[0].handle)
		&& bgfx::isValid(next.doors[1].handle)
		&& bgfx::isValid(next.palettes[0])
		&& bgfx::isValid(next.palettes[1]);
	if (!next.ready)
	{
		destroy_briefing_assets(next);
		return false;
	}
	destroy_briefing_assets(renderer.briefing);
	renderer.briefing = static_cast<FrontendBriefingAssets&&>(next);
	for (bgfx::TextureHandle& palette : next.palettes)
	{
		palette = BGFX_INVALID_HANDLE;
	}
	next.ready = false;
	return true;
}

bool frontend_loadout_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage (&backgrounds)[2],
	const assets::TextureImage& panels,
	const assets::TextureImage (&disc)[4],
	const assets::TextureImage& glow,
	const assets::TextureImage& hardpoints,
	const assets::SpriteList& sprites,
	const assets::ShipModel (&ships)[12],
	const assets::ShipModel (&guns)[12],
	const assets::TextureImage (&ship_textures)[12],
	const assets::ShipModel (&missiles)[10],
	const assets::TextureImage& missile_texture,
	const assets::Font& title_font,
	const assets::Font& info_font,
	const std::uint8_t (&palette)[256 * 4])
{
	if (!renderer.ready
		|| backgrounds[0].width != kFrontendWidth
		|| backgrounds[0].height != kFrontendHeight
		|| backgrounds[1].width != kFrontendWidth
		|| backgrounds[1].height != kFrontendHeight
		|| panels.width != 256
		|| panels.height != 256
		|| glow.width == 0
		|| glow.height == 0
		|| sprites.shape_count != kLoadoutShapeCount)
	{
		return false;
	}
	FrontendLoadoutAssets next;
	LoadoutRenderer next_renderer;
	if (!loadout_renderer_build(
			renderer,
			next_renderer,
			panels,
			disc,
			glow,
			hardpoints,
			ships,
			guns,
			ship_textures,
			missiles,
			missile_texture))
	{
		return false;
	}
	for (std::uint32_t index = 0; index < 2; ++index)
	{
		next.backgrounds[index] = create_texture(
			kFrontendWidth,
			kFrontendHeight,
			bgfx::TextureFormat::RGBA8,
			backgrounds[index].pixels.data,
			static_cast<std::uint32_t>(backgrounds[index].pixels.size));
	}
	{
		SpriteAtlasBuilder atlas;
		if (!add_sprite_range_to_atlas(
				atlas,
				sprites,
				0,
				kLoadoutShapeCount,
				next.shapes)
			|| !finish_sprite_atlas(atlas, next.sprite_atlas))
		{
			destroy_loadout_assets(next);
			loadout_renderer_shutdown(next_renderer);
			return false;
		}
	}
	next.palette = create_palette(palette);
	constexpr std::uint8_t info_translation[16] = {
		0xff, 0x00, 0x05, 0x0d, 0x15, 0x14, 0x2e, 0x2f,
		0x32, 0x35, 0x43, 0x9c, 0xa6, 0xaf, 0xb8, 0xc9,
	};
	std::uint8_t title_palette[256 * 4]{};
	std::uint8_t info_palette[256 * 4]{};
	for (std::uint32_t index = 0; index < 16; ++index)
	{
		std::memcpy(
			info_palette + index * 4,
			palette + info_translation[index] * 4,
			4);
		if (index != 0)
		{
			std::memcpy(
				title_palette + index * 4,
				palette + 0x05 * 4,
				4);
		}
	}
	title_palette[3] = 0;
	info_palette[3] = 0;
	next.title_font_palette = create_palette(title_palette);
	next.info_font_palette = create_palette(info_palette);
	if (!create_font_atlas(
			title_font,
			next.title_font,
			next.title_glyphs,
			next.title_glyph_count,
			next.title_font_height)
		|| !create_font_atlas(
			info_font,
			next.info_font,
			next.info_glyphs,
			next.info_glyph_count,
			next.info_font_height))
	{
		destroy_loadout_assets(next);
		loadout_renderer_shutdown(next_renderer);
		return false;
	}
	next.ready =
		bgfx::isValid(next.backgrounds[0].handle)
		&& bgfx::isValid(next.backgrounds[1].handle)
		&& bgfx::isValid(next.title_font.handle)
		&& bgfx::isValid(next.info_font.handle)
		&& bgfx::isValid(next.title_font_palette)
		&& bgfx::isValid(next.info_font_palette)
		&& bgfx::isValid(next.palette);
	if (!next.ready)
	{
		destroy_loadout_assets(next);
		loadout_renderer_shutdown(next_renderer);
		return false;
	}
	destroy_loadout_assets(renderer.loadout);
	renderer.loadout = static_cast<FrontendLoadoutAssets&&>(next);
	loadout_renderer_commit(renderer.loadout_renderer, next_renderer);
	next.palette = BGFX_INVALID_HANDLE;
	next.title_font_palette = BGFX_INVALID_HANDLE;
	next.info_font_palette = BGFX_INVALID_HANDLE;
	next.ready = false;
	return true;
}

bool frontend_debrief_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage& background,
	const assets::SpriteList& sprites)
{
	if (!renderer.ready
		|| background.width != kFrontendWidth
		|| background.height != kFrontendHeight
		|| sprites.shape_count != kDebriefShapeCount)
	{
		return false;
	}
	FrontendDebriefAssets next;
	next.background = create_texture(
		kFrontendWidth,
		kFrontendHeight,
		bgfx::TextureFormat::RGBA8,
		background.pixels.data,
		static_cast<std::uint32_t>(background.pixels.size));
	{
		SpriteAtlasBuilder atlas;
		for (std::uint32_t index = 0; index < 21; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					sprites,
					index + kDebriefCursorFirst,
					next.cursor[index]))
			{
				destroy_debrief_assets(next);
				return false;
			}
		}
		for (std::uint32_t index = 0; index < 2; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					sprites,
					index + kDebriefScrollArrowFirst,
					next.scroll_arrow[index])
				|| !add_sprite_to_atlas(
					atlas,
					sprites,
					index + kDebriefActionButtonFirst,
					next.action_button[index]))
			{
				destroy_debrief_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.sprite_atlas))
		{
			destroy_debrief_assets(next);
			return false;
		}
	}
	std::uint8_t palette[256 * 4];
	if (!assets::decode_sprite_palette(
			sprites, kDebriefCursorPalette, palette))
	{
		destroy_debrief_assets(next);
		return false;
	}
	next.cursor_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(
			sprites, kDebriefControlPalette, palette))
	{
		destroy_debrief_assets(next);
		return false;
	}
	next.control_palette = create_palette(palette);
	next.ready =
		bgfx::isValid(next.background.handle)
		&& bgfx::isValid(next.cursor[0].handle)
		&& bgfx::isValid(next.scroll_arrow[0].handle)
		&& bgfx::isValid(next.scroll_arrow[1].handle)
		&& bgfx::isValid(next.action_button[0].handle)
		&& bgfx::isValid(next.action_button[1].handle)
		&& bgfx::isValid(next.cursor_palette)
		&& bgfx::isValid(next.control_palette);
	if (!next.ready)
	{
		destroy_debrief_assets(next);
		return false;
	}
	destroy_debrief_assets(renderer.debrief);
	renderer.debrief = static_cast<FrontendDebriefAssets&&>(next);
	next.cursor_palette = BGFX_INVALID_HANDLE;
	next.control_palette = BGFX_INVALID_HANDLE;
	next.ready = false;
	return true;
}

bool frontend_restart_assets_init(
	FrontendRenderer& renderer,
	const assets::SpriteList& sprites)
{
	if (!renderer.ready || sprites.shape_count != kRestartShapeCount)
	{
		return false;
	}
	FrontendRestartAssets next;
	{
		SpriteAtlasBuilder atlas;
		for (std::uint32_t index = 0; index < 16; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas,
					sprites,
					index + 1,
					next.cursor[index]))
			{
				destroy_restart_assets(next);
				return false;
			}
		}
		if (!add_sprite_to_atlas(
				atlas, sprites, 18, next.background)
			|| !add_sprite_to_atlas(
				atlas, sprites, 19, next.highlight)
			|| !finish_sprite_atlas(atlas, next.sprite_atlas))
		{
			destroy_restart_assets(next);
			return false;
		}
	}
	std::uint8_t palette[256 * 4];
	if (!assets::decode_sprite_palette(sprites, 0, palette))
	{
		destroy_restart_assets(next);
		return false;
	}
	next.cursor_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(sprites, 17, palette))
	{
		destroy_restart_assets(next);
		return false;
	}
	next.screen_palette = create_palette(palette);
	next.ready =
		bgfx::isValid(next.background.handle)
		&& bgfx::isValid(next.highlight.handle)
		&& bgfx::isValid(next.cursor[0].handle)
		&& bgfx::isValid(next.cursor_palette)
		&& bgfx::isValid(next.screen_palette);
	if (!next.ready)
	{
		destroy_restart_assets(next);
		return false;
	}
	destroy_restart_assets(renderer.restart);
	renderer.restart = static_cast<FrontendRestartAssets&&>(next);
	next.cursor_palette = BGFX_INVALID_HANDLE;
	next.screen_palette = BGFX_INVALID_HANDLE;
	next.ready = false;
	return true;
}

bool frontend_sim_pod_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage (&backgrounds)[2],
	const assets::SpriteList& sprites)
{
	if (!renderer.ready
		|| backgrounds[0].width != kFrontendWidth
		|| backgrounds[0].height != kFrontendHeight
		|| backgrounds[1].width != kFrontendWidth
		|| backgrounds[1].height != kFrontendHeight)
	{
		return false;
	}
	FrontendSimPodAssets next;
	for (std::uint8_t index = 0; index < 2; ++index)
	{
		next.backgrounds[index] = create_texture(
			kFrontendWidth,
			kFrontendHeight,
			bgfx::TextureFormat::RGBA8,
			backgrounds[index].pixels.data,
			static_cast<std::uint32_t>(backgrounds[index].pixels.size));
	}
	{
		SpriteAtlasBuilder atlas;
		for (std::uint8_t shape = 1; shape <= 8; ++shape)
		{
			if (!add_sprite_to_atlas(
					atlas,
					sprites,
					shape,
					next.shapes[shape - 1]))
			{
				destroy_sim_pod_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.sprite_atlas))
		{
			destroy_sim_pod_assets(next);
			return false;
		}
	}
	std::uint8_t palette[256 * 4];
	if (!assets::decode_sprite_palette(sprites, 0, palette))
	{
		destroy_sim_pod_assets(next);
		return false;
	}
	next.palette = create_palette(palette);
	next.ready =
		bgfx::isValid(next.backgrounds[0].handle)
		&& bgfx::isValid(next.backgrounds[1].handle)
		&& bgfx::isValid(next.palette);
	if (!next.ready)
	{
		destroy_sim_pod_assets(next);
		return false;
	}
	destroy_sim_pod_assets(renderer.sim_pod);
	renderer.sim_pod = static_cast<FrontendSimPodAssets&&>(next);
	next.palette = BGFX_INVALID_HANDLE;
	next.ready = false;
	return true;
}

bool frontend_cd_assets_init(
	FrontendRenderer& renderer,
	const assets::TextureImage& early_background,
	const assets::TextureImage& late_background,
	const assets::SpriteList& sprites,
	const assets::Font& font)
{
	if (!renderer.ready
		|| early_background.width != kFrontendWidth
		|| early_background.height != kFrontendHeight
		|| late_background.width != kFrontendWidth
		|| late_background.height != kFrontendHeight)
	{
		return false;
	}
	FrontendCdAssets next;
	next.early_background = create_texture(
		kFrontendWidth,
		kFrontendHeight,
		bgfx::TextureFormat::RGBA8,
		early_background.pixels.data,
		static_cast<std::uint32_t>(early_background.pixels.size));
	next.late_background = create_texture(
		kFrontendWidth,
		kFrontendHeight,
		bgfx::TextureFormat::RGBA8,
		late_background.pixels.data,
		static_cast<std::uint32_t>(late_background.pixels.size));
	{
		SpriteAtlasBuilder atlas;
		for (std::uint32_t index = 0; index < 10; ++index)
		{
			if (!add_sprite_to_atlas(
					atlas, sprites, index + 1, next.shapes[index]))
			{
				destroy_cd_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.sprite_atlas))
		{
			destroy_cd_assets(next);
			return false;
		}
	}
	std::uint8_t palette[256 * 4];
	if (!assets::decode_sprite_palette(sprites, 0, palette)
		|| !create_font_atlas(
			font,
			next.font_atlas,
			next.glyphs,
			next.glyph_count,
			next.font_height))
	{
		destroy_cd_assets(next);
		return false;
	}
	next.palette = create_palette(palette);
	next.ready =
		bgfx::isValid(next.early_background.handle)
		&& bgfx::isValid(next.late_background.handle)
		&& bgfx::isValid(next.palette)
		&& bgfx::isValid(next.font_atlas.handle);
	if (!next.ready)
	{
		destroy_cd_assets(next);
		return false;
	}
	destroy_cd_assets(renderer.cd);
	renderer.cd = static_cast<FrontendCdAssets&&>(next);
	next.palette = BGFX_INVALID_HANDLE;
	next.ready = false;
	return true;
}

bool frontend_medal_assets_init(
	FrontendRenderer& renderer,
	const assets::SpriteList (&early_medals)[3],
	const assets::SpriteList (&early_bars)[3],
	const assets::SpriteList (&late_medals)[6],
	const assets::SpriteList (&late_bars)[5])
{
	constexpr std::uint32_t kLateMedalShapeCounts[] = {6, 6, 6, 8, 7, 8};
	if (!renderer.ready)
	{
		return false;
	}
	FrontendMedalAssets next;
	std::uint8_t palette[256 * 4];
	for (std::uint32_t award = 0; award < 3; ++award)
	{
		if (!assets::decode_sprite_palette(
				early_medals[award], 0, palette))
		{
			destroy_medal_assets(next);
			return false;
		}
		next.early_medal_palettes[award] = create_palette(palette);
		SpriteAtlasBuilder medal_atlas;
		for (std::uint32_t shape = 0; shape < 17; ++shape)
		{
			if (!add_sprite_to_atlas(
					medal_atlas,
					early_medals[award],
					shape + 1,
					next.early_medals[award][shape]))
			{
				destroy_medal_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(
				medal_atlas, next.early_medal_atlases[award]))
		{
			destroy_medal_assets(next);
			return false;
		}
		if (!assets::decode_sprite_palette(early_bars[award], 0, palette))
		{
			destroy_medal_assets(next);
			return false;
		}
		next.early_bar_palettes[award] = create_palette(palette);
		SpriteAtlasBuilder bar_atlas;
		for (std::uint32_t shape = 0; shape < 15; ++shape)
		{
			if (!add_sprite_to_atlas(
					bar_atlas,
					early_bars[award],
					shape + 1,
					next.early_bars[award][shape]))
			{
				destroy_medal_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(
				bar_atlas, next.early_bar_atlases[award]))
		{
			destroy_medal_assets(next);
			return false;
		}
	}
	for (std::uint32_t award = 0; award < 6; ++award)
	{
		if (!assets::decode_sprite_palette(late_medals[award], 0, palette))
		{
			destroy_medal_assets(next);
			return false;
		}
		next.late_medal_palettes[award] = create_palette(palette);
		SpriteAtlasBuilder atlas;
		for (std::uint32_t shape = 0;
			shape < kLateMedalShapeCounts[award];
			++shape)
		{
			if (!add_sprite_to_atlas(
					atlas,
					late_medals[award],
					shape + 1,
					next.late_medals[award][shape]))
			{
				destroy_medal_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.late_medal_atlases[award]))
		{
			destroy_medal_assets(next);
			return false;
		}
	}
	for (std::uint32_t award = 0; award < 5; ++award)
	{
		if (!assets::decode_sprite_palette(late_bars[award], 0, palette))
		{
			destroy_medal_assets(next);
			return false;
		}
		next.late_bar_palettes[award] = create_palette(palette);
		SpriteAtlasBuilder atlas;
		for (std::uint32_t shape = 0; shape < 8; ++shape)
		{
			if (!add_sprite_to_atlas(
					atlas,
					late_bars[award],
					shape + 1,
					next.late_bars[award][shape]))
			{
				destroy_medal_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.late_bar_atlases[award]))
		{
			destroy_medal_assets(next);
			return false;
		}
	}
	bool palettes_valid = true;
	auto check_palettes = [&palettes_valid](const auto& palettes)
	{
		for (bgfx::TextureHandle palette : palettes)
		{
			palettes_valid = bgfx::isValid(palette) && palettes_valid;
		}
	};
	check_palettes(next.early_medal_palettes);
	check_palettes(next.early_bar_palettes);
	check_palettes(next.late_medal_palettes);
	check_palettes(next.late_bar_palettes);
	if (!palettes_valid)
	{
		destroy_medal_assets(next);
		return false;
	}
	next.ready = true;
	destroy_medal_assets(renderer.medals);
	renderer.medals = static_cast<FrontendMedalAssets&&>(next);
	auto invalidate_palettes = [](auto& palettes)
	{
		for (bgfx::TextureHandle& palette : palettes)
		{
			palette = BGFX_INVALID_HANDLE;
		}
	};
	invalidate_palettes(next.early_medal_palettes);
	invalidate_palettes(next.early_bar_palettes);
	invalidate_palettes(next.late_medal_palettes);
	invalidate_palettes(next.late_bar_palettes);
	next.ready = false;
	return true;
}

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
	const assets::SpriteList& kills_sprites)
{
	if (!renderer.ready)
	{
		return false;
	}
	FrontendItacAssets next;
	for (std::uint32_t index = 0; index < 8; ++index)
	{
		if (backgrounds[index].width != kFrontendWidth
			|| backgrounds[index].height != kFrontendHeight)
		{
			destroy_itac_assets(next);
			return false;
		}
		next.backgrounds[index] = create_texture(
			kFrontendWidth,
			kFrontendHeight,
			bgfx::TextureFormat::RGBA8,
			backgrounds[index].pixels.data,
			static_cast<std::uint32_t>(backgrounds[index].pixels.size));
		if (!bgfx::isValid(next.backgrounds[index].handle))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	if (!create_font_atlas(
			font,
			next.font_atlas,
			next.glyphs,
			next.glyph_count,
			next.font_height))
	{
		destroy_itac_assets(next);
		return false;
	}
	constexpr std::uint8_t kNewsShapeIds[] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
		15, 16, 17, 19, 20, 21, 22, 24, 25, 27, 28, 29, 30};
	SpriteAtlasBuilder news_atlas;
	for (std::uint8_t shape : kNewsShapeIds)
	{
		if (!add_sprite_to_atlas(
				news_atlas,
				news_sprites, shape, next.news[shape - 1]))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	if (!finish_sprite_atlas(news_atlas, next.news_atlas))
	{
		destroy_itac_assets(next);
		return false;
	}
	constexpr std::uint8_t kNewsPaletteIds[] = {0, 13, 26};
	std::uint8_t palette[256 * 4];
	for (std::uint32_t index = 0; index < 3; ++index)
	{
		if (!assets::decode_sprite_palette(
				news_sprites, kNewsPaletteIds[index], palette))
		{
			destroy_itac_assets(next);
			return false;
		}
		next.news_palettes[index] = create_palette(palette);
		if (!bgfx::isValid(next.news_palettes[index]))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	{
		SpriteAtlasBuilder atlas;
		for (std::uint8_t shape = 29; shape <= 35; ++shape)
		{
			if (!add_sprite_to_atlas(
					atlas,
					video_sprites,
					shape,
					next.video[shape - 29]))
			{
				destroy_itac_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.video_atlas))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	if (!assets::decode_sprite_palette(video_sprites, 0, palette))
	{
		destroy_itac_assets(next);
		return false;
	}
	next.video_palette = create_palette(palette);
	if (!assets::decode_sprite_palette(video_sprites, 26, palette))
	{
		destroy_itac_assets(next);
		return false;
	}
	next.video_thumbnail_palette = create_palette(palette);
	if (!bgfx::isValid(next.video_palette)
		|| !bgfx::isValid(next.video_thumbnail_palette))
	{
		destroy_itac_assets(next);
		return false;
	}
	{
		SpriteAtlasBuilder atlas;
		if (!add_sprite_to_atlas(
				atlas, gfx_sprites, 24, next.faction_icons[1])
			|| !add_sprite_to_atlas(
				atlas, gfx_sprites, 25, next.faction_icons[0])
			|| !finish_sprite_atlas(atlas, next.gfx_atlas)
			|| !assets::decode_sprite_palette(gfx_sprites, 23, palette))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	next.faction_palette = create_palette(palette);
	constexpr std::uint8_t kSquadPaletteIds[] = {
		0, 4, 8, 12, 16, 20, 24, 26, 30, 34};
	{
		SpriteAtlasBuilder atlas;
		for (std::uint8_t shape = 1; shape <= 36; ++shape)
		{
			bool palette_record = false;
			for (std::uint8_t palette_id : kSquadPaletteIds)
			{
				if (shape == palette_id)
				{
					palette_record = true;
					break;
				}
			}
			if (!palette_record
				&& !add_sprite_to_atlas(
					atlas,
					squad_sprites,
					shape,
					next.squads[shape - 1]))
			{
				destroy_itac_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.squad_atlas))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	for (std::uint32_t index = 0; index < 10; ++index)
	{
		if (!assets::decode_sprite_palette(
				squad_sprites, kSquadPaletteIds[index], palette))
		{
			destroy_itac_assets(next);
			return false;
		}
		next.squad_palettes[index] = create_palette(palette);
		if (!bgfx::isValid(next.squad_palettes[index]))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	constexpr std::uint8_t kPersonPaletteIds[] = {0, 9, 18, 24, 33, 35};
	{
		SpriteAtlasBuilder atlas;
		for (std::uint8_t shape = 1; shape <= 41; ++shape)
		{
			bool palette_record = false;
			for (std::uint8_t palette_id : kPersonPaletteIds)
			{
				if (shape == palette_id)
				{
					palette_record = true;
					break;
				}
			}
			if (!palette_record
				&& !add_sprite_to_atlas(
					atlas,
					person_sprites,
					shape,
					next.persons[shape - 1]))
			{
				destroy_itac_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.person_atlas))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	for (std::uint32_t index = 0; index < 6; ++index)
	{
		if (!assets::decode_sprite_palette(
				person_sprites, kPersonPaletteIds[index], palette))
		{
			destroy_itac_assets(next);
			return false;
		}
		next.person_palettes[index] = create_palette(palette);
		if (!bgfx::isValid(next.person_palettes[index]))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	constexpr std::uint8_t kCapitalShapeIds[] = {
		1, 2, 4, 5, 7, 8, 10, 11, 13, 14, 16, 17, 19, 20, 22, 23,
		25, 26, 28, 29, 31, 32, 35, 37, 38, 40, 41, 43, 44, 46,
		47, 49, 50, 52, 53, 55, 56};
	{
		SpriteAtlasBuilder atlas;
		for (std::uint8_t shape : kCapitalShapeIds)
		{
			if (!add_sprite_to_atlas(
					atlas,
					capital_sprites,
					shape,
					next.capitals[shape - 1]))
			{
				destroy_itac_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.capital_atlas))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	for (std::uint8_t index = 0; index < 19; ++index)
	{
		if (!assets::decode_sprite_palette(
				capital_sprites,
				static_cast<std::uint8_t>(index * 3),
				palette))
		{
			destroy_itac_assets(next);
			return false;
		}
		next.capital_palettes[index] = create_palette(palette);
		if (!bgfx::isValid(next.capital_palettes[index]))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	constexpr std::uint8_t kFighterShapeIds[] = {
		1, 2, 3, 4, 6, 7, 8, 9, 11, 13, 14, 16, 18, 19, 20, 21,
		23, 24, 25, 26, 28};
	{
		SpriteAtlasBuilder atlas;
		for (std::uint8_t shape : kFighterShapeIds)
		{
			if (!add_sprite_to_atlas(
					atlas,
					fighter_sprites,
					shape,
					next.fighters[shape - 1]))
			{
				destroy_itac_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.fighter_atlas))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	constexpr std::uint8_t kFighterPaletteIds[] = {0, 5, 10, 15, 17, 22, 27};
	for (std::uint8_t index = 0; index < 7; ++index)
	{
		if (!assets::decode_sprite_palette(
				fighter_sprites, kFighterPaletteIds[index], palette))
		{
			destroy_itac_assets(next);
			return false;
		}
		next.fighter_palettes[index] = create_palette(palette);
		if (!bgfx::isValid(next.fighter_palettes[index]))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	constexpr std::uint8_t kKillsShapeIds[] = {
		1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12,
		13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
	{
		SpriteAtlasBuilder atlas;
		for (std::uint8_t shape : kKillsShapeIds)
		{
			if (!add_sprite_to_atlas(
					atlas,
					kills_sprites,
					shape,
					next.kills[shape - 1]))
			{
				destroy_itac_assets(next);
				return false;
			}
		}
		if (!finish_sprite_atlas(atlas, next.kills_atlas))
		{
			destroy_itac_assets(next);
			return false;
		}
	}
	constexpr std::uint8_t kKillsPaletteIds[] = {0, 6};
	for (std::uint8_t index = 0; index < 2; ++index)
	{
		if (!assets::decode_sprite_palette(
				kills_sprites, kKillsPaletteIds[index], palette))
		{
			destroy_itac_assets(next);
			return false;
		}
		next.kills_palettes[index] = create_palette(palette);
	}
	if (!bgfx::isValid(next.faction_palette)
		|| !bgfx::isValid(next.kills_palettes[0])
		|| !bgfx::isValid(next.kills_palettes[1]))
	{
		destroy_itac_assets(next);
		return false;
	}
	next.ready = true;
	destroy_itac_assets(renderer.itac);
	renderer.itac = static_cast<FrontendItacAssets&&>(next);
	auto invalidate_palettes = [](auto& palettes)
	{
		for (bgfx::TextureHandle& palette : palettes)
		{
			palette = BGFX_INVALID_HANDLE;
		}
	};
	invalidate_palettes(next.news_palettes);
	next.video_palette = BGFX_INVALID_HANDLE;
	next.video_thumbnail_palette = BGFX_INVALID_HANDLE;
	next.faction_palette = BGFX_INVALID_HANDLE;
	invalidate_palettes(next.squad_palettes);
	invalidate_palettes(next.person_palettes);
	invalidate_palettes(next.capital_palettes);
	invalidate_palettes(next.fighter_palettes);
	invalidate_palettes(next.kills_palettes);
	next.ready = false;
	return true;
}

bool frontend_credits_assets_init(
	FrontendRenderer& renderer,
	const assets::SpriteList& sprites,
	const assets::Font& font)
{
	if (!renderer.ready || sprites.shape_count < 12)
	{
		return false;
	}
	FrontendCreditsAssets next;
	SpriteAtlasBuilder atlas;
	std::uint8_t palette[256 * 4];
	for (std::uint32_t visual = 0;
		visual < kCreditsVisualCount;
		++visual)
	{
		const std::uint32_t palette_shape = visual * 2;
		const std::uint32_t image_shape = palette_shape + 1;
		if (!assets::decode_sprite_palette(
				sprites, palette_shape, palette)
			|| !add_sprite_to_atlas(
				atlas,
				sprites,
				image_shape,
				next.backgrounds[visual]))
		{
			destroy_credits_assets(next);
			return false;
		}
		next.background_palettes[visual] = create_palette(palette);
		if (!bgfx::isValid(next.background_palettes[visual]))
		{
			destroy_credits_assets(next);
			return false;
		}
	}
	if (!finish_sprite_atlas(atlas, next.sprite_atlas)
		|| !create_font_atlas(
			font,
			next.font_atlas,
			next.glyphs,
			next.glyph_count,
			next.font_height))
	{
		destroy_credits_assets(next);
		return false;
	}
	make_credits_font_palette(palette, 255, 126, 0);
	next.orange_palette = create_palette(palette);
	make_credits_font_palette(palette, 255, 255, 255);
	next.white_palette = create_palette(palette);
	next.ready =
		bgfx::isValid(next.backgrounds[0].handle)
		&& bgfx::isValid(next.font_atlas.handle)
		&& bgfx::isValid(next.orange_palette)
		&& bgfx::isValid(next.white_palette);
	if (!next.ready)
	{
		destroy_credits_assets(next);
		return false;
	}

	destroy_credits_assets(renderer.credits);
	renderer.credits = static_cast<FrontendCreditsAssets&&>(next);
	for (bgfx::TextureHandle& palette_handle : next.background_palettes)
	{
		palette_handle = BGFX_INVALID_HANDLE;
	}
	next.orange_palette = BGFX_INVALID_HANDLE;
	next.white_palette = BGFX_INVALID_HANDLE;
	next.ready = false;
	return true;
}

void frontend_renderer_shutdown(FrontendRenderer& renderer)
{
	destroy_shell_assets(renderer.shell);
	destroy_campaign_assets(renderer.campaign);
	destroy_multiplayer_assets(renderer.multiplayer);
	destroy_vr_assets(renderer.vr);
	for (FrontendVrAmbientAssets& ambient : renderer.vr_ambient)
	{
		destroy_vr_ambient_assets(ambient);
	}
	destroy_briefing_assets(renderer.briefing);
	destroy_loadout_assets(renderer.loadout);
	loadout_renderer_shutdown(renderer.loadout_renderer);
	destroy_debrief_assets(renderer.debrief);
	destroy_restart_assets(renderer.restart);
	destroy_sim_pod_assets(renderer.sim_pod);
	destroy_cd_assets(renderer.cd);
	destroy_medal_assets(renderer.medals);
	destroy_itac_assets(renderer.itac);
	destroy_credits_assets(renderer.credits);
	destroy_renderer_core(renderer);
}

bool frontend_movie_texture_init(
	FrontendTexture& texture,
	std::uint16_t width,
	std::uint16_t height)
{
	if (bgfx::isValid(texture.handle)
		&& bgfx::isValid(texture.stream_back)
		&& texture.owns_handle
		&& texture.width == width
		&& texture.height == height)
	{
		return true;
	}
	frontend_texture_shutdown(texture);
	texture.handle = bgfx::createTexture2D(
		width,
		height,
		false,
		1,
		bgfx::TextureFormat::RGBA8,
		kTextureFlags);
	if (!bgfx::isValid(texture.handle))
	{
		texture = {};
		return false;
	}
	texture.stream_back = bgfx::createTexture2D(
		width,
		height,
		false,
		1,
		bgfx::TextureFormat::RGBA8,
		kTextureFlags);
	if (!bgfx::isValid(texture.stream_back))
	{
		bgfx::destroy(texture.handle);
		texture = {};
		return false;
	}
	g_texture_stats.created += 2;
	g_texture_stats.live += 2;
	g_texture_stats.high_water =
		std::max(g_texture_stats.high_water, g_texture_stats.live);
	texture.width = width;
	texture.height = height;
	texture.owns_handle = true;
	return true;
}

bool frontend_texture_upload(
	FrontendTexture& texture,
	const assets::TextureImage& image)
{
	const std::uint64_t base_bytes =
		static_cast<std::uint64_t>(image.width) * image.height * 4;
	if (image.width == 0
		|| image.height == 0
		|| image.width > UINT16_MAX
		|| image.height > UINT16_MAX
		|| base_bytes > UINT32_MAX
		|| base_bytes > image.pixels.size)
	{
		return false;
	}
	FrontendTexture next = create_texture(
		static_cast<std::uint16_t>(image.width),
		static_cast<std::uint16_t>(image.height),
		bgfx::TextureFormat::RGBA8,
		image.pixels.data,
		static_cast<std::uint32_t>(base_bytes));
	if (!bgfx::isValid(next.handle))
	{
		return false;
	}
	frontend_texture_shutdown(texture);
	texture = static_cast<FrontendTexture&&>(next);
	return true;
}

bool mission_texture_upload(
	FrontendTexture& texture,
	const assets::TextureImage& image)
{
	const std::uint32_t bytes_per_pixel =
		image.pixel_format == assets::TexturePixelFormat::rgb565
			? 2u
			: 4u;
	const std::uint64_t base_bytes =
		static_cast<std::uint64_t>(image.width)
			* image.height * bytes_per_pixel;
	if (image.width == 0
		|| image.height == 0
		|| image.width > UINT16_MAX
		|| image.height > UINT16_MAX
		|| image.mip_levels == 0
		|| base_bytes > UINT32_MAX
		|| base_bytes > image.pixels.size)
	{
		return false;
	}
	// srd3d device setup (0x100044d5..0x100045ec) sets both texture
	// stages to D3DTFG_LINEAR, D3DTFN_LINEAR, and D3DTFP_LINEAR.
	// D3DTSS_ADDRESS remains D3DTADDRESS_WRAP (0x1000460c..0x10004622).
	// In bgfx those retail defaults are represented by zero sampler flags.
	FrontendTexture next = create_mip_texture(image, 0);
	if (!bgfx::isValid(next.handle))
	{
		return false;
	}
	frontend_texture_shutdown(texture);
	texture = static_cast<FrontendTexture&&>(next);
	return true;
}

bool frontend_sprite_atlas_upload(
	FrontendSpriteAtlas& atlas,
	const FrontendSpriteAtlasSource* sources,
	std::uint32_t source_count)
{
	if (sources == nullptr || source_count == 0)
	{
		return false;
	}
	SpriteAtlasBuilder builder;
	for (std::uint32_t index = 0; index < source_count; ++index)
	{
		const FrontendSpriteAtlasSource& source = sources[index];
		if (source.sprites == nullptr || source.destination == nullptr
			|| !add_sprite_to_atlas(
				builder,
				*source.sprites,
				source.shape,
				*source.destination))
		{
			return false;
		}
	}
	return finish_sprite_atlas(builder, atlas);
}

void frontend_movie_texture_update(
	FrontendTexture& texture,
	const std::uint8_t* rgba)
{
	if (!bgfx::isValid(texture.stream_back) || rgba == nullptr)
	{
		return;
	}
	const std::uint32_t byte_count =
		static_cast<std::uint32_t>(texture.width) * texture.height * 4;
	bgfx::updateTexture2D(
		texture.stream_back,
		0,
		0,
		0,
		0,
		texture.width,
		texture.height,
		bgfx::copy(rgba, byte_count),
		static_cast<std::uint16_t>(texture.width * 4));
	std::swap(texture.handle, texture.stream_back);
}

void frontend_gameplay_powerball_update(
	FrontendRenderer& renderer,
	float cursor_x,
	float cursor_y)
{
	FrontendShellAssets& assets = renderer.shell;
	if (!bgfx::isValid(assets.gameplay_powerball.handle))
	{
		return;
	}
	// LANCER.EXE HUD panel case 7 combines the two half-scale cursor
	// components into the wrapped 16-bit lookup offset before adding the
	// precomputed projected coordinate at each sphere pixel.
	const std::int32_t base =
		static_cast<std::int32_t>(std::trunc(cursor_x * 0.5f))
		- static_cast<std::int32_t>(
				std::trunc(cursor_y * -0.5f))
			* 256;
	const std::uint16_t offset = static_cast<std::uint16_t>(base);
	if (assets.gameplay_powerball_valid
		&& assets.gameplay_powerball_offset == offset)
	{
		return;
	}
	assets.gameplay_powerball_offset = offset;
	assets.gameplay_powerball_valid = true;
	std::memset(
		assets.gameplay_powerball_rgba,
		0,
		sizeof(assets.gameplay_powerball_rgba));
	for (std::int32_t row = -31; row < 31; ++row)
	{
		const std::int32_t output_y = row + 31;
		const std::int32_t half_width =
			assets.gameplay_powerball_half_width[output_y];
		for (std::int32_t column = -half_width;
			column < half_width;
			++column)
		{
			const std::int32_t output_x = column + 31;
			const std::uint32_t cell =
				static_cast<std::uint32_t>(output_y * 62 + output_x);
			const std::uint16_t lookup = static_cast<std::uint16_t>(
				base + assets.gameplay_powerball_coordinates[cell]);
			const std::uint8_t shade =
				assets.gameplay_powerball_lookup[lookup];
			const std::uint16_t color = static_cast<std::uint16_t>(
				assets.gameplay_powerball_lighting[cell] * 32 + shade);
			std::memcpy(
				assets.gameplay_powerball_rgba + cell * 4,
				assets.gameplay_powerball_ramp + color * 4,
				4);
		}
	}
	frontend_movie_texture_update(
		assets.gameplay_powerball,
		assets.gameplay_powerball_rgba);
}

void frontend_texture_shutdown(FrontendTexture& texture)
{
	destroy_texture(texture);
}

FrontendTextureStats frontend_texture_stats()
{
	return g_texture_stats;
}

void frontend_commands_begin(FrontendCommands& commands)
{
	commands.count = 0;
	commands.loadout = {};
}

void frontend_rgba_quad(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	float x,
	float y,
	float width,
	float height,
	std::uint32_t rgba)
{
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::rgba_quad);
	command.texture = texture.handle;
	command.x = x;
	command.y = y;
	command.width = width;
	command.height = height;
	command.u = texture.u;
	command.v = texture.v;
	command.uv_width = texture.uv_width;
	command.uv_height = texture.uv_height;
	command.rgba = rgba;
}

void frontend_rgba_additive_quad(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	float x,
	float y,
	float width,
	float height,
	std::uint32_t rgba)
{
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::rgba_additive_quad);
	command.texture = texture.handle;
	command.x = x;
	command.y = y;
	command.width = width;
	command.height = height;
	command.u = texture.u;
	command.v = texture.v;
	command.uv_width = texture.uv_width;
	command.uv_height = texture.uv_height;
	command.rgba = rgba;
}

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
	std::uint32_t rgba)
{
	if (source_x >= texture.width || source_y >= texture.height)
	{
		return;
	}
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::rgba_quad);
	command.texture = texture.handle;
	command.x = destination_x;
	command.y = destination_y;
	command.width = destination_width;
	command.height = destination_height;
	command.u = texture.u
		+ static_cast<float>(source_x) / texture.width * texture.uv_width;
	command.v = texture.v
		+ static_cast<float>(source_y) / texture.height * texture.uv_height;
	command.uv_width =
		static_cast<float>(source_width) / texture.width * texture.uv_width;
	command.uv_height =
		static_cast<float>(source_height) / texture.height * texture.uv_height;
	command.rgba = rgba;
}

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
	std::uint32_t rgba)
{
	if (source_x >= texture.width || source_y >= texture.height)
	{
		return;
	}
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::rgba_quad);
	command.texture = texture.handle;
	command.x = destination_x;
	command.y = destination_y;
	command.width = destination_width;
	command.height = destination_height;
	command.u = texture.u
		+ static_cast<float>(source_x) / texture.width * texture.uv_width;
	command.v = texture.v
		+ static_cast<float>(source_y) / texture.height * texture.uv_height;
	command.uv_width =
		static_cast<float>(source_width) / texture.width * texture.uv_width;
	command.uv_height =
		static_cast<float>(source_height) / texture.height * texture.uv_height;
	command.rotation = rotation;
	command.rgba = rgba;
}

void frontend_loadout_scene(
	FrontendCommands& commands,
	std::uint8_t selected_ship,
	std::uint8_t available_ships,
	std::uint16_t available_ship_mask,
	std::uint8_t page,
	std::uint8_t difficulty,
	bool use_default_loadout,
	const std::int16_t (&mounted_loadout)[20],
	const bool (&missile_animation_active)[20],
	const bool (&missile_animation_removing)[20],
	const std::uint8_t (&missile_animation_item)[20],
	const std::uint64_t (&missile_animation_at)[20],
	std::uint16_t available_missile_mask,
	std::uint8_t missile_layout_tier,
	std::uint8_t selected_missile,
	std::int8_t hovered_hardpoint,
	std::uint8_t previous_page,
	float page_transition,
	float hardpoint_zoom,
	std::uint8_t previous_ship,
	float ship_selection,
	float activation,
	bool reverse,
	float previous_spin,
	std::uint64_t now)
{
	push_command(commands, FrontendCommandType::loadout_scene);
	LoadoutRenderState& state = commands.loadout;
	state.selected_ship = std::min<std::uint8_t>(selected_ship, 11);
	state.available_ships =
		std::clamp<std::uint8_t>(available_ships, 1, 12);
	state.available_ship_mask =
		static_cast<std::uint16_t>(available_ship_mask & 0x0fff);
	state.page = std::min<std::uint8_t>(page, 2);
	state.previous_ship = std::min<std::uint8_t>(previous_ship, 11);
	state.difficulty = difficulty;
	state.use_default_loadout = use_default_loadout;
	state.spin = static_cast<float>(now % 2000000)
		* 0.0015707963611930609f;
	state.activation = std::clamp(activation, 0.0f, 1.0f);
	state.reverse = reverse;
	state.previous_spin = previous_spin;
	state.missile_mask = available_missile_mask;
	state.missile_layout_tier =
		std::min<std::uint8_t>(missile_layout_tier, 3);
	state.selected_missile = selected_missile;
	state.hovered_hardpoint = hovered_hardpoint;
	state.previous_page = std::min<std::uint8_t>(previous_page, 2);
	state.page_transition =
		std::clamp(page_transition, 0.0f, 1.0f);
	state.hardpoint_zoom =
		std::clamp(hardpoint_zoom, 0.001f, 1.0f);
	state.ship_selection = std::clamp(ship_selection, 0.0f, 1.0f);
	std::copy(
		std::begin(mounted_loadout),
		std::end(mounted_loadout),
		std::begin(state.mounted_loadout));
	const std::uint64_t animation_now = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	for (std::uint8_t hardpoint = 0; hardpoint < 20; ++hardpoint)
	{
		state.missile_animation_active[hardpoint] =
			missile_animation_active[hardpoint];
		state.missile_animation_removing[hardpoint] =
			missile_animation_removing[hardpoint];
		state.missile_animation_item[hardpoint] =
			missile_animation_item[hardpoint];
		state.missile_animation_progress[hardpoint] =
			missile_animation_active[hardpoint]
				? std::clamp(
					static_cast<float>(
						animation_now > missile_animation_at[hardpoint]
							? animation_now
								- missile_animation_at[hardpoint]
							: 0)
						/ 1000.0f,
					0.0f,
					1.0f)
				: 1.0f;
	}
}

void frontend_loadout_text(
	FrontendCommands& commands,
	std::uint8_t panel,
	float x,
	float y,
	const char* text)
{
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::loadout_text);
	command.x = x;
	command.y = y;
	command.height = panel;
	std::snprintf(command.text, sizeof(command.text), "%s", text);
}

void frontend_loadout_bar(
	FrontendCommands& commands,
	float x,
	float y,
	float width,
	float height,
	bool active)
{
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::loadout_bar);
	command.x = x;
	command.y = y;
	command.width = width;
	command.height = height;
	command.rgba = active ? 1u : 0u;
}

void frontend_loadout_cursor(
	FrontendCommands& commands,
	float x,
	float y)
{
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::loadout_cursor);
	command.x = x;
	command.y = y;
}

void frontend_rgba_line(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	float x1,
	float y1,
	float x2,
	float y2,
	float thickness,
	std::uint32_t rgba)
{
	const float delta_x = x2 - x1;
	const float delta_y = y2 - y1;
	const float angle = std::atan2(delta_y, delta_x);
	const float half_thickness = thickness * 0.5f;
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::rgba_quad);
	command.texture = texture.handle;
	command.x = x1 + std::sin(angle) * half_thickness;
	command.y = y1 - std::cos(angle) * half_thickness;
	command.width = std::sqrt(delta_x * delta_x + delta_y * delta_y);
	command.height = thickness;
	// math::srt applies the bgfx-compatible negative Euler rotation.
	// Negate the screen-space angle here so a positive framebuffer Y
	// delta still rotates the quad toward increasing framebuffer Y.
	command.rotation = -angle;
	command.rgba = rgba;
}

void frontend_indexed_scaled_quad(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	bgfx::TextureHandle palette,
	float x,
	float y,
	float width,
	float height,
	std::uint32_t rgba)
{
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::indexed_quad);
	command.texture = texture.handle;
	command.palette = palette;
	command.x = x;
	command.y = y;
	command.width = width;
	command.height = height;
	command.u = texture.u;
	command.v = texture.v;
	command.uv_width = texture.uv_width;
	command.uv_height = texture.uv_height;
	command.rgba = rgba;
}

void frontend_indexed_quad(
	FrontendCommands& commands,
	const FrontendTexture& texture,
	bgfx::TextureHandle palette,
	float x,
	float y,
	std::uint32_t rgba)
{
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::indexed_quad);
	command.texture = texture.handle;
	command.palette = palette;
	command.x = x + texture.offset_x;
	command.y = y + texture.offset_y;
	command.width = texture.width;
	command.height = texture.height;
	command.u = texture.u;
	command.v = texture.v;
	command.uv_width = texture.uv_width;
	command.uv_height = texture.uv_height;
	command.rgba = rgba;
}

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
	std::uint32_t rgba)
{
	if (source_x >= texture.width || source_y >= texture.height)
	{
		return;
	}
	const std::uint16_t clipped_width = static_cast<std::uint16_t>(
		source_x + width > texture.width
			? texture.width - source_x
			: width);
	const std::uint16_t clipped_height = static_cast<std::uint16_t>(
		source_y + height > texture.height
			? texture.height - source_y
			: height);
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::indexed_quad);
	command.texture = texture.handle;
	command.palette = palette;
	command.x = destination_x;
	command.y = destination_y;
	command.width = clipped_width;
	command.height = clipped_height;
	command.u = texture.u
		+ static_cast<float>(source_x) / texture.width * texture.uv_width;
	command.v = texture.v
		+ static_cast<float>(source_y) / texture.height * texture.uv_height;
	command.uv_width =
		static_cast<float>(clipped_width) / texture.width * texture.uv_width;
	command.uv_height =
		static_cast<float>(clipped_height) / texture.height * texture.uv_height;
	command.rgba = rgba;
}

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
	std::uint32_t rgba)
{
	if (source_width == 0 || source_height == 0
		|| texture.width == 0 || texture.height == 0)
	{
		return;
	}
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::indexed_quad);
	command.texture = texture.handle;
	command.palette = palette;
	command.x = destination_x;
	command.y = destination_y;
	command.width = destination_width;
	command.height = destination_height;
	command.u = texture.u
		+ texture.uv_width
			* static_cast<float>(source_x)
			/ static_cast<float>(texture.width);
	command.v = texture.v
		+ texture.uv_height
			* static_cast<float>(source_y)
			/ static_cast<float>(texture.height);
	command.uv_width = texture.uv_width
		* static_cast<float>(source_width)
		/ static_cast<float>(texture.width);
	command.uv_height = texture.uv_height
		* static_cast<float>(source_height)
		/ static_cast<float>(texture.height);
	command.rgba = rgba;
}

namespace
{
void frontend_font_text(
	FrontendCommands& commands,
	const FrontendTexture& atlas,
	const FrontendGlyph* glyphs,
	std::uint32_t glyph_count,
	std::uint32_t font_height,
	const char* text,
	float x,
	float y,
	bgfx::TextureHandle palette,
	std::uint32_t rgba,
	float scale)
{
	for (const auto* character =
		reinterpret_cast<const std::uint8_t*>(text);
		*character != 0;
		++character)
	{
		const std::uint32_t index = *character;
		if (index >= glyph_count)
		{
			continue;
		}
		const FrontendGlyph& glyph = glyphs[index];
		FrontendCommand& command =
			push_command(commands, FrontendCommandType::indexed_quad);
		command.texture = atlas.handle;
		command.palette = palette;
		command.x = x;
		command.y = y;
		command.width = glyph.width * scale;
		command.height = font_height * scale;
		command.u =
			static_cast<float>(glyph.x) / atlas.width;
		command.v =
			static_cast<float>(glyph.y) / atlas.height;
		command.uv_width =
			static_cast<float>(glyph.width) / atlas.width;
		command.uv_height =
			static_cast<float>(font_height) / atlas.height;
		command.rgba = rgba;
		x += glyph.width * scale;
	}
}
}

void frontend_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	bgfx::TextureHandle palette,
	std::uint32_t rgba,
	float scale)
{
	frontend_font_text(
		commands,
		renderer.shell.font_atlas,
		renderer.shell.glyphs,
		renderer.shell.glyph_count,
		renderer.shell.font_height,
		text,
		x,
		y,
		palette,
		rgba,
		scale);
}

void frontend_pause_small_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	bgfx::TextureHandle palette,
	std::uint32_t rgba,
	float scale)
{
	frontend_font_text(
		commands,
		renderer.shell.pause_small_font_atlas,
		renderer.shell.pause_small_glyphs,
		renderer.shell.pause_small_glyph_count,
		renderer.shell.pause_small_font_height,
		text,
		x,
		y,
		palette,
		rgba,
		scale);
}

void frontend_gameplay_hud_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	std::uint32_t rgba,
	float scale)
{
	frontend_font_text(
		commands,
		renderer.shell.gameplay_hud_font_atlas,
		renderer.shell.gameplay_hud_glyphs,
		renderer.shell.gameplay_hud_glyph_count,
		renderer.shell.gameplay_hud_font_height,
		text,
		x,
		y,
		renderer.shell.gameplay_hud_palette,
		rgba,
		scale);
}

void frontend_gameplay_scoreboard_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	std::uint32_t rgba,
	float scale)
{
	frontend_font_text(
		commands,
		renderer.shell.gameplay_scoreboard_font_atlas,
		renderer.shell.gameplay_scoreboard_glyphs,
		renderer.shell.gameplay_scoreboard_glyph_count,
		renderer.shell.gameplay_scoreboard_font_height,
		text,
		x,
		y,
		renderer.shell.gameplay_scoreboard_palette,
		rgba,
		scale);
}

void frontend_gameplay_hud_target_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	std::uint8_t allegiance_class,
	std::uint32_t rgba,
	float scale)
{
	frontend_font_text(
		commands,
		renderer.shell.gameplay_hud_font_atlas,
		renderer.shell.gameplay_hud_glyphs,
		renderer.shell.gameplay_hud_glyph_count,
		renderer.shell.gameplay_hud_font_height,
		text,
		x,
		y,
		renderer.shell.gameplay_hud_target_palette[
			std::min<std::uint8_t>(allegiance_class, 2)],
		rgba,
		scale);
}

void frontend_gameplay_message_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	std::uint32_t rgba,
	float scale)
{
	frontend_font_text(
		commands,
		renderer.shell.gameplay_message_font_atlas,
		renderer.shell.gameplay_message_glyphs,
		renderer.shell.gameplay_message_glyph_count,
		renderer.shell.gameplay_message_font_height,
		text,
		x,
		y,
		renderer.shell.gameplay_hud_palette,
		rgba,
		scale);
}

float frontend_pause_small_text_width(
	const FrontendRenderer& renderer,
	const char* text,
	float scale)
{
	float width = 0.0f;
	for (const auto* character =
		reinterpret_cast<const std::uint8_t*>(text);
		*character != 0;
		++character)
	{
		if (*character < renderer.shell.pause_small_glyph_count)
		{
			width += renderer.shell.pause_small_glyphs[*character].width
				* scale;
		}
	}
	return width;
}

void frontend_cd_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	bgfx::TextureHandle palette,
	std::uint32_t rgba,
	float scale)
{
	frontend_font_text(
		commands,
		renderer.cd.font_atlas,
		renderer.cd.glyphs,
		renderer.cd.glyph_count,
		renderer.cd.font_height,
		text,
		x,
		y,
		palette,
		rgba,
		scale);
}

void frontend_itac_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	bgfx::TextureHandle palette,
	std::uint32_t rgba,
	float scale)
{
	frontend_font_text(
		commands,
		renderer.itac.font_atlas,
		renderer.itac.glyphs,
		renderer.itac.glyph_count,
		renderer.itac.font_height,
		text,
		x,
		y,
		palette,
		rgba,
		scale);
}

float frontend_credits_text_width(
	const FrontendRenderer& renderer,
	const char* text)
{
	float width = 0.0f;
	for (const auto* character =
		reinterpret_cast<const std::uint8_t*>(text);
		*character != 0;
		++character)
	{
		if (*character < renderer.credits.glyph_count)
		{
			width += renderer.credits.glyphs[*character].width;
		}
	}
	return width;
}

void frontend_credits_text(
	FrontendCommands& commands,
	const FrontendRenderer& renderer,
	const char* text,
	float x,
	float y,
	bgfx::TextureHandle palette,
	std::uint32_t rgba)
{
	frontend_font_text(
		commands,
		renderer.credits.font_atlas,
		renderer.credits.glyphs,
		renderer.credits.glyph_count,
		renderer.credits.font_height,
		text,
		x,
		y,
		palette,
		rgba,
		1.0f);
}

void frontend_scissor(
	FrontendCommands& commands,
	std::uint16_t x,
	std::uint16_t y,
	std::uint16_t width,
	std::uint16_t height)
{
	FrontendCommand& command =
		push_command(commands, FrontendCommandType::scissor);
	command.x = x;
	command.y = y;
	command.width = width;
	command.height = height;
}

void frontend_submit(
	const FrontendRenderer& renderer,
	const FrontendCommands& commands,
	std::uint32_t backbuffer_width,
	std::uint32_t backbuffer_height,
	float brightness,
	bool full_viewport,
	bool native_canvas)
{
	bgfx::setViewRect(kClearView, 0, 0, bgfx::BackbufferRatio::Equal);
	bgfx::setViewClear(
		kClearView,
		BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
		0x000000ff,
		1.0f);
	bgfx::touch(kClearView);

	float scale_x = 1.0f;
	float scale_y = 1.0f;
	std::uint16_t viewport_x = 0;
	std::uint16_t viewport_y = 0;
	std::uint16_t viewport_width =
		static_cast<std::uint16_t>(backbuffer_width);
	std::uint16_t viewport_height =
		static_cast<std::uint16_t>(backbuffer_height);
	if (native_canvas)
	{
		scale_x = 1.0f;
		scale_y = 1.0f;
	}
	else if (full_viewport)
	{
		scale_x = static_cast<float>(backbuffer_width) / kFrontendWidth;
		scale_y = static_cast<float>(backbuffer_height) / kFrontendHeight;
	}
	else
	{
		canvas_viewport(
			backbuffer_width,
			backbuffer_height,
			scale_x,
			viewport_x,
			viewport_y,
			viewport_width,
			viewport_height);
		scale_y = scale_x;
	}
	bgfx::setViewRect(
		kFrontendView,
		viewport_x,
		viewport_y,
		viewport_width,
		viewport_height);
	bgfx::setViewMode(kFrontendView, bgfx::ViewMode::Sequential);
	bgfx::setViewRect(
		kFrontendOverlayView,
		viewport_x,
		viewport_y,
		viewport_width,
		viewport_height);
	bgfx::setViewMode(kFrontendOverlayView, bgfx::ViewMode::Sequential);

	const glm::mat4 view{1.0f};
	const glm::mat4 projection = sl_open::math::orthographic_lh(
		0.0f,
		native_canvas
			? static_cast<float>(backbuffer_width)
			: static_cast<float>(kFrontendWidth),
		native_canvas
			? static_cast<float>(backbuffer_height)
			: static_cast<float>(kFrontendHeight),
		0.0f,
		0.0f,
		100.0f,
		bgfx::getCaps()->homogeneousDepth);
	bgfx::setViewTransform(
		kFrontendView,
		glm::value_ptr(view),
		glm::value_ptr(projection));
	bgfx::setViewTransform(
		kFrontendOverlayView,
		glm::value_ptr(view),
		glm::value_ptr(projection));
	bgfx::touch(kFrontendView);

	bgfx::ViewId frontend_view = kFrontendView;
	for (std::uint32_t index = 0; index < commands.count; ++index)
	{
		const FrontendCommand& command = commands.items[index];
		if (command.type == FrontendCommandType::loadout_scene)
		{
			loadout_renderer_submit(
				renderer, commands, index, viewport_x, viewport_y,
				viewport_width, viewport_height, brightness);
			frontend_view = kFrontendOverlayView;
			continue;
		}
		if (command.type == FrontendCommandType::loadout_text
			|| command.type == FrontendCommandType::loadout_bar)
		{
			continue;
		}
		if (command.type == FrontendCommandType::loadout_cursor)
		{
			// InterfaceCursor_mesh_data_create (0x004244e0) builds this
			// seven-point, ten-triangle cursor. The retail loadout creates it
			// only after the spin-disc activation boundary and shades its
			// uniform ld_cursor material with the dedicated green cursor
			// light. Duplicate vertices preserve its flat-shaded facets.
			constexpr glm::vec3 positions[7] = {
				{0.0f, 0.0f, 0.0f},
				{0.6f, 1.6f, 0.0f},
				{0.3f, 2.4f, 0.0f},
				{-0.3f, 2.4f, 0.0f},
				{-0.6f, 1.6f, 0.0f},
				{0.0f, 1.2f, -0.2f},
				{0.0f, 1.2f, 0.2f},
			};
			constexpr std::uint8_t triangles[10][3] = {
				{1, 0, 5}, {2, 1, 5}, {3, 2, 5}, {4, 3, 5}, {4, 5, 0},
				{6, 5, 4}, {6, 0, 4}, {6, 2, 3}, {6, 1, 2}, {6, 0, 1},
			};
			constexpr std::uint32_t facet_colors[10] = {
				0xff5ce848, 0xff38942c, 0xff205818, 0xff286c20,
				0xff58d844, 0xff307c28, 0xff48d040, 0xff287020,
				0xff3c9c30, 0xff50dc44,
			};
			constexpr float scale = 6.0f;
			FrontendVertex vertices[30];
			for (std::uint32_t triangle = 0; triangle < 10; ++triangle)
			{
				for (std::uint32_t corner = 0; corner < 3; ++corner)
				{
					const glm::vec3 source =
						positions[triangles[triangle][corner]];
					FrontendVertex& vertex =
						vertices[triangle * 3 + corner];
					vertex = {
						command.x
							+ (source.x + source.z * 0.65f) * scale,
						command.y
							+ (source.y - source.z * 0.30f) * scale,
						1.0f - source.z * 0.1f,
						facet_colors[triangle],
						0.5f,
						0.5f,
					};
				}
			}
			FrameVertexBuffer cursor_vertices;
			if (get_available_frame_vertices(renderer.frame_geometry,
					30, renderer.layout) >= 30)
			{
				alloc_frame_vertex_buffer(renderer.frame_geometry,
					&cursor_vertices, 30, renderer.layout);
				std::memcpy(
					cursor_vertices.data, vertices, sizeof(vertices));
				const glm::mat4 identity{1.0f};
				const float full_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};
				const float cursor_tint[] = {
					std::min(brightness, 1.0f),
					std::min(brightness, 1.0f),
					std::min(brightness, 1.0f),
					1.0f,
				};
				bgfx::setTransform(glm::value_ptr(identity));
				bgfx::setUniform(renderer.uv_rect_uniform, full_uv);
				bgfx::setUniform(renderer.tint_uniform, cursor_tint);
				set_frame_vertex_buffer(0, &cursor_vertices);
				bgfx::setTexture(
					0, renderer.texture_sampler, renderer.white.handle);
				bgfx::setState(
					BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_WRITE_Z
					| BGFX_STATE_BLEND_ALPHA
					| BGFX_STATE_DEPTH_TEST_LESS);
				bgfx::submit(frontend_view, renderer.rgba_program);
			}
			continue;
		}
		if (command.type == FrontendCommandType::scissor)
		{
			bgfx::setScissor(
				static_cast<std::uint16_t>(viewport_x + command.x * scale_x),
				static_cast<std::uint16_t>(viewport_y + command.y * scale_y),
				static_cast<std::uint16_t>(command.width * scale_x),
				static_cast<std::uint16_t>(command.height * scale_y));
			continue;
		}

		const glm::mat4 transform = sl_open::math::srt(
			{command.width, command.height, 1.0f},
			{0.0f, 0.0f, command.rotation},
			{command.x, command.y, 0.0f});
		const float uv_rect[] = {
			command.u, command.v, command.uv_width, command.uv_height};
		const float tint[] = {
			std::min(
				static_cast<float>((command.rgba >> 24) & 0xff)
					/ 255.0f * brightness,
				1.0f),
			std::min(
				static_cast<float>((command.rgba >> 16) & 0xff)
					/ 255.0f * brightness,
				1.0f),
			std::min(
				static_cast<float>((command.rgba >> 8) & 0xff)
					/ 255.0f * brightness,
				1.0f),
			static_cast<float>(command.rgba & 0xff) / 255.0f,
		};
		bgfx::setTransform(glm::value_ptr(transform));
		bgfx::setUniform(renderer.uv_rect_uniform, uv_rect);
		bgfx::setUniform(renderer.tint_uniform, tint);
		bgfx::setVertexBuffer(0, renderer.quad_vertices);
		bgfx::setIndexBuffer(renderer.quad_indices);
		bgfx::setTexture(0, renderer.texture_sampler, command.texture);
		if (command.type == FrontendCommandType::indexed_quad)
		{
			bgfx::setTexture(1, renderer.palette_sampler, command.palette);
		}
		const std::uint64_t blend =
			command.type == FrontendCommandType::rgba_additive_quad
				? BGFX_STATE_BLEND_ADD
				: BGFX_STATE_BLEND_ALPHA;
		bgfx::setState(
			BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| blend);
		bgfx::submit(
			frontend_view,
			(command.type == FrontendCommandType::rgba_quad
				|| command.type
					== FrontendCommandType::rgba_additive_quad)
				? renderer.rgba_program
				: renderer.indexed_program);
	}
	bgfx::setScissor();
	bgfx::frame();
}

bool frontend_map_input(
	std::uint32_t backbuffer_width,
	std::uint32_t backbuffer_height,
	float physical_x,
	float physical_y,
	float& logical_x,
	float& logical_y)
{
	float scale = 1.0f;
	std::uint16_t viewport_x = 0;
	std::uint16_t viewport_y = 0;
	std::uint16_t viewport_width = 0;
	std::uint16_t viewport_height = 0;
	canvas_viewport(
		backbuffer_width,
		backbuffer_height,
		scale,
		viewport_x,
		viewport_y,
		viewport_width,
		viewport_height);
	logical_x = (physical_x - viewport_x) / scale;
	logical_y = (physical_y - viewport_y) / scale;
	logical_x = std::clamp(logical_x, 0.0f, kFrontendWidth - 1.0f);
	logical_y = std::clamp(logical_y, 0.0f, kFrontendHeight - 1.0f);
	return true;
}
}
