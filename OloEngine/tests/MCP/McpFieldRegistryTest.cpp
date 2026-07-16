#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpFieldRegistryTest — unit test (headless, no GL, no live editor).
//
// Pins the GENERATED MCP writable-field registry (issue #607). The registry used
// to be a hand-written list of 9 components; OloHeaderTool now emits it from the
// same component data-member scan that drives the scene serializer
// (MCP/Generated/McpFieldRegistry.Generated.inl, #include'd by BuildRegistry()).
//
// What this file guards, and why each guard exists:
//
//   1. BREADTH — the registry actually reaches the components an agent debugging
//      rendering / physics / gameplay reaches for. The gap that motivated #607 was
//      `olo_entity_set_field` refusing VirtualMeshComponent (and MeshComponent, and
//      the physics bodies, and …) with "Unknown or non-editable component", which
//      pushed a live debugging session into hand-editing scene YAML.
//
//   2. RUNTIME-ONLY EXCLUSION — the per-tick *StateComponent family,
//      UIResolvedRectComponent, WorldTransformComponent and IDComponent must stay
//      unwritable (a write is meaningless or corrupting), and the refusal must name
//      the alternatives.
//
//   3. CLAMPS — a field whose scene-load path clamps out-of-range values clamps
//      here too, so MCP can never produce a component state a scene load couldn't.
//
//   4. HONESTY — the write result echoes the value READ BACK from the live
//      component (not the value we intended to write) and an explicit `changed`
//      flag, so a caller can tell "the call returned 200" from "the write landed".
//      A no-op write reports changed:false and pushes no undo command.
//
// The registry itself is header-only (renderer/httplib-free), so this test links
// no extra editor TU — same discipline as McpGenericFieldWriteTest.cpp.
// =============================================================================

#include "MCP/McpGenericFieldWrite.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <string>
#include <vector>

// OLO_TEST_LAYER: unit

namespace
{
    using OloEngine::CommandHistory;
    using OloEngine::Entity;
    using OloEngine::PointLightComponent;
    using OloEngine::Ref;
    using OloEngine::Scene;
    using OloEngine::SphereAreaLightComponent;
    using OloEngine::TextComponent;
    using OloEngine::UUID;
    using OloEngine::VirtualMeshComponent;
    namespace GFW = OloEngine::MCP::GenericFieldWrite;
    using Json = GFW::Json;

    [[nodiscard]] bool RegistryHas(const std::string& component, const std::string& field)
    {
        return GFW::Find(component, field) != nullptr;
    }

    [[nodiscard]] bool RegistryHasComponent(const std::string& component)
    {
        const std::vector<std::string> components = GFW::EditableComponents();
        return std::find(components.begin(), components.end(), component) != components.end();
    }

    // A scene + one entity, with the caller's component attached.
    struct Fixture
    {
        Ref<Scene> Scene_;
        Entity TheEntity;
        u64 Uuid = 0;
        CommandHistory History;

        Fixture()
        {
            Scene_ = Ref<Scene>::Create();
            TheEntity = Scene_->CreateEntity("Subject");
            Uuid = static_cast<u64>(TheEntity.GetUUID());
        }
    };
} // namespace

// ---- 1. breadth: the registry covers the real component surface -------------

// The gap that opened #607: VirtualMeshComponent (the Nanite virtualized-geometry
// component) was not writable at all, so forcing a DAG LOD level meant editing
// scene YAML by hand and reloading.
TEST(McpFieldRegistry, CoversVirtualMeshComponent)
{
    EXPECT_TRUE(RegistryHas("VirtualMeshComponent", "Enabled"));
    EXPECT_TRUE(RegistryHas("VirtualMeshComponent", "ErrorThresholdPixels"));
    EXPECT_TRUE(RegistryHas("VirtualMeshComponent", "MeshSource"));
    EXPECT_TRUE(RegistryHas("VirtualMeshComponent", "CastShadows"));
}

