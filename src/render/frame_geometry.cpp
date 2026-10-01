#include "render/frontend_renderer.hpp"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <utility>
#include <vector>

namespace sl_open::render
{
namespace
{
constexpr std::uint32_t kVertexBufferSize = 8u << 20;
constexpr std::uint32_t kIndexBufferSize = 4u << 20;
constexpr std::uint32_t kFrameBufferCount = 2;

template <typename Handle>
struct Slot
{
	Handle handle{bgfx::kInvalidHandle};
	std::vector<std::uint8_t> data;
	std::uint32_t offset{};
};

template <typename Handle>
struct Chunk
{
	Slot<Handle> slots[kFrameBufferCount];
};

struct VertexArena
{
	std::vector<Chunk<bgfx::DynamicVertexBufferHandle>> chunks;
	bgfx::VertexLayout layout;
	std::uint32_t chunk_index{};
	std::uint32_t layout_hash{};
	std::uint16_t stride{};
};

struct IndexArena
{
	std::vector<Chunk<bgfx::DynamicIndexBufferHandle>> chunks;
	std::uint32_t chunk_index{};
	std::uint16_t stride{};
};

template <typename Handle>
void destroy_handle(Handle& handle)
{
	if (bgfx::isValid(handle))
	{
		bgfx::destroy(handle);
	}
	handle = BGFX_INVALID_HANDLE;
}
}

struct FrameGeometryBuffersState
{
	VertexArena vertices[3];
	IndexArena indices[2];
	std::deque<std::uint8_t> uploaded;
	std::uint32_t allocation_count{};
	std::uint32_t vertex_arena_count{};
	std::uint32_t frame{};
};

namespace
{
VertexArena& vertex_arena(
	FrameGeometryBuffersState& state, const bgfx::VertexLayout& layout)
{
	for (std::uint32_t index = 0; index < state.vertex_arena_count; ++index)
	{
		if (state.vertices[index].layout_hash == layout.m_hash)
		{
			return state.vertices[index];
		}
	}
	std::abort();
}

IndexArena& index_arena(FrameGeometryBuffersState& state, bool index32)
{
	return state.indices[index32 ? 1 : 0];
}

std::uint8_t* next_upload_flag(FrameGeometryBuffersState& state)
{
	if (state.allocation_count == state.uploaded.size())
	{
		state.uploaded.push_back(0);
	}
	std::uint8_t* uploaded = &state.uploaded[state.allocation_count++];
	*uploaded = 0;
	return uploaded;
}

bool add_vertex_chunk(VertexArena& arena, std::uint32_t vertex_count)
{
	Chunk<bgfx::DynamicVertexBufferHandle> chunk;
	const std::uint32_t capacity = std::max(
		kVertexBufferSize / arena.stride, vertex_count);
	for (auto& slot : chunk.slots)
	{
		slot.data.resize(static_cast<std::size_t>(capacity) * arena.stride);
		slot.handle = bgfx::createDynamicVertexBuffer(
			capacity, arena.layout);
		if (!bgfx::isValid(slot.handle))
		{
			for (auto& created : chunk.slots)
			{
				destroy_handle(created.handle);
			}
			return false;
		}
	}
	arena.chunks.push_back(std::move(chunk));
	return true;
}

bool add_index_chunk(IndexArena& arena, std::uint32_t index_count)
{
	Chunk<bgfx::DynamicIndexBufferHandle> chunk;
	const std::uint32_t capacity = std::max(
		kIndexBufferSize / arena.stride, index_count);
	for (auto& slot : chunk.slots)
	{
		slot.data.resize(static_cast<std::size_t>(capacity) * arena.stride);
		slot.handle = bgfx::createDynamicIndexBuffer(
			capacity,
			arena.stride == sizeof(std::uint32_t) ? BGFX_BUFFER_INDEX32 : 0);
		if (!bgfx::isValid(slot.handle))
		{
			for (auto& created : chunk.slots)
			{
				destroy_handle(created.handle);
			}
			return false;
		}
	}
	arena.chunks.push_back(std::move(chunk));
	return true;
}

bool reserve_vertices(FrameGeometryBuffersState& state,
	VertexArena& arena, std::uint32_t count)
{
	for (std::size_t index = arena.chunk_index;
		index < arena.chunks.size(); ++index)
	{
		const auto& slot = arena.chunks[index].slots[state.frame];
		if ((slot.data.size() - slot.offset) / arena.stride >= count)
		{
			return true;
		}
	}
	return add_vertex_chunk(arena, count);
}

bool reserve_indices(FrameGeometryBuffersState& state,
	IndexArena& arena, std::uint32_t count)
{
	for (std::size_t index = arena.chunk_index;
		index < arena.chunks.size(); ++index)
	{
		const auto& slot = arena.chunks[index].slots[state.frame];
		if ((slot.data.size() - slot.offset) / arena.stride >= count)
		{
			return true;
		}
	}
	return add_index_chunk(arena, count);
}

bool register_vertex_layout(
	FrameGeometryBuffersState& state, const bgfx::VertexLayout& layout)
{
	for (std::uint32_t index = 0; index < state.vertex_arena_count; ++index)
	{
		if (state.vertices[index].layout_hash == layout.m_hash)
		{
			return true;
		}
	}
	if (state.vertex_arena_count == std::size(state.vertices))
	{
		return false;
	}
	VertexArena& arena = state.vertices[state.vertex_arena_count];
	arena.layout = layout;
	arena.layout_hash = layout.m_hash;
	arena.stride = layout.getStride();
	if (!add_vertex_chunk(arena, 0))
	{
		return false;
	}
	++state.vertex_arena_count;
	return true;
}
}

bool frame_geometry_init(FrameGeometryBuffers& buffers,
	const bgfx::VertexLayout& frontend_layout,
	const bgfx::VertexLayout& model_layout)
{
	frame_geometry_shutdown(buffers);
	auto* state = new FrameGeometryBuffersState;
	buffers.state = state;
	state->uploaded.resize(4096);
	state->indices[0].stride = sizeof(std::uint16_t);
	state->indices[1].stride = sizeof(std::uint32_t);
	if (!add_index_chunk(state->indices[0], 0)
		|| !add_index_chunk(state->indices[1], 0)
		|| !register_vertex_layout(*state, frontend_layout)
		|| !register_vertex_layout(*state, model_layout))
	{
		frame_geometry_shutdown(buffers);
		return false;
	}
	return true;
}

bool frame_geometry_register_layout(
	FrameGeometryBuffers& buffers, const bgfx::VertexLayout& layout)
{
	return register_vertex_layout(*buffers.state, layout);
}

void frame_geometry_shutdown(FrameGeometryBuffers& buffers)
{
	if (buffers.state == nullptr)
	{
		return;
	}
	for (VertexArena& arena : buffers.state->vertices)
	{
		for (auto& chunk : arena.chunks)
		{
			for (auto& slot : chunk.slots)
			{
				destroy_handle(slot.handle);
			}
		}
	}
	for (IndexArena& arena : buffers.state->indices)
	{
		for (auto& chunk : arena.chunks)
		{
			for (auto& slot : chunk.slots)
			{
				destroy_handle(slot.handle);
			}
		}
	}
	delete buffers.state;
	buffers.state = nullptr;
}

void frame_geometry_begin(FrameGeometryBuffers& buffers)
{
	if (buffers.state == nullptr)
	{
		return;
	}
	FrameGeometryBuffersState& state = *buffers.state;
	state.frame = (state.frame + 1) % kFrameBufferCount;
	state.allocation_count = 0;
	for (VertexArena& arena : state.vertices)
	{
		arena.chunk_index = 0;
		for (auto& chunk : arena.chunks)
		{
			chunk.slots[state.frame].offset = 0;
		}
	}
	for (IndexArena& arena : state.indices)
	{
		arena.chunk_index = 0;
		for (auto& chunk : arena.chunks)
		{
			chunk.slots[state.frame].offset = 0;
		}
	}
}

std::uint32_t get_available_frame_vertices(const FrameGeometryBuffers& buffers,
	std::uint32_t vertex_count,
	const bgfx::VertexLayout& layout)
{
	FrameGeometryBuffersState& state = *buffers.state;
	VertexArena& arena = vertex_arena(state, layout);
	return reserve_vertices(state, arena, vertex_count) ? vertex_count : 0;
}

std::uint32_t get_available_frame_indices(const FrameGeometryBuffers& buffers,
	std::uint32_t index_count,
	bool index32)
{
	FrameGeometryBuffersState& state = *buffers.state;
	IndexArena& arena = index_arena(state, index32);
	return reserve_indices(state, arena, index_count) ? index_count : 0;
}

void alloc_frame_vertex_buffer(FrameGeometryBuffers& buffers,
	FrameVertexBuffer* output,
	std::uint32_t vertex_count,
	const bgfx::VertexLayout& layout)
{
	FrameGeometryBuffersState& state = *buffers.state;
	VertexArena& arena = vertex_arena(state, layout);
	for (std::size_t index = arena.chunk_index;
		index < arena.chunks.size(); ++index)
	{
		auto& slot = arena.chunks[index].slots[state.frame];
		if ((slot.data.size() - slot.offset) / arena.stride < vertex_count)
		{
			continue;
		}
		arena.chunk_index = static_cast<std::uint32_t>(index);
		FrameVertexBuffer& buffer = *output;
		buffer.data = slot.data.data() + slot.offset;
		buffer.handle = slot.handle;
		buffer.start_vertex = slot.offset / arena.stride;
		buffer.vertex_count = vertex_count;
		buffer.byte_size = vertex_count * arena.stride;
		buffer.uploaded = next_upload_flag(state);
		slot.offset += buffer.byte_size;
		return;
	}
	std::abort();
}

void alloc_frame_index_buffer(FrameGeometryBuffers& buffers,
	FrameIndexBuffer* output,
	std::uint32_t index_count,
	bool index32)
{
	FrameGeometryBuffersState& state = *buffers.state;
	IndexArena& arena = index_arena(state, index32);
	for (std::size_t index = arena.chunk_index;
		index < arena.chunks.size(); ++index)
	{
		auto& slot = arena.chunks[index].slots[state.frame];
		if ((slot.data.size() - slot.offset) / arena.stride < index_count)
		{
			continue;
		}
		arena.chunk_index = static_cast<std::uint32_t>(index);
		FrameIndexBuffer& buffer = *output;
		buffer.data = slot.data.data() + slot.offset;
		buffer.handle = slot.handle;
		buffer.start_index = slot.offset / arena.stride;
		buffer.index_count = index_count;
		buffer.byte_size = index_count * arena.stride;
		buffer.uploaded = next_upload_flag(state);
		slot.offset += buffer.byte_size;
		return;
	}
	std::abort();
}

void set_frame_vertex_buffer(std::uint8_t stream, FrameVertexBuffer* input)
{
	FrameVertexBuffer& buffer = *input;
	if (!*buffer.uploaded)
	{
		bgfx::update(buffer.handle,
			buffer.start_vertex,
			bgfx::copy(buffer.data, buffer.byte_size));
		*buffer.uploaded = 1;
	}
	bgfx::setVertexBuffer(
		stream, buffer.handle, buffer.start_vertex, buffer.vertex_count);
}

void set_frame_index_buffer(FrameIndexBuffer* input,
	std::uint32_t first_index,
	std::uint32_t index_count)
{
	FrameIndexBuffer& buffer = *input;
	if (!*buffer.uploaded)
	{
		bgfx::update(buffer.handle,
			buffer.start_index,
			bgfx::copy(buffer.data, buffer.byte_size));
		*buffer.uploaded = 1;
	}
	bgfx::setIndexBuffer(buffer.handle,
		buffer.start_index + first_index,
		std::min(index_count, buffer.index_count - first_index));
}
}
