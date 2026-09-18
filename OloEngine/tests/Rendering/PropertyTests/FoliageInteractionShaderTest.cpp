// OLO_TEST_LAYER: L2
//
// The GPU-side contract of issue #1238's bend, read back a pixel at a time out
// of the PRODUCTION include. Same harness as FoliageWindShaderTest next to it,
// and for the same reason: a bend that is right on the CPU and wrong in the
// vertex stage looks exactly like a bend that is right.
#include "OloEnginePCH.h"
#include "RenderPropertyTest.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Terrain/Foliage/FoliageInteraction.h"
#include <glad/gl.h>
#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    TEST(FoliageInteractionShader, BendIsRootedBoundedDirectionalAndReprojected)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        GLStateGuard guard("FoliageInteractionShader", GLStateGuard::Policy::Restore);
        FramebufferSpecification spec;
        spec.Width = 9;
        spec.Height = 1;
        spec.Attachments = { FramebufferTextureFormat::RGBA16F };
        auto framebuffer = Framebuffer::Create(spec);
        auto shader = Shader::Create("assets/shaders/tests/ShaderUnit_FoliageInteraction.glsl");
        ASSERT_TRUE(shader);
        ASSERT_TRUE(shader->IsReady());
        framebuffer->Bind();
        glViewport(0, 0, 9, 1);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        shader->Bind();
        FullscreenPass pass;
        pass.Draw(0);
        glFinish();
        framebuffer->Unbind();

        std::vector<f32> pixels;
        ReadbackRgbaFloat(framebuffer->GetColorAttachmentRendererID(0), 9, 1, pixels);
        ASSERT_EQ(pixels.size(), 36u);
        const auto magnitude = [&pixels](sizet column)
        { return glm::length(glm::vec3(pixels[column * 4], pixels[column * 4 + 1], pixels[column * 4 + 2])); };

        // EXACT zero, not "small": these are the columns that promise a scene
        // with no influence source renders as it did before the feature landed.
        EXPECT_FLOAT_EQ(magnitude(0), 0.0f) << "an empty influence set produced a displacement";
        EXPECT_FLOAT_EQ(magnitude(7), 0.0f) << "a zero-response layer bent anyway";
        EXPECT_FLOAT_EQ(magnitude(1), 0.0f) << "the plant's ROOT moved — it is not anchored";
        EXPECT_FLOAT_EQ(magnitude(3), 0.0f) << "an influence reached outside its own radius";
        EXPECT_FLOAT_EQ(magnitude(5), 0.0f) << "an influence reached below its own cylinder";

        EXPECT_GT(magnitude(2), 1e-3f) << "an influence the plant is standing in did nothing";
        EXPECT_GT(magnitude(6), 1e-4f) << "the previous snapshot is not a separate evaluation";

        // The bound the culler rests on, measured rather than asserted about:
        // sixteen influences each pushing twice the cap still cannot exceed the
        // number the CPU pads every instance AABB by. Unclamped the sum would
        // be around 24, so the lower bound below is what proves the clamp
        // actually engaged rather than the column simply being quiet.
        EXPECT_LE(magnitude(4), FoliageInteractionMaximumDisplacement(1.0f) + 1e-3f)
            << "the summed push escaped its clamp";
        EXPECT_GT(magnitude(4), 0.5f) << "the saturation column did not actually saturate";

        // The radial term points AWAY from the influence, and the bend stays
        // horizontal — a vertical component here would disagree with
        // foliageWindOffset about where the same plant's tip is.
        EXPECT_GT(pixels[8 * 4], 1e-3f) << "the radial push pointed at the actor, not away from it";
        EXPECT_FLOAT_EQ(pixels[8 * 4 + 1], 0.0f) << "the bend acquired a vertical component";
    }
} // namespace OloEngine::Tests
