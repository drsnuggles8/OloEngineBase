// OLO_TEST_LAYER: L8
// =============================================================================
// TemporalLongSequenceEvidenceTest.cpp — long-running temporal feedback,
// secondary-visibility perturbations and the reset policy (issue #1348).
//
// THE PROTOCOL. docs/agent-rules/temporal-long-sequence-protocol.md is the
// rule; this file is its executable half on OpenGL. Every claim here comes from
// a PAIRED REPLAY: two or more runs of one sequence started from the same
// sampling context (Renderer3D::ResetFrameSequences with one seed), which
// therefore share every stochastic sample frame by frame and differ only in
// what the arm changes. That has three consequences the file relies on:
//
//   * The replay contract is itself under test. Two arms must be BIT-IDENTICAL
//     until the frame they diverge on. A hidden piece of state the sampling
//     context does not cover (a history the reset missed, a pass-local frame
//     counter, a cache) shows up as a difference before the perturbation.
//   * Responses carry no noise floor. Ghosting is measured against the arm
//     that was in the new state all along, at the same frame, so a resolve
//     with no history reads exactly 0 and whatever it reads above 0 is history.
//   * Independent runs are SEEDS, not frames. Each statistic is summarised per
//     run and the interval is a Student-t interval over runs
//     (Oracle::MeanOverIndependentRuns). Frames inside a run are correlated
//     and are never counted as samples.
//
// WHAT IS MEASURED, SEPARATELY:
//   raw signal      SSRSignal / SSGISignal, or TAA's input (the feedback-0 arm)
//   reconstruction  SSRResolved / SSGIResolved / TAAColor
//   bias            time-mean of the reconstruction minus the raw signal, as a
//                   fraction of the signal, on a still sequence. The raw signal
//                   is the reconstruction's unbiased reference: these resolves
//                   CLIP their history (a biased mode), so the bias is reported
//                   per estimator and never pooled with an unclamped claim.
//   drift           the bias of the second half of a long still run minus the
//                   first half's: feedback that drifts moves it.
//   variance        the temporal variance the reconstruction keeps, per pixel,
//                   over the raw signal's.
//   response        frames until a perturbation's paired residual falls to 10%,
//                   against the slowest an unclipped exponential history could
//                   take, ceil(ln 0.1 / ln feedback).
//   ghosting        the same residual after the perturbation is undone.
//
// PERTURBATIONS ARE SECONDARY. The camera and the receiver never move: a lamp
// (an emissive block the floor reflects and the wall receives bounce from)
// switches off and back on, or an occluder outside the frustum moves into the
// key light's path and back. Primary receiver validity cannot see either, which
// is the point (#1348): every history here stays valid through the change, and
// the registry's lineage AOV is asserted to say so.
//
// THE RESET POLICY. Each event the renderer must answer with a new lineage (a
// camera cut, a projection change, a render-scale change, a sampling-sequence
// restart) or must ride through (a pause, a shader-library reload, a floating-
// origin rebase) is checked on the frame it happens, with a model-free test:
// on a frame with no history, a resolve at the shipped feedback and the same
// resolve at feedback 0 are the same image, because both blend the current
// frame with nothing. On the frame before, they must differ, or the check is
// vacuous. The stale-history negative control (Levers::
// FaultKeepStaleTemporalHistory) re-runs the reset cases with every
// invalidation dropped and must see them fail.
//
// Runs in the normal suite; SKIPs without a GL 4.6 context. The Vulkan half of
// the matrix (RT shadows, ReSTIR DI/GI/PT, and the Vulkan cells of TAA, SSR and
// SSGI) is live-only and recorded in the PR.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "Rendering/Oracles/OracleStatistics.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/TemporalHistoryRegistry.h"
#include "OloEngine/Renderer/TemporalSequenceMetrics.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        using Oracle::MeanOverIndependentRuns;
        using Oracle::RunInterval;
        using Oracle::SampleProvenance;
        using TemporalSequenceMetrics::MeasurePairedResponse;
        using TemporalSequenceMetrics::PairedResponseResult;

        constexpr u32 kWidth = 320;
        constexpr u32 kHeight = 240;
        // A frozen clock: nothing in the scene animates, and every renderer-side
        // accumulator reads a zero dt, so the only thing that moves a frame is
        // the sampling sequence and the arm's own perturbation.
        constexpr f32 kMockTime = 3.0f;

        // Three independent runs per claim (the issue asks for at least two).
        constexpr std::array<u32, 3> kSeeds{ 0u, 1u, 2u };

        // The schedule of one long sequence. The still window [kStep, kOldEnd)
        // is 192 frames: nineteen time constants of TAA's 0.9 feedback, which
        // is long enough for a drifting history to show as a trend between its
        // two halves.
        constexpr u32 kRecordFrom = 48;
        constexpr u32 kStep = 64;
        constexpr u32 kRestore = 112;
        constexpr u32 kStepEnd = 160;
        constexpr u32 kOldEnd = 256;
        constexpr u32 kDriftSplit = (kStep + kOldEnd) / 2u;

        // The settled fraction a response is measured to, and the residual a
        // restored sequence may still carry after (kStepEnd - kRestore) frames.
        constexpr f64 kSettledFraction = 0.1;
        constexpr f64 kRestoredResidual = 0.02;
        // Drift and bias are judged against the signal's own size.
        constexpr f64 kDriftFloor = 0.005;

        enum class Estimator : u8
        {
            TAA,
            SSR,
            SSGI,
        };

        enum class Perturbation : u8
        {
            SecondaryLight,
            SecondaryOccluder,
        };

        [[nodiscard]] const char* EstimatorName(Estimator e)
        {
            switch (e)
            {
                case Estimator::TAA:
                    return "TAA";
                case Estimator::SSR:
                    return "SSR";
                case Estimator::SSGI:
                    return "SSGI";
            }
            return "?";
        }

        [[nodiscard]] const char* PerturbationName(Perturbation p)
        {
            return p == Perturbation::SecondaryLight ? "Light" : "Occluder";
        }

        [[nodiscard]] const char* PathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
            }
            return "Unknown";
        }

        [[nodiscard]] TemporalHistoryEffect EffectOf(Estimator e)
        {
            switch (e)
            {
                case Estimator::TAA:
                    return TemporalHistoryEffect::TAA;
                case Estimator::SSR:
                    return TemporalHistoryEffect::SSR;
                case Estimator::SSGI:
                    return TemporalHistoryEffect::SSGI;
            }
            return TemporalHistoryEffect::TAA;
        }

        // The shipped feedback of each resolve, and therefore the slowest a
        // history can let go of a changed answer: an unclipped exponential at
        // this feedback reaches 10% after ceil(ln 0.1 / ln a) frames. The
        // neighbourhood clip can only make it faster.
        [[nodiscard]] f32 ShippedFeedback(Estimator e)
        {
            return e == Estimator::TAA ? 0.9f : 0.92f;
        }

        [[nodiscard]] u32 ExponentialResponseBound(f32 feedback)
        {
            return static_cast<u32>(std::ceil(std::log(kSettledFraction) / std::log(static_cast<f64>(feedback))));
        }

        // Rec. 709 luminance of an RGBA float field: the scalar every metric
        // below judges. A resolve's history is per channel, but the questions
        // (did it respond, did it drift) are about one quantity over time.
        [[nodiscard]] std::vector<f32> LumaOf(const std::vector<f32>& rgba)
        {
            std::vector<f32> luma(rgba.size() / 4u, 0.0f);
            for (std::size_t px = 0u; px < luma.size(); ++px)
                luma[px] = 0.2126f * rgba[px * 4u] + 0.7152f * rgba[px * 4u + 1u] + 0.0722f * rgba[px * 4u + 2u];
            return luma;
        }

        [[nodiscard]] f32 MaxAbsDifference(const std::vector<f32>& a, const std::vector<f32>& b)
        {
            if (a.size() != b.size() || a.empty())
                return std::numeric_limits<f32>::infinity();
            f32 worst = 0.0f;
            for (std::size_t i = 0u; i < a.size(); ++i)
            {
                const f32 d = std::abs(a[i] - b[i]);
                if (!std::isfinite(d))
                    return std::numeric_limits<f32>::infinity();
                worst = std::max(worst, d);
            }
            return worst;
        }

        [[nodiscard]] f64 MeanAbs(const std::vector<f32>& a)
        {
            if (a.empty())
                return 0.0;
            f64 sum = 0.0;
            for (const f32 v : a)
                sum += std::abs(static_cast<f64>(v));
            return sum / static_cast<f64>(a.size());
        }

        // One arm of a paired replay: the raw and reconstructed fields of the
        // frames from kRecordFrom on, plus the lineage the registry reported at
        // the end and the sampling context it started from.
        struct ArmRecord
        {
            std::vector<std::vector<f32>> Raw;
            std::vector<std::vector<f32>> Resolved;
            u64 StartFingerprint = 0;
            Renderer3D::FrameSamplingContext StartContext;
            u32 FinalAge = 0;
            TemporalHistoryInvalidationCause FinalLineageCause = TemporalHistoryInvalidationCause::None;
        };

        // The per-run summaries of one cell, and what a run-level interval is
        // computed over.
        struct RunSummary
        {
            // Bias of the temporal stage alone: the reconstruction against the
            // same frames resolved with no history (the feedback-0 arm).
            f64 Bias = 0.0;
            // Bias of the spatial stage before it (SSR's and SSGI's pre-blur):
            // the no-history resolve against the raw signal. 0 for TAA.
            f64 SpatialBias = 0.0;
            f64 Drift = 0.0;
            f64 VarianceRatio = 0.0;
            u32 Response = 0;
            u32 RawResponse = 0;
            u32 RestoreResponse = 0;
            f64 RestoredResidual = 0.0;
            u32 StepPixels = 0;
        };

        struct LineageView
        {
            bool Found = false;
            bool Valid = false;
            u32 Age = 0;
            TemporalHistoryInvalidationCause LineageCause = TemporalHistoryInvalidationCause::None;
        };

        [[nodiscard]] LineageView SignalLineage(TemporalHistoryEffect effect)
        {
            const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
            if (!graph)
                return {};
            for (const TemporalHistorySnapshot& history : graph->GetTemporalHistoryRegistry().Snapshot())
            {
                if (history.Key.Effect == effect && history.Key.Plane == TemporalHistoryPlane::Signal)
                {
                    return { .Found = true, .Valid = history.Valid, .Age = history.Age, .LineageCause = history.LineageCause };
                }
            }
            return {};
        }

        // Whether any history of `effect`, on any plane, still holds storage or
        // a usable frame.
        [[nodiscard]] bool HoldsAnyHistory(TemporalHistoryEffect effect)
        {
            const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
            return graph && graph->GetTemporalHistoryRegistry().HoldsAny(effect);
        }

        // A float target's luminance, read at the target's own size. With
        // transient aliasing off (the fixture's SetUp) a target read after the
        // frame still holds its own pass's output.
        [[nodiscard]] std::vector<f32> ReadTargetLuma(std::string_view name)
        {
            u32 texture = 0;
            if (const Ref<Framebuffer> fb = Renderer3D::ResolveFrameGraphFramebuffer(name); fb)
                texture = fb->GetColorAttachmentRendererID(0);
            if (texture == 0)
                texture = Renderer3D::ResolveFrameGraphTexture(name);
            if (texture == 0)
                return {};
            i32 width = 0;
            i32 height = 0;
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_WIDTH, &width);
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_HEIGHT, &height);
            if (width <= 0 || height <= 0)
                return {};
            std::vector<f32> rgba;
            ReadbackRgbaFloat(texture, static_cast<u32>(width), static_cast<u32>(height), rgba);
            return LumaOf(rgba);
        }

        // The run-level reset check of one event: did the resolve at the
        // shipped feedback and at feedback 0 agree on the frame before the
        // event (they must not), on the frame of it, and on the frame after
        // it (they must not again: a reset that never resumes is a history
        // that is never imported, and reads exactly like a clean reset).
        struct ResetObservation
        {
            f32 BeforeDifference = 0.0f;
            f32 EventDifference = 0.0f;
            f32 AfterDifference = 0.0f;
            LineageView Lineage;
        };

        enum class ResetEvent : u8
        {
            CameraCut,
            ProjectionChange,
            RenderScaleChange,
            SequenceRestart,
            Pause,
            ShaderLibraryReload,
            // The FSR1 upscale preset: it resizes the scene band, on Deferred
            // too, where a dynamic render scale is not honoured (#1537).
            UpscalePresetChange,
        };

        [[nodiscard]] const char* EventName(ResetEvent event)
        {
            switch (event)
            {
                case ResetEvent::CameraCut:
                    return "CameraCut";
                case ResetEvent::ProjectionChange:
                    return "ProjectionChange";
                case ResetEvent::RenderScaleChange:
                    return "RenderScaleChange";
                case ResetEvent::SequenceRestart:
                    return "SequenceRestart";
                case ResetEvent::Pause:
                    return "Pause";
                case ResetEvent::ShaderLibraryReload:
                    return "ShaderLibraryReload";
                case ResetEvent::UpscalePresetChange:
                    return "UpscalePresetChange";
            }
            return "?";
        }

        // The intended policy: a new lineage, with this cause, or none.
        [[nodiscard]] std::optional<TemporalHistoryInvalidationCause> ExpectedReset(ResetEvent event)
        {
            switch (event)
            {
                case ResetEvent::CameraCut:
                    return TemporalHistoryInvalidationCause::CameraCut;
                case ResetEvent::ProjectionChange:
                    return TemporalHistoryInvalidationCause::ProjectionChanged;
                case ResetEvent::RenderScaleChange:
                    return TemporalHistoryInvalidationCause::DynamicResolutionChanged;
                case ResetEvent::SequenceRestart:
                    return TemporalHistoryInvalidationCause::SamplingSequenceReset;
                case ResetEvent::UpscalePresetChange:
                    // A scene-band history is resized (DescriptorChanged); the
                    // display-band TAA history sees the render-scale change.
                    return TemporalHistoryInvalidationCause::None;
                case ResetEvent::Pause:
                case ResetEvent::ShaderLibraryReload:
                    return std::nullopt;
            }
            return std::nullopt;
        }

        // Every field of a sampling context, for the message of a fingerprint
        // mismatch: which dimension differs is the whole diagnosis.
        [[nodiscard]] std::string Describe(const Renderer3D::FrameSamplingContext& c)
        {
            char text[512];
            std::snprintf(text, sizeof(text),
                          "seed %u stochastic %u jitter %u fsr2 %u cloud %u fog %u currJitter (%g, %g) prevJitter "
                          "(%g, %g) passes %llx lineage %llx mock %d %g",
                          c.SequenceSeed, c.StochasticFrameIndex, c.TAAJitterFrameIndex, c.TemporalUpscalePhaseIndex,
                          c.CloudFrameIndex, c.FogFrameIndex, c.CurrJitterUV.x, c.CurrJitterUV.y, c.PrevJitterUV.x,
                          c.PrevJitterUV.y, static_cast<unsigned long long>(c.PassSequenceState),
                          static_cast<unsigned long long>(c.HistoryLineage), c.MockTimeActive ? 1 : 0,
                          static_cast<f64>(c.MockTime));
            return text;
        }

        class ScopedStaleHistoryFault
        {
          public:
            ScopedStaleHistoryFault() : m_Previous(Levers::FaultKeepStaleTemporalHistory())
            {
                Levers::SetFaultKeepStaleTemporalHistory(true);
            }
            ~ScopedStaleHistoryFault()
            {
                Levers::SetFaultKeepStaleTemporalHistory(m_Previous);
            }
            ScopedStaleHistoryFault(const ScopedStaleHistoryFault&) = delete;
            ScopedStaleHistoryFault& operator=(const ScopedStaleHistoryFault&) = delete;

          private:
            bool m_Previous;
        };
    } // namespace

    class TemporalLongSequenceEvidenceTest : public RendererAttachedTest
    {
      protected:
        void SetUp() override
        {
            // Every raw and resolved field is a transient read after the frame;
            // with aliasing on, a later pass may already own its texture and the
            // read returns that pass's image under the old name.
            m_SavedDisableAliasing = Levers::DisableTransientAliasing();
            Levers::SetDisableTransientAliasing(true);
            Time::SetMockTime(kMockTime);
            RendererAttachedTest::SetUp();
        }

        void TearDown() override
        {
            if (GetSceneRef())
            {
                Renderer3D::SetRenderScale(1.0f);
                Renderer3D::ResetFrameSequences();
            }
            RendererAttachedTest::TearDown();
            Time::ClearMockTime();
            Levers::SetDisableTransientAliasing(m_SavedDisableAliasing);
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity key = scene.CreateEntity("Key");
                auto& dl = key.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.3f, -0.85f, -0.4f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 2.5f;
                dl.m_CastShadows = true;
            }
            {
                Entity fill = scene.CreateEntity("Fill");
                auto& dl = fill.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.6f, -0.5f, 0.4f));
                dl.m_Color = glm::vec3(0.55f, 0.58f, 0.72f);
                dl.m_Intensity = 0.8f;
                dl.m_CastShadows = false;
            }

            // The RECEIVER: a moderately rough metal floor (a stochastic SSR
            // lobe, see ScreenSpaceTemporalResolveEvidenceTest) and a diffuse
            // wall behind the lamp that SSGI gathers its bounce onto. Neither
            // moves in any arm.
            {
                Entity floor = AddMesh("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 60.0f, 1.0f, 60.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.8f, 0.8f, 0.82f, 1.0f));
                mat.m_Material.SetMetallicFactor(1.0f);
                mat.m_Material.SetRoughnessFactor(0.25f);
            }
            {
                Entity wall = AddMesh("Wall", MeshPrimitive::Cube, { 0.0f, 4.0f, -9.0f }, { 20.0f, 8.0f, 0.5f });
                auto& mat = wall.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.75f, 0.75f, 0.75f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.9f);
            }

            // The SECONDARY LIGHT: an emissive block the floor reflects and
            // the wall receives bounce from, switched by its emission alone.
            m_Lamp = AddMesh("Lamp", MeshPrimitive::Cube, { -2.5f, 1.5f, -6.0f }, { 3.0f, 3.0f, 1.0f });
            {
                auto& mat = m_Lamp.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.1f, 0.05f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }
            SetLampOn(true);

            // The SECONDARY OCCLUDER: a slab above the view, outside the
            // frustum, that moves into the key light's path and drops a shadow
            // on the diffuse WALL in view. Not on the floor: the floor is
            // metal, and a directional light reaches a metal surface through
            // its specular lobe alone, so a shadow there is invisible except
            // under the highlight (the first version of this case measured the
            // gizmo instead). Parked far below the floor when out of the path.
            m_Occluder = AddMesh("Occluder", MeshPrimitive::Cube, kOccluderParked, { 4.0f, 0.4f, 4.0f });
            {
                auto& mat = m_Occluder.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.3f, 0.3f, 0.3f, 1.0f));
                mat.m_Material.SetRoughnessFactor(0.9f);
            }

            // A runtime camera at the editor camera's pose, for the cases that
            // drive Scene::OnUpdateRuntime (the pause).
            {
                Entity camera = scene.CreateEntity("Camera");
                auto& tc = camera.GetComponent<TransformComponent>();
                tc.Translation = kEye;
                tc.SetRotationEuler(glm::vec3(-kPitch, 0.0f, 0.0f));
                auto& cc = camera.AddComponent<CameraComponent>();
                cc.Primary = true;
                cc.Camera.SetPerspective(glm::radians(60.0f), 0.05f, 500.0f);
                cc.Camera.SetViewportSize(kWidth, kHeight);
            }
        }

        static constexpr glm::vec3 kEye{ 0.0f, 3.0f, 6.0f };
        static constexpr f32 kPitch = 0.3f;
        static constexpr glm::vec3 kOccluderParked{ 0.0f, -200.0f, 0.0f };
        // Between the key light and the wall in view, above the frustum: the
        // light comes from (+0.3, +0.85, +0.4), so a slab eight units up the
        // light direction from the wall's lit face at (0, 4, -8.75) shadows
        // it, while sitting 34 degrees above the camera's view axis.
        static constexpr glm::vec3 kOccluderInPath{ 2.4f, 10.8f, -5.55f };

        Entity AddMesh(const char* name, MeshPrimitive prim, const glm::vec3& pos, const glm::vec3& scale)
        {
            Entity e = GetScene().CreateEntity(name);
            auto& tc = e.GetComponent<TransformComponent>();
            tc.Translation = pos;
            tc.Scale = scale;
            auto& mc = e.AddComponent<MeshComponent>();
            mc.m_Primitive = prim;
            Ref<Mesh> mesh;
            switch (prim)
            {
                case MeshPrimitive::Plane:
                    mesh = MeshPrimitives::CreatePlane();
                    break;
                case MeshPrimitive::Sphere:
                    mesh = MeshPrimitives::CreateSphere();
                    break;
                default:
                    mesh = MeshPrimitives::CreateCube();
                    break;
            }
            if (mesh)
                mc.m_MeshSource = mesh->GetMeshSource();
            return e;
        }

        void SetLampOn(bool on)
        {
            auto& mat = m_Lamp.GetComponent<MaterialComponent>();
            mat.m_Material.SetEmissiveFactor(on ? glm::vec4(6.0f, 0.6f, 0.2f, 1.0f) : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        }

        void SetOccluderInPath(bool inPath)
        {
            m_Occluder.GetComponent<TransformComponent>().Translation = inPath ? kOccluderInPath : kOccluderParked;
        }

        // The perturbed state is lamp OFF / occluder IN; the base state the
        // opposite. Both arms of a pair start from whichever their role says.
        void ApplyPerturbation(Perturbation p, bool perturbed)
        {
            SetLampOn(!(p == Perturbation::SecondaryLight && perturbed));
            SetOccluderInPath(p == Perturbation::SecondaryOccluder && perturbed);
        }

        [[nodiscard]] static EditorCamera MakeCamera(f32 fovDegrees = 60.0f, const glm::vec3& eye = kEye,
                                                     f32 yaw = 0.0f)
        {
            EditorCamera camera(fovDegrees, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, kPitch);
            return camera;
        }

        static void SetPath(RenderingPath path)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
        }

        // Exactly one estimator on, at `feedback`; everything else that keeps
        // history off, so a pair's arms differ in that one resolve alone.
        static void ConfigureEstimator(Estimator e, f32 feedback)
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.TAAEnabled = e == Estimator::TAA;
            pp.TAAFeedback = feedback;
            pp.TAASharpness = 0.0f; // a sharpened frame is not the resolve
            pp.SSREnabled = e == Estimator::SSR;
            pp.SSGIEnabled = e == Estimator::SSGI;
            pp.AutoExposureEnabled = false;
            if (e == Estimator::SSR)
            {
                pp.SSRIntensity = 1.0f;
                pp.SSRMaxDistance = 40.0f;
                pp.SSRThickness = 0.8f;
                pp.SSRStride = 0.25f;
                pp.SSRMaxSteps = 64;
                pp.SSRBinarySearchSteps = 6;
                pp.SSRMaxRoughness = 0.8f;
                pp.SSREdgeFade = 0.1f;
                pp.SSRTemporalResolve = true;
                pp.SSRTemporalFeedback = feedback;
            }
            if (e == Estimator::SSGI)
            {
                pp.SSGIIntensity = 2.5f;
                pp.SSGIMaxDistance = 30.0f;
                pp.SSGIThickness = 1.5f;
                pp.SSGIStride = 0.6f;
                pp.SSGIMaxSteps = 24;
                pp.SSGIRayCount = 8;
                pp.SSGIEdgeFade = 0.1f;
                pp.SSGITemporalResolve = true;
                pp.SSGITemporalFeedback = feedback;
            }
        }

        // The raw signal and the reconstruction an estimator writes. TAA's raw
        // signal is its input, which this reads from the paired feedback-0 arm
        // instead (see RunCell), so TAA returns the reconstruction alone.
        static void CaptureSignals(Estimator e, std::vector<f32>& raw, std::vector<f32>& resolved)
        {
            switch (e)
            {
                case Estimator::TAA:
                    raw.clear();
                    resolved = ReadTargetLuma(ResourceNames::TAAColor);
                    return;
                case Estimator::SSR:
                    raw = ReadTargetLuma(ResourceNames::SSRSignal);
                    resolved = ReadTargetLuma(ResourceNames::SSRResolved);
                    return;
                case Estimator::SSGI:
                    raw = ReadTargetLuma(ResourceNames::SSGISignal);
                    resolved = ReadTargetLuma(ResourceNames::SSGIResolved);
                    return;
            }
        }

        enum class ArmRole : u8
        {
            Old,  // the base state throughout
            New,  // the perturbed state throughout
            Step, // base, perturbed at kStep, restored at kRestore
        };

        ArmRecord RunArm(Estimator e, Perturbation p, ArmRole role, u32 seed, f32 feedback, u32 frames,
                         const std::string& pngPrefix = {})
        {
            ConfigureEstimator(e, feedback);
            ApplyPerturbation(p, role == ArmRole::New);
            Renderer3D::ResetFrameSequences(seed);

            ArmRecord record;
            record.StartContext = Renderer3D::GetFrameSamplingContext();
            record.StartFingerprint = record.StartContext.Fingerprint();
            // Runtime ticks through the scene's own primary camera: the
            // editor path draws the grid, gizmos and entity icons into the
            // frame, and a perturbation that moves an entity moves its icon.
            for (u32 frame = 0u; frame < frames; ++frame)
            {
                if (role == ArmRole::Step && frame == kStep)
                    ApplyPerturbation(p, true);
                if (role == ArmRole::Step && frame == kRestore)
                    ApplyPerturbation(p, false);
                RunFrames(1);
                if (frame < kRecordFrom)
                    continue;
                std::vector<f32> raw;
                std::vector<f32> resolved;
                CaptureSignals(e, raw, resolved);
                record.Raw.push_back(std::move(raw));
                record.Resolved.push_back(std::move(resolved));
                if (!pngPrefix.empty() &&
                    (frame == kStep - 1u || frame == kStep + ExponentialResponseBound(ShippedFeedback(e)) ||
                     frame == frames - 1u))
                {
                    const char* phase = frame == kStep - 1u ? "Before" : (frame == frames - 1u ? "Restored" : "Responded");
                    WriteCompositePng(pngPrefix + "_" + phase);
                }
            }
            const LineageView lineage = SignalLineage(EffectOf(e));
            record.FinalAge = lineage.Age;
            record.FinalLineageCause = lineage.LineageCause;
            return record;
        }

        void WriteCompositePng(const std::string& name)
        {
            std::vector<u8> rgba;
            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(rgba, width, height));
            const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
            std::vector<u8> row(rowBytes);
            for (u32 y = 0; y < height / 2u; ++y)
            {
                u8* top = rgba.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bottom = rgba.data() + static_cast<std::size_t>(height - 1u - y) * rowBytes;
                std::memcpy(row.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, row.data(), rowBytes);
            }
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / (name + ".png")).string();
            ASSERT_NE(::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, rgba.data(),
                                       static_cast<int>(rowBytes)),
                      0)
                << path;
        }

        // Frames [from, to) of an arm, as offsets into its recorded fields.
        [[nodiscard]] static std::span<const std::vector<f32>> Window(const std::vector<std::vector<f32>>& fields,
                                                                      u32 from, u32 to)
        {
            return std::span<const std::vector<f32>>(fields).subspan(from - kRecordFrom, to - from);
        }

        // Signed relative bias of `resolved` against `raw` over frames
        // [from, to): the time-mean difference over the time-mean magnitude,
        // pooled over every pixel the raw signal touches.
        [[nodiscard]] static f64 RelativeBias(std::span<const std::vector<f32>> resolved,
                                              std::span<const std::vector<f32>> raw)
        {
            f64 difference = 0.0;
            f64 magnitude = 0.0;
            for (std::size_t t = 0u; t < resolved.size(); ++t)
            {
                for (std::size_t i = 0u; i < resolved[t].size() && i < raw[t].size(); ++i)
                {
                    difference += static_cast<f64>(resolved[t][i]) - static_cast<f64>(raw[t][i]);
                    magnitude += std::abs(static_cast<f64>(raw[t][i]));
                }
            }
            return magnitude > 0.0 ? difference / magnitude : 0.0;
        }

        // Mean per-pixel temporal variance of `fields`.
        [[nodiscard]] static f64 MeanTemporalVariance(std::span<const std::vector<f32>> fields)
        {
            if (fields.size() < 2u || fields[0].empty())
                return 0.0;
            const std::size_t pixels = fields[0].size();
            f64 total = 0.0;
            for (std::size_t i = 0u; i < pixels; ++i)
            {
                f64 sum = 0.0;
                f64 squares = 0.0;
                for (const auto& field : fields)
                {
                    const f64 v = static_cast<f64>(field[i]);
                    sum += v;
                    squares += v * v;
                }
                const f64 n = static_cast<f64>(fields.size());
                total += std::max(squares / n - (sum / n) * (sum / n), 0.0);
            }
            return total / static_cast<f64>(pixels);
        }

        // One cell of the long-sequence matrix: three seeds, each a paired
        // replay of OLD / NEW / STEP arms (plus a feedback-0 OLD arm for TAA's
        // raw signal), asserted and summarised.
        void RunCell(Estimator e, RenderingPath path, Perturbation p)
        {
            SetPath(path);
            const f32 feedback = ShippedFeedback(e);
            const u32 bound = ExponentialResponseBound(feedback);
            const std::string cell = std::string("TemporalLongSeq_") + EstimatorName(e) + "_GL_" + PathName(path) +
                                     "_" + PerturbationName(p);

            std::vector<RunSummary> runs;
            std::vector<std::vector<f32>> firstRawOfSeed;
            for (const u32 seed : kSeeds)
            {
                const ArmRecord oldArm = RunArm(e, p, ArmRole::Old, seed, feedback, kOldEnd);
                const ArmRecord newArm = RunArm(e, p, ArmRole::New, seed, feedback, kRestore);
                const ArmRecord stepArm =
                    RunArm(e, p, ArmRole::Step, seed, feedback, kStepEnd, seed == kSeeds[0] ? cell : std::string{});
                ASSERT_FALSE(HasFatalFailure());
                ASSERT_FALSE(oldArm.Resolved.front().empty())
                    << cell << ": the reconstruction target did not resolve; nothing below would measure anything";

                // The same frames resolved with NO history: the feedback-0 arm,
                // whose resolve blends nothing. For TAA that is its input, the
                // raw signal; for SSR and SSGI it is the raw signal after the
                // spatial pre-blur, which is what separates the two stages.
                const ArmRecord noHistoryArm = RunArm(e, p, ArmRole::Old, seed, 0.0f, kOldEnd);
                std::vector<std::vector<f32>> oldRaw = oldArm.Raw;
                std::vector<std::vector<f32>> stepRaw = stepArm.Raw;
                std::vector<std::vector<f32>> newRaw = newArm.Raw;
                if (e == Estimator::TAA)
                {
                    oldRaw = noHistoryArm.Resolved;
                    stepRaw = RunArm(e, p, ArmRole::Step, seed, 0.0f, kStepEnd).Resolved;
                    newRaw = RunArm(e, p, ArmRole::New, seed, 0.0f, kRestore).Resolved;
                }
                firstRawOfSeed.push_back(oldRaw.front());

                // 1. THE REPLAY CONTRACT. Same seed, same state, same frames:
                //    every field identical until the step.
                EXPECT_EQ(oldArm.StartFingerprint, stepArm.StartFingerprint)
                    << cell << " seed " << seed << "\n  old:  " << Describe(oldArm.StartContext)
                    << "\n  step: " << Describe(stepArm.StartContext);
                EXPECT_EQ(oldArm.StartFingerprint, newArm.StartFingerprint)
                    << cell << " seed " << seed << "\n  old: " << Describe(oldArm.StartContext)
                    << "\n  new: " << Describe(newArm.StartContext);
                for (u32 frame = kRecordFrom; frame < kStep; ++frame)
                {
                    const u32 i = frame - kRecordFrom;
                    EXPECT_EQ(MaxAbsDifference(oldArm.Resolved[i], stepArm.Resolved[i]), 0.0f)
                        << cell << " seed " << seed << ": the paired replay diverged at frame " << frame
                        << ", before the arms differ in anything. Some state the sampling context does not "
                           "restart reached this frame.";
                    EXPECT_EQ(MaxAbsDifference(oldRaw[i], stepRaw[i]), 0.0f)
                        << cell << " seed " << seed << ": raw signal diverged at frame " << frame;
                    if (HasFailure())
                        return;
                }

                RunSummary run;
                // 2. RESPONSE to the perturbation, reconstruction and raw.
                const PairedResponseResult response =
                    MeasurePairedResponse(Window(stepArm.Resolved, kStep, kRestore),
                                          Window(newArm.Resolved, kStep, kRestore),
                                          Window(oldArm.Resolved, kStep, kRestore), kSettledFraction, StepThreshold(oldArm));
                const PairedResponseResult rawResponse = MeasurePairedResponse(
                    Window(stepRaw, kStep, kRestore), Window(newRaw, kStep, kRestore), Window(oldRaw, kStep, kRestore),
                    kSettledFraction, StepThreshold(oldArm));
                // 3. GHOSTING after the restore: the target is the base state
                //    now, the perturbed arm what it must let go of. NEW stops at
                //    kRestore, so its last frame stands in for the perturbed
                //    state; a still camera keeps it there.
                std::vector<std::vector<f32>> heldNew(kStepEnd - kRestore, newArm.Resolved.back());
                const PairedResponseResult restore =
                    MeasurePairedResponse(Window(stepArm.Resolved, kRestore, kStepEnd),
                                          Window(oldArm.Resolved, kRestore, kStepEnd), heldNew, kSettledFraction,
                                          StepThreshold(oldArm));
                run.Response = response.ResponseFrames;
                run.RawResponse = rawResponse.ResponseFrames;
                run.RestoreResponse = restore.ResponseFrames;
                run.RestoredResidual = restore.FinalResidual;
                run.StepPixels = response.StepPixels;

                // 4. BIAS, DRIFT, VARIANCE on the long still arm, per stage:
                //    the temporal one against the no-history resolve, the
                //    spatial one (SSR, SSGI) against the raw signal.
                const auto& noHistory = noHistoryArm.Resolved;
                run.Bias = RelativeBias(Window(oldArm.Resolved, kStep, kOldEnd), Window(noHistory, kStep, kOldEnd));
                run.SpatialBias =
                    e == Estimator::TAA
                        ? 0.0
                        : RelativeBias(Window(noHistory, kStep, kOldEnd), Window(oldArm.Raw, kStep, kOldEnd));
                run.Drift =
                    RelativeBias(Window(oldArm.Resolved, kDriftSplit, kOldEnd), Window(noHistory, kDriftSplit, kOldEnd)) -
                    RelativeBias(Window(oldArm.Resolved, kStep, kDriftSplit), Window(noHistory, kStep, kDriftSplit));
                const f64 noHistoryVariance = MeanTemporalVariance(Window(noHistory, kStep, kOldEnd));
                run.VarianceRatio =
                    noHistoryVariance > 0.0
                        ? MeanTemporalVariance(Window(oldArm.Resolved, kStep, kOldEnd)) / noHistoryVariance
                        : 0.0;

                // 5. LINEAGE through the perturbation: a secondary change is not
                //    a reset, so the step arm's history is exactly as old as
                //    the still arm's at the same frame count.
                EXPECT_EQ(stepArm.FinalLineageCause, TemporalHistoryInvalidationCause::SamplingSequenceReset)
                    << cell << ": the perturbation broke the lineage";
                EXPECT_GE(stepArm.FinalAge, kStepEnd - 2u) << cell << " seed " << seed;

                std::printf("[temporal-long-seq] %s seed %u: response %u (raw %u, bound %u), restore %u, "
                            "restored residual %.4f, step px %u, temporal bias %+.4f, spatial bias %+.4f, drift %+.5f, "
                            "variance ratio %.3f, age %u\n",
                            cell.c_str(), seed, run.Response, run.RawResponse, bound, run.RestoreResponse,
                            run.RestoredResidual, run.StepPixels, run.Bias, run.SpatialBias, run.Drift,
                            run.VarianceRatio, stepArm.FinalAge);
                runs.push_back(run);
            }
            ASSERT_EQ(runs.size(), kSeeds.size());

            // Independent runs must actually be independent: different seeds
            // draw different raw samples. For TAA they differ by jitter phase.
            EXPECT_GT(MaxAbsDifference(firstRawOfSeed[0], firstRawOfSeed[1]), 0.0f)
                << cell << ": seeds 0 and 1 rendered the same raw frame, so the 'independent' runs are one run";

            std::vector<f64> biases;
            std::vector<f64> spatialBiases;
            std::vector<f64> drifts;
            std::vector<f64> ratios;
            for (const RunSummary& run : runs)
            {
                biases.push_back(run.Bias);
                spatialBiases.push_back(run.SpatialBias);
                drifts.push_back(run.Drift);
                ratios.push_back(run.VarianceRatio);
                EXPECT_GT(run.StepPixels, 0u) << cell << ": the perturbation moved no pixel of the reconstruction";
                // The clip can only shorten the exponential; a history slower
                // than its own feedback allows is holding on to something. A raw
                // signal that never settled (kNeverSettled) allows anything;
                // the sum saturates rather than wrapping below `bound`.
                const u32 allowance = run.RawResponse > std::numeric_limits<u32>::max() - bound
                                          ? std::numeric_limits<u32>::max()
                                          : std::max(bound, run.RawResponse + bound);
                EXPECT_LE(run.Response, allowance) << cell;
                EXPECT_LE(run.RestoreResponse, allowance) << cell;
                EXPECT_LE(run.RestoredResidual, kRestoredResidual) << cell << ": residual ghosting after the restore";
            }
            const RunInterval bias = MeanOverIndependentRuns(biases, SampleProvenance::IndependentRunSummaries);
            const RunInterval spatialBias =
                MeanOverIndependentRuns(spatialBiases, SampleProvenance::IndependentRunSummaries);
            const RunInterval drift = MeanOverIndependentRuns(drifts, SampleProvenance::IndependentRunSummaries);
            const RunInterval ratio = MeanOverIndependentRuns(ratios, SampleProvenance::IndependentRunSummaries);
            ASSERT_TRUE(bias.Valid && spatialBias.Valid && drift.Valid && ratio.Valid);
            std::printf("[temporal-long-seq] %s: clip gamma 1.25, feedback %.2f (biased: clipped history); "
                        "temporal bias %s; spatial bias %s; drift %s; variance ratio %s\n",
                        cell.c_str(), feedback, bias.Describe().c_str(), spatialBias.Describe().c_str(),
                        drift.Describe().c_str(), ratio.Describe().c_str());
            std::fflush(stdout);

            // No persistent feedback drift: the second half of a long still run
            // carries the bias the first did, within the runs' uncertainty or
            // half a percent of the signal, whichever is larger.
            EXPECT_TRUE(drift.Lo() <= kDriftFloor && drift.Hi() >= -kDriftFloor)
                << cell << ": the bias moved between the two halves of the still run: " << drift.Describe();
            // A reconstruction keeps less temporal variance than the same frames
            // resolved with no history.
            EXPECT_LT(ratio.Hi(), 1.0) << cell << ": " << ratio.Describe();
        }

        // The pixels a step must move by more than 2% of the still arm's mean
        // magnitude to count: below that the paired residual is dominated by a
        // pixel the step barely touched.
        [[nodiscard]] static f32 StepThreshold(const ArmRecord& oldArm)
        {
            return static_cast<f32>(std::max(1.0e-3, 0.02 * MeanAbs(oldArm.Resolved.back())));
        }

        // ---- reset policy ----------------------------------------------------

        // Settle, apply `event`, render the event frame; report how the shipped
        // resolve and its feedback-0 twin compared on the frame before and the
        // frame of the event, and the lineage two frames later.
        ResetObservation ObserveReset(Estimator e, ResetEvent event, u32 seed)
        {
            constexpr u32 kSettle = 24;
            std::array<std::vector<f32>, 2> before;
            std::array<std::vector<f32>, 2> at;
            std::array<std::vector<f32>, 2> after;
            LineageView lineage;
            for (u32 arm = 0; arm < 2u; ++arm)
            {
                ConfigureEstimator(e, arm == 0u ? ShippedFeedback(e) : 0.0f);
                ApplyPerturbation(Perturbation::SecondaryLight, false);
                Renderer3D::SetRenderScale(1.0f);
                Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Off;
                GetScene().SetPaused(false);
                Renderer3D::ResetFrameSequences(seed);
                EditorCamera camera = MakeCamera();
                // A pause is a runtime state: Scene::OnUpdateRuntime keeps
                // rendering while it holds the simulation, the animation clock
                // and the wind. Every other event is an editor-camera one.
                const auto tick = [this, event, &camera](u32 frames)
                {
                    if (event == ResetEvent::Pause)
                        RunFrames(frames);
                    else
                        RunEditorFrames(camera, frames);
                };
                std::vector<f32> raw;
                tick(kSettle);
                CaptureSignals(e, raw, before[arm]);

                switch (event)
                {
                    case ResetEvent::CameraCut:
                        // What the editor's teleports and the MCP camera setters do.
                        camera = MakeCamera(60.0f, kEye + glm::vec3(4.0f, 1.0f, -1.0f), 0.5f);
                        Renderer3D::InvalidateTemporalHistories(TemporalHistoryInvalidationCause::CameraCut);
                        break;
                    case ResetEvent::ProjectionChange:
                        camera = MakeCamera(45.0f);
                        break;
                    case ResetEvent::RenderScaleChange:
                        Renderer3D::SetRenderScale(0.75f);
                        break;
                    case ResetEvent::SequenceRestart:
                        Renderer3D::ResetFrameSequences(seed);
                        break;
                    case ResetEvent::Pause:
                        GetScene().SetPaused(true);
                        break;
                    case ResetEvent::ShaderLibraryReload:
                        Renderer3D::GetShaderLibrary().ReloadShaders();
                        break;
                    case ResetEvent::UpscalePresetChange:
                        Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Performance;
                        break;
                }
                tick(1);
                CaptureSignals(e, raw, at[arm]);
                tick(1);
                CaptureSignals(e, raw, after[arm]);
                tick(1);
                if (arm == 0u)
                    lineage = SignalLineage(EffectOf(e));
            }
            GetScene().SetPaused(false);
            Renderer3D::SetRenderScale(1.0f);
            Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Off;
            return { .BeforeDifference = MaxAbsDifference(before[0], before[1]),
                     .EventDifference = MaxAbsDifference(at[0], at[1]),
                     .AfterDifference = MaxAbsDifference(after[0], after[1]),
                     .Lineage = lineage };
        }

        // Whether the event frame shows the intended policy. A reset frame has
        // no history, so the two feedbacks agree to float rounding; a kept
        // history keeps them as far apart as they were.
        [[nodiscard]] static bool LooksReset(const ResetObservation& o)
        {
            return o.EventDifference <= 1.0e-4f * std::max(1.0f, o.BeforeDifference);
        }

        void CheckResetPolicy(Estimator e, RenderingPath path, ResetEvent event)
        {
            SetPath(path);
            const std::string cell = std::string(EstimatorName(e)) + "_GL_" + PathName(path) + "_" + EventName(event);
            const std::optional<TemporalHistoryInvalidationCause> expected = ExpectedReset(event);
            for (const u32 seed : { kSeeds[0], kSeeds[1] })
            {
                const ResetObservation o = ObserveReset(e, event, seed);
                std::printf("[temporal-reset] %s seed %u: before %.6f, event frame %.6f, after %.6f, lineage age %u "
                            "cause %u\n",
                            cell.c_str(), seed, o.BeforeDifference, o.EventDifference, o.AfterDifference,
                            o.Lineage.Age, static_cast<u32>(o.Lineage.LineageCause));
                std::fflush(stdout);
                // The instrument: with history in play the two feedbacks differ.
                EXPECT_GT(o.BeforeDifference, 1.0e-3f)
                    << cell << ": shipped and feedback-0 resolves agree on a settled frame, so this check cannot see "
                               "history at all";
                ASSERT_TRUE(o.Lineage.Found) << cell << ": no registry history for this estimator";
                if (expected)
                {
                    EXPECT_TRUE(LooksReset(o))
                        << cell << ": the event frame still blended a history (shipped vs feedback-0 differ by "
                        << o.EventDifference << ")";
                    // None: the policy is a reset, whichever cause carries it.
                    if (*expected != TemporalHistoryInvalidationCause::None)
                        EXPECT_EQ(o.Lineage.LineageCause, *expected) << cell;
                    EXPECT_LE(o.Lineage.Age, 3u) << cell << ": the lineage did not restart at the event";
                    // And it resumed: the frame after the event blends the
                    // history the event frame wrote. A history the next frame
                    // never imports reads, on the event frame, exactly like a
                    // correct reset (an upscale toggle did this to SSR).
                    EXPECT_GT(o.AfterDifference, 1.0e-4f * std::max(1.0f, o.BeforeDifference))
                        << cell << ": the history did not resume the frame after the reset";
                }
                else
                {
                    EXPECT_FALSE(LooksReset(o)) << cell << ": a history the policy keeps was dropped";
                    EXPECT_GE(o.Lineage.Age, 24u) << cell << ": the lineage restarted on an event that keeps it";
                }
            }
        }

        // The stale-history negative control: with every invalidation dropped,
        // a reset event must look like it kept its history, on both checks.
        void CheckResetNegativeControl(Estimator e, RenderingPath path, ResetEvent event)
        {
            SetPath(path);
            const ScopedStaleHistoryFault fault;
            const ResetObservation o = ObserveReset(e, event, kSeeds[0]);
            const std::string cell = std::string(EstimatorName(e)) + "_GL_" + PathName(path) + "_" + EventName(event);
            std::printf("[temporal-reset] FAULT %s: event frame %.6f, lineage age %u\n", cell.c_str(),
                        o.EventDifference, o.Lineage.Age);
            EXPECT_FALSE(LooksReset(o)) << cell << ": the stale-history fault did not reach the image check";
            EXPECT_GT(o.Lineage.Age, 3u) << cell << ": the stale-history fault did not reach the lineage check";
        }

        // The populate sweep: a resolve switched off for a frame stops
        // declaring its history, so the populate releases it, and switched
        // back on it starts a new lineage instead of resuming the old one.
        void CheckFeatureToggle(Estimator e, RenderingPath path)
        {
            SetPath(path);
            const std::string cell = std::string(EstimatorName(e)) + "_GL_" + PathName(path) + "_FeatureToggle";
            ConfigureEstimator(e, ShippedFeedback(e));
            ApplyPerturbation(Perturbation::SecondaryLight, false);
            Renderer3D::ResetFrameSequences(kSeeds[0]);
            const EditorCamera camera = MakeCamera();
            RunEditorFrames(camera, 24);
            const LineageView settled = SignalLineage(EffectOf(e));
            ASSERT_TRUE(settled.Found) << cell;
            EXPECT_GE(settled.Age, 24u) << cell << ": the lineage never got going, so a restart cannot show";

            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.TAAEnabled = false;
            pp.SSREnabled = false;
            pp.SSGIEnabled = false;
            RunEditorFrames(camera, 1);
            EXPECT_FALSE(HoldsAnyHistory(EffectOf(e))) << cell << ": a history its effect stopped declaring was kept";

            ConfigureEstimator(e, ShippedFeedback(e));
            RunEditorFrames(camera, 2);
            const LineageView resumed = SignalLineage(EffectOf(e));
            std::printf("[temporal-reset] %s: settled age %u, re-enabled age %u cause %u\n", cell.c_str(), settled.Age,
                        resumed.Age, static_cast<u32>(resumed.LineageCause));
            std::fflush(stdout);
            ASSERT_TRUE(resumed.Found) << cell;
            EXPECT_LE(resumed.Age, 2u) << cell << ": re-enabling resumed the lineage from before the toggle";
            // TAA's re-enable also switches the projection jitter back on, and
            // that invalidation lands after the release: the newer break names
            // the lineage.
            const TemporalHistoryInvalidationCause expected = e == Estimator::TAA
                                                                  ? TemporalHistoryInvalidationCause::JitterReset
                                                                  : TemporalHistoryInvalidationCause::FeatureToggled;
            EXPECT_EQ(resumed.LineageCause, expected) << cell;
        }

        // ---- order independence ------------------------------------------------

        // The motion half of the replay contract: the first frame after
        // ResetFrameSequences measures velocity against itself, so a pre-roll
        // from another camera leaves no motion behind. Before #1348's review
        // that frame's camera velocity was taken against the pre-roll's
        // view-projection, which motion blur and every velocity reader saw.
        void CheckFirstFrameMotion()
        {
            SetPath(RenderingPath::Deferred);
            ConfigureEstimator(Estimator::TAA, ShippedFeedback(Estimator::TAA));
            std::array<std::vector<f32>, 2> velocity;
            for (u32 arm = 0; arm < 2u; ++arm)
            {
                if (arm == 1u)
                    RunEditorFrames(MakeCamera(50.0f, kEye + glm::vec3(-3.0f, 2.0f, 2.0f), -0.6f), 20);
                Renderer3D::ResetFrameSequences(kSeeds[1]);
                RunEditorFrames(MakeCamera(), 1);
                const u32 texture = Renderer3D::ResolveFrameGraphTexture(ResourceNames::Velocity);
                ASSERT_NE(texture, 0u) << "no velocity target on Deferred";
                i32 width = 0;
                i32 height = 0;
                glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_WIDTH, &width);
                glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_HEIGHT, &height);
                ASSERT_GT(width, 0);
                ASSERT_GT(height, 0);
                ReadbackRgbaFloat(texture, static_cast<u32>(width), static_cast<u32>(height), velocity[arm]);
            }
            f32 largest = 0.0f;
            for (sizet i = 0; i + 1u < velocity[0].size(); i += 4u)
                largest = std::max({ largest, std::abs(velocity[0][i]), std::abs(velocity[0][i + 1u]) });
            std::printf("[temporal-reset] first-frame motion: clean max |v| %.9f, pre-roll vs clean %.9f\n", largest,
                        MaxAbsDifference(velocity[0], velocity[1]));
            std::fflush(stdout);
            // A still camera over a still scene, on a frame that is its own
            // history: no motion at all, jitter included.
            EXPECT_EQ(largest, 0.0f) << "the first replayed frame reports motion";
            EXPECT_EQ(MaxAbsDifference(velocity[0], velocity[1]), 0.0f)
                << "the first replayed frame's velocity depends on the pre-roll";
        }

        // The replay contract stated as the failure #1489 reported: a render
        // must not depend on what rendered before it. A sequence replayed from
        // one seed after a DIFFERENT pre-roll (another camera, the lamp off, the
        // occluder in) must be bit-identical, raw and resolved, to the same
        // sequence replayed after none. Whatever the reset leaves behind shows
        // up here: TAA's colour history did, until #1348 put it in the registry.
        void CheckOrderIndependence(Estimator e, RenderingPath path)
        {
            SetPath(path);
            constexpr u32 kFrames = 12;
            const std::string cell = std::string(EstimatorName(e)) + "_GL_" + PathName(path);
            std::array<std::vector<f32>, 2> raw;
            std::array<std::vector<f32>, 2> resolved;
            std::array<Renderer3D::FrameSamplingContext, 2> contexts{};
            for (u32 arm = 0; arm < 2u; ++arm)
            {
                ConfigureEstimator(e, ShippedFeedback(e));
                if (arm == 1u)
                {
                    // The pre-roll: a different view of a different state.
                    ApplyPerturbation(Perturbation::SecondaryLight, true);
                    SetOccluderInPath(true);
                    RunEditorFrames(MakeCamera(50.0f, kEye + glm::vec3(-3.0f, 2.0f, 2.0f), -0.6f), 20);
                }
                ApplyPerturbation(Perturbation::SecondaryLight, false);
                Renderer3D::ResetFrameSequences(kSeeds[1]);
                contexts[arm] = Renderer3D::GetFrameSamplingContext();
                RunEditorFrames(MakeCamera(), kFrames);
                CaptureSignals(e, raw[arm], resolved[arm]);
            }
            ASSERT_FALSE(resolved[0].empty()) << cell;
            EXPECT_EQ(contexts[0].Fingerprint(), contexts[1].Fingerprint())
                << cell << ": the reset left a different sampling context\n  clean:    " << Describe(contexts[0])
                << "\n  pre-roll: " << Describe(contexts[1]);
            EXPECT_EQ(MaxAbsDifference(resolved[0], resolved[1]), 0.0f)
                << cell << ": the replay depends on what rendered before the reset";
            if (!raw[0].empty())
                EXPECT_EQ(MaxAbsDifference(raw[0], raw[1]), 0.0f) << cell << ": raw signal";
        }

        bool m_SavedDisableAliasing = false;
        Entity m_Lamp;
        Entity m_Occluder;
    };

    // ---- long sequences: perturb and restore a secondary light or occluder --

    TEST_F(TemporalLongSequenceEvidenceTest, TAAOnForwardRespondsToASecondaryOccluder)
    {
        RunCell(Estimator::TAA, RenderingPath::Forward, Perturbation::SecondaryOccluder);
    }

    TEST_F(TemporalLongSequenceEvidenceTest, TAAOnForwardPlusRespondsToASecondaryOccluder)
    {
        RunCell(Estimator::TAA, RenderingPath::ForwardPlus, Perturbation::SecondaryOccluder);
    }

    TEST_F(TemporalLongSequenceEvidenceTest, TAAOnDeferredRespondsToASecondaryOccluder)
    {
        RunCell(Estimator::TAA, RenderingPath::Deferred, Perturbation::SecondaryOccluder);
    }

    TEST_F(TemporalLongSequenceEvidenceTest, TAAOnDeferredRespondsToASecondaryLight)
    {
        RunCell(Estimator::TAA, RenderingPath::Deferred, Perturbation::SecondaryLight);
    }

    // SSR and SSGI run on Deferred only (RenderPipeline: ptOwnership && deferredPath).
    TEST_F(TemporalLongSequenceEvidenceTest, SSRRespondsToASecondaryLight)
    {
        RunCell(Estimator::SSR, RenderingPath::Deferred, Perturbation::SecondaryLight);
    }

    TEST_F(TemporalLongSequenceEvidenceTest, SSGIRespondsToASecondaryLight)
    {
        RunCell(Estimator::SSGI, RenderingPath::Deferred, Perturbation::SecondaryLight);
    }

    TEST_F(TemporalLongSequenceEvidenceTest, SSGIRespondsToASecondaryOccluder)
    {
        RunCell(Estimator::SSGI, RenderingPath::Deferred, Perturbation::SecondaryOccluder);
    }

    // ---- order independence (#1489's report, stated generally) ---------------

    TEST_F(TemporalLongSequenceEvidenceTest, AReplayDoesNotDependOnWhatRenderedBeforeIt)
    {
        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
            CheckOrderIndependence(Estimator::TAA, path);
        CheckOrderIndependence(Estimator::SSR, RenderingPath::Deferred);
        CheckOrderIndependence(Estimator::SSGI, RenderingPath::Deferred);
    }

    TEST_F(TemporalLongSequenceEvidenceTest, AReplayedFirstFrameCarriesNoMotionFromThePreRoll)
    {
        CheckFirstFrameMotion();
    }

    // The engine TAA jitter has eight phases. Seeds k and k + 8 start at the
    // same phase, so the second lap shifts the pattern on the torus: their
    // jitter must still differ, or two "independent" TAA runs repeat each
    // other's sub-pixel samples exactly.
    TEST_F(TemporalLongSequenceEvidenceTest, SeedsAPhaseLapApartJitterDifferently)
    {
        SetPath(RenderingPath::Forward);
        ConfigureEstimator(Estimator::TAA, ShippedFeedback(Estimator::TAA));
        const EditorCamera camera = MakeCamera();
        for (const u32 seed : { 0u, 3u, 7u })
        {
            Renderer3D::ResetFrameSequences(seed);
            RunEditorFrames(camera, 1);
            const glm::vec2 first = Renderer3D::GetFrameSamplingContext().CurrJitterUV;
            Renderer3D::ResetFrameSequences(seed + 8u);
            RunEditorFrames(camera, 1);
            const glm::vec2 second = Renderer3D::GetFrameSamplingContext().CurrJitterUV;
            std::printf("[temporal-reset] jitter seed %u (%.6f, %.6f) vs seed %u (%.6f, %.6f)\n", seed, first.x, first.y,
                        seed + 8u, second.x, second.y);
            EXPECT_GT(glm::length(first - second), 1.0e-6f) << "seeds " << seed << " and " << seed + 8u
                                                            << " jitter identically";
        }
        Renderer3D::ResetFrameSequences(0u);
    }

    // ---- reset policy ----------------------------------------------------------

    TEST_F(TemporalLongSequenceEvidenceTest, ACameraCutStartsANewLineageOnEveryResolve)
    {
        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
            CheckResetPolicy(Estimator::TAA, path, ResetEvent::CameraCut);
        CheckResetPolicy(Estimator::SSR, RenderingPath::Deferred, ResetEvent::CameraCut);
        CheckResetPolicy(Estimator::SSGI, RenderingPath::Deferred, ResetEvent::CameraCut);
    }

    TEST_F(TemporalLongSequenceEvidenceTest, AProjectionChangeStartsANewLineageOnEveryResolve)
    {
        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
            CheckResetPolicy(Estimator::TAA, path, ResetEvent::ProjectionChange);
        CheckResetPolicy(Estimator::SSR, RenderingPath::Deferred, ResetEvent::ProjectionChange);
        CheckResetPolicy(Estimator::SSGI, RenderingPath::Deferred, ResetEvent::ProjectionChange);
    }

    // Deferred does not honour a dynamic render scale (#1537), so this policy
    // is reachable on the forward paths alone, and SSR and SSGI (Deferred
    // only) cannot reach it at all.
    TEST_F(TemporalLongSequenceEvidenceTest, ARenderScaleChangeStartsANewTAALineageOnTheForwardPaths)
    {
        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus })
            CheckResetPolicy(Estimator::TAA, path, ResetEvent::RenderScaleChange);
    }

    // The FSR1 preset resizes the scene band on every path, Deferred included,
    // so it reaches SSR's and SSGI's scene-band histories as a resize. Its
    // resumption half is the one that matters: the resized history must be
    // imported again the next frame, which the declaration key did not ask
    // for until the descriptor joined it (#1348).
    TEST_F(TemporalLongSequenceEvidenceTest, AnUpscalePresetChangeResetsAndResumesEveryResolve)
    {
        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
            CheckResetPolicy(Estimator::TAA, path, ResetEvent::UpscalePresetChange);
        CheckResetPolicy(Estimator::SSR, RenderingPath::Deferred, ResetEvent::UpscalePresetChange);
        CheckResetPolicy(Estimator::SSGI, RenderingPath::Deferred, ResetEvent::UpscalePresetChange);
    }

    TEST_F(TemporalLongSequenceEvidenceTest, ASamplingSequenceRestartStartsANewLineageOnEveryResolve)
    {
        CheckResetPolicy(Estimator::TAA, RenderingPath::Deferred, ResetEvent::SequenceRestart);
        CheckResetPolicy(Estimator::SSR, RenderingPath::Deferred, ResetEvent::SequenceRestart);
        CheckResetPolicy(Estimator::SSGI, RenderingPath::Deferred, ResetEvent::SequenceRestart);
    }

    // A paused scene is a still scene: the histories keep converging.
    TEST_F(TemporalLongSequenceEvidenceTest, PausingTheSceneKeepsEveryLineage)
    {
        CheckResetPolicy(Estimator::TAA, RenderingPath::Deferred, ResetEvent::Pause);
        CheckResetPolicy(Estimator::SSR, RenderingPath::Deferred, ResetEvent::Pause);
        CheckResetPolicy(Estimator::SSGI, RenderingPath::Deferred, ResetEvent::Pause);
    }

    // A shader edit that keeps the history layout keeps the history: the
    // editor's hot reload is ShaderLibrary::ReloadShaders. A layout edit is a
    // descriptor LayoutVersion change, pinned in TemporalHistoryRegistryTest.
    TEST_F(TemporalLongSequenceEvidenceTest, AShaderLibraryReloadKeepsEveryLineage)
    {
        CheckResetPolicy(Estimator::TAA, RenderingPath::Deferred, ResetEvent::ShaderLibraryReload);
        CheckResetPolicy(Estimator::SSR, RenderingPath::Deferred, ResetEvent::ShaderLibraryReload);
        CheckResetPolicy(Estimator::SSGI, RenderingPath::Deferred, ResetEvent::ShaderLibraryReload);
    }

    TEST_F(TemporalLongSequenceEvidenceTest, TogglingAResolveOffReleasesItsHistoryAndRestartsItsLineage)
    {
        CheckFeatureToggle(Estimator::TAA, RenderingPath::Forward);
        CheckFeatureToggle(Estimator::TAA, RenderingPath::Deferred);
        CheckFeatureToggle(Estimator::SSR, RenderingPath::Deferred);
        CheckFeatureToggle(Estimator::SSGI, RenderingPath::Deferred);
    }

    // The stale-history negative control. Both checks above must FAIL when no
    // invalidation reaches any history; a check that stays green with the
    // fault on is not a check.
    TEST_F(TemporalLongSequenceEvidenceTest, StaleHistoryNegativeControlFailsEveryResetCheck)
    {
        CheckResetNegativeControl(Estimator::TAA, RenderingPath::Deferred, ResetEvent::CameraCut);
        CheckResetNegativeControl(Estimator::TAA, RenderingPath::Forward, ResetEvent::ProjectionChange);
        CheckResetNegativeControl(Estimator::SSR, RenderingPath::Deferred, ResetEvent::CameraCut);
        CheckResetNegativeControl(Estimator::SSGI, RenderingPath::Deferred, ResetEvent::SequenceRestart);
    }
} // namespace OloEngine::Tests
