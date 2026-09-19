#pragma once

// =============================================================================
// GroomBuilder.h — the one way to construct a GroomAsset. Issue #1232.
//
// GroomAsset's arrays are private and its invariants are cross-array (the
// offset table must agree with the point count, which must agree with the width
// count, ...). A builder exists so those invariants are checked ONCE, at the
// single point where a groom comes into being, instead of at every consumer —
// and so an importer cannot produce a half-built groom that validates later.
//
// Every AddCurve() is checked against GroomLimits on the spot and REJECTED by
// name, never clamped: acceptance criterion 3 is that malformed curve data is
// explicit, and a clamp is the silent-fallback this repo forbids.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomAsset.h"

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // One curve as handed to the builder. Points run ROOT -> TIP; `Widths` is
    // parallel to `Points` and holds DIAMETERS.
    struct GroomCurveInput
    {
        std::span<const glm::vec3> Points;
        std::span<const f32> Widths;
        glm::vec2 RootUV{ 0.0f, 0.0f };
        u16 GroupId = 0;
        bool IsGuide = false;
    };

    class GroomBuilder
    {
      public:
        GroomBuilder() = default;

        // Registers a group name and returns its id. Calling it twice with the
        // same name returns the same id — group ids are assigned by FIRST
        // APPEARANCE, which is what makes them a deterministic function of the
        // source file's traversal order rather than of hash-map iteration.
        // Returns false (and leaves outGroupId untouched) when the name is
        // empty, too long, or the group budget is exhausted.
        //
        // The new group's coat description (#1251) starts at identity with its
        // ROLE INFERRED FROM THE NAME — see InferGroomCoatRole. The inference
        // happens exactly here, at the one point a group comes into being, and
        // the answer is then data: nothing downstream ever re-reads a group name
        // to decide how to shade it, so renaming a group in a DCC cannot change
        // how an already-cooked coat looks.
        [[nodiscard]] bool AddGroup(const std::string& name, u16& outGroupId, std::string& outReason);

        // Replaces `groupId`'s authored coat description. Every float is
        // sanitised on the way in (GroomCoat.h) and the repairs are appended to
        // `outReasons`, so an importer reading garbage out of an arbGeomParam
        // gets a coat that is still authored rather than a groom that refuses to
        // build — a malformed CURVE is a rejection, a malformed coat PARAMETER
        // is a repair with a name, because the second cannot corrupt topology.
        //
        // Returns false only when `groupId` names no group.
        [[nodiscard]] bool SetGroupCoat(u16 groupId, const GroomCoatGroupDesc& coat,
                                        std::vector<std::string>& outReasons);

        // Appends one curve. Returns false with a named reason on any violation
        // of the input convention; the builder is left unchanged so the caller
        // can report and abort without a partially-appended strand.
        [[nodiscard]] bool AddCurve(const GroomCurveInput& curve, std::string& outReason);

        void SetBasis(GroomCurveBasis basis) noexcept
        {
            m_Basis = basis;
        }
        void SetProvenance(GroomProvenance provenance)
        {
            m_Provenance = std::move(provenance);
        }
        void SetName(std::string name)
        {
            m_Name = std::move(name);
        }

        [[nodiscard]] u32 GetCurveCount() const noexcept
        {
            return static_cast<u32>(m_RootUVs.size());
        }
        [[nodiscard]] u32 GetGroupCount() const noexcept
        {
            return static_cast<u32>(m_GroupNames.size());
        }

        // Moves the accumulated data into a GroomAsset, recomputes the derived
        // data and validates. Returns nullptr with a named reason on failure.
        // The builder is left empty either way — a groom is built once.
        [[nodiscard]] Ref<GroomAsset> Build(std::string& outReason);

      private:
        std::string m_Name;
        std::vector<u32> m_CurveOffsets{ 0u };
        std::vector<glm::vec3> m_Points;
        std::vector<f32> m_PointWidths;
        std::vector<glm::vec2> m_RootUVs;
        std::vector<u16> m_CurveGroupIds;
        std::vector<u8> m_CurveFlags;

        std::vector<std::string> m_GroupNames;
        std::vector<GroomCoatGroupDesc> m_GroupCoats; // parallel to m_GroupNames
        // Name -> id. Iteration order of this map is never used; ids come from
        // m_GroupNames' order, which is insertion order.
        std::unordered_map<std::string, u16> m_GroupIdsByName;

        GroomCurveBasis m_Basis = GroomCurveBasis::Linear;
        GroomProvenance m_Provenance;
    };
} // namespace OloEngine
