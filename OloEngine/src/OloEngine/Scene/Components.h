#pragma once
#include "OloEngine/Scene/SceneCamera.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Audio/AudioListener.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Particle/ParticleSystem.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/TerrainStreamer.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"
#include "OloEngine/Terrain/Voxel/MarchingCubes.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"

#include <box2d/id.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <utility>
#include <vector>
#include <string>

namespace OloEngine
{

    struct IDComponent
    {
        UUID ID;

        IDComponent() = default;
        IDComponent(const IDComponent&) = default;
    };

    struct TagComponent
    {
        std::string Tag;
        bool renaming = false;

        TagComponent() = default;
        TagComponent(const TagComponent& other) = default;
        explicit TagComponent(std::string tag)
            : Tag(std::move(tag)) {}

        explicit operator std::string&()
        {
            return Tag;
        }
        explicit operator const std::string&() const
        {
            return Tag;
        }
    };

    struct PrefabComponent
    {
        UUID m_PrefabID{};
        UUID m_PrefabEntityID{};
        PrefabComponent() = default;
        PrefabComponent(const PrefabComponent&) = default;
        PrefabComponent(UUID prefabID, UUID prefabEntityID)
            : m_PrefabID(prefabID), m_PrefabEntityID(prefabEntityID) {}

        [[nodiscard]] inline bool IsValid() const noexcept
        {
            return static_cast<u64>(m_PrefabID) != 0 && static_cast<u64>(m_PrefabEntityID) != 0;
        }
    };

    struct TransformComponent
    {
        glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Rotation = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };

        TransformComponent() = default;
        TransformComponent(const TransformComponent& other) = default;
        explicit TransformComponent(const glm::vec3& translation)
            : Translation(translation) {}