// A representative slice of what an agent reaches for while debugging. This is a
// rot guard, not an exhaustive list: if one of these disappears, the generator (or
// the component) changed in a way that silently shrank the MCP write surface.
TEST(McpFieldRegistry, CoversTheComponentsAnAgentDebugsWith)
{
    for (const char* component : { "TransformComponent", "TagComponent", "CameraComponent",
                                   "MeshComponent", "ModelComponent", "MaterialComponent",
                                   "VirtualMeshComponent", "TextComponent",
                                   "DirectionalLightComponent", "PointLightComponent", "SpotLightComponent",
                                   "SphereAreaLightComponent", "FogVolumeComponent", "LightProbeVolumeComponent",
                                   "ReflectionProbeComponent", "DecalComponent", "EnvironmentMapComponent",
                                   "Rigidbody2DComponent", "Rigidbody3DComponent",
                                   "BoxCollider2DComponent", "BoxCollider3DComponent", "SphereCollider3DComponent",
                                   "WaterComponent", "TerrainComponent", "NavAgentComponent" })
    {
        EXPECT_TRUE(RegistryHasComponent(component)) << component << " should be MCP-writable";
    }
}

// The generated registry is a strict superset of the old hand-written 9-component
// list (nothing that used to be writable stopped being writable).
TEST(McpFieldRegistry, KeepsEveryPreviouslyHandWrittenField)
{
    EXPECT_TRUE(RegistryHas("TransformComponent", "Translation"));
    EXPECT_TRUE(RegistryHas("TransformComponent", "Scale"));
    EXPECT_TRUE(RegistryHas("TagComponent", "Tag"));
    EXPECT_TRUE(RegistryHas("SpriteRendererComponent", "Color"));
    EXPECT_TRUE(RegistryHas("SpriteRendererComponent", "TilingFactor"));
    EXPECT_TRUE(RegistryHas("CircleRendererComponent", "Thickness"));
    EXPECT_TRUE(RegistryHas("CameraComponent", "Primary"));
    EXPECT_TRUE(RegistryHas("DirectionalLightComponent", "Intensity"));
    EXPECT_TRUE(RegistryHas("PointLightComponent", "Range"));
    EXPECT_TRUE(RegistryHas("SpotLightComponent", "OuterCutoff"));
    EXPECT_TRUE(RegistryHas("SphereAreaLightComponent", "Radius"));

    // …and it is much bigger than that hand-written list ever was.
    EXPECT_GT(GFW::EditableComponents().size(), 50u);
}

// TransformComponent::Rotation is a private euler/quat pair behind setters — the
// field scan never collects a non-public member, so it stays unwritable (rather
// than the generator emitting `&TransformComponent::Rotation`, which would not
// even compile).
TEST(McpFieldRegistry, DoesNotExposeNonPublicFields)
{
    EXPECT_FALSE(RegistryHas("TransformComponent", "Rotation"));
}

// ---- 2. runtime-only components / fields stay unwritable ---------------------

TEST(McpFieldRegistry, RefusesRuntimeOnlyComponents)
{
    for (const char* component : { "IDComponent", "UIResolvedRectComponent", "WorldTransformComponent",
                                   "AnimationStateComponent", "DialogueStateComponent",
                                   "SpringBoneStateComponent", "NoiseAnimationStateComponent",
                                   "RetargetingStateComponent", "FootIKStateComponent",
                                   "LocomotionStateComponent" })
    {
        EXPECT_FALSE(RegistryHasComponent(component)) << component << " is per-tick/identity state — must not be MCP-writable";
    }
}

// A write to a runtime-only component is REFUSED, and the error names the valid
// alternatives (an agent can self-correct without a round-trip).
TEST(McpFieldRegistry, RuntimeOnlyComponentWriteIsRefusedWithGuidance)
{
    Fixture f;
    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "SpringBoneStateComponent", "Anything", Json(1.0));
    EXPECT_FALSE(result.Ok);
    EXPECT_NE(result.Error.find("Unknown or non-editable component"), std::string::npos);
    EXPECT_NE(result.Error.find("Editable components"), std::string::npos);
    EXPECT_FALSE(f.History.CanUndo());
}

// NavAgentComponent's runtime pathfinder state carries OLO_SERIALIZE(Skip), so the
// shared field scan drops it — it is neither serialized nor MCP-writable — while
// the component's authored params ARE writable.
TEST(McpFieldRegistry, SkipAnnotatedRuntimeFieldsAreNotWritable)
{
    EXPECT_TRUE(RegistryHas("NavAgentComponent", "MaxSpeed"));   // authored param
    EXPECT_FALSE(RegistryHas("NavAgentComponent", "HasPath"));   // OLO_SERIALIZE(Skip)
    EXPECT_FALSE(RegistryHas("NavAgentComponent", "HasTarget")); // OLO_SERIALIZE(Skip)
}

// ---- 3. clamps mirror the scene serializer's load-time range -----------------

