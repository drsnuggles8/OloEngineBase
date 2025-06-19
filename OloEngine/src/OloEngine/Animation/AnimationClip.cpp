#include "OloEngine/Animation/AnimationClip.h"

namespace OloEngine
{
    const BoneAnimation* AnimationClip::FindBoneAnimation(const std::string& boneName) const
    {
        for (const auto& anim : BoneAnimations)
        {
            if (anim.BoneName == boneName)
                return &anim;
        }
        return nullptr;
    }
}
