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

#include <algorithm>
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
            // The GroomCoatRole per curve (issue #1251). Authored per curve
            // because Alembic has no per-group scope, and REQUIRED by the
            // importer to be constant within a group — which is a rejection the
            // rejection tests can now author, since a fixture can write two
            // different values into one group on purpose.
            std::vector<i32> Roles;

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
                    writeIntParam("groom_role", prim.Roles);
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

        // ── The two reference animals (issue #1251) ─────────────────────────
        //
        // Criterion 4 asks for a SHORT-COATED and a LONG-COATED animal with
        // distinct muzzle, ears, body and tail regions, and for whisker /
        // long-hair groups where present. Both are built here, from the same
        // integer arithmetic the two grooms above use, for the same reason: the
        // capture that judges the coat has to be a capture of a groom that is
        // bit-identical on every machine, or the picture and the numbers beside
        // it are measurements of different animals.
        //
        // HOW THE STRUCTURE MAPS ONTO THE IMPORTER'S CONVENTIONS, because this is
        // the part that is easy to get subtly wrong:
        //
        //   * ONE PRIM PER REGION (muzzle, ears, body, tail, mane). A prim's path
        //     is its group name, so the regions are separable by name in the
        //     editor and in a log line.
        //   * `groom_group` WITHIN a prim splits it into coat LAYERS, so a
        //     region's undercoat and guard hairs are different groups and can be
        //     adjusted apart. The importer names them "<prim>#<n>".
        //   * `groom_role` carries the GroomCoatRole per curve, constant within
        //     each of those groups — which is what the importer requires, and
        //     what makes the runtime undercoat/guard sliders reach them.
        //   * ROOT UVs are laid out in DISJOINT V BANDS per region, so one
        //     regional map painted in [0,1]^2 addresses the muzzle, the ears, the
        //     body and the tail separately. That is what criterion 2's "regional
        //     maps" needs to be testable at all: without disjoint bands every
        //     region samples the same texels and a regional map is indistinguish-
        //     able from a global multiplier.

        // Which layer a curve belongs to, spelled out rather than left as a bare
        // integer at each call site.
        enum class CoatLayer : i32
        {
            Undercoat = 0,
            Guard = 1,
        };

        // The GroomCoatRole values, restated as plain ints so this header does
        // not have to include the engine's Groom headers — it is compiled into
        // the test binary beside them, and the numbers are a FORMAT contract
        // (GroomCoat.h says "append, never renumber"), so pinning them here is a
        // second place that notices if they ever move.
        constexpr i32 kRoleUndercoat = 1;
        constexpr i32 kRoleGuardHair = 2;
        constexpr i32 kRoleWhisker = 3;
        constexpr i32 kRoleLongHair = 4;

        // One region of a coat: a band of strands over a section of the body,
        // split into an undercoat and a guard layer.
        struct CoatRegionSpec
        {
            std::string Name; // becomes the prim path, and so the group name
            u32 UndercoatStrands = 0;
            u32 GuardStrands = 0;
            u32 PointsPerStrand = 4;
            f32 UndercoatLength = 0.02f;
            f32 GuardLength = 0.05f;
            f32 UndercoatWidth = 0.00004f;
            f32 GuardWidth = 0.00010f;
            // The body interval this region covers, along the animal's z axis.
            f32 BodyFrom = -0.4f;
            f32 BodyTo = 0.4f;
            f32 BodyRadius = 0.12f;
            // The DISJOINT root-UV v band. See the note above.
            f32 VFrom = 0.0f;
            f32 VTo = 1.0f;
            // When set, the whole region is ONE group at this role instead of an
            // undercoat/guard pair — whiskers and a mane are single layers.
            i32 SingleRole = 0;
            u32 SingleStrands = 0;
            f32 SingleLength = 0.0f;
            f32 SingleWidth = 0.0f;
        };

        // Builds one region's ICurves prim.
        //
        // Deterministic by construction: every position comes from the strand
        // index through fixed arithmetic, with no random number generator and no
        // floating-point accumulation across strands.
        inline CurvesPrim MakeCoatRegion(const CoatRegionSpec& spec)
        {
            CurvesPrim prim;
            prim.Name = spec.Name;
            prim.Type = AbcG::kLinear;
            prim.WidthScope = AbcG::kVertexScope;
            prim.UVScope = AbcG::kUniformScope;

            constexpr f32 kGoldenAngle = 2.39996323f;
            constexpr f32 kTwoPi = 6.28318531f;

            // Layer, count, length, width, role — the four or two layers this
            // region emits, in a fixed order so the group ids are a pure
            // function of the spec.
            struct LayerPlan
            {
                u32 Count;
                f32 Length;
                f32 Width;
                i32 Role;
                i32 SubGroup;
            };
            std::vector<LayerPlan> layers;
            if (spec.SingleRole != 0)
            {
                layers.push_back({ spec.SingleStrands, spec.SingleLength, spec.SingleWidth, spec.SingleRole, 0 });
            }
            else
            {
                layers.push_back({ spec.UndercoatStrands, spec.UndercoatLength, spec.UndercoatWidth, kRoleUndercoat,
                                   static_cast<i32>(CoatLayer::Undercoat) });
                layers.push_back({ spec.GuardStrands, spec.GuardLength, spec.GuardWidth, kRoleGuardHair,
                                   static_cast<i32>(CoatLayer::Guard) });
            }

            u32 total = 0;
            for (const LayerPlan& layer : layers)
            {
                total += layer.Count;
            }
            prim.Positions.reserve(static_cast<sizet>(total) * spec.PointsPerStrand);
            prim.Widths.reserve(static_cast<sizet>(total) * spec.PointsPerStrand);
            prim.VertexCounts.reserve(total);
            prim.UVs.reserve(total);
            prim.GuideFlags.reserve(total);
            prim.SubGroups.reserve(total);
            prim.Roles.reserve(total);

            for (const LayerPlan& layer : layers)
            {
                for (u32 s = 0; s < layer.Count; ++s)
                {
                    const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(std::max(1u, layer.Count));
                    // The golden angle is OFFSET PER LAYER, so the undercoat and
                    // the guard hairs do not land on the same points of the body
                    // — a guard hair growing out of the exact root of an
                    // undercoat strand would make the two layers perfectly
                    // correlated, and every test of "the guard coat survived the
                    // budget" would then be measuring the undercoat.
                    const f32 phi = (kGoldenAngle * static_cast<f32>(s)) + (static_cast<f32>(layer.SubGroup) * 1.1f);
                    const f32 alongBody = spec.BodyFrom + (t * (spec.BodyTo - spec.BodyFrom));

                    const Imath::V3f normal(std::cos(phi), std::sin(phi), 0.0f);
                    const Imath::V3f root(normal.x * spec.BodyRadius, normal.y * spec.BodyRadius, alongBody);

                    for (u32 p = 0; p < spec.PointsPerStrand; ++p)
                    {
                        const f32 along = static_cast<f32>(p) / static_cast<f32>(spec.PointsPerStrand - 1);
                        Imath::V3f point = root + (normal * (layer.Length * along));
                        // Laid backwards along the body, as a coat lies, and
                        // drooping — so the strand has a real shape for the
                        // clump and length scaling to act on rather than being a
                        // straight spike whose modifications are invisible.
                        point.z += layer.Length * 0.6f * along;
                        point.y -= layer.Length * 0.25f * along * along;
                        prim.Positions.push_back(point);
                        prim.Widths.push_back(layer.Width * (1.0f - (0.7f * along)));
                    }

                    prim.VertexCounts.push_back(static_cast<i32>(spec.PointsPerStrand));
                    // u from the angle around the body, v inside this region's
                    // OWN band. Disjoint bands are what make a regional map
                    // regional.
                    const f32 u = std::fmod(phi / kTwoPi, 1.0f);
                    prim.UVs.emplace_back(u < 0.0f ? u + 1.0f : u, spec.VFrom + (t * (spec.VTo - spec.VFrom)));
                    // Every 23rd strand is a guide. A prime stride so it does not
                    // beat against the layer counts.
                    prim.GuideFlags.push_back(((s % 23u) == 0) ? 1 : 0);
                    prim.SubGroups.push_back(layer.SubGroup);
                    prim.Roles.push_back(layer.Role);
                }
            }
            return prim;
        }

        // The SHORT-COATED animal: a dense fine undercoat under sparse short
        // guard hairs, plus whiskers on the muzzle. The coat is close to the
        // body everywhere, so its silhouette is carried almost entirely by the
        // guard layer — which is what makes it the animal that shows a budget
        // eating the silhouette.
        inline std::vector<CurvesPrim> MakeShortCoatAnimal(u32 scale = 1000)
        {
            std::vector<CurvesPrim> prims;

            CoatRegionSpec body;
            body.Name = "body";
            body.UndercoatStrands = scale * 6u;
            body.GuardStrands = scale;
            body.UndercoatLength = 0.012f;
            body.GuardLength = 0.022f;
            body.BodyFrom = -0.35f;
            body.BodyTo = 0.30f;
            body.BodyRadius = 0.11f;
            body.VFrom = 0.00f;
            body.VTo = 0.50f;
            prims.push_back(MakeCoatRegion(body));

            CoatRegionSpec head = body;
            head.Name = "head";
            head.UndercoatStrands = scale;
            head.GuardStrands = scale / 4u;
            head.UndercoatLength = 0.006f;
            head.GuardLength = 0.010f;
            head.BodyFrom = 0.30f;
            head.BodyTo = 0.42f;
            head.BodyRadius = 0.075f;
            head.VFrom = 0.50f;
            head.VTo = 0.70f;
            prims.push_back(MakeCoatRegion(head));

            CoatRegionSpec ears = body;
            ears.Name = "ears";
            ears.UndercoatStrands = scale / 4u;
            ears.GuardStrands = scale / 8u;
            ears.UndercoatLength = 0.004f;
            ears.GuardLength = 0.014f;
            ears.BodyFrom = 0.42f;
            ears.BodyTo = 0.48f;
            ears.BodyRadius = 0.05f;
            ears.VFrom = 0.70f;
            ears.VTo = 0.85f;
            prims.push_back(MakeCoatRegion(ears));

            CoatRegionSpec tail = body;
            tail.Name = "tail";
            tail.UndercoatStrands = scale / 2u;
            tail.GuardStrands = scale / 4u;
            tail.UndercoatLength = 0.015f;
            tail.GuardLength = 0.030f;
            tail.BodyFrom = -0.50f;
            tail.BodyTo = -0.35f;
            tail.BodyRadius = 0.035f;
            tail.VFrom = 0.85f;
            tail.VTo = 1.00f;
            prims.push_back(MakeCoatRegion(tail));

            // The whiskers: a handful, very long, very thick, on the muzzle.
            // They are a SINGLE group at the Whisker role, and they are why that
            // role exists — a budget that removed three of them would be visible
            // damage rather than distance.
            CoatRegionSpec muzzle;
            muzzle.Name = "muzzle_whiskers";
            muzzle.PointsPerStrand = 6;
            muzzle.SingleRole = kRoleWhisker;
            muzzle.SingleStrands = 24;
            muzzle.SingleLength = 0.070f;
            muzzle.SingleWidth = 0.00025f;
            muzzle.BodyFrom = 0.44f;
            muzzle.BodyTo = 0.47f;
            muzzle.BodyRadius = 0.045f;
            muzzle.VFrom = 0.50f;
            muzzle.VTo = 0.52f;
            prims.push_back(MakeCoatRegion(muzzle));

            return prims;
        }

        // The LONG-COATED animal: the same regions, with a long guard coat, a
        // mane and a tail plume. The mane and plume are the LongHair role —
        // neither structural guard coat nor whisker — and they are what a capture
        // of this animal is checked for: a long coat that renders as the short
        // one's fuzz at a different scale is criterion 4's failure.
        inline std::vector<CurvesPrim> MakeLongCoatAnimal(u32 scale = 1000)
        {
            std::vector<CurvesPrim> prims;

            CoatRegionSpec body;
            body.Name = "body";
            body.PointsPerStrand = 6;
            body.UndercoatStrands = scale * 5u;
            body.GuardStrands = scale;
            body.UndercoatLength = 0.030f;
            body.GuardLength = 0.090f;
            body.UndercoatWidth = 0.00005f;
            body.GuardWidth = 0.00014f;
            body.BodyFrom = -0.35f;
            body.BodyTo = 0.30f;
            body.BodyRadius = 0.12f;
            body.VFrom = 0.00f;
            body.VTo = 0.50f;
            prims.push_back(MakeCoatRegion(body));

            CoatRegionSpec head = body;
            head.Name = "head";
            head.UndercoatStrands = scale;
            head.GuardStrands = scale / 4u;
            head.UndercoatLength = 0.012f;
            head.GuardLength = 0.028f;
            head.BodyFrom = 0.30f;
            head.BodyTo = 0.42f;
            head.BodyRadius = 0.080f;
            head.VFrom = 0.50f;
            head.VTo = 0.70f;
            prims.push_back(MakeCoatRegion(head));

            CoatRegionSpec ears = body;
            ears.Name = "ears";
            ears.UndercoatStrands = scale / 4u;
            ears.GuardStrands = scale / 8u;
            ears.UndercoatLength = 0.008f;
            ears.GuardLength = 0.035f;
            ears.BodyFrom = 0.42f;
            ears.BodyTo = 0.48f;
            ears.BodyRadius = 0.055f;
            ears.VFrom = 0.70f;
            ears.VTo = 0.85f;
            prims.push_back(MakeCoatRegion(ears));

            CoatRegionSpec tail = body;
            tail.Name = "tail_plume";
            tail.SingleRole = kRoleLongHair;
            tail.SingleStrands = scale;
            tail.SingleLength = 0.160f;
            tail.SingleWidth = 0.00012f;
            tail.BodyFrom = -0.52f;
            tail.BodyTo = -0.35f;
            tail.BodyRadius = 0.040f;
            tail.VFrom = 0.85f;
            tail.VTo = 1.00f;
            prims.push_back(MakeCoatRegion(tail));

            CoatRegionSpec mane = body;
            mane.Name = "mane";
            mane.SingleRole = kRoleLongHair;
            mane.SingleStrands = scale * 2u;
            mane.SingleLength = 0.140f;
            mane.SingleWidth = 0.00013f;
            mane.BodyFrom = 0.16f;
            mane.BodyTo = 0.32f;
            mane.BodyRadius = 0.115f;
            mane.VFrom = 0.52f;
            mane.VTo = 0.68f;
            prims.push_back(MakeCoatRegion(mane));

            CoatRegionSpec muzzle;
            muzzle.Name = "muzzle_whiskers";
            muzzle.PointsPerStrand = 6;
            muzzle.SingleRole = kRoleWhisker;
            muzzle.SingleStrands = 24;
            muzzle.SingleLength = 0.080f;
            muzzle.SingleWidth = 0.00028f;
            muzzle.BodyFrom = 0.44f;
            muzzle.BodyTo = 0.47f;
            muzzle.BodyRadius = 0.048f;
            muzzle.VFrom = 0.50f;
            muzzle.VTo = 0.52f;
            prims.push_back(MakeCoatRegion(muzzle));

            return prims;
        }
    } // namespace GroomFixture
} // namespace OloEngine::Tests

#endif // OLO_WITH_ALEMBIC