// VirtualMeshComponent::m_ErrorThresholdPixels is OLO_SERIALIZE(Clamp, Min=0.05f,
// Max=64.0f) — the generated registry carries the same range, so a write above the
// max is CLAMPED (and says so) instead of putting the component into a state a
// scene load could never produce.
TEST(McpFieldRegistry, ClampsAnnotatedRangeAndReportsIt)
{
    Fixture f;
    f.TheEntity.AddComponent<VirtualMeshComponent>();

    const auto high = GFW::Apply(f.Scene_, f.History, f.Uuid, "VirtualMeshComponent", "ErrorThresholdPixels", Json(1000.0));
    ASSERT_TRUE(high.Ok) << high.Error;
    EXPECT_TRUE(high.Data["clamped"].get<bool>());
    EXPECT_TRUE(high.Data["changed"].get<bool>());
    EXPECT_DOUBLE_EQ(high.Data["requestedValue"].get<double>(), 1000.0);
    EXPECT_FLOAT_EQ(high.Data["value"].get<f32>(), 64.0f);
    EXPECT_FLOAT_EQ(f.TheEntity.GetComponent<VirtualMeshComponent>().m_ErrorThresholdPixels, 64.0f);

    const auto low = GFW::Apply(f.Scene_, f.History, f.Uuid, "VirtualMeshComponent", "ErrorThresholdPixels", Json(-5.0));
    ASSERT_TRUE(low.Ok) << low.Error;
    EXPECT_TRUE(low.Data["clamped"].get<bool>());
    EXPECT_FLOAT_EQ(low.Data["value"].get<f32>(), 0.05f);
    EXPECT_FLOAT_EQ(f.TheEntity.GetComponent<VirtualMeshComponent>().m_ErrorThresholdPixels, 0.05f);
}

// An in-range write is NOT reported as clamped.
TEST(McpFieldRegistry, InRangeWriteIsNotClamped)
{
    Fixture f;
    f.TheEntity.AddComponent<VirtualMeshComponent>();

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "VirtualMeshComponent", "ErrorThresholdPixels", Json(4.0));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_FALSE(result.Data["clamped"].get<bool>());
    EXPECT_TRUE(result.Data["changed"].get<bool>());
    EXPECT_FLOAT_EQ(f.TheEntity.GetComponent<VirtualMeshComponent>().m_ErrorThresholdPixels, 4.0f);
}

// SphereAreaLightComponent's HAND-WRITTEN deserialize rejects a negative radius /
// intensity / range (keeping the default). That range is re-declared in the
// generator's kMcpFieldClamps table, so MCP clamps to the same floor.
TEST(McpFieldRegistry, ClampsHandWrittenSerializerRange)
{
    Fixture f;
    f.TheEntity.AddComponent<SphereAreaLightComponent>();

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "SphereAreaLightComponent", "Radius", Json(-3.0));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_TRUE(result.Data["clamped"].get<bool>());
    EXPECT_FLOAT_EQ(result.Data["value"].get<f32>(), 0.0f);
    EXPECT_FLOAT_EQ(f.TheEntity.GetComponent<SphereAreaLightComponent>().m_Radius, 0.0f);
}

// The range is discoverable BEFORE writing: olo_entity_list_fields reports min/max
// for every ranged field (and omits them for an unranged one).
TEST(McpFieldRegistry, ListFieldsReportsTheRange)
{
    Fixture f;
    f.TheEntity.AddComponent<VirtualMeshComponent>();

    bool found = false;
    const Json out = GFW::ListFields(f.Scene_, f.Uuid, "VirtualMeshComponent", found);
    ASSERT_TRUE(found);
    ASSERT_EQ(out["components"].size(), 1u);

    bool sawRangedField = false;
    for (const auto& field : out["components"][0]["fields"])
    {
        if (field["field"] == "ErrorThresholdPixels")
        {
            sawRangedField = true;
            ASSERT_TRUE(field.contains("min"));
            ASSERT_TRUE(field.contains("max"));
            EXPECT_NEAR(field["min"].get<double>(), 0.05, 1e-6);
            EXPECT_NEAR(field["max"].get<double>(), 64.0, 1e-6);
        }
        if (field["field"] == "CastShadows")
        {
            EXPECT_FALSE(field.contains("min")); // a bool has no range
            EXPECT_FALSE(field.contains("max"));
        }
    }
    EXPECT_TRUE(sawRangedField);
}

