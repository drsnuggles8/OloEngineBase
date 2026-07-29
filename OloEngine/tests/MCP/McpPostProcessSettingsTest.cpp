#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpPostProcessSettingsTest — unit test (headless, no GL, no live editor).
//
// Pins the shared core behind olo_postprocess_settings_get / _set (issue #607's
// post-process-settings capability gap): the post-process / AO / fog block that
// olo_renderer_settings_set never reached. The motivating case is
// `ActiveAOTechnique` — the same-scene, same-pose GTAO-vs-SSAO A/B that could
// not be driven from an agent session at all, because the field is not
// scene-serialised.
//
// What is asserted here (MCP/McpPostProcessSettings.h, header-only — no editor
// TU is pulled in, matching McpRendererSettingsTest's split):
//   * the field table is well-formed: every entry has the accessor pair its type
//     needs, unique tokens, and a sane range — a malformed row would otherwise
//     surface as a null-pointer call inside a live editor;
//   * token lookup is case- and separator-insensitive, and an unknown token
//     produces suggestions rather than a bare "unknown";
//   * Apply coerces + CLAMPS numerics, rejects the wrong JSON type and non-finite
//     floats, round-trips enums by token, and reports previousValue/restoreWith
//     so a session can put the renderer back (restore-prior-value, not undo);
//   * `ActiveAOTechnique` is flagged RequiresRendererApply — forgetting that flag
//     means the AO pass is never re-registered and the write silently does
//     nothing visible (issue #533);
//   * `Tonemap` / `Upscale` are deliberately ABSENT: olo_renderer_settings_set
//     owns them, and two write paths onto one field is exactly the divergence
//     this split exists to prevent.
// =============================================================================

#include "MCP/McpPostProcessSettings.h"

#include "OloEngine/Renderer/PostProcessSettings.h"

#include <cmath>
#include <limits>
#include <set>
#include <string>

// OLO_TEST_LAYER: unit

namespace
{
    namespace PP = OloEngine::MCP::PostProcess;
    using OloEngine::AOTechnique;
    using OloEngine::FogMode;
    using OloEngine::FogSettings;
    using OloEngine::PostProcessSettings;
    using Json = nlohmann::json;

    // Apply one field by token against a fresh pair of PODs.
    PP::ApplyResult ApplyToken(std::string_view token, const Json& value,
                               PostProcessSettings& pp, FogSettings& fog)
    {
        const PP::FieldInfo* field = PP::FindField(token);
        EXPECT_NE(field, nullptr) << "unknown field token: " << token;
        if (field == nullptr)
            return {};
        return PP::Apply(*field, value, pp, fog);
    }
} // namespace

// ---- table integrity --------------------------------------------------------

TEST(McpPostProcessSettings, FieldTableIsWellFormed)
{
    std::set<std::string> normalizedTokens;
    for (const auto& field : PP::kFields)
    {
        const std::string token(field.Token);
        EXPECT_FALSE(token.empty());
        EXPECT_FALSE(field.Description.empty()) << token << " has no description";
        EXPECT_FALSE(field.Group.empty()) << token << " has no group";

        // Tokens must be unique AFTER normalization — two rows that only differ
        // by case/separator would make FindField silently pick the first.
        const std::string key = PP::Normalize(field.Token);
        EXPECT_TRUE(normalizedTokens.insert(key).second) << "duplicate token: " << token;

        // The accessor pair must match the type, or a live call dereferences null.
        if (field.Type == PP::FieldType::Vec3)
        {
            EXPECT_NE(field.GetV3, nullptr) << token;
            EXPECT_NE(field.SetV3, nullptr) << token;
        }
        else
        {
            EXPECT_NE(field.Get, nullptr) << token;
            EXPECT_NE(field.Set, nullptr) << token;
        }

        if (field.Type == PP::FieldType::Enum)
            EXPECT_FALSE(field.Values.empty()) << token << " is an enum with no value table";
        else
            EXPECT_TRUE(field.Values.empty()) << token << " is not an enum but carries values";

        // A numeric range must be orderable and finite — a reversed one makes
        // std::clamp undefined behaviour.
        if (field.Type == PP::FieldType::Float || field.Type == PP::FieldType::Int ||
            field.Type == PP::FieldType::Vec3)
        {
            EXPECT_TRUE(std::isfinite(field.Min)) << token;
            EXPECT_TRUE(std::isfinite(field.Max)) << token;
            EXPECT_LT(field.Min, field.Max) << token << " has a non-increasing range";
        }
    }
    EXPECT_GT(PP::kFields.size(), 80u) << "the table should cover the whole post-process + fog surface";
}

