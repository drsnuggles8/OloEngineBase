#include "SceneHierarchyPanel.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/UI/UI.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>

namespace OloEngine
{
    SceneHierarchyPanel::SceneHierarchyPanel(const Ref<Scene>& context)
    {
        SetContext(context);
    }

    void SceneHierarchyPanel::SetContext(const Ref<Scene>& context)
    {
        m_Context = context;
        m_SelectionContext = {};
    }

    void SceneHierarchyPanel::OnImGuiRender()
    {
        ImGui::Begin("Scene Hierarchy");

        if (m_Context)
        {
            m_Context->m_Registry.view<entt::entity>().each([&](const auto e)
                                                            { DrawEntityNode({ e, *m_Context }); });

            if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered())
            {
                m_SelectionContext = {};
            }

            // Right-click on blank space
            if (ImGui::BeginPopupContextWindow(nullptr, 1))
            {
                if (ImGui::MenuItem("Create Empty Entity"))
                {
                    m_SelectionContext = m_Context->CreateEntity("Empty Entity");
                }

                if (ImGui::BeginMenu("Create UI"))
                {
                    if (ImGui::MenuItem("UI Canvas"))
                    {
                        auto canvas = m_Context->CreateEntity("UI Canvas");
                        canvas.AddComponent<UICanvasComponent>();
                        canvas.AddComponent<UIRectTransformComponent>();
                        m_SelectionContext = canvas;
                    }

                    ImGui::Separator();

                    if (ImGui::MenuItem("Panel"))
                    {
                        auto widget = CreateUIWidget("UI Panel");
                        widget.AddComponent<UIPanelComponent>();
                        m_SelectionContext = widget;
                    }

                    if (ImGui::MenuItem("Text"))
                    {
                        auto widget = CreateUIWidget("UI Text");
                        widget.AddComponent<UITextComponent>();
                        m_SelectionContext = widget;
                    }

                    if (ImGui::MenuItem("Image"))
                    {
                        auto widget = CreateUIWidget("UI Image");
                        widget.AddComponent<UIImageComponent>();
                        m_SelectionContext = widget;
                    }

                    if (ImGui::MenuItem("Button"))
                    {
                        auto widget = CreateUIWidget("UI Button");
                        widget.AddComponent<UIButtonComponent>();
                        m_SelectionContext = widget;
                    }

                    if (ImGui::MenuItem("Slider"))
                    {
                        auto widget = CreateUIWidget("UI Slider");
                        widget.AddComponent<UISliderComponent>();
                        m_SelectionContext = widget;
                    }

                    if (ImGui::MenuItem("Checkbox"))
                    {
                        auto widget = CreateUIWidget("UI Checkbox");
                        widget.AddComponent<UICheckboxComponent>();
                        m_SelectionContext = widget;
                    }

                    if (ImGui::MenuItem("Toggle"))
                    {
                        auto widget = CreateUIWidget("UI Toggle");
                        widget.AddComponent<UIToggleComponent>();
                        m_SelectionContext = widget;
                    }

                    if (ImGui::MenuItem("Progress Bar"))
                    {
                        auto widget = CreateUIWidget("UI Progress Bar");
                        widget.AddComponent<UIProgressBarComponent>();
                        m_SelectionContext = widget;
                    }

                    if (ImGui::MenuItem("Input Field"))
                    {
                        auto widget = CreateUIWidget("UI Input Field");
                        widget.AddComponent<UIInputFieldComponent>();
                        m_SelectionContext = widget;
                    }

                    if (ImGui::MenuItem("Dropdown"))
                    {
                        auto widget = CreateUIWidget("UI Dropdown");
                        widget.AddComponent<UIDropdownComponent>();
                        m_SelectionContext = widget;
                    }

                    if (ImGui::MenuItem("Scroll View"))
                    {
                        auto widget = CreateUIWidget("UI Scroll View");
                        widget.AddComponent<UIScrollViewComponent>();
                        m_SelectionContext = widget;
                    }

                    if (ImGui::MenuItem("Grid Layout"))
                    {
                        auto widget = CreateUIWidget("UI Grid Layout");
                        widget.AddComponent<UIGridLayoutComponent>();
                        m_SelectionContext = widget;
                    }

                    ImGui::EndMenu();
                }

                ImGui::EndPopup();
            }
        }

        ImGui::End();

        ImGui::Begin("Properties");
        if (m_SelectionContext)
        {
            DrawComponents(m_SelectionContext);
        }

