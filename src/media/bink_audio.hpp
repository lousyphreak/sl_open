#pragma once

#include <cstddef>
#include <cstdint>

namespace sl_open::media
{
constexpr std::uint32_t kBinkAudioMaxSamples = 4096;

struct BinkAudioDecoder
{
	const std::uint8_t* packet{};
	std::size_t packet_size{};
	std::uint32_t bit_position{};
	std::uint32_t expected_bytes{};
	std::uint32_t sample_rate{};
	std::uint32_t frame_length{};
	std::uint32_t overlap_length{};
	std::uint32_t block_samples{};
	std::uint32_t bands[26]{};
	float quant_table[96]{};
	float coefficients[kBinkAudioMaxSamples]{};
	float previous[kBinkAudioMaxSamples / 16]{};
	std::uint8_t channels{};
	std::uint8_t band_count{};
	bool first_block{};
	bool packet_active{};
};

bool bink_audio_init(
	BinkAudioDecoder& decoder,
	std::uint32_t sample_rate,
	std::uint8_t channels);
bool bink_audio_packet_begin(
	BinkAudioDecoder& decoder,
	const std::uint8_t* packet,
	std::size_t packet_size);
bool bink_audio_decode_block(
	BinkAudioDecoder& decoder,
	std::int16_t* output,
	std::size_t frame_capacity,
	std::uint32_t& decoded_frames,
	bool& packet_finished);
}