TEST(McpPostProcessSettings, ToneMapAndUpscaleStayOnTheSiblingTool)
{
    // olo_renderer_settings_set owns these. Exposing them here too would give one
    // field two write paths — the divergence class this split prevents.
    EXPECT_EQ(PP::FindField("Tonemap"), nullptr);
    EXPECT_EQ(PP::FindField("Upscale"), nullptr);
    // ...but its neighbours in the same struct ARE reachable.
    EXPECT_NE(PP::FindField("Exposure"), nullptr);
    EXPECT_NE(PP::FindField("RCASSharpness"), nullptr);
}

TEST(McpPostProcessSettings, TokenLookupIgnoresCaseAndSeparators)
{
    const PP::FieldInfo* canonical = PP::FindField("GTAORadius");
    ASSERT_NE(canonical, nullptr);
    EXPECT_EQ(PP::FindField("gtaoradius"), canonical);
    EXPECT_EQ(PP::FindField("gtao_radius"), canonical);
    EXPECT_EQ(PP::FindField("gtao.radius"), canonical);
    EXPECT_EQ(PP::FindField("GTAO Radius"), canonical);
    EXPECT_EQ(PP::FindField(""), nullptr);
}

TEST(McpPostProcessSettings, UnknownTokenSuggestsNeighbours)
{
    // The catalogue is 100+ fields, so an error can't just list them all — a
    // near-miss must come back with candidates.
    const std::vector<std::string> suggestions = PP::SuggestFields("gtao");
    EXPECT_FALSE(suggestions.empty());
    bool sawRadius = false;
    for (const auto& s : suggestions)
        sawRadius = sawRadius || s == "GTAORadius";
    EXPECT_TRUE(sawRadius);
}

// ---- apply / coercion -------------------------------------------------------

TEST(McpPostProcessSettings, AppliesBoolAndReportsPriorValue)
{
    PostProcessSettings pp;
    FogSettings fog;
    pp.GTAODebugView = false;

    const PP::ApplyResult result = ApplyToken("GTAODebugView", Json(true), pp, fog);
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_TRUE(pp.GTAODebugView);
    EXPECT_EQ(result.Data["previousValue"], Json(false));
    EXPECT_EQ(result.Data["value"], Json(true));
    EXPECT_EQ(result.Data["changed"], Json(true));
    // Restore-prior-value: calling again with restoreWith must undo it.
    const PP::ApplyResult restored = ApplyToken("GTAODebugView", result.Data["restoreWith"], pp, fog);
    ASSERT_TRUE(restored.Ok);
    EXPECT_FALSE(pp.GTAODebugView);
}

TEST(McpPostProcessSettings, ClampsNumericToDeclaredRange)
{
    PostProcessSettings pp;
    FogSettings fog;

    // SSRMaxSteps' upper bound mirrors kSSRMaxSteps, the runtime UBO clamp — a
    // write above it would be stored but silently ignored when rendering.
    const PP::ApplyResult high = ApplyToken("SSRMaxSteps", Json(100000), pp, fog);
    ASSERT_TRUE(high.Ok) << high.Error;
    EXPECT_EQ(pp.SSRMaxSteps, OloEngine::kSSRMaxSteps);
    EXPECT_EQ(high.Data["clamped"], Json(true));
    EXPECT_TRUE(high.Data.contains("range"));

    const PP::ApplyResult low = ApplyToken("SSRMaxSteps", Json(-5), pp, fog);
    ASSERT_TRUE(low.Ok) << low.Error;
    EXPECT_EQ(pp.SSRMaxSteps, 1);

    // In-range writes are not flagged as clamped.
    const PP::ApplyResult ok = ApplyToken("SSRMaxSteps", Json(32), pp, fog);
    ASSERT_TRUE(ok.Ok);
    EXPECT_EQ(pp.SSRMaxSteps, 32);
    EXPECT_EQ(ok.Data["clamped"], Json(false));
}

TEST(McpPostProcessSettings, IntFieldRoundsRatherThanTruncates)
{
    PostProcessSettings pp;
    FogSettings fog;
    ASSERT_TRUE(ApplyToken("BloomIterations", Json(4.6), pp, fog).Ok);
    EXPECT_EQ(pp.BloomIterations, 5);
}

