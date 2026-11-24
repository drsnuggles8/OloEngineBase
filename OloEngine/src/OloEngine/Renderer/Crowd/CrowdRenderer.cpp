#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Crowd/CrowdRenderer.h"

#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Crowd/CrowdGeometry.h"
#include "OloEngine/Renderer/Crowd/CrowdLod.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"
#include "OloEngine/Renderer/RayTracing/RayTracingTypes.h"

#include <array>
#include <cmath>
#include <limits>

namespace OloEngine
{
    namespace
    {
        constexpr u32 kShadowPhases = CrowdCoarseIntervals;
        struct Surface
        {
            Ref<MeshSource> Source;
            TArray<Ref<Mesh>> Meshes;

            bool Set(Ref<MeshSource> source)
            {
                if (!source || source->GetSubmeshes().IsEmpty())
                    return false;
                Source = std::move(source);
                for (i32 i = 0; i < Source->GetSubmeshes().Num(); ++i)
                    Meshes.Add(Ref<Mesh>::Create(Source, static_cast<u32>(i)));
                return true;
            }
        };

        f32 ClipPhase(f32 cycles, bool loop)
        {
            if (!std::isfinite(cycles))
                return 0.0f;
            return loop ? cycles - std::floor(cycles) : std::clamp(cycles, 0.0f, 1.0f);
        }

        struct PartMaterial
        {
            const Material& MaterialToDraw;
            GPUSceneMaterialKey Key;
            u32 VisibilityMask;
        };

        PartMaterial ExtractCrowdMaterial(u64 owner, const Ref<MeshSource>& source, u32 part, const CrowdMaterials& materials)
        {
            const auto* imported = source->GetImportedMaterialPtrForSubmesh(part);
            const auto* patch = ResolveMaterialPatch(materials.Patches, imported);
            const auto& material = ResolveSubmeshMaterial(materials.Override, patch, imported, materials.Default);
            const auto key = Renderer3D::ResolveGPUSceneMaterialKey(materials.Override, patch, owner, source, imported,
                                                                    source->GetSubmeshes()[part].m_MaterialIndex, materials.Lane);
            Renderer3D::ExtractGPUSceneMaterial(key, material);
            const u32 mask = materials.CastShadows && !material.GetFlag(MaterialFlag::DisableShadowCasting)
                                 ? GPUSceneInstanceInput{}.m_VisibilityMask
                                 : RayTracing::kVisibilityMaskNoShadowCast;
            return { material, key, mask };
        }

        // Clips in a cross-fade and rigs outside the atlas budget can still
        // use the ordinary independently posed draw. A failed allocation
        // must never make a whole population disappear.
        void SubmitIndividual(CrowdIndividual& individual, u64 owner, i32 pickingID, const Ref<MeshSource>& source, const Ref<Skeleton>& rig,
                              const AnimationStateComponent& animation, std::span<const InstanceData> members,
                              const CrowdMaterials& materials, bool shadows)
        {
            for (u32 part = 0; part < static_cast<u32>(source->GetSubmeshes().Num()); ++part)
            {
                const auto [material, key, visibilityMask] = ExtractCrowdMaterial(owner, source, part, materials);
                const auto mesh = Ref<Mesh>::Create(source, part);
                for (sizet row = 0; row < members.size(); ++row)
                {
                    const auto& member = members[row];
                    individual.SubmitPart(owner, pickingID, mesh, rig, animation, member, material, shadows, key, visibilityMask);
                }
            }
        }
    } // namespace

    struct CrowdRenderer::Owner
    {
        u64 Serial = std::numeric_limits<u64>::max();
        std::unordered_map<u64, glm::mat4> Transforms, PreviousTransforms;
        TArray<InstanceData> Members;

        std::span<const InstanceData> Prepare(std::span<const InstanceData> placements, sizet inlineCount, i32 pickingID)
        {
            const u64 frame = Renderer3D::GetPhysicalFrameSerial();
            if (Serial != frame)
            {
                const bool continuous = Serial != std::numeric_limits<u64>::max() && frame == Serial + 1;
                Serial = frame;
                PreviousTransforms.swap(Transforms);
                Transforms.clear();
                Members.Reset();
                Members.Reserve(placements.size());
                for (sizet i = 0; i < placements.size(); ++i)
                {
                    InstanceData member = placements[i];
                    if (i >= inlineCount)
                        member.StableID |= InstancedMeshComponent::AssetStableIDNamespace;
                    if (member.EntityID < 0)
                        member.EntityID = pickingID;
                    member.Normal = glm::transpose(glm::inverse(member.Transform));
                    const auto old = PreviousTransforms.find(member.StableID);
                    const bool hasPrevious = continuous && old != PreviousTransforms.end();
                    member.PrevTransform = hasPrevious ? old->second : member.Transform;
                    // A member moving to a new clip needs fresh bone history
                    // but its world transform still belongs to the same actor.
                    member.CrowdPose.y = hasPrevious ? 1u : 0u;
                    Transforms.emplace(member.StableID, member.Transform);
                    Members.Add(member);
                }
            }
            return { Members.GetData(), static_cast<sizet>(Members.Num()) };
        }
    };

