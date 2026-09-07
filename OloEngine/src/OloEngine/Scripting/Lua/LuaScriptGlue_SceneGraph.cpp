#include "OloEnginePCH.h"
#include "LuaScriptGlueInternal.h"

// =============================================================================
// LuaScriptGlue_SceneGraph.cpp — scene-graph and renderer components, plus the UI widget family.
//
// One of the LuaScriptGlue_*.cpp parts. See LuaScriptGlueInternal.h for why the
// glue is split and why the call order in RegisterAllTypes is load-bearing.
// =============================================================================

namespace OloEngine
{
    void LuaScriptGlue::RegisterSceneGraphTypes(sol::state& lua)
    {
        // --- PrefabComponent ---
        // Read-only window into prefab-instance identity & override state.
        // Mutating the override sets from scripts is intentionally not exposed —
        // that goes through the editor or the Prefab API.
        lua.new_usertype<PrefabComponent>("PrefabComponent", sol::no_constructor,
                                          "prefabID", sol::readonly_property([](const PrefabComponent& p)
                                                                             { return static_cast<u64>(p.m_PrefabID); }),
                                          "prefabEntityID", sol::readonly_property([](const PrefabComponent& p)
                                                                                   { return static_cast<u64>(p.m_PrefabEntityID); }),
                                          "isValid", sol::readonly_property(&PrefabComponent::IsValid),
                                          "hasAnyOverrides", sol::readonly_property(&PrefabComponent::HasAnyOverrides),
                                          "isComponentOverridden", &PrefabComponent::IsComponentOverridden,
                                          "isComponentAdded", &PrefabComponent::IsComponentAdded,
                                          "isComponentRemoved", &PrefabComponent::IsComponentRemoved);

        // --- RelationshipComponent ---
        // Read-only hierarchy view. Children come back as a Lua-side table
        // of u64 UUIDs; scripts that need to walk the hierarchy can resolve
        // each ID via Scene.FindEntityByUUID.
        lua.new_usertype<RelationshipComponent>("RelationshipComponent", sol::no_constructor,
                                                "parentHandle", sol::readonly_property([](const RelationshipComponent& r)
                                                                                       { return static_cast<u64>(r.m_ParentHandle); }),
                                                "childCount", sol::readonly_property([](const RelationshipComponent& r)
                                                                                     { return r.m_Children.size(); }),
                                                "children", sol::readonly_property([](const RelationshipComponent& r, sol::this_state s) -> sol::table
                                                                                   {
            sol::state_view lua_state(s);
            sol::table t = lua_state.create_table(static_cast<int>(r.m_Children.size()), 0);
            for (sizet i = 0; i < r.m_Children.size(); ++i)
                t[i + 1] = static_cast<u64>(r.m_Children[i]);
            return t; }));

        // --- TagComponent ---
        lua.new_usertype<TagComponent>("TagComponent",
                                       "tag", &TagComponent::Tag);

        // --- ScriptComponent ---
        lua.new_usertype<ScriptComponent>("ScriptComponent",
                                          "className", &ScriptComponent::ClassName);

        // --- LuaScriptComponent ---
        lua.new_usertype<LuaScriptComponent>("LuaScriptComponent",
                                             "scriptFile", &LuaScriptComponent::ScriptFile);

        // --- ModelComponent ---
        lua.new_usertype<ModelComponent>("ModelComponent",
                                         "filePath", &ModelComponent::m_FilePath,
                                         "visible", &ModelComponent::m_Visible,
                                         // Baked-GI receiver flag (issue #867). Toggling it stales
                                         // any existing bake, which the runtime detects on its own;
                                         // a script cannot re-bake, so this is an authoring lever
                                         // for tools rather than something to flip at runtime.
                                         "lightmapStatic", &ModelComponent::m_LightmapStatic,
                                         "isLoaded", sol::property(&ModelComponent::IsLoaded));

        // --- SceneCamera (needed by CameraComponent) ---
        lua.new_usertype<SceneCamera>("SceneCamera",
                                      "projectionType", sol::property(&SceneCamera::GetProjectionType, &SceneCamera::SetProjectionType),
                                      "perspectiveFOV", sol::property(&SceneCamera::GetPerspectiveVerticalFOV, &SceneCamera::SetPerspectiveVerticalFOV),
                                      "perspectiveNearClip", sol::property(&SceneCamera::GetPerspectiveNearClip, &SceneCamera::SetPerspectiveNearClip),
                                      "perspectiveFarClip", sol::property(&SceneCamera::GetPerspectiveFarClip, &SceneCamera::SetPerspectiveFarClip),
                                      "orthographicSize", sol::property(&SceneCamera::GetOrthographicSize, &SceneCamera::SetOrthographicSize),
                                      "orthographicNearClip", sol::property(&SceneCamera::GetOrthographicNearClip, &SceneCamera::SetOrthographicNearClip),
                                      "orthographicFarClip", sol::property(&SceneCamera::GetOrthographicFarClip, &SceneCamera::SetOrthographicFarClip));

        // --- CameraComponent ---
        lua.new_usertype<CameraComponent>("CameraComponent",
                                          "camera", &CameraComponent::Camera,
                                          "primary", &CameraComponent::Primary,
                                          "fixedAspectRatio", &CameraComponent::FixedAspectRatio);

        // --- SpriteRendererComponent ---
        lua.new_usertype<SpriteRendererComponent>("SpriteRendererComponent",
                                                  "color", sol::property([](const SpriteRendererComponent& c)
                                                                         { return c.Color; }, [](SpriteRendererComponent& c, const glm::vec4& v)
                                                                         { if (IsFiniteVec4(v)) c.Color = v; }),
                                                  "tilingFactor", sol::property([](const SpriteRendererComponent& c)
                                                                                { return c.TilingFactor; }, [](SpriteRendererComponent& c, f32 v)
                                                                                { if (std::isfinite(v) && v >= 0.0f) c.TilingFactor = v; }));

        // --- CircleRendererComponent ---
        lua.new_usertype<CircleRendererComponent>("CircleRendererComponent",
                                                  "color", sol::property([](const CircleRendererComponent& c)
                                                                         { return c.Color; }, [](CircleRendererComponent& c, const glm::vec4& v)
                                                                         { if (IsFiniteVec4(v)) c.Color = v; }),
                                                  "thickness", sol::property([](const CircleRendererComponent& c)
                                                                             { return c.Thickness; }, [](CircleRendererComponent& c, f32 v)
                                                                             { if (std::isfinite(v) && v >= 0.0f) c.Thickness = v; }),
                                                  "fade", sol::property([](const CircleRendererComponent& c)
                                                                        { return c.Fade; }, [](CircleRendererComponent& c, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) c.Fade = v; }));

