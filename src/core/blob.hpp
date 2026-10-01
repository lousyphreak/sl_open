#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace sl_open
{
struct Blob
{
	std::uint8_t* data{};
	std::size_t size{};

	Blob() = default;
	Blob(const Blob&) = delete;
	Blob& operator=(const Blob&) = delete;

	Blob(Blob&& other) noexcept
		: data(std::exchange(other.data, nullptr))
		, size(std::exchange(other.size, 0))
	{
	}

	Blob& operator=(Blob&& other) noexcept
	{
		if (this != &other)
		{
			reset();
			data = std::exchange(other.data, nullptr);
			size = std::exchange(other.size, 0);
		}
		return *this;
	}

	~Blob()
	{
		reset();
	}

	bool allocate(std::size_t byte_count)
	{
		reset();
		if (byte_count == 0)
		{
			return true;
		}
		data = static_cast<std::uint8_t*>(std::malloc(byte_count));
		if (data == nullptr)
		{
			return false;
		}
		size = byte_count;
		return true;
	}

	void reset()
	{
		std::free(data);
		data = nullptr;
		size = 0;
	}
};
}
