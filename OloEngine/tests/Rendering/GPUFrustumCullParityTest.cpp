// =============================================================================
// GPUFrustumCullParityTest.cpp
//
// Pins the GLSL-side math in `assets/shaders/compute/InstanceFrustumCull.comp`
// to match the CPU code in `OloEngine/Renderer/Frustum.cpp` and
// `BoundingSphere::Transform`. If either side drifts in isolation, scenes
// that route their dense scatter through `Renderer3D::SubmitGPUCulledInstanced`
// would silently render a different visible set than the CPU loop produces —
// either dropping visible instances (popping at the frustum edge) or
// admitting offscreen ones (wasted draw bandwidth).
//
// Strategy: re-implement the compute shader's math in C++ exactly as it
// appears in the shader, then drive both that re-implementation and the
// engine's CPU cull (`Frustum::IsBoundingSphereVisible` after
// `BoundingSphere::Transform`) over a randomised but deterministic set of
// (transform, viewProjection) pairs. Any survivor-set mismatch fails.
//
// Classification: L1 / shaderpipe (pure CPU, no GL context).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <random>

namespace OloEngine::Tests
{
    namespace
    {
        // Re-implementation of InstanceFrustumCull.comp's plane extraction.
        // Must stay byte-for-byte equivalent to the GLSL `extractPlane()`
        // helper — any divergence is the bug this test catches.
        glm::vec4 ExtractPlane_GLSLEquivalent(const glm::mat4& vp, int col, f32 colSign)
        {
            glm::vec4 p{
                vp[0][3] + colSign * vp[0][col],
                vp[1][3] + colSign * vp[1][col],
                vp[2][3] + colSign * vp[2][col],
                vp[3][3] + colSign * vp[3][col]
            };
            const f32 len = glm::length(glm::vec3(p));
            return p / len;
        }

        // Re-implementation of InstanceFrustumCull.comp's per-instance test.
        bool IsInstanceVisible_GLSLEquivalent(const glm::mat4& instanceTransform,
                                              const glm::vec4& localSphere,
                                              f32 radiusExpansion,
                                              const glm::mat4& viewProjection)
        {
            const glm::vec4 wc4 = instanceTransform * glm::vec4(glm::vec3(localSphere), 1.0f);
            const glm::vec3 worldCenter = glm::vec3(wc4) / wc4.w;
            const f32 maxColumnScale = glm::max(glm::max(
                                                    glm::length(glm::vec3(instanceTransform[0])),
                                                    glm::length(glm::vec3(instanceTransform[1]))),
                                                glm::length(glm::vec3(instanceTransform[2])));
            const f32 worldRadius = localSphere.w * maxColumnScale * radiusExpansion;

            const std::array<glm::vec4, 6> planes{
                ExtractPlane_GLSLEquivalent(viewProjection, 0, 1.0f),  // Left
                ExtractPlane_GLSLEquivalent(viewProjection, 0, -1.0f), // Right
                ExtractPlane_GLSLEquivalent(viewProjection, 1, 1.0f),  // Bottom
                ExtractPlane_GLSLEquivalent(viewProjection, 1, -1.0f), // Top
                ExtractPlane_GLSLEquivalent(viewProjection, 2, 1.0f),  // Near
                ExtractPlane_GLSLEquivalent(viewProjection, 2, -1.0f)  // Far
            };
            for (const auto& plane : planes)
            {
                const f32 signedDistance = glm::dot(glm::vec3(plane), worldCenter) + plane.w;
                if (signedDistance < -worldRadius)
                    return false;
            }
            return true;
        }

        // Engine CPU path — what `Renderer3D::DrawMeshInstanced` would do.
        bool IsInstanceVisible_CPU(const glm::mat4& instanceTransform,
                                   const BoundingSphere& localSphere,
                                   const Frustum& frustum)
        {
            // The CPU path applies a 1.3× pre-expansion to the local sphere,
            // then calls BoundingSphere::Transform which adds another 1.05×
            // safety margin. The combined factor (1.365×) is exactly the
            // `radiusExpansion` uniform the shader receives.
            BoundingSphere expanded = localSphere;
            expanded.Radius *= 1.3f;
            const BoundingSphere worldSphere = expanded.Transform(instanceTransform);
            return frustum.IsBoundingSphereVisible(worldSphere);
        }
    } // namespace

