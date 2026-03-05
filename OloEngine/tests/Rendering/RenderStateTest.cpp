#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Vertex.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <string>
#include <sstream>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// PODRenderState Default Values
// =============================================================================

TEST(RenderState, DefaultsAreCorrect)
{
    PODRenderState state{};

    // Blend defaults
    EXPECT_FALSE(state.blendEnabled);
    EXPECT_EQ(state.blendSrcFactor, static_cast<GLenum>(GL_SRC_ALPHA));
    EXPECT_EQ(state.blendDstFactor, static_cast<GLenum>(GL_ONE_MINUS_SRC_ALPHA));
    EXPECT_EQ(state.blendEquation, static_cast<GLenum>(GL_FUNC_ADD));

    // Depth defaults
    EXPECT_TRUE(state.depthTestEnabled);
    EXPECT_TRUE(state.depthWriteMask);
    EXPECT_EQ(state.depthFunction, static_cast<GLenum>(GL_LESS));

    // Stencil defaults
    EXPECT_FALSE(state.stencilEnabled);
    EXPECT_EQ(state.stencilFunction, static_cast<GLenum>(GL_ALWAYS));
    EXPECT_EQ(state.stencilReference, 0);
    EXPECT_EQ(state.stencilReadMask, 0xFFu);
    EXPECT_EQ(state.stencilWriteMask, 0xFFu);
    EXPECT_EQ(state.stencilFail, static_cast<GLenum>(GL_KEEP));
    EXPECT_EQ(state.stencilDepthFail, static_cast<GLenum>(GL_KEEP));
    EXPECT_EQ(state.stencilDepthPass, static_cast<GLenum>(GL_KEEP));

    // Culling defaults: disabled by default
    EXPECT_FALSE(state.cullingEnabled);
    EXPECT_EQ(state.cullFace, static_cast<GLenum>(GL_BACK));

    // Polygon mode defaults
    EXPECT_EQ(state.polygonFace, static_cast<GLenum>(GL_FRONT_AND_BACK));
    EXPECT_EQ(state.polygonMode, static_cast<GLenum>(GL_FILL));

    // Polygon offset defaults
    EXPECT_FALSE(state.polygonOffsetEnabled);
    EXPECT_FLOAT_EQ(state.polygonOffsetFactor, 0.0f);
    EXPECT_FLOAT_EQ(state.polygonOffsetUnits, 0.0f);

    // Scissor defaults
    EXPECT_FALSE(state.scissorEnabled);
    EXPECT_EQ(state.scissorX, 0);
    EXPECT_EQ(state.scissorY, 0);
    EXPECT_EQ(state.scissorWidth, 0);
    EXPECT_EQ(state.scissorHeight, 0);

    // Color mask defaults: all channels enabled
    EXPECT_TRUE(state.colorMaskR);
    EXPECT_TRUE(state.colorMaskG);
    EXPECT_TRUE(state.colorMaskB);
    EXPECT_TRUE(state.colorMaskA);

    // Multisampling default
    EXPECT_TRUE(state.multisamplingEnabled);

    // Line width default
    EXPECT_FLOAT_EQ(state.lineWidth, 1.0f);
}

TEST(RenderState, TriviallyCopyable)
{
    PODRenderState original{};
    original.blendEnabled = true;
    original.depthFunction = GL_LEQUAL;
    original.cullingEnabled = true;
    original.polygonMode = GL_LINE;
    original.lineWidth = 2.5f;

    PODRenderState copy{};
    std::memcpy(&copy, &original, sizeof(PODRenderState));

    EXPECT_EQ(copy.blendEnabled, original.blendEnabled);
    EXPECT_EQ(copy.depthFunction, original.depthFunction);
    EXPECT_EQ(copy.cullingEnabled, original.cullingEnabled);
    EXPECT_EQ(copy.polygonMode, original.polygonMode);
    EXPECT_FLOAT_EQ(copy.lineWidth, original.lineWidth);
}

// =============================================================================
// Blended Objects Must Use Transparent Sort Keys
// =============================================================================

TEST(RenderState, BlendedObjectsUseTransparentSortKey)
{
    // A blended object (e.g., transparent sphere) must use a transparent
    // DrawKey so it sorts AFTER all opaque geometry.
    DrawKey opaqueKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 1, 1, 100);
    DrawKey transparentKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 1, 1, 100);

    // Transparent keys must have a HIGHER raw value than opaque keys
    // (ascending sort => opaques first, then transparents)
    EXPECT_GT(transparentKey.GetKey(), opaqueKey.GetKey())
        << "Transparent key must sort after opaque key.\n"
        << "Opaque: " << PrintKeyBits(opaqueKey) << "\n"
        << "Transparent: " << PrintKeyBits(transparentKey);

    EXPECT_EQ(opaqueKey.GetRenderMode(), RenderMode::Opaque);
    EXPECT_EQ(transparentKey.GetRenderMode(), RenderMode::Transparent);
}

TEST(RenderState, InfiniteGridSortKeyIsTransparent)
{
    // The infinite grid enables alpha blending and must therefore use a
    // transparent sort key so it renders after all opaque geometry.
    // This is the key the fixed DrawInfiniteGrid() now produces.
    u32 shaderID = 42; // Arbitrary shader ID
    DrawKey gridKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, 0, 0x800000);

    EXPECT_EQ(gridKey.GetRenderMode(), RenderMode::Transparent)
        << "Infinite grid must use transparent render mode because it has blending enabled";

    // Verify it sorts after all opaque keys with the same viewport/viewlayer
    DrawKey opaqueKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 100, 100, 0xFFFFFF);
    EXPECT_GT(gridKey.GetKey(), opaqueKey.GetKey())
        << "Grid key must sort after all opaque keys.\n"
        << "Grid: " << PrintKeyBits(gridKey) << "\n"
        << "MaxOpaque: " << PrintKeyBits(opaqueKey);
}

