#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "MockRendererAPI.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"

using namespace OloEngine;          // NOLINT(google-build-using-namespace)
using namespace OloEngine::Testing; // NOLINT(google-build-using-namespace)

// =============================================================================
// Dispatch Table Completeness
// =============================================================================

TEST(CommandDispatch, DispatchTableIsComplete)
{
    CommandDispatch::Initialize();

    // Verify every valid CommandType has a dispatch function
    for (u8 i = 1; i < static_cast<u8>(CommandType::COUNT); ++i)
    {
        auto type = static_cast<CommandType>(i);
        auto fn = CommandDispatch::GetDispatchFunction(type);
        EXPECT_NE(fn, nullptr)
            << "Missing dispatch function for CommandType " << static_cast<int>(type)
            << " (" << CommandTypeToString(type) << ")";
    }

    CommandDispatch::Shutdown();
}

TEST(CommandDispatch, InvalidTypeReturnsNull)
{
    CommandDispatch::Initialize();

    auto fn = CommandDispatch::GetDispatchFunction(CommandType::Invalid);
    EXPECT_EQ(fn, nullptr);

    CommandDispatch::Shutdown();
}

// =============================================================================
// CommandTypeToString Coverage
// =============================================================================

TEST(CommandDispatch, CommandTypeToStringCoversAll)
{
    for (u8 i = 0; i < static_cast<u8>(CommandType::COUNT); ++i)
    {
        auto type = static_cast<CommandType>(i);
        const char* name = CommandTypeToString(type);
        EXPECT_NE(name, nullptr) << "Null name for CommandType " << static_cast<int>(type);

        // Invalid should return "Invalid", others should have a real name
        if (type == CommandType::Invalid)
        {
            EXPECT_STREQ(name, "Invalid");
        }
        else
        {
            EXPECT_STRNE(name, "") << "Empty name for CommandType " << static_cast<int>(type);
        }
    }
}

// =============================================================================
// Dispatch Records Correct Type (via MockRendererAPI)
// =============================================================================

class CommandDispatchIntegration : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        CommandDispatch::Initialize();
        m_Allocator = std::make_unique<CommandAllocator>();
        m_MockAPI = std::make_unique<MockRendererAPI>();
    }

    void TearDown() override
    {
        m_MockAPI.reset();
        m_Allocator.reset();
        CommandDispatch::Shutdown();
    }

    std::unique_ptr<CommandAllocator> m_Allocator;
    std::unique_ptr<MockRendererAPI> m_MockAPI;
};

TEST_F(CommandDispatchIntegration, SetViewportDispatch)
{
    auto cmd = MakeSyntheticViewportCommand(0, 0, 1920, 1080);
    auto* packet = m_Allocator->CreateCommandPacket(cmd);
    ASSERT_NE(packet, nullptr);

    packet->Execute(*m_MockAPI);

    EXPECT_TRUE(m_MockAPI->HasCall("SetViewport"));
    const auto& calls = m_MockAPI->GetRecordedCalls();
    ASSERT_GE(calls.size(), 1u);

    // Find the SetViewport call
    for (const auto& call : calls)
    {
        if (call.Name == "SetViewport")
        {
            EXPECT_EQ(call.ParamU32_2, 1920u);
            EXPECT_EQ(call.ParamU32_3, 1080u);
            break;
        }
    }
}

TEST_F(CommandDispatchIntegration, ClearDispatch)
{
    auto cmd = MakeSyntheticClearCommand(true, true);
    auto* packet = m_Allocator->CreateCommandPacket(cmd);
    ASSERT_NE(packet, nullptr);

    packet->Execute(*m_MockAPI);

    // Clear command should trigger Clear or ClearColorAndDepth depending on implementation
    EXPECT_GE(m_MockAPI->GetCallCount(), 1u);
}

