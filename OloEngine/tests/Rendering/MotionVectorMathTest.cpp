// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// MotionVectorMathTest.cpp
//
// CPU contract tests for the per-object screen-space velocity (motion vector)
// math implemented in the deferred / forward G-Buffer shaders
// (PBR_GBuffer.glsl, PBR_GBuffer_Skinned.glsl, PBR_MultiLight.glsl). These pin
// the formula WITHOUT a GL context (headless CI), mirroring the *MathTest.cpp
// style of ScreenSpaceReflectionMathTest / ContactShadowMathTest.
//
// The shaders compute, per vertex/fragment:
//     ndcCurr  = (u_ViewProjection     * u_Model     * pos).xy / w
//     ndcPrev  = (u_PrevViewProjection * u_PrevModel * pos).xy / w
//     velocity = (ndcCurr - ndcPrev) * 0.5            // → UV-space delta
//
// The 0.5 converts the [-2,2] NDC difference into the [0,1] UV delta that TAA
// and motion blur consume as `prevUV = uv - velocity`. So velocity is exactly
// `uvCurr - uvPrev`: it points from where the surface WAS to where it is now.
//
// What this file proves (sign + magnitude, the two failure modes that silently
// break TAA reprojection and motion-blur direction):
//   * A perfectly static object under a static camera produces zero velocity.
//   * An object translating +X (world) under a static camera produces +X
//     screen velocity (and ~0 in Y).
//   * A static object under a camera panning +X produces -X screen velocity
//     (the world appears to slide left) — the opposite sign of object motion.
//   * Velocity magnitude scales ~linearly with object displacement.
//
// Complementary GPU coverage (PostProcessPropertyTests.cpp) proves the motion
// blur shader actually consumes this buffer and smears along the velocity
// vector; this file proves the buffer's contents are correct in the first place.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test brevity

namespace
{
    // Mirror of the velocity computation in the G-Buffer shaders. `localPos` is
    // the object-space vertex position; the model matrices place it in the world
    // and the view-projection matrices project current vs previous frame.
    glm::vec2 ComputeMotionVector(const glm::mat4& viewProjection,
                                  const glm::mat4& model,
                                  const glm::mat4& prevViewProjection,
                                  const glm::mat4& prevModel,
                                  const glm::vec3& localPos)
    {
        const glm::vec4 clipCurr = viewProjection * model * glm::vec4(localPos, 1.0f);
        const glm::vec4 clipPrev = prevViewProjection * prevModel * glm::vec4(localPos, 1.0f);
        const glm::vec2 ndcCurr = glm::vec2(clipCurr) / clipCurr.w;
        const glm::vec2 ndcPrev = glm::vec2(clipPrev) / clipPrev.w;
        return (ndcCurr - ndcPrev) * 0.5f;
    }

    // A camera at `eye` looking down -Z (world +X maps to screen +X, +Y up).
    glm::mat4 MakeViewProjection(const glm::vec3& eye)
    {
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
        const glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        return proj * view;
    }
} // namespace

// A stationary object viewed by a stationary camera must produce exactly zero
// motion — otherwise TAA reprojects history off the static surface (ghosting)
// and motion blur smears a still frame.
TEST(MotionVectorMath, StaticObjectStaticCameraIsZero)
{
    const glm::mat4 vp = MakeViewProjection(glm::vec3(0.0f));
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f));

    const glm::vec2 velocity = ComputeMotionVector(vp, model, vp, model, glm::vec3(0.0f));

    EXPECT_NEAR(velocity.x, 0.0f, 1e-6f);
    EXPECT_NEAR(velocity.y, 0.0f, 1e-6f);
}

// An object translating +X in world space (static camera) moves rightward on
// screen, so the velocity's X component is positive and Y is ~zero.
TEST(MotionVectorMath, ObjectMovingRightYieldsPositiveXVelocity)
{
    const glm::mat4 vp = MakeViewProjection(glm::vec3(0.0f));
    const glm::mat4 prevModel = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f));
    const glm::mat4 currModel = glm::translate(glm::mat4(1.0f), glm::vec3(0.1f, 0.0f, -5.0f));

    const glm::vec2 velocity = ComputeMotionVector(vp, currModel, vp, prevModel, glm::vec3(0.0f));

    EXPECT_GT(velocity.x, 1e-4f) << "object moving +X should give +X screen velocity";
    EXPECT_NEAR(velocity.y, 0.0f, 1e-5f) << "pure horizontal motion has no vertical velocity";
}

// A static object viewed by a camera panning +X appears to slide left, so the
// velocity's X component is negative — the opposite sign of object motion. This
// is the case camera-only reprojection already handled; the per-object buffer
// must reproduce it for stationary geometry.
TEST(MotionVectorMath, CameraPanningRightYieldsNegativeXVelocity)
{
    const glm::mat4 prevVP = MakeViewProjection(glm::vec3(0.0f));
    const glm::mat4 currVP = MakeViewProjection(glm::vec3(0.1f, 0.0f, 0.0f));
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f));

    const glm::vec2 velocity = ComputeMotionVector(currVP, model, prevVP, model, glm::vec3(0.0f));

    EXPECT_LT(velocity.x, -1e-4f) << "camera panning +X makes the static world slide -X on screen";
    EXPECT_NEAR(velocity.y, 0.0f, 1e-5f);
}

// Object and camera motion of equal-and-opposite world displacement cancel:
// tracking the object with the camera leaves it stationary on screen.
TEST(MotionVectorMath, ObjectAndCameraMovingTogetherCancel)
{
    const glm::mat4 prevVP = MakeViewProjection(glm::vec3(0.0f));
    const glm::mat4 currVP = MakeViewProjection(glm::vec3(0.1f, 0.0f, 0.0f));
    const glm::mat4 prevModel = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f));
    const glm::mat4 currModel = glm::translate(glm::mat4(1.0f), glm::vec3(0.1f, 0.0f, -5.0f));

    const glm::vec2 velocity = ComputeMotionVector(currVP, currModel, prevVP, prevModel, glm::vec3(0.0f));

    // The object sits at the same screen position both frames (camera follows
    // it exactly), so the residual velocity is negligible.
    EXPECT_NEAR(velocity.x, 0.0f, 1e-4f);
    EXPECT_NEAR(velocity.y, 0.0f, 1e-4f);
}

// Velocity magnitude scales ~linearly with object displacement for small steps
// (constant w at fixed depth), so doubling the move roughly doubles velocity.
TEST(MotionVectorMath, VelocityScalesWithDisplacement)
{
    const glm::mat4 vp = MakeViewProjection(glm::vec3(0.0f));
    const glm::mat4 prevModel = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -5.0f));
    const glm::mat4 model1 = glm::translate(glm::mat4(1.0f), glm::vec3(0.05f, 0.0f, -5.0f));
    const glm::mat4 model2 = glm::translate(glm::mat4(1.0f), glm::vec3(0.10f, 0.0f, -5.0f));

    const f32 v1 = ComputeMotionVector(vp, model1, vp, prevModel, glm::vec3(0.0f)).x;
    const f32 v2 = ComputeMotionVector(vp, model2, vp, prevModel, glm::vec3(0.0f)).x;

    EXPECT_GT(v1, 0.0f);
    EXPECT_GT(v2, v1) << "larger displacement → larger velocity";
    EXPECT_NEAR(v2 / v1, 2.0f, 0.02f) << "doubling the world displacement should ~double the velocity";
}
