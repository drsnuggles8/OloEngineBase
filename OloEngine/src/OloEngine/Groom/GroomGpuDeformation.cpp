#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomGpuDeformation.h"
#include "OloEngine/Task/ParallelFor.h"

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Math/Math.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace OloEngine
{
    namespace
    {
        // Units (16 bytes) per record of each region. Named so the layout, the
        // byte image and the GLSL twin's strides are one set of numbers.
        constexpr u32 kUnitBytes = 16u;
        constexpr u32 kUnitsPerWeights = 2u;
        constexpr u32 kUnitsPerBind = 2u;
        constexpr u32 kUnitsPerRoot = 4u;
        constexpr u32 kUnitsPerSlot = 1u;
        constexpr u32 kUnitsPerDisplacement = 2u;
        constexpr u32 kUnitsPerSkin = 2u;
        constexpr u32 kUnitsPerVertex = 3u;
        constexpr u32 kUnitsPerBone = 4u; // one mat4
        static_assert(sizeof(GroomRootSkinRecord) == kUnitsPerSkin * kUnitBytes);
        static_assert(sizeof(GroomSurfaceVertexRecord) == kUnitsPerVertex * kUnitBytes);
        static_assert(sizeof(glm::mat4) == kUnitsPerBone * kUnitBytes);

        static_assert(sizeof(GroomGuideWeights) == kUnitsPerWeights * kUnitBytes);
        static_assert(sizeof(GroomDeformBindRecord) == kUnitsPerBind * kUnitBytes);
        static_assert(sizeof(GroomDeformRootRecord) == kUnitsPerRoot * kUnitBytes);
        static_assert(sizeof(GroomDeformSlotRecord) == kUnitsPerSlot * kUnitBytes);
        static_assert(sizeof(GroomDeformDisplacementRecord) == kUnitsPerDisplacement * kUnitBytes);
        static_assert(std::is_trivially_copyable_v<GroomGuideWeights> &&
                          std::is_trivially_copyable_v<GroomDeformRootRecord> &&
                          std::is_trivially_copyable_v<GroomDeformSlotRecord> &&
                          std::is_trivially_copyable_v<GroomDeformDisplacementRecord>,
                      "the byte image is assembled by memcpy");

        [[nodiscard]] glm::vec4 PackQuat(const glm::quat& q) noexcept
        {
            // Spelled out rather than memcpy'd from the quat: glm's storage order
            // is a configuration macro (GLM_FORCE_QUAT_DATA_WXYZ), and the shader
            // reads (x, y, z, w) whatever it is.
            return { q.x, q.y, q.z, q.w };
        }

        [[nodiscard]] glm::quat UnpackQuat(const glm::vec4& v) noexcept
        {
            // (w, x, y, z): the constructor order every other quaternion in the
            // groom code is written in (GroomRootTransform's identity).
            return glm::quat(v.w, v.x, v.y, v.z);
        }

        // Copies `records` into the byte image at `baseUnit`.
        template<typename T>
        void WriteRegion(TArray64<u8>& bytes, u32 baseUnit, std::span<const T> records) noexcept
        {
            if (records.empty())
            {
                return;
            }
            const sizet offset = static_cast<sizet>(baseUnit) * kUnitBytes;
            std::memcpy(bytes.GetData() + offset, records.data(), records.size_bytes());
        }

        template<typename T>
        [[nodiscard]] std::span<const T> AsSpan(const TArray<T>& array, sizet count) noexcept
        {
            return { array.GetData(), std::min(count, static_cast<sizet>(array.Num())) };
        }

        // SampleGroomGuideDisplacement, reading the packed slot and displacement
        // records instead of the simulation's spans. Every reason that function
        // skips a slot is either the same test here or was folded into a zero
        // Count by PackFrame.
        [[nodiscard]] glm::vec3 SampleDisplacement(const GroomDeformBuffer& buffer, u32 rootSlot, f32 t,
                                                   bool previous) noexcept
        {
            const auto weights = buffer.Weights();
            const auto slots = buffer.Slots();
            const auto displacements = buffer.Displacements();
            if (rootSlot >= weights.size() || !std::isfinite(t))
            {
                return glm::vec3(0.0f);
            }

            const f32 parameter = std::clamp(t, 0.0f, 1.0f);
            const GroomGuideWeights& influence = weights[rootSlot];

            glm::vec3 result{ 0.0f };
            f32 appliedWeight = 0.0f;
            for (u32 k = 0; k < GroomGuideInfluenceCount; ++k)
            {
                const u32 slot = influence.Guides[k];
                if (slot >= slots.size() || !(influence.Weights[k] > 0.0f))
                {
                    continue;
                }
                const GroomDeformSlotRecord& record = slots[slot];
                if (record.Count == 0u || static_cast<u64>(record.First) + record.Count > displacements.size())
                {
                    continue;
                }

                const auto sampleAt = [&](u32 index) -> glm::vec3
                {
                    const GroomDeformDisplacementRecord& d = displacements[record.First + index];
                    return glm::vec3(previous ? d.Previous : d.Current);
                };

                if (record.Count == 1u)
                {
                    result += sampleAt(0u) * influence.Weights[k];
                    appliedWeight += influence.Weights[k];
                    continue;
                }

                const f32 scaled = parameter * static_cast<f32>(record.Count - 1u);
                const f32 floored = std::floor(scaled);
                const u32 lower = static_cast<u32>(floored);
                const u32 upper = std::min(lower + 1u, record.Count - 1u);
                const f32 fraction = scaled - floored;
                result += glm::mix(sampleAt(lower), sampleAt(upper), fraction) * influence.Weights[k];
                appliedWeight += influence.Weights[k];
            }

            if (!(appliedWeight > 0.0f))
            {
                return glm::vec3(0.0f);
            }
            return result / appliedWeight;
        }
    } // namespace

    GroomDeformBufferLayout GroomDeformBufferLayout::Make(u32 rootCount, u32 slotCount,
                                                          u32 displacementCapacity) noexcept
    {
        GroomDeformBufferLayout layout;
        layout.RootCount = rootCount;
        layout.SlotCount = slotCount;
        layout.DisplacementCapacity = displacementCapacity;
        // GroomStrandDeform.glsl derives BindBase as RootCount * 2; keep them twins.
        layout.BindBase = rootCount * kUnitsPerWeights;
        layout.RootBase = layout.BindBase + rootCount * kUnitsPerBind;
        layout.SlotBase = layout.RootBase + rootCount * kUnitsPerRoot;
        layout.DisplacementBase = layout.SlotBase + slotCount * kUnitsPerSlot;
        // Never zero units: a zero-sized storage buffer is not a legal
        // allocation on either backend, and a coat with no strands is refused
        // before it gets here anyway.
        layout.TotalUnits = std::max(1u, layout.DisplacementBase + displacementCapacity * kUnitsPerDisplacement);
        return layout;
    }

    GroomDeformBufferLayout GroomDeformBufferLayout::Make(u32 rootCount, u32 slotCount, u32 displacementCapacity,
                                                          u32 vertexCount, u32 boneCount) noexcept
    {
        GroomDeformBufferLayout layout = Make(rootCount, slotCount, displacementCapacity);
        if (vertexCount == 0u || boneCount == 0u)
        {
            return layout;
        }
        layout.VertexCount = vertexCount;
        layout.BoneCount = boneCount;
        // After everything the vertex stage reads, so its layout -- and every
        // lane it is handed -- is unchanged.
        layout.SkinBase = layout.DisplacementBase + displacementCapacity * kUnitsPerDisplacement;
        layout.VertexBase = layout.SkinBase + rootCount * kUnitsPerSkin;
        layout.PaletteBase = layout.VertexBase + vertexCount * kUnitsPerVertex;
        // This frame's palette, then last frame's.
        layout.TotalUnits = layout.PaletteBase + boneCount * kUnitsPerBone * 2u;
        return layout;
    }

    u32 GroomDeformDisplacementCapacity(const GroomAsset& groom, const GroomGuideInfluenceTable* influence) noexcept
    {
        if (influence == nullptr)
        {
            return 0u;
        }
        u64 points = 0;
        for (const u32 curve : influence->GetGuideCurves())
        {
            points += groom.GetCurvePointCount(curve);
        }
        return static_cast<u32>(std::min<u64>(points, 0xFFFFFFFFull));
    }

    void GroomDeformBuffer::Reset(const GroomDeformBufferLayout& layout, std::span<const u32> rootCurves,
                                  const GroomGuideInfluenceTable* influence)
    {
        m_Layout = layout;
        m_Frame = {};
        m_Weights.Init(GroomGuideWeights{}, static_cast<i32>(layout.RootCount));
        m_BindFrames.Init(GroomDeformBindRecord{}, static_cast<i32>(layout.RootCount));
        m_BindFramesWritten = false;
        m_Roots.Init(GroomDeformRootRecord{}, static_cast<i32>(layout.RootCount));
        m_Slots.Init(GroomDeformSlotRecord{}, static_cast<i32>(layout.SlotCount));
        m_Displacements.Init(GroomDeformDisplacementRecord{}, static_cast<i32>(layout.DisplacementCapacity));
        m_Bytes.Init(u8{ 0 }, static_cast<i64>(layout.TotalBytes()));

        // The STATIC region: each drawn strand's guide weights, in root-slot
        // order. A strand with no table, or a table that does not span the
        // groom, keeps the all-NoGuide default — which samples to exactly zero
        // displacement, the answer an unguided strand gets on the CPU path.
        const u32 count = std::min(layout.RootCount, static_cast<u32>(rootCurves.size()));
        if (influence != nullptr)
        {
            const auto& table = influence->GetWeights();
            for (u32 slot = 0; slot < count; ++slot)
            {
                if (rootCurves[slot] < table.size())
                {
                    m_Weights[slot] = table[rootCurves[slot]];
                }
            }
        }
        WriteRegion(m_Bytes, 0u, AsSpan(m_Weights, layout.RootCount));
    }

    GroomDeformFrameStats GroomDeformBuffer::PackFrame(std::span<const u32> rootCurves,
                                                       const GroomBindingAsset& binding,
                                                       std::span<const GroomRootTransform> rootTransforms,
                                                       const GroomStrandSimulation* simulation, u32 baseCurveCount)
    {
        m_Frame = {};
        if (m_Bytes.Num() == 0)
        {
            return m_Frame;
        }

        // ── Roots ────────────────────────────────────────────────────────────
        const u32 rootCount = std::min(m_Layout.RootCount, static_cast<u32>(rootCurves.size()));
        const bool bindingSpans = binding.GetRootCount() == baseCurveCount;

        // THE BIND FRAMES (#1533), once per Reset: static, so they go up with
        // the weights in the relayout upload and never again. A root the
        // binding does not reach keeps the identity, which is also what its
        // held-at-rest record below carries it with.
        if (!m_BindFramesWritten)
        {
            for (u32 slot = 0; slot < rootCount; ++slot)
            {
                const u32 curve = rootCurves[slot];
                if (bindingSpans && curve < baseCurveCount)
                {
                    const GroomRootBinding& rest = binding.GetRoot(curve);
                    m_BindFrames[slot].Origin = glm::vec4(rest.RestOrigin, 0.0f);
                    m_BindFrames[slot].Rotation = PackQuat(rest.RestRotation);
                }
            }
            WriteRegion(m_Bytes, m_Layout.BindBase, AsSpan(m_BindFrames, m_Layout.RootCount));
            m_BindFramesWritten = true;
        }
        // THE GPU WRITES THE ROOTS (#1533 E1) when the layout carries the surface
        // and the palette: the compute pass evaluates every drawn root from them
        // and the region is never packed or sent. What the CPU still owes is
        // above (the bind frames the held roots fall back to) and below (the
        // guides).
        if (!m_Layout.RootsOnGpu())
        {
            m_Frame.StrandsHeldAtRest += PackRootRecords(rootCurves, binding, rootTransforms, baseCurveCount);
            WriteRegion(m_Bytes, m_Layout.RootBase, AsSpan(m_Roots, m_Layout.RootCount));
        }

        // ── Guides ───────────────────────────────────────────────────────────
        //
        // An unusable simulation is ABSENT, never partly applied — the rule
        // BuildGroomStrandMesh states for the same input. The slot region is
        // cleared either way, so a frame that stops simulating cannot leave the
        // previous frame's windows behind for the shader to read.
        for (GroomDeformSlotRecord& slot : m_Slots)
        {
            slot = GroomDeformSlotRecord{};
        }
        const auto finishSlots = [this]()
        { WriteRegion(m_Bytes, m_Layout.SlotBase, AsSpan(m_Slots, m_Layout.SlotCount)); };

        if (simulation == nullptr || !simulation->IsUsable(baseCurveCount))
        {
            finishSlots();
            return m_Frame;
        }

        const GroomGuideDisplacements& guides = simulation->Displacements;
        const u32 displacementCount = static_cast<u32>(guides.Displacements.size());
        if (displacementCount > m_Layout.DisplacementCapacity ||
            simulation->Influence->GetGuideCount() != m_Layout.SlotCount)
        {
            // The caller sized the buffer for this table; a mismatch means the
            // table changed under a live entry. The coat stays on its bound rest
            // shape this frame, reported as unsimulated, and the caller relays
            // the buffer out.
            finishSlots();
            return m_Frame;
        }

        for (u32 slot = 0; slot < m_Layout.SlotCount; ++slot)
        {
            const u32 guide = slot < simulation->GuideOfSlot.size() ? simulation->GuideOfSlot[slot] : GroomNoGuide;
            if (guide == GroomNoGuide || static_cast<sizet>(guide) + 1u >= guides.GuideOffsets.size())
            {
                continue; // not simulated this frame
            }
            const u32 first = guides.GuideOffsets[guide];
            const u32 last = guides.GuideOffsets[guide + 1u];
            if (last <= first || last > displacementCount)
            {
                continue;
            }
            m_Slots[slot] = GroomDeformSlotRecord{ first, last - first, 0u, 0u };
        }
        finishSlots();

        const bool hasPrevious = !guides.PrevDisplacements.empty();
        for (u32 i = 0; i < displacementCount; ++i)
        {
            GroomDeformDisplacementRecord& record = m_Displacements[i];
            record.Current = glm::vec4(guides.Displacements[i], 0.0f);
            // "No previous frame" is "the same as current", resolved here
            // rather than per read — the velocity it produces is then exactly
            // zero, as SampleGroomGuideDisplacement's is.
            record.Previous = hasPrevious ? glm::vec4(guides.PrevDisplacements[i], 0.0f) : record.Current;
        }
        WriteRegion(m_Bytes, m_Layout.DisplacementBase, AsSpan(m_Displacements, displacementCount));

        m_Frame.Simulated = true;
        m_Frame.DisplacementCount = displacementCount;

        // The strands a guide actually moved this frame, counted the way the
        // CPU build counted them: HasGroomGuideInfluence, per drawn strand.
        for (u32 slot = 0; slot < rootCount; ++slot)
        {
            if (HasGroomGuideInfluence(*simulation, rootCurves[slot]))
            {
                ++m_Frame.StrandsSimulated;
            }
            else
            {
                ++m_Frame.StrandsUnguided;
            }
        }
        return m_Frame;
    }

    std::span<const GroomGuideWeights> GroomDeformBuffer::Weights() const noexcept
    {
        return AsSpan(m_Weights, static_cast<sizet>(m_Weights.Num()));
    }

    std::span<const GroomDeformBindRecord> GroomDeformBuffer::BindFrames() const noexcept
    {
        return AsSpan(m_BindFrames, static_cast<sizet>(m_BindFrames.Num()));
    }

    u32 GroomDeformBuffer::PackRootRecords(std::span<const u32> rootCurves, const GroomBindingAsset& binding,
                                           std::span<const GroomRootTransform> rootTransforms, u32 baseCurveCount)
    {
        const u32 rootCount = std::min(m_Layout.RootCount, static_cast<u32>(rootCurves.size()));
        const bool bindingSpans = binding.GetRootCount() == baseCurveCount;
        // In parallel (#1533 E1): each slot writes only its own record; the
        // held-at-rest count is summed per worker.
        TArray<u32> heldByWorker;
        ParallelForWithTaskContext("GroomDeformPackRoots", heldByWorker, static_cast<i32>(rootCount),
                                   [&](u32& held, i32 index)
                                   {
                                       const u32 slot = static_cast<u32>(index);
                                       const u32 curve = rootCurves[slot];
                                       GroomDeformRootRecord record;
                                       const bool inRange = bindingSpans && curve < rootTransforms.size() && curve < baseCurveCount;
                                       if (inRange && rootTransforms[curve].Valid)
                                       {
                                           const GroomRootTransform& transform = rootTransforms[curve];
                                           record.Origin = glm::vec4(transform.Origin, 0.0f);
                                           record.Rotation = PackQuat(transform.Rotation);
                                           record.PrevOrigin = glm::vec4(transform.PrevOrigin, 0.0f);
                                           record.PrevRotation = PackQuat(transform.PrevRotation);
                                       }
                                       else
                                       {
                                           // HELD AT REST: the bind frame, both frames. The rest stream's
                                           // points are bind-local, so this carries them back to where
                                           // they rest and emits exactly zero motion — the strand
                                           // ApplyGroomRootTransform returns for an invalid transform.
                                           if (inRange)
                                           {
                                               const GroomRootBinding& rest = binding.GetRoot(curve);
                                               record.Origin = glm::vec4(rest.RestOrigin, 0.0f);
                                               record.Rotation = PackQuat(rest.RestRotation);
                                               record.PrevOrigin = record.Origin;
                                               record.PrevRotation = record.Rotation;
                                           }
                                           ++held;
                                       }
                                       m_Roots[slot] = record;
                                   });
        u32 heldTotal = 0;
        for (const u32 held : heldByWorker)
        {
            heldTotal += held;
        }
        return heldTotal;
    }

    u32 GroomDeformBuffer::PackCpuRoots(std::span<const u32> rootCurves, const GroomBindingAsset& binding,
                                        std::span<const GroomRootTransform> rootTransforms, u32 baseCurveCount)
    {
        return PackRootRecords(rootCurves, binding, rootTransforms, baseCurveCount);
    }

    u32 GroomDeformBuffer::WriteSurfaceSkin(std::span<const u32> rootCurves, const GroomBindingAsset& binding,
                                            const GroomSurfaceView& surface, const GroomSkinningView& skinning,
                                            u32 baseCurveCount)
    {
        if (!m_Layout.RootsOnGpu())
        {
            return 0u;
        }
        // Each root's triangle, as rows of the vertex region below. Reached only
        // where the CPU evaluation would reach it: a binding spanning the groom,
        // a curve inside it, a triangle inside the surface.
        const bool bindingSpans = binding.GetRootCount() == baseCurveCount;
        TArray<GroomRootSkinRecord> skins;
        skins.Init(GroomRootSkinRecord{}, static_cast<i32>(m_Layout.RootCount));
        const u32 roots = std::min(m_Layout.RootCount, static_cast<u32>(rootCurves.size()));
        u32 reached = 0;
        for (u32 slot = 0; slot < roots; ++slot)
        {
            const u32 curve = rootCurves[slot];
            if (!bindingSpans || curve >= baseCurveCount)
            {
                continue;
            }
            const GroomRootBinding& record = binding.GetRoot(curve);
            if (!surface.TriangleInRange(record.TriangleIndex))
            {
                continue;
            }
            const glm::uvec3 corners = surface.TriangleIndices(record.TriangleIndex);
            if (corners.x >= m_Layout.VertexCount || corners.y >= m_Layout.VertexCount ||
                corners.z >= m_Layout.VertexCount)
            {
                continue;
            }
            skins[slot].Corners = glm::uvec4(corners, 1u);
            skins[slot].Barycentric = glm::vec4(record.Barycentric, 0.0f);
            ++reached;
        }
        WriteRegion(m_Bytes, m_Layout.SkinBase, AsSpan(skins, m_Layout.RootCount));

        // The surface: rest positions and influences, read exactly as
        // SkinGroomSurfaceVertex reads them. An unskinned vertex keeps zero
        // weights, which the shader answers with the rest position -- the CPU's
        // answer for an unweighted vertex.
        TArray<GroomSurfaceVertexRecord> vertices;
        vertices.Init(GroomSurfaceVertexRecord{}, static_cast<i32>(m_Layout.VertexCount));
        const u32 vertexCount = std::min(m_Layout.VertexCount, surface.VertexCount);
        const bool influences = skinning.BoneIds != nullptr && skinning.Weights != nullptr &&
                                skinning.Stride >= 32u && skinning.VertexCount >= vertexCount;
        for (u32 v = 0; v < vertexCount; ++v)
        {
            GroomSurfaceVertexRecord& out = vertices[static_cast<i32>(v)];
            out.Position = glm::vec4(surface.Position(v), 1.0f);
            if (influences)
            {
                const auto offset = static_cast<sizet>(v) * skinning.Stride;
                u32 ids[4] = { 0, 0, 0, 0 };
                f32 weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                std::memcpy(ids, reinterpret_cast<const std::byte*>(skinning.BoneIds) + offset, sizeof(ids));
                std::memcpy(weights, reinterpret_cast<const std::byte*>(skinning.Weights) + offset, sizeof(weights));
                out.BoneIds = glm::uvec4(ids[0], ids[1], ids[2], ids[3]);
                out.Weights = glm::vec4(weights[0], weights[1], weights[2], weights[3]);
            }
        }
        WriteRegion(m_Bytes, m_Layout.VertexBase, AsSpan(vertices, m_Layout.VertexCount));
        return reached;
    }

    GroomRootBoneBounds BuildGroomRootBoneBounds(std::span<const u32> rootCurves, const GroomBindingAsset& binding,
                                                 const GroomSurfaceView& surface, const GroomSkinningView& skinning,
                                                 u32 boneCount, u32 baseCurveCount)
    {
        GroomRootBoneBounds bounds;
        constexpr f32 kBig = std::numeric_limits<f32>::max();
        bounds.Min.assign(boneCount, glm::vec3(kBig));
        bounds.Max.assign(boneCount, glm::vec3(-kBig));
        bounds.RestMin = bounds.HeldMin = glm::vec3(kBig);
        bounds.RestMax = bounds.HeldMax = glm::vec3(-kBig);

        const bool bindingSpans = binding.GetRootCount() == baseCurveCount;
        const bool influences = skinning.BoneIds != nullptr && skinning.Weights != nullptr && skinning.Stride >= 32u &&
                                skinning.VertexCount >= surface.VertexCount;
        std::vector<u8> seen(surface.VertexCount, 0u);
        const auto hold = [&bounds](const glm::vec3& origin)
        {
            bounds.HeldMin = glm::min(bounds.HeldMin, origin);
            bounds.HeldMax = glm::max(bounds.HeldMax, origin);
            bounds.HasHeld = true;
        };
        const auto addCorner = [&](u32 vertex)
        {
            if (seen[vertex] != 0u)
            {
                return;
            }
            seen[vertex] = 1u;
            const glm::vec3 rest = surface.Position(vertex);
            bool moved = false;
            if (influences)
            {
                // Read, and rejected, exactly as the kernel's oloGroomSkinVertex
                // rejects them: a bone the kernel skips cannot move the vertex.
                const auto offset = static_cast<sizet>(vertex) * skinning.Stride;
                u32 ids[4] = { 0, 0, 0, 0 };
                f32 weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                std::memcpy(ids, reinterpret_cast<const std::byte*>(skinning.BoneIds) + offset, sizeof(ids));
                std::memcpy(weights, reinterpret_cast<const std::byte*>(skinning.Weights) + offset, sizeof(weights));
                for (u32 k = 0; k < 4u; ++k)
                {
                    if (!std::isfinite(weights[k]) || weights[k] <= 0.0f || ids[k] >= boneCount)
                    {
                        continue;
                    }
                    bounds.Min[ids[k]] = glm::min(bounds.Min[ids[k]], rest);
                    bounds.Max[ids[k]] = glm::max(bounds.Max[ids[k]], rest);
                    moved = true;
                }
            }
            if (!moved)
            {
                bounds.RestMin = glm::min(bounds.RestMin, rest);
                bounds.RestMax = glm::max(bounds.RestMax, rest);
                bounds.HasRest = true;
            }
        };

        for (const u32 curve : rootCurves)
        {
            if (!bindingSpans || curve >= baseCurveCount)
            {
                // The kernel holds it on an identity bind frame (see PackFrame).
                hold(glm::vec3(0.0f));
                continue;
            }
            const GroomRootBinding& record = binding.GetRoot(curve);
            if (!surface.TriangleInRange(record.TriangleIndex))
            {
                hold(record.RestOrigin);
                continue;
            }
            const glm::uvec3 corners = surface.TriangleIndices(record.TriangleIndex);
            if (corners.x >= surface.VertexCount || corners.y >= surface.VertexCount ||
                corners.z >= surface.VertexCount)
            {
                hold(record.RestOrigin);
                continue;
            }
            addCorner(corners.x);
            addCorner(corners.y);
            addCorner(corners.z);
        }
        return bounds;
    }

    bool PoseGroomRootBoneBounds(const GroomRootBoneBounds& bounds, std::span<const glm::mat4> palette,
                                 const glm::mat4& surfaceToGroom, glm::vec3& outMin, glm::vec3& outMax) noexcept
    {
        glm::vec3 lo(std::numeric_limits<f32>::max());
        glm::vec3 hi(std::numeric_limits<f32>::lowest());
        bool any = false;
        const auto addBox = [&](const glm::mat4& toGroom, const glm::vec3& boxMin, const glm::vec3& boxMax)
        {
            for (u32 corner = 0; corner < 8u; ++corner)
            {
                const glm::vec3 p((corner & 1u) != 0u ? boxMax.x : boxMin.x, (corner & 2u) != 0u ? boxMax.y : boxMin.y,
                                  (corner & 4u) != 0u ? boxMax.z : boxMin.z);
                const glm::vec3 posed(toGroom * glm::vec4(p, 1.0f));
                lo = glm::min(lo, posed);
                hi = glm::max(hi, posed);
            }
            any = true;
        };
        for (sizet bone = 0; bone < bounds.Min.size(); ++bone)
        {
            if (bounds.Min[bone].x > bounds.Max[bone].x)
            {
                continue;
            }
            // A bone past this frame's palette is the identity in the kernel's
            // palette region (WritePalette), so the same here.
            addBox(bone < palette.size() ? surfaceToGroom * palette[bone] : surfaceToGroom, bounds.Min[bone],
                   bounds.Max[bone]);
        }
        if (bounds.HasRest)
        {
            addBox(surfaceToGroom, bounds.RestMin, bounds.RestMax);
        }
        if (bounds.HasHeld)
        {
            lo = glm::min(lo, bounds.HeldMin);
            hi = glm::max(hi, bounds.HeldMax);
            any = true;
        }
        if (!any || !Math::IsFinite(lo) || !Math::IsFinite(hi))
        {
            return false;
        }
        outMin = lo;
        outMax = hi;
        return true;
    }

    void GroomDeformBuffer::WritePalette(std::span<const glm::mat4> current, std::span<const glm::mat4> previous)
    {
        if (!m_Layout.RootsOnGpu())
        {
            return;
        }
        const u32 bones = m_Layout.BoneCount;
        TArray<glm::mat4> palette;
        palette.Init(glm::mat4(1.0f), static_cast<i32>(bones * 2u));
        const u32 have = std::min(bones, static_cast<u32>(current.size()));
        const bool history = previous.size() >= have;
        for (u32 b = 0; b < have; ++b)
        {
            palette[static_cast<i32>(b)] = current[b];
            palette[static_cast<i32>(bones + b)] = history ? previous[b] : current[b];
        }
        WriteRegion(m_Bytes, m_Layout.PaletteBase, AsSpan(palette, bones * 2u));
    }

    std::span<const GroomDeformRootRecord> GroomDeformBuffer::Roots() const noexcept
    {
        return AsSpan(m_Roots, static_cast<sizet>(m_Roots.Num()));
    }

    std::span<const GroomDeformSlotRecord> GroomDeformBuffer::Slots() const noexcept
    {
        return m_Frame.Simulated ? AsSpan(m_Slots, static_cast<sizet>(m_Slots.Num()))
                                 : std::span<const GroomDeformSlotRecord>{};
    }

    std::span<const GroomDeformDisplacementRecord> GroomDeformBuffer::Displacements() const noexcept
    {
        return AsSpan(m_Displacements, m_Frame.DisplacementCount);
    }

    glm::vec3 EvaluateGroomDeformedPoint(const GroomDeformBuffer& buffer, u32 rootSlot, const glm::vec3& local, f32 t,
                                         bool previous) noexcept
    {
        const auto roots = buffer.Roots();
        if (rootSlot >= roots.size())
        {
            return local;
        }
        const GroomDeformRootRecord& root = roots[rootSlot];
        // ApplyGroomRootTransform's second half, verbatim: the first half —
        // conjugate(RestRotation) * (rest - RestOrigin) — was taken once, when
        // the rest stream was built.
        glm::vec3 placed = previous ? glm::vec3(root.PrevOrigin) + UnpackQuat(root.PrevRotation) * local
                                    : glm::vec3(root.Origin) + UnpackQuat(root.Rotation) * local;
        if (buffer.GetFrameStats().Simulated)
        {
            placed += SampleDisplacement(buffer, rootSlot, t, previous);
        }
        return placed;
    }

    void EvaluateGroomDeformedPose(const GroomDeformBuffer& buffer, std::span<const GroomRestPoseSegment> pose,
                                   std::vector<GroomCoatShadow::CoatSegment>& outSegments)
    {
        outSegments.clear();
        outSegments.reserve(pose.size());
        for (const GroomRestPoseSegment& segment : pose)
        {
            GroomCoatShadow::CoatSegment drawn;
            drawn.A = EvaluateGroomDeformedPoint(buffer, segment.RootSlot, segment.Local0, segment.T0, false);
            drawn.B = EvaluateGroomDeformedPoint(buffer, segment.RootSlot, segment.Local1, segment.T1, false);
            drawn.RadiusA = segment.Radius0;
            drawn.RadiusB = segment.Radius1;
            outSegments.push_back(drawn);
        }
    }
} // namespace OloEngine
