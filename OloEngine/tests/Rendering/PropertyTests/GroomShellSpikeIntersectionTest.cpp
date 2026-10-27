#include "OloEnginePCH.h"
// OLO_TEST_LAYER: shaderpipe
#include "../../Groom/GroomShellSpikeGLState.h"
#include "RenderPropertyTest.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"

#include <glm/glm.hpp>
#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    TEST(GroomShellSpikeIntersection, DistantCameraKeepsTheCylinderRadiusAndHeightSignal)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        GroomShellSpike::ExtraState extra;
        GLStateGuard guard("GroomShellSpikeIntersection", GLStateGuard::Policy::Restore);
        FramebufferSpecification spec;
        spec.Width = 64;
        spec.Height = 1;
        spec.Attachments = { FramebufferTextureFormat::RGBA32F };
        auto output = Framebuffer::Create(spec);
        const auto shader = Shader::Create("assets/shaders/tests/GroomShellSpikeIntersectionProbe.glsl");
        FullscreenPass pass;
        output->Bind();
        glViewport(0, 0, 64, 1);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        shader->Bind();
        pass.Draw(0);
        glFinish();
        std::vector<f32> values;
        ReadbackRgbaFloat(output->GetColorAttachmentHandle(0), 64, 1, values);
        ASSERT_EQ(values.size(), 64u * 4u);
        const glm::vec3 ray = glm::normalize(glm::vec3(-1.0f, 0.07f, -0.3f));
        const glm::vec3 perpendicular = glm::normalize(glm::vec3(-ray.y, ray.x, 0.0f));
        for (u32 pixel = 0; pixel < 64; ++pixel)
        {
            const f32 distance = 1.0f + static_cast<f32>(pixel) * 8.0f;
            const glm::vec3 origin = -ray * distance + perpendicular * 0.10f;
            // Independent double-precision quadratic on the float inputs,
            // not a CPU transcription of the shader's stable solver.
            const glm::dvec3 o(origin);
            const glm::dvec3 d(ray);
            const f64 a = d.x * d.x + d.y * d.y;
            const f64 b = o.x * d.x + o.y * d.y;
            const f64 radius = static_cast<f64>(0.11f);
            const f64 c = o.x * o.x + o.y * o.y - radius * radius;
            const f64 discriminant = b * b - a * c;
            ASSERT_GT(discriminant, 0.0);
            const f64 t = (-b - std::sqrt(discriminant)) / a;
            const glm::dvec3 expected = o + d * t;
            EXPECT_NEAR(values[pixel * 4 + 3], 1.0f, 1.0e-6f);
            for (u32 channel = 0; channel < 3; ++channel)
            {
                EXPECT_NEAR(values[pixel * 4 + channel], expected[channel], 1.0e-4) << "camera distance " << distance;
            }
        }
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
    }
} // namespace OloEngine::Tests
