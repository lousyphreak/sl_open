cmake_minimum_required(VERSION 3.25)

project(sl_open VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep build-machine paths out of diagnostics and debug information.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	add_compile_options("-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL/CMakeLists.txt")
	message(FATAL_ERROR "SDL submodule is missing; run: git submodule update --init --recursive")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake/CMakeLists.txt")
	message(FATAL_ERROR "bgfx.cmake submodule is missing; run: git submodule update --init --recursive")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm/CMakeLists.txt")
	message(FATAL_ERROR "GLM submodule is missing; run: git submodule update --init --recursive")
endif()
if(NOT EMSCRIPTEN
	AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openal-soft/CMakeLists.txt")
	message(FATAL_ERROR "OpenAL Soft submodule is missing; run: git submodule update --init --recursive")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dr_libs/dr_mp3.h")
	message(FATAL_ERROR "dr_libs submodule is missing; run: git submodule update --init --recursive")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL_emfs/CMakeLists.txt")
	message(FATAL_ERROR "SDL_emfs submodule is missing; run: git submodule update --init --recursive")
endif()

# SDL is part of the executable on every initial target. Keep its own examples
# and tests out of this small game build.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)
add_subdirectory(third_party/SDL_emfs EXCLUDE_FROM_ALL)

# bgfx.cmake owns the pinned bgfx/bx/bimg revisions. Build shaderc, but none of
# the optional example, test, geometry, texture, or install targets.
if(EMSCRIPTEN)
	set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
else()
	set(BGFX_BUILD_TOOLS ON CACHE BOOL "" FORCE)
endif()
set(BGFX_BUILD_TOOLS_BIN2C OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_SHADER ON CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_GEOMETRY OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_TEXTURE OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
set(BGFX_CUSTOM_TARGETS OFF CACHE BOOL "" FORCE)
set(BGFX_CONFIG_MULTITHREADED OFF CACHE BOOL "" FORCE)
set(BGFX_CONFIG_VIDEO OFF CACHE BOOL "" FORCE)
set(BGFX_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
add_subdirectory(third_party/bgfx.cmake EXCLUDE_FROM_ALL)

# GLM is the common runtime vector and matrix representation. Keep its
# optional compiled library, tests, and install rules out of the game build.
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glm EXCLUDE_FROM_ALL)

if(NOT EMSCRIPTEN)
	# The native targets embed the approved OpenAL Soft implementation. The
	# browser links Emscripten's OpenAL compatibility implementation instead.
	set(LIBTYPE STATIC CACHE STRING "" FORCE)
	set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
	set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
	set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
	set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
	set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
	set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
	set(ALSOFT_INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(ALSOFT_INSTALL_UTILS OFF CACHE BOOL "" FORCE)
	set(ALSOFT_UPDATE_BUILD_VERSION OFF CACHE BOOL "" FORCE)
	add_subdirectory(third_party/openal-soft EXCLUDE_FROM_ALL)
endif()

if(EMSCRIPTEN)
	# bgfx.cmake currently carries its x86 SSE flag into Emscripten builds.
	# Emscripten accepts that compatibility path when wasm SIMD is explicit.
	target_compile_options(bx PUBLIC -msimd128)
endif()

include(cmake/sl_open_shaders.cmake)

add_executable(sl_open
	${SL_OPEN_SHADER_HEADERS}
	src/ai/runtime.cpp
	src/ai/scripted_commands.cpp
	src/assets/game_stats.cpp
	src/assets/gameplay_model.cpp
	src/assets/gun_stats.cpp
	src/assets/image.cpp
	src/assets/pilot_stats.cpp
	src/assets/pilot_presentation.cpp
	src/assets/missile_stats.cpp
	src/assets/object_type_catalog.cpp
	src/assets/refpack.cpp
	src/assets/ship_stats.cpp
	src/assets/ship_model.cpp
	src/assets/sro_chunks.cpp
	src/assets/texture_cache.cpp
	src/assets/vfx.cpp
	src/audio/audio.cpp
	src/audio/cb97.cpp
	src/audio/cb97_stream.cpp
	src/audio/fat.cpp
	src/audio/mp3.cpp
	src/audio/music.cpp
	src/audio/sound3d.cpp
	src/audio/wav.cpp
	src/campaign/campaign.cpp
	src/campaign/cinematics.cpp
	src/campaign/mission_metadata.cpp
	src/campaign/multiplayer_progression.cpp
	src/campaign/progression.cpp
	src/config/config.cpp
	src/frontend/campaign_frontend.cpp
	src/frontend/campaign_frontend_render.cpp
	src/frontend/campaign_save_browser.cpp
	src/frontend/campaign_induction.cpp
	src/frontend/campaign_hub.cpp
	src/frontend/cd_player.cpp
	src/frontend/cd_player_render.cpp
	src/frontend/credits.cpp
	src/frontend/gameplay_pause.cpp
	src/frontend/itac_people.cpp
	src/frontend/itac_reports.cpp
	src/frontend/itac_shell.cpp
	src/frontend/itac_render.cpp
	src/frontend/itac_ships.cpp
	src/frontend/loadout.cpp
	src/frontend/loadout_render.cpp
	src/frontend/medal_display.cpp
	src/frontend/main_menu.cpp
	src/frontend/mission_briefing.cpp
	src/frontend/multiplayer_debrief.cpp
	src/frontend/multiplayer_frontend.cpp
	src/frontend/multiplayer_lobby.cpp
	src/frontend/news_report.cpp
	src/frontend/options.cpp
	src/frontend/options_audio.cpp
	src/frontend/options_controls.cpp
	src/frontend/options_video.cpp
	src/frontend/post_mission_choice.cpp
	src/frontend/quit_dialog.cpp
	src/frontend/sim_pod.cpp
	src/game/clock.cpp
	src/game/chaff.cpp
	src/game/collisions.cpp
	src/game/damage.cpp
	src/game/disruption_effects.cpp
	src/game/death_effects.cpp
	src/game/attachments.cpp
	src/game/camera_controls.cpp
	src/game/camera_runtime.cpp
	src/game/exhaust_hazard.cpp
	src/game/mission_session.cpp
	src/game/missiles.cpp
	src/game/model_animation.cpp
	src/game/particle_emitters.cpp
	src/game/player_controls.cpp
	src/game/transition_effects.cpp
	src/game/retained_components.cpp
	src/game/shields.cpp
	src/game/weapons.cpp
	src/game/world.cpp
	src/hud/layout.cpp
	src/hud/movie.cpp
	src/hud/render.cpp
	src/hud/runtime.cpp
	src/input/controls.cpp
	src/input/gameplay_input.cpp
	src/io/vfs.cpp
	src/localization/language.cpp
	src/media/bink.cpp
	src/media/bink_audio.cpp
	src/media/bink_dsp.cpp
	src/media/bink_player.cpp
	src/media/bink_video.cpp
	src/mission/dte.cpp
	src/mission/deathmatch_scenarios.cpp
	src/mission/director.cpp
	src/mission/environment_effects.cpp
	src/mission/player_comms.cpp
	src/mission/events.cpp
	src/mission/executor.cpp
	src/mission/executor_commands.cpp
	src/mission/network_runtime.cpp
	src/mission/objectives.cpp
	src/mission/runtime.cpp
	src/network/multiplayer_transport.cpp
	src/platform/app.cpp
	src/platform/display.cpp
	src/platform/frontend_flow.cpp
	src/platform/frontend_loading.cpp
	src/platform/input_queue.cpp
	src/render/frame_geometry.cpp
	src/render/frontend_renderer.cpp
	src/render/loadout_renderer.cpp
	src/render/mission_renderer.cpp
)

target_link_libraries(
	sl_open PRIVATE SDL3::SDL3-static SDL_emfs::SDL_emfs bgfx bx glm::glm)
if(EMSCRIPTEN)
	target_link_libraries(sl_open PRIVATE openal)
else()
	target_link_libraries(sl_open PRIVATE OpenAL::OpenAL)
endif()
if(WIN32)
	target_link_libraries(sl_open PRIVATE ws2_32)
endif()
target_include_directories(sl_open PRIVATE
	src
	third_party/dr_libs
	"${SL_OPEN_SHADER_OUTPUT_DIR}"
)
if(MSVC)
	target_compile_options(sl_open PRIVATE /W4 /permissive-)
else()
	target_compile_options(sl_open PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT EMSCRIPTEN)
	add_executable(sl_open_inspect EXCLUDE_FROM_ALL
		src/assets/gameplay_model.cpp
		src/assets/gun_stats.cpp
		src/assets/image.cpp
		src/assets/pilot_stats.cpp
		src/assets/missile_stats.cpp
		src/assets/refpack.cpp
		src/assets/ship_stats.cpp
		src/assets/sro_chunks.cpp
		src/assets/texture_cache.cpp
		src/assets/vfx.cpp
		src/io/vfs.cpp
		src/localization/language.cpp
		src/mission/dte.cpp
		src/tools/mission_inspect.cpp
	)
	target_link_libraries(
		sl_open_inspect PRIVATE
		SDL3::SDL3-static SDL_emfs::SDL_emfs glm::glm)
	target_include_directories(sl_open_inspect PRIVATE src)
	if(MSVC)
		target_compile_options(sl_open_inspect PRIVATE /W4 /permissive-)
	else()
		target_compile_options(
			sl_open_inspect PRIVATE -Wall -Wextra -Wpedantic)
	endif()

endif()

set_target_properties(sl_open PROPERTIES
	OUTPUT_NAME sl_open
	VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(EMSCRIPTEN)
	set_target_properties(sl_open PROPERTIES SUFFIX ".html")
	set_property(TARGET sl_open APPEND PROPERTY LINK_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web_shell.html")
	target_link_options(sl_open PRIVATE
		"SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web_shell.html"
		-sASYNCIFY
		-sSTACK_SIZE=4194304
		-sASYNCIFY_STACK_SIZE=4194304
		-sMIN_WEBGL_VERSION=2
		-sMAX_WEBGL_VERSION=2
		-sALLOW_MEMORY_GROWTH=1
		-sEXIT_RUNTIME=1
	)
endif()
