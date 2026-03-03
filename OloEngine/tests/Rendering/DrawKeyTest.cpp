#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// Bitwise Round-Trip Tests
// =============================================================================

TEST(DrawKey, BitwiseRoundTrip_Opaque)
{
    constexpr u32 viewportID = 3;
    constexpr auto viewLayer = ViewLayerType::TwoD;
    constexpr u32 shaderID = 0x1234;
    constexpr u32 materialID = 0xABCD;
    constexpr u32 depth = 0x567890;

    DrawKey key = DrawKey::CreateOpaque(viewportID, viewLayer, shaderID, materialID, depth);

    EXPECT_EQ(key.GetViewportID(), viewportID) << PrintKeyBits(key);
    EXPECT_EQ(key.GetViewLayer(), viewLayer) << PrintKeyBits(key);
    EXPECT_EQ(key.GetRenderMode(), RenderMode::Opaque) << PrintKeyBits(key);
    EXPECT_EQ(key.GetShaderID(), shaderID) << PrintKeyBits(key);
    EXPECT_EQ(key.GetMaterialID(), materialID) << PrintKeyBits(key);
    EXPECT_EQ(key.GetDepth(), depth) << PrintKeyBits(key);
}

TEST(DrawKey, BitwiseRoundTrip_Transparent)
{
    constexpr u32 viewportID = 5;
    constexpr auto viewLayer = ViewLayerType::ThreeD;
    constexpr u32 shaderID = 42;
    constexpr u32 materialID = 99;
    constexpr u32 depth = 500;

    DrawKey key = DrawKey::CreateTransparent(viewportID, viewLayer, shaderID, materialID, depth);

    EXPECT_EQ(key.GetViewportID(), viewportID) << PrintKeyBits(key);
    EXPECT_EQ(key.GetViewLayer(), viewLayer) << PrintKeyBits(key);
    EXPECT_EQ(key.GetRenderMode(), RenderMode::Transparent) << PrintKeyBits(key);
    EXPECT_EQ(key.GetShaderID(), shaderID) << PrintKeyBits(key);
    EXPECT_EQ(key.GetMaterialID(), materialID) << PrintKeyBits(key);
    // Transparent inverts depth: stored = 0xFFFFFF - depth
    EXPECT_EQ(key.GetDepth(), 0xFFFFFFu - depth) << PrintKeyBits(key);
}

TEST(DrawKey, BitwiseRoundTrip_Custom)
{
    constexpr u32 viewportID = 2;
    constexpr auto viewLayer = ViewLayerType::UI;
    constexpr u32 priority = 12345;

    DrawKey key = DrawKey::CreateCustom(viewportID, viewLayer, priority);

    EXPECT_EQ(key.GetViewportID(), viewportID) << PrintKeyBits(key);
    EXPECT_EQ(key.GetViewLayer(), viewLayer) << PrintKeyBits(key);
    EXPECT_EQ(key.GetRenderMode(), RenderMode::Opaque) << PrintKeyBits(key);
    EXPECT_EQ(key.GetPriority(), priority) << PrintKeyBits(key);
}

// =============================================================================
// Opaque Before Transparent — Render Mode Ordering
// =============================================================================

TEST(DrawKey, OpaqueBeforeTransparent)
{
    // For any shader/material/depth, opaque keys must sort before transparent keys.
    // operator< returns m_Key < other.m_Key (ascending raw key order, matching radix sort).
    // RenderMode bits [57:56]: Opaque=0, Transparent=1.
    // Opaque has lower RenderMode value → lower raw key → sorts first → rendered first.
    // This is the correct rendering convention: opaque geometry before transparent.

    auto& rng = GetTestRNG();
    std::uniform_int_distribution<u32> shaderDist(1, 100);
    std::uniform_int_distribution<u32> materialDist(1, 100);
    std::uniform_int_distribution<u32> depthDist(0, 0xFFFF);

    for (int trial = 0; trial < 100; ++trial)
    {
        u32 shader = shaderDist(rng);
        u32 material = materialDist(rng);
        u32 depth = depthDist(rng);

        DrawKey opaque = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shader, material, depth);
        DrawKey transparent = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shader, material, depth);

        std::vector<DrawKey> keys = { transparent, opaque };
        std::sort(keys.begin(), keys.end());

        // Opaque (lower raw key) must sort before transparent
        EXPECT_EQ(keys[0].GetRenderMode(), RenderMode::Opaque)
            << "Trial " << trial
            << ": Opaque must sort before transparent (lower RenderMode bits)"
            << "\n  opaque:      " << PrintKeyBits(opaque)
            << "\n  transparent: " << PrintKeyBits(transparent);
    }
}

