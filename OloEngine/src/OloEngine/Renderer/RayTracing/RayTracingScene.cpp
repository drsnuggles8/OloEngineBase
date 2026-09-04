#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"

#include "OloEngine/Math/Math.h"

#include <algorithm>
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Vertex.h"

#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanRayTracingBackend.h"
#endif

namespace OloEngine::RayTracing
{
    namespace
    {
        // A cheap, order-independent fingerprint of the fields a BLAS build
        // actually consumes. Deliberately NOT the whole record: the geometry
        // record's Generation and Flags change for reasons a BLAS does not
        // care about, and hashing them would rebuild every BLAS for nothing.
        //
        // It also deliberately DOES include the two device addresses. A mesh
        // rebuilt in place keeps its GPU Scene identity, and the address is
        // the only field that moves — see the in-place-vertex-rewrite caveat
        // in the header.
        [[nodiscard]] u64 FingerprintGeometry(const GPUSceneGeometry& geometry)
        {
            u64 hash = 1469598103934665603ull; // FNV-1a offset basis
            const auto mix = [&hash](u64 value)
            {
                hash ^= value;
                hash *= 1099511628211ull;
            };
            mix(geometry.VertexAddress);
            mix(geometry.IndexAddress);
            mix(static_cast<u64>(geometry.FirstIndex) | (static_cast<u64>(geometry.IndexCount) << 32));
            mix(static_cast<u64>(static_cast<u32>(geometry.BaseVertex)) | (static_cast<u64>(geometry.VertexCount) << 32));
            mix(static_cast<u64>(geometry.VertexFormat) | (static_cast<u64>(geometry.IndexFormat) << 32));
            return hash;
        }

        [[nodiscard]] bool GeometryIsTraceable(const GPUSceneGeometry& geometry)
        {
            // A build needs: real addresses on both streams, the one vertex
            // and index format the builder understands, and at least one
            // whole triangle. Anything else is Unsupported — counted, not
            // asserted, because a scene legitimately contains records the
            // canonical scene could not resolve.
            if ((geometry.Flags & GPUSceneGeometryFlagActive) == 0u)
            {
                return false;
            }
            if (geometry.VertexAddress == 0u || geometry.IndexAddress == 0u)
            {
                return false;
            }
            if (geometry.VertexFormat != static_cast<u32>(GPUSceneVertexFormat::OloVertex))
            {
                return false;
            }
            if (geometry.IndexFormat != static_cast<u32>(GPUSceneIndexFormat::UInt32))
            {
                return false;
            }
            return geometry.IndexCount >= 3u && (geometry.IndexCount % 3u) == 0u && geometry.VertexCount > 0u;
        }
    } // namespace

    std::unique_ptr<IRayTracingBackend> CreateRayTracingBackend()
    {
        // Neutral factory with an explicit switch, never a Platform/ TU that
        // unconditionally constructs its own backend — that shape is
        // rhi-abstraction-boundary.md §9a's leak, and it becomes a null volk
        // pointer on the other backend.
        switch (RendererAPI::GetAPI())
        {
            case RendererAPI::API::Vulkan:
#if OLO_WITH_VULKAN
                return CreateVulkanRayTracingBackend();
#else
                return nullptr;
#endif
            case RendererAPI::API::OpenGL:
            case RendererAPI::API::None:
                break;
        }
        return nullptr;
    }

    void RayTracingScene::Init()
    {
        if (m_Backend != nullptr)
        {
            return;
        }
        m_Backend = CreateRayTracingBackend();
        if (m_Backend == nullptr)
        {
            m_Capabilities = Capabilities{};
            m_Capabilities.Reason = UnsupportedReason::BackendNotVulkan;
            return;
        }
        m_Capabilities = m_Backend->GetCapabilities();
        if (!m_Capabilities.Supported)
        {
            // The backend exists but the device refused. Keep it — its
            // capability value carries the reason the renderer reports — but
            // nothing will ever be recorded through it.
            OLO_CORE_INFO("[RayTracing] scene unavailable: {}", m_Capabilities.ReasonText());
        }
    }

