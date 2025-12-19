#pragma once

namespace OloEngine
{
    class Timestep
    {
      public:
        // Allow implicit conversion from float for convenience (e.g., Timestep ts = 0.016f)
        explicit(false) Timestep(const f32 timeStep = 0.0f)
            : m_Time(timeStep)
        {
        }

        // Allow implicit conversion to float for arithmetic operations
        explicit(false) operator f32() const
        {
            return m_Time;
        }

        [[nodiscard("Store this!")]] f32 GetSeconds() const
        {
            return m_Time;
        }
        [[nodiscard("Store this!")]] f32 GetMilliseconds() const
        {
            return m_Time * 1000.0f;
        }

      private:
        f32 m_Time;
    };
} // namespace OloEngine
