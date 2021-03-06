#pragma once

namespace OloEngine {

	class Timestep
	{
	public:
		//TODO(olbu): Figure out how we can get Timestep to be explicit
		explicit(false) Timestep(const float timeStep = 0.0f)
			: m_Time(timeStep)
		{
		}

		//TODO(olby): figure out how we can get float operator to be explicit
		explicit(false) operator float() const { return m_Time; }

		[[nodiscard("Returns m_Time, you probably wanted some other function!")]] float GetSeconds() const { return m_Time; }
		[[nodiscard("Returns m_Time in milliseconds, you probably wanted some other function!")]] float GetMilliseconds() const { return m_Time * 1000.0f; }
	private:
		float m_Time;
	};

}
