#pragma once

#include "OloEngine/Core/Base.h"

#include <memory>

namespace OloEngine
{
    struct CommandDispatchData;

    // One replay's mutable frontend state. Prepare on the render thread before
    // the fork; reuse its upload objects between regions and frames.
    class CommandDispatchRecordingState
    {
      public:
        CommandDispatchRecordingState();
        ~CommandDispatchRecordingState();
        CommandDispatchRecordingState(const CommandDispatchRecordingState&) = delete;
        CommandDispatchRecordingState& operator=(const CommandDispatchRecordingState&) = delete;

        void Prepare(u32 instanceCapacity);
        void Publish();

      private:
        friend class ScopedCommandDispatchRecordingState;
        std::unique_ptr<CommandDispatchData> m_Data;
    };

    // Item lifetime, including when ParallelFor executes an item on its caller.
    class ScopedCommandDispatchRecordingState
    {
      public:
        explicit ScopedCommandDispatchRecordingState(CommandDispatchRecordingState& state);
        ~ScopedCommandDispatchRecordingState();
        ScopedCommandDispatchRecordingState(const ScopedCommandDispatchRecordingState&) = delete;
        ScopedCommandDispatchRecordingState& operator=(const ScopedCommandDispatchRecordingState&) = delete;

      private:
        CommandDispatchData* m_Previous;
    };
} // namespace OloEngine
