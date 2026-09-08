// OLO_TEST_LAYER: L1
// =============================================================================
// LightmapSkyAndTextureBakeTest.cpp
//
// Issue #869's two acceptance criteria, end to end through the real bake:
//
//   * "a textured test scene shows bounce colour tracking the TEXTURE rather
//     than the factor, with region-mean evidence"
//   * "an exterior scene bakes visible sky bounce, validated against whatever
//     ground truth the chosen option establishes"
//
// Both are asserted as an A/B against the SAME room, differing only in the
// build option under test — which is the shape ADR 0022 chose deliberately.
// The richer world is opt-in at the call site, so "with maps" and "without
// maps" are two builds of one description, and the difference between them is
// attributable to nothing else. A single absolute number could not make that
// claim: an unexpected bake value has a dozen candidate causes, a DIFFERENCE
// between two builds of one room has one.
//
// The sky half also carries the ground truth ADR 0022 §3 promised. An
// unoccluded upward-facing texel under a uniform sky of radiance L must bake
// E = pi * L exactly — closed form, no renderer in it — and this asserts that
// on a real baked atlas texel rather than on an isolated integrator call. The
// directional half is pinned in ReferenceEnvironmentTest against a
// deterministic quadrature; here it only has to arrive at the atlas.
//
// Headless: no GPU, no ECS. The material maps come from a synthetic provider
// (this is exactly what ReferenceSceneBuildOptions::MaterialMapProvider is
// injectable for) and the sky from ReferenceEnvironmentCubemap, so nothing
// here needs a readback.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Baking/LightmapBaker.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/PathTracing/ReferenceSceneBuilder.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;

    namespace
    {
        constexpr f32 kPi = std::numbers::pi_v<f32>;

        [[nodiscard]] Ref<MeshSource> MakeCube()
        {
            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            return cube ? cube->GetMeshSource() : nullptr;
        }

        [[nodiscard]] glm::mat4 MakeTransform(const glm::vec3& translation, const glm::vec3& scale)
        {
            return glm::translate(glm::mat4(1.0f), translation) * glm::scale(glm::mat4(1.0f), scale);
        }

        // A uniform 8-bit image. sRGB decode is FromRgba8's job, and passing
        // srgb=false keeps this fixture's expected values arithmetic rather
        // than transfer-function algebra — the sRGB path is already pinned by
        // GpuPathTracerContract.ReferenceTextureSamplesLikeTheMaterialSampler.
        [[nodiscard]] std::shared_ptr<const ReferenceTexture> MakeFlatTexture(const glm::u8vec4& rgba)
        {
            const std::vector<u8> pixels{ rgba.r, rgba.g, rgba.b, rgba.a };
            return std::make_shared<const ReferenceTexture>(
                ReferenceTexture::FromRgba8(1, 1, std::span<const u8>(pixels), /*srgb=*/false));
        }

        struct Piece
        {
            u64 Uuid;
            glm::mat4 Transform;
            glm::vec3 BaseColor;
            // The resolved Material's name. A map provider is handed the
            // Material, and two pieces may share a base colour, so the name is
            // how a fixture addresses one of them.
            std::string MaterialName;
        };

        struct BakeOutcome
        {
            LightmapBakeResult Result;
            LightmapBakePrepared Prepared;
        };

        // One bake of `pieces`, with whatever richer-population options the
        // caller supplies. `light` is optional (an exterior lit only by sky
        // passes intensity 0, which the builder skips silently).
        BakeOutcome BakeRoom(const std::vector<Piece>& pieces, const PointLightComponent* light,
                             const glm::vec3& lightPosition, const ReferenceSceneBuildOptions& options,
                             u32 samplesPerTexel = 64)
        {
            BakeOutcome outcome;
            LightmapBakeSettings settings;
            settings.AtlasSize = 64;
            settings.MinRegionSize = 8;
            settings.SamplesPerTexel = samplesPerTexel;
            settings.MaxBounces = 3;
            settings.TexelsPerMeter = 1.5f;
            settings.DilationPasses = 2;
            settings.BakeKey = 0x8690'0001u;

            std::vector<LightmapBakeInput> inputs;
            ReferenceSceneBuilder builder;
            std::vector<Ref<Material>> materials; // must outlive Build()

            for (const Piece& piece : pieces)
            {
                LightmapBakeInput input;
                input.EntityUUID = piece.Uuid;
                input.Mesh = MakeCube();
                input.WorldTransform = piece.Transform;
                inputs.push_back(input);

                Ref<Material> material = Material::CreatePBR(piece.MaterialName, piece.BaseColor, 0.0f, 0.9f);
                materials.push_back(material);
                builder.AddMeshEntity(input.Mesh, input.WorldTransform, material.get());
            }
            if (light)
                builder.AddPointLight(*light, lightPosition);

            const ReferenceScene world = builder.Build(options);

            std::string error;
            const bool preparedOk = LightmapBaker::Prepare(inputs, settings, outcome.Prepared, error);
            EXPECT_TRUE(preparedOk) << error;
            if (!preparedOk)
                return outcome;
            // Every texel lookup below indexes a SINGLE page (see
            // LightmapBakeParityTest for why that is asserted rather than
            // assumed — a second page silently turns these into wrong-address
            // reads).
            EXPECT_EQ(outcome.Prepared.PageCount, 1u);

            outcome.Result = LightmapBaker::BakeTexels(outcome.Prepared, world, settings);
            return outcome;
        }

        // Mean baked irradiance over every texel job the predicate accepts.
        template <typename Predicate>
        [[nodiscard]] glm::vec3 RegionMean(const BakeOutcome& baked, Predicate&& accept, u32& outCount)
        {
            const auto& texels = baked.Result.Asset->GetTexelData();
            const u32 atlasSize = baked.Prepared.AtlasSize;
            glm::vec3 sum(0.0f);
            outCount = 0;
            for (const auto& job : baked.Prepared.Jobs)
            {
                if (!accept(job))
                    continue;
                const sizet t = (static_cast<sizet>(job.AtlasY) * atlasSize + job.AtlasX) * 4;
                sum += glm::vec3(texels[t + 0], texels[t + 1], texels[t + 2]);
                ++outCount;
            }
            return outCount > 0 ? sum / static_cast<f32>(outCount) : glm::vec3(0.0f);
        }

        // The floor's top surface.
        [[nodiscard]] bool IsFloorTop(const LightmapTexelJob& job)
        {
            return job.WorldNormal.y > 0.9f && std::abs(job.WorldPos.y) < 0.05f;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Acceptance 3: bounce colour tracks the TEXTURE, not the factor
    // -------------------------------------------------------------------------

    TEST(LightmapSkyAndTextureBake, BounceColourTracksTheAlbedoTextureRatherThanTheFactor)
    {
        // A grey floor between two WHITE-factor walls. The factors are white on
        // both sides, so a factor-only bake cannot tint the floor at all — and
        // that is not a contrived fixture, it is what an imported material with
        // its colour in a texture looks like. The albedo maps then paint the
        // -X wall red and the +X wall green.
        const std::vector<Piece> pieces{
            { 0x2001, MakeTransform({ 0.0f, -0.1f, 0.0f }, { 6.0f, 0.2f, 4.0f }), { 0.6f, 0.6f, 0.6f }, "Floor" },
            { 0x2002, MakeTransform({ -3.1f, 1.5f, 0.0f }, { 0.2f, 3.0f, 4.0f }), { 1.0f, 1.0f, 1.0f }, "WallMinusX" },
            { 0x2003, MakeTransform({ 3.1f, 1.5f, 0.0f }, { 0.2f, 3.0f, 4.0f }), { 1.0f, 1.0f, 1.0f }, "WallPlusX" },
            { 0x2004, MakeTransform({ 0.0f, 3.1f, 0.0f }, { 6.0f, 0.2f, 4.0f }), { 0.6f, 0.6f, 0.6f }, "Ceiling" },
        };
        PointLightComponent light;
        light.m_Color = { 1.0f, 1.0f, 1.0f };
        light.m_Intensity = 10.0f;
        light.m_Range = 14.0f;
        light.m_Attenuation = 2.0f;
        const glm::vec3 lightPosition{ 0.0f, 1.6f, 0.0f };

        // Arm A: the shipped pre-#869 world — no provider, so factor-only.
        const BakeOutcome factorOnly =
            BakeRoom(pieces, &light, lightPosition, ReferenceSceneBuildOptions{});
        ASSERT_TRUE(factorOnly.Result.Success) << factorOnly.Result.Error;

        // Arm B: identical, plus albedo maps. The provider keys off the base
        // colour the fixture gave each piece, which is how one lambda can
        // paint the two walls differently without the builder exposing
        // per-entity identity.
        const auto red = MakeFlatTexture({ 230, 20, 20, 255 });
        const auto green = MakeFlatTexture({ 20, 230, 20, 255 });
        ReferenceSceneBuildOptions textured;
        textured.MaterialMapProvider = [&red, &green](const Material& material) -> ReferenceMaterialMaps
        {
            ReferenceMaterialMaps maps;
            if (material.GetName() == "WallMinusX")
                maps.Albedo = red;
            else if (material.GetName() == "WallPlusX")
                maps.Albedo = green;
            return maps;
        };
        const BakeOutcome withMaps = BakeRoom(pieces, &light, lightPosition, textured);
        ASSERT_TRUE(withMaps.Result.Success) << withMaps.Result.Error;

        u32 redSideCount = 0;
        u32 greenSideCount = 0;
        const auto nearMinusX = [](const LightmapTexelJob& job) { return IsFloorTop(job) && job.WorldPos.x < -2.0f; };
        const auto nearPlusX = [](const LightmapTexelJob& job) { return IsFloorTop(job) && job.WorldPos.x > 2.0f; };

        const glm::vec3 flatMinusX = RegionMean(factorOnly, nearMinusX, redSideCount);
        const glm::vec3 flatPlusX = RegionMean(factorOnly, nearPlusX, greenSideCount);
        ASSERT_GT(redSideCount, 3u) << "no floor texels landed beside the -X wall";
        ASSERT_GT(greenSideCount, 3u) << "no floor texels landed beside the +X wall";

        const glm::vec3 texturedMinusX = RegionMean(withMaps, nearMinusX, redSideCount);
        const glm::vec3 texturedPlusX = RegionMean(withMaps, nearPlusX, greenSideCount);

        // The control: with white factors on both walls the factor-only bake
        // is colour-NEUTRAL on both sides. If this ever fails, the fixture has
        // stopped isolating what it claims to isolate and every assertion
        // below it is measuring something else.
        EXPECT_NEAR(flatMinusX.r / flatMinusX.g, 1.0f, 0.06f)
            << "factor-only floor beside the -X wall is already tinted: " << flatMinusX.r << " vs " << flatMinusX.g;
        EXPECT_NEAR(flatPlusX.g / flatPlusX.r, 1.0f, 0.06f)
            << "factor-only floor beside the +X wall is already tinted: " << flatPlusX.g << " vs " << flatPlusX.r;

        // The claim: bounce colour now carries the TEXTURE. Asserted as an
        // ordering within each side, so no tolerance can wash it out.
        // Measured 1.86x (0.3266 vs 0.1753) against the factor-only control's
        // 1.000x (0.3912 vs 0.3912). Asserted as an ordering at 1.3 rather
        // than as a value, because the quantity under test is "does the bounce
        // carry the texture at all", which a tolerance band cannot express.
        EXPECT_GT(texturedMinusX.r, texturedMinusX.g * 1.3f)
            << "floor beside the red-TEXTURED wall is not red-shifted: r=" << texturedMinusX.r
            << " g=" << texturedMinusX.g;
        EXPECT_GT(texturedPlusX.g, texturedPlusX.r * 1.3f)
            << "floor beside the green-TEXTURED wall is not green-shifted: g=" << texturedPlusX.g
            << " r=" << texturedPlusX.r;

        // And the size of what #869 called "an approximation everywhere else":
        // the same room, the same lights, the same geometry, with and without
        // the maps the raster path has always shaded from.
        OLO_CORE_INFO("#869 texture divergence, floor beside the -X wall: factor-only rgb ({:.4f}, {:.4f}, {:.4f}) "
                      "vs textured ({:.4f}, {:.4f}, {:.4f})",
                      flatMinusX.r, flatMinusX.g, flatMinusX.b, texturedMinusX.r, texturedMinusX.g, texturedMinusX.b);
    }

    TEST(LightmapSkyAndTextureBake, AProviderThatSuppliesNoMapsChangesNothingBitForBit)
    {
        // Half of what ADR 0022 claims when it says the parity suites still
        // pin what they pinned. The other half is that those suites pass no
        // provider at all and still pass unchanged, which only the suite can
        // show; this pins the seam BETWEEN the two — that the provider hook
        // itself, exercised and returning nothing, perturbs no texel. memcmp
        // rather than a tolerance, because the bake's contract is
        // bit-identity (baked-lightmap-pipeline.md §4) and a tolerance here
        // would swallow exactly the drift this is looking for.
        const std::vector<Piece> pieces{
            { 0x2101, MakeTransform({ 0.0f, -0.1f, 0.0f }, { 6.0f, 0.2f, 4.0f }), { 0.6f, 0.6f, 0.6f }, "Floor" },
            { 0x2102, MakeTransform({ -3.1f, 1.5f, 0.0f }, { 0.2f, 3.0f, 4.0f }), { 0.7f, 0.05f, 0.05f }, "Wall" },
        };
        PointLightComponent light;
        light.m_Color = { 1.0f, 1.0f, 1.0f };
        light.m_Intensity = 10.0f;
        light.m_Range = 14.0f;
        light.m_Attenuation = 2.0f;

        ReferenceSceneBuildOptions defaulted;
        ReferenceSceneBuildOptions explicitlyEmpty;
        explicitlyEmpty.MaterialMapProvider = [](const Material&) { return ReferenceMaterialMaps{}; };

        const BakeOutcome a = BakeRoom(pieces, &light, { 0.0f, 1.6f, 0.0f }, defaulted, 32);
        const BakeOutcome b = BakeRoom(pieces, &light, { 0.0f, 1.6f, 0.0f }, explicitlyEmpty, 32);
        ASSERT_TRUE(a.Result.Success) << a.Result.Error;
        ASSERT_TRUE(b.Result.Success) << b.Result.Error;

        const auto& left = a.Result.Asset->GetTexelData();
        const auto& right = b.Result.Asset->GetTexelData();
        ASSERT_EQ(left.size(), right.size());
        EXPECT_EQ(std::memcmp(left.data(), right.data(), left.size() * sizeof(f32)), 0)
            << "a provider that returns no maps changed the bake — the factor-only path is not clean";
    }

    // -------------------------------------------------------------------------
    // Acceptance 4: an exterior bakes visible sky bounce, against ground truth
    // -------------------------------------------------------------------------

    TEST(LightmapSkyAndTextureBake, ExteriorBakesSkyBounceAndTheOpenTexelMatchesPiTimesRadiance)
    {
        // An EXTERIOR: a floor slab and one wall, no ceiling, no lights at all.
        // Every photon in this bake comes from the sky, so a factor-of-two
        // error cannot hide behind a direct term.
        const std::vector<Piece> pieces{
            { 0x2201, MakeTransform({ 0.0f, -0.1f, 0.0f }, { 8.0f, 0.2f, 4.0f }), { 0.5f, 0.5f, 0.5f }, "Ground" },
            // An overhang at +X that shades the floor beneath it, so "sky
            // bounce arrived" can be asserted as a CONTRAST rather than as an
            // absolute brightness. LOW (underside at y = 0.7) on purpose: at
            // y = 1.9 it subtended little enough of the hemisphere to dim the
            // floor by only 20%, which is a real shadow but a weak assertion.
            { 0x2202, MakeTransform({ 3.0f, 0.8f, 0.0f }, { 2.0f, 0.2f, 4.0f }), { 0.5f, 0.5f, 0.5f }, "Overhang" },
        };

        const glm::vec3 skyRadiance(0.2f, 0.35f, 0.8f); // a blue sky
        ReferenceSceneBuildOptions withSky;
        withSky.EnvironmentCubemap = std::make_shared<const ReferenceEnvironmentCubemap>(
            ReferenceEnvironmentCubemap::Constant(skyRadiance));

        const BakeOutcome lit = BakeRoom(pieces, nullptr, glm::vec3(0.0f), withSky, 96);
        ASSERT_TRUE(lit.Result.Success) << lit.Result.Error;

        // The control arm: the identical exterior with no sky, which is what
        // #869 says the bake does today — "an exterior under a sky/HDRI bakes
        // with NO sky contribution".
        const BakeOutcome dark = BakeRoom(pieces, nullptr, glm::vec3(0.0f), ReferenceSceneBuildOptions{}, 96);
        ASSERT_TRUE(dark.Result.Success) << dark.Result.Error;

        // Open floor: away from the overhang, so the hemisphere above is
        // entirely sky.
        const auto openFloor = [](const LightmapTexelJob& job)
        { return IsFloorTop(job) && job.WorldPos.x < -1.0f; };
        // Under the overhang, and away from its open edge at x = 4 — an edge
        // texel sees most of the sky and would dilute the contrast with
        // geometry rather than with transport.
        const auto shadedFloor = [](const LightmapTexelJob& job)
        { return IsFloorTop(job) && job.WorldPos.x > 2.4f && job.WorldPos.x < 3.6f; };

        u32 openCount = 0;
        u32 shadedCount = 0;
        const glm::vec3 open = RegionMean(lit, openFloor, openCount);
        const glm::vec3 shaded = RegionMean(lit, shadedFloor, shadedCount);
        u32 darkCount = 0;
        const glm::vec3 openWithoutSky = RegionMean(dark, openFloor, darkCount);
        ASSERT_GT(openCount, 3u) << "no open floor texels";
        ASSERT_GT(shadedCount, 3u) << "no shaded floor texels under the overhang";

        // 1. The sky arrives at all, and did not before.
        EXPECT_LT(openWithoutSky.r + openWithoutSky.g + openWithoutSky.b, 1e-5f)
            << "the no-sky control is not dark — this exterior has another light in it";
        EXPECT_GT(open.b, 0.1f) << "no sky bounce reached the open floor";

        // 2. It is the SKY's colour, not a grey. Asserted as an ordering.
        EXPECT_GT(open.b, open.g * 1.5f);
        EXPECT_GT(open.g, open.r * 1.3f);

        // 3. GROUND TRUTH (ADR 0022 §3). An unoccluded upward-facing texel sees
        //    nothing but sky, so its irradiance is the closed form E = pi * L —
        //    no renderer, no convention, no calibration constant. The lightmap
        //    stores INDIRECT irradiance E by construction, and with no punctual
        //    lights in this scene there is no direct term to subtract: what the
        //    atlas holds IS that integral.
        const glm::vec3 expected = skyRadiance * kPi;
        for (u32 c = 0; c < 3; ++c)
        {
            // Measured: the bake lands 0.26% BELOW pi*L on all three channels
            // (0.6267 / 1.0967 / 2.5068 against 0.6283 / 1.0996 / 2.5133) —
            // the residual is the sliver of the open region's hemisphere the
            // overhang still subtends, and it errs low, which is the side a
            // missing-energy bug would also err on. 3% is an order of
            // magnitude of headroom over that without being able to swallow a
            // real one (a dropped bounce or a lost pi would move this by tens
            // of percent, not by one).
            EXPECT_NEAR(open[c] / expected[c], 1.0f, 0.03f)
                << "channel " << c << ": baked " << open[c] << " vs the analytic pi*L " << expected[c];
        }

        // 4. Occlusion still works — a sky that lit everything uniformly would
        //    pass 1-3 and be a broken visibility term.
        // Measured 0.46 of the open floor (1.1524 vs 2.5068). Asserted at
        // 0.65 so the claim is "the overhang casts a real shadow", not a
        // pinned number: an occlusion term that had died entirely would read
        // 1.0 here, and one that merely shifted would still pass.
        EXPECT_LT(shaded.b, open.b * 0.65f)
            << "the overhang cast no shadow in the sky bake: shaded " << shaded.b << " vs open " << open.b;

        OLO_CORE_INFO("#869 sky bake: open floor rgb ({:.4f}, {:.4f}, {:.4f}), analytic pi*L ({:.4f}, {:.4f}, "
                      "{:.4f}), shaded floor blue {:.4f}",
                      open.r, open.g, open.b, expected.r, expected.g, expected.b, shaded.b);
    }
} // namespace OloEngine::Tests
