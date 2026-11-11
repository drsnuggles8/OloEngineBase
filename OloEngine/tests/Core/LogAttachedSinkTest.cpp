// OLO_TEST_LAYER: unit
//
// Log::AttachSink / DetachSink: how a sink joins and leaves the three loggers
// while other threads are logging (the editor's console panel does it as the
// editor starts and shuts down). A logger walks its own sink list without a
// lock, so the panel's old push_back and erase on that list raced every log
// call on another thread; ThreadSanitizer reports that as a data race.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"

#include <spdlog/sinks/callback_sink.h>

#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    using namespace OloEngine;

    const char* const kMarker = "LogAttachedSinkTest marker";

    [[nodiscard]] spdlog::sink_ptr CountingSink(std::atomic<int>& count)
    {
        return std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&count](const spdlog::details::log_msg& msg)
            {
                if (std::string_view(msg.payload.data(), msg.payload.size()).find(kMarker) != std::string_view::npos)
                {
                    count.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }
} // namespace

TEST(LogAttachedSink, ReceivesEveryLoggerUntilItIsDetached)
{
    std::atomic<int> count{ 0 };
    const spdlog::sink_ptr sink = CountingSink(count);

    Log::Get().AttachSink(sink);
    Log::Get().GetCoreLogger()->info("{} core", kMarker);
    Log::Get().GetClientLogger()->info("{} client", kMarker);
    Log::Get().GetEditorConsoleLogger()->info("{} console", kMarker);
    EXPECT_EQ(count.load(), 3);

    Log::Get().DetachSink(sink);
    Log::Get().GetCoreLogger()->info("{} after detach", kMarker);
    EXPECT_EQ(count.load(), 3);
}

TEST(LogAttachedSink, AttachesAndDetachesWhileOtherThreadsLog)
{
    constexpr int kLoggers = 4;
    constexpr int kRounds = 200;
    std::atomic<bool> stop{ false };
    std::vector<std::thread> loggers;
    for (int i = 0; i < kLoggers; ++i)
    {
        loggers.emplace_back(
            [&stop]
            {
                while (!stop.load(std::memory_order_relaxed))
                {
                    Log::Get().GetCoreLogger()->trace("{} from a logging thread", kMarker);
                }
            });
    }

    std::atomic<int> count{ 0 };
    for (int round = 0; round < kRounds; ++round)
    {
        const spdlog::sink_ptr sink = CountingSink(count);
        Log::Get().AttachSink(sink);
        Log::Get().DetachSink(sink);
        // The sink dies here; no log call may still be inside it.
    }

    stop.store(true, std::memory_order_relaxed);
    for (std::thread& logger : loggers)
    {
        logger.join();
    }
    SUCCEED();
}
