// OLO_TEST_LAYER: L1
//
// ShadowReceiverTexelRect (#1533): the texels of a shadow view a groom's
// strands can read, which the shadow pass copies into the opaque copy the
// strands sample. A texel a strand reads outside the rect holds whatever an
// earlier frame copied there, so the rect must contain every texel the
// SAMPLING reads, on every backend. The sampling goes through the matrix
// ShadowMap uploads -- RHI::AdjustProjectionForShaderReconstruction of the
// GL-convention light matrix, the row flip on Vulkan, where every shadow map
// is stored top-down -- and `projCoords * 0.5 + 0.5`, offset into the atlas
// entry's tile. These cases model that lookup independently of the rect's
// arithmetic and hold the rect to it, on GL and on Vulkan.
//
// The rect used to be projected through the raw matrix on both backends. On
// Vulkan that is the mirror image of the receivers' texels: live, the dog's
// coat went dark in straight-edged patches wherever it read the rows an
// earlier camera's copy had left (the full-body view after the low hero
// view). The negative control below keeps that rect and expects it to miss
// the Vulkan receivers, so a case that stops being adversarial is noticed.
// Pure CPU math: no GL or Vulkan context.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/RendererAPI.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    struct ScopedRendererApi
    {
        explicit ScopedRendererApi(RendererAPI::API api)
            : m_Previous(RendererAPI::GetAPI())
        {
            RendererAPI::SetAPI(api);
        }
        ~ScopedRendererApi()
        {
            RendererAPI::SetAPI(m_Previous);
        }
        ScopedRendererApi(const ScopedRendererApi&) = delete;
        ScopedRendererApi& operator=(const ScopedRendererApi&) = delete;
        RendererAPI::API m_Previous;
    };

    struct TexelRect
    {
        u32 X = 0;
        u32 Y = 0;
        u32 Width = 0;
        u32 Height = 0;
        [[nodiscard]] bool Contains(const glm::vec2& texel) const
        {
            return texel.x >= static_cast<f32>(X) && texel.x < static_cast<f32>(X + Width) &&
                   texel.y >= static_cast<f32>(Y) && texel.y < static_cast<f32>(Y + Height);
        }
    };

    struct Tile
    {
        u32 X = 0;
        u32 Y = 0;
        u32 Size = 0;
    };

    // One light view and a dog-sized coat in it, placed so the coat's rows and
    // their mirror image do not overlap even with the rect's padding.
    struct Case
    {
        const char* What = "";
        glm::mat4 LightVP{ 1.0f };
        Tile Where;
        BoundingBox Coat;
    };

    std::array<Case, 2> Cases()
    {
        // A cascade-like view: an orthographic box around the camera, the sun
        // coming in from high on one side; the coat 5 m in front of its centre.
        const glm::vec3 toSun = glm::normalize(glm::vec3(-0.45f, 0.80f, 0.40f));
        const glm::mat4 sun = glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 120.0f) *
                              glm::lookAt(toSun * 50.0f, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        // A spot light a few metres off, aimed above the dog: the coat sits in
        // the lower part of a 2048 entry of the atlas, wholly inside the cone.
        const glm::mat4 spot = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 30.0f) *
                               glm::lookAt(glm::vec3(5.5f, 4.5f, 6.5f), glm::vec3(2.6f, 2.0f, 2.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        return { {
            { "sun cascade", sun, Tile{ 0u, 0u, 4096u }, BoundingBox(glm::vec3(-1.0f, 0.0f, 4.0f), glm::vec3(0.0f, 0.75f, 5.7f)) },
            { "spot atlas entry", spot, Tile{ 2048u, 0u, 2048u }, BoundingBox(glm::vec3(2.1f, 0.0f, 1.2f), glm::vec3(3.1f, 0.75f, 2.9f)) },
        } };
    }

    // Where the receivers' lookup reads a world point, in the view's [0,1]
    // square: the uploaded matrix and the shader's own mapping. Under whichever
    // backend is set when it runs.
    glm::vec2 SampledUv(const glm::mat4& lightVP, const glm::vec3& point)
    {
        const glm::vec4 clip = RHI::AdjustProjectionForShaderReconstruction(lightVP) * glm::vec4(point, 1.0f);
        return (glm::vec2(clip) / clip.w) * 0.5f + 0.5f;
    }

    // The texel that lookup reads, inside the entry's tile.
    glm::vec2 SampledTexel(const glm::mat4& lightVP, const Tile& tile, const glm::vec3& point)
    {
        return glm::vec2(static_cast<f32>(tile.X), static_cast<f32>(tile.Y)) + SampledUv(lightVP, point) * static_cast<f32>(tile.Size);
    }

    TexelRect RectFor(const Case& c)
    {
        TexelRect rect;
        EXPECT_TRUE(ShadowReceiverTexelRect(c.Coat, c.LightVP, c.Where.X, c.Where.Y, c.Where.Size, rect.X, rect.Y, rect.Width,
                                            rect.Height));
        return rect;
    }

    // Points through the box, its corners and faces included.
    template<typename Visit>
    void ForEachPointIn(const BoundingBox& box, Visit&& visit)
    {
        constexpr int kSteps = 8;
        for (int i = 0; i <= kSteps; ++i)
        {
            for (int j = 0; j <= kSteps; ++j)
            {
                for (int k = 0; k <= kSteps; ++k)
                {
                    const glm::vec3 t(static_cast<f32>(i) / kSteps, static_cast<f32>(j) / kSteps, static_cast<f32>(k) / kSteps);
                    visit(glm::mix(box.Min, box.Max, t));
                }
            }
        }
    }

    struct Tally
    {
        int Missed = 0;    // sampled texels outside the rect
        int OutOfView = 0; // points the view does not see (the case is wrong if any)
        int Total = 0;
    };

    Tally Measure(const Case& c, const TexelRect& rect)
    {
        Tally tally;
        ForEachPointIn(c.Coat, [&](const glm::vec3& p)
                       {
                           ++tally.Total;
                           const glm::vec2 uv = SampledUv(c.LightVP, p);
                           tally.OutOfView += (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) ? 1 : 0;
                           tally.Missed += rect.Contains(SampledTexel(c.LightVP, c.Where, p)) ? 0 : 1; });
        return tally;
    }
} // namespace