    struct CrowdRenderer::Batch
    {
        Ref<AnimationClip> Clip;
        Ref<Skeleton> Rig;
        CrowdClip Baked;
        std::array<Surface, 3> Tiers;
        std::array<std::array<Surface, kShadowPhases + 1>, 2> Proxies;
        struct Caster
        {
            u64 ID = 0;
            CrowdLod Tier = CrowdLod::Unseen;
            u32 Cohort = 0;
        };
        struct View
        {
            i32 CountSource = -1;
            TArray<Ref<CrowdCull>> Cullers;
            TArray<Caster> Casters;
            u64 LastFrame = std::numeric_limits<u64>::max();
        };
        std::unordered_map<u64, View> Views;
        u32 MemberCount = 0;
        glm::vec4 LocalSphere{};
        glm::vec4 CullSphere{};
        u64 Serial = std::numeric_limits<u64>::max();
        f32 Clock = 0.0f, PreviousClock = 0.0f;
        bool Discontinuous = true;

        bool Init(const Ref<MeshSource>& source, const Ref<Skeleton>& rig, const Ref<AnimationClip>& clip)
        {
            Clip = clip;
            Rig = rig;
            if (!BakeCrowdClip(*rig, clip, Baked) || !Tiers[0].Set(source) ||
                !Tiers[1].Set(MakeCrowdMidGeometry(source)) || !Tiers[2].Set(MakeCrowdFarGeometry(source)))
                return false;
            const i32 parts = Tiers[0].Meshes.Num();
            const auto coarse = SampleCoarseCrowdClip(Baked);
            for (u32 level = 0; level < 2; ++level)
            {
                if (Tiers[level + 1].Meshes.Num() != parts)
                    return false;
                for (u32 phase = 0; phase <= kShadowPhases; ++phase)
                {
                    if (!Proxies[level][phase].Set(BakeCrowdProxy(Tiers[level + 1].Source, coarse, phase)))
                        return false;
                }
            }
            return CalculateClipBounds(*source);
        }

        bool CalculateClipBounds(const MeshSource& source)
        {
            // Compute the visual diameter from actual vertices. MeshSource's
            // general-purpose skinned bound includes a large culling margin.
            const auto& verts = source.GetVertices();
            if (verts.IsEmpty())
                return false;
            glm::vec3 lo = verts[0].Position, hi = lo;
            for (const auto& vertex : verts)
            {
                lo = glm::min(lo, vertex.Position);
                hi = glm::max(hi, vertex.Position);
            }
            LocalSphere = glm::vec4(0.5f * (lo + hi), 0.5f * glm::length(hi - lo));
            // Weighted skinning lies inside the union's convex hull. Bound
            // each bone's influencing vertices once, then transform its box
            // at every sampled pose. Linear interpolation stays inside this
            // bound even when a limb swings outside the rest mesh.
            TArray<BoundingBox> boxes;
            boxes.Init(BoundingBox(glm::vec3(std::numeric_limits<f32>::max()), glm::vec3(std::numeric_limits<f32>::lowest())), Baked.BoneCount);
            const auto& influences = source.GetBoneInfluences();
            if (influences.Num() != verts.Num())
                return false;
            for (i32 i = 0; i < verts.Num(); ++i)
                for (u32 lane = 0; lane < 4; ++lane)
                {
                    const u32 bone = influences[i].m_BoneIDs[lane];
                    if (bone < static_cast<u32>(boxes.Num()) && influences[i].m_Weights[lane] > 0.0f)
                    {
                        boxes[bone].Min = glm::min(boxes[bone].Min, verts[i].Position);
                        boxes[bone].Max = glm::max(boxes[bone].Max, verts[i].Position);
                    }
                }
            for (u32 bone = 0; bone < Baked.BoneCount; ++bone)
            {
                const auto& box = boxes[bone];
                if (box.Min.x > box.Max.x)
                    continue;
                for (u32 frame = 0; frame < Baked.FrameCount; ++frame)
                    for (u32 corner = 0; corner < 8; ++corner)
                    {
                        const glm::vec3 v{ corner & 1u ? box.Max.x : box.Min.x,
                                           corner & 2u ? box.Max.y : box.Min.y,
                                           corner & 4u ? box.Max.z : box.Min.z };
                        const glm::vec3 posed = Baked.Bone(frame, bone) * glm::vec4(v, 1.0f);
                        lo = glm::min(lo, posed);
                        hi = glm::max(hi, posed);
                    }
            }
            CullSphere = glm::vec4(0.5f * (lo + hi), 0.5f * glm::length(hi - lo));
            return true;
        }

