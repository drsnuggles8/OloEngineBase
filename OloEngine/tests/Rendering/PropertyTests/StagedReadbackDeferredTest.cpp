// OLO_TEST_LAYER: L7
// =============================================================================
// StagedReadbackDeferredTest.cpp -- the fenced, never-waiting pair of
// StagedBufferReadback: StageDeferred() and ReadCompleted().
//
// A per-frame statistic is copied to a staging buffer in the frame that wrote
// it and read on a later frame. It used to be read with nothing tying the read
// to the copy having run. On Vulkan, with frames in flight, the read returned
// the copy before last or, on a new staging buffer, whatever its memory held:
// the virtual shadow map's page counters skipped the one frame that drew every
// page, and once reported 0xFF9D9D9D pages (#1697).
//
// THE CONTRACT. A copy is handed out only once its fence has signalled; copies
// are handed out once each, one per call, in the order they were staged; when
// every ring slot is waiting to be read, a new copy is refused rather than
// written over one in flight.
//
// Runs in both test processes. In the Vulkan process a copy staged inside a
// frame cannot have run before that frame is submitted, so a read in the same
// frame MUST be refused: that is the deterministic form of the fault.
//
// Classification: L7 (device, no scene). SKIPs cleanly without a GPU.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"

#include "OloEngine/Renderer/Debug/StagedBufferReadback.h"
#include "OloEngine/Renderer/RenderCommand.h"

#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"

#include <cstdint>
#endif

#include <gtest/gtest.h>