// =============================================================================
// Opaque Depth: Front-to-Back (lower depth = higher priority)
// =============================================================================

TEST(DrawKey, OpaqueDepthFrontToBack)
{
    // For opaque rendering, nearer objects (lower depth) should sort before farther ones
    // to maximise early-z rejection and reduce overdraw.
    // With ascending sort (operator< returns m_Key < other), lower depth value = lower
    // raw key = sorted FIRST → front-to-back. This is the correct opaque convention.

    DrawKey nearKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 1, 1, 100);
    DrawKey farKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 1, 1, 1000);

    std::vector<DrawKey> keys = { farKey, nearKey };
    std::sort(keys.begin(), keys.end());

    // Near (depth=100) has lower raw key → sorts first → front-to-back
    EXPECT_EQ(keys[0].GetDepth(), 100u)
        << "Near object (lower depth) should sort first for opaque front-to-back"
        << "\n  nearKey: " << PrintKeyBits(nearKey)
        << "\n  farKey:  " << PrintKeyBits(farKey);
}

// =============================================================================
// Transparent Depth: Back-to-Front (farther depth = higher priority)
// =============================================================================

TEST(DrawKey, TransparentDepthBackToFront)
{
    // For transparent rendering, farther objects must sort before nearer ones
    // for correct alpha blending. CreateTransparent inverts depth (0xFFFFFF - depth).
    //
    // Near object: depth=100  → stored = 0xFFFFFF - 100  = 16777115 (high stored value)
    // Far object:  depth=1000 → stored = 0xFFFFFF - 1000 = 16776215 (lower stored value)
    //
    // Ascending sort puts lower stored value first → far object sorts first → back-to-front.
    // This is the correct transparent convention.

    DrawKey nearKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 1, 1, 100);
    DrawKey farKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 1, 1, 1000);

    std::vector<DrawKey> keys = { nearKey, farKey };
    std::sort(keys.begin(), keys.end());

    // Far object (lower stored depth) sorts first
    EXPECT_LT(farKey.GetDepth(), nearKey.GetDepth())
        << "Far object should have lower stored (inverted) depth";
    EXPECT_LT(farKey.GetKey(), nearKey.GetKey())
        << "Far object should have lower raw key → sorts first in ascending order";
    EXPECT_EQ(keys[0].GetKey(), farKey.GetKey())
        << "Far object must sort first for transparent back-to-front";
}

// =============================================================================
// Total Ordering — Strict Weak Ordering Property
// =============================================================================

TEST(DrawKey, TotalOrdering)
{
    auto& rng = GetTestRNG();
    std::uniform_int_distribution<u32> vpDist(0, 7);
    std::uniform_int_distribution<u32> layerDist(0, 3);
    std::uniform_int_distribution<u32> shaderDist(0, 0xFFFF);
    std::uniform_int_distribution<u32> matDist(0, 0xFFFF);
    std::uniform_int_distribution<u32> depthDist(0, 0xFFFFFF);

    std::vector<DrawKey> keys;
    keys.reserve(1000);

    for (int i = 0; i < 1000; ++i)
    {
        auto viewLayer = static_cast<ViewLayerType>(layerDist(rng));
        keys.push_back(DrawKey::CreateOpaque(
            vpDist(rng), viewLayer, shaderDist(rng), matDist(rng), depthDist(rng)));
    }

    // Sort should not throw or crash
    ASSERT_NO_FATAL_FAILURE(std::sort(keys.begin(), keys.end()));

    // Verify strict weak ordering: no adjacent pair violates the order
    for (sizet i = 1; i < keys.size(); ++i)
    {
        // After sort, keys[i-1] should NOT be "after" keys[i]
        EXPECT_FALSE(keys[i] < keys[i - 1])
            << "Strict weak ordering violated at index " << i
            << "\n  key[" << (i - 1) << "]: " << PrintKeyBits(keys[i - 1])
            << "\n  key[" << i << "]: " << PrintKeyBits(keys[i]);
    }
}

// =============================================================================
// Sort Stability — Equal Keys Preserve Insertion Order
// =============================================================================

TEST(DrawKey, SortStability)
{
    // Create 10 keys with identical values
    std::vector<std::pair<DrawKey, int>> keysWithIndex;
    for (int i = 0; i < 10; ++i)
    {
        keysWithIndex.emplace_back(
            DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 5, 10, 500), i);
    }

    // Stable sort should preserve insertion order for equal keys
    std::stable_sort(keysWithIndex.begin(), keysWithIndex.end(),
                     [](const auto& a, const auto& b)
                     { return a.first < b.first; });

    for (int i = 0; i < 10; ++i)
    {
        EXPECT_EQ(keysWithIndex[i].second, i)
            << "Stable sort did not preserve insertion order at index " << i;
    }
}