        void UpdateShadowLods(View& view, const CrowdAnimationComponent& settings, bool loop, std::span<const InstanceData> members) const
        {
            const auto& lodView = Renderer3D::GetLODViewParams();
            const auto& projection = Renderer3D::GetCullProjectionMatrix();
            const bool ortho = std::abs(projection[3][3] - 1.0f) < 1.0e-5f;
            const f32 pixelScale = lodView.ScreenHeight * std::abs(projection[1][1]);
            const bool reset = Discontinuous || view.LastFrame + 1 != Serial;
            view.LastFrame = Serial;
            std::unordered_map<u64, CrowdLod> previousTiers;
            const bool orderChanged = !reset && (view.Casters.Num() != static_cast<i32>(members.size()) ||
                                                 !std::equal(view.Casters.begin(), view.Casters.end(), members.begin(),
                                                             [](const Caster& caster, const InstanceData& member)
                                                             { return caster.ID == member.StableID; }));
            if (orderChanged)
                for (const auto& caster : view.Casters)
                    previousTiers.emplace(caster.ID, caster.Tier);
            view.Casters.SetNum(static_cast<i32>(members.size()), EAllowShrinking::No);
            for (i32 i = 0; i < static_cast<i32>(members.size()); ++i)
            {
                const auto& member = members[i];
                auto& caster = view.Casters[i];
                auto lastTier = !reset && caster.ID == member.StableID ? caster.Tier : CrowdLod::Unseen;
                if (orderChanged)
                {
                    const auto prior = previousTiers.find(member.StableID);
                    lastTier = prior == previousTiers.end() ? CrowdLod::Unseen : prior->second;
                }
                const glm::vec3 center = member.Transform * glm::vec4(glm::vec3(LocalSphere), 1.0f);
                const f32 scale = std::max({ glm::length(glm::vec3(member.Transform[0])), glm::length(glm::vec3(member.Transform[1])), glm::length(glm::vec3(member.Transform[2])) });
                const f32 distance = ortho ? 1.0f : std::max(glm::length(center - lodView.ViewPosition), 0.001f);
                const f32 pixels = LocalSphere.w * scale * pixelScale / distance;
                const f32 offset = std::isfinite(member.Custom) ? member.Custom : 0.0f;
                const f32 phase = ClipPhase(Clock + offset, loop);
                caster.ID = member.StableID;
                caster.Tier = SelectCrowdLod(pixels, lastTier, settings.m_NearPixels, settings.m_FarPixels, settings.m_Hysteresis);
                caster.Cohort = std::min(static_cast<u32>(phase * kShadowPhases), kShadowPhases);
            }
        }

        void Prepare(const AnimationStateComponent& animation, u32 memberCount)
        {
            const u64 serial = Renderer3D::GetPhysicalFrameSerial();
            if (Serial == serial)
                return;
            Discontinuous = Serial == std::numeric_limits<u64>::max() || serial != Serial + 1 || !Rig->HasBoneHistory();
            PreviousClock = Clock;
            Clock = animation.m_CurrentTime / Baked.Duration;
            if (!std::isfinite(Clock))
            {
                Clock = 0.0f;
                Discontinuous = true;
            }
            if (Discontinuous)
                PreviousClock = Clock;
            Serial = serial;
            MemberCount = memberCount;
            std::erase_if(Views, [](const auto& view)
                          { return !Renderer3D::IsViewAlive(RenderViewId{ view.first }); });
        }
    };

    CrowdRenderer::CrowdRenderer() = default;
    CrowdRenderer::~CrowdRenderer() = default;

    void CrowdRenderer::Remove(u64 owner)
    {
        m_Individual.Remove(owner);
        m_Owners.erase(owner);
        std::erase_if(m_Batches, [owner](const auto& entry)
                      { return entry.first.first == owner; });
    }