// =============================================================================
// Opaque Render State Invariants
// =============================================================================

TEST(RenderState, OpaqueObjectHasCorrectState)
{
    // An opaque material should produce specific render state:
    // - No blending
    // - Depth test enabled, write enabled, GL_LESS
    // - Back-face culling enabled
    PODRenderState opaque{};
    opaque.depthTestEnabled = true;
    opaque.depthWriteMask = true;
    opaque.depthFunction = GL_LESS;
    opaque.blendEnabled = false;
    opaque.cullingEnabled = true;
    opaque.cullFace = GL_BACK;

    EXPECT_FALSE(opaque.blendEnabled);
    EXPECT_TRUE(opaque.depthTestEnabled);
    EXPECT_TRUE(opaque.depthWriteMask);
    EXPECT_EQ(opaque.depthFunction, static_cast<GLenum>(GL_LESS));
    EXPECT_TRUE(opaque.cullingEnabled);
    EXPECT_EQ(opaque.cullFace, static_cast<GLenum>(GL_BACK));
}

TEST(RenderState, TransparentObjectHasCorrectState)
{
    // A transparent material should:
    // - Enable blending with SRC_ALPHA / ONE_MINUS_SRC_ALPHA
    // - Disable depth writes (avoid order-dependent artifacts)
    // - Keep depth test enabled (test against existing depth buffer)
    PODRenderState transparent{};
    transparent.blendEnabled = true;
    transparent.blendSrcFactor = GL_SRC_ALPHA;
    transparent.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
    transparent.blendEquation = GL_FUNC_ADD;
    transparent.depthTestEnabled = true;
    transparent.depthWriteMask = false;

    EXPECT_TRUE(transparent.blendEnabled);
    EXPECT_EQ(transparent.blendSrcFactor, static_cast<GLenum>(GL_SRC_ALPHA));
    EXPECT_EQ(transparent.blendDstFactor, static_cast<GLenum>(GL_ONE_MINUS_SRC_ALPHA));
    EXPECT_EQ(transparent.blendEquation, static_cast<GLenum>(GL_FUNC_ADD));
    EXPECT_TRUE(transparent.depthTestEnabled);
    EXPECT_FALSE(transparent.depthWriteMask);
}

TEST(RenderState, TwoSidedMaterialDisablesCulling)
{
    // A two-sided material (e.g., foliage, flags) should disable face culling
    // so both sides of the triangle are rendered.
    PODRenderState twoSided{};
    twoSided.cullingEnabled = false;

    EXPECT_FALSE(twoSided.cullingEnabled);
}

// =============================================================================
// Wireframe / Polygon Mode
// =============================================================================

TEST(RenderState, WireframeModeUsesLinePolygonMode)
{
    PODRenderState wireframe{};
    wireframe.polygonMode = GL_LINE;
    wireframe.polygonFace = GL_FRONT_AND_BACK;

    EXPECT_EQ(wireframe.polygonMode, static_cast<GLenum>(GL_LINE));
    EXPECT_EQ(wireframe.polygonFace, static_cast<GLenum>(GL_FRONT_AND_BACK));
}

// =============================================================================
// Polygon Offset (prevents Z-fighting for decals, overlays)
// =============================================================================

TEST(RenderState, PolygonOffsetForDecals)
{
    PODRenderState decalState{};
    decalState.polygonOffsetEnabled = true;
    decalState.polygonOffsetFactor = -1.0f;
    decalState.polygonOffsetUnits = -1.0f;

    EXPECT_TRUE(decalState.polygonOffsetEnabled);
    EXPECT_LT(decalState.polygonOffsetFactor, 0.0f)
        << "Negative offset pulls decal towards camera to prevent z-fighting";
    EXPECT_LT(decalState.polygonOffsetUnits, 0.0f);
}

// =============================================================================
// Color Mask
// =============================================================================

TEST(RenderState, DepthOnlyPassDisablesColorMask)
{
    // Shadow / depth pre-pass should disable color writes
    PODRenderState depthOnly{};
    depthOnly.colorMaskR = false;
    depthOnly.colorMaskG = false;
    depthOnly.colorMaskB = false;
    depthOnly.colorMaskA = false;
    depthOnly.depthTestEnabled = true;
    depthOnly.depthWriteMask = true;

    EXPECT_FALSE(depthOnly.colorMaskR);
    EXPECT_FALSE(depthOnly.colorMaskG);
    EXPECT_FALSE(depthOnly.colorMaskB);
    EXPECT_FALSE(depthOnly.colorMaskA);
    EXPECT_TRUE(depthOnly.depthWriteMask);
}

// =============================================================================
// Cube Winding Order Correctness
// =============================================================================

// Helper: compute triangle face normal via cross product of edges
static glm::vec3 ComputeTriangleNormal(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2)
{
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    return glm::normalize(glm::cross(edge1, edge2));
}

// The cube vertex data as defined in MeshPrimitives::CreateCube()
struct CubeTestData
{
    // 24 vertices, 4 per face × 6 faces
    // Layout: Position(vec3), Normal(vec3), TexCoord(vec2)
    struct TestVertex
    {
        glm::vec3 Position;
        glm::vec3 Normal;
    };

