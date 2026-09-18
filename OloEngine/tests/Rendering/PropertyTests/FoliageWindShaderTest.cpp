// OLO_TEST_LAYER: L2
#include "OloEnginePCH.h"
#include "RenderPropertyTest.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Terrain/Foliage/FoliageWind.h"
#include "OloEngine/Wind/WindSystem.h"
#include <glad/gl.h>
#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    TEST(FoliageWindShader, ProductionDeformationAnchorsDesynchronisesAndReprojects)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        GLStateGuard guard("FoliageWindShader", GLStateGuard::Policy::Restore);
        FramebufferSpecification spec;
        spec.Width = 10;
        spec.Height = 1;
        spec.Attachments = { FramebufferTextureFormat::RGBA16F };
        auto framebuffer = Framebuffer::Create(spec);
        auto shader = Shader::Create("assets/shaders/tests/ShaderUnit_FoliageWind.glsl");
        ASSERT_TRUE(shader);
        ASSERT_TRUE(shader->IsReady());
        framebuffer->Bind();
        glViewport(0, 0, 10, 1);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        shader->Bind();
        FullscreenPass pass;
        pass.Draw(0);
        glFinish();
        framebuffer->Unbind();
        std::vector<f32> pixels;
        ReadbackRgbaFloat(framebuffer->GetColorAttachmentRendererID(0), 10, 1, pixels);
        ASSERT_EQ(pixels.size(), 40u);
        const auto magnitude = [&pixels](sizet column)
        { return glm::length(glm::vec3(pixels[column * 4], pixels[column * 4 + 1], pixels[column * 4 + 2])); };
        EXPECT_LT(magnitude(0), 1e-5f) << "legacy defaults changed";
        EXPECT_LT(magnitude(1), 1e-5f) << "root moved";
        EXPECT_GT(magnitude(2), 1e-4f) << "previous wind was not evaluated";
        EXPECT_LT(magnitude(3), 1e-5f) << "pause emitted wind velocity";
        EXPECT_LT(magnitude(4), 1e-5f) << "wind parameter reset emitted stale velocity";
        EXPECT_GT(magnitude(5), 1e-4f) << "canonical phases moved in lockstep";
        EXPECT_LT(magnitude(7), 0.01f) << "legacy enabled field clock or amplitude changed";
        EXPECT_GT(magnitude(8), 1e-3f) << "noncentral impostor history must rotate with its centre";
        EXPECT_LE(magnitude(6), FoliageWindMaximumDisplacement(2.0f, { 0.4f, 1.0f, 1.0f, 0.0f }) + 0.005f);
        // Column 9 (issue #1238): a LEGACY layer under an enabled wind field,
        // bent by an interaction, is the first case that reaches
        // foliageWindNormal with u_WindClock.x != u_Time. If its finite
        // difference evaluates the wind on the wrong clock, the gap is divided
        // by epsilon = 0.001 and the transported normal points anywhere; a
        // small bend near the root must leave it within a few degrees of +Y.
        EXPECT_GT(pixels[9 * 4], 0.9f) << "the normal Jacobian differenced two different clocks";
    }
    TEST(FoliageWindShader, ReinitializationRequiresAFrameBeforeWindHistoryIsValid)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const bool restoreInitialized = WindSystem::IsInitialized();
        WindSystem::Shutdown();
        struct RestoreWind
        {
            bool Initialized;
            ~RestoreWind()
            {
                WindSystem::Shutdown();
                if (Initialized)
                    WindSystem::Init();
            }
        } restore{ restoreInitialized };
        GLStateGuard guard("WindLifecycle", GLStateGuard::Policy::Restore);
        for (const bool enabled : { false, true })
            for (u32 lifecycle = 0; lifecycle < 2; ++lifecycle)
            {
                SCOPED_TRACE(::testing::Message() << "enabled=" << enabled << " lifecycle=" << lifecycle);
                WindSystem::Init();
                ASSERT_TRUE(WindSystem::IsInitialized());
                EXPECT_FALSE(WindSystem::HasStableParameters());
                EXPECT_FLOAT_EQ(WindSystem::GetGPUData().TimeAndFlags.x, 0.0f);
                EXPECT_FLOAT_EQ(WindSystem::GetGPUData().TimeAndFlags.w, 0.0f);
                WindSettings settings;
                settings.Enabled = enabled;
                settings.GridResolution = 1;
                WindSystem::Update(settings, glm::vec3(0.0f), Timestep(0.25f));
                EXPECT_FALSE(WindSystem::HasStableParameters());
                EXPECT_FLOAT_EQ(WindSystem::GetGPUData().TimeAndFlags.w, 0.0f);
                WindSystem::Update(settings, glm::vec3(0.0f), Timestep(0.5f));
                EXPECT_TRUE(WindSystem::HasStableParameters());
                EXPECT_FLOAT_EQ(WindSystem::GetGPUData().TimeAndFlags.x, 0.75f);
                EXPECT_FLOAT_EQ(WindSystem::GetGPUData().TimeAndFlags.w, 0.25f);
                settings.Speed += 1.0f;
                WindSystem::Update(settings, glm::vec3(0.0f), Timestep(0.0f));
                EXPECT_FALSE(WindSystem::HasStableParameters());
                WindSystem::Update(settings, glm::vec3(0.0f), Timestep(0.0f));
                EXPECT_TRUE(WindSystem::HasStableParameters());
                WindSystem::Shutdown();
            }
    }
} // namespace OloEngine::Tests
