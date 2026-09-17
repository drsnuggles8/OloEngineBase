#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Shader.h"
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
                                 { ShaderDataType::Float, "a_Pad0" },
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
        const GroomStrandMeshStats stats =
            BuildGroomStrandMesh(*request.Groom, request.Build, vertices, indices, deformed ? &deformation : nullptr);
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
            m_CacheBytes -= it->second.Bytes;
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

            CacheEntry* entry = AcquireGeometry(request);
            if (entry == nullptr || !entry->Array)
            {
                continue;
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
            params.Model = request.Transform;
            params.PrevModel = request.PreviousTransform;
            params.Color = glm::vec4(request.Color, 1.0f);
            params.IDs = glm::ivec4(request.EntityID, 0, 0, 0);
            params.Viewport = glm::vec4(static_cast<f32>(spec.Width), static_cast<f32>(spec.Height), 0.0f, 0.0f);
            params.RampWidth =
                glm::vec4(request.RampFloor, request.WidthScale, objectScale, request.AlphaCutoff);
            params.ModeFrame = glm::ivec4(static_cast<i32>(decision.Effective),
                                          static_cast<i32>(m_FrameState.FrameIndex),
                                          static_cast<i32>(kStochasticSeed), 0);
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