    void RayTracingScene::Shutdown()
    {
        if (m_Backend != nullptr)
        {
            m_Backend->Shutdown();
            m_Backend.reset();
        }
        m_Blas.clear();
        m_Instances.clear();
        m_PendingBuilds.clear();
        m_PendingRetires.clear();
        m_Stats = SceneStats{};
        m_PreviousInstanceCount = 0;
        m_EverBuiltTlas = false;
        m_HasRenderOrigin = false;
        m_Capabilities = Capabilities{};
    }

    void RayTracingScene::SetBackendForTesting(std::unique_ptr<IRayTracingBackend> backend)
    {
        m_Backend = std::move(backend);
        m_Capabilities = m_Backend != nullptr ? m_Backend->GetCapabilities() : Capabilities{};
    }

    GeometryClass RayTracingScene::Classify(const GPUSceneInstance& instance, const GPUSceneGeometry* geometry,
                                            const GPUSceneMaterial* material)
    {
        // Most-restrictive first, and every rejection lands in Unsupported so
        // it is COUNTED rather than silently dropped.
        if ((instance.Flags & GPUSceneInstanceFlagActive) == 0u)
        {
            return GeometryClass::Unsupported;
        }
        if (geometry == nullptr || !GeometryIsTraceable(*geometry))
        {
            return GeometryClass::Unsupported;
        }
        // The instance slot has to survive the 24-bit instanceCustomIndex, or
        // the shader resolves the wrong record on every hit.
        if (!FitsInstanceCustomIndex(instance.StableIndex))
        {
            return GeometryClass::Unsupported;
        }
        // A dead material slot means the alpha mode is unknown, and guessing
        // "opaque" would make a cutout mesh solid. Refuse instead.
        if (material == nullptr || (material->Flags & GPUSceneMaterialFlagActive) == 0u)
        {
            return GeometryClass::Unsupported;
        }

        // Deformed geometry does not reach GPU Scene today: skinned, cloth,
        // virtualized-cluster and particle entities are excluded upstream and
        // counted in GPUSceneUnsupportedCategory instead. The class and its
        // refit policy exist and are tested, so the day a deformed-vertex
        // stream lands in GPU Scene this is the one line that changes.
        //
        // Masked takes precedence over the motion classes because it decides
        // the BLAS geometry FLAGS (opaque or not), which is a property of the
        // build; UpdatePolicyFor keeps its build-once behaviour.
        if (material->AlphaMode == static_cast<u32>(AlphaMode::Mask))
        {
            return GeometryClass::Masked;
        }

        // Rigid-dynamic vs static: the record tells us whether this instance
        // moved between frames. GPU Scene seeds PreviousTransform equal to
        // CurrentTransform on a fresh slot, so a first-frame instance reads as
        // static, which is correct — it has not moved.
        //
        // Bitwise, not float ==: these are the exact bytes GPU Scene encoded,
        // and "did the record change" is a byte question, not a numeric one.
        const bool moved = !Math::BitwiseEqual(instance.CurrentTransform, instance.PreviousTransform);
        return moved ? GeometryClass::RigidDynamic : GeometryClass::Static;
    }

    std::optional<BuildReason> RayTracingScene::DecideBuild(GeometryClass previousClass, GeometryClass currentClass,
                                                            bool geometryChanged, bool hasBlas, u32 consecutiveRefits)
    {
        if (UpdatePolicyFor(currentClass) == UpdatePolicy::Never)
        {
            return std::nullopt;
        }
        if (!hasBlas)
        {
            return BuildReason::FirstBuild;
        }
        // A class change under a stable identity means the BLAS was built with
        // the wrong flags (an opaque BLAS for what is now a cutout mesh, or a
        // compacted BLAS for what is now deformed). Neither can be fixed by a
        // refit.
        if (previousClass != currentClass)
        {
            return BuildReason::ClassChanged;
        }
        if (geometryChanged)
        {
            return BuildReason::GeometryChanged;
        }
        if (UpdatePolicyFor(currentClass) == UpdatePolicy::RefitOrRebuild)
        {
            // The documented heuristic. A refit reuses the tree built for the
            // ORIGINAL vertex positions, so its quality decays as the vertices
            // drift; after kMaxConsecutiveRefits the run is broken with a full
            // rebuild. The counter is a run length, not a total, so a mesh
            // that stops deforming stops rebuilding.
            return consecutiveRefits >= kMaxConsecutiveRefits ? BuildReason::DeformedRefitBudget
                                                              : BuildReason::DeformedRefit;
        }
        // Build-once classes with an unchanged record: nothing to do. This is
        // the acceptance criterion "rigid transform changes update TLAS state
        // without rebuilding static BLASes" — a moved rigid instance changes
        // its INSTANCE record, never its GEOMETRY record, so it never gets
        // here with geometryChanged set.
        return std::nullopt;
    }

