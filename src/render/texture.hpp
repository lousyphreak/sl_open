#pragma once

#include <bgfx/bgfx.h>

#include <cstdint>
#include <utility>

namespace sl_open::render
{
struct FrontendTexture
{
	FrontendTexture() = default;
	FrontendTexture(
		bgfx::TextureHandle texture_handle,
		std::uint16_t texture_width,
		std::uint16_t texture_height,
		std::int16_t texture_offset_x,
		std::int16_t texture_offset_y,
		float texture_u,
		float texture_v,
		float texture_uv_width,
		float texture_uv_height,
		bool texture_owns_handle)
		: handle(texture_handle)
		, width(texture_width)
		, height(texture_height)
		, offset_x(texture_offset_x)
		, offset_y(texture_offset_y)
		, u(texture_u)
		, v(texture_v)
		, uv_width(texture_uv_width)
		, uv_height(texture_uv_height)
		, owns_handle(texture_owns_handle)
	{
	}
	FrontendTexture(const FrontendTexture&) = delete;
	FrontendTexture& operator=(const FrontendTexture&) = delete;
	FrontendTexture(FrontendTexture&& other) noexcept
	{
		*this = static_cast<FrontendTexture&&>(other);
	}
	FrontendTexture& operator=(FrontendTexture&& other) noexcept
	{
		if (this != &other)
		{
			handle = std::exchange(
				other.handle, bgfx::TextureHandle{bgfx::kInvalidHandle});
			stream_back = std::exchange(
				other.stream_back, bgfx::TextureHandle{bgfx::kInvalidHandle});
			width = std::exchange(other.width, 0);
			height = std::exchange(other.height, 0);
			offset_x = std::exchange(other.offset_x, 0);
			offset_y = std::exchange(other.offset_y, 0);
			u = std::exchange(other.u, 0.0f);
			v = std::exchange(other.v, 0.0f);
			uv_width = std::exchange(other.uv_width, 1.0f);
			uv_height = std::exchange(other.uv_height, 1.0f);
			owns_handle = std::exchange(other.owns_handle, false);
		}
		return *this;
	}

	bgfx::TextureHandle handle{bgfx::kInvalidHandle};
	bgfx::TextureHandle stream_back{bgfx::kInvalidHandle};
	std::uint16_t width{};
	std::uint16_t height{};
	std::int16_t offset_x{};
	std::int16_t offset_y{};
	float u{};
	float v{};
	float uv_width{1.0f};
	float uv_height{1.0f};
	bool owns_handle{};
};

}
