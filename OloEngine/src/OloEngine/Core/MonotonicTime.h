// MonotonicTime.h - Monotonic time types for precise timing
// Ported from UE5.7 UE::FMonotonicTimeSpan / UE::FMonotonicTimePoint

#pragma once

#include "OloEngine/Core/Base.h"
#include <cmath>
#include <limits>

namespace OloEngine
{
    /**
     * A span of time measured in seconds between two time points.
     *
     * @see FMonotonicTimePoint
     */
    struct FMonotonicTimeSpan
    {
      public:
        constexpr FMonotonicTimeSpan() = default;

        constexpr static FMonotonicTimeSpan Zero()
        {
            return FMonotonicTimeSpan();
        }

        constexpr static FMonotonicTimeSpan Infinity()
        {
            return FromSeconds(std::numeric_limits<f64>::infinity());
        }

        constexpr static FMonotonicTimeSpan FromSeconds(f64 Seconds)
        {
            FMonotonicTimeSpan TimeSpan;
            TimeSpan.m_Time = Seconds;
            return TimeSpan;
        }

        constexpr static FMonotonicTimeSpan FromMilliseconds(f64 Milliseconds)
        {
            return FromSeconds(Milliseconds * 0.001);
        }

        constexpr f64 ToSeconds() const
        {
            return m_Time;
        }
        constexpr f64 ToMilliseconds() const
        {
            return m_Time * 1000.0;
        }

        constexpr bool IsZero() const
        {
            return *this == Zero();
        }
        constexpr bool IsInfinity() const
        {
            return *this == Infinity();
        }
        bool IsNaN() const
        {
            return std::isnan(m_Time);
        }

        constexpr bool operator==(const FMonotonicTimeSpan Other) const
        {
            return m_Time == Other.m_Time;
        }
        constexpr bool operator!=(const FMonotonicTimeSpan Other) const
        {
            return m_Time != Other.m_Time;
        }
        constexpr bool operator<=(const FMonotonicTimeSpan Other) const
        {
            return m_Time <= Other.m_Time;
        }
        constexpr bool operator<(const FMonotonicTimeSpan Other) const
        {
            return m_Time < Other.m_Time;
        }
        constexpr bool operator>=(const FMonotonicTimeSpan Other) const
        {
            return m_Time >= Other.m_Time;
        }
        constexpr bool operator>(const FMonotonicTimeSpan Other) const
        {
            return m_Time > Other.m_Time;
        }

        constexpr FMonotonicTimeSpan operator+() const
        {
            return *this;
        }
        constexpr FMonotonicTimeSpan operator-() const
        {
            return FromSeconds(-m_Time);
        }

        constexpr FMonotonicTimeSpan operator+(const FMonotonicTimeSpan Span) const
        {
            return FromSeconds(m_Time + Span.m_Time);
        }
        constexpr FMonotonicTimeSpan operator-(const FMonotonicTimeSpan Span) const
        {
            return FromSeconds(m_Time - Span.m_Time);
        }

        constexpr FMonotonicTimeSpan& operator+=(const FMonotonicTimeSpan Span)
        {
            return *this = *this + Span;
        }
        constexpr FMonotonicTimeSpan& operator-=(const FMonotonicTimeSpan Span)
        {
            return *this = *this - Span;
        }

      private:
        f64 m_Time = 0;
    };

    /**
     * A point in time measured in seconds since an arbitrary epoch.
     *
     * This is a monotonic clock which means the current time will never decrease. This time is meant
     * primarily for measuring intervals. The interval between ticks of this clock is constant except
     * for the time that the system is suspended on certain platforms.
     */
    struct FMonotonicTimePoint
    {
      public:
        static FMonotonicTimePoint Now();

        constexpr FMonotonicTimePoint() = default;

        constexpr static FMonotonicTimePoint Infinity()
        {
            return FromSeconds(std::numeric_limits<f64>::infinity());
        }

        /** Construct from seconds since the epoch. */
        constexpr static FMonotonicTimePoint FromSeconds(const f64 Seconds)
        {
            FMonotonicTimePoint TimePoint;
            TimePoint.m_Time = Seconds;
            return TimePoint;
        }

        /** Seconds since the epoch. */
        constexpr f64 ToSeconds() const
        {
            return m_Time;
        }

        constexpr bool IsInfinity() const
        {
            return *this == Infinity();
        }
        bool IsNaN() const
        {
            return std::isnan(m_Time);
        }

        constexpr bool operator==(const FMonotonicTimePoint Other) const
        {
            return m_Time == Other.m_Time;
        }
        constexpr bool operator!=(const FMonotonicTimePoint Other) const
        {
            return m_Time != Other.m_Time;
        }
        constexpr bool operator<=(const FMonotonicTimePoint Other) const
        {
            return m_Time <= Other.m_Time;
        }
        constexpr bool operator<(const FMonotonicTimePoint Other) const
        {
            return m_Time < Other.m_Time;
        }
        constexpr bool operator>=(const FMonotonicTimePoint Other) const
        {
            return m_Time >= Other.m_Time;
        }
        constexpr bool operator>(const FMonotonicTimePoint Other) const
        {
            return m_Time > Other.m_Time;
        }

        constexpr FMonotonicTimePoint operator+(const FMonotonicTimeSpan Span) const
        {
            return FromSeconds(m_Time + Span.ToSeconds());
        }
        constexpr FMonotonicTimePoint operator-(const FMonotonicTimeSpan Span) const
        {
            return FromSeconds(m_Time - Span.ToSeconds());
        }

        constexpr FMonotonicTimeSpan operator-(const FMonotonicTimePoint Point) const
        {
            return FMonotonicTimeSpan::FromSeconds(m_Time - Point.m_Time);
        }

      private:
        f64 m_Time = 0;
    };

} // namespace OloEngine