// =============================================================================
// All Field Combinations — Parametric Coverage
// =============================================================================

class DrawKeyFieldCombinations : public ::testing::TestWithParam<std::tuple<u32, ViewLayerType>>
{
};

TEST_P(DrawKeyFieldCombinations, PackingRoundTrip)
{
    auto [viewportID, viewLayer] = GetParam();
    constexpr u32 shaderID = 100;
    constexpr u32 materialID = 200;
    constexpr u32 depth = 5000;

    DrawKey key = DrawKey::CreateOpaque(viewportID, viewLayer, shaderID, materialID, depth);

    EXPECT_EQ(key.GetViewportID(), viewportID) << PrintKeyBits(key);
    EXPECT_EQ(key.GetViewLayer(), viewLayer) << PrintKeyBits(key);
    EXPECT_EQ(key.GetShaderID(), shaderID) << PrintKeyBits(key);
    EXPECT_EQ(key.GetMaterialID(), materialID) << PrintKeyBits(key);
    EXPECT_EQ(key.GetDepth(), depth) << PrintKeyBits(key);
}

INSTANTIATE_TEST_SUITE_P(
    ViewportLayerMatrix,
    DrawKeyFieldCombinations,
    ::testing::Combine(
        ::testing::Values(0u, 1u, 3u, 7u), // ViewportID (3 bits max = 7)
        ::testing::Values(ViewLayerType::ThreeD, ViewLayerType::TwoD,
                          ViewLayerType::UI, ViewLayerType::Skybox)));

// =============================================================================
// Mutator Independence
// =============================================================================

TEST(DrawKey, SettersAreIndependent)
{
    DrawKey key;
    key.SetViewportID(3);
    key.SetViewLayer(ViewLayerType::UI);
    key.SetRenderMode(RenderMode::Transparent);
    key.SetShaderID(0xAAAA);
    key.SetMaterialID(0xBBBB);
    key.SetDepth(0xCCCCCC);

    EXPECT_EQ(key.GetViewportID(), 3u);
    EXPECT_EQ(key.GetViewLayer(), ViewLayerType::UI);
    EXPECT_EQ(key.GetRenderMode(), RenderMode::Transparent);
    EXPECT_EQ(key.GetShaderID(), 0xAAAAu);
    EXPECT_EQ(key.GetMaterialID(), 0xBBBBu);
    EXPECT_EQ(key.GetDepth(), 0xCCCCCCu);

    // Modifying one field shouldn't affect others
    key.SetShaderID(0x1111);
    EXPECT_EQ(key.GetViewportID(), 3u) << "SetShaderID corrupted ViewportID";
    EXPECT_EQ(key.GetViewLayer(), ViewLayerType::UI) << "SetShaderID corrupted ViewLayer";
    EXPECT_EQ(key.GetRenderMode(), RenderMode::Transparent) << "SetShaderID corrupted RenderMode";
    EXPECT_EQ(key.GetShaderID(), 0x1111u);
    EXPECT_EQ(key.GetMaterialID(), 0xBBBBu) << "SetShaderID corrupted MaterialID";
    EXPECT_EQ(key.GetDepth(), 0xCCCCCCu) << "SetShaderID corrupted Depth";
}

// =============================================================================
// Default Key is Zero
// =============================================================================

TEST(DrawKey, DefaultIsZero)
{
    DrawKey key;
    EXPECT_EQ(key.GetKey(), 0u);
    EXPECT_EQ(key.GetViewportID(), 0u);
    EXPECT_EQ(key.GetViewLayer(), ViewLayerType::ThreeD);
    EXPECT_EQ(key.GetRenderMode(), RenderMode::Opaque);
    EXPECT_EQ(key.GetShaderID(), 0u);
    EXPECT_EQ(key.GetMaterialID(), 0u);
    EXPECT_EQ(key.GetDepth(), 0u);
}

// =============================================================================
// Explicit u64 Constructor
// =============================================================================

TEST(DrawKey, ExplicitU64Constructor)
{
    constexpr u64 rawKey = 0x123456789ABCDEF0ULL;
    DrawKey key(rawKey);
    EXPECT_EQ(key.GetKey(), rawKey);
    EXPECT_EQ(static_cast<u64>(key), rawKey);
}

// =============================================================================
// Equality and Inequality
// =============================================================================

