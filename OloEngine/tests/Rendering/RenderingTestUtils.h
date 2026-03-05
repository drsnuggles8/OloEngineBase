#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Frustum.h"

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test utility header

// =============================================================================
// Validation Helpers
// =============================================================================

/// Asserts no NaN or Inf in any element of a 4x4 matrix.
/// Optionally checks that the determinant is non-zero (invertible).
inline void ValidateTransform(const glm::mat4& m, bool checkInvertible = false)
{
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            EXPECT_FALSE(std::isnan(m[col][row]))
                << "NaN at [" << col << "][" << row << "]";
            EXPECT_FALSE(std::isinf(m[col][row]))
                << "Inf at [" << col << "][" << row << "]";
        }
    }
    if (checkInvertible)
    {
        float det = glm::determinant(m);
        EXPECT_NE(det, 0.0f) << "Transform matrix is singular (determinant == 0)";
    }
}

/// Asserts no NaN or Inf in a vec3.
inline void ValidateVec3(const glm::vec3& v, const char* label = "vec3")
{
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_FALSE(std::isnan(v[i])) << label << "[" << i << "] is NaN";
        EXPECT_FALSE(std::isinf(v[i])) << label << "[" << i << "] is Inf";
    }
}

/// Asserts no NaN or Inf in a vec4.
inline void ValidateVec4(const glm::vec4& v, const char* label = "vec4")
{
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_FALSE(std::isnan(v[i])) << label << "[" << i << "] is NaN";
        EXPECT_FALSE(std::isinf(v[i])) << label << "[" << i << "] is Inf";
    }
}

// =============================================================================
// Synthetic DrawKey Factories
// =============================================================================

/// Create an opaque DrawKey with known field values.
inline DrawKey MakeSyntheticOpaqueKey(u32 viewportID = 0,
                                      ViewLayerType viewLayer = ViewLayerType::ThreeD,
                                      u32 shaderID = 1,
                                      u32 materialID = 1,
                                      u32 depth = 100)
{
    return DrawKey::CreateOpaque(viewportID, viewLayer, shaderID, materialID, depth);
}

/// Create a transparent DrawKey with known field values.
inline DrawKey MakeSyntheticTransparentKey(u32 viewportID = 0,
                                           ViewLayerType viewLayer = ViewLayerType::ThreeD,
                                           u32 shaderID = 1,
                                           u32 materialID = 1,
                                           u32 depth = 100)
{
    return DrawKey::CreateTransparent(viewportID, viewLayer, shaderID, materialID, depth);
}

/// Create a custom-priority DrawKey.
inline DrawKey MakeSyntheticCustomKey(u32 viewportID = 0,
                                      ViewLayerType viewLayer = ViewLayerType::ThreeD,
                                      u32 priority = 0)
{
    return DrawKey::CreateCustom(viewportID, viewLayer, priority);
}

// =============================================================================
// Synthetic Command Factories
// =============================================================================

/// Create a DrawMeshCommand with known, valid POD values.
inline DrawMeshCommand MakeSyntheticDrawMeshCommand(u32 shaderID = 1,
                                                    u32 materialID = 1,
                                                    f32 depth = 0.5f,
                                                    i32 entityID = 42)
{
    DrawMeshCommand cmd{};
    cmd.header.type = CommandType::DrawMesh;
    cmd.header.dispatchFn = nullptr; // Tests don't dispatch
    cmd.meshHandle = UUID(0);        // Deterministic handle for batching tests
    cmd.vertexArrayID = 1;
    cmd.indexCount = 36;
    cmd.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -depth));
    cmd.entityID = entityID;
    cmd.shaderHandle = static_cast<AssetHandle>(shaderID);
    cmd.materialDataIndex = static_cast<u16>(materialID); // Used as material identity for batching tests
    return cmd;
}

/// Create a ClearCommand.
inline ClearCommand MakeSyntheticClearCommand(bool color = true, bool depth = true)
{
    ClearCommand cmd{};
    cmd.header.type = CommandType::Clear;
    cmd.header.dispatchFn = nullptr;
    cmd.clearColor = color;
    cmd.clearDepth = depth;
    return cmd;
}

/// Create a SetViewportCommand.
inline SetViewportCommand MakeSyntheticViewportCommand(u32 x = 0, u32 y = 0,
                                                       u32 w = 1920, u32 h = 1080)
{
    SetViewportCommand cmd{};
    cmd.header.type = CommandType::SetViewport;
    cmd.header.dispatchFn = nullptr;
    cmd.x = x;
    cmd.y = y;
    cmd.width = w;
    cmd.height = h;
    return cmd;
}

/// Create a SetDepthTestCommand.
inline SetDepthTestCommand MakeSyntheticDepthTestCommand(bool enabled = true)
{
    SetDepthTestCommand cmd{};
    cmd.header.type = CommandType::SetDepthTest;
    cmd.header.dispatchFn = nullptr;
    cmd.enabled = enabled;
    return cmd;
}

// =============================================================================
// Sort Verification
// =============================================================================

/// Verify that a sequence of DrawKeys is sorted in ascending raw key order
/// (matching the radix sort output: lower raw keys first).
inline void ExpectCommandOrder(const std::vector<DrawKey>& keys)
{
    for (sizet i = 1; i < keys.size(); ++i)
    {
        // Radix sort produces ascending raw key order.
        // Lower raw key values come first in the sorted sequence.
        EXPECT_LE(keys[i - 1].GetKey(), keys[i].GetKey())
            << "Keys out of order at index " << i
            << ": key[" << (i - 1) << "]=0x" << std::hex << keys[i - 1].GetKey()
            << " key[" << i << "]=0x" << keys[i].GetKey() << std::dec;
    }
}

// =============================================================================
// State Change Counting
// =============================================================================

/// Count shader ID transitions in a sequence of DrawMeshCommand-compatible keys.
inline u32 CountShaderChanges(const std::vector<DrawKey>& keys)
{
    if (keys.empty())
        return 0;

    u32 changes = 0;
    u32 lastShader = keys[0].GetShaderID();
    for (sizet i = 1; i < keys.size(); ++i)
    {
        u32 currentShader = keys[i].GetShaderID();
        if (currentShader != lastShader)
        {
            changes++;
            lastShader = currentShader;
        }
    }
    return changes;
}

// =============================================================================
// Diagnostic Helpers
// =============================================================================

/// Print the bit-field breakdown of a DrawKey for failure diagnostics.
inline std::string PrintKeyBits(const DrawKey& key)
{
    std::ostringstream oss;
    oss << "DrawKey{raw=0x" << std::hex << key.GetKey() << std::dec
        << " viewport=" << key.GetViewportID()
        << " layer=" << static_cast<int>(key.GetViewLayer())
        << " mode=" << static_cast<int>(key.GetRenderMode())
        << " shader=" << key.GetShaderID()
        << " material=" << key.GetMaterialID()
        << " depth=" << key.GetDepth()
        << "}";
    return oss.str();
}

/// Deterministic random engine for reproducible tests.
inline std::mt19937& GetTestRNG()
{
    static std::mt19937 rng(42); // Fixed seed for reproducibility
    return rng;
}