    void CrowdRenderer::FinishView()
    {
        const auto serial = Renderer3D::GetPhysicalFrameSerial();
        m_Individual.FinishFrame(serial);
        std::erase_if(m_Owners, [serial](const auto& owner)
                      { return owner.second->Serial != serial; });
        std::erase_if(m_Batches, [serial](const auto& entry)
                      { return !entry.second || entry.second->Serial != serial; });
    }

    CrowdVisibilityCounts CrowdRenderer::ReadVisibilityCounts() const
    {
        CrowdVisibilityCounts result;
        for (const auto& [owner, batch] : m_Batches)
        {
            const auto view = batch->Views.find(Renderer3D::GetActiveView().Value);
            if (view == batch->Views.end() || view->second.LastFrame != Renderer3D::GetPhysicalFrameSerial())
                continue;
            result.Members += batch->MemberCount;
            if (view->second.CountSource >= 0)
                for (u32 tier = 0; tier < result.Visible.size(); ++tier)
                {
                    u32 count = 0;
                    RenderCommand::ReadBufferSubData(view->second.Cullers[view->second.CountSource]->Indirect(tier), sizeof(u32), sizeof(count), &count);
                    result.Visible[tier] += count;
                }
        }
        return result;
    }

    bool CrowdRenderer::Submit(u64 owner, i32 pickingID, const Ref<MeshSource>& source, const Ref<Skeleton>& rig,
                               const AnimationStateComponent& animation, const CrowdAnimationComponent& settings,
                               std::span<const InstanceData> placements, sizet inlineCount,
                               const CrowdMaterials& materials, bool castShadows)
    {
        if (!source || !rig)
            return false;
        if (placements.empty())
            return true;
        const u64 resetFrame = Renderer3D::GetLastPoseHistoryResetFrame();
        if (m_LastPoseHistoryResetFrame != resetFrame)
        {
            m_LastPoseHistoryResetFrame = resetFrame;
            m_Owners.clear();
            m_Individual = CrowdIndividual{};
            // A reset invalidates previous poses and per-view LOD history. The
            // baked palettes, meshes and allocated culling buffers are still valid.
            for (auto& entry : m_Batches)
                if (entry.second)
                    entry.second->Serial = std::numeric_limits<u64>::max();
        }
        auto& motion = m_Owners[owner];
        if (!motion)
            motion = std::make_unique<Owner>();
        placements = motion->Prepare(placements, inlineCount, pickingID);
        RendererProfiler::GetInstance().RecordCrowdMembers(static_cast<u32>(placements.size()));
        if (std::ranges::none_of(placements, [](const InstanceData& m)
                                 { return m.GetCrowdClipIndex() >= 0; }))
            return SubmitGroup(owner, -1, pickingID, source, rig, animation, settings, placements, materials, castShadows);

        // Members keep the same canonical (owner, StableID) when changing clips.
        // An override samples its chosen clip at the owner's normalised playback
        // phase; its independent offset remains in Custom. Batches with distinct
        // native playback rates use separate clock owners.
        std::map<i32, TArray<InstanceData>> groups;
        for (sizet i = 0; i < placements.size(); ++i)
        {
            const auto& member = placements[i];
            const i32 clip = member.GetCrowdClipIndex();
            const bool valid = clip >= 0 && static_cast<sizet>(clip) < animation.m_AvailableClips.size() && animation.m_AvailableClips[clip];
            groups[valid ? clip : -1].Add(member);
        }
        for (auto& [index, group] : groups)
        {
            AnimationStateComponent state = animation;
            if (index >= 0)
            {
                const f32 phase = state.m_CurrentClip && state.m_CurrentClip->Duration > 0.0f ? state.m_CurrentTime / state.m_CurrentClip->Duration : 0.0f;
                state.m_CurrentClipIndex = index;
                state.m_CurrentClip = state.m_AvailableClips[index];
                state.m_CurrentTime = phase * state.m_CurrentClip->Duration;
                state.m_Blending = false;
                state.m_NextClip = nullptr;
            }
            const auto count = static_cast<sizet>(group.Num());
            SubmitGroup(owner, index, pickingID, source, rig, state, settings, { group.GetData(), count }, materials, castShadows);
        }
        return true;
    }

