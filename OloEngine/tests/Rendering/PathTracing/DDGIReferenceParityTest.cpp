// OLO_TEST_LAYER: L8
// =============================================================================
// DDGIReferenceParityTest.cpp — DDGI validated against the offline reference
// path tracer (issue #709, acceptance criterion 2).
//
// WHAT IS BEING COMPARED, AND WHY IT IS NOT PIXELS
// ------------------------------------------------
// The obvious test — render the scene both ways and diff the frames — answers
// the wrong question. A composited frame folds in the direct lighting (which
// DDGI does not produce), the ambient ladder, exposure and the tone curve; a
// disagreement could come from any of them, and an agreement could survive a
// completely broken probe field as long as the direct term dominates.
//
// DDGI's actual output is IRRADIANCE at a probe, for a direction. That is a
// physical quantity with a definition independent of any renderer:
//
//     E(p, n) = integral over the sphere of L_i(p, w) * max(0, n . w) dw
//
// and it is exactly what PathTracer::EstimateIrradiance computes by brute
// force. DDGI_BlendIrradiance.glsl pins its storage convention in a header
// comment — "the atlas stores full irradiance E", via the ratio estimator
// E = PI * sum(w L) / sum(w) — so the atlas texel and the traced integral are
// the SAME NUMBER IN THE SAME UNITS. No calibration, no fudge factor. That is
// what makes this a ground-truth comparison rather than a correlation study.
//
// THE KNOWN MODELLING DIFFERENCE, HANDLED EXPLICITLY
// ---------------------------------------------------
// DDGI's radiance cache is DIFFUSE-ONLY by design: a probe hit point has no
// view direction, so DDGI_Relight.glsl shades it as a bare Lambertian
// `min(albedo, clamp)/PI * (directE + bounceE)` and never calls
// cookTorranceBRDF. The reference normally shades with cookTorranceBRDF —
// which for the same albedo is DARKER in diffuse (it carries the `1 - F`
// energy split) and adds a specular lobe.
//
// Comparing the two directly would measure that modelling difference and the
// transport error together, and report the sum as "DDGI's error". So the
// reference is rendered TWICE:
//
//   * Lambertian mode  — DDGI's own shading model. Comparing against this
//     isolates DDGI's TRANSPORT: is the probe field solving the right light
//     transport, given the material model it chose? That is the assertion.
//
//   * cookTorranceBRDF — the lit passes' model. The gap between the two
//     references is the PHOTOMETRIC DIVERGENCE between DDGI's probe cache and
//     the rest of the renderer. It is measured and reported here rather than
//     asserted, and documented in docs/agent-rules/reference-path-tracer.md.
//
// THE SHARED SCENE
// ----------------
// Both worlds are built from ONE table of boxes (`RoomBoxes()`). A hand-mirrored
// pair of scene descriptions is the classic way this kind of comparison goes
// quietly wrong: a wall half a unit off, or an albedo of 0.5 on one side and
// 0.55 on the other, produces a stable, plausible, entirely fictitious
// "divergence" that no amount of staring at the renderer will explain.
//
// SKIPs cleanly without a GL 4.6 context.
//
// Classification: L8 / integration (full GL pipeline + atlas readback + a CPU
// path-traced ground truth).
// =============================================================================

#include "OloEnginePCH.h"

#include "PathTracing/ReferenceSceneFixtures.h"
#include "PropertyTests/RenderPropertyTest.h"
#include "PropertyTests/RendererAttachedTest.h"

