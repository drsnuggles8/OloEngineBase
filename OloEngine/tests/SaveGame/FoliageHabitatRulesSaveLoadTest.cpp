// OLO_TEST_LAYER: unit
// =============================================================================
// FoliageHabitatRulesSaveLoadTest.cpp
//
// The save-game cell for issue #1254's species habitat rules, clumping and
// ground contact — the v35 band of SerializeFoliageLayer.
//
// Two things can go wrong here and both are silent. A field appended in the
// wrong ORDER desynchronises the fixed-order archive, corrupting every
// component after this one rather than just this one; and a v34-or-older save
// that acquired the new rules by default would MOVE every plant in every world
// that save ever produced, because DecorrelatedVariation changes the
// cell -> XZ mapping.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        /// Every #1254 field set to a distinctive, exactly-representable value,
        /// so a field read back from the wrong offset cannot coincidentally
        /// match the one it was supposed to be.
        [[nodiscard]] FoliageLayer MakeHabitatLayer()
        {
            FoliageLayer layer;
            layer.Name = "WoodlandFringe";
            layer.AlbedoPath = "assets/textures/grass.png";
            layer.SlopeFeather = 7.5f;
            layer.UseAltitudeBand = true;
            layer.MinAltitude = 12.25f;
            layer.MaxAltitude = 48.75f;
            layer.AltitudeFeather = 3.125f;
            layer.UseMoisture = true;
            layer.MinMoisture = 0.15625f;
            layer.MaxMoisture = 0.78125f;
            layer.MoistureFeather = 0.09375f;
            layer.ExclusionSplatmapChannel = 2;
            layer.ExclusionThreshold = 0.65625f;
            layer.ClumpStrength = 0.71875f;
            layer.ClumpScale = 23.5f;
            layer.ClumpFalloff = 1.375f;
            layer.ClumpScaleInfluence = 0.40625f;
            layer.ClumpGroup = 3;
            layer.GroundOffset = -0.09375f;
            layer.SlopeSinkFactor = 0.84375f;
            layer.DecorrelatedVariation = true;
            return layer;
        }

        [[nodiscard]] FoliageComponent RoundTrip(const FoliageComponent& seed, u32 readVersion)
        {
            std::vector<u8> buffer;
            {
                FMemoryWriter writer(buffer);
                writer.ArIsSaveGame = true;
                writer.SetArchiveVersion(kSaveGameFormatVersion);
                FoliageComponent copy = seed;
                SaveGameComponentSerializer::Serialize(writer, copy);
            }

            FoliageComponent loaded{};
            FMemoryReader reader(buffer);
            reader.ArIsSaveGame = true;
            reader.SetArchiveVersion(readVersion);
            SaveGameComponentSerializer::Serialize(reader, loaded);
            EXPECT_FALSE(reader.IsError());
            EXPECT_TRUE(reader.AtEnd())
                << "the reader did not consume exactly the payload — a field-order desync, which in a "
                   "fixed-order archive corrupts every component after this one, not just this one";
            return loaded;
        }

        // A FoliageComponent payload in the EXACTLY v34 layout: one layer, every
        // field SerializeFoliageLayer wrote through the v33 leaf band and
        // nothing after, then the component's trailing m_Enabled. (No foliage
        // field was added at v34 — that version belongs to GroomComponent — so
        // the v34 foliage layout IS the v33 one.)
        //
        // Hand-written because a v34 save can no longer be PRODUCED: saving
        // always emits the current layout, since HasFieldsSince short-circuits
        // on IsSaving. Writing at version 34 and reading at 34 therefore does
        // not test an old save at all; it desyncs, which is how this test first
        // failed. Mirror any change to SerializeFoliageLayer's pre-v35 field
        // ORDER here; if the two drift, this fails with a desync rather than
        // passing wrongly, which is the behaviour to want.
        [[nodiscard]] std::vector<u8> BuildV34Payload(const FoliageLayer& l, bool componentEnabled)
        {
            std::vector<u8> buffer;
            FMemoryWriter ar(buffer);
            ar.ArIsSaveGame = true;
            ar.SetArchiveVersion(34);

            u32 layerCount = 1;
            ar << layerCount;

            std::string name = l.Name;
            std::string meshPath = l.MeshPath;
            std::string albedoPath = l.AlbedoPath;
            ar << name << meshPath << albedoPath;
            f32 density = l.Density;
            ar << density;
            i32 splat = l.SplatmapChannel;
            ar << splat;
            f32 minSlope = l.MinSlopeAngle;
            f32 maxSlope = l.MaxSlopeAngle;
            ar << minSlope << maxSlope;
            f32 minScale = l.MinScale;
            f32 maxScale = l.MaxScale;
            ar << minScale << maxScale;
            f32 minHeight = l.MinHeight;
            f32 maxHeight = l.MaxHeight;
            ar << minHeight << maxHeight;
            bool randomRotation = l.RandomRotation;
            ar << randomRotation;
            f32 viewDistance = l.ViewDistance;
            f32 fadeStart = l.FadeStartDistance;
            ar << viewDistance << fadeStart;
            f32 windStrength = l.WindStrength;
            f32 windSpeed = l.WindSpeed;
            ar << windStrength << windSpeed;
            glm::vec3 baseColor = l.BaseColor;
            ar << baseColor;
            f32 roughness = l.Roughness;
            f32 alphaCutoff = l.AlphaCutoff;
            ar << roughness << alphaCutoff;
            bool enabled = l.Enabled;
            ar << enabled;

            // v11 impostor block
            bool useImpostor = l.UseImpostor;
            ar << useImpostor;
            f32 impostorStart = l.ImpostorStartDistance;
            f32 impostorBand = l.ImpostorTransitionBand;
            ar << impostorStart << impostorBand;
            u32 impostorFrames = l.ImpostorFramesPerAxis;
            u32 impostorRes = l.ImpostorAtlasResolution;
            ar << impostorFrames << impostorRes;
            bool impostorHemi = l.ImpostorHemiOctahedral;
            ar << impostorHemi;

            // v32 authored-mesh block
            bool useAuthoredMesh = l.UseAuthoredMesh;
            ar << useAuthoredMesh;
            f32 meshViewDistance = l.MeshViewDistance;
            f32 meshFadeStart = l.MeshFadeStartDistance;
            ar << meshViewDistance << meshFadeStart;

            // v33 leaf-material block — and then nothing, which is the point.
            std::string normalMap = l.NormalMapPath;
            std::string roughnessMap = l.RoughnessMapPath;
            std::string thicknessMap = l.ThicknessMapPath;
            ar << normalMap << roughnessMap << thicknessMap;
            f32 normalStrength = l.NormalStrength;
            ar << normalStrength;
            f32 transmissionStrength = l.TransmissionStrength;
            glm::vec3 transmissionColor = l.TransmissionColor;
            ar << transmissionStrength << transmissionColor;
            f32 thickness = l.Thickness;
            ar << thickness;
            f32 transmissionDistortion = l.TransmissionDistortion;
            f32 transmissionPower = l.TransmissionPower;
            ar << transmissionDistortion << transmissionPower;
            f32 transmissionWrap = l.TransmissionWrap;
            f32 transmissionAmbient = l.TransmissionAmbient;
            ar << transmissionWrap << transmissionAmbient;

            bool compEnabled = componentEnabled;
            ar << compEnabled;
            return buffer;
        }
    } // namespace

    TEST(FoliageHabitatRulesSaveLoad, EveryHabitatFieldSurvivesTheCurrentFormat)
    {
        FoliageComponent seed;
        seed.m_Enabled = true;
        seed.m_Layers.push_back(MakeHabitatLayer());

        const FoliageComponent loaded = RoundTrip(seed, kSaveGameFormatVersion);
        ASSERT_EQ(loaded.m_Layers.size(), 1u);
        const FoliageLayer& got = loaded.m_Layers[0];
        const FoliageLayer& want = seed.m_Layers[0];

        EXPECT_TRUE(Math::BitwiseEqual(got.SlopeFeather, want.SlopeFeather));
        EXPECT_EQ(got.UseAltitudeBand, want.UseAltitudeBand);
        EXPECT_TRUE(Math::BitwiseEqual(got.MinAltitude, want.MinAltitude));
        EXPECT_TRUE(Math::BitwiseEqual(got.MaxAltitude, want.MaxAltitude));
        EXPECT_TRUE(Math::BitwiseEqual(got.AltitudeFeather, want.AltitudeFeather));
        EXPECT_EQ(got.UseMoisture, want.UseMoisture);
        EXPECT_TRUE(Math::BitwiseEqual(got.MinMoisture, want.MinMoisture));
        EXPECT_TRUE(Math::BitwiseEqual(got.MaxMoisture, want.MaxMoisture));
        EXPECT_TRUE(Math::BitwiseEqual(got.MoistureFeather, want.MoistureFeather));
        EXPECT_EQ(got.ExclusionSplatmapChannel, want.ExclusionSplatmapChannel);
        EXPECT_TRUE(Math::BitwiseEqual(got.ExclusionThreshold, want.ExclusionThreshold));
        EXPECT_TRUE(Math::BitwiseEqual(got.ClumpStrength, want.ClumpStrength));
        EXPECT_TRUE(Math::BitwiseEqual(got.ClumpScale, want.ClumpScale));
        EXPECT_TRUE(Math::BitwiseEqual(got.ClumpFalloff, want.ClumpFalloff));
        EXPECT_TRUE(Math::BitwiseEqual(got.ClumpScaleInfluence, want.ClumpScaleInfluence));
        EXPECT_EQ(got.ClumpGroup, want.ClumpGroup);
        EXPECT_TRUE(Math::BitwiseEqual(got.GroundOffset, want.GroundOffset));
        EXPECT_TRUE(Math::BitwiseEqual(got.SlopeSinkFactor, want.SlopeSinkFactor));
        EXPECT_EQ(got.DecorrelatedVariation, want.DecorrelatedVariation);

        // operator== is the editor-undo path, and a field missing from it means
        // undo silently skips that value. A full round-trip must compare equal.
        EXPECT_TRUE(got == want) << "a #1254 field round-tripped but is missing from FoliageLayer::operator==, "
                                    "so editor undo will not see it change";
    }

    TEST(FoliageHabitatRulesSaveLoad, AnOlderSaveKeepsEveryRuleOff)
    {
        // The conservative default a pre-#1254 save is entitled to. This matters
        // more than usual: DecorrelatedVariation changes the cell -> XZ mapping,
        // so defaulting it the other way would move every plant in every world
        // an older save ever produced.
        FoliageComponent seed;
        seed.m_Enabled = true;
        seed.m_Layers.push_back(MakeHabitatLayer());

        const std::vector<u8> buffer = BuildV34Payload(seed.m_Layers[0], /*componentEnabled=*/true);

        FoliageComponent loaded{};
        FMemoryReader reader(buffer);
        reader.ArIsSaveGame = true;
        reader.SetArchiveVersion(34);
        SaveGameComponentSerializer::Serialize(reader, loaded);
        ASSERT_FALSE(reader.IsError());
        EXPECT_TRUE(reader.AtEnd()) << "a v34 reader did not consume exactly the v34 payload";

        ASSERT_EQ(loaded.m_Layers.size(), 1u);
        const FoliageLayer& got = loaded.m_Layers[0];
        const FoliageLayer defaults;

        EXPECT_TRUE(Math::BitwiseEqual(got.SlopeFeather, defaults.SlopeFeather));
        EXPECT_EQ(got.UseAltitudeBand, defaults.UseAltitudeBand);
        EXPECT_EQ(got.UseMoisture, defaults.UseMoisture);
        EXPECT_EQ(got.ExclusionSplatmapChannel, defaults.ExclusionSplatmapChannel);
        EXPECT_TRUE(Math::BitwiseEqual(got.ClumpStrength, defaults.ClumpStrength));
        EXPECT_TRUE(Math::BitwiseEqual(got.ClumpScaleInfluence, defaults.ClumpScaleInfluence));
        EXPECT_EQ(got.ClumpGroup, defaults.ClumpGroup);
        EXPECT_TRUE(Math::BitwiseEqual(got.GroundOffset, defaults.GroundOffset));
        EXPECT_TRUE(Math::BitwiseEqual(got.SlopeSinkFactor, defaults.SlopeSinkFactor));
        EXPECT_FALSE(got.DecorrelatedVariation)
            << "an older save acquired the new placement hash, which moves every plant it ever had";

        // The fields that DID round-trip before v35 still do.
        EXPECT_EQ(got.Name, seed.m_Layers[0].Name);
        EXPECT_EQ(got.AlbedoPath, seed.m_Layers[0].AlbedoPath);
    }

    TEST(FoliageHabitatRulesSaveLoad, ACorruptSaveIsClampedRatherThanTrusted)
    {
        // A save file is no more trusted than a .olo: every float here reaches
        // the placement generator, where a NaN band silently empties the layer
        // and a zero clump scale is a division by zero.
        FoliageComponent seed;
        seed.m_Enabled = true;
        FoliageLayer hostile = MakeHabitatLayer();
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        hostile.SlopeFeather = nan;
        hostile.MinMoisture = nan;
        hostile.ClumpScale = 0.0f;
        hostile.ClumpFalloff = nan;
        hostile.SlopeSinkFactor = 1.0e9f;
        hostile.AltitudeFeather = -5.0f;
        seed.m_Layers.push_back(hostile);

        const FoliageComponent loaded = RoundTrip(seed, kSaveGameFormatVersion);
        ASSERT_EQ(loaded.m_Layers.size(), 1u);
        const FoliageLayer& got = loaded.m_Layers[0];

        EXPECT_TRUE(std::isfinite(got.SlopeFeather));
        EXPECT_TRUE(std::isfinite(got.MinMoisture));
        EXPECT_TRUE(std::isfinite(got.ClumpFalloff));
        EXPECT_GE(got.ClumpScale, 0.01f) << "a zero clump scale is a division by zero in ClumpField";
        EXPECT_GE(got.AltitudeFeather, 0.0f);
        EXPECT_LE(got.SlopeSinkFactor, 4.0f);
    }
} // namespace OloEngine::Tests
