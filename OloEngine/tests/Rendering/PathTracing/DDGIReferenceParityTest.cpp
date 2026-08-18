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
#include <limits>
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

        // Largest channel anywhere in the irradiance atlas. A probe-by-probe
        // convergence check can only speak for the probes it samples; a
        // feedback loop that goes unstable does so somewhere, not everywhere,
        // so the stability contract is stated over the WHOLE field.
        [[nodiscard]] bool ReadAtlasPeak(f32& outPeak, bool& outAllFinite)
        {
            auto* pass = Renderer3D::GetDDGIPass();
            if (pass == nullptr)
                return false;
            const RHI::ResourceHandle atlas = pass->GetIrradianceAtlasID();
            if (!atlas.IsValid())
                return false;

            const glm::ivec3 dims = VolumeResolution();
            const glm::ivec2 tiles = DDGI::AtlasTileDimensions(dims);
            const i32 width = tiles.x * DDGI::kIrradianceTileTexels;
            const i32 height = tiles.y * DDGI::kIrradianceTileTexels;

            // Poisoned, and the GL error state drained first: a
            // glGetTextureSubImage that does nothing would otherwise leave a
            // zero-filled buffer, which reads as peak 0 / all-finite / success
            // — and a peak of 0 makes the growth ratio 0, so every divergence
            // assertion below would pass on a readback that never happened.
            while (::glGetError() != GL_NO_ERROR)
            {
            }
            std::vector<f32> texels(static_cast<sizet>(width) * static_cast<sizet>(height) * 4u,
                                    std::numeric_limits<f32>::quiet_NaN());
            ::glGetTextureSubImage(Debug::NativeTextureIdForDiagnostics(atlas), 0, 0, 0, 0, width, height, 1,
                                   GL_RGBA, GL_FLOAT, static_cast<GLsizei>(texels.size() * sizeof(f32)),
                                   texels.data());
            if (::glGetError() != GL_NO_ERROR)
            {
                return false;
            }

            outPeak = 0.0f;
            outAllFinite = true;
            for (sizet i = 0; i < texels.size(); i += 4)
            {
                for (sizet c = 0; c < 3; ++c)
                {
                    const f32 v = texels[i + c];
                    if (!std::isfinite(v))
                        outAllFinite = false;
                    else
                        outPeak = std::max(outPeak, v);
                }
            }
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

        // The pass's own #751 diagnostic, printed beside the probe table so
        // the evidence is self-describing: the mean fraction of the bounce
        // term this volume's bounds let through. It read 0 before the fix (an
        // air-fitted volume contains none of its own hit points) and is what
        // the editor inspector warns on.
        std::cout << "[ddgi-parity] bounce coverage (air-fitted volume): " << pass->GetBounceCoverage() << "\n";

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

            // MAGNITUDE, against the FULL (multi-bounce) ground truth.
            //
            // This assertion used to be made against the DIRECT-ONLY reference
            // at a 10% tolerance, because direct transport was all DDGI
            // computed: with a volume fitted to the room's air its
            // infinite-bounce term contributed EXACTLY NOTHING (issue #751),
            // and it landed at 0.41-0.61x of the full reference. Now that the
            // bounce path gathers with a one-spacing margin, the full
            // reference is the right target, and the direct-only one becomes
            // the FLOOR that proves the bounce is alive.
            //
            // The band is wider than the old direct-only one on purpose:
            // multi-bounce is where a real-time probe field spends its
            // approximation budget (6x6 octahedral irradiance, FP16 atlases,
            // 256 fixed capture directions, Chebyshev visibility, and a
            // constant extrapolation faded out across the boundary margin),
            // whereas direct transport is nearly exact. The wall-enclosing
            // fixture below — which has always had a live bounce term —
            // measured 1.12x, which is the honest scale of that budget.
            const f32 ratio = ddgiMean / referenceMean;
            EXPECT_GT(ratio, 0.75f) << "probe (" << probeCoord.x << "," << probeCoord.y << "," << probeCoord.z
                                    << "): DDGI is " << ratio << "x the FULL reference irradiance — too dark";
            EXPECT_LT(ratio, 1.25f) << "probe (" << probeCoord.x << "," << probeCoord.y << "," << probeCoord.z
                                    << "): DDGI is " << ratio << "x the FULL reference irradiance — too bright";

            // THE #751 ANTI-REGRESSION, stated as its own contract rather than
            // left to the band above: the bounce term must carry real energy
            // on an AIR-FITTED volume. Every cached hit point is on a surface,
            // so every one of them sits OUTSIDE this volume; if the gather
            // ever goes back to refusing them, this ratio snaps to 1.00 (it
            // measured 0.93-1.005 across these four probes) while the
            // magnitude band above merely goes soft.
            const f32 bounceRatio = ddgiMean / referenceDirectMean;
            EXPECT_GT(bounceRatio, 1.15f)
                << "probe (" << probeCoord.x << "," << probeCoord.y << "," << probeCoord.z << "): DDGI is "
                << bounceRatio
                << "x the DIRECT-ONLY reference — the infinite-bounce term is carrying little or nothing. "
                   "That is issue #751: every cached hit point is on a surface, so a volume fitted to the "
                   "room's air excludes all of them.";
        }

        // FIELD SHAPE. Magnitude can be off by a constant and still be useful;
        // getting the SPATIAL ORDERING wrong cannot. Every pair of probes must
        // rank the same way in both worlds — this is the assertion that a
        // global scale error cannot pass and that a broken visibility term
        // cannot pass either.
        //
        // Ranked against the FULL reference, matching the magnitude assertion
        // above. Before #751 this had to rank against the direct-only
        // reference: the bounce term contributed nothing, and indirect light
        // does not distribute the way direct light does, so comparing DDGI's
        // field shape against a field it was not computing would have been a
        // coin flip. Now that it computes multi-bounce, the multi-bounce field
        // is the one whose shape it has to reproduce.
        for (sizet a = 0; a < probes.size(); ++a)
        {
            for (sizet b = a + 1; b < probes.size(); ++b)
            {
                // Skip pairs the reference itself considers a tie (within 15%):
                // ordering is not a meaningful claim there, and asserting it
                // would make the test a coin flip.
                const f32 relativeGap = std::abs(referenceMeans[a] - referenceMeans[b]) / std::max(referenceMeans[a], referenceMeans[b]);
                if (relativeGap < 0.15f)
                    continue;

                const bool referenceOrder = referenceMeans[a] > referenceMeans[b];
                const bool ddgiOrder = ddgiMeans[a] > ddgiMeans[b];
                EXPECT_EQ(referenceOrder, ddgiOrder)
                    << "probes " << a << " and " << b << " rank differently: reference " << referenceMeans[a]
                    << " vs " << referenceMeans[b] << ", DDGI " << ddgiMeans[a] << " vs " << ddgiMeans[b]
                    << " — the probe field's spatial structure does not match ground truth";
            }
        }
    }

    // =========================================================================
    // THE OTHER SIDE OF THE #751 PIN.
    //
    // The finding this fixture was built for (issue #709 acceptance criterion
    // 2, filed as #751): with the probe volume covering only the room's AIR —
    // the natural authoring, and what the DDGI bring-up rig
    // (OloEditor/SandboxProject/Assets/Scenes/DDGITest.olo) and
    // DDGIVisualEvidenceTest both do — DDGI's infinite-bounce feedback term
    // contributed EXACTLY NOTHING, and probe irradiance came out at roughly
    // half of ground truth (0.41-0.61x). The mechanism was a two-line chain,
    // invisible from either end:
    //
    //   * DDGI_Relight.glsl computes the bounce term as the previous frame's
    //     irradiance AT THE CACHED HIT POINT.
    //   * the gather opened with `if (!ddgiIsInsideVolume(worldPos))
    //     return vec3(0.0);`.
    //
    // Every cached hit point is ON A SURFACE, so a volume fitted to the
    // interior air excluded every wall, floor and ceiling — every surface the
    // bounce light was supposed to come from — on every probe, silently.
    //
    // This fixture is the CONTROL that made the diagnosis unambiguous: the
    // same room, the same lights, the same reference, only the bounds change
    // so the wall slabs fall inside. Its bounce term was alive throughout
    // (DDGI/direct 1.74, DDGI/full 1.12), which is what proved the machinery
    // worked and was only ever being handed hit points it refused to answer
    // for.
    //
    // It stays, and it is still load-bearing. The fix widened the bounce
    // gather by one probe spacing; this fixture is the case that needs NO
    // margin at all, so it pins that the change did not disturb a volume that
    // was already authored correctly — the two authorings must now agree with
    // ground truth, not just with each other.
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

    TEST_F(DDGIReferenceParityWideVolumeTest, EnclosingTheWallsAlsoMatchesTheReference)
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

        // The bounce term is alive — the assertion this fixture has always
        // made, kept as the floor.
        EXPECT_GT(ddgiMean, directMean * 1.05f)
            << "DDGI's infinite-bounce term is dead even with the walls inside the probe volume — "
               "DDGI "
            << ddgiMean << " vs direct-only ground truth " << directMean;

        // ...and it lands in the SAME parity band as the air-fitted fixture.
        // Before #751 only this fixture could make that claim; now both do,
        // which is the real deliverable — the two authorings converge on the
        // same physics instead of differing by a factor of two.
        const f32 ratio = ddgiMean / fullMean;
        EXPECT_GT(ratio, 0.75f) << "DDGI is " << ratio << "x the FULL reference irradiance — too dark";
        EXPECT_LT(ratio, 1.25f) << "DDGI is " << ratio << "x the FULL reference irradiance — too bright";

        // A volume that already encloses its geometry must need NO margin: the
        // pass's own diagnostic has to read essentially 1.0 here, which is what
        // makes it a usable authoring signal rather than a number that is
        // always a bit under 1.
        auto* pass = Renderer3D::GetDDGIPass();
        ASSERT_NE(pass, nullptr);
        const f32 coverage = pass->GetBounceCoverage();
        std::cout << "[ddgi-parity] bounce coverage (wall-enclosing volume): " << coverage << "\n";
        EXPECT_GT(coverage, 0.95f)
            << "a volume that encloses its own walls reports only " << coverage
            << " bounce coverage — the diagnostic is measuring something other than the authoring";
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

    // =========================================================================
    // The stability half of the #751 fix, and the reason it needed a design
    // decision rather than a one-line patch.
    //
    // `ddgiIsInsideVolume` was not only a correctness guard: by zeroing the
    // bounce term it also held the feedback loop's gain at ZERO, which is
    // trivially contractive. Letting the bounce through restores the loop, and
    // a loop that has just been switched on for the first time in the common
    // authoring case is exactly where a slow divergence or a limit cycle would
    // hide — a failure that takes hundreds of frames to show and that a single
    // brighter-looking screenshot cannot rule out.
    //
    // The analytic argument (ADR 0007):
    //
    //   L_hit  = min(albedo, c)/PI * (directE + bounceE)          [relight]
    //   E_next = PI * sum(w L) / sum(w)                           [blend]
    //   bounceE(x) = sum(W_i E_i) / sum(W_i), scaled by a volume weight <= 1
    //
    // Both the blend's ratio estimator and the gather's weight normalization
    // are convex averages, so in the sup norm ||E_next|| <= c * (||direct|| +
    // ||E||): the map is a contraction with Lipschitz constant exactly c =
    // u_DDGIEnergyConservation = 0.9, INDEPENDENT of the margin. (This is also
    // why the gather passes intensity 1.0 on the bounce path — an authored
    // gain would multiply that constant and an intensity above ~1.11 would
    // turn the contraction into a divergence.) The EMA on top is a convex
    // combination with the previous state, which cannot increase it.
    //
    // This test is the measurement behind that argument.
    // =========================================================================
    TEST_F(DDGIReferenceParityTest, InfiniteBounceFeedbackConverges)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Well past the 60 frames the parity measurements use: at hysteresis
        // 0.5 the EMA settles in ~10, so 300 frames is ~30 time constants and
        // ~300 round trips of the feedback loop. A creep of even 0.1% per
        // frame would compound to +35% over this window.
        constexpr u32 kWarmupFrames = kConvergenceFrames;
        constexpr u32 kSampleStride = 20;
        constexpr u32 kSampleCount = 15;

        RunFrames(kWarmupFrames);

        auto* pass = Renderer3D::GetDDGIPass();
        ASSERT_NE(pass, nullptr) << "the DDGI pass never ran";
        ASSERT_GT(pass->GetCapturedFraction(), 0.99f);
        ASSERT_GT(pass->GetBounceCoverage(), 0.0f)
            << "the bounce term is dead on this volume, so there is no feedback loop to test — see #751";

        constexpr glm::ivec3 kProbe{ 1, 1, 1 };
        std::vector<f32> probeSeries;
        std::vector<f32> peakSeries;
        probeSeries.reserve(kSampleCount);
        peakSeries.reserve(kSampleCount);

        glm::vec3 probePosition(0.0f);
        for (u32 sample = 0; sample < kSampleCount; ++sample)
        {
            RunFrames(kSampleStride);

            ProbeIrradiance probe;
            ASSERT_TRUE(ReadProbeIrradiance(kProbe, probe, probePosition));
            probeSeries.push_back(MeanChannel(probe.Mean));

            f32 peak = 0.0f;
            bool allFinite = true;
            ASSERT_TRUE(ReadAtlasPeak(peak, allFinite));
            ASSERT_TRUE(allFinite) << "the irradiance atlas contains a non-finite texel after "
                                   << (kWarmupFrames + (sample + 1) * kSampleStride)
                                   << " frames — the feedback loop diverged";
            // A lit room's atlas has to have SOME energy in it. This is what
            // makes the growth ratio below a real assertion rather than 0/0.
            ASSERT_GT(peak, 0.0f) << "the whole irradiance atlas is zero — either the readback did nothing "
                                     "or the probe field is dead";
            peakSeries.push_back(peak);
        }

        std::cout << "[ddgi-stability] frame / probe(1,1,1) mean E / whole-atlas peak\n";
        for (u32 sample = 0; sample < kSampleCount; ++sample)
        {
            std::cout << "[ddgi-stability]   " << (kWarmupFrames + (sample + 1) * kSampleStride) << "  "
                      << probeSeries[sample] << "  " << peakSeries[sample] << "\n";
        }

        // 1. It settled. The last step of 20 frames must move the probe by
        //    essentially nothing.
        ASSERT_GE(probeSeries.size(), 2u);
        const f32 finalValue = probeSeries.back();
        ASSERT_GT(finalValue, 1e-4f) << "the probe went dark";
        const f32 lastStep = std::abs(probeSeries.back() - probeSeries[probeSeries.size() - 2]) / finalValue;
        EXPECT_LT(lastStep, 0.005f) << "probe irradiance is still moving " << (lastStep * 100.0f)
                                    << "% per 20 frames after " << (kWarmupFrames + kSampleCount * kSampleStride)
                                    << " frames — the feedback loop has not settled";

        // 2. It is not creeping or oscillating. Over the whole measured window
        //    — not just the tail — the spread must be small. A creep shows up
        //    here as a large spread with a settled last step; a limit cycle
        //    shows up as a large spread with a small one.
        const auto [minIt, maxIt] = std::minmax_element(probeSeries.begin(), probeSeries.end());
        const f32 spread = (*maxIt - *minIt) / finalValue;
        EXPECT_LT(spread, 0.02f) << "probe irradiance varies by " << (spread * 100.0f)
                                 << "% across the measured window (min " << *minIt << ", max " << *maxIt
                                 << ") — the feedback loop is creeping or oscillating rather than converging";

        // 3. The whole FIELD is bounded, not just the sampled probe. The peak
        //    texel anywhere in the atlas must not grow across the window: that
        //    is what a local instability (one probe pumping its own light back
        //    into itself) looks like, and probe (1,1,1) would never show it.
        const f32 peakGrowth = peakSeries.back() / std::max(peakSeries.front(), 1e-6f);
        EXPECT_LT(peakGrowth, 1.02f) << "the atlas peak grew " << peakGrowth << "x over "
                                     << (kSampleCount * kSampleStride)
                                     << " frames — some probe's feedback is diverging";

        // 4. The fixed point is where ground truth says it should be. A
        //    contraction to the WRONG value is still a contraction, and — the
        //    sharper risk for a test like this — a feedback loop that stopped
        //    running entirely would also read as perfectly stable. So the
        //    settled state is checked against the path tracer from BOTH sides,
        //    including a floor over the direct-only reference: a frozen or
        //    bounce-less atlas fails that even though every assertion above
        //    would pass.
        ReferenceScene reference;
        BuildReferenceRoom(reference, /*lambertian*/ true);
        const ProbeIrradiance traced = TraceProbeIrradiance(reference, probePosition, 192, 0x751u);
        const ProbeIrradiance tracedDirect =
            TraceProbeIrradiance(reference, probePosition, 96, 0x751u, /*maxBounces*/ 1);
        const f32 referenceMean = MeanChannel(traced.Mean);
        const f32 referenceDirectMean = MeanChannel(tracedDirect.Mean);
        ASSERT_GT(referenceMean, 1e-4f);
        ASSERT_GT(referenceDirectMean, 1e-4f);
        const f32 settledRatio = finalValue / referenceMean;
        std::cout << "[ddgi-stability] settled DDGI " << finalValue << " vs reference(4 bounces) " << referenceMean
                  << "  (ratio " << settledRatio << ", vs direct-only " << (finalValue / referenceDirectMean)
                  << ")\n";
        EXPECT_LT(settledRatio, 1.25f) << "the loop settled, but " << settledRatio
                                       << "x above ground truth — a stable over-brightening is still a bug";
        EXPECT_GT(settledRatio, 0.7f) << "the loop settled at " << settledRatio
                                      << "x ground truth — stable, and much too dark";
        EXPECT_GT(finalValue, referenceDirectMean * 1.15f)
            << "the settled field carries no multi-bounce energy (" << (finalValue / referenceDirectMean)
            << "x the direct-only reference) — the loop is stable because it is dead, which is exactly the "
               "#751 failure this test exists to detect";
    }
} // namespace OloEngine::Tests