        [[nodiscard("Store this!")]] glm::mat4 GetTransform() const
        {
            glm::mat4 const rotation = glm::toMat4(glm::quat(Rotation));

            return glm::translate(glm::mat4(1.0f), Translation) * rotation * glm::scale(glm::mat4(1.0f), Scale);
        }
    };

    struct SpriteRendererComponent
    {
        glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
        Ref<Texture2D> Texture = nullptr;
        f32 TilingFactor = 1.0f;

        SpriteRendererComponent() = default;
        SpriteRendererComponent(const SpriteRendererComponent&) = default;
        explicit SpriteRendererComponent(const glm::vec4& color)
            : Color(color) {}
    };

    struct CircleRendererComponent
    {
        glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
        f32 Thickness = 1.0f;
        f32 Fade = 0.005f;

        CircleRendererComponent() = default;
        CircleRendererComponent(const CircleRendererComponent&) = default;
    };

    struct CameraComponent
    {
        // TODO(olbu): think about moving to Scene
        SceneCamera Camera;
        bool Primary = true;
        bool FixedAspectRatio = false;

        CameraComponent() = default;
        CameraComponent(const CameraComponent&) = default;
    };

    // Physics

    struct Rigidbody2DComponent
    {
        enum class BodyType
        {
            Static = 0,
            Dynamic,
            Kinematic
        };
        BodyType Type = BodyType::Static;
        bool FixedRotation = false;

        // Storage for runtime
        b2BodyId RuntimeBody = b2_nullBodyId;

        Rigidbody2DComponent() = default;
        Rigidbody2DComponent(const Rigidbody2DComponent&) = default;
    };

    struct BoxCollider2DComponent
    {
        glm::vec2 Offset = { 0.0f, 0.0f };
        glm::vec2 Size = { 0.5f, 0.5f };

        // TODO(olbu): move into physics material in the future maybe
        f32 Density = 1.0f;
        f32 Friction = 0.5f;
        f32 Restitution = 0.0f;
        f32 RestitutionThreshold = 0.5f;

        // Storage for runtime
        void* RuntimeFixture = nullptr;

        BoxCollider2DComponent() = default;
        BoxCollider2DComponent(const BoxCollider2DComponent&) = default;
    };

    struct CircleCollider2DComponent
    {
        glm::vec2 Offset = { 0.0f, 0.0f };
        f32 Radius = 0.5f;

        // TODO(olbu): move into physics material in the future maybe
        f32 Density = 1.0f;
        f32 Friction = 0.5f;
        f32 Restitution = 0.0f;
        f32 RestitutionThreshold = 0.5f;

        // Storage for runtime
        void* RuntimeFixture = nullptr;

        CircleCollider2DComponent() = default;
        CircleCollider2DComponent(const CircleCollider2DComponent&) = default;
    };

    // 3D Physics Components

    enum class BodyType3D
    {
        Static = 0,
        Dynamic,
        Kinematic
    };

    struct Rigidbody3DComponent
    {
        BodyType3D m_Type = BodyType3D::Static;
        u32 m_LayerID = 0;
        f32 m_Mass = 1.0f;
        f32 m_LinearDrag = 0.01f;
        f32 m_AngularDrag = 0.05f;
        bool m_DisableGravity = false;
        bool m_IsTrigger = false;
        EActorAxis m_LockedAxes = EActorAxis::None;

        glm::vec3 m_InitialLinearVelocity = { 0.0f, 0.0f, 0.0f };
        glm::vec3 m_InitialAngularVelocity = { 0.0f, 0.0f, 0.0f };

        f32 m_MaxLinearVelocity = 500.0f;
        f32 m_MaxAngularVelocity = 50.0f;

        // Storage for runtime - Jolt BodyID token for safe access
        u64 m_RuntimeBodyToken = 0;

        Rigidbody3DComponent() = default;
        Rigidbody3DComponent(const Rigidbody3DComponent&) = default;
    };

    struct BoxCollider3DComponent
    {
        glm::vec3 m_HalfExtents = { 0.5f, 0.5f, 0.5f };
        glm::vec3 m_Offset = { 0.0f, 0.0f, 0.0f };

        // Physics material properties
        ColliderMaterial m_Material{};

        BoxCollider3DComponent() = default;
        BoxCollider3DComponent(const BoxCollider3DComponent&) = default;
    };

    struct SphereCollider3DComponent
    {
        f32 m_Radius = 0.5f;
        glm::vec3 m_Offset = { 0.0f, 0.0f, 0.0f };

        // Physics material properties
        ColliderMaterial m_Material{};

        SphereCollider3DComponent() = default;
        SphereCollider3DComponent(const SphereCollider3DComponent&) = default;
    };

    struct CapsuleCollider3DComponent
    {
        f32 m_Radius = 0.5f;
        f32 m_HalfHeight = 1.0f;
        glm::vec3 m_Offset = { 0.0f, 0.0f, 0.0f };

        // Physics material properties
        ColliderMaterial m_Material{};

        CapsuleCollider3DComponent() = default;
        CapsuleCollider3DComponent(const CapsuleCollider3DComponent&) = default;
    };

    struct MeshCollider3DComponent
    {
        AssetHandle m_ColliderAsset = 0; // Reference to MeshColliderAsset
        glm::vec3 m_Offset = { 0.0f, 0.0f, 0.0f };
        glm::vec3 m_Scale = { 1.0f, 1.0f, 1.0f };

        // Physics material properties
        ColliderMaterial m_Material{};

        // Collision complexity setting
        bool m_UseComplexAsSimple = false; // If true, use triangle mesh for dynamic bodies

        MeshCollider3DComponent() = default;
        MeshCollider3DComponent(const MeshCollider3DComponent&) = default;
        explicit MeshCollider3DComponent(AssetHandle colliderAsset) : m_ColliderAsset(colliderAsset) {}
    };

    struct ConvexMeshCollider3DComponent
    {
        AssetHandle m_ColliderAsset = 0; // Reference to MeshColliderAsset
        glm::vec3 m_Offset = { 0.0f, 0.0f, 0.0f };
        glm::vec3 m_Scale = { 1.0f, 1.0f, 1.0f };

        // Physics material properties
        ColliderMaterial m_Material{};

        // Convex hull settings
        f32 m_ConvexRadius = 0.05f; // Jolt convex radius for shape rounding
        u32 m_MaxVertices = 256;    // Maximum vertices in convex hull

        ConvexMeshCollider3DComponent() = default;
        ConvexMeshCollider3DComponent(const ConvexMeshCollider3DComponent&) = default;
        explicit ConvexMeshCollider3DComponent(AssetHandle colliderAsset) : m_ColliderAsset(colliderAsset) {}
    };

    struct TriangleMeshCollider3DComponent
    {
        AssetHandle m_ColliderAsset = 0; // Reference to MeshColliderAsset
        glm::vec3 m_Offset = { 0.0f, 0.0f, 0.0f };
        glm::vec3 m_Scale = { 1.0f, 1.0f, 1.0f };

        // Physics material properties
        ColliderMaterial m_Material{};

        // Triangle mesh is always static - no additional settings needed

        TriangleMeshCollider3DComponent() = default;
        TriangleMeshCollider3DComponent(const TriangleMeshCollider3DComponent&) = default;
        explicit TriangleMeshCollider3DComponent(AssetHandle colliderAsset) : m_ColliderAsset(colliderAsset) {}
    };

    struct CharacterController3DComponent
    {
        f32 m_SlopeLimitDeg = 45.0f;
        f32 m_StepOffset = 0.4f;
        f32 m_JumpPower = 8.0f;
        u32 m_LayerID = 0;

        bool m_DisableGravity = false;
        bool m_ControlMovementInAir = false;
        bool m_ControlRotationInAir = false;

        CharacterController3DComponent() = default;
        CharacterController3DComponent(const CharacterController3DComponent&) = default;
    };

    struct TextComponent
    {
        std::string TextString;
        Ref<Font> FontAsset = Font::GetDefault();
        glm::vec4 Color{ 1.0f };
        f32 Kerning = 0.0f;
        f32 LineSpacing = 0.0f;
    };

    struct ScriptComponent
    {
        std::string ClassName;

        ScriptComponent() = default;
        ScriptComponent(const ScriptComponent&) = default;
    };

    struct AudioSourceComponent
    {
        AudioSourceConfig Config;

        Ref<AudioSource> Source = nullptr;

        AudioSourceComponent() = default;
        AudioSourceComponent(const AudioSourceComponent&) = default;
    };

    struct AudioListenerComponent
    {
        bool Active = true;
        AudioListenerConfig Config;

        Ref<AudioListener> Listener;

        AudioListenerComponent() = default;
        AudioListenerComponent(const AudioListenerComponent&) = default;
    };

    // Note: SubmeshComponent, MeshComponent, AnimationStateComponent,
    // and SkeletonComponent are now defined in OloEngine/Animation/AnimatedMeshComponents.h
    // which is already included above

    // Material component for storing PBR material data
    struct MaterialComponent
    {
        Material m_Material;

        MaterialComponent() = default;
        MaterialComponent(const Material& material) : m_Material(material) {}
        MaterialComponent(const MaterialComponent&) = default;
    };

    // 3D Light Components

    struct DirectionalLightComponent
    {
        glm::vec3 m_Direction = { 0.0f, -1.0f, 0.0f };
        glm::vec3 m_Color = { 1.0f, 1.0f, 1.0f };
        f32 m_Intensity = 1.0f;
        bool m_CastShadows = true;

        // Shadow settings
        f32 m_ShadowBias = 0.005f;
        f32 m_ShadowNormalBias = 0.01f;
        f32 m_MaxShadowDistance = 200.0f;
        f32 m_CascadeSplitLambda = 0.5f;
        bool m_CascadeDebugVisualization = false;

        DirectionalLightComponent() = default;
        DirectionalLightComponent(const DirectionalLightComponent&) = default;
    };

    struct PointLightComponent
    {
        glm::vec3 m_Color = { 1.0f, 1.0f, 1.0f };
        f32 m_Intensity = 1.0f;
        f32 m_Range = 10.0f;      // Falloff range
        f32 m_Attenuation = 2.0f; // Attenuation power
        bool m_CastShadows = false;

        // Shadow settings
        f32 m_ShadowBias = 0.005f;
        f32 m_ShadowNormalBias = 0.01f;

        PointLightComponent() = default;
        PointLightComponent(const PointLightComponent&) = default;
    };

    struct SpotLightComponent
    {
        glm::vec3 m_Direction = { 0.0f, -1.0f, 0.0f };
        glm::vec3 m_Color = { 1.0f, 1.0f, 1.0f };
        f32 m_Intensity = 1.0f;
        f32 m_Range = 10.0f;
        f32 m_InnerCutoff = 12.5f; // Inner cone angle in degrees
        f32 m_OuterCutoff = 17.5f; // Outer cone angle in degrees
        f32 m_Attenuation = 2.0f;
        bool m_CastShadows = false;

        // Shadow settings
        f32 m_ShadowBias = 0.005f;
        f32 m_ShadowNormalBias = 0.01f;

        SpotLightComponent() = default;
        SpotLightComponent(const SpotLightComponent&) = default;
    };

    // Environment map component for skybox and IBL
    struct EnvironmentMapComponent
    {
        AssetHandle m_EnvironmentMapAsset = 0;
        std::string m_FilePath;               // Path to HDR/EXR file OR folder containing cubemap faces
        Ref<EnvironmentMap> m_EnvironmentMap; // Cached environment map (loaded from file path)

        // Cubemap mode: if true, m_FilePath is a folder with right.jpg, left.jpg, top.jpg, bottom.jpg, front.jpg, back.jpg
        // If false, m_FilePath is an HDR/EXR equirectangular file
        bool m_IsCubemapFolder = true; // Default to cubemap folder mode

        // Skybox display settings
        bool m_EnableSkybox = true;
        f32 m_Rotation = 0.0f;   // Rotation around Y axis in degrees
        f32 m_Exposure = 1.0f;   // Exposure multiplier
        f32 m_BlurAmount = 0.0f; // Blur for background (0 = sharp, 1 = fully blurred)

        // IBL settings
        bool m_EnableIBL = true;
        f32 m_IBLIntensity = 1.0f;

        // Tint/color adjustment
        glm::vec3 m_Tint = glm::vec3(1.0f);

        EnvironmentMapComponent() = default;
        EnvironmentMapComponent(const EnvironmentMapComponent&) = default;
        explicit EnvironmentMapComponent(const std::string& filepath) : m_FilePath(filepath) {}
    };

    // Entity relationship component for parent-child hierarchies (Hazel-style)
    struct RelationshipComponent
    {
        UUID m_ParentHandle{};
        std::vector<UUID> m_Children;

        RelationshipComponent() = default;
        RelationshipComponent(const RelationshipComponent&) = default;
        RelationshipComponent(RelationshipComponent&&) = default;
        RelationshipComponent& operator=(const RelationshipComponent&) = default;
        RelationshipComponent& operator=(RelationshipComponent&&) = default;
        explicit RelationshipComponent(UUID parent) : m_ParentHandle(parent) {}
    };

    // ── UI Components ────────────────────────────────────────────────────

    enum class UICanvasRenderMode : u8
    {
        ScreenSpaceOverlay = 0,
        WorldSpace
    };

    enum class UICanvasScaleMode : u8
    {
        ConstantPixelSize = 0,
        ScaleWithScreenSize
    };

    struct UICanvasComponent
    {
        UICanvasRenderMode m_RenderMode = UICanvasRenderMode::ScreenSpaceOverlay;
        UICanvasScaleMode m_ScaleMode = UICanvasScaleMode::ConstantPixelSize;
        i32 m_SortOrder = 0;
        glm::vec2 m_ReferenceResolution = { 1920.0f, 1080.0f };

        UICanvasComponent() = default;
        UICanvasComponent(const UICanvasComponent&) = default;
    };

    struct UIRectTransformComponent
    {
        glm::vec2 m_AnchorMin = { 0.5f, 0.5f };
        glm::vec2 m_AnchorMax = { 0.5f, 0.5f };
        glm::vec2 m_AnchoredPosition = { 0.0f, 0.0f };
        glm::vec2 m_SizeDelta = { 100.0f, 100.0f };
        glm::vec2 m_Pivot = { 0.5f, 0.5f };
        f32 m_Rotation = 0.0f;
        glm::vec2 m_Scale = { 1.0f, 1.0f };

        UIRectTransformComponent() = default;
        UIRectTransformComponent(const UIRectTransformComponent&) = default;
    };

    // Transient per-frame component — resolved screen-pixel rect, NOT serialized
    struct UIResolvedRectComponent
    {
        glm::vec2 m_Position = { 0.0f, 0.0f }; // Top-left corner in pixels
        glm::vec2 m_Size = { 0.0f, 0.0f };     // Width/height in pixels

        UIResolvedRectComponent() = default;
        UIResolvedRectComponent(const UIResolvedRectComponent&) = default;
    };

    enum class UITextAlignment : u8
    {
        TopLeft = 0,
        TopCenter,
        TopRight,
        MiddleLeft,
        MiddleCenter,
        MiddleRight,
        BottomLeft,
        BottomCenter,
        BottomRight
    };

    enum class UIButtonState : u8
    {
        Normal = 0,
        Hovered,
        Pressed,
        Disabled
    };

    struct UIImageComponent
    {
        Ref<Texture2D> m_Texture = nullptr;
        glm::vec4 m_Color = { 1.0f, 1.0f, 1.0f, 1.0f };
        // 9-slice border insets (left, right, top, bottom) in pixels
        glm::vec4 m_BorderInsets = { 0.0f, 0.0f, 0.0f, 0.0f };

        UIImageComponent() = default;
        UIImageComponent(const UIImageComponent&) = default;
    };

    struct UIPanelComponent
    {
        glm::vec4 m_BackgroundColor = { 0.2f, 0.2f, 0.2f, 1.0f };
        Ref<Texture2D> m_BackgroundTexture = nullptr;

        UIPanelComponent() = default;
        UIPanelComponent(const UIPanelComponent&) = default;
    };

    struct UITextComponent
    {
        std::string m_Text;
        Ref<Font> m_FontAsset = Font::GetDefault();
        f32 m_FontSize = 24.0f;
        glm::vec4 m_Color = { 1.0f, 1.0f, 1.0f, 1.0f };
        UITextAlignment m_Alignment = UITextAlignment::MiddleCenter;
        f32 m_Kerning = 0.0f;
        f32 m_LineSpacing = 0.0f;

        UITextComponent() = default;
        UITextComponent(const UITextComponent&) = default;
    };

    struct UIButtonComponent
    {
        glm::vec4 m_NormalColor = { 0.3f, 0.3f, 0.3f, 1.0f };
        glm::vec4 m_HoveredColor = { 0.4f, 0.4f, 0.4f, 1.0f };
        glm::vec4 m_PressedColor = { 0.2f, 0.2f, 0.2f, 1.0f };
        glm::vec4 m_DisabledColor = { 0.15f, 0.15f, 0.15f, 0.5f };
        bool m_Interactable = true;

        // Runtime state — not serialized
        UIButtonState m_State = UIButtonState::Normal;

        UIButtonComponent() = default;
        UIButtonComponent(const UIButtonComponent&) = default;
    };

    enum class UISliderDirection : u8
    {
        LeftToRight = 0,
        RightToLeft,
        TopToBottom,
        BottomToTop
    };

    struct UISliderComponent
    {
        f32 m_Value = 0.0f;
        f32 m_MinValue = 0.0f;
        f32 m_MaxValue = 1.0f;
        UISliderDirection m_Direction = UISliderDirection::LeftToRight;
        glm::vec4 m_BackgroundColor = { 0.15f, 0.15f, 0.15f, 1.0f };
        glm::vec4 m_FillColor = { 0.3f, 0.6f, 1.0f, 1.0f };
        glm::vec4 m_HandleColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        bool m_Interactable = true;

        // Runtime state — not serialized
        bool m_IsDragging = false;

        UISliderComponent() = default;
        UISliderComponent(const UISliderComponent&) = default;
    };

    struct UICheckboxComponent
    {
        bool m_IsChecked = false;
        glm::vec4 m_UncheckedColor = { 0.2f, 0.2f, 0.2f, 1.0f };
        glm::vec4 m_CheckedColor = { 0.3f, 0.6f, 1.0f, 1.0f };
        glm::vec4 m_CheckmarkColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        bool m_Interactable = true;

        UICheckboxComponent() = default;
        UICheckboxComponent(const UICheckboxComponent&) = default;
    };

    enum class UIFillMethod : u8
    {
        Horizontal = 0,
        Vertical
    };

    struct UIProgressBarComponent
    {
        f32 m_Value = 0.0f;
        f32 m_MinValue = 0.0f;
        f32 m_MaxValue = 1.0f;
        UIFillMethod m_FillMethod = UIFillMethod::Horizontal;
        glm::vec4 m_BackgroundColor = { 0.15f, 0.15f, 0.15f, 1.0f };
        glm::vec4 m_FillColor = { 0.3f, 0.8f, 0.3f, 1.0f };

        UIProgressBarComponent() = default;
        UIProgressBarComponent(const UIProgressBarComponent&) = default;
    };

    struct UIInputFieldComponent
    {
        std::string m_Text;
        std::string m_Placeholder = "Enter text...";
        Ref<Font> m_FontAsset = Font::GetDefault();
        f32 m_FontSize = 24.0f;
        glm::vec4 m_TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        glm::vec4 m_PlaceholderColor = { 0.5f, 0.5f, 0.5f, 1.0f };
        glm::vec4 m_BackgroundColor = { 0.15f, 0.15f, 0.15f, 1.0f };
        i32 m_CharacterLimit = 0; // 0 = no limit
        bool m_Interactable = true;

        // Runtime state — not serialized
        bool m_IsFocused = false;
        i32 m_CursorPosition = 0;

        UIInputFieldComponent() = default;
        UIInputFieldComponent(const UIInputFieldComponent&) = default;
    };

    // --- Phase 4: Complex Widgets ---

    enum class UIScrollDirection : u8
    {
        Vertical = 0,
        Horizontal,
        Both
    };

    struct UIScrollViewComponent
    {
        glm::vec2 m_ScrollPosition = { 0.0f, 0.0f };
        glm::vec2 m_ContentSize = { 0.0f, 0.0f }; // total scrollable content area
        UIScrollDirection m_ScrollDirection = UIScrollDirection::Vertical;
        f32 m_ScrollSpeed = 20.0f;
        bool m_ShowHorizontalScrollbar = false;
        bool m_ShowVerticalScrollbar = true;
        glm::vec4 m_ScrollbarColor = { 0.4f, 0.4f, 0.4f, 0.6f };
        glm::vec4 m_ScrollbarTrackColor = { 0.15f, 0.15f, 0.15f, 0.3f };

        UIScrollViewComponent() = default;
        UIScrollViewComponent(const UIScrollViewComponent&) = default;
    };

    struct UIDropdownOption
    {
        std::string m_Label;
    };

    struct UIDropdownComponent
    {
        std::vector<UIDropdownOption> m_Options;
        i32 m_SelectedIndex = -1;
        glm::vec4 m_BackgroundColor = { 0.2f, 0.2f, 0.2f, 1.0f };
        glm::vec4 m_HighlightColor = { 0.3f, 0.6f, 1.0f, 1.0f };
        glm::vec4 m_TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        Ref<Font> m_FontAsset = Font::GetDefault();
        f32 m_FontSize = 24.0f;
        f32 m_ItemHeight = 30.0f;
        bool m_Interactable = true;

        // Runtime state — not serialized
        bool m_IsOpen = false;
        i32 m_HoveredIndex = -1;

        UIDropdownComponent() = default;
        UIDropdownComponent(const UIDropdownComponent&) = default;
    };

    enum class UIGridLayoutStartCorner : u8
    {
        UpperLeft = 0,
        UpperRight,
        LowerLeft,
        LowerRight
    };

    enum class UIGridLayoutAxis : u8
    {
        Horizontal = 0,
        Vertical
    };

    struct UIGridLayoutComponent
    {
        glm::vec2 m_CellSize = { 100.0f, 100.0f };
        glm::vec2 m_Spacing = { 5.0f, 5.0f };
        glm::vec4 m_Padding = { 5.0f, 5.0f, 5.0f, 5.0f }; // left, right, top, bottom
        UIGridLayoutStartCorner m_StartCorner = UIGridLayoutStartCorner::UpperLeft;
        UIGridLayoutAxis m_StartAxis = UIGridLayoutAxis::Horizontal;
        i32 m_ConstraintCount = 0; // 0 = flexible, >0 = fixed columns (Horizontal) or rows (Vertical)

        UIGridLayoutComponent() = default;
        UIGridLayoutComponent(const UIGridLayoutComponent&) = default;
    };

    struct UIToggleComponent
    {
        bool m_IsOn = false;
        glm::vec4 m_OffColor = { 0.3f, 0.3f, 0.3f, 1.0f };
        glm::vec4 m_OnColor = { 0.3f, 0.8f, 0.3f, 1.0f };
        glm::vec4 m_KnobColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        bool m_Interactable = true;

        UIToggleComponent() = default;
        UIToggleComponent(const UIToggleComponent&) = default;
    };

    // ── Particle System ──────────────────────────────────────────────────

    struct ParticleSystemComponent
    {
        ParticleSystem System;
        Ref<Texture2D> Texture = nullptr;
        Ref<Mesh> ParticleMesh = nullptr; // Mesh for ParticleRenderMode::Mesh

        // Child particle systems for sub-emitters (each has independent settings)
        std::vector<ParticleSystem> ChildSystems;
        std::vector<Ref<Texture2D>> ChildTextures;

        ParticleSystemComponent() = default;
        ParticleSystemComponent(const ParticleSystemComponent&) = default;
    };

    // ── Terrain ──────────────────────────────────────────────────────────

    struct TerrainComponent
    {
        // Serialized properties
        std::string m_HeightmapPath;
        f32 m_WorldSizeX = 256.0f;
        f32 m_WorldSizeZ = 256.0f;
        f32 m_HeightScale = 64.0f;

        // Procedural generation settings (serialized, used when m_HeightmapPath is empty)
        bool m_ProceduralEnabled = false;
        i32 m_ProceduralSeed = 42;
        u32 m_ProceduralResolution = 512;
        u32 m_ProceduralOctaves = 6;
        f32 m_ProceduralFrequency = 3.0f;
        f32 m_ProceduralLacunarity = 2.0f;
        f32 m_ProceduralPersistence = 0.45f;

        // LOD / tessellation settings (serialized)
        bool m_TessellationEnabled = true;
        f32 m_TargetTriangleSize = 8.0f; // Screen-space pixel target
        f32 m_MorphRegion = 0.3f;        // Morph blend fraction [0,1]

        // Streaming settings (serialized)
        bool m_StreamingEnabled = false;
        std::string m_TileDirectory; // Directory containing tile files
        std::string m_TileFilePattern = "tile_%d_%d.raw";
        f32 m_TileWorldSize = 256.0f;  // World-space size per tile
        u32 m_TileResolution = 513;    // Heightmap resolution per tile
        u32 m_StreamingLoadRadius = 3; // Tile load radius around camera
        u32 m_StreamingMaxTiles = 25;  // LRU tile budget

        // Voxel override settings (serialized)
        bool m_VoxelEnabled = false;
        f32 m_VoxelSize = 1.0f;

        // Runtime state — not serialized
        Ref<TerrainData> m_TerrainData;
        Ref<TerrainChunkManager> m_ChunkManager;
        Ref<TerrainMaterial> m_Material;
        Ref<TerrainStreamer> m_Streamer;
        Ref<VoxelOverride> m_VoxelOverride;
        std::unordered_map<VoxelCoord, VoxelMesh, VoxelCoordHash> m_VoxelMeshes;
        bool m_NeedsRebuild = true;
        bool m_MaterialNeedsRebuild = true;

        TerrainComponent() = default;
        TerrainComponent(const TerrainComponent& other)
            : m_HeightmapPath(other.m_HeightmapPath), m_WorldSizeX(other.m_WorldSizeX), m_WorldSizeZ(other.m_WorldSizeZ), m_HeightScale(other.m_HeightScale), m_ProceduralEnabled(other.m_ProceduralEnabled), m_ProceduralSeed(other.m_ProceduralSeed), m_ProceduralResolution(other.m_ProceduralResolution), m_ProceduralOctaves(other.m_ProceduralOctaves), m_ProceduralFrequency(other.m_ProceduralFrequency), m_ProceduralLacunarity(other.m_ProceduralLacunarity), m_ProceduralPersistence(other.m_ProceduralPersistence), m_TessellationEnabled(other.m_TessellationEnabled), m_TargetTriangleSize(other.m_TargetTriangleSize), m_MorphRegion(other.m_MorphRegion), m_StreamingEnabled(other.m_StreamingEnabled), m_TileDirectory(other.m_TileDirectory), m_TileFilePattern(other.m_TileFilePattern), m_TileWorldSize(other.m_TileWorldSize), m_TileResolution(other.m_TileResolution), m_StreamingLoadRadius(other.m_StreamingLoadRadius), m_StreamingMaxTiles(other.m_StreamingMaxTiles), m_VoxelEnabled(other.m_VoxelEnabled), m_VoxelSize(other.m_VoxelSize)
        {
            // Runtime state intentionally NOT copied — force rebuild
        }
        TerrainComponent& operator=(const TerrainComponent& other)
        {
            if (this != &other)
            {
                m_HeightmapPath = other.m_HeightmapPath;
                m_WorldSizeX = other.m_WorldSizeX;
                m_WorldSizeZ = other.m_WorldSizeZ;
                m_HeightScale = other.m_HeightScale;
                m_ProceduralEnabled = other.m_ProceduralEnabled;
                m_ProceduralSeed = other.m_ProceduralSeed;
                m_ProceduralResolution = other.m_ProceduralResolution;
                m_ProceduralOctaves = other.m_ProceduralOctaves;
                m_ProceduralFrequency = other.m_ProceduralFrequency;
                m_ProceduralLacunarity = other.m_ProceduralLacunarity;
                m_ProceduralPersistence = other.m_ProceduralPersistence;
                m_TessellationEnabled = other.m_TessellationEnabled;
                m_TargetTriangleSize = other.m_TargetTriangleSize;
                m_MorphRegion = other.m_MorphRegion;
                m_StreamingEnabled = other.m_StreamingEnabled;
                m_TileDirectory = other.m_TileDirectory;
                m_TileFilePattern = other.m_TileFilePattern;
                m_TileWorldSize = other.m_TileWorldSize;
                m_TileResolution = other.m_TileResolution;
                m_StreamingLoadRadius = other.m_StreamingLoadRadius;
                m_StreamingMaxTiles = other.m_StreamingMaxTiles;
                m_VoxelEnabled = other.m_VoxelEnabled;
                m_VoxelSize = other.m_VoxelSize;
                // Runtime state reset — force rebuild
                m_TerrainData = nullptr;
                m_ChunkManager = nullptr;
                m_Material = nullptr;
                m_Streamer = nullptr;
                m_VoxelOverride = nullptr;
                m_VoxelMeshes.clear();
                m_NeedsRebuild = true;
                m_MaterialNeedsRebuild = true;
            }
            return *this;
        }
        TerrainComponent(TerrainComponent&&) noexcept = default;
        TerrainComponent& operator=(TerrainComponent&&) noexcept = default;
    };

    struct FoliageComponent
    {
        // Serialized
        std::vector<FoliageLayer> m_Layers;
        bool m_Enabled = true;

        // Runtime (not serialized)
        Ref<FoliageRenderer> m_Renderer;
        bool m_NeedsRebuild = true;

        FoliageComponent() = default;
        FoliageComponent(const FoliageComponent& other)
            : m_Layers(other.m_Layers), m_Enabled(other.m_Enabled)
        {
            // Runtime state intentionally NOT copied — force rebuild
        }
        FoliageComponent& operator=(const FoliageComponent& other)
        {
            if (this != &other)
            {
                m_Layers = other.m_Layers;
                m_Enabled = other.m_Enabled;
                m_Renderer = nullptr;
                m_NeedsRebuild = true;
            }
            return *this;
        }
        FoliageComponent(FoliageComponent&&) noexcept = default;
        FoliageComponent& operator=(FoliageComponent&&) noexcept = default;
    };

    struct SnowDeformerComponent
    {
        f32 m_DeformRadius = 0.5f;     // World-space radius of the deformation stamp
        f32 m_DeformDepth = 0.1f;      // How deep the deformer stamps into snow (meters)
        f32 m_FalloffExponent = 2.0f;  // Radial falloff curve (1=linear, 2=quadratic)
        f32 m_CompactionFactor = 0.5f; // 0=full removal, 1=compact only (no displacement)
        bool m_EmitEjecta = true;      // Emit snow puff particles on deformation

        SnowDeformerComponent() = default;
        SnowDeformerComponent(const SnowDeformerComponent&) = default;
        SnowDeformerComponent& operator=(const SnowDeformerComponent&) = default;
        SnowDeformerComponent(SnowDeformerComponent&&) noexcept = default;
        SnowDeformerComponent& operator=(SnowDeformerComponent&&) noexcept = default;
    };

    template<typename... Component>
    struct ComponentGroup
    {
    };

    using AllComponents = ComponentGroup<
        TransformComponent,
        SpriteRendererComponent,
        CircleRendererComponent,
        CameraComponent,
        PrefabComponent,
        Rigidbody2DComponent,
        BoxCollider2DComponent,
        CircleCollider2DComponent,
        Rigidbody3DComponent,
        BoxCollider3DComponent,
        SphereCollider3DComponent,
        CapsuleCollider3DComponent,
        MeshCollider3DComponent,
        ConvexMeshCollider3DComponent,
        TriangleMeshCollider3DComponent,
        CharacterController3DComponent,
        TextComponent,
        ScriptComponent,
        AudioSourceComponent,
        AudioListenerComponent,
        SubmeshComponent,
        MeshComponent,
        ModelComponent,
        AnimationStateComponent,
        SkeletonComponent,
        MaterialComponent,
        DirectionalLightComponent,
        PointLightComponent,
        SpotLightComponent,
        EnvironmentMapComponent,
        RelationshipComponent,
        UICanvasComponent,
        UIRectTransformComponent,
        UIImageComponent,
        UIPanelComponent,
        UITextComponent,
        UIButtonComponent,
        UISliderComponent,
        UICheckboxComponent,
        UIProgressBarComponent,
        UIInputFieldComponent,
        UIScrollViewComponent,
        UIDropdownComponent,
        UIGridLayoutComponent,
        UIToggleComponent,
        ParticleSystemComponent,
        TerrainComponent,
        FoliageComponent,
        SnowDeformerComponent>;
} // namespace OloEngine