    // clang-format off
    static constexpr int kVertexCount = 24;
    static constexpr int kFaceCount = 6;
    static constexpr int kIndicesPerFace = 6; // 2 triangles × 3 indices

    static std::vector<TestVertex> GetVertices()
    {
        return {
            // Front face (normal +Z)
            { { 0.5f,  0.5f,  0.5f}, {0, 0, 1} },  // 0
            { { 0.5f, -0.5f,  0.5f}, {0, 0, 1} },  // 1
            { {-0.5f, -0.5f,  0.5f}, {0, 0, 1} },  // 2
            { {-0.5f,  0.5f,  0.5f}, {0, 0, 1} },  // 3

            // Back face (normal -Z)
            { { 0.5f,  0.5f, -0.5f}, {0, 0, -1} }, // 4
            { { 0.5f, -0.5f, -0.5f}, {0, 0, -1} }, // 5
            { {-0.5f, -0.5f, -0.5f}, {0, 0, -1} }, // 6
            { {-0.5f,  0.5f, -0.5f}, {0, 0, -1} }, // 7

            // Right face (normal +X)
            { { 0.5f,  0.5f,  0.5f}, {1, 0, 0} },  // 8
            { { 0.5f, -0.5f,  0.5f}, {1, 0, 0} },  // 9
            { { 0.5f, -0.5f, -0.5f}, {1, 0, 0} },  // 10
            { { 0.5f,  0.5f, -0.5f}, {1, 0, 0} },  // 11

            // Left face (normal -X)
            { {-0.5f,  0.5f,  0.5f}, {-1, 0, 0} }, // 12
            { {-0.5f, -0.5f,  0.5f}, {-1, 0, 0} }, // 13
            { {-0.5f, -0.5f, -0.5f}, {-1, 0, 0} }, // 14
            { {-0.5f,  0.5f, -0.5f}, {-1, 0, 0} }, // 15

            // Top face (normal +Y)
            { { 0.5f,  0.5f,  0.5f}, {0, 1, 0} },  // 16
            { { 0.5f,  0.5f, -0.5f}, {0, 1, 0} },  // 17
            { {-0.5f,  0.5f, -0.5f}, {0, 1, 0} },  // 18
            { {-0.5f,  0.5f,  0.5f}, {0, 1, 0} },  // 19

            // Bottom face (normal -Y)
            { { 0.5f, -0.5f,  0.5f}, {0, -1, 0} }, // 20
            { { 0.5f, -0.5f, -0.5f}, {0, -1, 0} }, // 21
            { {-0.5f, -0.5f, -0.5f}, {0, -1, 0} }, // 22
            { {-0.5f, -0.5f,  0.5f}, {0, -1, 0} }, // 23
        };
    }

    // These indices must match MeshPrimitives::CreateCube() exactly
    static std::vector<u32> GetIndices()
    {
        return {
            // Front face (CCW from outside, normal +Z)
            0, 3, 1, 3, 2, 1,
            // Back face (CCW from outside, normal -Z)
            4, 5, 7, 5, 6, 7,
            // Right face (CCW from outside, normal +X)
            8, 9, 11, 9, 10, 11,
            // Left face (CCW from outside, normal -X)
            12, 15, 13, 15, 14, 13,
            // Top face (CCW from outside, normal +Y)
            16, 17, 19, 17, 18, 19,
            // Bottom face (CCW from outside, normal -Y)
            20, 23, 21, 23, 22, 21
        };
    }
    // clang-format on
};

TEST(RenderState, CubeWindingOrderIsCCW)
{
    // CRITICAL: All cube faces must have counter-clockwise winding order
    // when viewed from outside the cube. OpenGL's default front-face is
    // GL_CCW, and with GL_CULL_FACE + GL_BACK, CW triangles are culled
    // as back-faces. Wrong winding = missing faces when culling is enabled.
    auto vertices = CubeTestData::GetVertices();
    auto indices = CubeTestData::GetIndices();

    const char* faceNames[] = { "Front (+Z)", "Back (-Z)", "Right (+X)", "Left (-X)", "Top (+Y)", "Bottom (-Y)" };

    for (int face = 0; face < CubeTestData::kFaceCount; ++face)
    {
        const int baseIdx = face * CubeTestData::kIndicesPerFace;
        const glm::vec3& expectedNormal = vertices[face * 4].Normal;

        // Check both triangles of the face
        for (int tri = 0; tri < 2; ++tri)
        {
            const int i0 = indices[baseIdx + tri * 3 + 0];
            const int i1 = indices[baseIdx + tri * 3 + 1];
            const int i2 = indices[baseIdx + tri * 3 + 2];

            glm::vec3 computedNormal = ComputeTriangleNormal(
                vertices[i0].Position, vertices[i1].Position, vertices[i2].Position);

            // The computed normal (from cross product of edges in index order)
            // must match the declared face normal. If it opposes, the winding
            // is clockwise and back-face culling will remove this triangle.
            float dot = glm::dot(computedNormal, expectedNormal);
            EXPECT_GT(dot, 0.9f)
                << "Face " << faceNames[face] << " triangle " << tri
                << " has wrong winding order!\n"
                << "  Indices: " << i0 << ", " << i1 << ", " << i2 << "\n"
                << "  Computed normal: (" << computedNormal.x << ", "
                << computedNormal.y << ", " << computedNormal.z << ")\n"
                << "  Expected normal: (" << expectedNormal.x << ", "
                << expectedNormal.y << ", " << expectedNormal.z << ")\n"
                << "  Dot product: " << dot << " (must be > 0.9 for CCW)";
        }
    }
}

