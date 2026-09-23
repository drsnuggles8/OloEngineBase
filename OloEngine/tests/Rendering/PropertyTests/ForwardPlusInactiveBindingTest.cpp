// OLO_TEST_LAYER: L4
// =============================================================================
// ForwardPlusInactiveBindingTest.cpp — the Forward+ light buffers stay
// PUBLISHED on a frame where Forward+ is inactive.
//
// Every shader that includes ForwardPlusCommon.glsl declares storage bindings
// 9-12 and 18 and gates its READS on the Forward+ UBO's Enabled flag. On
// Vulkan a declared storage binding with no published occupant is logged as an
// [error] (issue #1052), and TiledForwardPlus::UnbindAfterShading is what
// empties those slots — so the first Forward frame after any Deferred or
// Forward+ frame used to draw PBR_MultiLight and DeferredLighting against
// empty slots and log five errors per shader. Found during #1394's live
// verification; it reproduced on the skin-free MaterialLab.olo on a
// Deferred -> Forward switch.
//
// The contract is backend-agnostic — "BindForShading publishes this pass's
// buffers whether or not it is active" — so it is asserted here through the GL
// binding points, which the Vulkan binding state mirrors (Bind() is
// glBindBufferBase semantics on both, see VulkanStorageBuffer::Bind).
//
// WHAT THIS DOES NOT COVER, and it bit once. The contract has two halves:
// BindForShading must publish while inactive (asserted here), AND its callers
// must call it while inactive. The first version of the fix changed only the
// first half; SceneRenderPass and DeferredLightingPass both still wrapped the
// call in `if (ShouldUseForwardPlus())`, so this test passed and the live
// editor logged the same five errors. The call-site half is evidenced live
// (the Vulkan path-switch sweep in #1394's PR: 5 errors per run before, 0
// after, 2 runs each), because a frame unbinds the slots before any readback
// could see them.
//
// Classification: L4 (GPU binding state across passes). SKIPs cleanly with no
// GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/LightCulling/TiledForwardPlus.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <gtest/gtest.h>

#include <array>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr std::array<u32, 5> kForwardPlusShadingBindings = {
            ShaderBindingLayout::SSBO_FPLUS_POINT_LIGHTS, ShaderBindingLayout::SSBO_FPLUS_SPOT_LIGHTS,
            ShaderBindingLayout::SSBO_FPLUS_LIGHT_INDICES, ShaderBindingLayout::SSBO_FPLUS_LIGHT_GRID,
            ShaderBindingLayout::SSBO_FPLUS_SPHERE_AREA_LIGHTS
        };

        [[nodiscard]] GLint BoundStorageBuffer(u32 binding)
        {
            GLint bound = 0;
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, binding, &bound);
            return bound;
        }
    } // namespace

    class ForwardPlusInactiveBinding : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(64, 64);
        }
    };

    TEST_F(ForwardPlusInactiveBinding, BindForShadingPublishesTheBuffersWhileInactive)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        TiledForwardPlus& forwardPlus = Renderer3D::GetForwardPlus();
        ASSERT_TRUE(forwardPlus.IsInitialized()) << "Forward+ never initialised, so there is nothing to publish";

        // FORCE the inactive state rather than hoping for it. Forward+ is a
        // process-wide singleton and "active" is latched by the last
        // SetLights; the first version of this test ran a frame of an empty
        // scene and relied on that, and in a broad run an earlier fixture's
        // lights left it active. Restored on every exit path.
        struct ModeRestore
        {
            TiledForwardPlus& ForwardPlus;
            ForwardPlusMode Saved;
            ~ModeRestore()
            {
                ForwardPlus.SetMode(Saved);
            }
        } restore{ forwardPlus, forwardPlus.GetMode() };
        forwardPlus.SetMode(ForwardPlusMode::Never);
        forwardPlus.SetLights({}, {}, {});
        ASSERT_FALSE(forwardPlus.IsActive()) << "the precondition is an INACTIVE Forward+ frame";

        // The state UnbindAfterShading leaves behind: every slot empty.
        for (const u32 binding : kForwardPlusShadingBindings)
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, 0);

        forwardPlus.BindForShading();
        for (const u32 binding : kForwardPlusShadingBindings)
        {
            EXPECT_NE(BoundStorageBuffer(binding), 0)
                << "binding " << binding
                << " is empty after BindForShading on an inactive frame. Every shader that includes "
                   "ForwardPlusCommon.glsl declares it, and Vulkan logs an [error] per shader for a declared "
                   "storage binding with no occupant (issue #1052).";
        }
        // And the occupant is THIS pass's buffer, not whatever another tenant
        // of the binding number left behind — which is how the slots used to
        // look healthy until the first unbind.
        const LightGrid& grid = forwardPlus.GetLightGrid();
        ASSERT_TRUE(grid.GetLightIndexSSBO() && grid.GetLightGridSSBO());
        EXPECT_EQ(static_cast<u32>(BoundStorageBuffer(ShaderBindingLayout::SSBO_FPLUS_LIGHT_INDICES)),
                  grid.GetLightIndexSSBO()->GetRendererID());
        EXPECT_EQ(static_cast<u32>(BoundStorageBuffer(ShaderBindingLayout::SSBO_FPLUS_LIGHT_GRID)),
                  grid.GetLightGridSSBO()->GetRendererID());

        // Symmetric: the matching unbind releases what it published.
        forwardPlus.UnbindAfterShading();
        EXPECT_EQ(BoundStorageBuffer(ShaderBindingLayout::SSBO_FPLUS_LIGHT_GRID), 0)
            << "UnbindAfterShading left the grid bound after an inactive frame";
    }
} // namespace OloEngine::Tests
