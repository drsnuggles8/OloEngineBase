#pragma once

namespace OloEngine {

	class Timestep
	{
	public:
		//TODO(olbu): Figure out how we can get Timestep to be explicit
		Timestep(float time = 0.0f)
			: m_Time(time)
		{
		}

		//TODO(olby): figure out how we can get float operator to be explicit
		operator float() const { return m_Time; }

		[[nodiscard]] float GetSeconds() const { return m_Time; }
		[[nodiscard]] float GetMilliseconds() const { return m_Time * 1000.0f; }
	private:
		float m_Time;
	};

}
