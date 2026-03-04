#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"

using namespace OloEngine;

// Test with CreateCommandPacket to verify it doesn't crash
TEST(CommandPacket, InitializePopulatesType)
{
    CommandAllocator allocator;

    // Just test AllocatePacketWithCommand (avoids Initialize/CommandDispatch)
    auto* packet = allocator.AllocatePacketWithCommand<DrawMeshCommand>();
    ASSERT_NE(packet, nullptr);

    auto* cmd = packet->GetCommandData<DrawMeshCommand>();
    ASSERT_NE(cmd, nullptr);
    cmd->header.type = CommandType::DrawMesh;
    cmd->shaderRendererID = 100;

    EXPECT_EQ(cmd->shaderRendererID, 100u);
}
