#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Networking/Prediction/ClientPrediction.h"

#include <vector>

#include <glm/glm.hpp>

namespace OloEngine
{
    // The engine's built-in input command: a displacement to apply to an entity's
    // transform.
    //
    // WHY A DISPLACEMENT AND NOT A DIRECTION + dt. Reconciliation replays every
    // unacknowledged input on top of freshly-received server state, and that replay
    // has no timeline — it is a loop over buffered commands, not a re-simulation at
    // the original frame times. An input that meant "move at speed s" would replay
    // with whatever dt the replay happened to assume and land somewhere the server
    // never went, so prediction would never converge. Baking the step on the client
    // makes each command a discrete, order-independent, exactly-replayable delta.
    //
    // The server still validates it: MakeMovementApplyCallback clamps the step
    // length, so a client that fabricates a 10 000-unit delta gets a legal step, not
    // a teleport. That check is the whole point of the input being a *request*.
    struct NetworkMovementInput
    {
        glm::vec3 Delta{ 0.0f };

        [[nodiscard]] std::vector<u8> Encode() const;

        // Returns false on a truncated payload or a non-finite component — every
        // field here arrives from an untrusted client.
        [[nodiscard]] static bool Decode(const u8* data, u32 size, NetworkMovementInput& out);
    };

    // Build the InputApplyCallback that applies NetworkMovementInput commands.
    //
    // Register the SAME callback on both ends (NetworkManager::SetInputApplyCallback
    // does exactly that) so the client's prediction and the server's authoritative
    // application are the same function — the only way the two can agree without a
    // second, drift-prone implementation.
    //
    // `maxStepDistance` bounds one command's displacement; 0 disables the clamp.
    [[nodiscard]] InputApplyCallback MakeMovementApplyCallback(f32 maxStepDistance = 1.0f);
} // namespace OloEngine
