#pragma once

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Renderer/BoundingVolume.h"

namespace OloEngine
{
    // CPU-only input for a layer's group/instance compute cull.
    struct FFoliageCullTable
    {
        TArray<u32> Words;
        BoundingBox LocalBounds;
        u32 GroupCount = 0;
        u32 InstanceCount = 0;
        u64 Generation = ~0ull;
    };

    template<>
    struct TIsTriviallyRelocatable<FFoliageCullTable>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(FFoliageCullTable::Words)> &&
                                      TIsTriviallyRelocatable_V<decltype(FFoliageCullTable::LocalBounds)> &&
                                      TIsTriviallyRelocatable_V<decltype(FFoliageCullTable::GroupCount)> &&
                                      TIsTriviallyRelocatable_V<decltype(FFoliageCullTable::InstanceCount)> &&
                                      TIsTriviallyRelocatable_V<decltype(FFoliageCullTable::Generation)>;
    };
} // namespace OloEngine
