#pragma once

#include "SkeletonData.h"

namespace OloEngine
{
    /**
     * @brief Skeleton class for animated mesh skinning
     * 
     * Inherits from SkeletonData to provide the common skeleton structure
     * and adds skeleton-specific methods for setup and manipulation.
     */
    class Skeleton : public SkeletonData
    {
    public:
        using SkeletonData::SkeletonData; // Inherit constructors
    };
}