TEST(ShadowReceiverTexelRect, EveryTexelAReceiverSamplesIsCopiedOnBothBackends)
{
    for (const RendererAPI::API api : { RendererAPI::API::OpenGL, RendererAPI::API::Vulkan })
    {
        SCOPED_TRACE(api == RendererAPI::API::Vulkan ? "Vulkan" : "OpenGL");
        const ScopedRendererApi backend(api);
        for (const Case& c : Cases())
        {
            SCOPED_TRACE(c.What);
            const TexelRect rect = RectFor(c);
            EXPECT_GE(rect.X, c.Where.X);
            EXPECT_GE(rect.Y, c.Where.Y);
            EXPECT_LE(rect.X + rect.Width, c.Where.X + c.Where.Size);
            EXPECT_LE(rect.Y + rect.Height, c.Where.Y + c.Where.Size);
            const Tally tally = Measure(c, rect);
            ASSERT_EQ(tally.OutOfView, 0) << "the case's coat leaves the view; the rect is clamped there and proves nothing";
            EXPECT_EQ(tally.Missed, 0) << tally.Missed << " of " << tally.Total << " sampled texels fall outside the copied rect";
        }
    }
}

// The rect the pass copied before (#1533): projected through the raw matrix,
// which is what the GL backend's rect still is. Under Vulkan's sampling it is
// the mirror image of the coat's rows, so it must miss them -- if it stops
// missing, the case no longer separates the two and the test above proves
// nothing about the flip.
TEST(ShadowReceiverTexelRect, NegativeControlTheGlConventionRectMissesTheVulkanReceivers)
{
    for (const Case& c : Cases())
    {
        SCOPED_TRACE(c.What);
        TexelRect glRect;
        {
            const ScopedRendererApi gl(RendererAPI::API::OpenGL);
            glRect = RectFor(c);
        }
        const ScopedRendererApi vulkan(RendererAPI::API::Vulkan);
        const TexelRect vulkanRect = RectFor(c);
        // The flip moves rows only.
        EXPECT_EQ(vulkanRect.X, glRect.X);
        EXPECT_EQ(vulkanRect.Width, glRect.Width);
        EXPECT_NE(vulkanRect.Y, glRect.Y);
        const Tally tally = Measure(c, glRect);
        ASSERT_EQ(tally.OutOfView, 0);
        EXPECT_EQ(tally.Missed, tally.Total) << "the GL-convention rect holds " << (tally.Total - tally.Missed) << " of "
                                             << tally.Total << " texels Vulkan samples; the case no longer tells the conventions apart";
    }
}

TEST(ShadowReceiverTexelRect, AnUnknownOrUnprojectableBoxCopiesTheWholeTile)
{
    const std::array<Case, 2> cases = Cases();
    const Case& sun = cases[0];
    const Case& spot = cases[1];
    for (const RendererAPI::API api : { RendererAPI::API::OpenGL, RendererAPI::API::Vulkan })
    {
        SCOPED_TRACE(api == RendererAPI::API::Vulkan ? "Vulkan" : "OpenGL");
        const ScopedRendererApi backend(api);
        TexelRect rect;
        ASSERT_TRUE(ShadowReceiverTexelRect(NoBounds, spot.LightVP, spot.Where.X, spot.Where.Y, spot.Where.Size, rect.X, rect.Y,
                                            rect.Width, rect.Height));
        EXPECT_EQ(rect.X, spot.Where.X);
        EXPECT_EQ(rect.Y, spot.Where.Y);
        EXPECT_EQ(rect.Width, spot.Where.Size);
        EXPECT_EQ(rect.Height, spot.Where.Size);

        // A box reaching behind a perspective light has corners with no texel.
        const BoundingBox behind(glm::vec3(4.0f, 2.0f, 5.0f), glm::vec3(7.0f, 6.0f, 8.0f));
        ASSERT_TRUE(ShadowReceiverTexelRect(behind, spot.LightVP, spot.Where.X, spot.Where.Y, spot.Where.Size, rect.X, rect.Y,
                                            rect.Width, rect.Height));
        EXPECT_EQ(rect.Width, spot.Where.Size);
        EXPECT_EQ(rect.Height, spot.Where.Size);

        // A box the view does not see copies nothing.
        const BoundingBox outside(glm::vec3(40.0f, 0.0f, 40.0f), glm::vec3(41.0f, 1.0f, 41.0f));
        EXPECT_FALSE(ShadowReceiverTexelRect(outside, sun.LightVP, sun.Where.X, sun.Where.Y, sun.Where.Size, rect.X, rect.Y,
                                             rect.Width, rect.Height));
    }
}
