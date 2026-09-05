#pragma once

#include "OloEngine/Renderer/Commands/CommandDispatchRecordingState.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"

namespace OloEngine
{
    struct FrontendRecordingContext
    {
        CommandDispatchRecordingState Dispatch;
        HeapBinding::RecordingState Heap;
        RendererProfiler::RecordingStats Profiler;

        void Prepare(u32 instanceCapacity)
        {
            Dispatch.Prepare(instanceCapacity);
            Heap.Prepare();
            Profiler.Reset();
        }

        void Publish()
        {
            Dispatch.Publish();
            Heap.Publish();
            Profiler.Publish();
        }
    };

    class ScopedFrontendRecordingContext
    {
      public:
        explicit ScopedFrontendRecordingContext(FrontendRecordingContext& context)
            : m_Dispatch(context.Dispatch), m_Heap(context.Heap), m_Profiler(context.Profiler)
        {
        }

      private:
        ScopedCommandDispatchRecordingState m_Dispatch;
        HeapBinding::ScopedRecordingState m_Heap;
        RendererProfiler::ScopedRecordingStats m_Profiler;
    };
} // namespace OloEngine