TEST(DrawKey, EqualityOperator)
{
    DrawKey a = DrawKey::CreateOpaque(1, ViewLayerType::ThreeD, 10, 20, 30);
    DrawKey b = DrawKey::CreateOpaque(1, ViewLayerType::ThreeD, 10, 20, 30);
    DrawKey c = DrawKey::CreateOpaque(1, ViewLayerType::ThreeD, 10, 20, 31);

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

// =============================================================================
// Viewport Priority — Higher Viewport Sorts First
// =============================================================================

TEST(DrawKey, HigherViewportSortsLast)
{
    // Ascending sort: lower ViewportID = lower raw key = sorted first.
    // ViewportID is in the most-significant bits [63:61], so it dominates sorting.
    DrawKey vp0 = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 1, 1, 100);
    DrawKey vp7 = DrawKey::CreateOpaque(7, ViewLayerType::ThreeD, 1, 1, 100);

    std::vector<DrawKey> keys = { vp7, vp0 };
    std::sort(keys.begin(), keys.end());

    // Lower viewport ID sorts first in ascending order
    EXPECT_EQ(keys[0].GetViewportID(), 0u)
        << "Lower viewport ID should sort first in ascending key order"
        << "\n  vp0: " << PrintKeyBits(vp0)
        << "\n  vp7: " << PrintKeyBits(vp7);
}

// =============================================================================
// ViewLayer Ordering
// =============================================================================

TEST(DrawKey, ViewLayerOrderingWithinViewport)
{
    // Ascending sort: lower ViewLayer enum = lower raw key = sorted first.
    // ThreeD(0) < TwoD(1) < UI(2) < Skybox(3) in terms of raw key bits.
    DrawKey threeD = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 1, 1, 100);
    DrawKey twoD = DrawKey::CreateOpaque(0, ViewLayerType::TwoD, 1, 1, 100);
    DrawKey ui = DrawKey::CreateOpaque(0, ViewLayerType::UI, 1, 1, 100);
    DrawKey skybox = DrawKey::CreateOpaque(0, ViewLayerType::Skybox, 1, 1, 100);

    std::vector<DrawKey> keys = { twoD, skybox, threeD, ui };
    std::sort(keys.begin(), keys.end());

    // Lower ViewLayer enum value sorts first (ascending)
    EXPECT_EQ(keys[0].GetViewLayer(), ViewLayerType::ThreeD);
    EXPECT_EQ(keys[1].GetViewLayer(), ViewLayerType::TwoD);
    EXPECT_EQ(keys[2].GetViewLayer(), ViewLayerType::UI);
    EXPECT_EQ(keys[3].GetViewLayer(), ViewLayerType::Skybox);
}

// =============================================================================
// Shader Grouping — Same Shader Sorts Together
// =============================================================================

TEST(DrawKey, SameShaderGroupsTogether)
{
    std::vector<DrawKey> keys;
    // Interleave shader IDs 1-5
    for (u32 i = 0; i < 50; ++i)
    {
        u32 shader = (i % 5) + 1;
        keys.push_back(DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shader, 1, i));
    }

    std::sort(keys.begin(), keys.end());

    // After sorting, all commands with the same shader should be contiguous
    u32 prevShader = keys[0].GetShaderID();
    std::set<u32> seenShaders;
    seenShaders.insert(prevShader);

    for (sizet i = 1; i < keys.size(); ++i)
    {
        u32 currentShader = keys[i].GetShaderID();
        if (currentShader != prevShader)
        {
            // This shader should not have been seen before (contiguous grouping)
            EXPECT_EQ(seenShaders.count(currentShader), 0u)
                << "Shader " << currentShader << " appears non-contiguously at index " << i;
            seenShaders.insert(currentShader);
            prevShader = currentShader;
        }
    }
}

// =============================================================================
// ToString Utilities
// =============================================================================

TEST(DrawKey, ToStringViewLayerType)
{
    EXPECT_STREQ(ToString(ViewLayerType::ThreeD), "3D");
    EXPECT_STREQ(ToString(ViewLayerType::TwoD), "2D");
    EXPECT_STREQ(ToString(ViewLayerType::UI), "UI");
    EXPECT_STREQ(ToString(ViewLayerType::Skybox), "Skybox");
}

TEST(DrawKey, ToStringRenderMode)
{
    EXPECT_STREQ(ToString(RenderMode::Opaque), "Opaque");
    EXPECT_STREQ(ToString(RenderMode::Transparent), "Transparent");
    EXPECT_STREQ(ToString(RenderMode::Additive), "Additive");
    EXPECT_STREQ(ToString(RenderMode::Subtractive), "Subtractive");
}