// ---- 4. honesty: read-back value + explicit changed flag ---------------------

// The reported `value` is read back OUT of the live component after the command
// ran — not the value we intended to write. A caller can therefore verify the
// write landed instead of trusting that the call returned successfully.
TEST(McpFieldRegistry, ReportedValueIsReadBackFromTheComponent)
{
    Fixture f;
    f.TheEntity.AddComponent<PointLightComponent>();

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "PointLightComponent", "Range", Json(12.5));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_TRUE(result.Data["changed"].get<bool>());
    EXPECT_TRUE(result.Data["undoable"].get<bool>());

    const f32 live = f.TheEntity.GetComponent<PointLightComponent>().m_Range;
    EXPECT_FLOAT_EQ(live, 12.5f);
    EXPECT_FLOAT_EQ(result.Data["value"].get<f32>(), live); // echoes the LIVE value
}

// Writing a field the value it already has is an explicit no-op: changed:false,
// undoable:false, nothing on the undo stack — never a silent "success" that an
// agent could mistake for a real change.
TEST(McpFieldRegistry, NoOpWriteReportsChangedFalse)
{
    Fixture f;
    auto& vm = f.TheEntity.AddComponent<VirtualMeshComponent>();
    vm.m_ErrorThresholdPixels = 2.0f;

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "VirtualMeshComponent", "ErrorThresholdPixels", Json(2.0));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_FALSE(result.Data["changed"].get<bool>());
    EXPECT_FALSE(result.Data["undoable"].get<bool>());
    EXPECT_FALSE(result.Data["clamped"].get<bool>());
    EXPECT_FLOAT_EQ(result.Data["value"].get<f32>(), 2.0f); // still the live value
    EXPECT_FALSE(f.History.CanUndo());
}

// A clamped write whose clamped value equals the current one is a no-op TOO, and
// reports both facts (clamped:true, changed:false) rather than one masking the other.
TEST(McpFieldRegistry, ClampedNoOpReportsBothFlags)
{
    Fixture f;
    auto& vm = f.TheEntity.AddComponent<VirtualMeshComponent>();
    vm.m_ErrorThresholdPixels = 64.0f; // already at the max

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "VirtualMeshComponent", "ErrorThresholdPixels", Json(500.0));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_TRUE(result.Data["clamped"].get<bool>());
    EXPECT_FALSE(result.Data["changed"].get<bool>());
    EXPECT_FALSE(f.History.CanUndo());
}

// ---- newly-reachable field types round-trip ---------------------------------

// An AssetHandle field (VirtualMeshComponent::m_MeshSource) travels as a decimal
// string (a u64 exceeds JSON's safe-integer range) and round-trips undoably.
TEST(McpFieldRegistry, AssetHandleFieldRoundTrips)
{
    Fixture f;
    f.TheEntity.AddComponent<VirtualMeshComponent>();

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "VirtualMeshComponent", "MeshSource", Json("1234567890123456789"));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_EQ(result.Data["type"], "handle");
    EXPECT_EQ(result.Data["value"], "1234567890123456789");
    EXPECT_EQ(static_cast<u64>(f.TheEntity.GetComponent<VirtualMeshComponent>().m_MeshSource), 1234567890123456789ull);

    f.History.Undo();
    EXPECT_EQ(static_cast<u64>(f.TheEntity.GetComponent<VirtualMeshComponent>().m_MeshSource), 0ull);
}

// An enum field travels as its integer value (FogVolumeComponent::m_Shape). Its
// OLO_SERIALIZE(Clamp, Min=0, Max=2) range applies to the enum's underlying int,
// so an out-of-range enum is clamped to a valid enumerator rather than cast into a
// value the enum doesn't have.
TEST(McpFieldRegistry, EnumFieldRoundTripsAndClampsToValidEnumerators)
{
    Fixture f;
    auto& fog = f.TheEntity.AddComponent<OloEngine::FogVolumeComponent>();
    const int original = static_cast<int>(fog.m_Shape);
    const int target = original == 0 ? 1 : 0;

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "FogVolumeComponent", "Shape", Json(target));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_EQ(result.Data["type"], "int");
    EXPECT_FALSE(result.Data["clamped"].get<bool>());
    EXPECT_EQ(result.Data["value"].get<int>(), target);
    EXPECT_EQ(static_cast<int>(f.TheEntity.GetComponent<OloEngine::FogVolumeComponent>().m_Shape), target);

    f.History.Undo();
    EXPECT_EQ(static_cast<int>(f.TheEntity.GetComponent<OloEngine::FogVolumeComponent>().m_Shape), original);

    const auto tooBig = GFW::Apply(f.Scene_, f.History, f.Uuid, "FogVolumeComponent", "Shape", Json(99));
    ASSERT_TRUE(tooBig.Ok) << tooBig.Error;
    EXPECT_TRUE(tooBig.Data["clamped"].get<bool>());
    EXPECT_EQ(tooBig.Data["value"].get<int>(), 2); // the highest valid enumerator
    EXPECT_EQ(static_cast<int>(f.TheEntity.GetComponent<OloEngine::FogVolumeComponent>().m_Shape), 2);
}

