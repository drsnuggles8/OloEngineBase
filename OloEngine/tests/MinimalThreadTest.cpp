#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/LockFreeFIFO.h"
#include <thread>
#include <atomic>

using namespace OloEngine;
using namespace OloEngine::Audio;

//===========================================
// Minimal Thread Test without AudioThread
//===========================================

TEST(MinimalThreadTest, BasicStdThread)
{
    std::atomic<bool> threadActive{false};
    std::atomic<bool> threadExecuted{false};
    
    auto threadFunc = [&threadActive, &threadExecuted]()
    {
        threadExecuted.store(true);
        while (threadActive.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    
    // Create and start thread
    threadActive.store(true);
    std::thread testThread(threadFunc);
    
    // Wait for thread to start
    auto start = std::chrono::steady_clock::now();
    while (!threadExecuted.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
        {
            FAIL() << "Thread failed to start";
        }
    }
    
    EXPECT_TRUE(threadExecuted.load());
    
    // Stop thread
    threadActive.store(false);
    testThread.join();
    
    EXPECT_TRUE(true); // If we get here, basic threading works
}

TEST(MinimalThreadTest, FIFOWithThread)
{
    SingleReaderSingleWriterFIFO<int> fifo;
    fifo.reset(16);
    
    std::atomic<bool> threadActive{true};
    std::atomic<int> itemsReceived{0};
    
    // Consumer thread
    std::thread consumer([&fifo, &threadActive, &itemsReceived]()
    {
        while (threadActive.load() || !fifo.isEmpty())
        {
            int value;
            if (fifo.pop(value))
            {
                itemsReceived.fetch_add(1);
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    });
    
    // Producer (main thread)
    for (int i = 0; i < 100; ++i)
    {
        while (!fifo.push(i))
        {
            std::this_thread::yield();
        }
    }
    
    // Stop consumer
    threadActive.store(false);
    consumer.join();
    
    EXPECT_EQ(itemsReceived.load(), 100);
}