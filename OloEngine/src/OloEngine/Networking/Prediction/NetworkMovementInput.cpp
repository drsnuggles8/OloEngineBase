#include "OloEnginePCH.h"
#include "OloEngine/Networking/Prediction/NetworkMovementInput.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Serialization/Archive.h"

#include <cmath>

namespace OloEngine
{
    std::vector<u8> NetworkMovementInput::Encode() const
    {
        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsNetArchive = true;
        f32 x = Delta.x;
        f32 y = Delta.y;
        f32 z = Delta.z;
        writer << x << y << z;
        return buffer;
    }

    bool NetworkMovementInput::Decode(const u8* data, u32 size, NetworkMovementInput& out)
    {
        if (data == nullptr || size < 3 * sizeof(f32))
        {
            return false;
        }

        FMemoryReader reader(data, static_cast<i64>(size));
        reader.ArIsNetArchive = true;
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 z = 0.0f;
        reader << x << y << z;
        if (reader.IsError())
        {
            return false;
        }

        // A NaN component would propagate into the transform and poison every
        // downstream computation (culling, physics, the next snapshot) silently.
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        {
            return false;
        }

        out.Delta = { x, y, z };
        return true;
    }

    InputApplyCallback MakeMovementApplyCallback(f32 maxStepDistance)
    {
        // A non-finite bound would disable the clamp silently (every comparison
        // with NaN is false), which is the one failure mode this guard exists to
        // prevent. Fall back to the default rather than to "no limit".
        if (!std::isfinite(maxStepDistance) || maxStepDistance < 0.0f)
        {
            OLO_CORE_WARN_TAG("Networking", "MakeMovementApplyCallback: invalid maxStepDistance {}; using 1.0",
                              maxStepDistance);
            maxStepDistance = 1.0f;
        }

        return [maxStepDistance](Scene& scene, u64 entityUUID, const u8* data, u32 size)
        {
            OLO_PROFILE_SCOPE("MovementApplyCallback");

            NetworkMovementInput input;
            if (!NetworkMovementInput::Decode(data, size, input))
            {
                return;
            }

            // Authority: bound the step a single command may produce. Without this
            // a client can simply send a huge delta and the server will faithfully
            // apply it — authoritative in structure but not in effect.
            if (maxStepDistance > 0.0f)
            {
                if (const f32 length = glm::length(input.Delta); length > maxStepDistance)
                {
                    // length > maxStepDistance > 0, so the division is safe.
                    input.Delta *= (maxStepDistance / length);
                }
            }

            auto entityOpt = scene.TryGetEntityWithUUID(UUID(entityUUID));
            if (!entityOpt.has_value())
            {
                return;
            }

            Entity entity = *entityOpt;
            if (!entity.HasComponent<TransformComponent>())
            {
                return;
            }

            entity.GetComponent<TransformComponent>().Translation += input.Delta;
        };
    }
} // namespace OloEngine
