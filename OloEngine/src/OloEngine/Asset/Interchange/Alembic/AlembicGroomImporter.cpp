#include "OloEnginePCH.h"
#include "OloEngine/Asset/Interchange/Alembic/AlembicGroomImporter.h"

#if defined(OLO_WITH_ALEMBIC)

#include "OloEngine/Core/Log.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCooker.h"

#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OloEngine
{
    namespace
    {
        namespace Abc = Alembic::Abc;
        namespace AbcG = Alembic::AbcGeom;
        namespace AbcF = Alembic::AbcCoreFactory;

        // Object-hierarchy nesting is a property of the INPUT archive, and both
        // traversals below recurse once per level. A deeply nested archive would
        // exhaust the stack, and a stack overflow is not something the try/catch
        // in Import can turn into a rejection — the process just dies. So depth
        // is bounded and reported like every other malformed-input case. 256 is
        // far past any DCC's export nesting (a groom is typically xform/curves,
        // two levels).
        constexpr u32 kMaxTraversalDepth = 256;

        constexpr const char* kGuideParamName = "groom_guide";
        constexpr const char* kGroupParamName = "groom_group";
        // Issue #1251. A GroomCoatRole per curve, constant within a group — see
        // the header's convention note for why a mixed group is a rejection.
        constexpr const char* kRoleParamName = "groom_role";
        constexpr const char* kGroomParamPrefix = "groom_";

        // Accumulated across the whole archive. `Failed` short-circuits the
        // traversal: the first rejection is the one reported, and no later prim
        // can overwrite it with a less specific message.
        struct GroomTraversalState
        {
            GroomBuilder Builder;
            u32 PrimsRead = 0;
            u32 CurvesRead = 0;
            bool AnyBasisSeen = false;
            GroomCurveBasis Basis = GroomCurveBasis::Linear;
            bool WarnedMissingWidths = false;
            bool WarnedMissingUVs = false;
            bool Failed = false;
            std::string Diagnostic;
            std::vector<std::string> Warnings;

            void Warn(std::string warning)
            {
                Warnings.push_back(std::move(warning));
            }

            void Fail(std::string diagnostic)
            {
                if (!Failed)
                {
                    Failed = true;
                    Diagnostic = std::move(diagnostic);
                }
            }
        };

        // Alembic uses the row-vector convention (point * matrix), matching
        // AlembicMeshImporter. Keeping the math in Imath avoids the glm
        // column-vector mismatch entirely.
        glm::vec3 TransformPoint(const Imath::M44d& xf, const Imath::V3f& p)
        {
            Imath::V3d out;
            xf.multVecMatrix(Imath::V3d(p.x, p.y, p.z), out);
            return { static_cast<f32>(out.x), static_cast<f32>(out.y), static_cast<f32>(out.z) };
        }

        // The scalar a width is multiplied by under `xf`. The mean of the three
        // basis-vector lengths: exact for a uniform scale, and the least
        // surprising single number for a non-uniform one (which the caller logs
        // rather than silently accepting as exact).
        f32 WidthScaleOf(const Imath::M44d& xf, bool& outNonUniform)
        {
            const Imath::V3d x(xf[0][0], xf[0][1], xf[0][2]);
            const Imath::V3d y(xf[1][0], xf[1][1], xf[1][2]);
            const Imath::V3d z(xf[2][0], xf[2][1], xf[2][2]);
            const f64 lx = x.length();
            const f64 ly = y.length();
            const f64 lz = z.length();
            const f64 maxLen = std::max({ lx, ly, lz });
            const f64 minLen = std::min({ lx, ly, lz });
            outNonUniform = (maxLen - minLen) > (1e-6 * std::max(1.0, maxLen));
            return static_cast<f32>((lx + ly + lz) / 3.0);
        }

        [[nodiscard]] const char* ScopeName(AbcG::GeometryScope scope)
        {
            switch (scope)
            {
                case AbcG::kConstantScope:
                    return "constant";
                case AbcG::kUniformScope:
                    return "uniform";
                case AbcG::kVaryingScope:
                    return "varying";
                case AbcG::kVertexScope:
                    return "vertex";
                case AbcG::kFacevaryingScope:
                    return "facevarying";
                case AbcG::kUnknownScope:
                default:
                    return "unknown";
            }
        }

        // Rejects any arbGeomParam this build does not implement, so authored
        // groom intent can never be silently dropped (AC 3). Non-`groom_`
        // params belong to the DCC, not to us, and are only listed.
        void CheckArbGeomParams(GroomTraversalState& state, const Abc::ICompoundProperty& arbGeomParams,
                                const std::string& primPath)
        {
            if (!arbGeomParams.valid())
            {
                return;
            }
            for (sizet i = 0; i < arbGeomParams.getNumProperties(); ++i)
            {
                const Abc::PropertyHeader& header = arbGeomParams.getPropertyHeader(i);
                const std::string& name = header.getName();
                if (!name.starts_with(kGroomParamPrefix))
                {
                    OLO_CORE_TRACE("AlembicGroomImporter: '{}' carries arbGeomParam '{}', which is not groom data — ignored.",
                                   primPath, name);
                    continue;
                }
                if (name == kGuideParamName || name == kGroupParamName || name == kRoleParamName)
                {
                    continue;
                }
                state.Fail(std::format("'{}' carries the groom attribute '{}', which this build does not implement. "
                                       "Importing without it would silently drop authored groom intent; remove the "
                                       "attribute or use a build that supports it.",
                                       primPath, name));
                return;
            }
        }

        // Reads an optional uniform int32 arbGeomParam into `out` (one value
        // per curve). Returns false (and fails the traversal) when the param
        // exists but is not the uniform int32 this convention requires.
        [[nodiscard]] bool ReadUniformIntParam(GroomTraversalState& state, const Abc::ICompoundProperty& arbGeomParams,
                                               const char* name, sizet expectedCount, const std::string& primPath,
                                               std::vector<i32>& out)
        {
            out.clear();
            if (!arbGeomParams.valid() || !arbGeomParams.getPropertyHeader(name))
            {
                return true; // absent is legal
            }

            AbcG::IInt32GeomParam param(arbGeomParams, name);
            if (!param.valid())
            {
                state.Fail(std::format("'{}' attribute '{}' is not an int32 geometry parameter", primPath, name));
                return false;
            }
            if (param.getScope() != AbcG::kUniformScope)
            {
                state.Fail(std::format("'{}' attribute '{}' has {} scope; this convention requires uniform scope "
                                       "(one value per curve)",
                                       primPath, name, ScopeName(param.getScope())));
                return false;
            }

            AbcG::IInt32GeomParam::Sample sample;
            param.getExpanded(sample, Abc::ISampleSelector(Abc::index_t(0)));
            const Abc::Int32ArraySamplePtr values = sample.getVals();
            if (!values || values->size() != expectedCount)
            {
                state.Fail(std::format("'{}' attribute '{}' holds {} values but the prim has {} curves",
                                       primPath, name, values ? values->size() : 0, expectedCount));
                return false;
            }

            out.assign(values->get(), values->get() + values->size());
            return true;
        }

        void ReadCurves(GroomTraversalState& state, const Abc::IObject& obj, const Imath::M44d& worldXf)
        {
            const std::string primPath = obj.getFullName();

            AbcG::ICurves curves(obj, Abc::kWrapExisting);
            AbcG::ICurvesSchema& schema = curves.getSchema();
            if (schema.getNumSamples() == 0)
            {
                state.Fail(std::format("'{}' is an ICurves prim with no samples", primPath));
                return;
            }
            if (schema.getNumSamples() > 1)
            {
                OLO_CORE_INFO("AlembicGroomImporter: '{}' has {} samples; importing the REST POSE (sample 0) only "
                              "(animated grooms are a follow-up).",
                              primPath, schema.getNumSamples());
            }

            AbcG::ICurvesSchema::Sample sample;
            schema.get(sample, Abc::ISampleSelector(Abc::index_t(0)));

            // ── Basis and periodicity ──
            const AbcG::CurveType type = sample.getType();
            GroomCurveBasis basis = GroomCurveBasis::Linear;
            switch (type)
            {
                case AbcG::kLinear:
                    basis = GroomCurveBasis::Linear;
                    break;
                case AbcG::kCubic:
                    basis = GroomCurveBasis::BSpline;
                    break;
                case AbcG::kVariableOrder:
                default:
                    state.Fail(std::format("'{}' uses variable-order curves. The cooked groom format carries ONE "
                                           "basis for the whole asset, and collapsing per-curve orders would change "
                                           "the strand shapes; re-export as uniform linear or cubic curves.",
                                           primPath));
                    return;
            }
            if (sample.getWrap() != AbcG::kNonPeriodic)
            {
                state.Fail(std::format("'{}' contains periodic (closed) curves. A hair strand is an open curve with "
                                       "a root and a tip; re-export as non-periodic.",
                                       primPath));
                return;
            }
            if (state.AnyBasisSeen && state.Basis != basis)
            {
                state.Fail(std::format("'{}' is {} but an earlier prim in the same archive is {}. One cooked groom "
                                       "carries one basis; split the archive or re-export consistently.",
                                       primPath, ToString(basis), ToString(state.Basis)));
                return;
            }
            state.Basis = basis;
            state.AnyBasisSeen = true;

            // ── Topology ──
            const Abc::P3fArraySamplePtr positions = sample.getPositions();
            const Abc::Int32ArraySamplePtr vertexCounts = sample.getCurvesNumVertices();
            if (!positions || !vertexCounts)
            {
                state.Fail(std::format("'{}' is missing its positions or per-curve vertex counts", primPath));
                return;
            }
            const sizet curveCount = vertexCounts->size();
            if (curveCount == 0)
            {
                state.Fail(std::format("'{}' declares zero curves", primPath));
                return;
            }

            u64 totalVertices = 0;
            for (sizet c = 0; c < curveCount; ++c)
            {
                const i32 count = (*vertexCounts)[c];
                if (count < 0)
                {
                    state.Fail(std::format("'{}' curve {} declares a negative vertex count ({})", primPath, c, count));
                    return;
                }
                totalVertices += static_cast<u64>(count);
            }
            if (totalVertices != positions->size())
            {
                state.Fail(std::format("'{}' per-curve vertex counts sum to {} but the prim holds {} control points — "
                                       "the curve topology is malformed",
                                       primPath, totalVertices, positions->size()));
                return;
            }

            // ── Widths ──
            std::vector<f32> widthValues;
            AbcG::GeometryScope widthScope = AbcG::kUnknownScope;
            if (AbcG::IFloatGeomParam widthsParam = schema.getWidthsParam(); widthsParam.valid())
            {
                widthScope = widthsParam.getScope();
                if (widthScope != AbcG::kVertexScope && widthScope != AbcG::kUniformScope &&
                    widthScope != AbcG::kConstantScope && widthScope != AbcG::kVaryingScope)
                {
                    state.Fail(std::format("'{}' widths have {} scope; this convention supports constant, uniform, "
                                           "varying or vertex scope",
                                           primPath, ScopeName(widthScope)));
                    return;
                }
                AbcG::IFloatGeomParam::Sample widthSample;
                widthsParam.getExpanded(widthSample, Abc::ISampleSelector(Abc::index_t(0)));
                const Abc::FloatArraySamplePtr values = widthSample.getVals();
                if (!values)
                {
                    state.Fail(std::format("'{}' declares a widths parameter with no values", primPath));
                    return;
                }
                widthValues.assign(values->get(), values->get() + values->size());

                // kVaryingScope on curves means one value per curve, like
                // uniform — normalise here so the emit loop has two cases.
                const sizet expected = (widthScope == AbcG::kVertexScope)     ? static_cast<sizet>(totalVertices)
                                       : (widthScope == AbcG::kConstantScope) ? 1u
                                                                              : curveCount;
                if (widthValues.size() != expected)
                {
                    state.Fail(std::format("'{}' widths have {} scope and {} values, but {} were expected",
                                           primPath, ScopeName(widthScope), widthValues.size(), expected));
                    return;
                }
            }
            else if (!state.WarnedMissingWidths)
            {
                state.WarnedMissingWidths = true;
                state.Warn(std::format("'{}' carries no `widths`; substituted {} source units for every control "
                                       "point. The groom imports, but its strand thickness is NOT authored data.",
                                       primPath, AlembicGroomImporter::kDefaultWidth));
                OLO_CORE_WARN("AlembicGroomImporter: {}", state.Warnings.back());
            }

            // ── Root UVs ──
            std::vector<glm::vec2> uvValues;
            AbcG::GeometryScope uvScope = AbcG::kUnknownScope;
            if (AbcG::IV2fGeomParam uvParam = schema.getUVsParam(); uvParam.valid())
            {
                uvScope = uvParam.getScope();
                if (uvScope != AbcG::kUniformScope && uvScope != AbcG::kVertexScope && uvScope != AbcG::kVaryingScope &&
                    uvScope != AbcG::kConstantScope)
                {
                    state.Fail(std::format("'{}' uvs have {} scope; this convention supports constant, uniform, "
                                           "varying or vertex scope",
                                           primPath, ScopeName(uvScope)));
                    return;
                }
                AbcG::IV2fGeomParam::Sample uvSample;
                uvParam.getExpanded(uvSample, Abc::ISampleSelector(Abc::index_t(0)));
                const AbcG::V2fArraySamplePtr values = uvSample.getVals();
                if (!values)
                {
                    state.Fail(std::format("'{}' declares a uvs parameter with no values", primPath));
                    return;
                }
                uvValues.reserve(values->size());
                for (sizet i = 0; i < values->size(); ++i)
                {
                    uvValues.emplace_back((*values)[i].x, (*values)[i].y);
                }

                const sizet expected = (uvScope == AbcG::kVertexScope)     ? static_cast<sizet>(totalVertices)
                                       : (uvScope == AbcG::kConstantScope) ? 1u
                                                                           : curveCount;
                if (uvValues.size() != expected)
                {
                    state.Fail(std::format("'{}' uvs have {} scope and {} values, but {} were expected",
                                           primPath, ScopeName(uvScope), uvValues.size(), expected));
                    return;
                }
                if (uvScope == AbcG::kVertexScope)
                {
                    OLO_CORE_INFO("AlembicGroomImporter: '{}' authors per-VERTEX uvs; a groom's root UV is per-curve, "
                                  "so the value at each curve's ROOT is kept and the rest are discarded.",
                                  primPath);
                }
            }
            else if (!state.WarnedMissingUVs)
            {
                state.WarnedMissingUVs = true;
                state.Warn(std::format("'{}' carries no `uvs`; every root UV is (0,0). Root UVs are what bind a "
                                       "groom to its surface parameterisation — this groom has none.",
                                       primPath));
                OLO_CORE_WARN("AlembicGroomImporter: {}", state.Warnings.back());
            }

            // ── Arbitrary geometry parameters ──
            const Abc::ICompoundProperty arbGeomParams = schema.getArbGeomParams();
            CheckArbGeomParams(state, arbGeomParams, primPath);
            if (state.Failed)
            {
                return;
            }

            std::vector<i32> guideFlags;
            std::vector<i32> subGroups;
            std::vector<i32> roles;
            if (!ReadUniformIntParam(state, arbGeomParams, kGuideParamName, curveCount, primPath, guideFlags) ||
                !ReadUniformIntParam(state, arbGeomParams, kGroupParamName, curveCount, primPath, subGroups) ||
                !ReadUniformIntParam(state, arbGeomParams, kRoleParamName, curveCount, primPath, roles))
            {
                return;
            }

            // ── Groups ──
            // Registered up front, in ascending sub-group order, so ids are a
            // deterministic function of the file rather than of the order the
            // curves happen to appear in.
            std::string reason;
            std::vector<u16> groupIdBySubGroup;
            u16 primGroupId = 0;
            if (subGroups.empty())
            {
                if (!state.Builder.AddGroup(primPath, primGroupId, reason))
                {
                    state.Fail(std::format("'{}': {}", primPath, reason));
                    return;
                }
            }
            else
            {
                std::vector<i32> distinct = subGroups;
                std::sort(distinct.begin(), distinct.end());
                distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
                for (const i32 subGroup : distinct)
                {
                    u16 id = 0;
                    if (!state.Builder.AddGroup(std::format("{}#{}", primPath, subGroup), id, reason))
                    {
                        state.Fail(std::format("'{}': {}", primPath, reason));
                        return;
                    }
                }
                // Map sub-group value -> builder id by looking each one up
                // again; AddGroup is idempotent on an existing name.
                groupIdBySubGroup.resize(subGroups.size());
                for (sizet c = 0; c < subGroups.size(); ++c)
                {
                    u16 id = 0;
                    if (!state.Builder.AddGroup(std::format("{}#{}", primPath, subGroups[c]), id, reason))
                    {
                        state.Fail(std::format("'{}': {}", primPath, reason));
                        return;
                    }
                    groupIdBySubGroup[c] = id;
                }
            }

            // ── Coat roles (issue #1251) ──
            //
            // A ROLE IS A PROPERTY OF THE GROUP, not of the curve, so the
            // per-curve param is collapsed here and a group whose curves
            // disagree is REJECTED by name rather than resolved by a majority
            // vote. The rejection is the point: "half of my undercoat is tagged
            // as guard hair" is an authoring mistake the artist can fix in
            // seconds and cannot see at all once it has been averaged away.
            //
            // With no `groom_role` param at all, every group keeps the role
            // GroomBuilder::AddGroup inferred from its NAME — which is the path
            // every groom exported by a DCC that has never heard of this engine
            // takes, and is why the name heuristic exists.
            if (!roles.empty())
            {
                std::unordered_map<u16, i32> roleByGroup;
                for (sizet c = 0; c < curveCount; ++c)
                {
                    const u16 groupId = groupIdBySubGroup.empty() ? primGroupId : groupIdBySubGroup[c];
                    if (!IsValidGroomCoatRole(roles[c]))
                    {
                        state.Fail(std::format("'{}': curve {} has groom_role {}, which is not one of the {} "
                                               "GroomCoatRole values",
                                               primPath, c, roles[c], GroomCoatRoleCount));
                        return;
                    }
                    const auto [it, inserted] = roleByGroup.emplace(groupId, roles[c]);
                    if (!inserted && it->second != roles[c])
                    {
                        state.Fail(std::format("'{}': group {} carries both groom_role {} and {}; a coat role is a "
                                               "property of the group and must be constant within it",
                                               primPath, groupId, it->second, roles[c]));
                        return;
                    }
                }
                // Iterated as a SORTED vector, not as the map: the map's own
                // order is unspecified, and although only the reasons (not the
                // asset) would differ, GroomCooker.h's determinism rule is that
                // nothing here iterates an unordered container to produce
                // output.
                std::vector<std::pair<u16, i32>> ordered(roleByGroup.begin(), roleByGroup.end());
                std::sort(ordered.begin(), ordered.end());
                std::vector<std::string> repairs;
                for (const auto& [groupId, role] : ordered)
                {
                    GroomCoatGroupDesc desc;
                    desc.Role = static_cast<u8>(role);
                    if (!state.Builder.SetGroupCoat(groupId, desc, repairs))
                    {
                        state.Fail(std::format("'{}': {}", primPath,
                                               repairs.empty() ? std::string("coat role assignment failed")
                                                               : repairs.back()));
                        return;
                    }
                }
                for (const std::string& repair : repairs)
                {
                    OLO_CORE_WARN("AlembicGroomImporter: '{}': {}", primPath, repair);
                }
            }

            // ── Emit ──
            bool nonUniformScale = false;
            const f32 widthScale = WidthScaleOf(worldXf, nonUniformScale);
            if (nonUniformScale)
            {
                OLO_CORE_WARN("AlembicGroomImporter: '{}' sits under a NON-UNIFORM transform. Strand widths are a "
                              "single scalar, so they are scaled by the mean axis length ({:.6f}) — thickness will "
                              "not match an anisotropically scaled render.",
                              primPath, widthScale);
            }

            std::vector<glm::vec3> curvePoints;
            std::vector<f32> curveWidths;
            sizet vertexCursor = 0;
            for (sizet c = 0; c < curveCount; ++c)
            {
                const auto pointsInCurve = static_cast<sizet>((*vertexCounts)[c]);

                curvePoints.clear();
                curveWidths.clear();
                curvePoints.reserve(pointsInCurve);
                curveWidths.reserve(pointsInCurve);

                for (sizet i = 0; i < pointsInCurve; ++i)
                {
                    curvePoints.push_back(TransformPoint(worldXf, (*positions)[vertexCursor + i]));

                    f32 width = AlembicGroomImporter::kDefaultWidth;
                    if (!widthValues.empty())
                    {
                        width = (widthScope == AbcG::kVertexScope)     ? widthValues[vertexCursor + i]
                                : (widthScope == AbcG::kConstantScope) ? widthValues[0]
                                                                       : widthValues[c];
                    }
                    curveWidths.push_back(width * widthScale);
                }

                glm::vec2 rootUV(0.0f);
                if (!uvValues.empty())
                {
                    rootUV = (uvScope == AbcG::kVertexScope)     ? uvValues[vertexCursor]
                             : (uvScope == AbcG::kConstantScope) ? uvValues[0]
                                                                 : uvValues[c];
                }

                GroomCurveInput input;
                input.Points = curvePoints;
                input.Widths = curveWidths;
                input.RootUV = rootUV;
                input.GroupId = groupIdBySubGroup.empty() ? primGroupId : groupIdBySubGroup[c];
                input.IsGuide = !guideFlags.empty() && guideFlags[c] != 0;

                if (!state.Builder.AddCurve(input, reason))
                {
                    state.Fail(std::format("'{}': {}", primPath, reason));
                    return;
                }

                vertexCursor += pointsInCurve;
                ++state.CurvesRead;
            }

            ++state.PrimsRead;
        }

        void Visit(const Abc::IObject& obj, const Imath::M44d& parentXf, GroomTraversalState& state, u32 depth)
        {
            if (state.Failed)
            {
                return;
            }
            if (depth > kMaxTraversalDepth)
            {
                state.Fail(std::format("object hierarchy is nested deeper than {} levels at '{}'; refusing to "
                                       "recurse further (a deeper archive would exhaust the stack, which cannot be "
                                       "reported as a rejection)",
                                       kMaxTraversalDepth, obj.getFullName()));
                return;
            }

            Imath::M44d worldXf = parentXf;
            if (AbcG::IXform::matches(obj.getHeader()))
            {
                AbcG::IXform xform(obj, Abc::kWrapExisting);
                const AbcG::XformSample xformSample = xform.getSchema().getValue(Abc::ISampleSelector(Abc::index_t(0)));
                // Same rule as AlembicMeshImporter: child-times-parent in the
                // row-vector convention, but a non-inheriting xform is already
                // world-space and must not fold in the parent.
                worldXf = xformSample.getInheritsXforms() ? (xformSample.getMatrix() * parentXf) : xformSample.getMatrix();
            }

            if (AbcG::ICurves::matches(obj.getHeader()))
            {
                ReadCurves(state, obj, worldXf);
                if (state.Failed)
                {
                    return;
                }
            }

            for (sizet i = 0; i < obj.getNumChildren(); ++i)
            {
                Visit(obj.getChild(i), worldXf, state, depth + 1u);
                if (state.Failed)
                {
                    return;
                }
            }
        }

        // Same depth bound as Visit, for the same reason. This one answers a
        // yes/no routing question, so hitting the bound answers "no" rather than
        // failing — an archive that deep is not one this build will import
        // anyway, and Import's own Visit reports the depth by name.
        [[nodiscard]] bool AnyCurvesUnder(const Abc::IObject& obj, u32 depth = 0)
        {
            if (depth > kMaxTraversalDepth)
            {
                return false;
            }
            if (AbcG::ICurves::matches(obj.getHeader()))
            {
                return true;
            }
            for (sizet i = 0; i < obj.getNumChildren(); ++i)
            {
                if (AnyCurvesUnder(obj.getChild(i), depth + 1u))
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool OpenArchive(const std::filesystem::path& path, Abc::IArchive& outArchive, std::string& outReason)
        {
            if (!std::filesystem::exists(path))
            {
                outReason = std::format("file does not exist: {}", path.string());
                return false;
            }

            AbcF::IFactory factory;
            factory.setPolicy(Abc::ErrorHandler::kQuietNoopPolicy);
            AbcF::IFactory::CoreType coreType = AbcF::IFactory::kUnknown;
            try
            {
                outArchive = factory.getArchive(path.string(), coreType);
            }
            catch (const std::exception& e)
            {
                outReason = std::format("failed to open archive '{}': {}", path.string(), e.what());
                return false;
            }
            if (!outArchive.valid())
            {
                outReason = std::format("'{}' is not a readable Alembic (Ogawa) archive", path.string());
                return false;
            }
            return true;
        }
    } // anonymous namespace

    bool AlembicGroomImporter::ArchiveContainsCurves(const std::filesystem::path& path)
    {
        Abc::IArchive archive;
        std::string reason;
        if (!OpenArchive(path, archive, reason))
        {
            OLO_CORE_TRACE("AlembicGroomImporter::ArchiveContainsCurves: {}", reason);
            return false;
        }
        try
        {
            return AnyCurvesUnder(archive.getTop());
        }
        catch (const std::exception& e)
        {
            OLO_CORE_WARN("AlembicGroomImporter::ArchiveContainsCurves: traversal of '{}' threw: {}", path.string(), e.what());
            return false;
        }
    }

    AlembicGroomImporter::SidecarCookResult AlembicGroomImporter::ImportAndCookToSidecar(
        const std::filesystem::path& abcPath, const Options& options, const std::filesystem::path& outputPath)
    {
        SidecarCookResult cooked;
        cooked.OutputPath = outputPath.empty() ? std::filesystem::path(abcPath).replace_extension(".ologroom")
                                               : outputPath;

        // The distinction the whole feature rests on, said plainly: a polygon
        // archive is not a groom, and importing it as one would produce nothing.
        if (!ArchiveContainsCurves(abcPath))
        {
            cooked.Diagnostic = std::format("'{}' holds no ICurves prims, so it is not a groom. Polygon Alembic "
                                            "import is a separate path (AlembicMeshImporter, via MeshSource).",
                                            abcPath.string());
            return cooked;
        }

        const Result imported = Import(abcPath, options);
        cooked.Warnings = imported.Warnings;
        if (!imported.Succeeded())
        {
            cooked.Diagnostic = imported.Diagnostic;
            return cooked;
        }

        std::vector<u8> bytes;
        std::string reason;
        if (!GroomCooker::CookToBytes(*imported.Groom, bytes, reason))
        {
            cooked.Diagnostic = std::format("failed to cook '{}': {}", abcPath.string(), reason);
            return cooked;
        }

        if (const std::filesystem::path parent = cooked.OutputPath.parent_path(); !parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                cooked.Diagnostic = std::format("could not create '{}': {}", parent.string(), ec.message());
                return cooked;
            }
        }

        std::ofstream out(cooked.OutputPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            cooked.Diagnostic = std::format("could not open '{}' for writing", cooked.OutputPath.string());
            return cooked;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            cooked.Diagnostic = std::format("failed while writing '{}'", cooked.OutputPath.string());
            return cooked;
        }
        out.close();

        cooked.Ok = true;
        cooked.CurveCount = imported.Groom->GetCurveCount();
        cooked.GroupCount = imported.Groom->GetGroupCount();
        cooked.GuideCount = imported.Groom->GetGuideCount();
        cooked.CookedBytes = bytes.size();
        return cooked;
    }

    AlembicGroomImporter::Result AlembicGroomImporter::Import(const std::filesystem::path& path, const Options& options)
    {
        Abc::IArchive archive;
        std::string reason;
        if (!OpenArchive(path, archive, reason))
        {
            return Result::Failure(std::format("AlembicGroomImporter: {}", reason));
        }

        // Hash the SOURCE BYTES for provenance before parsing: the hash must
        // identify the file the groom came from, not this importer's
        // interpretation of it.
        u64 sourceHash = 0;
        {
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open())
            {
                return Result::Failure(std::format("AlembicGroomImporter: cannot re-open '{}' to hash its contents",
                                                   path.string()));
            }
            // Streamed FNV-1a 64, chunk state carried across reads so the
            // result equals GroomCooker::HashSourceBytes over the whole file —
            // the determinism test relies on those two agreeing.
            constexpr u64 kOffsetBasis = 14695981039346656037ull;
            constexpr u64 kPrime = 1099511628211ull;
            std::vector<char> buffer(64 * 1024);
            u64 hash = kOffsetBasis;
            while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0)
            {
                const auto read = static_cast<sizet>(in.gcount());
                const auto* bytes = reinterpret_cast<const u8*>(buffer.data());
                for (sizet i = 0; i < read; ++i)
                {
                    hash ^= static_cast<u64>(bytes[i]);
                    hash *= kPrime;
                }
            }
            sourceHash = hash;
        }

        GroomTraversalState state;
        try
        {
            Visit(archive.getTop(), Imath::M44d(), state, 0u);
        }
        catch (const std::exception& e)
        {
            return Result::Failure(std::format("AlembicGroomImporter: traversal of '{}' threw: {}", path.string(), e.what()));
        }

        if (state.Failed)
        {
            return Result::Failure(std::format("AlembicGroomImporter: rejected '{}': {}", path.string(), state.Diagnostic));
        }
        if (state.PrimsRead == 0)
        {
            return Result::Failure(std::format(
                "AlembicGroomImporter: '{}' contains no ICurves prims. Polygon Alembic import is NOT curve support — "
                "if this archive holds a groom exported as ribbons or tubes, re-export it as curves.",
                path.string()));
        }

        GroomProvenance provenance;
        provenance.SourcePath = options.ProvenancePath.empty() ? path.filename().string() : options.ProvenancePath;
        provenance.SourceFormat = "AlembicCurves";
        provenance.SourceContentHash = sourceHash;
        provenance.ImporterVersion = kImporterVersion;

        state.Builder.SetBasis(state.Basis);
        state.Builder.SetProvenance(std::move(provenance));
        state.Builder.SetName(path.stem().string());

        Ref<GroomAsset> groom = state.Builder.Build(reason);
        if (!groom)
        {
            return Result::Failure(std::format("AlembicGroomImporter: '{}' produced an invalid groom: {}",
                                               path.string(), reason));
        }

        // Canonicalise here rather than at save time so that the groom a
        // caller holds and the groom that gets cooked are the SAME ordering —
        // otherwise a preview drawn before the first save would disagree with
        // one drawn after it.
        if (!GroomCooker::Canonicalize(*groom, reason))
        {
            return Result::Failure(std::format("AlembicGroomImporter: '{}' failed canonicalisation: {}",
                                               path.string(), reason));
        }

        // ── The cooked card level (issue #1252) ─────────────────────────────
        //
        // AFTER canonicalisation, because the level's source map indexes the
        // base groom's curves and the reorder renumbers them. (GroomCooker's
        // Canonicalize remaps the map when it does reorder, so building either
        // side of it is correct — but building it here means the level is a
        // function of the FINAL ordering and the cook has nothing to fix up,
        // which is one fewer thing to be right about.)
        //
        // A FAILED BUILD IS NOT A FAILED IMPORT, and the reason is logged by
        // name rather than swallowed: the commonest cause is a groom whose
        // strands are too few or too evenly spread for the cell to cluster
        // anything, which is a fact about the groom. The asset then simply
        // never leaves the strand tier, and both the inspector and
        // GroomLodFallbackReason::LevelNotCooked say so out loud.
        if (options.BuildCardLod)
        {
            GroomLodLevel cards;
            GroomCardBuildStats cardStats;
            std::string cardReason;
            if (GroomLodBuilder::BuildCardLevel(*groom, options.Cards, cards, cardReason, &cardStats))
            {
                if (GroomLodBuilder::AttachLodLevels(*groom, { std::move(cards) }, cardReason))
                {
                    OLO_CORE_INFO("AlembicGroomImporter: '{}' cooked {} cards from {} strands (cell {:.4f}, "
                                  "cluster min {} mean {:.1f} max {})",
                                  path.filename().string(), cardStats.CardsBuilt, cardStats.CurvesConsidered,
                                  options.Cards.CellSize, cardStats.SmallestCluster, cardStats.MeanCluster,
                                  cardStats.LargestCluster);
                }
                else
                {
                    OLO_CORE_WARN("AlembicGroomImporter: '{}' built a card level that did not validate ({}); the "
                                  "groom will never leave the strand tier",
                                  path.filename().string(), cardReason);
                }
            }
            else
            {
                OLO_CORE_INFO("AlembicGroomImporter: '{}' cooked no card level: {}", path.filename().string(),
                              cardReason);
            }
        }

        OLO_CORE_INFO("AlembicGroomImporter: imported '{}' — {} curves, {} control points, {} groups, {} guides, "
                      "basis {}, bounds ({:.4f},{:.4f},{:.4f})-({:.4f},{:.4f},{:.4f}), source hash 0x{:016X}",
                      path.filename().string(), groom->GetCurveCount(), groom->GetPointCount(),
                      groom->GetGroupCount(), groom->GetGuideCount(), ToString(groom->GetBasis()),
                      groom->GetBoundsMin().x, groom->GetBoundsMin().y, groom->GetBoundsMin().z,
                      groom->GetBoundsMax().x, groom->GetBoundsMax().y, groom->GetBoundsMax().z,
                      groom->GetProvenance().SourceContentHash);

        Result result;
        result.Groom = groom;
        result.Warnings = std::move(state.Warnings);
        result.CurvesRead = state.CurvesRead;
        result.PrimsRead = state.PrimsRead;
        return result;
    }
} // namespace OloEngine

#endif // OLO_WITH_ALEMBIC
