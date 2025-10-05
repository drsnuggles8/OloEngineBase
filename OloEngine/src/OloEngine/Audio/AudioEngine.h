#pragma once

struct ma_engine;

namespace OloEngine
{
	using AudioEngineInternal = void*;

	class AudioEngine
	{
	public:
		static bool Init();
		static void Shutdown();

		static AudioEngineInternal GetEngine();

	private:
		static ma_engine* s_Engine;
	};
}
