#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace OloEngine::MCP::Detail
{
    // The admission object must outlive its leases and queued cleanup callbacks.
    // Production owns one for the editor lifetime; a failed cleanup requires restart.
    class BenchmarkCaptureAdmission
    {
      public:
        class Lease
        {
          public:
            Lease(const Lease&) = delete;
            auto operator=(const Lease&) -> Lease& = delete;
            ~Lease()
            {
                m_Owner.Release();
            }

          private:
            friend class BenchmarkCaptureAdmission;
            explicit Lease(BenchmarkCaptureAdmission& owner) : m_Owner(owner) {}
            BenchmarkCaptureAdmission& m_Owner;
        };

        explicit BenchmarkCaptureAdmission(void (*onFailure)() = nullptr) : m_OnFailure(onFailure) {}

        [[nodiscard]] std::shared_ptr<Lease> TryAcquire()
        {
            State expected = State::Idle;
            if (!m_State.compare_exchange_strong(expected, State::Active))
                return {};
            Lease* lease;
            try
            {
                lease = new Lease(*this);
            }
            catch (...)
            {
                Release();
                throw;
            }
            // A failed control-block allocation deletes lease and releases once.
            // Do not catch it above and accidentally release a subsequent owner.
            return std::shared_ptr<Lease>(lease);
        }

        void FailCleanup() noexcept
        {
            m_State.store(State::CleanupFailed);
            // Reporting must not throw from an unwinding guard or queued task.
            try
            {
                if (m_OnFailure)
                    m_OnFailure();
            }
            catch (...)
            {
            }
        }

        [[nodiscard]] bool CleanupFailed() const
        {
            return m_State.load() == State::CleanupFailed;
        }

      private:
        enum class State
        {
            Idle,
            Active,
            CleanupFailed
        };
        void Release() noexcept
        {
            State expected = State::Active;
            (void)m_State.compare_exchange_strong(expected, State::Idle);
        }
        std::atomic<State> m_State{ State::Idle };
        void (*m_OnFailure)();
    };

    template<typename Cleanup, typename Marshal, typename Enqueue>
    void DispatchBenchmarkCleanup(BenchmarkCaptureAdmission& admission, Cleanup cleanup,
                                  Marshal&& marshal, Enqueue&& enqueue) noexcept
    {
        try
        {
            marshal(cleanup);
            return;
        }
        catch (...)
        {
        }
        try
        {
            enqueue([&admission, cleanup = std::move(cleanup)]()
                    {
                try
                {
                    (void)cleanup();
                }
                catch (...)
                {
                    admission.FailCleanup();
                } });
        }
        catch (...)
        {
            admission.FailCleanup();
        }
    }
} // namespace OloEngine::MCP::Detail