TEST(McpPostProcessSettings, RejectsNonFiniteAndWrongTypes)
{
    PostProcessSettings pp;
    FogSettings fog;
    const f32 before = pp.GTAORadius;

    const PP::FieldInfo* radius = PP::FindField("GTAORadius");
    ASSERT_NE(radius, nullptr);

    // NaN/Inf must never reach the settings — the CLAUDE.md rule for any float
    // arriving from external data.
    Json nan = Json(std::numeric_limits<f64>::quiet_NaN());
    EXPECT_FALSE(PP::Apply(*radius, nan, pp, fog).Ok);
    EXPECT_FLOAT_EQ(pp.GTAORadius, before);

    EXPECT_FALSE(PP::Apply(*radius, Json("half a metre"), pp, fog).Ok);
    EXPECT_FLOAT_EQ(pp.GTAORadius, before);

    // A boolean is a number in JSON's loose sense but never a valid scalar here.
    EXPECT_FALSE(PP::Apply(*radius, Json(true), pp, fog).Ok);
    EXPECT_FLOAT_EQ(pp.GTAORadius, before);

    // ...and the mirror: a numeric value on a boolean field.
    const PP::FieldInfo* flag = PP::FindField("BloomEnabled");
    ASSERT_NE(flag, nullptr);
    EXPECT_FALSE(PP::Apply(*flag, Json(1), pp, fog).Ok);
}

TEST(McpPostProcessSettings, EnumRoundTripsByTokenAndFlagsRendererApply)
{
    PostProcessSettings pp;
    FogSettings fog;
    pp.ActiveAOTechnique = AOTechnique::SSAO;

    const PP::ApplyResult toGtao = ApplyToken("ActiveAOTechnique", Json("gtao"), pp, fog);
    ASSERT_TRUE(toGtao.Ok) << toGtao.Error;
    EXPECT_EQ(pp.ActiveAOTechnique, AOTechnique::GTAO);
    EXPECT_EQ(toGtao.Data["previousValue"], Json("ssao"));
    EXPECT_EQ(toGtao.Data["value"], Json("gtao"));
    // Without this flag the handler never calls Renderer3D::ApplyRendererSettings,
    // the AO pass is not re-registered, and the write renders as a no-op (#533).
    EXPECT_TRUE(toGtao.RequiresRendererApply);

    // Writing the same value again is not a change, so no rebuild is requested.
    const PP::ApplyResult again = ApplyToken("ActiveAOTechnique", Json("GTAO"), pp, fog);
    ASSERT_TRUE(again.Ok);
    EXPECT_FALSE(again.RequiresRendererApply);
    EXPECT_EQ(again.Data["changed"], Json(false));

    // An unknown token is refused and lists the valid ones.
    const PP::FieldInfo* technique = PP::FindField("ActiveAOTechnique");
    ASSERT_NE(technique, nullptr);
    const PP::ApplyResult bad = PP::Apply(*technique, Json("hbao"), pp, fog);
    EXPECT_FALSE(bad.Ok);
    EXPECT_NE(bad.Error.find("gtao"), std::string::npos);
    EXPECT_EQ(pp.ActiveAOTechnique, AOTechnique::GTAO);
}

TEST(McpPostProcessSettings, FogFieldsReachTheFogStructAndAcceptVec3)
{
    PostProcessSettings pp;
    FogSettings fog;

    ASSERT_TRUE(ApplyToken("FogDensity", Json(0.5), pp, fog).Ok);
    EXPECT_FLOAT_EQ(fog.Density, 0.5f);

    ASSERT_TRUE(ApplyToken("FogMode", Json("linear"), pp, fog).Ok);
    EXPECT_EQ(fog.Mode, FogMode::Linear);

    const PP::ApplyResult colour = ApplyToken("FogColor", Json::array({ 0.25, 0.5, 0.75 }), pp, fog);
    ASSERT_TRUE(colour.Ok) << colour.Error;
    EXPECT_FLOAT_EQ(fog.Color.r, 0.25f);
    EXPECT_FLOAT_EQ(fog.Color.g, 0.5f);
    EXPECT_FLOAT_EQ(fog.Color.b, 0.75f);

    // Out-of-range components clamp, not error — a colour is continuous.
    const PP::ApplyResult clamped = ApplyToken("FogColor", Json::array({ -1.0, 2.0, 0.5 }), pp, fog);
    ASSERT_TRUE(clamped.Ok);
    EXPECT_FLOAT_EQ(fog.Color.r, 0.0f);
    EXPECT_FLOAT_EQ(fog.Color.g, 1.0f);
    EXPECT_EQ(clamped.Data["clamped"], Json(true));

    // Wrong arity / a non-finite element leaves the colour untouched.
    const PP::FieldInfo* fogColour = PP::FindField("FogColor");
    ASSERT_NE(fogColour, nullptr);
    EXPECT_FALSE(PP::Apply(*fogColour, Json::array({ 0.1, 0.2 }), pp, fog).Ok);
    EXPECT_FALSE(PP::Apply(*fogColour, Json::array({ 0.1, std::numeric_limits<f64>::infinity(), 0.2 }), pp, fog).Ok);
    EXPECT_FLOAT_EQ(fogColour->GetV3(pp, fog).g, 1.0f);
}

