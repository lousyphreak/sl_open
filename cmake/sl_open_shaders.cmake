set(SL_OPEN_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders")
set(SL_OPEN_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")

if(EMSCRIPTEN)
	set(
		SL_OPEN_HOST_SHADERC
		"${CMAKE_CURRENT_SOURCE_DIR}/build/linux-debug/third_party/bgfx.cmake/cmake/bgfx/shaderc"
		CACHE FILEPATH
		"Host shaderc executable used while cross-compiling")
	if(NOT EXISTS "${SL_OPEN_HOST_SHADERC}")
		message(FATAL_ERROR
			"Emscripten needs a host shaderc. Build the linux-debug shaderc target "
			"first, or set SL_OPEN_HOST_SHADERC.")
	endif()
	set(SL_OPEN_SHADERC "${SL_OPEN_HOST_SHADERC}")
	set(SL_OPEN_SHADERC_DEPENDENCY "${SL_OPEN_HOST_SHADERC}")
else()
	set(SL_OPEN_SHADERC "$<TARGET_FILE:shaderc>")
	set(SL_OPEN_SHADERC_DEPENDENCY shaderc)
endif()

function(sl_open_compile_shader output_var source type profile platform symbol folder)
	set(output
		"${SL_OPEN_SHADER_OUTPUT_DIR}/${folder}/${source}.bin.h")
	add_custom_command(
		OUTPUT "${output}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"${SL_OPEN_SHADER_OUTPUT_DIR}/${folder}"
		COMMAND
			"${SL_OPEN_SHADERC}"
			-f "${SL_OPEN_SHADER_SOURCE_DIR}/${source}"
			-o "${output}"
			--bin2c "${symbol}"
			--type "${type}"
			--platform "${platform}"
			-p "${profile}"
			--varyingdef "${SL_OPEN_SHADER_SOURCE_DIR}/varying.def.sc"
			-i "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake/bgfx/src"
			--Werror
			--depends
		DEPENDS
			"${SL_OPEN_SHADER_SOURCE_DIR}/${source}"
			"${SL_OPEN_SHADER_SOURCE_DIR}/varying.def.sc"
			${SL_OPEN_SHADERC_DEPENDENCY}
		DEPFILE "${output}.d"
		VERBATIM)
	set(${output_var} "${output}" PARENT_SCOPE)
endfunction()

set(SL_OPEN_SHADER_HEADERS)

set(SL_OPEN_SHADER_STAGES
	vs_frontend
	vs_mission
	vs_model
	vs_model_lit
	vs_model_planet
	fs_rgba
	fs_indexed
	fs_model_rgba
	fs_model_mode_eight
	fs_model_mode_six
	fs_model_mode_seven)

if(NOT EMSCRIPTEN)
	foreach(stage IN LISTS SL_OPEN_SHADER_STAGES)
		if(stage MATCHES "^vs_")
			set(type vertex)
		else()
			set(type fragment)
		endif()
		sl_open_compile_shader(
			header "${stage}.sc" "${type}" 120 linux
			"${stage}_glsl" glsl)
		list(APPEND SL_OPEN_SHADER_HEADERS "${header}")
		sl_open_compile_shader(
			header "${stage}.sc" "${type}" spirv linux
			"${stage}_spirv" spirv)
		list(APPEND SL_OPEN_SHADER_HEADERS "${header}")
	endforeach()
endif()

foreach(stage IN LISTS SL_OPEN_SHADER_STAGES)
	if(stage MATCHES "^vs_")
		set(type vertex)
	else()
		set(type fragment)
	endif()
	sl_open_compile_shader(
		header "${stage}.sc" "${type}" 100_es asm.js
		"${stage}_essl" essl)
	list(APPEND SL_OPEN_SHADER_HEADERS "${header}")
endforeach()

if(WIN32)
	foreach(stage IN LISTS SL_OPEN_SHADER_STAGES)
		if(stage MATCHES "^vs_")
			set(type vertex)
		else()
			set(type fragment)
		endif()
		sl_open_compile_shader(
			header "${stage}.sc" "${type}" s_5_0 windows
			"${stage}_dxbc" dxbc)
		list(APPEND SL_OPEN_SHADER_HEADERS "${header}")
	endforeach()
endif()