// A string field on a newly-reachable component (TextComponent::TextString).
TEST(McpFieldRegistry, StringFieldOnNewlyRegisteredComponentRoundTrips)
{
    Fixture f;
    f.TheEntity.AddComponent<TextComponent>();

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "TextComponent", "TextString", Json("Hello MCP"));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_EQ(result.Data["type"], "string");
    EXPECT_EQ(f.TheEntity.GetComponent<TextComponent>().TextString, "Hello MCP");
    EXPECT_EQ(result.Data["value"], "Hello MCP");
}

// An unknown FIELD on a known (newly-registered) component lists that component's
// valid fields — the same self-correcting error the hand-written registry gave.
TEST(McpFieldRegistry, UnknownFieldOnKnownComponentListsItsFields)
{
    Fixture f;
    f.TheEntity.AddComponent<VirtualMeshComponent>();

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "VirtualMeshComponent", "NoSuchField", Json(1.0));
    EXPECT_FALSE(result.Ok);
    EXPECT_NE(result.Error.find("has no editable field"), std::string::npos);
    EXPECT_NE(result.Error.find("ErrorThresholdPixels"), std::string::npos);
}

// ---- 5. sub-object addressing (nested non-POD members) -----------------------
//
// Some components keep their ENTIRE authored surface inside a nested object, so a
// scan that only looked at a component's own top-level members left them with zero
// writable fields. ParticleSystemComponent was the worst case: everything an author
// tunes lives in its `ParticleSystem System` member (itself a `class`, whose own
// authored parameters nest one level further into ParticleEmitter / the modules).
// `olo_entity_set_field ParticleSystemComponent …` answered "Unknown or non-editable
// component" for every field name that exists.
//
// The generator now DESCENDS into a nested record — reusing the very same
// classification that drives the scene serializer's nested-struct support, with a
// laxer acceptance rule (a PARTIAL classification is fine: take the public,
// JSON-coercible members it recognised, ignore the rest) — and emits a dotted field
// name addressed by a member-access chain.

TEST(McpFieldRegistry, CoversFieldsNestedInsideANonPodMember)
{
    // The component that motivated the slice: previously ZERO writable fields.
    ASSERT_TRUE(RegistryHasComponent("ParticleSystemComponent"))
        << "ParticleSystemComponent's whole authored surface lives inside its `System` member";

    // One level down (a public field of the nested `class ParticleSystem`)…
    EXPECT_TRUE(RegistryHas("ParticleSystemComponent", "System.Playing"));
    EXPECT_TRUE(RegistryHas("ParticleSystemComponent", "System.Duration"));
    EXPECT_TRUE(RegistryHas("ParticleSystemComponent", "System.WindInfluence"));
    // …and two levels down (a public field of ParticleSystem's own nested members).
    EXPECT_TRUE(RegistryHas("ParticleSystemComponent", "System.Emitter.RateOverTime"));
    EXPECT_TRUE(RegistryHas("ParticleSystemComponent", "System.Emitter.InitialColor"));
    EXPECT_TRUE(RegistryHas("ParticleSystemComponent", "System.GravityModule.Gravity"));

    // Other components pick the descent up for free — these were all unreachable.
    EXPECT_TRUE(RegistryHas("TerrainComponent", "HeightShaping.WarpStrength"));
    EXPECT_TRUE(RegistryHas("AudioListenerComponent", "Config.ConeInnerAngle"));
    EXPECT_TRUE(RegistryHas("LODGroupComponent", "LODGroup.Bias"));
}