TEST_F(CommandDispatchIntegration, SetDepthTestDispatch)
{
    auto cmd = MakeSyntheticDepthTestCommand(true);
    auto* packet = m_Allocator->CreateCommandPacket(cmd);
    ASSERT_NE(packet, nullptr);

    packet->Execute(*m_MockAPI);

    EXPECT_TRUE(m_MockAPI->HasCall("SetDepthTest"));
}

// =============================================================================
// Bucket Execute Dispatches in Order
// =============================================================================

TEST_F(CommandDispatchIntegration, BucketExecuteDispatchesInOrder)
{
    CommandBucket bucket;

    auto viewport = MakeSyntheticViewportCommand(0, 0, 1920, 1080);
    auto clear = MakeSyntheticClearCommand();
    auto depthTest = MakeSyntheticDepthTestCommand(true);

    // Submit in specific order with execution order metadata
    PacketMetadata meta0, meta1, meta2;
    meta0.m_ExecutionOrder = 0;
    meta0.m_DependsOnPrevious = true;
    meta1.m_ExecutionOrder = 1;
    meta1.m_DependsOnPrevious = true;
    meta2.m_ExecutionOrder = 2;
    meta2.m_DependsOnPrevious = true;

    bucket.Submit(viewport, meta0, m_Allocator.get());
    bucket.Submit(clear, meta1, m_Allocator.get());
    bucket.Submit(depthTest, meta2, m_Allocator.get());

    bucket.SortCommands();
    bucket.Execute(*m_MockAPI);

    const auto& calls = m_MockAPI->GetRecordedCalls();
    ASSERT_GE(calls.size(), 3u)
        << "Expected at least 3 API calls from 3 commands";

    // Verify execution order: viewport first, then clear, then depth test
    sizet viewportIdx = SIZE_MAX;
    sizet clearIdx = SIZE_MAX;
    sizet depthTestIdx = SIZE_MAX;
    for (sizet i = 0; i < calls.size(); ++i)
    {
        if (calls[i].Name == "SetViewport" && viewportIdx == SIZE_MAX)
            viewportIdx = i;
        else if ((calls[i].Name == "Clear" || calls[i].Name == "ClearColorAndDepth") && clearIdx == SIZE_MAX)
            clearIdx = i;
        else if (calls[i].Name == "SetDepthTest" && depthTestIdx == SIZE_MAX)
            depthTestIdx = i;
    }

    EXPECT_NE(viewportIdx, SIZE_MAX) << "SetViewport call not found";
    EXPECT_NE(clearIdx, SIZE_MAX) << "Clear call not found";
    EXPECT_NE(depthTestIdx, SIZE_MAX) << "SetDepthTest call not found";

    if (viewportIdx != SIZE_MAX && clearIdx != SIZE_MAX && depthTestIdx != SIZE_MAX)
    {
        EXPECT_LT(viewportIdx, clearIdx)
            << "SetViewport should execute before Clear";
        EXPECT_LT(clearIdx, depthTestIdx)
            << "Clear should execute before SetDepthTest";
    }
}

// =============================================================================
// POD Render State Application
// =============================================================================

TEST(CommandDispatch, PODRenderStateFieldCount)
{
    // Verify PODRenderState has all expected fields by constructing one
    PODRenderState state;
    state.blendEnabled = true;
    state.depthTestEnabled = false;
    state.stencilEnabled = true;
    state.cullingEnabled = true;
    state.polygonOffsetEnabled = true;
    state.scissorEnabled = true;
    state.multisamplingEnabled = false;

    EXPECT_TRUE(state.blendEnabled);
    EXPECT_FALSE(state.depthTestEnabled);
    EXPECT_TRUE(state.stencilEnabled);
    EXPECT_TRUE(state.cullingEnabled);
    EXPECT_TRUE(state.polygonOffsetEnabled);
    EXPECT_TRUE(state.scissorEnabled);
    EXPECT_FALSE(state.multisamplingEnabled);
}