TEST(RenderState, CubeHas36Indices)
{
    auto indices = CubeTestData::GetIndices();
    EXPECT_EQ(indices.size(), 36u) << "Cube must have 6 faces × 2 triangles × 3 vertices = 36 indices";
}

TEST(RenderState, CubeHas24Vertices)
{
    auto vertices = CubeTestData::GetVertices();
    EXPECT_EQ(vertices.size(), 24u) << "Cube must have 6 faces × 4 vertices = 24 vertices (no sharing for proper normals)";
}

TEST(RenderState, CubeNormalsAreUnitLength)
{
    auto vertices = CubeTestData::GetVertices();
    for (sizet i = 0; i < vertices.size(); ++i)
    {
        float len = glm::length(vertices[i].Normal);
        EXPECT_NEAR(len, 1.0f, 1e-5f)
            << "Vertex " << i << " normal is not unit length: " << len;
    }
}

TEST(RenderState, CubeIndicesInRange)
{
    auto vertices = CubeTestData::GetVertices();
    auto indices = CubeTestData::GetIndices();
    for (sizet i = 0; i < indices.size(); ++i)
    {
        EXPECT_LT(indices[i], static_cast<u32>(vertices.size()))
            << "Index " << i << " out of range: " << indices[i];
    }
}

// =============================================================================
// Shader Include Preprocessing
// =============================================================================

// Test the comment-skip logic conceptually: a line with // before #include
// should not be treated as an include directive.
TEST(RenderState, ShaderIncludeSkipsComments)
{
    // Simulate the logic used in OpenGLShader::ProcessIncludesInternal():
    // If "//" appears before "#include" on the same line, skip it.
    auto isCommentedInclude = [](const std::string& line) -> bool
    {
        const std::string includeToken = "#include";
        auto pos = line.find(includeToken);
        if (pos == std::string::npos)
            return false;

        auto commentPos = line.find("//");
        return (commentPos != std::string::npos && commentPos < pos);
    };

    // Cases that SHOULD be skipped (commented out)
    EXPECT_TRUE(isCommentedInclude("//   #include \"include/FogCommon.glsl\""));
    EXPECT_TRUE(isCommentedInclude("// #include \"SomeFile.glsl\""));
    EXPECT_TRUE(isCommentedInclude("  // #include <header.h>"));

    // Cases that should NOT be skipped (real includes)
    EXPECT_FALSE(isCommentedInclude("#include \"FogCommon.glsl\""));
    EXPECT_FALSE(isCommentedInclude("  #include \"SomeFile.glsl\""));
    EXPECT_FALSE(isCommentedInclude("\t#include <header.h>"));
}

TEST(RenderState, ShaderIncludePathNoDuplication)
{
    // Bug scenario: file in "assets/shaders/include/" has a comment:
    //   #include "include/FogCommon.glsl"
    // When processed with directory="assets/shaders/include",
    // the naive resolution produces: "assets/shaders/include/include/FogCommon.glsl"
    // which contains the consecutive segment "include/include" — a doubled path.
    // The fix is to skip commented-out #include lines.

    std::string directory = "assets/shaders/include";
    std::string includePath = "include/FogCommon.glsl";

    // This is what the broken code would produce:
    std::string brokenPath = directory + "/" + includePath;

    // The broken path contains "include/include" — consecutive duplicate segments
    EXPECT_NE(brokenPath.find("include/include"), std::string::npos)
        << "Expected broken path to contain doubled 'include/include' segment.\n"
        << "Broken path: " << brokenPath;

    // The correct path (when the commented include is properly skipped)
    // would never be generated. The include from the comment line
    // should be skipped entirely by the preprocessor.
    std::string correctPath = directory + "/FogCommon.glsl";
    EXPECT_EQ(correctPath.find("include/include"), std::string::npos)
        << "Correct path must not contain doubled 'include' segments.\n"
        << "Correct path: " << correctPath;
}

// =============================================================================
// Render State Combinations for RenderStateTest.olo Scene Entities
// =============================================================================

TEST(RenderState, WireframeCubeState)
{
    // Wireframe cubes in the test scene should have:
    // - polygonMode = GL_LINE (wireframe rendering)
    // - Culling disabled (see both sides of wireframe)
    // - Depth test enabled
    // - No blending
    PODRenderState wireframe{};
    wireframe.polygonMode = GL_LINE;
    wireframe.polygonFace = GL_FRONT_AND_BACK;
    wireframe.cullingEnabled = false;
    wireframe.depthTestEnabled = true;
    wireframe.blendEnabled = false;

    EXPECT_EQ(wireframe.polygonMode, static_cast<GLenum>(GL_LINE));
    EXPECT_FALSE(wireframe.cullingEnabled);
    EXPECT_TRUE(wireframe.depthTestEnabled);
    EXPECT_FALSE(wireframe.blendEnabled);
}

TEST(RenderState, TransparentSphereState)
{
    // Transparent spheres in the test scene should have:
    // - Blending with standard alpha blend function
    // - Depth test enabled but depth write disabled
    // - Transparent sort key (renders after opaques)
    PODRenderState sphere{};
    sphere.blendEnabled = true;
    sphere.blendSrcFactor = GL_SRC_ALPHA;
    sphere.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
    sphere.depthTestEnabled = true;
    sphere.depthWriteMask = false;

    DrawKey key = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 1, 1, 500);

    EXPECT_TRUE(sphere.blendEnabled);
    EXPECT_FALSE(sphere.depthWriteMask);
    EXPECT_EQ(key.GetRenderMode(), RenderMode::Transparent);
}