// A nested write goes through the identical path a top-level one does: coerce,
// no-op detect, ONE undoable command, and a value READ BACK from the live component.
TEST(McpFieldRegistry, NestedFieldWriteRoundTripsAndUndoes)
{
    Fixture f;
    auto& particles = f.TheEntity.AddComponent<OloEngine::ParticleSystemComponent>();
    particles.System.Emitter.RateOverTime = 10.0f;

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "ParticleSystemComponent",
                                   "System.Emitter.RateOverTime", Json(250.0));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_EQ(result.Data["type"], "float");
    EXPECT_TRUE(result.Data["changed"].get<bool>());
    EXPECT_TRUE(result.Data["undoable"].get<bool>());
    EXPECT_FLOAT_EQ(result.Data["previousValue"].get<f32>(), 10.0f);

    // The LIVE component actually moved (not just the reported value).
    const f32 live = f.TheEntity.GetComponent<OloEngine::ParticleSystemComponent>().System.Emitter.RateOverTime;
    EXPECT_FLOAT_EQ(live, 250.0f);
    EXPECT_FLOAT_EQ(result.Data["value"].get<f32>(), live);

    ASSERT_TRUE(f.History.CanUndo());
    f.History.Undo();
    EXPECT_FLOAT_EQ(f.TheEntity.GetComponent<OloEngine::ParticleSystemComponent>().System.Emitter.RateOverTime, 10.0f);
}

// A vec4 two levels deep coerces and clamps exactly like a top-level one.
TEST(McpFieldRegistry, NestedVectorFieldRoundTrips)
{
    Fixture f;
    f.TheEntity.AddComponent<OloEngine::ParticleSystemComponent>();

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "ParticleSystemComponent",
                                   "System.Emitter.InitialColor", Json::array({ 0.25, 0.5, 0.75, 1.0 }));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_EQ(result.Data["type"], "vec4");

    const glm::vec4 live = f.TheEntity.GetComponent<OloEngine::ParticleSystemComponent>().System.Emitter.InitialColor;
    EXPECT_FLOAT_EQ(live.x, 0.25f);
    EXPECT_FLOAT_EQ(live.y, 0.5f);
    EXPECT_FLOAT_EQ(live.z, 0.75f);
    EXPECT_FLOAT_EQ(live.w, 1.0f);
}

// A nested field inherits its load-time range the same way a top-level one does —
// and here that is a MEMORY-SAFETY contract, not a taste one. ParticleCurve::KeyCount
// is a length into a fixed `std::array<Key, 8>` that Evaluate() indexes as
// `Keys[KeyCount - 1]`; every scene-load path bounds it with
// `std::min(…, Keys.size())`. Sub-object addressing made the field reachable, so it
// carries OLO_SERIALIZE(Clamp, Min = 0, Max = 8) and MCP inherits that bound: a write
// of 100 CANNOT put the curve into a state a scene load could not produce.
TEST(McpFieldRegistry, NestedFieldInheritsTheSerializerClampInsteadOfIndexingOutOfBounds)
{
    Fixture f;
    f.TheEntity.AddComponent<OloEngine::ParticleSystemComponent>();

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "ParticleSystemComponent",
                                   "System.SizeModule.SizeCurve.KeyCount", Json(100));
    ASSERT_TRUE(result.Ok) << result.Error;
    // ASSERT, not EXPECT: an unclamped write carries no `requestedValue` at all, and
    // reading a missing JSON key would abort the process rather than fail the test.
    ASSERT_TRUE(result.Data["clamped"].get<bool>())
        << "the write was accepted verbatim — KeyCount is now " << result.Data["value"]
        << " on an 8-slot Keys array, which ParticleCurve::Evaluate would index out of bounds";
    EXPECT_EQ(result.Data["requestedValue"].get<u32>(), 100u);
    EXPECT_EQ(result.Data["value"].get<u32>(), 8u);

    const auto& curve = f.TheEntity.GetComponent<OloEngine::ParticleSystemComponent>().System.SizeModule.SizeCurve;
    EXPECT_EQ(curve.KeyCount, 8u);
    EXPECT_LE(curve.KeyCount, static_cast<u32>(curve.Keys.size())); // never out of bounds
}

// The descent stops at a container: an element of a std::vector / set / map has no
// static field name a dotted path could address, so none is invented.
TEST(McpFieldRegistry, DescentDoesNotInventFieldsForContainerElements)
{
    EXPECT_FALSE(RegistryHas("ParticleSystemComponent", "System.Emitter.Bursts"));
    EXPECT_FALSE(RegistryHas("ParticleSystemComponent", "System.ForceFields"));
    EXPECT_FALSE(RegistryHas("ParticleSystemComponent", "ChildSystems"));
    EXPECT_FALSE(RegistryHas("LODGroupComponent", "LODGroup.Levels"));
}

