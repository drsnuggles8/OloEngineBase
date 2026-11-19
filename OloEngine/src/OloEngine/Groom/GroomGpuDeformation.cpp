#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomGpuDeformation.h"
#include "OloEngine/Core/PerformanceProfiler.h"
#include "OloEngine/Task/ParallelFor.h"

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomWetness.h"
#include "OloEngine/Math/Math.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
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
        constexpr u32 kUnitsPerClump = 1u;
        constexpr u32 kUnitsPerWet = 4u;
        constexpr u32 kUnitsPerRoot = 4u;
        constexpr u32 kUnitsPerSlot = 1u;
        constexpr u32 kUnitsPerDisplacement = 2u;
        constexpr u32 kUnitsPerSkin = 2u;
        constexpr u32 kUnitsPerVertex = 3u;
        constexpr u32 kUnitsPerBone = 4u; // one mat4
        // The GPU guide expansion's regions (#1589): a slot's stand-ins are a
        // GroomGuideWeights, a simulated point a displacement record, and the
        // guides' prefix four u32s to a unit.
        constexpr u32 kUnitsPerStandIn = kUnitsPerWeights;
        constexpr u32 kGuideOffsetsPerUnit = 4u;
        // GroomExpandGuides.comp's local_size_x.
        constexpr u32 kExpandGroupSize = 64u;
        static_assert(sizeof(GroomRootSkinRecord) == kUnitsPerSkin * kUnitBytes);
        static_assert(sizeof(GroomSurfaceVertexRecord) == kUnitsPerVertex * kUnitBytes);
        static_assert(sizeof(glm::mat4) == kUnitsPerBone * kUnitBytes);

        static_assert(sizeof(GroomGuideWeights) == kUnitsPerWeights * kUnitBytes);
        static_assert(sizeof(GroomDeformBindRecord) == kUnitsPerBind * kUnitBytes);
        static_assert(sizeof(glm::vec4) == kUnitsPerClump * kUnitBytes);
        static_assert(sizeof(GroomDeformWetRecord) == kUnitsPerWet * kUnitBytes);
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

        // Copies `records` into the byte image at `baseUnit`; nothing for a
        // CPU-only buffer, which has no image.
        template<typename T>
        void WriteRegion(bool cpuOnly, TArray64<u8>& bytes, u32 baseUnit, std::span<const T> records) noexcept
        {
            if (cpuOnly || records.empty())
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

        // Twin of GroomStrandDeform.glsl's oloGroomDeformWetLocal (#1615): the
        // bind-local point as the coat's wetness at this root lays it down, from
        // the frame whose root is being evaluated.
        [[nodiscard]] glm::vec3 DeformWetLocal(const GroomDeformBuffer& buffer, u32 rootSlot, const glm::vec3& origin,
                                               const glm::quat& rotation, const glm::vec3& local, f32 t) noexcept
        {
            const auto clumps = buffer.ClumpGrowth();
            const glm::vec3 clump = rootSlot < clumps.size() ? glm::vec3(clumps[rootSlot]) : glm::vec3(0.0f);
            return GroomDeformWetLocal(buffer.Wet(), origin, rotation, clump, local, t);
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
        // ...and ClumpBase as RootCount * 4, WetBase as RootCount * 5 (#1615).
        layout.ClumpBase = layout.BindBase + rootCount * kUnitsPerBind;
        layout.WetBase = layout.ClumpBase + rootCount * kUnitsPerClump;
        layout.RootBase = layout.WetBase + kUnitsPerWet;
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

    GroomDeformBufferLayout GroomDeformBufferLayout::WithGuideExpansion(u32 sourceCapacity) const noexcept
    {
        if (sourceCapacity == 0u || SlotCount == 0u)
        {
            return *this;
        }
        // After everything else, so no region the vertex stage or the root
        // kernel reads moves, and a layout without them is byte for byte the
        // layout it was.
        GroomDeformBufferLayout layout = *this;
        layout.SourceCapacity = sourceCapacity;
        layout.StandInBase = TotalUnits;
        layout.GuideOffsetBase = layout.StandInBase + SlotCount * kUnitsPerStandIn;
        // A guide per slot at most, and the prefix has one entry more.
        const u32 offsetUnits = (SlotCount + 1u + kGuideOffsetsPerUnit - 1u) / kGuideOffsetsPerUnit;
        layout.SourceBase = layout.GuideOffsetBase + offsetUnits;
        layout.TotalUnits = layout.SourceBase + sourceCapacity * kUnitsPerDisplacement;
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
                                  const GroomGuideInfluenceTable* influence, bool cpuOnly)
    {
        m_Layout = layout;
        m_CpuOnly = cpuOnly;
        m_HasLayout = layout.TotalBytes() > 0u;
        m_Frame = {};
        m_Weights.Init(GroomGuideWeights{}, static_cast<i32>(layout.RootCount));
        m_BindFrames.Init(GroomDeformBindRecord{}, static_cast<i32>(layout.RootCount));
        m_BindFramesWritten = false;
        m_ClumpObject.clear();
        m_Clump.Init(glm::vec4(0.0f), static_cast<i32>(layout.RootCount));
        m_Wet = GroomDeformWetRecord{};
        m_Roots.Init(GroomDeformRootRecord{}, static_cast<i32>(layout.RootCount));
        m_Slots.Init(GroomDeformSlotRecord{}, static_cast<i32>(layout.SlotCount));
        m_Displacements.Init(GroomDeformDisplacementRecord{}, static_cast<i32>(layout.DisplacementCapacity));
        m_Bytes.Init(u8{ 0 }, cpuOnly ? 0 : static_cast<i64>(layout.TotalBytes()));
        m_CountsValid = false;
        m_Source.Reset();
        m_GuideOffsets.Reset();
        m_PackedStandInSerial = 0;
        m_ExpandedValid = false;
        m_Expanded = {};
        m_CpuGuidesResolved = true;

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
        WriteRegion(m_CpuOnly, m_Bytes, 0u, AsSpan(m_Weights, layout.RootCount));
        if (m_HasLayout)
        {
            WriteRegion(m_CpuOnly, m_Bytes, layout.WetBase, std::span<const GroomDeformWetRecord>(&m_Wet, 1u));
        }
    }

    void GroomDeformBuffer::SetClumpGrowth(std::span<const glm::vec3> curveGrowth, std::span<const u32> rootCurves)
    {
        m_ClumpObject.assign(rootCurves.size(), glm::vec3(0.0f));
        for (sizet slot = 0; slot < rootCurves.size(); ++slot)
        {
            if (rootCurves[slot] < curveGrowth.size())
            {
                m_ClumpObject[slot] = curveGrowth[rootCurves[slot]];
            }
        }
        if (!m_BindFramesWritten || !m_HasLayout)
        {
            return; // PackFrame takes them into the bind frames as it writes those
        }
        // Handed over after the bind frames went up (a coat that got wet on a
        // live entry): taken into them here, and the caller sends the region.
        const u32 count = std::min(m_Layout.RootCount, static_cast<u32>(m_ClumpObject.size()));
        for (u32 slot = 0; slot < count; ++slot)
        {
            m_Clump[slot] = glm::vec4(glm::conjugate(UnpackQuat(m_BindFrames[slot].Rotation)) * m_ClumpObject[slot], 0.0f);
        }
        WriteRegion(m_CpuOnly, m_Bytes, m_Layout.ClumpBase, AsSpan(m_Clump, m_Layout.RootCount));
    }

    void GroomDeformBuffer::SetWetness(const GroomDeformWetRecord& wet)
    {
        m_Wet = wet;
        if (m_HasLayout)
        {
            WriteRegion(m_CpuOnly, m_Bytes, m_Layout.WetBase, std::span<const GroomDeformWetRecord>(&m_Wet, 1u));
        }
    }

    std::span<const glm::vec4> GroomDeformBuffer::ClumpGrowth() const noexcept
    {
        return AsSpan(m_Clump, static_cast<sizet>(m_Clump.Num()));
    }

    GroomDeformFrameStats GroomDeformBuffer::PackFrame(std::span<const u32> rootCurves,
                                                       const GroomBindingAsset& binding,
                                                       std::span<const GroomRootTransform> rootTransforms,
                                                       const GroomStrandSimulation* simulation, u32 baseCurveCount,
                                                       bool gpuExpandsGuides)
    {
        OLO_PROFILE_FUNCTION();
        OLO_PERF_SCOPE_AUTO("Groom::PackFrame");
        m_Frame = {};
        if (!m_HasLayout)
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
            // The clump growth (#1615), into the frame each strand's points are
            // in -- conjugate(bind rotation), as the rest stream took its points;
            // the identity frame of a root the binding does not reach included.
            const u32 clumps = std::min(rootCount, static_cast<u32>(m_ClumpObject.size()));
            for (u32 slot = 0; slot < clumps; ++slot)
            {
                m_Clump[slot] = glm::vec4(glm::conjugate(UnpackQuat(m_BindFrames[slot].Rotation)) * m_ClumpObject[slot], 0.0f);
            }
            WriteRegion(m_CpuOnly, m_Bytes, m_Layout.BindBase, AsSpan(m_BindFrames, m_Layout.RootCount));
            WriteRegion(m_CpuOnly, m_Bytes, m_Layout.ClumpBase, AsSpan(m_Clump, m_Layout.RootCount));
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
            WriteRegion(m_CpuOnly, m_Bytes, m_Layout.RootBase, AsSpan(m_Roots, m_Layout.RootCount));
        }

        // ── Guides ───────────────────────────────────────────────────────────
        //
        // An unusable simulation is ABSENT, never partly applied — the rule
        // BuildGroomStrandMesh states for the same input. The slot region is
        // cleared either way, so a frame that stops simulating cannot leave the
        // previous frame's windows behind for the shader to read.
        m_ExpandedValid = false;
        m_CpuGuidesResolved = true;
        for (GroomDeformSlotRecord& slot : m_Slots)
        {
            slot = GroomDeformSlotRecord{};
        }
        // Every frame that writes the slot region from here forgets the set
        // whose dense windows it held, so a later expanded frame writes them
        // again.
        const auto finishSlots = [this]()
        {
            WriteRegion(m_CpuOnly, m_Bytes, m_Layout.SlotBase, AsSpan(m_Slots, m_Layout.SlotCount));
            m_PackedStandInSerial = 0;
        };

        if (simulation == nullptr || !simulation->IsUsable(baseCurveCount))
        {
            finishSlots();
            return m_Frame;
        }

        const GroomGuideDisplacements& sparse = simulation->Displacements;
        if (simulation->Influence->GetGuideCount() != m_Layout.SlotCount)
        {
            // The caller sized the buffer for this table; a mismatch means the
            // table changed under a live entry. The coat stays on its bound rest
            // shape this frame, reported as unsimulated, and the caller relays
            // the buffer out.
            finishSlots();
            return m_Frame;
        }

        // THE STAND-INS EXPANDED ON THE GPU (#1589): only the simulated points
        // are packed, for GroomExpandGuides.comp to expand into the dense
        // region the vertex stage reads. Needs a layout with the kernel's
        // regions, room for this frame's points, and a byte image to send.
        const bool expands = simulation->ExpandsStandIns();
        if (expands && gpuExpandsGuides && !m_CpuOnly && m_Layout.ExpandsGuides() &&
            sparse.Displacements.size() <= m_Layout.SourceCapacity &&
            sparse.GuideCount() <= m_Layout.SlotCount &&
            simulation->StandIns->GetPointCount() <= m_Layout.DisplacementCapacity)
        {
            PackGuideSource(*simulation);
            m_Frame.Simulated = true;
            m_Frame.DisplacementCount = simulation->StandIns->GetPointCount();
            m_CpuGuidesResolved = false;
            if (m_IdentitySlots.Num() != static_cast<i32>(m_Layout.SlotCount))
            {
                m_IdentitySlots.SetNumUninitialized(static_cast<i32>(m_Layout.SlotCount));
                for (i32 slot = 0; slot < m_IdentitySlots.Num(); ++slot)
                {
                    m_IdentitySlots[slot] = static_cast<u32>(slot);
                }
            }
            // Every slot has a displacement: counted as the expansion's own map.
            CountStrands(rootCurves, *simulation,
                         std::span<const u32>(m_IdentitySlots.GetData(), static_cast<sizet>(m_IdentitySlots.Num())));
            return m_Frame;
        }

        // DENSE: the simulation as it is, or -- for stand-ins the GPU does not
        // expand here (a CPU-only buffer, a layout without the kernel's
        // regions) -- its expansion, made on the CPU as before #1589.
        if (expands)
        {
            m_Expanded = m_Expansion.Expand(*simulation);
            m_ExpandedValid = true;
        }
        const GroomStrandSimulation& dense = expands ? m_Expanded : *simulation;
        const GroomGuideDisplacements& guides = dense.Displacements;
        const u32 displacementCount = static_cast<u32>(guides.Displacements.size());
        if (displacementCount > m_Layout.DisplacementCapacity)
        {
            m_ExpandedValid = false;
            finishSlots();
            return m_Frame;
        }

        for (u32 slot = 0; slot < m_Layout.SlotCount; ++slot)
        {
            const u32 guide = slot < dense.GuideOfSlot.size() ? dense.GuideOfSlot[slot] : GroomNoGuide;
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

        (void)PackDisplacementRecords(guides, false);
        m_Frame.Simulated = true;
        m_Frame.DisplacementCount = displacementCount;
        CountStrands(rootCurves, dense, dense.GuideOfSlot);
        return m_Frame;
    }

    u32 GroomDeformBuffer::PackDisplacementRecords(const GroomGuideDisplacements& guides, bool cpuMirrorOnly)
    {
        const u32 displacementCount =
            std::min(static_cast<u32>(guides.Displacements.size()), static_cast<u32>(m_Displacements.Num()));
        if (displacementCount == 0u)
        {
            return 0u;
        }
        const bool hasPrevious = !guides.PrevDisplacements.empty();
        // In parallel (#1533): every guide point of the coat, every frame, for
        // the raster pass and again for the ray-traced proxy.
        GroomDeformDisplacementRecord* const records = &m_Displacements[0];
        ParallelFor("GroomPackDisplacements", static_cast<i32>(displacementCount), 8192,
                    [records, &guides, hasPrevious](i32 index)
                    {
                        const auto i = static_cast<sizet>(index);
                        GroomDeformDisplacementRecord& record = records[i];
                        record.Current = glm::vec4(guides.Displacements[i], 0.0f);
                        // "No previous frame" is "the same as current", resolved here
                        // rather than per read — the velocity it produces is then exactly
                        // zero, as SampleGroomGuideDisplacement's is.
                        record.Previous = hasPrevious ? glm::vec4(guides.PrevDisplacements[i], 0.0f) : record.Current;
                    });
        if (!cpuMirrorOnly)
        {
            WriteRegion(m_CpuOnly, m_Bytes, m_Layout.DisplacementBase, AsSpan(m_Displacements, displacementCount));
        }
        return displacementCount;
    }

    void GroomDeformBuffer::PackGuideSource(const GroomStrandSimulation& simulation)
    {
        OLO_PERF_SCOPE_AUTO("Groom::PackGuideSource");
        const GroomGuideStandInSet& set = *simulation.StandIns;
        const GroomGuideDisplacements& guides = simulation.Displacements;
        const u32 slots = m_Layout.SlotCount;

        // Each slot's window in the dense region: the running sum of the slots'
        // point counts, which is ExpandGroomGuideDisplacements' layout. Into
        // the typed records every frame (the CPU twin reads them); into the
        // image, with the stand-ins, only for a set the image does not hold.
        const std::span<const u32> windows = set.GetSlotOffsets();
        const std::span<const u32> counts = set.GetSlotPointCounts();
        for (u32 slot = 0; slot < slots; ++slot)
        {
            m_Slots[slot] = GroomDeformSlotRecord{ windows[slot], counts[slot], 0u, 0u };
        }
        if (set.GetSerial() != m_PackedStandInSerial)
        {
            WriteRegion(m_CpuOnly, m_Bytes, m_Layout.SlotBase, AsSpan(m_Slots, slots));
            WriteRegion(m_CpuOnly, m_Bytes, m_Layout.StandInBase, set.GetStandIns());
            m_PackedStandInSerial = set.GetSerial();
            m_Frame.StandInsWritten = true;
        }

        // The simulated guides' prefix, padded to whole units with zeros the
        // kernel never reads (it reads entries 0..guides).
        const u32 guideCount = guides.GuideCount();
        const u32 offsetUnits = (guideCount + 1u + kGuideOffsetsPerUnit - 1u) / kGuideOffsetsPerUnit;
        m_GuideOffsets.SetNumZeroed(static_cast<i32>(offsetUnits * kGuideOffsetsPerUnit), EAllowShrinking::No);
        std::memcpy(m_GuideOffsets.GetData(), guides.GuideOffsets.data(), guides.GuideOffsets.size_bytes());
        WriteRegion(m_CpuOnly, m_Bytes, m_Layout.GuideOffsetBase,
                    std::span<const u32>(m_GuideOffsets.GetData(), static_cast<sizet>(m_GuideOffsets.Num())));

        // The simulated points, this frame and last, as the dense records are
        // laid out: "no previous frame" is resolved by the kernel, which writes
        // this frame's expansion in both lanes.
        const u32 sourceCount = static_cast<u32>(guides.Displacements.size());
        const bool hasPrevious = !guides.PrevDisplacements.empty();
        m_Source.SetNumUninitialized(static_cast<i32>(sourceCount), EAllowShrinking::No);
        GroomDeformDisplacementRecord* const records = m_Source.GetData();
        ParallelFor("GroomPackGuideSource", static_cast<i32>(sourceCount), 8192,
                    [records, &guides, hasPrevious](i32 index)
                    {
                        const auto i = static_cast<sizet>(index);
                        records[i].Current = glm::vec4(guides.Displacements[i], 0.0f);
                        records[i].Previous = hasPrevious ? glm::vec4(guides.PrevDisplacements[i], 0.0f) : records[i].Current;
                    });
        WriteRegion(m_CpuOnly, m_Bytes, m_Layout.SourceBase, AsSpan(m_Source, sourceCount));

        m_Frame.GuidesExpandedOnGpu = true;
        m_Frame.SourceCount = sourceCount;
        m_Frame.SourceGuides = guideCount;
        m_Frame.SourceHasPrevious = hasPrevious;
    }

    void GroomDeformBuffer::CountStrands(std::span<const u32> rootCurves, const GroomStrandSimulation& simulation,
                                         std::span<const u32> guideOfSlot)
    {
        // The strands a guide actually moved this frame, counted the way the
        // CPU build counted them: HasGroomGuideInfluence, per drawn strand --
        // and only when the guides it moved are not the ones last counted.
        const u32 rootCount = std::min(m_Layout.RootCount, static_cast<u32>(rootCurves.size()));
        const bool countsCurrent = m_CountsValid && m_CountedTable == simulation.Influence &&
                                   std::ranges::equal(std::span<const u32>(m_CountedGuideOfSlot), guideOfSlot);
        if (!countsCurrent)
        {
            OLO_PERF_SCOPE_AUTO("Groom::CountStrands");
            GroomStrandSimulation counted = simulation;
            counted.GuideOfSlot = guideOfSlot;
            m_CountedSimulated = 0;
            m_CountedUnguided = 0;
            for (u32 slot = 0; slot < rootCount; ++slot)
            {
                if (HasGroomGuideInfluence(counted, rootCurves[slot]))
                {
                    ++m_CountedSimulated;
                }
                else
                {
                    ++m_CountedUnguided;
                }
            }
            m_CountedGuideOfSlot.assign(guideOfSlot.begin(), guideOfSlot.end());
            m_CountedTable = simulation.Influence;
            m_CountsValid = true;
        }
        m_Frame.StrandsSimulated = m_CountedSimulated;
        m_Frame.StrandsUnguided = m_CountedUnguided;
    }

    GroomGuideExpansionDispatch GroomDeformBuffer::GuideExpansionDispatch() const noexcept
    {
        GroomGuideExpansionDispatch dispatch;
        if (!m_Frame.GuidesExpandedOnGpu)
        {
            return dispatch;
        }
        dispatch.Counts = glm::ivec4(static_cast<i32>(m_Layout.SlotCount), static_cast<i32>(m_Frame.SourceGuides),
                                     m_Frame.SourceHasPrevious ? 1 : 0, static_cast<i32>(m_Frame.SourceCount));
        dispatch.Bases = glm::ivec4(static_cast<i32>(m_Layout.StandInBase), static_cast<i32>(m_Layout.SlotBase),
                                    static_cast<i32>(m_Layout.GuideOffsetBase), static_cast<i32>(m_Layout.SourceBase));
        dispatch.Target = glm::ivec4(static_cast<i32>(m_Layout.DisplacementBase), 0, 0, 0);
        dispatch.Groups = (m_Layout.SlotCount + kExpandGroupSize - 1u) / kExpandGroupSize;
        return dispatch;
    }

    GroomStrandSimulation GroomDeformBuffer::ResolveCpuGuides(const GroomStrandSimulation& simulation)
    {
        if (m_ExpandedValid)
        {
            return m_Expanded; // PackFrame expanded on the CPU, or a reader already asked
        }
        if (!m_Frame.GuidesExpandedOnGpu)
        {
            // Nothing expanded: the frame was packed from `simulation` as it is
            // -- or not simulated at all, which the GPU draws at rest too.
            return m_Frame.Simulated ? simulation : GroomStrandSimulation{};
        }
        OLO_PERF_SCOPE_AUTO("Groom::ResolveCpuGuides");
        m_Expanded = m_Expansion.Expand(simulation);
        m_ExpandedValid = true;
        // The CPU twin's records, from the expansion the kernel was handed the
        // inputs of; the image is not written -- that region is the kernel's.
        (void)PackDisplacementRecords(m_Expanded.Displacements, true);
        m_CpuGuidesResolved = true;
        return m_Expanded;
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
        WriteRegion(m_CpuOnly, m_Bytes, m_Layout.SkinBase, AsSpan(skins, m_Layout.RootCount));
        WriteSurfaceVertices(surface, skinning);
        return reached;
    }

    void GroomDeformBuffer::WriteSurfaceVertices(const GroomSurfaceView& surface, const GroomSkinningView& skinning)
    {
        if (!m_Layout.RootsOnGpu())
        {
            return;
        }
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
        WriteRegion(m_CpuOnly, m_Bytes, m_Layout.VertexBase, AsSpan(vertices, m_Layout.VertexCount));
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
        WriteRegion(m_CpuOnly, m_Bytes, m_Layout.PaletteBase, AsSpan(palette, bones * 2u));
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
        // While the GPU expands the frame's guides the CPU twin holds no
        // expansion until a reader asks for one (ResolveCpuGuides). A reader
        // that did not ask gets no displacement -- its coat at rest -- and that
        // is said: an assert in a debug build, an error once in any build.
        OLO_CORE_ASSERT(m_CpuGuidesResolved, "GroomDeformBuffer: the GPU expands this frame's guides; call "
                                             "ResolveCpuGuides before a CPU reader samples them");
        if (!m_CpuGuidesResolved)
        {
            static std::atomic<bool> s_Reported{ false };
            if (!s_Reported.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_ERROR_TAG("Groom", "GroomDeformBuffer: a CPU reader sampled guides the GPU expands without "
                                            "ResolveCpuGuides; it reads the coat at rest (further reads not logged)");
            }
            return {};
        }
        return AsSpan(m_Displacements, m_Frame.DisplacementCount);
    }

    namespace
    {
        // ONE POINT OF THE DEFORMED COAT, with what is constant across a loop
        // over the coat -- the root records, whether the frame is simulated,
        // and whether the coat is wet -- hoisted out of it. The wet hook (#1615)
        // is compiled into the Wet instantiation only: the coat bake evaluates a
        // million points a frame, and a per-point wet test plus a hook that no
        // longer inlined cost a dry horse's GroomPass four milliseconds of CPU.
        template<bool Wet>
        [[nodiscard]] glm::vec3 DeformedPoint(const GroomDeformBuffer& buffer, std::span<const GroomDeformRootRecord> roots,
                                              bool simulated, u32 rootSlot, const glm::vec3& local, f32 t,
                                              bool previous) noexcept
        {
            if (rootSlot >= roots.size())
            {
                return local;
            }
            const GroomDeformRootRecord& root = roots[rootSlot];
            // ApplyGroomRootTransform's second half, verbatim: the first half --
            // conjugate(RestRotation) * (rest - RestOrigin) -- was taken once,
            // when the rest stream was built. A wet coat's point is first laid
            // down, as the shader lays it down.
            const glm::vec3 origin(previous ? root.PrevOrigin : root.Origin);
            const glm::quat rotation = UnpackQuat(previous ? root.PrevRotation : root.Rotation);
            glm::vec3 placed;
            if constexpr (Wet)
            {
                placed = origin + rotation * DeformWetLocal(buffer, rootSlot, origin, rotation, local, t);
            }
            else
            {
                placed = origin + rotation * local;
            }
            if (simulated)
            {
                placed += SampleDisplacement(buffer, rootSlot, t, previous);
            }
            return placed;
        }

        [[nodiscard]] bool IsWet(const GroomDeformBuffer& buffer) noexcept
        {
            return buffer.Wet().Frame.w > 0.5f;
        }
    } // namespace

    glm::vec3 EvaluateGroomDeformedPoint(const GroomDeformBuffer& buffer, u32 rootSlot, const glm::vec3& local, f32 t,
                                         bool previous) noexcept
    {
        const bool simulated = buffer.GetFrameStats().Simulated;
        return IsWet(buffer) ? DeformedPoint<true>(buffer, buffer.Roots(), simulated, rootSlot, local, t, previous)
                             : DeformedPoint<false>(buffer, buffer.Roots(), simulated, rootSlot, local, t, previous);
    }

    void DeformGroomRestStream(const GroomDeformBuffer& buffer, std::span<const GroomStrandVertex> rest,
                               std::vector<GroomStrandVertex>& outVertices)
    {
        outVertices.resize(rest.size());
        GroomStrandVertex* const out = outVertices.data();
        const auto deform = [&rest, out, &buffer]<bool Wet>(std::bool_constant<Wet>)
        {
            const std::span<const GroomDeformRootRecord> roots = buffer.Roots();
            const bool simulated = buffer.GetFrameStats().Simulated;
            const auto point = [&](u32 root, const glm::vec3& local, f32 t, bool previous)
            { return DeformedPoint<Wet>(buffer, roots, simulated, root, local, t, previous); };
            ParallelFor("GroomDeformRestStream", static_cast<i32>(rest.size()), 4096,
                        [&rest, out, &point](i32 index)
                        {
                            // The rest stream's lanes, read as GroomStrand.glsl's mode-1
                            // branch reads them: PrevPosition carries the root slot, the
                            // other end's parameter and which end this corner is at.
                            const GroomStrandVertex& corner = rest[static_cast<sizet>(index)];
                            const u32 root = static_cast<u32>(corner.PrevPosition.x + 0.5f);
                            const f32 tSelf = corner.Coords.x;
                            const f32 tOther = corner.PrevPosition.y;
                            const bool atP1 = corner.PrevPosition.z > 0.5f;
                            const glm::vec3 self = point(root, corner.Position, tSelf, false);
                            const glm::vec3 otherEnd = point(root, corner.Other, tOther, false);
                            GroomStrandVertex deformed = corner;
                            deformed.Position = self;
                            deformed.Other = self + (atP1 ? (self - otherEnd) : (otherEnd - self));
                            deformed.PrevPosition = point(root, corner.Position, tSelf, true);
                            out[index] = deformed;
                        });
        };
        if (IsWet(buffer))
        {
            deform(std::true_type{});
        }
        else
        {
            deform(std::false_type{});
        }
    }

    void EvaluateGroomDeformedPose(const GroomDeformBuffer& buffer, std::span<const GroomRestPoseSegment> pose,
                                   std::vector<GroomCoatShadow::CoatSegment>& outSegments)
    {
        // On the task system, as DeformGroomRestStream above (#1630): the
        // rigged head's coat spent ~38 ms a frame here on one thread. Every
        // segment is independent and written to its own slot, so the result
        // is the serial loop's.
        outSegments.resize(pose.size());
        GroomCoatShadow::CoatSegment* const out = outSegments.data();
        const auto evaluate = [&]<bool Wet>(std::bool_constant<Wet>)
        {
            const std::span<const GroomDeformRootRecord> roots = buffer.Roots();
            const bool simulated = buffer.GetFrameStats().Simulated;
            ParallelFor("GroomEvaluateDeformedPose", static_cast<i32>(pose.size()), 4096,
                        [&](i32 index)
                        {
                            const GroomRestPoseSegment& segment = pose[static_cast<sizet>(index)];
                            GroomCoatShadow::CoatSegment& drawn = out[index];
                            drawn.A = DeformedPoint<Wet>(buffer, roots, simulated, segment.RootSlot, segment.Local0,
                                                         segment.T0, false);
                            drawn.B = DeformedPoint<Wet>(buffer, roots, simulated, segment.RootSlot, segment.Local1,
                                                         segment.T1, false);
                            drawn.RadiusA = segment.Radius0;
                            drawn.RadiusB = segment.Radius1;
                        });
        };
        if (IsWet(buffer))
        {
            evaluate(std::true_type{});
        }
        else
        {
            evaluate(std::false_type{});
        }
    }
} // namespace OloEngine
