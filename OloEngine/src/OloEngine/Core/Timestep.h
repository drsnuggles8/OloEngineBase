#pragma once

namespace OloEngine
{
	class Timestep
	{
	public:
		// TODO(olbu): Figure out how we can get Timestep to be explicit
		explicit(false) Timestep(const f32 timeStep = 0.0f)
			: m_Time(timeStep)
		{
		}

		// TODO(olbu): figure out how we can get float operator to be explicit
		explicit(false) operator f32() const { return m_Time; }

		[[nodiscard("Store this!")]] f32 GetSeconds() const { return m_Time; }
		[[nodiscard("Store this!")]] f32 GetMilliseconds() const { return m_Time * 1000.0f; }
	private:
		f32 m_Time;
	};
}