// Descending must not smuggle RUNTIME state back in through the side door.
// InstancedMeshComponent::_MergedCache is a public member ("Internal — do not write
// from user code; not serialized") whose PlacementHandle is a plain AssetHandle, so
// the descent WOULD have exposed it; it carries OLO_SERIALIZE(Skip), the same
// mechanism a top-level runtime field uses. The component's authored fields stay.
TEST(McpFieldRegistry, DescentRespectsSkipOnARuntimeCacheSubObject)
{
    EXPECT_FALSE(RegistryHas("InstancedMeshComponent", "_MergedCache.PlacementHandle"));
    EXPECT_FALSE(RegistryHas("InstancedMeshComponent", "_MergedCache.InlineSize"));
    EXPECT_TRUE(RegistryHas("InstancedMeshComponent", "CullDistance")); // authored — still there
}

// Nested fields are DISCOVERABLE, not just writable-if-you-guess-the-name:
// olo_entity_list_fields reports each dotted name with its current value.
TEST(McpFieldRegistry, ListFieldsReportsNestedDottedFields)
{
    Fixture f;
    auto& particles = f.TheEntity.AddComponent<OloEngine::ParticleSystemComponent>();
    particles.System.Emitter.RateOverTime = 42.0f;

    bool found = false;
    const Json out = GFW::ListFields(f.Scene_, f.Uuid, "ParticleSystemComponent", found);
    ASSERT_TRUE(found);
    ASSERT_EQ(out["components"].size(), 1u);

    bool sawNested = false;
    for (const auto& field : out["components"][0]["fields"])
    {
        if (field["field"] == "System.Emitter.RateOverTime")
        {
            sawNested = true;
            EXPECT_EQ(field["type"], "float");
            EXPECT_FLOAT_EQ(field["value"].get<f32>(), 42.0f);
        }
    }
    EXPECT_TRUE(sawNested) << "a nested field must be discoverable via olo_entity_list_fields";
}

// ---- 6. what sub-object addressing deliberately does NOT reach ---------------
//
// One component still exposes no authored field, and it is not a gap a dotted
// member-access path can close. Pinned here so the limitation is a stated contract
// rather than a silent surprise, and so the day someone closes it the test tells them
// to update this comment.
//
//   * MorphTargetComponent's authored surface is a DYNAMIC keyset
//     (`std::unordered_map<std::string, f32> Weights`), which needs map-key
//     addressing ("Weights.<targetName>"), not sub-object addressing: the field names
//     do not exist until a MorphTargetSet is bound at runtime, so no static registry
//     entry can name them.
//
// AudioSourceComponent used to be pinned here too (its 16 parameters live behind a
// PRIVATE cold blob a member-access chain cannot cross) — issue #607's
// AudioSourceComponent slice closed that gap with a setter-expression-based
// mechanism instead of sub-object addressing. See section 7 below.
TEST(McpFieldRegistry, KnownGapsWithNoStaticFieldPathStayUnreachable)
{
    EXPECT_FALSE(RegistryHas("MorphTargetComponent", "Weights")); // dynamic keyset, not a field
}

// ---- 7. AudioSourceComponent: setter-expression-based fields (issue #607) ----
//
// AudioSourceComponent's 16 authored parameters live behind `private
// std::unique_ptr<AudioSourceColdData> m_Cold`, reached only through GetConfig(). A
// member-access chain cannot cross a private member, so the plain field scan
// (MakeFieldAccess, sections 1-5 above) correctly never reaches them. Each parameter
// already carries an OLO_PROPERTY(Get=..., Set=...) annotation — the SAME expressions
// Lua/C# scripting compiles — so OloHeaderTool's EmitMcpSetterFields reuses that scan
// to emit MakeSetterField<C, F> registrations instead: they call the Set expression
// DIRECTLY on the live component (never a whole-object copy+swap), so a write reaches
// the live Ref<AudioSource> Source exactly like a scripted SetVolume() call would.

