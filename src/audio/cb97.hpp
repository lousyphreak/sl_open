#pragma once

#include <cstddef>
#include <cstdint>

namespace sl_open::audio
{
constexpr std::uint32_t kCb97SampleRate = 22050;
constexpr std::uint32_t kCb97FrameSamples = 432;

struct Cb97Decoder
{
	const std::uint8_t* file{};
	std::size_t file_size{};
	std::size_t byte_position{};
	std::uint32_t reservoir{};
	std::uint32_t reservoir_bits{};
	float amplitudes[64]{};
	float coefficients[12]{};
	float synthesis_history[12]{};
	float overlap[324]{};
	float frame[kCb97FrameSamples]{};
	std::uint32_t total_samples{};
	std::uint32_t sample_position{};
	std::uint32_t frame_position{kCb97FrameSamples};
	std::uint32_t first_coefficient_threshold{};
	std::uint8_t interleaved_mode{};
	bool valid{};
};

bool cb97_init(
	Cb97Decoder& decoder,
	const std::uint8_t* file,
	std::size_t file_size);
std::uint32_t cb97_read(
	Cb97Decoder& decoder,
	std::int16_t* output,
	std::uint32_t sample_count,
	bool loop);
}
