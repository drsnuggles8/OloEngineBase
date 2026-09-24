#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomGpuDeformation.h"

#include "OloEngine/Groom/GroomAsset.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace OloEngine
{
    namespace
    {
        // Units (16 bytes) per record of each region. Named so the layout, the
        // byte image and the GLSL twin's strides are one set of numbers.
        constexpr u32 kUnitBytes = 16u;
        constexpr u32 kUnitsPerWeights = 2u;
        constexpr u32 kUnitsPerRoot = 4u;
        constexpr u32 kUnitsPerSlot = 1u;
        constexpr u32 kUnitsPerDisplacement = 2u;

        static_assert(sizeof(GroomGuideWeights) == kUnitsPerWeights * kUnitBytes);
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
        layout.RootBase = rootCount * kUnitsPerWeights;
        layout.SlotBase = layout.RootBase + rootCount * kUnitsPerRoot;
        layout.DisplacementBase = layout.SlotBase + slotCount * kUnitsPerSlot;
        // Never zero units: a zero-sized storage buffer is not a legal
        // allocation on either backend, and a coat with no strands is refused
        // before it gets here anyway.
        layout.TotalUnits = std::max(1u, layout.DisplacementBase + displacementCapacity * kUnitsPerDisplacement);
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
        for (u32 slot = 0; slot < rootCount; ++slot)
        {
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
                ++m_Frame.StrandsHeldAtRest;
            }
            m_Roots[slot] = record;
        }
        WriteRegion(m_Bytes, m_Layout.RootBase, AsSpan(m_Roots, m_Layout.RootCount));

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