// ---- argument parsing / introspection ---------------------------------------

TEST(McpPostProcessSettings, ParseSetArgsIntrospectsWhenFieldOmitted)
{
    bool introspect = false;
    const PP::FieldInfo* field = nullptr;
    Json value;

    EXPECT_FALSE(PP::ParseSetArgs(Json::object(), introspect, field, value).has_value());
    EXPECT_TRUE(introspect);

    // A value without a field is a mistake, not a silent listing.
    EXPECT_TRUE(PP::ParseSetArgs(Json{ { "value", 1.0 } }, introspect, field, value).has_value());

    // A field without a value names the expected type.
    const auto missingValue = PP::ParseSetArgs(Json{ { "field", "GTAORadius" } }, introspect, field, value);
    ASSERT_TRUE(missingValue.has_value());
    EXPECT_NE(missingValue->find("GTAORadius"), std::string::npos);

    // An unknown field suggests neighbours and points at the listing.
    const auto unknown = PP::ParseSetArgs(Json{ { "field", "gtaoradiuss" }, { "value", 1.0 } }, introspect, field, value);
    ASSERT_TRUE(unknown.has_value());
    EXPECT_NE(unknown->find("GTAORadius"), std::string::npos);

    // The happy path resolves the field and hands the raw value through.
    ASSERT_FALSE(PP::ParseSetArgs(Json{ { "field", "gtao.radius" }, { "value", 2.0 } }, introspect, field, value).has_value());
    EXPECT_FALSE(introspect);
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->Token, "GTAORadius");
    EXPECT_EQ(value, Json(2.0));
}

TEST(McpPostProcessSettings, DescribeListsLiveValuesAndFiltersByGroup)
{
    PostProcessSettings pp;
    FogSettings fog;
    pp.GTAORadius = 1.25f;

    bool unknownGroup = true;
    const Json all = PP::Describe(pp, fog, {}, unknownGroup);
    EXPECT_FALSE(unknownGroup);
    EXPECT_EQ(all["fields"].size(), PP::kFields.size());
    EXPECT_FALSE(all["groups"].empty());

    bool found = false;
    for (const auto& entry : all["fields"])
    {
        if (entry["field"] == "GTAORadius")
        {
            found = true;
            EXPECT_DOUBLE_EQ(entry["value"].get<f64>(), 1.25);
            EXPECT_TRUE(entry.contains("min"));
            EXPECT_TRUE(entry.contains("max"));
        }
    }
    EXPECT_TRUE(found);

    const Json aoOnly = PP::Describe(pp, fog, "ao", unknownGroup);
    EXPECT_FALSE(unknownGroup);
    EXPECT_FALSE(aoOnly["fields"].empty());
    EXPECT_LT(aoOnly["fields"].size(), all["fields"].size());
    for (const auto& entry : aoOnly["fields"])
        EXPECT_EQ(entry["group"], "ao");

    // An unrecognised group is reported rather than returning an empty list that
    // reads as "this group has no settings".
    (void)PP::Describe(pp, fog, "nosuchgroup", unknownGroup);
    EXPECT_TRUE(unknownGroup);
}

TEST(McpPostProcessSettings, SchemasAreClosedObjects)
{
    for (const Json& schema : { PP::GetInputSchema(), PP::SetInputSchema() })
    {
        EXPECT_EQ(schema["type"], "object");
        EXPECT_EQ(schema["additionalProperties"], false);
        EXPECT_TRUE(schema["properties"].contains("field"));
    }
    EXPECT_TRUE(PP::SetInputSchema()["properties"].contains("value"));
    EXPECT_TRUE(PP::GetInputSchema()["properties"].contains("group"));
}
