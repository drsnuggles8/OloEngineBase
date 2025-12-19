#pragma once

#include "SkeletonData.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    /**
     * @brief Skeleton class for animated mesh skinning
     *
     * Inherits from SkeletonData to provide the common skeleton structure
     * and adds skeleton-specific methods for setup and manipulation.
     */
    class Skeleton : public SkeletonData, public RefCounted
    {
      public:
        using SkeletonData::SkeletonData; // Inherit constructors
    };
} // namespace OloEngine
