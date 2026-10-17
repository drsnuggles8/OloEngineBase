#pragma once

// The traversal order both Alembic importers share (#1560). Only included by
// translation units compiled with OLO_WITH_ALEMBIC.

#if defined(OLO_WITH_ALEMBIC)

#include "OloEngine/Core/Base.h"

#include <Alembic/Abc/All.h>

#include <algorithm>
#include <numeric>
#include <vector>

namespace OloEngine::AlembicTraversal
{
    // `obj`'s child indices in NAME order, never archive order. An importer
    // that walks children in archive order emits its output in the order the
    // exporter happened to write objects, and Blender writes them in pointer
    // order, which changes on every export: a re-export of the same geometry
    // then imports to a different curve or submesh order. Alembic requires
    // sibling names to be unique, so this is a total order and needs no
    // tie-break; std::string's operator< compares bytes, so it is the same on
    // every platform.
    [[nodiscard]] inline std::vector<sizet> SortedChildIndices(const Alembic::Abc::IObject& obj)
    {
        std::vector<sizet> children(obj.getNumChildren());
        std::iota(children.begin(), children.end(), sizet{ 0 });
        std::sort(children.begin(), children.end(),
                  [&obj](sizet lhs, sizet rhs)
                  { return obj.getChildHeader(lhs).getName() < obj.getChildHeader(rhs).getName(); });
        return children;
    }
} // namespace OloEngine::AlembicTraversal

#endif // OLO_WITH_ALEMBIC
