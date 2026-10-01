#include "mission/executor.hpp"

#include "core/mission_log.hpp"
#include "io/endian.hpp"
#include "mission/events.hpp"
#include "mission/executor_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace sl_open::mission
{
namespace
{
constexpr std::uint8_t kCommandArgumentCounts[95] = {
	2,4,1,1,1,1,1,0,3,0,1,4,1,2,2,3,5,2,3,3,
	1,1,1,0,3,3,2,4,2,2,0,1,1,2,2,2,0,2,3,1,
	3,3,3,2,2,0,1,2,2,2,0,2,2,2,2,0,0,1,1,2,
	1,2,2,0,1,1,2,2,3,1,2,0,2,2,2,4,1,0,2,4,
	0,0,5,1,2,2,1,0,2,1,1,2,2,2,2,
};

constexpr bool invalid_opcode(std::uint8_t opcode)
{
	return opcode < 2 || (opcode >= 8 && opcode <= 19) || opcode == 80
		|| opcode >= 86;
}

std::uint16_t read_be16(const std::uint8_t* source)
{
	return static_cast<std::uint16_t>(
		static_cast<std::uint16_t>(source[0]) << 8 | source[1]);
}

void initialize_context_block(
	ExecutorContext& context,
	const DteFile& file,
	std::uint32_t block);

bool push(ExecutorContext& context, ExecutorValue value)
{
	if (context.stack_count >= game::kExecutorStackCells)
	{
		return false;
	}
	context.stack[context.stack_count++] = value;
	return true;
}

bool pop(ExecutorContext& context, ExecutorValue& value)
{
	if (context.stack_count == 0)
	{
		return false;
	}
	value = context.stack[--context.stack_count];
	return true;
}

ExecutorValue decode_stored_value(std::uint32_t value)
{
	// Mission-event producers encode retail object pointers as ffKKIIII,
	// where KK is the DTE reference kind and IIII its stable index. Decoding
	// on VM entry preserves pointer equality and makes event operands valid
	// inputs to the retail group/set membership opcodes.
	if (value == UINT32_MAX)
	{
		return {UINT32_MAX, ValueKind::null_value};
	}
	if ((value & 0xff000000u) != 0xff000000u)
	{
		return {value, ValueKind::scalar};
	}
	switch (static_cast<std::uint8_t>(value >> 16))
	{
	case 0:
		return {
			static_cast<std::uint16_t>(value),
			ValueKind::object};
	case 1:
		return {
			static_cast<std::uint16_t>(value),
			ValueKind::group};
	case 0x16:
		return {
			static_cast<std::uint16_t>(value),
			ValueKind::reference_set};
	default:
		return {value, ValueKind::scalar};
	}
}

std::uint32_t ftol_low_dword(long double value)
{
	// MSVC's helper at 0x004cf28c switches x87 to truncate mode, FISTPs a
	// signed qword, and returns EAX. Invalid/out-of-range conversion produces
	// the x87 indefinite integer 0x8000000000000000, whose low dword is zero.
	constexpr long double kInt64Minimum = -9223372036854775808.0L;
	constexpr long double kInt64Limit = 9223372036854775808.0L;
	if (!std::isfinite(value)
		|| value < kInt64Minimum
		|| value >= kInt64Limit)
	{
		return 0;
	}
	return static_cast<std::uint32_t>(
		static_cast<std::uint64_t>(
			static_cast<std::int64_t>(value)));
}

ExecutorContext* allocate_context(
	Executor& executor,
	std::uint8_t& context_index)
{
	for (std::uint8_t index = 0;
		index < game::kMaxExecutorContexts;
		++index)
	{
		ExecutorContext& context = executor.contexts[index];
		if (context.active)
		{
			continue;
		}
		// ExecutorVM_allocate_context only resets the operand pointer.
		// Script activation owns the remaining fields; last_result and
		// event payload deliberately survive physical-slot reuse.
		context.stack_count = 0;
		context_index = index;
		return &context;
	}
	return nullptr;
}

bool activate_context(
	Executor& executor,
	ExecutorContext& context,
	std::uint32_t block,
	const DteFile& file,
	std::uint8_t script_identity)
{
	// Retail rejects the transition from 31 to 32 active contexts, leaving
	// the final physical slot available to trigger-condition scratch work.
	if (executor.active_contexts >= game::kMaxExecutorContexts - 1)
	{
		if (!executor.context_overflow_logged)
		{
			executor.context_overflow_logged = true;
			diagnostics::mission_log(
				"executor context pool saturated active=%u capacity=%zu",
				static_cast<unsigned>(executor.active_contexts),
				game::kMaxExecutorContexts - 1);
		}
		return false;
	}
	context.active = true;
	context.owner_frame = 0;
	context.wait_until_tick = 0;
	context.call_depth = 0;
	context.suspended = false;
	context.dispatch_tag = UINT8_MAX;
	context.script_identity = script_identity;
	initialize_context_block(context, file, block);
	++executor.active_contexts;
	executor.context_high_water =
		std::max(executor.context_high_water, executor.active_contexts);
	return true;
}

bool function_block(
	const DteFile& file,
	std::uint8_t table,
	std::uint32_t function,
	std::uint32_t& block,
	std::uint8_t& argument_count)
{
	const std::size_t record_section = table == 0 ? 8 : 17;
	const std::size_t code_section = table == 0 ? 6 : 18;
	const DteSection& functions = file.sections[record_section];
	const DteSection& words = file.sections[code_section];
	if (function >= functions.count)
	{
		return false;
	}
	const std::uint8_t* record =
		dte_section_data(file, record_section)
		+ static_cast<std::size_t>(function) * 0x1c;
	const std::uint16_t word = io::read_le16(record + 0x0a);
	if (word == UINT16_MAX)
	{
		return false;
	}
	const std::uint64_t relative =
		static_cast<std::uint64_t>(word) * sizeof(std::uint16_t);
	if (relative + 2 > words.bytes)
	{
		return false;
	}
	block = words.offset + static_cast<std::uint32_t>(relative);
	argument_count = record[0x0d];
	const std::uint16_t locals = io::read_le16(file.image.data + block);
	return locals >= 2 && relative + locals <= words.bytes;
}

bool script_block(
	const DteFile& file,
	std::uint16_t script_word,
	std::uint32_t& block)
{
	const DteSection& words = file.sections[6];
	const std::uint64_t relative =
		static_cast<std::uint64_t>(script_word) * sizeof(std::uint16_t);
	if (relative + 2 > words.bytes)
	{
		return false;
	}
	block = words.offset + static_cast<std::uint32_t>(relative);
	const std::uint16_t locals = io::read_le16(file.image.data + block);
	return locals >= 2 && relative + locals <= words.bytes;
}

void initialize_context_block(
	ExecutorContext& context,
	const DteFile& file,
	std::uint32_t block)
{
	context.pc = block + 2;
	context.local_base =
		block + io::read_le16(file.image.data + block);
}

bool start_function(
	Executor& executor,
	const DteFile& file,
	std::uint8_t table,
	std::uint32_t function,
	const ExecutorValue* arguments = nullptr,
	std::uint8_t supplied_arguments = 0,
	ExecutorContext** started = nullptr)
{
	std::uint32_t block = 0;
	std::uint8_t declared_arguments = 0;
	if (!function_block(
			file, table, function, block, declared_arguments)
		|| declared_arguments != supplied_arguments)
	{
		return false;
	}
	std::uint8_t context_index = UINT8_MAX;
	ExecutorContext* context =
		allocate_context(executor, context_index);
	if (context == nullptr)
	{
		return false;
	}
	for (std::uint8_t index = 0; index < supplied_arguments; ++index)
	{
		if (!push(*context, arguments[index]))
		{
			return false;
		}
	}
	context->function = static_cast<std::uint16_t>(function);
	context->function_table = table;
	if (!activate_context(
		executor, *context, block, file, UINT8_MAX))
	{
		return false;
	}
	if (started != nullptr)
	{
		*started = context;
	}
	return true;
}

bool start_script_word(
	Executor& executor,
	const DteFile& file,
	std::uint16_t script_word,
	std::uint8_t script_identity,
	const std::uint32_t* event_arguments,
	std::uint8_t argument_count,
	ExecutorContext** started = nullptr)
{
	std::uint32_t block = 0;
	if (!script_block(file, script_word, block))
	{
		return false;
	}
	std::uint8_t context_index = UINT8_MAX;
	ExecutorContext* context =
		allocate_context(executor, context_index);
	if (context == nullptr)
	{
		return false;
	}
	if (event_arguments != nullptr)
	{
		std::copy_n(
			event_arguments,
			std::min<std::uint8_t>(argument_count, 5),
			context->event_arguments);
	}
	context->function = UINT8_MAX;
	context->function_table = 0;
	if (!activate_context(
		executor, *context, block, file, script_identity))
	{
		return false;
	}
	if (started != nullptr)
	{
		*started = context;
	}
	return true;
}

void destroy_timers_by_identity(
	Executor& executor,
	std::uint16_t identity)
{
	// ExecutorTimer_tick delegates expiry to DestroyTimer_command
	// (LANCER.EXE 0x0045d290), including its active-record snapshot and
	// function-only invalidation.
	const std::uint16_t snapshot = executor.active_timers;
	std::uint16_t visited = 0;
	for (ExecutorTimer& timer : executor.timers)
	{
		if (timer.function == -1)
		{
			continue;
		}
		if (++visited > snapshot)
		{
			break;
		}
		if (timer.identity != identity)
		{
			continue;
		}
		timer.function = -1;
		--executor.active_timers;
	}
}

std::uint8_t context_index(
	const Executor& executor,
	const ExecutorContext& context)
{
	return static_cast<std::uint8_t>(&context - executor.contexts);
}

void clear_context_watches(
	Executor& executor,
	std::uint8_t owner)
{
	for (ExecutorWatch& watch : executor.watches)
	{
		if (watch.context == owner)
		{
			watch = {};
		}
	}
}

void add_watch(
	Executor& executor,
	const ExecutorContext& context,
	std::uint8_t stack_cell,
	std::uint8_t component)
{
	std::uint16_t active = 0;
	for (ExecutorWatch& watch : executor.watches)
	{
		if (watch.context != UINT8_MAX)
		{
			++active;
			continue;
		}
		watch.context = context_index(executor, context);
		watch.stack_cell = stack_cell;
		watch.component = component;
		executor.watch_high_water =
			std::max<std::uint16_t>(
				executor.watch_high_water,
				static_cast<std::uint16_t>(active + 1));
		return;
	}
	if (!executor.watch_overflow_logged)
	{
		executor.watch_overflow_logged = true;
		diagnostics::mission_log(
			"executor component-watch pool saturated capacity=%zu",
			game::kExecutorWatchPairs);
	}
}

struct LvalueReference
{
	ExecutorValue* frame{};
	std::uint32_t* value{};
	std::uint8_t* kind{};
};

bool resolve_lvalue(
	Executor& executor,
	Runtime& runtime,
	LvalueReference& output)
{
	switch (executor.lvalue_kind)
	{
	case LvalueKind::session:
		if (executor.lvalue_index < std::size(runtime.session_state))
		{
			output.value = &runtime.session_state[executor.lvalue_index];
			output.kind =
				&runtime.session_state_kinds[executor.lvalue_index];
			return true;
		}
		break;
	case LvalueKind::global:
		if (executor.lvalue_index < std::size(runtime.globals))
		{
			output.value = &runtime.globals[executor.lvalue_index];
			output.kind = &runtime.global_kinds[executor.lvalue_index];
			return true;
		}
		break;
	case LvalueKind::frame:
		if (executor.lvalue_context < game::kMaxExecutorContexts)
		{
			ExecutorContext& context =
				executor.contexts[executor.lvalue_context];
			if (executor.lvalue_index < context.stack_count)
			{
				output.frame = &context.stack[executor.lvalue_index];
				return true;
			}
		}
		break;
	case LvalueKind::none:
		break;
	}
	return false;
}

ExecutorValue read_lvalue(const LvalueReference& reference)
{
	if (reference.frame != nullptr)
	{
		return *reference.frame;
	}
	return {
		*reference.value,
		static_cast<ValueKind>(*reference.kind)};
}

void write_lvalue(
	const LvalueReference& reference,
	ExecutorValue value)
{
	value.component = UINT8_MAX;
	if (reference.frame != nullptr)
	{
		*reference.frame = value;
		return;
	}
	*reference.value = value.value;
	*reference.kind = static_cast<std::uint8_t>(value.kind);
}

bool select_lvalue(
	Executor& executor,
	ExecutorContext& context,
	Runtime& runtime,
	LvalueKind kind,
	std::uint16_t index)
{
	ExecutorValue value;
	if (kind == LvalueKind::session)
	{
		if (index >= std::size(runtime.session_state))
		{
			return false;
		}
		value = {
			runtime.session_state[index],
			static_cast<ValueKind>(runtime.session_state_kinds[index])};
	}
	else if (kind == LvalueKind::global)
	{
		if (index >= std::size(runtime.globals))
		{
			return false;
		}
		value = {
			runtime.globals[index],
			static_cast<ValueKind>(runtime.global_kinds[index])};
	}
	else
	{
		const std::uint32_t frame_index = context.owner_frame + index;
		if (frame_index >= context.stack_count)
		{
			return false;
		}
		value = context.stack[frame_index];
		index = static_cast<std::uint16_t>(frame_index);
	}
	if (!push(context, value))
	{
		return false;
	}
	executor.lvalue_kind = kind;
	executor.lvalue_index = index;
	executor.lvalue_context = context_index(executor, context);
	return true;
}

bool values_equal(
	const ExecutorValue& left,
	const ExecutorValue& right)
{
	// Retail compares raw dwords. DTE references are pointers into distinct
	// tables, so equal numeric indices of different reference kinds are not
	// equal. Component watches are out-of-band and do not affect identity.
	if (left.value == UINT32_MAX && right.value == UINT32_MAX)
	{
		return true;
	}
	return left.kind == right.kind && left.value == right.value;
}

bool binary(
	ExecutorContext& context,
	std::uint8_t opcode)
{
	ExecutorValue right;
	ExecutorValue left;
	if (!pop(context, right) || !pop(context, left))
	{
		return false;
	}
	std::uint32_t result = 0;
	switch (opcode)
	{
	case 2: result = values_equal(left, right); break;
	case 3: result = !values_equal(left, right); break;
	case 4:
	case 51: result = left.value > right.value; break;
	case 5:
	case 52: result = left.value >= right.value; break;
	case 6:
	case 53: result = left.value < right.value; break;
	case 7:
	case 54: result = left.value <= right.value; break;
	case 27: result = left.value + right.value; break;
	case 28: result = left.value - right.value; break;
	case 29: result = left.value * right.value; break;
	case 30:
		if (right.value == 0) return false;
		result = left.value / right.value;
		break;
	case 31: result = left.value != 0 && right.value != 0; break;
	case 32: result = left.value != 0 || right.value != 0; break;
	default: return false;
	}
	return push(context, {result});
}

bool object_group_test(
	const Runtime& runtime,
	const ExecutorValue& object,
	const ExecutorValue& group)
{
	return object.kind == ValueKind::object
		&& group.kind == ValueKind::group
		&& object.value < runtime.object_count
		&& runtime.objects[object.value].group == group.value;
}

bool set_contains(
	const Runtime& runtime,
	std::uint16_t set,
	std::uint16_t object,
	std::uint8_t selector)
{
	if (set >= runtime.reference_set_count
		|| object >= runtime.object_count)
	{
		return false;
	}
	ExpandedTargetReference targets[game::kMaxMissionObjects];
	const std::uint16_t count = runtime_expand_target_reference(
		runtime,
		ReferenceKind::set,
		set,
		targets,
		static_cast<std::uint16_t>(std::size(targets)));
	for (std::uint16_t index = 0; index < count; ++index)
	{
		if (targets[index].object == object
			&& (targets[index].model < 0
				? UINT8_MAX
				: static_cast<std::uint8_t>(targets[index].model))
				== selector)
		{
			return true;
		}
	}
	return false;
}

bool enter_function(
	ExecutorContext& context,
	const DteFile& file,
	std::uint8_t table,
	std::uint8_t function)
{
	const std::size_t record_section = table == 0 ? 8 : 17;
	const DteSection& functions = file.sections[record_section];
	if (function >= functions.count)
	{
		return false;
	}
	const std::uint8_t* record =
		dte_section_data(file, record_section)
		+ static_cast<std::size_t>(function) * 0x1c;
	if (io::read_le16(record + 0x0a) == UINT16_MAX)
	{
		// Both nested-call handlers test the compiled block pointer and
		// return the incoming continue flag unchanged when it is null.
		return true;
	}
	std::uint32_t block = 0;
	std::uint8_t argument_count = 0;
	if (!function_block(
			file, table, function, block, argument_count)
		|| context.stack_count
			> game::kExecutorStackCells - 4
		|| context.stack_count < argument_count)
	{
		return false;
	}
	const std::uint8_t frame =
		static_cast<std::uint8_t>(
			context.stack_count - argument_count);
	if (!push(context, {argument_count})
		|| !push(context, {context.pc})
		|| !push(context, {context.owner_frame})
		|| !push(context, {context.local_base}))
	{
		return false;
	}
	context.owner_frame = frame;
	context.function = function;
	context.function_table = table;
	initialize_context_block(context, file, block);
	++context.call_depth;
	// The selected lvalue is process-global VM scratch at 0x00537408.
	// Nested entry (0x0045bfa0/0x0045c110) does not clear it, so a caller
	// may select a cell, invoke a function, and assign through it afterward.
	return true;
}

bool spawn_function(
	ExecutorContext& context,
	Executor& executor,
	const DteFile& file,
	std::uint8_t table,
	std::uint8_t function)
{
	const std::size_t record_section = table == 0 ? 8 : 17;
	const DteSection& functions = file.sections[record_section];
	if (function >= functions.count)
	{
		return false;
	}
	const std::uint8_t* record =
		dte_section_data(file, record_section)
		+ static_cast<std::size_t>(function) * 0x1c;
	if (io::read_le16(record + 0x0a) == UINT16_MAX)
	{
		// Spawn handlers also leave the caller's arguments untouched when
		// the derived function pointer is null.
		return true;
	}
	std::uint32_t block = 0;
	std::uint8_t argument_count = 0;
	if (!function_block(
			file, table, function, block, argument_count)
		|| context.stack_count < argument_count)
	{
		return false;
	}
	const std::uint8_t first =
		static_cast<std::uint8_t>(
			context.stack_count - argument_count);
	ExecutorValue arguments[game::kExecutorStackCells];
	std::copy_n(
		context.stack + first, argument_count, arguments);
	context.stack_count = first;
	return start_function(
		executor,
		file,
		table,
		function,
		arguments,
		argument_count);
}

bool return_from_function(
	ExecutorContext& context,
	const DteFile& file)
{
	if (context.call_depth == 0)
	{
		context.active = false;
		return true;
	}
	ExecutorValue result;
	ExecutorValue old_locals;
	ExecutorValue old_owner;
	ExecutorValue return_pc;
	ExecutorValue argument_count;
	if (!pop(context, result)
		|| !pop(context, old_locals)
		|| !pop(context, old_owner)
		|| !pop(context, return_pc)
		|| !pop(context, argument_count)
		|| context.stack_count < argument_count.value)
	{
		return false;
	}
	context.last_result = result.value;
	context.last_result_kind = result.kind;
	context.stack_count = static_cast<std::uint8_t>(
		context.stack_count - argument_count.value);
	context.local_base = old_locals.value;
	context.owner_frame = old_owner.value;
	context.pc = return_pc.value;
	const DteSection& primary = file.sections[6];
	const DteSection& secondary = file.sections[18];
	context.function_table =
		context.pc >= secondary.offset
			&& context.pc < secondary.offset + secondary.bytes
		? 1
		: context.pc >= primary.offset
				&& context.pc < primary.offset + primary.bytes
			? 0
			: context.function_table;
	--context.call_depth;
	// ExecutorVM_return (0x0045c6e0) restores the frame and publishes the
	// frame-changed byte, but deliberately leaves global lvalue scratch
	// untouched.
	return true;
}

bool run_context(
	ExecutorContext& context,
	Executor& executor,
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	std::uint32_t script_tick,
	std::uint32_t simulation_tick)
{
	if (context.wait_until_tick != 0)
	{
		// ExecutorContext_run, LANCER.EXE 0x0045ba30, treats the stored
		// deadline as inclusive: execution remains suspended while the
		// current unsigned script tick is <= the deadline and resumes only
		// after it. A zero deadline deliberately bypasses this gate.
		if (context.wait_until_tick >= script_tick)
		{
			return true;
		}
		context.wait_until_tick = 0;
	}
	while (true)
	{
		const DteSection& code =
			file.sections[context.function_table == 0 ? 6 : 18];
		const std::uint32_t code_end = code.offset + code.bytes;
		if (context.pc >= code_end)
		{
			executor.error_pc = context.pc;
			return false;
		}
		const std::uint32_t opcode_pc = context.pc;
		const std::uint8_t opcode = file.image.data[context.pc++];
		if (invalid_opcode(opcode))
		{
			executor.error_pc = opcode_pc;
			executor.error_opcode = opcode;
			return false;
		}
		ExecutorValue left;
		ExecutorValue right;
		switch (opcode)
		{
		case 2: case 3: case 4: case 5: case 6: case 7:
		case 27: case 28: case 29: case 30: case 31: case 32:
		case 51: case 52: case 53: case 54:
			if (!binary(context, opcode)) return false;
			break;
		case 20:
		case 21:
			if (!pop(context, right) || !pop(context, left)
				|| !push(context, {
					object_group_test(runtime, left, right)
						== (opcode == 20) ? 1u : 0u}))
			{
				return false;
			}
			break;
		case 22: case 23: case 24: case 25: case 26:
		{
			if (!pop(context, right) || !pop(context, left))
			{
				return false;
			}
			LvalueReference destination;
			if (!resolve_lvalue(executor, runtime, destination))
			{
				return false;
			}
			ExecutorValue stored = read_lvalue(destination);
			switch (opcode)
			{
			case 22:
				stored = right;
				break;
			case 23:
				stored.value += right.value;
				stored.kind = ValueKind::scalar;
				break;
			case 24:
				stored.value -= right.value;
				stored.kind = ValueKind::scalar;
				break;
			case 25:
				stored.value *= right.value;
				stored.kind = ValueKind::scalar;
				break;
			case 26:
				if (right.value == 0) return false;
				stored.value /= right.value;
				stored.kind = ValueKind::scalar;
				break;
			}
			write_lvalue(destination, stored);
			break;
		}
		case 33:
		{
			if (context.pc >= code_end) return false;
			const std::uint8_t command = file.image.data[context.pc++];
			if (command >= std::size(kCommandArgumentCounts))
			{
				return false;
			}
			const std::uint8_t count = kCommandArgumentCounts[command];
			const DteSection& policies = file.sections[24];
			if (static_cast<std::size_t>(command) * 2
					>= policies.bytes
				|| context.stack_count < count)
			{
				return false;
			}
			// ExecutorVM_call_executor_command (0x0045bea0) publishes
			// !(section24[command] & 1) before invoking the callback.
			// Group/reference-set visitors consult this retained policy to
			// omit the human-player live-object prefix where authored.
			runtime.reference_skip_player_prefix =
				(dte_section_data(file, 24)[command * 2] & 1) == 0;
			ExecutorValue* arguments =
				context.stack + context.stack_count - count;
			// Retail publishes the pointer to the consumed cells, then moves
			// SP below them before entering the callback. Wait callbacks that
			// rewind four bytes consequently replay the one-cell operand.
			context.stack_count = static_cast<std::uint8_t>(
				context.stack_count - count);
			const CommandFlow flow = executor_execute_command(
				command,
				arguments,
				context,
				executor,
				runtime,
				world,
				stats,
				file,
				script_tick,
				simulation_tick);
			for (ExecutorWatch& watch : executor.watches)
			{
				watch = {};
			}
			if (command != 30 && command != 63 && command != 69)
			{
				context.last_result =
					flow == CommandFlow::continue_execution ? 1u : 0u;
			}
			// Every compiled Executor callback returns a scalar status/result.
			// Bytecode-function returns retain their reference identity in
			// return_from_function instead.
			context.last_result_kind = ValueKind::scalar;
			if (flow == CommandFlow::rewind_two_and_yield)
			{
				context.pc -= 2;
				return true;
			}
			if (flow == CommandFlow::rewind_four_and_yield)
			{
				context.pc -= 4;
				return true;
			}
			if (flow == CommandFlow::yield)
			{
				return true;
			}
			break;
		}
		case 34:
		case 74:
			if (context.pc >= code_end
				|| !enter_function(
					context,
					file,
					opcode == 34 ? 0 : 1,
					file.image.data[context.pc++]))
			{
				return false;
			}
			break;
		case 35:
		case 36:
		{
			if (context.pc + 2 > code_end || !pop(context, left))
			{
				return false;
			}
			const std::uint32_t operand = context.pc;
			const std::uint16_t displacement =
				read_be16(file.image.data + operand);
			context.pc =
				left.value == 0
					? operand + displacement
					: operand + 2;
			break;
		}
		case 37:
		case 67:
			if (!return_from_function(context, file))
			{
				return false;
			}
			if (!context.active) return true;
			break;
		case 38:
		{
			if (context.pc >= code_end) return false;
			const std::uint8_t index = file.image.data[context.pc++];
			if (index >= std::size(runtime.session_state)
				|| !push(context, {
					runtime.session_state[index],
					static_cast<ValueKind>(
						runtime.session_state_kinds[index])}))
			{
				return false;
			}
			break;
		}
		case 39:
		{
			if (context.pc >= code_end) return false;
			const std::uint8_t index = file.image.data[context.pc++];
			if (index >= std::size(runtime.globals)
				|| !push(context, {
					runtime.globals[index],
					static_cast<ValueKind>(
						runtime.global_kinds[index])}))
			{
				return false;
			}
			break;
		}
		case 40:
		case 41:
		{
			const std::uint16_t index =
				opcode == 40
					? (context.pc < code_end
						? file.image.data[context.pc++]
						: UINT16_MAX)
					: (context.pc + 2 <= code_end
						? read_be16(
							file.image.data
							+ std::exchange(
								context.pc, context.pc + 2))
						: UINT16_MAX);
			const std::uint64_t address =
				static_cast<std::uint64_t>(context.local_base)
				+ static_cast<std::uint64_t>(index) * 4;
			if (index == UINT16_MAX || address + 4 > file.image.size
				|| !push(
					context,
					decode_stored_value(
						io::read_le32(file.image.data + address))))
			{
				return false;
			}
			break;
		}
		case 42:
		case 43:
		{
			if (context.pc >= code_end) return false;
			const std::uint8_t bytes = file.image.data[context.pc];
			if (bytes == 0 || context.pc + bytes > code_end
				|| !push(context, {
					context.pc + 1, ValueKind::string}))
			{
				return false;
			}
			context.pc += bytes;
			break;
		}
		case 44:
		case 45:
		case 46:
		case 50:
		case 68:
		case 73:
		case 84:
		{
			if (context.pc >= code_end) return false;
			const std::uint8_t value = file.image.data[context.pc++];
			ValueKind kind = ValueKind::scalar;
			if (opcode == 44) kind = ValueKind::object;
			else if (opcode == 45) kind = ValueKind::group;
			else if (opcode == 68) kind = ValueKind::reference_set;
			else if (opcode == 73) kind = ValueKind::curve;
			else if (opcode == 84) kind = ValueKind::section19;
			if (!push(context, {value, kind})) return false;
			break;
		}
		case 47:
		{
			if (context.pc >= code_end || context.stack_count == 0)
			{
				return false;
			}
			const std::uint8_t scale = file.image.data[context.pc++];
			const std::uint32_t source =
				context.stack[context.stack_count - 1].value;
			if (!push(context, {
				ftol_low_dword(
					static_cast<long double>(
						source)
					* static_cast<long double>(scale)
					* static_cast<long double>(0.01f))}))
			{
				return false;
			}
			break;
		}
		case 48:
		{
			if (context.pc >= code_end) return false;
			const std::uint8_t index = file.image.data[context.pc++];
			if (index >= std::size(context.event_arguments)
				|| !push(
					context,
					decode_stored_value(
						context.event_arguments[index])))
			{
				return false;
			}
			break;
		}
		case 49:
		{
			if (context.pc >= code_end) return false;
			const std::uint32_t index =
				context.owner_frame + file.image.data[context.pc++];
			if (index >= context.stack_count
				|| !push(context, context.stack[index]))
			{
				return false;
			}
			break;
		}
		case 55: case 56: case 57: case 58:
		{
			if (!pop(context, right) || !pop(context, left))
			{
				return false;
			}
			LvalueReference destination;
			if (!resolve_lvalue(executor, runtime, destination))
			{
				return false;
			}
			ExecutorValue stored = read_lvalue(destination);
			float stored_float = 0.0f;
			std::memcpy(
				&stored_float, &stored.value, sizeof(stored_float));
			long double value =
				static_cast<long double>(stored_float);
			const long double operand =
				static_cast<long double>(right.value);
			if (opcode == 55) value += operand;
			else if (opcode == 56) value -= operand;
			else if (opcode == 57) value *= operand;
			else value /= operand;
			stored_float = static_cast<float>(value);
			std::memcpy(
				&stored.value, &stored_float, sizeof(stored.value));
			stored.kind = ValueKind::scalar;
			write_lvalue(destination, stored);
			break;
		}
		case 59: case 60: case 61: case 62:
		{
			if (!pop(context, right) || !pop(context, left))
			{
				return false;
			}
			// These handlers are asymmetric in retail: one operand is
			// zero-extended and loaded with FILD qword, while the other is
			// consumed by a signed FI* dword instruction.
			const long double lhs_unsigned =
				static_cast<long double>(left.value);
			const long double rhs_unsigned =
				static_cast<long double>(right.value);
			const long double lhs_signed =
				static_cast<long double>(
					static_cast<std::int32_t>(left.value));
			const long double rhs_signed =
				static_cast<long double>(
					static_cast<std::int32_t>(right.value));
			long double result = 0.0L;
			if (opcode == 59)
			{
				result = rhs_unsigned + lhs_signed;
			}
			else if (opcode == 60)
			{
				result = lhs_unsigned - rhs_signed;
			}
			else if (opcode == 61)
			{
				result = rhs_unsigned * lhs_signed;
			}
			else
			{
				result = lhs_unsigned / rhs_signed;
			}
			if (!push(context, {ftol_low_dword(result)}))
			{
				return false;
			}
			break;
		}
		case 63:
		case 64:
		case 65:
		{
			if (context.pc >= code_end) return false;
			const std::uint8_t index = file.image.data[context.pc++];
			if (!select_lvalue(
				executor,
				context,
				runtime,
				opcode == 63 ? LvalueKind::session
					: opcode == 64 ? LvalueKind::global
					: LvalueKind::frame,
				index))
			{
				return false;
			}
			break;
		}
		case 66:
		{
			if (context.pc + 2 > code_end) return false;
			const std::uint32_t operand = context.pc;
			context.pc =
				operand + read_be16(file.image.data + operand);
			break;
		}
		case 69:
		case 70:
		{
			if (!pop(context, right) || !pop(context, left))
			{
				return false;
			}
			const bool contained =
				right.kind == ValueKind::reference_set
				&& left.kind == ValueKind::object
				&& set_contains(
					runtime,
					static_cast<std::uint16_t>(right.value),
					static_cast<std::uint16_t>(left.value),
					UINT8_MAX);
			if (!push(context, {
				contained == (opcode == 69) ? 1u : 0u}))
			{
				return false;
			}
			break;
		}
		case 71:
		case 85:
		{
			if (context.pc + 2 > code_end) return false;
			const std::uint8_t object = file.image.data[context.pc++];
			const std::uint8_t component = file.image.data[context.pc++];
			const std::uint8_t cell = context.stack_count;
			if (!push(context, {
				object, ValueKind::object, component}))
			{
				return false;
			}
			add_watch(executor, context, cell, component);
			break;
		}
		case 72:
			if (!push(context, {
				UINT32_MAX, ValueKind::null_value}))
			{
				return false;
			}
			break;
		case 75:
		{
			if (context.pc + 3 > code_end) return false;
			const std::uint8_t event = file.image.data[context.pc++];
			const std::uint8_t field = file.image.data[context.pc++];
			const std::uint8_t subject_span =
				file.image.data[context.pc++];
			// Opcode 75 indexes the 0x1c-byte compiled event descriptor
			// first, then reads its derived +0x0c capture-slot byte. Only
			// ShotAt and Destroyed own the two retained five-dword slots.
			const std::uint8_t cache_slot = events_capture_slot(
				static_cast<EventType>(event));
			if (cache_slot == UINT8_MAX
				|| subject_span >= runtime.reference_span_count
				|| field >= 5
				|| !push(
					context,
					decode_stored_value(
						runtime.event_captures[
							subject_span][cache_slot][field])))
			{
				return false;
			}
			break;
		}
		case 76:
			if (!push(context, {
					context.last_result,
					context.last_result_kind}))
			{
				return false;
			}
			break;
		case 77:
		case 78:
			if (context.pc >= code_end
				|| !spawn_function(
					context,
					executor,
					file,
					opcode == 77 ? 0 : 1,
					file.image.data[context.pc++]))
			{
				return false;
			}
			break;
		case 79:
		{
			if (context.pc >= code_end) return false;
			const std::uint8_t descriptor =
				file.image.data[context.pc++];
			const std::uint8_t argument_count =
				events_argument_count(
					static_cast<EventType>(descriptor));
			const DteSection& policies = file.sections[25];
			if (descriptor >= kEventTypeCount
				|| static_cast<std::size_t>(descriptor) * 2
					>= policies.bytes
				|| argument_count == UINT8_MAX
				|| context.stack_count < argument_count)
			{
				return false;
			}
			// ExecutorVM_call_section7_descriptor (0x0045bf20) reads the
			// descriptor's complete 0x74-byte argument count and publishes
			// !(section25[descriptor] & 1) at 0x00537584 before invoking
			// the retained target callback. That callback returns one, but
			// the prefix policy remains observable by later group/set
			// mission-reference expansion.
			runtime.reference_skip_player_prefix =
				(dte_section_data(file, 25)[descriptor * 2] & 1) == 0;
			context.stack_count = static_cast<std::uint8_t>(
				context.stack_count - argument_count);
			context.last_result = 1;
			context.last_result_kind = ValueKind::scalar;
			for (ExecutorWatch& watch : executor.watches)
			{
				watch = {};
			}
			break;
		}
		case 81:
		{
			if (context.pc + 3 > code_end) return false;
			const std::uint32_t operand = context.pc;
			const std::uint8_t count = file.image.data[context.pc++];
			const std::uint16_t default_displacement =
				read_be16(file.image.data + context.pc);
			context.pc += 2;
			if (context.pc + static_cast<std::uint32_t>(count) * 4
				> code_end)
			{
				return false;
			}
			world.random_seed =
				world.random_seed * 0x343fdu + 0x269ec3u;
			const std::uint8_t choice =
				static_cast<std::uint8_t>(
					((world.random_seed >> 16) & 0x7fffu)
						% 100);
			std::uint16_t displacement = UINT16_MAX;
			for (std::uint8_t index = 0; index < count; ++index)
			{
				const std::uint8_t* record =
					file.image.data + context.pc + index * 4;
				if (displacement == UINT16_MAX
					&& choice < record[2])
				{
					displacement = read_be16(record);
				}
			}
			context.pc += static_cast<std::uint32_t>(count) * 4;
			if (displacement == UINT16_MAX)
			{
				displacement = default_displacement;
			}
			context.pc = operand + displacement - 1;
			break;
		}
		case 82:
		{
			if (context.pc + 2 > code_end) return false;
			const std::uint16_t object =
				read_be16(file.image.data + context.pc);
			context.pc += 2;
			if (!push(context, {object, ValueKind::object}))
			{
				return false;
			}
			break;
		}
		case 83:
			break;
		default:
			executor.error_pc = opcode_pc;
			executor.error_opcode = opcode;
			return false;
		}
	}
}

bool run_started_context(
	Executor& executor,
	ExecutorContext& context,
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	std::uint32_t script_tick,
	std::uint32_t simulation_tick)
{
	const std::uint8_t owner = context_index(executor, context);
	if (!run_context(
		context,
		executor,
		runtime,
		world,
		stats,
		file,
		script_tick,
		simulation_tick))
	{
		return false;
	}
	if (!context.active)
	{
		clear_context_watches(executor, owner);
		--executor.active_contexts;
	}
	return true;
}
}

bool executor_initialize(
	Executor& executor,
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	std::uint32_t tick)
{
	executor = {};
	for (ExecutorTimer& timer : executor.timers)
	{
		timer.function = -1;
	}
	for (ExecutorWatch& watch : executor.watches)
	{
		watch = {};
	}
	const DteSection& globals = file.sections[2];
	const std::uint8_t* global_records = dte_section_data(file, 2);
	for (std::uint16_t index = 0;
		index < globals.count
			&& index < std::size(runtime.globals);
		++index)
	{
		const ExecutorValue initial = decode_stored_value(
			io::read_le32(
				global_records
				+ static_cast<std::size_t>(index) * 0x0c + 4));
		runtime.globals[index] = initial.value;
		runtime.global_kinds[index] =
			static_cast<std::uint8_t>(initial.kind);
	}
	const DteSection& functions = file.sections[8];
	const std::uint8_t* records = dte_section_data(file, 8);
	for (std::uint16_t index = 0; index < functions.count; ++index)
	{
		const std::uint8_t* record =
			records + static_cast<std::size_t>(index) * 0x1c;
		if ((record[0x0c] & 1) == 0
			|| io::read_le16(record + 0x0a) == UINT16_MAX)
		{
			continue;
		}
		ExecutorContext* started = nullptr;
		if (!start_function(
				executor,
				file,
				0,
				index,
				nullptr,
				0,
				&started)
			|| started == nullptr
			|| !run_started_context(
				executor,
				*started,
				runtime,
				world,
					stats,
					file,
					tick,
					tick))
		{
			executor.failed = true;
			return false;
		}
	}
	return !executor.failed;
}

bool executor_start_trigger(
	Executor& executor,
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	std::uint16_t script_word,
	std::uint8_t script_identity,
	const std::uint32_t* event_arguments,
	std::uint8_t argument_count,
	bool deferred,
	std::uint32_t script_tick,
	std::uint32_t simulation_tick)
{
	bool found = false;
	for (ExecutorContext& context : executor.contexts)
	{
		if (!context.active
			|| context.script_identity != script_identity)
		{
			continue;
		}
		context.suspended = false;
		found = true;
	}
	if (found)
	{
		return true;
	}
	ExecutorContext* started = nullptr;
	if (!start_script_word(
		executor,
		file,
		script_word,
		script_identity,
		event_arguments,
		argument_count,
		&started))
	{
		return false;
	}
	if (!deferred && started != nullptr)
	{
		if (!run_started_context(
			executor,
			*started,
			runtime,
			world,
				stats,
				file,
				script_tick,
				simulation_tick))
		{
			executor.failed = true;
			return false;
		}
	}
	return true;
}

void executor_service_timers(
	Executor& executor,
	const DteFile& file,
	std::uint32_t script_tick)
{
	if (executor.failed)
	{
		return;
	}
	const std::uint16_t timer_snapshot = executor.active_timers;
	std::uint16_t timers_visited = 0;
	for (ExecutorTimer& timer : executor.timers)
	{
		if (timer.function == -1)
		{
			continue;
		}
		if (++timers_visited > timer_snapshot)
		{
			break;
		}
		if (timer.last_fire_tick == script_tick)
		{
			continue;
		}
		// ExecutorTimer_tick (LANCER.EXE 0x0045d178) always decrements the
		// 16-bit counter before testing it. An authored interval of zero
		// therefore wraps to 0xffff instead of firing immediately.
		--timer.countdown;
		if (timer.countdown != 0)
		{
			continue;
		}
		timer.last_fire_tick = script_tick;
		const std::uint32_t function =
			static_cast<std::uint32_t>(timer.function);
		if (timer.activations != 0 && --timer.activations == 0)
		{
			// ExecutorTimer_tick delegates expiry to DestroyTimer_command
			// with the record's 16-bit identity, so every matching record is
			// removed rather than only the record currently being scanned.
			const std::uint16_t identity = timer.identity;
			destroy_timers_by_identity(executor, identity);
		}
		else
		{
			timer.countdown = timer.interval;
		}
		// ExecutorTimer_tick calls ExecutorVM_start_script and deliberately
		// ignores its return. A saturated context pool drops this activation;
		// it must not poison every remaining mission script.
		start_function(executor, file, 0, function);
	}
}

void executor_tick(
	Executor& executor,
	Runtime& runtime,
	game::World& world,
	const assets::ShipStatsTable& stats,
	const DteFile& file,
	std::uint32_t script_tick,
	std::uint32_t simulation_tick)
{
	if (executor.failed)
	{
		return;
	}
	const std::uint16_t snapshot = executor.active_contexts;
	std::uint16_t visited = 0;
	for (ExecutorContext& context : executor.contexts)
	{
		if (!context.active)
		{
			continue;
		}
		if (++visited > snapshot)
		{
			break;
		}
		if (context.suspended)
		{
			continue;
		}
		const std::uint8_t owner = context_index(executor, context);
		if (!run_context(
			context,
			executor,
			runtime,
			world,
			stats,
			file,
			script_tick,
			simulation_tick))
		{
			executor.failed = true;
			return;
		}
		if (!context.active)
		{
			clear_context_watches(executor, owner);
			--executor.active_contexts;
		}
	}
}
}