    TEST(GPUFrustumCullParity, RandomisedInstancesMatchCPU)
    {
        // Standard perspective camera roughly matching the editor's defaults:
        // 60° FOV, 16:9, near 0.1, far 200. Same matrix gets handed to both
        // sides — divergence in survival decisions is what the test pins.
        const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 200.0f);
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 5.0f, 20.0f),
                                           glm::vec3(0.0f, 0.0f, 0.0f),
                                           glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 viewProjection = projection * view;
        const Frustum frustum(viewProjection);

        // Unit-sphere local-space bounds — covers the cube/sphere/cone
        // primitives the InstancingDemo uses.
        const BoundingSphere localSphereCPU{ glm::vec3(0.0f, 0.0f, 0.0f), 1.0f };
        const glm::vec4 localSphereGLSL{ localSphereCPU.Center, localSphereCPU.Radius };
        constexpr f32 kRadiusExpansion = 1.3f * 1.05f;

        std::mt19937 rng(0xC0FFEE);
        std::uniform_real_distribution<f32> posDist(-30.0f, 30.0f);
        std::uniform_real_distribution<f32> scaleDist(0.25f, 3.5f);
        std::uniform_real_distribution<f32> rotDist(0.0f, glm::two_pi<f32>());

        // 200 random instances. Each is a translate * rotate * scale matrix
        // covering positions both well inside the frustum and well outside.
        constexpr u32 kInstanceCount = 200;
        u32 visibleCPU = 0;
        u32 visibleGLSL = 0;
        u32 mismatchCount = 0;
        for (u32 i = 0; i < kInstanceCount; ++i)
        {
            const glm::vec3 pos{ posDist(rng), posDist(rng), posDist(rng) };
            const glm::vec3 axis = glm::normalize(glm::vec3(rotDist(rng) - glm::pi<f32>(),
                                                            rotDist(rng) - glm::pi<f32>(),
                                                            rotDist(rng) - glm::pi<f32>()) +
                                                  glm::vec3(0.001f));
            const f32 angle = rotDist(rng);
            const glm::vec3 scale{ scaleDist(rng), scaleDist(rng), scaleDist(rng) };

            const glm::mat4 transform = glm::translate(glm::mat4(1.0f), pos) *
                                        glm::rotate(glm::mat4(1.0f), angle, axis) *
                                        glm::scale(glm::mat4(1.0f), scale);

            const bool cpu = IsInstanceVisible_CPU(transform, localSphereCPU, frustum);
            const bool glsl = IsInstanceVisible_GLSLEquivalent(transform, localSphereGLSL, kRadiusExpansion, viewProjection);

            if (cpu)
                ++visibleCPU;
            if (glsl)
                ++visibleGLSL;
            if (cpu != glsl)
                ++mismatchCount;

            EXPECT_EQ(cpu, glsl) << "Instance " << i << " visibility mismatch — CPU=" << cpu
                                 << ", GLSL=" << glsl
                                 << ". Pos=(" << pos.x << "," << pos.y << "," << pos.z << ")"
                                 << " scaleMax=" << glm::max(glm::max(scale.x, scale.y), scale.z);
        }

        // Sanity: with random positions in [-30,30]^3 and a frustum looking
        // toward the origin, the survivor count should land in a roughly
        // overlapping band on both sides. If either side culls everything,
        // something has broken upstream (e.g., the plane extraction had a
        // sign flip).
        EXPECT_GT(visibleCPU, 0u) << "CPU cull rejected EVERY randomised instance — frustum setup likely broken";
        EXPECT_GT(visibleGLSL, 0u) << "GLSL cull rejected EVERY randomised instance — plane extraction likely broken";
        EXPECT_LT(visibleCPU, kInstanceCount) << "CPU cull admitted EVERY randomised instance — frustum likely degenerate";
        EXPECT_EQ(mismatchCount, 0u) << "GLSL math drifted from CPU math; check InstanceFrustumCull.comp vs Frustum.cpp";
    }

    TEST(GPUFrustumCullParity, EdgeCaseInstancesMatchCPU)
    {
        // Hand-crafted edge cases the random sampler may not hit reliably:
        // instances dead-centre, just-inside frustum, just-outside frustum,
        // exactly on the near plane, far-corner. Each one exercises a
        // different plane being the limiting factor.
        const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 200.0f);
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f),
                                           glm::vec3(0.0f, 0.0f, 0.0f),
                                           glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 viewProjection = projection * view;
        const Frustum frustum(viewProjection);

        const BoundingSphere localSphereCPU{ glm::vec3(0.0f), 1.0f };
        const glm::vec4 localSphereGLSL{ 0.0f, 0.0f, 0.0f, 1.0f };
        constexpr f32 kRadiusExpansion = 1.3f * 1.05f;

        struct EdgeCase
        {
            const char* Name;
            glm::vec3 Position;
            f32 UniformScale;
        };
        const std::array<EdgeCase, 6> cases{ {
            { "Origin (visible)", { 0.0f, 0.0f, 0.0f }, 1.0f },
            { "Just inside near plane (z = 9.7)", { 0.0f, 0.0f, 9.7f }, 1.0f },
            { "Behind camera (z = 15)", { 0.0f, 0.0f, 15.0f }, 1.0f },
            { "Far away (z = -190)", { 0.0f, 0.0f, -190.0f }, 1.0f },
            { "Past far plane (z = -250)", { 0.0f, 0.0f, -250.0f }, 1.0f },
            { "Huge scale far away", { 0.0f, 0.0f, -195.0f }, 5.0f },
        } };
        for (const auto& c : cases)
        {
            const glm::mat4 transform = glm::translate(glm::mat4(1.0f), c.Position) *
                                        glm::scale(glm::mat4(1.0f), glm::vec3(c.UniformScale));
            const bool cpu = IsInstanceVisible_CPU(transform, localSphereCPU, frustum);
            const bool glsl = IsInstanceVisible_GLSLEquivalent(transform, localSphereGLSL, kRadiusExpansion, viewProjection);
            EXPECT_EQ(cpu, glsl) << c.Name << ": CPU=" << cpu << " GLSL=" << glsl;
        }
    }
} // namespace OloEngine::Tests