TEST(McpFieldRegistry, CoversAudioSourceComponent)
{
    for (const char* field : { "Volume", "Pitch", "PlayOnAwake", "Looping", "Spatialization",
                               "AttenuationModel", "RollOff", "MinGain", "MaxGain", "MinDistance",
                               "MaxDistance", "ConeInnerAngle", "ConeOuterAngle", "ConeOuterGain",
                               "DopplerFactor", "SoundConfigHandle" })
    {
        EXPECT_TRUE(RegistryHas("AudioSourceComponent", field)) << field << " should be MCP-writable";
    }
}

// A scalar float property round-trips undoably, exactly like a plain field.
TEST(McpFieldRegistry, AudioSourcePropertyRoundTripsAndUndoes)
{
    Fixture f;
    auto& audio = f.TheEntity.AddComponent<OloEngine::AudioSourceComponent>();
    audio.GetConfig().VolumeMultiplier = 1.0f;

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "AudioSourceComponent", "Volume", Json(0.4));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_EQ(result.Data["type"], "float");
    EXPECT_TRUE(result.Data["changed"].get<bool>());
    EXPECT_TRUE(result.Data["undoable"].get<bool>());
    EXPECT_FLOAT_EQ(result.Data["previousValue"].get<f32>(), 1.0f);
    EXPECT_FLOAT_EQ(f.TheEntity.GetComponent<OloEngine::AudioSourceComponent>().GetConfig().VolumeMultiplier, 0.4f);

    ASSERT_TRUE(f.History.CanUndo());
    f.History.Undo();
    EXPECT_FLOAT_EQ(f.TheEntity.GetComponent<OloEngine::AudioSourceComponent>().GetConfig().VolumeMultiplier, 1.0f);
}

// The AssetHandle-shaped property (SoundConfigHandle) round-trips as a decimal
// string, same contract as every other AssetHandle field in the registry.
TEST(McpFieldRegistry, AudioSourceHandleFieldRoundTrips)
{
    Fixture f;
    f.TheEntity.AddComponent<OloEngine::AudioSourceComponent>();

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "AudioSourceComponent", "SoundConfigHandle",
                                   Json("1234567890123456789"));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_EQ(result.Data["type"], "handle");
    EXPECT_EQ(static_cast<u64>(f.TheEntity.GetComponent<OloEngine::AudioSourceComponent>().GetSoundConfigHandle()),
              1234567890123456789ull);

    f.History.Undo();
    EXPECT_EQ(static_cast<u64>(f.TheEntity.GetComponent<OloEngine::AudioSourceComponent>().GetSoundConfigHandle()), 0ull);
}

// THE regression this slice exists to prevent: a whole-object copy+swap
// (ComponentChangeCommand<AudioSourceComponent>) would go through operator=, which
// UNCONDITIONALLY resets ActiveEventID to 0 on every write — independent of whether
// a live Source is even attached — and never re-invokes Source->SetVolume()/etc, so a
// write would silently detach a playing sound's event handle while leaving the
// actually-playing sound's parameters unchanged. MakeSetterField writes through the
// OLO_PROPERTY setter directly (PropertySetCommand, not ComponentChangeCommand<C>),
// so ActiveEventID survives an unrelated-field write — the half of this bug that is
// observable without a live audio backend. (AudioSource::Create requires a real file
// on disk the headless unit-test environment does not provide — see
// AudioEventsManagerTest.cpp's note on the same constraint — so the Source->SetVolume()
// side of this fix is exercised manually against a real playing sound instead of here.)
TEST(McpFieldRegistry, WriteDoesNotResetActiveEventID)
{
    Fixture f;
    auto& audio = f.TheEntity.AddComponent<OloEngine::AudioSourceComponent>();
    audio.ActiveEventID = 4242;
    audio.GetConfig().VolumeMultiplier = 1.0f;

    const auto result = GFW::Apply(f.Scene_, f.History, f.Uuid, "AudioSourceComponent", "Volume", Json(0.25));
    ASSERT_TRUE(result.Ok) << result.Error;
    EXPECT_TRUE(result.Data["changed"].get<bool>());
    EXPECT_EQ(f.TheEntity.GetComponent<OloEngine::AudioSourceComponent>().ActiveEventID, 4242u)
        << "a field write must not reset the live event handle";

    f.History.Undo();
    EXPECT_EQ(f.TheEntity.GetComponent<OloEngine::AudioSourceComponent>().ActiveEventID, 4242u);
    EXPECT_FLOAT_EQ(f.TheEntity.GetComponent<OloEngine::AudioSourceComponent>().GetConfig().VolumeMultiplier, 1.0f);
}
