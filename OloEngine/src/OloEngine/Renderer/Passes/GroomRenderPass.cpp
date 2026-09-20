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

        // How many coat volumes may be resident at once. A budget rather than
        // "as many as ask", because the representation is a 3D texture whose size
        // grows with the CUBE of its resolution, so a scene full of coats could
        // otherwise spend a gigabyte with nothing saying so. A coat that does not
        // get a slot falls back unshadowed with a counted reason
        // (BudgetExhausted), which is the loud answer.
        constexpr u32 kMaxResidentCoatVolumes = 8;

        // Releasing a coat volume is THREE steps that must never be separated:
        // give the bytes back to the cache total, drop the texture, and clear
        // the resolution that says a bake is resident. Done by hand at each of
        // the four sites that release one, the accounting drifted — EvictToBudget
        // subtracted only the geometry's bytes, so every evicted coat left
        // m_CacheBytes permanently inflated and the pass could evict healthy
        // entries forever while reporting itself over budget.
        template<typename EntryT>
        void ReleaseCoatVolume(EntryT& entry, u64& cacheBytes) noexcept
        {
            cacheBytes -= std::min(cacheBytes, entry.CoatBytes);
            entry.CoatVolume = nullptr;
            entry.CoatResolution = 0;
            entry.CoatBytes = 0;
        }

        // Frames an unused cache entry survives before eviction is allowed to
        // consider it. One second at 60 fps — long enough that toggling a
        // groom's visibility does not rebuild its buffers, short enough that a
        // scene switch does not hold the old scene's grooms resident.
        constexpr u32 kCacheRetentionFrames = 60;

        [[nodiscard]] BufferLayout StrandVertexLayout()
        {
            // Mirrors GroomStrandVertex and GroomStrand.glsl's attribute
            // block. On Vulkan none of this is used — ADR 0011 §5 leaves that
            // backend with no vertex input state and the shader pulls the same
            // bytes by index — so the two descriptions have to agree by the
            // struct's size, which GroomStrandMesh.h static_asserts.
            return BufferLayout{ { ShaderDataType::Float3, "a_Position" },
                                 { ShaderDataType::Float3, "a_Other" },
                                 { ShaderDataType::Float, "a_Side" },
                                 { ShaderDataType::Float, "a_Radius" },
                                 { ShaderDataType::Float2, "a_Coords" },
                                 { ShaderDataType::Float, "a_SegmentId" },
                                 { ShaderDataType::Float, "a_Tint" },
                                 // #1249: last frame's centreline point. Declared
                                 // even though an unbound groom writes it equal to
                                 // a_Position, because a layout that varied with
                                 // the binding would make NVIDIA specialize a
                                 // vertex-shader variant per groom (GL debug id
                                 // 131218) and would need a second shader.
                                 { ShaderDataType::Float3, "a_PrevPosition" },
                                 { ShaderDataType::Float, "a_Pad1" } };
        }
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

    u64 GroomRenderPass::CacheKey(const GroomStrandRequest& request) noexcept
    {
        // Handle AND settings. Two entities may reference one groom asset at
        // different budgets — the same asset at two LODs is the obvious
        // authoring case — and keying on the handle alone made each of their
        // draws evict the other, rebuilding the CPU mesh and both GPU buffers
        // twice per frame for as long as both were visible.
        //
        // FIELD BY FIELD, never over the object representation.
        // GroomStrandBuildSettings is 9 bytes of members in 12, and the default
        // member initializers do not touch the three padding bytes — Scene
        // default-constructs the request and assigns only the named fields, so
        // two logically identical settings can carry different padding and hash
        // differently. That misses the cache, rebuilds the geometry and leaves a
        // duplicate set of GPU buffers behind: precisely the failure this key
        // was widened to prevent.
        //
        // The cost is that a field added to the struct must be added here too.
        // GroomStrandMeshTest.TheCacheKeySeparatesSettingsThatProduceDifferentMeshes
        // is what catches forgetting.
        const auto mix = [](u64 key, u64 value)
        {
            key ^= value;
            key *= 1099511628211ull; // FNV-1a prime, as elsewhere in the groom code
            return key;
        };
        u64 key = static_cast<u64>(request.Handle);
        key = mix(key, static_cast<u64>(request.Build.MaxStrands));
        key = mix(key, static_cast<u64>(request.Build.MaxSegments));
        key = mix(key, request.Build.GuidesOnly ? 1ull : 0ull);
        // The coat authoring (#1251), as one digest. It is a FIELD of the build
        // settings for exactly this reason: the geometry a coat produces depends
        // on every slider on GroomCoatComponent and on the CONTENT of both root-UV
        // maps, and none of that could live in a key that only saw the budget.
        // GroomCoatDigest folds the maps in by content hash, so repainting one in
        // place rebuilds the coat instead of serving the strands the old pixels
        // made.
        key = mix(key, request.Build.CoatDigest);
        // The REPRESENTATION (#1252). A card level and the base groom are two
        // different curve sets, and at the same budget they would otherwise
        // hash to the same key -- so a coat that handed over to cards would be
        // served the strand geometry it had a moment ago, and the hand-over
        // would do nothing at all until something else happened to evict it.
        // The value is the tier rather than a pointer, so two entities on the
        // same tier of the same asset still share one buffer.
        key = mix(key, static_cast<u64>(std::to_underlying(request.Lod.Representation)));
        // A DEFORMED groom's vertices depend on a body's pose, so its geometry
        // is per ENTITY: two characters sharing one groom asset at one budget
        // must not share one buffer. Mixing the entity id in only on the
        // deformed arm keeps the unbound key, and therefore every unbound
        // groom's sharing, exactly as it was.
        if (IsDeformed(request))
        {
            key = mix(key, static_cast<u64>(static_cast<u32>(request.EntityID)));
            key = mix(key, 0x1249ull);
        }
        return key;
    }

    bool GroomRenderPass::IsDeformed(const GroomStrandRequest& request) noexcept
    {
        // The transform array must span the whole groom, not merely be
        // non-empty: the build indexes it by curve, and a short array would read
        // past its end on the first strand past the boundary.
        return request.Groom && request.Binding &&
               request.RootTransforms.size() == request.Groom->GetCurveCount();
    }

    GroomRenderPass::CacheEntry* GroomRenderPass::AcquireGeometry(const GroomStrandRequest& request)
    {
        const bool deformed = IsDeformed(request);
        const u64 key = CacheKey(request);

        const auto existing = m_Cache.find(key);
        if (existing != m_Cache.end())
        {
            // The settings are in the KEY, so a hit is already a settings
            // match; the comparison survives only to catch a hash collision,
            // which would otherwise hand back geometry built for a different
            // budget.
            const bool usable = existing->second.Settings == request.Build && existing->second.Array &&
                                existing->second.Dynamic == deformed;
            if (usable && !deformed)
            {
                existing->second.LastUsedFrame = m_CacheTick;
                return &existing->second;
            }
            if (!usable)
            {
                m_CacheBytes -= existing->second.Bytes;
                m_Cache.erase(existing);
            }
        }

        // A bound groom is rebuilt EVERY frame and its buffers are refilled in
        // place. There is no skip-if-unchanged here, deliberately: the pose that
        // would have to be compared lives on the tick thread and is rewritten
        // before this pass runs, so a comparison made here would be against the
        // wrong frame's palette. The producer is where a skip belongs if one is
        // ever wanted, and it has the pose (see
        // RayTracing::DeformedSurfaceCache, which does exactly that).
        GroomStrandDeformation deformation;
        if (deformed)
        {
            deformation.Binding = request.Binding.Raw();
            deformation.RootTransforms = request.RootTransforms;
        }

        std::vector<GroomStrandVertex> vertices;
        std::vector<u32> indices;
        // The coat context is assembled HERE, at the point of use, because it is
        // the only place both halves are certainly alive: the settings come from
        // the request and the per-group table belongs to the asset the request
        // holds. It is a view — a span over the asset's own table and a pointer
        // to the request's settings — so nothing is copied per build.
        const GroomCoatContext coat{ &request.Coat, request.Groom->GetGroupCoats() };
        // The guide simulation (#1250), assembled at the point of use for the
        // reason the coat context is: the request is MOVED into the frame's
        // request vector, so a span stored beside it would have dangled the
        // moment that vector reallocated. See GroomStrandRequest::Simulation.
        //
        // NO CACHE-KEY CHANGE IS NEEDED for it, and that is a consequence rather
        // than an omission: a simulated groom is always a BOUND one (Scene steps
        // the solver inside DeformGroomAgainstSurface, against the root
        // transforms), so IsDeformed is already true, the entry is already keyed
        // per entity and it is already rebuilt and refilled every frame.
        const GroomStrandSimulation simulation = request.Simulation();
        // THE CURVE SET THE LOD SELECTED (#1252), which is the cooked card
        // level past the hand-over and the base groom otherwise. Both go
        // through this one build, this one shader and this one BCSDF, which is
        // what makes criterion 1's "colour and highlight response are preserved
        // across transitions" true by construction rather than by care.
        const GroomStrandMeshStats stats =
            BuildGroomStrandMesh(request.BuildSource(), request.Build, vertices, indices,
                                 deformed ? &deformation : nullptr, &coat,
                                 simulation.IsUsable(request.Groom->GetCurveCount()) ? &simulation : nullptr);
        if (vertices.empty() || indices.empty())
        {
            // An empty groom is not an error — a guides-only view of a groom
            // with no guides is legitimately empty — but it is also not
            // something to cache an empty VAO for.
            return nullptr;
        }

        // The refill path: same entity, same budget, same vertex and index
        // counts, so only the BYTES changed. Reallocating instead would churn a
        // GPU buffer every frame for every bound groom in the scene, which is
        // the cost this branch exists to remove — and the counts are stable by
        // construction, because deforming a strand moves its points and never
        // changes how many segments it has.
        if (const auto entryIt = m_Cache.find(key); deformed && entryIt != m_Cache.end())
        {
            CacheEntry& entry = entryIt->second;
            if (entry.Dynamic && entry.Array && entry.Vertices && entry.Stats.VertexCount == stats.VertexCount &&
                entry.Stats.IndexCount == stats.IndexCount)
            {
                entry.Vertices->SetData({ vertices.data(), static_cast<u32>(stats.VertexBytes) });
                entry.Stats = stats;
                entry.LastUsedFrame = m_CacheTick;
                ++m_Stats.DeformedRebuilds;
                return &entry;
            }
            m_CacheBytes -= entry.Bytes;
            m_Cache.erase(entryIt);
        }

        CacheEntry entry;
        entry.Settings = request.Build;
        entry.Stats = stats;
        entry.Bytes = stats.VertexBytes + stats.IndexBytes;
        entry.LastUsedFrame = m_CacheTick;
        entry.Dynamic = deformed;

        if (deformed)
        {
            // Sized-then-filled, so the buffer is created with a usage the
            // backend can refill. Create(data, size) mints an immutable one on
            // the GL backend, and SetData on it is a silent no-op — the coat
            // would render at whatever pose it was first built in, forever, with
            // nothing logged.
            entry.Vertices = VertexBuffer::Create(static_cast<u32>(stats.VertexBytes));
            entry.Vertices->SetData({ vertices.data(), static_cast<u32>(stats.VertexBytes) });
            ++m_Stats.DeformedRebuilds;
        }
        else
        {
            entry.Vertices = VertexBuffer::Create(vertices.data(), static_cast<u32>(stats.VertexBytes));
        }
        entry.Vertices->SetLayout(StrandVertexLayout());
        entry.Indices = IndexBuffer::Create(indices.data(), static_cast<u32>(indices.size()));
        entry.Array = VertexArray::Create();
        entry.Array->AddVertexBuffer(entry.Vertices);
        entry.Array->SetIndexBuffer(entry.Indices);

        m_CacheBytes += entry.Bytes;
        ++m_Stats.CacheBuilds;

        OLO_CORE_TRACE("GroomRenderPass: built strand geometry for groom {} — {} of {} strands (stride {}), "
                       "{} segments, {:.2f} MiB",
                       static_cast<u64>(request.Handle), stats.StrandsSelected, stats.StrandsAvailable, stats.Stride,
                       stats.SegmentCount, static_cast<f64>(entry.Bytes) / (1024.0 * 1024.0));

        const auto [it, inserted] = m_Cache.emplace(key, std::move(entry));
        return inserted ? &it->second : nullptr;
    }

    // Rebuilds this coat's density volume when the resident bake is not the one
    // the frame wants, and reports what the coat actually gets.
    //
    // THE BAKE IS IN GROOM OBJECT SPACE. A coat that merely MOVES therefore
    // reuses it — which is most of the update policy, and is why an animated
    // light or a walking character costs zero rebuilds. What does invalidate it
    // is a change of RESOLUTION (the shadow LOD) or of the geometry itself.
    u32 GroomRenderPass::CountResidentCoatVolumes() const noexcept
    {
        u32 resident = 0;
        for (const auto& [key, entry] : m_Cache)
        {
            if (entry.CoatVolume)
            {
                ++resident;
            }
        }
        return resident;
    }

    bool GroomRenderPass::ReclaimLeastRecentlyUsedCoatVolume()
    {
        CacheEntry* oldest = nullptr;
        for (auto& [key, entry] : m_Cache)
        {
            if (!entry.CoatVolume)
            {
                continue;
            }
            // NEVER an entry used this frame: its draw has already been
            // recorded against the texture this would free, and the bind is
            // per-draw rather than deferred.
            if (entry.LastUsedFrame == m_CacheTick)
            {
                continue;
            }
            if (oldest == nullptr || entry.LastUsedFrame < oldest->LastUsedFrame)
            {
                oldest = &entry;
            }
        }
        if (oldest == nullptr)
        {
            return false;
        }
        ReleaseCoatVolume(*oldest, m_CacheBytes);
        return true;
    }

    GroomCoatShadowDecision GroomRenderPass::AcquireCoatVolume(const GroomStrandRequest& request, CacheEntry& entry,
                                                               u32& residentVolumes)
    {
        GroomCoatShadowInputs inputs;
        inputs.Requested = request.CoatShadow;
        inputs.SegmentCount = entry.Stats.SegmentCount;
        // Both volume modes need a 3D texture; the RHI exposes no capability
        // query for one, and every backend the engine ships can create one, so
        // this is true rather than assumed-true. If a backend ever cannot, this
        // is the single place that has to learn about it, and the reason it
        // would report is already written.
        inputs.VolumeTexturesSupported = true;
        // A density volume is light-independent, so it needs no directional
        // light. Only a deep opacity map would, and that mode is measured
        // rather than shipped.
        inputs.HasDirectionalLight = true;
        inputs.MinResolution = request.CoatLod.MinResolution;

        // A BOUND, DEFORMING GROOM IS REFUSED, and that is the honest answer
        // rather than a limitation left to be discovered. The bake reads the
        // GroomAsset's REST-POSE curves, so on a character whose body animates
        // the drawn strands move and the volume does not: the coat would carry
        // its bind-pose shadow around, which looks like a shading bug rather
        // than like the missing feature it is. Following a deformation means
        // baking from the deformed strand positions the pass builds, which is
        // a larger change than this slice — so it is counted and named here
        // instead of approximated.
        // Refused before the bake, not after: the bake does not branch on the
        // mode, so building here and refusing later would spend a volume's
        // memory and build time on a draw that will render unshadowed.
        if (!GroomCoatShadowModeIsImplemented(request.CoatShadow))
        {
            ReleaseCoatVolume(entry, m_CacheBytes);
            inputs.ResolvedResolution = request.CoatLod.BaseResolution;
            inputs.RepresentationReady = false;
            inputs.GrantedSlot = kNoGroomCoatShadowSlot;
            return SelectGroomCoatShadow(inputs);
        }

        inputs.GroomIsDeformed = IsDeformed(request);
        if (inputs.GroomIsDeformed && request.CoatShadow != GroomCoatShadowTechnique::None)
        {
            ReleaseCoatVolume(entry, m_CacheBytes);
            inputs.ResolvedResolution = request.CoatLod.BaseResolution;
            inputs.RepresentationReady = false;
            inputs.GrantedSlot = kNoGroomCoatShadowSlot;
            return SelectGroomCoatShadow(inputs);
        }

        if (request.CoatShadow == GroomCoatShadowTechnique::None)
        {
            // Nothing to do, and NOT a fallback: a coat that never asked must
            // not be counted as a failure, or the counter that explains a
            // missing shadow is saturated by coats working exactly as authored.
            entry.CoatRequestedLodStep = 0;
            entry.CoatLodStableFrames = 0;
            inputs.ResolvedResolution = request.CoatLod.BaseResolution;
            inputs.RepresentationReady = false;
            inputs.GrantedSlot = kNoGroomCoatShadowSlot;
            return SelectGroomCoatShadow(inputs);
        }

        // ── The shadow LOD, with hysteresis ─────────────────────────────
        //
        // The apparent size comes from the CULL view rather than the render
        // view, which is what every other LOD in this engine uses (issue #726)
        // so that a frozen cut keeps its LODs.
        f32 pixelSize = request.CoatLod.PixelSizeForLod0;
        if (entry.CoatBoundsMax.x > entry.CoatBoundsMin.x)
        {
            const glm::vec3 objectCentre = (entry.CoatBoundsMin + entry.CoatBoundsMax) * 0.5f;
            const glm::vec3 worldCentre = glm::vec3(request.Transform * glm::vec4(objectCentre, 1.0f));
            // The radius has to be in WORLD units, because the distance below
            // is. The bounds are OBJECT space, so the transform's scale has to
            // come with them: without it a groom authored at scale 10 reads as
            // a tenth of its apparent size and drops to the coarsest LOD — or
            // straight through the floor into a counted fallback — while
            // filling the screen.
            const f32 transformScale = (glm::length(glm::vec3(request.Transform[0])) +
                                        glm::length(glm::vec3(request.Transform[1])) +
                                        glm::length(glm::vec3(request.Transform[2]))) /
                                       3.0f;
            const f32 radius = glm::length(entry.CoatBoundsMax - entry.CoatBoundsMin) * 0.5f * transformScale;
            const f32 distance = glm::length(worldCentre - Renderer3D::GetCullViewPosition());
            const glm::mat4& projection = Renderer3D::GetCullProjectionMatrix();
            // abs(): Vulkan's clip space has +Y down, so the engine uploads a
            // projection whose [1][1] is negative there. The sign is a clip
            // convention and the MAGNITUDE is what a scale is asking for — the
            // same trap, invisible on OpenGL and silent on Vulkan, that
            // groom-strand-visibility.md gives its own heading to.
            const f32 cotHalfFov = std::abs(projection[1][1]);
            const f32 viewportHeight =
                static_cast<f32>(m_SceneFramebuffer ? m_SceneFramebuffer->GetSpecification().Height : 1080u);
            if (distance > 1.0e-4f && std::isfinite(radius) && std::isfinite(cotHalfFov))
            {
                pixelSize = (2.0f * radius) * cotHalfFov * viewportHeight * 0.5f / distance;
            }
        }

        // BIASED BY THE REPRESENTATION LOD'S SHADOW STEP (#1252). Added to the
        // coat policy's own answer rather than replacing it, so the two remain
        // separable: GroomCoatShadowComponent still decides what resolution a
        // coat deserves at a size, and GroomLodComponent decides how much
        // further down that ladder distance pushes it. Clamped to the coat
        // policy's MaxLodSteps because that is what CoatLodResolution honours
        // anyway -- a step past it would report a LOD the bake never used.
        //
        // This is the third and last of criterion 3's independent axes, and it
        // is the one that is a BIAS rather than a budget because a shadow
        // volume spends a resolution, not a count.
        const u32 requested = std::min(GroomCoatShadow::SelectCoatLodStep(request.CoatLod, pixelSize) +
                                           request.Lod.ShadowStep,
                                       request.CoatLod.MaxLodSteps);
        entry.CoatLodStableFrames = requested == entry.CoatRequestedLodStep ? entry.CoatLodStableFrames + 1u : 0u;
        entry.CoatRequestedLodStep = requested;
        // Three frames, so a coat sitting exactly on a LOD boundary cannot
        // rebuild its representation every frame. Refining is immediate; only
        // coarsening waits. GroomCoatShadowLod.AnOscillatingRequestCannotRebuildEveryFrame
        // is the case that pins the behaviour.
        const u32 lodStep =
            GroomCoatShadow::ApplyCoatLodHysteresis(entry.CoatLodStep, requested, entry.CoatLodStableFrames, 3u);
        const u32 resolution = GroomCoatShadow::CoatLodResolution(request.CoatLod, lodStep);
        inputs.ResolvedResolution = resolution;

        // The slot goes to a coat that already holds one before any newcomer,
        // so a coat that is already resident is never displaced by one that
        // merely arrived later in the same frame — which would make which coats
        // are shadowed depend on entity iteration order.
        // THE BUDGET BOUNDS THE RESIDENT SET — the whole cache's worth, not
        // this frame's draws. `residentVolumes` is seeded from
        // CountResidentCoatVolumes(), so a coat that stopped being visible
        // still occupies its slot until something reclaims it; counting live
        // draws instead let alternating groups of eight coats hold far more
        // than the cap indefinitely, with BudgetExhausted never reported.
        //
        // An ALREADY-RESIDENT coat keeps its slot without competing for one,
        // so which coats are shadowed does not depend on entity iteration
        // order. A newcomer takes a free slot, or reclaims the
        // least-recently-used volume belonging to a groom NOT drawn this frame.
        // Only when every resident volume is in use this frame is the budget
        // genuinely exhausted — and that is what gets reported.
        const bool alreadyResident = entry.CoatVolume != nullptr;
        if (alreadyResident)
        {
            inputs.GrantedSlot = 0u;
        }
        else if (residentVolumes < kMaxResidentCoatVolumes)
        {
            inputs.GrantedSlot = residentVolumes;
        }
        else if (ReclaimLeastRecentlyUsedCoatVolume())
        {
            --residentVolumes;
            inputs.GrantedSlot = residentVolumes;
        }
        else
        {
            inputs.GrantedSlot = kNoGroomCoatShadowSlot;
        }

        // WIDTH SCALE IS PART OF THE BAKE, so it has to be part of what
        // invalidates it: it multiplies the cooked diameters and therefore the
        // areal density the volume stores. Without it, dragging Width Scale in
        // the inspector changes every ribbon on screen and leaves the shadow
        // describing the coat's previous thickness.
        const bool needsRebuild = !entry.CoatVolume || entry.CoatResolution != resolution ||
                                  entry.CoatLodStep != lodStep ||
                                  !Math::BitwiseEqual(entry.CoatWidthScale, request.WidthScale);

        if (needsRebuild && inputs.GrantedSlot != kNoGroomCoatShadowSlot &&
            resolution >= request.CoatLod.MinResolution)
        {
            GroomCoatShadow::CoatSampleSettings sampleSettings;
            sampleSettings.MaxStrands = request.Build.MaxStrands;
            sampleSettings.MaxSegments = request.Build.MaxSegments;
            sampleSettings.WidthScale = request.WidthScale;
            sampleSettings.GuidesOnly = request.Build.GuidesOnly;

            // IDENTITY, not the model matrix: the bake is in OBJECT space so a
            // coat that moves reuses it. GroomStrand.glsl transforms the shading
            // point back through u_GroomCoatWorldToObject.
            std::vector<GroomCoatShadow::CoatSegment> segments;
            const u32 emitted =
                GroomCoatShadow::BuildCoatSegments(*request.Groom, glm::mat4(1.0f), sampleSettings, segments);

            GroomCoatShadow::DensityVolumeSettings volumeSettings;
            volumeSettings.Resolution = resolution;
            GroomCoatShadow::DensityVolume volume;

            if (emitted > 0 && GroomCoatShadow::BuildDensityVolume(segments, volumeSettings, volume, nullptr))
            {
                // ONE RGBA texture: xyz = mean fibre direction * coherence,
                // w = areal density. Packed rather than two textures because the
                // march is a per-fragment hot loop and two fetches per step
                // would double its bandwidth — and because the sampler
                // namespace had exactly one index left.
                const sizet voxels = static_cast<sizet>(volume.Dimensions.x) *
                                     static_cast<sizet>(volume.Dimensions.y) *
                                     static_cast<sizet>(volume.Dimensions.z);
                std::vector<f32> packed(voxels * 4u, 0.0f);
                for (sizet i = 0; i < voxels; ++i)
                {
                    packed[i * 4u + 0u] = volume.Direction[i].x;
                    packed[i * 4u + 1u] = volume.Direction[i].y;
                    packed[i * 4u + 2u] = volume.Direction[i].z;
                    packed[i * 4u + 3u] = volume.Density[i];
                }

                Texture3DSpecification spec;
                spec.Width = static_cast<u32>(volume.Dimensions.x);
                spec.Height = static_cast<u32>(volume.Dimensions.y);
                spec.Depth = static_cast<u32>(volume.Dimensions.z);
                // RGBA32F, 16 bytes a voxel, and NOT the RGBA16F the packing
                // would prefer: Texture3D's RGBA16F declares 8 bytes a texel
                // but uploads its client data as GL_FLOAT, so SetData's own
                // size check rejects the only buffer it could be handed. The
                // engine's one other RGBA16F volume is written by a compute
                // image store and never goes through SetData, which is why the
                // mismatch has not been hit before. Half the memory is
                // available here the moment that path is fixed.
                spec.Format = Texture3DFormat::RGBA32F;
                // CLAMP, never repeat. A march that leaves the box must read the
                // empty boundary voxel, not wrap round to the other side of the
                // animal — and the bake pads its bounds precisely so those
                // boundary voxels are empty.
                spec.Repeat = false;

                if (Ref<Texture3D> texture = Texture3D::Create(spec))
                {
                    texture->SetData(packed.data(), static_cast<u32>(packed.size() * sizeof(f32)));
                    entry.CoatVolume = texture;
                    entry.CoatBoundsMin = volume.BoundsMin;
                    entry.CoatBoundsMax = volume.BoundsMax;
                    entry.CoatResolution = resolution;
                    entry.CoatLodStep = lodStep;
                    entry.CoatWidthScale = request.WidthScale;
                    // The REAL voxel size, carried rather than re-derived. The
                    // grid's dimensions differ per axis (only the longest gets
                    // `resolution`), so extent/resolution is the voxel size on
                    // that axis alone -- on a flat coat it comes out several
                    // times too fine, the march hits its step cap, and what
                    // ships is a one-voxel march rather than the measured
                    // three-voxel one.
                    const glm::vec3 voxelSize = volume.VoxelSize();
                    entry.CoatVoxelSize = std::min({ voxelSize.x, voxelSize.y, voxelSize.z });
                    // Replacing a bake: the old bytes come off before the new
                    // ones go on, or a resolution change leaks the difference.
                    m_CacheBytes -= std::min(m_CacheBytes, entry.CoatBytes);
                    entry.CoatBytes = static_cast<u64>(packed.size() * sizeof(f32));
                    m_CacheBytes += entry.CoatBytes;
                    entry.CoatBuiltTick = m_CacheTick;
                    entry.CoatMode = request.CoatShadow;
                    ++m_Stats.CoatShadow.Rebuilds;
                    if (!alreadyResident)
                    {
                        // A slot has just been taken. Counted HERE, where the
                        // texture actually came into existence, rather than by
                        // the caller after the fact — a build that failed must
                        // not consume one.
                        ++residentVolumes;
                    }
                }
            }
        }

        // The frame's OWN answer, never the request's expectation: a volume that
        // failed to build leaves this false and the coat falls back with a
        // counted reason rather than sampling a texture that is not there.
        inputs.RepresentationReady = entry.CoatVolume != nullptr && entry.CoatResolution == resolution;
        return SelectGroomCoatShadow(inputs);
    }

    void GroomRenderPass::EvictToBudget()
    {
        if (m_CacheBytes <= m_CacheBudgetBytes)
        {
            return;
        }

        // Oldest first, and never an entry used this frame — its draw has
        // already been recorded against the buffer this would free.
        std::vector<std::pair<u32, u64>> candidates;
        candidates.reserve(m_Cache.size());
        for (const auto& [key, entry] : m_Cache)
        {
            if (m_CacheTick - entry.LastUsedFrame < kCacheRetentionFrames)
            {
                continue;
            }
            candidates.emplace_back(entry.LastUsedFrame, key);
        }
        std::sort(candidates.begin(), candidates.end());

        for (const auto& [lastUsed, key] : candidates)
        {
            if (m_CacheBytes <= m_CacheBudgetBytes)
            {
                break;
            }
            const auto it = m_Cache.find(key);
            if (it == m_Cache.end())
            {
                continue;
            }
            // BOTH halves of the entry's footprint. The geometry's bytes and
            // the coat volume's are added to m_CacheBytes separately, so
            // subtracting only the geometry left the total permanently
            // inflated by every evicted coat — after which the pass evicts
            // healthy entries forever and reports itself over budget with
            // nothing left to free.
            ReleaseCoatVolume(it->second, m_CacheBytes);
            m_CacheBytes -= std::min(m_CacheBytes, it->second.Bytes);
            m_Cache.erase(it);
            ++m_Stats.CacheEvictions;
        }

        if (m_CacheBytes > m_CacheBudgetBytes)
        {
            // Every resident groom is in use, so the budget cannot be met.
            // Said out loud rather than dropping a draw: a groom that vanished
            // to fit a budget is indistinguishable from a broken asset.
            OLO_CORE_WARN("GroomRenderPass: strand cache is {:.1f} MiB over its {:.1f} MiB budget and every entry is "
                          "in use this frame; nothing was evicted and every groom is still drawn",
                          static_cast<f64>(m_CacheBytes - m_CacheBudgetBytes) / (1024.0 * 1024.0),
                          static_cast<f64>(m_CacheBudgetBytes) / (1024.0 * 1024.0));
        }
    }

    void GroomRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        m_Stats.Reset();
        m_Stats.GroomsSubmitted = static_cast<u32>(m_Requests.size());
        // One tick per executed frame, 64-bit and owned by this pass. See
        // m_CacheTick for why GroomFrameState::FrameIndex cannot serve.
        ++m_CacheTick;

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
            m_Stats.CachedBytes = m_CacheBytes;
            m_Stats.CachedGrooms = static_cast<u32>(m_Cache.size());
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
            m_Stats.CachedBytes = m_CacheBytes;
            m_Stats.CachedGrooms = static_cast<u32>(m_Cache.size());
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
        u32 residentCoatVolumes = CountResidentCoatVolumes();

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
            if (IsDeformed(request))
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

            CacheEntry* entry = AcquireGeometry(request);
            if (entry == nullptr || !entry->Array)
            {
                continue;
            }

            // Coat self-shadowing (#1248). The bake, the shadow LOD and the
            // decision all happen here because this is the only place that
            // knows what the frame actually resolved -- the same
            // producer/transport/consumer split the composition mode uses.
            const GroomCoatShadowDecision coatDecision =
                AcquireCoatVolume(request, *entry, residentCoatVolumes);
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
                    m_Stats.CoatShadow.MaxAgeFrames, static_cast<u32>(m_CacheTick - entry->CoatBuiltTick));
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
            // FROM THE ACHIEVED FRACTION, never the requested one, and this is
            // the only place the achieved fraction exists: the strand budget is
            // spent as an integer STRIDE PER ROLE, so a budget asked for 0.4 of
            // a role retains a third of it. Compensating by the policy's 1/0.4
            // would leave the coat a sixth thinner than it started, and the
            // error compounds at every step down the ladder.
            //
            // The numbers come out of the cache entry's own build stats, which
            // cost nothing to read — they were computed when the geometry was
            // built and are what the inspector already shows.
            //
            // It multiplies the AUTHORING width scale rather than replacing it:
            // m_WidthScale is a unit-scale lever for a groom exported at a
            // different scale, and this is a density correction. Folding them
            // into one number would make turning the LOD off change a coat that
            // was authored at 0.5.
            const f32 achievedFraction =
                entry->Stats.StrandsAvailable > 0u
                    ? static_cast<f32>(entry->Stats.StrandsSelected) / static_cast<f32>(entry->Stats.StrandsAvailable)
                    : 1.0f;
            const f32 widthCompensation =
                request.LodPolicy.Enabled
                    ? GroomLodWidthCompensation(achievedFraction, request.LodPolicy.MaxWidthCompensation)
                    : 1.0f;
            const f32 effectiveWidthScale = request.WidthScale * widthCompensation;

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
            // CacheEntry::BytesCountedTick. The tick is the pass's own
            // monotonic counter, so an entry drawn by two entities this frame
            // contributes its allocation once and the figure stays a RESIDENT
            // byte count rather than a sum of draws.
            if (entry->BytesCountedTick != m_CacheTick)
            {
                entry->BytesCountedTick = m_CacheTick;
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

            // The coat lanes. They stay at their INACTIVE defaults unless this
            // draw actually has a built, bound volume -- the structural
            // fallback technique-selection-seams.md asks for, rather than a
            // flag someone has to remember to reset on each early return.
            if (coatActive)
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

                    const glm::vec3 extent = entry->CoatBoundsMax - entry->CoatBoundsMin;
                    if (extent.x > 0.0f && extent.y > 0.0f && extent.z > 0.0f)
                    {
                        const f32 voxelLength = entry->CoatVoxelSize;

                        params.CoatWorldToObject = worldToObject;
                        params.CoatBoundsMin = glm::vec4(entry->CoatBoundsMin, request.CoatKappa);
                        params.CoatInvExtent =
                            glm::vec4(1.0f / extent, voxelLength * request.CoatStepVoxels);
                        params.CoatModes = glm::ivec4(static_cast<i32>(coatDecision.Effective), 0, 0, 0);
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

        EvictToBudget();
        m_Stats.CachedBytes = m_CacheBytes;
        m_Stats.CachedGrooms = static_cast<u32>(m_Cache.size());

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
        // The cache holds GPU buffers whose device is going away, so it is
        // dropped here rather than left to be rebuilt against a dead context.
        m_Cache.clear();
        m_CacheBytes = 0;
        m_CacheTick = 0;
        m_Requests.clear();
        m_SceneFramebuffer = nullptr;
        m_LastReportedReason = GroomCompositionFallbackReason::None;
    }
} // namespace OloEngine