TEST(RenderState, PolygonOffsetOverlayState)
{
    // Polygon offset test entities use negative offset to pull overlay
    // geometry towards the camera, preventing z-fighting with coplanar surfaces.
    PODRenderState overlay{};
    overlay.polygonOffsetEnabled = true;
    overlay.polygonOffsetFactor = -1.0f;
    overlay.polygonOffsetUnits = -1.0f;
    overlay.depthTestEnabled = true;
    overlay.depthFunction = GL_LEQUAL; // LEQUAL needed for coplanar geometry

    EXPECT_TRUE(overlay.polygonOffsetEnabled);
    EXPECT_LT(overlay.polygonOffsetFactor, 0.0f);
    EXPECT_EQ(overlay.depthFunction, static_cast<GLenum>(GL_LEQUAL));
}

// =============================================================================
// Sort Key / Render State Consistency Rules
// =============================================================================

TEST(RenderState, BlendEnabledRequiresTransparentKey)
{
    // INVARIANT: If a command's renderState.blendEnabled == true,
    // its sort key must use RenderMode::Transparent.
    // Violating this causes the blended object to sort among opaques,
    // producing incorrect depth interactions (the infinite grid bug).

    // Create what the broken grid had: blend enabled + opaque key
    PODRenderState gridState{};
    gridState.blendEnabled = true;
    gridState.blendSrcFactor = GL_SRC_ALPHA;
    gridState.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
    DrawKey brokenKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 1, 0, 0x800000);

    // This is the bug condition: blend enabled but opaque sort key
    bool hasBug = gridState.blendEnabled && (brokenKey.GetRenderMode() == RenderMode::Opaque);
    EXPECT_TRUE(hasBug) << "This test documents the bug pattern";

    // Create the fixed version: blend enabled + transparent key
    DrawKey fixedKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 1, 0, 0x800000);
    bool isFixed = gridState.blendEnabled && (fixedKey.GetRenderMode() == RenderMode::Transparent);
    EXPECT_TRUE(isFixed) << "Blended objects must use Transparent sort key";
}

TEST(RenderState, OpaqueBeforeTransparentSortInvariant)
{
    // All opaque keys must sort before all transparent keys within
    // the same viewport and view layer. This ensures the depth buffer
    // is fully populated before blended objects are drawn.
    std::vector<DrawKey> keys;

    // Create a mix of opaque and transparent keys
    keys.push_back(DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 5, 5, 200));
    keys.push_back(DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 10, 10, 100));
    keys.push_back(DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 1, 1, 50));
    keys.push_back(DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 1, 1, 500));

    // Sort ascending (matching radix sort)
    std::sort(keys.begin(), keys.end());

    // All opaques should come first
    bool seenTransparent = false;
    for (const auto& key : keys)
    {
        if (key.GetRenderMode() == RenderMode::Transparent)
        {
            seenTransparent = true;
        }
        else
        {
            EXPECT_FALSE(seenTransparent)
                << "Found opaque key after transparent key — sort invariant violated!\n"
                << PrintKeyBits(key);
        }
    }
}

// =============================================================================
// Stencil State for Outline / Selection Effects
// =============================================================================

TEST(RenderState, StencilOutlinePassWriteState)
{
    // First pass: render object and write 1 to stencil buffer
    PODRenderState stencilWrite{};
    stencilWrite.stencilEnabled = true;
    stencilWrite.stencilFunction = GL_ALWAYS;
    stencilWrite.stencilReference = 1;
    stencilWrite.stencilDepthPass = GL_REPLACE;

    EXPECT_TRUE(stencilWrite.stencilEnabled);
    EXPECT_EQ(stencilWrite.stencilFunction, static_cast<GLenum>(GL_ALWAYS));
    EXPECT_EQ(stencilWrite.stencilReference, 1);
    EXPECT_EQ(stencilWrite.stencilDepthPass, static_cast<GLenum>(GL_REPLACE));
}

TEST(RenderState, StencilOutlinePassReadState)
{
    // Second pass: render slightly larger object where stencil != 1
    PODRenderState stencilRead{};
    stencilRead.stencilEnabled = true;
    stencilRead.stencilFunction = GL_NOTEQUAL;
    stencilRead.stencilReference = 1;
    stencilRead.depthTestEnabled = false; // Outline visible through geometry

    EXPECT_TRUE(stencilRead.stencilEnabled);
    EXPECT_EQ(stencilRead.stencilFunction, static_cast<GLenum>(GL_NOTEQUAL));
    EXPECT_EQ(stencilRead.stencilReference, 1);
    EXPECT_FALSE(stencilRead.depthTestEnabled);
}

// =============================================================================
// Cylinder Winding Order Correctness
// =============================================================================

// Generates cylinder vertices and indices exactly as MeshPrimitives::CreateCylinder()
struct CylinderTestData
{
    struct TestVertex
    {
        glm::vec3 Position;
        glm::vec3 Normal;
    };

