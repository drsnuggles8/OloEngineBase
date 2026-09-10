#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <stdexcept>
#include <string>
#include <utility>

namespace OloEngine::Automation
{
    // A queued operation has exactly one owner: the main thread claims execution,
    // or the waiting caller cancels it. Once execution starts, the caller must
    // await its actual outcome; a timeout must never report a write as abandoned
    // while that write can still run against the caller's captured editor state.
    class AutomationMainThreadJob
    {
      public:
        explicit AutomationMainThreadJob(std::function<nlohmann::json()> job)
            : m_Job(std::move(job)), m_Result(m_Promise.get_future())
        {
        }

        void Execute()
        {
            State expected = State::Pending;
            if (!m_State.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel))
                return;

            try
            {
                nlohmann::json result = m_Job();
                m_Job = {};
                m_Promise.set_value(std::move(result));
            }
            catch (...)
            {
                const std::exception_ptr error = std::current_exception();
                m_Job = {};
                m_Promise.set_exception(error);
            }
        }

        // False means another thread already owns execution or cancellation.
        // Either way, Get() waits for that owner's final result. Clear captures
        // before publishing cancellation so an abandoned queue entry retains no
        // references to the caller's handler frame or editor resources.
        bool CancelPending(const std::string& message)
        {
            State expected = State::Pending;
            if (!m_State.compare_exchange_strong(expected, State::Cancelled, std::memory_order_acq_rel))
                return false;

            m_Job = {};
            m_Promise.set_exception(std::make_exception_ptr(std::runtime_error(message)));
            return true;
        }

        [[nodiscard]] std::future_status WaitFor(std::chrono::milliseconds timeout)
        {
            return m_Result.wait_for(timeout);
        }

        [[nodiscard]] nlohmann::json Get()
        {
            return m_Result.get();
        }

      private:
        enum class State
        {
            Pending,
            Running,
            Cancelled,
        };

        std::atomic<State> m_State{ State::Pending };
        std::function<nlohmann::json()> m_Job;
        std::promise<nlohmann::json> m_Promise;
        std::future<nlohmann::json> m_Result;
    };
} // namespace OloEngine::Automation
