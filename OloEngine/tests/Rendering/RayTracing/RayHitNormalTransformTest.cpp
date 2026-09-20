// OLO_TEST_LAYER: plumbing
// =============================================================================
// RayHitNormalTransformTest.cpp — the device-free half of issue #1326: how an
// object-space hit normal becomes a world-space one under an arbitrary instance
// transform.
//
// WHY A CPU TEST AT ALL, when the operation only ever runs in GLSL. Because the
// operation is ALGEBRA, and the algebra is the thing that was wrong. The ray
// query's matrices are a device concern; that a normal maps by the inverse
// transpose is not, and it is checkable here on every runner this project has,
// including the ones that will never see a ray-query device (the Vulkan
// ray-query suites in CI have historically never run — see #1294).
//
// THE DISCRIMINATING CASE IS THE POINT. Every assertion below that matters is
// one the SUPERSEDED direct-basis expression FAILS. A test that both expressions
// pass — a rigid instance, a uniform scale — proves only that the two agree
// where they always agreed, so that case appears here exactly once, as the
// no-regression case, and is labelled as such.
//
// C++ MIRRORS GLSL LITERALLY. `n * mat3(W)` in GLSL is the row-vector product,
// transpose(W3) * n, and `glm::vec3 * glm::mat3` in glm is the same product with
// the same column-major storage. So the expression under test is written here
// character for character as include/RayHitNormalTransform.glsl writes it,
// rather than as a hand-derived equivalent that could be equivalent to the wrong
// thing. The GLSL text itself is pinned by the last case in this file.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        // include/RayHitNormalTransform.glsl's oloRtObjectNormalToWorld, in the
        // same form. `worldToObject` is the ray query's inverse instance matrix.
        [[nodiscard]] glm::vec3 ObjectNormalToWorld(const glm::vec3& objectNormal, const glm::mat4& worldToObject)
        {
            return objectNormal * glm::mat3(worldToObject);
        }

        // The expression this issue REPLACED, kept so the tests can show it
        // failing rather than merely asserting the new one passes.
        [[nodiscard]] glm::vec3 LegacyDirectBasis(const glm::vec3& objectNormal, const glm::mat4& objectToWorld)
        {
            return glm::mat3(objectToWorld) * objectNormal;
        }

        // oloRtInstanceWindingSign.
        [[nodiscard]] f32 InstanceWindingSign(const glm::mat4& objectToWorld)
        {
            return (glm::determinant(glm::mat3(objectToWorld)) < 0.0f) ? -1.0f : 1.0f;
        }

        // oloRtNormalizeHitNormal: false means the input was degenerate, and the
        // caller's own value is left alone.
        [[nodiscard]] bool NormalizeHitNormal(const glm::vec3& worldNormal, glm::vec3& result)
        {
            const f32 len = glm::length(worldNormal);
            if (!(len > 1e-12f) || std::isinf(len))
                return false;
            result = worldNormal / len;
            return true;
        }

        [[nodiscard]] bool NearlyEqual(const glm::vec3& a, const glm::vec3& b, f32 tolerance = 1e-5f)
        {
            return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
                   std::abs(a.z - b.z) <= tolerance;
        }

        // `relative` is a path under OloEditor/assets/shaders, searched upward
        // from the working directory — the same walk GpuPathTracerContractTest
        // uses, local for the same reason it is local there.
        [[nodiscard]] std::filesystem::path ResolveShaderPath(const char* relative)
        {
            namespace fs = std::filesystem;
            const fs::path candidates[] = {
                fs::path("OloEditor") / "assets" / "shaders" / relative,
                fs::path("assets") / "shaders" / relative,
                fs::path("..") / "OloEditor" / "assets" / "shaders" / relative,
            };
            fs::path current = fs::current_path();
            for (int depth = 0; depth < 6; ++depth)
            {
                for (const auto& candidate : candidates)
                {
                    if (fs::exists(current / candidate))
                        return current / candidate;
                }
                if (!current.has_parent_path() || current.parent_path() == current)
                    break;
                current = current.parent_path();
            }
            return {};
        }

        [[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path)
        {
            std::ifstream file(path);
            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        // Occurrences, not presence. A "does this string appear at all" check
        // passes a PARTIAL revert — put the old expression back on one of the
        // three vertex normals below and every presence test still sees the
        // other two. That was not hypothetical; it was the first thing this
        // case failed to catch.
        [[nodiscard]] sizet CountOccurrences(const std::string& haystack, const std::string& needle)
        {
            sizet count = 0;
            for (sizet at = haystack.find(needle); at != std::string::npos;
                 at = haystack.find(needle, at + needle.size()))
                ++count;
            return count;
        }
    } // namespace

    // -----------------------------------------------------------------------
    // The issue's worked counter-example, verbatim.
    // -----------------------------------------------------------------------
    TEST(RayHitNormalTransform, TheIssuesWorkedNonUniformCaseAndTheOldExpressionFailingIt)
    {
        // M = diag(2, 1, 1), n = (1, 1, 0)/sqrt(2).
        const glm::mat4 objectToWorld = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 1.0f));
        const glm::mat4 worldToObject = glm::inverse(objectToWorld);
        const glm::vec3 objectNormal = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));

        const glm::vec3 expected = glm::normalize(glm::vec3(1.0f, 2.0f, 0.0f));       // (1,2,0)/sqrt(5)
        const glm::vec3 legacyExpected = glm::normalize(glm::vec3(2.0f, 1.0f, 0.0f)); // (2,1,0)/sqrt(5)

        glm::vec3 corrected{};
        ASSERT_TRUE(NormalizeHitNormal(ObjectNormalToWorld(objectNormal, worldToObject), corrected));
        EXPECT_TRUE(NearlyEqual(corrected, expected))
            << "corrected = (" << corrected.x << ", " << corrected.y << ", " << corrected.z << ")";

        // The old expression, and the assertion that carries this issue: it does
        // not merely differ in magnitude, it points somewhere else. Normalising
        // it cannot repair that, which is why the pre-#1326 normalize() did not
        // save it.
        const glm::vec3 legacy = glm::normalize(LegacyDirectBasis(objectNormal, objectToWorld));
        EXPECT_TRUE(NearlyEqual(legacy, legacyExpected));
        EXPECT_FALSE(NearlyEqual(legacy, expected, 1e-3f))
            << "the superseded direct-basis expression must FAIL the non-uniform case; if it passes, "
               "the test is not discriminating and proves nothing";

        // How far off, stated as an angle, so the failure has a magnitude rather
        // than only a direction: ~36.87 degrees here.
        const f32 cosAngle = glm::clamp(glm::dot(legacy, corrected), -1.0f, 1.0f);
        EXPECT_GT(std::acos(cosAngle), glm::radians(36.0f));
    }

    // -----------------------------------------------------------------------
    // Criterion 1: the corrected world normal matches the RASTER convention.
    // -----------------------------------------------------------------------
    TEST(RayHitNormalTransform, MatchesTheRasterNormalMatrixConvention)
    {
        // What raster does: `mat3(inst.NormalMatrix) * localNormal` with
        // NormalMatrix = transpose(inverse(model)) — VirtualGBufferVertexStage,
        // VirtualVisibilityResolve, DDGI_Capture. A ray hit that disagreed with
        // this would be self-consistently wrong against the G-buffer, which is
        // not a fix.
        const glm::mat4 transforms[] = {
            glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 1.0f)),
            // Rotation combined with scale — the criterion names it, and it is
            // the case where a naive "just normalise it" argument is most
            // tempting, because the result LOOKS unit-length either way.
            glm::rotate(glm::scale(glm::mat4(1.0f), glm::vec3(0.5f, 3.0f, 1.25f)), glm::radians(37.0f),
                        glm::normalize(glm::vec3(0.3f, 1.0f, -0.6f))),
            glm::scale(glm::rotate(glm::mat4(1.0f), glm::radians(-115.0f), glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f))),
                       glm::vec3(4.0f, 0.25f, 1.0f)),
            // A mirrored basis: raster does NOT negate, and neither may we.
            glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f)),
        };
        const glm::vec3 normals[] = {
            glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)),
            glm::normalize(glm::vec3(0.0f, 0.0f, 1.0f)),
            glm::normalize(glm::vec3(-0.4f, 0.7f, 0.59f)),
        };

        for (const glm::mat4& objectToWorld : transforms)
        {
            const glm::mat3 rasterNormalMatrix = glm::transpose(glm::inverse(glm::mat3(objectToWorld)));
            const glm::mat4 worldToObject = glm::inverse(objectToWorld);
            for (const glm::vec3& n : normals)
            {
                glm::vec3 ours{};
                ASSERT_TRUE(NormalizeHitNormal(ObjectNormalToWorld(n, worldToObject), ours));
                const glm::vec3 raster = glm::normalize(rasterNormalMatrix * n);
                EXPECT_TRUE(NearlyEqual(ours, raster, 1e-4f))
                    << "ray-hit normal and the raster normal matrix must agree, including sign";
            }
        }
    }

    // -----------------------------------------------------------------------
    // The no-regression case: where the two expressions always agreed, they
    // still do.
    // -----------------------------------------------------------------------
    TEST(RayHitNormalTransform, RigidAndUniformlyScaledInstancesAreUnchanged)
    {
        const glm::mat4 rigid =
            glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, -2.0f, 7.5f)) *
            glm::rotate(glm::mat4(1.0f), glm::radians(63.0f), glm::normalize(glm::vec3(0.2f, -0.9f, 0.4f)));
        const glm::mat4 uniform = glm::scale(rigid, glm::vec3(2.75f));
        const glm::vec3 n = glm::normalize(glm::vec3(0.3f, -0.8f, 0.5f));

        for (const glm::mat4& objectToWorld : { rigid, uniform })
        {
            glm::vec3 corrected{};
            ASSERT_TRUE(NormalizeHitNormal(ObjectNormalToWorld(n, glm::inverse(objectToWorld)), corrected));
            const glm::vec3 legacy = glm::normalize(LegacyDirectBasis(n, objectToWorld));
            EXPECT_TRUE(NearlyEqual(corrected, legacy, 1e-5f))
                << "rigid and uniform-scale instances are the class the two expressions share; the "
                   "correction must not move them. That is also the class the CPU reference path tracer "
                   "accepts (ReferenceScene::AddInstance), so parity against it is preserved.";
        }
    }

    // -----------------------------------------------------------------------
    // Criterion 2: negative determinant (mirrored) transforms, and what that
    // means for face orientation.
    // -----------------------------------------------------------------------
    TEST(RayHitNormalTransform, AMirroredInstanceFlipsTheWindingButNotTheShadingNormal)
    {
        // A reflection through the yz plane, composed with a rotation so the
        // case is not axis-trivial.
        const glm::mat4 objectToWorld =
            glm::rotate(glm::mat4(1.0f), glm::radians(24.0f), glm::normalize(glm::vec3(0.1f, 0.8f, 0.5f))) *
            glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
        ASSERT_LT(glm::determinant(glm::mat3(objectToWorld)), 0.0f);
        EXPECT_EQ(InstanceWindingSign(objectToWorld), -1.0f);

        // One triangle in object space, wound so its outward face normal is +z.
        const glm::vec3 v0(0.0f, 0.0f, 0.0f);
        const glm::vec3 v1(1.0f, 0.0f, 0.0f);
        const glm::vec3 v2(0.0f, 1.0f, 0.0f);
        const glm::vec3 objectFaceNormal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
        ASSERT_TRUE(NearlyEqual(objectFaceNormal, glm::vec3(0.0f, 0.0f, 1.0f)));

        const glm::mat4 worldToObject = glm::inverse(objectToWorld);
        const glm::vec3 p0 = glm::vec3(objectToWorld * glm::vec4(v0, 1.0f));
        const glm::vec3 p1 = glm::vec3(objectToWorld * glm::vec4(v1, 1.0f));
        const glm::vec3 p2 = glm::vec3(objectToWorld * glm::vec4(v2, 1.0f));

        glm::vec3 shading{};
        ASSERT_TRUE(NormalizeHitNormal(ObjectNormalToWorld(objectFaceNormal, worldToObject), shading));

        // The raw winding cross, as RayTracedSurfaceHit computed it before
        // #1326: it points the OTHER way from the shading normal, because
        // cross(Ma, Mb) = det(M) * M^-T cross(a, b).
        const glm::vec3 rawWinding = glm::normalize(glm::cross(p1 - p0, p2 - p0));
        EXPECT_LT(glm::dot(rawWinding, shading), -0.99f)
            << "a mirrored instance's raw winding cross must oppose the inverse-transpose normal; if it "
               "did not, the winding sign below would be correcting nothing";

        // With the winding sign applied they agree — which is what keeps the
        // snap-to-the-geometric-side step from throwing the corrected shading
        // normal away, and what keeps a shadow-ray offset outside the surface.
        const glm::vec3 corrected = glm::normalize(glm::cross(p1 - p0, p2 - p0) * InstanceWindingSign(objectToWorld));
        EXPECT_GT(glm::dot(corrected, shading), 0.99f);

        // And the sign is exactly det(M)'s and nothing else: a
        // handedness-preserving instance leaves the winding cross untouched, so
        // every non-mirrored scene is bit-identical to before.
        const glm::mat4 preserving = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 1.0f));
        EXPECT_EQ(InstanceWindingSign(preserving), 1.0f);
    }

    // -----------------------------------------------------------------------
    // Criterion 2: singular input is handled by a documented finite fallback,
    // never by a plausible-looking number.
    // -----------------------------------------------------------------------
    TEST(RayHitNormalTransform, SingularAndDegenerateInputsAreReportedNotSubstitutedSilently)
    {
        const glm::vec3 n = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
        glm::vec3 sentinel(0.0f, 0.0f, -1.0f);
        const glm::vec3 untouched = sentinel;

        // A collapsed axis: inverse() of this is non-finite, which is what the
        // ray query hands back for a degenerate instance.
        const glm::mat4 singular = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, 0.0f));
        ASSERT_FLOAT_EQ(glm::determinant(glm::mat3(singular)), 0.0f);
        const glm::vec3 product = ObjectNormalToWorld(n, glm::inverse(singular));
        EXPECT_FALSE(NormalizeHitNormal(product, sentinel));
        EXPECT_TRUE(NearlyEqual(sentinel, untouched))
            << "a failed normalise must leave the caller's own value alone, so the caller's documented "
               "substitute survives";

        // A zero object normal — an unnormalised mesh, not a transform problem.
        EXPECT_FALSE(NormalizeHitNormal(glm::vec3(0.0f), sentinel));

        // An explicit infinity, for the isinf arm that the NaN comparison does
        // not cover on its own.
        EXPECT_FALSE(NormalizeHitNormal(glm::vec3(std::numeric_limits<f32>::infinity(), 0.0f, 0.0f), sentinel));

        // And the case that MUST still succeed, so the guard is not simply
        // rejecting everything.
        glm::vec3 fine{};
        EXPECT_TRUE(NormalizeHitNormal(glm::vec3(0.0f, 3.0f, 4.0f), fine));
        EXPECT_TRUE(NearlyEqual(fine, glm::vec3(0.0f, 0.6f, 0.8f)));
    }

    // -----------------------------------------------------------------------
    // Criterion 2: a NORMAL MAP's tangent frame needs no separate correction,
    // because it is built from WORLD-space positions.
    // -----------------------------------------------------------------------
    TEST(RayHitNormalTransform, AWorldSpaceTangentFrameNeedsNoSeparateCorrection)
    {
        // OloRtApplyNormalMap derives the tangent from world-space edges and UV
        // deltas, then Gram-Schmidts it against the shading normal. A tangent is
        // an ordinary direction vector, so it maps by M — which the world
        // positions already carry. This asserts the property that makes that
        // sound: the corrected normal is perpendicular to every world-space edge
        // of its own triangle, so the orthogonalised frame it anchors is the
        // right one. Under the OLD expression it is not.
        const glm::mat4 objectToWorld = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 1.0f));
        const glm::mat4 worldToObject = glm::inverse(objectToWorld);

        // A slanted triangle — the first criterion's own wording.
        const glm::vec3 v0(0.0f, 0.0f, 0.0f);
        const glm::vec3 v1(1.0f, 1.0f, 0.0f);
        const glm::vec3 v2(0.0f, 0.3f, 1.0f);
        const glm::vec3 objectNormal = glm::normalize(glm::cross(v1 - v0, v2 - v0));

        const glm::vec3 p0 = glm::vec3(objectToWorld * glm::vec4(v0, 1.0f));
        const glm::vec3 p1 = glm::vec3(objectToWorld * glm::vec4(v1, 1.0f));
        const glm::vec3 p2 = glm::vec3(objectToWorld * glm::vec4(v2, 1.0f));

        glm::vec3 corrected{};
        ASSERT_TRUE(NormalizeHitNormal(ObjectNormalToWorld(objectNormal, worldToObject), corrected));
        EXPECT_NEAR(glm::dot(corrected, glm::normalize(p1 - p0)), 0.0f, 1e-5f);
        EXPECT_NEAR(glm::dot(corrected, glm::normalize(p2 - p0)), 0.0f, 1e-5f);

        const glm::vec3 legacy = glm::normalize(LegacyDirectBasis(objectNormal, objectToWorld));
        EXPECT_GT(std::abs(glm::dot(legacy, glm::normalize(p1 - p0))), 1e-2f)
            << "the superseded expression is NOT perpendicular to the world triangle it shades — the "
               "discriminating half of this case";
    }

    // -----------------------------------------------------------------------
    // Criterion 4: the GLSL text itself, so the algebra above and the shaders
    // cannot drift apart silently. A C++ model of an expression no shader uses
    // any more would pass every case above.
    // -----------------------------------------------------------------------
    TEST(RayHitNormalTransform, EveryRayHitNormalSiteUsesTheSharedCorrectedOperation)
    {
        const std::filesystem::path sharedPath = ResolveShaderPath("include/RayHitNormalTransform.glsl");
        if (sharedPath.empty())
            GTEST_SKIP() << "Could not locate OloEditor/assets/shaders from the working directory.";

        const std::string shared = ReadTextFile(sharedPath);
        EXPECT_NE(shared.find("return objectNormal * mat3(worldToObject);"), std::string::npos)
            << "the shared operation must stay the row-vector product against the ray query's "
               "world-to-object matrix — no per-hit inverse";
        EXPECT_NE(shared.find("determinant(mat3(objectToWorld))"), std::string::npos);

        // Every site that turns an object-space hit normal into a world-space
        // one. RayTracedShadow.glsl is deliberately absent: it reads the
        // G-buffer's already-world normal and never touches an instance basis.
        const char* const sites[] = {
            "RayTracedReflection.glsl",
            "include/RayTracedSurfaceHit.glsl",
            "compute/RayTracingProbe.comp",
        };

        for (const char* site : sites)
        {
            const std::filesystem::path path = ResolveShaderPath(site);
            ASSERT_FALSE(path.empty()) << site;
            const std::string source = ReadTextFile(path);

            EXPECT_NE(source.find("RayHitNormalTransform.glsl"), std::string::npos)
                << site << " must include the shared operation rather than re-deriving it";
            EXPECT_NE(source.find("oloRtObjectNormalToWorld("), std::string::npos)
                << site << " must call the shared operation";
            EXPECT_NE(source.find("rayQueryGetIntersectionWorldToObjectEXT"), std::string::npos)
                << site << " must take the matrix from the ray query itself";

            // The misconception #1326 removed, in the words it was written in.
            EXPECT_EQ(source.find("inverse the ray query does not hand back"), std::string::npos)
                << site << " still carries the comment claiming the correction is unavailable; a reader "
                           "will re-derive the same wrong conclusion from it";
        }

        // And the specific shape of the old bug, in the two shaders that had it.
        //
        // Counted, and stated as "no object-to-world basis is ever applied to a
        // normal", because the bug's spelling is not fixed: `basis * ln0`,
        // `mat3(objectToWorld) * ln0` and `mat3(objectToWorld[0], ...) * ln0`
        // are the same defect written three ways.
        const std::string reflection = ReadTextFile(ResolveShaderPath("RayTracedReflection.glsl"));
        EXPECT_EQ(CountOccurrences(reflection, "oloRtObjectNormalToWorld(objectNormal, worldToObject)"), 1u);
        EXPECT_EQ(reflection.find("* objectNormal"), std::string::npos)
            << "RayTracedReflection must not left-multiply the object normal by any basis";

        const std::string surfaceHit = ReadTextFile(ResolveShaderPath("include/RayTracedSurfaceHit.glsl"));
        // All THREE vertex normals, not just the first one.
        EXPECT_EQ(CountOccurrences(surfaceHit, "oloRtObjectNormalToWorld(ln"), 3u)
            << "every vertex normal in OloRtFetchTriangle must go through the shared operation; a "
               "partial revert is still the bug";
        EXPECT_EQ(surfaceHit.find("* ln0"), std::string::npos)
            << "no basis may be left-multiplied onto an object-space vertex normal";
        EXPECT_EQ(surfaceHit.find("* ln1"), std::string::npos);
        EXPECT_EQ(surfaceHit.find("* ln2"), std::string::npos);
        EXPECT_NE(surfaceHit.find("oloRtInstanceWindingSign(objectToWorld)"), std::string::npos)
            << "the winding-derived geometric normal must carry the mirrored-instance sign";
    }
} // namespace OloEngine::Tests