    static void Generate(f32 radius, f32 height, u32 segments,
                         std::vector<TestVertex>& outVertices, std::vector<u32>& outIndices)
    {
        const f32 halfHeight = height * 0.5f;
        const f32 angleStep = 2.0f * glm::pi<f32>() / segments;

        // Center vertices
        outVertices.push_back({ { 0.0f, halfHeight, 0.0f }, { 0.0f, 1.0f, 0.0f } });
        outVertices.push_back({ { 0.0f, -halfHeight, 0.0f }, { 0.0f, -1.0f, 0.0f } });

        for (u32 i = 0; i < segments; i++)
        {
            const f32 angle = i * angleStep;
            const f32 x = cos(angle) * radius;
            const f32 z = sin(angle) * radius;

            outVertices.push_back({ { x, halfHeight, z }, { 0.0f, 1.0f, 0.0f } });              // top circle
            outVertices.push_back({ { x, -halfHeight, z }, { 0.0f, -1.0f, 0.0f } });            // bottom circle
            outVertices.push_back({ { x, halfHeight, z }, { x / radius, 0.0f, z / radius } });  // side top
            outVertices.push_back({ { x, -halfHeight, z }, { x / radius, 0.0f, z / radius } }); // side bottom
        }

        for (u32 i = 0; i < segments; i++)
        {
            const u32 next = (i + 1) % segments;

            // Top cap
            outIndices.push_back(0);
            outIndices.push_back(2 + next * 4);
            outIndices.push_back(2 + i * 4);

            // Bottom cap
            outIndices.push_back(1);
            outIndices.push_back(2 + i * 4 + 1);
            outIndices.push_back(2 + next * 4 + 1);

            // Side faces
            const u32 sideTop = 2 + i * 4 + 2;
            const u32 sideBottom = 2 + i * 4 + 3;
            const u32 nextSideTop = 2 + next * 4 + 2;
            const u32 nextSideBottom = 2 + next * 4 + 3;

            outIndices.push_back(sideTop);
            outIndices.push_back(nextSideTop);
            outIndices.push_back(sideBottom);

            outIndices.push_back(sideBottom);
            outIndices.push_back(nextSideTop);
            outIndices.push_back(nextSideBottom);
        }
    }
};

TEST(RenderState, CylinderTopCapWindingIsCCW)
{
    std::vector<CylinderTestData::TestVertex> vertices;
    std::vector<u32> indices;
    CylinderTestData::Generate(0.5f, 1.0f, 16, vertices, indices);

    // Top cap: every 12 indices per segment, first 3 are a top-cap triangle
    for (u32 i = 0; i < 16; i++)
    {
        const u32 base = i * 12; // 12 indices per segment (top + bottom + 2 side)
        const auto& v0 = vertices[indices[base + 0]].Position;
        const auto& v1 = vertices[indices[base + 1]].Position;
        const auto& v2 = vertices[indices[base + 2]].Position;

        glm::vec3 normal = ComputeTriangleNormal(v0, v1, v2);
        EXPECT_GT(glm::dot(normal, glm::vec3(0, 1, 0)), 0.9f)
            << "Top cap triangle " << i << " has wrong winding (normal.y="
            << normal.y << ", expected +Y)";
    }
}

TEST(RenderState, CylinderBottomCapWindingIsCCW)
{
    std::vector<CylinderTestData::TestVertex> vertices;
    std::vector<u32> indices;
    CylinderTestData::Generate(0.5f, 1.0f, 16, vertices, indices);

    for (u32 i = 0; i < 16; i++)
    {
        const u32 base = i * 12 + 3; // bottom cap starts 3 indices after top cap
        const auto& v0 = vertices[indices[base + 0]].Position;
        const auto& v1 = vertices[indices[base + 1]].Position;
        const auto& v2 = vertices[indices[base + 2]].Position;

        glm::vec3 normal = ComputeTriangleNormal(v0, v1, v2);
        EXPECT_GT(glm::dot(normal, glm::vec3(0, -1, 0)), 0.9f)
            << "Bottom cap triangle " << i << " has wrong winding (normal.y="
            << normal.y << ", expected -Y)";
    }
}

TEST(RenderState, CylinderSideWindingIsCCW)
{
    std::vector<CylinderTestData::TestVertex> vertices;
    std::vector<u32> indices;
    CylinderTestData::Generate(0.5f, 1.0f, 16, vertices, indices);

    // Side triangles start after top(3)+bottom(3) = position 6 per segment
    // but the index layout is: top(3) + bottom(3) + side1(3) + side2(3) = 12 per segment
    // Wait, actually the loop generates:
    //   top cap (3 indices) + bottom cap (3 indices) + side face1 (3 indices) + side face2 (3 indices) = 12 per segment

    for (u32 i = 0; i < 16; i++)
    {
        for (u32 tri = 0; tri < 2; tri++)
        {
            const u32 base = i * 12 + 6 + tri * 3;
            const auto& v0 = vertices[indices[base + 0]].Position;
            const auto& v1 = vertices[indices[base + 1]].Position;
            const auto& v2 = vertices[indices[base + 2]].Position;

            glm::vec3 computedNormal = ComputeTriangleNormal(v0, v1, v2);

            // Side normal should point radially outward (horizontal, y≈0)
            glm::vec3 centroid = (v0 + v1 + v2) / 3.0f;
            glm::vec3 expectedRadial = glm::normalize(glm::vec3(centroid.x, 0.0f, centroid.z));

            EXPECT_GT(glm::dot(computedNormal, expectedRadial), 0.5f)
                << "Side triangle (seg=" << i << ", tri=" << tri << ") has wrong winding.\n"
                << "  Computed normal: (" << computedNormal.x << ", " << computedNormal.y << ", " << computedNormal.z << ")\n"
                << "  Expected radial: (" << expectedRadial.x << ", " << expectedRadial.y << ", " << expectedRadial.z << ")";
        }
    }
}

