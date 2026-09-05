// OLO_TEST_LAYER: plumbing
//
// #1056 — the space a shadow ray is traced in.
//
// WHY THIS FILE EXISTS AS ITS OWN TENANT. Every other check on this feature
// looks at a rendered frame, and a rendered frame cannot see this bug: the
// acceleration structure is built from GPU Scene's RENDER-RELATIVE instance
// transforms (issue #429), so a ray traced from an ABSOLUTE world position is
// displaced from the geometry by exactly the render origin — and the render
// origin is (0,0,0) in every test scene, every benchmark scene, and the
// Courtyard captures this feature was brought up on. The defect is a
// byte-identical no-op there and a 1024 m miss the moment the origin grid
// snaps.
//
// It shipped in the first commit of this feature and was caught by review, not
// by a test. So the conversion is pure and pinned by arithmetic here, where
// distance from the world origin is a variable rather than a property of
// whichever scene someone happened to open.
//
// The oracle is the composition identity, computed by hand rather than by a
// second copy of the transform: for any world point P,
//
//     viewRelative * (P - origin)  ==  viewWorld * P
//
// which is the ONLY property that matters — it says the ray origin the shader
// reconstructs lands in the same space the TLAS was built in.
//
// AND THE SEAM THAT CREATES. Testing the helper does not test that the PASS
// calls it: someone could put `glm::inverse(m_View)` back and every assertion
// above would still pass, which is precisely the substitution
// substituted-seams-compound.md warns about. So the last test in this file
// reads RayTracedShadowPass.cpp and checks the call is there and the absolute
// form is not — the same source-scan shape GPUSceneAntiDuplicationRatchetTest
// uses, and for the same reason: the property is about the CALLER, and no unit
// test of the callee can see it.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Shadow/ShadowTechnique.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        // A camera that is not axis-aligned and not at the origin, so a wrong
        // matrix cannot pass by symmetry.
        [[nodiscard]] glm::mat4 MakeTestView(const glm::vec3& eye)
        {
            return glm::lookAt(eye, eye + glm::normalize(glm::vec3(0.4f, -0.3f, -1.0f)),
                               glm::vec3(0.0f, 1.0f, 0.0f));
        }

        constexpr f32 kTolerance = 1.0e-3f;

        // THE WORLD-SPACE SIDE IS THE IMPRECISE ONE, and that is the point of
        // camera-relative rendering rather than a weakness of this test. The
        // reference `viewWorld * P` multiplies a matrix whose translation column
        // is ~|origin| by a point that is also ~|origin|; in f32 the ULP there
        // is |origin| * 2^-23, which at the 131 km origin below is about 16 mm.
        // The RELATIVE path keeps both operands small and is accurate to
        // millimetres. So the tolerance has to track the magnitude of the
        // reference, or this test would be asserting that f32 is exact at 131 km.
        [[nodiscard]] f32 ToleranceAtOrigin(const glm::vec3& origin)
        {
            // 2^-23 is one f32 mantissa step; 8x for the handful of multiply-adds
            // a mat4 * vec4 accumulates.
            return kTolerance + glm::length(origin) * (8.0f / 8388608.0f);
        }

        void ExpectNear(const glm::vec3& actual, const glm::vec3& expected, const char* what,
                        f32 tolerance = kTolerance)
        {
            EXPECT_NEAR(actual.x, expected.x, tolerance) << what << " .x";
            EXPECT_NEAR(actual.y, expected.y, tolerance) << what << " .y";
            EXPECT_NEAR(actual.z, expected.z, tolerance) << what << " .z";
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The view matrix
    // -------------------------------------------------------------------------

    // THE regression test for the shipped defect. A world point must reach the
    // same view-space position through the relative matrix fed render-relative
    // coordinates as it does through the world matrix fed world coordinates.
    // If the pass hands the shader the WORLD view matrix (what the first commit
    // did), the reconstructed ray origin is off by the render origin and this
    // fails at every origin except zero.
    TEST(RayTracedShadowSpace, RelativeViewOfARelativePointMatchesWorldViewOfTheWorldPoint)
    {
        // Far enough out that the floating-origin grid has snapped several
        // times — the regime where the defect is a 1024 m miss.
        const std::array<glm::vec3, 4> origins = { glm::vec3(0.0f), glm::vec3(1024.0f, 0.0f, -1024.0f),
                                                   glm::vec3(-8192.0f, 512.0f, 4096.0f),
                                                   glm::vec3(131072.0f, -2048.0f, 65536.0f) };

        for (const glm::vec3& origin : origins)
        {
            const glm::vec3 eye = origin + glm::vec3(3.0f, 12.0f, 25.0f);
            const glm::mat4 worldView = MakeTestView(eye);
            const glm::mat4 relativeView = MakeRayTracedShadowView(worldView, origin);

            // A handful of world points around the camera, including one behind
            // it, so a sign error cannot hide.
            const std::array<glm::vec3, 4> worldPoints = {
                origin + glm::vec3(0.0f, 0.0f, 0.0f), origin + glm::vec3(17.0f, -4.0f, -31.0f),
                origin + glm::vec3(-9.5f, 2.25f, 40.0f), origin + glm::vec3(250.0f, 60.0f, -180.0f)
            };

            for (const glm::vec3& worldPoint : worldPoints)
            {
                const glm::vec4 throughWorld = worldView * glm::vec4(worldPoint, 1.0f);
                const glm::vec4 throughRelative = relativeView * glm::vec4(worldPoint - origin, 1.0f);
                ExpectNear(glm::vec3(throughRelative), glm::vec3(throughWorld),
                           "view-space position at a non-zero render origin", ToleranceAtOrigin(origin));
            }
        }
    }

    // The inverse is what the shader actually applies: it reconstructs a
    // position from view space, and that position has to come out RELATIVE,
    // because that is the space the TLAS instance transforms are in.
    TEST(RayTracedShadowSpace, TheInverseReconstructsARenderRelativePosition)
    {
        const glm::vec3 origin(4096.0f, 0.0f, -2048.0f);
        const glm::vec3 eye = origin + glm::vec3(0.0f, 5.0f, 20.0f);
        const glm::mat4 worldView = MakeTestView(eye);
        const glm::mat4 inverseRelativeView = glm::inverse(MakeRayTracedShadowView(worldView, origin));

        const glm::vec3 worldPoint = origin + glm::vec3(11.0f, 3.0f, -7.0f);
        const glm::vec4 viewSpace = worldView * glm::vec4(worldPoint, 1.0f);

        const glm::vec3 reconstructed = glm::vec3(inverseRelativeView * viewSpace);
        ExpectNear(reconstructed, worldPoint - origin, "reconstructed ray origin", ToleranceAtOrigin(origin));

        // And emphatically NOT the absolute world position — that is precisely
        // the value the shipped defect produced.
        EXPECT_GT(glm::length(reconstructed - worldPoint), 1000.0f)
            << "the reconstruction returned an ABSOLUTE world position; the TLAS is render-relative, so "
               "every ray would miss by the render origin";
    }

    // The engine already owns this construction; the header carries its own copy
    // so it stays free of renderer includes. Pin the two equal, or the copy is
    // free to drift and nothing would say so.
    TEST(RayTracedShadowSpace, TheLocalCopyMatchesTheEngineCameraRelativeHelper)
    {
        const glm::vec3 origin(777.0f, -13.0f, 91.0f);
        const glm::mat4 worldView = MakeTestView(origin + glm::vec3(2.0f, 4.0f, 8.0f));

        const glm::mat4 mine = MakeRayTracedShadowView(worldView, origin);
        const glm::mat4 engine = MakeViewRelative(worldView, origin);

        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                EXPECT_NEAR(mine[column][row], engine[column][row], 1.0e-5f)
                    << "column " << column << " row " << row;
            }
        }
    }

    // -------------------------------------------------------------------------
    // The light vector
    // -------------------------------------------------------------------------

    // A DIRECTION IS TRANSLATION-INVARIANT. Shifting a directional light's
    // vector by the render origin would rotate the sun as the camera walks —
    // shadows swinging with the player, which reads as a physics bug rather
    // than a space bug.
    TEST(RayTracedShadowSpace, ADirectionalLightVectorIsNeverShifted)
    {
        RayTracedShadowLightRequest request;
        request.Directional = true;
        request.Vector = glm::normalize(glm::vec3(0.3f, 0.9f, -0.2f));

        for (const glm::vec3& origin : { glm::vec3(0.0f), glm::vec3(1024.0f, 0.0f, 0.0f),
                                         glm::vec3(-65536.0f, 4096.0f, 32768.0f) })
        {
            ExpectNear(MakeRayTracedShadowLightVector(request, origin), request.Vector,
                       "directional light vector");
        }
    }

    // A POSITION MOVES WITH THE ORIGIN. A punctual light left in absolute world
    // space would sit a render origin away from the geometry, so every shadow
    // it casts points the wrong way.
    TEST(RayTracedShadowSpace, APunctualLightVectorMovesWithTheRenderOrigin)
    {
        const glm::vec3 origin(2048.0f, 16.0f, -512.0f);

        RayTracedShadowLightRequest request;
        request.Directional = false;
        request.Vector = origin + glm::vec3(6.0f, 9.0f, -3.0f);

        ExpectNear(MakeRayTracedShadowLightVector(request, origin), glm::vec3(6.0f, 9.0f, -3.0f),
                   "punctual light position");
    }

    // -------------------------------------------------------------------------
    // The caller
    // -------------------------------------------------------------------------

    namespace
    {
        // Walk up from the working directory to the repo root. Same shape as
        // GPUSceneAntiDuplicationRatchetTest's, and it SKIPS rather than fails
        // when the tree is not reachable — a ctest working directory that
        // differs is not a defect in this feature.
        [[nodiscard]] std::filesystem::path FindRepoRoot()
        {
            std::filesystem::path here = std::filesystem::current_path();
            for (int depth = 0; depth < 8; ++depth)
            {
                if (std::filesystem::exists(here / "OloEngine" / "src" / "OloEngine" / "Renderer"))
                    return here;
                if (!here.has_parent_path() || here.parent_path() == here)
                    break;
                here = here.parent_path();
            }
            return {};
        }

        // Strip // line comments so a comment MENTIONING the absolute form (this
        // file's own history is full of such prose) cannot fail the scan.
        [[nodiscard]] std::string StripLineComments(const std::string& source)
        {
            std::string code;
            code.reserve(source.size());
            std::istringstream lines(source);
            for (std::string line; std::getline(lines, line);)
            {
                const auto comment = line.find("//");
                code.append(comment == std::string::npos ? line : line.substr(0, comment));
                code.push_back('\n');
            }
            return code;
        }
    } // namespace

    TEST(RayTracedShadowSpace, ThePassActuallyUsesTheConversion)
    {
        const std::filesystem::path root = FindRepoRoot();
        if (root.empty())
            GTEST_SKIP() << "engine source tree not reachable from the working directory";

        const std::filesystem::path passSource =
            root / "OloEngine" / "src" / "OloEngine" / "Renderer" / "Passes" / "RayTracedShadowPass.cpp";
        std::ifstream file(passSource);
        ASSERT_TRUE(file.is_open()) << "cannot read " << passSource.string();
        std::stringstream buffer;
        buffer << file.rdbuf();
        const std::string code = StripLineComments(buffer.str());

        // Anti-vacuity: if the scan matches nothing at all it is testing nothing.
        ASSERT_NE(code.find("RayTracingShadowUBO"), std::string::npos)
            << "the scan did not find the UBO fill it is supposed to be checking";

        EXPECT_NE(code.find("MakeRayTracedShadowView"), std::string::npos)
            << "RayTracedShadowPass.cpp does not convert its view matrix to render-relative space. The TLAS "
               "is built from render-relative instance transforms, so an absolute view matrix puts every ray "
               "origin a render origin away from the geometry — a no-op at the world origin and a 1024 m miss "
               "once the grid snaps.";
        EXPECT_NE(code.find("MakeRayTracedShadowLightVector"), std::string::npos)
            << "RayTracedShadowPass.cpp does not convert its light vectors; a punctual light left in absolute "
               "world space casts its shadows from the wrong place.";

        // The exact shape of the shipped defect.
        EXPECT_EQ(code.find("glm::inverse(m_View)"), std::string::npos)
            << "RayTracedShadowPass.cpp inverts the WORLD view matrix directly. That is the defect this file "
               "exists for: it renders correctly at the world origin and wrongly everywhere else.";
    }

    // At the origin the whole conversion is a no-op — which is the fact that
    // made the defect invisible, and is worth asserting so a reader of this file
    // understands why the tests above use large origins on purpose.
    TEST(RayTracedShadowSpace, EverythingIsAByteIdenticalNoOpAtTheWorldOrigin)
    {
        const glm::vec3 origin(0.0f);
        const glm::mat4 worldView = MakeTestView(glm::vec3(1.0f, 2.0f, 3.0f));

        // Compared as BIT PATTERNS, which is what this test's name claims and
        // what the assertion has to be for it to mean anything: an approximate
        // compare would also pass if the conversion perturbed the matrix by an
        // ulp, and an ulp at the origin is exactly the drift this file exists to
        // catch. Translating by a zero vector must return the input untouched,
        // not merely something close to it. bit_cast to u32 rather than `==` on
        // floats: the house rule bans the operator, not the identity it cannot
        // express.
        const glm::mat4 relative = MakeRayTracedShadowView(worldView, origin);
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                EXPECT_EQ(std::bit_cast<u32>(relative[column][row]),
                          std::bit_cast<u32>(worldView[column][row]))
                    << "view matrix element [" << column << "][" << row << "] changed at the world origin";
            }
        }

        RayTracedShadowLightRequest punctual;
        punctual.Directional = false;
        punctual.Vector = glm::vec3(5.0f, -6.0f, 7.0f);
        const glm::vec3 shifted = MakeRayTracedShadowLightVector(punctual, origin);
        for (int component = 0; component < 3; ++component)
        {
            EXPECT_EQ(std::bit_cast<u32>(shifted[component]), std::bit_cast<u32>(punctual.Vector[component]))
                << "punctual light component " << component << " changed at the world origin";
        }
    }
} // namespace OloEngine::Tests
