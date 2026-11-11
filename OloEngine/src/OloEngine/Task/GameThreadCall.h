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
#include <vector>

namespace OloEngine::Tasks
{
    namespace Private
    {
        // One owner: the game thread claims a call (Pending -> Running) or the
        // waiter abandons it (Pending -> Abandoned), never both. At namespace
        // scope: MSVC rejects a function-local enum in a local struct's default
        // member initializer inside a function template (C2653).
        enum class GameThreadCallState
        {
            Pending,
            Running,
            Abandoned,
            Finished,
        };

        template<typename T>
        struct GameThreadCall
        {
            std::mutex Mutex;
            std::condition_variable Done;
            GameThreadCallState Status = GameThreadCallState::Pending;
            std::function<T()> Work;
            std::optional<T> Result;
            std::exception_ptr Error;
        };
    } // namespace Private

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

        using State = Private::GameThreadCallState;
        auto call = std::make_shared<Private::GameThreadCall<T>>();
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

    namespace Private
    {
        struct FPostedWork
        {
            FNamedThreadTask::FTaskFunction Work;
            bool bClaimed = false;
        };

        // The work PostToGameThread has queued and nothing has run yet. Never
        // destroyed: work still here at process exit is left alone rather than
        // released after the renderer it releases into is gone.
        struct FPostedWorkRegistry
        {
            std::mutex Mutex;
            std::vector<std::shared_ptr<FPostedWork>> Pending;

            static FPostedWorkRegistry& Get()
            {
                static FPostedWorkRegistry* s_Registry = new FPostedWorkRegistry();
                return *s_Registry;
            }
        };

        // Runs `posted` unless something already has. The queued task and
        // RunPostedGameThreadWork both come here; whichever is first runs it.
        inline bool RunPostedWorkOnce(const std::shared_ptr<FPostedWork>& posted)
        {
            FNamedThreadTask::FTaskFunction work;
            {
                FPostedWorkRegistry& registry = FPostedWorkRegistry::Get();
                const std::lock_guard<std::mutex> lock(registry.Mutex);
                if (posted->bClaimed)
                {
                    return false;
                }
                posted->bClaimed = true;
                work = MoveTemp(posted->Work);
                std::erase(registry.Pending, posted);
            }
            work();
            return true;
        }
    } // namespace Private

    // Hand work to the game thread without waiting: what releases resources
    // the game thread created (an asset manager whose assets own GPU objects).
    // Moving the last reference INTO `work` is the point — the caller keeps
    // none, so the release happens where `work` runs. Inline when the caller is
    // the game thread or none is attached. Not waiting is deliberate: a worker
    // the game thread is joining (a cancelled build at teardown) must not block
    // on it. The game thread's queue runs the work on its next drain; at
    // teardown, where no drain follows, RunPostedGameThreadWork does.
    inline void PostToGameThread(FNamedThreadTask::FTaskFunction work, const char* debugName = "PostToGameThread")
    {
        auto& threads = FNamedThreadManager::Get();
        if (threads.GetCurrentThreadIfKnown() == ENamedThread::GameThread ||
            !threads.IsThreadAttached(ENamedThread::GameThread))
        {
            work();
            return;
        }
        auto posted = std::make_shared<Private::FPostedWork>();
        posted->Work = MoveTemp(work);
        {
            Private::FPostedWorkRegistry& registry = Private::FPostedWorkRegistry::Get();
            const std::lock_guard<std::mutex> lock(registry.Mutex);
            registry.Pending.push_back(posted);
        }
        threads.EnqueueTask(ENamedThread::GameThread,
                            FNamedThreadTask([posted]()
                                             { Private::RunPostedWorkOnce(posted); },
                                             EExtendedTaskPriority::GameThreadNormalPri, debugName));
    }

    // Run, on the calling thread, the work PostToGameThread queued that has not
    // run yet, and nothing else in the game thread's queue. For the game
    // thread's teardown: ~Application stops draining the queue once its layers
    // are gone, but a layer's destructor may just have joined a thread whose
    // last act was to post a release here, and that release must run while the
    // renderer is alive. Ordinary queued tasks are not run: they may use the
    // layers that were just destroyed.
    // @return how many pieces of work ran
    inline u32 RunPostedGameThreadWork()
    {
        u32 ran = 0;
        for (;;)
        {
            std::shared_ptr<Private::FPostedWork> next;
            {
                Private::FPostedWorkRegistry& registry = Private::FPostedWorkRegistry::Get();
                const std::lock_guard<std::mutex> lock(registry.Mutex);
                if (registry.Pending.empty())
                {
                    break;
                }
                next = registry.Pending.front();
            }
            if (Private::RunPostedWorkOnce(next))
            {
                ++ran;
            }
        }
        return ran;
    }
} // namespace OloEngine::Tasks