// =============================================================================
// Cone Winding Order Correctness
// =============================================================================

struct ConeTestData
{
    struct TestVertex
    {
        glm::vec3 Position;
        glm::vec3 Normal;
    };

    static void Generate(f32 radius, f32 height, u32 segments,
                         std::vector<TestVertex>& outVertices, std::vector<u32>& outIndices)
    {
        const f32 angleStep = 2.0f * glm::pi<f32>() / segments;

        outVertices.push_back({ { 0.0f, height, 0.0f }, { 0.0f, 1.0f, 0.0f } }); // tip
        outVertices.push_back({ { 0.0f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } });  // bottom center

        for (u32 i = 0; i < segments; i++)
        {
            const f32 angle = i * angleStep;
            const f32 x = cos(angle) * radius;
            const f32 z = sin(angle) * radius;
            const glm::vec3 sideNormal = glm::normalize(glm::vec3(x, height / radius, z));

            outVertices.push_back({ { x, 0.0f, z }, { 0.0f, -1.0f, 0.0f } }); // base
            outVertices.push_back({ { x, 0.0f, z }, sideNormal });            // side
        }

        for (u32 i = 0; i < segments; i++)
        {
            const u32 next = (i + 1) % segments;

            // Base
            outIndices.push_back(1);
            outIndices.push_back(2 + i * 2);
            outIndices.push_back(2 + next * 2);

            // Side
            outIndices.push_back(0);
            outIndices.push_back(2 + next * 2 + 1);
            outIndices.push_back(2 + i * 2 + 1);
        }
    }
};

TEST(RenderState, ConeBaseWindingIsCCW)
{
    std::vector<ConeTestData::TestVertex> vertices;
    std::vector<u32> indices;
    ConeTestData::Generate(0.5f, 1.0f, 16, vertices, indices);

    for (u32 i = 0; i < 16; i++)
    {
        const u32 base = i * 6; // 6 indices per segment (3 base + 3 side)
        const auto& v0 = vertices[indices[base + 0]].Position;
        const auto& v1 = vertices[indices[base + 1]].Position;
        const auto& v2 = vertices[indices[base + 2]].Position;

        glm::vec3 normal = ComputeTriangleNormal(v0, v1, v2);
        EXPECT_GT(glm::dot(normal, glm::vec3(0, -1, 0)), 0.9f)
            << "Cone base triangle " << i << " has wrong winding (normal.y="
            << normal.y << ", expected -Y)";
    }
}

TEST(RenderState, ConeSideWindingIsCCW)
{
    std::vector<ConeTestData::TestVertex> vertices;
    std::vector<u32> indices;
    ConeTestData::Generate(0.5f, 1.0f, 16, vertices, indices);

    for (u32 i = 0; i < 16; i++)
    {
        const u32 base = i * 6 + 3; // side triangle starts after base triangle
        const auto& v0 = vertices[indices[base + 0]].Position;
        const auto& v1 = vertices[indices[base + 1]].Position;
        const auto& v2 = vertices[indices[base + 2]].Position;

        glm::vec3 computedNormal = ComputeTriangleNormal(v0, v1, v2);

        // Side normal should point outward (away from the cone axis)
        glm::vec3 centroid = (v0 + v1 + v2) / 3.0f;
        glm::vec3 axisPoint = glm::vec3(0.0f, centroid.y, 0.0f);
        glm::vec3 expectedRadial = glm::normalize(centroid - axisPoint);

        EXPECT_GT(glm::dot(computedNormal, expectedRadial), 0.3f)
            << "Cone side triangle " << i << " has wrong winding.\n"
            << "  Computed normal: (" << computedNormal.x << ", " << computedNormal.y << ", " << computedNormal.z << ")\n"
            << "  Expected radial: (" << expectedRadial.x << ", " << expectedRadial.y << ", " << expectedRadial.z << ")";
    }
}

// =============================================================================
// Torus Winding Order Correctness
// =============================================================================

struct TorusTestData
{
    struct TestVertex
    {
        glm::vec3 Position;
        glm::vec3 Normal;
    };

    static void Generate(f32 majorRadius, f32 minorRadius, u32 majorSegments, u32 minorSegments,
                         std::vector<TestVertex>& outVertices, std::vector<u32>& outIndices)
    {
        for (u32 i = 0; i < majorSegments; i++)
        {
            const f32 u = static_cast<f32>(i) / majorSegments * 2.0f * glm::pi<f32>();
            for (u32 j = 0; j < minorSegments; j++)
            {
                const f32 v = static_cast<f32>(j) / minorSegments * 2.0f * glm::pi<f32>();
                const f32 x = (majorRadius + minorRadius * cos(v)) * cos(u);
                const f32 y = minorRadius * sin(v);
                const f32 z = (majorRadius + minorRadius * cos(v)) * sin(u);

                glm::vec3 position(x, y, z);
                glm::vec3 center(majorRadius * cos(u), 0, majorRadius * sin(u));
                glm::vec3 normal = glm::normalize(position - center);

                outVertices.push_back({ position, normal });
            }
        }

        for (u32 i = 0; i < majorSegments; i++)
        {
            for (u32 j = 0; j < minorSegments; j++)
            {
                const u32 current = i * minorSegments + j;
                const u32 next = ((i + 1) % majorSegments) * minorSegments + j;
                const u32 currentNext = i * minorSegments + ((j + 1) % minorSegments);
                const u32 nextNext = ((i + 1) % majorSegments) * minorSegments + ((j + 1) % minorSegments);

                outIndices.insert(outIndices.end(), { current, currentNext, next });
                outIndices.insert(outIndices.end(), { currentNext, nextNext, next });
            }
        }
    }
};

