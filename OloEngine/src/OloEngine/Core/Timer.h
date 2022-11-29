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

		[[nodiscard("Store this!")]] float Elapsed() const
		{
			return static_cast<float>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - this->m_Start).count()) * 0.001f * 0.001f * 0.001f;
		}

		[[nodiscard("Store this!")]] float ElapsedMillis() const
		{
			return Elapsed() * 1000.0f;
		}

	private:
		std::chrono::time_point<std::chrono::high_resolution_clock> m_Start;
	};
}
