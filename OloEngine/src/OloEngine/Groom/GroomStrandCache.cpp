#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomStrandCache.h"

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Groom/GroomLod.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Texture3D.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <algorithm>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // How many coat volumes may be resident at once. A budget rather than
        // "as many as ask", because the representation is a 3D texture whose size
        // grows with the CUBE of its resolution.
        constexpr u32 kMaxResidentCoatVolumes = 8;

        // Releasing a coat volume is THREE steps that must never be separated:
        // give the bytes back to the cache total, drop the texture, and clear
        // the resolution that says a bake is resident.
        void ReleaseCoatVolume(GroomStrandCache::Entry& entry, u64& cacheBytes) noexcept
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
        constexpr u64 kCacheRetentionFrames = 60;

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

    void GroomStrandCache::BeginFrame()
    {
        OLO_PROFILE_FUNCTION();

        // EVICT FIRST, THEN ADVANCE. Eviction reads LastUsedTick against the
        // tick it is retiring, so running it before the increment keeps the
        // retention window meaning "frames since last used" rather than
        // "frames since last used, minus one".
        EvictToBudget();
        m_Stats.Reset();
        m_ShadowStats.Reset();
        ++m_Tick;
    }

    u64 GroomStrandCache::CacheKey(const GroomStrandRequest& request) noexcept
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
        // duplicate set of GPU buffers behind.
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
        key = mix(key, request.Build.CoatDigest);
        // The REPRESENTATION (#1252). A card level and the base groom are two
        // different curve sets, and at the same budget they would otherwise
        // hash to the same key.
        key = mix(key, static_cast<u64>(std::to_underlying(request.Lod.Representation)));
        // A DEFORMED groom's vertices depend on a body's pose, so its geometry
        // is per ENTITY: two characters sharing one groom asset at one budget
        // must not share one buffer.
        if (IsDeformed(request))
        {
            key = mix(key, static_cast<u64>(static_cast<u32>(request.EntityID)));
            key = mix(key, 0x1249ull);
        }
        return key;
    }

    bool GroomStrandCache::IsDeformed(const GroomStrandRequest& request) noexcept
    {
        // The transform array must span the whole groom, not merely be
        // non-empty: the build indexes it by curve, and a short array would read
        // past its end on the first strand past the boundary.
        return request.Groom && request.Binding &&
               request.RootTransforms.size() == request.Groom->GetCurveCount();
    }

    GroomStrandCache::Entry* GroomStrandCache::AcquireGeometry(const GroomStrandRequest& request)
    {
        OLO_PROFILE_FUNCTION();

        if (!request.Groom)
        {
            return nullptr;
        }

        const bool deformed = IsDeformed(request);
        const u64 key = CacheKey(request);

        const auto existing = m_Entries.find(key);
        if (existing != m_Entries.end())
        {
            // The settings are in the KEY, so a hit is already a settings
            // match; the comparison survives only to catch a hash collision,
            // which would otherwise hand back geometry built for a different
            // budget.
            const bool usable = existing->second.Settings == request.Build && existing->second.Array &&
                                existing->second.Dynamic == deformed;
            // ONE BUILD PER KEY PER FRAME (#1323). Both the shadow pass and the
            // strand pass acquire the same key in one frame, and a DEFORMED
            // groom rebuilds unconditionally — so without this the body's pose
            // would be re-evaluated and the vertex buffer refilled twice, at
            // twice the CPU cost, and DeformedRebuilds would read two for a
            // coat that is behaving. An UNBOUND groom already short-circuits on
            // the next line for the same reason.
            if (usable && (!deformed || existing->second.LastUsedTick == m_Tick))
            {
                existing->second.LastUsedTick = m_Tick;
                return &existing->second;
            }
            if (!usable)
            {
                m_Bytes -= std::min(m_Bytes, existing->second.Bytes);
                ReleaseCoatVolume(existing->second, m_Bytes);
                m_Entries.erase(existing);
            }
        }

        // A bound groom is rebuilt EVERY frame and its buffers are refilled in
        // place. There is no skip-if-unchanged here, deliberately: the pose that
        // would have to be compared lives on the tick thread and is rewritten
        // before this runs, so a comparison made here would be against the
        // wrong frame's palette.
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
        // holds.
        const GroomCoatContext coat{ &request.Coat, request.Groom->GetGroupCoats() };
        const GroomStrandSimulation simulation = request.Simulation();
        // THE CURVE SET THE LOD SELECTED (#1252).
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
        // GPU buffer every frame for every bound groom in the scene.
        if (const auto entryIt = m_Entries.find(key); deformed && entryIt != m_Entries.end())
        {
            Entry& entry = entryIt->second;
            if (entry.Dynamic && entry.Array && entry.Vertices && entry.Stats.VertexCount == stats.VertexCount &&
                entry.Stats.IndexCount == stats.IndexCount)
            {
                entry.Vertices->SetData({ vertices.data(), static_cast<u32>(stats.VertexBytes) });
                entry.Stats = stats;
                entry.LastUsedTick = m_Tick;
                ++m_Stats.DeformedRebuilds;
                return &entry;
            }
            m_Bytes -= std::min(m_Bytes, entry.Bytes);
            ReleaseCoatVolume(entry, m_Bytes);
            m_Entries.erase(entryIt);
        }

        Entry entry;
        entry.Settings = request.Build;
        entry.Stats = stats;
        entry.Bytes = stats.VertexBytes + stats.IndexBytes;
        entry.LastUsedTick = m_Tick;
        entry.Dynamic = deformed;

        if (deformed)
        {
            // Sized-then-filled, so the buffer is created with a usage the
            // backend can refill. Create(data, size) mints an immutable one on
            // the GL backend, and SetData on it is a silent no-op — the coat
            // would render at whatever pose it was first built in, forever,
            // with nothing logged.
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

        m_Bytes += entry.Bytes;
        ++m_Stats.CacheBuilds;

        OLO_CORE_TRACE("GroomStrandCache: built strand geometry for groom {} — {} of {} strands (stride {}), "
                       "{} segments, {:.2f} MiB",
                       static_cast<u64>(request.Handle), stats.StrandsSelected, stats.StrandsAvailable, stats.Stride,
                       stats.SegmentCount, static_cast<f64>(entry.Bytes) / (1024.0 * 1024.0));

        const auto [it, inserted] = m_Entries.emplace(key, std::move(entry));
        return inserted ? &it->second : nullptr;
    }

    u32 GroomStrandCache::CountResidentCoatVolumes() const noexcept
    {
        u32 resident = 0;
        for (const auto& [key, entry] : m_Entries)
        {
            if (entry.CoatVolume)
            {
                ++resident;
            }
        }
        return resident;
    }

    bool GroomStrandCache::ReclaimLeastRecentlyUsedCoatVolume()
    {
        Entry* oldest = nullptr;
        for (auto& [key, entry] : m_Entries)
        {
            if (!entry.CoatVolume)
            {
                continue;
            }
            // NEVER an entry used this frame: its draw has already been
            // recorded against the texture this would free, and the bind is
            // per-draw rather than deferred.
            if (entry.LastUsedTick == m_Tick)
            {
                continue;
            }
            if (oldest == nullptr || entry.LastUsedTick < oldest->LastUsedTick)
            {
                oldest = &entry;
            }
        }
        if (oldest == nullptr)
        {
            return false;
        }
        ReleaseCoatVolume(*oldest, m_Bytes);
        return true;
    }

    // Rebuilds this coat's density volume when the resident bake is not the one
    // the frame wants, and reports what the coat actually gets.
    //
    // THE BAKE IS IN GROOM OBJECT SPACE. A coat that merely MOVES therefore
    // reuses it — which is most of the update policy, and is why an animated
    // light or a walking character costs zero rebuilds.
    GroomCoatShadowDecision GroomStrandCache::AcquireCoatVolume(const GroomStrandRequest& request, Entry& entry,
                                                                u32& residentVolumes, f32 viewportHeight)
    {
        OLO_PROFILE_FUNCTION();

        GroomCoatShadowInputs inputs;
        inputs.Requested = request.CoatShadow;
        inputs.SegmentCount = entry.Stats.SegmentCount;
        // Both volume modes need a 3D texture; the RHI exposes no capability
        // query for one, and every backend the engine ships can create one.
        inputs.VolumeTexturesSupported = true;
        // A density volume is light-independent, so it needs no directional
        // light.
        inputs.HasDirectionalLight = true;
        inputs.MinResolution = request.CoatLod.MinResolution;

        // A BOUND, DEFORMING GROOM IS STILL REFUSED (#1323 restated the
        // deferral rather than closing it). The bake reads the GroomAsset's
        // REST-POSE curves, so on a character whose body animates the drawn
        // strands move and the volume does not.
        //
        // #1323 LIFTED THE GEOMETRY CACHE, which is the plumbing half this was
        // waiting on — and the remaining half is a COST contract, not plumbing.
        // The volume's whole update policy rests on the bake being
        // light-independent and object-space, so a static coat rebuilds NEVER
        // (groom-coat-self-shadowing.md rule 6). A deformed coat's strands move
        // every frame, so its bake would be per entity AND per frame: eight
        // resident 64³ RGBA32F volumes is 128 MiB of re-upload per frame at the
        // resident cap. That is a different feature with a different budget, and
        // approximating it by rebuilding "sometimes" is exactly the stale-volume
        // flicker criterion 3 was written against.
        //
        // Refused before the bake, not after: the bake does not branch on the
        // mode, so building here and refusing later would spend a volume's
        // memory and build time on a draw that will render unshadowed.
        if (!GroomCoatShadowModeIsImplemented(request.CoatShadow))
        {
            ReleaseCoatVolume(entry, m_Bytes);
            inputs.ResolvedResolution = request.CoatLod.BaseResolution;
            inputs.RepresentationReady = false;
            inputs.GrantedSlot = kNoGroomCoatShadowSlot;
            return SelectGroomCoatShadow(inputs);
        }

        inputs.GroomIsDeformed = IsDeformed(request);
        if (inputs.GroomIsDeformed && request.CoatShadow != GroomCoatShadowTechnique::None)
        {
            ReleaseCoatVolume(entry, m_Bytes);
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
            // come with them.
            const f32 transformScale = (glm::length(glm::vec3(request.Transform[0])) +
                                        glm::length(glm::vec3(request.Transform[1])) +
                                        glm::length(glm::vec3(request.Transform[2]))) /
                                       3.0f;
            const f32 radius = glm::length(entry.CoatBoundsMax - entry.CoatBoundsMin) * 0.5f * transformScale;
            const f32 distance = glm::length(worldCentre - Renderer3D::GetCullViewPosition());
            const glm::mat4& projection = Renderer3D::GetCullProjectionMatrix();
            // abs(): Vulkan's clip space has +Y down, so the engine uploads a
            // projection whose [1][1] is negative there. The sign is a clip
            // convention and the MAGNITUDE is what a scale is asking for.
            const f32 cotHalfFov = std::abs(projection[1][1]);
            if (distance > 1.0e-4f && std::isfinite(radius) && std::isfinite(cotHalfFov))
            {
                pixelSize = (2.0f * radius) * cotHalfFov * viewportHeight * 0.5f / distance;
            }
        }

        // BIASED BY THE REPRESENTATION LOD'S SHADOW STEP (#1252).
        const u32 requested = std::min(GroomCoatShadow::SelectCoatLodStep(request.CoatLod, pixelSize) +
                                           request.Lod.ShadowStep,
                                       request.CoatLod.MaxLodSteps);
        entry.CoatLodStableFrames = requested == entry.CoatRequestedLodStep ? entry.CoatLodStableFrames + 1u : 0u;
        entry.CoatRequestedLodStep = requested;
        // Three frames, so a coat sitting exactly on a LOD boundary cannot
        // rebuild its representation every frame. Refining is immediate; only
        // coarsening waits.
        const u32 lodStep =
            GroomCoatShadow::ApplyCoatLodHysteresis(entry.CoatLodStep, requested, entry.CoatLodStableFrames, 3u);
        const u32 resolution = GroomCoatShadow::CoatLodResolution(request.CoatLod, lodStep);
        inputs.ResolvedResolution = resolution;

        // An ALREADY-RESIDENT coat keeps its slot without competing for one, so
        // which coats are shadowed does not depend on entity iteration order.
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
        // areal density the volume stores.
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
            // coat that moves reuses it.
            std::vector<GroomCoatShadow::CoatSegment> segments;
            const u32 emitted =
                GroomCoatShadow::BuildCoatSegments(*request.Groom, glm::mat4(1.0f), sampleSettings, segments);

            GroomCoatShadow::DensityVolumeSettings volumeSettings;
            volumeSettings.Resolution = resolution;
            GroomCoatShadow::DensityVolume volume;

            if (emitted > 0 && GroomCoatShadow::BuildDensityVolume(segments, volumeSettings, volume, nullptr))
            {
                // ONE RGBA texture: xyz = mean fibre direction * coherence,
                // w = areal density.
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
                // size check rejects the only buffer it could be handed.
                spec.Format = Texture3DFormat::RGBA32F;
                // CLAMP, never repeat. A march that leaves the box must read the
                // empty boundary voxel, not wrap round to the other side of the
                // animal.
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
                    // The REAL voxel size, carried rather than re-derived.
                    const glm::vec3 voxelSize = volume.VoxelSize();
                    entry.CoatVoxelSize = std::min({ voxelSize.x, voxelSize.y, voxelSize.z });
                    // Replacing a bake: the old bytes come off before the new
                    // ones go on, or a resolution change leaks the difference.
                    m_Bytes -= std::min(m_Bytes, entry.CoatBytes);
                    entry.CoatBytes = static_cast<u64>(packed.size() * sizeof(f32));
                    m_Bytes += entry.CoatBytes;
                    entry.CoatBuiltTick = m_Tick;
                    entry.CoatMode = request.CoatShadow;
                    ++m_Stats.CoatRebuilds;
                    if (!alreadyResident)
                    {
                        // A slot has just been taken. Counted HERE, where the
                        // texture actually came into existence.
                        ++residentVolumes;
                    }
                }
            }
        }

        // The frame's OWN answer, never the request's expectation.
        inputs.RepresentationReady = entry.CoatVolume != nullptr && entry.CoatResolution == resolution;
        return SelectGroomCoatShadow(inputs);
    }

    void GroomStrandCache::EvictToBudget()
    {
        if (m_Bytes <= m_BudgetBytes)
        {
            return;
        }

        // Oldest first, and never an entry inside the retention window.
        std::vector<std::pair<u64, u64>> candidates;
        candidates.reserve(m_Entries.size());
        for (const auto& [key, entry] : m_Entries)
        {
            if (m_Tick - entry.LastUsedTick < kCacheRetentionFrames)
            {
                continue;
            }
            candidates.emplace_back(entry.LastUsedTick, key);
        }
        std::sort(candidates.begin(), candidates.end());

        for (const auto& [lastUsed, key] : candidates)
        {
            if (m_Bytes <= m_BudgetBytes)
            {
                break;
            }
            const auto it = m_Entries.find(key);
            if (it == m_Entries.end())
            {
                continue;
            }
            // BOTH halves of the entry's footprint. The geometry's bytes and
            // the coat volume's are added to m_Bytes separately, so subtracting
            // only the geometry left the total permanently inflated by every
            // evicted coat.
            ReleaseCoatVolume(it->second, m_Bytes);
            m_Bytes -= std::min(m_Bytes, it->second.Bytes);
            m_Entries.erase(it);
            ++m_Stats.CacheEvictions;
        }

        if (m_Bytes > m_BudgetBytes)
        {
            // Every resident groom is recent, so the budget cannot be met.
            // Said out loud rather than dropping a draw: a groom that vanished
            // to fit a budget is indistinguishable from a broken asset.
            OLO_CORE_WARN("GroomStrandCache: strand cache is {:.1f} MiB over its {:.1f} MiB budget and every entry is "
                          "still in its retention window; nothing was evicted and every groom is still drawn",
                          static_cast<f64>(m_Bytes - m_BudgetBytes) / (1024.0 * 1024.0),
                          static_cast<f64>(m_BudgetBytes) / (1024.0 * 1024.0));
        }
    }

    f32 GroomStrandCache::EffectiveWidthScale(const GroomStrandRequest& request, const Entry& entry) noexcept
    {
        // FROM THE ACHIEVED FRACTION, never the requested one, and this is the
        // only place the achieved fraction exists: the strand budget is spent
        // as an integer STRIDE PER ROLE, so a budget that asked for 0.4 of a
        // role retains a third of it. Compensating by the policy's 1/0.4 would
        // leave the coat a sixth thinner than it started, and the error
        // compounds at every step down the ladder.
        //
        // It MULTIPLIES the authoring width scale rather than replacing it:
        // WidthScale is a unit-scale lever for a groom exported at a different
        // scale, and this is a density correction. Folding them into one number
        // would make turning the LOD off change a coat that was authored at 0.5.
        const f32 achievedFraction =
            entry.Stats.StrandsAvailable > 0u
                ? static_cast<f32>(entry.Stats.StrandsSelected) / static_cast<f32>(entry.Stats.StrandsAvailable)
                : 1.0f;
        const f32 compensation =
            request.LodPolicy.Enabled
                ? GroomLodWidthCompensation(achievedFraction, request.LodPolicy.MaxWidthCompensation)
                : 1.0f;
        return request.WidthScale * compensation;
    }

    void GroomStrandCache::Clear()
    {
        m_Entries.clear();
        m_Bytes = 0;
        m_Tick = 0;
        m_Stats.Reset();
        m_ShadowStats.Reset();
    }
} // namespace OloEngine