#include <functional>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u64 kBytes = 256u;

        // A payload no other copy has and that is not a fill pattern.
        [[nodiscard]] std::vector<u8> Payload(u32 index)
        {
            std::vector<u8> bytes(kBytes);
            for (sizet i = 0; i < bytes.size(); ++i)
            {
                bytes[i] = static_cast<u8>((i * 31u + index * 97u + 5u) & 0xFFu);
            }
            return bytes;
        }
    } // namespace

    class StagedReadbackDeferredTest : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override {}

        void TearDown() override
        {
            if (::testing::Test::IsSkipped())
            {
                RendererAttachedTest::TearDown();
                return;
            }
            // Inside a frame, as every other GPU call of this suite is.
            RunInFrame([this]()
                       {
                           m_Readback.Release();
                           for (const RHI::ResourceHandle source : m_Sources)
                           {
                               RenderCommand::DeleteBuffer(source);
                           }
                           m_Sources.clear(); });
            RendererAttachedTest::TearDown();
        }

        // A source of its own per copy: the copy runs when its frame is
        // submitted, and a source rewritten by the next frame's upload could
        // be read after that upload.
        [[nodiscard]] RHI::ResourceHandle MakeSource(u32 index)
        {
            const RHI::ResourceHandle source = RenderCommand::CreateBufferHandle();
            RenderCommand::AllocateBufferStorage(source, kBytes, RHI::MemoryResidency::HostToDevice);
            const std::vector<u8> payload = Payload(index);
            RenderCommand::UploadBufferSubData(source, 0, payload.size(), payload.data());
            m_Sources.push_back(source);
            return source;
        }

        // One frame of GPU work. In the Vulkan process RunInFrame submits a
        // frame. In the OpenGL process it runs the work inline and nothing is
        // presented, so a "frame" of this suite lasts microseconds and the GPU
        // has not started it when the next one begins: four of them fill the
        // ring before the first copy has run. A real frame ends in a swap. This
        // gives the OpenGL frame that end: wait until the GPU has caught up.
        void Frame(const std::function<void()>& work)
        {
            RunInFrame(work);
            if (!TestRenderBackend::IsVulkanProcess())
            {
                const u64 caughtUp = RenderCommand::CreateFence();
                (void)RenderCommand::ClientWaitFence(caughtUp, 2'000'000'000ull);
                RenderCommand::DestroyFence(caughtUp);
            }
        }

        // One read, as a per-frame reader makes.
        void Poll(std::vector<std::vector<u8>>& received)
        {
            std::vector<u8> got(kBytes, 0xEEu);
            if (m_Readback.ReadCompleted(got.data(), got.size()))
            {
                received.push_back(std::move(got));
            }
        }

        StagedBufferReadback m_Readback;
        std::vector<RHI::ResourceHandle> m_Sources;
    };

    TEST_F(StagedReadbackDeferredTest, EveryCopyIsHandedOutOnceInOrderAndNotBeforeItHasRun)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        constexpr u32 kCopies = 7u;
        std::vector<std::vector<u8>> received;
        bool readInTheFrameThatStaged = false;

        for (u32 i = 0; i < kCopies; ++i)
        {
            bool staged = false;
            Frame([this, i, &received, &staged, &readInTheFrameThatStaged]()
                  {
                           Poll(received);
                           staged = m_Readback.StageDeferred(MakeSource(i), 0, kBytes);
                           if (i == 0u)
                           {
                               // Nothing else is outstanding, so a true here
                               // can only be this frame's own copy.
                               std::vector<u8> early(kBytes, 0xEEu);
                               readInTheFrameThatStaged = m_Readback.ReadCompleted(early.data(), early.size());
                               if (readInTheFrameThatStaged)
                               {
                                   received.push_back(std::move(early));
                               }
                           } });
            ASSERT_TRUE(staged) << "copy " << i << " was refused with a reader polling every frame";
        }
        if (TestRenderBackend::IsVulkanProcess())
        {
            EXPECT_FALSE(readInTheFrameThatStaged)
                << "a copy was handed out in the frame that recorded it, before that frame was submitted: the bytes "
                   "are not the copy's";
        }

        // The copies still in flight arrive over the next frames.
        for (u32 frame = 0; frame < 16u && received.size() < kCopies; ++frame)
        {
            Frame([this, &received]()
                  { Poll(received); });
        }

        ASSERT_EQ(received.size(), static_cast<sizet>(kCopies)) << "not every staged copy was handed out";
        for (u32 i = 0; i < kCopies; ++i)
        {
            EXPECT_EQ(received[i], Payload(i)) << "read " << i << " is not copy " << i
                                               << ": a copy was skipped, repeated, reordered or read before it landed";
        }
        EXPECT_EQ(m_Readback.GetDeferredCopiesOutstanding(), 0u);

        // And nothing more: a copy is handed out once.
        Frame([this, &received]()
              { Poll(received); });
        EXPECT_EQ(received.size(), static_cast<sizet>(kCopies));
    }

    TEST_F(StagedReadbackDeferredTest, AFullRingRefusesANewCopyAndKeepsTheOnesItHolds)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        constexpr u32 kSlots = StagedBufferReadback::kRingSlots;
        u32 accepted = 0;
        bool overflowAccepted = true;
        // Nobody reads: the reader has fallen kSlots copies behind.
        for (u32 i = 0; i < kSlots; ++i)
        {
            Frame([this, i, &accepted]()
                  { accepted += m_Readback.StageDeferred(MakeSource(i), 0, kBytes) ? 1u : 0u; });
        }
        Frame([this, &overflowAccepted]()
              { overflowAccepted = m_Readback.StageDeferred(MakeSource(StagedBufferReadback::kRingSlots), 0, kBytes); });
        ASSERT_EQ(accepted, kSlots);
        EXPECT_FALSE(overflowAccepted) << "a copy was staged over one that had not been read";
        EXPECT_EQ(m_Readback.GetDeferredCopiesOutstanding(), kSlots);

        std::vector<std::vector<u8>> received;
        for (u32 frame = 0; frame < 16u && received.size() < kSlots; ++frame)
        {
            Frame([this, &received]()
                  { Poll(received); });
        }
        ASSERT_EQ(received.size(), static_cast<sizet>(kSlots));
        for (u32 i = 0; i < kSlots; ++i)
        {
            EXPECT_EQ(received[i], Payload(i)) << "read " << i << " is not the copy the ring held in that place";
        }

        // A slot that has been read is free again.
        bool stagedAfterDrain = false;
        Frame([this, &stagedAfterDrain]()
              { stagedAfterDrain = m_Readback.StageDeferred(MakeSource(99u), 0, kBytes); });
        EXPECT_TRUE(stagedAfterDrain);
    }