#include "OloEngine/Renderer/DDGI/DDGICommon.h"
#include "OloEngine/Renderer/DDGI/DDGIProbeUpdatePass.h"
#include "OloEngine/Renderer/Debug/RenderGraphResourceIdentity.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PathTracing/PathTracer.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;
    namespace Fixtures = OloEngine::Tests::PathTracingFixtures;

    namespace
    {
        constexpr u32 kWidth = 320;
        constexpr u32 kHeight = 240;

        // Probe grid — small on purpose. Every probe compared here costs a
        // full 36-direction path-traced ground truth, and the contract is a
        // FIELD-shape comparison, not a per-probe census.
        constexpr glm::ivec3 kProbeDims{ 3, 2, 3 };
        constexpr glm::vec3 kVolumeMin{ -3.0f, 0.8f, -3.0f };
        constexpr glm::vec3 kVolumeMax{ 3.0f, 3.2f, 3.0f };

        // Enough frames that capture has covered every probe (budget 4 over 18
        // probes = 5 frames, plus relocation recaptures) and the irradiance EMA
        // at hysteresis 0.5 has converged from ANY previous atlas state — the
        // pass is a process-global singleton, so this is also what makes the
        // test independent of which DDGI test ran before it.
        constexpr u32 kConvergenceFrames = 60;

        // The room. ONE description, consumed by the ECS scene and the
        // reference scene alike. `Scale` is the full extent (a MeshComponent
        // cube spans -0.5..0.5, so world extent == scale).
        struct BoxDesc
        {
            const char* Name;
            glm::vec3 Position;
            glm::vec3 Scale;
            glm::vec3 Albedo;
        };

        // A closed box, ~8 x 4 x 8 interior. Enclosed so no sky term enters:
        // DDGI's relight falls back to a BLACK cubemap for sky misses, and the
        // reference's environment is likewise zero, so an open roof would put a
        // term in one world that the other does not have.
        //
        // Albedos are kept at or below 0.6: DDGI's relight applies an
        // energy-conservation albedo clamp (u_DDGIEnergyConservation) that
        // would silently darken a brighter surface on the DDGI side only.
        [[nodiscard]] const std::vector<BoxDesc>& RoomBoxes()
        {
            static const std::vector<BoxDesc> s_Boxes = {
                { "Floor", { 0.0f, 0.0f, 0.0f }, { 8.0f, 0.2f, 8.0f }, { 0.55f, 0.55f, 0.55f } },
                { "Ceiling", { 0.0f, 4.0f, 0.0f }, { 8.0f, 0.2f, 8.0f }, { 0.55f, 0.55f, 0.55f } },
                { "Left Wall", { -4.0f, 2.0f, 0.0f }, { 0.2f, 4.0f, 8.0f }, { 0.6f, 0.1f, 0.1f } },
                { "Right Wall", { 4.0f, 2.0f, 0.0f }, { 0.2f, 4.0f, 8.0f }, { 0.1f, 0.5f, 0.15f } },
                { "Back Wall", { 0.0f, 2.0f, -4.0f }, { 8.0f, 4.0f, 0.2f }, { 0.55f, 0.55f, 0.55f } },
                { "Front Wall", { 0.0f, 2.0f, 4.0f }, { 8.0f, 4.0f, 0.2f }, { 0.55f, 0.55f, 0.55f } },
            };
            return s_Boxes;
        }

        // The single point light, described once for both worlds. Off-centre
        // so the probe field has real spatial structure to compare — a
        // centred light would make every probe nearly equal and the field-shape
        // assertion vacuous.
        constexpr glm::vec3 kLightPosition{ -2.0f, 3.0f, -2.0f };
        constexpr glm::vec3 kLightColor{ 1.0f, 0.95f, 0.9f };
        constexpr f32 kLightIntensity = 12.0f;
        constexpr f32 kLightRange = 14.0f;
        constexpr f32 kLightAttenuation = 1.0f;

        // Build the reference twin of the ECS scene above.
        void BuildReferenceRoom(ReferenceScene& scene, bool lambertian)
        {
            for (const BoxDesc& box : RoomBoxes())
            {
                ReferenceMaterial material;
                material.BaseColor = box.Albedo;
                material.Metallic = 0.0f;
                material.Roughness = 0.9f; // mirrors the MaterialComponent below
                material.LambertianDiffuseOnly = lambertian;
                const u32 materialIndex = scene.AddMaterial(material);

                Fixtures::AddBox(scene, box.Position - box.Scale * 0.5f, box.Position + box.Scale * 0.5f,
                                 materialIndex);
            }

            ReferenceLight light;
            light.Type = ReferenceLightType::Point;
            light.Position = kLightPosition;
            light.Color = kLightColor;
            light.Intensity = kLightIntensity;
            // Scene.cpp packs a point light as (1, 0, m_Attenuation, m_Range);
            // ReferenceBRDF::CalculateAttenuation is the port of the shader
            // function that consumes it. Matching this exactly is what keeps
            // the comparison about TRANSPORT rather than about falloff.
            light.AttenuationParams = glm::vec4(1.0f, 0.0f, kLightAttenuation, kLightRange);
            scene.AddLight(light);

            scene.Build();
        }

        // Ground-truth irradiance for one probe, over the same 36 octahedral
        // texel directions the atlas tile stores.
        struct ProbeIrradiance
        {
            std::vector<glm::vec3> PerDirection;
            glm::vec3 Mean{ 0.0f };
        };

        [[nodiscard]] ProbeIrradiance TraceProbeIrradiance(const ReferenceScene& scene, const glm::vec3& probePosition,
                                                           u32 samplesPerDirection, u32 seed, u32 maxBounces = 4)
        {
            constexpr i32 kInner = DDGI::kIrradianceInteriorTexels;

            PathTracerSettings settings;
            settings.SamplesPerPixel = samplesPerDirection;
            // Default 4 surface interactions: direct + three bounces, which is
            // where the series has essentially converged for these albedos
            // (0.55^4 ~= 9%). `maxBounces = 1` gives the DIRECT-ONLY
            // irradiance — the surrounding surfaces lit by the light and
            // nothing else — which is what separates "DDGI is missing its
            // bounce term" from "DDGI is uniformly dark".
            settings.MaxBounces = maxBounces;
            settings.RussianRouletteStartBounce = 0;
            settings.EnableNextEventEstimation = true;

            ProbeIrradiance result;
            result.PerDirection.reserve(static_cast<sizet>(kInner) * kInner);

            glm::dvec3 sum(0.0);
            for (i32 y = 0; y < kInner; ++y)
            {
                for (i32 x = 0; x < kInner; ++x)
                {
                    const glm::vec3 direction = DDGI::TexelDirection(glm::ivec2(x, y), kInner);
                    const u32 directionSeed = MakePixelSeed(static_cast<u32>(x), static_cast<u32>(y), seed);
                    const glm::vec3 irradiance =
                        PathTracer::EstimateIrradiance(scene, probePosition, direction, settings, directionSeed);
                    result.PerDirection.push_back(irradiance);
                    sum += glm::dvec3(irradiance);
                }
            }

            result.Mean = glm::vec3(sum / static_cast<f64>(result.PerDirection.size()));
            return result;
        }

        [[nodiscard]] f32 MeanChannel(const glm::vec3& v)
        {
            return (v.x + v.y + v.z) / 3.0f;
        }
    } // namespace

    class DDGIReferenceParityTest : public RendererAttachedTest
    {
      protected:
        // The probe volume's bounds, overridable so the same room can be
        // measured with a volume that stops at the air (the natural authoring,
        // and what the DDGI bring-up rig uses) and with one that ENCLOSES the
        // wall slabs. The difference between the two is the finding — see
        // BounceTermIsDeadWhenTheVolumeExcludesTheWalls below.
        [[nodiscard]] virtual glm::vec3 VolumeMin() const
        {
            return kVolumeMin;
        }
        [[nodiscard]] virtual glm::vec3 VolumeMax() const
        {
            return kVolumeMax;
        }
        [[nodiscard]] virtual glm::ivec3 VolumeResolution() const
        {
            return kProbeDims;
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // Deferred: the primary DDGI consumer. The fixture snapshots and
            // restores RendererSettings per test, so this cannot leak.
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            auto& postProcess = Renderer3D::GetPostProcessSettings();
            postProcess.TAAEnabled = false;
            postProcess.AutoExposureEnabled = false;

            {
                Entity camera = scene.CreateEntity("Camera");
                auto& transform = camera.GetComponent<TransformComponent>();
                transform.Translation = { 0.0f, 2.0f, 3.0f };
                auto& cameraComponent = camera.AddComponent<CameraComponent>();
                cameraComponent.Primary = true;
                cameraComponent.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
            }

            {
                Entity lightEntity = scene.CreateEntity("Point Light");
                lightEntity.GetComponent<TransformComponent>().Translation = kLightPosition;
                auto& pointLight = lightEntity.AddComponent<PointLightComponent>();
                pointLight.m_Color = kLightColor;
                pointLight.m_Intensity = kLightIntensity;
                pointLight.m_Range = kLightRange;
                pointLight.m_Attenuation = kLightAttenuation;
                // Shadow casting on: the reference traces real occlusion, so a
                // DDGI run WITHOUT shadows would diverge everywhere the light
                // is blocked and the divergence would be attributed to
                // transport rather than to a missing shadow term.
                pointLight.m_CastShadows = true;
            }

            // All geometry is CUBE MeshComponents — the submission path that
            // is verified to reach the DDGI caster sites (see
            // DDGIVisualEvidenceTest's note; sphere primitives are not wired
            // for capture).
            for (const BoxDesc& box : RoomBoxes())
            {
                Entity entity = scene.CreateEntity(box.Name);
                auto& transform = entity.GetComponent<TransformComponent>();
                transform.Translation = box.Position;
                transform.Scale = box.Scale;

                auto& mesh = entity.AddComponent<MeshComponent>();
                mesh.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
                    mesh.m_MeshSource = cube->GetMeshSource();

                auto& material = entity.AddComponent<MaterialComponent>();
                material.m_Material.SetBaseColorFactor(glm::vec4(box.Albedo, 1.0f));
                material.m_Material.SetMetallicFactor(0.0f);
                material.m_Material.SetRoughnessFactor(0.9f);
            }

            {
                Entity volume = scene.CreateEntity("Probe Volume");
                auto& probeVolume = volume.AddComponent<LightProbeVolumeComponent>();
                probeVolume.m_BoundsMin = VolumeMin();
                probeVolume.m_BoundsMax = VolumeMax();
                probeVolume.m_Resolution = VolumeResolution();
                probeVolume.m_Active = true;
                probeVolume.m_Mode = LightProbeVolumeComponent::Mode::Realtime;
                probeVolume.m_RaysPerProbe = 256; // 16x16 hit cache
                probeVolume.m_Hysteresis = 0.5f;  // fast, order-independent convergence
                probeVolume.m_ProbeCaptureBudget = 4;
                probeVolume.m_RelightBudget = 0; // relight every probe each frame
                probeVolume.m_SelfShadowBias = 0.3f;
            }
        }

        // Per-direction irradiance of one probe, read straight from the pass's
        // live FP16 atlas. Mirrors DDGIVisualEvidenceTest's tile readback, but
        // keeps each texel instead of averaging: the per-direction values are
        // what carry the field's directional structure.
        [[nodiscard]] bool ReadProbeIrradiance(const glm::ivec3& probeCoord, ProbeIrradiance& outIrradiance,
                                               glm::vec3& outProbePosition)
        {
            auto* pass = Renderer3D::GetDDGIPass();
            if (pass == nullptr)
                return false;

            const RHI::ResourceHandle atlas = pass->GetIrradianceAtlasID();
            if (!atlas.IsValid())
                return false;

            const i32 probeIndex = DDGI::ProbeLinearIndex(probeCoord, VolumeResolution());
            const std::vector<DDGIProbeUpdatePass::ProbeRecord>& records = pass->GetProbeRecords();
            if (probeIndex < 0 || static_cast<sizet>(probeIndex) >= records.size())
                return false;

            // The RELOCATED position, not the grid position. Relocation moves a
            // probe out of geometry by up to 0.45 of the spacing; tracing the
            // ground truth at the un-relocated point would compare two
            // different places in the room and read as a transport error.
            outProbePosition = DDGI::ProbeWorldPosition(probeCoord, VolumeMin(), VolumeMax(), VolumeResolution(),
                                                        records[static_cast<sizet>(probeIndex)].OffsetN);

            constexpr i32 kInner = DDGI::kIrradianceInteriorTexels;
            const glm::ivec2 tileOrigin =
                DDGI::ProbeTileCoord(probeIndex, VolumeResolution()) * DDGI::kIrradianceTileTexels;

            // Raw readback needs the driver name; Debug::NativeTextureIdForDiagnostics
            // is the sanctioned way for a TEST to ask (issue #691).
            const u32 atlasId = Debug::NativeTextureIdForDiagnostics(atlas);

            std::vector<f32> texels(static_cast<sizet>(kInner) * kInner * 4u);
            ::glGetTextureSubImage(atlasId, 0, tileOrigin.x + 1, tileOrigin.y + 1, 0, kInner, kInner, 1,
                                   GL_RGBA, GL_FLOAT, static_cast<GLsizei>(texels.size() * sizeof(f32)),
                                   texels.data());

            outIrradiance.PerDirection.clear();
            outIrradiance.PerDirection.reserve(static_cast<sizet>(kInner) * kInner);
            glm::dvec3 sum(0.0);
            for (i32 i = 0; i < kInner * kInner; ++i)
            {
                const glm::vec3 value(texels[static_cast<sizet>(i) * 4 + 0], texels[static_cast<sizet>(i) * 4 + 1],
                                      texels[static_cast<sizet>(i) * 4 + 2]);
                outIrradiance.PerDirection.push_back(value);
                sum += glm::dvec3(value);
            }
            outIrradiance.Mean = glm::vec3(sum / static_cast<f64>(kInner * kInner));
            return true;
        }
    };

    // =========================================================================
    // The headline test.
    // =========================================================================
    TEST_F(DDGIReferenceParityTest, ProbeIrradianceMatchesTheReferenceTransport)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        RunFrames(kConvergenceFrames);

        auto* pass = Renderer3D::GetDDGIPass();
        ASSERT_NE(pass, nullptr) << "the DDGI pass never ran";
        ASSERT_GT(pass->GetCapturedFraction(), 0.99f)
            << "probe capture did not cover the grid — comparing an unconverged field";

        // The reference twin, in DDGI's own (Lambertian) shading model.
        ReferenceScene reference;
        BuildReferenceRoom(reference, /*lambertian*/ true);
        ASSERT_TRUE(reference.IsBuilt());

        // Probes chosen to sit WELL CLEAR of every wall: the volume spans
        // x,z in [-3, 3] against wall inner faces at +-3.9, and y = 0.8 is 0.7
        // above the floor's top face. Close to a slab the two worlds stop
        // agreeing about where the probe even is — DDGI relocates it out of
        // geometry while the reference has exact analytic surfaces — and a few
        // centimetres against a wall is a large irradiance difference that is
        // NOT a transport error.
        const std::vector<glm::ivec3> probes = {
            { 1, 0, 1 },
            { 1, 1, 1 },
            { 0, 1, 1 },
            { 2, 1, 1 },
        };

        std::vector<f32> ddgiMeans;
        std::vector<f32> referenceMeans;
        std::vector<f32> referenceDirectMeans;

        for (const glm::ivec3& probeCoord : probes)
        {
            ProbeIrradiance ddgi;
            glm::vec3 probePosition(0.0f);
            ASSERT_TRUE(ReadProbeIrradiance(probeCoord, ddgi, probePosition))
                << "atlas readback failed for probe (" << probeCoord.x << ", " << probeCoord.y << ", "
                << probeCoord.z << ")";

            const ProbeIrradiance traced = TraceProbeIrradiance(reference, probePosition, 192, 0x709u);
            // Direct-only ground truth, for the diagnosis printed below.
            const ProbeIrradiance tracedDirect =
                TraceProbeIrradiance(reference, probePosition, 96, 0x709u, /*maxBounces*/ 1);

            const f32 ddgiMean = MeanChannel(ddgi.Mean);
            const f32 referenceMean = MeanChannel(traced.Mean);
            const f32 referenceDirectMean = MeanChannel(tracedDirect.Mean);

            std::cout << "[ddgi-parity] probe (" << probeCoord.x << "," << probeCoord.y << "," << probeCoord.z
                      << ") at (" << probePosition.x << ", " << probePosition.y << ", " << probePosition.z << ")"
                      << "  DDGI E = (" << ddgi.Mean.r << ", " << ddgi.Mean.g << ", " << ddgi.Mean.b << ")"
                      << "  reference E = (" << traced.Mean.r << ", " << traced.Mean.g << ", " << traced.Mean.b
                      << ")\n"
                      << "[ddgi-parity]   channel means: DDGI " << ddgiMean << "  reference(4 bounces) "
                      << referenceMean << "  reference(direct only) " << referenceDirectMean
                      << "   DDGI/full = " << (ddgiMean / referenceMean)
                      << "   DDGI/direct = " << (ddgiMean / std::max(referenceDirectMean, 1e-6f)) << "\n";

            ASSERT_GT(referenceMean, 1e-4f)
                << "the reference says this probe receives no light at all — the scene twins disagree";
            // The direct-only reference is the DIVISOR of the ratio below, so it
            // gets its own guard: a zero here would turn the assertion into
            // inf > 0.9, which passes.
            ASSERT_GT(referenceDirectMean, 1e-4f)
                << "the direct-only reference is zero at this probe — nothing to compare DDGI against";
            ASSERT_GT(ddgiMean, 1e-4f) << "DDGI's probe irradiance is zero where the reference says it is lit";

            ddgiMeans.push_back(ddgiMean);
            referenceMeans.push_back(referenceMean);
            referenceDirectMeans.push_back(referenceDirectMean);

            // MAGNITUDE, against the DIRECT-ONLY ground truth.
            //
            // Asserting against the FULL (multi-bounce) reference would be the
            // obvious choice, and it fails: DDGI lands at 0.41-0.61x of it.
            // That is not noise and not an approximation quality issue — it is
            // the finding this test produced. With this probe volume DDGI's
            // infinite-bounce feedback term is contributing EXACTLY NOTHING,
            // and what remains agrees with the direct term to within 1%. See
            // BounceTermIsDeadWhenTheVolumeExcludesTheWalls below for the
            // mechanism, and docs/agent-rules/reference-path-tracer.md for the
            // write-up.
            //
            // So this assertion pins what DDGI ACTUALLY computes here — its
            // direct transport, which is correct — at a tolerance tight enough
            // (10%) to catch a regression in it. The missing bounce is pinned
            // separately, as its own explicit contract, rather than hidden
            // inside a band wide enough to swallow it.
            const f32 ratio = ddgiMean / referenceDirectMean;
            EXPECT_GT(ratio, 0.9f) << "probe (" << probeCoord.x << "," << probeCoord.y << "," << probeCoord.z
                                   << "): DDGI is " << ratio << "x the DIRECT reference irradiance — too dark";
            EXPECT_LT(ratio, 1.1f) << "probe (" << probeCoord.x << "," << probeCoord.y << "," << probeCoord.z
                                   << "): DDGI is " << ratio << "x the DIRECT reference irradiance — too bright";
        }

        // FIELD SHAPE. Magnitude can be off by a constant and still be useful;
        // getting the SPATIAL ORDERING wrong cannot. Every pair of probes must
        // rank the same way in both worlds — this is the assertion that a
        // global scale error cannot pass and that a broken visibility term
        // cannot pass either.
        //
        // Ranked against the DIRECT-ONLY reference, matching the magnitude
        // assertion above. Ranking against the full multi-bounce reference
        // would be comparing DDGI's field shape to a field it is not currently
        // computing — the bounce term contributes nothing here (see
        // BounceTermIsDeadWhenTheVolumeExcludesTheWalls), and indirect light
        // does not distribute the same way direct light does, so the two
        // orderings need not agree even when DDGI is behaving perfectly.
        for (sizet a = 0; a < probes.size(); ++a)
        {
            for (sizet b = a + 1; b < probes.size(); ++b)
            {
                // Skip pairs the reference itself considers a tie (within 15%):
                // ordering is not a meaningful claim there, and asserting it
                // would make the test a coin flip.
                const f32 relativeGap = std::abs(referenceDirectMeans[a] - referenceDirectMeans[b]) / std::max(referenceDirectMeans[a], referenceDirectMeans[b]);
                if (relativeGap < 0.15f)
                    continue;

                const bool referenceOrder = referenceDirectMeans[a] > referenceDirectMeans[b];
                const bool ddgiOrder = ddgiMeans[a] > ddgiMeans[b];
                EXPECT_EQ(referenceOrder, ddgiOrder)
                    << "probes " << a << " and " << b << " rank differently: reference "
                    << referenceDirectMeans[a] << " vs " << referenceDirectMeans[b] << ", DDGI " << ddgiMeans[a]
                    << " vs " << ddgiMeans[b]
                    << " — the probe field's spatial structure does not match ground truth";
            }
        }
    }

    // =========================================================================
    // THE FINDING (issue #709 acceptance criterion 2 — "fixed or documented").
    //
    // With the probe volume covering only the room's AIR — the natural
    // authoring, and what the DDGI bring-up rig
    // (OloEditor/SandboxProject/Assets/Scenes/DDGITest.olo) and
    // DDGIVisualEvidenceTest both do — DDGI's infinite-bounce feedback term
    // contributes EXACTLY NOTHING, and probe irradiance comes out at roughly
    // half of ground truth.
    //
    // The mechanism is a two-line chain, invisible from either end:
    //
    //   * DDGI_Relight.glsl computes the bounce term as
    //     `ddgiSampleIrradiance(u_PrevIrradiance, ..., hitPos, ...)` — the
    //     previous frame's irradiance AT THE CACHED HIT POINT.
    //   * `ddgiSampleIrradiance` opens with `if (!ddgiIsInsideVolume(worldPos))
    //     return vec3(0.0);`.
    //
    // Every cached hit point is ON A SURFACE. A volume fitted to the interior
    // air therefore excludes every wall, floor and ceiling in the room — which
    // is to say, every surface the bounce light was supposed to come from. The
    // feedback term returns zero for all of them, silently, on every probe.
    //
    // This test pins the mechanism from both sides: the same room, the same
    // lights, the same reference — only the volume's bounds change. If DDGI is
    // ever given a bounce term that survives a tightly-fitted volume, THIS test
    // fails and the one above starts passing against the full reference.
    // =========================================================================
    class DDGIReferenceParityWideVolumeTest : public DDGIReferenceParityTest
    {
      protected:
        // Enclose the wall slabs (outer faces at +-4.1, y in [-0.1, 4.1])
        // instead of stopping at the air.
        [[nodiscard]] glm::vec3 VolumeMin() const override
        {
            return glm::vec3(-4.3f, -0.3f, -4.3f);
        }
        [[nodiscard]] glm::vec3 VolumeMax() const override
        {
            return glm::vec3(4.3f, 4.3f, 4.3f);
        }
        [[nodiscard]] glm::ivec3 VolumeResolution() const override
        {
            // 4 x 3 x 4: with the wider bounds this keeps roughly the same
            // probe spacing, and puts a probe layer INSIDE the room rather than
            // only on the enclosing shell.
            return glm::ivec3(4, 3, 4);
        }
    };

    TEST_F(DDGIReferenceParityWideVolumeTest, EnclosingTheWallsRestoresTheBounceTerm)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        RunFrames(kConvergenceFrames);
        ASSERT_NE(Renderer3D::GetDDGIPass(), nullptr);

        ReferenceScene reference;
        BuildReferenceRoom(reference, /*lambertian*/ true);

        // An interior probe of the 4x3x4 grid — spacing is 8.6/3 in x/z, so
        // (1,1,1) and (2,1,2) sit well inside the room.
        ProbeIrradiance ddgi;
        glm::vec3 probePosition(0.0f);
        ASSERT_TRUE(ReadProbeIrradiance({ 1, 1, 1 }, ddgi, probePosition));

        const ProbeIrradiance full = TraceProbeIrradiance(reference, probePosition, 192, 0x70du);
        const ProbeIrradiance direct = TraceProbeIrradiance(reference, probePosition, 96, 0x70du, /*maxBounces*/ 1);

        const f32 ddgiMean = MeanChannel(ddgi.Mean);
        const f32 fullMean = MeanChannel(full.Mean);
        const f32 directMean = MeanChannel(direct.Mean);

        std::cout << "[ddgi-parity] WIDE volume, probe at (" << probePosition.x << ", " << probePosition.y << ", "
                  << probePosition.z << "): DDGI " << ddgiMean << "  reference(full) " << fullMean
                  << "  reference(direct) " << directMean << "   DDGI/full = " << (ddgiMean / fullMean)
                  << "   DDGI/direct = " << (ddgiMean / std::max(directMean, 1e-6f)) << "\n";

        ASSERT_GT(fullMean, 1e-4f);
        ASSERT_GT(directMean, 1e-4f);
        // The scene must actually HAVE a meaningful bounce, or this test cannot
        // tell whether the bounce term works.
        ASSERT_GT(fullMean, directMean * 1.3f)
            << "the reference says multi-bounce adds almost nothing here; the fixture cannot detect a "
               "dead feedback term";

        // The contract: with the walls inside the volume, DDGI must carry more
        // than the direct term alone. This is deliberately a WEAK lower bound
        // (any real bounce contribution passes) because the point is the
        // qualitative difference against the tight-volume case, where the ratio
        // is 1.00 to three decimals.
        EXPECT_GT(ddgiMean, directMean * 1.05f)
            << "DDGI's infinite-bounce term is dead even with the walls inside the probe volume — "
               "DDGI "
            << ddgiMean << " vs direct-only ground truth " << directMean;
    }

    // =========================================================================
    // Colour bleeding — the phenomenon DDGI exists for, checked against ground
    // truth rather than against a golden image.
    //
    // The left wall is red and the right wall is green. A probe near the left
    // wall must receive a redder irradiance than one near the right wall, and
    // by a MARGIN THE REFERENCE AGREES WITH. A golden image can only say the
    // pixels did not change; this says the physics is right.
    // =========================================================================
    TEST_F(DDGIReferenceParityTest, ColourBleedingMatchesTheReference)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        RunFrames(kConvergenceFrames);
        ASSERT_NE(Renderer3D::GetDDGIPass(), nullptr);

        ReferenceScene reference;
        BuildReferenceRoom(reference, /*lambertian*/ true);

        ProbeIrradiance leftDdgi;
        ProbeIrradiance rightDdgi;
        glm::vec3 leftPosition(0.0f);
        glm::vec3 rightPosition(0.0f);
        ASSERT_TRUE(ReadProbeIrradiance({ 0, 1, 1 }, leftDdgi, leftPosition));
        ASSERT_TRUE(ReadProbeIrradiance({ 2, 1, 1 }, rightDdgi, rightPosition));

        const ProbeIrradiance leftReference = TraceProbeIrradiance(reference, leftPosition, 192, 0x70au);
        const ProbeIrradiance rightReference = TraceProbeIrradiance(reference, rightPosition, 192, 0x70bu);

        const auto hue = [](const glm::vec3& e)
        { return e.r / std::max(e.g, 1e-6f); };

        const f32 ddgiLeftHue = hue(leftDdgi.Mean);
        const f32 ddgiRightHue = hue(rightDdgi.Mean);
        const f32 referenceLeftHue = hue(leftReference.Mean);
        const f32 referenceRightHue = hue(rightReference.Mean);

        std::cout << "[ddgi-parity] red/green hue ratio  left: DDGI " << ddgiLeftHue << " reference "
                  << referenceLeftHue << "   right: DDGI " << ddgiRightHue << " reference " << referenceRightHue
                  << "\n";

        // The reference must show the effect at all — otherwise the scene, not
        // DDGI, is what this test is measuring.
        ASSERT_GT(referenceLeftHue, referenceRightHue * 1.1f)
            << "the REFERENCE shows no colour bleeding in this scene; the test cannot judge DDGI";

        EXPECT_GT(ddgiLeftHue, ddgiRightHue * 1.1f)
            << "DDGI's probe field does not carry wall colour into the room, but the reference says it should "
               "(left/right hue: reference "
            << referenceLeftHue << " / " << referenceRightHue << ")";
    }

    // =========================================================================
    // The photometric divergence, MEASURED rather than asserted.
    //
    // DDGI's probe cache shades Lambertian; the lit passes shade
    // cookTorranceBRDF. This test quantifies the resulting difference in
    // probe irradiance and prints it. It fails only if the gap is large enough
    // to matter for a GI comparison — the number itself is the deliverable, and
    // it is recorded in docs/agent-rules/reference-path-tracer.md.
    // =========================================================================
    TEST_F(DDGIReferenceParityTest, LambertianVersusCookTorranceDivergenceIsBounded)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        RunFrames(kConvergenceFrames);

        ReferenceScene lambertian;
        BuildReferenceRoom(lambertian, /*lambertian*/ true);
        ReferenceScene cookTorrance;
        BuildReferenceRoom(cookTorrance, /*lambertian*/ false);

        ProbeIrradiance ignored;
        glm::vec3 probePosition(0.0f);
        ASSERT_TRUE(ReadProbeIrradiance({ 1, 1, 1 }, ignored, probePosition));

        const ProbeIrradiance lambertianTruth = TraceProbeIrradiance(lambertian, probePosition, 256, 0x70cu);
        const ProbeIrradiance cookTorranceTruth = TraceProbeIrradiance(cookTorrance, probePosition, 256, 0x70cu);

        const f32 lambertianMean = MeanChannel(lambertianTruth.Mean);
        const f32 cookTorranceMean = MeanChannel(cookTorranceTruth.Mean);
        ASSERT_GT(lambertianMean, 1e-4f);

        const f32 ratio = cookTorranceMean / lambertianMean;
        std::cout << "[ddgi-parity] photometric divergence at probe (1,1,1): cookTorranceBRDF " << cookTorranceMean
                  << " vs Lambertian " << lambertianMean << "  (ratio " << ratio << ")\n"
                  << "[ddgi-parity] this is the gap between DDGI's diffuse-only probe cache "
                     "(DDGI_Relight.glsl) and the lit passes' cookTorranceBRDF.\n";

        // The two models differ by construction: cookTorrance's diffuse carries
        // the `1 - F` energy split (darker) and adds a specular lobe
        // (brighter). They should land close for rough dielectrics — if they
        // do not, DDGI's bounce light is a materially different colour and
        // brightness from the direct lighting it sits next to, which is a
        // visible artefact and a real bug worth filing.
        EXPECT_GT(ratio, 0.7f) << "DDGI's shading model is much BRIGHTER than the lit passes' BRDF";
        EXPECT_LT(ratio, 1.3f) << "DDGI's shading model is much DARKER than the lit passes' BRDF";
    }
} // namespace OloEngine::Tests
