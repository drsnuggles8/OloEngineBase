#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <algorithm>
#include <array>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // The stochastic hash's seed. Fixed rather than random: two runs of the
        // same frame must produce the same picture, or every pixel A/B in the
        // verification matrix becomes a comparison of two noise fields.
        constexpr u32 kStochasticSeed = 1246;

    } // namespace

    GroomRenderPass::GroomRenderPass()
    {
        SetName("GroomRenderPass");
    }

    void GroomRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_Shader = Shader::Create("assets/shaders/GroomStrand.glsl");
        // ONE buffer on the shared PASS-LOCAL slot. See GroomStrandParamsUBO:
        // a second buffer on UBO_MODEL would have fought the engine's own
        // per-draw model UBO for binding 3, and a UniformBuffer claims its
        // binding point at construction.
        m_ParamsUBO =
            UniformBuffer::Create(UBOStructures::GroomStrandParamsUBO::GetSize(), ShaderBindingLayout::UBO_USER_0);

        // A 1x1x1 ZERO volume, bound whenever a draw has no coat volume of its
        // own. The shader declares the sampler unconditionally, and a dangling
        // sampler is undefined behaviour rather than a zero read -- so
        // something valid is bound ALWAYS and the routing lane, never the
        // binding, decides whether it is sampled. Same discipline, and the same
        // reason, as VolumetricFogPass's density-volume placeholder.
        //
        // Zero density also means a coat that somehow DID sample it would read
        // "no hair here" -- fully lit, the loud failure -- rather than a black
        // coat, which is indistinguishable from a correct silhouette.
        Texture3DSpecification placeholder;
        placeholder.Width = 1;
        placeholder.Height = 1;
        placeholder.Depth = 1;
        placeholder.Format = Texture3DFormat::RGBA32F;
        placeholder.Repeat = false;
        m_CoatPlaceholder = Texture3D::Create(placeholder);
        if (m_CoatPlaceholder)
        {
            const std::array<f32, 4> zero{ 0.0f, 0.0f, 0.0f, 0.0f };
            m_CoatPlaceholder->SetData(zero.data(), static_cast<u32>(zero.size() * sizeof(f32)));
        }
    }

    GroomCompositionDecision GroomRenderPass::DecideComposition(GroomCompositionMode requested) const noexcept
    {
        GroomCompositionInputs inputs;
        inputs.Requested = requested;
        // The RESOLVED target's sample count, not a setting's: DeferredSettings
        // clamps its request to the device at G-Buffer creation, and the scene
        // framebuffer this pass draws into is a different target again. Reading
        // the framebuffer is the only way to be describing the thing the draw
        // will actually land in.
        inputs.TargetSampleCount =
            m_SceneFramebuffer ? std::max(1u, m_SceneFramebuffer->GetSpecification().Samples) : 1u;
        // No branch on the backend here, and that is the honest state of
        // things: nothing in this engine sets a sample mask, so the answer is
        // the same on OpenGL and on Vulkan. The input exists so implementing it
        // later is one line in this function rather than a new branch in the
        // seam. See GroomCompositionFallbackReason::AlphaToCoverageUnimplemented.
        inputs.AlphaToCoverageSupported = false;
        inputs.TemporalResolveActive = m_FrameState.TemporalResolveActive;
        inputs.OITTargetsAvailable = m_FrameState.OITTargetsAvailable;
        // Strands are drawn into the scene colour target with depth, and the
        // transparent modifiers run after this pass, so this groom must leave
        // depth behind for them to compose against.
        inputs.RequiresDepthComposition = true;
        return SelectGroomComposition(inputs);
    }

    void GroomRenderPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);

        if (m_Requests.empty())
        {
            return;
        }

        if (board.Scene.SceneColor.IsValid())
        {
            // Inter-pass read-modify-write on SceneColor, the same shape
            // FoliageRenderPass uses: read the prior version, advertise a
            // renamed output, and order behind whoever wrote it last.
            SetPrimaryInputFramebufferHandle(board.Scene.SceneColor);
            [[maybe_unused]] const auto sceneColorRead =
                builder.Read(board.Scene.SceneColor, RGReadUsage::RenderTargetRead);
            constexpr std::string_view groomVersionTag = "GroomPass";
            [[maybe_unused]] const auto sceneColorNew =
                builder.WriteNewVersion(board.Scene.SceneColor, RGWriteUsage::RenderTarget, groomVersionTag);
            builder.DependsOnPreviousWriter(ResourceNames::SceneColor);
        }
    }

    void GroomRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        m_Stats.Reset();
        m_Stats.GroomsSubmitted = static_cast<u32>(m_Requests.size());

        // THE CACHE IS NOT THIS PASS'S ANY MORE (#1323). RenderPipeline owns
        // it, advances its tick once per frame and hands the same instance to
        // ShadowRenderPass, which acquires from it BEFORE this pass runs —
        // which is the whole point, because a groom caster needs its buffers to
        // exist while the shadow map is being rasterised. Without one there is
        // nothing to draw from, and building a private one here would give the
        // shadow pass a cache it could never see.
        if (m_Cache == nullptr)
        {
            if (!m_WarnedNoCache && !m_Requests.empty())
            {
                m_WarnedNoCache = true;
                OLO_CORE_ERROR_TAG("Groom",
                                   "GroomRenderPass has no strand cache; {} groom requests were dropped. "
                                   "RenderPipeline::CreateFramePasses is what wires it.",
                                   m_Requests.size());
            }
            m_Requests.clear();
            return;
        }

        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
        {
            if (auto resolved = context.ResolveFramebuffer(sceneHandle))
            {
                m_SceneFramebuffer = resolved;
            }
        }

        if (m_Requests.empty() || !m_SceneFramebuffer || !m_Shader || !m_ParamsUBO)
        {
            m_Requests.clear();
            m_Stats.CachedBytes = m_Cache->GetBytes();
            m_Stats.CachedGrooms = m_Cache->GetEntryCount();
            return;
        }

        // The coat-volume placeholder is a PRECONDITION, not a nicety -- see the
        // bind in the draw loop. A device that could not create a 1x1x1 RGBA
        // volume cannot serve this pass safely, so say so and draw nothing
        // rather than binding a descriptor of the wrong shape to a sampler3D.
        //
        // CHECKED HERE, before anything is bound, and taking the same exit the
        // empty-request case takes. An early return further down would leave
        // the framebuffer bound, the depth state changed and the request list
        // un-cleared -- a pass that fails safe must not leave the next one to
        // discover it.
        if (!m_CoatPlaceholder)
        {
            OLO_CORE_ERROR_TAG("Groom",
                               "GroomRenderPass has no coat-shadow placeholder volume; skipping the strand draws "
                               "rather than binding a null 3D sampler.");
            m_Requests.clear();
            m_Stats.CachedBytes = m_Cache->GetBytes();
            m_Stats.CachedGrooms = m_Cache->GetEntryCount();
            return;
        }

        m_SceneFramebuffer->Bind();

        const auto& spec = m_SceneFramebuffer->GetSpecification();
        context.SetViewport(0, 0, spec.Width, spec.Height);
        // Depth test AND depth write: this is what makes the coat compose
        // against the body and against ordinary opaque geometry through the
        // ordinary depth test, which is acceptance criterion 2. Blending is
        // OFF — every implemented mode resolves its coverage by discarding
        // whole fragments, so a blend would double-count what the discard
        // already decided.
        context.SetDepthTest(true);
        context.SetDepthMask(true);
        context.SetBlendState(false);
        // Ribbons are two-sided by construction: a camera-facing quad has no
        // meaningful winding, and culling one side halves the coat.
        context.SetCulling(false);
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);

        // All five scene MRT attachments. Writing a subset would leave the
        // others undefined, and attachment 4 in particular is blurred into
        // scene colour by SkinDiffusion.glsl — see glsl-shaders.md §4.
        constexpr std::array<u32, 5> kAttachments{ 0u, 1u, 2u, 3u, 4u };
        context.SetDrawBuffers(kAttachments);

        m_Shader->Bind();

        // The scene's global IBL, bound EXPLICITLY rather than inherited — the
        // same decision, for the same reason, as the foliage draw's (see
        // CommandDispatch::DrawFoliageInstances). In a frame with lit meshes
        // TEX_USER_0 would usually already hold the irradiance cube, and
        // "usually" is the problem: reading it from Renderer3D's global IBL
        // state makes a coat's ambient independent of what drew before it.
        //
        // Cube, so the typed null kind matters: with no environment bound the
        // slot must answer as a black CUBE, and a 2D null sampler on a
        // samplerCube declaration is undefined behaviour, not a black read.
        // Persistent lifetime because the map is asset-owned rather than
        // graph-owned, so its heap offset is worth memoising.
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_USER_0, Renderer3D::GetGlobalIrradianceMapHandle(),
                                        RHI::HeapSlotLifetime::Persistent, {}, RHI::NullSamplerKind::Cube);

        // The scene's shadow inputs (#1323): the CSM array, the local-light
        // atlas, their comparison-off raw views for the PCSS blocker search,
        // and the Virtual Shadow Map's sampling publish.
        //
        // THROUGH CommandDispatch, not re-implemented here. Those four units
        // carry a specific sampler state AND a specific typed null kind, and
        // every site that stages a shadow-map offset must agree about both or
        // whichever pass ran last silently wins (issue #691). BindForSampling
        // inside it is equally load-bearing: VirtualShadowResources.glsl is a
        // shared header this shader now includes, so its buffers must have an
        // occupant even when VSM is off or the RHI substitutes a null block and
        // logs an error per shader (#1190).
        //
        // UNCONDITIONAL, not gated on a groom asking to receive. It publishes
        // into the SAME heap-offset table the flush below drains, so making it
        // conditional would make the table's contents depend on which grooms
        // happened to be in the frame -- and the declarations in the shader are
        // unconditional either way. The routing lane (u_GroomCoatModes.y), not
        // the binding, decides whether any of it is sampled.
        CommandDispatch::BindSceneShadowTextures();
        context.FlushHeapOffsets();

        // Camera-relative rendering (#429). Every world matrix the GPU sees is
        // relative to this point, including the one behind u_ViewProjection
        // (RenderPipeline.cpp's MakeViewProjectionRelative) and the light
        // positions in the multi-light UBO (Renderer3D::UploadMultiLightUBO).
        // A groom whose model matrix stayed absolute would be drawn offset from
        // the body it grows on, and lit from a direction shifted by the same
        // amount — invisible near the world origin, which is where every test
        // scene sits, and wrong everywhere else.
        const glm::vec3 renderOrigin = Renderer3D::GetRenderOrigin();

        // Coat volumes resident across the WHOLE cache, against
        // kMaxResidentCoatVolumes. Seeded from the cache rather than from zero:
        // a groom that stopped being visible still holds its volume, and
        // counting only this frame's draws let the resident set grow past the
        // cap indefinitely while BudgetExhausted stayed at zero.
        //
        // AcquireCoatVolume maintains it from here — it is the only place that
        // creates or reclaims one.
        u32 residentCoatVolumes = m_Cache->CountResidentCoatVolumes();

        for (const auto& request : m_Requests)
        {
            if (!request.Groom)
            {
                continue;
            }

            const GroomCompositionDecision decision = DecideComposition(request.RequestedMode);
            m_Stats.Composition.Record(decision);

            // Counted BEFORE the geometry is acquired, so a refused binding is
            // reported even on a frame whose groom produced no geometry at all.
            // Counting it after the `continue` below is how a counter that means
            // "a coat is stuck at its bind pose" reads zero on exactly the
            // frames it matters.
            if (request.BindingReject != GroomBindingRejectReason::None)
            {
                ++m_Stats.GroomsBindingRefused;
            }
            if (GroomStrandCache::IsDeformed(request))
            {
                ++m_Stats.GroomsDeformed;
                m_Stats.RootsDeformed += request.DeformationStats.RootsDeformed;
                m_Stats.RootsHeldAtRest += request.DeformationStats.RootsHeldDegenerate;
                if (!request.DeformationStats.HasHistory)
                {
                    ++m_Stats.GroomsHistoryRejected;
                }
            }

            // The guide simulation (#1250), counted HERE and for the same
            // reason the binding is: a coat whose solver refused produced no
            // geometry, and a counter that means "this coat is not moving"
            // must not read zero on exactly the frames it matters.
            if (request.Influence)
            {
                const GroomSimulationStats& simulation = request.SimulationStats;
                ++m_Stats.GroomsSimulated;
                m_Stats.GuidesSimulated += simulation.GuidesSimulated;
                m_Stats.GuidePointsSimulated += simulation.PointsSimulated;
                m_Stats.SimulationContacts += simulation.ContactsResolved;
                m_Stats.GuidesWithHeldRoots += simulation.GuidesWithHeldRoots;
                m_Stats.SimulationSteps += simulation.StepsTaken;
                m_Stats.SimulationStepsClamped =
                    m_Stats.SimulationStepsClamped || simulation.StepsClamped;
                if (simulation.Reseeded)
                {
                    ++m_Stats.SimulationReseeds;
                }
                // The WORST over every groom, and the TIGHTEST tolerance any of
                // them declared: one coat out of contract must not be averaged
                // away by nine that are in it.
                if (std::abs(simulation.MaxStretchRatio - 1.0f) >
                    std::abs(m_Stats.WorstStretchRatio - 1.0f))
                {
                    m_Stats.WorstStretchRatio = simulation.MaxStretchRatio;
                }
                m_Stats.WorstRestDeviation =
                    std::max(m_Stats.WorstRestDeviation, simulation.MaxRestDeviation);
                m_Stats.DeclaredStretchTolerance =
                    std::min(m_Stats.DeclaredStretchTolerance, request.SimulationStretchTolerance);
            }

            GroomStrandCache::Entry* entry = m_Cache->AcquireGeometry(request);
            if (entry == nullptr || !entry->Array)
            {
                continue;
            }

            // Coat self-shadowing (#1248). The bake, the shadow LOD and the
            // decision all happen here because this is the only place that
            // knows what the frame actually resolved -- the same
            // producer/transport/consumer split the composition mode uses.
            const GroomCoatShadowDecision coatDecision =
                m_Cache->AcquireCoatVolume(request, *entry, residentCoatVolumes,
                                           static_cast<f32>(spec.Height));
            m_Stats.CoatShadow.Record(coatDecision);
            // From the BUILD, not from the request: the interpolation happens
            // inside BuildGroomStrandMesh, so it is the only place that knows
            // how many strands actually took a guide displacement (#1250).
            m_Stats.StrandsSimulated += entry->Stats.StrandsSimulated;
            m_Stats.StrandsUnguided += entry->Stats.StrandsUnguided;
            const bool coatActive =
                coatDecision.Effective != GroomCoatShadowTechnique::None && entry->CoatVolume != nullptr;
            if (coatActive)
            {
                m_Stats.CoatShadow.ResidentBytes += entry->CoatBytes;
                m_Stats.CoatShadow.ResolutionInForce = std::max(m_Stats.CoatShadow.ResolutionInForce,
                                                                entry->CoatResolution);
                m_Stats.CoatShadow.LodStepInForce = std::max(m_Stats.CoatShadow.LodStepInForce, entry->CoatLodStep);
                m_Stats.CoatShadow.MaxAgeFrames = std::max(
                    m_Stats.CoatShadow.MaxAgeFrames,
                    static_cast<u32>(m_Cache->GetTick() - entry->CoatBuiltTick));
            }

            // ALWAYS A REAL 3D TEXTURE, never a null handle. The shader
            // declares the sampler unconditionally, and NullSamplerKind has no
            // Texture3D arm -- so a null here would hand a 2D null descriptor
            // to a sampler3D declaration, which is undefined behaviour rather
            // than a black read (the same trap the irradiance cube's explicit
            // Cube kind exists to avoid). The placeholder is therefore a
            // PRECONDITION of drawing at all: Execute refuses to run without
            // one rather than binding something of the wrong shape.
            context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_GROOM_COAT_VOLUME,
                                            coatActive ? entry->CoatVolume->GetRHIHandle()
                                                       : m_CoatPlaceholder->GetRHIHandle(),
                                            RHI::HeapSlotLifetime::Persistent);
            context.FlushHeapOffsets();

            // ── The coverage compensation (#1252) ───────────────────
            //
            // SHARED WITH THE SHADOW CASTER (#1323). The expression moved to
            // GroomStrandCache::EffectiveWidthScale so the ribbons this pass
            // draws and the ribbons ShadowRenderPass rasterises from the light
            // are the same thickness; two copies would drift and the symptom
            // would be a shadow slightly the wrong size, which nobody reads as
            // a bug.
            const f32 effectiveWidthScale = GroomStrandCache::EffectiveWidthScale(request, *entry);
            const f32 widthCompensation =
                request.WidthScale > 0.0f ? effectiveWidthScale / request.WidthScale : 1.0f;

            // ── The LOD counters (#1252) ────────────────────────────
            //
            // Recorded HERE rather than where the decision was made, because
            // criterion 4 asks for cost and memory BY REPRESENTATION and only
            // this point knows both the tier and what it cost. A groom that
            // produced no geometry never reaches here and is therefore not
            // counted against a representation it did not draw.
            const auto tier = static_cast<sizet>(request.Lod.Representation);
            m_Stats.Lod.Record(request.Lod);
            m_Stats.Lod.StrandsByRepresentation[tier] += entry->Stats.StrandsSelected;
            // ONCE PER ENTRY PER FRAME, not once per draw: see
            // GroomStrandCache::Entry::BytesCountedTick. The tick is the
            // cache's own monotonic counter, so an entry drawn by two entities
            // this frame contributes its allocation once and the figure stays a
            // RESIDENT byte count rather than a sum of draws.
            if (entry->BytesCountedTick != m_Cache->GetTick())
            {
                entry->BytesCountedTick = m_Cache->GetTick();
                m_Stats.Lod.BytesByRepresentation[tier] += entry->Bytes;
            }
            m_Stats.Lod.MaxWidthCompensation =
                std::max(m_Stats.Lod.MaxWidthCompensation, widthCompensation);
            // A coat AT the cap is genuinely thinner than it was authored, and
            // criterion 1 is a claim about exactly that. The comparison is
            // against the sanitised policy's cap, so it cannot be true because
            // an author typed a NaN.
            if (widthCompensation >= request.LodPolicy.MaxWidthCompensation && widthCompensation > 1.0f)
            {
                ++m_Stats.Lod.GroomsAtCompensationCap;
            }

            const f32 axisX = glm::length(glm::vec3(request.Transform[0]));
            const f32 axisY = glm::length(glm::vec3(request.Transform[1]));
            const f32 axisZ = glm::length(glm::vec3(request.Transform[2]));
            // The mean axis length, matching GroomCoverage::ProjectGroom and
            // AlembicGroomImporter's convention for a non-uniform transform.
            // One scalar cannot describe an anisotropically scaled strand, and
            // the three places that scale a width must at least be wrong the
            // same way.
            const f32 objectScale = (axisX + axisY + axisZ) / 3.0f;

            UBOStructures::GroomStrandParamsUBO params;
            params.Model = MakeModelRelative(request.Transform, renderOrigin);
            // The SAME origin for both, not last frame's: the previous-frame
            // clip matrix is itself made relative with the current origin
            // (RenderPipeline.cpp), so a previous model relative to a different
            // point would emit a velocity equal to the origin's jump on every
            // cell crossing — a whole-screen smear once per grid cell.
            params.PrevModel = MakeModelRelative(request.PreviousTransform, renderOrigin);
            params.Color = glm::vec4(request.Color, 1.0f);
            params.IDs = glm::ivec4(request.EntityID, 0, 0, 0);
            params.Viewport = glm::vec4(static_cast<f32>(spec.Width), static_cast<f32>(spec.Height), 0.0f, 0.0f);
            params.RampWidth =
                glm::vec4(request.RampFloor, effectiveWidthScale, objectScale, request.AlphaCutoff);
            params.ModeFrame = glm::ivec4(static_cast<i32>(decision.Effective),
                                          static_cast<i32>(m_FrameState.FrameIndex),
                                          static_cast<i32>(kStochasticSeed), 0);

            // The DERIVED fibre parameters, straight across. Nothing is
            // recomputed here: Scene built them once with the same
            // MakeGroomFibreParams the tests and the analysis call, so the
            // shader, the CPU model and the measured numbers cannot drift.
            params.FibreSigmaEta = glm::vec4(request.Fibre.SigmaA, request.Fibre.Eta);
            params.FibreLobe = glm::vec4(request.Fibre.V[0], request.Fibre.S, request.Fibre.Intensity, 0.0f);
            params.FibreSinAlpha = glm::vec4(request.Fibre.Sin2kAlpha[0], request.Fibre.Sin2kAlpha[1],
                                             request.Fibre.Sin2kAlpha[2], 0.0f);
            params.FibreCosAlpha = glm::vec4(request.Fibre.Cos2kAlpha[0], request.Fibre.Cos2kAlpha[1],
                                             request.Fibre.Cos2kAlpha[2], 0.0f);
            params.FibreModes = glm::ivec4(request.Lit ? 1 : 0, static_cast<i32>(request.Fibre.HSamples),
                                           static_cast<i32>(request.FibreDebug), 0);

            // THE RECEIVE LANE GOES UP ON EVERY DRAW (#1323), outside the
            // block below: whether this coat samples the scene's shadow is a
            // different question from whether it has a density volume.
            params.CoatModes.y = request.ReceivesSceneShadow ? 1 : 0;

            // The coat's OBJECT BOX serves two independent consumers, so it is
            // filled when EITHER wants it:
            //
            //   * the density march (#1248), gated by CoatModes.x, which needs
            //     a built and bound volume; and
            //   * the receiver OFFSET (#1323), gated by CoatModes.z, which
            //     needs only that this groom is a CASTER.
            //
            // THE SECOND GATE IS CASTING AND NOT THE VOLUME, and that was
            // measured rather than reasoned: with the offset tied to the
            // volume, a caster with no volume kept the fragment as its
            // receiver and the evidence coat fell from 44.98 mean luma to
            // 0.22. A shadow map is a BINARY visibility test and a coat is
            // not binary, so every strand behind the outermost widened ribbon
            // read as fully shadowed. A black coat is the silent failure
            // groom-coat-self-shadowing.md rule 10 forbids; replacing the
            // map's occlusion with a GRADED one is the volume's job, and a
            // coat without one is #1247's unshadowed picture, which is a
            // legitimate state.
            //
            // Everything still stays at its INACTIVE default when neither
            // consumer wants it -- the structural fallback
            // technique-selection-seams.md asks for, rather than a flag
            // someone has to remember to reset on each early return.
            const bool wantsObjectBox = coatActive || request.CastsSceneShadow;
            if (wantsObjectBox)
            {
                // RIGID world -> object. Built from the RENDER-RELATIVE model
                // matrix, because v_WorldPos in the shader is render-relative
                // too (issue #429); inverting the absolute transform instead
                // would march from a point offset by the render origin, which
                // is invisible near the world origin -- where every test scene
                // sits -- and wrong everywhere else.
                //
                // The scale is divided out rather than inverted with it: the
                // march compares ANGLES in this space against each voxel's mean
                // fibre direction, and a scale in the rotation would tilt every
                // fibre by an amount that depends on which way the ray points.
                glm::mat4 modelRelative = params.Model;
                glm::vec3 axisLengths{ glm::length(glm::vec3(modelRelative[0])),
                                       glm::length(glm::vec3(modelRelative[1])),
                                       glm::length(glm::vec3(modelRelative[2])) };
                const bool degenerate = axisLengths.x <= 1.0e-8f || axisLengths.y <= 1.0e-8f ||
                                        axisLengths.z <= 1.0e-8f;
                if (!degenerate)
                {
                    glm::mat3 rotation(glm::vec3(modelRelative[0]) / axisLengths.x,
                                       glm::vec3(modelRelative[1]) / axisLengths.y,
                                       glm::vec3(modelRelative[2]) / axisLengths.z);
                    const glm::vec3 translation = glm::vec3(modelRelative[3]);
                    const glm::mat3 inverseRotation = glm::transpose(rotation);
                    // The object-space point is recovered at the OBJECT scale
                    // the bake used, so the mean axis length divides here and
                    // the bounds below stay in the units BuildCoatSegments
                    // emitted.
                    const f32 meanScale = (axisLengths.x + axisLengths.y + axisLengths.z) / 3.0f;
                    glm::mat4 worldToObject(1.0f);
                    const glm::mat3 scaled = inverseRotation / meanScale;
                    worldToObject[0] = glm::vec4(scaled[0], 0.0f);
                    worldToObject[1] = glm::vec4(scaled[1], 0.0f);
                    worldToObject[2] = glm::vec4(scaled[2], 0.0f);
                    worldToObject[3] = glm::vec4(-(scaled * translation), 1.0f);

                    // THE VOLUME'S BOX WHEN THERE IS ONE, the strand build's
                    // otherwise. Both are in the same object space and both
                    // bound the same coat; the volume's is the one the march
                    // indexes, so it has to win wherever a march is happening.
                    // The build's box is the coat in THIS pose, which is the
                    // only box a deformed groom may use.
                    const bool haveBuildBounds = entry->Stats.BoundsValid;
                    const glm::vec3 boundsMin = coatActive ? entry->CoatBoundsMin : entry->Stats.BoundsMin;
                    const glm::vec3 boundsMax = coatActive ? entry->CoatBoundsMax : entry->Stats.BoundsMax;
                    const glm::vec3 extent = boundsMax - boundsMin;
                    if ((coatActive || haveBuildBounds) && extent.x > 0.0f && extent.y > 0.0f &&
                        extent.z > 0.0f)
                    {
                        const f32 voxelLength = entry->CoatVoxelSize;

                        params.CoatWorldToObject = worldToObject;
                        params.CoatBoundsMin = glm::vec4(boundsMin, request.CoatKappa);
                        params.CoatInvExtent =
                            glm::vec4(1.0f / extent, voxelLength * request.CoatStepVoxels);
                        // The box is usable, so the receiver may be offset by
                        // it. Written from the SUCCESS path, like every other
                        // lane here: a box that failed to resolve leaves this
                        // zero and the receiver stays at the fragment.
                        params.CoatModes.z = request.CastsSceneShadow ? 1 : 0;
                        // .x ONLY, and only with a volume: .y and .z are the
                        // scene-shadow lanes, and a whole-vector assign here
                        // would silently clear them.
                        if (coatActive)
                        {
                            params.CoatModes.x = static_cast<i32>(coatDecision.Effective);
                        }
                    }
                }
            }
            if (request.Lit)
            {
                ++m_Stats.GroomsLit;
                m_Stats.FibreSamplesPerFragment = std::max(m_Stats.FibreSamplesPerFragment, request.Fibre.HSamples);
            }
            // UPLOAD, THEN BIND — in that order, every draw, the shape
            // CloudscapeRenderPass::UploadAndBindUBO established.
            //
            // Both halves are load-bearing and each was learned from a frame
            // that drew nothing:
            //
            //   * BINDING at all, because a UniformBuffer claims its binding
            //     point at CONSTRUCTION and nothing rebinds it, so by the time
            //     this pass runs the slot belongs to whoever constructed a
            //     buffer on it last. Without it the strand draws read another
            //     pass's bytes on OpenGL.
            //   * The ORDER, because the Vulkan backend's UBOs are
            //     arena-versioned: SetData mints a NEW allocation (ADR 0011
            //     §4), so binding first publishes the address of the PREVIOUS
            //     one. The draw then reads a zero viewport, every ribbon is
            //     widened by a garbage factor and lands off-screen, and the
            //     pass reports thousands of segments drawn while changing not
            //     one pixel — on Vulkan only, with no error anywhere.
            m_ParamsUBO->SetData(&params, UBOStructures::GroomStrandParamsUBO::GetSize());
            m_ParamsUBO->Bind();

            entry->Array->Bind();
            context.DrawIndexed(entry->Array, entry->Stats.IndexCount);

            ++m_Stats.GroomsDrawn;
            m_Stats.StrandsDrawn += entry->Stats.StrandsSelected;
            m_Stats.SegmentsDrawn += entry->Stats.SegmentCount;
            m_Stats.TrianglesDrawn += entry->Stats.SegmentCount * 2u;
        }

        // Leave the state as the next pass expects to find it. The depth func
        // is reset for the same reason FoliageRenderPass resets it: a pass
        // that leaves a non-default compare behind breaks a later one in a way
        // that looks like the later one's bug.
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
        CommandDispatch::InvalidateRenderStateCache();
        // This pass bound TEX_USER_0 through the RGCommandContext seam, which
        // does not go through the dispatcher's redundant-bind cache — so that
        // cache still believes whatever the last command-queue draw left there.
        // A later pass binding its own texture to the same slot would find a
        // matching entry and skip the bind, and would then sample this pass's
        // irradiance cube. Telling the cache the slot was clobbered is the
        // documented way out (CommandDispatch::InvalidateTextureSlot).
        CommandDispatch::InvalidateTextureSlot(ShaderBindingLayout::TEX_USER_0);
        m_SceneFramebuffer->Unbind();

        // A change of dominant reason, not a per-frame line: the same warning
        // every frame is a log flood, and the thing worth knowing is that the
        // answer moved.
        const auto dominant = m_Stats.Composition.DominantFallbackReason();
        if (dominant != m_LastReportedReason)
        {
            m_LastReportedReason = dominant;
            if (dominant != GroomCompositionFallbackReason::None)
            {
                OLO_CORE_INFO("GroomRenderPass: {} of {} grooms are not on their requested composition mode — {}",
                              m_Stats.Composition.GroomsFellBack, m_Stats.Composition.GroomsConsidered,
                              ToString(dominant));
            }
        }

        // NO EVICTION HERE. The shared cache evicts at the TOP of a frame
        // (GroomStrandCache::BeginFrame), because since #1323 this pass is no
        // longer the last groom consumer of the frame — the shadow pass has
        // already recorded draws against these same buffers.
        m_Stats.CachedBytes = m_Cache->GetBytes();
        m_Stats.CachedGrooms = m_Cache->GetEntryCount();
        m_Stats.CacheBuilds = m_Cache->GetStats().CacheBuilds;
        m_Stats.CacheEvictions = m_Cache->GetStats().CacheEvictions;
        m_Stats.DeformedRebuilds = m_Cache->GetStats().DeformedRebuilds;
        m_Stats.CoatShadow.Rebuilds = m_Cache->GetStats().CoatRebuilds;
        // The CASTING half of #1323, written by ShadowRenderPass earlier in
        // this same frame. Read back here so the editor has ONE groom readout
        // that answers both directions -- "why does this coat cast no shadow"
        // has to be answerable from the panel that asks for the shadow.
        m_Stats.SceneShadow = m_Cache->GetShadowStats();

        // ONE LINE, ON A CHANGE. The same discipline as the composition
        // fallback above: the same numbers every frame are a log flood, and
        // the thing worth knowing is that the answer moved. What makes it
        // worth logging at all is that a caster family reaches a technique
        // only if somebody wired it there and NOTHING else detects the gap --
        // so a zero in one of the three technique counters, beside a non-zero
        // GroomsCasting, is the detector, and it has to be readable without
        // the inspector.
        const GroomShadowCasterStats& shadowStats = m_Stats.SceneShadow;
        const bool tallyMoved = shadowStats.GroomsAskedToCast != m_LastReportedShadowStats.GroomsAskedToCast ||
                                shadowStats.GroomsCasting != m_LastReportedShadowStats.GroomsCasting ||
                                shadowStats.GroomsWithoutGeometry != m_LastReportedShadowStats.GroomsWithoutGeometry ||
                                (shadowStats.CascadeDraws > 0u) != (m_LastReportedShadowStats.CascadeDraws > 0u) ||
                                (shadowStats.AtlasDraws > 0u) != (m_LastReportedShadowStats.AtlasDraws > 0u) ||
                                (shadowStats.VirtualShadowLevelDraws > 0u) !=
                                    (m_LastReportedShadowStats.VirtualShadowLevelDraws > 0u) ||
                                shadowStats.VirtualShadowMapActive != m_LastReportedShadowStats.VirtualShadowMapActive;
        if (tallyMoved && shadowStats.GroomsAskedToCast > 0u)
        {
            m_LastReportedShadowStats = shadowStats;
            OLO_CORE_INFO("GroomRenderPass: {} of {} groom(s) casting ({} had no geometry) — draws: {} cascade, "
                          "{} virtual-shadow level, {} atlas; the frame's directional technique is {}",
                          shadowStats.GroomsCasting, shadowStats.GroomsAskedToCast,
                          shadowStats.GroomsWithoutGeometry, shadowStats.CascadeDraws,
                          shadowStats.VirtualShadowLevelDraws, shadowStats.AtlasDraws,
                          shadowStats.VirtualShadowMapActive ? "the Virtual Shadow Map" : "the CSM cascades");
        }
        else if (tallyMoved)
        {
            m_LastReportedShadowStats = shadowStats;
        }

        // Requests are per-frame; holding them would draw last frame's grooms
        // on a frame that published none.
        m_Requests.clear();
    }

    Ref<Framebuffer> GroomRenderPass::GetTarget() const
    {
        return m_SceneFramebuffer;
    }

    void GroomRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void GroomRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void GroomRenderPass::OnReset()
    {
        // The cache holds GPU buffers whose device is going away. It is NOT
        // this pass's to drop since #1323 — RenderPipeline owns it and clears
        // it on the same reset, once, so two passes cannot each decide to.
        m_Requests.clear();
        m_SceneFramebuffer = nullptr;
        m_LastReportedReason = GroomCompositionFallbackReason::None;
        m_LastReportedShadowStats.Reset();
    }
} // namespace OloEngine
