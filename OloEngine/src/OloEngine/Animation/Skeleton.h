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

        /// Start an actor with the imported rig's pose. Its animation and history
        /// can then advance independently of other actors using the model.
        [[nodiscard]] Ref<Skeleton> Instantiate() const
        {
            auto pose = Ref<Skeleton>::Create();
            static_cast<SkeletonData&>(*pose) = *this;
            pose->m_BoneHistoryEverValid = false;
            pose->m_DeformationRevision = 0;
            pose->NoteDeformationResetCause(kCauseFirstUse);
            pose->ResetBoneHistory();
            return pose;
        }
    };
} // namespace OloEngine
