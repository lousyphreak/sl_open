#pragma once

#include "core/blob.hpp"

#include <cstddef>
#include <cstdint>

namespace sl_open::media
{
struct BinkVideoFrame
{
	const std::uint8_t* planes[3]{};
	std::uint32_t strides[3]{};
	std::uint32_t width{};
	std::uint32_t height{};
};

struct BinkVideoDecoder
{
	sl_open::Blob pixels;
	void* context{};
	BinkVideoFrame frame;
	std::uint32_t frame_index{};
};

bool bink_video_init(
	BinkVideoDecoder& decoder,
	std::uint32_t width,
	std::uint32_t height,
	std::uint32_t tag,
	std::uint32_t flags);
bool bink_video_decode(
	BinkVideoDecoder& decoder,
	const std::uint8_t* packet,
	std::size_t packet_size);
void bink_video_shutdown(BinkVideoDecoder& decoder);
}
