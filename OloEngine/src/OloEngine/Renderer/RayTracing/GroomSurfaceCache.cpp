#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RayTracing/GroomSurfaceCache.h"
#include "OloEngine/Renderer/RayTracing/GroomProxyDiagnostics.h"

#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MaterialKind.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <optional>
#include <span>
#include <utility>

namespace OloEngine::RayTracing
{
    namespace GroomProxyDiagnostics
    {
        namespace
        {
            // Render-thread-only, and deliberately a plain global rather
            // than anything atomic: this whole subsystem is render-thread
            // only, and an atomic here would imply a second writer that
            // does not exist.
            //
            // SEEDED FROM THE ENVIRONMENT ONCE, so the A/B is also runnable
            // across two launches when clicking the panel is not available —
            // an unattended session drives the editor through MCP, and the
            // statistics dock is not always reachable at a high DPI scale.
            // Read once rather than per frame: a lever that could change
            // under a running comparison is not a control.
            bool ReadDisabledFromEnvironment()
            {
                const char* value = std::getenv("OLO_GROOM_RT_PROXIES");
                return value != nullptr && value[0] == '0' && value[1] == '\0';
            }
            bool s_Disabled = ReadDisabledFromEnvironment();
            std::optional<GroomProxyTier> s_ForcedTier;
        } // namespace

        void SetDisabled(bool disabled)
        {
            s_Disabled = disabled;
        }
        bool GetDisabled()
        {
            return s_Disabled;
        }
        void SetForcedTier(std::optional<GroomProxyTier> tier)
        {
            s_ForcedTier = tier;
        }
        std::optional<GroomProxyTier> GetForcedTier()
        {
            return s_ForcedTier;
        }
    } // namespace GroomProxyDiagnostics

    namespace
    {
        struct Hash
        {
            u64 Value = 1469598103934665603ull;
            template<typename T>
            void Mix(const T& value)
            {
                for (const std::byte byte : std::as_bytes(std::span(&value, 1u)))
                {
                    Value ^= std::to_integer<u8>(byte);
                    Value *= 1099511628211ull;
                }
            }
        };

        // The same predicate GroomRenderPass::IsDeformed applies, and the same
        // reason for the length test rather than a non-empty test: the build
        // indexes the transform array by curve, so a short array reads past its
        // end on the first strand past the boundary.
        [[nodiscard]] bool IsDeformed(const GroomStrandRequest& request) noexcept
        {
            return request.Groom && request.Binding &&
                   static_cast<sizet>(request.RootTransforms.Num()) == request.Groom->GetCurveCount();
        }

