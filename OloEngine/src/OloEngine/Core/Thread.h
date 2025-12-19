#pragma once

#include <string>
#include <thread>
#include <utility>

namespace OloEngine
{

    class Thread
    {
      public:
        Thread(const std::string& name);
        ~Thread();

        // Disable copy semantics to prevent thread ownership issues
        Thread(const Thread&) = delete;
        Thread& operator=(const Thread&) = delete;

        // Enable move semantics for transferring thread ownership
        Thread(Thread&& other) noexcept;
        Thread& operator=(Thread&& other) noexcept;

        template<typename Fn, typename... Args>
        void Dispatch(Fn&& func, Args&&... args)
        {
            // Ensure any existing thread is properly handled before creating a new one
            if (m_Thread.joinable())
                m_Thread.join();

            m_Thread = std::thread(std::forward<Fn>(func), std::forward<Args>(args)...);
            SetName(m_Name);
        }

        void SetName(const std::string& name);

        void Join();

        std::thread::id GetID() const;

      private:
        std::string m_Name;
        std::thread m_Thread;
    };

    class ThreadSignal
    {
      public:
        ThreadSignal(const std::string& name, bool manualReset = false);
        ~ThreadSignal() noexcept;

        // Disable copy semantics to prevent double-closing handles
        ThreadSignal(const ThreadSignal&) = delete;
        ThreadSignal& operator=(const ThreadSignal&) = delete;

        // Enable move semantics for transferring handle ownership
        ThreadSignal(ThreadSignal&& other) noexcept;
        ThreadSignal& operator=(ThreadSignal&& other) noexcept;

        void Wait();
        void Signal();
        void Reset();

      private:
        void* m_SignalHandle = nullptr;
    };

} // namespace OloEngine
