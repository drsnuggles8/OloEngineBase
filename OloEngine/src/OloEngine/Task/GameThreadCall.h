#pragma once

// =============================================================================
// RunOnGameThreadAndWait — run work that belongs to the game thread from a
// worker, and wait for its result.
//
// The game thread owns the graphics context. Work that creates GPU resources
// (an asset load: texture pixels, mesh buffers) must run there: on Vulkan a
// load-time upload submits to the queue the frame submits to, and a pack build
// that loaded its assets on its own thread lost the device two seconds in. The
// game thread drains its task queue at the top of every frame
// (Application::Run), outside any frame recording, so the work runs between
// frames.
//
// A caller that IS the game thread, or a host with no game thread attached
// (the test binary, a command-line cook), runs the work inline.
//
// The wait polls `cancel`. A call cancelled while still queued is abandoned —
// the game thread skips it — and returns nullopt. One the game thread has
// already started is always waited out, so a cancelled caller never tears down
// state the work is still using. An exception the work throws is rethrown here.
// =============================================================================

#include "OloEngine/Task/NamedThreads.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace OloEngine::Tasks
{
    template<typename T>
    [[nodiscard]] std::optional<T> RunOnGameThreadAndWait(std::function<T()> work, const std::atomic<bool>* cancel = nullptr,
                                                          const char* debugName = "RunOnGameThreadAndWait")
    {
        auto& threads = FNamedThreadManager::Get();
        if (threads.GetCurrentThreadIfKnown() == ENamedThread::GameThread ||
            !threads.IsThreadAttached(ENamedThread::GameThread))
        {
            return work();
        }

        // One owner: the game thread claims the call (Pending -> Running) or the
        // waiter abandons it (Pending -> Abandoned), never both.
        enum class State
        {
            Pending,
            Running,
            Abandoned,
            Finished,
        };
        struct Call
        {
            std::mutex Mutex;
            std::condition_variable Done;
            State Status = State::Pending;
            std::function<T()> Work;
            std::optional<T> Result;
            std::exception_ptr Error;
        };
        auto call = std::make_shared<Call>();
        call->Work = std::move(work);

        threads.EnqueueTask(ENamedThread::GameThread,
                            FNamedThreadTask(
                                [call]()
                                {
                                    std::function<T()> claimed;
                                    {
                                        std::lock_guard<std::mutex> lock(call->Mutex);
                                        if (call->Status != State::Pending)
                                            return;
                                        call->Status = State::Running;
                                        claimed = std::move(call->Work);
                                    }
                                    std::optional<T> result;
                                    std::exception_ptr error;
                                    try
                                    {
                                        result.emplace(claimed());
                                    }
                                    catch (...)
                                    {
                                        error = std::current_exception();
                                    }
                                    {
                                        std::lock_guard<std::mutex> lock(call->Mutex);
                                        call->Result = std::move(result);
                                        call->Error = error;
                                        call->Status = State::Finished;
                                    }
                                    call->Done.notify_all();
                                },
                                EExtendedTaskPriority::GameThreadNormalPri, debugName));

        std::unique_lock<std::mutex> lock(call->Mutex);
        while (call->Status != State::Finished)
        {
            if (cancel != nullptr && cancel->load(std::memory_order_acquire) && call->Status == State::Pending)
            {
                call->Status = State::Abandoned;
                call->Work = {}; // release what the work captured; the queued task will skip it
                return std::nullopt;
            }
            call->Done.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (call->Error)
        {
            std::rethrow_exception(call->Error);
        }
        return std::move(call->Result);
    }

    // Hand work to the game thread without waiting: what releases resources
    // the game thread created (an asset manager whose assets own GPU objects).
    // Moving the last reference INTO `work` is the point — the caller keeps
    // none, so the release happens where `work` runs. Inline when the caller is
    // the game thread or none is attached. Not waiting is deliberate: a worker
    // the game thread is joining (a cancelled build at teardown) must not block
    // on it; anything left queued then dies with the queue, on the main thread.
    inline void PostToGameThread(FNamedThreadTask::FTaskFunction work, const char* debugName = "PostToGameThread")
    {
        auto& threads = FNamedThreadManager::Get();
        if (threads.GetCurrentThreadIfKnown() == ENamedThread::GameThread ||
            !threads.IsThreadAttached(ENamedThread::GameThread))
        {
            work();
            return;
        }
        threads.EnqueueTask(ENamedThread::GameThread,
                            FNamedThreadTask(MoveTemp(work), EExtendedTaskPriority::GameThreadNormalPri, debugName));
    }
} // namespace OloEngine::Tasks