    TlasBuildReason RayTracingScene::DecideTlasBuild(u32 previousInstanceCount, u32 currentInstanceCount,
                                                     bool topologyChanged, bool renderOriginRebased, bool everBuilt)
    {
        if (!everBuilt)
        {
            return TlasBuildReason::FirstBuild;
        }
        if (currentInstanceCount > previousInstanceCount)
        {
            return TlasBuildReason::InstanceCountGrew;
        }
        // A refit cannot re-point an instance at a different BLAS, so any
        // change to which acceleration structures are referenced is a rebuild.
        if (topologyChanged)
        {
            return TlasBuildReason::TopologyChanged;
        }
        if (renderOriginRebased)
        {
            return TlasBuildReason::RenderOriginRebased;
        }
        // Shrinking is allowed to refit down to a point: a TLAS built for N
        // instances can be updated with fewer, but the empty space is dead
        // weight in the tree. Rebuild once it has halved.
        if (currentInstanceCount * 2u < previousInstanceCount)
        {
            return TlasBuildReason::InstanceCountShrank;
        }
        return TlasBuildReason::Update;
    }

    void RayTracingScene::Update(const GPUScene& scene)
    {
        m_Stats.Frame.Reset();
        if (!IsAvailable())
        {
            return;
        }

        ++m_FrameNumber;
        const glm::vec3 renderOrigin = scene.GetRenderOrigin();
        // A camera-relative origin rebase re-encodes every instance transform
        // in the same frame, so it is a whole-TLAS event rather than N
        // per-instance ones. Compared bitwise for the same reason as the
        // transforms: these are the exact bytes GPU Scene encoded against, and
        // a spurious "rebased" costs one extra rebuild while a missed one
        // leaves every instance in the wrong place.
        const bool renderOriginRebased = m_HasRenderOrigin && !Math::BitwiseEqual(renderOrigin, m_RenderOrigin);
        m_RenderOrigin = renderOrigin;
        m_HasRenderOrigin = true;

        m_Instances.clear();
        m_PendingBuilds.clear();
        m_PendingRetires.clear();

        ResidentCounters resident{};
        // NOT seeded from renderOriginRebased. Both force a rebuild, but they
        // are different ANSWERS to "why", and the rebuild reason is a
        // telemetry item the issue asks for by name — conflating them makes
        // every rebase read as a topology change in the counters.
        bool topologyChanged = false;

        // A BLAS is per GEOMETRY, an instance is per INSTANCE, and several
        // instances routinely share one geometry. So the instance walk
        // ACCUMULATES per-geometry demand and a second pass decides the builds:
        // pushing a build inside the instance loop would hand
        // vkCmdBuildAccelerationStructuresKHR two entries writing the same
        // destination structure, which is invalid usage.
        struct GeometryDemand
        {
            const GPUSceneGeometry* Record = nullptr;
            UpdatePolicy Policy = UpdatePolicy::BuildOnce;
            // The class this geometry is COUNTED as. Distinct from Policy
            // because "how is this BLAS updated" and "what is it" are
            // different questions: a Masked mesh is still built once.
            GeometryClass ReportedClass = GeometryClass::Static;
            u64 Fingerprint = 0;
        };
        std::unordered_map<GeometryKey, GeometryDemand, GeometryKeyHash> demand;

        const u32 instanceSlots = scene.GetInstanceSlotCount();
        for (u32 slot = 0; slot < instanceSlots; ++slot)
        {
            const GPUSceneInstance* instance = scene.GetLiveInstanceRecordBySlot(slot);
            if (instance == nullptr)
            {
                continue;
            }

            const GPUSceneGeometry* geometry =
                scene.GetLiveGeometryRecordBySlot(instance->GeometryIndex, instance->GeometryGeneration);
            const GPUSceneMaterial* material =
                scene.GetLiveMaterialRecordBySlot(instance->MaterialIndex, instance->MaterialGeneration);

            const GeometryClass geometryClass = Classify(*instance, geometry, material);
            if (geometryClass == GeometryClass::Unsupported)
            {
                // Counted per INSTANCE, and reported as an instance count:
                // rejection is an instance-level verdict (a dead material slot
                // rejects one instance of a mesh other instances still trace).
                ++resident.UnsupportedInstances;
                ++m_Stats.Frame.InstancesSkipped;
                continue;
            }

            const GeometryKey key{ instance->GeometryIndex, instance->GeometryGeneration };
            GeometryDemand& entry = demand[key];
            entry.Record = geometry;
            entry.Fingerprint = FingerprintGeometry(*geometry);
            // The strictest policy any instance of this geometry asks for
            // wins: one deformed user means the shared BLAS has to stay
            // refittable for everyone.
            if (UpdatePolicyFor(geometryClass) == UpdatePolicy::RefitOrRebuild)
            {
                entry.Policy = UpdatePolicy::RefitOrRebuild;
            }
            // ...and the strictest CLASS is what the geometry is reported as,
            // for the same reason. Ordering is the enum's own most-restrictive
            // -first order, so a mesh used both opaquely and as a cutout
            // reports once, as Masked.
            entry.ReportedClass = std::min(entry.ReportedClass, geometryClass);

            InstanceRecord record{};
            record.Transform[0] = instance->CurrentTransform.Row0;
            record.Transform[1] = instance->CurrentTransform.Row1;
            record.Transform[2] = instance->CurrentTransform.Row2;
            record.CustomIndex = instance->StableIndex;
            record.Mask = PackInstanceMask(instance->VisibilityMask);
            // Opacity is decided PER INSTANCE, not baked into the shared BLAS:
            // VK_GEOMETRY_INSTANCE_FORCE_OPAQUE / FORCE_NO_OPAQUE override the
            // geometry's own flag, so one mesh used opaquely by one entity and
            // as a cutout by another needs one BLAS, not two.
            record.ForceOpaque = !RequiresCandidateConfirmation(geometryClass);
            record.Geometry = key;
            m_Instances.push_back(record);
        }

        // Second pass: at most one build request per geometry.
        for (const auto& [key, entry] : demand)
        {
            auto found = m_Blas.find(key);
            const bool known = found != m_Blas.end();
            const GeometryClass buildClass =
                entry.Policy == UpdatePolicy::RefitOrRebuild ? GeometryClass::Deformed : GeometryClass::Static;
            const GeometryClass previousClass = known ? found->second.Class : buildClass;
            const bool geometryChanged = known && found->second.GeometryFingerprint != entry.Fingerprint;
            const u32 consecutiveRefits = known ? found->second.ConsecutiveRefits : 0u;

            if (!known)
            {
                topologyChanged = true;
            }

            const auto reason = DecideBuild(previousClass, buildClass, geometryChanged,
                                            known && m_Backend->IsBlasResident(key), consecutiveRefits);

            // ONE structure, counted once. This loop is per unique geometry,
            // which is what a BLAS is — the instance walk above would have
            // counted a mesh drawn 500 times as 500 acceleration structures.
            ++resident.BlasByClass[static_cast<sizet>(entry.ReportedClass)];

            BlasState& state = m_Blas[key];
            state.Class = buildClass;
            state.GeometryFingerprint = entry.Fingerprint;
            state.LastSeenFrame = m_FrameNumber;
            if (reason.has_value())
            {
                // A refit extends the run; any full rebuild resets it. A
                // geometry that stops deforming therefore keeps its run length
                // rather than rebuilding on the next change.
                state.ConsecutiveRefits = *reason == BuildReason::DeformedRefit ? consecutiveRefits + 1u : 0u;
                m_PendingBuilds.push_back(BlasBuildRequest{
                    .Key = key,
                    .Class = buildClass,
                    .Reason = *reason,
                    .VertexAddress = entry.Record->VertexAddress,
                    .IndexAddress = entry.Record->IndexAddress,
                    .VertexStride = static_cast<u32>(sizeof(Vertex)),
                    .VertexCount = entry.Record->VertexCount,
                    .FirstIndex = entry.Record->FirstIndex,
                    .IndexCount = entry.Record->IndexCount,
                    .BaseVertex = entry.Record->BaseVertex,
                });
            }
        }

        // Retire every BLAS whose geometry did not come back this frame. GPU
        // Scene's removal mechanism is "not re-staged", so this is the only
        // signal there is — and it is why a stale TLAS record cannot survive:
        // the instance list is rebuilt from live records every frame, never
        // patched, so a dead instance is simply absent from the next build.
        for (const auto& [key, state] : m_Blas)
        {
            if (state.LastSeenFrame != m_FrameNumber)
            {
                m_PendingRetires.push_back(key);
            }
        }
        for (const GeometryKey& key : m_PendingRetires)
        {
            m_Backend->RetireBlas(key);
            m_Blas.erase(key);
            ++m_Stats.Frame.BlasRetired;
            topologyChanged = true;
        }

        // Builds first: the TLAS build reads the BLASes, and both are recorded
        // into the same command buffer in this order.
        //
        // CALLED UNCONDITIONALLY, even with nothing to build. Compaction is a
        // multi-frame handshake polled from inside this call, and a settled
        // scene — which is most scenes, most of the time — produces no builds
        // at all. Guarding this on a non-empty list is how compaction silently
        // never completes in production while a test that calls the backend
        // directly still passes.
        const u32 recorded = m_Backend->RecordBlasBuilds(m_PendingBuilds);
        if (recorded < m_PendingBuilds.size())
        {
            OLO_CORE_WARN("[RayTracing] {} of {} BLAS builds could not be recorded this frame",
                          m_PendingBuilds.size() - recorded, m_PendingBuilds.size());
        }

        // Belt to the retire loop's braces: a build that failed above would
        // otherwise leave an instance in the TLAS pointing at no structure.
        std::erase_if(m_Instances, [this](const InstanceRecord& record)
                      { return !m_Backend->IsBlasResident(record.Geometry); });

        const u32 instanceCount = static_cast<u32>(m_Instances.size());
        const TlasBuildReason requested = DecideTlasBuild(m_PreviousInstanceCount, instanceCount, topologyChanged,
                                                          renderOriginRebased, m_EverBuiltTlas);
        const TlasBuildReason used = m_Backend->RecordTlasBuild(m_Instances, requested);
        if (used == TlasBuildReason::Update)
        {
            ++m_Stats.Frame.TlasUpdates;
        }
        else
        {
            ++m_Stats.Frame.TlasBuilds;
        }
        m_Stats.LastTlasReason = used;
        m_Stats.Frame.InstancesTraced = instanceCount;
        m_PreviousInstanceCount = instanceCount;
        m_EverBuiltTlas = true;

        resident.TlasInstances = instanceCount;
        m_Stats.Resident = resident;
        m_Backend->PublishStats(m_Stats);
    }

    void RayTracingScene::RecordBuildToReadBarrier()
    {
        if (IsAvailable())
        {
            m_Backend->RecordBuildToReadBarrier();
        }
    }

    u64 RayTracingScene::GetTlasDeviceAddress() const
    {
        return IsAvailable() ? m_Backend->GetTlasDeviceAddress() : 0u;
    }
} // namespace OloEngine::RayTracing