#if OLO_WITH_VULKAN
    // A mid-frame flush hands the reader the frame's writes. When its submit
    // fails, the command-buffer reset throws the recorded commands away, and
    // the layout transitions they held never happen: the next barrier must
    // start from the layout the image is really in, the executed one. Once the
    // queue accepts the recording its transitions do happen, even when the wait
    // then fails, so a later rollback must not undo them.
    TEST_F(StagedReadbackDeferredTest, AFlushPublishesExactlyTheLayoutsTheQueueAccepted)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        if (!TestRenderBackend::IsVulkanProcess())
            GTEST_SKIP() << "the mid-frame flush belongs to the Vulkan frame recording";
        auto& tracker = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI()).LayoutTracker();
        // Bookkeeping only: no barrier or Vulkan call names this handle, and it
        // is forgotten inside the frame that registered it.
        const auto image = reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(0x0F1A5400u));
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
        constexpr VkImageLayout kResting = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        enum class Outcome
        {
            Rejected,
            WaitFails,
            Completes
        };
        struct Arm
        {
            bool Flushed = false;
            u32 PendingInjection = 0u;
            VkImageLayout Recorded = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageLayout Executed = VK_IMAGE_LAYOUT_UNDEFINED;
            // After a second, rejected flush that records nothing new.
            VkImageLayout RecordedAfterRollback = VK_IMAGE_LAYOUT_UNDEFINED;
        };
        const auto flush = [](Outcome outcome, u32& pending)
        {
            VulkanContext::SetFailNextFlushSubmitsForTesting(outcome == Outcome::Rejected ? 1u : 0u);
            VulkanContext::SetFailNextFlushWaitsForTesting(outcome == Outcome::WaitFails ? 1u : 0u);
            const bool flushed = VulkanContext::Get() != nullptr && VulkanContext::Get()->FlushFrameRecordingAndWait();
            pending = VulkanContext::GetPendingFailNextFlushSubmitsForTesting() + VulkanContext::GetPendingFailNextFlushWaitsForTesting();
            VulkanContext::SetFailNextFlushSubmitsForTesting(0u);
            VulkanContext::SetFailNextFlushWaitsForTesting(0u);
            return flushed;
        };
        const auto run = [&](Outcome outcome)
        {
            Arm arm;
            RunInFrame([&]()
                       {
                           tracker.RegisterImage(image, 1u, 1u, 0u, kResting);
                           tracker.SetLayout(image, range, VK_IMAGE_LAYOUT_GENERAL);
                           arm.Flushed = flush(outcome, arm.PendingInjection);
                           arm.Recorded = tracker.CurrentLayout(image, range);
                           arm.Executed = tracker.CurrentExecutedLayout(image, range);
                           u32 ignored = 0u;
                           (void)flush(Outcome::Rejected, ignored);
                           arm.RecordedAfterRollback = tracker.CurrentLayout(image, range);
                           tracker.ForgetImage(image); });
            return arm;
        };

        const Arm rejected = run(Outcome::Rejected);
        EXPECT_FALSE(rejected.Flushed);
        EXPECT_EQ(rejected.PendingInjection, 0u) << "the flush must reach its submit for this test to mean anything";
        EXPECT_EQ(rejected.Executed, kResting);
        EXPECT_EQ(rejected.Recorded, kResting) << "a discarded transition is still recorded";

        // Accepted, then the wait reports an error: the GPU still runs the work.
        const Arm waitFailed = run(Outcome::WaitFails);
        EXPECT_FALSE(waitFailed.Flushed);
        EXPECT_EQ(waitFailed.PendingInjection, 0u) << "the flush must reach its wait for this test to mean anything";
        EXPECT_EQ(waitFailed.Executed, VK_IMAGE_LAYOUT_GENERAL) << "an accepted transition was not published as executed";
        EXPECT_EQ(waitFailed.Recorded, VK_IMAGE_LAYOUT_GENERAL);
        EXPECT_EQ(waitFailed.RecordedAfterRollback, VK_IMAGE_LAYOUT_GENERAL) << "a later rollback undid an accepted transition";

        // The same frame without an injection: the flush executes the
        // transition, so both views move. A tracker that never recorded
        // anything would pass the rejected assertions but not these.
        const Arm accepted = run(Outcome::Completes);
        ASSERT_TRUE(accepted.Flushed);
        EXPECT_EQ(accepted.Recorded, VK_IMAGE_LAYOUT_GENERAL);
        EXPECT_EQ(accepted.Executed, VK_IMAGE_LAYOUT_GENERAL);
        EXPECT_EQ(accepted.RecordedAfterRollback, VK_IMAGE_LAYOUT_GENERAL);
    }
#endif
} // namespace OloEngine::Tests
