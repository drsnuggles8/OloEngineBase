// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GPUOcclusionCullGPUTest.cpp
//
// Layer-2 GPU harness for the Hi-Z occlusion cull compute (#431). Where
// GPUOcclusionCullParityTest pins the math on the CPU, this drives the REAL
// `assets/shaders/compute/InstanceOcclusionCull.comp` on the GPU and reads back
// the indirect draw's surviving instance count — proving the shader compiles,
// links, and culls the right instances on hardware.
//
// Setup mirrors the production GPUFrustumCuller dispatch (same SSBO bindings:
// 16 = input InstanceData[], 15 = compacted survivors, 17 = indirect command;
// camera UBO at binding 0; HZB at texture unit 0), but uses raw GL so it needs
// only a GL context — no Renderer::Init().
//
// Scene: a full-screen occluder "wall" baked into a constant-depth HZB pyramid,
// with a field of instances split between BEHIND the wall (must be culled) and
// IN FRONT of it (must survive). The asserts:
//   * occlusion OFF  -> all frustum-visible instances survive (sanity);
//   * occlusion ON   -> only the in-front instances survive (real reduction).
//
// Classification: shaderpipe (single compute shader on the GPU).
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <array>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Indirect command layout matching the std430 InstanceCullIndirect block
        // (5 active u32 + 3 pad) — 32 bytes, same as IndirectCommandPOD in
        // GPUFrustumCuller.cpp.
        struct IndirectCommand
        {
            u32 Count;
            u32 InstanceCount;
            u32 FirstIndex;
            u32 BaseVertex;
            u32 BaseInstance;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };

        constexpr u32 kHZBSize = 256;
        constexpr u32 kHZBMips = 9; // log2(256) + 1

        glm::mat4 MakeViewProjection()
        {
            const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
            const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            return projection * view;
        }

        f32 DeviceZForDistance(const glm::mat4& vp, f32 dist)
        {
            const glm::vec4 clip = vp * glm::vec4(0.0f, 0.0f, -dist, 1.0f);
            return (clip.z / clip.w) * 0.5f + 0.5f;
        }

        // Constant-depth occluder pyramid: every texel at every mip holds wallZ.
        GLuint MakeWallHZB(f32 wallZ)
        {
            GLuint tex = 0;
            ::glCreateTextures(GL_TEXTURE_2D, 1, &tex);
            ::glTextureStorage2D(tex, static_cast<GLsizei>(kHZBMips), GL_R32F,
                                 static_cast<GLsizei>(kHZBSize), static_cast<GLsizei>(kHZBSize));
            for (u32 m = 0; m < kHZBMips; ++m)
                ::glClearTexImage(tex, static_cast<GLint>(m), GL_RED, GL_FLOAT, &wallZ);
            ::glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
            ::glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            ::glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            ::glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            return tex;
        }

        // Build the instance field: `behind` cubes at z = behindZ (occluded by
        // the wall), `front` cubes at z = frontZ (visible). All near screen
        // centre so their reprojected rectangles stay fully on-screen.
        std::vector<InstanceData> MakeInstances(u32 behind, u32 front, f32 behindZ, f32 frontZ)
        {
            std::vector<InstanceData> out;
            out.reserve(behind + front);
            const auto push = [&out](f32 x, f32 z)
            {
                InstanceData inst;
                inst.Transform = glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, z));
                // Static instances: previous transform == current. The occlusion
                // cull reprojects phase-1 / single-phase bounds with PrevTransform
                // (matching the previous-frame HZB), so it must be populated.
                inst.PrevTransform = inst.Transform;
                out.push_back(inst);
            };
            for (u32 i = 0; i < behind; ++i)
                push(-1.0f + 2.0f * (static_cast<f32>(i) / static_cast<f32>(std::max(behind - 1, 1u))), behindZ);
            for (u32 i = 0; i < front; ++i)
                push(-1.0f + 2.0f * (static_cast<f32>(i) / static_cast<f32>(std::max(front - 1, 1u))), frontZ);
            return out;
        }

        // Dispatch InstanceOcclusionCull.comp over `instances`; return the
        // surviving instance count read back from the indirect command.
        u32 RunCull(ComputeShader& cs, const std::vector<InstanceData>& instances,
                    const glm::mat4& vp, GLuint hzbTex, bool occlusionEnabled)
        {
            const u32 count = static_cast<u32>(instances.size());

            // Input (16), output (15), indirect (17) SSBOs.
            GLuint inBuf = 0, outBuf = 0, indirectBuf = 0;
            ::glCreateBuffers(1, &inBuf);
            ::glNamedBufferStorage(inBuf, static_cast<GLsizeiptr>(count * sizeof(InstanceData)),
                                   instances.data(), 0);
            ::glCreateBuffers(1, &outBuf);
            ::glNamedBufferStorage(outBuf, static_cast<GLsizeiptr>(count * sizeof(InstanceData)),
                                   nullptr, GL_DYNAMIC_STORAGE_BIT);
            IndirectCommand seed{};
            seed.Count = 36;        // arbitrary index count
            seed.InstanceCount = 0; // compute atomic-adds survivors
            ::glCreateBuffers(1, &indirectBuf);
            ::glNamedBufferStorage(indirectBuf, sizeof(IndirectCommand), &seed,
                                   GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);

            // Camera UBO (binding 0) — only u_ViewProjection is read by the cull.
            std::array<glm::mat4, 4> cameraData{ vp, glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
            GLuint cameraUBO = 0;
            ::glCreateBuffers(1, &cameraUBO);
            ::glNamedBufferStorage(cameraUBO, static_cast<GLsizeiptr>(cameraData.size() * sizeof(glm::mat4)),
                                   cameraData.data(), 0);

            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, outBuf);
            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, inBuf);
            ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 17, indirectBuf);
            ::glBindBufferBase(GL_UNIFORM_BUFFER, 0, cameraUBO);
            ::glBindTextureUnit(0, hzbTex);

            cs.Bind();
            cs.SetUint("u_InstanceCount", count);
            cs.SetFloat4("u_LocalBoundingSphere", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            cs.SetFloat("u_RadiusExpansion", 1.3f * 1.05f);
            cs.SetInt("u_OcclusionEnabled", occlusionEnabled ? 1 : 0);
            cs.SetMat4("u_PrevViewProjection", vp);
            cs.SetFloat2("u_HZBSize", glm::vec2(static_cast<f32>(kHZBSize)));
            cs.SetFloat2("u_HZBUVFactor", glm::vec2(1.0f));
            cs.SetInt("u_HZBMipCount", static_cast<int>(kHZBMips));
            cs.SetFloat("u_OcclusionDepthBias", 0.0f);

            constexpr u32 kLocalSize = 256;
            ::glDispatchCompute((count + kLocalSize - 1) / kLocalSize, 1, 1);
            ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

            IndirectCommand result{};
            ::glGetNamedBufferSubData(indirectBuf, 0, sizeof(IndirectCommand), &result);

            ::glDeleteBuffers(1, &inBuf);
            ::glDeleteBuffers(1, &outBuf);
            ::glDeleteBuffers(1, &indirectBuf);
            ::glDeleteBuffers(1, &cameraUBO);

            return result.InstanceCount;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The real GPU shader must keep every frustum-visible instance when
    // occlusion is off, and drop the ones hidden behind the wall when on.
    // -------------------------------------------------------------------------
    TEST(GPUOcclusionCullGPUTest, WallHidesBehindInstancesKeepsFront)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto cs = ComputeShader::Create("assets/shaders/compute/InstanceOcclusionCull.comp");
        ASSERT_TRUE(cs && cs->IsValid()) << "InstanceOcclusionCull.comp failed to compile/link";

        const glm::mat4 vp = MakeViewProjection();
        const f32 wallZ = DeviceZForDistance(vp, 15.0f);
        const GLuint hzb = MakeWallHZB(wallZ);

        constexpr u32 kBehind = 40;
        constexpr u32 kFront = 10;
        const auto instances = MakeInstances(kBehind, kFront, -40.0f, -6.0f);

        // Sanity: with occlusion off, the frustum test alone keeps them all
        // (every instance is on-screen and within the far plane).
        const u32 survivorsNoOcclusion = RunCull(*cs, instances, vp, hzb, /*occlusionEnabled*/ false);
        EXPECT_EQ(survivorsNoOcclusion, kBehind + kFront)
            << "Frustum-only must keep every on-screen instance.";

        // With occlusion on, the wall hides the behind instances; the front
        // ones survive — a real, GPU-verified draw reduction.
        const u32 survivorsOcclusion = RunCull(*cs, instances, vp, hzb, /*occlusionEnabled*/ true);
        EXPECT_EQ(survivorsOcclusion, kFront)
            << "HZB occlusion must cull instances hidden behind the wall and keep the front ones.";
        EXPECT_LT(survivorsOcclusion, survivorsNoOcclusion)
            << "Occlusion must produce a measurable survivor reduction on the GPU.";

        ::glDeleteTextures(1, &hzb);
    }

    // -------------------------------------------------------------------------
    // Empty depth (far plane everywhere) must never occlude — guards against an
    // inverted depth comparison that would cull visible geometry.
    // -------------------------------------------------------------------------
    TEST(GPUOcclusionCullGPUTest, EmptyDepthCullsNothing)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto cs = ComputeShader::Create("assets/shaders/compute/InstanceOcclusionCull.comp");
        ASSERT_TRUE(cs && cs->IsValid()) << "InstanceOcclusionCull.comp failed to compile/link";

        const glm::mat4 vp = MakeViewProjection();
        const GLuint farHZB = MakeWallHZB(1.0f); // far plane = nothing in front

        const auto instances = MakeInstances(40, 10, -40.0f, -6.0f);
        const u32 survivors = RunCull(*cs, instances, vp, farHZB, /*occlusionEnabled*/ true);
        EXPECT_EQ(survivors, 50u) << "An empty (far-plane) HZB must not occlude anything.";

        ::glDeleteTextures(1, &farHZB);
    }

    // -------------------------------------------------------------------------
    // Two-phase (#431 Stage 2): the load-bearing no-popping contract. Phase 1
    // tests against LAST frame's depth (a wall in front of the back cubes), so
    // those cubes are deferred to the reject list. Phase 2 tests the reject list
    // against THIS frame's depth (the wall gone — a disocclusion), so every
    // deferred cube is recovered. Single-phase would have dropped them for a
    // frame → the visible pop this test guards against.
    // -------------------------------------------------------------------------
    TEST(GPUOcclusionCullGPUTest, TwoPhaseRecoversDisoccludedInstances)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto cs = ComputeShader::Create("assets/shaders/compute/InstanceOcclusionCull.comp");
        ASSERT_TRUE(cs && cs->IsValid()) << "InstanceOcclusionCull.comp failed to compile/link";

        const glm::mat4 vp = MakeViewProjection();
        const f32 wallZ = DeviceZForDistance(vp, 15.0f);
        const GLuint wallHZB = MakeWallHZB(wallZ); // last frame: wall occludes the back cubes
        const GLuint emptyHZB = MakeWallHZB(1.0f); // this frame: wall gone (disocclusion)

        constexpr u32 kBehind = 40;
        constexpr u32 kFront = 10;
        const auto instances = MakeInstances(kBehind, kFront, -40.0f, -6.0f);
        const u32 count = static_cast<u32>(instances.size());

        const auto makeInstanceSSBO = [](const std::vector<InstanceData>* init, u32 instanceCount) -> GLuint
        {
            GLuint b = 0;
            ::glCreateBuffers(1, &b);
            ::glNamedBufferStorage(b, static_cast<GLsizeiptr>(instanceCount * sizeof(InstanceData)),
                                   init ? init->data() : nullptr, GL_DYNAMIC_STORAGE_BIT);
            return b;
        };
        const auto makeIndirect = []() -> GLuint
        {
            IndirectCommand seed{};
            seed.Count = 36;
            GLuint b = 0;
            ::glCreateBuffers(1, &b);
            ::glNamedBufferStorage(b, sizeof(IndirectCommand), &seed, GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);
            return b;
        };

        const GLuint inBuf = makeInstanceSSBO(&instances, count);
        const GLuint p1Out = makeInstanceSSBO(nullptr, count);
        const GLuint rejBuf = makeInstanceSSBO(nullptr, count);
        const GLuint p2Out = makeInstanceSSBO(nullptr, count);
        const GLuint p1Ind = makeIndirect();
        const GLuint p2Ind = makeIndirect();
        const u32 zero = 0;
        GLuint rejCnt = 0;
        ::glCreateBuffers(1, &rejCnt);
        ::glNamedBufferStorage(rejCnt, sizeof(u32), &zero, GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT);

        std::array<glm::mat4, 4> cameraData{ vp, glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
        GLuint camUBO = 0;
        ::glCreateBuffers(1, &camUBO);
        ::glNamedBufferStorage(camUBO, static_cast<GLsizeiptr>(cameraData.size() * sizeof(glm::mat4)), cameraData.data(), 0);
        ::glBindBufferBase(GL_UNIFORM_BUFFER, 0, camUBO);

        const auto setCommon = [&]()
        {
            cs->SetUint("u_InstanceCount", count);
            cs->SetFloat4("u_LocalBoundingSphere", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            cs->SetFloat("u_RadiusExpansion", 1.3f * 1.05f);
            cs->SetInt("u_OcclusionEnabled", 1);
            cs->SetMat4("u_PrevViewProjection", vp);
            cs->SetFloat2("u_HZBSize", glm::vec2(static_cast<f32>(kHZBSize)));
            cs->SetFloat2("u_HZBUVFactor", glm::vec2(1.0f));
            cs->SetInt("u_HZBMipCount", static_cast<int>(kHZBMips));
            cs->SetFloat("u_OcclusionDepthBias", 0.0f);
        };
        const u32 groups = (count + 255u) / 256u;

        // ── Phase 1: vs the wall HZB; defer occluded back cubes to the reject list.
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, inBuf);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, p1Out);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 17, p1Ind);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 18, rejBuf);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 19, rejCnt);
        ::glBindTextureUnit(0, wallHZB);
        cs->Bind();
        setCommon();
        cs->SetInt("u_WriteRejected", 1);
        cs->SetInt("u_Phase2", 0);
        ::glDispatchCompute(groups, 1, 1);
        ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        IndirectCommand p1{};
        ::glGetNamedBufferSubData(p1Ind, 0, sizeof(IndirectCommand), &p1);
        u32 rejectedCount = 0;
        ::glGetNamedBufferSubData(rejCnt, 0, sizeof(u32), &rejectedCount);

        EXPECT_EQ(p1.InstanceCount, kFront) << "Phase 1 must draw only the unoccluded front cubes.";
        EXPECT_EQ(rejectedCount, kBehind) << "Phase 1 must defer the wall-occluded back cubes to the reject list.";

        // ── Phase 2: re-test the reject list (bound as input @16) vs the empty HZB.
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, rejBuf);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, p2Out);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 17, p2Ind);
        ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 19, rejCnt);
        ::glBindTextureUnit(0, emptyHZB);
        cs->Bind();
        setCommon();
        cs->SetInt("u_WriteRejected", 0);
        cs->SetInt("u_Phase2", 1);
        ::glDispatchCompute(groups, 1, 1);
        ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        IndirectCommand p2{};
        ::glGetNamedBufferSubData(p2Ind, 0, sizeof(IndirectCommand), &p2);

        EXPECT_EQ(p2.InstanceCount, kBehind)
            << "Phase 2 must recover every disoccluded cube (the wall is gone this frame) — no popping.";
        EXPECT_EQ(p1.InstanceCount + p2.InstanceCount, kFront + kBehind)
            << "Across both phases every visible instance must be drawn exactly once.";

        const std::array<GLuint, 8> buffers{ inBuf, p1Out, rejBuf, p2Out, p1Ind, p2Ind, rejCnt, camUBO };
        ::glDeleteBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
        ::glDeleteTextures(1, &wallHZB);
        ::glDeleteTextures(1, &emptyHZB);
    }
} // namespace OloEngine::Tests
