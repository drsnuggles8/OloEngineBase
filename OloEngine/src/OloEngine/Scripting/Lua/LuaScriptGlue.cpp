#include "OloEnginePCH.h"
#include "LuaScriptGlueInternal.h"

// =============================================================================
// LuaScriptGlue.cpp — the component registry and the registration dispatcher.
//
// The 151 Sol2 usertype registrations that used to live here in one 5,112-line
// function now live in the LuaScriptGlue_*.cpp parts; this file keeps the pieces
// that must stay in exactly one TU. See LuaScriptGlueInternal.h for the why.
// =============================================================================

namespace OloEngine
{
    template<typename T>
    static constexpr ComponentEntry MakeEntry()
    {
        return {
            [](Entity& e) -> bool
            { return e.HasComponent<T>(); },
            // Returns a raw T* wrapped in sol::object — only used internally by
            // the proxy's __index/__newindex to perform a single property
            // read/write.  Lua scripts never see this pointer directly.
            [](Entity& e, sol::this_state s) -> sol::object
            {
                if (e.HasComponent<T>())
                    return sol::make_object(s, &e.GetComponent<T>());
                return sol::make_object(s, sol::nil);
            }
        };
    }

    // clang-format off
    // NOT static: LuaScriptGlueInternal.h declares this with external linkage so
    // the parts can reach it, and the definition has to agree. clang-cl accepted
    // the mismatch; Linux clang++ rejects it outright, which is where it surfaced.
    const std::unordered_map<std::string_view, ComponentEntry>& GetComponentRegistry()
    {
        static const std::unordered_map<std::string_view, ComponentEntry> s_Registry = {
            #define REGISTER_COMPONENT(T) { #T, MakeEntry<T>() }
            // Core
            REGISTER_COMPONENT(TagComponent),
            REGISTER_COMPONENT(TransformComponent),
            REGISTER_COMPONENT(Rigidbody2DComponent),
            REGISTER_COMPONENT(CameraComponent),
            REGISTER_COMPONENT(SpriteRendererComponent),
            REGISTER_COMPONENT(CircleRendererComponent),
            REGISTER_COMPONENT(TilemapComponent),
            REGISTER_COMPONENT(TextComponent),
            REGISTER_COMPONENT(MeshComponent),
            REGISTER_COMPONENT(InstancedMeshComponent),
            REGISTER_COMPONENT(MaterialComponent),
            REGISTER_COMPONENT(BoxCollider2DComponent),
            REGISTER_COMPONENT(CircleCollider2DComponent),
            REGISTER_COMPONENT(AudioSourceComponent),
            REGISTER_COMPONENT(AudioListenerComponent),
            REGISTER_COMPONENT(AudioSoundGraphComponent),
            REGISTER_COMPONENT(VideoOverlayComponent),
            REGISTER_COMPONENT(VideoSurfaceComponent),
            REGISTER_COMPONENT(ParticleSystemComponent),
            REGISTER_COMPONENT(NavAgentComponent),
            REGISTER_COMPONENT(BoidComponent),
            REGISTER_COMPONENT(BoidObstacleComponent),
            REGISTER_COMPONENT(PlayerRigComponent),
            REGISTER_COMPONENT(CameraRigComponent),
            REGISTER_COMPONENT(WeaponComponent),
            REGISTER_COMPONENT(PlayerRespawnComponent),
            REGISTER_COMPONENT(AbilityComponent),
            REGISTER_COMPONENT(DialogueComponent),
            REGISTER_COMPONENT(VisualScriptComponent),
            REGISTER_COMPONENT(NetworkIdentityComponent),
            REGISTER_COMPONENT(IKTargetComponent),
            REGISTER_COMPONENT(SpringBoneComponent),
            REGISTER_COMPONENT(RetargetingComponent),
            REGISTER_COMPONENT(FootIKComponent),
            REGISTER_COMPONENT(LocomotionComponent),
            REGISTER_COMPONENT(NoiseAnimationComponent),
            REGISTER_COMPONENT(NameplateComponent),
            REGISTER_COMPONENT(InventoryComponent),
            REGISTER_COMPONENT(ItemPickupComponent),
            REGISTER_COMPONENT(ItemContainerComponent),
            REGISTER_COMPONENT(QuestJournalComponent),
            REGISTER_COMPONENT(QuestGiverComponent),
            REGISTER_COMPONENT(ProgressionComponent),
            REGISTER_COMPONENT(ScriptComponent),
            REGISTER_COMPONENT(LuaScriptComponent),
            REGISTER_COMPONENT(ModelComponent),
            // 3D Physics
            REGISTER_COMPONENT(Rigidbody3DComponent),
            REGISTER_COMPONENT(DestructibleComponent),
            REGISTER_COMPONENT(BuoyancyComponent),
            REGISTER_COMPONENT(FluidComponent),
            REGISTER_COMPONENT(FluidEmitterComponent),
            REGISTER_COMPONENT(FluidKillVolumeComponent),
            REGISTER_COMPONENT(BoxCollider3DComponent),
            REGISTER_COMPONENT(SphereCollider3DComponent),
            REGISTER_COMPONENT(CapsuleCollider3DComponent),
            REGISTER_COMPONENT(MeshCollider3DComponent),
            REGISTER_COMPONENT(ConvexMeshCollider3DComponent),
            REGISTER_COMPONENT(TriangleMeshCollider3DComponent),
            REGISTER_COMPONENT(PhysicsJoint3DComponent),
            REGISTER_COMPONENT(VehicleComponent),
            REGISTER_COMPONENT(BoatComponent),
            REGISTER_COMPONENT(SailComponent),
            REGISTER_COMPONENT(AircraftComponent),
            REGISTER_COMPONENT(RagdollComponent),
            REGISTER_COMPONENT(ClothComponent),
            // UI
            REGISTER_COMPONENT(UICanvasComponent),
            REGISTER_COMPONENT(UIRectTransformComponent),
            REGISTER_COMPONENT(UIImageComponent),
            REGISTER_COMPONENT(UIPanelComponent),
            REGISTER_COMPONENT(UITextComponent),
            REGISTER_COMPONENT(UIButtonComponent),
            REGISTER_COMPONENT(UISliderComponent),
            REGISTER_COMPONENT(UICheckboxComponent),
            REGISTER_COMPONENT(UIProgressBarComponent),
            REGISTER_COMPONENT(UIInputFieldComponent),
            REGISTER_COMPONENT(UIScrollViewComponent),
            REGISTER_COMPONENT(UIDropdownComponent),
            REGISTER_COMPONENT(UIGridLayoutComponent),
            REGISTER_COMPONENT(UIToggleComponent),
            REGISTER_COMPONENT(UIWorldAnchorComponent),
            // Lighting
            REGISTER_COMPONENT(DirectionalLightComponent),
            REGISTER_COMPONENT(PointLightComponent),
            REGISTER_COMPONENT(SpotLightComponent),
            REGISTER_COMPONENT(SphereAreaLightComponent),
            REGISTER_COMPONENT(ProceduralSkyComponent),
            REGISTER_COMPONENT(StarNestSkyComponent),
            REGISTER_COMPONENT(LightProbeComponent),
            REGISTER_COMPONENT(LightProbeVolumeComponent),
            // Atmosphere & weather (issue #633)
            REGISTER_COMPONENT(TimeOfDayComponent),
            REGISTER_COMPONENT(WeatherStateComponent),
            REGISTER_COMPONENT(CloudscapeComponent),
            // Streaming
            REGISTER_COMPONENT(StreamingVolumeComponent),
            // Terrain & water
            REGISTER_COMPONENT(TerrainComponent),
            // WaterComponent had a full Sol2 usertype (~50 properties, added
            // with the water renderer) but was never in THIS registry, which is
            // what `entity_utils.get_component` resolves against — so the whole
            // surface was unreachable from a script and every lookup logged
            // "unknown or missing component 'WaterComponent'". Found wiring
            // Drift's sea state to the wind (#882). Nothing guards this list
            // against a usertype it forgot, by design (CLAUDE.md: many
            // components legitimately aren't Lua-exposed, so a completeness
            // test here would be noise) — so the usertype and the registry
            // entry are two edits, not one.
            REGISTER_COMPONENT(WaterComponent),
            // Animation
            REGISTER_COMPONENT(AnimationGraphComponent),
            REGISTER_COMPONENT(MorphTargetComponent),
            // Cinematic
            REGISTER_COMPONENT(CinematicComponent),
            // AI / Behavior
            REGISTER_COMPONENT(NavMeshBoundsComponent),
            REGISTER_COMPONENT(BehaviorTreeComponent),
            REGISTER_COMPONENT(StateMachineComponent),
            REGISTER_COMPONENT(GoapAgentComponent),
            REGISTER_COMPONENT(PerceptibleComponent),
            REGISTER_COMPONENT(PerceptionComponent),
            #undef REGISTER_COMPONENT
        };
        return s_Registry;
    }
    // clang-format on
    void LuaScriptGlue::RegisterAllTypes()
    {
        RegisterAllTypes(*Scripting::GetState());
    }

    // ORDER IS LOAD-BEARING and is exactly the order the single function used:
    // the GLM vector types have to be registered before any component exposing
    // them, SceneCamera before CameraComponent, and RegisterEngineApiTypes
    // appends to the  table RegisterGameplayTypes creates.
    void LuaScriptGlue::RegisterAllTypes(sol::state& lua)
    {
        RegisterCoreTypes(lua);
        RegisterEnvironmentTypes(lua);
        RegisterCharacterTypes(lua);
        RegisterSceneGraphTypes(lua);
        RegisterEffectsTypes(lua);
        RegisterPlatformTypes(lua);
        RegisterWorldTypes(lua);
        RegisterGameplayTypes(lua);
        RegisterEngineApiTypes(lua);
    }
} // namespace OloEngine
