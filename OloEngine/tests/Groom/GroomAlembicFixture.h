#pragma once

// =============================================================================
// GroomAlembicFixture.h — authors REAL Alembic ICurves archives for the groom
// tests (issue #1232).
//
// WHY THE TESTS WRITE THEIR OWN .abc INSTEAD OF COMMITTING FIXTURES.
// The thing under test is the ICurves reader, and a committed binary fixture
// can only ever exercise the one shape whoever produced it happened to export.
// Authoring archives here means a test can say "widths at UNIFORM scope, uvs
// at VERTEX scope, two sub-groups, three guides" and get exactly that — which
// is what makes the rejection tests (AC 3) possible at all, since a malformed
// archive is not something a DCC will export for you.
//
// It also keeps the repo free of opaque binary blobs whose provenance nobody
// can check, and it uses Alembic's own writer, so the bytes the reader sees are
// bytes Alembic produced rather than bytes this repo believes Alembic produces.
// =============================================================================

#if defined(OLO_WITH_ALEMBIC)

#include "OloEngine/Core/Base.h"

#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include <glm/glm.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace GroomFixture
    {
        namespace Abc = Alembic::Abc;
        namespace AbcG = Alembic::AbcGeom;

        // One ICurves prim's worth of authored data. Everything is optional
        // except Positions and VertexCounts, which is exactly the freedom the
        // convention in AlembicGroomImporter.h documents.
        struct CurvesPrim
        {
            std::string Name = "groom";

            std::vector<Imath::V3f> Positions;
            std::vector<i32> VertexCounts;

            AbcG::CurveType Type = AbcG::kLinear;
            AbcG::CurvePeriodicity Wrap = AbcG::kNonPeriodic;
            AbcG::BasisType Basis = AbcG::kNoBasis;

            // Empty leaves the param off the prim entirely.
            std::vector<f32> Widths;
            AbcG::GeometryScope WidthScope = AbcG::kVertexScope;

            std::vector<Imath::V2f> UVs;
            AbcG::GeometryScope UVScope = AbcG::kUniformScope;

            // Uniform-scope int32 arbGeomParams. Empty leaves them off.
            std::vector<i32> GuideFlags;
            std::vector<i32> SubGroups;

            // Any extra arbGeomParam to author, by name. Used by the rejection
            // tests to plant an unsupported `groom_*` attribute.
            std::vector<std::string> ExtraIntParamNames;

            // Applied as an OXform above the curves when not identity.
            Imath::M44d Transform = Imath::M44d();
        };

        // Writes every prim into one Ogawa archive at `path`. Returns false if
        // Alembic threw — the caller should ASSERT on it rather than proceed,
        // because a failed write makes the subsequent import test meaningless.
        inline bool WriteArchive(const std::filesystem::path& path, const std::vector<CurvesPrim>& prims)
        {
            try
            {
                Abc::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path.string());

                for (const CurvesPrim& prim : prims)
                {
                    Abc::OObject parent = archive.getTop();
                    AbcG::OXform xform;
                    if (prim.Transform != Imath::M44d())
                    {
                        xform = AbcG::OXform(parent, prim.Name + "_xf");
                        AbcG::XformSample xformSample;
                        xformSample.setMatrix(prim.Transform);
                        xform.getSchema().set(xformSample);
                        parent = xform;
                    }

                    AbcG::OCurves curves(parent, prim.Name);
                    AbcG::OCurvesSchema& schema = curves.getSchema();

                    AbcG::OFloatGeomParam::Sample widthSample;
                    if (!prim.Widths.empty())
                    {
                        widthSample = AbcG::OFloatGeomParam::Sample(
                            Abc::FloatArraySample(prim.Widths.data(), prim.Widths.size()), prim.WidthScope);
                    }

                    AbcG::OV2fGeomParam::Sample uvSample;
                    if (!prim.UVs.empty())
                    {
                        uvSample = AbcG::OV2fGeomParam::Sample(
                            Abc::V2fArraySample(prim.UVs.data(), prim.UVs.size()), prim.UVScope);
                    }

                    AbcG::OCurvesSchema::Sample sample(
                        Abc::P3fArraySample(prim.Positions.data(), prim.Positions.size()),
                        Abc::Int32ArraySample(prim.VertexCounts.data(), prim.VertexCounts.size()),
                        prim.Type, prim.Wrap, widthSample, uvSample,
                        AbcG::ON3fGeomParam::Sample(), prim.Basis);

                    schema.set(sample);

                    auto writeIntParam = [&schema](const char* name, const std::vector<i32>& values)
                    {
                        if (values.empty())
                        {
                            return;
                        }
                        Abc::OCompoundProperty arb = schema.getArbGeomParams();
                        AbcG::OInt32GeomParam param(arb, name, false, AbcG::kUniformScope, 1);
                        AbcG::OInt32GeomParam::Sample paramSample;
                        paramSample.setVals(Abc::Int32ArraySample(values.data(), values.size()));
                        paramSample.setScope(AbcG::kUniformScope);
                        param.set(paramSample);
                    };

                    writeIntParam("groom_guide", prim.GuideFlags);
                    writeIntParam("groom_group", prim.SubGroups);
                    for (const std::string& extra : prim.ExtraIntParamNames)
                    {
                        // One value per curve, so the param itself is
                        // well-formed and the rejection under test is the NAME,
                        // not the shape.
                        std::vector<i32> filler(prim.VertexCounts.size(), 0);
                        writeIntParam(extra.c_str(), filler);
                    }
                }
                return true;
            }
            catch (const std::exception&)
            {
                return false;
            }
        }

        // ── Deterministic groom generators ──────────────────────────────────
        // Both build strands from integer indices through fixed arithmetic, so
        // the same call produces bit-identical positions on every run and every
        // platform. That is what makes the determinism test meaningful: if the
        // GENERATOR drifted, a byte difference downstream would prove nothing.

        // A human scalp groom: strands on a hemisphere, tapering root->tip,
        // root UVs from the hemisphere's spherical parameterisation, and every
        // Nth strand marked as a guide.
        inline CurvesPrim MakeHumanScalpGroom(u32 strandCount, u32 pointsPerStrand, u32 guideStride,
                                              const std::string& name = "scalp")
        {
            CurvesPrim prim;
            prim.Name = name;
            prim.Type = AbcG::kLinear;
            prim.WidthScope = AbcG::kVertexScope;
            prim.UVScope = AbcG::kUniformScope;
            prim.Positions.reserve(static_cast<sizet>(strandCount) * pointsPerStrand);
            prim.Widths.reserve(static_cast<sizet>(strandCount) * pointsPerStrand);
            prim.VertexCounts.reserve(strandCount);
            prim.UVs.reserve(strandCount);
            prim.GuideFlags.reserve(strandCount);

            constexpr f32 kHeadRadius = 0.09f;   // metres — a human skull
            constexpr f32 kStrandLength = 0.25f; // a shoulder-length strand
            constexpr f32 kRootWidth = 0.00008f; // 0.08 mm
            constexpr f32 kGoldenAngle = 2.39996323f;

            for (u32 s = 0; s < strandCount; ++s)
            {
                // Fibonacci hemisphere: even coverage without a random number
                // generator, so the layout is a pure function of the index.
                const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(strandCount);
                const f32 cosTheta = 1.0f - t; // upper hemisphere only
                const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - (cosTheta * cosTheta)));
                const f32 phi = kGoldenAngle * static_cast<f32>(s);

                const Imath::V3f normal(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
                const Imath::V3f root = normal * kHeadRadius;

                for (u32 p = 0; p < pointsPerStrand; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(pointsPerStrand - 1);
                    // Grow out along the normal, then droop under gravity —
                    // quadratic in `along`, so the strand curves.
                    Imath::V3f point = root + (normal * (kStrandLength * along));
                    point.y -= kStrandLength * 0.6f * along * along;
                    prim.Positions.push_back(point);
                    prim.Widths.push_back(kRootWidth * (1.0f - (0.8f * along)));
                }

                prim.VertexCounts.push_back(static_cast<i32>(pointsPerStrand));
                // The hemisphere's own parameterisation: phi wrapped to [0,1),
                // and the polar angle. A real groom's root UV comes from the
                // scalp mesh, but the property under test is that it SURVIVES.
                prim.UVs.emplace_back(std::fmod(phi / (2.0f * 3.14159265f), 1.0f), t);
                prim.GuideFlags.push_back((guideStride != 0 && (s % guideStride) == 0) ? 1 : 0);
            }
            return prim;
        }

        // An animal fur groom: shorter, denser strands over a capsule body,
        // split into named sub-groups (back / flank / belly) so the group round
        // trip is exercised with more than one group.
        inline CurvesPrim MakeAnimalFurGroom(u32 strandCount, u32 pointsPerStrand, u32 subGroupCount,
                                             const std::string& name = "pelt")
        {
            CurvesPrim prim;
            prim.Name = name;
            prim.Type = AbcG::kCubic;
            prim.Basis = AbcG::kBsplineBasis;
            prim.WidthScope = AbcG::kUniformScope; // one width per strand
            prim.UVScope = AbcG::kUniformScope;
            prim.Positions.reserve(static_cast<sizet>(strandCount) * pointsPerStrand);
            prim.Widths.reserve(strandCount);
            prim.VertexCounts.reserve(strandCount);
            prim.UVs.reserve(strandCount);
            prim.GuideFlags.reserve(strandCount);
            prim.SubGroups.reserve(strandCount);

            constexpr f32 kBodyLength = 0.8f;
            constexpr f32 kBodyRadius = 0.12f;
            constexpr f32 kFurLength = 0.03f;
            constexpr f32 kFurWidth = 0.00006f;
            constexpr f32 kGoldenAngle = 2.39996323f;

            for (u32 s = 0; s < strandCount; ++s)
            {
                const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(strandCount);
                const f32 phi = kGoldenAngle * static_cast<f32>(s);
                const f32 alongBody = (t * kBodyLength) - (kBodyLength * 0.5f);

                const Imath::V3f normal(std::cos(phi), std::sin(phi), 0.0f);
                const Imath::V3f root((normal.x * kBodyRadius), (normal.y * kBodyRadius), alongBody);

                for (u32 p = 0; p < pointsPerStrand; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(pointsPerStrand - 1);
                    Imath::V3f point = root + (normal * (kFurLength * along));
                    // Lay the fur backwards along the body, as a coat lies.
                    point.z += kFurLength * 0.5f * along;
                    prim.Positions.push_back(point);
                }

                prim.VertexCounts.push_back(static_cast<i32>(pointsPerStrand));
                prim.Widths.push_back(kFurWidth);
                prim.UVs.emplace_back(std::fmod(phi / (2.0f * 3.14159265f), 1.0f), t);
                prim.GuideFlags.push_back(((s % 17u) == 0) ? 1 : 0);
                // Deliberately INTERLEAVED across sub-groups, so the cook's
                // grouping sort has something real to do — a generator that
                // emitted group 0 then group 1 would make Canonicalize a no-op
                // and the group-contiguity assertion vacuous.
                prim.SubGroups.push_back(static_cast<i32>(s % std::max(1u, subGroupCount)));
            }
            return prim;
        }
    } // namespace GroomFixture
} // namespace OloEngine::Tests

#endif // OLO_WITH_ALEMBIC
