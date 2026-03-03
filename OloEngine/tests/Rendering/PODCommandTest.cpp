#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

#include <cstring>
#include <type_traits>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// Trivially Copyable Static Assertions (runtime confirmation via memcpy)
// =============================================================================

template<typename T>
void AssertTriviallyCopyableByMemcpy(const T& original, const char* typeName)
{
    static_assert(std::is_trivially_copyable_v<T>);

    T copy{};
    std::memcpy(&copy, &original, sizeof(T));

    // Verify the header survived the copy
    EXPECT_EQ(copy.header.type, original.header.type)
        << typeName << ": header.type corrupted by memcpy";
}

TEST(PODCommand, AllCommandsTrivialCopy)
{
    {
        SetViewportCommand cmd{};
        cmd.header.type = CommandType::SetViewport;
        cmd.x = 10;
        cmd.y = 20;
        cmd.width = 1920;
        cmd.height = 1080;
        AssertTriviallyCopyableByMemcpy(cmd, "SetViewportCommand");
    }
    {
        ClearCommand cmd{};
        cmd.header.type = CommandType::Clear;
        cmd.clearColor = true;
        cmd.clearDepth = true;
        AssertTriviallyCopyableByMemcpy(cmd, "ClearCommand");
    }
    {
        ClearStencilCommand cmd{};
        cmd.header.type = CommandType::ClearStencil;
        AssertTriviallyCopyableByMemcpy(cmd, "ClearStencilCommand");
    }
    {
        SetClearColorCommand cmd{};
        cmd.header.type = CommandType::SetClearColor;
        cmd.color = glm::vec4(0.2f, 0.3f, 0.4f, 1.0f);
        AssertTriviallyCopyableByMemcpy(cmd, "SetClearColorCommand");
    }
    {
        SetDepthTestCommand cmd{};
        cmd.header.type = CommandType::SetDepthTest;
        cmd.enabled = true;
        AssertTriviallyCopyableByMemcpy(cmd, "SetDepthTestCommand");
    }
    {
        SetBlendStateCommand cmd{};
        cmd.header.type = CommandType::SetBlendState;
        cmd.enabled = true;
        AssertTriviallyCopyableByMemcpy(cmd, "SetBlendStateCommand");
    }
    {
        DrawMeshCommand cmd = MakeSyntheticDrawMeshCommand(10, 20, 5.0f, 100);
        AssertTriviallyCopyableByMemcpy(cmd, "DrawMeshCommand");
    }
    {
        DrawSkyboxCommand cmd{};
        cmd.header.type = CommandType::DrawSkybox;
        cmd.vertexArrayID = 1;
        cmd.indexCount = 36;
        cmd.transform = glm::mat4(1.0f);
        cmd.shaderRendererID = 5;
        cmd.skyboxTextureID = 10;
        AssertTriviallyCopyableByMemcpy(cmd, "DrawSkyboxCommand");
    }
    {
        DrawTerrainPatchCommand cmd{};
        cmd.header.type = CommandType::DrawTerrainPatch;
        cmd.vertexArrayID = 1;
        cmd.indexCount = 1024;
        AssertTriviallyCopyableByMemcpy(cmd, "DrawTerrainPatchCommand");
    }
    {
        DrawVoxelMeshCommand cmd{};
        cmd.header.type = CommandType::DrawVoxelMesh;
        cmd.vertexArrayID = 1;
        cmd.indexCount = 500;
        AssertTriviallyCopyableByMemcpy(cmd, "DrawVoxelMeshCommand");
    }
    {
        DrawDecalCommand cmd{};
        cmd.header.type = CommandType::DrawDecal;
        cmd.vertexArrayID = 1;
        cmd.indexCount = 36;
        AssertTriviallyCopyableByMemcpy(cmd, "DrawDecalCommand");
    }
    {
        DrawFoliageLayerCommand cmd{};
        cmd.header.type = CommandType::DrawFoliageLayer;
        cmd.vertexArrayID = 1;
        cmd.instanceCount = 1000;
        AssertTriviallyCopyableByMemcpy(cmd, "DrawFoliageLayerCommand");
    }
}

// =============================================================================
// Command Size Bounds
// =============================================================================

