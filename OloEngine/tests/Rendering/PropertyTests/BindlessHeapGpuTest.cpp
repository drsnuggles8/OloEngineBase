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

#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "Platform/OpenGL/OpenGLDescriptorHeap.h"

#include <gtest/gtest.h>

#include <glad/gl.h>

#include <array>
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

            [[nodiscard]] bool BringUp(bool poison)
            {
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
        nearestClamp.MinFilter = RHI::Filter::Nearest;
        nearestClamp.MagFilter = RHI::Filter::Nearest;
        nearestClamp.LinearMipFilter = false;

        RHI::SamplerDesc linearRepeat;
        linearRepeat.AddressU = RHI::AddressMode::Repeat;
        linearRepeat.AddressV = RHI::AddressMode::Repeat;

        auto& heap = RHI::DescriptorHeap::Get();
        const RHI::ViewHandle a = heap.CreateView(resource, RHI::ViewDesc{}, nearestClamp,
                                                  RHI::HeapSlotLifetime::Persistent);
        const RHI::ViewHandle b = heap.CreateView(resource, RHI::ViewDesc{}, linearRepeat,
                                                  RHI::HeapSlotLifetime::Persistent);
        ASSERT_TRUE(a.IsValid());
        ASSERT_TRUE(b.IsValid());

        EXPECT_NE(heap.SamplerOffsetOf(a).Value, heap.SamplerOffsetOf(b).Value);
        EXPECT_EQ(Backend.GetStats().SamplerObjects, 2u)
            << "Two distinct SamplerDesc values must produce two GL sampler objects — and only two.";
        EXPECT_EQ(Backend.GetStats().ResidentHandles, 2u)
            << "One texture, two sampler configurations, two DISTINCT bindless handles.";
    }
} // namespace OloEngine::Tests
