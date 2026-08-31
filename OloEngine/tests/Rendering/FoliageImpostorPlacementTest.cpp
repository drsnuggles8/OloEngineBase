// =============================================================================
// FoliageImpostorPlacementTest.cpp
//
// Pins the placement/sizing contract shared by the two foliage vertex stages
// (issue #953). Both draw the SAME instance stream — FoliageRenderer emits one
// `{PositionScale, RotationHeight, ColorAlpha}` per plant — so they must agree
// on what that stream means. They did not:
//
//   * positions are TERRAIN-LOCAL (FoliageRenderer::GenerateInstances derives
//     x/z from tile coordinates starting at 0, and y from
//     `GetHeightAt() * heightScale` with no base offset), and become world
//     positions only through the owning terrain's transform, which
//     CommandDispatch::DrawFoliageLayer uploads as the single `u_Model` entry.
//     Foliage_Instance.glsl multiplied through it; Foliage_Impostor.glsl
//     subtracted the render origin directly instead, on the belief that the
//     positions were already absolute world. No island in Drift sits at the
//     origin, so every impostor card rendered at its island's LOCAL
//     coordinates — all six islands' pines in one heap over open water near
//     (0,0,0), above anything the terrain can reach. From the boat that reads
//     as a swarm of dark specks hanging in the sky, which is how it was
//     reported, and it is why the search started at "sky/atmosphere bug".
//
//   * the per-instance world height lives in `a_RotationHeight.y`. The meshes
//     are authored unit-height (pine.obj spans y in [0,1]), so a card sized
//     without it is short by that whole factor — a 9-16 m pine drew as a
//     ~1.5 m card the moment it crossed ImpostorStartDistance.
//
// Source-text assertions rather than a render, because the failure mode is a
// shader silently diverging from its sibling: a GPU test would only catch it in
// a scene whose terrain is NOT at the origin, and the visual result (foliage
// slightly misplaced, or absent at range) is easy to read as art. Text is also
// what makes this cheap enough to run in headless CI, where the #953 repro
// (a large viewport, an island off the origin) does not exist.
//
// OLO_TEST_LAYER: unit
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cctype>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    [[nodiscard]] std::string ReadShader(const char* name)
    {
        const fs::path path = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / name;
        std::ifstream in(path);
        if (!in)
            return {};
        std::ostringstream oss;
        oss << in.rdbuf();
        return oss.str();
    }

    /// The vertex stage only — `#type fragment` onward is a different program
    /// and has its own `u_Model`-free coordinate conventions.
    [[nodiscard]] std::string VertexStageOf(const std::string& source)
    {
        const auto vs = source.find("#type vertex");
        if (vs == std::string::npos)
            return {};
        const auto fsStart = source.find("#type fragment", vs);
        return source.substr(vs, (fsStart == std::string::npos) ? std::string::npos : fsStart - vs);
    }

    /// Strip // and /* */ comments so a doc comment quoting an expression can
    /// never satisfy (or break) an assertion about the CODE.
    [[nodiscard]] std::string StripComments(const std::string& src)
    {
        std::string out;
        out.reserve(src.size());
        for (std::size_t i = 0; i < src.size();)
        {
            if (src.compare(i, 2, "//") == 0)
            {
                while (i < src.size() && src[i] != '\n')
                    ++i;
            }
            else if (src.compare(i, 2, "/*") == 0)
            {
                const auto end = src.find("*/", i + 2);
                i = (end == std::string::npos) ? src.size() : end + 2;
            }
            else
            {
                out.push_back(src[i]);
                ++i;
            }
        }
        return out;
    }
    /// The comma-separated arguments of the first `vec3(...)` in `expr`, with all
    /// whitespace removed. Returns empty if there is no balanced vec3 call.
    [[nodiscard]] std::vector<std::string> Vec3Args(const std::string& expr)
    {
        const auto open = expr.find("vec3(");
        if (open == std::string::npos)
            return {};
        std::vector<std::string> args;
        std::string current;
        int depth = 0;
        for (std::size_t i = open + 4; i < expr.size(); ++i)
        {
            const char c = expr[i];
            if (c == '(')
            {
                if (++depth == 1)
                    continue; // the vec3's own opening paren
            }
            else if (c == ')')
            {
                if (--depth == 0)
                {
                    args.push_back(current);
                    return args;
                }
            }
            else if (c == ',' && depth == 1)
            {
                args.push_back(current);
                current.clear();
                continue;
            }
            if (!std::isspace(static_cast<unsigned char>(c)))
                current.push_back(c);
        }
        return {}; // unbalanced
    }

} // namespace

// Both stages must place the instance stream the same way. This is the
// two-mirrors-drift guard: whichever stage is edited, the other has to keep up.
TEST(FoliageImpostorPlacementTest, BothFoliageStagesPlaceInstancesThroughTheTerrainTransform)
{
    for (const char* shader : { "Foliage_Instance.glsl", "Foliage_Impostor.glsl" })
    {
        const std::string source = ReadShader(shader);
        ASSERT_FALSE(source.empty()) << "could not read " << shader;

        const std::string vertex = StripComments(VertexStageOf(source));
        ASSERT_FALSE(vertex.empty()) << shader << " has no #type vertex stage";

        EXPECT_NE(vertex.find("u_Model * vec4(a_PositionScale.xyz"), std::string::npos)
            << shader
            << ": the vertex stage must turn the TERRAIN-LOCAL instance position into a world "
               "position through u_Model (the owning terrain's transform, uploaded by "
               "CommandDispatch::DrawFoliageLayer and already made render-relative by "
               "UploadModelInstance). Without it the plants render at their island's local "
               "coordinates — see issue #953.";
    }
}