        ImGui::End();
    }

    void SceneHierarchyPanel::SetSelectedEntity(const Entity entity)
    {
        m_SelectionContext = entity;
    }

    Entity SceneHierarchyPanel::FindOrCreateCanvas()
    {
        OLO_PROFILE_FUNCTION();
        // Look for an existing canvas entity
        auto view = m_Context->GetAllEntitiesWith<UICanvasComponent>();
        if (auto it = view.begin(); it != view.end())
        {
            return Entity{ *it, m_Context.get() };
        }

        // None found — create a new canvas
        auto canvas = m_Context->CreateEntity("UI Canvas");
        canvas.AddComponent<UICanvasComponent>();
        canvas.AddComponent<UIRectTransformComponent>();
        return canvas;
    }

    Entity SceneHierarchyPanel::CreateUIWidget(const std::string& name)
    {
        OLO_PROFILE_FUNCTION();
        Entity canvas = FindOrCreateCanvas();
        auto widget = m_Context->CreateEntity(name);
        widget.AddComponent<UIRectTransformComponent>();
        widget.SetParent(canvas);
        return widget;
    }

    void SceneHierarchyPanel::DrawEntityNode(Entity entity)
    {
        auto& tagComponent = entity.GetComponent<TagComponent>();
        auto& tag = tagComponent.Tag;

        ImGuiTreeNodeFlags flags = ((m_SelectionContext == entity) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_OpenOnArrow;
        flags |= ImGuiTreeNodeFlags_SpanAvailWidth;
        bool opened = ImGui::TreeNodeEx((void*)static_cast<u64>(static_cast<u32>(entity)), flags, tag.c_str());
        if (ImGui::IsItemClicked())
        {
            m_SelectionContext = entity;
        }

        bool entityDeleted = false;
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("Rename"))
            {
                tagComponent.renaming = true;
            }

            if (ImGui::MenuItem("Delete Entity"))
            {
                entityDeleted = true;
            }

            ImGui::EndPopup();
        }

        if (tagComponent.renaming)
        {
            char buffer[256];
            ::memset(buffer, 0, sizeof(buffer));
            ::strncpy_s(buffer, tag.c_str(), sizeof(buffer));
            if (ImGui::InputText("##Tag", buffer, sizeof(buffer)))
            {
                tag = std::string(buffer);
            }

            if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered())
            {
                tagComponent.renaming = false;
            }
        }

        if (opened)
        {
            flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            opened = ImGui::TreeNodeEx((void*)9817239, flags, tag.c_str());
            if (opened)
            {
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }

        if (entityDeleted)
        {
            m_Context->DestroyEntity(entity);
            if (m_SelectionContext == entity)
            {
                m_SelectionContext = {};
            }
        }
    }

    static void DrawVec3Control(const std::string& label, glm::vec3& values, const f32 resetValue = 0.0f, const f32 columnWidth = 100.0f)
    {
        ImGuiIO& io = ImGui::GetIO();
        const auto boldFont = io.Fonts->Fonts[0];

        ImGui::PushID(label.c_str());

        ImGui::Columns(2);
        ImGui::SetColumnWidth(0, columnWidth);
        ImGui::Text(label.c_str());
        ImGui::NextColumn();

        ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ 0, 0 });

        const f32 lineHeight = ImGui::GetFontSize() + (::GImGui->Style.FramePadding.y * 2.0f);
        const ImVec2 buttonSize = { lineHeight + 3.0f, lineHeight };

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.8f, 0.1f, 0.15f, 1.0f });
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{ 0.9f, 0.2f, 0.2f, 1.0f });
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{ 0.8f, 0.1f, 0.15f, 1.0f });
        ImGui::PushFont(boldFont);
        if (ImGui::Button("X", buttonSize))
        {
            values.x = resetValue;
        }
        ImGui::PopFont();
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        ImGui::DragFloat("##X", &values.x, 0.1f, 0.0f, 0.0f, "%.2f");
        ImGui::PopItemWidth();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.2f, 0.7f, 0.2f, 1.0f });
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{ 0.3f, 0.8f, 0.3f, 1.0f });
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{ 0.2f, 0.7f, 0.2f, 1.0f });
        ImGui::PushFont(boldFont);
        if (ImGui::Button("Y", buttonSize))
        {
            values.y = resetValue;
        }
        ImGui::PopFont();
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        ImGui::DragFloat("##Y", &values.y, 0.1f, 0.0f, 0.0f, "%.2f");
        ImGui::PopItemWidth();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.1f, 0.25f, 0.8f, 1.0f });
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{ 0.2f, 0.35f, 0.9f, 1.0f });
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{ 0.1f, 0.25f, 0.8f, 1.0f });
        ImGui::PushFont(boldFont);
        if (ImGui::Button("Z", buttonSize))
        {
            values.z = resetValue;
        }
        ImGui::PopFont();
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        ImGui::DragFloat("##Z", &values.z, 0.1f, 0.0f, 0.0f, "%.2f");
        ImGui::PopItemWidth();

        ImGui::PopStyleVar();

        ImGui::Columns(1);

        ImGui::PopID();
    }

    // ── Curve editor widget for ParticleCurve ──────────────────────────────
    static bool DrawParticleCurveEditor(const char* label, ParticleCurve& curve,
                                        f32 valueMin = 0.0f, f32 valueMax = 1.0f)
    {
        bool modified = false;
        ImGui::PushID(label);

        const f32 canvasWidth = ImGui::GetContentRegionAvail().x;
        constexpr f32 canvasHeight = 100.0f;
        const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize(canvasWidth, canvasHeight);

        ImGui::InvisibleButton("##curve_canvas", canvasSize);
        const bool isHovered = ImGui::IsItemHovered();
        const bool isActive  = ImGui::IsItemActive();

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 canvasEnd(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);

        // Background + border
        drawList->AddRectFilled(canvasPos, canvasEnd, IM_COL32(30, 30, 30, 255));
        drawList->AddRect(canvasPos, canvasEnd, IM_COL32(80, 80, 80, 255));

        // Grid lines (quarters)
        for (int i = 1; i < 4; ++i)
        {
            f32 x = canvasPos.x + canvasSize.x * (static_cast<f32>(i) / 4.0f);
            f32 y = canvasPos.y + canvasSize.y * (static_cast<f32>(i) / 4.0f);
            drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasEnd.y), IM_COL32(50, 50, 50, 255));
            drawList->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasEnd.x, y), IM_COL32(50, 50, 50, 255));
        }

        const f32 valueRange = valueMax - valueMin;
        auto toScreen = [&](f32 time, f32 value) -> ImVec2
        {
            f32 ny = (valueRange > 0.0f) ? (value - valueMin) / valueRange : 0.5f;
            return { canvasPos.x + time * canvasSize.x,
                     canvasPos.y + (1.0f - ny) * canvasSize.y };
        };
        auto fromScreen = [&](ImVec2 screen) -> std::pair<f32, f32>
        {
            f32 t = (canvasSize.x > 0.0f) ? (screen.x - canvasPos.x) / canvasSize.x : 0.0f;
            f32 ny = (canvasSize.y > 0.0f) ? 1.0f - (screen.y - canvasPos.y) / canvasSize.y : 0.0f;
            return { std::clamp(t, 0.0f, 1.0f),
                     std::clamp(valueMin + ny * valueRange, valueMin, valueMax) };
        };

        // Draw curve as polyline
        if (curve.KeyCount > 0)
        {
            constexpr int numSegments = 128;
            ImVec2 prev = toScreen(0.0f, curve.Evaluate(0.0f));
            for (int s = 1; s <= numSegments; ++s)
            {
                f32 t = static_cast<f32>(s) / static_cast<f32>(numSegments);
                ImVec2 cur = toScreen(t, curve.Evaluate(t));
                drawList->AddLine(prev, cur, IM_COL32(220, 220, 80, 255), 1.5f);
                prev = cur;
            }
        }

        // Per-widget drag state stored via static + pointer guard
        static int sDragKey = -1;
        static const void* sDragOwner = nullptr;

        constexpr f32 keyRadius = 5.0f;
        const ImVec2 mousePos = ImGui::GetIO().MousePos;

        // Draw key points and detect hover
        int hoveredKey = -1;
        for (u32 k = 0; k < curve.KeyCount; ++k)
        {
            ImVec2 ks = toScreen(curve.Keys[k].Time, curve.Keys[k].Value);
            if (std::abs(mousePos.x - ks.x) <= keyRadius + 2.0f &&
                std::abs(mousePos.y - ks.y) <= keyRadius + 2.0f)
            {
                hoveredKey = static_cast<int>(k);
            }
            ImU32 col = (hoveredKey == static_cast<int>(k))
                            ? IM_COL32(255, 255, 100, 255)
                            : IM_COL32(220, 220, 220, 255);
            drawList->AddCircleFilled(ks, keyRadius, col);
            drawList->AddCircle(ks, keyRadius, IM_COL32(100, 100, 100, 255));
        }

        // Start drag on left-click
        if (isHovered && hoveredKey >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            sDragKey = hoveredKey;
            sDragOwner = &curve;
        }

        // Drag
        if (sDragKey >= 0 && sDragOwner == &curve && isActive)
        {
            auto [time, value] = fromScreen(mousePos);
            auto dk = static_cast<u32>(sDragKey);
            if (dk == 0)
                time = 0.0f;
            else if (dk == curve.KeyCount - 1)
                time = 1.0f;
            else
                time = std::clamp(time,
                                  curve.Keys[dk - 1].Time + 0.001f,
                                  curve.Keys[dk + 1].Time - 0.001f);
            curve.Keys[dk] = { time, value };
            modified = true;
        }

        // Release drag
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && sDragOwner == &curve)
        {
            sDragKey = -1;
            sDragOwner = nullptr;
        }

        // Right-click: remove key (keep at least 2)
        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hoveredKey >= 0
            && curve.KeyCount > 2)
        {
            auto rk = static_cast<u32>(hoveredKey);
            for (u32 j = rk; j < curve.KeyCount - 1; ++j)
                curve.Keys[j] = curve.Keys[j + 1];
            curve.KeyCount--;
            modified = true;
        }

        // Double-click on empty area: add key (max 8)
        if (isHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
            && hoveredKey < 0 && curve.KeyCount < 8)
        {
            auto [time, value] = fromScreen(mousePos);
            u32 insertIdx = curve.KeyCount;
            for (u32 k = 0; k < curve.KeyCount; ++k)
            {
                if (time < curve.Keys[k].Time)
                {
                    insertIdx = k;
                    break;
                }
            }
            for (u32 k = curve.KeyCount; k > insertIdx; --k)
                curve.Keys[k] = curve.Keys[k - 1];
            curve.Keys[insertIdx] = { time, value };
            curve.KeyCount++;
            modified = true;
        }

        // Value labels at corners
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", valueMax);
        drawList->AddText(ImVec2(canvasPos.x + 2, canvasPos.y + 1), IM_COL32(120, 120, 120, 255), buf);
        snprintf(buf, sizeof(buf), "%.2f", valueMin);
        drawList->AddText(ImVec2(canvasPos.x + 2, canvasEnd.y - ImGui::GetFontSize() - 1),
                          IM_COL32(120, 120, 120, 255), buf);

        ImGui::TextDisabled("%s  (dbl-click: add key, right-click: remove key)", label);

        ImGui::PopID();
        return modified;
    }

    // ── Gradient preview bar for ParticleCurve4 ────────────────────────────
    static void DrawGradientBar(const ParticleCurve4& curve, f32 width, f32 height)
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        constexpr int segments = 64;
        f32 segW = width / static_cast<f32>(segments);

        for (int i = 0; i < segments; ++i)
        {
            f32 t0 = static_cast<f32>(i) / static_cast<f32>(segments);
            f32 t1 = static_cast<f32>(i + 1) / static_cast<f32>(segments);
            glm::vec4 c0 = curve.Evaluate(t0);
            glm::vec4 c1 = curve.Evaluate(t1);
            auto toCol = [](const glm::vec4& c) -> ImU32
            {
                return IM_COL32(static_cast<int>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f),
                                static_cast<int>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f),
                                static_cast<int>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f),
                                static_cast<int>(std::clamp(c.a, 0.0f, 1.0f) * 255.0f));
            };
            ImVec2 p0(pos.x + segW * static_cast<f32>(i), pos.y);
            ImVec2 p1(pos.x + segW * static_cast<f32>(i + 1), pos.y + height);
            drawList->AddRectFilledMultiColor(p0, p1, toCol(c0), toCol(c1), toCol(c1), toCol(c0));
        }
        drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(80, 80, 80, 255));
        ImGui::Dummy(ImVec2(width, height));
    }

    // ── Combined color curve editor for ParticleCurve4 ─────────────────────
    static bool DrawParticleCurve4Editor(const char* label, ParticleCurve4& curve)
    {
        bool modified = false;
        ImGui::PushID(label);

        // Gradient preview
        DrawGradientBar(curve, ImGui::GetContentRegionAvail().x, 20.0f);

        // Per-channel curve editors in tree nodes
        struct ChannelInfo { const char* name; ParticleCurve* ch; ImU32 lineColor; };
        ChannelInfo channels[] = {
            { "Red",   &curve.R, IM_COL32(255, 80, 80, 255) },
            { "Green", &curve.G, IM_COL32(80, 255, 80, 255) },
            { "Blue",  &curve.B, IM_COL32(80, 130, 255, 255) },
            { "Alpha", &curve.A, IM_COL32(200, 200, 200, 255) },
        };
        for (auto& [name, ch, lineColor] : channels)
        {
            if (ImGui::TreeNode(name))
            {
                modified |= DrawParticleCurveEditor(name, *ch, 0.0f, 1.0f);
                ImGui::TreePop();
            }
        }

        ImGui::PopID();
        return modified;
    }

    template<typename T, typename UIFunction>
    static void DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction)
    {
        const ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_FramePadding;
        if (entity.HasComponent<T>())
        {
            static char imguiPopupID[64];
            ::sprintf_s(imguiPopupID, 64, "ComponentSettings%s", typeid(T).name());
            ImGui::PushID(imguiPopupID);

            auto& component = entity.GetComponent<T>();
            const ImVec2 contentRegionAvailable = ImGui::GetContentRegionAvail();

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{ 4, 4 });
            const f32 lineHeight = ImGui::GetFontSize() + (::GImGui->Style.FramePadding.y * 2.0f);
            ImGui::Separator();
            const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(typeid(T).hash_code()), treeNodeFlags, name.c_str());
            ImGui::PopStyleVar();
            ImGui::SameLine(contentRegionAvailable.x - (lineHeight * 0.5f));
            if (ImGui::Button("+", ImVec2{ lineHeight, lineHeight }))
            {
                ImGui::OpenPopup("ComponentSettings");
            }

            bool removeComponent = false;
            if (ImGui::BeginPopup("ComponentSettings"))
            {
                if (ImGui::MenuItem("Remove component"))
                {
                    removeComponent = true;
                }

                ImGui::EndPopup();
            }

            ImGui::PopID();

            if (open)
            {
                uiFunction(component);
                ImGui::TreePop();
            }

            if (removeComponent)
            {
                entity.RemoveComponent<T>();
            }
        }
    }

    void SceneHierarchyPanel::DrawComponents(Entity entity)
    {
        if (entity.HasComponent<TagComponent>())
        {
            auto& tag = entity.GetComponent<TagComponent>().Tag;

            char buffer[256];
            ::memset(buffer, 0, sizeof(buffer));
            ::strncpy_s(buffer, sizeof(buffer), tag.c_str(), sizeof(buffer));
            if (ImGui::InputText("##Tag", buffer, sizeof(buffer)))
            {
                tag = std::string(buffer);
            }
        }

        ImGui::SameLine();
        ImGui::PushItemWidth(-1);

        if (ImGui::Button("Add Component"))
        {
            ImGui::OpenPopup("AddComponent");
        }

        if (ImGui::BeginPopup("AddComponent"))
        {
            DisplayAddComponentEntry<CameraComponent>("Camera");
            DisplayAddComponentEntry<ScriptComponent>("Script");
            DisplayAddComponentEntry<SpriteRendererComponent>("Sprite Renderer");
            DisplayAddComponentEntry<CircleRendererComponent>("Circle Renderer");
            DisplayAddComponentEntry<Rigidbody2DComponent>("Rigidbody 2D");
            DisplayAddComponentEntry<BoxCollider2DComponent>("Box Collider 2D");
            DisplayAddComponentEntry<CircleCollider2DComponent>("Circle Collider 2D");
            DisplayAddComponentEntry<TextComponent>("Text Component");

            ImGui::Separator();

            // 3D Components
            DisplayAddComponentEntry<MeshComponent>("Mesh");
            DisplayAddComponentEntry<ModelComponent>("Model (with Materials)");
            DisplayAddComponentEntry<MaterialComponent>("Material");
            DisplayAddComponentEntry<DirectionalLightComponent>("Directional Light");
            DisplayAddComponentEntry<PointLightComponent>("Point Light");
            DisplayAddComponentEntry<SpotLightComponent>("Spot Light");
            DisplayAddComponentEntry<EnvironmentMapComponent>("Environment Map (Skybox/IBL)");

            ImGui::Separator();

            // 3D Physics Components
            DisplayAddComponentEntry<Rigidbody3DComponent>("Rigidbody 3D");
            DisplayAddComponentEntry<BoxCollider3DComponent>("Box Collider 3D");
            DisplayAddComponentEntry<SphereCollider3DComponent>("Sphere Collider 3D");
            DisplayAddComponentEntry<CapsuleCollider3DComponent>("Capsule Collider 3D");
            DisplayAddComponentEntry<MeshCollider3DComponent>("Mesh Collider 3D");
            DisplayAddComponentEntry<ConvexMeshCollider3DComponent>("Convex Mesh Collider 3D");
            DisplayAddComponentEntry<TriangleMeshCollider3DComponent>("Triangle Mesh Collider 3D");
            DisplayAddComponentEntry<CharacterController3DComponent>("Character Controller 3D");

            ImGui::Separator();

            // Audio Components
            DisplayAddComponentEntry<AudioSourceComponent>("Audio Source");
            DisplayAddComponentEntry<AudioListenerComponent>("Audio Listener");

            ImGui::Separator();

            // Particle System
            DisplayAddComponentEntry<ParticleSystemComponent>("Particle System");

            ImGui::Separator();

            // Animation Components
            DisplayAddComponentEntry<AnimationStateComponent>("Animation State");
            DisplayAddComponentEntry<SkeletonComponent>("Skeleton");
            DisplayAddComponentEntry<SubmeshComponent>("Submesh");

            ImGui::Separator();

            // UI Components
            DisplayAddComponentEntry<UICanvasComponent>("UI Canvas");
            DisplayAddComponentEntry<UIRectTransformComponent>("UI Rect Transform");
            DisplayAddComponentEntry<UIPanelComponent>("UI Panel");
            DisplayAddComponentEntry<UIImageComponent>("UI Image");
            DisplayAddComponentEntry<UITextComponent>("UI Text");
            DisplayAddComponentEntry<UIButtonComponent>("UI Button");
            DisplayAddComponentEntry<UISliderComponent>("UI Slider");
            DisplayAddComponentEntry<UICheckboxComponent>("UI Checkbox");
            DisplayAddComponentEntry<UIProgressBarComponent>("UI Progress Bar");
            DisplayAddComponentEntry<UIInputFieldComponent>("UI Input Field");
            DisplayAddComponentEntry<UIScrollViewComponent>("UI Scroll View");
            DisplayAddComponentEntry<UIDropdownComponent>("UI Dropdown");
            DisplayAddComponentEntry<UIGridLayoutComponent>("UI Grid Layout");
            DisplayAddComponentEntry<UIToggleComponent>("UI Toggle");

            ImGui::EndPopup();
        }

        ImGui::PopItemWidth();

        DrawComponent<TransformComponent>("Transform", entity, [](auto& component)
                                          {
			DrawVec3Control("Translation", component.Translation);
			glm::vec3 rotation = glm::degrees(component.Rotation);
			DrawVec3Control("Rotation", rotation);
			component.Rotation = glm::radians(rotation);
			DrawVec3Control("Scale", component.Scale, 1.0f); });

        DrawComponent<CameraComponent>("Camera", entity, [](auto& component)
                                       {
			auto& camera = component.Camera;

			ImGui::Checkbox("Primary", &component.Primary);

			const char* const projectionTypeStrings[2] = { "Perspective", "Orthographic" };
			if (const char* currentProjectionTypeString = projectionTypeStrings[static_cast<int>(camera.GetProjectionType())]; ImGui::BeginCombo("Projection", currentProjectionTypeString))
			{
				for (int i = 0; i < 2; ++i)
				{
					const bool isSelected = currentProjectionTypeString == projectionTypeStrings[i];
					if (ImGui::Selectable(projectionTypeStrings[i], isSelected))
					{
						currentProjectionTypeString = projectionTypeStrings[i];
						camera.SetProjectionType(static_cast<SceneCamera::ProjectionType>(i));
					}

					if (isSelected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}

				ImGui::EndCombo();
			}

			if (camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
			{
				if (f32 perspectiveVerticalFov = glm::degrees(camera.GetPerspectiveVerticalFOV()); ImGui::DragFloat("Vertical FOV", &perspectiveVerticalFov))
				{
					camera.SetPerspectiveVerticalFOV(glm::radians(perspectiveVerticalFov));
				}

				if (f32 perspectiveNear = camera.GetPerspectiveNearClip(); ImGui::DragFloat("Near", &perspectiveNear))
				{
					camera.SetPerspectiveNearClip(perspectiveNear);
				}

				f32 perspectiveFar = camera.GetPerspectiveFarClip();
				if (ImGui::DragFloat("Far", &perspectiveFar))
				{
					camera.SetPerspectiveFarClip(perspectiveFar);
				}
			}

			if (camera.GetProjectionType() == SceneCamera::ProjectionType::Orthographic)
			{
				if (f32 orthoSize = camera.GetOrthographicSize(); ImGui::DragFloat("Size", &orthoSize))
				{
					camera.SetOrthographicSize(orthoSize);
				}

				if (f32 orthoNear = camera.GetOrthographicNearClip(); ImGui::DragFloat("Near", &orthoNear))
				{
					camera.SetOrthographicNearClip(orthoNear);
				}

				if (f32 orthoFar = camera.GetOrthographicFarClip(); ImGui::DragFloat("Far", &orthoFar))
				{
					camera.SetOrthographicFarClip(orthoFar);
				}

				ImGui::Checkbox("Fixed Aspect Ratio", &component.FixedAspectRatio);
			} });

        DrawComponent<ScriptComponent>("Script", entity, [entity, scene = m_Context](auto& component) mutable
                                       {
			bool scriptClassExists = ScriptEngine::EntityClassExists(component.ClassName);

			static char buffer[64];
			::strcpy_s(buffer, sizeof(buffer), component.ClassName.c_str());

			UI::ScopedStyleColor textColor(ImGuiCol_Text, ImVec4(0.9f, 0.2f, 0.3f, 1.0f), !scriptClassExists);

			if (ImGui::InputText("Class", buffer, sizeof(buffer)))
			{
				component.ClassName = buffer;
				return;
			}

			// Fields

			if (bool sceneRunning = scene->IsRunning(); sceneRunning)
			{
				if (Ref<ScriptInstance> scriptInstance = ScriptEngine::GetEntityScriptInstance(entity.GetUUID()); scriptInstance)
				{
					for (const auto& fields = scriptInstance->GetScriptClass()->GetFields(); const auto& [name, field] : fields)
					{
						if (field.Type == ScriptFieldType::Float)
						{
							if (f32 data = scriptInstance->GetFieldValue<f32>(name); ImGui::DragFloat(name.c_str(), &data))
							{
								scriptInstance->SetFieldValue(name, data);
							}
						}
					}
				}
			}
			else
			{
			if (scriptClassExists)
			{
				Ref<ScriptClass> entityClass = ScriptEngine::GetEntityClass(component.ClassName);
				const auto& fields = entityClass->GetFields();
				auto& entityFields = ScriptEngine::GetScriptFieldMap(entity);
					for (const auto& [name, field] : fields)
					{
						// Field has been set in editor
						if (entityFields.contains(name))
						{
							ScriptFieldInstance& scriptField = entityFields.at(name);

							if (field.Type == ScriptFieldType::Float)
							{
								f32 data = scriptField.GetValue<f32>();
								if (ImGui::DragFloat(name.c_str(), &data))
									scriptField.SetValue(data);
							}
						}
						else
						{
							if (field.Type == ScriptFieldType::Float)
							{
								f32 data = 0.0f;
								if (ImGui::DragFloat(name.c_str(), &data))
								{
									ScriptFieldInstance& fieldInstance = entityFields[name];
									fieldInstance.Field = field;
									fieldInstance.SetValue(data);
								}
							}
						}
					}
				}
			} });

        DrawComponent<SpriteRendererComponent>("Sprite Renderer", entity, [](auto& component)
                                               {
			ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));

			ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
			if (ImGui::BeginDragDropTarget())
			{
				if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
				{
					auto* const path = static_cast<wchar_t*>(payload->Data);
					std::filesystem::path texturePath(path);
					Ref<Texture2D> const texture = Texture2D::Create(texturePath.string());
					if (texture->IsLoaded())
					{
						component.Texture = texture;
					}
					else
					{
						OLO_WARN("Could not load texture {0}", texturePath.filename().string());
					}
				}
				ImGui::EndDragDropTarget();
			}
			ImGui::DragFloat("Tiling Factor", &component.TilingFactor, 0.1f, 0.0f, 100.0f); });

        DrawComponent<CircleRendererComponent>("Circle Renderer", entity, [](auto& component)
                                               {
			ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));
			ImGui::DragFloat("Thickness", &component.Thickness, 0.025f, 0.0f, 1.0f);
			ImGui::DragFloat("Fade", &component.Fade, 0.00025f, 0.0f, 1.0f); });

        DrawComponent<Rigidbody2DComponent>("Rigidbody 2D", entity, [](auto& component)
                                            {
			const char* const bodyTypeStrings[] = { "Static", "Dynamic", "Kinematic" };
			if (const char* currentBodyTypeString = bodyTypeStrings[static_cast<int>(component.Type)]; ImGui::BeginCombo("Body Type", currentBodyTypeString))
			{
				for (int i = 0; i < 2; ++i)
				{
					const bool isSelected = currentBodyTypeString == bodyTypeStrings[i];
					if (ImGui::Selectable(bodyTypeStrings[i], isSelected))
					{
						currentBodyTypeString = bodyTypeStrings[i];
						component.Type = static_cast<Rigidbody2DComponent::BodyType>(i);
					}

					if (isSelected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}

				ImGui::EndCombo();
			}

			ImGui::Checkbox("Fixed Rotation", &component.FixedRotation); });

        DrawComponent<BoxCollider2DComponent>("Box Collider 2D", entity, [](auto& component)
                                              {
			ImGui::DragFloat2("Offset", glm::value_ptr(component.Offset));
			ImGui::DragFloat2("Size", glm::value_ptr(component.Size));
			ImGui::DragFloat("Density", &component.Density, 0.01f, 0.0f, 1.0f);
			ImGui::DragFloat("Friction", &component.Friction, 0.01f, 0.0f, 1.0f);
			ImGui::DragFloat("Restitution", &component.Restitution, 0.01f, 0.0f, 1.0f);
			ImGui::DragFloat("Restitution Threshold", &component.RestitutionThreshold, 0.01f, 0.0f); });

        DrawComponent<CircleCollider2DComponent>("Circle Collider 2D", entity, [](auto& component)
                                                 {
			ImGui::DragFloat2("Offset", glm::value_ptr(component.Offset));
			ImGui::DragFloat("Radius", &component.Radius);
			ImGui::DragFloat("Density", &component.Density, 0.01f, 0.0f, 1.0f);
			ImGui::DragFloat("Friction", &component.Friction, 0.01f, 0.0f, 1.0f);
			ImGui::DragFloat("Restitution", &component.Restitution, 0.01f, 0.0f, 1.0f);
			ImGui::DragFloat("Restitution Threshold", &component.RestitutionThreshold, 0.01f, 0.0f); });

        DrawComponent<TextComponent>("Text Renderer", entity, [](auto& component)
                                     {
			ImGui::InputTextMultiline("Text String", &component.TextString);
			ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));
			ImGui::DragFloat("Kerning", &component.Kerning, 0.025f);
			ImGui::DragFloat("Line Spacing", &component.LineSpacing, 0.025f); });

        // 3D Components
        DrawComponent<MeshComponent>("Mesh", entity, [entity, scene = m_Context](auto& component) mutable
                                     {
			ImGui::Text("Mesh Source: %s", component.m_MeshSource ? "Loaded" : "None");

			if (component.m_MeshSource)
			{
				ImGui::Text("Submeshes: %d", component.m_MeshSource->GetSubmeshes().Num());
				ImGui::Text("Vertices: %d", component.m_MeshSource->GetVertices().Num());
			}

			// Import static model from file
			if (ImGui::Button("Import Static Model..."))
			{
				std::string filepath = FileDialogs::OpenFile(
					"3D Models (*.obj;*.fbx;*.gltf;*.glb)\0*.obj;*.fbx;*.gltf;*.glb\0"
					"Wavefront OBJ (*.obj)\0*.obj\0"
					"FBX (*.fbx)\0*.fbx\0"
					"glTF (*.gltf;*.glb)\0*.gltf;*.glb\0"
					"All Files (*.*)\0*.*\0");
				if (!filepath.empty())
				{
					auto model = Ref<Model>::Create(filepath);
					if (model && model->GetMeshCount() > 0)
					{
						// Create a combined MeshSource from all meshes in the model
						auto combinedMeshSource = model->CreateCombinedMeshSource();
						if (combinedMeshSource)
						{
							component.m_MeshSource = combinedMeshSource;
							OLO_CORE_INFO("Imported static model: {} ({} meshes combined)", filepath, model->GetMeshCount());
						}
						else
						{
							OLO_CORE_ERROR("Failed to create combined mesh from model: {}", filepath);
						}
					}
					else
					{
						OLO_CORE_ERROR("Failed to load model: {}", filepath);
					}
				}
			}

			ImGui::SameLine();

			// Import animated model from file (adds skeleton, animation components)
			if (ImGui::Button("Import Animated Model..."))
			{
				std::string filepath = FileDialogs::OpenFile(
					"Animated Models (*.fbx;*.gltf;*.glb)\0*.fbx;*.gltf;*.glb\0"
					"FBX (*.fbx)\0*.fbx\0"
					"glTF (*.gltf;*.glb)\0*.gltf;*.glb\0"
					"All Files (*.*)\0*.*\0");
				if (!filepath.empty())
				{
					auto animatedModel = Ref<AnimatedModel>::Create(filepath);
					if (animatedModel && !animatedModel->GetMeshes().empty())
					{
						// Set the mesh source from the animated model
						component.m_MeshSource = animatedModel->GetMeshes()[0];
						OLO_CORE_INFO("Imported animated model: {} ({} meshes)", filepath, animatedModel->GetMeshes().size());

						// Add MaterialComponent if the model has materials
						if (!animatedModel->GetMaterials().empty())
						{
							if (!entity.HasComponent<MaterialComponent>())
							{
								auto& materialComp = entity.AddComponent<MaterialComponent>();
								materialComp.m_Material = animatedModel->GetMaterials()[0];
								OLO_CORE_INFO("Added MaterialComponent from animated model");
							}
							else
							{
								auto& materialComp = entity.GetComponent<MaterialComponent>();
								materialComp.m_Material = animatedModel->GetMaterials()[0];
							}
						}

						// Add SkeletonComponent if the model has a skeleton
						if (animatedModel->HasSkeleton())
						{
							if (!entity.HasComponent<SkeletonComponent>())
							{
								auto& skeletonComp = entity.AddComponent<SkeletonComponent>();
								skeletonComp.m_Skeleton = animatedModel->GetSkeleton();
								OLO_CORE_INFO("Added SkeletonComponent: {} bones", skeletonComp.m_Skeleton->m_BoneNames.size());
							}
							else
							{
								auto& skeletonComp = entity.GetComponent<SkeletonComponent>();
								skeletonComp.m_Skeleton = animatedModel->GetSkeleton();
							}
						}

						// Add AnimationStateComponent if the model has animations
						if (animatedModel->HasAnimations())
						{
							if (!entity.HasComponent<AnimationStateComponent>())
							{
								auto& animStateComp = entity.AddComponent<AnimationStateComponent>();
								// Store all available clips
								animStateComp.m_AvailableClips = animatedModel->GetAnimations();
								animStateComp.m_CurrentClip = animStateComp.m_AvailableClips[0];
								animStateComp.m_CurrentClipIndex = 0;
								animStateComp.m_State = AnimationStateComponent::State::Idle;
								animStateComp.m_CurrentTime = 0.0f;
								animStateComp.m_IsPlaying = false;
								animStateComp.m_SourceFilePath = filepath; // Save for serialization
								OLO_CORE_INFO("Added AnimationStateComponent: {} animations available", animStateComp.m_AvailableClips.size());

								// List all available animations
								for (sizet i = 0; i < animStateComp.m_AvailableClips.size(); i++)
								{
									auto& anim = animStateComp.m_AvailableClips[i];
									OLO_CORE_INFO("  Animation [{}]: '{}' - Duration: {:.2f}s", i, anim->Name, anim->Duration);
								}
							}
							else
							{
								auto& animStateComp = entity.GetComponent<AnimationStateComponent>();
								animStateComp.m_AvailableClips = animatedModel->GetAnimations();
								animStateComp.m_CurrentClip = animStateComp.m_AvailableClips[0];
								animStateComp.m_CurrentClipIndex = 0;
								animStateComp.m_SourceFilePath = filepath; // Save for serialization
							}
						}
						else
						{
							OLO_CORE_WARN("Animated model has no animations: {}", filepath);
						}
					}
					else
					{
						OLO_CORE_ERROR("Failed to load animated model: {}", filepath);
					}
				}
			}

			// Primitive mesh creation dropdown
			const char* primitives[] = { "Create Primitive...", "Cube", "Sphere", "Plane", "Cylinder", "Cone", "Icosphere", "Torus" };
			static int currentPrimitive = 0;
			ImGui::SetNextItemWidth(150.0f);
			if (ImGui::Combo("##PrimitiveCombo", &currentPrimitive, primitives, IM_ARRAYSIZE(primitives)))
			{
				Ref<Mesh> mesh = nullptr;
				switch (currentPrimitive)
				{
				case 1: mesh = MeshPrimitives::CreateCube(); break;
				case 2: mesh = MeshPrimitives::CreateSphere(); break;
				case 3: mesh = MeshPrimitives::CreatePlane(); break;
				case 4: mesh = MeshPrimitives::CreateCylinder(); break;
				case 5: mesh = MeshPrimitives::CreateCone(); break;
				case 6: mesh = MeshPrimitives::CreateIcosphere(); break;
				case 7: mesh = MeshPrimitives::CreateTorus(); break;
				}
				if (mesh)
				{
					component.m_MeshSource = mesh->GetMeshSource();
				}
				currentPrimitive = 0; // Reset selection
			}

			// Clear mesh button
			if (component.m_MeshSource)
			{
				if (ImGui::Button("Clear Mesh"))
				{
					component.m_MeshSource.Reset();
				}
			} });

        DrawComponent<ModelComponent>("Model", entity, [](auto& component)
                                      {
            ImGui::Text("Model: %s", component.IsLoaded() ? "Loaded" : "None");

            if (component.IsLoaded())
            {
                ImGui::Text("Meshes: %zu", component.m_Model->GetMeshCount());
                if (!component.m_FilePath.empty())
                {
                    // Show just the filename, not the full path
                    auto lastSlash = component.m_FilePath.find_last_of("/\\");
                    std::string filename = (lastSlash != std::string::npos)
                        ? component.m_FilePath.substr(lastSlash + 1)
                        : component.m_FilePath;
                    ImGui::Text("File: %s", filename.c_str());
                }
            }

            ImGui::Checkbox("Visible", &component.m_Visible);

            // Import model from file
            if (ImGui::Button("Import Model...##ModelComponent"))
            {
                std::string filepath = FileDialogs::OpenFile(
                    "3D Models (*.obj;*.fbx;*.gltf;*.glb)\0*.obj;*.fbx;*.gltf;*.glb\0"
                    "Wavefront OBJ (*.obj)\0*.obj\0"
                    "FBX (*.fbx)\0*.fbx\0"
                    "glTF (*.gltf;*.glb)\0*.gltf;*.glb\0"
                    "All Files (*.*)\0*.*\0");
                if (!filepath.empty())
                {
                    component.m_FilePath = filepath;
                    component.Reload();
                    if (component.IsLoaded())
                    {
                        OLO_CORE_INFO("Imported model with materials: {} ({} meshes)",
                            filepath, component.m_Model->GetMeshCount());
                    }
                    else
                    {
                        OLO_CORE_ERROR("Failed to load model: {}", filepath);
                    }
                }
            }

            // Reload button
            if (component.IsLoaded())
            {
                ImGui::SameLine();
                if (ImGui::Button("Reload##ModelComponent"))
                {
                    component.Reload();
                }

                ImGui::SameLine();
                if (ImGui::Button("Clear##ModelComponent"))
                {
                    component.m_Model.Reset();
                    component.m_FilePath.clear();
                }
            } });

        DrawComponent<MaterialComponent>("Material", entity, [](auto& component)
                                         {
            // Material Presets Dropdown
            const char* presets[] = { "Custom", "Default", "Metallic", "Rough Plastic", "Polished Metal", "Rubber", "Glass", "Gold", "Silver", "Copper", "Wood", "Marble" };
            static int currentPreset = 0;
            if (ImGui::Combo("Preset", &currentPreset, presets, IM_ARRAYSIZE(presets)))
            {
                switch (currentPreset)
                {
                    case 1: // Default
                        component.m_Material.SetBaseColorFactor(glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));
                        component.m_Material.SetMetallicFactor(0.0f);
                        component.m_Material.SetRoughnessFactor(0.5f);
                        break;
                    case 2: // Metallic
                        component.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.9f, 0.9f, 1.0f));
                        component.m_Material.SetMetallicFactor(1.0f);
                        component.m_Material.SetRoughnessFactor(0.2f);
                        break;
                    case 3: // Rough Plastic
                        component.m_Material.SetBaseColorFactor(glm::vec4(0.2f, 0.2f, 0.8f, 1.0f));
                        component.m_Material.SetMetallicFactor(0.0f);
                        component.m_Material.SetRoughnessFactor(0.8f);
                        break;
                    case 4: // Polished Metal
                        component.m_Material.SetBaseColorFactor(glm::vec4(0.95f, 0.95f, 0.95f, 1.0f));
                        component.m_Material.SetMetallicFactor(1.0f);
                        component.m_Material.SetRoughnessFactor(0.05f);
                        break;
                    case 5: // Rubber
                        component.m_Material.SetBaseColorFactor(glm::vec4(0.1f, 0.1f, 0.1f, 1.0f));
                        component.m_Material.SetMetallicFactor(0.0f);
                        component.m_Material.SetRoughnessFactor(0.95f);
                        break;
                    case 6: // Glass
                        component.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.9f, 0.95f, 0.3f));
                        component.m_Material.SetMetallicFactor(0.0f);
                        component.m_Material.SetRoughnessFactor(0.05f);
                        break;
                    case 7: // Gold
                        component.m_Material.SetBaseColorFactor(glm::vec4(1.0f, 0.766f, 0.336f, 1.0f));
                        component.m_Material.SetMetallicFactor(1.0f);
                        component.m_Material.SetRoughnessFactor(0.3f);
                        break;
                    case 8: // Silver
                        component.m_Material.SetBaseColorFactor(glm::vec4(0.972f, 0.960f, 0.915f, 1.0f));
                        component.m_Material.SetMetallicFactor(1.0f);
                        component.m_Material.SetRoughnessFactor(0.2f);
                        break;
                    case 9: // Copper
                        component.m_Material.SetBaseColorFactor(glm::vec4(0.955f, 0.637f, 0.538f, 1.0f));
                        component.m_Material.SetMetallicFactor(1.0f);
                        component.m_Material.SetRoughnessFactor(0.25f);
                        break;
                    case 10: // Wood
                        component.m_Material.SetBaseColorFactor(glm::vec4(0.55f, 0.35f, 0.2f, 1.0f));
                        component.m_Material.SetMetallicFactor(0.0f);
                        component.m_Material.SetRoughnessFactor(0.7f);
                        break;
                    case 11: // Marble
                        component.m_Material.SetBaseColorFactor(glm::vec4(0.95f, 0.93f, 0.88f, 1.0f));
                        component.m_Material.SetMetallicFactor(0.0f);
                        component.m_Material.SetRoughnessFactor(0.15f);
                        break;
                    default:
                        break;
                }
                currentPreset = 0; // Reset to Custom after applying
            }

            ImGui::Separator();

            auto baseColor = component.m_Material.GetBaseColorFactor();
            glm::vec3 albedo(baseColor.r, baseColor.g, baseColor.b);
            if (ImGui::ColorEdit3("Albedo", glm::value_ptr(albedo)))
                component.m_Material.SetBaseColorFactor(glm::vec4(albedo, baseColor.a));

            f32 metallic = component.m_Material.GetMetallicFactor();
            if (ImGui::DragFloat("Metallic", &metallic, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetMetallicFactor(metallic);

            f32 roughness = component.m_Material.GetRoughnessFactor();
            if (ImGui::DragFloat("Roughness", &roughness, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetRoughnessFactor(roughness); });

        DrawComponent<DirectionalLightComponent>("Directional Light", entity, [](auto& component)
                                                 {
			DrawVec3Control("Direction", component.m_Direction);
			ImGui::ColorEdit3("Color", glm::value_ptr(component.m_Color));
			ImGui::DragFloat("Intensity##DirectionalLight", &component.m_Intensity, 0.1f, 0.0f, 10.0f);
			ImGui::Checkbox("Cast Shadows##DirectionalLight", &component.m_CastShadows); });

        DrawComponent<PointLightComponent>("Point Light", entity, [](auto& component)
                                           {
			ImGui::ColorEdit3("Color##PointLight", glm::value_ptr(component.m_Color));
			ImGui::DragFloat("Intensity##PointLight", &component.m_Intensity, 0.1f, 0.0f, 10.0f);
			ImGui::DragFloat("Range##PointLight", &component.m_Range, 0.1f, 0.1f, 100.0f);
			ImGui::DragFloat("Attenuation##PointLight", &component.m_Attenuation, 0.1f, 0.1f, 4.0f);
			ImGui::Checkbox("Cast Shadows##PointLight", &component.m_CastShadows); });

        DrawComponent<SpotLightComponent>("Spot Light", entity, [](auto& component)
                                          {
			DrawVec3Control("Direction##SpotLight", component.m_Direction);
			ImGui::ColorEdit3("Color##SpotLight", glm::value_ptr(component.m_Color));
			ImGui::DragFloat("Intensity##SpotLight", &component.m_Intensity, 0.1f, 0.0f, 10.0f);
			ImGui::DragFloat("Range##SpotLight", &component.m_Range, 0.1f, 0.1f, 100.0f);
			ImGui::DragFloat("Inner Cutoff##SpotLight", &component.m_InnerCutoff, 0.1f, 0.0f, 90.0f);
			ImGui::DragFloat("Outer Cutoff##SpotLight", &component.m_OuterCutoff, 0.1f, 0.0f, 90.0f);
			ImGui::DragFloat("Attenuation##SpotLight", &component.m_Attenuation, 0.1f, 0.1f, 4.0f);
			ImGui::Checkbox("Cast Shadows##SpotLight", &component.m_CastShadows); });

        DrawComponent<EnvironmentMapComponent>("Environment Map", entity, [](auto& component)
                                               {
            // Mode toggle
            ImGui::Checkbox("Use Cubemap Folder", &component.m_IsCubemapFolder);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("If enabled, specify a folder path containing:\nright.jpg, left.jpg, top.jpg, bottom.jpg, front.jpg, back.jpg\n\nIf disabled, specify an HDR/EXR equirectangular file.");
            }

            // Current environment map display
            if (!component.m_FilePath.empty())
            {
                auto lastSlash = component.m_FilePath.find_last_of("/\\");
                std::string displayName = (lastSlash != std::string::npos)
                    ? component.m_FilePath.substr(lastSlash + 1)
                    : component.m_FilePath;
                ImGui::Text("%s: %s", component.m_IsCubemapFolder ? "Folder" : "File", displayName.c_str());
            }
            else
            {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No environment map loaded");
            }

            // Path input (editable)
            char pathBuffer[512];
            std::strncpy(pathBuffer, component.m_FilePath.c_str(), sizeof(pathBuffer) - 1);
            pathBuffer[sizeof(pathBuffer) - 1] = '\0';
            if (ImGui::InputText("Path##EnvMapPath", pathBuffer, sizeof(pathBuffer)))
            {
                component.m_FilePath = pathBuffer;
                component.m_EnvironmentMap = nullptr;  // Force reload
            }

            // Browse button (for HDR files only; for cubemap folders, user types path)
            if (!component.m_IsCubemapFolder)
            {
                if (ImGui::Button("Browse HDR...##EnvMap"))
                {
                    std::string filepath = FileDialogs::OpenFile(
                        "HDR Images (*.hdr;*.exr)\0*.hdr;*.exr\0"
                        "All Files (*.*)\0*.*\0");
                    if (!filepath.empty())
                    {
                        component.m_FilePath = filepath;
                        component.m_EnvironmentMap = nullptr;  // Force reload
                    }
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Example: assets/textures/Skybox");
            }

            if (!component.m_FilePath.empty())
            {
                ImGui::SameLine();
                if (ImGui::Button("Clear##EnvMap"))
                {
                    component.m_FilePath.clear();
                    component.m_EnvironmentMapAsset = 0;
                    component.m_EnvironmentMap = nullptr;
                }
            }

            ImGui::Separator();

            // Skybox settings
            ImGui::Checkbox("Enable Skybox##EnvMap", &component.m_EnableSkybox);

            if (component.m_EnableSkybox)
            {
                ImGui::DragFloat("Rotation##EnvMap", &component.m_Rotation, 1.0f, 0.0f, 360.0f, "%.1f deg");
                ImGui::DragFloat("Exposure##EnvMap", &component.m_Exposure, 0.01f, 0.1f, 10.0f);
                ImGui::DragFloat("Blur##EnvMap", &component.m_BlurAmount, 0.01f, 0.0f, 1.0f);
                ImGui::ColorEdit3("Tint##EnvMap", glm::value_ptr(component.m_Tint));
            }

            ImGui::Separator();

            // IBL settings
            ImGui::Checkbox("Enable IBL##EnvMap", &component.m_EnableIBL);

            if (component.m_EnableIBL)
            {
                ImGui::DragFloat("IBL Intensity##EnvMap", &component.m_IBLIntensity, 0.01f, 0.0f, 5.0f);
            } });

        DrawComponent<Rigidbody3DComponent>("Rigidbody 3D", entity, [](auto& component)
                                            {
			const char* bodyTypeStrings[] = { "Static", "Dynamic", "Kinematic" };
			const char* currentBodyTypeString = bodyTypeStrings[static_cast<int>(component.m_Type)];
			if (ImGui::BeginCombo("Body Type", currentBodyTypeString))
			{
				for (int i = 0; i < 3; ++i)
				{
					const bool isSelected = currentBodyTypeString == bodyTypeStrings[i];
					if (ImGui::Selectable(bodyTypeStrings[i], isSelected))
					{
						component.m_Type = static_cast<BodyType3D>(i);
					}
					if (isSelected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::DragFloat("Mass##Rigidbody3D", &component.m_Mass, 0.01f, 0.1f, 1000.0f);
			ImGui::DragFloat("Linear Drag##Rigidbody3D", &component.m_LinearDrag, 0.001f, 0.0f, 1.0f);
			ImGui::DragFloat("Angular Drag##Rigidbody3D", &component.m_AngularDrag, 0.001f, 0.0f, 1.0f);
			ImGui::Checkbox("Disable Gravity##Rigidbody3D", &component.m_DisableGravity);
			ImGui::Checkbox("Is Trigger##Rigidbody3D", &component.m_IsTrigger); });

        DrawComponent<BoxCollider3DComponent>("Box Collider 3D", entity, [](auto& component)
                                              {
			DrawVec3Control("Half Extents##BoxCollider3D", component.m_HalfExtents);
			DrawVec3Control("Offset##BoxCollider3D", component.m_Offset);
            f32 staticFriction = component.m_Material.GetStaticFriction();
            if (ImGui::DragFloat("Static Friction##BoxCollider3D", &staticFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetStaticFriction(staticFriction);
            f32 dynamicFriction = component.m_Material.GetDynamicFriction();
            if (ImGui::DragFloat("Dynamic Friction##BoxCollider3D", &dynamicFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetDynamicFriction(dynamicFriction);
            f32 restitution = component.m_Material.GetRestitution();
            if (ImGui::DragFloat("Restitution##BoxCollider3D", &restitution, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetRestitution(restitution); });

        DrawComponent<SphereCollider3DComponent>("Sphere Collider 3D", entity, [](auto& component)
                                                 {
			ImGui::DragFloat("Radius##SphereCollider3D", &component.m_Radius, 0.01f, 0.01f, 100.0f);
			DrawVec3Control("Offset##SphereCollider3D", component.m_Offset);
            f32 staticFriction = component.m_Material.GetStaticFriction();
            if (ImGui::DragFloat("Static Friction##SphereCollider3D", &staticFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetStaticFriction(staticFriction);
            f32 dynamicFriction = component.m_Material.GetDynamicFriction();
            if (ImGui::DragFloat("Dynamic Friction##SphereCollider3D", &dynamicFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetDynamicFriction(dynamicFriction);
            f32 restitution = component.m_Material.GetRestitution();
            if (ImGui::DragFloat("Restitution##SphereCollider3D", &restitution, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetRestitution(restitution); });

        DrawComponent<CapsuleCollider3DComponent>("Capsule Collider 3D", entity, [](auto& component)
                                                  {
			ImGui::DragFloat("Radius##CapsuleCollider3D", &component.m_Radius, 0.01f, 0.01f, 100.0f);
			ImGui::DragFloat("Half Height##CapsuleCollider3D", &component.m_HalfHeight, 0.01f, 0.01f, 100.0f);
			DrawVec3Control("Offset##CapsuleCollider3D", component.m_Offset);
            f32 staticFriction = component.m_Material.GetStaticFriction();
            if (ImGui::DragFloat("Static Friction##CapsuleCollider3D", &staticFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetStaticFriction(staticFriction);
            f32 dynamicFriction = component.m_Material.GetDynamicFriction();
            if (ImGui::DragFloat("Dynamic Friction##CapsuleCollider3D", &dynamicFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetDynamicFriction(dynamicFriction);
            f32 restitution = component.m_Material.GetRestitution();
            if (ImGui::DragFloat("Restitution##CapsuleCollider3D", &restitution, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetRestitution(restitution); });

        DrawComponent<MeshCollider3DComponent>("Mesh Collider 3D", entity, [](auto& component)
                                               {
            ImGui::Text("Collider Asset: %s", component.m_ColliderAsset ? "Set" : "None");
            DrawVec3Control("Offset##MeshCollider3D", component.m_Offset);
            DrawVec3Control("Scale##MeshCollider3D", component.m_Scale, 1.0f);
            ImGui::Checkbox("Use Complex As Simple##MeshCollider3D", &component.m_UseComplexAsSimple);
            f32 staticFriction = component.m_Material.GetStaticFriction();
            if (ImGui::DragFloat("Static Friction##MeshCollider3D", &staticFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetStaticFriction(staticFriction);
            f32 dynamicFriction = component.m_Material.GetDynamicFriction();
            if (ImGui::DragFloat("Dynamic Friction##MeshCollider3D", &dynamicFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetDynamicFriction(dynamicFriction);
            f32 restitution = component.m_Material.GetRestitution();
            if (ImGui::DragFloat("Restitution##MeshCollider3D", &restitution, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetRestitution(restitution); });

        DrawComponent<ConvexMeshCollider3DComponent>("Convex Mesh Collider 3D", entity, [](auto& component)
                                                     {
            ImGui::Text("Collider Asset: %s", component.m_ColliderAsset ? "Set" : "None");
            DrawVec3Control("Offset##ConvexMeshCollider3D", component.m_Offset);
            DrawVec3Control("Scale##ConvexMeshCollider3D", component.m_Scale, 1.0f);
            ImGui::DragFloat("Convex Radius##ConvexMeshCollider3D", &component.m_ConvexRadius, 0.01f, 0.0f, 1.0f);
            int maxVertices = static_cast<int>(component.m_MaxVertices);
            if (ImGui::DragInt("Max Vertices##ConvexMeshCollider3D", &maxVertices, 1, 4, 256))
                component.m_MaxVertices = static_cast<u32>(maxVertices);
            f32 staticFriction = component.m_Material.GetStaticFriction();
            if (ImGui::DragFloat("Static Friction##ConvexMeshCollider3D", &staticFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetStaticFriction(staticFriction);
            f32 dynamicFriction = component.m_Material.GetDynamicFriction();
            if (ImGui::DragFloat("Dynamic Friction##ConvexMeshCollider3D", &dynamicFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetDynamicFriction(dynamicFriction);
            f32 restitution = component.m_Material.GetRestitution();
            if (ImGui::DragFloat("Restitution##ConvexMeshCollider3D", &restitution, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetRestitution(restitution); });

        DrawComponent<TriangleMeshCollider3DComponent>("Triangle Mesh Collider 3D", entity, [](auto& component)
                                                       {
            ImGui::Text("Collider Asset: %s", component.m_ColliderAsset ? "Set" : "None");
            ImGui::TextWrapped("Note: Triangle mesh colliders are always static.");
            DrawVec3Control("Offset##TriangleMeshCollider3D", component.m_Offset);
            DrawVec3Control("Scale##TriangleMeshCollider3D", component.m_Scale, 1.0f);
            f32 staticFriction = component.m_Material.GetStaticFriction();
            if (ImGui::DragFloat("Static Friction##TriangleMeshCollider3D", &staticFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetStaticFriction(staticFriction);
            f32 dynamicFriction = component.m_Material.GetDynamicFriction();
            if (ImGui::DragFloat("Dynamic Friction##TriangleMeshCollider3D", &dynamicFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetDynamicFriction(dynamicFriction);
            f32 restitution = component.m_Material.GetRestitution();
            if (ImGui::DragFloat("Restitution##TriangleMeshCollider3D", &restitution, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetRestitution(restitution); });

        DrawComponent<CharacterController3DComponent>("Character Controller 3D", entity, [](auto& component)
                                                      {
            ImGui::DragFloat("Slope Limit (deg)##CharacterController3D", &component.m_SlopeLimitDeg, 1.0f, 0.0f, 90.0f);
            ImGui::DragFloat("Step Offset##CharacterController3D", &component.m_StepOffset, 0.01f, 0.0f, 2.0f);
            ImGui::DragFloat("Jump Power##CharacterController3D", &component.m_JumpPower, 0.1f, 0.0f, 50.0f);
            int layerID = static_cast<int>(component.m_LayerID);
            if (ImGui::DragInt("Layer ID##CharacterController3D", &layerID, 1, 0, 31))
                component.m_LayerID = static_cast<u32>(layerID);
            ImGui::Checkbox("Disable Gravity##CharacterController3D", &component.m_DisableGravity);
            ImGui::Checkbox("Control Movement In Air##CharacterController3D", &component.m_ControlMovementInAir);
            ImGui::Checkbox("Control Rotation In Air##CharacterController3D", &component.m_ControlRotationInAir); });

        // Audio Components
        DrawComponent<AudioSourceComponent>("Audio Source", entity, [](auto& component)
                                            {
            ImGui::Text("Audio Source: %s", component.Source ? "Loaded" : "None");
            if (component.Source)
            {
                ImGui::Text("File: %s", component.Source->GetPath());
            }

            ImGui::DragFloat("Volume##AudioSource", &component.Config.VolumeMultiplier, 0.01f, 0.0f, 2.0f);
            ImGui::DragFloat("Pitch##AudioSource", &component.Config.PitchMultiplier, 0.01f, 0.1f, 3.0f);
            ImGui::Checkbox("Play On Awake##AudioSource", &component.Config.PlayOnAwake);
            ImGui::Checkbox("Looping##AudioSource", &component.Config.Looping);

            ImGui::Separator();
            ImGui::Text("Spatialization");
            ImGui::Checkbox("Spatialization##AudioSource", &component.Config.Spatialization);

            if (component.Config.Spatialization)
            {
                const char* attenuationModels[] = { "None", "Inverse", "Linear", "Exponential" };
                int currentModel = static_cast<int>(component.Config.AttenuationModel);
                if (ImGui::Combo("Attenuation Model##AudioSource", &currentModel, attenuationModels, IM_ARRAYSIZE(attenuationModels)))
                    component.Config.AttenuationModel = static_cast<AttenuationModelType>(currentModel);

                ImGui::DragFloat("Roll Off##AudioSource", &component.Config.RollOff, 0.1f, 0.0f, 10.0f);
                ImGui::DragFloat("Min Gain##AudioSource", &component.Config.MinGain, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Max Gain##AudioSource", &component.Config.MaxGain, 0.01f, 0.0f, 2.0f);
                ImGui::DragFloat("Min Distance##AudioSource", &component.Config.MinDistance, 0.1f, 0.0f, 100.0f);
                ImGui::DragFloat("Max Distance##AudioSource", &component.Config.MaxDistance, 1.0f, 0.0f, 1000.0f);

                ImGui::Separator();
                ImGui::Text("Cone Settings");
                ImGui::DragFloat("Inner Angle##AudioSource", &component.Config.ConeInnerAngle, 1.0f, 0.0f, 360.0f);
                ImGui::DragFloat("Outer Angle##AudioSource", &component.Config.ConeOuterAngle, 1.0f, 0.0f, 360.0f);
                ImGui::DragFloat("Outer Gain##AudioSource", &component.Config.ConeOuterGain, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Doppler Factor##AudioSource", &component.Config.DopplerFactor, 0.1f, 0.0f, 10.0f);
            } });

        DrawComponent<AudioListenerComponent>("Audio Listener", entity, [](auto& component)
                                              {
            ImGui::Checkbox("Active##AudioListener", &component.Active);

            ImGui::Separator();
            ImGui::Text("Cone Settings");
            ImGui::DragFloat("Inner Angle##AudioListener", &component.Config.ConeInnerAngle, 1.0f, 0.0f, 360.0f);
            ImGui::DragFloat("Outer Angle##AudioListener", &component.Config.ConeOuterAngle, 1.0f, 0.0f, 360.0f);
            ImGui::DragFloat("Outer Gain##AudioListener", &component.Config.ConeOuterGain, 0.01f, 0.0f, 1.0f); });

        // Animation Components
        DrawComponent<AnimationStateComponent>("Animation State", entity, [](auto& component)
                                               {
            const char* stateStrings[] = { "Idle", "Bounce", "Custom" };
            int currentState = static_cast<int>(component.m_State);
            if (ImGui::Combo("State##AnimationState", &currentState, stateStrings, IM_ARRAYSIZE(stateStrings)))
                component.m_State = static_cast<AnimationStateComponent::State>(currentState);

            ImGui::Text("Current Clip: %s", component.m_CurrentClip ? "Loaded" : "None");
            ImGui::Text("Next Clip: %s", component.m_NextClip ? "Loaded" : "None");

            ImGui::DragFloat("Current Time##AnimationState", &component.m_CurrentTime, 0.01f, 0.0f, 100.0f);
            ImGui::DragFloat("Blend Duration##AnimationState", &component.m_BlendDuration, 0.01f, 0.0f, 5.0f);

            if (component.m_Blending)
            {
                ImGui::Text("Blending: %.2f", component.m_BlendFactor);
                ImGui::ProgressBar(component.m_BlendFactor, ImVec2(-1, 0), "Blend Progress");
            }

            ImGui::Text("Bone Entities: %zu", component.m_BoneEntityIds.size()); });

        DrawComponent<SkeletonComponent>("Skeleton", entity, [](auto& component)
                                         {
            ImGui::Text("Skeleton: %s", component.m_Skeleton ? "Loaded" : "None");
            if (component.m_Skeleton)
            {
                ImGui::Text("Bones: %zu", component.m_Skeleton->m_BoneNames.size());
            }

            if (ImGui::Button("Invalidate Cache##Skeleton"))
            {
                component.InvalidateCache();
            } });

        DrawComponent<SubmeshComponent>("Submesh", entity, [](auto& component)
                                        {
            ImGui::Text("Mesh: %s", component.m_Mesh ? "Loaded" : "None");
            int submeshIndex = static_cast<int>(component.m_SubmeshIndex);
            if (ImGui::DragInt("Submesh Index##Submesh", &submeshIndex, 1, 0, 255))
                component.m_SubmeshIndex = static_cast<u32>(submeshIndex);
            ImGui::Checkbox("Visible##Submesh", &component.m_Visible);
            ImGui::Text("Bone Entities: %zu", component.m_BoneEntityIds.size()); });

        // --- UI Components ---

        DrawComponent<UICanvasComponent>("UI Canvas", entity, [](auto& component)
                                         {
            const char* renderModeStrings[] = { "Screen Space Overlay", "World Space" };
            if (const char* current = renderModeStrings[static_cast<int>(component.m_RenderMode)]; ImGui::BeginCombo("Render Mode", current))
            {
                for (int i = 0; i < 2; ++i)
                {
                    const bool isSelected = (current == renderModeStrings[i]);
                    if (ImGui::Selectable(renderModeStrings[i], isSelected))
                        component.m_RenderMode = static_cast<UICanvasRenderMode>(i);
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::DragInt("Sort Order", &component.m_SortOrder);

            const char* scaleModeStrings[] = { "Constant Pixel Size", "Scale With Screen Size" };
            if (const char* current = scaleModeStrings[static_cast<int>(component.m_ScaleMode)]; ImGui::BeginCombo("Scale Mode", current))
            {
                for (int i = 0; i < 2; ++i)
                {
                    const bool isSelected = (current == scaleModeStrings[i]);
                    if (ImGui::Selectable(scaleModeStrings[i], isSelected))
                        component.m_ScaleMode = static_cast<UICanvasScaleMode>(i);
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::DragFloat2("Reference Resolution", glm::value_ptr(component.m_ReferenceResolution), 1.0f, 1.0f, 7680.0f); });

        DrawComponent<UIRectTransformComponent>("UI Rect Transform", entity, [](auto& component)
                                                {
            ImGui::DragFloat2("Anchor Min", glm::value_ptr(component.m_AnchorMin), 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat2("Anchor Max", glm::value_ptr(component.m_AnchorMax), 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat2("Anchored Position", glm::value_ptr(component.m_AnchoredPosition));
            ImGui::DragFloat2("Size Delta", glm::value_ptr(component.m_SizeDelta));
            ImGui::DragFloat2("Pivot", glm::value_ptr(component.m_Pivot), 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Rotation", &component.m_Rotation, 0.1f);
            ImGui::DragFloat2("Scale", glm::value_ptr(component.m_Scale), 0.01f, 0.01f, 10.0f); });

        DrawComponent<UIPanelComponent>("UI Panel", entity, [](auto& component)
                                        { ImGui::ColorEdit4("Background Color", glm::value_ptr(component.m_BackgroundColor)); });

        DrawComponent<UIImageComponent>("UI Image", entity, [](auto& component)
                                        {
            ImGui::ColorEdit4("Color", glm::value_ptr(component.m_Color));
            ImGui::DragFloat4("Border Insets (L,R,T,B)", glm::value_ptr(component.m_BorderInsets), 1.0f, 0.0f, 512.0f);

            ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                if (auto const* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    auto const* path = static_cast<wchar_t const*>(payload->Data);
                    std::filesystem::path texturePath(path);
                    Ref<Texture2D> texture = Texture2D::Create(texturePath.string());
                    if (texture->IsLoaded())
                        component.m_Texture = texture;
                }
                ImGui::EndDragDropTarget();
            } });

        DrawComponent<UITextComponent>("UI Text", entity, [](auto& component)
                                       {
            ImGui::InputTextMultiline("Text", &component.m_Text);
            ImGui::DragFloat("Font Size", &component.m_FontSize, 0.5f, 1.0f, 200.0f);
            ImGui::ColorEdit4("Color", glm::value_ptr(component.m_Color));

            const char* alignmentStrings[] = { "Top Left", "Top Center", "Top Right", "Middle Left", "Middle Center", "Middle Right", "Bottom Left", "Bottom Center", "Bottom Right" };
            if (const char* current = alignmentStrings[static_cast<int>(component.m_Alignment)]; ImGui::BeginCombo("Alignment", current))
            {
                for (int i = 0; i < 9; ++i)
                {
                    const bool isSelected = (current == alignmentStrings[i]);
                    if (ImGui::Selectable(alignmentStrings[i], isSelected))
                        component.m_Alignment = static_cast<UITextAlignment>(i);
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::DragFloat("Kerning", &component.m_Kerning, 0.025f);
            ImGui::DragFloat("Line Spacing", &component.m_LineSpacing, 0.025f); });

        DrawComponent<UIButtonComponent>("UI Button", entity, [](auto& component)
                                         {
            ImGui::ColorEdit4("Normal Color", glm::value_ptr(component.m_NormalColor));
            ImGui::ColorEdit4("Hovered Color", glm::value_ptr(component.m_HoveredColor));
            ImGui::ColorEdit4("Pressed Color", glm::value_ptr(component.m_PressedColor));
            ImGui::ColorEdit4("Disabled Color", glm::value_ptr(component.m_DisabledColor));
            ImGui::Checkbox("Interactable", &component.m_Interactable); });

        DrawComponent<UISliderComponent>("UI Slider", entity, [](auto& component)
                                         {
            ImGui::DragFloat("Value", &component.m_Value, 0.01f, component.m_MinValue, component.m_MaxValue);
            ImGui::DragFloat("Min Value", &component.m_MinValue, 0.1f);
            ImGui::DragFloat("Max Value", &component.m_MaxValue, 0.1f);

            const char* directionStrings[] = { "Left To Right", "Right To Left", "Top To Bottom", "Bottom To Top" };
            if (const char* current = directionStrings[static_cast<int>(component.m_Direction)]; ImGui::BeginCombo("Direction", current))
            {
                for (int i = 0; i < 4; ++i)
                {
                    const bool isSelected = (current == directionStrings[i]);
                    if (ImGui::Selectable(directionStrings[i], isSelected))
                        component.m_Direction = static_cast<UISliderDirection>(i);
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::ColorEdit4("Background Color", glm::value_ptr(component.m_BackgroundColor));
            ImGui::ColorEdit4("Fill Color", glm::value_ptr(component.m_FillColor));
            ImGui::ColorEdit4("Handle Color", glm::value_ptr(component.m_HandleColor));
            ImGui::Checkbox("Interactable", &component.m_Interactable); });

        DrawComponent<UICheckboxComponent>("UI Checkbox", entity, [](auto& component)
                                           {
            ImGui::Checkbox("Is Checked", &component.m_IsChecked);
            ImGui::ColorEdit4("Unchecked Color", glm::value_ptr(component.m_UncheckedColor));
            ImGui::ColorEdit4("Checked Color", glm::value_ptr(component.m_CheckedColor));
            ImGui::ColorEdit4("Checkmark Color", glm::value_ptr(component.m_CheckmarkColor));
            ImGui::Checkbox("Interactable", &component.m_Interactable); });

        DrawComponent<UIProgressBarComponent>("UI Progress Bar", entity, [](auto& component)
                                              {
            ImGui::DragFloat("Value", &component.m_Value, 0.01f, component.m_MinValue, component.m_MaxValue);
            ImGui::DragFloat("Min Value", &component.m_MinValue, 0.1f);
            ImGui::DragFloat("Max Value", &component.m_MaxValue, 0.1f);

            const char* fillMethodStrings[] = { "Horizontal", "Vertical" };
            if (const char* current = fillMethodStrings[static_cast<int>(component.m_FillMethod)]; ImGui::BeginCombo("Fill Method", current))
            {
                for (int i = 0; i < 2; ++i)
                {
                    const bool isSelected = (current == fillMethodStrings[i]);
                    if (ImGui::Selectable(fillMethodStrings[i], isSelected))
                        component.m_FillMethod = static_cast<UIFillMethod>(i);
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::ColorEdit4("Background Color", glm::value_ptr(component.m_BackgroundColor));
            ImGui::ColorEdit4("Fill Color", glm::value_ptr(component.m_FillColor)); });

        DrawComponent<UIInputFieldComponent>("UI Input Field", entity, [](auto& component)
                                             {
            ImGui::InputText("Text", &component.m_Text);
            ImGui::InputText("Placeholder", &component.m_Placeholder);
            ImGui::DragFloat("Font Size", &component.m_FontSize, 0.5f, 1.0f, 200.0f);
            ImGui::ColorEdit4("Text Color", glm::value_ptr(component.m_TextColor));
            ImGui::ColorEdit4("Placeholder Color", glm::value_ptr(component.m_PlaceholderColor));
            ImGui::ColorEdit4("Background Color", glm::value_ptr(component.m_BackgroundColor));
            int charLimit = component.m_CharacterLimit;
            if (ImGui::DragInt("Character Limit", &charLimit, 1, 0, 10000))
                component.m_CharacterLimit = charLimit;
            ImGui::Checkbox("Interactable", &component.m_Interactable); });

        DrawComponent<UIScrollViewComponent>("UI Scroll View", entity, [](auto& component)
                                             {
            ImGui::DragFloat2("Scroll Position", glm::value_ptr(component.m_ScrollPosition));
            ImGui::DragFloat2("Content Size", glm::value_ptr(component.m_ContentSize), 1.0f, 0.0f, 10000.0f);

            const char* scrollDirStrings[] = { "Vertical", "Horizontal", "Both" };
            if (const char* current = scrollDirStrings[static_cast<int>(component.m_ScrollDirection)]; ImGui::BeginCombo("Scroll Direction", current))
            {
                for (int i = 0; i < 3; ++i)
                {
                    const bool isSelected = (current == scrollDirStrings[i]);
                    if (ImGui::Selectable(scrollDirStrings[i], isSelected))
                        component.m_ScrollDirection = static_cast<UIScrollDirection>(i);
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::DragFloat("Scroll Speed", &component.m_ScrollSpeed, 0.5f, 0.0f, 200.0f);
            ImGui::Checkbox("Show Horizontal Scrollbar", &component.m_ShowHorizontalScrollbar);
            ImGui::Checkbox("Show Vertical Scrollbar", &component.m_ShowVerticalScrollbar);
            ImGui::ColorEdit4("Scrollbar Color", glm::value_ptr(component.m_ScrollbarColor));
            ImGui::ColorEdit4("Scrollbar Track Color", glm::value_ptr(component.m_ScrollbarTrackColor)); });

        DrawComponent<UIDropdownComponent>("UI Dropdown", entity, [](auto& component)
                                           {
            int selectedIndex = component.m_SelectedIndex;
            if (ImGui::DragInt("Selected Index", &selectedIndex, 1, -1, static_cast<int>(component.m_Options.size()) - 1))
                component.m_SelectedIndex = selectedIndex;

            ImGui::Text("Options (%zu):", component.m_Options.size());
            for (sizet i = 0; i < component.m_Options.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                ImGui::InputText("##option", &component.m_Options[i].m_Label);
                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                {
                    const int removedIndex = static_cast<int>(i);
                    component.m_Options.erase(component.m_Options.begin() + static_cast<std::ptrdiff_t>(i));
                    if (component.m_SelectedIndex == removedIndex)
                        component.m_SelectedIndex = -1;
                    else if (component.m_SelectedIndex > removedIndex)
                        component.m_SelectedIndex--;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (ImGui::SmallButton("Add Option"))
            {
                component.m_Options.push_back({ "New Option" });
            }

            ImGui::ColorEdit4("Background Color", glm::value_ptr(component.m_BackgroundColor));
            ImGui::ColorEdit4("Highlight Color", glm::value_ptr(component.m_HighlightColor));
            ImGui::ColorEdit4("Text Color", glm::value_ptr(component.m_TextColor));
            ImGui::DragFloat("Font Size", &component.m_FontSize, 0.5f, 1.0f, 200.0f);
            ImGui::DragFloat("Item Height", &component.m_ItemHeight, 0.5f, 10.0f, 200.0f);
            ImGui::Checkbox("Interactable", &component.m_Interactable); });

        DrawComponent<UIGridLayoutComponent>("UI Grid Layout", entity, [](auto& component)
                                             {
            ImGui::DragFloat2("Cell Size", glm::value_ptr(component.m_CellSize), 1.0f, 1.0f, 1000.0f);
            ImGui::DragFloat2("Spacing", glm::value_ptr(component.m_Spacing), 0.5f, 0.0f, 100.0f);
            ImGui::DragFloat4("Padding (L,R,T,B)", glm::value_ptr(component.m_Padding), 0.5f, 0.0f, 200.0f);

            const char* startCornerStrings[] = { "Upper Left", "Upper Right", "Lower Left", "Lower Right" };
            if (const char* current = startCornerStrings[static_cast<int>(component.m_StartCorner)]; ImGui::BeginCombo("Start Corner", current))
            {
                for (int i = 0; i < 4; ++i)
                {
                    const bool isSelected = (current == startCornerStrings[i]);
                    if (ImGui::Selectable(startCornerStrings[i], isSelected))
                        component.m_StartCorner = static_cast<UIGridLayoutStartCorner>(i);
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            const char* startAxisStrings[] = { "Horizontal", "Vertical" };
            if (const char* current = startAxisStrings[static_cast<int>(component.m_StartAxis)]; ImGui::BeginCombo("Start Axis", current))
            {
                for (int i = 0; i < 2; ++i)
                {
                    const bool isSelected = (current == startAxisStrings[i]);
                    if (ImGui::Selectable(startAxisStrings[i], isSelected))
                        component.m_StartAxis = static_cast<UIGridLayoutAxis>(i);
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            int constraintCount = component.m_ConstraintCount;
            if (ImGui::DragInt("Constraint Count", &constraintCount, 1, 0, 100))
                component.m_ConstraintCount = constraintCount; });

        DrawComponent<UIToggleComponent>("UI Toggle", entity, [](auto& component)
                                         {
            ImGui::Checkbox("Is On", &component.m_IsOn);
            ImGui::ColorEdit4("Off Color", glm::value_ptr(component.m_OffColor));
            ImGui::ColorEdit4("On Color", glm::value_ptr(component.m_OnColor));
            ImGui::ColorEdit4("Knob Color", glm::value_ptr(component.m_KnobColor));
            ImGui::Checkbox("Interactable", &component.m_Interactable); });

        DrawComponent<ParticleSystemComponent>("Particle System", entity, [](auto& component)
                                               {
            auto& sys = component.System;
            auto& emitter = sys.Emitter;

            // Playback
            ImGui::Checkbox("Playing", &sys.Playing);
            ImGui::SameLine();
            if (ImGui::Button("Reset"))
                sys.Reset();
            ImGui::Checkbox("Looping", &sys.Looping);
            ImGui::DragFloat("Duration", &sys.Duration, 0.1f, 0.1f, 100.0f);
            ImGui::DragFloat("Playback Speed", &sys.PlaybackSpeed, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Warm Up Time", &sys.WarmUpTime, 0.1f, 0.0f, 10.0f);

            int maxP = static_cast<int>(sys.GetMaxParticles());
            if (ImGui::DragInt("Max Particles", &maxP, 10, 1, 100000))
                sys.SetMaxParticles(static_cast<u32>(maxP));
            ImGui::Text("Alive: %u", sys.GetAliveCount());

            const char* spaceItems[] = { "Local", "World" };
            int spaceIdx = static_cast<int>(sys.SimulationSpace);
            if (ImGui::Combo("Simulation Space", &spaceIdx, spaceItems, 2))
                sys.SimulationSpace = static_cast<ParticleSpace>(spaceIdx);

            // Emission
            if (ImGui::CollapsingHeader("Emission", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::DragFloat("Rate Over Time", &emitter.RateOverTime, 0.5f, 0.0f, 10000.0f);
                ImGui::DragFloat("Initial Speed", &emitter.InitialSpeed, 0.1f, 0.0f, 100.0f);
                ImGui::DragFloat("Speed Variance", &emitter.SpeedVariance, 0.1f, 0.0f, 50.0f);
                ImGui::DragFloat("Lifetime Min", &emitter.LifetimeMin, 0.05f, 0.01f, 100.0f);
                ImGui::DragFloat("Lifetime Max", &emitter.LifetimeMax, 0.05f, 0.01f, 100.0f);
                ImGui::DragFloat("Initial Size", &emitter.InitialSize, 0.01f, 0.001f, 50.0f);
                ImGui::DragFloat("Size Variance", &emitter.SizeVariance, 0.01f, 0.0f, 25.0f);
                ImGui::DragFloat("Initial Rotation", &emitter.InitialRotation, 1.0f, -360.0f, 360.0f);
                ImGui::DragFloat("Rotation Variance", &emitter.RotationVariance, 1.0f, 0.0f, 360.0f);
                ImGui::ColorEdit4("Initial Color", glm::value_ptr(emitter.InitialColor));

                const char* shapeItems[] = { "Point", "Sphere", "Box", "Cone", "Ring", "Edge" };
                int shapeIdx = static_cast<int>(emitter.Shape.index());
                if (ImGui::Combo("Emission Shape", &shapeIdx, shapeItems, 6))
                {
                    switch (shapeIdx)
                    {
                        case 0: emitter.Shape = EmitPoint{}; break;
                        case 1: emitter.Shape = EmitSphere{}; break;
                        case 2: emitter.Shape = EmitBox{}; break;
                        case 3: emitter.Shape = EmitCone{}; break;
                        case 4: emitter.Shape = EmitRing{}; break;
                        case 5: emitter.Shape = EmitEdge{}; break;
                    }
                }
                // Shape-specific parameters
                if (auto* sphere = std::get_if<EmitSphere>(&emitter.Shape))
                    ImGui::DragFloat("Sphere Radius", &sphere->Radius, 0.1f, 0.0f, 100.0f);
                if (auto* box = std::get_if<EmitBox>(&emitter.Shape))
                    ImGui::DragFloat3("Box Half Extents", glm::value_ptr(box->HalfExtents), 0.1f, 0.0f, 100.0f);
                if (auto* cone = std::get_if<EmitCone>(&emitter.Shape))
                {
                    ImGui::DragFloat("Cone Angle", &cone->Angle, 1.0f, 0.0f, 90.0f);
                    ImGui::DragFloat("Cone Radius", &cone->Radius, 0.1f, 0.0f, 100.0f);
                }
                if (auto* ring = std::get_if<EmitRing>(&emitter.Shape))
                {
                    ImGui::DragFloat("Inner Radius", &ring->InnerRadius, 0.1f, 0.0f, 100.0f);
                    ImGui::DragFloat("Outer Radius", &ring->OuterRadius, 0.1f, 0.0f, 100.0f);
                }
                if (auto* edge = std::get_if<EmitEdge>(&emitter.Shape))
                    ImGui::DragFloat("Edge Length", &edge->Length, 0.1f, 0.0f, 100.0f);
            }

            // Texture
            if (ImGui::CollapsingHeader("Rendering"))
            {
                // Blend mode
                const char* blendModes[] = { "Alpha", "Additive", "Premultiplied Alpha" };
                int blendIdx = static_cast<int>(sys.BlendMode);
                if (ImGui::Combo("Blend Mode", &blendIdx, blendModes, 3))
                    sys.BlendMode = static_cast<ParticleBlendMode>(blendIdx);

                // Render mode
                const char* renderModes[] = { "Billboard", "Stretched Billboard", "Mesh" };
                int renderIdx = static_cast<int>(sys.RenderMode);
                if (ImGui::Combo("Render Mode", &renderIdx, renderModes, 3))
                    sys.RenderMode = static_cast<ParticleRenderMode>(renderIdx);

                ImGui::Checkbox("Depth Sort", &sys.DepthSortEnabled);
                ImGui::DragFloat("Velocity Inheritance", &sys.VelocityInheritance, 0.01f, 0.0f, 1.0f);

                ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
                if (ImGui::BeginDragDropTarget())
                {
                    if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                    {
                        auto* const path = static_cast<wchar_t*>(payload->Data);
                        std::filesystem::path texturePath(path);
                        Ref<Texture2D> const texture = Texture2D::Create(texturePath.string());
                        if (texture->IsLoaded())
                        {
                            component.Texture = texture;
                        }
                        else
                        {
                            OLO_WARN("Could not load texture {0}", texturePath.filename().string());
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (component.Texture)
                {
                    ImGui::SameLine();
                    ImGui::Text("%s", "Loaded");
                    ImGui::SameLine();
                    if (ImGui::Button("Clear Texture"))
                        component.Texture = nullptr;
                }
            }

            // Texture Sheet Animation
            if (ImGui::CollapsingHeader("Texture Sheet Animation"))
            {
                ImGui::Checkbox("Sheet Enabled", &sys.TextureSheetModule.Enabled);
                if (sys.TextureSheetModule.Enabled)
                {
                    int gridX = static_cast<int>(sys.TextureSheetModule.GridX);
                    int gridY = static_cast<int>(sys.TextureSheetModule.GridY);
                    int totalFrames = static_cast<int>(sys.TextureSheetModule.TotalFrames);
                    if (ImGui::DragInt("Grid X", &gridX, 1, 1, 64))
                        sys.TextureSheetModule.GridX = static_cast<u32>(gridX);
                    if (ImGui::DragInt("Grid Y", &gridY, 1, 1, 64))
                        sys.TextureSheetModule.GridY = static_cast<u32>(gridY);
                    if (ImGui::DragInt("Total Frames", &totalFrames, 1, 1, 4096))
                        sys.TextureSheetModule.TotalFrames = static_cast<u32>(totalFrames);
                    const char* sheetModes[] = { "Over Lifetime", "By Speed" };
                    int sheetIdx = static_cast<int>(sys.TextureSheetModule.Mode);
                    if (ImGui::Combo("Animation Mode", &sheetIdx, sheetModes, 2))
                        sys.TextureSheetModule.Mode = static_cast<TextureSheetAnimMode>(sheetIdx);
                    if (sys.TextureSheetModule.Mode == TextureSheetAnimMode::BySpeed)
                        ImGui::DragFloat("Speed Range", &sys.TextureSheetModule.SpeedRange, 0.1f, 0.1f, 100.0f);
                }
            }

            // Modules
            if (ImGui::CollapsingHeader("Gravity"))
            {
                ImGui::Checkbox("Gravity Enabled", &sys.GravityModule.Enabled);
                ImGui::DragFloat3("Gravity", glm::value_ptr(sys.GravityModule.Gravity), 0.1f);
            }
            if (ImGui::CollapsingHeader("Drag"))
            {
                ImGui::Checkbox("Drag Enabled", &sys.DragModule.Enabled);
                ImGui::DragFloat("Drag Coefficient", &sys.DragModule.DragCoefficient, 0.01f, 0.0f, 10.0f);
            }
            if (ImGui::CollapsingHeader("Color Over Lifetime"))
            {
                ImGui::Checkbox("Color OL Enabled", &sys.ColorModule.Enabled);
                if (sys.ColorModule.Enabled)
                {
                    DrawParticleCurve4Editor("Color Curve", sys.ColorModule.ColorCurve);
                }
            }
            if (ImGui::CollapsingHeader("Size Over Lifetime"))
            {
                ImGui::Checkbox("Size OL Enabled", &sys.SizeModule.Enabled);
                if (sys.SizeModule.Enabled)
                {
                    DrawParticleCurveEditor("Size Curve", sys.SizeModule.SizeCurve, 0.0f, 2.0f);
                }
            }
            if (ImGui::CollapsingHeader("Velocity Over Lifetime"))
            {
                ImGui::Checkbox("Velocity OL Enabled", &sys.VelocityModule.Enabled);
                if (sys.VelocityModule.Enabled)
                {
                    ImGui::DragFloat3("Linear Velocity", glm::value_ptr(sys.VelocityModule.LinearVelocity), 0.1f);
                    ImGui::DragFloat("Speed Multiplier", &sys.VelocityModule.SpeedMultiplier, 0.01f, 0.0f, 10.0f);
                    DrawParticleCurveEditor("Speed Curve", sys.VelocityModule.SpeedCurve, 0.0f, 2.0f);
                }
            }
            if (ImGui::CollapsingHeader("Rotation Over Lifetime"))
            {
                ImGui::Checkbox("Rotation OL Enabled", &sys.RotationModule.Enabled);
                ImGui::DragFloat("Angular Velocity", &sys.RotationModule.AngularVelocity, 1.0f, -1000.0f, 1000.0f);
            }
            if (ImGui::CollapsingHeader("Noise"))
            {
                ImGui::Checkbox("Noise Enabled", &sys.NoiseModule.Enabled);
                ImGui::DragFloat("Noise Strength", &sys.NoiseModule.Strength, 0.1f, 0.0f, 100.0f);
                ImGui::DragFloat("Noise Frequency", &sys.NoiseModule.Frequency, 0.1f, 0.0f, 100.0f);
            }

            // Phase 2 modules
            if (ImGui::CollapsingHeader("Collision"))
            {
                ImGui::Checkbox("Collision Enabled", &sys.CollisionModule.Enabled);
                const char* collisionModes[] = { "World Plane", "Scene Raycast" };
                int modeIdx = static_cast<int>(sys.CollisionModule.Mode);
                if (ImGui::Combo("Collision Mode", &modeIdx, collisionModes, 2))
                    sys.CollisionModule.Mode = static_cast<CollisionMode>(modeIdx);
                if (sys.CollisionModule.Mode == CollisionMode::WorldPlane)
                {
                    ImGui::DragFloat3("Plane Normal", glm::value_ptr(sys.CollisionModule.PlaneNormal), 0.01f, -1.0f, 1.0f);
                    ImGui::DragFloat("Plane Offset", &sys.CollisionModule.PlaneOffset, 0.1f);
                }
                ImGui::DragFloat("Bounce", &sys.CollisionModule.Bounce, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Lifetime Loss", &sys.CollisionModule.LifetimeLoss, 0.01f, 0.0f, 1.0f);
                ImGui::Checkbox("Kill On Collide", &sys.CollisionModule.KillOnCollide);
            }
            if (ImGui::CollapsingHeader("Force Fields"))
            {
                const char* ffTypes[] = { "Attraction", "Repulsion", "Vortex" };
                for (size_t fi = 0; fi < sys.ForceFields.size(); ++fi)
                {
                    auto& ff = sys.ForceFields[fi];
                    ImGui::PushID(static_cast<int>(fi));
                    std::string label = "Force Field " + std::to_string(fi);
                    if (ImGui::TreeNode(label.c_str()))
                    {
                        ImGui::Checkbox("Enabled", &ff.Enabled);
                        int ffIdx = static_cast<int>(ff.Type);
                        if (ImGui::Combo("Force Type", &ffIdx, ffTypes, 3))
                            ff.Type = static_cast<ForceFieldType>(ffIdx);
                        ImGui::DragFloat3("Position", glm::value_ptr(ff.Position), 0.1f);
                        ImGui::DragFloat("Strength", &ff.Strength, 0.1f, 0.0f, 1000.0f);
                        ImGui::DragFloat("Radius", &ff.Radius, 0.1f, 0.01f, 1000.0f);
                        if (ff.Type == ForceFieldType::Vortex)
                            ImGui::DragFloat3("Vortex Axis", glm::value_ptr(ff.Axis), 0.01f, -1.0f, 1.0f);
                        if (ImGui::Button("Remove"))
                        {
                            sys.ForceFields.erase(sys.ForceFields.begin() + static_cast<ptrdiff_t>(fi));
                            ImGui::TreePop();
                            ImGui::PopID();
                            break;
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                if (ImGui::Button("Add Force Field"))
                {
                    sys.ForceFields.emplace_back();
                }
            }
            if (ImGui::CollapsingHeader("Trail"))
            {
                ImGui::Checkbox("Trail Enabled", &sys.TrailModule.Enabled);
                int maxPts = static_cast<int>(sys.TrailModule.MaxTrailPoints);
                if (ImGui::DragInt("Max Trail Points", &maxPts, 1, 2, 128))
                    sys.TrailModule.MaxTrailPoints = static_cast<u32>(maxPts);
                ImGui::DragFloat("Trail Lifetime", &sys.TrailModule.TrailLifetime, 0.01f, 0.01f, 10.0f);
                ImGui::DragFloat("Min Vertex Distance", &sys.TrailModule.MinVertexDistance, 0.01f, 0.001f, 10.0f);
                ImGui::DragFloat("Width Start", &sys.TrailModule.WidthStart, 0.01f, 0.001f, 10.0f);
                ImGui::DragFloat("Width End", &sys.TrailModule.WidthEnd, 0.01f, 0.0f, 10.0f);
                ImGui::ColorEdit4("Trail Color Start", glm::value_ptr(sys.TrailModule.ColorStart));
                ImGui::ColorEdit4("Trail Color End", glm::value_ptr(sys.TrailModule.ColorEnd));
            }
            if (ImGui::CollapsingHeader("Sub-Emitter"))
            {
                ImGui::Checkbox("Sub-Emitter Enabled", &sys.SubEmitterModule.Enabled);
                if (sys.SubEmitterModule.Enabled)
                    ImGui::TextDisabled("Configure sub-emitter entries via scripting");
            }
            if (ImGui::CollapsingHeader("LOD"))
            {
                ImGui::DragFloat("LOD Distance 1", &sys.LODDistance1, 1.0f, 0.0f, 10000.0f);
                ImGui::DragFloat("LOD Distance 2", &sys.LODDistance2, 1.0f, 0.0f, 10000.0f);
                ImGui::DragFloat("LOD Max Distance", &sys.LODMaxDistance, 1.0f, 0.0f, 10000.0f);
            } });
    }

    template<typename T>
    void SceneHierarchyPanel::DisplayAddComponentEntry(const std::string& entryName)
    {
        if ((!m_SelectionContext.HasComponent<T>()) && ImGui::MenuItem(entryName.c_str()))
        {
            m_SelectionContext.AddComponent<T>();
            ImGui::CloseCurrentPopup();
        }
    }
} // namespace OloEngine