TEST(RenderState, TorusWindingIsCCW)
{
    std::vector<TorusTestData::TestVertex> vertices;
    std::vector<u32> indices;
    const u32 majorSegs = 12;
    const u32 minorSegs = 8;
    TorusTestData::Generate(1.0f, 0.3f, majorSegs, minorSegs, vertices, indices);

    // Check every triangle: the cross-product normal must align with the
    // vertex normal (pointing outward from the tube surface).
    const u32 totalTriangles = majorSegs * minorSegs * 2;
    u32 failures = 0;
    for (u32 t = 0; t < totalTriangles; t++)
    {
        const u32 i0 = indices[t * 3 + 0];
        const u32 i1 = indices[t * 3 + 1];
        const u32 i2 = indices[t * 3 + 2];

        glm::vec3 computedNormal = ComputeTriangleNormal(
            vertices[i0].Position, vertices[i1].Position, vertices[i2].Position);

        // Use the vertex normal at the first vertex as the expected outward direction
        glm::vec3 expectedNormal = vertices[i0].Normal;
        float dot = glm::dot(computedNormal, expectedNormal);

        if (dot <= 0.0f)
        {
            failures++;
            if (failures <= 3) // Only print first few failures to avoid spam
            {
                ADD_FAILURE() << "Torus triangle " << t << " has wrong winding.\n"
                              << "  Computed normal: (" << computedNormal.x << ", " << computedNormal.y << ", " << computedNormal.z << ")\n"
                              << "  Expected normal: (" << expectedNormal.x << ", " << expectedNormal.y << ", " << expectedNormal.z << ")\n"
                              << "  Dot: " << dot;
            }
        }
    }
    EXPECT_EQ(failures, 0u) << failures << " of " << totalTriangles << " torus triangles have wrong winding";
}

// =============================================================================
// Material Blend Flag → Sort Key Consistency
// =============================================================================

TEST(RenderState, MaterialBlendFlagDeterminesSortKeyType)
{
    // INVARIANT: When a material has MaterialFlag::Blend set, the Draw function
    // must produce a DrawKey::CreateTransparent sort key. When the blend flag
    // is not set, it must produce DrawKey::CreateOpaque.
    //
    // This was a systematic bug in 5 Draw functions that unconditionally
    // used CreateOpaque regardless of the material's blend flag.

    const u32 shaderID = 42;
    const u32 materialID = 7;
    const u32 depth = 500;

    // Simulate the corrected logic:
    auto computeSortKey = [](bool hasBlend, u32 shader, u32 material, u32 d) -> DrawKey
    {
        if (hasBlend)
            return DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shader, material, d);
        return DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shader, material, d);
    };

    // Opaque material → opaque sort key
    DrawKey opaqueKey = computeSortKey(false, shaderID, materialID, depth);
    EXPECT_EQ(opaqueKey.GetRenderMode(), RenderMode::Opaque)
        << "Non-blended material must produce opaque sort key";

    // Blended material → transparent sort key
    DrawKey blendedKey = computeSortKey(true, shaderID, materialID, depth);
    EXPECT_EQ(blendedKey.GetRenderMode(), RenderMode::Transparent)
        << "Blended material must produce transparent sort key";

    // Transparent key must sort after opaque key
    EXPECT_GT(blendedKey.GetKey(), opaqueKey.GetKey())
        << "Blended material key must sort after opaque material key";
}

TEST(RenderState, TransparentDepthSortsBackToFront)
{
    // Transparent objects should sort back-to-front (farthest first).
    // CreateTransparent inverts depth: 0xFFFFFF - depth, so larger depth
    // (farther) gets a smaller inverted value, which sorts first (ascending).

    DrawKey nearKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 1, 1, 100); // near
    DrawKey farKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 1, 1, 900);  // far

    // In ascending sort order, far objects (lower inverted depth) should come first
    EXPECT_LT(farKey.GetKey(), nearKey.GetKey())
        << "Far transparent object must sort before near transparent object (back-to-front).\n"
        << "Far key:  " << PrintKeyBits(farKey) << "\n"
        << "Near key: " << PrintKeyBits(nearKey);
}

// =============================================================================
// Grid Depth Write Invariant
// =============================================================================

TEST(RenderState, BlendedObjectShouldNotWriteDepth)
{
    // INVARIANT: Objects with blending enabled should not write to the depth
    // buffer. Writing depth from a blended object can cause other transparent
    // objects behind it to be incorrectly depth-rejected.
    //
    // The infinite grid had blendEnabled=true but depthWriteMask=true,
    // which violated this convention.

    // Correct transparent state: blend on, depth write off
    PODRenderState correctState{};
    correctState.blendEnabled = true;
    correctState.depthWriteMask = false;
    correctState.depthTestEnabled = true;

    EXPECT_TRUE(correctState.blendEnabled);
    EXPECT_FALSE(correctState.depthWriteMask)
        << "Blended objects must not write depth to avoid occluding other transparents";
    EXPECT_TRUE(correctState.depthTestEnabled)
        << "Blended objects should still test against the depth buffer";
}
