// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// BindlessHeapGpuTest.cpp
//
// Issue #691 Phase 3. The END-TO-END proof that the heap model works on real
// hardware: a texture goes into RHI::DescriptorHeap, the pass writes nothing but
// an integer offset into a UBO, and a shader that never names a sampler binding
// reads the right texels back.
//
// WHY THIS TEST AS WELL AS A CONVERTED PRODUCTION PASS. `SSAORenderPass` IS
// converted, and running the editor with `OLO_RHI_BINDLESS=1` does render it
// through the heap. That is necessary evidence and it is not sufficient: a
// converted pass proves "SSAO still looks like SSAO", which a dozen unrelated
// bugs also produce, and against a screen-space effect the difference between
// "correct" and "sampling a plausible wrong texture" sits under the noise floor
// (docs/agent-rules/live-verification-noise-floor.md).
//
// This renders a KNOWN pattern through the real heap and reads the exact texels
// back, so a wrong offset, a dead descriptor and an unbound heap each fail
// differently and specifically. The slot-indexed case goes further and drives
// `RGCommandContext`'s actual pass-side seam, which is what makes the std140
// offset-table layout — the one thing in the conversion that fails silently and
// plausibly — a caught bug rather than a shipped one.
//
// It also covers the three failure modes that are silent by construction:
//
//   * A WRONG OFFSET renders a different real texture, not black. The two heap
//     textures here are deliberately different solid colours, so swapping their
//     offsets is a colour change rather than an absence.
//   * A STALE DESCRIPTOR renders the previous tenant of the slot. Poison turns
//     that into black; `PoisonedSlotSamplesBlackRatherThanThePreviousTenant`
//     is the test that says so.
//   * AN UNBOUND HEAP leaves every offset a perfectly valid index into whatever
//     buffer is at that binding point.
//
// Skips cleanly with no GL 4.6 context, and again if the driver lacks
// GL_ARB_bindless_texture — the extension is not universally available, which is
// the whole reason the slot-based path survives as a fallback.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Renderer/Texture3D.h"
#include "Platform/OpenGL/OpenGLDescriptorHeap.h"

#include <gtest/gtest.h>

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Mirrors include/BindlessHeap.glsl's OLO_BINDLESS branch. Duplicated
        // rather than #include'd because this shader is compiled with
        // glShaderSource directly — the engine's includer resolves paths for the
        // shaderc path, which bindless GLSL cannot travel at all
        // (BindlessShaderPipelineTest pins that). The duplication is deliberate
        // and small; if the two ever disagree, this test fails rather than
        // silently proving a different shader.
        constexpr const char* kVertexSource = R"(#version 460 core
layout(location = 0) in vec2 a_Position;
layout(location = 0) out vec2 v_TexCoord;
void main()
{
    v_TexCoord = a_Position * 0.5 + 0.5;
    gl_Position = vec4(a_Position, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 460 core
#extension GL_ARB_bindless_texture : require

layout(std430, binding = 45) readonly buffer OloResourceHeapBlock
{
    uvec2 g_OloResourceHeap[];
};

// The ONLY thing the "pass" transports. Under the slot-based model this would
// be a bind call per texture; here it is two integers in a uniform block.
layout(std140, binding = 9) uniform HeapOffsets
{
    uint u_LeftOffset;
    uint u_RightOffset;
};

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

void main()
{
    uint offset = (v_TexCoord.x < 0.5) ? u_LeftOffset : u_RightOffset;
    o_Color = texture(sampler2D(g_OloResourceHeap[offset]), v_TexCoord);
}
)";

        // The shape a CONVERTED PASS produces: no direct offsets, only TEX_*
        // slot numbers going through the std140 offset table. Mirrors
        // include/BindlessHeap.glsl's OLO_HEAP_OFFSET / OLO_HEAP_TEX_2D macros
        // and ShaderBindingLayout::UBO_HEAP_OFFSETS, and the `#define u_X` form
        // is exactly how SSAO.glsl declares its inputs under OLO_BINDLESS — so
        // if the two ever drift, this test fails rather than silently proving a
        // different shader.
        constexpr const char* kSlotIndexedFragment = R"(#version 460 core
#extension GL_ARB_bindless_texture : require

layout(std430, binding = 45) readonly buffer OloResourceHeapBlock
{
    uvec2 g_OloResourceHeap[];
};

layout(std140, binding = 56) uniform OloHeapOffsetBlock
{
    uvec4 g_OloHeapOffsets[16];
};

#define OLO_HEAP_OFFSET(texSlot) (g_OloHeapOffsets[(texSlot) >> 2][(texSlot) & 3])
#define OLO_HEAP_TEX_2D(texSlot) sampler2D(g_OloResourceHeap[OLO_HEAP_OFFSET(texSlot)])

#define u_DepthTexture OLO_HEAP_TEX_2D(19)   // TEX_POSTPROCESS_DEPTH
#define u_NormalsTexture OLO_HEAP_TEX_2D(22) // TEX_SCENE_NORMALS

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

void main()
{
    o_Color = (v_TexCoord.x < 0.5) ? texture(u_DepthTexture, v_TexCoord)
                                   : texture(u_NormalsTexture, v_TexCoord);
}
)";

        // -------------------------------------------------------------------
        // STORAGE IMAGES — the second descriptor kind (ADR 0011 amendment (26)).
        //
        // Both compute sources below mirror `include/BindlessHeap.glsl`'s image
        // macros CHARACTER FOR CHARACTER after expansion, for the same reason the
        // sampler sources above do: this test compiles with glShaderSource
        // directly, so if the header and these ever disagree the test fails
        // rather than silently proving a different shader.
        //
        // The three things that make an image macro different from a sampler one
        // are all visible here and all forced:
        //   * the format layout qualifier is part of the type,
        //   * the declaration is LOCAL (an image initialised from a buffer read is
        //     not a constant expression, so it cannot sit at global scope),
        //   * image units are rebased by OLO_HEAP_IMAGE_BASE, because GL image
        //     units and texture units are separate namespaces that both start at 0.
        // -------------------------------------------------------------------

        // Direct-offset form: the offset arrives in its own tiny UBO, so this
        // isolates "can a shader build an image from a heap descriptor at all"
        // from "does the offset table reach it".
        constexpr const char* kImageComputeSource = R"(#version 460 core
#extension GL_ARB_bindless_texture : require
layout(local_size_x = 2, local_size_y = 2) in;

layout(std430, binding = 45) readonly buffer OloResourceHeapBlock
{
    uvec2 g_OloResourceHeap[];
};

layout(std140, binding = 9) uniform HeapOffsets
{
    uint u_ImageOffset;
};

void main()
{
    layout(r32f) image2D u_Output = layout(r32f) image2D(g_OloResourceHeap[u_ImageOffset]);
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(u_Output, coord, vec4(float(coord.x) + 10.0 * float(coord.y), 0.0, 0.0, 0.0));
}
)";

        // Slot-indexed form: exactly what a CONVERTED compute pass produces —
        // no offset of its own, only an image UNIT going through the shared
        // offset table at its rebased index.
        constexpr const char* kSlotIndexedImageCompute = R"(#version 460 core
#extension GL_ARB_bindless_texture : require
layout(local_size_x = 2, local_size_y = 2) in;

layout(std430, binding = 45) readonly buffer OloResourceHeapBlock
{
    uvec2 g_OloResourceHeap[];
};

layout(std140, binding = 56) uniform OloHeapOffsetBlock
{
    uvec4 g_OloHeapOffsets[19];
};

#define OLO_HEAP_OFFSET(texSlot) (g_OloHeapOffsets[(texSlot) >> 2][(texSlot) & 3])
// Mirrors ShaderBindingLayout::HEAP_IMAGE_SLOT_BASE, which MOVES whenever an
// engine texture slot is added (68 since #723's TEX_VOLUMETRIC_SHADOW, 67 since
// #702's TEX_VSM_PHYSICAL). Pinned by
// BindlessShaderPipeline.HeapImageBaseMatchesTheBindingLayout, which reads this
// literal out of this file for exactly that reason.
#define OLO_HEAP_IMAGE_BASE 68u
#define OLO_HEAP_IMAGE_OFFSET(imgUnit) OLO_HEAP_OFFSET(OLO_HEAP_IMAGE_BASE + uint(imgUnit))
#define OLO_HEAP_IMAGE_RW
#define OLO_HEAP_IMAGE(fmt, mem, type, name, imgUnit) \
    layout(fmt) mem type name = layout(fmt) type(g_OloResourceHeap[OLO_HEAP_IMAGE_OFFSET(imgUnit)])