        [[nodiscard]] bool IsFiniteTransform(const glm::mat4& matrix) noexcept
        {
            for (u32 column = 0; column < 4u; ++column)
            {
                for (u32 row = 0; row < 4u; ++row)
                {
                    if (!std::isfinite(matrix[column][row]))
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    } // namespace

    GroomSurfaceCache::GroomSurfaceCache() = default;
    GroomSurfaceCache::~GroomSurfaceCache() = default;

    void GroomSurfaceCache::SetEnabled(bool enabled)
    {
        if (m_Enabled && !enabled)
        {
            Shutdown();
        }
        m_Enabled = enabled;
    }

    void GroomSurfaceCache::Shutdown()
    {
        m_Entries.clear();
        m_TierState.clear();
        m_StrandVertices.clear();
        m_StrandVertices.shrink_to_fit();
        m_StrandIndices.clear();
        m_StrandIndices.shrink_to_fit();
        m_ProxyVertices.clear();
        m_ProxyVertices.shrink_to_fit();
        m_ProxyIndices.clear();
        m_ProxyIndices.shrink_to_fit();
        m_Stats.Reset();
        m_Enabled = false;
    }

    GroomProxyRefusalReason GroomSurfaceCache::Refresh(Entry& entry, const GroomStrandRequest& request,
                                                       const GroomProxyDecision& decision, u64 shapeHash,
                                                       GroomProxyFrameBudget& budget)
    {
        // ── The one cap that can be tested before doing the work ─────────
        //
        // The vertex and triangle caps need a real size, which only the
        // conversion knows. The UPDATE cap does not — so it is tested here,
        // before a strand build and a ribbon conversion are paid for and
        // thrown away. Without it a scene past the budget burns a full
        // rebuild per coat per frame for nothing.
        if (budget.Exhausted())
        {
            return GroomProxyRefusalReason::BudgetExhausted;
        }

        // ── The strand set this tier asks for ────────────────────────────
        //
        // The request's OWN budget is the ceiling: a coat the entity already
        // thinned to 500 strands must not be traced at 6144 of them. The tier
        // lowers it and never raises it, which is the "coarser of the two wins"
        // rule #1258 applies to the LOD budgets, for the same reason.
        GroomStrandBuildSettings build = request.Build;
        build.MaxStrands = std::min(build.MaxStrands, decision.StrandBudget);

        GroomStrandDeformation deformation;
        const bool deformed = IsDeformed(request);
        if (deformed)
        {
            deformation.Binding = request.Binding.Raw();
            deformation.RootTransforms = std::span<const GroomRootTransform>(
                request.RootTransforms.GetData(), static_cast<sizet>(request.RootTransforms.Num()));
        }
        const GroomCoatContext coat{ &request.Coat, request.Groom->GetGroupCoats() };
        const GroomStrandSimulation simulation = request.Simulation();

        const GroomStrandMeshStats strandStats =
            BuildGroomStrandMesh(request.BuildSource(), build, m_StrandVertices, m_StrandIndices,
                                 deformed ? &deformation : nullptr, &coat,
                                 simulation.IsUsable(request.Groom->GetCurveCount()) ? &simulation : nullptr);
        if (m_StrandVertices.empty() || strandStats.StrandsSelected == 0u)
        {
            return GroomProxyRefusalReason::GroomHasNoGeometry;
        }

        // ── The compensation, on the ACHIEVED fraction ───────────────────
        //
        // StrandsSelected / StrandsAvailable, never the fraction the tier asked
        // for: the budget is spent as an integer stride, so asking for 0.4 of
        // the curves retains 1/3 of them. See GroomProxyWidthCompensation, which
        // carries that warning, and GroomLod.h, which explains why the whole
        // engine compensates on the achieved number.
        const f32 achieved = strandStats.StrandsAvailable > 0u
                                 ? static_cast<f32>(strandStats.StrandsSelected) /
                                       static_cast<f32>(strandStats.StrandsAvailable)
                                 : 1.0f;
        const f32 compensation =
            GroomProxyWidthCompensation(achieved, GroomProxyPolicy::MaxWidthCompensation);

        GroomProxyConversionSettings conversion;
        // BOTH FACTORS, exactly as the raster draw composes them.
        // BuildGroomStrandMesh emits the COOKED radii and applies neither:
        // the raster path multiplies them in the shader by
        // `request.WidthScale * widthCompensation` (GroomRenderPass), so a
        // proxy that applied only the compensation would be thinner in ray
        // space than on screen for every groom exported at another unit
        // scale — a coat whose shadow is too light with nothing to say why.
        //
        // The two compensations are INDEPENDENT and both belong here: the
        // raster one puts back what the LOD's thinning removed, this one
        // puts back what the PROXY's thinning removed, and the proxy is
        // built from the strand set the LOD budget already thinned.
        conversion.WidthScale = std::isfinite(request.WidthScale) && request.WidthScale > 0.0f
                                    ? request.WidthScale * compensation
                                    : compensation;
        const GroomProxyMeshStats proxyStats = ConvertGroomStrandMeshToProxy(m_StrandVertices, conversion,
                                                                             m_ProxyVertices, m_ProxyIndices);
        if (proxyStats.VertexCount == 0u || proxyStats.IndexCount == 0u)
        {
            return GroomProxyRefusalReason::BuildFailed;
        }

        // ── The gates, in the order that keeps the budget transactional ──
        //
        // THE RESIDENT-BYTE GATE COMES FIRST and the reservation LAST. Both
        // need the real size, so neither can run before the conversion; but
        // reserving first and then failing the byte gate would spend frame
        // budget on a coat that was rejected anyway, and starve the coats
        // behind it — which is precisely the claim GroomProxyFrameBudget
        // makes about itself. The reservation is the last thing that can
        // fail before the buffers exist, so nothing consumes it and returns.
        const u64 bytes = proxyStats.VertexBytes + proxyStats.IndexBytes;
        const u64 residentWithout = m_Stats.ResidentBytes - entry.Bytes;
        if (bytes > GroomProxyPolicy::GeometryBytes - residentWithout)
        {
            return GroomProxyRefusalReason::ResidencyExhausted;
        }
        if (!budget.Reserve(proxyStats.VertexCount, proxyStats.TriangleCount()))
        {
            return GroomProxyRefusalReason::BudgetExhausted;
        }

        // ── The buffers ──────────────────────────────────────────────────
        //
        // GL refills a stable shape in place. Vulkan BLAS builds consume the
        // persistent device address, so even a same-shape refill publishes a
        // fresh vertex allocation while earlier frame builds may be in flight.
        //
        // Sized-then-filled, never Create(data, size): the latter mints an
        // IMMUTABLE buffer on the GL backend and SetData on it is a silent
        // no-op, so the coat's proxy would hold the pose it was first built in
        // forever, with nothing logged. The same trap GroomRenderPass documents.
        const bool reallocate = !entry.Vertices || !entry.Indices || entry.ShapeHash != shapeHash ||
                                entry.VertexCount != proxyStats.VertexCount ||
                                entry.IndexCount != proxyStats.IndexCount;
        Ref<IndexBuffer> replacementIndices;
        if (reallocate)
        {
            replacementIndices = IndexBuffer::Create(m_ProxyIndices.data(), proxyStats.IndexCount);
            if (!replacementIndices)
                return GroomProxyRefusalReason::BuildFailed;
        }
        if (reallocate || RendererAPI::GetAPI() == RendererAPI::API::Vulkan)
        {
            auto replacement = VertexBuffer::Create(static_cast<u32>(proxyStats.VertexBytes));
            if (!replacement)
                return GroomProxyRefusalReason::BuildFailed;
            replacement->SetLayout(Vertex::GetLayout());
            replacement->SetData({ m_ProxyVertices.data(), static_cast<u32>(proxyStats.VertexBytes) });
            entry.Vertices = std::move(replacement);
            if (reallocate)
                entry.Indices = std::move(replacementIndices);
        }
        else
        {
            entry.Vertices->SetData({ m_ProxyVertices.data(), static_cast<u32>(proxyStats.VertexBytes) });
        }

        // A buffer with no device address cannot back a BLAS. Refused here
        // rather than staged: an acceleration structure built over address zero
        // is a device loss with no validation message.
        if (entry.Vertices->GetDeviceAddress() == 0u || entry.Indices->GetDeviceAddress() == 0u)
        {
            // The buffers go, but `Bytes` STAYS at the value that is still
            // counted in m_Stats.ResidentBytes. Zeroing it here made the
            // caller's `ResidentBytes -= entry.Bytes` subtract nothing, so
            // the dead coat's bytes were counted for the rest of the session
            // — and since ResidentBytes carries across frames, enough of them
            // underflow `GeometryBytes - residentWithout` above and the
            // resident cap fails OPEN, which is the one direction a budget
            // must never fail.
            entry.Vertices.Reset();
            entry.Indices.Reset();
            entry.VertexCount = 0u;
            entry.IndexCount = 0u;
            return GroomProxyRefusalReason::BuildFailed;
        }

        m_Stats.ResidentBytes = residentWithout + bytes;
        entry.Bytes = bytes;
        entry.ShapeHash = shapeHash;
        entry.VertexCount = proxyStats.VertexCount;
        entry.IndexCount = proxyStats.IndexCount;
        entry.Compensation = compensation;
        entry.Deformed = deformed;
        // Saturating rather than wrapping: a revision that wrapped to a value
        // the consumer already saw reads as "this coat did not move", and the
        // structure would then never refit again. 2^32 refills is 2.3 years at
        // 60 Hz, so the saturation is a statement about correctness rather than
        // a case anyone reaches.
        entry.Revision = entry.Revision == std::numeric_limits<u32>::max() ? entry.Revision : entry.Revision + 1u;

        entry.LastRefreshed = m_Frame;
        ++m_Stats.Rebuilds;
        m_Stats.SegmentsConverted += proxyStats.SegmentCount;
        m_Stats.TrianglesBuilt += proxyStats.TriangleCount();
        return GroomProxyRefusalReason::None;
    }

    void GroomSurfaceCache::Extract(GPUScene& scene, std::span<const GroomStrandRequest> requests, bool wanted)
    {
        const auto started = std::chrono::steady_clock::now();
        ++m_Frame;
        const u64 previousResident = m_Stats.ResidentBytes;
        m_Stats.Reset();
        m_Stats.ResidentBytes = previousResident;

        const bool active = m_Enabled && wanted && !GroomProxyDiagnostics::GetDisabled();
        const std::optional<GroomProxyTier> forcedTier = GroomProxyDiagnostics::GetForcedTier();

        // Entities the frame offered, whatever became of them. The tier
        // hysteresis is pruned against THIS rather than against the resident
        // set: a coat refused for one frame because the budget was spent is
        // still in the scene, and dropping its hold would let it re-decide
        // its tier from scratch on the frame it comes back.
        std::vector<i64> seen;
        seen.reserve(requests.size());

        // ── Oldest first ────────────────────────────────────────────────
        //
        // A scene with more animated coats than the per-frame budget covers
        // refreshes a SUBSET each frame. Walking the request vector in order
        // would make that subset the same one every frame — so the animals at
        // the back would hold a structure from the moment they appeared while
        // the ones at the front stayed current, which is a permanent artefact
        // on identifiable animals rather than a shared, moving one.
        //
        // Never-refreshed coats sort first (LastRefreshed 0), then the
        // stalest. The request INDEX breaks ties so the order cannot depend
        // on how the map happens to be laid out. The same rule, for the same
        // reason, that VegetationSurfaceCache applies to its own snapshots.
        std::vector<u32> order;
        order.reserve(requests.size());
        for (u32 index = 0; index < static_cast<u32>(requests.size()); ++index)
        {
            if (requests[index].Groom)
            {
                order.push_back(index);
            }
        }
        std::sort(order.begin(), order.end(),
                  [this, &requests](u32 lhs, u32 rhs)
                  {
                      const auto age = [this, &requests](u32 index) -> u64
                      {
                          const auto& r = requests[index];
                          const auto found = m_Entries.find(
                              Key{ static_cast<i64>(r.EntityID), static_cast<u64>(r.Handle) });
                          return found == m_Entries.end() ? 0u : found->second.LastRefreshed;
                      };
                      const u64 a = age(lhs), b = age(rhs);
                      return a != b ? a < b : lhs < rhs;
                  });

        GroomProxyFrameBudget budget;
        for (const u32 index : order)
        {
            const auto& request = requests[index];
            const Key key{ static_cast<i64>(request.EntityID), static_cast<u64>(request.Handle) };

            GroomProxyInputs inputs;
            inputs.PixelSize = request.ApparentPixelSize;
            inputs.StrandsAvailable = request.Groom->GetCurveCount();
            inputs.Requested = active;

            const i64 entity = static_cast<i64>(request.EntityID);
            seen.push_back(entity);
            GroomProxyState& state = m_TierState[entity];
            GroomProxyDecision decision = AdvanceGroomProxyTier(inputs, state);
            // AFTER the ladder advanced, never instead of it: the
            // hysteresis must keep tracking what the apparent size asks
            // for, or clearing the override would leave every coat on
            // whichever tier the diagnostic pinned it to until the camera
            // moved. The same reason #1258 applies its population budget
            // after AdvanceGroomLod rather than in place of it.
            if (forcedTier.has_value())
            {
                decision.Tier = *forcedTier;
                decision.StrandBudget = GroomProxyPolicy::StrandBudget(decision.Tier);
            }
            // A transform that is not a usable number would put this coat's
            // instance anywhere at all. Refused with a reason rather than
            // folded into NotRequested, which claims nobody asked — and
            // rather than staged, because a TLAS instance holding a NaN
            // transform is undefined at the device with nothing reported.
            if (!decision.IsRefused() && !IsFiniteTransform(request.Transform))
            {
                decision.Reason = GroomProxyRefusalReason::BuildFailed;
            }

            const auto found = m_Entries.find(key);
            const bool resident = found != m_Entries.end();
            // BEFORE the conversion, not after it: a coat that cannot have
            // a slot must not first pay for a strand build and a ribbon
            // conversion it is about to have thrown away.
            if (!resident && !decision.IsRefused() && m_Entries.size() >= GroomProxyPolicy::ResidentGrooms)
            {
                decision.Reason = GroomProxyRefusalReason::ResidencyExhausted;
            }
            if (decision.IsRefused())
            {
                // A refused coat loses its resident structure rather than
                // keeping a stale one. The alternative — hold what it has and
                // keep tracing it — is a coat whose ray-traced shadow is at a
                // pose the raster tier left several seconds ago, which is
                // strictly worse than no ray-traced shadow at all and is
                // invisible in every counter.
                if (resident)
                {
                    m_Stats.ResidentBytes -= found->second.Bytes;
                    m_Entries.erase(found);
                }
                m_Stats.Record(decision);
                continue;
            }

            // ── What would make this coat's geometry different ───────────
            //
            // ONE HASH, not a shape hash and a content hash, and that is a
            // fact about this producer rather than a simplification: every
            // input that changes the emitted BYTES for an UNDEFORMED groom
            // also changes how many there are. WidthScale is the near miss
            // — it only scales the radii — and it is folded in here for
            // exactly that reason.
            //
            // It has to stay that way. A refill that did NOT reallocate
            // would leave the GPU Scene geometry record identical, so an
            // undeformed coat classifies Static, its BLAS is built once
            // and compacted, and nothing would ever rebuild it — the coat
            // would cast the shadow of the shape it had when it was first
            // seen, for the rest of the session, with every counter
            // healthy. A DEFORMED groom is exempt because it carries the
            // animated flag and a revision, which is precisely how
            // RayTracingScene tells that it must refit.
            Hash shape;
            shape.Mix(static_cast<u64>(request.Handle));
            shape.Mix(request.Build.MaxStrands);
            shape.Mix(decision.StrandBudget);
            shape.Mix(request.Build.MaxSegments);
            shape.Mix(request.Build.GuidesOnly);
            shape.Mix(request.Build.CoatDigest);
            shape.Mix(request.LodLevel != nullptr);
            shape.Mix(std::to_underlying(decision.Tier));
            shape.Mix(request.WidthScale);

            const bool deformed = IsDeformed(request);
            Entry& entry = m_Entries[key];
            const bool changed = !entry.Vertices || entry.ShapeHash != shape.Value || deformed;
            // Captured BEFORE the refresh, because a failed one may drop the
            // buffers: the question below is whether this coat had a usable
            // structure to fall back ON, not whether it has one now.
            const bool wasResident = entry.Vertices && entry.Indices;
            if (changed)
            {
                const GroomProxyRefusalReason failure =
                    Refresh(entry, request, decision, shape.Value, budget);
                // A SPENT BUDGET IS NOT A REASON TO LEAVE THE SCENE. It is
                // transient by construction, and a coat that already has a
                // structure keeps occluding with it until its turn comes
                // round — which is exactly what BudgetExhausted's own
                // documentation promises. Erasing instead made an animal past
                // the budget vanish from the ray-traced world AND pay a full
                // rebuild every frame for the privilege.
                //
                // It is counted as REPRESENTED, because it is, and the frame
                // is marked incomplete, because a deforming coat held this way
                // is one frame behind its raster twin.
                if (failure == GroomProxyRefusalReason::BudgetExhausted && wasResident)
                {
                    ++m_Stats.RefreshDeferred;
                    m_Stats.Complete = false;
                }
                else if (failure != GroomProxyRefusalReason::None)
                {
                    decision.Reason = failure;
                    m_Stats.ResidentBytes -= entry.Bytes;
                    m_Entries.erase(key);
                    m_Stats.Record(decision);
                    continue;
                }
            }
            else
            {
                ++m_Stats.Reused;
            }

            entry.LastSeen = m_Frame;

            // ── The records ─────────────────────────────────────────────
            const GPUSceneGeometryKey geometryKey{ RHI::HashKey(entry.Vertices->GetRHIHandle()),
                                                   RHI::HashKey(entry.Indices->GetRHIHandle()), 0u };
            const GPUSceneMaterialKey materialKey{ static_cast<u64>(request.EntityID), 0u,
                                                   std::to_underlying(GPUSceneMaterialSource::Groom) };

            GPUSceneMaterialInput material;
            // The coat's neutral albedo. A fibre BCSDF is not a surface BRDF and
            // there is no closure here that could evaluate one, so what a
            // reflection ray sees is the coat's COLOUR at a plausible roughness
            // rather than #1247's lighting. Stated as the approximation it is:
            // criterion 2 asks for a credible silhouette in reflections, which
            // this delivers, and not for the fibre response to be re-derived in
            // ray space, which would be a second BCSDF implementation.
            material.m_BaseColorFactor = glm::vec4(request.Color, 1.0f);
            material.m_RoughnessFactor = 0.6f;
            material.m_MetallicFactor = 0.0f;
            material.m_AlphaMode = std::to_underlying(AlphaMode::Opaque);
            material.m_ClosureVersion = std::to_underlying(PBRModel::ClosureV2);
            material.m_MaterialKind = std::to_underlying(MaterialKind::Generic);
            // OPAQUE AND TWO-SIDED, and both halves matter. Two-sided because a
            // ribbon has no meaningful facing; opaque because the coverage the
            // coat would have got from an alpha test is already in the RADII —
            // that is what the compensation is — so an alpha-tested proxy would
            // remove it a second time. Opaque also keeps the instance off
            // GeometryClass::Masked, so a ray commits on intersection instead of
            // stopping on every strand as a candidate.
            material.m_Flags = GPUSceneMaterialFlagPBR | GPUSceneMaterialFlagTwoSided | GPUSceneMaterialFlagDepthTest;
            scene.ExtractMaterial(materialKey, material);

            scene.ExtractGeometry(geometryKey,
                                  {
                                      .m_VertexBuffer = entry.Vertices->GetRHIHandle(),
                                      .m_IndexBuffer = entry.Indices->GetRHIHandle(),
                                      .m_VertexAddress = entry.Vertices->GetDeviceAddress(),
                                      .m_IndexAddress = entry.Indices->GetDeviceAddress(),
                                      .m_VertexFormat = std::to_underlying(GPUSceneVertexFormat::OloVertex),
                                      .m_IndexFormat = std::to_underlying(GPUSceneIndexFormat::UInt32),
                                      .m_FirstIndex = 0u,
                                      .m_IndexCount = entry.IndexCount,
                                      .m_VertexCount = entry.VertexCount,
                                      .m_Flags = static_cast<u32>(GPUSceneGeometryFlagGroom) |
                                                 (deformed ? static_cast<u32>(GPUSceneGeometryFlagDeformed) : 0u),
                                  });

            // THE SAME TRANSFORM THE RASTER DRAW USES, unconditionally, and
            // that is the rule rather than a simplification: the two have to
            // agree about where this coat is, and the cheapest way to be sure
            // is for the proxy to carry whatever the draw carries.
            //
            // BuildGroomStrandMesh emits OBJECT-space points for a bound coat
            // and for an unbound one alike — GroomRenderPass sets
            // params.Model from request.Transform for every groom, with no
            // bound/unbound branch — so the binding deforms the coat WITHIN
            // the groom's own space and the entity transform still applies on
            // top. Passing an identity here for a bound coat would leave every
            // animal's ray-traced fur at the origin.
            //
            // The ABSOLUTE transform, not a render-relative one: GPU Scene
            // owns the camera-relative encoding, and RayTracingScene reads the
            // origin GPU Scene encoded against. Rebasing here would apply it
            // twice.
            scene.ExtractInstance({ static_cast<u64>(request.EntityID), geometryKey, 0u },
                                  {
                                      .m_WorldTransform = request.Transform,
                                      .m_Material = materialKey,
                                      .m_Flags = deformed ? static_cast<u32>(GPUSceneInstanceFlagAnimated) : 0u,
                                      .m_DeformedContentRevision = entry.Revision,
                                  });

            m_Stats.Record(decision);
        }

        // Retire everything that did not appear this frame. A coat that left
        // the scene, was hidden, or swapped its asset drops its structures here
        // rather than at some later eviction: a resident BLAS over a buffer
        // nothing refills is a coat frozen at the pose it last held.
        std::erase_if(m_Entries,
                      [this](const auto& item)
                      {
                          if (item.second.LastSeen == m_Frame)
                          {
                              return false;
                          }
                          m_Stats.ResidentBytes -= item.second.Bytes;
                          return true;
                      });
        // An entity that LEFT THE SCENE drops its hold, so that coming back
        // starts from the tier its apparent size selects rather than from
        // wherever the camera left it — #1252's reason for pruning
        // GroomLodState the same way.
        std::sort(seen.begin(), seen.end());
        std::erase_if(m_TierState,
                      [&seen](const auto& item)
                      { return !std::binary_search(seen.begin(), seen.end(), item.first); });

        // The frame-wide figures, summed over what is RESIDENT rather than
        // over what rebuilt. A still scene rebuilds nothing and must not
        // therefore report an empty ray-traced coat set — see the note on
        // GroomProxyStats::ResidentTriangles.
        for (const auto& [key, entry] : m_Entries)
        {
            static_cast<void>(key);
            m_Stats.ResidentTriangles += entry.IndexCount / 3u;
            m_Stats.MaxWidthCompensation = std::max(m_Stats.MaxWidthCompensation, entry.Compensation);
        }

        m_Stats.UpdateMicroseconds = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    }
} // namespace OloEngine::RayTracing
