// MonotonicTime.cpp - Monotonic time implementation
// Ported from UE5.7

#include "OloEngine/Core/MonotonicTime.h"
#include "OloEngine/Core/PlatformTime.h"

namespace OloEngine
{

    FMonotonicTimePoint FMonotonicTimePoint::Now()
    {
        return FromSeconds(FPlatformTime::Seconds());
    }

} // namespace OloEngine
