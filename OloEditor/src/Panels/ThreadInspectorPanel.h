#pragma once

namespace OloEngine
{
    // Editor debug panel that enumerates the engine's live OS threads.
    //
    // Backed by FThreadManager (HAL/ThreadManager.h): every thread created via
    // FRunnableThread::Create — scheduler workers, the audio/network threads,
    // async tasks and editor build threads — registers itself there. The game
    // (main) thread is the only long-lived thread not created that way, so it is
    // synthesised from the current thread id (this panel renders on the game
    // thread). Also surfaces a scheduler summary and named-thread queue backlog.
    class ThreadInspectorPanel
    {
      public:
        ThreadInspectorPanel() = default;

        void OnImGuiRender(bool* p_open = nullptr);

      private:
        bool m_OnlyRunning = false;
    };
} // namespace OloEngine