        // --- TilemapComponent ---
        // Tiles are addressed through getTile/setTile rather than an exposed
        // vector: the biased encoding (0 = empty) and the bounds check belong in
        // one place, and a script that indexed Layers directly could resize a
        // layer out of step with Width/Height.
        lua.new_usertype<TilemapComponent>("TilemapComponent", "tilesetHandle", sol::property([](const TilemapComponent& c)
                                                                                              { return static_cast<u64>(c.TilesetHandle); }, [](TilemapComponent& c, u64 v)
                                                                                              { c.TilesetHandle = AssetHandle(v); }),
                                           "width", sol::readonly_property([](const TilemapComponent& c)
                                                                           { return c.Width; }),
                                           "height", sol::readonly_property([](const TilemapComponent& c)
                                                                            { return c.Height; }),
                                           "tileSize", sol::property([](const TilemapComponent& c)
                                                                     { return c.TileSize; }, [](TilemapComponent& c, f32 v)
                                                                     { if (std::isfinite(v) && v > 0.0f) c.TileSize = v; }),
                                           "color", sol::property([](const TilemapComponent& c)
                                                                  { return c.Color; }, [](TilemapComponent& c, const glm::vec4& v)
                                                                  { if (IsFiniteVec4(v)) c.Color = v; }),
                                           "generateColliders", &TilemapComponent::GenerateColliders, "layerCount", sol::readonly_property([](const TilemapComponent& c)
                                                                                                                                           { return static_cast<u32>(c.Layers.size()); }),
                                           "resize", [](TilemapComponent& c, u32 w, u32 h)
                                           {
                                               // A tile grid is w*h entries, so an unbounded pair from script
                                               // is an allocation request with no ceiling. 4096 per axis is
                                               // far past any authored map and still only ~16M cells.
                                               constexpr u32 kMaxTilemapAxis = 4096;
                                               if (w > kMaxTilemapAxis || h > kMaxTilemapAxis)
                                               {
                                                   OLO_CORE_WARN("[Lua Tilemap] resize({}, {}) exceeds the {} per-axis limit — ignored", w, h, kMaxTilemapAxis);
                                                   return;
                                               }
                                               c.Resize(w, h); }, "addLayer", [](TilemapComponent& c, const std::string& name)
                                           { return static_cast<u32>(c.AddLayer(name)); },
                                           // Lua indexes are 1-based by convention, but a tile grid is not a
                                           // Lua table — these stay 0-based so they match the editor's tile
                                           // coordinates and the serialized layout.
                                           "getTile", [](const TilemapComponent& c, u32 layer, u32 x, u32 y)
                                           { return c.GetTile(layer, x, y); }, "setTile", [](TilemapComponent& c, u32 layer, u32 x, u32 y, u32 value)
                                           { return c.SetTile(layer, x, y, value); });