    bool CrowdRenderer::SubmitGroup(u64 owner, i32 clipIndex, i32 pickingID, const Ref<MeshSource>& source, const Ref<Skeleton>& rig,
                                    const AnimationStateComponent& animation, const CrowdAnimationComponent& settings,
                                    std::span<const InstanceData> placements,
                                    const CrowdMaterials& materials, bool castShadows)
    {
        if (!animation.m_CurrentClip || animation.m_Blending || animation.m_RootMotion)
        {
            SubmitIndividual(m_Individual, owner, pickingID, source, rig, animation, placements, materials, castShadows);
            return true;
        }
        auto& cache = m_Batches[{ owner, clipIndex }];
        if (!cache || cache->Tiers[0].Source != source || cache->Rig != rig || cache->Clip != animation.m_CurrentClip)
        {
            auto candidate = std::make_unique<Batch>();
            if (!candidate->Init(source, rig, animation.m_CurrentClip))
            {
                SubmitIndividual(m_Individual, owner, pickingID, source, rig, animation, placements, materials, castShadows);
                return true;
            }
            cache = std::move(candidate);
        }
        Batch& batch = *cache;
        batch.Prepare(animation, static_cast<u32>(placements.size()));
        auto& view = batch.Views[Renderer3D::GetActiveView().Value];
        auto& cullers = view.Cullers;
        const auto parts = static_cast<u32>(source->GetSubmeshes().Num());
        cullers.SetNum(parts, EAllowShrinking::No);
        batch.UpdateShadowLods(view, settings, animation.m_Loop, placements);
        view.CountSource = -1;
        u32 packets = 0;
        for (u32 part = 0; part < parts; ++part)
        {
            const auto [material, key, visibilityMask] = ExtractCrowdMaterial(owner, source, part, materials);
            std::array<CrowdDrawRange, 3> ranges;
            for (u32 tier = 0; tier < ranges.size(); ++tier)
            {
                const auto& mesh = batch.Tiers[tier].Meshes[part];
                ranges[tier] = { mesh->GetIndexCount(), mesh->GetBaseIndex() };
            }
            if (!cullers[part])
                cullers[part] = Ref<CrowdCull>::Create();
            auto& cull = *cullers[part];
            const auto members = placements;
            const bool sortable = !material.GetFlag(MaterialFlag::Blend) && material.GetAlphaMode() != AlphaMode::Blend &&
                                  material.GetTransmissionFactor() <= 0.0f && !material.GetShader();
            const std::array<Ref<Mesh>, 3> meshes{ batch.Tiers[0].Meshes[part], batch.Tiers[1].Meshes[part], batch.Tiers[2].Meshes[part] };
            const bool batched = sortable && cull.Dispatch(members, batch.Baked, batch.Clock, batch.PreviousClock, animation.m_Loop, batch.Discontinuous, settings, batch.LocalSphere, batch.CullSphere, ranges) &&
                                 Renderer3D::SubmitCrowdTiers(meshes, material, static_cast<u32>(members.size()), cull);
            if (batched && view.CountSource < 0)
                view.CountSource = static_cast<i32>(part);
            if (!batched)
            {
                for (const auto& member : members)
                    m_Individual.SubmitPart(owner, pickingID, meshes[0], rig, animation, member, material, castShadows, key, visibilityMask);
                continue;
            }
            packets += 3u;

            for (const auto& member : members)
            {
                // Casters outside the camera frustum still enter each light's
                // cull. Rigid cohort VAOs batch through both CSM and VSM and
                // give ray tracing a posed surface with shared BLAS geometry.
                const auto& caster = view.Casters[static_cast<sizet>(&member - members.data())];
                const auto& proxy = batch.Proxies[caster.Tier == CrowdLod::Far ? 1 : 0][caster.Cohort];
                const auto& mesh = proxy.Meshes[part];
                (void)Renderer3D::ExtractGPUSceneMesh(owner, member.StableID, proxy.Source, part, member.Transform, key,
                                                      GPUSceneDrawLinkRequest::None, {}, InstanceTint(member.Color), visibilityMask);
                if (castShadows && material.GetAlphaMode() == AlphaMode::Opaque && !material.GetFlag(MaterialFlag::DisableShadowCasting))
                {
                    Renderer3D::AddMeshShadowCaster(mesh->GetVertexArray()->GetRHIHandle(), mesh->GetIndexCount(),
                                                    mesh->GetBaseIndex(), member.Transform, {}, mesh->GetTransformedBoundingBox(member.Transform),
                                                    material.GetFlag(MaterialFlag::TwoSided));
                }
            }
        }
        RendererProfiler::GetInstance().RecordAnimatedSubmission(packets, 0.0);
        return true;
    }
} // namespace OloEngine
