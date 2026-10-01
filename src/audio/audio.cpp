#include "audio/audio.hpp"

#if !defined(__EMSCRIPTEN__)
#include <AL/alext.h>
#endif

#include <algorithm>

namespace sl_open::audio
{
namespace
{
void clear_al_error()
{
	while (alGetError() != AL_NO_ERROR)
	{
	}
}

bool make_voice(Voice& voice)
{
	clear_al_error();
	alGenSources(1, &voice.source);
	if (alGetError() != AL_NO_ERROR)
	{
		voice = {};
		return false;
	}

	alGenBuffers(1, &voice.buffer);
	if (alGetError() == AL_NO_ERROR)
	{
		return true;
	}

	alDeleteSources(1, &voice.source);
	voice = {};
	return false;
}

bool make_stream(Stream& stream)
{
	clear_al_error();
	alGenSources(1, &stream.source);
	if (alGetError() != AL_NO_ERROR)
	{
		stream = {};
		return false;
	}

	alGenBuffers(kStreamBufferCount, stream.buffers);
	if (alGetError() == AL_NO_ERROR)
	{
		return true;
	}

	alDeleteSources(1, &stream.source);
	stream = {};
	return false;
}

void destroy_voice(Voice& voice)
{
	if (voice.source != 0)
	{
		alSourceStop(voice.source);
		alSourcei(voice.source, AL_BUFFER, 0);
	}
	if (voice.buffer != 0)
	{
		alDeleteBuffers(1, &voice.buffer);
	}
	if (voice.source != 0)
	{
		alDeleteSources(1, &voice.source);
	}
	voice = {};
}

void destroy_stream(Stream& stream)
{
	if (stream.source != 0)
	{
		alSourceStop(stream.source);
		ALint queued = 0;
		alGetSourcei(stream.source, AL_BUFFERS_QUEUED, &queued);
		while (queued-- > 0)
		{
			ALuint buffer = 0;
			alSourceUnqueueBuffers(stream.source, 1, &buffer);
		}
	}

	for (ALuint& buffer : stream.buffers)
	{
		if (buffer != 0)
		{
			alDeleteBuffers(1, &buffer);
		}
	}
	if (stream.source != 0)
	{
		alDeleteSources(1, &stream.source);
	}
	stream = {};
}
}

bool init(Runtime& runtime)
{
	runtime = {};
	runtime.device = alcOpenDevice(nullptr);
	if (runtime.device == nullptr)
	{
		return false;
	}

	const ALCint attributes[] = {
		ALC_MONO_SOURCES, static_cast<ALCint>(kVoiceCount),
		ALC_STEREO_SOURCES, static_cast<ALCint>(kStreamCount),
		0,
	};
	runtime.context = alcCreateContext(runtime.device, attributes);
	if (runtime.context == nullptr
		|| alcMakeContextCurrent(runtime.context) == ALC_FALSE)
	{
		shutdown(runtime);
		return false;
	}

	alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
	alDopplerFactor(1.0f);
	alSpeedOfSound(343.3f);

#if !defined(__EMSCRIPTEN__)
	runtime.hrtf_supported =
		alcIsExtensionPresent(runtime.device, "ALC_SOFT_HRTF") == ALC_TRUE;
#endif

	for (Voice& voice : runtime.voices)
	{
		if (!make_voice(voice))
		{
			shutdown(runtime);
			return false;
		}
		++runtime.voice_count;
	}
	for (Stream& stream : runtime.streams)
	{
		if (!make_stream(stream))
		{
			shutdown(runtime);
			return false;
		}
		++runtime.stream_count;
	}

	runtime.ready = true;
	return true;
}

void shutdown(Runtime& runtime)
{
	if (runtime.context != nullptr)
	{
		alcMakeContextCurrent(runtime.context);
		for (Stream& stream : runtime.streams)
		{
			destroy_stream(stream);
		}
		for (Voice& voice : runtime.voices)
		{
			destroy_voice(voice);
		}
		alcMakeContextCurrent(nullptr);
		alcDestroyContext(runtime.context);
	}
	if (runtime.device != nullptr)
	{
		alcCloseDevice(runtime.device);
	}
	runtime = {};
}

bool set_spatial_mode(Runtime& runtime, SpatialMode mode)
{
	if (!runtime.ready)
	{
		return false;
	}
	if (mode == SpatialMode::Hrtf && !runtime.hrtf_supported)
	{
		return false;
	}

#if !defined(__EMSCRIPTEN__)
	const ALCint attributes[] = {
		ALC_HRTF_SOFT, mode == SpatialMode::Hrtf ? ALC_TRUE : ALC_FALSE,
		0,
	};
	const auto reset_device = reinterpret_cast<LPALCRESETDEVICESOFT>(
		alcGetProcAddress(runtime.device, "alcResetDeviceSOFT"));
	if (runtime.hrtf_supported
		&& (reset_device == nullptr
			|| reset_device(runtime.device, attributes) == ALC_FALSE))
	{
		return false;
	}
#endif

	runtime.spatial_mode = mode;
	return true;
}

float speech_gain(const Runtime& runtime)
{
	constexpr float scale = 1.0f / (127.0f * 127.0f);
	return runtime.speech_volume * runtime.master_volume * scale;
}

void apply_music_gain(Runtime& runtime, int requested_volume)
{
	if (!runtime.ready)
	{
		return;
	}
	constexpr float scale =
		1.0f / (127.0f * 127.0f * 127.0f);
	alSourcef(
		runtime.streams[0].source,
		AL_GAIN,
		std::clamp(requested_volume, 0, 127)
			* runtime.music_volume * runtime.master_volume * scale);
}

void apply_stream_gains(Runtime& runtime)
{
	if (!runtime.ready)
	{
		return;
	}
	const float scale = 1.0f / (127.0f * 127.0f);
	apply_music_gain(runtime, 127);
	alSourcef(
		runtime.streams[1].source,
		AL_GAIN,
		speech_gain(runtime));
	alSourcef(
		runtime.streams[2].source,
		AL_GAIN,
		static_cast<float>(runtime.master_volume) / 127.0f);
	alSourcef(
		runtime.streams[3].source,
		AL_GAIN,
		runtime.effects_volume * runtime.master_volume * scale);
	alSourcef(
		runtime.streams[4].source,
		AL_GAIN,
		speech_gain(runtime));
}

const char* device_name(const Runtime& runtime)
{
	if (runtime.device == nullptr)
	{
		return "";
	}
	const ALCchar* name = alcGetString(runtime.device, ALC_DEVICE_SPECIFIER);
	return name != nullptr ? name : "";
}
}