        // --- TextComponent ---
        lua.new_usertype<TextComponent>("TextComponent",
                                        "text", &TextComponent::TextString,
                                        "color", sol::property([](const TextComponent& c)
                                                               { return c.Color; }, [](TextComponent& c, const glm::vec4& v)
                                                               { if (IsFiniteVec4(v)) c.Color = v; }),
                                        "kerning", sol::property([](const TextComponent& c)
                                                                 { return c.Kerning; }, [](TextComponent& c, f32 v)
                                                                 { if (std::isfinite(v)) c.Kerning = v; }),
                                        "lineSpacing", sol::property([](const TextComponent& c)
                                                                     { return c.LineSpacing; }, [](TextComponent& c, f32 v)
                                                                     { if (std::isfinite(v) && v >= 0.0f) c.LineSpacing = v; }),
                                        "maxWidth", sol::property([](const TextComponent& c)
                                                                  { return c.MaxWidth; }, [](TextComponent& c, f32 v)
                                                                  { if (std::isfinite(v) && v >= 0.0f) c.MaxWidth = v; }),
                                        "dropShadow", &TextComponent::DropShadow,
                                        "shadowDistance", sol::property([](const TextComponent& c)
                                                                        { return c.ShadowDistance; }, [](TextComponent& c, f32 v)
                                                                        { if (std::isfinite(v)) c.ShadowDistance = v; }),
                                        "shadowColor", sol::property([](const TextComponent& c)
                                                                     { return c.ShadowColor; }, [](TextComponent& c, const glm::vec4& v)
                                                                     { if (IsFiniteVec4(v)) c.ShadowColor = v; }));

        // --- MeshComponent ---
        lua.new_usertype<MeshComponent>("MeshComponent",
                                        "primitive", sol::property([](const MeshComponent& c) -> int
                                                                   { return std::to_underlying(c.m_Primitive); }, [](MeshComponent& c, int v)
                                                                   { if (v >= 0 && v <= 7) c.m_Primitive = static_cast<MeshPrimitive>(v); }));