void main()
{
    OLO_HEAP_IMAGE(r32f, OLO_HEAP_IMAGE_RW, image2D, u_Output, 1);
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(u_Output, coord, vec4(float(coord.x) + 10.0 * float(coord.y), 0.0, 0.0, 0.0));
}
)";

        [[nodiscard]] GLuint CompileStage(GLenum stage, const char* source, std::string& outLog)
        {
            const GLuint shader = glCreateShader(stage);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);

            GLint compiled = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_FALSE)
            {
                GLint length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> log(static_cast<sizet>(length > 0 ? length : 1));
                glGetShaderInfoLog(shader, length, nullptr, log.data());
                outLog.assign(log.data());
                glDeleteShader(shader);
                return 0u;
            }
            return shader;
        }

        // Raw glShaderSource, no SPIR-V. That is not a shortcut taken for test
        // convenience — it is the ONLY route bindless GLSL can take on this
        // engine, and saying so here keeps the constraint next to the code that
        // demonstrates it.
        [[nodiscard]] GLuint BuildProgram(const char* fragmentSource, std::string& outLog)
        {
            const GLuint vs = CompileStage(GL_VERTEX_SHADER, kVertexSource, outLog);
            if (vs == 0u)
            {
                return 0u;
            }
            const GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fragmentSource, outLog);
            if (fs == 0u)
            {
                glDeleteShader(vs);
                return 0u;
            }

            const GLuint program = glCreateProgram();
            glAttachShader(program, vs);
            glAttachShader(program, fs);
            glLinkProgram(program);

            GLint linked = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            glDetachShader(program, vs);
            glDetachShader(program, fs);
            glDeleteShader(vs);
            glDeleteShader(fs);

            if (linked == GL_FALSE)
            {
                GLint length = 0;
                glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> log(static_cast<sizet>(length > 0 ? length : 1));
                glGetProgramInfoLog(program, length, nullptr, log.data());
                outLog.assign(log.data());
                glDeleteProgram(program);
                return 0u;
            }
            return program;
        }

        // A solid-colour RGBA8 texture with its storage IMMUTABLE
        // (glTextureStorage2D, not glTexImage2D). Required, not stylistic:
        // ARB_bindless_texture refuses a handle for a texture whose storage can
        // still change, and the failure is a zero handle rather than an error at
        // the call that mattered.
        [[nodiscard]] GLuint MakeSolidTexture(u8 r, u8 g, u8 b, u8 a)
        {
            GLuint texture = 0u;
            glCreateTextures(GL_TEXTURE_2D, 1, &texture);
            glTextureStorage2D(texture, 1, GL_RGBA8, 2, 2);

            const std::array<u8, 16> pixels = { r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a };
            glTextureSubImage2D(texture, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            return texture;
        }

        struct HeapGpuFixture : ::testing::Test
        {
            OpenGLDescriptorHeapBackend Backend;
            GLuint Program = 0u;
            GLuint OffsetsUbo = 0u;
            GLuint Fbo = 0u;
            GLuint ColorTarget = 0u;
            GLuint Vao = 0u;
            GLuint Vbo = 0u;
            std::vector<GLuint> OwnedTextures;
            // Registry entries this fixture minted. The registry is process-wide,
            // so leaving them registered would keep resolving to textures the
            // fixture then deletes — coupling unrelated tests through shared
            // state and re-creating the recycled-name hazard the generation
            // counter exists to prevent.
            std::vector<RHI::ResourceHandle> OwnedHandles;

            static constexpr u32 kWidth = 8u;
            static constexpr u32 kHeight = 4u;

            RHI::IDescriptorHeapBackend* EngineBackend = nullptr;
            RHI::HeapDesc EngineDesc;
            bool EngineHeapWasEnabled = false;

            [[nodiscard]] bool BringUp(bool poison)
            {
                // Remember the engine's own heap so TearDown can hand the process
                // back the one it found rather than a shut-down one.
                EngineBackend = RHI::DescriptorHeap::Get().GetBackend();
                EngineDesc = RHI::DescriptorHeap::Get().GetDesc();
                EngineHeapWasEnabled = RHI::DescriptorHeap::Get().IsEnabled();
                Backend.Initialize(kDescriptorHeapSlots);
                if (!Backend.IsBindlessSupported())
                {
                    return false;
                }

                RHI::HeapDesc desc;
                desc.ResourceSlotCapacity = 64u;
                desc.SamplerSlotCapacity = 8u;
                desc.FrameTransientRingSlots = 16u;
                desc.PoisonOnFree = poison;
                RHI::DescriptorHeap::Get().Initialize(desc, &Backend);
                RHI::DescriptorHeap::Get().SetEnabled(true);

                std::string log;
                Program = BuildProgram(kFragmentSource, log);
                EXPECT_NE(Program, 0u) << "Bindless GLSL failed to build via glShaderSource:\n"
                                       << log;

                glCreateBuffers(1, &OffsetsUbo);
                glNamedBufferStorage(OffsetsUbo, static_cast<GLsizeiptr>(sizeof(u32) * 4), nullptr,
                                     GL_DYNAMIC_STORAGE_BIT);

                glCreateTextures(GL_TEXTURE_2D, 1, &ColorTarget);
                glTextureStorage2D(ColorTarget, 1, GL_RGBA8, kWidth, kHeight);
                glCreateFramebuffers(1, &Fbo);
                glNamedFramebufferTexture(Fbo, GL_COLOR_ATTACHMENT0, ColorTarget, 0);

                // A full-viewport quad as two triangles. The fullscreen helper in
                // RenderPropertyTest binds a texture to unit 0, which is exactly
                // the thing this test must not do.
                static constexpr std::array<f32, 12> kQuad = { -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
                                                               -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f };
                glCreateVertexArrays(1, &Vao);
                glCreateBuffers(1, &Vbo);
                glNamedBufferStorage(Vbo, static_cast<GLsizeiptr>(sizeof(kQuad)), kQuad.data(), 0);
                glVertexArrayVertexBuffer(Vao, 0, Vbo, 0, static_cast<GLsizei>(sizeof(f32) * 2));
                glEnableVertexArrayAttrib(Vao, 0);
                glVertexArrayAttribFormat(Vao, 0, 2, GL_FLOAT, GL_FALSE, 0);
                glVertexArrayAttribBinding(Vao, 0, 0);

                return Program != 0u;
            }

            [[nodiscard]] RHI::ResourceHandle RegisterTexture(GLuint texture)
            {
                const RHI::ResourceHandle handle = RHI::ResourceRegistry::Get().Register(
                    RHI::ResourceKind::Texture, static_cast<u64>(texture), RHI::Backend::OpenGL);
                OwnedHandles.push_back(handle);
                return handle;
            }

            [[nodiscard]] RHI::ViewHandle AddTexture(u8 r, u8 g, u8 b, RHI::HeapSlotLifetime lifetime)
            {
                const GLuint texture = MakeSolidTexture(r, g, b, 255u);
                OwnedTextures.push_back(texture);

                const RHI::ResourceHandle resource = RegisterTexture(texture);

                RHI::SamplerDesc sampler;
                sampler.Source = RHI::SamplerSource::Explicit;
                sampler.MinFilter = RHI::Filter::Nearest;
                sampler.MagFilter = RHI::Filter::Nearest;
                sampler.LinearMipFilter = false;

                return RHI::DescriptorHeap::Get().CreateView(resource, RHI::ViewDesc{}, sampler, lifetime);
            }

            void DrawThroughHeap(u32 leftOffset, u32 rightOffset)
            {
                const std::array<u32, 4> offsets = { leftOffset, rightOffset, 0u, 0u };
                glNamedBufferSubData(OffsetsUbo, 0, static_cast<GLsizeiptr>(sizeof(offsets)), offsets.data());

                // Publishes the table AND binds it. Note there is no texture bind
                // anywhere in this function — that absence is the whole point.
                RHI::DescriptorHeap::Get().Flush();

                glBindFramebuffer(GL_FRAMEBUFFER, Fbo);
                glViewport(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight));
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                glClearColor(1.0f, 0.0f, 1.0f, 1.0f); // magenta: "the draw never happened"
                glClear(GL_COLOR_BUFFER_BIT);

                glUseProgram(Program);
                glBindBufferBase(GL_UNIFORM_BUFFER, 9, OffsetsUbo);
                glBindVertexArray(Vao);
                glDrawArrays(GL_TRIANGLES, 0, 6);

                glBindVertexArray(0);
                glUseProgram(0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glFinish();
            }

            // Draw with the slot-indexed program: no offsets UBO of its own, the
            // shader reads the shared heap-offset table the seam just published.
            void DrawSlotIndexed()
            {
                glBindFramebuffer(GL_FRAMEBUFFER, Fbo);
                glViewport(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight));
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                glClearColor(0.0f, 1.0f, 0.0f, 1.0f); // green: "the draw never happened"
                glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(Program);
                glBindVertexArray(Vao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                glBindVertexArray(0);
                glUseProgram(0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glFinish();
            }

            [[nodiscard]] std::array<u8, 4> SampleAt(u32 x, u32 y)
            {
                std::vector<u8> pixels;
                ReadbackRgba8(ColorTarget, kWidth, kHeight, pixels);

                // A short readback means the capture failed, not that the pixel is
                // black — indexing past the end would turn a broken harness into a
                // confident wrong answer.
                const sizet base = (static_cast<sizet>(y) * kWidth + x) * 4u;
                if (pixels.size() < base + 4u)
                {
                    ADD_FAILURE() << "Readback returned " << pixels.size() << " bytes; expected at least "
                                  << (base + 4u) << ". The framebuffer read failed.";
                    return { 0u, 0u, 0u, 0u };
                }
                return { pixels[base], pixels[base + 1], pixels[base + 2], pixels[base + 3] };
            }

            void TearDown() override
            {
                // Process-wide, so leaving it set would tell an unrelated test's
                // slot-based program that it reads the heap.
                Shader::SetBoundProgramBindless(false);

                // THIS FIXTURE OWNS A PROCESS-WIDE SINGLETON, and leaving it shut
                // down poisons every test that runs after it.
                //
                // A shader's variant is baked at COMPILE time. Any production
                // shader already built as the bindless variant keeps reading the
                // offset table forever; shutting the heap down here just stops
                // the seam writing it, so those shaders sample stale offsets for
                // the rest of the process. That cost 7 suite failures which read
                // exactly like conversion defects — SSAO, Fog, GTAO, EASU and
                // friends, each of which PASSES when run alone.
                //
                // "Passes alone, fails in the suite" is the signature of shared
                // state, and this fixture is the sharer. Re-initialise the heap
                // against this fixture's backend so the global stays live and
                // enabled for whoever runs next; the next fixture's BringUp
                // re-initialises it again anyway.
                RHI::DescriptorHeap::Get().Shutdown();
                Backend.Shutdown();

                // Retire before deleting the GL objects, and before the next test
                // runs: a live entry pointing at a deleted texture is exactly the
                // stale-resolution state the registry exists to make impossible.
                for (const RHI::ResourceHandle handle : OwnedHandles)
                {
                    RHI::ResourceRegistry::Get().Unregister(handle);
                }
                OwnedHandles.clear();

                for (const GLuint texture : OwnedTextures)
                {
                    glDeleteTextures(1, &texture);
                }
                if (Program != 0u)
                {
                    glDeleteProgram(Program);
                }
                if (OffsetsUbo != 0u)
                {
                    glDeleteBuffers(1, &OffsetsUbo);
                }
                if (Vbo != 0u)
                {
                    glDeleteBuffers(1, &Vbo);
                }
                if (Vao != 0u)
                {
                    glDeleteVertexArrays(1, &Vao);
                }
                if (Fbo != 0u)
                {
                    glDeleteFramebuffers(1, &Fbo);
                }
                if (ColorTarget != 0u)
                {
                    glDeleteTextures(1, &ColorTarget);
                }

                // Hand the process back a live heap. Without this the singleton
                // stays disabled and every later bindless-variant shader in the
                // run samples an offset table nobody publishes (see above).
                RestoreProcessHeap();
            }

            // Bring the global heap back up against the engine's own backend, so
            // later tests see the state they would have seen had this fixture
            // never run. Deliberately NOT enabling it when the environment did
            // not ask for bindless — that would change the configuration under
            // tests that expect the slot path.
            void RestoreProcessHeap()
            {
                if (EngineBackend == nullptr)
                {
                    return;
                }
                // The desc the ENGINE was using, not a restatement of the same
                // constants — a restatement drifts silently the day the engine
                // resizes its heap.
                RHI::DescriptorHeap::Get().Initialize(EngineDesc, EngineBackend);
                RHI::DescriptorHeap::Get().SetEnabled(EngineHeapWasEnabled);
            }
        };

#define OLO_ENSURE_BINDLESS_OR_SKIP(fixture, poison)                                            \
    do                                                                                          \
    {                                                                                           \
        OLO_ENSURE_GPU_OR_SKIP();                                                               \
        if (!(fixture).BringUp(poison))                                                         \
        {                                                                                       \
            GTEST_SKIP() << "GL_ARB_bindless_texture unavailable — the slot-based path is the " \
                            "supported configuration on this device, and that is by design.";   \
        }                                                                                       \
    } while (false)
    } // namespace

    // -------------------------------------------------------------------------
    // The headline: two textures reachable through nothing but two integers.
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, TwoTexturesAreSampledThroughOffsetsWithNoTextureBind)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        const RHI::ViewHandle red = AddTexture(255u, 0u, 0u, RHI::HeapSlotLifetime::Persistent);
        const RHI::ViewHandle blue = AddTexture(0u, 0u, 255u, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(red.IsValid());
        ASSERT_TRUE(blue.IsValid());

        const RHI::HeapOffset redOffset = RHI::OffsetOf(red);
        const RHI::HeapOffset blueOffset = RHI::OffsetOf(blue);
        ASSERT_TRUE(redOffset.IsValid());
        ASSERT_TRUE(blueOffset.IsValid());
        ASSERT_NE(redOffset.Value, blueOffset.Value);

        DrawThroughHeap(redOffset.Value, blueOffset.Value);

        const auto left = SampleAt(1u, 2u);
        const auto right = SampleAt(6u, 2u);

        EXPECT_EQ(left[0], 255u) << "Left half must sample the RED texture through its heap offset.";
        EXPECT_EQ(left[2], 0u);
        EXPECT_EQ(right[2], 255u) << "Right half must sample the BLUE texture through its heap offset.";
        EXPECT_EQ(right[0], 0u);
    }

    // Swapping the two offsets must swap the two halves — nothing else. This is
    // what makes the test above evidence rather than coincidence: a shader that
    // ignored the offsets entirely would pass it and fail this.
    TEST_F(HeapGpuFixture, SwappingTheOffsetsSwapsThePixels)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        const RHI::ViewHandle red = AddTexture(255u, 0u, 0u, RHI::HeapSlotLifetime::Persistent);
        const RHI::ViewHandle blue = AddTexture(0u, 0u, 255u, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(red.IsValid());
        ASSERT_TRUE(blue.IsValid());

        DrawThroughHeap(RHI::OffsetOf(blue).Value, RHI::OffsetOf(red).Value);

        EXPECT_EQ(SampleAt(1u, 2u)[2], 255u) << "Left half must now be BLUE.";
        EXPECT_EQ(SampleAt(6u, 2u)[0], 255u) << "Right half must now be RED.";
    }

    // -------------------------------------------------------------------------
    // Poison, on the GPU. The CPU test proves the mirror is overwritten; this
    // proves the overwrite actually reaches the shader — and that a null handle
    // samples as zero rather than trapping, which is the property that makes
    // poison a usable instrument instead of a crash.
    //
    // The black comes from a REAL RESIDENT 1x1 BLACK TEXTURE
    // (`IDescriptorHeapBackend::NullDescriptor`), not from handle 0. Sampling an
    // invalid or non-resident bindless handle is UNDEFINED BEHAVIOUR, so this
    // assertion against a zero handle would have been asserting on driver luck —
    // a deterministic instrument resting on undefined behaviour is not an
    // instrument.
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, PoisonedSlotSamplesBlackRatherThanThePreviousTenant)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ true);

        const RHI::ViewHandle red = AddTexture(255u, 0u, 0u, RHI::HeapSlotLifetime::Persistent);
        const RHI::ViewHandle blue = AddTexture(0u, 0u, 255u, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(red.IsValid());
        ASSERT_TRUE(blue.IsValid());

        const u32 redSlot = RHI::OffsetOf(red).Value;
        const u32 blueSlot = RHI::OffsetOf(blue).Value;

        // Establish that the slot really did hold red, so the assertion after
        // the free is about poison and not about a slot that was never written.
        DrawThroughHeap(redSlot, blueSlot);
        ASSERT_EQ(SampleAt(1u, 2u)[0], 255u);

        RHI::DescriptorHeap::Get().DestroyView(red);

        // The offset is now stale. A pass holding it is the use-after-free this
        // instrument exists to catch — without poison it would sample red's
        // texels for as long as the slot stayed unreused, which is exactly how
        // LIFO reuse hides this class of bug in steady state.
        EXPECT_FALSE(RHI::OffsetOf(red).IsValid()) << "The CPU-side guard must fire first and cheaply.";

        DrawThroughHeap(redSlot, blueSlot);
        const auto left = SampleAt(1u, 2u);
        EXPECT_EQ(left[0], 0u) << "A freed slot must render deterministically black, not the previous tenant.";
        EXPECT_EQ(left[1], 0u);
        EXPECT_EQ(left[2], 0u);
        EXPECT_EQ(SampleAt(6u, 2u)[2], 255u) << "The untouched slot must be unaffected.";
    }

    // -------------------------------------------------------------------------
    // The transient ring, on the GPU. Two logical resources on one physical
    // object, two offsets, one frame — ADR 0011 §1.2's aliasing model.
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, TransientViewsOfOneObjectRenderIdenticallyThroughTwoOffsets)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        const GLuint texture = MakeSolidTexture(0u, 255u, 0u, 255u);
        OwnedTextures.push_back(texture);
        const RHI::ResourceHandle physical = RegisterTexture(texture);

        RHI::SamplerDesc sampler;
        sampler.Source = RHI::SamplerSource::Explicit;
        sampler.MinFilter = RHI::Filter::Nearest;
        sampler.MagFilter = RHI::Filter::Nearest;
        sampler.LinearMipFilter = false;

        auto& heap = RHI::DescriptorHeap::Get();
        const RHI::ViewHandle versionA =
            heap.CreateView(physical, RHI::ViewDesc{}, sampler, RHI::HeapSlotLifetime::FrameTransient);
        const RHI::ViewHandle versionB =
            heap.CreateView(physical, RHI::ViewDesc{}, sampler, RHI::HeapSlotLifetime::FrameTransient);
        ASSERT_TRUE(versionA.IsValid());
        ASSERT_TRUE(versionB.IsValid());
        ASSERT_NE(RHI::OffsetOf(versionA).Value, RHI::OffsetOf(versionB).Value);

        DrawThroughHeap(RHI::OffsetOf(versionA).Value, RHI::OffsetOf(versionB).Value);

        // Two offsets, one object, same pixels. Under persistent slots this
        // would have required rewriting one offset mid-frame; under the ring it
        // is simply two entries, and the alias is VISIBLE in the heap.
        EXPECT_EQ(SampleAt(1u, 2u)[1], 255u);
        EXPECT_EQ(SampleAt(6u, 2u)[1], 255u);

        heap.ResetFrameTransients();
        EXPECT_FALSE(RHI::OffsetOf(versionA).IsValid()) << "Transient offsets must not survive the frame boundary.";
        EXPECT_FALSE(RHI::OffsetOf(versionB).IsValid());
    }

    // -------------------------------------------------------------------------
    // Residency bookkeeping. Not cosmetic: making an already-resident handle
    // resident again is an INVALID_OPERATION, and a leaked residency keeps its
    // texture permanently immutable.
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, ResidencyIsRefcountedAndFullyReleasedOnShutdown)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        ASSERT_TRUE(AddTexture(255u, 0u, 0u, RHI::HeapSlotLifetime::Persistent).IsValid());
        ASSERT_TRUE(AddTexture(0u, 255u, 0u, RHI::HeapSlotLifetime::Persistent).IsValid());
        EXPECT_EQ(Backend.GetStats().ResidentHandles, 2u);

        // No GL error may have been raised getting here. A double-residency
        // transition is the specific mistake this bookkeeping prevents, and it
        // reports as INVALID_OPERATION rather than as a wrong pixel.
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

        RHI::DescriptorHeap::Get().Shutdown();
        EXPECT_EQ(Backend.GetStats().ResidentHandles, 0u)
            << "A leaked residency keeps its texture immutable for the rest of the process.";
    }

    // -------------------------------------------------------------------------
    // THE OFFSET TABLE, through the real pass-side seam.
    //
    // This is the test that earns its keep, because it covers the one thing in
    // the conversion that is both easy to get wrong and impossible to see:
    // std140 pads a `uint` array to a 16-BYTE STRIDE. A shader that declared
    // `uint g_Offsets[64]` instead of `uvec4 g_Offsets[16]` would read every
    // fourth entry, so three textures out of four would sample whatever offset
    // happened to sit at 4*i — a wrong REAL texture, not black, in a frame that
    // still looks plausible.
    //
    // The two slots are SSAO's real ones (19 and 22) precisely because they land
    // in different uvec4 groups AND different components: 19 -> [4][3],
    // 22 -> [5][2]. A layout bug cannot satisfy both.
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, OffsetsReachTheShaderThroughTheRealSeamAtTheirSlotIndices)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        // Rebuilt with the slot-indexed program rather than the direct-offset
        // one the other tests use — same heap, one more layer of indirection,
        // and that layer is what a converted pass actually relies on.
        std::string log;
        glDeleteProgram(Program);
        Program = BuildProgram(kSlotIndexedFragment, log);
        ASSERT_NE(Program, 0u) << "Slot-indexed bindless GLSL failed to build:\n"
                               << log;

        const GLuint redTex = MakeSolidTexture(255u, 0u, 0u, 255u);
        const GLuint blueTex = MakeSolidTexture(0u, 0u, 255u, 255u);
        OwnedTextures.push_back(redTex);
        OwnedTextures.push_back(blueTex);

        const RHI::ResourceHandle red = RegisterTexture(redTex);
        const RHI::ResourceHandle blue = RegisterTexture(blueTex);

        RHI::SamplerDesc sampler;
        sampler.Source = RHI::SamplerSource::Explicit;
        sampler.MinFilter = RHI::Filter::Nearest;
        sampler.MagFilter = RHI::Filter::Nearest;
        sampler.LinearMipFilter = false;

        // The REAL seam a converted pass calls — not a reimplementation of it.
        // These fixtures build their program with raw glShaderSource +
        // glUseProgram, so nothing ever runs OpenGLShader::Bind() — which is what
        // normally records whether the program in flight reads the heap. Declare
        // it here, because the seam now (correctly) refuses the heap path for a
        // program it believes is slot-based.
        Shader::SetBoundProgramBindless(true);

        const RGCommandContext context;
        const RHI::HeapOffset depthSlotOffset = context.BindTextureOrHeapOffset(
            ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, red, RHI::HeapSlotLifetime::Persistent, sampler);
        const RHI::HeapOffset normalSlotOffset = context.BindTextureOrHeapOffset(
            ShaderBindingLayout::TEX_SCENE_NORMALS, blue, RHI::HeapSlotLifetime::Persistent, sampler);

        ASSERT_TRUE(depthSlotOffset.IsValid()) << "The seam must have taken the heap path, not the fallback bind.";
        ASSERT_TRUE(normalSlotOffset.IsValid());
        ASSERT_NE(depthSlotOffset.Value, normalSlotOffset.Value);

        // ONLY the pass-side call, deliberately. An earlier version also called
        // `RHI::DescriptorHeap::Get().Flush()` here — and that hid a real engine
        // bug for a whole batch of conversions, because it supplied by hand the
        // publish the engine was failing to do (the frame-level flush runs before
        // any pass mints its views). A test that sequences a mechanism for itself
        // cannot detect that the engine fails to sequence it, so this must stay a
        // single call: exactly what a converted pass writes.
        context.FlushHeapOffsets();

        DrawSlotIndexed();

        // Left half reads slot 19, right half reads slot 22 — the same constants
        // the bindful branch would have written in `layout(binding = N)`.
        const auto left = SampleAt(1u, 2u);
        const auto right = SampleAt(6u, 2u);
        EXPECT_EQ(left[0], 255u) << "Slot 19 must resolve to the RED texture through the offset table.";
        EXPECT_EQ(left[2], 0u);
        EXPECT_EQ(right[2], 255u) << "Slot 22 must resolve to the BLUE texture through the offset table.";
        EXPECT_EQ(right[0], 0u);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }

    // -------------------------------------------------------------------------
    // UNBIND DOES NOT SURVIVE THE TRANSLATION TO A HEAP.
    //
    // A slot-based pass clears an input by binding a null texture — ToneMap does
    // exactly this with `RHI::NullResource`. Under the heap there is no bind to
    // clear: the shader reads an OFFSET, so leaving the previous one in the table
    // means it goes on sampling last frame's texture through a perfectly valid
    // index. That is the worst shape of bug this model can produce — a real,
    // plausible, wrong image — so the seam must point the slot at the reserved
    // null descriptor instead.
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, BindingANullResourceClearsTheOffsetInsteadOfLeavingItStale)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        std::string log;
        glDeleteProgram(Program);
        Program = BuildProgram(kSlotIndexedFragment, log);
        ASSERT_NE(Program, 0u) << log;

        const GLuint redTex = MakeSolidTexture(255u, 0u, 0u, 255u);
        const GLuint blueTex = MakeSolidTexture(0u, 0u, 255u, 255u);
        OwnedTextures.push_back(redTex);
        OwnedTextures.push_back(blueTex);
        const RHI::ResourceHandle red = RegisterTexture(redTex);
        const RHI::ResourceHandle blue = RegisterTexture(blueTex);

        RHI::SamplerDesc sampler;
        sampler.Source = RHI::SamplerSource::Explicit;
        sampler.MinFilter = RHI::Filter::Nearest;
        sampler.MagFilter = RHI::Filter::Nearest;
        sampler.LinearMipFilter = false;

        Shader::SetBoundProgramBindless(true);
        const RGCommandContext context;

        // Frame 1: both slots carry a real texture.
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, red,
                                        RHI::HeapSlotLifetime::Persistent, sampler);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_SCENE_NORMALS, blue,
                                        RHI::HeapSlotLifetime::Persistent, sampler);
        context.FlushHeapOffsets();
        DrawSlotIndexed();
        ASSERT_EQ(SampleAt(1u, 2u)[0], 255u) << "Left half must start out RED.";

        // Frame 2: the pass declares it is no longer using slot 19, the way every
        // slot-based pass does.
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, RHI::NullResource,
                                        RHI::HeapSlotLifetime::Persistent, sampler);
        context.FlushHeapOffsets();
        DrawSlotIndexed();

        const auto left = SampleAt(1u, 2u);
        EXPECT_EQ(left[0], 0u) << "A cleared input must sample the null descriptor, NOT the previous texture.";
        EXPECT_EQ(left[1], 0u);
        EXPECT_EQ(left[2], 0u);
        EXPECT_EQ(SampleAt(6u, 2u)[2], 255u) << "The untouched slot must be unaffected.";
    }

    // -------------------------------------------------------------------------
    // A DEFAULT SamplerDesc INHERITS THE TEXTURE OBJECT'S STATE, which is what
    // makes a converted shader sample identically to an unconverted one.
    //
    // WHY THIS IS ASSERTED ON THE MECHANISM RATHER THAN ON PIXELS. The
    // consequences of getting it wrong are all real but none of them is a
    // portable pixel assertion: an integer texture with a LINEAR filter is
    // INCOMPLETE and samples as zero on Mesa while NVIDIA quietly tolerates it
    // (Texture.h::IsIntegerFormat), and the wrap difference only shows outside
    // [0, 1], which this fixture's quad never reaches. A test that renders green
    // on the machine that runs it and black on the machine that ships is worse
    // than one that checks the contract. The pixel evidence for this lives in the
    // suite's fixed-camera visual-evidence PNGs, whose same-config noise floor is
    // 0.000 — see ADR 0011 amendment (38).
    //
    // WHAT WENT WRONG WITHOUT IT: every call site that passes `{}` — which is
    // most of them — got a sampler object built from struct defaults instead of
    // the texture's own state. There is no set of defaults that is right for all
    // four targets (2D is REPEAT, colour arrays and cubemaps are CLAMP_TO_EDGE,
    // depth arrays are CLAMP_TO_BORDER), which is why a per-target table was
    // whack-a-mole: fixing 2D broke the terrain arrays (issue #691 Phase 3).
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, ADefaultSamplerDescInheritsTheTextureObjectRatherThanMintingOne)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        const GLuint texture = MakeSolidTexture(32u, 64u, 96u, 255u);
        OwnedTextures.push_back(texture);
        const RHI::ResourceHandle resource = RegisterTexture(texture);

        const u32 samplerObjectsBefore = Backend.GetStats().SamplerObjects;
        const u64 inheritsBefore = Backend.GetStats().DefaultSamplerInherits;

        auto& heap = RHI::DescriptorHeap::Get();
        const RHI::ViewHandle inherited =
            heap.CreateView(resource, RHI::ViewDesc{}, RHI::SamplerDesc{}, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(inherited.IsValid());

        EXPECT_EQ(Backend.GetStats().DefaultSamplerInherits, inheritsBefore + 1u)
            << "A default-constructed SamplerDesc must take the glGetTextureHandleARB path.";
        EXPECT_EQ(Backend.GetStats().SamplerObjects, samplerObjectsBefore)
            << "Inheriting must mint NO sampler object — one built from struct defaults is exactly "
               "the state that diverges from the slot path.";

        // The other half, so this cannot pass by the backend simply never
        // building sampler objects any more.
        RHI::SamplerDesc stated;
        stated.Source = RHI::SamplerSource::Explicit;
        stated.MinFilter = RHI::Filter::Nearest;
        stated.MagFilter = RHI::Filter::Nearest;
        stated.LinearMipFilter = false;
        const RHI::ViewHandle explicitView =
            heap.CreateView(resource, RHI::ViewDesc{}, stated, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(explicitView.IsValid());

        EXPECT_EQ(Backend.GetStats().DefaultSamplerInherits, inheritsBefore + 1u)
            << "A STATED sampler must not be treated as an inherit.";
        EXPECT_GT(Backend.GetStats().SamplerObjects, samplerObjectsBefore)
            << "A stated sampler must still mint a sampler object — that is what models a split heap.";
        EXPECT_NE(heap.SamplerOffsetOf(inherited).Value, heap.SamplerOffsetOf(explicitView).Value)
            << "Inherited and stated are two different descriptors of one texture.";
    }

    // -------------------------------------------------------------------------
    // THE TWO HALVES OF SamplerDesc::Source, and the second one is a backstop
    // against a trap the FIRST one creates.
    //
    // Half one: `Source = Explicit` with otherwise-default fields must mint a
    // sampler object rather than inherit. That is the whole reason the
    // discriminator exists — before it, "inherit" was inferred from equality with
    // the defaults, so a caller wanting Linear+Repeat EXPLICITLY on a colour
    // Texture2DArray (whose object is ClampToEdge) silently got the object.
    //
    // Half two: a desc that sets fields but forgets `Source` must still have its
    // fields honoured. Making Source the sole test would mean forgetting one line
    // silently replaces a caller's whole sampler with the texture's state —
    // verified reachable by deleting it from HeapBinding::ShadowDepthSampler, at
    // which point 136 tests passed with the shadow comparison sampler inheriting.
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, SamplerSourceDistinguishesInheritFromExplicitAndSurvivesAForgottenDiscriminator)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        const GLuint texture = MakeSolidTexture(10u, 20u, 30u, 255u);
        OwnedTextures.push_back(texture);
        const RHI::ResourceHandle resource = RegisterTexture(texture);

        auto& heap = RHI::DescriptorHeap::Get();
        const u64 inheritsBefore = Backend.GetStats().DefaultSamplerInherits;
        const u32 objectsBefore = Backend.GetStats().SamplerObjects;

        // Half one: explicit, every field left at its default.
        RHI::SamplerDesc explicitDefaults;
        explicitDefaults.Source = RHI::SamplerSource::Explicit;
        const RHI::ViewHandle stated =
            heap.CreateView(resource, RHI::ViewDesc{}, explicitDefaults, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(stated.IsValid());

        EXPECT_EQ(Backend.GetStats().DefaultSamplerInherits, inheritsBefore)
            << "Source = Explicit must NOT inherit, even when every other field matches the defaults.";
        EXPECT_GT(Backend.GetStats().SamplerObjects, objectsBefore)
            << "Explicit-with-default-values is exactly the case the discriminator was added to express.";

        // Half two: fields stated, discriminator forgotten.
        RHI::SamplerDesc forgotten;
        forgotten.MinFilter = RHI::Filter::Nearest;
        forgotten.MagFilter = RHI::Filter::Nearest;
        forgotten.LinearMipFilter = false;
        const u64 inheritsBeforeForgotten = Backend.GetStats().DefaultSamplerInherits;
        const RHI::ViewHandle honoured =
            heap.CreateView(resource, RHI::ViewDesc{}, forgotten, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(honoured.IsValid());

        EXPECT_EQ(Backend.GetStats().DefaultSamplerInherits, inheritsBeforeForgotten)
            << "A desc that STATES fields must be honoured whatever its Source says — otherwise forgetting "
               "one line silently swaps the caller's sampler for the texture object's state.";

        // And a genuinely unstated desc still inherits, so neither branch above
        // passes by the backend having stopped inheriting altogether.
        const u64 beforeInherit = Backend.GetStats().DefaultSamplerInherits;
        const RHI::ViewHandle inherited =
            heap.CreateView(resource, RHI::ViewDesc{}, RHI::SamplerDesc{}, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(inherited.IsValid());
        EXPECT_EQ(Backend.GetStats().DefaultSamplerInherits, beforeInherit + 1u)
            << "A default-constructed desc must still take the inherit path.";
    }

    // -------------------------------------------------------------------------
    // Sampler dedup, checked against the driver rather than against our own
    // bookkeeping: one texture, two sampler configurations, two distinct
    // handles. This is the mechanism that makes the second GL texture object
    // CreateDepthArrayCompareOffViewHandle allocates unnecessary under the heap.
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, OneTextureWithTwoSamplerConfigurationsYieldsTwoDistinctHandles)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        const GLuint texture = MakeSolidTexture(128u, 128u, 128u, 255u);
        OwnedTextures.push_back(texture);
        const RHI::ResourceHandle resource = RegisterTexture(texture);

        RHI::SamplerDesc nearestClamp;
        nearestClamp.Source = RHI::SamplerSource::Explicit;
        nearestClamp.MinFilter = RHI::Filter::Nearest;
        nearestClamp.MagFilter = RHI::Filter::Nearest;
        nearestClamp.LinearMipFilter = false;

        // DELIBERATELY NOT A DEFAULT-CONSTRUCTED DESC, and this is the whole
        // reason the test needed revisiting: `SamplerDesc{}` now means "inherit
        // the texture object's state" and mints its view with
        // glGetTextureHandleARB, so it creates NO sampler object at all
        // (issue #691 Phase 3, AcquireSampledDescriptor). An earlier version of
        // this test built `linearRepeat` out of what happen to be the default
        // values and would have silently started measuring one object instead of
        // two. ClampToEdge is what makes it an intent rather than a shrug.
        RHI::SamplerDesc linearClamp;
        linearClamp.Source = RHI::SamplerSource::Explicit;
        linearClamp.AddressU = RHI::AddressMode::ClampToEdge;
        linearClamp.AddressV = RHI::AddressMode::ClampToEdge;

        auto& heap = RHI::DescriptorHeap::Get();
        const RHI::ViewHandle a = heap.CreateView(resource, RHI::ViewDesc{}, nearestClamp,
                                                  RHI::HeapSlotLifetime::Persistent);
        const RHI::ViewHandle b = heap.CreateView(resource, RHI::ViewDesc{}, linearClamp,
                                                  RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(a.IsValid());
        ASSERT_TRUE(b.IsValid());

        EXPECT_NE(heap.SamplerOffsetOf(a).Value, heap.SamplerOffsetOf(b).Value);
        EXPECT_EQ(Backend.GetStats().SamplerObjects, 2u)
            << "Two distinct SamplerDesc values must produce two GL sampler objects — and only two.";
        EXPECT_EQ(Backend.GetStats().ResidentHandles, 2u)
            << "One texture, two sampler configurations, two DISTINCT bindless handles.";
    }

    // =========================================================================
    // STORAGE IMAGES, END TO END (ADR 0011 amendment (26)).
    //
    // The sampler tests above prove a texture can be READ through an offset.
    // These prove a texture can be WRITTEN through one — a different descriptor
    // (`glGetImageHandleARB`), a different residency namespace, a different
    // shader-side type, and a different reserved null. Nothing about the sampler
    // path generalises to it for free, which is the whole point of amendment (26).
    //
    // They read the written texels back exactly, so a wrong offset, a wrong mip,
    // a wrong format and an unbound heap each fail differently and specifically.
    // =========================================================================
    namespace
    {
        [[nodiscard]] GLuint BuildComputeProgram(const char* source, std::string& outLog)
        {
            const GLuint cs = CompileStage(GL_COMPUTE_SHADER, source, outLog);
            if (cs == 0u)
            {
                return 0u;
            }

            const GLuint program = glCreateProgram();
            glAttachShader(program, cs);
            glLinkProgram(program);

            GLint linked = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            glDetachShader(program, cs);
            glDeleteShader(cs);

            if (linked == GL_FALSE)
            {
                GLint length = 0;
                glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> log(static_cast<sizet>(length > 0 ? length : 1));
                glGetProgramInfoLog(program, length, nullptr, log.data());
                outLog.assign(log.data());
                glDeleteProgram(program);
                return 0u;
            }
            return program;
        }

        // A 2x2 R32F texture prefilled with a value no correct dispatch produces,
        // so "the shader never ran" and "the shader wrote the right thing" cannot
        // be confused.
        [[nodiscard]] GLuint MakeStorageTexture()
        {
            GLuint texture = 0u;
            glCreateTextures(GL_TEXTURE_2D, 1, &texture);
            glTextureStorage2D(texture, 1, GL_R32F, 2, 2);
            const std::array<f32, 4> sentinel = { -1.0f, -1.0f, -1.0f, -1.0f };
            glTextureSubImage2D(texture, 0, 0, 0, 2, 2, GL_RED, GL_FLOAT, sentinel.data());
            return texture;
        }

        [[nodiscard]] std::array<f32, 4> ReadStorageTexture(GLuint texture)
        {
            std::array<f32, 4> pixels = { 0.0f, 0.0f, 0.0f, 0.0f };
            glGetTextureImage(texture, 0, GL_RED, GL_FLOAT, static_cast<GLsizei>(sizeof(pixels)), pixels.data());
            return pixels;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // THE SPELLING PROBE.
    //
    // `ARB_bindless_texture` says an image can be constructed from a handle and
    // that image types carry a format layout qualifier, but it does not settle in
    // one place WHERE the qualifier goes for a constructor, nor whether a memory
    // qualifier (`writeonly` / `readonly`) may ride along on a local. Those are
    // exactly the details `include/BindlessHeap.glsl`'s macros have to commit to,
    // and getting one wrong makes every converted compute shader fail to build.
    //
    // So this asks the driver rather than the spec, and reports EVERY answer
    // instead of stopping at the first. It fails only if the spelling the header
    // actually uses is rejected — the others are recorded so the next person
    // widening the macro set does not have to re-derive them.
    //
    // Generalisable: when a mechanism's syntax is decided by an extension rather
    // than by the core language, pin it with a probe that names the alternatives.
    // A test that only checks the chosen spelling tells you it broke, not what to
    // replace it with.
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, TheImageConstructorSpellingTheHeaderUsesIsAcceptedByTheDriver)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        struct Candidate
        {
            const char* Name;
            const char* Declaration;
        };

        // Every spelling is the same program apart from the one line under test.
        static constexpr std::array kCandidates = {
            Candidate{ "qualifier-on-both (what BindlessHeap.glsl uses)",
                       "layout(r32f) image2D img = layout(r32f) image2D(g_OloResourceHeap[u_ImageOffset]);" },
            Candidate{ "qualifier-on-declaration-only",
                       "layout(r32f) image2D img = image2D(g_OloResourceHeap[u_ImageOffset]);" },
            Candidate{ "qualifier-on-constructor-only",
                       "image2D img = layout(r32f) image2D(g_OloResourceHeap[u_ImageOffset]);" },
            Candidate{ "writeonly on both",
                       "layout(r32f) writeonly image2D img = layout(r32f) writeonly "
                       "image2D(g_OloResourceHeap[u_ImageOffset]);" },
            Candidate{ "writeonly on declaration only",
                       "layout(r32f) writeonly image2D img = layout(r32f) "
                       "image2D(g_OloResourceHeap[u_ImageOffset]);" },
            // `coherent` — Terrain_Erosion and Precipitation_Feed need it for
            // cross-invocation visibility, so dropping it would be a silent
            // correctness change rather than a cosmetic one.
            Candidate{ "coherent on declaration",
                       "layout(r32f) coherent image2D img = layout(r32f) "
                       "image2D(g_OloResourceHeap[u_ImageOffset]);" },
            // `readonly` MUST FAIL, and this entry is the one that earns the
            // probe's keep. Initialising a readonly variable is a write, so the
            // driver rejects the declaration outright — but nothing in the test
            // suite compiles a bindless branch (the suite runs with the heap off,
            // so only the default variant is built), and it took a live editor run
            // to surface it across four shaders at once.
            //
            // The original probe enumerated where the qualifier GOES and not which
            // qualifiers EXIST, which is exactly the hole this closes.
            Candidate{ "readonly on declaration (EXPECTED TO FAIL)",
                       "layout(r32f) readonly image2D img = layout(r32f) "
                       "image2D(g_OloResourceHeap[u_ImageOffset]);" },
        };
        // Index of the readonly candidate above — it is the one spelling the
        // header must NOT use, so the assertion below is inverted for it.
        constexpr sizet kReadonlyCandidate = 6;

        static constexpr const char* kPrefix = R"(#version 460 core
#extension GL_ARB_bindless_texture : require
layout(local_size_x = 1) in;
layout(std430, binding = 45) readonly buffer OloResourceHeapBlock { uvec2 g_OloResourceHeap[]; };
layout(std140, binding = 9) uniform HeapOffsets { uint u_ImageOffset; };
void main()
{
    )";
        static constexpr const char* kSuffix = R"(
    imageStore(img, ivec2(gl_GlobalInvocationID.xy), vec4(1.0, 0.0, 0.0, 0.0));
}
)";

        bool headerSpellingCompiles = false;
        for (sizet i = 0; i < kCandidates.size(); ++i)
        {
            const std::string source = std::string(kPrefix) + kCandidates[i].Declaration + kSuffix;
            std::string log;
            const GLuint shader = CompileStage(GL_COMPUTE_SHADER, source.c_str(), log);
            const bool ok = shader != 0u;
            if (ok)
            {
                glDeleteShader(shader);
            }

            std::cout << "[bindless image spelling] " << (ok ? "OK      " : "REJECTED") << "  "
                      << kCandidates[i].Name << (ok ? "" : "\n    driver said: ") << (ok ? "" : log)
                      << std::endl;

            if (i == 0)
            {
                headerSpellingCompiles = ok;
            }
            if (i == kReadonlyCandidate)
            {
                EXPECT_FALSE(ok)
                    << "`readonly` on a bindless image local COMPILED, which contradicts the rule "
                       "include/BindlessHeap.glsl documents (initialising a readonly variable is a write). "
                       "If a driver starts accepting it, the header's advice to pass OLO_HEAP_IMAGE_RW in "
                       "place of `readonly` is over-cautious and can be revisited — but do not simply "
                       "delete this expectation, because the shaders that hit it fall back SILENTLY.";
            }
        }

        EXPECT_TRUE(headerSpellingCompiles)
            << "include/BindlessHeap.glsl's OLO_HEAP_IMAGE_* macros use a spelling this driver rejects. "
               "The log above lists which alternatives it accepts — change the header to one of them.";
    }

    // The headline: a compute shader writes real texels through nothing but an
    // integer, and the texels come back exactly right. `imageStore(coord) =
    // x + 10*y` makes every texel distinguishable, so a transposed or collapsed
    // write is a different failure from a missing one.
    TEST_F(HeapGpuFixture, AComputeShaderWritesThroughAStorageImageDescriptor)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        std::string log;
        const GLuint compute = BuildComputeProgram(kImageComputeSource, log);
        ASSERT_NE(compute, 0u) << "Bindless storage-image GLSL failed to build via glShaderSource:\n"
                               << log;

        const GLuint texture = MakeStorageTexture();
        OwnedTextures.push_back(texture);
        const RHI::ResourceHandle resource = RegisterTexture(texture);

        auto& heap = RHI::DescriptorHeap::Get();
        const RHI::ViewHandle view = heap.CreateStorageView(
            resource,
            RHI::MakeStorageViewDesc(resource, 0u, false, 0u, RHI::Access::StorageWrite, RHI::Format::R32Float),
            RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(view.IsValid()) << "The backend must be able to realise a whole-level storage view.";

        const RHI::HeapOffset offset = RHI::OffsetOf(view);
        ASSERT_TRUE(offset.IsValid());

        const std::array<u32, 4> offsets = { offset.Value, 0u, 0u, 0u };
        glNamedBufferSubData(OffsetsUbo, 0, static_cast<GLsizeiptr>(sizeof(offsets)), offsets.data());

        heap.Flush();
        glUseProgram(compute);
        glBindBufferBase(GL_UNIFORM_BUFFER, 9, OffsetsUbo);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
        glFinish();
        glUseProgram(0);

        const auto pixels = ReadStorageTexture(texture);
        EXPECT_FLOAT_EQ(pixels[0], 0.0f);  // (0,0)
        EXPECT_FLOAT_EQ(pixels[1], 1.0f);  // (1,0)
        EXPECT_FLOAT_EQ(pixels[2], 10.0f); // (0,1)
        EXPECT_FLOAT_EQ(pixels[3], 11.0f); // (1,1)
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        EXPECT_EQ(Backend.GetStats().ResidentImageHandles, 1u)
            << "An image handle has its OWN residency namespace; it must not be counted as a sampler handle.";

        glDeleteProgram(compute);
    }

    // THE REBASE, through the real seam. Image units and texture units are
    // separate GL namespaces that both start at 0, so image unit 1 and
    // TEX_SPECULAR would collide in a single-index offset table. This drives
    // `HeapBinding::BindImageOrOffset` — the call a converted compute pass writes
    // — and the shader reads image unit 1 through OLO_HEAP_IMAGE_BASE + 1.
    //
    // A missing or mismatched base does not render black: it reads whatever
    // descriptor the colliding TEXTURE slot published, which is a real resource
    // of the wrong kind.
    TEST_F(HeapGpuFixture, AnImageUnitReachesTheShaderThroughTheRebasedOffsetTable)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        std::string log;
        const GLuint compute = BuildComputeProgram(kSlotIndexedImageCompute, log);
        ASSERT_NE(compute, 0u) << "Slot-indexed bindless storage-image GLSL failed to build:\n"
                               << log;

        const GLuint texture = MakeStorageTexture();
        OwnedTextures.push_back(texture);
        const RHI::ResourceHandle resource = RegisterTexture(texture);

        // A DIFFERENT texture on the colliding TEXTURE slot, so a missing rebase
        // is caught as "wrote the wrong object" rather than passing by luck on an
        // empty table.
        const GLuint decoy = MakeSolidTexture(255u, 255u, 255u, 255u);
        OwnedTextures.push_back(decoy);
        const RHI::ResourceHandle decoyResource = RegisterTexture(decoy);

        // Nothing runs OpenGLShader::Bind() here (the program is raw
        // glShaderSource + glUseProgram), so declare the program bindless by hand
        // — the seam correctly refuses the heap path for a program it believes is
        // slot-based.
        Shader::SetBoundProgramBindless(true);

        RHI::SamplerDesc sampler;
        sampler.Source = RHI::SamplerSource::Explicit;
        sampler.MinFilter = RHI::Filter::Nearest;
        sampler.MagFilter = RHI::Filter::Nearest;
        sampler.LinearMipFilter = false;
        (void)HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_SPECULAR, decoyResource,
                                               RHI::HeapSlotLifetime::Persistent, sampler);

        const RHI::HeapOffset imageOffset =
            HeapBinding::BindImageOrOffset(/*imageUnit*/ 1u, resource, /*mipLevel*/ 0u, /*layered*/ false,
                                           /*layer*/ 0u, RHI::Access::StorageWrite, RHI::Format::R32Float,
                                           RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(imageOffset.IsValid()) << "The seam must have taken the heap path, not the fallback bind.";

        // The two must land at DIFFERENT table indices even though both name "1".
        EXPECT_EQ(HeapBinding::StagedOffsetAt(ShaderBindingLayout::HEAP_IMAGE_SLOT_BASE + 1u).Value,
                  imageOffset.Value);
        EXPECT_NE(HeapBinding::StagedOffsetAt(ShaderBindingLayout::TEX_SPECULAR).Value, imageOffset.Value)
            << "Image unit 1 must not have overwritten texture slot 1's offset.";

        // ONLY the pass-side call — the same discipline the sampler seam test
        // documents. Calling DescriptorHeap::Flush() by hand here would supply the
        // publish the engine is supposed to perform and hide its absence.
        HeapBinding::FlushOffsets();

        glUseProgram(compute);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
        glFinish();
        glUseProgram(0);

        const auto pixels = ReadStorageTexture(texture);
        EXPECT_FLOAT_EQ(pixels[0], 0.0f);
        EXPECT_FLOAT_EQ(pixels[1], 1.0f);
        EXPECT_FLOAT_EQ(pixels[2], 10.0f);
        EXPECT_FLOAT_EQ(pixels[3], 11.0f);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

        glDeleteProgram(compute);
    }

    // Two mip levels of ONE texture are two descriptors. HZBGenerator binds four
    // in a single dispatch, and under the pre-storage memo key — which carried no
    // mip — the second would have been served the first's view and the whole
    // pyramid would have been written at level 0. That failure renders a
    // plausible frame with a broken occlusion pyramid.
    TEST_F(HeapGpuFixture, TwoMipsOfOneTextureProduceTwoDistinctImageDescriptors)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        GLuint texture = 0u;
        glCreateTextures(GL_TEXTURE_2D, 1, &texture);
        glTextureStorage2D(texture, 2, GL_R32F, 2, 2); // two mips
        OwnedTextures.push_back(texture);
        const RHI::ResourceHandle resource = RegisterTexture(texture);

        auto& heap = RHI::DescriptorHeap::Get();
        const RHI::ViewHandle mip0 = heap.GetOrCreateStorageView(
            resource,
            RHI::MakeStorageViewDesc(resource, 0u, false, 0u, RHI::Access::StorageWrite, RHI::Format::R32Float),
            RHI::HeapSlotLifetime::Persistent);
        const RHI::ViewHandle mip1 = heap.GetOrCreateStorageView(
            resource,
            RHI::MakeStorageViewDesc(resource, 1u, false, 0u, RHI::Access::StorageWrite, RHI::Format::R32Float),
            RHI::HeapSlotLifetime::Persistent);

        ASSERT_TRUE(mip0.IsValid());
        ASSERT_TRUE(mip1.IsValid());
        EXPECT_NE(RHI::OffsetOf(mip0).Value, RHI::OffsetOf(mip1).Value) << "Two heap slots.";
        EXPECT_EQ(Backend.GetStats().ResidentImageHandles, 2u)
            << "…and two DISTINCT driver handles: glGetImageHandleARB keys on the level.";
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    }

    // ACCESS WIDENING. `glGetImageHandleARB` takes no access, so a read-only and
    // a read-write view of the same (texture, level, layer, format) are literally
    // the same driver handle — but reading a WRITE_ONLY-resident handle is
    // undefined. GTAO does exactly this within one frame, so the backend has to
    // notice and re-make the handle resident READ_WRITE.
    //
    // Asserted through glGetError as well as the counter, because the failure
    // mode if the widening were done naively (resident-while-already-resident) is
    // an INVALID_OPERATION rather than a wrong pixel.
    TEST_F(HeapGpuFixture, TwoAccessesOfOneImageWidenResidencyInsteadOfDoubleResidentTransitioning)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        const GLuint texture = MakeStorageTexture();
        OwnedTextures.push_back(texture);
        const RHI::ResourceHandle resource = RegisterTexture(texture);

        auto& heap = RHI::DescriptorHeap::Get();
        const RHI::ViewHandle writeView = heap.GetOrCreateStorageView(
            resource,
            RHI::MakeStorageViewDesc(resource, 0u, false, 0u, RHI::Access::StorageWrite, RHI::Format::R32Float),
            RHI::HeapSlotLifetime::Persistent);
        const RHI::ViewHandle readView = heap.GetOrCreateStorageView(
            resource,
            RHI::MakeStorageViewDesc(resource, 0u, false, 0u, RHI::Access::StorageRead, RHI::Format::R32Float),
            RHI::HeapSlotLifetime::Persistent);

        ASSERT_TRUE(writeView.IsValid());
        ASSERT_TRUE(readView.IsValid());
        EXPECT_NE(RHI::OffsetOf(writeView).Value, RHI::OffsetOf(readView).Value)
            << "Access is part of the neutral view key, so these are two views…";
        EXPECT_EQ(Backend.GetStats().ResidentImageHandles, 1u)
            << "…but GL folds them to ONE handle, which is why residency needs widening rather than a second "
               "transition.";
        EXPECT_EQ(Backend.GetStats().ImageResidencyWidenings, 1u);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
            << "Making an already-resident handle resident again is an INVALID_OPERATION.";
    }

    // A storage view with no format is DECLINED, not guessed. The format is part
    // of the binding contract (it has to match the shader's layout qualifier), so
    // inheriting the resource's would hand the shader a handle it reads through
    // the wrong interpretation — a plausible wrong image, the worst outcome this
    // model can produce.
    TEST_F(HeapGpuFixture, AStorageViewWithoutAFormatIsDeclinedRatherThanGuessed)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        const GLuint texture = MakeStorageTexture();
        OwnedTextures.push_back(texture);
        const RHI::ResourceHandle resource = RegisterTexture(texture);

        const u64 unsupportedBefore = Backend.GetStats().UnsupportedViews;

        RHI::ViewDesc desc = RHI::MakeStorageViewDesc(resource, 0u, false, 0u, RHI::Access::StorageWrite,
                                                      RHI::Format::Unknown);
        const RHI::ViewHandle view =
            RHI::DescriptorHeap::Get().CreateStorageView(resource, desc, RHI::HeapSlotLifetime::Persistent);

        EXPECT_FALSE(view.IsValid()) << "No handle at all — a caller must fall back to the slot-based bind.";
        EXPECT_EQ(Backend.GetStats().UnsupportedViews, unsupportedBefore + 1u)
            << "…and the refusal must be COUNTED, or it is invisible in a frame that merely looks wrong.";
    }

    // -------------------------------------------------------------------------
    // DELETING A TEXTURE THAT STILL HAS A RESIDENT HANDLE IS UNDEFINED, and this
    // is the test that stands in for a person resizing a window.
    //
    // A framebuffer resize deletes its attachments. The heap held persistent
    // views for them, and the destruction path called `InvalidateResource` —
    // which RE-ACQUIRES, so the handle was made resident again on a texture about
    // to be deleted. The editor exited silently on every maximise: no assertion,
    // no GL error queued, no log line, because the fault is inside the driver.
    //
    // Nothing else in the suite could see it. A compile cannot; the full suite
    // cannot (it never enables the heap); a screenshot A/B cannot (the symptom is
    // a teardown fault, and the companion symptom — a flickering viewport — is
    // temporal, which a still frame is blind to by construction).
    //
    // So this asserts the ONE property that makes the delete legal: after
    // retiring, the handle is no longer resident.
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, RetiringAResourceDropsResidencySoTheTextureCanBeDeletedSafely)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        const GLuint texture = MakeSolidTexture(200u, 100u, 50u, 255u);
        const RHI::ResourceHandle resource = RegisterTexture(texture);

        RHI::SamplerDesc sampler;
        sampler.Source = RHI::SamplerSource::Explicit;
        sampler.MinFilter = RHI::Filter::Nearest;
        sampler.MagFilter = RHI::Filter::Nearest;
        sampler.LinearMipFilter = false;

        auto& heap = RHI::DescriptorHeap::Get();
        const RHI::ViewHandle view = heap.CreateView(resource, RHI::ViewDesc{}, sampler,
                                                     RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(view.IsValid());
        ASSERT_EQ(Backend.GetStats().ResidentHandles, 1u) << "Precondition: the handle is resident.";

        heap.RetireResource(resource);

        EXPECT_EQ(Backend.GetStats().ResidentHandles, 0u)
            << "RESIDENCY MUST BE DROPPED BEFORE THE OBJECT DIES. With it still resident, the "
               "glDeleteTextures below is undefined behaviour — which is how the editor was exiting "
               "with no diagnostic at all.";
        EXPECT_FALSE(RHI::OffsetOf(view).IsValid()) << "…and the view must report stale.";

        // The delete the engine performs next. Legal only because of the above.
        glDeleteTextures(1, &texture);
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

        // Retire already unregistered nothing on our behalf, so tidy the registry
        // entry by hand — the fixture would otherwise resolve it to a dead name.
        RHI::ResourceRegistry::Get().Unregister(resource);
        OwnedHandles.erase(std::remove(OwnedHandles.begin(), OwnedHandles.end(), resource),
                           OwnedHandles.end());
    }

    // -------------------------------------------------------------------------
    // THE SAME PROPERTY, BUT DRIVEN THROUGH THE REAL DESTRUCTOR.
    //
    // The test above proves `RetireResource` drops residency. It does NOT prove
    // that the engine's texture types actually CALL it, and for two of them they
    // did not: `~OpenGLTexture2DArray` and `~OpenGLTexture3D` minted an RHI
    // handle in their constructors and retired nothing in their destructors,
    // through two PRs and a full suite run.
    //
    // Both types are bound as STORAGE IMAGE descriptors through the heap in
    // production — Texture3D is the wind field, the froxel fog scatter and
    // integrated volumes, the cloud base/detail noise volumes and the terrain
    // erosion heightmap; Texture2DArray is the ocean FFT ping-pong pair. So each
    // destruction left an `ARB_bindless_texture` image handle resident on a
    // texture the very next statement queued for deletion.
    //
    // WHY IT SURVIVED: the symptom is one `GL_INVALID_OPERATION: Not a valid
    // texture` in the log at process shutdown, under `OLO_RHI_BINDLESS=1` only.
    // It fails nothing, renders nothing wrong, and CI does not run that
    // configuration. These tests exist because "check the log after a full
    // bindless run" is not a thing anything does automatically.
    //
    // Constructed through the public factory on purpose: `Ref<OpenGLTexture3D>`
    // would still test the destructor, but going through `Texture3D::Create`
    // also pins that the factory hands back the type whose destructor was fixed.
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, DestroyingATexture3DRetiresItsStorageDescriptors)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        Texture3DSpecification spec;
        spec.Width = 4u;
        spec.Height = 4u;
        spec.Depth = 4u;
        spec.Format = Texture3DFormat::RGBA16F;

        Ref<Texture3D> volume = Texture3D::Create(spec);
        ASSERT_TRUE(volume) << "Texture3D::Create returned null — no GL context?";
        const RHI::ResourceHandle resource = volume->GetRHIHandle();
        ASSERT_TRUE(resource.IsValid()) << "Precondition: the type mints an RHI identity at all.";

        // Exactly the binding VolumetricFogPass issues for m_ScatterVolume:
        // layered, mip 0, write-only, RGBA16F.
        const RHI::ViewDesc desc = RHI::MakeStorageViewDesc(resource, 0u, /*layered*/ true, 0u,
                                                            RHI::Access::StorageWrite,
                                                            RHI::Format::RGBA16Float);
        const RHI::ViewHandle view = RHI::DescriptorHeap::Get().CreateStorageView(
            resource, desc, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(view.IsValid()) << "Precondition: the heap accepted a storage view for it.";
        // ResidentImageHandles, not ResidentHandles: glGetImageHandleARB has its
        // own residency namespace, so a storage view never moves the sampler
        // counter. Asserting the wrong one reads 0 both before and after the
        // destructor and would pass whether or not the retire happened.
        ASSERT_EQ(Backend.GetStats().ResidentImageHandles, 1u)
            << "Precondition: the image handle is resident.";

        volume = nullptr; // ~OpenGLTexture3D

        EXPECT_EQ(Backend.GetStats().ResidentImageHandles, 0u)
            << "~OpenGLTexture3D must retire its heap descriptors. The glDeleteTextures it queues "
               "next would otherwise destroy an object with a resident bindless image handle — "
               "undefined behaviour if anything samples it, and the shutdown "
               "'GL_INVALID_OPERATION: Not a valid texture' if nothing does.";
        EXPECT_FALSE(RHI::OffsetOf(view).IsValid()) << "…and the view must report stale.";
    }

    TEST_F(HeapGpuFixture, DestroyingATexture2DArrayRetiresItsStorageDescriptors)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        Texture2DArraySpecification spec;
        spec.Width = 4u;
        spec.Height = 4u;
        spec.Layers = 2u;
        spec.Format = Texture2DArrayFormat::RGBA32F;

        Ref<Texture2DArray> pingPong = Texture2DArray::Create(spec);
        ASSERT_TRUE(pingPong) << "Texture2DArray::Create returned null — no GL context?";
        const RHI::ResourceHandle resource = pingPong->GetRHIHandle();
        ASSERT_TRUE(resource.IsValid()) << "Precondition: the type mints an RHI identity at all.";

        // Exactly the binding OceanFFTGpu issues for m_PingPong: layered, mip 0.
        const RHI::ViewDesc desc = RHI::MakeStorageViewDesc(resource, 0u, /*layered*/ true, 0u,
                                                            RHI::Access::StorageReadWrite,
                                                            RHI::Format::RGBA32Float);
        const RHI::ViewHandle view = RHI::DescriptorHeap::Get().CreateStorageView(
            resource, desc, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(view.IsValid()) << "Precondition: the heap accepted a storage view for it.";
        ASSERT_EQ(Backend.GetStats().ResidentImageHandles, 1u)
            << "Precondition: the image handle is resident (the STORAGE namespace — see above).";

        pingPong = nullptr; // ~OpenGLTexture2DArray

        EXPECT_EQ(Backend.GetStats().ResidentImageHandles, 0u)
            << "~OpenGLTexture2DArray must retire its heap descriptors — same reasoning as the "
               "Texture3D case above.";
        EXPECT_FALSE(RHI::OffsetOf(view).IsValid()) << "…and the view must report stale.";
    }

    // The image kind's reserved null slot holds an IMAGE descriptor. Pointing a
    // cleared image binding at slot 0 — which holds a SAMPLER handle — would swap
    // a stale-read bug for an undefined-behaviour one. This checks the seam
    // stages the right one, which is the decision a call site cannot see.
    TEST_F(HeapGpuFixture, AFailedImageBindingPointsAtTheImageNullNotTheSamplerNull)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        Shader::SetBoundProgramBindless(true);

        // NullResource is dead by construction, so the heap declines and the seam
        // takes its fallback path — exactly what a pass saying "I am not using
        // this image this frame" produces.
        const RHI::HeapOffset offset =
            HeapBinding::BindImageOrOffset(/*imageUnit*/ 2u, RHI::NullResource, 0u, false, 0u,
                                           RHI::Access::StorageWrite, RHI::Format::R32Float,
                                           RHI::HeapSlotLifetime::Persistent);

        EXPECT_FALSE(offset.IsValid());
        EXPECT_EQ(HeapBinding::StagedOffsetAt(ShaderBindingLayout::HEAP_IMAGE_SLOT_BASE + 2u).Value,
                  RHI::kNullStorageHeapOffset)
            << "A cleared IMAGE binding must point at the image null (slot 1), not the sampler null (slot 0).";
    }

    // -------------------------------------------------------------------------
    // An offset is an index INTO THE HEAP THAT MINTED IT.
    //
    // The offset table is a process-lifetime static; the heap is re-initialised
    // whenever the device is torn down and rebuilt (and once per fixture that
    // stands up its own). Across that boundary every slot the next pass does not
    // re-stage still holds a number addressing the PREVIOUS heap's descriptor —
    // and the bindless shader samples it without complaining.
    //
    // This is the shape that made a suite-wide failure look like six unrelated
    // ones: the victim is whichever converted pass happens to run next, so the
    // failing set moves when test order moves, and nothing reproduces alone.
    // -------------------------------------------------------------------------
    TEST_F(HeapGpuFixture, OffsetTableIsRebasedWhenTheHeapIsReinitialised)
    {
        OLO_ENSURE_BINDLESS_OR_SKIP(*this, /*poison*/ false);

        Shader::SetBoundProgramBindless(true);

        constexpr u32 kSlot = 3u;
        const GLuint texture = MakeSolidTexture(255u, 0u, 0u, 255u);
        OwnedTextures.push_back(texture);
        const RHI::ResourceHandle resource = RegisterTexture(texture);

        const RHI::HeapOffset live =
            HeapBinding::BindTextureOrOffset(kSlot, resource, RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(live.IsValid());
        ASSERT_NE(live.Value, RHI::kNullHeapOffset)
            << "Precondition: the slot must hold a REAL offset, or the assertion below "
               "would pass without the reset ever happening.";

        // Tear the heap down and stand a fresh one up — the epoch bump every
        // device teardown performs.
        RHI::DescriptorHeap::Get().Shutdown();
        RHI::HeapDesc desc;
        desc.ResourceSlotCapacity = 64u;
        desc.SamplerSlotCapacity = 8u;
        desc.FrameTransientRingSlots = 16u;
        RHI::DescriptorHeap::Get().Initialize(desc, &Backend);
        RHI::DescriptorHeap::Get().SetEnabled(true);

        // A pass that stages nothing still publishes — this is the path that used
        // to return early on `!Dirty` and never notice the heap had changed.
        HeapBinding::FlushOffsets();

        EXPECT_EQ(HeapBinding::StagedOffsetAt(kSlot).Value, RHI::kNullHeapOffset)
            << "A texture slot carrying an offset minted by the previous heap must fall back "
               "to the sampler null, not keep indexing the new heap with a stale number.";
        EXPECT_EQ(HeapBinding::StagedOffsetAt(ShaderBindingLayout::HEAP_IMAGE_SLOT_BASE).Value,
                  RHI::kNullStorageHeapOffset)
            << "The image region must reset to the IMAGE null. Zeroing it would hand image2D a "
               "sampler descriptor — the undefined behaviour the two reserved nulls exist to avoid.";
    }
} // namespace OloEngine::Tests
