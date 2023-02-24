#pragma once

#include <chrono>

namespace OloEngine
{
	class Timer
	{
	public:
		Timer()
		{
			Reset();
		}

		void Reset()
		{
			m_Start = std::chrono::high_resolution_clock::now();
		}

		[[nodiscard("Store this!")]] f32 Elapsed() const
		{
			return static_cast<f32>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - this->m_Start).count()) * 0.001f * 0.001f * 0.001f;
		}

		[[nodiscard("Store this!")]] f32 ElapsedMillis() const
		{
			return Elapsed() * 1000.0f;
		}

	private:
		std::chrono::time_point<std::chrono::high_resolution_clock> m_Start;
	};
}