        // --- InstancedMeshComponent ---
        // Scripts can drive per-instance placements at runtime (procedural
        // foliage / debris / crowds). The component's mesh source and override
        // material are authored in editor / via YAML — Lua exposes the
        // behavioural fields plus instance-list manipulation.
        lua.new_usertype<InstancedMeshComponent>("InstancedMeshComponent", "cast_shadows", &InstancedMeshComponent::CastShadows, "frustum_cull_per_instance", &InstancedMeshComponent::FrustumCullPerInstance, "cull_distance", &InstancedMeshComponent::CullDistance, "lightmap_static", &InstancedMeshComponent::LightmapStatic, "instance_count", sol::readonly_property([](const InstancedMeshComponent& c) -> int
                                                                                                                                                                                                                                                                                                                                                                            { return static_cast<int>(c.Instances.size()); }),
                                                 "clear_instances", [](InstancedMeshComponent& c)
                                                 { c.Instances.clear(); }, "add_instance", [](InstancedMeshComponent& c, f32 px, f32 py, f32 pz, f32 ex, f32 ey, f32 ez, f32 sx, f32 sy, f32 sz, f32 cr, f32 cg, f32 cb, f32 ca, f32 custom, i32 instanceEntityID)
                                                 {
                                // Fifteen raw floats straight from script. A NaN in any of
                                // them, or a zero scale component, makes the transform
                                // singular — and inst.Normal is transpose(inverse(Transform)),
                                // so the NaN would be baked into the instance buffer and
                                // spread through every lighting term that touches it.
                                const glm::vec3 position{ px, py, pz };
                                const glm::vec3 euler{ ex, ey, ez };
                                const glm::vec3 scale{ sx, sy, sz };
                                const glm::vec4 color{ cr, cg, cb, ca };
                                if (!IsFiniteVec3(position) || !IsFiniteVec3(euler) || !IsFiniteVec3(scale) ||
                                    !IsFiniteVec4(color) || !std::isfinite(custom))
                                {
                                    OLO_CORE_WARN("[Lua InstancedMesh] add_instance got a non-finite value — ignored");
                                    return;
                                }
                                if (scale.x == 0.0f || scale.y == 0.0f || scale.z == 0.0f)
                                {
                                    OLO_CORE_WARN("[Lua InstancedMesh] add_instance got a zero scale component — ignored (the transform would be singular)");
                                    return;
                                }
                                glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(px, py, pz));
                                glm::mat4 r = glm::toMat4(glm::quat(glm::vec3(ex, ey, ez)));
                                glm::mat4 s = glm::scale(glm::mat4(1.0f), glm::vec3(sx, sy, sz));
                                InstanceData inst;
                                inst.Transform = t * r * s;
                                inst.Normal = glm::transpose(glm::inverse(inst.Transform));
                                inst.PrevTransform = inst.Transform;
                                inst.Color = glm::vec4(cr, cg, cb, ca);
                                inst.Custom = custom;
                                inst.EntityID = instanceEntityID;
                                c.Instances.push_back(inst); });

        // --- UICanvasComponent ---
        lua.new_usertype<UICanvasComponent>("UICanvasComponent",
                                            "renderMode", sol::property([](const UICanvasComponent& c) -> int
                                                                        { return static_cast<int>(std::to_underlying(c.m_RenderMode)); }, [](UICanvasComponent& c, int v)
                                                                        { if (v >= 0 && v <= 1) c.m_RenderMode = static_cast<UICanvasRenderMode>(v); }),
                                            "scaleMode", sol::property([](const UICanvasComponent& c) -> int
                                                                       { return static_cast<int>(std::to_underlying(c.m_ScaleMode)); }, [](UICanvasComponent& c, int v)
                                                                       { if (v >= 0 && v <= 1) c.m_ScaleMode = static_cast<UICanvasScaleMode>(v); }),
                                            "sortOrder", &UICanvasComponent::m_SortOrder,
                                            "referenceResolution", sol::property([](const UICanvasComponent& c)
                                                                                 { return c.m_ReferenceResolution; }, [](UICanvasComponent& c, const glm::vec2& v)
                                                                                 { if (IsFiniteVec2(v)) c.m_ReferenceResolution = v; }));

        // --- UIRectTransformComponent ---
        lua.new_usertype<UIRectTransformComponent>("UIRectTransformComponent",
                                                   "anchorMin", sol::property([](const UIRectTransformComponent& c)
                                                                              { return c.m_AnchorMin; }, [](UIRectTransformComponent& c, const glm::vec2& v)
                                                                              { if (IsFiniteVec2(v)) c.m_AnchorMin = v; }),
                                                   "anchorMax", sol::property([](const UIRectTransformComponent& c)
                                                                              { return c.m_AnchorMax; }, [](UIRectTransformComponent& c, const glm::vec2& v)
                                                                              { if (IsFiniteVec2(v)) c.m_AnchorMax = v; }),
                                                   "anchoredPosition", sol::property([](const UIRectTransformComponent& c)
                                                                                     { return c.m_AnchoredPosition; }, [](UIRectTransformComponent& c, const glm::vec2& v)
                                                                                     { if (IsFiniteVec2(v)) c.m_AnchoredPosition = v; }),
                                                   "sizeDelta", sol::property([](const UIRectTransformComponent& c)
                                                                              { return c.m_SizeDelta; }, [](UIRectTransformComponent& c, const glm::vec2& v)
                                                                              { if (IsFiniteVec2(v)) c.m_SizeDelta = v; }),
                                                   "pivot", sol::property([](const UIRectTransformComponent& c)
                                                                          { return c.m_Pivot; }, [](UIRectTransformComponent& c, const glm::vec2& v)
                                                                          { if (IsFiniteVec2(v)) c.m_Pivot = v; }),
                                                   "rotation", sol::property([](const UIRectTransformComponent& c)
                                                                             { return c.m_Rotation; }, [](UIRectTransformComponent& c, f32 v)
                                                                             { if (std::isfinite(v)) c.m_Rotation = v; }),
                                                   "scale", sol::property([](const UIRectTransformComponent& c)
                                                                          { return c.m_Scale; }, [](UIRectTransformComponent& c, const glm::vec2& v)
                                                                          { if (IsFiniteVec2(v)) c.m_Scale = v; }));

        // --- UIImageComponent ---
        lua.new_usertype<UIImageComponent>("UIImageComponent",
                                           "color", sol::property([](const UIImageComponent& c)
                                                                  { return c.m_Color; }, [](UIImageComponent& c, const glm::vec4& v)
                                                                  { if (IsFiniteVec4(v)) c.m_Color = v; }),
                                           "borderInsets", sol::property([](const UIImageComponent& c)
                                                                         { return c.m_BorderInsets; }, [](UIImageComponent& c, const glm::vec4& v)
                                                                         { if (IsFiniteVec4(v) && v.x >= 0.0f && v.y >= 0.0f && v.z >= 0.0f && v.w >= 0.0f) c.m_BorderInsets = v; }));

        // --- UIPanelComponent ---
        lua.new_usertype<UIPanelComponent>("UIPanelComponent",
                                           "backgroundColor", sol::property([](const UIPanelComponent& c)
                                                                            { return c.m_BackgroundColor; }, [](UIPanelComponent& c, const glm::vec4& v)
                                                                            { if (IsFiniteVec4(v)) c.m_BackgroundColor = v; }));

        // --- UITextComponent ---
        lua.new_usertype<UITextComponent>("UITextComponent",
                                          "text", &UITextComponent::m_Text,
                                          "fontSize", sol::property([](const UITextComponent& c)
                                                                    { return c.m_FontSize; }, [](UITextComponent& c, f32 v)
                                                                    { if (std::isfinite(v) && v > 0.0f) c.m_FontSize = v; }),
                                          "color", sol::property([](const UITextComponent& c)
                                                                 { return c.m_Color; }, [](UITextComponent& c, const glm::vec4& v)
                                                                 { if (IsFiniteVec4(v)) c.m_Color = v; }),
                                          "alignment", sol::property([](const UITextComponent& c) -> int
                                                                     { return static_cast<int>(std::to_underlying(c.m_Alignment)); }, [](UITextComponent& c, int v)
                                                                     { if (v >= 0 && v <= 8) c.m_Alignment = static_cast<UITextAlignment>(v); }),
                                          "kerning", sol::property([](const UITextComponent& c)
                                                                   { return c.m_Kerning; }, [](UITextComponent& c, f32 v)
                                                                   { if (std::isfinite(v)) c.m_Kerning = v; }),
                                          "lineSpacing", sol::property([](const UITextComponent& c)
                                                                       { return c.m_LineSpacing; }, [](UITextComponent& c, f32 v)
                                                                       { if (std::isfinite(v) && v >= 0.0f) c.m_LineSpacing = v; }));

        // --- UIButtonComponent ---
        lua.new_usertype<UIButtonComponent>("UIButtonComponent",
                                            "normalColor", sol::property([](const UIButtonComponent& c)
                                                                         { return c.m_NormalColor; }, [](UIButtonComponent& c, const glm::vec4& v)
                                                                         { if (IsFiniteVec4(v)) c.m_NormalColor = v; }),
                                            "hoveredColor", sol::property([](const UIButtonComponent& c)
                                                                          { return c.m_HoveredColor; }, [](UIButtonComponent& c, const glm::vec4& v)
                                                                          { if (IsFiniteVec4(v)) c.m_HoveredColor = v; }),
                                            "pressedColor", sol::property([](const UIButtonComponent& c)
                                                                          { return c.m_PressedColor; }, [](UIButtonComponent& c, const glm::vec4& v)
                                                                          { if (IsFiniteVec4(v)) c.m_PressedColor = v; }),
                                            "disabledColor", sol::property([](const UIButtonComponent& c)
                                                                           { return c.m_DisabledColor; }, [](UIButtonComponent& c, const glm::vec4& v)
                                                                           { if (IsFiniteVec4(v)) c.m_DisabledColor = v; }),
                                            "interactable", &UIButtonComponent::m_Interactable,
                                            "state", sol::readonly(&UIButtonComponent::m_State));

        // --- UISliderComponent ---
        lua.new_usertype<UISliderComponent>("UISliderComponent",
                                            "value", sol::property([](const UISliderComponent& c)
                                                                   { return c.m_Value; }, [](UISliderComponent& c, f32 v)
                                                                   { if (std::isfinite(v)) c.m_Value = v; }),
                                            "minValue", sol::property([](const UISliderComponent& c)
                                                                      { return c.m_MinValue; }, [](UISliderComponent& c, f32 v)
                                                                      { if (std::isfinite(v)) c.m_MinValue = v; }),
                                            "maxValue", sol::property([](const UISliderComponent& c)
                                                                      { return c.m_MaxValue; }, [](UISliderComponent& c, f32 v)
                                                                      { if (std::isfinite(v)) c.m_MaxValue = v; }),
                                            "direction", sol::property([](const UISliderComponent& c) -> int
                                                                       { return static_cast<int>(std::to_underlying(c.m_Direction)); }, [](UISliderComponent& c, int v)
                                                                       { if (v >= 0 && v <= 3) c.m_Direction = static_cast<UISliderDirection>(v); }),
                                            "backgroundColor", sol::property([](const UISliderComponent& c)
                                                                             { return c.m_BackgroundColor; }, [](UISliderComponent& c, const glm::vec4& v)
                                                                             { if (IsFiniteVec4(v)) c.m_BackgroundColor = v; }),
                                            "fillColor", sol::property([](const UISliderComponent& c)
                                                                       { return c.m_FillColor; }, [](UISliderComponent& c, const glm::vec4& v)
                                                                       { if (IsFiniteVec4(v)) c.m_FillColor = v; }),
                                            "handleColor", sol::property([](const UISliderComponent& c)
                                                                         { return c.m_HandleColor; }, [](UISliderComponent& c, const glm::vec4& v)
                                                                         { if (IsFiniteVec4(v)) c.m_HandleColor = v; }),
                                            "interactable", &UISliderComponent::m_Interactable);

        // --- UICheckboxComponent ---
        lua.new_usertype<UICheckboxComponent>("UICheckboxComponent",
                                              "isChecked", &UICheckboxComponent::m_IsChecked,
                                              "uncheckedColor", sol::property([](const UICheckboxComponent& c)
                                                                              { return c.m_UncheckedColor; }, [](UICheckboxComponent& c, const glm::vec4& v)
                                                                              { if (IsFiniteVec4(v)) c.m_UncheckedColor = v; }),
                                              "checkedColor", sol::property([](const UICheckboxComponent& c)
                                                                            { return c.m_CheckedColor; }, [](UICheckboxComponent& c, const glm::vec4& v)
                                                                            { if (IsFiniteVec4(v)) c.m_CheckedColor = v; }),
                                              "checkmarkColor", sol::property([](const UICheckboxComponent& c)
                                                                              { return c.m_CheckmarkColor; }, [](UICheckboxComponent& c, const glm::vec4& v)
                                                                              { if (IsFiniteVec4(v)) c.m_CheckmarkColor = v; }),
                                              "interactable", &UICheckboxComponent::m_Interactable);

        // --- UIProgressBarComponent ---
        lua.new_usertype<UIProgressBarComponent>("UIProgressBarComponent",
                                                 "value", sol::property([](const UIProgressBarComponent& c)
                                                                        { return c.m_Value; }, [](UIProgressBarComponent& c, f32 v)
                                                                        { if (std::isfinite(v)) c.m_Value = v; }),
                                                 "minValue", sol::property([](const UIProgressBarComponent& c)
                                                                           { return c.m_MinValue; }, [](UIProgressBarComponent& c, f32 v)
                                                                           { if (std::isfinite(v)) c.m_MinValue = v; }),
                                                 "maxValue", sol::property([](const UIProgressBarComponent& c)
                                                                           { return c.m_MaxValue; }, [](UIProgressBarComponent& c, f32 v)
                                                                           { if (std::isfinite(v)) c.m_MaxValue = v; }),
                                                 "fillMethod", sol::property([](const UIProgressBarComponent& c) -> int
                                                                             { return static_cast<int>(std::to_underlying(c.m_FillMethod)); }, [](UIProgressBarComponent& c, int v)
                                                                             { if (v >= 0 && v <= 1) c.m_FillMethod = static_cast<UIFillMethod>(v); }),
                                                 "backgroundColor", sol::property([](const UIProgressBarComponent& c)
                                                                                  { return c.m_BackgroundColor; }, [](UIProgressBarComponent& c, const glm::vec4& v)
                                                                                  { if (IsFiniteVec4(v)) c.m_BackgroundColor = v; }),
                                                 "fillColor", sol::property([](const UIProgressBarComponent& c)
                                                                            { return c.m_FillColor; }, [](UIProgressBarComponent& c, const glm::vec4& v)
                                                                            { if (IsFiniteVec4(v)) c.m_FillColor = v; }));

        // --- UIInputFieldComponent ---
        lua.new_usertype<UIInputFieldComponent>("UIInputFieldComponent",
                                                "text", &UIInputFieldComponent::m_Text,
                                                "placeholder", &UIInputFieldComponent::m_Placeholder,
                                                "fontSize", sol::property([](const UIInputFieldComponent& c)
                                                                          { return c.m_FontSize; }, [](UIInputFieldComponent& c, f32 v)
                                                                          { if (std::isfinite(v) && v > 0.0f) c.m_FontSize = v; }),
                                                "textColor", sol::property([](const UIInputFieldComponent& c)
                                                                           { return c.m_TextColor; }, [](UIInputFieldComponent& c, const glm::vec4& v)
                                                                           { if (IsFiniteVec4(v)) c.m_TextColor = v; }),
                                                "placeholderColor", sol::property([](const UIInputFieldComponent& c)
                                                                                  { return c.m_PlaceholderColor; }, [](UIInputFieldComponent& c, const glm::vec4& v)
                                                                                  { if (IsFiniteVec4(v)) c.m_PlaceholderColor = v; }),
                                                "backgroundColor", sol::property([](const UIInputFieldComponent& c)
                                                                                 { return c.m_BackgroundColor; }, [](UIInputFieldComponent& c, const glm::vec4& v)
                                                                                 { if (IsFiniteVec4(v)) c.m_BackgroundColor = v; }),
                                                "characterLimit", &UIInputFieldComponent::m_CharacterLimit,
                                                "interactable", &UIInputFieldComponent::m_Interactable);

        // --- UIScrollViewComponent ---
        lua.new_usertype<UIScrollViewComponent>("UIScrollViewComponent",
                                                "scrollPosition", sol::property([](const UIScrollViewComponent& c)
                                                                                { return c.m_ScrollPosition; }, [](UIScrollViewComponent& c, const glm::vec2& v)
                                                                                { if (IsFiniteVec2(v)) c.m_ScrollPosition = v; }),
                                                "contentSize", sol::property([](const UIScrollViewComponent& c)
                                                                             { return c.m_ContentSize; }, [](UIScrollViewComponent& c, const glm::vec2& v)
                                                                             { if (IsFiniteVec2(v)) c.m_ContentSize = v; }),
                                                "scrollDirection", sol::property([](const UIScrollViewComponent& c) -> int
                                                                                 { return static_cast<int>(std::to_underlying(c.m_ScrollDirection)); }, [](UIScrollViewComponent& c, int v)
                                                                                 { if (v >= 0 && v <= 2) c.m_ScrollDirection = static_cast<UIScrollDirection>(v); }),
                                                "scrollSpeed", sol::property([](const UIScrollViewComponent& c)
                                                                             { return c.m_ScrollSpeed; }, [](UIScrollViewComponent& c, f32 v)
                                                                             { if (std::isfinite(v) && v >= 0.0f) c.m_ScrollSpeed = v; }),
                                                "showHorizontalScrollbar", &UIScrollViewComponent::m_ShowHorizontalScrollbar,
                                                "showVerticalScrollbar", &UIScrollViewComponent::m_ShowVerticalScrollbar,
                                                "scrollbarColor", sol::property([](const UIScrollViewComponent& c)
                                                                                { return c.m_ScrollbarColor; }, [](UIScrollViewComponent& c, const glm::vec4& v)
                                                                                { if (IsFiniteVec4(v)) c.m_ScrollbarColor = v; }),
                                                "scrollbarTrackColor", sol::property([](const UIScrollViewComponent& c)
                                                                                     { return c.m_ScrollbarTrackColor; }, [](UIScrollViewComponent& c, const glm::vec4& v)
                                                                                     { if (IsFiniteVec4(v)) c.m_ScrollbarTrackColor = v; }));

        // --- UIDropdownComponent ---
        lua.new_usertype<UIDropdownComponent>("UIDropdownComponent",
                                              "selectedIndex", &UIDropdownComponent::m_SelectedIndex,
                                              "backgroundColor", sol::property([](const UIDropdownComponent& c)
                                                                               { return c.m_BackgroundColor; }, [](UIDropdownComponent& c, const glm::vec4& v)
                                                                               { if (IsFiniteVec4(v)) c.m_BackgroundColor = v; }),
                                              "highlightColor", sol::property([](const UIDropdownComponent& c)
                                                                              { return c.m_HighlightColor; }, [](UIDropdownComponent& c, const glm::vec4& v)
                                                                              { if (IsFiniteVec4(v)) c.m_HighlightColor = v; }),
                                              "textColor", sol::property([](const UIDropdownComponent& c)
                                                                         { return c.m_TextColor; }, [](UIDropdownComponent& c, const glm::vec4& v)
                                                                         { if (IsFiniteVec4(v)) c.m_TextColor = v; }),
                                              "fontSize", sol::property([](const UIDropdownComponent& c)
                                                                        { return c.m_FontSize; }, [](UIDropdownComponent& c, f32 v)
                                                                        { if (std::isfinite(v) && v > 0.0f) c.m_FontSize = v; }),
                                              "itemHeight", sol::property([](const UIDropdownComponent& c)
                                                                          { return c.m_ItemHeight; }, [](UIDropdownComponent& c, f32 v)
                                                                          { if (std::isfinite(v) && v > 0.0f) c.m_ItemHeight = v; }),
                                              "interactable", &UIDropdownComponent::m_Interactable);

        // --- UIGridLayoutComponent ---
        lua.new_usertype<UIGridLayoutComponent>("UIGridLayoutComponent",
                                                "cellSize", sol::property([](const UIGridLayoutComponent& c)
                                                                          { return c.m_CellSize; }, [](UIGridLayoutComponent& c, const glm::vec2& v)
                                                                          { if (IsFiniteVec2(v) && v.x >= 0.0f && v.y >= 0.0f) c.m_CellSize = v; }),
                                                "spacing", sol::property([](const UIGridLayoutComponent& c)
                                                                         { return c.m_Spacing; }, [](UIGridLayoutComponent& c, const glm::vec2& v)
                                                                         { if (IsFiniteVec2(v)) c.m_Spacing = v; }),
                                                "padding", sol::property([](const UIGridLayoutComponent& c)
                                                                         { return c.m_Padding; }, [](UIGridLayoutComponent& c, const glm::vec4& v)
                                                                         { if (IsFiniteVec4(v) && v.x >= 0.0f && v.y >= 0.0f && v.z >= 0.0f && v.w >= 0.0f) c.m_Padding = v; }),
                                                "startCorner", sol::property([](const UIGridLayoutComponent& c) -> int
                                                                             { return static_cast<int>(std::to_underlying(c.m_StartCorner)); }, [](UIGridLayoutComponent& c, int v)
                                                                             { if (v >= 0 && v <= 3) c.m_StartCorner = static_cast<UIGridLayoutStartCorner>(v); }),
                                                "startAxis", sol::property([](const UIGridLayoutComponent& c) -> int
                                                                           { return static_cast<int>(std::to_underlying(c.m_StartAxis)); }, [](UIGridLayoutComponent& c, int v)
                                                                           { if (v >= 0 && v <= 1) c.m_StartAxis = static_cast<UIGridLayoutAxis>(v); }),
                                                "constraintCount", &UIGridLayoutComponent::m_ConstraintCount);

        // --- UIToggleComponent ---
        lua.new_usertype<UIToggleComponent>("UIToggleComponent",
                                            "isOn", &UIToggleComponent::m_IsOn,
                                            "offColor", sol::property([](const UIToggleComponent& c)
                                                                      { return c.m_OffColor; }, [](UIToggleComponent& c, const glm::vec4& v)
                                                                      { if (IsFiniteVec4(v)) c.m_OffColor = v; }),
                                            "onColor", sol::property([](const UIToggleComponent& c)
                                                                     { return c.m_OnColor; }, [](UIToggleComponent& c, const glm::vec4& v)
                                                                     { if (IsFiniteVec4(v)) c.m_OnColor = v; }),
                                            "knobColor", sol::property([](const UIToggleComponent& c)
                                                                       { return c.m_KnobColor; }, [](UIToggleComponent& c, const glm::vec4& v)
                                                                       { if (IsFiniteVec4(v)) c.m_KnobColor = v; }),
                                            "interactable", &UIToggleComponent::m_Interactable);
    }
} // namespace OloEngine