TEST(PODCommand, CommandSizeBound)
{
    EXPECT_LE(sizeof(SetViewportCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(ClearCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(ClearStencilCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetClearColorCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetBlendStateCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetBlendFuncCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetBlendEquationCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetDepthTestCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetDepthMaskCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetDepthFuncCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetStencilTestCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetStencilFuncCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetStencilMaskCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetStencilOpCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetCullingCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetCullFaceCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetLineWidthCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetPolygonModeCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetPolygonOffsetCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetScissorTestCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetScissorBoxCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetColorMaskCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(SetMultisamplingCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawIndexedCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawIndexedInstancedCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawArraysCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawLinesCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawMeshCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawMeshInstancedCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawSkyboxCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawInfiniteGridCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawQuadCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawTerrainPatchCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawVoxelMeshCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawDecalCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(DrawFoliageLayerCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(BindTextureCommand), MAX_COMMAND_SIZE);
    EXPECT_LE(sizeof(BindDefaultFramebufferCommand), MAX_COMMAND_SIZE);
}

// =============================================================================
// Synthetic Field Round-Trip via memcpy
// =============================================================================

TEST(PODCommand, DrawMeshFieldRoundTrip)
{
    DrawMeshCommand original = MakeSyntheticDrawMeshCommand(42, 99, 10.0f, 777);

    DrawMeshCommand copy{};
    std::memcpy(&copy, &original, sizeof(DrawMeshCommand));

    EXPECT_EQ(copy.header.type, CommandType::DrawMesh);
    EXPECT_EQ(copy.vertexArrayID, 1u);
    EXPECT_EQ(copy.indexCount, 36u);
    EXPECT_EQ(copy.entityID, 777);
    EXPECT_EQ(copy.shaderRendererID, 42u);
    EXPECT_FLOAT_EQ(copy.shininess, 32.0f);
    EXPECT_EQ(copy.useTextureMaps, false);
    EXPECT_EQ(copy.diffuseMapID, 99u);

    ValidateTransform(copy.transform);
    ValidateVec3(copy.ambient, "ambient");
    ValidateVec3(copy.diffuse, "diffuse");
    ValidateVec3(copy.specular, "specular");
}

// =============================================================================
// Zero-Init Produces No NaN
// =============================================================================

TEST(PODCommand, ZeroInitNoNaN)
{
    DrawMeshCommand cmd{};
    ValidateTransform(cmd.transform);
    ValidateVec3(cmd.ambient, "ambient");
    ValidateVec3(cmd.diffuse, "diffuse");
    ValidateVec3(cmd.specular, "specular");

    DrawSkyboxCommand skybox{};
    ValidateTransform(skybox.transform);

    DrawQuadCommand quad{};
    ValidateTransform(quad.transform);

    DrawTerrainPatchCommand terrain{};
    ValidateTransform(terrain.transform);

    DrawDecalCommand decal{};
    ValidateTransform(decal.decalTransform);
    ValidateTransform(decal.inverseDecalTransform);
}

// =============================================================================
// PODRenderState is Trivially Copyable
// =============================================================================

TEST(PODCommand, PODRenderStateTrivialCopy)
{
    PODRenderState original{};
    original.blendEnabled = true;
    original.depthTestEnabled = false;
    original.cullingEnabled = true;
    original.lineWidth = 2.5f;

    PODRenderState copy{};
    std::memcpy(&copy, &original, sizeof(PODRenderState));

    EXPECT_EQ(copy.blendEnabled, true);
    EXPECT_EQ(copy.depthTestEnabled, false);
    EXPECT_EQ(copy.cullingEnabled, true);
    EXPECT_FLOAT_EQ(copy.lineWidth, 2.5f);
}

// =============================================================================
// PODRenderState Defaults
// =============================================================================

TEST(PODCommand, PODRenderStateDefaults)
{
    PODRenderState state{};
    EXPECT_FALSE(state.blendEnabled);
    EXPECT_TRUE(state.depthTestEnabled);
    EXPECT_TRUE(state.depthWriteMask);
    EXPECT_FALSE(state.stencilEnabled);
    EXPECT_FALSE(state.cullingEnabled);
    EXPECT_FALSE(state.polygonOffsetEnabled);
    EXPECT_FALSE(state.scissorEnabled);
    EXPECT_TRUE(state.colorMaskR);
    EXPECT_TRUE(state.colorMaskG);
    EXPECT_TRUE(state.colorMaskB);
    EXPECT_TRUE(state.colorMaskA);
    EXPECT_TRUE(state.multisamplingEnabled);
    EXPECT_FLOAT_EQ(state.lineWidth, 1.0f);
}

// =============================================================================
// CommandType Enum Coverage
// =============================================================================

TEST(PODCommand, CommandTypeToStringCoverage)
{
    // Every CommandType enum value should have a string representation
    for (u8 i = 0; i < static_cast<u8>(CommandType::COUNT); ++i)
    {
        auto type = static_cast<CommandType>(i);
        const char* str = CommandTypeToString(type);
        EXPECT_NE(str, nullptr) << "CommandType " << static_cast<int>(type) << " has null string";

        if (type != CommandType::Invalid)
        {
            EXPECT_STRNE(str, "Unknown")
                << "CommandType " << static_cast<int>(type) << " maps to 'Unknown'";
        }
    }
}

// =============================================================================
// CommandHeader Default
// =============================================================================

TEST(PODCommand, CommandHeaderDefault)
{
    CommandHeader header{};
    EXPECT_EQ(header.type, CommandType::Invalid);
    EXPECT_EQ(header.dispatchFn, nullptr);
}