// The specific regression: the impostor stage must not go back to treating the
// instance position as absolute world.
TEST(FoliageImpostorPlacementTest, ImpostorDoesNotTreatInstancePositionsAsAbsoluteWorld)
{
    const std::string vertex = StripComments(VertexStageOf(ReadShader("Foliage_Impostor.glsl")));
    ASSERT_FALSE(vertex.empty());

    EXPECT_EQ(vertex.find("a_PositionScale.xyz - u_RenderOrigin"), std::string::npos)
        << "Foliage_Impostor.glsl is subtracting the render origin from the instance position "
           "again. That treats a TERRAIN-LOCAL position as absolute world (issue #953). Reading "
           "u_Model here is safe: OLO_INSTANCE_SINGLE pins it to instances[0] rather than "
           "indexing by gl_InstanceIndex, which is the out-of-bounds hazard from issue #433 that "
           "the original comment conflated this with.";
}

// The card's ANCHOR has to scale with the card too. Sizing the card by the plant
// height without moving the anchor lifted the card centre — and the tree drawn
// around it — metres into the air, which put trees in the sky. The bake centres
// each tile on the mesh's bounding-box centre, so the anchor is half the drawn
// height, never the card radius.
TEST(FoliageImpostorPlacementTest, ImpostorCardIsAnchoredOnTheMeshCentreNotItsRadius)
{
    const std::string vertex = StripComments(VertexStageOf(ReadShader("Foliage_Impostor.glsl")));
    ASSERT_FALSE(vertex.empty());

    const auto at = vertex.find("cardCenter = instWorld");
    ASSERT_NE(at, std::string::npos) << "no card anchor expression found";
    const std::string expr = vertex.substr(at, vertex.find(';', at) - at);

    // Assert what the offset IS, not merely that the word "height" appears in it:
    // `vec3(0.0, height, 0.0)` and `vec3(0.0, height * 2.0, 0.0)` both mention the
    // height and both float the tree. Term ORDER is free — `0.5 * height * scale`,
    // `height * scale * 0.5` and `scale * height * 0.5` are the same offset — so
    // the Y term is checked by the factors it carries, not by its spelling.
    const std::vector<std::string> args = Vec3Args(expr);
    ASSERT_EQ(args.size(), 3u) << "card anchor is not a vec3(x, y, z) offset: " << expr;

    EXPECT_TRUE(args[0] == "0.0" || args[0] == "0") << "card anchor shifts X: " << expr;
    EXPECT_TRUE(args[2] == "0.0" || args[2] == "0") << "card anchor shifts Z: " << expr;

    const std::string& y = args[1];
    const bool carriesHeight = y.find("height") != std::string::npos;
    const bool carriesScale = y.find("scale") != std::string::npos;
    const bool carriesHalf = y.find("0.5") != std::string::npos || y.find("/2.0") != std::string::npos ||
                             y.find("/2") != std::string::npos;

    EXPECT_TRUE(carriesHeight && carriesScale && carriesHalf)
        << "the card anchor must be HALF the DRAWN height (height * scale), because the bake "
           "centres each tile on the source mesh's bounding-box centre. Got: "
        << y
        << " -- missing" << (carriesHalf ? "" : " the 1/2 factor") << (carriesHeight ? "" : " the height")
        << (carriesScale ? "" : " the instance scale") << " (issue #953).";

    EXPECT_EQ(y.find("radius"), std::string::npos)
        << "the card anchor is back to offsetting by the card radius: " << y
        << " -- that floats the tree inside its own card once the radius carries the height.";
}

// The card has to carry the per-instance height, or it is short by the whole
// unit-height-mesh factor.
TEST(FoliageImpostorPlacementTest, ImpostorCardRadiusUsesThePerInstanceHeight)
{
    const std::string vertex = StripComments(VertexStageOf(ReadShader("Foliage_Impostor.glsl")));
    ASSERT_FALSE(vertex.empty());

    EXPECT_NE(vertex.find("a_RotationHeight.y"), std::string::npos)
        << "Foliage_Impostor.glsl never reads the per-instance height. The foliage meshes are "
           "authored unit-height (pine.obj spans y in [0,1]) and the near path applies the world "
           "height itself (`localPos.y *= height * scale`), so a card sized from the bake radius "
           "and `scale` alone is short by that factor — issue #953 measured a 9-16 m pine drawn "
           "as a ~1.5 m card.";

    const auto radiusAt = vertex.find("float radius =");
    ASSERT_NE(radiusAt, std::string::npos) << "no card radius expression found";
    const std::string radiusExpr = vertex.substr(radiusAt, vertex.find(';', radiusAt) - radiusAt);
    EXPECT_NE(radiusExpr.find("height"), std::string::npos)
        << "the card radius expression does not include the per-instance height: " << radiusExpr;
}
